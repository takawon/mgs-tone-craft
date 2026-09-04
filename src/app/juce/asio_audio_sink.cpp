#include "asio_audio_sink.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "mgstc/audio/stereo_sample_rate_converter.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"

namespace mgstc::audio {
namespace {

// Keep the resampling refill small so a command does not wait for a large
// source block. At the native engine rate the converter is bypassed below.
constexpr int kDefaultSourceChunkFrames = 64;

[[nodiscard]] double chooseSampleRate(
    juce::AudioIODevice& device,
    double requested) {
    const auto rates = device.getAvailableSampleRates();
    if (rates.isEmpty()) {
        return requested > 0.0 ? requested
                               : StereoSampleRateConverter::kSourceSampleRate;
    }
    if (requested > 0.0) {
        return *std::min_element(
            rates.begin(),
            rates.end(),
            [requested](double left, double right) {
                return std::abs(left - requested)
                    < std::abs(right - requested);
            });
    }
    const auto current = device.getCurrentSampleRate();
    if (current > 0.0) {
        return current;
    }
    constexpr auto preferred = StereoSampleRateConverter::kSourceSampleRate;
    return *std::min_element(
        rates.begin(),
        rates.end(),
        [](double left, double right) {
            return std::abs(left - preferred)
                < std::abs(right - preferred);
        });
}

[[nodiscard]] int chooseBufferFrames(
    juce::AudioIODevice& device,
    int requested) noexcept {
    if (requested > 0) {
        return requested;
    }
    const auto default_size = device.getDefaultBufferSize();
    return default_size > 0 ? default_size : 512;
}

}  // namespace

struct AsioAudioSink::Impl final : juce::AudioIODeviceCallback {
    static_assert(
        std::atomic<engine::RealtimeEngineHost*>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::unique_ptr<juce::AudioIODeviceType> type{
        juce::AudioIODeviceType::createAudioIODeviceType_ASIO()};
    std::unique_ptr<juce::AudioIODevice> device;
    StereoSampleRateConverter converter;
    std::atomic<std::uint32_t> pending_statuses{};
    std::array<std::atomic<std::int32_t>, 4> status_errors{};
    std::atomic<engine::RealtimeEngineHost*> engine{};
    std::atomic<bool> running{};
    std::atomic<bool> render_failed{};
    std::atomic<bool> format_changed{};
    std::atomic<std::uint32_t> master_volume_percent{100};
    std::vector<float> direct_interleaved;
    bool direct_render{};
    mutable std::mutex text_mutex;
    std::string driver_name;
    std::vector<std::string> driver_names;
    std::string status_text{"ASIO is not open"};
    std::string last_error;
    double prepared_sample_rate{};

    void setText(std::string status, std::string error = {}) {
        const std::scoped_lock lock(text_mutex);
        status_text = std::move(status);
        last_error = std::move(error);
    }

    void push(
        AudioSinkStatusType type_value,
        std::int32_t native_error = 0) noexcept {
        const auto index = static_cast<std::size_t>(type_value);
        status_errors[index].store(native_error, std::memory_order_relaxed);
        pending_statuses.fetch_or(
            1U << static_cast<std::uint32_t>(index),
            std::memory_order_release);
    }

    [[nodiscard]] static bool renderSource(
        void* context,
        std::span<float> output) noexcept {
        auto* self = static_cast<Impl*>(context);
        auto* host = self->engine.load(std::memory_order_acquire);
        return host != nullptr && host->render(output).ok();
    }

    void audioDeviceIOCallbackWithContext(
        const float* const*,
        int,
        float* const* output_channel_data,
        int num_output_channels,
        int num_samples,
        const juce::AudioIODeviceCallbackContext&) override {
        if (num_samples <= 0) {
            return;
        }

        const auto frames = static_cast<std::size_t>(num_samples);
        if (output_channel_data != nullptr) {
            for (int channel = 0; channel < num_output_channels; ++channel) {
                if (output_channel_data[channel] != nullptr) {
                    std::fill_n(
                        output_channel_data[channel],
                        frames,
                        0.0F);
                }
            }
        }

        if (num_output_channels <= 0
            || output_channel_data == nullptr
            || output_channel_data[0] == nullptr
            || format_changed.load(std::memory_order_acquire)) {
            return;
        }

        auto* right = num_output_channels >= 2
                && output_channel_data[1] != nullptr
            ? output_channel_data[1]
            : nullptr;
        bool rendered = false;
        if (direct_render
            && frames <= direct_interleaved.size() / 2U) {
            auto* host = engine.load(std::memory_order_acquire);
            rendered = host != nullptr
                && host->render(std::span<float>(
                    direct_interleaved.data(),
                    frames * 2U)).ok();
            if (rendered) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    output_channel_data[0][frame]
                        = direct_interleaved[frame * 2U];
                    if (right != nullptr) {
                        right[frame]
                            = direct_interleaved[frame * 2U + 1U];
                    }
                }
            }
        } else {
            rendered = converter.process(
                output_channel_data[0],
                right,
                frames,
                {
                    .context = this,
                    .render = &Impl::renderSource,
                });
        }
        if (!rendered) {
            if (!render_failed.exchange(true, std::memory_order_relaxed)) {
                push(AudioSinkStatusType::DeviceError, -1);
            }
            return;
        }

