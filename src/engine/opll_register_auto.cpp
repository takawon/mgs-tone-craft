#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/composite_modulation.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace mgstc::engine {
namespace {

[[nodiscard]] std::uint8_t clampValue(
    OpllRegisterAutoTarget target,
    int value) noexcept {
    const int maximum = static_cast<int>(opllRegisterAutoValueMax(target));
    return static_cast<std::uint8_t>(std::clamp(value, 0, maximum));
}

[[nodiscard]] std::uint8_t coarsenessOrOne(std::uint8_t value) noexcept {
    return value == 0 ? std::uint8_t{1} : value;
}

[[nodiscard]] std::uint8_t speedOrOne(std::uint8_t value) noexcept {
    return value == 0 ? std::uint8_t{1} : value;
}

void appendWrite(
    std::vector<EnvelopeEvent>& events,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t count,
    std::uint8_t value) {
    events.push_back({
        .kind = EnvelopeEventKind::RegisterWrite,
        .value = opllRegisterAutoRegisterNumber(target),
        .secondary = packOpllRegisterAutoByte(
            target, base_register_byte, value),
        .count = count,
    });
}

[[nodiscard]] std::vector<EnvelopeEvent> expandRiseFall(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t length_counts,
    bool rising) {
    std::vector<EnvelopeEvent> events;
    if (lane.start_count > length_counts) {
        return events;
    }
    const auto step = coarsenessOrOne(lane.coarseness);
    const auto delta = speedOrOne(lane.change_speed);
    auto value = clampValue(target, lane.depth);
    const auto stop = clampValue(target, lane.stop_position);
    appendWrite(
        events, target, base_register_byte, lane.start_count, value);
    if (value == stop) {
        return events;
    }
    auto count = lane.start_count;
    while (count < length_counts) {
        const auto next = count + step;
        if (next > length_counts) {
            break;
        }
        count = next;
        if (rising) {
            if (value >= stop) {
                break;
            }
            const auto room = static_cast<int>(stop) - static_cast<int>(value);
            value = clampValue(
                target, static_cast<int>(value) + std::min<int>(delta, room));
        } else {
            if (value <= stop) {
                break;
            }
            const auto room = static_cast<int>(value) - static_cast<int>(stop);
            value = clampValue(
                target, static_cast<int>(value) - std::min<int>(delta, room));
        }
        appendWrite(
            events, target, base_register_byte, count, value);
        if (value == stop) {
            break;
        }
    }
    return events;
}

[[nodiscard]] std::vector<EnvelopeEvent> expandLfo(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t length_counts) {
    std::vector<EnvelopeEvent> events;
    if (lane.start_count > length_counts) {
        return events;
    }
    const auto step = coarsenessOrOne(lane.coarseness);
    const auto half_period = static_cast<int>(speedOrOne(lane.change_speed));
    const auto centre = clampValue(target, lane.stop_position);
    const auto amplitude = clampValue(target, lane.depth);
    const int maximum = static_cast<int>(opllRegisterAutoValueMax(target));
    const int low = std::max(0, static_cast<int>(centre) - amplitude);
    const int high = std::min(maximum, static_cast<int>(centre) + amplitude);
    if (low == high) {
        appendWrite(
            events, target, base_register_byte, lane.start_count,
            static_cast<std::uint8_t>(low));
        return events;
    }
    const int span = high - low;
    int phase = 0;
    bool ascending = true;
    for (std::uint32_t count = lane.start_count; count <= length_counts;
         count += step) {
        const int value = low
            + (span * phase + half_period) / (half_period * 2);
        appendWrite(
            events, target, base_register_byte, count,
            clampValue(target, value));
        if (ascending) {
            ++phase;
            if (phase >= half_period * 2) {
                phase = half_period * 2;
                ascending = false;
            }
        } else {
            --phase;
            if (phase <= 0) {
                phase = 0;
                ascending = true;
            }
        }
        if (count > length_counts - step) {
            break;
        }
    }
    return events;
}

[[nodiscard]] std::vector<EnvelopeEvent> expandFreeCurve(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t length_counts) {
    std::vector<EnvelopeEvent> events;
    if (lane.free_curve.empty() || lane.start_count > length_counts) {
        return events;
    }
    const auto step = coarsenessOrOne(lane.coarseness);
    for (std::size_t index = 0; index < lane.free_curve.size(); ++index) {
        const auto count = lane.start_count
            + static_cast<std::uint32_t>(index) * step;
        if (count > length_counts) {
            break;
        }
        appendWrite(
            events, target, base_register_byte, count,
            clampValue(target, lane.free_curve[index]));
    }
    return events;
}

[[nodiscard]] std::uint8_t fieldValueFromPacked(
    OpllRegisterAutoTarget target,
    std::uint8_t packed) noexcept {
    if (target == OpllRegisterAutoTarget::TotalLevel) {
        return static_cast<std::uint8_t>(packed & 0x3F);
    }
    return static_cast<std::uint8_t>(packed & 0x07);
}

[[nodiscard]] std::map<std::uint32_t, std::uint8_t> fieldSchedule(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint32_t length_counts) {
    std::map<std::uint32_t, std::uint8_t> schedule;
    // base 0 → packed byte holds only the field bits.
    for (const auto& event : expandOpllRegisterAuto(
             lane, target, 0, length_counts)) {
        schedule[event.count] = fieldValueFromPacked(
            target, static_cast<std::uint8_t>(event.secondary));
    }
    return schedule;
}

struct AutomationAtCount {
    std::vector<const EnvelopeEvent*> timbres;
    std::vector<const EnvelopeEvent*> register_writes;
};

[[nodiscard]] std::map<std::uint32_t, AutomationAtCount> groupAutomation(
    const CompositeLayer& layer);

// Profile evaluation is compilation/authoring work, never an audio callback.
// Schedules are expanded once; evaluating a point does not regenerate curves.
[[nodiscard]] std::vector<EnvelopeEvent> expandProfileRegisterAutos(
    const CompositeLayer& layer, const TimbreLibrary* library,
    bool expand_tl, bool expand_fb) {
    using Schedule = std::map<std::uint32_t, std::uint8_t>;
    std::vector<std::pair<const OpllRegisterAutoLane*, Schedule>> schedules;
    const auto add_schedule = [&](const OpllRegisterAutoLane& lane,
                                  OpllRegisterAutoTarget target) {
        schedules.emplace_back(&lane,
            fieldSchedule(lane, target, layer.envelope_timeline.length_counts));
    };
    add_schedule(layer.opll_tl_auto, OpllRegisterAutoTarget::TotalLevel);
    add_schedule(layer.opll_fb_auto, OpllRegisterAutoTarget::Feedback);
    for (const auto& profile : layer.register_auto_profiles)
        add_schedule(profile.lane, profile.target);
    const auto auto_value = [&](const OpllRegisterAutoLane& lane,
                                std::uint32_t count) -> std::optional<std::uint8_t> {
        for (const auto& [owner, schedule] : schedules) {
            if (owner != &lane) continue;
            auto it = schedule.upper_bound(count);
            if (it == schedule.begin()) return std::nullopt;
            return std::prev(it)->second;
        }
        return std::nullopt;
    };
    std::array<std::uint8_t, 8> registers{};
    bool have_image = layer.base_timbre && layer.base_timbre->source == TimbreSource::Opll;
    bool on_rom = !have_image;
    if (have_image) registers = layer.base_timbre->opll_registers;
    const auto automation = groupAutomation(layer);
    std::array<std::optional<std::uint8_t>, 2> last_values;
    std::vector<EnvelopeEvent> events;
    struct Action {
        const EnvelopeEvent* manual{};
        const OpllRegisterAutoProfile* automatic{};
        const CompositeModulationProfile* modulation{};
        OpllRegisterAutoTarget target{OpllRegisterAutoTarget::TotalLevel};
        bool after{};
        std::uint32_t order{};
        bool generator{};
    };
    std::map<std::uint32_t, std::vector<Action>> profile_actions;
    for (const auto& profile : layer.register_auto_profiles)
        profile_actions[profile.count].push_back({nullptr, &profile, nullptr,
            profile.target, profile.after_loop_start, profile.command_order});
    for (const auto& profile : layer.modulation_profiles) {
        if (profile.target != CompositeModulationTarget::TotalLevel
            && profile.target != CompositeModulationTarget::Feedback) continue;
        profile_actions[profile.count].push_back({nullptr, nullptr, &profile,
            profile.target == CompositeModulationTarget::TotalLevel
                ? OpllRegisterAutoTarget::TotalLevel : OpllRegisterAutoTarget::Feedback,
            profile.after_loop_start, profile.command_order});
    }
    std::map<std::uint32_t, std::array<const CompositeDerivedCommandPlacement*, 2>> placements;
    for (const auto& placement : layer.derived_command_placements) {
        if (placement.target != CompositeModulationTarget::TotalLevel
            && placement.target != CompositeModulationTarget::Feedback) continue;
        const auto index = placement.target == CompositeModulationTarget::TotalLevel ? 0U : 1U;
        if (!placements[placement.count][index]) placements[placement.count][index] = &placement;
    }
    std::array<const OpllRegisterAutoLane*, 2> lanes{&layer.opll_tl_auto, &layer.opll_fb_auto};
    std::array<const ModulationParameters*, 2> mods{&layer.opll_tl_modulation, &layer.opll_fb_modulation};
    for (std::uint32_t count = 0; count <= layer.envelope_timeline.length_counts; ++count) {
        std::vector<Action> actions;
        if (const auto found = profile_actions.find(count); found != profile_actions.end())
            actions = found->second;
        if (const auto found = automation.find(count); found != automation.end()) {
            for (const auto* event : found->second.timbres)
                actions.push_back({event, nullptr, nullptr, {}, event->after_loop_start, event->command_order});
            for (const auto* event : found->second.register_writes)
                actions.push_back({event, nullptr, nullptr, {}, event->after_loop_start, event->command_order});
        }
        for (auto target : {OpllRegisterAutoTarget::TotalLevel, OpllRegisterAutoTarget::Feedback}) {
            const auto index = static_cast<std::size_t>(target);
            const auto found = placements.find(count);
            const auto* placement = found == placements.end() ? nullptr : found->second[index];
            const auto& lane = *lanes[index];
            const auto& mod = *mods[index];
            const bool anchor = layer.envelope_timeline.loop_start_count
                && count == *layer.envelope_timeline.loop_start_count;
            const bool has_profile = std::any_of(actions.begin(), actions.end(), [target](const auto& action) {
                    return !action.manual && action.target == target;
                });
            actions.push_back({nullptr, nullptr, nullptr, target,
                placement ? placement->after_loop_start : !has_profile && anchor && mod.enabled && !lane.active(),
                placement ? placement->command_order : 0, true});
        }
        const bool loop_end = layer.envelope_timeline.loop_start_count
            && layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_start_count < *layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count <= layer.envelope_timeline.length_counts
            && count == *layer.envelope_timeline.loop_end_count;
        std::stable_sort(actions.begin(), actions.end(), [loop_end](const Action& a, const Action& b) {
            // At ], true denotes an inside command. The unreachable outside
            // side must not change the register image used for inside y.
            if (a.after != b.after) return loop_end ? a.after : !a.after;
            if (a.order && b.order) return a.order < b.order;
            if (a.order != b.order) return a.order != 0;
            if (bool(a.manual) != bool(b.manual)) return bool(a.manual);
            if (a.generator != b.generator) return a.generator;
            return a.manual && b.manual && a.manual->kind == EnvelopeEventKind::Timbre
                && b.manual->kind != EnvelopeEventKind::Timbre;
        });
        for (const auto& action : actions) {
            if (action.manual) {
                const auto& event = *action.manual;
                if (event.kind == EnvelopeEventKind::Timbre) {
                    last_values = {};
                    if (event.timbre_pick == TimbrePick::OpllRom) on_rom = true;
                    else if (const auto image = opllOriginalRegistersForLibraryId(layer, event.target_library_id, library)) {
                        registers = *image; have_image = true; on_rom = false;
                    } else have_image = false;
                } else if (event.value >= 0 && event.value <= 7 && event.secondary >= 0 && event.secondary <= 255) {
                    registers[static_cast<std::size_t>(event.value)] = static_cast<std::uint8_t>(event.secondary);
                    if (event.value == 2 || event.value == 3) last_values[static_cast<std::size_t>(event.value - 2)].reset();
                }
                continue;
            }
            const auto target = action.target;
            if ((target == OpllRegisterAutoTarget::TotalLevel && !expand_tl)
                || (target == OpllRegisterAutoTarget::Feedback && !expand_fb)) continue;
            const auto index = static_cast<std::size_t>(target);
            if (action.automatic) lanes[index] = &action.automatic->lane;
            if (action.modulation) mods[index] = &action.modulation->parameters;
            const auto& lane = *lanes[index];
            const auto& mod = *mods[index];
            if (!have_image || on_rom) continue;
            if (action.generator && lane.active()) {
                const auto schedule = std::find_if(schedules.begin(), schedules.end(),
                    [&lane](const auto& entry) { return entry.first == &lane; });
                if (schedule == schedules.end() || !schedule->second.contains(count)) continue;
            }
            const auto reg = opllRegisterAutoRegisterNumber(target);
            const auto base = fieldValueFromPacked(target, registers[reg]);
            if (action.modulation && !mod.enabled && !lane.active()
                && (!last_values[index] || *last_values[index] == base)) continue;
            const auto value = lane.active() ? auto_value(lane, count)
                : (mod.enabled || action.modulation) ? std::optional<std::uint8_t>{clampValue(target,
                    base + modulationOffsetAtCount(mod, count))} : std::nullopt;
            if (!value || (action.generator && last_values[index] == value)) continue;
            if (action.generator && !lane.active() && !last_values[index] && *value == base) {
                last_values[index] = value;
                continue;
            }
            const auto packed = packOpllRegisterAutoByte(target, registers[reg], *value);
            events.push_back({.kind = EnvelopeEventKind::RegisterWrite, .value = reg,
                .secondary = packed, .count = count, .after_loop_start = action.after,
                .command_order = action.order});
            last_values[index] = value;
            if (lane.active()) registers[reg] = packed;
        }
    }
    return events;
}

[[nodiscard]] std::map<std::uint32_t, AutomationAtCount> groupAutomation(
    const CompositeLayer& layer) {
    std::map<std::uint32_t, AutomationAtCount> by_count;
    for (const auto& event : layer.timbre_automation) {
        if (event.kind == EnvelopeEventKind::Timbre) {
            by_count[event.count].timbres.push_back(&event);
        } else if (event.kind == EnvelopeEventKind::RegisterWrite) {
            by_count[event.count].register_writes.push_back(&event);
        }
    }
    return by_count;
}

}  // namespace

