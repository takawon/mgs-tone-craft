#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "mgstc/audio/audio_sink.hpp"

namespace mgstc::audio {

#if defined(MGSTC_WASAPI_TESTING)
namespace detail { struct WasapiTestHooks; struct WasapiAudioSinkTestAccess; }
#endif

class WasapiAudioSink final : public AudioSink {
public:
    // stop() bounds its wait, retaining an unjoined generation on timeout.
    // UI callers use requestStop()/reapStopped() instead of waiting here.
    static constexpr std::uint32_t kStopWaitMs = 1500;
    static constexpr std::uint32_t kStartWaitMs = 5000;

    WasapiAudioSink();
    ~WasapiAudioSink() override;
    WasapiAudioSink(const WasapiAudioSink&) = delete;
    WasapiAudioSink& operator=(const WasapiAudioSink&) = delete;

    [[nodiscard]] bool start(
        engine::RealtimeEngineHost& engine) override;
    // The cancellation view must remain valid and must not be reset to false
    // until this generation is joined. It is an intent; close() is the barrier.
    [[nodiscard]] bool start(engine::RealtimeEngineHost& engine,
        const std::atomic<bool>& cancellation);
    void stop() noexcept override;
    void requestStop() noexcept;
    [[nodiscard]] bool reapStopped() noexcept;
    [[nodiscard]] bool stopPending() const noexcept;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] bool pollStatus(
        AudioSinkStatus& status) noexcept override;

    void setMasterVolumePercent(std::uint32_t percent) noexcept;
    [[nodiscard]] std::uint32_t masterVolumePercent() const noexcept;

private:
    [[nodiscard]] bool startImpl(engine::RealtimeEngineHost& engine,
        const std::atomic<bool>* cancellation);
    struct Impl;
    // Control calls are externally serialized. This one-slot registry owns the
    // active or retired generation until true thread exit and join. The worker
    // borrows Impl; it never owns or destroys it. Destruction waits for safety.
    std::unique_ptr<Impl> impl_;
    std::atomic<std::uint32_t> master_volume_percent_{100};
#if defined(MGSTC_WASAPI_TESTING)
    friend struct detail::WasapiAudioSinkTestAccess;
    detail::WasapiTestHooks* test_hooks_{};
#endif
};

}  // namespace mgstc::audio
