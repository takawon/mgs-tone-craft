#pragma once

#include <cstdint>
#include <memory>
#if defined(MGSTC_STANDALONE_AUDIO_TESTING)
#include <span>
#endif
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/audio/audio_sink.hpp"

namespace mgstc::audio {
#if defined(MGSTC_STANDALONE_AUDIO_TESTING)
namespace detail { struct AsioAudioSinkTestAccess; }
#endif

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
#if defined(MGSTC_STANDALONE_AUDIO_TESTING)
    friend struct detail::AsioAudioSinkTestAccess;
#endif
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#if defined(MGSTC_STANDALONE_AUDIO_TESTING)
namespace detail {
struct AsioAudioSinkTestAccess {
    static bool prepare(AsioAudioSink&, engine::RealtimeEngineHost&, double, std::size_t);
    static void callback(AsioAudioSink&, float*, float*, int);
    static bool renderSource(AsioAudioSink&, std::span<float>);
    static void stopped(AsioAudioSink&);
};
}  // namespace detail
#endif

}  // namespace mgstc::audio
