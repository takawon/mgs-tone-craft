#pragma once

#include <array>
#include <cstddef>
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

struct RateEnvelopeTrace {
    static constexpr std::size_t kPointCount = 512;
    // Preview window is 2s so the 1s key-off marker sits at the centre.
    static constexpr float kDurationSeconds = 2.0F;
    static constexpr float kKeyOffSeconds = 1.0F;
    static_assert(kKeyOffSeconds * 2.0F == kDurationSeconds);

    std::array<float, kPointCount> level{};
    std::array<float, kPointCount> quantized{};
    bool valid{};
};

[[nodiscard]] RateEnvelopeTrace traceRateEnvelope(
    RateEnvelopeDefinition definition,
    std::uint8_t track_volume,
    bool software_release) noexcept;

enum class RateEnvelopeHandleKind : std::uint8_t {
    AttackStart,
    AttackPeak,
    DecayEnd,
    KeyOff,
    ReleaseEnd,
};

struct RateEnvelopeHandle {
    RateEnvelopeHandleKind kind{RateEnvelopeHandleKind::AttackStart};
    float seconds{};
    std::uint8_t level{};
    bool available{true};
};

struct RateEnvelopeHandleLayout {
    static constexpr std::size_t kCount = 5;
    std::array<RateEnvelopeHandle, kCount> handles{};
};

[[nodiscard]] RateEnvelopeHandleLayout rateEnvelopeHandleLayout(
    RateEnvelopeDefinition definition,
    bool software_release) noexcept;

// Inverse of the 60 Hz @r vertices. One handle can update several of
// AL/AR/DR/SL/SR/RR so the dragged point stays on the execution curve.
[[nodiscard]] RateEnvelopeDefinition applyRateEnvelopeHandleDrag(
    RateEnvelopeDefinition definition,
    RateEnvelopeHandleKind kind,
    float seconds,
    std::uint8_t level,
    bool software_release) noexcept;

}  // namespace mgstc::engine
