// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/tone_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using mgstc::engine::compileCompositeEnvelopeLane;
using mgstc::engine::compileCompositeEnvelopes;
using mgstc::engine::CompositeEnvelopeLane;

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void requireNoEmptyLoop(const std::vector<std::uint8_t>& bytecode) {
    for (std::size_t i = 0; i + 1 < bytecode.size(); ++i) {
        require(
            !(bytecode[i] == 0x40 && bytecode[i + 1] == 0x60),
            "compiled a zero-wait [] loop");
    }
}

void testLoopEndPitchBeforeCloseBracket() {
    using namespace mgstc::engine;
    auto layer = defaultCompositeTimbre().layers.front();
    layer.volume = 15;
    layer.envelope_timeline = {
        .length_counts = 4,
        .loop_start_count = 2,
        .loop_end_count = 3,
    };
    layer.volume_envelope.events = {
        {EnvelopeEventKind::Volume, 15, 0, 0},
        {EnvelopeEventKind::Volume, 15, 0, 1},
        {EnvelopeEventKind::Volume, 15, 0, 2},
    };
    layer.pitch_envelope.events = {
        {
            .kind = EnvelopeEventKind::Pitch,
            .value = -1,
            .count = 2,
            .after_loop_start = true,
        },
        {
            .kind = EnvelopeEventKind::Pitch,
            .value = 1,
            .count = 3,
            .after_loop_start = true,
        },
    };
    const TimbreNumberResolution numbers{};
    const auto bytecode = compileCompositeEnvelopeLane(
        layer, CompositeEnvelopeLane::Volume, numbers);
    require(!bytecode.empty(), "empty looptest-style bytecode");
    bool saw_minus_one = false;
    bool saw_plus_one_before_loop_end = false;
    for (std::size_t i = 0; i + 1 < bytecode.size(); ++i) {
        if (bytecode[i] == 0x12 && bytecode[i + 1] == 0xFF) {
            saw_minus_one = true;
        }
        if (bytecode[i] == 0x12
            && bytecode[i + 1] == 0x01
            && i + 2 < bytecode.size()
            && bytecode[i + 2] == 0x60) {
            saw_plus_one_before_loop_end = true;
        }
    }
    require(saw_minus_one, "missing loop pitch -1");
    require(
        saw_plus_one_before_loop_end,
        "missing loop_end pitch +1 before 0x60");

    SequenceEnvelopeRuntime runtime(bytecode);
    runtime.resetForKeyOn(15);
    EventBuffer buffer(16);
    std::int32_t pitch_sum = 0;
    std::optional<std::int32_t> pitch_after_warmup;
    constexpr int warmup_ticks = 12;
    constexpr int total_ticks = 40;
    for (int tick = 0; tick < total_ticks; ++tick) {
        buffer.clear();
        require(
            runtime.processTick(buffer) == SequenceError::None,
            "looptest loop pitch budget fault");
        for (std::size_t index = 0; index < buffer.size(); ++index) {
            if (buffer.events()[index].kind
                == MeaningEventKind::FrequencyDelta) {
                pitch_sum += buffer.events()[index].arg0;
            }
        }
        if (tick + 1 == warmup_ticks) {
            pitch_after_warmup = pitch_sum;
        }
    }
    require(pitch_after_warmup.has_value(), "warmup pitch sample missing");
    require(
        pitch_sum == *pitch_after_warmup,
        "looptest loop pitch should not drift after warmup");
}

void testOneStepVolumeLoopCompilesWithWait() {
    using namespace mgstc::engine;
    auto layer = defaultCompositeTimbre().layers.front();
    layer.volume = 15;
    layer.envelope_timeline = {
        .length_counts = 1,
        .loop_start_count = 0,
        .loop_end_count = 1,
    };
    layer.volume_envelope.events = {
        {EnvelopeEventKind::Volume, 15, 0, 0},
        {EnvelopeEventKind::Volume, 15, 0, 1},
    };
    const TimbreNumberResolution numbers{};
    const auto bytecode = compileCompositeEnvelopeLane(
        layer, CompositeEnvelopeLane::Volume, numbers);
    require(!bytecode.empty(), "empty bytecode");
    requireNoEmptyLoop(bytecode);
    SequenceEnvelopeRuntime runtime(bytecode);
    runtime.resetForKeyOn();
    EventBuffer buffer(16);
    for (int tick = 0; tick < 16; ++tick) {
        buffer.clear();
        require(
            runtime.processTick(buffer) == SequenceError::None,
            "1-step [f] loop hit the instruction budget");
        require(runtime.volume() == 15, "1-step [f] lost volume 15");
    }
}

}  // namespace

