// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_timbre.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
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
        std::cout << "PatchSlide native conversion: 13950 definitions, six hold patterns, runtime, MGSC, persistence passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