        const auto percent = master_volume_percent.load(
            std::memory_order_relaxed);
        const auto gain = static_cast<float>(percent) / 100.0F;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            output_channel_data[0][frame] *= gain;
            if (right != nullptr) {
                right[frame] *= gain;
            }
        }
    }

    void audioDeviceAboutToStart(
        juce::AudioIODevice* starting_device) override {
        const auto rate = starting_device != nullptr
            ? starting_device->getCurrentSampleRate()
            : 0.0;
        const bool changed = rate <= 0.0
            || std::abs(rate - prepared_sample_rate) > 0.5;
        format_changed.store(changed, std::memory_order_release);
        if (changed) {
            push(AudioSinkStatusType::DeviceError, -2);
        }
    }

    void audioDeviceStopped() override {
        running.store(false, std::memory_order_release);
        push(AudioSinkStatusType::Stopped);
    }

    void audioDeviceError(const juce::String& error_message) override {
        setText("ASIO device error", error_message.toStdString());
        push(AudioSinkStatusType::DeviceError, -3);
    }
};

AsioAudioSink::AsioAudioSink()
    : impl_(std::make_unique<Impl>()) {
    if (impl_->type == nullptr) {
        impl_->setText(
            "ASIO is unavailable",
            "JUCE could not create its ASIO device type");
    }
}

AsioAudioSink::~AsioAudioSink() {
    close();
}

std::vector<std::string> AsioAudioSink::driverNames() {
    std::vector<std::string> names;
    if (impl_->type == nullptr) {
        return names;
    }
    if (running()) {
        const std::scoped_lock lock(impl_->text_mutex);
        if (!impl_->driver_names.empty()) {
            return impl_->driver_names;
        }
        if (!impl_->driver_name.empty()) {
            return {impl_->driver_name};
        }
        return names;
    }

    impl_->type->scanForDevices();
    const auto juce_names = impl_->type->getDeviceNames(false);
    names.reserve(static_cast<std::size_t>(juce_names.size()));
    for (const auto& name : juce_names) {
        names.push_back(name.toStdString());
    }
    {
        const std::scoped_lock lock(impl_->text_mutex);
        impl_->driver_names = names;
    }
    return names;
}