void testImportedSccBaseBelowFifteenAssignsAndRenders() {
    using namespace mgstc::engine;
    CompositeTimbre timbre;
    timbre.format_version = CompositeTimbre::kFormatVersion;
    CompositeLayer layer;
    layer.source = TimbreSource::Scc;
    layer.enabled = true;
    layer.volume = 15;
    SavedTimbreReference base;
    base.library_id = 0x4D47535400020001ULL;
    base.source = TimbreSource::Scc;
    base.number_mode = TimbreNumberMode::Manual;
    base.manual_number = static_cast<std::uint8_t>(5);
    for (auto& sample : base.scc_waveform) {
        sample = 0x40;
    }
    layer.base_timbre = base;
    EnvelopeEvent event{};
    event.kind = EnvelopeEventKind::Timbre;
    event.value = 16;
    event.timbre_pick = TimbrePick::Library;
    event.target_library_id = 0x4D47535400020002ULL;
    layer.timbre_automation.push_back(event);
    SavedTimbreReference wave16 = base;
    wave16.library_id = 0x4D47535400020002ULL;
    wave16.manual_number = 16;
    timbre.layers.push_back(layer);
    timbre.embedded_timbres = {*layer.base_timbre, wave16};

    const auto numbers = resolveTimbreNumbers(timbre);
    const auto assigned_base =
        assignedNumberForLibraryId(numbers, base.library_id);
    require(
        assigned_base.has_value(),
        "imported SCC @s5 base was not assigned a mapper slot");
    require(
        *assigned_base >= 15,
        "imported SCC @s5 base should remap into 15-31");
    require(
        layerBasePatchNumber(layer, &numbers) == assigned_base,
        "key-on patch still used the raw @s5 number");

    const auto envelopes = compileCompositeEnvelopes(layer, numbers);
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "beginProgramEdit failed for @s5 base");
    const bool configured =
        edit.engine->session().mapper().defineSccPatch(
            *assigned_base, base.scc_waveform)
            == MapError::None
        && edit.engine->session().setCompositeSequenceEnvelopes(
            3,
            envelopes.volume,
            envelopes.pitch,
            envelopes.timbre)
        && edit.engine->session().setTrackPatch(
            3, layerBasePatchNumber(layer, &numbers))
        && edit.engine->session().setTrackVolume(3, 15);
    require(configured, "configure failed for @s5 base");
    require(host.submitProgram(edit), "submitProgram failed for @s5 base");
    std::array<float, 256> drain{};
    host.drainPendingCommands(drain);
    require(
        host.submit(EngineCommand::noteOn(3, 60)),
        "noteOn failed for @s5 base");
    host.drainPendingCommands(drain);
    std::array<float, 800> pcm{};
    for (int frame = 0; frame < 60; ++frame) {
        const auto render_result = host.render(pcm);
        require(
            render_result.ok(),
            "render PatchNotFound after imported @s5 base");
    }
}

