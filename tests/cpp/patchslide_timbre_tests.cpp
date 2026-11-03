// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_timbre.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace mgstc::engine;
std::ofstream runtime_trace;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

PatchSlideCandidate candidate(int a, int b, int ha, int c = -1, int hb = 0) {
    PatchSlideCandidate item;
    item.candidate_id = "slide_" + std::to_string(a) + "_" + std::to_string(b);
    item.patch_a = a;
    item.patch_b = b;
    item.change_count = c < 0 ? 1 : 2;
    item.hold_a_steps = ha;
    item.hold_pattern = "h" + std::to_string(ha);
    if (c >= 0) {
        item.patch_c = c;
        item.hold_b_steps = hb;
        item.candidate_id += "_" + std::to_string(c);
        item.hold_pattern += std::to_string(hb);
    }
    item.candidate_id += "_" + item.hold_pattern;
    item.tags = {"Bright", "Decay"};
    return item;
}

std::vector<std::pair<Tick, int>> patchEvents(const CompositeTimbre& sound,
                                            bool early_key_off = false,
                                            const char* trace_id = nullptr) {
    EngineCore engine;
    require(compileCompositeProgram(engine, sound), "Native program must compile without a database");
    auto& runtime = engine.session();
    require(runtime.queueNoteOn(8, 60), "Queue OPLL note");
    std::vector<std::pair<Tick, int>> patches;
    bool saw_key_on = false;
    bool saw_key_off = false;
    const int key_off_count = early_key_off ? 1 : 30;
    for (int tick = 0; tick < (early_key_off ? 9 : 34); ++tick) {
        if (tick == key_off_count)
            require(runtime.queueKeyOff(8), "Queue OPLL key off");
        require(runtime.processTick().ok(), "OPLL tick succeeds");
        for (const auto& write : runtime.writes()) {
            if (write.chip != ChipId::Opll || write.track != 8) continue;
            if (trace_id != nullptr && runtime_trace.is_open())
                runtime_trace << trace_id << ',' << write.tick << ',' << write.sequence << ','
                    << static_cast<unsigned>(write.address) << ',' << static_cast<unsigned>(write.value)
                    << ',' << static_cast<unsigned>(write.reason) << '\n';
            if (write.address == 0x20) {
                if ((write.value & 0x10) != 0) {
                    require(tick == 0, "Patch change must retain the original key-on");
                    saw_key_on = true;
                } else {
                    require(tick == key_off_count, "No intermediate key-off");
                    saw_key_off = true;
                }
            }
            if (write.address == 0x30 && write.reason == WriteReason::Patch)
                patches.emplace_back(write.tick, (write.value >> 4) - 1);
        }
    }
    require(saw_key_on && saw_key_off, "Key-on and key-off reach the chip");
    return patches;
}

