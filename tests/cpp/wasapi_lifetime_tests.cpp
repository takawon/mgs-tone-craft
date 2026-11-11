// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/audio/detail/wasapi_audio_sink_test_access.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>

#include "mgstc/engine/realtime_engine_host.hpp"
#include "realtime_engine_host_test_access.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"

namespace {
using namespace std::chrono_literals;
using mgstc::audio::WasapiAudioSink;
using mgstc::audio::detail::WasapiAudioSinkTestAccess;
using mgstc::audio::detail::WasapiTestGenerationState;
using mgstc::audio::detail::WasapiTestHooks;
using mgstc::audio::detail::WasapiTestSnapshot;
using mgstc::audio::detail::WasapiTestStage;

std::atomic<class StageBarrier*> host_render_gap_barrier{};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class StageBarrier final {
public:
    void arm(WasapiTestStage stage) {
        std::lock_guard lock(mutex_);
        stage_ = stage;
        armed_ = true;
        entered_ = false;
        released_ = false;
    }

    void hit(WasapiTestStage stage) noexcept {
        std::unique_lock lock(mutex_);
        if (!armed_ || stage_ != stage) return;
        armed_ = false;
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, 8s, [this] { return released_; });
    }

    bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    WasapiTestStage stage_{};
    bool armed_{};
    bool entered_{};
    bool released_{};
};

void stageCallback(void* context, WasapiTestStage stage) noexcept {
    static_cast<StageBarrier*>(context)->hit(stage);
}

struct HookGuard final {
    StageBarrier& barrier;
    ~HookGuard() {
        // Keep hook fields stable while the sink may still be in its destructor.
        barrier.release();
    }
};

struct HostGapGuard final {
    StageBarrier& barrier;
    ~HostGapGuard() {
        barrier.release();
        host_render_gap_barrier.store(nullptr, std::memory_order_release);
    }
};

bool waitFor(const auto& predicate, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return predicate();
}

WasapiTestSnapshot snapshot(const WasapiAudioSink& sink) {
    return WasapiAudioSinkTestAccess::snapshot(sink);
}

void reapUntilComplete(WasapiAudioSink& sink) {
    require(waitFor([&] { return sink.reapStopped(); }, 3s),
        "Worker did not terminate and reap after the test released its barrier");
}

void testNormalRetryAndTransientBorrow() {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};

    require(sink.start(host), "Mock WASAPI generation failed to start");
    // Do not inject during the startup fill: startup must abort on a transient
    // miss. This injection targets only a later healthy stream period.
    hooks.transient_borrow_failures.store(1, std::memory_order_relaxed);
    require(waitFor([&] {
        const auto state = snapshot(sink);
        return state.borrow_transient >= 1
            && state.releases_silent >= 1
            && state.loop_renders >= 1;
    }), "Transient loop borrow did not return silence and retry");

    auto state = snapshot(sink);
    require(state.starts == 1, "Initial stream did not start exactly once");
    require(state.initial_renders == 1,
        "Initial fill did not render exactly once");
    require(state.borrow_success >= 2,
        "A later loop did not successfully borrow the Host");
    const auto first_generation = state.generation;

    sink.requestStop();
    require(sink.stopPending(), "A requested stop was not reported pending");
    reapUntilComplete(sink);
    state = snapshot(sink);
    require(state.joined && state.state == WasapiTestGenerationState::Joined,
        "Reap completed without recording a joined generation");
    require(state.stops == 1, "Worker did not stop its mock stream exactly once");

    require(sink.start(host), "Sink did not start after the old worker was joined");
    require(snapshot(sink).generation != first_generation,
        "Restart reused the previous lifetime generation");
    sink.requestStop();
    reapUntilComplete(sink);
}

void testCloseAtInitialStage(WasapiTestStage stage, bool render_expected,
    bool start_expected);

void testCloseBeforeInitialBorrowDoesNotRenderOrStart() {
    testCloseAtInitialStage(WasapiTestStage::BeforeInitialBorrow, false, false);
}
void testCloseBeforeInitialGetBuffer() {
    testCloseAtInitialStage(WasapiTestStage::BeforeInitialGetBuffer, false, false);
}

