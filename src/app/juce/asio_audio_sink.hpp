#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/audio/audio_sink.hpp"

namespace mgstc::audio {

class AsioAudioSink final : public AudioSink {
public:
    AsioAudioSink();
    ~AsioAudioSink() override;
    AsioAudioSink(const AsioAudioSink&) = delete;
    AsioAudioSink& operator=(const AsioAudioSink&) = delete;

    [[nodiscard]] std::vector<std::string> driverNames();
    [[nodiscard]] bool open(
        std::string_view driver_name,
        double requested_sample_rate = 0.0,
        int requested_buffer_frames = 0);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;

    [[nodiscard]] bool start(
        engine::RealtimeEngineHost& engine) override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] bool pollStatus(
        AudioSinkStatus& status) noexcept override;

    [[nodiscard]] bool hasControlPanel() const noexcept;
    [[nodiscard]] bool showControlPanel();
    [[nodiscard]] double currentSampleRate() const noexcept;
    [[nodiscard]] std::string selectedDriverName() const;
    [[nodiscard]] std::string statusText() const;
    [[nodiscard]] std::string lastError() const;

    void setMasterVolumePercent(std::uint32_t percent) noexcept;
    [[nodiscard]] std::uint32_t masterVolumePercent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mgstc::audio