void verifyCandidate(const PatchSlideCandidate& item) {
    std::string error = "stale";
    auto sound = makePatchSlideTimbre(item, &error);
    require(sound.has_value() && error.empty(), "Valid definition converts");
    const auto display=patchSlideDisplayName(item);
    require(display.find("OPLL @") == 0 && display.find("\xe5\x9b\x9e\xe5\xa4\x89\xe6\x9b\xb4") == std::string::npos,
        "Redundant change-count text remains in the display name");
    require(display.find(std::to_string(item.hold_a_steps)) != std::string::npos,
        "Count information disappeared from the display name");
    require(sound->layers.size() == 1, "One OPLL layer");
    const auto& layer = sound->layers.front();
    require(layer.source == TimbreSource::Opll && layer.channel == 0, "Native OPLL channel");
    require(layer.base_opll_rom == item.patch_a && !layer.base_timbre, "ROM numbering stays @0 through @14");
    require(layer.timbre_automation.size() == static_cast<std::size_t>(item.change_count + 1), "Preserve initial restoration and every patch change");
    require(layer.timbre_automation[0].count == 0 && layer.timbre_automation[0].value == item.patch_a,
            "Initial envelope patch restoration is explicit even for same-patch candidates");
    require(layer.timbre_automation[1].count == static_cast<unsigned>(item.hold_a_steps), "First hold is a 60 Hz count");
    const auto end_count = static_cast<unsigned>(item.hold_a_steps + item.hold_b_steps.value_or(0));
    require(layer.timbre_automation.back().count == end_count, "Second hold is cumulative");
    require(layer.envelope_timeline.length_counts == end_count + 1, "Final patch has a full-volume terminal count");
    require(!layer.envelope_timeline.loop_start_count && !layer.envelope_timeline.loop_end_count, "Final patch sustains without a loop");
    require(sound->embedded_timbres.empty() && sound->tags == item.tags, "Self-contained native sound retains initial tags");

    const auto envelope = formatMgsCompositeEnvelope(layer, 0);
    require(envelope.valid(), "Shared envelope compiler accepts conversion");
    require(envelope.bytecode.size() >= 2 && envelope.bytecode[0] == 0x10
                && envelope.bytecode[1] == item.patch_a,
            "Shared leading base-patch restoration remains active");
    std::vector<std::pair<Tick, int>> expected{{0, item.patch_a}, {0, item.patch_a},
        {static_cast<Tick>(item.hold_a_steps), item.patch_b}};
    if (item.patch_c) expected.emplace_back(end_count, *item.patch_c);
    require(patchEvents(*sound, false, item.candidate_id.c_str()) == expected,
            "Runtime preserves patch timing, order, and same-number writes");

    const auto source = formatMgsComposite(*sound);
    require(source.valid(), "Existing MGSC export accepts conversion");
    const auto restored_mgs = parseMgsComposite(source.source);
    require(restored_mgs.valid() && restored_mgs.timbre.layers.size() == 1, "Existing MGSC import accepts export");
    require(patchEvents(restored_mgs.timbre) == expected, "MGSC round trip preserves playback events");

    const auto file = CompositeTimbreLibrary::serializeTimbreFile(*sound, 0);
    const auto restored = CompositeTimbreLibrary::deserializeTimbreFile(file, &error);
    require(restored && *restored == *sound, "Portable save restores every native field without the DB");
    auto payload = deserializeCompositeSoundPayload(serializeCompositeSoundPayload(*sound),
        kCompositeSoundPayloadVersion, &error);
    require(payload && payload->layers == sound->layers, "Sound payload retains native layer without the DB");
    require(patchEvents(*payload) == expected, "Restored sound payload plays without the DB");
    CompositeTimbreLibrary library;
    const auto saved_id = library.add(*sound, 0);
    require(saved_id != 0 && library.find(saved_id) != nullptr, "Native PatchSlide registers in the existing composite library");
    auto registered = library.find(saved_id)->timbre;
    registered.name = "User renamed PatchSlide";
    registered.tags = {"User tag"};
    registered.memo = "User memo";
    require(library.update(saved_id, registered, 1), "Registered PatchSlide metadata remains editable");
    const auto restored_library = CompositeTimbreLibrary::deserialize(library.serialize(), &error);
    require(restored_library && restored_library->find(saved_id)
                && restored_library->find(saved_id)->timbre == registered,
            "Existing library serialization retains editable PatchSlide metadata and native sound");
    require(patchEvents(restored_library->find(saved_id)->timbre) == expected,
            "Registered library sound plays after reload without the DB");

    auto classified = item;
    classified.predicted_partition = "AAAA";
    classified.human_pattern = "ABBB";
    classified.auto_safe = true;
    const auto classified_sound = makePatchSlideTimbre(classified);
    require(classified_sound && classified_sound->layers == sound->layers,
            "Stability metadata never changes playback definitions");
}

void testAllDefinitions() {
    unsigned count = 0;
    for (int a = 0; a < 15; ++a) {
        for (int b = 0; b < 15; ++b) {
            for (int ha = 1; ha <= 2; ++ha) {
                require(makePatchSlideTimbre(candidate(a, b, ha)).has_value(), "All one-change definitions convert");
                ++count;
                for (int c = 0; c < 15; ++c) {
                    for (int hb = 1; hb <= 2; ++hb) {
                        require(makePatchSlideTimbre(candidate(a, b, ha, c, hb)).has_value(), "All two-change definitions convert");
                        ++count;
                    }
                }
            }
        }
    }
    require(count == 13950, "Complete canonical candidate coverage");
}