void testOldGenerationBlocksRestartUntilTrueJoin() {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    require(sink.start(host), "Initial generation failed to start");
    barrier.arm(WasapiTestStage::BeforeStop);

    sink.requestStop();
    require(barrier.waitUntilEntered(), "Worker did not reach the stop barrier");
    const auto old_generation = snapshot(sink).generation;
    require(!sink.reapStopped(), "Reaper treated an exit notification as thread exit");
    require(!sink.start(host), "A second stream started while the old worker was live");
    require(snapshot(sink).generation == old_generation,
        "Rejected restart replaced the still-live generation");

    barrier.release();
    reapUntilComplete(sink);
    require(sink.start(host), "A joined old generation did not permit restart");
    require(snapshot(sink).generation != old_generation,
        "A joined restart reused the retired generation");
    sink.requestStop();
    reapUntilComplete(sink);
}

void testSelectiveStartupFailuresCanRetry() {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};

    hooks.fail_allocation.store(true);
    require(!sink.start(host), "Injected generation allocation failure was ignored");
    hooks.fail_allocation.store(false);
    for (int event_index = 0; event_index < 3; ++event_index) {
        hooks.fail_event_index.store(event_index);
        require(!sink.start(host), "Injected event creation failure was ignored");
        hooks.fail_event_index.store(-1);
    }
    hooks.fail_thread.store(true);
    require(!sink.start(host), "Injected thread creation failure was ignored");
    hooks.fail_thread.store(false);
    hooks.fail_initialize.store(true);
    require(!sink.start(host), "Injected device initialization failure was ignored");
    hooks.fail_initialize.store(false);
    reapUntilComplete(sink);

    hooks.transient_borrow_failures.store(1);
    require(!sink.start(host), "A transient initial-fill borrow miss was reported as started");
    const auto transient_start = snapshot(sink);
    require(transient_start.starts == 0 && transient_start.initial_renders == 0,
        "Initial-fill transient failure rendered or started the stream");
    reapUntilComplete(sink);

    require(sink.start(host), "A clean generation could not start after setup failures");
    const auto state = snapshot(sink);
    require(state.state == WasapiTestGenerationState::Started,
        "Successful retry did not expose a started generation");
    sink.requestStop();
    reapUntilComplete(sink);
}

void testCloseAtInitialStage(WasapiTestStage stage, bool render_expected,
    bool start_expected) {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    barrier.arm(stage);

    std::atomic<bool> start_result{true};
    std::thread starter([&] { start_result.store(sink.start(host)); });
    const bool entered = barrier.waitUntilEntered();
    if (!entered) {
        barrier.release();
        starter.join();
    }
    require(entered, "Worker missed the selected initial-fill stage");
    const bool started_published = waitFor([&] {
        return snapshot(sink).state == WasapiTestGenerationState::Started;
    });
    if (!started_published) {
        barrier.release();
        starter.join();
    }
    require(started_published,
        "Started state was not published after the worker entered the initial-fill stage");
    sink.requestStop();
    require(!sink.reapStopped(), "Worker was reaped while its stage was held");
    const auto held = snapshot(sink);
    const bool borrowed = stage == WasapiTestStage::AfterInitialBorrow
        || stage == WasapiTestStage::AfterInitialRender;
    require((held.borrow_state & 0x7fffffffU) == (borrowed ? 1U : 0U),
        "Close changed the active initial Host-borrow count");
    barrier.release();
    starter.join();
    reapUntilComplete(sink);

    const auto state = snapshot(sink);
    require(!start_result.load(), "Closed initial generation reported startup success");
    require((state.initial_renders == 1) == render_expected,
        "Close at an initial stage rendered an unexpected number of buffers");
    require(state.starts == (start_expected ? 1U : 0U),
        "Start crossed the close boundary unexpectedly");
    require(state.stops == (start_expected ? 1U : 0U),
        "Started stream was not stopped on the worker after close");
}

void testCloseAtLoopStage(WasapiTestStage stage, bool render_expected,
    bool borrow_held) {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    require(sink.start(host), "Loop-close test stream failed to start");
    barrier.arm(stage);
    require(barrier.waitUntilEntered(), "Worker missed the selected loop stage");

    const auto before_close = snapshot(sink);
    sink.requestStop();
    require(sink.stopPending(), "Loop close was not reported pending");
    require(!sink.reapStopped(), "Loop worker was reaped while paused in its stage");
    const auto closed = snapshot(sink);
    require((closed.borrow_state & 0x80000000U) != 0,
        "Loop close did not set the closed bit");
    require((closed.borrow_state & 0x7fffffffU) == (borrow_held ? 1U : 0U),
        "Loop close lost or invented an active Host borrow");
    barrier.release();
    reapUntilComplete(sink);

    const auto after = snapshot(sink);
    require((after.loop_renders > before_close.loop_renders) == render_expected,
        "A close-stage boundary allowed an unexpected Host render");
    require((after.borrow_state & 0x7fffffffU) == 0,
        "Borrow count did not drain after the held loop call returned");
    require(after.starts == 1 && after.stops == 1,
        "Loop-close sequence did not stop its single started stream");
}

