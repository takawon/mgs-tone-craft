// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_step_edit.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace mgstc::engine;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
CompositeLayer fixture() {
    CompositeLayer layer;
    layer.volume = 15;
    layer.envelope_timeline.length_counts = 12;
    layer.volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 12, .count = 0},
        {.kind = EnvelopeEventKind::Volume, .value = 9, .count = 3},
        {.kind = EnvelopeEventKind::Volume, .value = 6, .count = 7},
    };
    layer.pitch_envelope.events = {
        {.kind = EnvelopeEventKind::Pitch, .value = 10, .count = 2},
        {.kind = EnvelopeEventKind::Pitch, .value = -3, .count = 4},
        {.kind = EnvelopeEventKind::Pitch, .value = 2, .count = 7},
    };
    layer.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .value = 2, .count = 3,
            .timbre_pick = TimbrePick::OpllRom},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 23, .count = 4},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 25, .count = 7},
    };
    return layer;
}
void testInsertAndIdentity() {
    const auto source = fixture();
    const auto result = editCompositeSteps(source, CompositeStepEditLane::All, 5, 3);
    require(result.accepted && result.applied_delta == 3, "all insertion rejected");
    require(result.layer.envelope_timeline.length_counts == 15, "END not shifted");
    require(result.layer.pitch_envelope.events.back().count == 10, "pitch suffix not shifted");
    require(result.layer.pitch_envelope.events.size() == 3, "insert repeated cumulative pitch command");
    const auto samples = sampleCompositeVolumeLane(result.layer, 15);
    require(samples[5] == samples[4] && samples[6] == samples[4]
        && samples[7] == samples[4], "insert did not duplicate held left volume");
    require(editCompositeSteps(source, CompositeStepEditLane::All, 5, 0).layer == source,
        "zero delta changed source");
    require(source == fixture(), "pure transformation mutated original");
    const auto head = editCompositeSteps(source, CompositeStepEditLane::Volume, 0, 2);
    require(head.accepted, "first-step insertion rejected");
    const auto head_samples = sampleCompositeVolumeLane(head.layer, 5);
    require(head_samples[0] == 15 && head_samples[1] == 15 && head_samples[2] == 12,
        "step zero did not use initial volume");
}
void testDeleteStateAndIsolation() {
    const auto source = fixture();
    const auto result = editCompositeSteps(source, CompositeStepEditLane::All, 6, -4);
    require(result.accepted && result.layer.envelope_timeline.length_counts == 8,
        "all deletion rejected");
    require(result.layer.pitch_envelope.events.size() == 3
        && result.layer.pitch_envelope.events[0].count == 2
        && result.layer.pitch_envelope.events[1].count == 2
        && result.layer.pitch_envelope.events[0].value == 10
        && result.layer.pitch_envelope.events[1].value == -3,
        "deletion lost ordered cumulative pitch prefix");
    require(result.layer.timbre_automation.front().count == 2
        && result.layer.timbre_automation.front().value == 2,
        "deletion lost held timbre at join");
    const auto samples = sampleCompositeVolumeLane(result.layer, 8);
    require(samples[2] == 9 && samples[3] == 6, "deletion changed held suffix volume");
    const auto pitch = editCompositeSteps(source, CompositeStepEditLane::Pitch, 6, -4);
    require(pitch.accepted && pitch.layer.volume_envelope == source.volume_envelope
        && pitch.layer.timbre_automation == source.timbre_automation
        && pitch.layer.envelope_timeline == source.envelope_timeline,
        "pitch row changed other rows or END");
    const auto y = editCompositeSteps(source, CompositeStepEditLane::RegisterWrite, 5, 2);
    require(y.accepted && y.layer.timbre_automation.front() == source.timbre_automation.front()
        && y.layer.timbre_automation.back().count == 9,
        "manual y row moved timbre row");
}
void testLimitsAndLoops() {
    auto source = fixture();
    source.envelope_timeline.loop_start_count = 3;
    source.envelope_timeline.loop_end_count = 8;
    const auto moved = editCompositeSteps(source, CompositeStepEditLane::All, 5, 2);
    require(moved.accepted && moved.layer.envelope_timeline.loop_start_count == 3
        && moved.layer.envelope_timeline.loop_end_count == 10, "loop markers did not move");
    const auto clamped = editCompositeSteps(source, CompositeStepEditLane::All, 5, -3);
    require(clamped.accepted && clamped.layer.envelope_timeline.loop_start_count == 2,
        "deleted loop marker did not clamp to boundary");
    const auto collapsed = editCompositeSteps(source, CompositeStepEditLane::All, 9, -7);
    require(!collapsed.accepted && collapsed.layer == source, "collapsed loop accepted or mutated source");
    source.pitch_envelope.events.back().count = 12;
    require(!editCompositeSteps(source, CompositeStepEditLane::Pitch, 5, 1).accepted,
        "row insertion truncated content beyond END");
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 5, 501).accepted,
        "512-step capacity exceeded");
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 1, -2).accepted,
        "negative timeline boundary accepted");
    source.volume_envelope.kind = EnvelopeKind::Rate;
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 5, 1).accepted,
        "rate editing unexpectedly enabled");
}
void testCurveModulationAndMorph() {
    auto source = fixture();
    source.opll_tl_auto = {.mode = OpllRegisterAutoMode::FreeCurve,
        .start_count = 2, .coarseness = 2, .free_curve = {10, 20, 30, 40}};
    const auto insert = editCompositeSteps(source, CompositeStepEditLane::Tl, 5, 2);
    require(insert.accepted && insert.layer.opll_tl_auto.coarseness == 1
        && insert.layer.opll_tl_auto.free_curve == std::vector<std::uint8_t>{
            10, 10, 20, 20, 20, 20, 30, 30, 40},
        "free-curve insertion lost held points");
    const auto remove = editCompositeSteps(source, CompositeStepEditLane::Tl, 5, -2);
    require(remove.accepted && remove.layer.opll_tl_auto.free_curve
        == std::vector<std::uint8_t>{10, 20, 30, 30, 40},
        "free-curve deletion changed retained suffix");
    source.pitch_modulation = {.enabled = true, .delay = 5, .depth = 2, .speed = 3};
    const auto delay = editCompositeSteps(source, CompositeStepEditLane::Pitch, 4, 3);
    require(delay.accepted && delay.layer.pitch_modulation.delay == 8
        && delay.layer.pitch_modulation.depth == 2, "modulation start or settings incorrect");
    source.envelope_timeline.length_counts = 300;
    source.pitch_modulation.delay = 255;
    require(!editCompositeSteps(source, CompositeStepEditLane::Pitch, 250, 1).accepted,
        "modulation byte delay wrapped");
    source = fixture();
    source.source = TimbreSource::Scc;
    source.timbre_automation.push_back({.kind = EnvelopeEventKind::Timbre, .count = 9,
        .target_library_id = 3, .scc_morph = {.enabled = true, .intermediate_count = 2,
            .explicit_plan = SccMorphPlan{{{0, 0}, {0.3, 2}, {0.7, 4}, {1, 6}}}}});
    const auto morph = editCompositeSteps(source, CompositeStepEditLane::Timbre, 5, 1);
    require(morph.accepted && morph.layer.timbre_automation.back().scc_morph.enabled
        && !morph.layer.timbre_automation.back().scc_morph.explicit_plan,
        "changed morph duration retained stale explicit plan");
    require(!editCompositeSteps(source, CompositeStepEditLane::Timbre, 9, -4).accepted,
        "morph accepted too few intermediate slots");
}
void testPatchRegisterOrderingAndUnreachableContent() {
    auto source = fixture();
    source.source = TimbreSource::Opll;
    source.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .value = 2, .count = 3,
            .timbre_pick = TimbrePick::OpllRom},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 23, .count = 3},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 3, .secondary = 4, .count = 4},
    };
    const auto restore = editCompositeSteps(source, CompositeStepEditLane::All, 6, -4);
    require(restore.accepted && restore.layer.timbre_automation.size() == 3
        && restore.layer.timbre_automation[0].kind == EnvelopeEventKind::Timbre
        && restore.layer.timbre_automation[1].secondary == 23,
        "patch followed by same-count y was not restored in order");
    source.timbre_automation.push_back({.kind = EnvelopeEventKind::Timbre,
        .value = 3, .count = 6, .timbre_pick = TimbrePick::OpllRom});
    const auto reset = editCompositeSteps(source, CompositeStepEditLane::All, 6, -4);
    require(reset.accepted && reset.layer.timbre_automation.size() == 1
        && reset.layer.timbre_automation[0].value == 3,
        "removed shared-register writes leaked through suffix patch reset");
    source = fixture();
    source.pitch_envelope.events.back().count = 20;
    const auto unreachable = editCompositeSteps(source, CompositeStepEditLane::All, 5, 2);
    require(unreachable.accepted && unreachable.layer.pitch_envelope.events.back().count == 22,
        "all-row edit discarded saved content beyond END");
}
void testBracketAwareJoinAndCheckedRanges() {
    auto source = fixture();
    source.source = TimbreSource::Opll;
    source.envelope_timeline.loop_start_count = 2;
    source.envelope_timeline.loop_end_count = 8;
    source.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .value = 2, .count = 7,
            .timbre_pick = TimbrePick::OpllRom, .after_loop_start = true},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 23,
            .count = 7, .after_loop_start = true},
        {.kind = EnvelopeEventKind::Timbre, .value = 3, .count = 8,
            .timbre_pick = TimbrePick::OpllRom, .after_loop_start = false},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 99,
            .count = 8, .after_loop_start = false},
    };
    const auto joined = editCompositeSteps(source, CompositeStepEditLane::All, 8, -2);
    require(joined.accepted && joined.layer.envelope_timeline.loop_end_count == 6,
        "bracket-aware join rejected");
    const auto& events = joined.layer.timbre_automation;
    require(events.size() == 4
        && events[0].kind == EnvelopeEventKind::Timbre && events[0].value == 2
        && events[0].count == 6 && events[0].after_loop_start
        && events[1].kind == EnvelopeEventKind::RegisterWrite && events[1].secondary == 23
        && events[1].count == 6 && events[1].after_loop_start
        && events[2].kind == EnvelopeEventKind::Timbre && events[2].value == 3
        && !events[2].after_loop_start
        && events[3].secondary == 99 && !events[3].after_loop_start,
        "unreachable suffix suppressed or changed executable restoration");
    source = fixture();
    const auto outside_end = editCompositeSteps(source, CompositeStepEditLane::Volume, 11, 3);
    require(!outside_end.accepted && outside_end.layer == source,
        "sparse row insertion created holds beyond fixed END");
    source.pitch_envelope.events.back().count = std::numeric_limits<std::uint32_t>::max();
    const auto overflow_event = editCompositeSteps(source, CompositeStepEditLane::All, 5, 2);
    require(!overflow_event.accepted && overflow_event.layer == source,
        "event count overflow wrapped into timeline");
    source = fixture();
    source.envelope_timeline.loop_start_count = std::numeric_limits<std::uint32_t>::max();
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 5, 2).accepted,
        "loop count overflow wrapped into timeline");
    source = fixture();
    source.opll_fb_auto = {.mode = OpllRegisterAutoMode::Rise,
        .start_count = std::numeric_limits<std::uint32_t>::max()};
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 5, 2).accepted,
        "automatic count overflow wrapped into timeline");
    source = fixture();
    source.volume_envelope.events.back().automatic = true;
    source.volume_envelope.events.back().secondary = std::numeric_limits<std::int32_t>::max();
    require(!editCompositeSteps(source, CompositeStepEditLane::All, 5, 2).accepted,
        "ramp origin overflow accepted");
}
} // namespace

int main() {
    try {
        testInsertAndIdentity();
        testDeleteStateAndIsolation();
        testLimitsAndLoops();
        testCurveModulationAndMorph();
        testPatchRegisterOrderingAndUnreachableContent();
        testBracketAwareJoinAndCheckedRanges();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
