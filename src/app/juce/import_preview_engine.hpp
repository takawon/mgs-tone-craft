// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "composite_envelope_compile.hpp"
#include "juce_chip_tracks.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "shared_audio_host.hpp"

namespace mgstc::app {

namespace import_preview_detail {

[[nodiscard]] inline std::uint8_t trackForLayer(
    const mgstc::engine::CompositeLayer& layer) noexcept {
    switch (layer.source) {
    case mgstc::engine::TimbreSource::Psg:
        return layer.channel;
    case mgstc::engine::TimbreSource::Scc:
        return static_cast<std::uint8_t>(kSccTrack + layer.channel);
    case mgstc::engine::TimbreSource::Opll:
        return static_cast<std::uint8_t>(kOpllTrack + layer.channel);
    }
    return 0;
}

[[nodiscard]] inline std::array<std::uint8_t, 3> audibleLayerCounts(
    const mgstc::engine::CompositeTimbre& timbre) {
    std::array<std::uint8_t, 3> counts{};
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        if (!mgstc::engine::layerIsAudible(timbre, index)) {
            continue;
        }
        ++counts[static_cast<std::size_t>(timbre.layers[index].source)];
    }
    return counts;
}

[[nodiscard]] inline std::uint8_t trackForVoice(
    const mgstc::engine::CompositeTimbre& timbre,
    std::size_t layer_index,
    std::uint8_t voice,
    const std::array<std::uint8_t, 3>& counts) {
    const auto& layer = timbre.layers[layer_index];
    const auto source = static_cast<std::size_t>(layer.source);
    std::uint8_t ordinal = 0;
    for (std::size_t index = 0; index < layer_index; ++index) {
        if (mgstc::engine::layerIsAudible(timbre, index)
            && timbre.layers[index].source == layer.source) {
            ++ordinal;
        }
    }
    constexpr std::array<std::uint8_t, 3> bases{0, 3, 8};
    return static_cast<std::uint8_t>(
        bases[source] + voice * counts[source] + ordinal);
}

[[nodiscard]] inline std::uint8_t compositeVoiceCapacity(
    const std::array<std::uint8_t, 3>& counts) noexcept {
    constexpr std::array<std::uint8_t, 3> capacities{3, 5, 9};
    std::uint8_t result = 1;
    bool has_source = false;
    for (std::size_t source = 0; source < counts.size(); ++source) {
        if (counts[source] == 0) {
            continue;
        }
        has_source = true;
        result = std::min<std::uint8_t>(
            result,
            static_cast<std::uint8_t>(
                capacities[source] / counts[source]));
    }
    return has_source ? std::max<std::uint8_t>(result, 1) : 1;
}

inline void drainProgramEdits(
    mgstc::engine::RealtimeEngineHost& engine,
    SharedAudioHost& audio_service) {
    std::array<float, 256> drain{};
    engine.drainPendingCommands(
        drain,
        250,
        !audio_service.running());
}

}  // namespace import_preview_detail

