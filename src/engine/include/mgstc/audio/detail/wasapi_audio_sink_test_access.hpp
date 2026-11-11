#pragma once

// Only the isolated WASAPI test target compiles these hooks and observations.
#if !defined(MGSTC_WASAPI_TESTING)
#error WASAPI test access requires MGSTC_WASAPI_TESTING
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include "mgstc/audio/wasapi_audio_sink.hpp"

namespace mgstc::audio::detail {
enum class WasapiTestStage {
    BeforeInitialize, AfterInitialize,
    BeforeInitialGetBuffer, AfterInitialGetBuffer,
    BeforeInitialBorrow, AfterInitialBorrow, AfterInitialRender,
    BeforeInitialRelease, AfterInitialRelease,
    BeforeStart, AfterStart, BeforeLoopWait,
    AfterLoopGetBuffer, BeforeLoopBorrow, AfterLoopBorrow, AfterLoopRender,
    BeforeLoopRelease, AfterLoopRelease, BeforeStop, AfterStop,
    BeforeExitSignal, AfterExitSignal,
};
enum class WasapiTestGenerationState { None, NotStarted, StartFailed, Started, Joined };
struct WasapiTestHooks {
    void* context{};
    void (*at)(void*, WasapiTestStage) noexcept{};
    bool fake_device{true};
    std::uint32_t fake_frames{64};
    std::atomic<bool> fail_allocation{};
    std::atomic<bool> fail_thread{};
    std::atomic<bool> fail_initialize{};
    std::atomic<int> fail_event_index{-1};
    std::atomic<std::uint32_t> transient_borrow_failures{};
};
struct WasapiTestSnapshot {
    std::uintptr_t generation{};
    std::uintptr_t host{};
    std::uint32_t borrow_state{};
    WasapiTestGenerationState state{WasapiTestGenerationState::None};
    bool worker_started{};
    bool joined{};
    bool running{};
    std::uint32_t initial_renders{}, loop_renders{};
    std::uint32_t borrow_success{}, borrow_closed{}, borrow_transient{};
    std::uint32_t releases_normal{}, releases_silent{}, starts{}, stops{};
    std::uint32_t render_runtime_errors{};
    std::uint64_t total_render_ns{}, max_render_ns{}, first_nonzero_pcm_ns{};
    // Host.render duration buckets: <=10,25,50,100,250,500,1000us; >1000us.
    std::array<std::uint32_t, 8> render_duration_histogram{};
    std::uint32_t render_deadline_misses{};
};
struct WasapiAudioSinkTestAccess {
    static void setHooks(WasapiAudioSink&, WasapiTestHooks*) noexcept;
    [[nodiscard]] static WasapiTestSnapshot snapshot(const WasapiAudioSink&) noexcept;
};
} // namespace mgstc::audio::detail
