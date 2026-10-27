// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_timbre.hpp"

#include <utility>

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

}  // namespace mgstc::engine
