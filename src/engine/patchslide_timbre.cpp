// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_timbre.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/composite_loop_edit.hpp"

namespace mgstc::engine {

std::string patchSlideDisplayName(const PatchSlideCandidate& candidate) {
    auto name = "OPLL @" + std::to_string(candidate.patch_a)
        + " → @" + std::to_string(candidate.patch_b);
    if (candidate.change_count == 2 && candidate.patch_c)
        name += " → @" + std::to_string(*candidate.patch_c);
    name += "（" + std::to_string(candidate.hold_a_steps);
    if (candidate.change_count == 2 && candidate.hold_b_steps)
        name += " / " + std::to_string(candidate.hold_a_steps + *candidate.hold_b_steps);
    return name + "カウント）";
}

std::optional<CompositeTimbre> makePatchSlideTimbre(
    const PatchSlideCandidate& candidate, std::string* error) {
    if (error) error->clear();
    const auto reject = [&](const char* message) -> std::optional<CompositeTimbre> {
        if (error) *error = message;
        return std::nullopt;
    };
    const auto valid_patch = [](int patch) { return patch >= 0 && patch <= 14; };
    const auto valid_hold = [](int hold) { return hold == 1 || hold == 2; };
    if (!valid_patch(candidate.patch_a) || !valid_patch(candidate.patch_b))
        return reject("PatchSlide requires OPLL ROM patches @0 through @14.");
    if (candidate.change_count != 1 && candidate.change_count != 2)
        return reject("PatchSlide requires one or two patch changes.");
    if (!valid_hold(candidate.hold_a_steps))
        return reject("PatchSlide holds must be one or two counts.");
    std::string expected_hold = "h" + std::to_string(candidate.hold_a_steps);
    if (candidate.change_count == 2) {
        if (!candidate.patch_c || !valid_patch(*candidate.patch_c)
            || !candidate.hold_b_steps || !valid_hold(*candidate.hold_b_steps))
            return reject("A two-change PatchSlide requires its final patch and second hold.");
        expected_hold += std::to_string(*candidate.hold_b_steps);
    } else if (candidate.patch_c || candidate.hold_b_steps) {
        return reject("A one-change PatchSlide must not contain a second change.");
    }
    if (candidate.hold_pattern != expected_hold)
        return reject("PatchSlide hold pattern does not match its count values.");

    CompositeTimbre sound;
    sound.name = patchSlideDisplayName(candidate);
    sound.tags = candidate.tags;
    sound.memo = "OPLL PatchSlide: " + candidate.candidate_id;
    CompositeLayer layer;
    layer.name = "OPLL PatchSlide";
    layer.source = TimbreSource::Opll;
    layer.base_opll_rom = static_cast<std::uint8_t>(candidate.patch_a);
    layer.volume_envelope.events.push_back({
        .kind = EnvelopeEventKind::Volume, .value = 15, .count = 0});
    layer.timbre_automation.push_back({
        .kind = EnvelopeEventKind::Timbre, .value = candidate.patch_a,
        .count = 0, .timbre_pick = TimbrePick::OpllRom});
    layer.timbre_automation.push_back({
        .kind = EnvelopeEventKind::Timbre, .value = candidate.patch_b,
        .count = static_cast<std::uint32_t>(candidate.hold_a_steps),
        .timbre_pick = TimbrePick::OpllRom});
    if (candidate.change_count == 2) {
        layer.timbre_automation.push_back({
            .kind = EnvelopeEventKind::Timbre, .value = *candidate.patch_c,
            .count = static_cast<std::uint32_t>(candidate.hold_a_steps + *candidate.hold_b_steps),
            .timbre_pick = TimbrePick::OpllRom});
    }
    // A final full-volume count leaves the last patch sounding until key-off.
    // An explicit initial @ also restores candidates whose patches are all
    // equal. The shared compiler recognizes it and supplies all waits.
    layer.envelope_timeline.length_counts = layer.timbre_automation.back().count + 1;
    sound.layers.push_back(std::move(layer));
    return sound;
}

namespace {
bool validPatchSlideTimeline(const EnvelopeTimeline& timeline) {
    return timeline.length_counts > 0
        && timeline.length_counts <= EnvelopeTimeline::kMaximumLengthCounts
        && timeline.loop_start_count.has_value() == timeline.loop_end_count.has_value()
        && (!timeline.loop_start_count
            || (*timeline.loop_start_count < *timeline.loop_end_count
                && *timeline.loop_end_count <= timeline.length_counts));
}

bool referencesSnapshot(const CompositeTimbre& sound, std::uint64_t id) {
    for (const auto& layer : sound.layers) {
        if (layer.base_timbre && layer.base_timbre->library_id == id) return true;
        for (const auto* events : {&layer.volume_envelope.events,
                                  &layer.pitch_envelope.events, &layer.timbre_automation}) {
            if (std::any_of(events->begin(), events->end(), [id](const auto& event) {
                    return envelopeEventReferencesLibrary(event, id);
                })) return true;
        }
    }
    return false;
}
}  // namespace

PatchSlideEnvelopeApplyResult preparePatchSlideEnvelopeApply(
    const CompositeTimbre& original, std::size_t layer_index,
    const PatchSlideCandidate& candidate,
    const std::optional<EnvelopeTimeline>& approved_timeline) {
    PatchSlideEnvelopeApplyResult result;
    if (layer_index >= original.layers.size()
        || original.layers[layer_index].source != TimbreSource::Opll) {
        result.error = "Select an OPLL layer before applying PatchSlide.";
        return result;
    }
    const auto& source = original.layers[layer_index];
    if (source.volume_envelope.kind != EnvelopeKind::Sequence) {
        result.error = "PatchSlide requires a sequence envelope; rate envelopes are unchanged.";
        return result;
    }
    if (!validPatchSlideTimeline(source.envelope_timeline)) {
        result.error = "The selected envelope has an invalid length or loop range.";
        return result;
    }
    auto slide = makePatchSlideTimbre(candidate, &result.error);
    if (!slide) return result;
    const auto& replacement = slide->layers.front();
    auto timeline = source.envelope_timeline;
    timeline.length_counts = std::max(timeline.length_counts,
        replacement.envelope_timeline.length_counts);

    if (timeline.loop_start_count) {
        PatchSlideEnvelopeLoopConflict conflict;
        for (const auto& event : replacement.timbre_automation) {
            if (event.count >= *timeline.loop_end_count)
                conflict.unreachable_counts.push_back(event.count);
            else if (event.count > *timeline.loop_start_count)
                conflict.repeated_counts.push_back(event.count);
        }
        if (!conflict.repeated_counts.empty() || !conflict.unreachable_counts.empty()) {
            // New @ events are before '[' at a shared count, so the final @
            // can share the new loop-start count without repeating.
            conflict.proposed_timeline = timeline;
            conflict.proposed_timeline.loop_start_count =
                replacement.timbre_automation.back().count;
            conflict.proposed_timeline.loop_end_count = std::max(
                *timeline.loop_end_count, *conflict.proposed_timeline.loop_start_count + 1);
            conflict.proposed_timeline.length_counts = std::max(
                timeline.length_counts, *conflict.proposed_timeline.loop_end_count);
            if (!approved_timeline) {
                result.loop_conflict = std::move(conflict);
                return result;
            }
            if (*approved_timeline != conflict.proposed_timeline) {
                result.error = "The approved loop adjustment does not match this PatchSlide plan.";
                return result;
            }
            timeline = *approved_timeline;
        } else if (approved_timeline && *approved_timeline != timeline) {
            result.error = "No loop adjustment is required for this PatchSlide plan.";
            return result;
        }
    } else if (approved_timeline && *approved_timeline != timeline) {
        result.error = "A loop adjustment cannot be added to a non-looping envelope.";
        return result;
    }
    if (!validPatchSlideTimeline(timeline)) {
        result.error = "PatchSlide would exceed the envelope length or loop limits.";
        return result;
    }
    // Extending END must not activate a previously hidden volume target/ramp.
    // Keep that authoring intact and ask the user to resolve it first.
    if (timeline.length_counts > source.envelope_timeline.length_counts
        && std::any_of(source.volume_envelope.events.begin(), source.volume_envelope.events.end(),
            [&](const auto& event) {
                return event.kind == EnvelopeEventKind::Volume
                    && event.count > source.envelope_timeline.length_counts
                    && event.count <= timeline.length_counts;
            })) {
        result.error = "Extending this envelope would activate a volume event beyond its current END.";
        return result;
    }

    auto sound = original;
    auto& layer = sound.layers[layer_index];
    std::vector<std::uint64_t> removed_ids;
    if (layer.base_timbre) removed_ids.push_back(layer.base_timbre->library_id);
    layer.base_timbre.reset();
    layer.base_opll_rom = replacement.base_opll_rom;
    const auto erase_commands = [&](auto& events) {
        std::erase_if(events, [&](const auto& event) {
            if (event.kind == EnvelopeEventKind::Timbre) {
                if (event.timbre_pick == TimbrePick::Library)
                    removed_ids.push_back(event.target_library_id);
                return true;
            }
            return event.kind == EnvelopeEventKind::RegisterWrite;
        });
    };
    erase_commands(layer.volume_envelope.events);
    erase_commands(layer.pitch_envelope.events);
    erase_commands(layer.timbre_automation);
    layer.opll_tl_auto = {};
    layer.opll_fb_auto = {};
    layer.opll_tl_modulation = {};
    layer.opll_fb_modulation = {};
    std::erase_if(layer.register_auto_profiles, [](const auto& profile) {
        return profile.target == OpllRegisterAutoTarget::TotalLevel
            || profile.target == OpllRegisterAutoTarget::Feedback;
    });
    const auto tl_or_fb = [](CompositeModulationTarget target) {
        return target == CompositeModulationTarget::TotalLevel
            || target == CompositeModulationTarget::Feedback;
    };
    std::erase_if(layer.modulation_profiles,
        [&](const auto& profile) { return tl_or_fb(profile.target); });
    std::erase_if(layer.derived_command_placements,
        [&](const auto& placement) { return tl_or_fb(placement.target); });
    // The serializer holds the final authored volume through an extended END;
    // retaining the event vector preserves ramps, zero volume and key-off.
    layer.envelope_timeline.length_counts = timeline.length_counts;
    if (timeline.loop_start_count
        && !setCompositeLoopRange(layer, *timeline.loop_start_count, *timeline.loop_end_count)) {
        result.error = "The approved loop adjustment is outside the envelope range.";
        return result;
    }
    // Existing boundary authoring follows the editor loop-move rules above.
    // New candidate @ commands deliberately belong to the first pass only.
    layer.timbre_automation.insert(layer.timbre_automation.end(),
        replacement.timbre_automation.begin(), replacement.timbre_automation.end());
    std::erase_if(sound.embedded_timbres, [&](const auto& snapshot) {
        return std::find(removed_ids.begin(), removed_ids.end(), snapshot.library_id) != removed_ids.end()
            && !referencesSnapshot(sound, snapshot.library_id);
    });
    const auto formatted = formatMgsCompositeEnvelope(layer, layer.envelope_number);
    if (!formatted.valid() || formatted.output_cutoff_count) {
        result.error = "The complete PatchSlide envelope cannot be exported within the MGSC envelope limits.";
        return result;
    }
    result.timbre = std::move(sound);
    return result;
}

}  // namespace mgstc::engine
