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

[[nodiscard]] constexpr std::uint8_t sequenceOutputVolume(
    std::uint8_t envelope_volume,
    std::uint8_t track_volume,
    std::uint8_t master_attenuation = 0,
    std::uint8_t track_attenuation = 0) noexcept {
    const auto sum =
        static_cast<unsigned>(envelope_volume) + track_volume;
    const auto premaster = sum > 15
        ? static_cast<std::uint8_t>(sum - 15)
        : static_cast<std::uint8_t>(0);
    return applyCommonAttenuation(
        premaster,
        master_attenuation,
        track_attenuation);
}

}  // namespace mgstc::engine
