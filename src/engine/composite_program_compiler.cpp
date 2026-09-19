// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_program_compiler.hpp"

#include <algorithm>
#include <utility>

#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/spectrum_capture.hpp"

namespace mgstc::engine {

std::uint8_t CompositePlaybackPlan::physicalTrack(
    const CompositePlaybackLayer& layer,
    std::uint8_t voice) const noexcept {
    const auto source = static_cast<std::size_t>(layer.source);
    return static_cast<std::uint8_t>(
        kCompositeSourceTrackBases[source]
        + voice * audible_counts[source]
        + layer.source_ordinal);
}

std::uint8_t CompositePlaybackPlan::physicalTrack(
    std::size_t layer_index,
    std::uint8_t voice) const noexcept {
    for (const auto& layer : audible_layers) {
        if (layer.layer_index == layer_index) {
            return physicalTrack(layer, voice);
        }
    }
    return 0;
}

std::uint8_t CompositePlaybackPlan::authoringTrack(
    const CompositePlaybackLayer& layer) const noexcept {
    const auto source = static_cast<std::size_t>(layer.source);
    return static_cast<std::uint8_t>(
        kCompositeSourceTrackBases[source] + layer.authoring_channel);
}

std::array<std::uint8_t, 3> audibleLayerCounts(
    const CompositeTimbre& timbre) noexcept {
    std::array<std::uint8_t, 3> counts{};
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        if (!layerIsAudible(timbre, index)) {
            continue;
        }
        ++counts[static_cast<std::size_t>(timbre.layers[index].source)];
    }
    return counts;
}

std::uint8_t compositeVoiceCapacity(
    const std::array<std::uint8_t, 3>& counts) noexcept {
    std::uint8_t result = kCompositeSourceChannelCounts.back();
    bool has_source = false;
    for (std::size_t source = 0; source < counts.size(); ++source) {
        if (counts[source] == 0) {
            continue;
        }
        has_source = true;
        result = std::min<std::uint8_t>(
            result,
            static_cast<std::uint8_t>(
                kCompositeSourceChannelCounts[source] / counts[source]));
    }
    return has_source ? std::max<std::uint8_t>(result, 1) : 1;
}

std::uint8_t physicalTrackForVoice(
    const CompositeTimbre& timbre,
    std::size_t layer_index,
    std::uint8_t voice,
    const std::array<std::uint8_t, 3>& counts) noexcept {
    const auto& layer = timbre.layers[layer_index];
    const auto source = static_cast<std::size_t>(layer.source);
    std::uint8_t ordinal = 0;
    for (std::size_t index = 0; index < layer_index; ++index) {
        if (layerIsAudible(timbre, index)
            && timbre.layers[index].source == layer.source) {
            ++ordinal;
        }
    }
    return static_cast<std::uint8_t>(
        kCompositeSourceTrackBases[source]
        + voice * counts[source]
        + ordinal);
}

std::uint8_t authoringTrackForLayer(const CompositeLayer& layer) noexcept {
    const auto source = static_cast<std::size_t>(layer.source);
    return static_cast<std::uint8_t>(
        kCompositeSourceTrackBases[source] + layer.channel);
}

CompositePlaybackPlan buildCompositePlaybackPlan(
    const CompositeTimbre& timbre) {
    CompositePlaybackPlan plan;
    plan.audible_counts = audibleLayerCounts(timbre);
    plan.voice_capacity = compositeVoiceCapacity(plan.audible_counts);
    std::array<std::uint8_t, 3> ordinals{};
    plan.audible_layers.reserve(timbre.layers.size());
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        if (!layerIsAudible(timbre, index)) {
            continue;
        }
        const auto& layer = timbre.layers[index];
        const auto source = static_cast<std::size_t>(layer.source);
        plan.audible_layers.push_back({
            .layer_index = index,
            .source = layer.source,
            .source_ordinal = ordinals[source],
            .authoring_channel = layer.channel,
            .relative_semitones = layer.relative_semitones,
            .start_delay_ms = startDelayMilliseconds(
                layer.start_delay_form,
                layer.start_delay_value,
                timbre.playback_tempo),
        });
        ++ordinals[source];
    }
    return plan;
}

RateEnvelopeDefinition rateEnvelopeDefinitionFrom(
    const RateEnvelope& rate) noexcept {
    RateEnvelopeDefinition definition;
    definition.attack_level = rate.attack_level;
    definition.attack_rate = rate.attack_rate;
    definition.decay_rate = rate.decay_rate;
    definition.sustain_level = rate.sustain_level;
    definition.sustain_rate = rate.sustain_rate;
    definition.release_rate = rate.release_rate;
    return definition;
}

