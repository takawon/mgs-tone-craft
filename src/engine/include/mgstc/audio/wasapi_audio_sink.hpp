#pragma once

#include <cstdint>
#include <memory>

#include "mgstc/audio/audio_sink.hpp"

namespace mgstc::audio {

class WasapiAudioSink final : public AudioSink {
public:
    // Longest the caller waits for the render worker to leave a stuck WASAPI
    // call before the worker is abandoned instead of joined.
    static constexpr std::uint32_t kStopWaitMs = 1500;
    static constexpr std::uint32_t kStartWaitMs = 5000;

    WasapiAudioSink();
    ~WasapiAudioSink() override;
    WasapiAudioSink(const WasapiAudioSink&) = delete;
    WasapiAudioSink& operator=(const WasapiAudioSink&) = delete;

    [[nodiscard]] bool start(
        engine::RealtimeEngineHost& engine) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] bool pollStatus(
        AudioSinkStatus& status) noexcept override;

    void setMasterVolumePercent(std::uint32_t percent) noexcept;
    [[nodiscard]] std::uint32_t masterVolumePercent() const noexcept;

private:
    void abandonStuckWorker() noexcept;

    struct Impl;
    // shared_ptr so an abandoned render worker keeps its own state (and event
    // handles) alive after the sink has moved on to a fresh Impl.
    std::shared_ptr<Impl> impl_;
};

}  // namespace mgstc::audio
