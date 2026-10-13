// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_envelope_compile.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/composite_modulation.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/opll_register_auto.hpp"

namespace mgstc::engine {
namespace {

void maybeOptimizeModulationLoop(
    CompositeLayer& layer, const TimbreLibrary* library) {
    auto& timeline = layer.envelope_timeline;
    if (timeline.loop_start_count || timeline.loop_end_count
        || !layer.modulation_profiles.empty()
        || !layer.register_auto_profiles.empty()
        || !layer.derived_command_placements.empty()
        || layer.opll_tl_auto.active()
        || layer.opll_fb_auto.active()) {
        return;
    }
    const ModulationParameters* selected = nullptr;
    enum class Target { Pitch, Volume, Tl, Fb };
    Target target = Target::Pitch;
    std::size_t active_count{};
    const auto select = [&](const ModulationParameters& mod, Target candidate) {
        if (!mod.enabled) {
            return true;
        }
        ++active_count;
        if (!selected) {
            selected = &mod;
            target = candidate;
        }
        return true;
    };
    if (!select(layer.pitch_modulation, Target::Pitch)
        || !select(layer.volume_modulation, Target::Volume)
        || !select(layer.opll_tl_modulation, Target::Tl)
        || !select(layer.opll_fb_modulation, Target::Fb)
        || !selected || (active_count == 1 && selected->roughness == 0)) {
        return;
    }
    const auto& mod = *selected;
    // Later serialized commands are barriers, regardless of target. The
    // legacy default Wait event is ignored by the canonical serializer and
    // must not prevent a loop. Repeated actual writes remain observable.
    const auto later = [](const auto& events, EnvelopeEventKind first_kind,
                          EnvelopeEventKind second_kind) {
        return std::any_of(
            events.begin(), events.end(),
            [first_kind, second_kind](const EnvelopeEvent& event) {
                // A count-zero automatic ramp still changes state later.
                return (event.kind == first_kind || event.kind == second_kind)
                    && (event.count != 0 || event.automatic
                        || event.after_loop_start);
            });
    };
    if (later(layer.volume_envelope.events, EnvelopeEventKind::Volume,
              EnvelopeEventKind::Volume)
        || later(layer.pitch_envelope.events, EnvelopeEventKind::Pitch,
                 EnvelopeEventKind::Pitch)
        || later(layer.timbre_automation, EnvelopeEventKind::Timbre,
                 EnvelopeEventKind::RegisterWrite)) {
        return;
    }
    if (active_count > 1) {
        // Equal steady periods may share one body despite different delays.
        // Each phase is evaluated at the common absolute count; no phase is
        // reset at the loop marker. Absolute Step targets remain excluded.
        const ModulationParameters* mods[] = {
            &layer.pitch_modulation, &layer.volume_modulation,
            &layer.opll_tl_modulation, &layer.opll_fb_modulation};
        std::uint32_t common_period{};
        std::uint32_t steady_start{};
        for (std::size_t index = 0; index < 4; ++index) {
            const auto& settings = *mods[index];
            if (!settings.enabled) continue;
            if (index != 0 && settings.mode != ModulationMode::Oscillate) return;
            const auto speed = softwareLfoSpeedPeriod(settings.speed);
            auto first = softwareLfoFirstUpdateTick(settings.delay, settings.speed);
            auto cycle = speed;
            if (settings.mode == ModulationMode::Oscillate) {
                const auto depth = static_cast<std::uint32_t>(
                    std::min<std::uint8_t>(settings.depth, 127)) + 1U;
                first += ((depth >> 1U) + 1U) * speed;
                cycle = 2U * depth * speed;
            }
            if (common_period && common_period != cycle) return;
            common_period = cycle;
            steady_start = std::max(steady_start, first);
        }
        int bases[] = {0, layer.volume, 0, 0};
        const int maxima[] = {0, 15, 63, 7};
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == EnvelopeEventKind::Volume) bases[1] = event.value;
        }
        if (bases[1] < 0 || bases[1] > 15) return;
        if (layer.opll_tl_modulation.enabled || layer.opll_fb_modulation.enabled) {
            const auto image = opllOriginalRegisterImageAt(layer, 0, library, true, false);
            if (!image) return;
            bases[2] = (*image)[2] & 0x3F;
            bases[3] = (*image)[3] & 0x07;
        }
        // Choose an actual state-change boundary. Otherwise forcing the
        // loop-start volume would add a write absent from the finite stream.
        auto anchor = steady_start;
        for (; anchor < steady_start + common_period; ++anchor) {
            bool changes = false;
            for (std::size_t index = 0; index < 4; ++index) {
                const auto& settings = *mods[index];
                if (!settings.enabled || settings.roughness == 0) continue;
                if (index == 0) {
                    changes |= modulationOffsetAtCount(settings, anchor)
                        != modulationOffsetAtCount(settings, anchor - 1U);
                } else {
                    changes |= modulationEffectiveValueAtCount(
                        settings, anchor, bases[index], 0, maxima[index])
                        != modulationEffectiveValueAtCount(
                            settings, anchor - 1U, bases[index], 0, maxima[index]);
                }
            }
            if (changes) break;
        }
        if (anchor == steady_start + common_period
            || anchor + common_period > timeline.length_counts) return;
        timeline.loop_start_count = anchor;
        timeline.loop_end_count = anchor + common_period;
        return;
    }
    const auto speed = softwareLfoSpeedPeriod(mod.speed);
    const auto first = softwareLfoFirstUpdateTick(
        mod.delay, mod.speed);
    int base = 0;
    int maximum = 0;
    if (target == Target::Volume) {
        base = layer.volume;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == EnvelopeEventKind::Volume) {
                base = event.value;
            }
        }
        if (base < 0 || base > 15) {
            return;
        }
        maximum = 15;
    } else if (target == Target::Tl || target == Target::Fb) {
        const auto image = opllOriginalRegisterImageAt(
            layer, 0, library, true, false);
        if (!image) {
            return;
        }
        base = target == Target::Tl ? (*image)[2] & 0x3F
                                    : (*image)[3] & 0x07;
        maximum = target == Target::Tl ? 63 : 7;
    }
    std::uint32_t start = first;
    std::uint32_t period = speed;
    if (mod.mode == ModulationMode::Oscillate) {
        const auto depth = static_cast<std::uint32_t>(
            std::min<std::uint8_t>(mod.depth, 127)) + 1U;
        // The first reversal has a transient prefix. A relative pitch loop
        // must begin after it so the value just before the body repeats.
        start += ((depth >> 1U) + 1U) * speed;
        period = 2U * depth * speed;
        if (target != Target::Pitch) {
            // Absolute y writes are emitted only on value changes. Start at
            // a real periodic change, so the next traversal repeats exactly
            // the same writes, including clamped plateaus at either extreme.
            const auto periodic_start = start;
            while (start < periodic_start + period
                   && modulationEffectiveValueAtCount(
                       mod, start, base, 0, maximum)
                       == modulationEffectiveValueAtCount(
                           mod, start - 1U, base, 0, maximum)) {
                start += speed;
            }
            if (start == periodic_start + period) {
                return;
            }
        }
    } else if (target != Target::Pitch) {
        // Only pitch has a relative ENV change command. Absolute volume/y
        // values cannot keep rising/falling by repeating the same body.
        return;
    }
    if (start + period > timeline.length_counts) {
        return;
    }
    timeline.loop_start_count = start;
    timeline.loop_end_count = start + period;
}