std::uint8_t packOpllRegisterAutoByte(
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint8_t value) noexcept {
    if (target == OpllRegisterAutoTarget::TotalLevel) {
        // Reg 2: KSL[7:6] | TL[5:0]
        return static_cast<std::uint8_t>(
            (base_register_byte & 0xC0) | (value & 0x3F));
    }
    // Reg 3: CAR KSL[7:6] | CAR wave[4] | MOD wave[3] | FB[2:0]
    return static_cast<std::uint8_t>(
        (base_register_byte & 0xF8) | (value & 0x07));
}

std::optional<std::array<std::uint8_t, 8>> opllOriginalRegistersForLibraryId(
    const CompositeLayer& layer,
    std::uint64_t library_id,
    const TimbreLibrary* library) noexcept {
    if (library_id == 0) {
        return std::nullopt;
    }
    if (layer.base_timbre
        && layer.base_timbre->library_id == library_id
        && layer.base_timbre->source == TimbreSource::Opll) {
        return layer.base_timbre->opll_registers;
    }
    if (library != nullptr) {
        if (const auto* entry = library->find(library_id)) {
            if (entry->category == TimbreCategory::Opll) {
                return entry->opll_registers;
            }
        }
    }
    return std::nullopt;
}

OpllActiveTimbreAtCount opllActiveTimbreAt(
    const CompositeLayer& layer,
    std::uint32_t count,
    bool after_loop_start) noexcept {
    OpllActiveTimbreAtCount active;
    if (layer.base_timbre
        && layer.base_timbre->source == TimbreSource::Opll) {
        active.kind = OpllActiveTimbreKind::Original;
        active.library_id = layer.base_timbre->library_id;
    }
    std::vector<const EnvelopeEvent*> timbres;
    for (const auto& event : layer.timbre_automation) {
        if (event.kind == EnvelopeEventKind::Timbre
            && event.count <= count
            && (event.count != count || after_loop_start || !event.after_loop_start)) {
            timbres.push_back(&event);
        }
    }
    std::stable_sort(
        timbres.begin(), timbres.end(),
        [](const EnvelopeEvent* left, const EnvelopeEvent* right) {
            if (left->count != right->count) {
                return left->count < right->count;
            }
            return compositeProfilePrecedes(*left, *right);
        });
    for (const auto* event : timbres) {
        if (event->timbre_pick == TimbrePick::OpllRom) {
            if (event->value < 0 || event->value > 14) {
                continue;
            }
            active.kind = OpllActiveTimbreKind::Rom;
            active.library_id = 0;
            active.rom_number = static_cast<std::uint8_t>(event->value);
        } else if (event->target_library_id != 0) {
            active.kind = OpllActiveTimbreKind::Original;
            active.library_id = event->target_library_id;
            active.rom_number = 0;
        }
    }
    return active;
}