bool AsioAudioSink::open(
    std::string_view driver_name,
    double requested_sample_rate,
    int requested_buffer_frames) {
    close();
    if (impl_->type == nullptr) {
        return false;
    }
    if (driver_name.empty()) {
        impl_->setText("ASIO open failed", "No ASIO driver was selected");
        return false;
    }

    impl_->type->scanForDevices();
    {
        std::vector<std::string> names;
        const auto juce_names = impl_->type->getDeviceNames(false);
        names.reserve(static_cast<std::size_t>(juce_names.size()));
        for (const auto& name : juce_names) {
            names.push_back(name.toStdString());
        }
        const std::scoped_lock lock(impl_->text_mutex);
        impl_->driver_names = std::move(names);
    }
    impl_->device.reset(impl_->type->createDevice(
        juce::String(
            driver_name.data(),
            static_cast<int>(driver_name.size())),
        {}));
    if (impl_->device == nullptr) {
        impl_->setText(
            "ASIO open failed",
            "The selected ASIO driver could not be created");
        return false;
    }

    juce::BigInteger output_channels;
    const auto channel_count =
        impl_->device->getOutputChannelNames().size();
    if (channel_count <= 0) {
        impl_->setText(
            "ASIO open failed",
            "The selected ASIO driver has no output channels");
        impl_->device.reset();
        return false;
    }
    output_channels.setRange(0, std::min(channel_count, 2), true);
    const auto sample_rate = chooseSampleRate(
        *impl_->device,
        requested_sample_rate);
    const auto buffer_frames = chooseBufferFrames(
        *impl_->device,
        requested_buffer_frames);
    const auto error = impl_->device->open(
        {},
        output_channels,
        sample_rate,
        buffer_frames);
    if (error.isNotEmpty()) {
        impl_->setText("ASIO open failed", error.toStdString());
        impl_->device->close();
        impl_->device.reset();
        return false;
    }

    const auto actual_rate = impl_->device->getCurrentSampleRate();
    if (actual_rate <= 0.0
        || !impl_->converter.prepare(
            actual_rate,
            kDefaultSourceChunkFrames)) {
        impl_->setText(
            "ASIO open failed",
            "The ASIO driver reported an invalid sample rate");
        impl_->device->close();
        impl_->device.reset();
        return false;
    }

    const auto current_buffer_frames =
        impl_->device->getCurrentBufferSizeSamples();
    const auto direct_buffer_frames = std::max(
        buffer_frames,
        current_buffer_frames > 0 ? current_buffer_frames : 0);
    impl_->direct_interleaved.assign(
        static_cast<std::size_t>(direct_buffer_frames) * 2U,
        0.0F);
    impl_->direct_render = std::abs(
        actual_rate - StereoSampleRateConverter::kSourceSampleRate) <= 0.5;
    impl_->prepared_sample_rate = actual_rate;
    {
        const std::scoped_lock lock(impl_->text_mutex);
        impl_->driver_name.assign(driver_name);
    }
    impl_->format_changed.store(false, std::memory_order_release);
    impl_->setText("ASIO is open");
    return true;
}

void AsioAudioSink::close() noexcept {
    stop();
    if (impl_->device != nullptr) {
        impl_->device->close();
        impl_->device.reset();
    }
    impl_->prepared_sample_rate = 0.0;
    impl_->direct_render = false;
    impl_->direct_interleaved.clear();
    impl_->format_changed.store(false, std::memory_order_release);
    if (impl_->type != nullptr) {
        impl_->setText("ASIO is closed");
    }
}

bool AsioAudioSink::isOpen() const noexcept {
    return impl_->device != nullptr && impl_->device->isOpen();
}