void testEnvelopePartialReplacement() {
    auto original = *makePatchSlideTimbre(candidate(1, 2, 1));
    original.name = "Authored composite";
    original.memo = "Preserve metadata";
    original.playback_tempo = 137;
    SavedTimbreReference base;
    base.library_id = 101;
    base.source = TimbreSource::Opll;
    base.opll_registers = {1, 2, 3, 4, 5, 6, 7, 8};
    auto later = base; later.library_id = 102;
    auto shared = base; shared.library_id = 103;
    auto unrelated = base; unrelated.library_id = 104;
    original.embedded_timbres = {base, later, shared, unrelated};
    auto& layer = original.layers.front();
    layer.base_timbre = base;
    layer.base_opll_rom.reset();
    layer.name = "Keep channel settings";
    layer.channel = 3;
    layer.envelope_number = 7;
    layer.relative_semitones = -3;
    layer.detune = 4;
    layer.micro_detune = 9;
    layer.start_delay_value = 1;
    layer.volume = 12;
    layer.opll_sustain = true;
    layer.envelope_timeline.length_counts = 2;
    layer.volume_envelope.events = {
        {.kind=EnvelopeEventKind::Volume, .value=15, .count=0},
        {.kind=EnvelopeEventKind::Volume, .value=9, .count=1}};
    layer.pitch_envelope.events = {
        {.kind=EnvelopeEventKind::Pitch, .value=1, .count=1}};
    layer.timbre_automation = {
        {.kind=EnvelopeEventKind::Timbre, .count=0, .target_library_id=101},
        {.kind=EnvelopeEventKind::Timbre, .count=1, .target_library_id=102},
        {.kind=EnvelopeEventKind::RegisterWrite, .value=2, .secondary=33, .count=1},
        {.kind=EnvelopeEventKind::Timbre, .count=51, .target_library_id=103, .after_loop_start=true},
        {.kind=EnvelopeEventKind::RegisterWrite, .value=56, .secondary=255, .count=65'535}};
    layer.opll_tl_auto = {.mode=OpllRegisterAutoMode::Rise, .depth=7};
    layer.opll_fb_auto = {.mode=OpllRegisterAutoMode::FreeCurve, .free_curve={1,2,3}};
    layer.opll_tl_modulation = {.enabled=true, .depth=3, .speed=1};
    layer.opll_fb_modulation = {.enabled=true, .depth=2, .speed=1};
    layer.modulation_profiles = {
        {.target=CompositeModulationTarget::TotalLevel, .count=0, .parameters={.enabled=true}},
        {.target=CompositeModulationTarget::Feedback, .count=1, .parameters={.enabled=true}},
        {.target=CompositeModulationTarget::Pitch, .count=0},
        {.target=CompositeModulationTarget::Volume, .count=0}};
    layer.register_auto_profiles = {
        {.target=OpllRegisterAutoTarget::TotalLevel, .count=0, .lane=layer.opll_tl_auto},
        {.target=OpllRegisterAutoTarget::Feedback, .count=1, .lane=layer.opll_fb_auto},
        {.target=OpllRegisterAutoTarget::Noise, .count=0}};
    layer.derived_command_placements = {
        {.count=1, .target=CompositeModulationTarget::TotalLevel},
        {.count=1, .target=CompositeModulationTarget::Feedback},
        {.count=1, .target=CompositeModulationTarget::Pitch}};
    auto other = layer;
    other.base_timbre = shared;
    other.channel = 4;
    other.envelope_number = 8;
    original.layers.push_back(other);
    CompositeLayer psg;
    psg.source = TimbreSource::Psg;
    psg.timbre_automation = {{.kind=EnvelopeEventKind::RegisterWrite, .value=6, .secondary=15}};
    psg.psg_noise_auto = {.mode=OpllRegisterAutoMode::Rise, .depth=10};
    original.layers.push_back(psg);
    const auto before = original;
    const auto item = candidate(0, 5, 2, 14, 2);
    const auto result = preparePatchSlideEnvelopeApply(original, 0, item);
    require(result.timbre.has_value() && result.error.empty() && !result.loop_conflict,
        "Partial envelope replacement succeeds");
    require(original == before, "Preparing an apply never mutates caller or Undo snapshot");
    auto expected = before;
    auto& dest = expected.layers.front();
    dest.base_timbre.reset();
    dest.base_opll_rom = 0;
    dest.timbre_automation = makePatchSlideTimbre(item)->layers.front().timbre_automation;
    dest.envelope_timeline.length_counts = 5;
    dest.opll_tl_auto = {};
    dest.opll_fb_auto = {};
    dest.opll_tl_modulation = {};
    dest.opll_fb_modulation = {};
    dest.modulation_profiles.erase(dest.modulation_profiles.begin(), dest.modulation_profiles.begin()+2);
    dest.register_auto_profiles.erase(dest.register_auto_profiles.begin(), dest.register_auto_profiles.begin()+2);
    dest.derived_command_placements.erase(dest.derived_command_placements.begin(), dest.derived_command_placements.begin()+2);
    // The other OPLL layer references all old originals; no snapshot is lost.
    require(*result.timbre == expected,
        "Only @/TL/FB/y and necessary END change; all metadata, other lanes and layers survive");
    const auto formatted = formatMgsCompositeEnvelope(result.timbre->layers.front(), 7);
    require(formatted.valid() && formatted.body.find('y') == std::string::npos,
        "Deleted manual and generated y never return in MGSC output");
    auto exclusive = before;
    exclusive.layers.erase(exclusive.layers.begin()+1);
    const auto cleaned = preparePatchSlideEnvelopeApply(exclusive, 0, item);
    require(cleaned.timbre && cleaned.timbre->embedded_timbres == std::vector<SavedTimbreReference>{unrelated},
        "Only newly unreferenced removed originals are pruned; unrelated snapshots stay");
    std::string error;
    const auto bytes = CompositeTimbreLibrary::serializeTimbreFile(*cleaned.timbre, 0);
    const auto restored = CompositeTimbreLibrary::deserializeTimbreFile(bytes, &error);
    require(restored && *restored == *cleaned.timbre, "Partial apply portable save retains all authoring and references");
    const auto preview = preparePatchSlideEnvelopeApply(original, 0, item);
    require(preview.timbre && *preview.timbre == *result.timbre,
        "Preview and commit use the identical pure transformation");
}

void testEnvelopeHeldExtensionAndLimits() {
    const auto item = candidate(0, 1, 2, 14, 2);
    for (const auto length : {2U, 5U, 8U, EnvelopeTimeline::kMaximumLengthCounts}) {
        auto source = *makePatchSlideTimbre(candidate(2, 3, 1));
        auto& layer = source.layers.front();
        layer.timbre_automation.clear();
        layer.envelope_timeline.length_counts = length;
        layer.volume_envelope.events = {
            {.kind=EnvelopeEventKind::Volume, .value=13, .count=0},
            {.kind=EnvelopeEventKind::Volume, .value=0, .count=1}};
        const auto before_volume = layer.volume_envelope;
        const auto result = preparePatchSlideEnvelopeApply(source, 0, item);
        require(result.timbre && result.timbre->layers[0].volume_envelope == before_volume,
            "Short/equal/long envelopes retain every existing volume event");
        require(result.timbre->layers[0].envelope_timeline.length_counts == std::max(5U, length),
            "END extends only to final @ plus one and never truncates");
        const auto volumes = sampleCompositeVolumeLane(result.timbre->layers[0], 10);
        require(volumes[0] == 13 && std::all_of(volumes.begin()+1,volumes.end(),[](auto v){return v==0;}),
            "Extension holds the last zero-volume step without restarting or raising volume");
    }
    auto source = *makePatchSlideTimbre(candidate(2, 3, 1));
    source.layers[0].envelope_timeline.length_counts = 2;
    source.layers[0].timbre_automation.clear();
    source.layers[0].volume_envelope.events = {
        {.kind=EnvelopeEventKind::Volume, .value=15, .count=0},
        {.kind=EnvelopeEventKind::Volume, .value=9, .count=2, .automatic=true}};
    const auto before = sampleCompositeVolumeLane(source.layers[0], 10);
    auto result = preparePatchSlideEnvelopeApply(source, 0, item);
    require(result.timbre && before == sampleCompositeVolumeLane(result.timbre->layers[0],10),
        "Ramps ending at original END retain samples and hold their final level");
    source.layers[0].volume_envelope.events.push_back({.kind=EnvelopeEventKind::Volume,.value=3,.count=4});
    const auto hidden = source;
    result = preparePatchSlideEnvelopeApply(source, 0, item);
    require(!result.timbre && !result.error.empty() && source==hidden,
        "Extension cannot silently activate a hidden volume target");
    source.layers[0].volume_envelope.events.pop_back();
    source.layers[0].envelope_timeline.length_counts = EnvelopeTimeline::kMaximumLengthCounts+1;
    require(!preparePatchSlideEnvelopeApply(source,0,item).timbre, "Out-of-range END is rejected without clamping");
    source.layers[0].envelope_timeline.length_counts = 300;
    source.layers[0].pitch_envelope.events.clear();
    for (std::uint32_t i=0;i<260;++i)
        source.layers[0].pitch_envelope.events.push_back({.kind=EnvelopeEventKind::Pitch,.value=1,.count=i});
    result = preparePatchSlideEnvelopeApply(source,0,item);
    require(!result.timbre && !result.error.empty(), "Partial MGSC prefix is rejected for PatchSlide apply");
    source.layers[0].volume_envelope.kind = EnvelopeKind::Rate;
    require(!preparePatchSlideEnvelopeApply(source,0,item).timbre, "Rate envelopes cannot silently become sequences");
    source.layers[0].source = TimbreSource::Scc;
    require(!preparePatchSlideEnvelopeApply(source,0,item).timbre
        && !preparePatchSlideEnvelopeApply(source,9,item).timbre, "Non-OPLL and stale indices are rejected");
}

void testEnvelopeInteriorRampPreservation() {
    auto source = *makePatchSlideTimbre(candidate(2,3,1));
    auto& layer = source.layers[0];
    layer.envelope_timeline.length_counts = 8;
    layer.timbre_automation.clear();
    layer.volume_envelope.events = {
        {.kind=EnvelopeEventKind::Volume,.value=15,.count=0},
        {.kind=EnvelopeEventKind::Volume,.value=0,.count=8,.automatic=true}};
    const auto original = source;
    const auto item = candidate(0,5,2,14,2);
    const auto result = preparePatchSlideEnvelopeApply(source,0,item);
    require(result.timbre.has_value(), "An ordinary automatic ramp accepts PatchSlide without reauthoring its volume steps");
    require(source == original && result.timbre->layers[0].volume_envelope == layer.volume_envelope,
        "PatchSlide leaves the existing volume values, counts, automatic and precise flags unchanged");
    require(result.error.empty(), "Existing volume-ramp boundary semantics do not cause rejection");
    require(sampleCompositeVolumeLane(layer,12) == std::vector<std::uint8_t>{15,15,14,12,10,8,6,4,2,0,0,0}
        && sampleCompositeVolumeLane(result.timbre->layers[0],12) == std::vector<std::uint8_t>{15,15,13,11,10,8,6,4,2,0,0,0},
        "Ordinary automatic ramps retain the established zero-time boundary rounding semantics");
    layer.volume_envelope.events.back().precise = true;
    const auto precise_before = sampleCompositeVolumeLane(layer,12);
    const auto precise = preparePatchSlideEnvelopeApply(source,0,item);
    require(precise.timbre.has_value(), "Existing precise ramp applies without changing authoring");
    require(precise.timbre->layers[0].volume_envelope == layer.volume_envelope,
        "An already precise authored ramp retains every existing volume step and flag");
    std::cout << "Existing precise ramp before:";
    for (const auto value : precise_before) std::cout << ' ' << unsigned(value);
    std::cout << "\nExisting precise ramp after:";
    for (const auto value : sampleCompositeVolumeLane(precise.timbre->layers[0],12)) std::cout << ' ' << unsigned(value);
    std::cout << '\n';
}

void testEnvelopeLoopApproval() {
    const auto item = candidate(0, 5, 2, 14, 2);
    auto source = *makePatchSlideTimbre(candidate(1, 2, 1));
    auto& layer = source.layers[0];
    layer.envelope_timeline = {.length_counts=3,.loop_start_count=1,.loop_end_count=3};
    layer.pitch_envelope.events = {
        {.kind=EnvelopeEventKind::Pitch,.value=1,.count=1,.after_loop_start=false},
        {.kind=EnvelopeEventKind::Pitch,.value=-1,.count=1,.after_loop_start=true},
        {.kind=EnvelopeEventKind::Pitch,.value=1,.count=3,.after_loop_start=true},
        {.kind=EnvelopeEventKind::Pitch,.value=2,.count=3,.after_loop_start=false},
        {.kind=EnvelopeEventKind::Pitch,.value=5,.count=4,.after_loop_start=false},
        {.kind=EnvelopeEventKind::Pitch,.value=6,.count=5,.after_loop_start=false}};
    layer.modulation_profiles = {
        {.target=CompositeModulationTarget::Pitch,.count=1,.after_loop_start=false},
        {.target=CompositeModulationTarget::Volume,.count=1,.after_loop_start=true}};
    const auto original = source;
    auto pending = preparePatchSlideEnvelopeApply(source,0,item);
    require(!pending.timbre && pending.error.empty() && pending.loop_conflict,
        "Repeated and unreachable candidate commands require approval before preview or apply");
    require(pending.loop_conflict->repeated_counts==std::vector<std::uint32_t>{2}
        && pending.loop_conflict->unreachable_counts==std::vector<std::uint32_t>{4},
        "Conflict identifies exact repeated and unreachable @ counts");
    require(source==original, "Rejecting or cancelling the pending plan leaves model unchanged");
    const auto proposed = pending.loop_conflict->proposed_timeline;
    require(proposed == EnvelopeTimeline{.length_counts=5,.loop_start_count=4,.loop_end_count=5},
        "Proposed loop adjustment keeps all @ first-pass and preserves a positive wait");
    auto wrong = proposed;wrong.loop_start_count=3;
    require(!preparePatchSlideEnvelopeApply(source,0,item,wrong).timbre,
        "An approval for different loop conditions is not reused");
    const auto approved = preparePatchSlideEnvelopeApply(source,0,item,proposed);
    require(approved.timbre && approved.timbre->layers[0].envelope_timeline==proposed,
        "Only exact approved loop adjustment is applied");
    const auto& adjusted = approved.timbre->layers[0];
    const auto& pitch = adjusted.pitch_envelope.events;
    require(pitch.size()==6 && pitch[0].value==1 && pitch[1].value==-1
        && !pitch[0].after_loop_start && !pitch[1].after_loop_start
        && pitch[0].command_order<pitch[1].command_order
        && pitch[2].value==1 && pitch[3].value==2
        && !pitch[2].after_loop_start && !pitch[3].after_loop_start
        && pitch[2].command_order<pitch[3].command_order,
        "Moved old loop boundaries flatten with first-pass execution order preserved");
    require(adjusted.modulation_profiles.size()==2
        && !adjusted.modulation_profiles[0].after_loop_start
        && !adjusted.modulation_profiles[1].after_loop_start
        && pitch[0].command_order<adjusted.modulation_profiles[0].command_order
        && adjusted.modulation_profiles[0].command_order<pitch[1].command_order
        && pitch[1].command_order<adjusted.modulation_profiles[1].command_order,
        "Old boundary mixed pitch/profile execution order is preserved across flattening");
    require(pitch[4].count==4 && pitch[4].after_loop_start
        && pitch[5].count==5 && pitch[5].after_loop_start
        && adjusted.timbre_automation.back().count==4
        && !adjusted.timbre_automation.back().after_loop_start,
        "Existing commands at new brackets enter body; new final @ stays before start");
    const auto formatted = formatMgsCompositeEnvelope(approved.timbre->layers[0],0);
    require(formatted.valid() && formatted.body.find("@14.[")!=std::string::npos,
        "Final @ at adjusted loop start remains before '[' and is not repeated");
    layer.envelope_timeline = {.length_counts=8,.loop_start_count=4,.loop_end_count=8};
    auto boundary = preparePatchSlideEnvelopeApply(source,0,item);
    require(boundary.timbre && !boundary.loop_conflict
        && boundary.timbre->layers[0].envelope_timeline == layer.envelope_timeline,
        "An @ before '[' at the same count is first-pass and needs no loop change");
    layer.envelope_timeline = {.length_counts=8,.loop_start_count=0,.loop_end_count=4};
    pending = preparePatchSlideEnvelopeApply(source,0,item);
    require(pending.loop_conflict && pending.loop_conflict->unreachable_counts==std::vector<std::uint32_t>{4},
        "An @ after ']' at the same count is unreachable");
    layer.envelope_timeline.loop_end_count.reset();
    require(!preparePatchSlideEnvelopeApply(source,0,item).error.empty(), "Incomplete loops are rejected, never repaired silently");
}

void testRejectMalformed() {
    const auto reject = [](PatchSlideCandidate item) {
        std::string error;
        require(!makePatchSlideTimbre(item, &error) && !error.empty(), "Malformed definitions cannot be approximated");
    };
    auto item = candidate(0, 1, 1);
    item.patch_a = -1; reject(item);
    item = candidate(0, 15, 1); reject(item);
    item = candidate(0, 1, 0); reject(item);
    item = candidate(0, 1, 1); item.change_count = 3; reject(item);
    item = candidate(0, 1, 1); item.patch_c = 2; reject(item);
    item = candidate(0, 1, 1, 2, 1); item.patch_c.reset(); reject(item);
    item = candidate(0, 1, 1, 2, 1); item.hold_b_steps = 3; reject(item);
    item = candidate(0, 1, 1); item.hold_pattern = "h2"; reject(item);
}
}  // namespace

int main() {
    try {
        if (const auto* trace_path = std::getenv("MGSTC_PATCHSLIDE_TRACE_OUTPUT")) {
            runtime_trace.open(trace_path);
            require(runtime_trace.is_open(), "Runtime trace output opens");
            runtime_trace << "candidate_id,count,sequence,address,value,reason\n";
        }
        testAllDefinitions();
        for (int ha = 1; ha <= 2; ++ha) {
            verifyCandidate(candidate(0, 14, ha));
            verifyCandidate(candidate(1, 1, ha));
            for (int hb = 1; hb <= 2; ++hb) {
                verifyCandidate(candidate(13, 1, ha, 7, hb));
                verifyCandidate(candidate(0, 0, ha, 0, hb));
            }
        }
        verifyCandidate(candidate(0, 1, 1));
        verifyCandidate(candidate(0, 1, 2));
        verifyCandidate(candidate(1, 5, 1));
        verifyCandidate(candidate(5, 8, 2, 1, 2));
        const auto sound = makePatchSlideTimbre(candidate(0, 1, 2, 14, 2));
        require(sound.has_value(), "Early key-off fixture converts");
        require(patchEvents(*sound, true) == std::vector<std::pair<Tick, int>>{{0, 0}, {0, 0}, {2, 1}, {4, 14}},
                "OPLL key-off preserves the existing sequence runtime semantics");
        testRejectMalformed();
        testEnvelopePartialReplacement();
        testEnvelopeHeldExtensionAndLimits();
        testEnvelopeLoopApproval();
        testEnvelopeInteriorRampPreservation();
        std::cout << "PatchSlide native conversion: 13950 definitions, six hold patterns, runtime, MGSC, persistence passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
