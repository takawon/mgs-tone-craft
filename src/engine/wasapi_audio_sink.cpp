#include "mgstc/audio/wasapi_audio_sink.hpp"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <span>
#include <thread>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/spsc_queue.hpp"

namespace mgstc::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kStreamFlags =
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK
    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
    | AUDCLNT_STREAMFLAGS_NOPERSIST;

WAVEFORMATEX makeFormat() noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48'000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(
        format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec =
        format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

}  // namespace

struct WasapiAudioSink::Impl {
    engine::SpscQueue<AudioSinkStatus, 64> statuses;
    std::atomic<bool> running{};
    std::atomic<bool> abandoned{};
    std::atomic<std::int32_t> startup_result{E_FAIL};
    std::thread worker;
    HANDLE stop_event{};
    HANDLE ready_event{};
    HANDLE exit_event{};
    engine::RealtimeEngineHost* engine{};
    std::atomic<std::uint32_t> master_volume_percent{100};

    Impl() {
        // Manual-reset and owned for the whole Impl lifetime, so an abandoned
        // worker never waits on a handle the sink already closed.
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl() {
        // Never self-join: an abandoned worker can be the last owner.
        if (worker.joinable()
            && worker.get_id() != std::this_thread::get_id()) {
            if (stop_event != nullptr) {
                SetEvent(stop_event);
            }
            worker.join();
        }
        for (HANDLE* handle : {&stop_event, &ready_event, &exit_event}) {
            if (*handle != nullptr) {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool eventsReady() const noexcept {
        return stop_event != nullptr && ready_event != nullptr
            && exit_event != nullptr;
    }

    void applyMasterVolume(std::span<float> samples) const noexcept {
        const auto percent = master_volume_percent.load(
            std::memory_order_relaxed);
        const auto gain = static_cast<float>(percent) / 100.0F;
        for (auto& sample : samples) {
            sample *= gain;
        }
    }

    void push(
        AudioSinkStatusType type,
        HRESULT error = S_OK) noexcept {
        static_cast<void>(statuses.tryPush({
            .type = type,
            .native_error = static_cast<std::int32_t>(error),
        }));
    }

    void signalStartup(HRESULT result) noexcept {
        startup_result.store(
            static_cast<std::int32_t>(result),
            std::memory_order_release);
        SetEvent(ready_event);
    }

    void run() noexcept {
        runWorker();
        // Last thing the worker does: lets stop() tell "left the device calls"
        // from "still stuck inside one".
        if (exit_event != nullptr) {
            SetEvent(exit_event);
        }
    }

    void runWorker() noexcept {
        const auto com_result =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_initialized = SUCCEEDED(com_result);
        if (FAILED(com_result)) {
            push(AudioSinkStatusType::DeviceError, com_result);
            signalStartup(com_result);
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render_client;
        HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT result = audio_event ? S_OK : HRESULT_FROM_WIN32(GetLastError());

        if (SUCCEEDED(result)) {
            result = CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator));
        }
        if (SUCCEEDED(result)) {
            result = enumerator->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                &device);
        }
        if (SUCCEEDED(result)) {
            result = device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(client.GetAddressOf()));
        }

        const auto format = makeFormat();
        if (SUCCEEDED(result)) {
            // Prefer shared-mode low latency when IAudioClient3 is
            // available; otherwise ask for ~10ms instead of OS default
            // (period 0). Fall back safely so startup still works.
            result = E_FAIL;
            ComPtr<IAudioClient3> client3;
            if (SUCCEEDED(client.As(&client3))) {
                UINT32 default_period_frames = 0;
                UINT32 fundamental_period_frames = 0;
                UINT32 min_period_frames = 0;
                UINT32 max_period_frames = 0;
                if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                        &format,
                        &default_period_frames,
                        &fundamental_period_frames,
                        &min_period_frames,
                        &max_period_frames))
                    && min_period_frames > 0) {
                    result = client3->InitializeSharedAudioStream(
                        kStreamFlags,
                        min_period_frames,
                        &format,
                        nullptr);
                }
            }
            if (FAILED(result)) {
                // REFERENCE_TIME is 100ns units; 10ms = 100000.
                constexpr REFERENCE_TIME kSharedBufferDuration = 100'000;
                result = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    kStreamFlags,
                    kSharedBufferDuration,
                    0,
                    &format,
                    nullptr);
            }
            if (FAILED(result)) {
                result = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    kStreamFlags,
                    0,
                    0,
                    &format,
                    nullptr);
            }
        }
        if (SUCCEEDED(result)) {
            result = client->SetEventHandle(audio_event);
        }
        UINT32 buffer_frames{};
        if (SUCCEEDED(result)) {
            result = client->GetBufferSize(&buffer_frames);
        }
        if (SUCCEEDED(result)) {
            result = client->GetService(
                IID_PPV_ARGS(&render_client));
        }

        BYTE* buffer{};
        if (SUCCEEDED(result)) {
            result = render_client->GetBuffer(buffer_frames, &buffer);
        }
        if (SUCCEEDED(result)) {
            auto samples = std::span<float>(
                reinterpret_cast<float*>(buffer),
                static_cast<std::size_t>(buffer_frames) * 2);
            const auto rendered = engine->render(samples);
            applyMasterVolume(samples);
            result = render_client->ReleaseBuffer(buffer_frames, 0);
            if (!rendered.ok() && SUCCEEDED(result)) {
                result = E_FAIL;
            }
        }
        if (SUCCEEDED(result)) {
            result = client->Start();
        }
        if (FAILED(result)) {
            push(AudioSinkStatusType::DeviceError, result);
            signalStartup(result);
            if (audio_event) {
                CloseHandle(audio_event);
            }
            if (com_initialized) {
                CoUninitialize();
            }
            return;
        }

        running.store(true, std::memory_order_release);
        push(AudioSinkStatusType::Started);
        signalStartup(S_OK);

        const HANDLE events[]{stop_event, audio_event};
        bool underrun_reported = false;
        while (true) {
            const auto wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0
                || abandoned.load(std::memory_order_acquire)) {
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                result = HRESULT_FROM_WIN32(GetLastError());
                push(AudioSinkStatusType::DeviceError, result);
                break;
            }

            UINT32 padding{};
            result = client->GetCurrentPadding(&padding);
            if (FAILED(result)) {
                push(AudioSinkStatusType::DeviceError, result);
                break;
            }
            if (padding == 0 && !underrun_reported) {
                underrun_reported = true;
                push(AudioSinkStatusType::Underrun);
            } else if (padding != 0) {
                underrun_reported = false;
            }

            const auto available = buffer_frames - padding;
            if (available == 0) {
                continue;
            }
            buffer = nullptr;
            result = render_client->GetBuffer(available, &buffer);
            if (FAILED(result)) {
                push(AudioSinkStatusType::DeviceError, result);
                break;
            }
            if (abandoned.load(std::memory_order_acquire)) {
                // A replacement sink owns the engine now: release the buffer
                // silently instead of rendering twice.
                static_cast<void>(render_client->ReleaseBuffer(
                    available, AUDCLNT_BUFFERFLAGS_SILENT));
                break;
            }
            auto samples = std::span<float>(
                reinterpret_cast<float*>(buffer),
                static_cast<std::size_t>(available) * 2);
            const auto rendered = engine->render(samples);
            applyMasterVolume(samples);
            result = render_client->ReleaseBuffer(available, 0);
            if (FAILED(result) || !rendered.ok()) {
                push(
                    AudioSinkStatusType::DeviceError,
                    FAILED(result) ? result : E_FAIL);
                break;
            }
        }

        static_cast<void>(client->Stop());
        running.store(false, std::memory_order_release);
        push(AudioSinkStatusType::Stopped);
        CloseHandle(audio_event);
        if (com_initialized) {
            CoUninitialize();
        }
    }
};