void testEayzs004Envelope26Renders() {
    using namespace mgstc::engine;
    const char* path =
        "C:\\Users\\takawo\\Desktop\\新しいフォルダー (2)\\EAYZS004.mgs";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const auto result = importTones(bytes, "mgs");
    require(result.valid(), "EAYZS004 import failed");
    const CompositeTimbre* timbre = nullptr;
    for (const auto& candidate : result.candidates) {
        if (candidate.type != ImportedToneType::Composite
            || candidate.name.find(" - 26") == std::string::npos) {
            continue;
        }
        timbre = std::get_if<CompositeTimbre>(&candidate.data);
        if (timbre != nullptr) {
            break;
        }
    }
    require(timbre != nullptr, "EAYZS004 @e26 composite missing");
    require(!timbre->layers.empty(), "EAYZS004 @e26 has no layers");

    const auto numbers = resolveTimbreNumbers(*timbre);
    const auto& layer = timbre->layers.front();
    const auto envelopes = compileCompositeEnvelopes(layer, numbers);
    require(!envelopes.volume.empty(), "empty volume bytecode");
    require(!envelopes.timbre.empty(), "empty timbre bytecode");

    SequenceEnvelopeRuntime volume_runtime(envelopes.volume);
    volume_runtime.resetForKeyOn(15);
    EventBuffer buffer(64);
    require(
        volume_runtime.processTick(buffer)
            == SequenceError::None,
        "EAYZS004 @e26 volume lane budget fault on tick 0");

    EngineCore engine;
    auto& session = engine.session();
    auto& mapper = session.mapper();
    std::array<std::uint8_t, 32> raw_scc{};
    const auto sine = generateSccPreset(SccWavePreset::Sine, SccHarmonic::One);
    for (std::size_t i = 0; i < raw_scc.size(); ++i) {
        raw_scc[i] = static_cast<std::uint8_t>(sine[i]);
    }
    require(
        mapper.defineSccPatch(0, raw_scc) == MapError::None,
        "defineSccPatch(0) failed");
    for (const auto& assignment : numbers.assignments) {
        const auto* snap =
            findEmbeddedTimbreSnapshot(*timbre, assignment.library_id);
        require(snap != nullptr, "missing embedded timbre snapshot");
        if (snap->source == TimbreSource::Scc) {
            require(
                mapper.defineSccPatch(assignment.number, snap->scc_waveform)
                    == MapError::None,
                "defineSccPatch failed");
        }
    }
    const std::uint8_t track = 3;
    require(
        session.setCompositeSequenceEnvelopes(
            track,
            envelopes.volume,
            envelopes.pitch,
            envelopes.timbre),
        "setCompositeSequenceEnvelopes failed");
    require(
        session.setTrackPatch(track, layerBasePatchNumber(layer, &numbers)),
        "setTrackPatch failed");
    require(session.setTrackVolume(track, layer.volume), "setTrackVolume failed");
    require(session.queueNoteOn(track, 60), "queueNoteOn failed");

    double peak = 0.0;
    std::array<float, 800> pcm{};
    for (int frame = 0; frame < 60 * 50; ++frame) {
        const auto render_result = engine.render(pcm);
        require(render_result.ok(), "EAYZS004 @e26 render fault");
        for (const auto sample : pcm) {
            peak = std::max(peak, static_cast<double>(std::fabs(sample)));
        }
    }
    require(peak > 0.001, "EAYZS004 @e26 rendered silence");
}

void testEayzs004AllCompositeImportsConfigure() {
    using namespace mgstc::engine;
    const char* path =
        "C:\\Users\\takawo\\Desktop\\新しいフォルダー (2)\\EAYZS004.mgs";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const auto result = importTones(bytes, "mgs");
    require(result.valid(), "EAYZS004 import failed");
    for (const auto& candidate : result.candidates) {
        if (candidate.type != ImportedToneType::Composite
            && candidate.default_register_as != ImportRegisterAs::Composite) {
            continue;
        }
        const auto timbre = makeImportedComposite(
            candidate, ImportRegisterAs::Composite);
        require(timbre.has_value(), "makeImportedComposite failed");
        const auto numbers = resolveTimbreNumbers(*timbre);
        for (const auto& assignment : numbers.assignments) {
            require(
                findEmbeddedTimbreSnapshot(*timbre, assignment.library_id)
                    != nullptr,
                "missing embedded snapshot after finalize");
        }
        for (std::size_t index = 0; index < timbre->layers.size(); ++index) {
            if (!layerIsAudible(*timbre, index)) {
                continue;
            }
            const auto envelopes = compileCompositeEnvelopes(
                timbre->layers[index], numbers);
            require(
                !envelopes.volume.empty(),
                "empty import preview volume bytecode");
        }
    }
}

