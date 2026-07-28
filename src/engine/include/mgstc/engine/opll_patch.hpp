#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace mgstc::engine {

struct OpllOperatorParameters {
    bool amplitude_modulation{};
    bool pitch_modulation{};
    bool sustained_tone{};
    bool key_rate_scaling{};
    std::uint8_t multiplier{};
    std::uint8_t key_scale_level{};
    std::uint8_t total_level{};
    bool waveform{};
    std::uint8_t attack_rate{};
    std::uint8_t decay_rate{};
    std::uint8_t sustain_level{};
    std::uint8_t release_rate{};

    friend bool operator==(
        const OpllOperatorParameters&,
        const OpllOperatorParameters&) = default;
};

struct OpllPatchParameters {
    OpllOperatorParameters modulator{};
    OpllOperatorParameters carrier{};
    std::uint8_t feedback{};

    friend bool operator==(
        const OpllPatchParameters&,
        const OpllPatchParameters&) = default;
};

[[nodiscard]] OpllPatchParameters decodeOpllPatch(
    std::span<const std::uint8_t, 8> registers) noexcept;

[[nodiscard]] std::array<std::uint8_t, 8> encodeOpllPatch(
    const OpllPatchParameters& parameters) noexcept;

[[nodiscard]] OpllPatchParameters defaultOpllPatch() noexcept;

[[nodiscard]] std::optional<OpllPatchParameters> ym2413RomPatch(
    std::uint8_t instrument) noexcept;

}  // namespace mgstc::engine