// Build and activate a composite program for import-list preview without
// touching an editor's editing model.
[[nodiscard]] inline bool configureCompositeImportProgram(
    SharedAudioHost& audio_service,
    const mgstc::engine::CompositeTimbre& program_timbre_in,
    const mgstc::engine::TimbreLibrary* timbre_library) {
    auto& engine = audio_service.engine();
    using namespace mgstc::engine;
    import_preview_detail::drainProgramEdits(engine, audio_service);
    auto edit = engine.beginProgramEdit();
    if (!edit.valid()) {
        engine.discardStuckProgramEdits();
        edit = engine.beginProgramEdit();
    }
    if (!edit.valid()) {
        return false;
    }

    const auto sine = generateSccPreset(SccWavePreset::Sine, SccHarmonic::One);
    std::array<std::uint8_t, 32> raw_scc{};
    std::transform(
        sine.begin(),
        sine.end(),
        raw_scc.begin(),
        [](std::int8_t sample) {
            return static_cast<std::uint8_t>(sample);
        });
    auto opll = encodeOpllPatch(defaultOpllPatch());
    for (const auto& layer : program_timbre_in.layers) {
        if (!layer.base_timbre
            || layer.base_timbre->source != layer.source) {
            continue;
        }
        if (layer.source == TimbreSource::Scc) {
            raw_scc = layer.base_timbre->scc_waveform;
        } else if (layer.source == TimbreSource::Opll) {
            opll = layer.base_timbre->opll_registers;
        }
    }

    const auto numbers = resolveTimbreNumbers(program_timbre_in);
    bool configured =
        edit.engine->session().mapper().defineSccPatch(0, raw_scc)
            == MapError::None
        && edit.engine->session().mapper().defineOpllOriginalPatch(16, opll)
            == MapError::None;
    for (const auto& assignment : numbers.assignments) {
        const SavedTimbreReference* snap =
            findEmbeddedTimbreSnapshot(
                program_timbre_in, assignment.library_id);
        SavedTimbreReference live_ref;
        if (snap == nullptr && timbre_library != nullptr) {
            if (const auto* live =
                    timbre_library->find(assignment.library_id)) {
                live_ref = makeSavedTimbreReference(*live);
                snap = &live_ref;
            }
        }
        if (snap == nullptr) {
            configured = false;
            break;
        }
        if (snap->source == TimbreSource::Scc) {
            configured = configured
                && edit.engine->session().mapper().defineSccPatch(
                       assignment.number, snap->scc_waveform)
                    == MapError::None;
            if (snap->manual_number
                && *snap->manual_number != assignment.number) {
                configured = configured
                    && edit.engine->session().mapper().defineSccPatch(
                           *snap->manual_number, snap->scc_waveform)
                        == MapError::None;
            }
        } else if (snap->source == TimbreSource::Opll) {
            configured = configured
                && edit.engine->session().mapper()
                       .defineOpllOriginalPatch(
                           assignment.number, snap->opll_registers)
                    == MapError::None;
            if (snap->manual_number
                && *snap->manual_number != assignment.number
                && *snap->manual_number > 14) {
                configured = configured
                    && edit.engine->session().mapper()
                           .defineOpllOriginalPatch(
                               *snap->manual_number,
                               snap->opll_registers)
                        == MapError::None;
            }
        }
    }

    const auto counts =
        import_preview_detail::audibleLayerCounts(program_timbre_in);
    const auto voice_capacity =
        import_preview_detail::compositeVoiceCapacity(counts);
    auto spectrum_map = identitySpectrumChannelMap();
    for (std::uint8_t voice = 0; voice < voice_capacity; ++voice) {
        for (std::size_t index = 0;
             index < program_timbre_in.layers.size();
             ++index) {
            if (layerIsAudible(program_timbre_in, index)) {
                spectrum_map[import_preview_detail::trackForVoice(
                    program_timbre_in, index, voice, counts)] =
                    import_preview_detail::trackForLayer(
                        program_timbre_in.layers[index]);
            }
        }
    }
    edit.engine->setSpectrumChannelMap(spectrum_map);

    CompositeTimbre program_timbre = program_timbre_in;
    static_cast<void>(enforceOpllRegisterAutoExclusivity(program_timbre));
    for (std::uint8_t voice = 0; voice < voice_capacity; ++voice) {
        const bool include_original_tone_y = voice == 0;
        for (std::size_t index = 0; index < program_timbre.layers.size();
             ++index) {
            const auto& layer = program_timbre.layers[index];
            if (!layerIsAudible(program_timbre, index)) {
                continue;
            }
            const auto track = import_preview_detail::trackForVoice(
                program_timbre, index, voice, counts);
            const bool expand_tl = include_original_tone_y
                && opllRegisterAutoOwnedByLayer(
                       program_timbre,
                       index,
                       OpllRegisterAutoTarget::TotalLevel);
            const bool expand_fb = include_original_tone_y
                && opllRegisterAutoOwnedByLayer(
                       program_timbre,
                       index,
                       OpllRegisterAutoTarget::Feedback);
            const bool rate_kind =
                layer.volume_envelope.kind == EnvelopeKind::Rate;
            if (rate_kind) {
                const auto rate = clampRateEnvelope(
                    layer.volume_envelope.rate);
                configured = configured
                    && edit.engine->session().setRateEnvelope(
                        track,
                        rateDefinitionFrom(rate),
                        layer.volume);
            } else {
                auto envelopes = compileCompositeEnvelopes(
                    layer,
                    numbers,
                    timbre_library,
                    include_original_tone_y,
                    expand_tl,
                    expand_fb);
                configured = configured
                    && edit.engine->session().setCompositeSequenceEnvelopes(
                        track,
                        std::move(envelopes.volume),
                        std::move(envelopes.pitch),
                        std::move(envelopes.timbre));
            }
            configured = configured
                && edit.engine->session().setTrackVolume(
                    track, layer.volume)
                && edit.engine->session().setTrackDetune(
                    track, layer.detune, layer.micro_detune)
                && edit.engine->session().setTrackPatch(
                    track, layerBasePatchNumber(layer, &numbers))
                && edit.engine->session().setTrackSoftwareLfo(
                    track, layer.software_lfo)
                && edit.engine->session().setTrackPitchSweep(
                    track, layer.pitch_sweep)
                && edit.engine->session().setTrackKeyOffHang(
                    track, layer.key_off_hang)
                && edit.engine->session().setTrackOpllSustain(
                    track,
                    layer.source == TimbreSource::Opll
                        && layer.opll_sustain);
            if (layer.source == TimbreSource::Psg) {
                const auto rate = clampRateEnvelope(
                    layer.volume_envelope.rate);
                const auto mixer = rate_kind
                    ? rate
                    : sequenceEnvelopeMixer(layer.volume_envelope.rate);
                configured = configured
                    && edit.engine->session().setPsgToneNoise(
                        track, mixer.tone_mode, mixer.noise)
                    && edit.engine->session().setPsgFixedVolume(
                        track, layer.volume);
            }
        }
    }

    if (!configured) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return false;
    }
    if (!engine.submitProgram(edit)) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return false;
    }
    audio_service.invalidateSharedEditorProgram();
    if (audio_service.running()) {
        if (!engine.waitForPendingProgramActivation()) {
            return false;
        }
    } else {
        import_preview_detail::drainProgramEdits(engine, audio_service);
    }
    return true;
}

