// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/software_lfo.hpp"

namespace mgstc::engine {

[[nodiscard]] inline const ModulationParameters& compositeBaseModulation(
    const CompositeLayer& layer, CompositeModulationTarget target) noexcept {
    switch (target) {
    case CompositeModulationTarget::Pitch: return layer.pitch_modulation;
    case CompositeModulationTarget::Volume: return layer.volume_modulation;
    case CompositeModulationTarget::TotalLevel: return layer.opll_tl_modulation;
    case CompositeModulationTarget::Feedback: return layer.opll_fb_modulation;
    }
    return layer.pitch_modulation;
}

template<class Profile>
[[nodiscard]] inline bool compositeProfilePrecedes(const Profile& left, const Profile& right) noexcept {
    if (left.count != right.count) return left.count < right.count;
    if (left.after_loop_start != right.after_loop_start) return !left.after_loop_start;
    if (left.command_order && right.command_order) return left.command_order < right.command_order;
    return left.command_order != right.command_order && left.command_order != 0;
}

[[nodiscard]] inline const ModulationParameters& compositeModulationAt(
    const CompositeLayer& layer, CompositeModulationTarget target,
    std::uint32_t count, bool after = true) noexcept {
    const CompositeModulationProfile* selected = nullptr;
    for (const auto& profile : layer.modulation_profiles) {
        if (profile.target != target || profile.count > count
            || (profile.count == count && !after && profile.after_loop_start))
            continue;
        if (!selected || !compositeProfilePrecedes(profile, *selected))
            selected = &profile;
    }
    return selected ? selected->parameters : compositeBaseModulation(layer, target);
}

[[nodiscard]] inline ModulationParameters& editCompositeModulationAt(
    CompositeLayer& layer, CompositeModulationTarget target,
    std::uint32_t count, bool after) {
    // Volume itself advances the envelope, so its loop anchor and tremolo
    // settings have one body side rather than a zero-time prefix value.
    if (target == CompositeModulationTarget::Volume
        && layer.envelope_timeline.loop_start_count == count) after = true;
    CompositeModulationProfile* selected = nullptr;
    for (auto& profile : layer.modulation_profiles)
        if (profile.target == target && profile.count == count && profile.after_loop_start == after
            && (!selected || !compositeProfilePrecedes(profile, *selected))) selected = &profile;
    if (selected) return selected->parameters;
    const auto current = compositeModulationAt(layer, target, count, after);
    layer.modulation_profiles.push_back({target, count, after, current});
    return layer.modulation_profiles.back().parameters;
}

[[nodiscard]] inline const OpllRegisterAutoLane& compositeRegisterAutoAt(
    const CompositeLayer& layer, OpllRegisterAutoTarget target,
    std::uint32_t count, bool after = true) noexcept {
    const OpllRegisterAutoProfile* selected = nullptr;
    for (const auto& profile : layer.register_auto_profiles) {
        if (profile.target != target || profile.count > count
            || (profile.count == count && !after && profile.after_loop_start))
            continue;
        if (!selected || !compositeProfilePrecedes(profile, *selected))
            selected = &profile;
    }
    return selected ? selected->lane
        : target == OpllRegisterAutoTarget::TotalLevel
            ? layer.opll_tl_auto : layer.opll_fb_auto;
}

[[nodiscard]] inline OpllRegisterAutoLane& editCompositeRegisterAutoAt(
    CompositeLayer& layer, OpllRegisterAutoTarget target,
    std::uint32_t count, bool after) {
    OpllRegisterAutoProfile* selected = nullptr;
    for (auto& profile : layer.register_auto_profiles)
        if (profile.target == target && profile.count == count && profile.after_loop_start == after
            && (!selected || !compositeProfilePrecedes(profile, *selected))) selected = &profile;
    if (selected) return selected->lane;
    const auto current = compositeRegisterAutoAt(layer, target, count, after);
    layer.register_auto_profiles.push_back({target, count, after, current});
    return layer.register_auto_profiles.back().lane;
}

[[nodiscard]] inline bool compositeHasModulation(
    const CompositeLayer& layer, CompositeModulationTarget target) noexcept {
    if (compositeBaseModulation(layer, target).enabled) return true;
    return std::any_of(layer.modulation_profiles.begin(),
        layer.modulation_profiles.end(), [target](const auto& profile) {
            return profile.target == target && profile.parameters.enabled;
        });
}

inline void disableCompositeModulation(
    CompositeLayer& layer, CompositeModulationTarget target) {
    switch (target) {
    case CompositeModulationTarget::Pitch: layer.pitch_modulation.enabled = false; break;
    case CompositeModulationTarget::Volume: layer.volume_modulation.enabled = false; break;
    case CompositeModulationTarget::TotalLevel: layer.opll_tl_modulation.enabled = false; break;
    case CompositeModulationTarget::Feedback: layer.opll_fb_modulation.enabled = false; break;
    }
    std::erase_if(layer.modulation_profiles,
        [target](const auto& profile) { return profile.target == target; });
}

inline void disableCompositeRegisterAuto(
    CompositeLayer& layer, OpllRegisterAutoTarget target) {
    (target == OpllRegisterAutoTarget::TotalLevel
        ? layer.opll_tl_auto : layer.opll_fb_auto) = {};
    std::erase_if(layer.register_auto_profiles,
        [target](const auto& profile) { return profile.target == target; });
}

[[nodiscard]] inline bool compositeHasRegisterAuto(
    const CompositeLayer& layer, OpllRegisterAutoTarget target) noexcept {
    if ((target == OpllRegisterAutoTarget::TotalLevel
            ? layer.opll_tl_auto : layer.opll_fb_auto).active()) return true;
    return std::any_of(layer.register_auto_profiles.begin(),
        layer.register_auto_profiles.end(), [target](const auto& profile) {
            return profile.target == target && profile.lane.active();
        });
}

[[nodiscard]] inline const CompositeDerivedCommandPlacement* compositeDerivedPlacementAt(
    const CompositeLayer& layer, CompositeModulationTarget target, std::uint32_t count) noexcept {
    for (const auto& placement : layer.derived_command_placements)
        if (placement.count == count && placement.target == target) return &placement;
    return nullptr;
}

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
