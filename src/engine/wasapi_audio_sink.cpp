#include "mgstc/audio/wasapi_audio_sink.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/spsc_queue.hpp"
#if defined(MGSTC_WASAPI_TESTING)
#include <chrono>
#include <cmath>
#include "mgstc/audio/detail/wasapi_audio_sink_test_access.hpp"
#endif

namespace mgstc::audio {
namespace {
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
using Microsoft::WRL::ComPtr;
constexpr DWORD kStreamFlags =
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
constexpr std::uint32_t kClosed = 0x80000000U;
constexpr unsigned kBorrowAttempts = 4;
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
WAVEFORMATEX makeFormat() noexcept {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48'000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}
#if defined(MGSTC_WASAPI_TESTING)
using Stage = detail::WasapiTestStage;
std::atomic<std::uintptr_t> test_generation_sequence{};
#define WASAPI_STAGE(point) at(Stage::point)
#define WASAPI_COUNT(name) name.fetch_add(1, std::memory_order_relaxed)
#define WASAPI_OWNER_COUNT(name) owner.name.fetch_add(1, std::memory_order_relaxed)
#else
#define WASAPI_STAGE(point) ((void)0)
#define WASAPI_COUNT(name) ((void)0)
#define WASAPI_OWNER_COUNT(name) ((void)0)
#endif
} // namespace

struct WasapiAudioSink::Impl {
    enum class State { NotStarted, StartFailed, Started, Joined };
    enum class BorrowResult { Acquired, Closed, Transient };
    engine::SpscQueue<AudioSinkStatus, 64> statuses;
    std::atomic<bool> running{};
    std::atomic<std::uint32_t> access{};
    std::atomic<std::int32_t> startup_result{E_FAIL};
    std::thread worker;
    HANDLE stop_event{}, ready_event{}, exit_event{};
    engine::RealtimeEngineHost* const engine;
    const std::atomic<bool>* const cancellation;
    std::atomic<State> state{State::NotStarted}; // serialized control owner mutates
    std::atomic<std::uint32_t> master_volume_percent;
#if defined(MGSTC_WASAPI_TESTING)
    detail::WasapiTestHooks* const hooks;
    const std::uintptr_t generation{test_generation_sequence.fetch_add(1) + 1};
    std::atomic<std::uint32_t> initial_renders{}, loop_renders{};
    std::atomic<std::uint32_t> borrow_success{}, borrow_closed{}, borrow_transient{};
    std::atomic<std::uint32_t> releases_normal{}, releases_silent{}, starts{}, stops{};
    std::atomic<std::uint32_t> render_runtime_errors{};
    std::atomic<std::uint64_t> total_render_ns{}, max_render_ns{}, first_nonzero_pcm_ns{};
    std::array<std::atomic<std::uint32_t>, 8> render_duration_histogram{};
    std::atomic<std::uint32_t> render_deadline_misses{};
    void at(Stage stage) noexcept {
        if (hooks != nullptr && hooks->at != nullptr) hooks->at(hooks->context, stage);
    }
#endif