void testCloseAfterInitialGetBuffer() {
    testCloseAtInitialStage(WasapiTestStage::AfterInitialGetBuffer, false, false);
}
void testCloseAfterInitialBorrow() {
    testCloseAtInitialStage(WasapiTestStage::AfterInitialBorrow, true, false);
}
void testCloseAfterInitialRender() {
    testCloseAtInitialStage(WasapiTestStage::AfterInitialRender, true, false);
}
void testCloseBeforeInitialRelease() {
    testCloseAtInitialStage(WasapiTestStage::BeforeInitialRelease, true, false);
}
void testCloseAfterInitialRelease() {
    testCloseAtInitialStage(WasapiTestStage::AfterInitialRelease, true, false);
}
void testCloseBeforeStart() {
    testCloseAtInitialStage(WasapiTestStage::BeforeStart, true, false);
}
void testCloseAfterStart() {
    testCloseAtInitialStage(WasapiTestStage::AfterStart, true, true);
}

void testCloseBeforeLoopBorrow() {
    testCloseAtLoopStage(WasapiTestStage::BeforeLoopBorrow, false, false);
}
void testCloseAfterLoopGetBuffer() {
    testCloseAtLoopStage(WasapiTestStage::AfterLoopGetBuffer, false, false);
}
void testCloseAfterLoopBorrow() {
    testCloseAtLoopStage(WasapiTestStage::AfterLoopBorrow, true, true);
}
void testCloseAfterLoopRender() {
    testCloseAtLoopStage(WasapiTestStage::AfterLoopRender, false, true);
}
void testCloseBeforeLoopRelease() {
    testCloseAtLoopStage(WasapiTestStage::BeforeLoopRelease, false, false);
}
void testCloseAfterLoopRelease() {
    testCloseAtLoopStage(WasapiTestStage::AfterLoopRelease, false, false);
}

void testCloseBetweenHostInternalReadersIsBorrowed() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    StageBarrier barrier;
    HostGapGuard gap_cleanup{barrier};
    WasapiTestHooks hooks;
    hooks.at = nullptr; // The pause belongs to Host::render, not the sink stages.
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    require(host_render_gap_barrier.load(std::memory_order_acquire) == nullptr,
        "A previous Host gap hook was not cleared");
    host_render_gap_barrier.store(&barrier, std::memory_order_release);
    HookGuard cleanup{barrier};
    require(sink.start(host), "Host-gap stream failed to start");
    barrier.arm(WasapiTestStage::AfterLoopBorrow);
    const bool entered = barrier.waitUntilEntered();
    if (!entered) {
        barrier.release();
        sink.requestStop();
        reapUntilComplete(sink);
        host_render_gap_barrier.store(nullptr, std::memory_order_release);
    }
    require(entered, "Host::render did not enter the gap between its internal readers");

    require(RealtimeEngineHostTestAccess::accessState(host) == 0,
        "Host gap still held an internal reader instead of only the external lifetime borrow");
    sink.requestStop();
    const auto closed = snapshot(sink);
    require((closed.borrow_state & 0x80000000U) != 0
            && (closed.borrow_state & 0x7fffffffU) == 1,
        "Close did not preserve the Host borrow during the internal-reader gap");
    require(!sink.reapStopped(), "Worker was reaped while Host::render remained in its gap");

    barrier.release();
    reapUntilComplete(sink);
    host_render_gap_barrier.store(nullptr, std::memory_order_release);
    const auto final = snapshot(sink);
    require((final.borrow_state & 0x7fffffffU) == 0,
        "Host borrow did not drain after the internal-reader gap returned");
    require(final.starts == 1 && final.stops == 1,
        "Gap-close sequence did not stop exactly one stream");
}