[[nodiscard]] std::int32_t interpolatedLaneVolume(
    std::int32_t start_value,
    std::uint32_t start_count,
    std::int32_t end_value,
    std::uint32_t end_count,
    std::uint32_t at) noexcept {
    if (at <= start_count) {
        return start_value;
    }
    if (at >= end_count) {
        return end_value;
    }
    const auto span = static_cast<std::int32_t>(end_count - start_count);
    const auto num = start_value * static_cast<std::int32_t>(end_count - at)
        + end_value * static_cast<std::int32_t>(at - start_count);
    if (num >= 0) {
        return (num + span / 2) / span;
    }
    return (num - span / 2) / span;
}

}  // namespace

[[nodiscard]] std::vector<std::uint8_t> compileCompositeEnvelopeLane(
    const CompositeLayer& source_layer,
    CompositeEnvelopeLane lane,
    const TimbreNumberResolution& numbers,
    const TimbreLibrary* library,
    bool include_original_tone_y,
    bool expand_tl_auto,
    bool expand_fb_auto,
    bool include_loop) {
    const auto expanded = expandCompositeLayerModulations(source_layer, library);
    const auto& layer = expanded;
    struct TimedEvent {
        std::uint32_t count{};
        mgstc::engine::EnvelopeEventKind kind{};
        std::int32_t value{};
        std::int32_t secondary{};
        std::uint64_t target_library_id{};
        mgstc::engine::TimbrePick timbre_pick{
            mgstc::engine::TimbrePick::Library};
        bool after_loop_start{};
        bool automatic{};
        bool precise{};
        std::uint32_t automatic_duration{};
    };
    std::vector<TimedEvent> events;
    const auto collect = [&events](
        const std::vector<mgstc::engine::EnvelopeEvent>& source,
        mgstc::engine::EnvelopeEventKind expected) {
        for (const auto& event : source) {
            if (event.kind == expected) {
                events.push_back({
                    event.count,
                    event.kind,
                    event.value,
                    event.secondary,
                    event.target_library_id,
                    event.timbre_pick,
                    event.after_loop_start,
                    event.automatic,
                    event.precise});
            }
        }
    };
    const mgstc::engine::EnvelopeTimeline* timeline{};
    switch (lane) {
    case CompositeEnvelopeLane::Volume:
        timeline = &layer.envelope_timeline;
        collect(
            layer.volume_envelope.events,
            mgstc::engine::EnvelopeEventKind::Volume);
        if (std::none_of(
                events.begin(), events.end(),
                [](const TimedEvent& event) {
                    return event.count == 0;
                })) {
            events.push_back({
                0,
                mgstc::engine::EnvelopeEventKind::Volume,
                layer.volume,
                0});
        }
        break;
    case CompositeEnvelopeLane::Pitch:
        timeline = &layer.envelope_timeline;
        collect(
            layer.pitch_envelope.events,
            mgstc::engine::EnvelopeEventKind::Pitch);
        break;
    case CompositeEnvelopeLane::Timbre:
        timeline = &layer.envelope_timeline;
        collect(
            layer.timbre_automation,
            mgstc::engine::EnvelopeEventKind::Timbre);
        for (const auto& event : layer.timbre_automation) {
            if (event.kind
                != mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                continue;
            }
            // Poly secondary composite voices omit shared original-tone
            // y (regs 0–7). Channel-local y stays.
            if (!include_original_tone_y
                && event.value >= 0
                && event.value <= 7) {
                continue;
            }
            events.push_back({
                event.count,
                event.kind,
                event.value,
                event.secondary,
                event.target_library_id,
                event.timbre_pick,
                    event.after_loop_start,
                    event.automatic});
        }
        if (include_original_tone_y
            && layer.source == mgstc::engine::TimbreSource::Opll
            && (expand_tl_auto || expand_fb_auto)) {
            for (const auto& event :
                 mgstc::engine::expandOpllLayerRegisterAutos(
                     layer,
                     library,
                     expand_tl_auto,
                     expand_fb_auto)) {
                if (event.kind
                    == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                    events.push_back({
                        event.count,
                        event.kind,
                        event.value,
                        event.secondary,
                        event.target_library_id,
                        event.timbre_pick,
                        event.after_loop_start});
                }
            }
        }
        if (std::none_of(
                events.begin(), events.end(),
                [](const TimedEvent& event) {
                    return event.kind
                        == mgstc::engine::EnvelopeEventKind::Timbre
                        && event.count == 0;
                })
            && mgstc::engine::layerEnvelopeNeedsLeadingBasePatch(
                layer, &numbers)) {
            mgstc::engine::EnvelopeEvent leading{
                .kind = mgstc::engine::EnvelopeEventKind::Timbre,
            };
            bool have_base = false;
            if (mgstc::engine::layerUsesOpllRomBase(layer)) {
                leading.timbre_pick =
                    mgstc::engine::TimbrePick::OpllRom;
                leading.value = static_cast<std::int32_t>(
                    *layer.base_opll_rom);
                have_base = true;
            } else if (layer.base_timbre) {
                leading.timbre_pick =
                    mgstc::engine::TimbrePick::Library;
                leading.target_library_id =
                    layer.base_timbre->library_id;
                have_base = true;
            }
            if (have_base) {
                // Insert first so stable_sort keeps `@` before same-count `y`
                // (§7.5 / formatMgsCompositeEnvelope prepend).
                events.insert(
                    events.begin(),
                    {
                        0,
                        leading.kind,
                        leading.value,
                        0,
                        leading.target_library_id,
                        leading.timbre_pick});
            }
        }
        break;
    }

    const bool valid_loop = include_loop
        && timeline->loop_start_count
        && timeline->loop_end_count
        && *timeline->loop_start_count < *timeline->loop_end_count
        && *timeline->loop_end_count <= timeline->length_counts;
    if (valid_loop) {
        events.push_back({
            *timeline->loop_start_count,
            mgstc::engine::EnvelopeEventKind::LoopStart,
            0,
            0});
        events.push_back({
            *timeline->loop_end_count,
            mgstc::engine::EnvelopeEventKind::LoopEnd,
            0,
            0});
    }
    events.erase(
        std::remove_if(
            events.begin(), events.end(),
            [timeline, valid_loop](const TimedEvent& event) {
                if (event.count > timeline->length_counts) {
                    return true;
                }
                // Match formatMgsCompositeEnvelope: loop_end is the `]`
                // marker only. A volume at that count must not sit after
                // `[` with no wait (1-step `[f]` would compile to `[]`).
                // Pitch/timbre at loop_end (e.g. `[\-1f\1]`) stay inside `[]`.
                return valid_loop
                    && event.count == *timeline->loop_end_count
                    && event.kind
                        == mgstc::engine::EnvelopeEventKind::Volume
                    && event.after_loop_start;
            }),
        events.end());

    const auto priority = [](const TimedEvent& event) {
        // §6.2.3 at one count: before-`[` cmds → `[` → after-`[` cmds
        // → volume wait. Volume is the 1-count (or hold) that `[f]` loops.
        if (event.kind == mgstc::engine::EnvelopeEventKind::LoopEnd) {
            return 0;
        }
        if (event.kind == mgstc::engine::EnvelopeEventKind::LoopStart) {
            return 2;
        }
        if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
            return 4;
        }
        return event.after_loop_start ? 3 : 1;
    };
    std::stable_sort(
        events.begin(), events.end(),
        [&priority](const TimedEvent& left, const TimedEvent& right) {
            if (left.count != right.count) {
                return left.count < right.count;
            }
            return priority(left) < priority(right);
        });

    if (lane == CompositeEnvelopeLane::Volume) {
        std::vector<std::uint32_t> zero_time_counts;
        const auto note_cut = [&zero_time_counts](std::uint32_t count) {
            zero_time_counts.push_back(count);
        };
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch) {
                note_cut(event.count);
            }
        }
        for (const auto& event : layer.timbre_automation) {
            note_cut(event.count);
        }
        std::sort(zero_time_counts.begin(), zero_time_counts.end());
        zero_time_counts.erase(
            std::unique(zero_time_counts.begin(), zero_time_counts.end()),
            zero_time_counts.end());

        std::vector<TimedEvent> scheduled;
        scheduled.reserve(events.size() + zero_time_counts.size());
        std::uint32_t previous_volume_count{};
        std::int32_t origin_value = layer.volume;
        for (const auto& event : events) {
            if (event.kind
                == mgstc::engine::EnvelopeEventKind::Volume) {
                if (event.automatic
                    && event.count > previous_volume_count) {
                    if (event.precise) {
                        const auto duration =
                            event.count - previous_volume_count;
                        const bool origin_scheduled = std::any_of(
                            scheduled.begin(),
                            scheduled.end(),
                            [previous_volume_count](const TimedEvent& item) {
                                return item.kind
                                    == mgstc::engine::EnvelopeEventKind::Volume
                                    && item.count == previous_volume_count;
                            });
                        const auto vols =
                            mgstc::engine::sampleAutomaticRampVolumes(
                                static_cast<std::uint8_t>(std::clamp(
                                    origin_value,
                                    std::int32_t{0},
                                    std::int32_t{15})),
                                static_cast<std::uint8_t>(std::clamp(
                                    event.value,
                                    std::int32_t{0},
                                    std::int32_t{15})),
                                duration,
                                origin_scheduled);
                        const auto start = origin_scheduled ? 1u : 0u;
                        for (std::uint32_t i = start;
                             i < vols.size();
                             ++i) {
                            auto step = event;
                            step.count = previous_volume_count + i;
                            step.value = static_cast<std::int32_t>(vols[i]);
                            step.automatic = false;
                            step.precise = false;
                            step.automatic_duration = 0;
                            scheduled.push_back(step);
                        }
                    } else {
                    std::uint32_t from = previous_volume_count;
                    for (const auto cut : zero_time_counts) {
                        if (cut <= previous_volume_count
                            || cut >= event.count) {
                            continue;
                        }
                        const auto duration = cut - from;
                        if (duration == 0) {
                            continue;
                        }
                        auto segment = event;
                        segment.count = from;
                        segment.value = interpolatedLaneVolume(
                            origin_value,
                            previous_volume_count,
                            event.value,
                            event.count,
                            cut);
                        segment.automatic_duration = duration;
                        scheduled.push_back(segment);
                        from = cut;
                    }
                    auto ramp = event;
                    ramp.count = from;
                    ramp.automatic_duration = event.count - from;
                    scheduled.push_back(ramp);
                    }
                } else if (!event.automatic) {
                    scheduled.push_back(event);
                }
                origin_value = event.value;
                previous_volume_count = event.count;
            } else {
                scheduled.push_back(event);
            }
        }
        events.swap(scheduled);
    }

    std::vector<std::uint8_t> bytecode;
    bytecode.reserve(events.size() * 3 + 8);
    std::uint32_t cursor{};
    std::uint32_t previous_volume_count{};
    auto volume = static_cast<std::uint8_t>(
        std::min<std::uint8_t>(layer.volume, 15));
    const auto append_wait = [&bytecode, &volume, lane](
                                 std::uint32_t count) {
        while (count != 0) {
            const auto chunk = static_cast<std::uint8_t>(
                std::min<std::uint32_t>(255, count));
            const auto held_volume = lane == CompositeEnvelopeLane::Volume
                ? volume
                : std::uint8_t{0};
            bytecode.push_back(static_cast<std::uint8_t>(
                0xE0 | held_volume));
            bytecode.push_back(chunk);
            count -= chunk;
        }
    };
    const auto append_zero_time_at = [&](
        std::uint32_t count,
        std::optional<bool> after_loop_start = std::nullopt) {
        if (lane != CompositeEnvelopeLane::Volume) {
            return;
        }
        for (const auto& timbre : layer.timbre_automation) {
            if (timbre.count != count
                || timbre.kind
                    != mgstc::engine::EnvelopeEventKind::Timbre) {
                continue;
            }
            if (after_loop_start
                && timbre.after_loop_start != *after_loop_start) {
                continue;
            }
            mgstc::engine::EnvelopeEvent resolved{
                .kind = mgstc::engine::EnvelopeEventKind::Timbre,
                .value = timbre.value,
                .target_library_id = timbre.target_library_id,
                .timbre_pick = timbre.timbre_pick,
            };
            const auto number =
                mgstc::engine::envelopeEventTimbreNumber(
                    resolved, &numbers);
            bytecode.insert(
                bytecode.end(),
                {0x10, number.value_or(0)});
        }
        for (const auto& pitch : layer.pitch_envelope.events) {
            if (pitch.count != count
                || pitch.kind
                    != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            if (after_loop_start
                && pitch.after_loop_start != *after_loop_start) {
                continue;
            }
            const auto clamped = std::clamp(
                pitch.value, std::int32_t{-127}, std::int32_t{127});
            const auto encoded = static_cast<std::uint8_t>(
                clamped < 0 ? static_cast<int>(clamped + 256) : clamped);
            bytecode.insert(bytecode.end(), {0x12, encoded});
        }
    };
    const auto catch_up_zero_time = [&](std::uint32_t until_exclusive) {
        if (lane != CompositeEnvelopeLane::Volume) {
            return;
        }
        std::uint32_t remaining = cursor;
        while (true) {
            std::optional<std::uint32_t> next;
            const auto consider = [&](std::uint32_t count) {
                if (count < remaining || count >= until_exclusive) {
                    return;
                }
                if (!next || count < *next) {
                    next = count;
                }
            };
            for (const auto& pitch : layer.pitch_envelope.events) {
                if (pitch.kind == mgstc::engine::EnvelopeEventKind::Pitch) {
                    consider(pitch.count);
                }
            }
            for (const auto& timbre : layer.timbre_automation) {
                if (timbre.kind == mgstc::engine::EnvelopeEventKind::Timbre
                    || timbre.kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                    consider(timbre.count);
                }
            }
            if (!next) {
                break;
            }
            if (*next > cursor) {
                append_wait(*next - cursor);
                cursor = *next;
            }
            // LoopStart / LoopEnd emit bracket-scoped zero-time at their
            // counts; catch_up must not duplicate (e.g. `[\-1f\1]`).
            if (!valid_loop
                || (*next != *timeline->loop_start_count
                    && *next != *timeline->loop_end_count)) {
                append_zero_time_at(*next);
            }
            remaining = *next + 1;
        }
    };
    for (const auto& event : events) {
        catch_up_zero_time(event.count);
        if (event.count > cursor) {
            append_wait(event.count - cursor);
            cursor = event.count;
        }
        // Same-count `\` / `@` sit immediately before this volume
        // (`e:7.\-127.d:4`). Do not inject on `[` / `]` or other kinds
        // (that would duplicate opcodes).
        if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
            const bool loop_start_volume =
                valid_loop
                && event.count == *timeline->loop_start_count;
            if (!loop_start_volume) {
                const std::optional<bool> bracket_side =
                    valid_loop
                    && event.count > *timeline->loop_start_count
                    && event.count <= *timeline->loop_end_count
                        ? std::optional<bool>{true}
                        : std::optional<bool>{false};
                append_zero_time_at(event.count, bracket_side);
            }
        }
        switch (event.kind) {
        case mgstc::engine::EnvelopeEventKind::Volume:
            {
                const auto target = static_cast<std::uint8_t>(
                    std::clamp(
                        event.value, std::int32_t{0}, std::int32_t{15}));
                const auto interval = event.automatic_duration != 0
                    ? event.automatic_duration
                    : event.count - previous_volume_count;
                if (event.automatic && interval != 0) {
                    if (interval == 1) {
                        volume = target;
                        bytecode.push_back(volume);
                        cursor = std::max(cursor, event.count + 1);
                    } else {
                        const auto duration = static_cast<std::uint8_t>(
                            std::clamp(
                                interval,
                                std::uint32_t{1},
                                std::uint32_t{255}));
                        bytecode.push_back(
                            static_cast<std::uint8_t>(0x20 | target));
                        bytecode.push_back(duration);
                        volume = target;
                        cursor = std::max(
                            cursor,
                            event.count + static_cast<std::uint32_t>(duration));
                    }
                } else {
                    volume = target;
                    bytecode.push_back(volume);
                    cursor = std::max(cursor, event.count + 1);
                }
                previous_volume_count = event.count;
            }
            break;
        case mgstc::engine::EnvelopeEventKind::Pitch:
            bytecode.insert(
                bytecode.end(),
                {0x12, static_cast<std::uint8_t>(
                    std::clamp(
                        event.value,
                        std::int32_t{-127},
                        std::int32_t{127}))});
            break;
        case mgstc::engine::EnvelopeEventKind::Timbre: {
            mgstc::engine::EnvelopeEvent resolved{
                .kind = mgstc::engine::EnvelopeEventKind::Timbre,
                .value = event.value,
                .target_library_id = event.target_library_id,
                .timbre_pick = event.timbre_pick,
            };
            const auto number =
                mgstc::engine::envelopeEventTimbreNumber(
                    resolved, &numbers);
            bytecode.insert(
                bytecode.end(),
                {0x10, number.value_or(0)});
            break;
        }
        case mgstc::engine::EnvelopeEventKind::RegisterWrite:
            bytecode.insert(
                bytecode.end(),
                {0x11,
                 static_cast<std::uint8_t>(
                     std::clamp(
                     event.value, std::int32_t{0}, std::int32_t{255})),
                 static_cast<std::uint8_t>(
                     std::clamp(
                         event.secondary,
                         std::int32_t{0},
                         std::int32_t{255}))});
            break;
        case mgstc::engine::EnvelopeEventKind::LoopStart:
            append_zero_time_at(event.count, false);
            bytecode.push_back(0x40);
            append_zero_time_at(event.count, true);
            break;
        case mgstc::engine::EnvelopeEventKind::LoopEnd:
            append_zero_time_at(event.count, true);
            bytecode.push_back(0x60);
            break;
        default:
            break;
        }
    }
    catch_up_zero_time(std::numeric_limits<std::uint32_t>::max());
    if (cursor < timeline->length_counts) {
        append_wait(timeline->length_counts - cursor);
    }
    return bytecode;
}