    explicit Impl(engine::RealtimeEngineHost& host, std::uint32_t volume,
        const std::atomic<bool>* cancel
#if defined(MGSTC_WASAPI_TESTING)
        , detail::WasapiTestHooks* test_hooks
#endif
        ) : engine(&host), cancellation(cancel), master_volume_percent(volume)
#if defined(MGSTC_WASAPI_TESTING)
        , hooks(test_hooks)
#endif
    {
        HANDLE* handles[]{&stop_event, &ready_event, &exit_event};
        for (int i = 0; i != 3; ++i) {
#if defined(MGSTC_WASAPI_TESTING)
            if (hooks != nullptr && hooks->fail_event_index.load() == i) continue;
#endif
            *handles[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        }
    }

    ~Impl() {
        // Only the sink/control owner destroys Impl, never its borrowed worker.
        // Safety fallback for exceptional owner teardown; no detach or self-owning
        // lambda can transfer the last release to the realtime thread.
        close();
        if (worker.joinable()) worker.join();
        for (HANDLE handle : {stop_event, ready_event, exit_event})
            if (handle != nullptr) CloseHandle(handle);
    }

    bool eventsReady() const noexcept {
        return stop_event != nullptr && ready_event != nullptr && exit_event != nullptr;
    }
    bool closed() noexcept {
        // The service owns this immutable view until join. Observing its intent
        // closes the SAME admission word; the flag alone is not the barrier.
        if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
            close();
        return (access.load(std::memory_order_acquire) & kClosed) != 0;
    }
    void close() noexcept {
        // This RMW linearizes against borrow's CAS. A successful borrower is
        // counted before close, or observes the closed bit and never touches Host.
        access.fetch_or(kClosed, std::memory_order_acq_rel);
        if (stop_event != nullptr) SetEvent(stop_event);
    }
    BorrowResult borrow() noexcept {
        if (closed()) {
            WASAPI_COUNT(borrow_closed);
            return BorrowResult::Closed;
        }
#if defined(MGSTC_WASAPI_TESTING)
        if (hooks != nullptr && hooks->transient_borrow_failures.load() != 0) {
            hooks->transient_borrow_failures.fetch_sub(1);
            if (closed()) { WASAPI_COUNT(borrow_closed); return BorrowResult::Closed; }
            WASAPI_COUNT(borrow_transient);
            return BorrowResult::Transient;
        }
#endif
        auto expected = access.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt != kBorrowAttempts; ++attempt) {
            if ((expected & kClosed) != 0) {
                WASAPI_COUNT(borrow_closed);
                return BorrowResult::Closed;
            }
            if (access.compare_exchange_weak(expected, expected + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                WASAPI_COUNT(borrow_success);
                return BorrowResult::Acquired;
            }
        }
        // A final acquire distinguishes closed from a bounded/spurious failure.
        if (closed()) { WASAPI_COUNT(borrow_closed); return BorrowResult::Closed; }
        WASAPI_COUNT(borrow_transient);
        return BorrowResult::Transient;
    }
    void release() noexcept {
        // Does not release ownership, invoke a destructor, or wake/wait on UI.
        access.fetch_sub(1, std::memory_order_release);
    }
    void push(AudioSinkStatusType type, HRESULT error = S_OK) noexcept {
        static_cast<void>(statuses.tryPush({type, static_cast<std::int32_t>(error)}));
    }
    void signalStartup(HRESULT result) noexcept {
        startup_result.store(static_cast<std::int32_t>(result), std::memory_order_release);
        SetEvent(ready_event);
    }
    void applyMasterVolume(std::span<float> samples) const noexcept {
        const auto gain = static_cast<float>(master_volume_percent.load(
            std::memory_order_relaxed)) / 100.0F;
        for (auto& sample : samples) sample *= gain;
    }

    // All WASAPI interfaces, matching buffer calls and COM final releases remain
    // on this worker. Test builds substitute calls, not a second render algorithm.
    struct DeviceSession {
        Impl& owner;
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render_client;
        HANDLE audio_event{};
        UINT32 frames{};
        bool com_initialized{}, started{};
#if defined(MGSTC_WASAPI_TESTING)
        std::array<float, 8192> fake_buffer{};
        bool fake() const noexcept { return owner.hooks != nullptr && owner.hooks->fake_device; }
#endif
        explicit DeviceSession(Impl& value) noexcept : owner(value) {}
        ~DeviceSession() {
            render_client.Reset();
            client.Reset();
            device.Reset();
            enumerator.Reset();
            if (audio_event != nullptr) CloseHandle(audio_event);
            if (com_initialized) CoUninitialize();
        }
        HRESULT initialize() noexcept {
#if defined(MGSTC_WASAPI_TESTING)
            if (owner.hooks != nullptr && owner.hooks->fail_initialize.exchange(false))
                return E_FAIL;
            if (fake()) {
                frames = std::clamp(owner.hooks->fake_frames, 1U, 4096U);
                return S_OK;
            }
#endif
            auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            com_initialized = SUCCEEDED(result);
            if (FAILED(result)) return result;
            audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            result = audio_event ? S_OK : HRESULT_FROM_WIN32(GetLastError());
            if (SUCCEEDED(result))
                result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(result))
                result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
            if (SUCCEEDED(result))
                result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(client.GetAddressOf()));
            const auto format = makeFormat();
            if (SUCCEEDED(result)) {
                result = E_FAIL;
                ComPtr<IAudioClient3> client3;
                if (SUCCEEDED(client.As(&client3))) {
                    UINT32 default_period{}, fundamental_period{}, min_period{}, max_period{};
                    if (SUCCEEDED(client3->GetSharedModeEnginePeriod(&format,
                            &default_period, &fundamental_period, &min_period, &max_period))
                        && min_period > 0) {
                        result = client3->InitializeSharedAudioStream(
                            kStreamFlags, min_period, &format, nullptr);
                    }
                }
                if (FAILED(result))
                    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        kStreamFlags, 100'000, 0, &format, nullptr);
                if (FAILED(result))
                    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                        kStreamFlags, 0, 0, &format, nullptr);
            }
            if (SUCCEEDED(result)) result = client->SetEventHandle(audio_event);
            if (SUCCEEDED(result)) result = client->GetBufferSize(&frames);
            if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&render_client));
            return result;
        }
        DWORD wait() noexcept {
#if defined(MGSTC_WASAPI_TESTING)
            if (fake()) {
                const auto result = WaitForSingleObject(owner.stop_event, 1);
                return result == WAIT_TIMEOUT ? WAIT_OBJECT_0 + 1 : result;
            }
#endif
            const HANDLE events[]{owner.stop_event, audio_event};
            return WaitForMultipleObjects(2, events, FALSE, INFINITE);
        }
        HRESULT padding(UINT32& value) noexcept {
#if defined(MGSTC_WASAPI_TESTING)
            if (fake()) { value = 0; return S_OK; }
#endif
            return client->GetCurrentPadding(&value);
        }
        HRESULT getBuffer(UINT32 count, BYTE*& data) noexcept {
#if defined(MGSTC_WASAPI_TESTING)
            if (fake()) { data = reinterpret_cast<BYTE*>(fake_buffer.data()); return S_OK; }
#endif
            return render_client->GetBuffer(count, &data);
        }
        HRESULT releaseBuffer(UINT32 count, bool silent) noexcept {
            if (silent) { WASAPI_OWNER_COUNT(releases_silent); }
            else { WASAPI_OWNER_COUNT(releases_normal); }
#if defined(MGSTC_WASAPI_TESTING)
            if (fake()) return S_OK;
#endif
            return render_client->ReleaseBuffer(count,
                silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        }
        HRESULT start() noexcept {
            WASAPI_OWNER_COUNT(starts);
#if defined(MGSTC_WASAPI_TESTING)
            if (fake()) { started = true; return S_OK; }
#endif
            const auto result = client->Start();
            started = SUCCEEDED(result);
            return result;
        }
        void stop() noexcept {
            if (!started) return;
            WASAPI_OWNER_COUNT(stops);
#if defined(MGSTC_WASAPI_TESTING)
            if (!fake())
#endif
                static_cast<void>(client->Stop());
            started = false;
        }
    };

    BorrowResult renderBuffer(std::span<float> samples, bool initial,
        bool& rendered_ok) noexcept {
        if (initial) { WASAPI_STAGE(BeforeInitialBorrow); }
        else { WASAPI_STAGE(BeforeLoopBorrow); }
        const auto result = borrow();
        if (result != BorrowResult::Acquired) return result;
        if (initial) { WASAPI_STAGE(AfterInitialBorrow); WASAPI_COUNT(initial_renders); }
        else { WASAPI_STAGE(AfterLoopBorrow); WASAPI_COUNT(loop_renders); }
        // Immutable pointer is only dereferenced while externally borrowed.
        // This covers public render's two internal readers AND the gap between.
#if defined(MGSTC_WASAPI_TESTING)
        const auto before_render = std::chrono::steady_clock::now();
#endif
        const auto rendered = engine->render(samples);
#if defined(MGSTC_WASAPI_TESTING)
        const auto after_render = std::chrono::steady_clock::now();
        const auto duration = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                after_render - before_render).count());
        total_render_ns.fetch_add(duration, std::memory_order_relaxed);
        // Single producer: no retry loop or compare/exchange is needed.
        max_render_ns.store(std::max(max_render_ns.load(std::memory_order_relaxed),
            duration), std::memory_order_relaxed);
        constexpr std::array<std::uint64_t, 7> histogram_upper_ns{
            10'000, 25'000, 50'000, 100'000, 250'000, 500'000, 1'000'000};
        std::size_t bucket{};
        while (bucket != histogram_upper_ns.size()
            && duration > histogram_upper_ns[bucket]) ++bucket;
        render_duration_histogram[bucket].fetch_add(1, std::memory_order_relaxed);
        // A mock render-budget proxy, not a measurement of device dropouts.
        const auto budget_ns = static_cast<std::uint64_t>(samples.size() / 2)
            * 1'000'000'000ULL / 48'000;
        if (duration > budget_ns) WASAPI_COUNT(render_deadline_misses);
        if (rendered.error == mgstc::engine::RenderError::Runtime)
            WASAPI_COUNT(render_runtime_errors);
        if (first_nonzero_pcm_ns.load(std::memory_order_relaxed) == 0
            && std::any_of(samples.begin(), samples.end(),
                [](float value) { return std::abs(value) > 1.0e-6F; })) {
            first_nonzero_pcm_ns.store(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after_render.time_since_epoch()).count()), std::memory_order_relaxed);
        }