void testTimeoutRepeatedStopAndAllocationFailureDoNotReuseGeneration() {
    mgstc::engine::RealtimeEngineHost first_host, next_host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    require(sink.start(first_host), "Initial generation failed before timeout case");
    const auto original = snapshot(sink);
    barrier.arm(WasapiTestStage::BeforeStop);

    const auto first_stop_begin = std::chrono::steady_clock::now();
    sink.stop();
    const auto first_stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - first_stop_begin).count();
    require(barrier.waitUntilEntered(), "Worker did not enter the held stop call");
    require(first_stop_ms >= 1000 && first_stop_ms < 3500,
        "Timed stop did not return near its configured bound");
    require(sink.stopPending() && !sink.reapStopped(),
        "Timed stop abandoned ownership or claimed worker completion");

    const auto second_stop_begin = std::chrono::steady_clock::now();
    sink.stop();
    const auto second_stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - second_stop_begin).count();
    require(second_stop_ms >= 1000 && second_stop_ms < 3500,
        "Repeated stop did not preserve its bound");
    hooks.fail_allocation.store(true);
    require(!sink.start(next_host), "A replacement generation started before old join");
    require(hooks.fail_allocation.load(),
        "Rejected restart consumed allocation fault before ownership was available");
    auto held = snapshot(sink);
    require(held.generation == original.generation && held.host == original.host,
        "Pending generation or immutable Host reference was replaced");

    barrier.release();
    reapUntilComplete(sink);
    require(!sink.start(next_host), "Selective replacement allocation failure was ignored");
    require(!hooks.fail_allocation.load(),
        "Selective allocation fault was not consumed by the eligible retry");
    held = snapshot(sink);
    require(held.generation == original.generation && held.host == original.host
            && held.state == WasapiTestGenerationState::Joined,
        "Allocation failure rewrote or discarded the retired Host binding");

    require(sink.start(next_host), "Clean replacement could not start after allocation failure");
    const auto replacement = snapshot(sink);
    require(replacement.generation != original.generation
            && replacement.host == reinterpret_cast<std::uintptr_t>(&next_host),
        "Successful retry reused the previous generation or Host reference");
    sink.requestStop();
    reapUntilComplete(sink);
    std::cout << "wasapi_stop_timeout_ms=" << first_stop_ms
              << " repeated_stop_ms=" << second_stop_ms << '\n';
}

void testInitialRuntimeErrorStillStartsStream() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "Could not prepare runtime-error fixture");
    require(edit.engine->session().setSequenceEnvelope(0, {0xef, 2, 0x7f}),
        "Could not configure runtime-error fixture");
    require(host.submitProgram(edit), "Could not submit runtime-error fixture");
    require(host.submit(EngineCommand::noteOn(0, 24)),
        "Could not enqueue runtime-error note");

    // The known host-independent fixture fails when its first tick is rendered.
    RealtimeEngineHost reference;
    auto reference_edit = reference.beginProgramEdit();
    require(reference_edit.valid(), "Could not prepare reference runtime fixture");
    require(reference_edit.engine->session().setSequenceEnvelope(0, {0xef, 2, 0x7f}),
        "Could not configure reference runtime fixture");
    require(reference.submitProgram(reference_edit), "Could not submit reference runtime fixture");
    require(reference.submit(EngineCommand::noteOn(0, 24)),
        "Could not enqueue reference runtime note");
    std::array<float, 4096> proof_pcm{};
    const auto proof = reference.render(proof_pcm);
    require(proof.error == RenderError::Runtime,
        "Fixture did not produce a Runtime error in the initial period");

    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    hooks.fake_frames = 2048;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    require(sink.start(host), "Initial Runtime error was treated as a device-start failure");
    require(waitFor([&] { return snapshot(sink).loop_renders != 0; }),
        "Stream did not continue into a later period after Runtime error");
    const auto state = snapshot(sink);
    require(state.initial_renders == 1 && state.starts == 1 && state.loop_renders != 0,
        "Runtime failure did not preserve the healthy stream");
    sink.requestStop();
    reapUntilComplete(sink);
}

