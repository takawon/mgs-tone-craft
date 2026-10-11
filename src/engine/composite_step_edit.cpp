// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_step_edit.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"

#include <algorithm>
#include <limits>

namespace mgstc::engine {
namespace {

struct StepMap {
    std::uint32_t pivot;
    int delta;
    [[nodiscard]] std::uint32_t boundary() const noexcept {
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(pivot) + delta);
    }
    [[nodiscard]] bool removed(std::uint32_t count) const noexcept {
        return delta < 0 && count >= boundary() && count < pivot;
    }
    [[nodiscard]] std::uint32_t map(std::uint32_t count) const noexcept {
        return static_cast<std::uint32_t>(wideMap(count));
    }
    [[nodiscard]] std::int64_t wideMap(std::uint32_t count) const noexcept {
        if (removed(count)) return boundary();
        return count >= pivot ? static_cast<std::int64_t>(count) + delta : count;
    }
    [[nodiscard]] bool fits(std::uint32_t count, std::uint32_t limit) const noexcept {
        const auto mapped = wideMap(count);
        return mapped >= 0 && mapped <= limit;
    }
};

bool isSelected(CompositeStepEditLane lane, CompositeStepEditLane target) {
    return lane == CompositeStepEditLane::All || lane == target;
}

bool isReachable(const EnvelopeEvent& event, const EnvelopeTimeline& timeline) {
    return !timeline.loop_start_count || !timeline.loop_end_count
        || event.count < *timeline.loop_end_count
        || (event.count == *timeline.loop_end_count && event.after_loop_start);
}

void setLoopSide(EnvelopeEvent& event, const EnvelopeTimeline& timeline) {
    if (timeline.loop_start_count && event.count < *timeline.loop_start_count)
        event.after_loop_start = false;
    else if (timeline.loop_end_count && event.count > *timeline.loop_end_count)
        event.after_loop_start = false;
    else if ((!timeline.loop_start_count || event.count > *timeline.loop_start_count)
        && (!timeline.loop_end_count || event.count < *timeline.loop_end_count))
        event.after_loop_start = timeline.loop_start_count.has_value();
    // Exact boundary events retain their authored bracket side.
}

bool moveModulation(ModulationParameters& modulation, StepMap mapping,
                    std::uint32_t length) {
    if (!modulation.enabled) return true;
    if (!mapping.fits(modulation.delay, std::min<std::uint32_t>(255, length))) return false;
    const auto delay = mapping.map(modulation.delay);
    if (delay > 255 || delay > length) return false;
    modulation.delay = static_cast<std::uint8_t>(delay);
    return true;
}

bool moveAuto(OpllRegisterAutoLane& automatic, StepMap mapping,
              std::uint32_t length, std::uint8_t initial) {
    if (!automatic.active()) return true;
    const auto original_start = automatic.start_count;
    if (!mapping.fits(original_start, length)) return false;
    if (automatic.mode != OpllRegisterAutoMode::FreeCurve
        || automatic.free_curve.empty()) {
        automatic.start_count = mapping.map(original_start);
        return automatic.start_count <= length;
    }
    const auto spacing = std::max<std::uint32_t>(1, automatic.coarseness);
    const auto original_end = static_cast<std::uint64_t>(original_start)
        + (automatic.free_curve.size() - 1) * spacing;
    if (original_end > std::numeric_limits<std::uint32_t>::max()) return false;
    if (!mapping.fits(static_cast<std::uint32_t>(original_end), length)) return false;
    // A wholly moved curve keeps its original control-point spacing.
    if (original_start >= mapping.pivot || (original_end < mapping.pivot
        && !mapping.removed(static_cast<std::uint32_t>(original_end)))) {
        automatic.start_count = mapping.map(original_start);
        return static_cast<std::uint64_t>(automatic.start_count)
            + (automatic.free_curve.size() - 1) * spacing <= length;
    }
    const auto new_start = mapping.map(original_start);
    const auto new_end = mapping.map(static_cast<std::uint32_t>(original_end));
    if (new_end > length || new_start > new_end) return false;
    const auto valueAt = [&](std::uint32_t at) {
        if (at < original_start) return initial;
        const auto index = std::min<std::size_t>(
            (at - original_start) / spacing, automatic.free_curve.size() - 1);
        return automatic.free_curve[index];
    };
    std::vector<std::uint8_t> values;
    values.reserve(new_end - new_start + 1);
    for (auto count = new_start; count <= new_end; ++count) {
        const auto original = mapping.delta > 0
            ? (count >= mapping.pivot + static_cast<std::uint32_t>(mapping.delta)
                ? count - static_cast<std::uint32_t>(mapping.delta)
                : count >= mapping.pivot
                    ? (mapping.pivot == 0 ? 0 : mapping.pivot - 1) : count)
            : count >= mapping.boundary()
                ? count + static_cast<std::uint32_t>(-mapping.delta) : count;
        values.push_back(valueAt(original));
    }
    automatic.start_count = new_start;
    automatic.coarseness = 1;
    automatic.free_curve = std::move(values);
    return true;
}

} // namespace

