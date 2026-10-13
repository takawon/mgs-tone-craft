// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_modulation.hpp"

namespace mgstc::engine {

namespace composite_loop_detail {
inline void preserveBoundaryCommandOrder(CompositeLayer& layer,
                                        std::uint32_t count, bool start,
                                        bool group_sides = true) {
    const auto automaticActive = [&](OpllRegisterAutoTarget target) {
        const auto& base = target == OpllRegisterAutoTarget::TotalLevel
            ? layer.opll_tl_auto : layer.opll_fb_auto;
        return compositeRegisterAutoAt(layer, target, count).active()
            || compositeRegisterAutoAt(layer, target, count, false).active()
            || (count ? compositeRegisterAutoAt(layer, target, count - 1).active() : base.active());
    };
    for (auto target : {CompositeModulationTarget::Pitch, CompositeModulationTarget::Volume,
                        CompositeModulationTarget::TotalLevel, CompositeModulationTarget::Feedback}) {
        const bool automatic = target == CompositeModulationTarget::TotalLevel
            ? automaticActive(OpllRegisterAutoTarget::TotalLevel)
            : target == CompositeModulationTarget::Feedback
                && automaticActive(OpllRegisterAutoTarget::Feedback);
        const bool active = automatic || compositeModulationAt(layer, target, count).enabled
            || compositeModulationAt(layer, target, count, false).enabled
            || (count ? compositeModulationAt(layer, target, count - 1).enabled
                      : compositeBaseModulation(layer, target).enabled);
        const bool explicit_profile = std::any_of(layer.modulation_profiles.begin(),
            layer.modulation_profiles.end(), [count, target](const auto& profile) {
                return profile.count == count && profile.target == target;
            });
        if (active && std::none_of(layer.derived_command_placements.begin(),
            layer.derived_command_placements.end(), [count, target](const auto& slot) {
                return slot.count == count && slot.target == target;
            })) {
            const auto loop_start = layer.envelope_timeline.loop_start_count;
            const bool after = !automatic && !explicit_profile && loop_start
                && (target == CompositeModulationTarget::Pitch || target == CompositeModulationTarget::Volume
                    ? count >= *loop_start : count == *loop_start);
            layer.derived_command_placements.push_back({count, target, after, 0});
        }
    }
    struct Slot { bool after; std::uint32_t* order; int priority; };
    std::vector<Slot> commands;
    for (auto& event : layer.timbre_automation)
        if (event.count == count) commands.push_back({event.after_loop_start, &event.command_order, 0});
    for (auto& event : layer.pitch_envelope.events)
        if (event.count == count) commands.push_back({event.after_loop_start, &event.command_order, 3});
    for (auto& profile : layer.register_auto_profiles)
        if (profile.count == count) commands.push_back({profile.after_loop_start, &profile.command_order, 2});
    for (auto& profile : layer.modulation_profiles)
        if (profile.count == count) commands.push_back({profile.after_loop_start, &profile.command_order,
            profile.target == CompositeModulationTarget::Pitch ? 5
                : profile.target == CompositeModulationTarget::Volume ? 7 : 2});
    for (auto& slot : layer.derived_command_placements)
        if (slot.count == count) commands.push_back({slot.after_loop_start, &slot.command_order,
            slot.target == CompositeModulationTarget::Pitch ? 4
                : slot.target == CompositeModulationTarget::Volume ? 6 : 1});
    const bool explicit_order = std::any_of(commands.begin(), commands.end(),
        [](const Slot& command) { return *command.order != 0; });
    std::stable_sort(commands.begin(), commands.end(),
        [start, group_sides, explicit_order](const Slot& a, const Slot& b) {
            // Ordinary legacy counts still serialize before-side first.
            // Explicit ordinals override these dormant side flags.
            if ((group_sides || !explicit_order) && a.after != b.after)
                return group_sides && !start ? a.after : !a.after;
            if (*a.order && *b.order) return *a.order < *b.order;
            if (*a.order != *b.order) return *a.order != 0;
            return a.priority < b.priority;
        });
    std::uint32_t order = 1;
    for (auto& command : commands) *command.order = order++;
}
// Authoring only. Keep repeated relative deltas and register writes exactly;
// flattening a bracket changes grouping, never the commands themselves.
template <class Event>
inline void flattenBoundary(std::vector<Event>& events,
                            std::uint32_t count, bool start) {
    std::stable_sort(events.begin(), events.end(),
        [count, start](const Event& a, const Event& b) {
            if (a.count != b.count) return a.count < b.count;
            if (a.count != count || a.after_loop_start == b.after_loop_start)
                return false;
            return start ? !a.after_loop_start : a.after_loop_start;
        });
    for (auto& event : events) {
        if (event.count == count) event.after_loop_start = false;
    }
}

template <class Operation>
inline void eachEventLane(CompositeLayer& layer, Operation operation) {
    operation(layer.volume_envelope.events);
    operation(layer.pitch_envelope.events);
    operation(layer.timbre_automation);
    operation(layer.modulation_profiles);
    operation(layer.register_auto_profiles);
    operation(layer.derived_command_placements);
}
}  // namespace composite_loop_detail

// A new/moved start makes existing destination commands part of the body.
// Invalid positions are rejected without changing author data. Re-selecting
// the existing start preserves its independently authored prefix and body.
[[nodiscard]] inline bool setCompositeLoopRange(
    CompositeLayer& layer, std::uint32_t start, std::uint32_t end) {
    if (start >= end || end > layer.envelope_timeline.length_counts) return false;
    const auto old_start = layer.envelope_timeline.loop_start_count;
    const auto old_end = layer.envelope_timeline.loop_end_count;
    if (old_start == start && old_end == end) return true;
    if (old_start && old_start != start)
        composite_loop_detail::preserveBoundaryCommandOrder(layer, *old_start, true);
    if (old_end && old_end != end)
        composite_loop_detail::preserveBoundaryCommandOrder(layer, *old_end, false);
    composite_loop_detail::eachEventLane(layer, [&](auto& events) {
        if (old_start && old_start != start)
            composite_loop_detail::flattenBoundary(events, *old_start, true);
        if (old_end && old_end != end)
            composite_loop_detail::flattenBoundary(events, *old_end, false);
    });
    if (old_start != start)
        composite_loop_detail::preserveBoundaryCommandOrder(layer, start, true, false);
    if (old_end != end)
        composite_loop_detail::preserveBoundaryCommandOrder(layer, end, false, false);
    composite_loop_detail::eachEventLane(layer, [&](auto& events) {
        for (auto& event : events) {
            if ((old_start != start && event.count == start)
                || (old_end != end && event.count == end))
                event.after_loop_start = true;
        }
    });
    layer.envelope_timeline.loop_start_count = start;
    layer.envelope_timeline.loop_end_count = end;
    return true;
}

[[nodiscard]] inline bool setCompositeLoopStart(
    CompositeLayer& layer, std::uint32_t count) {
    return setCompositeLoopRange(layer, count,
        layer.envelope_timeline.loop_end_count.value_or(layer.envelope_timeline.length_counts));
}

[[nodiscard]] inline bool setCompositeLoopEnd(
    CompositeLayer& layer, std::uint32_t count) {
    return layer.envelope_timeline.loop_start_count
        && setCompositeLoopRange(layer, *layer.envelope_timeline.loop_start_count, count);
}

inline void clearCompositeLoop(CompositeLayer& layer) {
    const auto start = layer.envelope_timeline.loop_start_count;
    const auto end = layer.envelope_timeline.loop_end_count;
    if (start) composite_loop_detail::preserveBoundaryCommandOrder(layer, *start, true);
    if (end && end != start)
        composite_loop_detail::preserveBoundaryCommandOrder(layer, *end, false);
    composite_loop_detail::eachEventLane(layer, [&](auto& events) {
        if (start) composite_loop_detail::flattenBoundary(events, *start, true);
        if (end && end != start)
            composite_loop_detail::flattenBoundary(events, *end, false);
        // Away from a bracket this flag has no semantic meaning.
        for (auto& event : events) event.after_loop_start = false;
    });
    layer.envelope_timeline.loop_start_count.reset();
    layer.envelope_timeline.loop_end_count.reset();
}

}  // namespace mgstc::engine