void testCancellationDuringInitialBorrowClosesAdmissionAndSkipsStart() {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    std::atomic<bool> cancellation{};
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};
    barrier.arm(WasapiTestStage::AfterInitialBorrow);

    std::atomic<bool> start_result{true};
    std::thread starter([&] { start_result.store(sink.start(host, cancellation)); });
    const bool entered = barrier.waitUntilEntered();
    if (!entered) {
        barrier.release();
        starter.join();
    }
    require(entered, "Worker did not pause with an initial Host borrow held");
    cancellation.store(true, std::memory_order_release);
    const auto held = snapshot(sink);
    require((held.borrow_state & 0x80000000U) == 0
            && (held.borrow_state & 0x7fffffffU) == 1,
        "Cancellation altered the borrow state before the worker observed it");
    barrier.release();
    starter.join();
    reapUntilComplete(sink);

    const auto state = snapshot(sink);
    require(!start_result.load(), "Cancelled initial fill reported a started stream");
    require(state.initial_renders == 1 && state.starts == 0,
        "Cancellation crossed the initial render or device-start boundary unexpectedly");
    require(state.releases_silent == 1 && state.releases_normal == 0,
        "Cancelled initial fill did not release its device buffer silently");
    require((state.borrow_state & 0x80000000U) != 0
            && (state.borrow_state & 0x7fffffffU) == 0,
        "Cancellation did not close admission and drain the Host borrow");
}

void configureAudibleNote(mgstc::engine::RealtimeEngineHost& host,
    std::uint8_t& track) {
    using namespace mgstc::engine;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "Could not prepare audible timbre");
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(*edit.engine, defaultCompositeTimbre(),
            {.polyphonic = true}, &plan),
        "Could not compile audible timbre");
    require(!plan.audible_layers.empty(), "Default timbre has no audible layers");
    track = plan.physicalTrack(plan.audible_layers.front(), 0);
    require(host.submitProgram(edit), "Could not submit audible timbre");
    std::array<float, 2> drain{};
    host.drainPendingCommands(drain);
}

void testMockPerformanceAndLifecycleMeasurements() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    std::uint8_t track{};
    configureAudibleNote(host, track);
    const auto note_on_time = std::chrono::steady_clock::now();
    require(host.submit(EngineCommand::noteOn(track, 60)),
        "Could not queue mock performance Note On");

    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    hooks.fake_frames = 2048;
    WasapiAudioSink sink;
    WasapiAudioSinkTestAccess::setHooks(sink, &hooks);
    HookGuard cleanup{barrier};

    const auto start_begin = std::chrono::steady_clock::now();
    require(sink.start(host), "Mock performance stream failed to start");
    const auto ready_time = std::chrono::steady_clock::now();
    require(waitFor([&] { return snapshot(sink).loop_renders >= 8; }),
        "Mock performance stream did not produce enough periods");
    auto state = snapshot(sink);
    const auto note_latency_ns = state.first_nonzero_pcm_ns
        - std::chrono::duration_cast<std::chrono::nanoseconds>(
            note_on_time.time_since_epoch()).count();
    require(state.render_runtime_errors == 0,
        "Healthy mock performance stream returned a Runtime error");
    require(note_latency_ns > 0,
        "Queued Note On did not produce observable nonzero PCM in the mock stream");
    const auto first_stream_metrics = state;

    RealtimeEngineHost direct_host;
    std::uint8_t direct_track{};
    configureAudibleNote(direct_host, direct_track);
    require(direct_host.submit(EngineCommand::noteOn(direct_track, 60)),
        "Could not queue direct Host control Note On");
    std::array<float, 4096> direct_pcm{};
    std::uint64_t direct_total_ns = 0;
    std::uint64_t direct_max_ns = 0;
    constexpr std::uint32_t direct_callbacks = 16;
    for (std::uint32_t i = 0; i < direct_callbacks; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto rendered = direct_host.render(direct_pcm);
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
        require(rendered.error == RenderError::None,
            "Direct Host control callback failed");
        direct_total_ns += elapsed;
        direct_max_ns = std::max(direct_max_ns, elapsed);
    }

    const auto stop_begin = std::chrono::steady_clock::now();
    sink.requestStop();
    reapUntilComplete(sink);
    const auto stop_end = std::chrono::steady_clock::now();
    const auto stopped_metrics = snapshot(sink);
    const auto histogram_total = std::accumulate(
        stopped_metrics.render_duration_histogram.begin(),
        stopped_metrics.render_duration_histogram.end(), std::uint64_t{});
    require(histogram_total == stopped_metrics.initial_renders + stopped_metrics.loop_renders,
        "Mock callback duration histogram omitted or duplicated a rendered period");
    const auto restart_begin = std::chrono::steady_clock::now();
    require(sink.start(host), "Mock stream failed to restart after join");
    const auto restart_ready = std::chrono::steady_clock::now();
    sink.requestStop();
    reapUntilComplete(sink);

    const auto mock_mean_ns = stopped_metrics.render_runtime_errors == 0
        ? stopped_metrics.total_render_ns / std::max<std::uint64_t>(
            1, stopped_metrics.initial_renders + stopped_metrics.loop_renders)
        : 0;
    std::cout << "wasapi_mock_note_on_to_pcm_ns=" << note_latency_ns
              << " start_ready_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(ready_time - start_begin).count()
              << " restart_ready_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(restart_ready - restart_begin).count()
              << " stop_request_to_join_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(stop_end - stop_begin).count()
              << " instrumented_render_mean_ns=" << mock_mean_ns
              << " instrumented_render_max_ns=" << stopped_metrics.max_render_ns
              << " direct_host_mean_ns=" << direct_total_ns / direct_callbacks
              << " direct_host_max_ns=" << direct_max_ns
              << " mock_periods=" << first_stream_metrics.initial_renders
                    + first_stream_metrics.loop_renders
              << " callback_histogram_le_10us=" << stopped_metrics.render_duration_histogram[0]
              << " callback_histogram_le_25us=" << stopped_metrics.render_duration_histogram[1]
              << " callback_histogram_le_50us=" << stopped_metrics.render_duration_histogram[2]
              << " callback_histogram_le_100us=" << stopped_metrics.render_duration_histogram[3]
              << " callback_histogram_le_250us=" << stopped_metrics.render_duration_histogram[4]
              << " callback_histogram_le_500us=" << stopped_metrics.render_duration_histogram[5]
              << " callback_histogram_le_1000us=" << stopped_metrics.render_duration_histogram[6]
              << " callback_histogram_gt_1000us=" << stopped_metrics.render_duration_histogram[7]
              << " deadline_proxy_misses=" << stopped_metrics.render_deadline_misses
              << " synthetic_device_errors=0 physical_dropout_measurement=unavailable\n";
}