void testEayzs004Envelope26RealtimeHostPreview() {
    using namespace mgstc::engine;
    const char* path =
        "C:\\Users\\takawo\\Desktop\\新しいフォルダー (2)\\EAYZS004.mgs";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const auto result = importTones(bytes, "mgs");
    require(result.valid(), "EAYZS004 import failed");
    const CompositeTimbre* timbre = nullptr;
    for (const auto& candidate : result.candidates) {
        if (candidate.type != ImportedToneType::Composite
            || candidate.name.find(" - 26") == std::string::npos) {
            continue;
        }
        timbre = std::get_if<CompositeTimbre>(&candidate.data);
        if (timbre != nullptr) {
            break;
        }
    }
    require(timbre != nullptr, "EAYZS004 @e26 composite missing");
    require(!timbre->layers.empty(), "EAYZS004 @e26 has no layers");

    const auto numbers = resolveTimbreNumbers(*timbre);
    const auto& layer = timbre->layers.front();
    const auto envelopes = compileCompositeEnvelopes(layer, numbers);

    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "beginProgramEdit failed for @e26 preview");

    std::array<std::uint8_t, 32> raw_scc{};
    const auto sine = generateSccPreset(SccWavePreset::Sine, SccHarmonic::One);
    for (std::size_t i = 0; i < raw_scc.size(); ++i) {
        raw_scc[i] = static_cast<std::uint8_t>(sine[i]);
    }
    auto opll = encodeOpllPatch(defaultOpllPatch());
    bool configured =
        edit.engine->session().mapper().defineSccPatch(0, raw_scc)
            == MapError::None
        && edit.engine->session().mapper().defineOpllOriginalPatch(16, opll)
            == MapError::None;
    for (const auto& assignment : numbers.assignments) {
        const auto* snap =
            findEmbeddedTimbreSnapshot(*timbre, assignment.library_id);
        require(snap != nullptr, "missing embedded timbre snapshot");
        if (snap->source == TimbreSource::Scc) {
            configured = configured
                && edit.engine->session().mapper().defineSccPatch(
                       assignment.number, snap->scc_waveform)
                    == MapError::None;
        }
    }
    const std::uint8_t track = 3;
    configured = configured
        && edit.engine->session().setCompositeSequenceEnvelopes(
            track,
            envelopes.volume,
            envelopes.pitch,
            envelopes.timbre)
        && edit.engine->session().setTrackPatch(
            track, layerBasePatchNumber(layer, &numbers))
        && edit.engine->session().setTrackVolume(track, layer.volume);
    require(configured, "configure @e26 preview program failed");
    require(host.submitProgram(edit), "submitProgram failed for @e26 preview");

    std::array<float, 256> drain{};
    host.drainPendingCommands(drain);
    require(
        host.submit(EngineCommand::noteOn(track, 60)),
        "noteOn failed for @e26 preview");
    host.drainPendingCommands(drain);

    double peak = 0.0;
    std::array<float, 800> pcm{};
    for (int frame = 0; frame < 60 * 50; ++frame) {
        const auto render_result = host.render(pcm);
        require(render_result.ok(), "EAYZS004 @e26 host render fault");
        for (const auto sample : pcm) {
            peak = std::max(peak, static_cast<double>(std::fabs(sample)));
        }
    }
    require(peak > 0.001, "EAYZS004 @e26 host preview rendered silence");
}

