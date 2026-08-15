#include "mgstc/engine/opll_register_auto.hpp"

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
    std::uint32_t count) noexcept {
    OpllActiveTimbreAtCount active;
    if (layer.base_timbre
        && layer.base_timbre->source == TimbreSource::Opll) {
        active.kind = OpllActiveTimbreKind::Original;
        active.library_id = layer.base_timbre->library_id;
    }
    std::vector<const EnvelopeEvent*> timbres;
    for (const auto& event : layer.timbre_automation) {
        if (event.kind == EnvelopeEventKind::Timbre
            && event.count <= count) {
            timbres.push_back(&event);
        }
    }
    std::stable_sort(
        timbres.begin(), timbres.end(),
        [](const EnvelopeEvent* left, const EnvelopeEvent* right) {
            if (left->count != right->count) {
                return left->count < right->count;
            }
            return false;
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

bool opllRegisterAutoAvailableAt(
    const CompositeLayer& layer,
    std::uint32_t count) noexcept {
    return opllActiveTimbreAt(layer, count).kind
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
        if (lane.active()) {
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
        if (layer.opll_tl_auto.active()) {
            if (tl_owner.has_value()) {
                layer.opll_tl_auto = {};
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
    const auto length = layer.envelope_timeline.length_counts;
    std::map<std::uint32_t, std::uint8_t> tl_schedule;
    std::map<std::uint32_t, std::uint8_t> fb_schedule;
    if (expand_tl) {
        tl_schedule = fieldSchedule(
            layer.opll_tl_auto,
            OpllRegisterAutoTarget::TotalLevel,
            length);
    }
    if (expand_fb) {
        fb_schedule = fieldSchedule(
            layer.opll_fb_auto,
            OpllRegisterAutoTarget::Feedback,
            length);
    }
    if (tl_schedule.empty() && fb_schedule.empty()) {
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
            }
        }

        const bool emit = !on_rom && have_image;
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