std::optional<std::array<std::uint8_t, 8>> opllOriginalRegisterImageAt(
    const CompositeLayer& layer,
    std::uint32_t count,
    const TimbreLibrary* library,
    bool include_manual_y_at_count,
    bool include_register_auto,
    bool after_loop_start,
    const std::vector<EnvelopeEvent>* expanded_register_events) noexcept {
    if (layer.source != TimbreSource::Opll) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 8> registers{};
    bool on_rom = false;
    bool have_image = false;

    const auto load_original = [&](std::uint64_t library_id) {
        if (const auto regs = opllOriginalRegistersForLibraryId(
                layer, library_id, library)) {
            registers = *regs;
            on_rom = false;
            have_image = true;
            return true;
        }
        have_image = false;
        on_rom = false;
        return false;
    };

    if (layer.base_timbre
        && layer.base_timbre->source == TimbreSource::Opll) {
        if (layer.base_timbre->library_id != 0) {
            load_original(layer.base_timbre->library_id);
        } else {
            registers = layer.base_timbre->opll_registers;
            on_rom = false;
            have_image = true;
        }
    }

    if (!after_loop_start || !layer.modulation_profiles.empty() || !layer.register_auto_profiles.empty()
        || !layer.derived_command_placements.empty()
        || std::any_of(layer.timbre_automation.begin(), layer.timbre_automation.end(),
            [](const auto& event) { return event.command_order != 0; })) {
        struct ImageEvent { const EnvelopeEvent* event; bool derived; };
        std::vector<ImageEvent> ordered;
        const auto owned_derived = include_register_auto && !expanded_register_events
            ? expandOpllLayerRegisterAutos(layer, library) : std::vector<EnvelopeEvent>{};
        const auto& derived = expanded_register_events && include_register_auto
            ? *expanded_register_events : owned_derived;
        for (const auto& event : layer.timbre_automation) {
            if (event.count <= count && (event.kind == EnvelopeEventKind::Timbre
                || event.kind == EnvelopeEventKind::RegisterWrite))
                ordered.push_back({&event, false});
        }
        for (const auto& event : derived)
            if (event.count <= count) ordered.push_back({&event, true});
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            if (a.event->count != b.event->count) return a.event->count < b.event->count;
            if (a.event->after_loop_start != b.event->after_loop_start)
                return !a.event->after_loop_start;
            if (a.event->command_order && b.event->command_order)
                return a.event->command_order < b.event->command_order;
            if (a.event->command_order != b.event->command_order)
                return a.event->command_order != 0;
            if (a.derived != b.derived) return !a.derived;
            return a.event->kind == EnvelopeEventKind::Timbre
                && b.event->kind != EnvelopeEventKind::Timbre;
        });
        for (const auto& item : ordered) {
            const auto& event = *item.event;
            if (event.count == count && event.after_loop_start && !after_loop_start) continue;
            if (event.kind == EnvelopeEventKind::Timbre) {
                if (event.timbre_pick == TimbrePick::OpllRom) {
                    on_rom = true;
                    have_image = false;
                } else load_original(event.target_library_id);
            } else if ((event.count < count || include_manual_y_at_count || item.derived)
                && event.value >= 0 && event.value <= 7
                && event.secondary >= 0 && event.secondary <= 255) {
                registers[static_cast<std::size_t>(event.value)] =
                    static_cast<std::uint8_t>(event.secondary);
                if (!on_rom) have_image = true;
            }
        }
        if (on_rom || !have_image) return std::nullopt;
        return registers;
    }

    const auto automation = groupAutomation(layer);
    for (std::uint32_t at = 0; at <= count; ++at) {
        const auto found = automation.find(at);
        if (found == automation.end()) {
            continue;
        }
        // §7.5: @ then y at the same count.
        for (const auto* event : found->second.timbres) {
            if (event->timbre_pick == TimbrePick::OpllRom) {
                if (event->value >= 0 && event->value <= 14) {
                    on_rom = true;
                    have_image = false;
                }
            } else if (event->target_library_id != 0) {
                load_original(event->target_library_id);
            }
        }
        const bool apply_y =
            at < count || (at == count && include_manual_y_at_count);
        if (!apply_y) {
            continue;
        }
        for (const auto* event : found->second.register_writes) {
            if (event->value < 0 || event->value > 7
                || event->secondary < 0 || event->secondary > 255) {
                continue;
            }
            registers[static_cast<std::size_t>(event->value)] =
                static_cast<std::uint8_t>(event->secondary);
            if (!on_rom) {
                have_image = true;
            }
        }
    }

    if (on_rom || !have_image) {
        return std::nullopt;
    }

    // §7.5: after manual y, TL/FB auto packs onto the held original image.
    if (include_register_auto) {
        const auto length = layer.envelope_timeline.length_counts;
        if (const auto tl = opllRegisterAutoValueAt(
                layer.opll_tl_auto,
                OpllRegisterAutoTarget::TotalLevel,
                count,
                length)) {
            registers[2] = packOpllRegisterAutoByte(
                OpllRegisterAutoTarget::TotalLevel, registers[2], *tl);
        }
        if (const auto fb = opllRegisterAutoValueAt(
                layer.opll_fb_auto,
                OpllRegisterAutoTarget::Feedback,
                count,
                length)) {
            registers[3] = packOpllRegisterAutoByte(
                OpllRegisterAutoTarget::Feedback, registers[3], *fb);
        }
    }
    return registers;
}

