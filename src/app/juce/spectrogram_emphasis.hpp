// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>

namespace mgstc::app::spectrogram {

[[nodiscard]] constexpr std::uint8_t emphasizeLevel(
    std::uint8_t level,
    std::uint8_t local_average,
    double strength) noexcept {
    strength = std::clamp(strength, 0.0, 1.0);
    if (strength <= 0.0 || level == local_average) {
        return level;
    }

    constexpr double kPeakBoost = 2.0;
    constexpr double kValleyReduction = 0.45;
    const double linear = static_cast<double>(level) / 255.0;
    const double average = static_cast<double>(local_average) / 255.0;
    const double detail = linear - average;
    const double adjustment = detail > 0.0
        ? kPeakBoost * detail * (1.0 - linear)
        : kValleyReduction * detail;
    const double emphasized = std::clamp(
        linear + strength * adjustment,
        0.0,
        1.0);
    return static_cast<std::uint8_t>(emphasized * 255.0 + 0.5);
}

}  // namespace mgstc::app::spectrogram