WasapiAudioSink::WasapiAudioSink()
    : impl_(std::make_shared<Impl>()) {}

WasapiAudioSink::~WasapiAudioSink() {
    stop();
}

bool WasapiAudioSink::start(
    engine::RealtimeEngineHost& engine) {
    if (impl_->worker.joinable() || !impl_->eventsReady()) {
        return false;
    }
    ResetEvent(impl_->stop_event);
    ResetEvent(impl_->ready_event);
    ResetEvent(impl_->exit_event);
    impl_->engine = &engine;
    impl_->startup_result.store(E_FAIL, std::memory_order_relaxed);
    impl_->worker = std::thread([self = impl_]() {
        self->run();
    });

    const auto wait =
        WaitForSingleObject(impl_->ready_event, kStartWaitMs);
    const bool started = wait == WAIT_OBJECT_0
        && SUCCEEDED(static_cast<HRESULT>(
            impl_->startup_result.load(std::memory_order_acquire)));
    if (!started) {
        stop();
    }
    return started;
}

void WasapiAudioSink::stop() noexcept {
    if (impl_->stop_event != nullptr) {
        SetEvent(impl_->stop_event);
    }
    if (impl_->worker.joinable()) {
        // A device call inside the worker (GetBuffer / Initialize / Stop) can
        // stall for seconds when the endpoint changes. Bound the wait and
        // abandon rather than freezing the caller — the message thread reaches
        // here from device switches and shutdown.
        const auto wait = impl_->exit_event != nullptr
            ? WaitForSingleObject(impl_->exit_event, kStopWaitMs)
            : WAIT_FAILED;
        if (wait == WAIT_OBJECT_0) {
            impl_->worker.join();
        } else {
            abandonStuckWorker();
            return;
        }
    }
    impl_->engine = nullptr;
    impl_->running.store(false, std::memory_order_release);
}

void WasapiAudioSink::abandonStuckWorker() noexcept {
    // The worker keeps its own Impl (and event handles) alive and exits as soon
    // as the stuck call returns; it renders nothing more once abandoned.
    impl_->abandoned.store(true, std::memory_order_release);
    impl_->running.store(false, std::memory_order_release);
    impl_->worker.detach();
    const auto volume =
        impl_->master_volume_percent.load(std::memory_order_relaxed);
    try {
        impl_ = std::make_shared<Impl>();
        impl_->master_volume_percent.store(
            volume, std::memory_order_relaxed);
    } catch (...) {
        // Out of memory: keep the abandoned Impl. It is muted and not
        // joinable, so start() will simply refuse until memory recovers.
    }
}

bool WasapiAudioSink::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

bool WasapiAudioSink::pollStatus(
    AudioSinkStatus& status) noexcept {
    return impl_->statuses.tryPop(status);
}

void WasapiAudioSink::setMasterVolumePercent(
    std::uint32_t percent) noexcept {
    impl_->master_volume_percent.store(
        std::min(percent, 100U), std::memory_order_relaxed);
}

std::uint32_t WasapiAudioSink::masterVolumePercent() const noexcept {
    return impl_->master_volume_percent.load(
        std::memory_order_relaxed);
}

}  // namespace mgstc::audio