[[nodiscard]] inline bool startCompositeImportNotes(
    mgstc::engine::RealtimeEngineHost& engine,
    const mgstc::engine::CompositeTimbre& timbre,
    std::uint8_t midi_note,
    std::vector<std::uint8_t>& sounding_tracks) {
    using namespace mgstc::engine;
    sounding_tracks.clear();
    const auto counts = import_preview_detail::audibleLayerCounts(timbre);
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        if (!layerIsAudible(timbre, index)) {
            continue;
        }
        const auto& layer = timbre.layers[index];
        const auto note = layerMidiNote(layer, midi_note);
        if (!note) {
            continue;
        }
        const auto track = import_preview_detail::trackForVoice(
            timbre, index, 0, counts);
        if (engine.submit(EngineCommand::noteOn(track, *note))) {
            sounding_tracks.push_back(track);
        }
    }
    return !sounding_tracks.empty();
}

[[nodiscard]] inline void stopCompositeImportNotes(
    mgstc::engine::RealtimeEngineHost& engine,
    std::vector<std::uint8_t>& sounding_tracks) {
    for (const auto track : sounding_tracks) {
        static_cast<void>(
            engine.submit(mgstc::engine::EngineCommand::noteOff(track)));
    }
    sounding_tracks.clear();
}

}  // namespace mgstc::app
