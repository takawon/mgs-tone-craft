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
    std::atomic<std::int32_t> startup_result{E_FAIL};
    std::thread worker;
    HANDLE stop_event{};
    HANDLE ready_event{};
    engine::RealtimeEngineHost* engine{};
    std::atomic<std::uint32_t> master_volume_percent{100};

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
            result = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                kStreamFlags,
                0,
                0,
                &format,
                nullptr);
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
            if (wait == WAIT_OBJECT_0) {
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
    : impl_(std::make_unique<Impl>()) {}

WasapiAudioSink::~WasapiAudioSink() {
    stop();
}

bool WasapiAudioSink::start(
    engine::RealtimeEngineHost& engine) {
    if (impl_->worker.joinable()) {
        return false;
    }
    impl_->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop_event || !impl_->ready_event) {
        stop();
        return false;
    }
    impl_->engine = &engine;
    impl_->startup_result.store(E_FAIL, std::memory_order_relaxed);
    impl_->worker = std::thread([this]() {
        impl_->run();
    });

    const auto wait = WaitForSingleObject(impl_->ready_event, 5'000);
    const bool started = wait == WAIT_OBJECT_0
        && SUCCEEDED(static_cast<HRESULT>(
            impl_->startup_result.load(std::memory_order_acquire)));
    if (!started) {
        stop();
    }
    return started;
}

void WasapiAudioSink::stop() noexcept {
    if (impl_->stop_event) {
        SetEvent(impl_->stop_event);
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (impl_->ready_event) {
        CloseHandle(impl_->ready_event);
        impl_->ready_event = nullptr;
    }
    if (impl_->stop_event) {
        CloseHandle(impl_->stop_event);
        impl_->stop_event = nullptr;
    }
    impl_->engine = nullptr;
    impl_->running.store(false, std::memory_order_release);
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