#endif
        // Tick-level program faults already mute the remainder. They are not
        // device faults, including when the first buffer contains such a tick.
        rendered_ok = rendered.ok() || rendered.error == mgstc::engine::RenderError::Runtime;
        if (initial) { WASAPI_STAGE(AfterInitialRender); }
        else { WASAPI_STAGE(AfterLoopRender); }
        release();
        applyMasterVolume(samples);
        return result;
    }

    void runWorker() noexcept {
        DeviceSession session(*this);
        WASAPI_STAGE(BeforeInitialize);
        auto result = session.initialize();
        WASAPI_STAGE(AfterInitialize);
        if (SUCCEEDED(result) && closed()) result = E_ABORT;
        BYTE* buffer{};
        if (SUCCEEDED(result)) {
            WASAPI_STAGE(BeforeInitialGetBuffer);
            result = session.getBuffer(session.frames, buffer);
            WASAPI_STAGE(AfterInitialGetBuffer);
        }
        if (SUCCEEDED(result)) {
            bool rendered_ok = true;
            auto samples = std::span<float>(reinterpret_cast<float*>(buffer),
                static_cast<std::size_t>(session.frames) * 2);
            const auto borrowed = renderBuffer(samples, true, rendered_ok);
            WASAPI_STAGE(BeforeInitialRelease);
            const bool silent = borrowed != BorrowResult::Acquired || closed();
            const auto released = session.releaseBuffer(session.frames, silent);
            WASAPI_STAGE(AfterInitialRelease);
            result = borrowed != BorrowResult::Acquired ? E_ABORT
                : (!rendered_ok ? E_FAIL : released);
        }
        if (SUCCEEDED(result)) {
            WASAPI_STAGE(BeforeStart);
            if (closed()) result = E_ABORT;
            else result = session.start();
            WASAPI_STAGE(AfterStart);
            if (SUCCEEDED(result) && closed()) result = E_ABORT;
        }
        if (FAILED(result)) {
            if (result != E_ABORT) push(AudioSinkStatusType::DeviceError, result);
            signalStartup(result);
            if (session.started) {
                WASAPI_STAGE(BeforeStop);
                session.stop();
                WASAPI_STAGE(AfterStop);
            }
            return;
        }
        running.store(true, std::memory_order_release);
        push(AudioSinkStatusType::Started);
        signalStartup(S_OK);
        bool underrun_reported = false;
        while (true) {
            WASAPI_STAGE(BeforeLoopWait);
            const auto wait = session.wait();
            if (wait == WAIT_OBJECT_0 || closed()) break;
            if (wait != WAIT_OBJECT_0 + 1) {
                push(AudioSinkStatusType::DeviceError, HRESULT_FROM_WIN32(GetLastError()));
                break;
            }
            UINT32 padding{};
            result = session.padding(padding);
            if (FAILED(result)) { push(AudioSinkStatusType::DeviceError, result); break; }
            if (padding == 0 && !underrun_reported) {
                underrun_reported = true;
                push(AudioSinkStatusType::Underrun);
            } else if (padding != 0) underrun_reported = false;
            const auto available = session.frames - padding;
            if (available == 0) continue;
            buffer = nullptr;
            result = session.getBuffer(available, buffer);
            WASAPI_STAGE(AfterLoopGetBuffer);
            if (FAILED(result)) { push(AudioSinkStatusType::DeviceError, result); break; }
            bool rendered_ok = true;
            auto samples = std::span<float>(reinterpret_cast<float*>(buffer),
                static_cast<std::size_t>(available) * 2);
            const auto borrowed = renderBuffer(samples, false, rendered_ok);
            WASAPI_STAGE(BeforeLoopRelease);
            const bool silent = borrowed != BorrowResult::Acquired || closed();
            result = session.releaseBuffer(available, silent);
            WASAPI_STAGE(AfterLoopRelease);
            if (FAILED(result)) { push(AudioSinkStatusType::DeviceError, result); break; }
            if (borrowed == BorrowResult::Closed || closed()) break;
            // A transient CAS failure mutes only this period and retries; runtime
            // render faults likewise must not stop a healthy device stream.
            static_cast<void>(rendered_ok);
        }
        WASAPI_STAGE(BeforeStop);
        session.stop();
        WASAPI_STAGE(AfterStop);
        running.store(false, std::memory_order_release);
        push(AudioSinkStatusType::Stopped);
    }
    void run() noexcept {
        runWorker(); // includes Stop and all COM destruction on this worker
        running.store(false, std::memory_order_release);
        WASAPI_STAGE(BeforeExitSignal);
        SetEvent(exit_event); // advisory only; never used as proof of thread exit
        WASAPI_STAGE(AfterExitSignal);
    }
};