[[nodiscard]] CompositeEnvelopePrograms compileCompositeEnvelopes(
    const CompositeLayer& layer,
    const TimbreNumberResolution& numbers,
    const TimbreLibrary* library,
    bool include_original_tone_y,
    bool expand_tl_auto,
    bool expand_fb_auto) {
    // MGSDRV executes one @e byte stream. Compile the finalized MML token
    // stream for audition as well, so ramp origin counts and intervening
    // pitch/timbre/y commands have exactly the exported order and timing.
    auto formatted_layer = layer;
    if (include_original_tone_y && !expand_tl_auto) {
        disableCompositeRegisterAuto(formatted_layer, OpllRegisterAutoTarget::TotalLevel);
        disableCompositeModulation(formatted_layer, CompositeModulationTarget::TotalLevel);
    }
    if (include_original_tone_y && !expand_fb_auto) {
        disableCompositeRegisterAuto(formatted_layer, OpllRegisterAutoTarget::Feedback);
        disableCompositeModulation(formatted_layer, CompositeModulationTarget::Feedback);
    }
    const auto formatted = formatMgsCompositeEnvelope(
        formatted_layer, 0, kMgscEnvelopeCompiledByteLimit,
        &numbers, library);
    if (!formatted.valid()) {
        return {};
    }
    if (include_original_tone_y) {
        return {.volume = formatted.bytecode};
    }
    auto voice_layer = layer;
    auto& voice_events = voice_layer.timbre_automation;
    voice_events.erase(std::remove_if(voice_events.begin(), voice_events.end(),
        [](const EnvelopeEvent& event) {
            return event.kind == EnvelopeEventKind::RegisterWrite
                && event.value >= 0 && event.value <= 7;
        }), voice_events.end());
    disableCompositeRegisterAuto(voice_layer, OpllRegisterAutoTarget::TotalLevel);
    disableCompositeRegisterAuto(voice_layer, OpllRegisterAutoTarget::Feedback);
    disableCompositeModulation(voice_layer, CompositeModulationTarget::TotalLevel);
    disableCompositeModulation(voice_layer, CompositeModulationTarget::Feedback);
    const auto base_patch = layerBasePatchNumber(layer, &numbers);
    const bool explicit_base_patch = std::any_of(
        voice_events.begin(), voice_events.end(), [&](const EnvelopeEvent& event) {
            return event.kind == EnvelopeEventKind::Timbre && event.count == 0
                && !event.after_loop_start
                && envelopeEventTimbreNumber(event, &numbers) == base_patch;
        });
    const bool omit_implicit_restore = base_patch && !explicit_base_patch
        && layerEnvelopeNeedsLeadingBasePatch(layer, &numbers)
        && !layerEnvelopeNeedsLeadingBasePatch(voice_layer, &numbers);
    // Secondary voices suppress shared y writes only after the export budget
    // and loop decision. Removing them before formatting would extend the
    // playable prefix or change its period, diverging from the exported @e.
    std::vector<std::uint8_t> voice_code;
    voice_code.reserve(formatted.bytecode.size());
    for (std::size_t at = 0; at < formatted.bytecode.size();) {
        const auto opcode = formatted.bytecode[at];
        const std::size_t size = opcode == 0x11 ? 3
            : opcode == 0x10 || opcode == 0x12
                || (opcode >= 0x20 && opcode <= 0x2F)
                || opcode >= 0xE0 ? 2 : 1;
        const bool shared_y = opcode == 0x11 && formatted.bytecode[at + 1] <= 7;
        const bool implicit_restore = at == 0 && opcode == 0x10
            && omit_implicit_restore && formatted.bytecode[at + 1] == *base_patch;
        if (!shared_y && !implicit_restore) {
            voice_code.insert(voice_code.end(), formatted.bytecode.begin() + at,
                formatted.bytecode.begin() + at + size);
        }
        at += size;
    }
    return {.volume = std::move(voice_code)};
}

