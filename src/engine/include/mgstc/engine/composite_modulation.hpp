// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/software_lfo.hpp"

namespace mgstc::engine {

[[nodiscard]] inline ModulationParameters clampModulation(
    ModulationParameters settings) noexcept {
    settings.depth = std::min<std::uint8_t>(settings.depth, 127);
    settings.roughness = static_cast<std::int8_t>(std::clamp(
        static_cast<int>(settings.roughness), -127, 127));
    return settings;
}

// ENV count zero is the first 60 Hz key-on tick. Oscillate follows the
// MGSDRV h countdown/reversal exactly, but produces an ENV-side offset.
// Step modes retain that delay and speed countdown while never reversing.
[[nodiscard]] inline std::int32_t modulationOffsetAtCount(
    const ModulationParameters& parameters,
    std::uint32_t count) noexcept {
    if (!parameters.enabled || parameters.roughness == 0) {
        return 0;
    }
    const auto first = softwareLfoFirstUpdateTick(
        parameters.delay, parameters.speed);
    if (count < first) {
        return 0;
    }
    const auto period = softwareLfoSpeedPeriod(parameters.speed);
    const std::uint32_t updates = 1U + (count - first) / period;
    if (parameters.mode != ModulationMode::Oscillate) {
        const auto magnitude = std::abs(static_cast<int>(parameters.roughness));
        const auto direction = parameters.mode == ModulationMode::StepUp
            ? 1 : -1;
        return static_cast<std::int32_t>(updates)
            * magnitude * direction;
    }

    const std::uint32_t depth =
        static_cast<std::uint32_t>(
            std::min<std::uint8_t>(parameters.depth, 127)) + 1U;
    const std::uint32_t first_reverse = (depth >> 1U) + 1U;
    const auto step = static_cast<std::int32_t>(parameters.roughness);
    if (updates < first_reverse) {
        return static_cast<std::int32_t>(updates) * step;
    }
    const auto after_initial = updates - (first_reverse - 1U);
    const auto phase = after_initial % (2U * depth);
    const auto signed_steps = static_cast<std::int32_t>(
        first_reverse - 1U)
        - static_cast<std::int32_t>(std::min(phase, depth))
        + static_cast<std::int32_t>(phase > depth ? phase - depth : 0U);
    return signed_steps * step;
}

[[nodiscard]] inline std::int32_t modulationEffectiveValueAtCount(
    const ModulationParameters& parameters,
    std::uint32_t count,
    std::int32_t base,
    std::int32_t minimum,
    std::int32_t maximum) noexcept {
    return std::clamp(
        base + modulationOffsetAtCount(parameters, count),
        minimum, maximum);
}

}  // namespace mgstc::engine