void testTrackMappingMatchesLegacyLayout() {
    using namespace mgstc::engine;
    auto timbre = defaultCompositeTimbre();
    const auto plan = buildCompositePlaybackPlan(timbre);
    require(plan.voice_capacity == 3, "PSG+SCC+OPLL capacity should be 3");
    require(plan.audible_counts[0] == 1, "one audible PSG layer");
    require(plan.audible_counts[1] == 1, "one audible SCC layer");
    require(plan.audible_counts[2] == 1, "one audible OPLL layer");
    require(plan.audible_layers.size() == 3, "three audible layers");
    require(
        plan.physicalTrack(plan.audible_layers[0], 0) == 0,
        "PSG voice0 -> track 0");
    require(
        plan.physicalTrack(plan.audible_layers[1], 0) == 3,
        "SCC voice0 -> track 3");
    require(
        plan.physicalTrack(plan.audible_layers[2], 0) == 8,
        "OPLL voice0 -> track 8");
    require(
        plan.physicalTrack(plan.audible_layers[0], 1) == 1,
        "PSG voice1 -> track 1");
    require(
        plan.physicalTrack(plan.audible_layers[1], 2) == 5,
        "SCC voice2 -> track 5");
    require(
        plan.physicalTrack(plan.audible_layers[2], 2) == 10,
        "OPLL voice2 -> track 10");
    require(
        authoringTrackForLayer(timbre.layers[0]) == 0,
        "PSG authoring track");
    require(
        authoringTrackForLayer(timbre.layers[1]) == 3,
        "SCC authoring track");
    require(
        authoringTrackForLayer(timbre.layers[2]) == 8,
        "OPLL authoring track");

    CompositeTimbre two_scc;
    two_scc.layers = {
        defaultCompositeTimbre().layers[1],
        defaultCompositeTimbre().layers[1],
    };
    two_scc.layers[0].channel = 0;
    two_scc.layers[1].channel = 1;
    const auto scc_plan = buildCompositePlaybackPlan(two_scc);
    require(scc_plan.voice_capacity == 2, "two SCC layers -> capacity 2");
    require(
        scc_plan.physicalTrack(scc_plan.audible_layers[0], 0) == 3,
        "SCC layer0 voice0 -> 3");
    require(
        scc_plan.physicalTrack(scc_plan.audible_layers[1], 0) == 4,
        "SCC layer1 voice0 -> 4");
    require(
        scc_plan.physicalTrack(scc_plan.audible_layers[0], 1) == 5,
        "SCC layer0 voice1 -> 5");
    require(
        scc_plan.physicalTrack(scc_plan.audible_layers[1], 1) == 6,
        "SCC layer1 voice1 -> 6");
    require(
        physicalTrackForVoice(two_scc, 1, 1, scc_plan.audible_counts) == 6,
        "legacy physicalTrackForVoice mismatch");
}

void testVoiceCapacityMatchesLegacyFormula() {
    using namespace mgstc::engine;
    require(compositeVoiceCapacity({0, 0, 0}) == 1, "empty capacity");
    require(compositeVoiceCapacity({1, 0, 0}) == 3, "one PSG -> 3");
    require(compositeVoiceCapacity({0, 1, 0}) == 5, "one SCC -> 5");
    require(compositeVoiceCapacity({0, 0, 1}) == 9, "one OPLL -> 9");
    require(compositeVoiceCapacity({1, 1, 1}) == 3, "one of each -> 3");
    require(compositeVoiceCapacity({2, 0, 0}) == 1, "two PSG -> 1");
    require(compositeVoiceCapacity({3, 0, 0}) == 1, "three PSG -> 1");
    require(compositeVoiceCapacity({0, 2, 0}) == 2, "two SCC -> 2");
    require(compositeVoiceCapacity({0, 3, 0}) == 1, "three SCC -> 1");
    require(compositeVoiceCapacity({1, 0, 5}) == 1, "PSG+5 OPLL -> 1");

    auto timbre = defaultCompositeTimbre();
    timbre.layers[0].muted = true;
    const auto counts = audibleLayerCounts(timbre);
    require(counts[0] == 0, "muted PSG is not audible");
    require(compositeVoiceCapacity(counts) == 5, "muted PSG leaves SCC cap 5");
}

void testRateEnvelopeDefinitionFromCopiesRuntimeFields() {
    using namespace mgstc::engine;
    RateEnvelope rate{};
    rate.tone_mode = 2;
    rate.noise = 7;
    rate.attack_level = 10;
    rate.attack_rate = 20;
    rate.decay_rate = 30;
    rate.sustain_level = 40;
    rate.sustain_rate = 50;
    rate.release_rate = 60;
    const auto definition = rateEnvelopeDefinitionFrom(rate);
    require(definition.attack_level == 10, "AL");
    require(definition.attack_rate == 20, "AR");
    require(definition.decay_rate == 30, "DR");
    require(definition.sustain_level == 40, "SL");
    require(definition.sustain_rate == 50, "SR");
    require(definition.release_rate == 60, "RR");
}