[[nodiscard]] std::vector<std::uint8_t> sampleCompositeVolumeLane(
    const CompositeLayer& layer,
    int ticks) {
    std::vector<std::uint8_t> volumes(
        static_cast<std::size_t>(std::max(0, ticks)), 0);
    if (ticks <= 0) {
        return volumes;
    }
    const mgstc::engine::TimbreNumberResolution numbers{};
    auto base_layer = layer;
    disableCompositeModulation(base_layer, CompositeModulationTarget::Pitch);
    disableCompositeModulation(base_layer, CompositeModulationTarget::Volume);
    disableCompositeModulation(base_layer, CompositeModulationTarget::TotalLevel);
    disableCompositeModulation(base_layer, CompositeModulationTarget::Feedback);
    disableCompositeRegisterAuto(base_layer, OpllRegisterAutoTarget::TotalLevel);
    disableCompositeRegisterAuto(base_layer, OpllRegisterAutoTarget::Feedback);
    base_layer.register_auto_profiles.clear();
    base_layer.derived_command_placements.clear();
    auto bytecode = formatMgsCompositeEnvelope(
        base_layer, 0, std::numeric_limits<std::size_t>::max(),
        &numbers).bytecode;
    mgstc::engine::SequenceEnvelopeRuntime runtime(std::move(bytecode));
    runtime.resetForKeyOn();
    mgstc::engine::EventBuffer buffer(16);
    for (int count = 0; count < ticks; ++count) {
        buffer.clear();
        static_cast<void>(runtime.processTick(buffer));
        volumes[static_cast<std::size_t>(count)] = runtime.volume();
    }
    return volumes;
}