bool compileCompositeProgram(
    EngineCore& engine,
    const CompositeTimbre& timbre,
    const CompositeProgramCompileOptions& options,
    CompositePlaybackPlan* plan_out) {
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
    for (const auto& layer : timbre.layers) {
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

    const auto numbers = resolveTimbreNumbers(timbre);
    bool configured =
        engine.session().mapper().defineSccPatch(0, raw_scc) == MapError::None
        && engine.session().mapper().defineOpllOriginalPatch(16, opll)
            == MapError::None;
    for (const auto& assignment : numbers.assignments) {
        const SavedTimbreReference* snap =
            findEmbeddedTimbreSnapshot(timbre, assignment.library_id);
        SavedTimbreReference live_ref;
        if (snap == nullptr && options.library != nullptr) {
            if (const auto* live = options.library->find(assignment.library_id)) {
                live_ref = makeSavedTimbreReference(*live);
                snap = &live_ref;
            }
        }
        if (snap == nullptr) {
            if (plan_out != nullptr) {
                *plan_out = {};
            }
            return false;
        }
        if (snap->source == TimbreSource::Scc) {
            configured = configured
                && engine.session().mapper().defineSccPatch(
                       assignment.number, snap->scc_waveform)
                    == MapError::None;
            if (snap->manual_number
                && *snap->manual_number != assignment.number) {
                configured = configured
                    && engine.session().mapper().defineSccPatch(
                           *snap->manual_number, snap->scc_waveform)
                        == MapError::None;
            }
        } else if (snap->source == TimbreSource::Opll) {
            configured = configured
                && engine.session().mapper().defineOpllOriginalPatch(
                       assignment.number, snap->opll_registers)
                    == MapError::None;
            if (snap->manual_number
                && *snap->manual_number != assignment.number
                && *snap->manual_number > 14) {
                configured = configured
                    && engine.session().mapper().defineOpllOriginalPatch(
                           *snap->manual_number, snap->opll_registers)
                        == MapError::None;
            }
        }
    }

    const auto plan = buildCompositePlaybackPlan(timbre);
    auto spectrum_map = identitySpectrumChannelMap();
    for (std::uint8_t voice = 0; voice < plan.voice_capacity; ++voice) {
        for (const auto& binding : plan.audible_layers) {
            spectrum_map[plan.physicalTrack(binding, voice)] =
                plan.authoringTrack(binding);
        }
    }
    engine.setSpectrumChannelMap(spectrum_map);

    for (std::uint8_t voice = 0; voice < plan.voice_capacity; ++voice) {
        const bool include_original_tone_y =
            !options.polyphonic || voice == 0;
        for (const auto& binding : plan.audible_layers) {
            const auto& layer = timbre.layers[binding.layer_index];
            const auto track = plan.physicalTrack(binding, voice);
            const bool expand_tl = include_original_tone_y
                && opllRegisterAutoOwnedByLayer(
                       timbre,
                       binding.layer_index,
                       OpllRegisterAutoTarget::TotalLevel);
            const bool expand_fb = include_original_tone_y
                && opllRegisterAutoOwnedByLayer(
                       timbre,
                       binding.layer_index,
                       OpllRegisterAutoTarget::Feedback);
            const bool rate_kind =
                layer.volume_envelope.kind == EnvelopeKind::Rate;
            if (rate_kind) {
                const auto rate = clampRateEnvelope(layer.volume_envelope.rate);
                configured = configured
                    && engine.session().setRateEnvelope(
                        track,
                        rateEnvelopeDefinitionFrom(rate),
                        layer.volume);
            } else {
                auto envelopes = compileCompositeEnvelopes(
                    layer,
                    numbers,
                    options.library,
                    include_original_tone_y,
                    expand_tl,
                    expand_fb);
                configured = configured
                    && engine.session().setCompositeSequenceEnvelopes(
                        track,
                        std::move(envelopes.volume),
                        std::move(envelopes.pitch),
                        std::move(envelopes.timbre));
            }
            configured = configured
                && engine.session().setTrackVolume(track, layer.volume)
                && engine.session().setTrackDetune(
                    track, layer.detune, layer.micro_detune)
                && engine.session().setTrackPatch(
                    track, layerBasePatchNumber(layer, &numbers))
                && engine.session().setTrackSoftwareLfo(
                    track, layer.software_lfo)
                && engine.session().setTrackPitchSweep(
                    track, layer.pitch_sweep)
                && engine.session().setTrackKeyOffHang(
                    track, layer.key_off_hang)
                && engine.session().setTrackOpllSustain(
                    track,
                    layer.source == TimbreSource::Opll && layer.opll_sustain);
            if (layer.source == TimbreSource::Psg) {
                const auto rate = clampRateEnvelope(layer.volume_envelope.rate);
                const auto mixer = rate_kind
                    ? rate
                    : sequenceEnvelopeMixer(layer.volume_envelope.rate);
                configured = configured
                    && engine.session().setPsgToneNoise(
                        track, mixer.tone_mode, mixer.noise)
                    && engine.session().setPsgFixedVolume(track, layer.volume);
            }
        }
    }

    if (plan_out != nullptr) {
        *plan_out = configured ? plan : CompositePlaybackPlan{};
    }
    return configured;
}

}  // namespace mgstc::engine
