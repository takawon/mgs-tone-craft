#pragma once

#include <cstdint>

#include "mgstc/engine/event.hpp"
#include "mgstc/engine/volume.hpp"

namespace mgstc::engine {

enum class RatePhase : std::uint8_t {
    Attack,
    Decay,
    Sustain,
    Release,
};

struct RateEnvelopeDefinition {
    std::uint8_t attack_level{};
    std::uint8_t attack_rate{};
    std::uint8_t decay_rate{};
    std::uint8_t sustain_level{};
    std::uint8_t sustain_rate{};
    std::uint8_t release_rate{};
};

class RateEnvelopeRuntime {
public:
    explicit RateEnvelopeRuntime(
        RateEnvelopeDefinition definition,
        std::uint8_t track_volume = 15) noexcept;

    void resetForKeyOn() noexcept;
    void keyOff(bool software_release = true) noexcept;
    void setTrackVolume(std::uint8_t track_volume) noexcept;

    [[nodiscard]] MeaningEvent processTick() noexcept;

    [[nodiscard]] std::uint8_t quantizedVolume(
        std::uint8_t master_attenuation = 0,
        std::uint8_t track_attenuation = 0) const noexcept;

    [[nodiscard]] std::uint8_t level() const noexcept {
        return level_;
    }

    [[nodiscard]] RatePhase phase() const noexcept {
        return phase_;
    }

    [[nodiscard]] Tick tick() const noexcept {
        return tick_;
    }

private:
    RateEnvelopeDefinition definition_;
    std::uint8_t track_volume_{15};
    std::uint8_t level_{};
    RatePhase phase_{RatePhase::Attack};
    Tick tick_{};
    bool key_off_pending_{};
};

}  // namespace mgstc::engine