bool opllRegisterAutoAvailableAt(
    const CompositeLayer& layer,
    std::uint32_t count,
    bool after_loop_start) noexcept {
    return opllActiveTimbreAt(layer, count, after_loop_start).kind
        == OpllActiveTimbreKind::Original;
}

std::optional<std::size_t> opllRegisterAutoOwnerLayer(
    const CompositeTimbre& timbre,
    OpllRegisterAutoTarget target) noexcept {
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        const auto& layer = timbre.layers[index];
        if (layer.source != TimbreSource::Opll) {
            continue;
        }
        const auto& lane = target == OpllRegisterAutoTarget::TotalLevel
            ? layer.opll_tl_auto
            : layer.opll_fb_auto;
        const auto& modulation = target == OpllRegisterAutoTarget::TotalLevel
            ? layer.opll_tl_modulation
            : layer.opll_fb_modulation;
        const auto mod_target = target == OpllRegisterAutoTarget::TotalLevel
            ? CompositeModulationTarget::TotalLevel : CompositeModulationTarget::Feedback;
        if (lane.active() || modulation.enabled || compositeHasRegisterAuto(layer, target)
            || compositeHasModulation(layer, mod_target)) {
            return index;
        }
    }
    return std::nullopt;
}

bool opllRegisterAutoOwnedByLayer(
    const CompositeTimbre& timbre,
    std::size_t layer_index,
    OpllRegisterAutoTarget target) noexcept {
    if (layer_index >= timbre.layers.size()
        || timbre.layers[layer_index].source != TimbreSource::Opll) {
        return false;
    }
    const auto owner = opllRegisterAutoOwnerLayer(timbre, target);
    return !owner.has_value() || *owner == layer_index;
}

