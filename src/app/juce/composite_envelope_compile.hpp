// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/timbre_library.hpp"

struct CompositeEnvelopePrograms {
    std::vector<std::uint8_t> volume;
    std::vector<std::uint8_t> pitch;
    std::vector<std::uint8_t> timbre;
};

enum class CompositeEnvelopeLane : std::uint8_t {
    Volume,
    Pitch,
    Timbre,
};

[[nodiscard]] inline std::int32_t interpolatedLaneVolume(
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

[[nodiscard]] inline std::vector<std::uint8_t> compileCompositeEnvelopeLane(
    const mgstc::engine::CompositeLayer& layer,
    CompositeEnvelopeLane lane,
    const mgstc::engine::TimbreNumberResolution& numbers,
    const mgstc::engine::TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true,
    bool include_loop = true) {
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
            [timeline](const TimedEvent& event) {
                return event.count > timeline->length_counts;
            }),
        events.end());

    const auto priority = [](const TimedEvent& event) {
        // §6.2.3 at one count: before-`[` cmds → `[` → after-`[` cmds.
        // `]` (loop end) sorts first only when it shares a count.
        if (event.kind == mgstc::engine::EnvelopeEventKind::LoopEnd) {
            return 0;
        }
        if (event.kind == mgstc::engine::EnvelopeEventKind::LoopStart) {
            return 2;
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
                                static_cast<std::uint8_t>(juce::jlimit(
                                    0, 15, origin_value)),
                                static_cast<std::uint8_t>(juce::jlimit(
                                    0, 15, event.value)),
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
        juce::jlimit(0, 15, static_cast<int>(layer.volume)));
    const auto append_wait = [&bytecode, &volume, lane](
                                 std::uint32_t count) {
        while (count != 0) {
            const auto chunk = static_cast<std::uint8_t>(
                juce::jmin<std::uint32_t>(255, count));
            const auto held_volume = lane == CompositeEnvelopeLane::Volume
                ? volume
                : std::uint8_t{0};
            bytecode.push_back(static_cast<std::uint8_t>(
                0xE0 | held_volume));
            bytecode.push_back(chunk);
            count -= chunk;
        }
    };
    const auto append_zero_time_at = [&](std::uint32_t count) {
        if (lane != CompositeEnvelopeLane::Volume) {
            return;
        }
        for (const auto& timbre : layer.timbre_automation) {
            if (timbre.count != count
                || timbre.kind
                    != mgstc::engine::EnvelopeEventKind::Timbre) {
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
            bytecode.insert(
                bytecode.end(),
                {0x12, static_cast<std::uint8_t>(
                    juce::jlimit(-127, 127, pitch.value))});
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
            append_zero_time_at(*next);
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
            append_zero_time_at(event.count);
        }
        switch (event.kind) {
        case mgstc::engine::EnvelopeEventKind::Volume:
            {
                const auto target = static_cast<std::uint8_t>(
                    juce::jlimit(0, 15, event.value));
                const auto interval = event.automatic_duration != 0
                    ? event.automatic_duration
                    : event.count - previous_volume_count;
                if (event.automatic && interval != 0) {
                    if (interval == 1) {
                        volume = target;
                        bytecode.push_back(volume);
                        cursor = juce::jmax(cursor, event.count + 1);
                    } else {
                        const auto duration = static_cast<std::uint8_t>(
                            juce::jlimit<std::uint32_t>(1, 255, interval));
                        bytecode.push_back(
                            static_cast<std::uint8_t>(0x20 | target));
                        bytecode.push_back(duration);
                        volume = target;
                        cursor = juce::jmax(
                            cursor,
                            event.count + static_cast<std::uint32_t>(duration));
                    }
                } else {
                    volume = target;
                    bytecode.push_back(volume);
                    cursor = juce::jmax(cursor, event.count + 1);
                }
                previous_volume_count = event.count;
            }
            break;
        case mgstc::engine::EnvelopeEventKind::Pitch:
            bytecode.insert(
                bytecode.end(),
                {0x12, static_cast<std::uint8_t>(
                    juce::jlimit(-127, 127, event.value))});
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
                     juce::jlimit(0, 255, event.value)),
                 static_cast<std::uint8_t>(
                     juce::jlimit(0, 255, event.secondary))});
            break;
        case mgstc::engine::EnvelopeEventKind::LoopStart:
            bytecode.push_back(0x40);
            break;
        case mgstc::engine::EnvelopeEventKind::LoopEnd:
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

[[nodiscard]] inline CompositeEnvelopePrograms compileCompositeEnvelopes(
    const mgstc::engine::CompositeLayer& layer,
    const mgstc::engine::TimbreNumberResolution& numbers,
    const mgstc::engine::TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true) {
    return {
        .volume = compileCompositeEnvelopeLane(
            layer,
            CompositeEnvelopeLane::Volume,
            numbers,
            library,
            include_original_tone_y,
            expand_tl_auto,
            expand_fb_auto),
        // Pitch `\` is in the volume `@e` stream (MGSDRV one bytecode).
        .pitch = {},
        .timbre = compileCompositeEnvelopeLane(
            layer,
            CompositeEnvelopeLane::Timbre,
            numbers,
            library,
            include_original_tone_y,
            expand_tl_auto,
            expand_fb_auto),
    };
}

[[nodiscard]] inline std::vector<std::uint8_t> sampleCompositeVolumeLane(
    const mgstc::engine::CompositeLayer& layer,
    int ticks) {
    std::vector<std::uint8_t> volumes(
        static_cast<std::size_t>(juce::jmax(0, ticks)), 0);
    if (ticks <= 0) {
        return volumes;
    }
    const mgstc::engine::TimbreNumberResolution numbers{};
    auto bytecode = compileCompositeEnvelopeLane(
        layer,
        CompositeEnvelopeLane::Volume,
        numbers,
        nullptr,
        true,
        false,
        false,
        false);
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