void testCompileCompositeProgramConfiguresMappedTracks() {
    using namespace mgstc::engine;
    auto timbre = defaultCompositeTimbre();
    timbre.layers[0].volume = 12;
    timbre.layers[0].detune = 3;
    timbre.layers[1].volume = 11;
    timbre.layers[2].volume = 10;
    timbre.layers[2].relative_semitones = 12;
    timbre.layers[2].start_delay_form = StartDelayForm::AbsoluteTicks;
    timbre.layers[2].start_delay_value = 6;

    EngineCore engine;
    CompositePlaybackPlan plan;
    require(
        compileCompositeProgram(
            engine,
            timbre,
            {.polyphonic = false},
            &plan),
        "compileCompositeProgram failed");
    require(plan.voice_capacity == 3, "default capacity");
    require(
        plan.audible_layers[2].start_delay_ms == 100.0,
        "r%6 should be 100ms");
    require(
        plan.audible_layers[2].relative_semitones == 12,
        "relative semitone dropped");

    require(engine.session().queueNoteOn(0, 60), "PSG noteOn");
    require(engine.session().queueNoteOn(3, 60), "SCC noteOn");
    require(engine.session().queueNoteOn(8, 72), "OPLL noteOn");
    const auto tick = engine.session().processTick();
    require(tick.ok(), "first tick after compile");
    require(!engine.session().writes().empty(), "compiled program wrote nothing");

    std::array<float, 800> pcm{};
    double peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto render_result = engine.render(pcm);
        require(render_result.ok(), "render after compileCompositeProgram");
        for (const auto sample : pcm) {
            peak = std::max(peak, static_cast<double>(std::fabs(sample)));
        }
    }
    require(peak > 0.001, "compiled default composite rendered silence");
}

void testCompileCompositeProgramRateEnvelopeRenders() {
    using namespace mgstc::engine;
    CompositeTimbre timbre;
    CompositeLayer layer;
    layer.source = TimbreSource::Psg;
    layer.enabled = true;
    layer.volume = 15;
    layer.volume_envelope.kind = EnvelopeKind::Rate;
    seedDefaultRateEnvelope(layer.volume_envelope.rate, TimbreSource::Psg);
    timbre.layers.push_back(layer);

    EngineCore engine;
    CompositePlaybackPlan plan;
    require(
        compileCompositeProgram(engine, timbre, {}, &plan),
        "rate program compile failed");
    require(plan.voice_capacity == 3, "single PSG rate capacity");
    require(plan.physicalTrack(0, 0) == 0, "rate PSG track 0");
    require(engine.session().queueNoteOn(0, 60), "rate noteOn");
    require(engine.session().processTick().ok(), "rate first tick");

    std::array<float, 800> pcm{};
    double peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto render_result = engine.render(pcm);
        require(render_result.ok(), "rate render fault");
        for (const auto sample : pcm) {
            peak = std::max(peak, static_cast<double>(std::fabs(sample)));
        }
    }
    require(peak > 0.001, "compiled rate envelope rendered silence");
}

int main() {
    try {
        testLoopEndPitchBeforeCloseBracket();
        testOneStepVolumeLoopCompilesWithWait();
        testImportedSccBaseBelowFifteenAssignsAndRenders();
        testEayzs004Envelope26Renders();
        testEayzs004AllCompositeImportsConfigure();
        testEayzs004Envelope26RealtimeHostPreview();
        testTrackMappingMatchesLegacyLayout();
        testVoiceCapacityMatchesLegacyFormula();
        testRateEnvelopeDefinitionFromCopiesRuntimeFields();
        testCompileCompositeProgramConfiguresMappedTracks();
        testCompileCompositeProgramRateEnvelopeRenders();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
