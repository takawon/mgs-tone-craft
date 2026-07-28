#pragma once

#include <memory>

#include "mgstc/audio/audio_sink.hpp"

namespace mgstc::audio {

class WasapiAudioSink final : public AudioSink {
public:
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mgstc::audio