void testDestructorWaitsForWorkerCleanup() {
    mgstc::engine::RealtimeEngineHost host;
    StageBarrier barrier;
    WasapiTestHooks hooks;
    hooks.context = &barrier;
    hooks.at = stageCallback;
    auto sink = std::make_unique<WasapiAudioSink>();
    WasapiAudioSinkTestAccess::setHooks(*sink, &hooks);
    require(sink->start(host), "Generation failed to start before destructor test");
    barrier.arm(WasapiTestStage::BeforeStop);

    std::atomic<bool> destroyed{};
    std::thread destroyer([&] {
        sink.reset();
        destroyed.store(true, std::memory_order_release);
    });
    const bool entered = barrier.waitUntilEntered();
    if (!entered) {
        barrier.release();
        destroyer.join();
    }
    require(entered, "Destructor did not request worker stop");
    std::this_thread::sleep_for(30ms);
    const bool returned_early = destroyed.load(std::memory_order_acquire);
    barrier.release();
    destroyer.join();
    require(!returned_early,
        "Sink destruction returned before the worker could clean up");
    require(destroyed.load(std::memory_order_acquire),
        "Sink destruction did not finish after worker cleanup");
}

using Test = void (*)();
struct Case { const wchar_t* name; Test run; };
constexpr Case cases[] = {
    {L"normal-retry", testNormalRetryAndTransientBorrow},
    {L"close-before-borrow", testCloseBeforeInitialBorrowDoesNotRenderOrStart},
    {L"close-before-initial-getbuffer", testCloseBeforeInitialGetBuffer},
    {L"close-after-initial-getbuffer", testCloseAfterInitialGetBuffer},
    {L"close-after-initial-borrow", testCloseAfterInitialBorrow},
    {L"close-after-initial-render", testCloseAfterInitialRender},
    {L"close-before-initial-release", testCloseBeforeInitialRelease},
    {L"close-after-initial-release", testCloseAfterInitialRelease},
    {L"close-before-start", testCloseBeforeStart},
    {L"close-after-start", testCloseAfterStart},
    {L"close-after-loop-getbuffer", testCloseAfterLoopGetBuffer},
    {L"close-before-loop-borrow", testCloseBeforeLoopBorrow},
    {L"close-after-loop-borrow", testCloseAfterLoopBorrow},
    {L"close-after-loop-render", testCloseAfterLoopRender},
    {L"close-before-loop-release", testCloseBeforeLoopRelease},
    {L"close-after-loop-release", testCloseAfterLoopRelease},
    {L"close-between-host-internal-readers", testCloseBetweenHostInternalReadersIsBorrowed},
    {L"join-before-restart", testOldGenerationBlocksRestartUntilTrueJoin},
    {L"startup-failures", testSelectiveStartupFailuresCanRetry},
    {L"timeout-repeat-stop-oom", testTimeoutRepeatedStopAndAllocationFailureDoNotReuseGeneration},
    {L"initial-runtime-continues", testInitialRuntimeErrorStillStartsStream},
    {L"cancel-initial-borrow", testCancellationDuringInitialBorrowClosesAdmissionAndSkipsStart},
    {L"destructor-waits", testDestructorWaitsForWorkerCleanup},
};

