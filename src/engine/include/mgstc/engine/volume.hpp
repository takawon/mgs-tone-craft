#pragma once

#include <algorithm>
#include <cstdint>

namespace mgstc::engine {

[[nodiscard]] constexpr std::uint8_t applyCommonAttenuation(
    std::uint8_t level,
    std::uint8_t master_attenuation = 0,
    std::uint8_t track_attenuation = 0) noexcept {
    const auto attenuation =
        static_cast<unsigned>(master_attenuation) + track_attenuation;
    return attenuation >= level
        ? 0
        : static_cast<std::uint8_t>(level - attenuation);
}

[[nodiscard]] constexpr std::uint8_t sequencePremasterVolume(
    std::uint8_t envelope_volume,
    std::uint8_t track_volume) noexcept {
    const auto sum =
        static_cast<unsigned>(envelope_volume) + track_volume;
    return sum > 15
        ? static_cast<std::uint8_t>(sum - 15)
        : static_cast<std::uint8_t>(0);
}

[[nodiscard]] constexpr std::uint8_t sequenceOutputVolume(
    std::uint8_t envelope_volume,
    std::uint8_t track_volume,
    std::uint8_t master_attenuation = 0,
    std::uint8_t track_attenuation = 0) noexcept {
    return applyCommonAttenuation(
        sequencePremasterVolume(envelope_volume, track_volume),
        master_attenuation,
        track_attenuation);
}

enum class KeyOffDecayWrite : std::uint8_t {
    None,
    Volume,
};

// Track MML `k` after PSG/SCC `@e` key-off. `k==0` writes volume 0 immediately.
// `k>=1` decreases premaster volume by 1 when progress reaches `k-1`.
[[nodiscard]] constexpr KeyOffDecayWrite stepSequenceKeyOffDecay(
    std::uint8_t k,
    std::uint8_t& progress,
    std::uint8_t& premaster_volume) noexcept {
    if (k == 0) {
        premaster_volume = 0;
        return KeyOffDecayWrite::Volume;
    }
    const auto remainder = static_cast<std::uint8_t>(
        static_cast<std::uint8_t>(k - 1U) - progress);
    if (remainder != 0) {
        ++progress;
        return KeyOffDecayWrite::None;
    }
    progress = 0;
    if (premaster_volume == 0) {
        return KeyOffDecayWrite::None;
    }
    --premaster_volume;
    return KeyOffDecayWrite::Volume;
}

}  // namespace mgstc::engine