bool AsioAudioSink::start(engine::RealtimeEngineHost& engine) {
    if (!isOpen()) {
        impl_->setText(
            "ASIO start failed",
            "Open an ASIO driver before starting it");
        return false;
    }
    if (running()) {
        return true;
    }

    impl_->converter.reset();
    impl_->render_failed.store(false, std::memory_order_relaxed);
    impl_->format_changed.store(false, std::memory_order_release);
    impl_->engine.store(&engine, std::memory_order_release);
    impl_->device->start(impl_.get());
    const bool format_changed =
        impl_->format_changed.load(std::memory_order_acquire);
    if (!impl_->device->isPlaying() || format_changed) {
        impl_->device->stop();
        impl_->engine.store(nullptr, std::memory_order_release);
        auto error = format_changed
            ? juce::String(
                "The ASIO sample rate changed during startup; reopen it")
            : impl_->device->getLastError();
        if (error.isEmpty()) {
            error = "The ASIO driver did not start";
        }
        impl_->setText("ASIO start failed", error.toStdString());
        impl_->push(AudioSinkStatusType::DeviceError, -4);
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->setText("ASIO is running");
    impl_->push(AudioSinkStatusType::Started);
    return true;
}

void AsioAudioSink::stop() noexcept {
    if (impl_->device != nullptr && impl_->device->isPlaying()) {
        impl_->device->stop();
    }
    impl_->running.store(false, std::memory_order_release);
    impl_->engine.store(nullptr, std::memory_order_release);
    if (impl_->device != nullptr && impl_->device->isOpen()) {
        impl_->setText("ASIO is stopped");
    }
}

bool AsioAudioSink::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

bool AsioAudioSink::pollStatus(AudioSinkStatus& status) noexcept {
    auto pending = impl_->pending_statuses.load(std::memory_order_acquire);
    while (pending != 0) {
        const auto index = static_cast<std::uint32_t>(
            std::countr_zero(pending));
        const auto bit = 1U << index;
        if (impl_->pending_statuses.compare_exchange_weak(
                pending,
                pending & ~bit,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            status = {
                .type = static_cast<AudioSinkStatusType>(index),
                .native_error = impl_->status_errors[index].load(
                    std::memory_order_relaxed),
            };
            return true;
        }
    }
    return false;
}

bool AsioAudioSink::hasControlPanel() const noexcept {
    return impl_->device != nullptr && impl_->device->hasControlPanel();
}

bool AsioAudioSink::showControlPanel() {
    if (impl_->device == nullptr) {
        impl_->setText(
            "ASIO control panel unavailable",
            "Open an ASIO driver first");
        return false;
    }
    if (running()) {
        impl_->setText(
            "ASIO control panel unavailable",
            "Stop ASIO before opening its control panel");
        return false;
    }
    if (!impl_->device->hasControlPanel()
        || !impl_->device->showControlPanel()) {
        impl_->setText(
            "ASIO control panel unavailable",
            impl_->device->getLastError().toStdString());
        return false;
    }

    impl_->setText("ASIO control panel closed");
    return true;
}

double AsioAudioSink::currentSampleRate() const noexcept {
    return impl_->device != nullptr
        ? impl_->device->getCurrentSampleRate()
        : 0.0;
}

std::string AsioAudioSink::selectedDriverName() const {
    const std::scoped_lock lock(impl_->text_mutex);
    return impl_->driver_name;
}

std::string AsioAudioSink::statusText() const {
    if (impl_->format_changed.load(std::memory_order_acquire)) {
        return "ASIO format changed; stop and reopen the driver";
    }
    if (impl_->render_failed.load(std::memory_order_relaxed)) {
        return "ASIO stopped rendering after an engine error";
    }
    const std::scoped_lock lock(impl_->text_mutex);
    return impl_->status_text;
}

std::string AsioAudioSink::lastError() const {
    const std::scoped_lock lock(impl_->text_mutex);
    return impl_->last_error;
}

void AsioAudioSink::setMasterVolumePercent(
    std::uint32_t percent) noexcept {
    impl_->master_volume_percent.store(
        std::min(percent, 100U),
        std::memory_order_relaxed);
}

std::uint32_t AsioAudioSink::masterVolumePercent() const noexcept {
    return impl_->master_volume_percent.load(std::memory_order_relaxed);
}

}  // namespace mgstc::audio