int runChild(const wchar_t* name) {
    for (const auto& test : cases) {
        if (_wcsicmp(test.name, name) == 0) {
            test.run();
            return 0;
        }
    }
    return 2;
}

std::wstring quote(const wchar_t* value) {
    std::wstring result = L"\"";
    for (const auto ch : std::wstring(value)) {
        if (ch == L'\"') result += L'\\';
        result += ch;
    }
    result += L'\"';
    return result;
}

bool runIsolated(const wchar_t* case_name) {
    wchar_t executable[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    require(length > 0 && length < MAX_PATH,
        "Could not obtain the test executable path");

    wchar_t temp_directory[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temp_directory) != 0,
        "Could not obtain a temporary directory for child diagnostics");
    wchar_t log_path[MAX_PATH]{};
    require(GetTempFileNameW(temp_directory, L"WATL", 0, log_path) != 0,
        "Could not create a child diagnostic log path");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log_file = CreateFileW(log_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    require(log_file != INVALID_HANDLE_VALUE,
        "Could not create the child diagnostic log");

    auto command = quote(executable) + L" --child=" + case_name;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log_file;
    startup.hStdError = log_file;
    PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    if (!launched) {
        CloseHandle(log_file);
        DeleteFileW(log_path);
        require(false, "Could not launch isolated WASAPI lifetime test child");
    }
    CloseHandle(log_file); // The child inherited its own handle.
    const auto waited = WaitForSingleObject(process.hProcess, 12000);
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 2000);
    }
    DWORD exit_code = 0;
    const bool got_exit = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    log_file = CreateFileW(log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    std::string diagnostics;
    if (log_file != INVALID_HANDLE_VALUE) {
        char chunk[1024];
        DWORD read = 0;
        while (ReadFile(log_file, chunk, sizeof(chunk), &read, nullptr) && read != 0)
            diagnostics.append(chunk, chunk + read);
        CloseHandle(log_file);
    }
    const bool passed = waited == WAIT_OBJECT_0 && got_exit && exit_code == 0;
    const std::wstring wide_case_name(case_name);
    const std::string narrow_case_name(wide_case_name.begin(), wide_case_name.end());
    std::cerr << "isolated_case=" << narrow_case_name
              << " result=" << (passed ? "pass" : "fail")
              << " wait=" << (waited == WAIT_TIMEOUT ? "timeout" : "signaled")
              << " exit_code=" << (got_exit ? std::to_string(exit_code) : "unavailable")
              << '\n';
    if (!diagnostics.empty())
        std::cerr << "child_log_begin\n" << diagnostics << "child_log_end\n";
    DeleteFileW(log_path);
    return passed;
}

} // namespace

void mgstcHostRenderGapTestHook() noexcept {
    auto* barrier = host_render_gap_barrier.load(std::memory_order_acquire);
    if (barrier != nullptr)
        barrier->hit(WasapiTestStage::AfterLoopBorrow);
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && wcsncmp(argv[1], L"--child=", 8) == 0)
            return runChild(argv[1] + 8);

        for (const auto& test : cases) {
            require(runIsolated(test.name), "Isolated lifetime test failed or timed out");
            std::wcout << L"PASS " << test.name << L'\n';
        }
#if defined(MGSTC_WASAPI_TEST_ASAN)
        std::cout << "performance_measurement_skipped=asan_build\n";
#else
        testMockPerformanceAndLifecycleMeasurements();
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