CompositeLayer expandCompositeLayerModulations(
    const CompositeLayer& layer,
    const TimbreLibrary* library,
    bool optimize_loop) {
    auto expanded = layer;
    if (layer.volume_envelope.kind != EnvelopeKind::Sequence) {
        disableCompositeModulation(expanded, CompositeModulationTarget::Pitch);
        disableCompositeModulation(expanded, CompositeModulationTarget::Volume);
        disableCompositeModulation(expanded, CompositeModulationTarget::TotalLevel);
        disableCompositeModulation(expanded, CompositeModulationTarget::Feedback);
        return expanded;
    }
    if (optimize_loop) {
        maybeOptimizeModulationLoop(expanded, library);
    }
    // The evaluator and compiler share these exact 60 Hz count values.
    // Only the temporary copy receives derived ENV commands.
    disableCompositeModulation(expanded, CompositeModulationTarget::Pitch);
    disableCompositeModulation(expanded, CompositeModulationTarget::Volume);
    if (compositeHasModulation(layer, CompositeModulationTarget::Pitch)) {
        std::map<std::uint32_t, std::vector<const CompositeModulationProfile*>> profiles;
        for (const auto& profile : layer.modulation_profiles)
            if (profile.target == CompositeModulationTarget::Pitch)
                profiles[profile.count].push_back(&profile);
        for (auto& [count, items] : profiles) {
            static_cast<void>(count);
            std::stable_sort(items.begin(), items.end(), [](const auto* a, const auto* b) {
                if (a->after_loop_start != b->after_loop_start) return !a->after_loop_start;
                if (a->command_order && b->command_order) return a->command_order < b->command_order;
                if (a->command_order != b->command_order) return a->command_order != 0;
                return false;
            });
        }
        const ModulationParameters* active = &layer.pitch_modulation;
        const auto end = std::min(
            expanded.envelope_timeline.loop_end_count.value_or(
                expanded.envelope_timeline.length_counts),
            expanded.envelope_timeline.length_counts);
        auto previous = std::int32_t{0};
        for (std::uint32_t count = 0; count < end; ++count) {
            const auto append_offset = [&](std::int32_t current, bool after, std::uint32_t order) {
                auto delta = current - previous;
                previous = current;
                // Profile changes can exceed one ENV signed-byte delta.
                // Keep their exact sum with bounded ordinary commands.
                while (delta != 0) {
                    const auto part = std::clamp(delta, -127, 127);
                    expanded.pitch_envelope.events.push_back({
                        .kind = EnvelopeEventKind::Pitch, .value = part,
                        .count = count, .after_loop_start = after, .command_order = order});
                    delta -= part;
                }
            };
            const auto& prior = *active;
            const bool boundary = expanded.envelope_timeline.loop_start_count
                && count >= *expanded.envelope_timeline.loop_start_count;
            const auto* placement = compositeDerivedPlacementAt(layer,
                CompositeModulationTarget::Pitch, count);
            bool has_profiles = false;
            if (const auto found = profiles.find(count); found != profiles.end()) {
                for (const auto* item : found->second) {
                    const auto& profile = *item;
                    if (!has_profiles) {
                        append_offset(modulationOffsetAtCount(prior, count),
                            placement ? placement->after_loop_start
                                : boundary && profile.after_loop_start,
                            placement ? placement->command_order : 0);
                        has_profiles = true;
                    }
                    append_offset(modulationOffsetAtCount(profile.parameters, count), profile.after_loop_start,
                        profile.command_order);
                    active = &profile.parameters;
                }
            }
            if (!has_profiles)
                append_offset(modulationOffsetAtCount(prior, count),
                    placement ? placement->after_loop_start : boundary,
                    placement ? placement->command_order : 0);
        }
    }
    if (compositeHasModulation(layer, CompositeModulationTarget::Volume)
        && layer.volume_envelope.kind == EnvelopeKind::Sequence) {
        const auto end = std::min(
            expanded.envelope_timeline.loop_end_count.value_or(
                expanded.envelope_timeline.length_counts),
            expanded.envelope_timeline.length_counts);
        // Sample the authoring Base ENV with modulation disabled and without
        // loop expansion; this also preserves MGSDRV's integer ramp values.
        const auto base = sampleCompositeVolumeLane(
            expanded, static_cast<int>(end));
        expanded.volume_envelope.events.erase(
            std::remove_if(
                expanded.volume_envelope.events.begin(),
                expanded.volume_envelope.events.end(),
                [](const EnvelopeEvent& event) {
                    return event.kind == EnvelopeEventKind::Volume;
                }),
            expanded.volume_envelope.events.end());
        std::int32_t previous = -1;
        for (std::uint32_t count = 0; count < end; ++count) {
            const auto value = modulationEffectiveValueAtCount(
                compositeModulationAt(layer, CompositeModulationTarget::Volume, count),
                count,
                base[static_cast<std::size_t>(count)], 0, 15);
            if (value == previous
                && (!expanded.envelope_timeline.loop_start_count
                    || count != *expanded.envelope_timeline.loop_start_count)) {
                continue;
            }
            expanded.volume_envelope.events.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = value,
                .count = count,
            });
            previous = value;
        }
    }
    return expanded;
}