WasapiAudioSink::WasapiAudioSink() = default;
WasapiAudioSink::~WasapiAudioSink() {
    requestStop();
    // Normally the service has already reaped while keeping its UI responsive.
    // Unexpected direct destruction still cannot free Host-related state early.
    if (impl_ != nullptr && impl_->worker.joinable()) {
        impl_->worker.join();
        impl_->state = Impl::State::Joined;
    }
}

bool WasapiAudioSink::start(engine::RealtimeEngineHost& engine) {
    return startImpl(engine, nullptr);
}
bool WasapiAudioSink::start(engine::RealtimeEngineHost& engine,
    const std::atomic<bool>& cancellation) {
    return startImpl(engine, &cancellation);
}
bool WasapiAudioSink::startImpl(engine::RealtimeEngineHost& engine,
    const std::atomic<bool>* cancellation) {
    if (!reapStopped()) return false;
    if (cancellation != nullptr && cancellation->load(std::memory_order_acquire))
        return false;
    try {
#if defined(MGSTC_WASAPI_TESTING)
        if (test_hooks_ != nullptr && test_hooks_->fail_allocation.exchange(false))
            throw std::bad_alloc{};
#endif
        auto next = std::make_unique<Impl>(engine, masterVolumePercent(), cancellation
#if defined(MGSTC_WASAPI_TESTING)
            , test_hooks_
#endif
            );
        impl_ = std::move(next);
        if (!impl_->eventsReady()) {
            impl_->state = Impl::State::StartFailed;
            impl_->close();
            return false;
        }
#if defined(MGSTC_WASAPI_TESTING)
        if (test_hooks_ != nullptr && test_hooks_->fail_thread.exchange(false))
            throw std::bad_alloc{};
#endif
        auto* generation = impl_.get();
        impl_->worker = std::thread([generation] { generation->run(); });
        impl_->state = Impl::State::Started;
    } catch (...) {
        if (impl_ != nullptr && impl_->state == Impl::State::NotStarted) {
            impl_->state = Impl::State::StartFailed;
            impl_->close();
        }
        return false;
    }
    const auto wait = WaitForSingleObject(impl_->ready_event, kStartWaitMs);
    const bool started = wait == WAIT_OBJECT_0
        && SUCCEEDED(static_cast<HRESULT>(impl_->startup_result.load(std::memory_order_acquire)));
    if (!started) requestStop();
    return started;
}