bool enforceOpllRegisterAutoExclusivity(CompositeTimbre& timbre) {
    bool changed = false;
    std::optional<std::size_t> tl_owner;
    std::optional<std::size_t> fb_owner;
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        auto& layer = timbre.layers[index];
        if (layer.source != TimbreSource::Opll) {
            continue;
        }
        const auto profiles_claim = [&](OpllRegisterAutoTarget target,
                                         std::optional<std::size_t>& owner) {
            const auto mod_target = target == OpllRegisterAutoTarget::TotalLevel
                ? CompositeModulationTarget::TotalLevel : CompositeModulationTarget::Feedback;
            const bool profile_active = std::any_of(layer.register_auto_profiles.begin(),
                layer.register_auto_profiles.end(), [target](const auto& profile) {
                    return profile.target == target && profile.lane.active();
                }) || std::any_of(layer.modulation_profiles.begin(), layer.modulation_profiles.end(),
                    [mod_target](const auto& profile) {
                        return profile.target == mod_target && profile.parameters.enabled;
                    });
            if (!profile_active) return;
            if (owner && *owner != index) {
                disableCompositeRegisterAuto(layer, target);
                disableCompositeModulation(layer, mod_target);
                changed = true;
            } else owner = index;
        };
        if (layer.opll_tl_auto.active()) {
            if (tl_owner.has_value()) {
                layer.opll_tl_auto = {};
                changed = true;
            } else {
                tl_owner = index;
            }
        }
        if (layer.opll_tl_modulation.enabled) {
            if (tl_owner.has_value()) {
                layer.opll_tl_modulation.enabled = false;
                changed = true;
            } else {
                tl_owner = index;
            }
        }
        if (layer.opll_fb_auto.active()) {
            if (fb_owner.has_value()) {
                layer.opll_fb_auto = {};
                changed = true;
            } else {
                fb_owner = index;
            }
        }
        if (layer.opll_fb_modulation.enabled) {
            if (fb_owner.has_value()) {
                layer.opll_fb_modulation.enabled = false;
                changed = true;
            } else {
                fb_owner = index;
            }
        }
        profiles_claim(OpllRegisterAutoTarget::TotalLevel, tl_owner);
        profiles_claim(OpllRegisterAutoTarget::Feedback, fb_owner);
    }
    return changed;
}