CompositeStepEditResult editCompositeSteps(
    const CompositeLayer& source, CompositeStepEditLane lane,
    std::uint32_t start_count, int delta, std::uint32_t maximum_length) {
    CompositeStepEditResult result{false, source, 0, {}};
    const auto reject = [&](const char* error) {
        result.layer = source;
        result.error = error;
        return result;
    };
    if (source.volume_envelope.kind != EnvelopeKind::Sequence)
        return reject("削除/挿入はシーケンス型エンベロープで使用できます。");
    const auto original_length = source.envelope_timeline.length_counts;
    if (start_count > original_length || original_length > 65'535
        || maximum_length == 0 || maximum_length > 65'535
        || static_cast<std::uint8_t>(lane) > static_cast<std::uint8_t>(CompositeStepEditLane::Fb)
        || static_cast<std::int64_t>(start_count) + delta < 0)
        return reject("編集位置がタイムラインの範囲外です。");
    if (delta == 0) { result.accepted = true; return result; }
    const auto new_length = static_cast<std::int64_t>(original_length)
        + (lane == CompositeStepEditLane::All ? delta : 0);
    if (new_length < 1 || new_length > maximum_length
        || static_cast<std::int64_t>(start_count) + delta > maximum_length)
        return reject("編集結果がタイムラインの上限を超えます。");
    StepMap mapping{start_count, delta};
    auto& layer = result.layer;
    auto& timeline = layer.envelope_timeline;
    timeline.length_counts = static_cast<std::uint32_t>(new_length);
    const auto content_limit = lane == CompositeStepEditLane::All
        ? maximum_length : timeline.length_counts;
    if (mapping.boundary() > content_limit)
        return reject("移動先がENDまたは編集上限を超えるため移動できません。");
    if (lane == CompositeStepEditLane::All) {
        if ((timeline.loop_start_count && !mapping.fits(*timeline.loop_start_count, timeline.length_counts))
            || (timeline.loop_end_count && !mapping.fits(*timeline.loop_end_count, timeline.length_counts)))
            return reject("ループ位置が編集範囲に収まりません。");
        if (timeline.loop_start_count)
            timeline.loop_start_count = mapping.map(*timeline.loop_start_count);
        if (timeline.loop_end_count)
            timeline.loop_end_count = mapping.map(*timeline.loop_end_count);
        if (timeline.loop_start_count && timeline.loop_end_count
            && *timeline.loop_start_count >= *timeline.loop_end_count)
            return reject("削除するとループ区間がなくなるため移動できません。");
    }
    const auto moveEvents = [&](std::vector<EnvelopeEvent>& events,
                                CompositeStepEditLane target) {
        if (!isSelected(lane, target)) return true;
        std::vector<EnvelopeEvent> moved;
        moved.reserve(events.size() + 2);
        for (auto event : events) {
            if ((target == CompositeStepEditLane::Timbre
                && event.kind != EnvelopeEventKind::Timbre)
                || (target == CompositeStepEditLane::RegisterWrite
                && event.kind != EnvelopeEventKind::RegisterWrite)) {
                moved.push_back(event);
                continue;
            }
            if (mapping.removed(event.count)) {
                // Pitch is a command stack, not an absolute value. Keep the
                // removed prefix's state at the join, including OPLL's
                // normalization after EACH command (never sum the deltas).
                if (target != CompositeStepEditLane::Pitch) continue;
            }
            if (!mapping.fits(event.count, content_limit)) return false;
            // Interior events are executable loop commands even when older
            // imported data left the boundary-only flag unset. Establish
            // their original side before a move lands exactly on a bracket.
            setLoopSide(event, source.envelope_timeline);
            event.count = mapping.map(event.count);
            if (event.automatic && event.secondary > 0) {
                if (!mapping.fits(static_cast<std::uint32_t>(event.secondary - 1), content_limit))
                    return false;
                event.secondary = static_cast<std::int32_t>(mapping.map(
                    static_cast<std::uint32_t>(event.secondary - 1)) + 1);
            }
            setLoopSide(event, timeline);
            moved.push_back(event);
        }
        std::stable_sort(moved.begin(), moved.end(), [](const auto& a, const auto& b) {
            return a.count < b.count;
        });
        events = std::move(moved);
        return true;
    };
    // Restore the held setting at the retained suffix's join when a removed
    // command supplied it. No repeated y writes or repeated pitch additions
    // are needed for inserted holds.
    const auto restoreState = [&](EnvelopeEventKind kind) {
        std::vector<EnvelopeEvent> restores;
        if (delta >= 0) return restores;
        for (const auto& event : source.timbre_automation) {
            if (event.kind != kind || !mapping.removed(event.count)
                || !isReachable(event, source.envelope_timeline)) continue;
            if (kind == EnvelopeEventKind::RegisterWrite
                && lane == CompositeStepEditLane::All && source.source == TimbreSource::Opll
                && event.value >= 0 && event.value <= 7
                && std::any_of(source.timbre_automation.begin(), source.timbre_automation.end(),
                    [&](const auto& other) {
                        return other.kind == EnvelopeEventKind::Timbre
                            && isReachable(other, source.envelope_timeline)
                            && other.count > event.count && other.count <= start_count;
                    })) continue;
            const auto sameSetting = [&](const EnvelopeEvent& other) {
                return other.kind == kind && (kind != EnvelopeEventKind::RegisterWrite
                    || other.value == event.value);
            };
            if (std::any_of(source.timbre_automation.begin(), source.timbre_automation.end(),
                [&](const auto& other) { return sameSetting(other) && other.count == start_count
                    && isReachable(other, source.envelope_timeline); }))
                continue;
            auto found = std::find_if(restores.begin(), restores.end(), sameSetting);
            if (found == restores.end()) restores.push_back(event);
            else if (found->count <= event.count) *found = event;
        }
        for (auto& restored : restores) {
            setLoopSide(restored, source.envelope_timeline);
            restored.count = mapping.boundary();
            restored.scc_morph.enabled = false;
            setLoopSide(restored, timeline);
        }
        return restores;
    };
    auto restored_tones = isSelected(lane, CompositeStepEditLane::Timbre)
        ? restoreState(EnvelopeEventKind::Timbre) : std::vector<EnvelopeEvent>{};
    auto restored_y = isSelected(lane, CompositeStepEditLane::RegisterWrite)
        ? restoreState(EnvelopeEventKind::RegisterWrite) : std::vector<EnvelopeEvent>{};
    if (!moveEvents(layer.volume_envelope.events, CompositeStepEditLane::Volume)
        || !moveEvents(layer.pitch_envelope.events, CompositeStepEditLane::Pitch)
        || !moveEvents(layer.timbre_automation, CompositeStepEditLane::Timbre)
        || !moveEvents(layer.timbre_automation, CompositeStepEditLane::RegisterWrite))
        return reject("移動先がENDまたは編集上限を超えるため移動できません。");
    layer.timbre_automation.insert(layer.timbre_automation.begin(), restored_y.begin(), restored_y.end());
    layer.timbre_automation.insert(layer.timbre_automation.begin(), restored_tones.begin(), restored_tones.end());
    if (isSelected(lane, CompositeStepEditLane::Volume)) {
        auto sample_layer = source;
        sample_layer.envelope_timeline.loop_start_count.reset();
        sample_layer.envelope_timeline.loop_end_count.reset();
        const auto samples = sampleCompositeVolumeLane(sample_layer, static_cast<int>(start_count));
        const auto value = start_count == 0 ? source.volume : samples.back();
        auto& events = layer.volume_envelope.events;
        const auto addHold = [&](std::uint32_t count) {
            EnvelopeEvent hold{.kind = EnvelopeEventKind::Volume, .value = value, .count = count};
            setLoopSide(hold, timeline);
            events.insert(events.begin(), hold);
        };
        if (delta > 0) {
            addHold(start_count);
            if (delta > 1) addHold(start_count + static_cast<std::uint32_t>(delta) - 1);
        } else if (std::none_of(events.begin(), events.end(), [&](const auto& event) {
            return event.kind == EnvelopeEventKind::Volume && event.count == mapping.boundary();
        })) addHold(mapping.boundary());
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.count < b.count; });
        std::uint32_t previous{};
        for (auto& event : events) {
            if (event.kind != EnvelopeEventKind::Volume) continue;
            if (event.automatic && event.count <= previous) {
                event.automatic = false;
                event.precise = false;
                event.secondary = 0;
            }
            previous = event.count;
        }
    }
    const auto moveLane = [&](CompositeStepEditLane target, OpllRegisterAutoLane& automatic,
                              ModulationParameters& modulation, std::uint8_t initial) {
        return !isSelected(lane, target) || (moveAuto(automatic, mapping, content_limit, initial)
            && moveModulation(modulation, mapping, content_limit));
    };
    const auto tl_initial = source.base_timbre ? source.base_timbre->opll_registers[2] & 63 : 0;
    const auto fb_initial = source.base_timbre ? source.base_timbre->opll_registers[3] & 7 : 0;
    if (!moveLane(CompositeStepEditLane::Tl, layer.opll_tl_auto, layer.opll_tl_modulation,
            static_cast<std::uint8_t>(tl_initial))
        || !moveLane(CompositeStepEditLane::Fb, layer.opll_fb_auto, layer.opll_fb_modulation,
            static_cast<std::uint8_t>(fb_initial))
        || (isSelected(lane, CompositeStepEditLane::Volume)
            && !moveModulation(layer.volume_modulation, mapping, content_limit))
        || (isSelected(lane, CompositeStepEditLane::Pitch)
            && !moveModulation(layer.pitch_modulation, mapping, content_limit)))
        return reject("自動設定の開始位置または自由曲線が編集範囲に収まりません。");
    std::stable_sort(layer.timbre_automation.begin(), layer.timbre_automation.end(),
        [](const auto& a, const auto& b) { return a.count < b.count; });
    if (isSelected(lane, CompositeStepEditLane::Timbre)) {
        std::uint32_t previous{};
        for (auto& event : layer.timbre_automation) {
            if (event.kind != EnvelopeEventKind::Timbre) continue;
            if (event.scc_morph.enabled) {
                if (event.count <= previous
                    || event.scc_morph.intermediate_count >= event.count - previous)
                    return reject("SCCモーフの中間音色を配置するステップが不足します。");
                if (event.scc_morph.explicit_plan
                    && (event.scc_morph.explicit_plan->points.empty()
                        || event.scc_morph.explicit_plan->points.back().event_count != event.count - previous))
                    event.scc_morph.explicit_plan.reset();
            }
            previous = event.count;
        }
    }
    result.accepted = true;
    result.applied_delta = delta;
    return result;
}

} // namespace mgstc::engine