void WasapiAudioSink::requestStop() noexcept {
    if (impl_ != nullptr) impl_->close();
}
bool WasapiAudioSink::reapStopped() noexcept {
    if (impl_ == nullptr || impl_->state == Impl::State::NotStarted
        || impl_->state == Impl::State::StartFailed || impl_->state == Impl::State::Joined)
        return true;
    // exit_event can be signalled before the thread epilogue. Only the native
    // thread handle becoming signalled proves that join itself will not wait.
    if (WaitForSingleObject(impl_->worker.native_handle(), 0) != WAIT_OBJECT_0)
        return false;
    impl_->worker.join();
    impl_->state = Impl::State::Joined;
    return true;
}
bool WasapiAudioSink::stopPending() const noexcept {
    return impl_ != nullptr && impl_->state == Impl::State::Started && impl_->closed();
}
void WasapiAudioSink::stop() noexcept {
    requestStop();
    if (impl_ != nullptr && impl_->state == Impl::State::Started)
        static_cast<void>(WaitForSingleObject(impl_->worker.native_handle(), kStopWaitMs));
    static_cast<void>(reapStopped());
}
bool WasapiAudioSink::running() const noexcept {
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}
bool WasapiAudioSink::pollStatus(AudioSinkStatus& status) noexcept {
    return impl_ != nullptr && impl_->statuses.tryPop(status);
}
void WasapiAudioSink::setMasterVolumePercent(std::uint32_t percent) noexcept {
    const auto volume = std::min(percent, 100U);
    master_volume_percent_.store(volume, std::memory_order_relaxed);
    if (impl_ != nullptr) impl_->master_volume_percent.store(volume, std::memory_order_relaxed);
}
std::uint32_t WasapiAudioSink::masterVolumePercent() const noexcept {
    return master_volume_percent_.load(std::memory_order_relaxed);
}