std::vector<EnvelopeEvent> expandOpllRegisterAuto(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint8_t base_register_byte,
    std::uint32_t length_counts) {
    switch (lane.mode) {
    case OpllRegisterAutoMode::Off:
        return {};
    case OpllRegisterAutoMode::Rise:
        return expandRiseFall(
            lane, target, base_register_byte, length_counts, true);
    case OpllRegisterAutoMode::Fall:
        return expandRiseFall(
            lane, target, base_register_byte, length_counts, false);
    case OpllRegisterAutoMode::Lfo:
        return expandLfo(
            lane, target, base_register_byte, length_counts);
    case OpllRegisterAutoMode::FreeCurve:
        return expandFreeCurve(
            lane, target, base_register_byte, length_counts);
    }
    return {};
}

std::vector<EnvelopeEvent> expandOpllLayerRegisterAutos(
    const CompositeLayer& layer,
    const TimbreLibrary* library,
    bool expand_tl,
    bool expand_fb) {
    if (layer.source != TimbreSource::Opll) {
        return {};
    }
    if (!layer.modulation_profiles.empty() || !layer.register_auto_profiles.empty()
        || !layer.derived_command_placements.empty())
        return expandProfileRegisterAutos(layer, library, expand_tl, expand_fb);
    const auto length = layer.envelope_timeline.length_counts;
    std::map<std::uint32_t, std::uint8_t> tl_schedule;
    std::map<std::uint32_t, std::uint8_t> fb_schedule;
    if (expand_tl) {
        if (layer.opll_tl_auto.active()
            || !layer.opll_tl_modulation.enabled) {
            tl_schedule = fieldSchedule(
                layer.opll_tl_auto,
                OpllRegisterAutoTarget::TotalLevel,
                length);
        }
    }
    if (expand_fb) {
        if (layer.opll_fb_auto.active()
            || !layer.opll_fb_modulation.enabled) {
            fb_schedule = fieldSchedule(
                layer.opll_fb_auto,
                OpllRegisterAutoTarget::Feedback,
                length);
        }
    }
    const bool tl_mod = expand_tl && layer.opll_tl_modulation.enabled
        && !layer.opll_tl_auto.active();
    const bool fb_mod = expand_fb && layer.opll_fb_modulation.enabled
        && !layer.opll_fb_auto.active();
    if (tl_schedule.empty() && fb_schedule.empty()
        && !tl_mod && !fb_mod) {
        return {};
    }

    const auto automation = groupAutomation(layer);
    std::array<std::uint8_t, 8> registers{};
    bool on_rom = false;
    bool have_image = false;

    const auto load_original = [&](std::uint64_t library_id) {
        if (const auto regs = opllOriginalRegistersForLibraryId(
                layer, library_id, library)) {
            registers = *regs;
            on_rom = false;
            have_image = true;
            return true;
        }
        have_image = false;
        on_rom = false;
        return false;
    };

    if (layer.base_timbre
        && layer.base_timbre->source == TimbreSource::Opll) {
        registers = layer.base_timbre->opll_registers;
        on_rom = false;
        have_image = true;
    }

    std::vector<EnvelopeEvent> events;
    std::optional<std::uint8_t> last_tl_mod;
    std::optional<std::uint8_t> last_fb_mod;
    std::uint32_t max_count = length;
    if (!tl_schedule.empty()) {
        max_count = std::max(max_count, tl_schedule.rbegin()->first);
    }
    if (!fb_schedule.empty()) {
        max_count = std::max(max_count, fb_schedule.rbegin()->first);
    }
    if (!automation.empty()) {
        max_count = std::max(max_count, automation.rbegin()->first);
    }

    for (std::uint32_t count = 0; count <= max_count; ++count) {
        if (const auto found = automation.find(count);
            found != automation.end()) {
            // §7.5: @ then y at the same count.
            if (!found->second.timbres.empty()) {
                last_tl_mod.reset();
                last_fb_mod.reset();
            }
            for (const auto* event : found->second.timbres) {
                if (event->timbre_pick == TimbrePick::OpllRom) {
                    if (event->value >= 0 && event->value <= 14) {
                        on_rom = true;
                    }
                } else if (event->target_library_id != 0) {
                    load_original(event->target_library_id);
                }
            }
            for (const auto* event : found->second.register_writes) {
                if (event->value < 0 || event->value > 7
                    || event->secondary < 0 || event->secondary > 255) {
                    continue;
                }
                registers[static_cast<std::size_t>(event->value)] =
                    static_cast<std::uint8_t>(event->secondary);
                if (event->value == 2) {
                    last_tl_mod.reset();
                } else if (event->value == 3) {
                    last_fb_mod.reset();
                }
            }
        }

        const bool emit = !on_rom && have_image;
        if (tl_mod && emit) {
            const auto base = static_cast<int>(registers[2] & 0x3F);
            const auto value = static_cast<std::uint8_t>(std::clamp(
                base + modulationOffsetAtCount(
                    layer.opll_tl_modulation, count), 0, 63));
            if ((!last_tl_mod || *last_tl_mod != value)
                && (value != base || last_tl_mod.has_value())) {
                const auto packed = packOpllRegisterAutoByte(
                    OpllRegisterAutoTarget::TotalLevel,
                    registers[2], value);
                events.push_back({
                    .kind = EnvelopeEventKind::RegisterWrite,
                    .value = 2,
                    .secondary = packed,
                    .count = count,
                    .after_loop_start = layer.envelope_timeline.loop_start_count
                        && count == *layer.envelope_timeline.loop_start_count,
                });
            }
            last_tl_mod = value;
        }
        if (fb_mod && emit) {
            const auto base = static_cast<int>(registers[3] & 0x07);
            const auto value = static_cast<std::uint8_t>(std::clamp(
                base + modulationOffsetAtCount(
                    layer.opll_fb_modulation, count), 0, 7));
            if ((!last_fb_mod || *last_fb_mod != value)
                && (value != base || last_fb_mod.has_value())) {
                const auto packed = packOpllRegisterAutoByte(
                    OpllRegisterAutoTarget::Feedback,
                    registers[3], value);
                events.push_back({
                    .kind = EnvelopeEventKind::RegisterWrite,
                    .value = 3,
                    .secondary = packed,
                    .count = count,
                    .after_loop_start = layer.envelope_timeline.loop_start_count
                        && count == *layer.envelope_timeline.loop_start_count,
                });
            }
            last_fb_mod = value;
        }
        if (const auto tl = tl_schedule.find(count);
            tl != tl_schedule.end() && emit) {
            const auto packed = packOpllRegisterAutoByte(
                OpllRegisterAutoTarget::TotalLevel,
                registers[2],
                tl->second);
            registers[2] = packed;
            events.push_back({
                .kind = EnvelopeEventKind::RegisterWrite,
                .value = 2,
                .secondary = packed,
                .count = count,
            });
        }
        if (const auto fb = fb_schedule.find(count);
            fb != fb_schedule.end() && emit) {
            const auto packed = packOpllRegisterAutoByte(
                OpllRegisterAutoTarget::Feedback,
                registers[3],
                fb->second);
            registers[3] = packed;
            events.push_back({
                .kind = EnvelopeEventKind::RegisterWrite,
                .value = 3,
                .secondary = packed,
                .count = count,
            });
        }
    }
    return events;
}

std::optional<std::uint8_t> opllRegisterAutoValueAt(
    const OpllRegisterAutoLane& lane,
    OpllRegisterAutoTarget target,
    std::uint32_t count,
    std::uint32_t length_counts) {
    const auto events = expandOpllRegisterAuto(
        lane, target, 0, length_counts);
    std::optional<std::uint8_t> current;
    for (const auto& event : events) {
        if (event.count > count) {
            break;
        }
        current = fieldValueFromPacked(
            target, static_cast<std::uint8_t>(event.secondary));
    }
    return current;
}

}  // namespace mgstc::engine