bool compositeModulationLoopRepresentable(
    const CompositeLayer& layer,
    const TimbreLibrary* library) {
    if (layer.volume_envelope.kind != EnvelopeKind::Sequence) {
        return true;
    }
    const auto& timeline = layer.envelope_timeline;
    if (!timeline.loop_start_count || !timeline.loop_end_count) {
        return true;
    }
    const auto start = *timeline.loop_start_count;
    const auto end = *timeline.loop_end_count;
    if (start >= end || end > timeline.length_counts) {
        return false;
    }
    if (!layer.modulation_profiles.empty() || !layer.register_auto_profiles.empty()) {
        // The existing phase proof applies to one steady profile per body.
        // Never silently compensate a prefix/body handoff or an internal
        // profile transition: a repeated relative delta would cause drift.
        for (const auto& profile : layer.modulation_profiles) {
            if (profile.count > start && profile.count < end) return false;
            if (profile.target == CompositeModulationTarget::Pitch
                && profile.count == start && profile.after_loop_start
                && modulationOffsetAtCount(profile.parameters, start)
                    != modulationOffsetAtCount(compositeModulationAt(layer,
                        CompositeModulationTarget::Pitch, start, false), start))
                return false;
        }
        auto steady = layer;
        steady.pitch_modulation = compositeModulationAt(layer,
            CompositeModulationTarget::Pitch, start);
        steady.volume_modulation = compositeModulationAt(layer,
            CompositeModulationTarget::Volume, start);
        steady.opll_tl_modulation = compositeModulationAt(layer,
            CompositeModulationTarget::TotalLevel, start);
        steady.opll_fb_modulation = compositeModulationAt(layer,
            CompositeModulationTarget::Feedback, start);
        steady.opll_tl_auto = compositeRegisterAutoAt(layer,
            OpllRegisterAutoTarget::TotalLevel, start);
        steady.opll_fb_auto = compositeRegisterAutoAt(layer,
            OpllRegisterAutoTarget::Feedback, start);
        steady.modulation_profiles.clear();
        steady.register_auto_profiles.clear();
        if (!compositeModulationLoopRepresentable(steady, library)) {
            return false;
        }
        // The steady phase proof cannot account for a different prefix
        // profile. In particular, a handoff may omit the steady profile's
        // first body delta even when both profiles have the same value at
        // start. Check the commands that will actually repeat, without
        // changing or compensating any authored command.
        const auto repeated_pitch_delta = [start, end, library](CompositeLayer source) {
            source.pitch_envelope.events.clear();
            disableCompositeModulation(source, CompositeModulationTarget::Volume);
            const auto generated = expandCompositeLayerModulations(source, library, false);
            std::int64_t delta = 0;
            for (const auto& event : generated.pitch_envelope.events) {
                if (event.kind == EnvelopeEventKind::Pitch
                    && ((event.count > start && event.count < end)
                        || ((event.count == start || event.count == end)
                            && event.after_loop_start))) {
                    delta += event.value;
                }
            }
            return delta;
        };
        return repeated_pitch_delta(layer) == repeated_pitch_delta(steady);
    }
    const auto span = end - start;
    const auto phase_fits = [start, span](
        const ModulationParameters& mod, bool relative) {
        if (!mod.enabled || mod.roughness == 0) {
            return true;
        }
        const auto first = softwareLfoFirstUpdateTick(
            mod.delay, mod.speed);
        const auto speed = softwareLfoSpeedPeriod(mod.speed);
        if (mod.mode == ModulationMode::Oscillate) {
            const auto depth = static_cast<std::uint32_t>(
                std::min<std::uint8_t>(mod.depth, 127)) + 1U;
            const auto periodic_start = first
                + (((depth >> 1U) + 1U) * speed);
            const auto cycle = 2U * depth * speed;
            return start >= periodic_start && span % cycle == 0;
        }
        return !relative || (start >= first && span % speed == 0);
    };
    if (!phase_fits(layer.pitch_modulation, true)
        || !phase_fits(layer.volume_modulation, false)
        || !phase_fits(layer.opll_tl_modulation, false)
        || !phase_fits(layer.opll_fb_modulation, false)) {
        return false;
    }

    // An absolute Step target is only invariant on all later loop passes
    // after it reaches the same clamp boundary that later updates hold.
    const auto stable_step = [start](
        const ModulationParameters& mod, int base, int max_value) {
        if (!mod.enabled || mod.roughness == 0
            || mod.mode == ModulationMode::Oscillate) {
            return true;
        }
        // The body must already be at its final clamp before the anchor.
        // Otherwise its first-pass transition would be retransmitted on
        // every traversal even though continuous Step has stopped changing.
        const auto effective = base + modulationOffsetAtCount(
            mod, start == 0 ? 0 : start - 1);
        return mod.mode == ModulationMode::StepUp
            ? effective >= max_value : effective <= 0;
    };
    if (layer.volume_modulation.enabled
        && layer.volume_modulation.roughness != 0) {
        if (std::any_of(
                layer.volume_envelope.events.begin(),
                layer.volume_envelope.events.end(),
                [start, end](const EnvelopeEvent& event) {
                    return event.kind == EnvelopeEventKind::Volume
                        && ((event.count >= start && event.count <= end)
                            || event.automatic);
                })) {
            return false;
        }
        if (layer.volume_modulation.mode != ModulationMode::Oscillate) {
            const auto base = sampleCompositeVolumeLane(
                layer, static_cast<int>(start) + 1);
            if (base.empty() || !stable_step(
                    layer.volume_modulation, base.back(), 15)) {
                return false;
            }
        }
    }
    if (layer.opll_tl_modulation.enabled
        || layer.opll_fb_modulation.enabled) {
        if (std::any_of(
                layer.timbre_automation.begin(),
                layer.timbre_automation.end(),
                [start, end](const EnvelopeEvent& event) {
                    return event.count >= start && event.count <= end;
                })) {
            return false;
        }
        const auto image = opllOriginalRegisterImageAt(
            layer, start, library, true, false);
        if (!image) {
            return false;
        }
        if (!stable_step(
                layer.opll_tl_modulation, (*image)[2] & 0x3F, 63)
            || !stable_step(
                layer.opll_fb_modulation, (*image)[3] & 0x07, 7)) {
            return false;
        }
    }
    return true;
}