#if defined(MGSTC_WASAPI_TESTING)
void detail::WasapiAudioSinkTestAccess::setHooks(
    WasapiAudioSink& sink, WasapiTestHooks* hooks) noexcept {
    sink.test_hooks_ = hooks;
}
detail::WasapiTestSnapshot detail::WasapiAudioSinkTestAccess::snapshot(
    const WasapiAudioSink& sink) noexcept {
    WasapiTestSnapshot out;
    if (sink.impl_ == nullptr) return out;
    const auto& impl = *sink.impl_;
    out.generation = impl.generation;
    out.host = reinterpret_cast<std::uintptr_t>(impl.engine);
    out.borrow_state = impl.access.load(std::memory_order_acquire);
    out.state = static_cast<WasapiTestGenerationState>(
        static_cast<unsigned>(impl.state.load()) + 1);
    out.worker_started = impl.state == WasapiAudioSink::Impl::State::Started
        || impl.state == WasapiAudioSink::Impl::State::Joined;
    out.joined = impl.state == WasapiAudioSink::Impl::State::Joined;
    out.running = impl.running.load(std::memory_order_acquire);
#define WASAPI_SNAPSHOT(name) out.name = impl.name.load(std::memory_order_relaxed)
    WASAPI_SNAPSHOT(initial_renders); WASAPI_SNAPSHOT(loop_renders);
    WASAPI_SNAPSHOT(borrow_success); WASAPI_SNAPSHOT(borrow_closed); WASAPI_SNAPSHOT(borrow_transient);
    WASAPI_SNAPSHOT(releases_normal); WASAPI_SNAPSHOT(releases_silent);
    WASAPI_SNAPSHOT(starts); WASAPI_SNAPSHOT(stops);
    WASAPI_SNAPSHOT(render_runtime_errors); WASAPI_SNAPSHOT(total_render_ns);
    WASAPI_SNAPSHOT(max_render_ns); WASAPI_SNAPSHOT(first_nonzero_pcm_ns);
    WASAPI_SNAPSHOT(render_deadline_misses);
    for (std::size_t i = 0; i != out.render_duration_histogram.size(); ++i)
        out.render_duration_histogram[i] = impl.render_duration_histogram[i].load(
            std::memory_order_relaxed);
#undef WASAPI_SNAPSHOT
    return out;
}
#endif
#undef WASAPI_STAGE
#undef WASAPI_COUNT
#undef WASAPI_OWNER_COUNT
} // namespace mgstc::audio
