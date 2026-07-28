#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/envelope_sequence.hpp"

namespace mgstc::engine {

class TrackRuntime {
public:
    [[nodiscard]] bool setSequenceEnvelope(
        std::vector<std::uint8_t> bytecode);
    [[nodiscard]] bool setRateEnvelope(
        RateEnvelopeDefinition definition,
        std::uint8_t track_volume = 15);
    void clear() noexcept;

    void resetForKeyOn() noexcept;
    void keyOff(bool software_release) noexcept;
    void setTrackVolume(std::uint8_t track_volume) noexcept;

    [[nodiscard]] SequenceError processTick(EventBuffer& output);

    [[nodiscard]] bool active() const noexcept {
        return !std::holds_alternative<std::monostate>(envelope_);
    }

    [[nodiscard]] bool rateEnvelope() const noexcept {
        return std::holds_alternative<RateEnvelopeRuntime>(envelope_);
    }

    [[nodiscard]] std::uint8_t trackVolume() const noexcept {
        return track_volume_;
    }

private:
    using EnvelopeRuntime = std::variant<
        std::monostate,
        SequenceEnvelopeRuntime,
        RateEnvelopeRuntime>;

    EnvelopeRuntime envelope_{};
    std::uint8_t track_volume_{15};
};

}  // namespace mgstc::engine
