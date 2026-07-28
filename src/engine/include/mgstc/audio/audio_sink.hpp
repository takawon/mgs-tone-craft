#pragma once

#include <cstdint>

namespace mgstc::engine {
class RealtimeEngineHost;
}

namespace mgstc::audio {

enum class AudioSinkStatusType : std::uint8_t {
    Started,
    Stopped,
    Underrun,
    DeviceError,
};

struct AudioSinkStatus {
    AudioSinkStatusType type{AudioSinkStatusType::Stopped};
    std::int32_t native_error{};
};

class AudioSink {
public:
    virtual ~AudioSink() = default;
    [[nodiscard]] virtual bool start(
        engine::RealtimeEngineHost& engine) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual bool pollStatus(
        AudioSinkStatus& status) noexcept = 0;
};

}  // namespace mgstc::audio