bool compositeLoopEffectiveWaveformFirstPassOnly(
    const CompositeLayer& layer,
    CompositeModulationTarget target,
    const TimbreLibrary* library) {
    const auto& timeline = layer.envelope_timeline;
    if (layer.volume_envelope.kind != EnvelopeKind::Sequence
        || !timeline.loop_start_count || !timeline.loop_end_count
        || *timeline.loop_start_count >= *timeline.loop_end_count
        || *timeline.loop_end_count > timeline.length_counts) {
        return false;
    }
    if ((target == CompositeModulationTarget::TotalLevel
            || target == CompositeModulationTarget::Feedback)
        && layer.source != TimbreSource::Opll) return false;
    auto isolated = layer;
    for (const auto other : {CompositeModulationTarget::Pitch,
            CompositeModulationTarget::Volume, CompositeModulationTarget::TotalLevel,
            CompositeModulationTarget::Feedback}) {
        if (other != target) disableCompositeModulation(isolated, other);
    }
    if (target != CompositeModulationTarget::TotalLevel)
        disableCompositeRegisterAuto(isolated, OpllRegisterAutoTarget::TotalLevel);
    if (target != CompositeModulationTarget::Feedback)
        disableCompositeRegisterAuto(isolated, OpllRegisterAutoTarget::Feedback);
    if (!compositeModulationLoopRepresentable(isolated, library)) return true;
    const auto start = *timeline.loop_start_count;
    const auto end = *timeline.loop_end_count;
    if (target == CompositeModulationTarget::Volume) {
        const auto expanded = expandCompositeLayerModulations(isolated, library, false);
        const auto span = end - start;
        const auto values = sampleCompositeVolumeLane(expanded,
            static_cast<int>(end + span));
        for (auto count = start; count < end; ++count)
            if (values[count] != values[count + span]) return true;
        return false;
    }
    if (target == CompositeModulationTarget::TotalLevel
        || target == CompositeModulationTarget::Feedback) {
        const auto register_target = target == CompositeModulationTarget::TotalLevel
            ? OpllRegisterAutoTarget::TotalLevel : OpllRegisterAutoTarget::Feedback;
        // A setting selected inside the body can leave a different field
        // state at the next loop entry, including an OFF-to-curve handoff.
        // The steady setting at start cannot prove that transition's phase.
        for (const auto& profile : layer.register_auto_profiles) {
            if (profile.target == register_target
                && ((profile.count > start && profile.count < end)
                    || (profile.count == end && profile.after_loop_start)))
                return true;
        }
        const auto& lane = compositeRegisterAutoAt(layer, register_target, start);
        if (lane.mode == OpllRegisterAutoMode::Lfo && lane.depth != 0) {
            const auto period = 4U * std::max<unsigned>(1, lane.change_speed)
                * std::max<unsigned>(1, lane.coarseness);
            if (start < lane.start_count || (end - start) % period != 0) return true;
        } else if (lane.mode == OpllRegisterAutoMode::Rise
                   || lane.mode == OpllRegisterAutoMode::Fall
                   || lane.mode == OpllRegisterAutoMode::FreeCurve) {
            if (opllRegisterAutoValueAt(lane, register_target,
                    start == 0 ? 0 : start - 1, timeline.length_counts)
                != opllRegisterAutoValueAt(lane, register_target,
                    end - 1, timeline.length_counts)) return true;
        }
        const auto reg = opllRegisterAutoRegisterNumber(register_target);
        const auto affects_field = [reg](const EnvelopeEvent& event) {
            return event.kind == EnvelopeEventKind::Timbre
                || (event.kind == EnvelopeEventKind::RegisterWrite && event.value == reg);
        };
        const auto derived = expandOpllLayerRegisterAutos(layer, library);
        const auto entry_restores_field = [&](const auto& events) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.count == start && event.after_loop_start && affects_field(event);
            });
        };
        if (!entry_restores_field(layer.timbre_automation)
            && !entry_restores_field(derived)) {
            // A write before [ runs once. Later body y/@ commands can leave
            // a different field at the next entry even with zero modulation
            // depth. Reuse the editor's ordered original-image replay.
            const auto first = opllOriginalRegisterImageAt(
                layer, start, library, true, true, true, &derived);
            const auto last = opllOriginalRegisterImageAt(
                layer, end - 1, library, true, true, true, &derived);
            if (first.has_value() != last.has_value()) return true;
            if (!first) {
                // Missing original images cannot establish a same-state
                // proof for commands which mutate/reload the body field.
                if (std::any_of(layer.timbre_automation.begin(),
                        layer.timbre_automation.end(), [&](const auto& event) {
                            return event.count > start && event.count < end
                                && affects_field(event);
                        })) return true;
            }
            const auto mask = register_target == OpllRegisterAutoTarget::TotalLevel
                ? 0x3F : 0x07;
            if (first && ((*first)[reg] & mask) != ((*last)[reg] & mask)) return true;
            const auto closing_field_write = [&](const auto& events) {
                return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                    return event.count == end && event.after_loop_start && affects_field(event);
                });
            };
            if (closing_field_write(layer.timbre_automation)
                || closing_field_write(derived)) return true;
        }
    }
    if (target != CompositeModulationTarget::Pitch) return false;

    if (layer.source != TimbreSource::Opll && layer.pitch_sweep.enabled
        && pitchSweepAmount(layer.pitch_sweep.value) != 0) return true;
    if (layer.software_lfo.enabled && layer.software_lfo.roughness != 0) {
        const auto speed = softwareLfoSpeedPeriod(layer.software_lfo.speed);
        const auto depth = static_cast<std::uint32_t>(
            std::min<std::uint8_t>(layer.software_lfo.depth, 127)) + 1U;
        const auto steady_start = softwareLfoFirstUpdateTick(
            layer.software_lfo.delay, layer.software_lfo.speed)
            + ((depth >> 1U) + 1U) * speed;
        if (start < steady_start || (end - start) % (2U * depth * speed) != 0)
            return true;
    }
    const auto expanded = expandCompositeLayerModulations(isolated, library, false);
    std::int64_t delta = 0;
    for (const auto& event : expanded.pitch_envelope.events) {
        if (event.kind == EnvelopeEventKind::Pitch
            && ((event.count > start && event.count < end)
                || ((event.count == start || event.count == end)
                    && event.after_loop_start))) {
            delta += event.value;
        }
    }
    return delta != 0;
}

}  // namespace mgstc::engine
