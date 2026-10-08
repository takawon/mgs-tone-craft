// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_conversion.hpp"
#include "mgstc/engine/composite_wav_renderer.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/note_pitch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mgstc::engine;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void printConversionWarnings(const char* label, const CompositeWavConversionResult& result,
    std::size_t maximumWarningCharacters = 240) {
    constexpr std::size_t maximumWarnings = 8;
    std::printf("%s warnings=%zu\n", label, result.warnings.size());
    const auto count = std::min(maximumWarnings, result.warnings.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& warning = result.warnings[i];
        std::printf("%s warning[%zu]: %.*s\n", label, i,
            static_cast<int>(std::min(maximumWarningCharacters, warning.size())), warning.c_str());
    }
    if (result.warnings.size() > count)
        std::printf("%s warnings omitted=%zu\n", label, result.warnings.size() - count);
}
std::shared_ptr<const SourceAnalysis> harmonicFixture(
    std::size_t frames, const std::function<std::array<double, 8>(double)>& partials,
    double amplitude = 0.22) {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48'000;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    pcm->mono_samples.reserve(frames);
    pcm->interleaved_samples.reserve(frames);
    constexpr double f0 = 261.625565;
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 48'000.0;
        const double level = amplitude * std::min(1.0, t / 0.025);
        const auto harmonics = partials(t);
        double value{};
        for (std::size_t h = 1; h < harmonics.size(); ++h)
            value += harmonics[h] * std::sin(2 * std::numbers::pi * f0 * h * t);
        const auto sample = static_cast<float>(level * value);
        pcm->mono_samples.push_back(sample);
        pcm->interleaved_samples.push_back(sample);
    }
    SourceAnalysisOptions options;
    options.reference_pitch_hz = f0;
    const auto analyzed = analyzeCompositeWaveSource(pcm, {0, frames}, options);
    require(analyzed.completion == SourceAnalysisCompletion::Completed,
        "synthetic acceptance source analyzes successfully");
    return analyzed.analysis;
}

std::shared_ptr<const SourceAnalysis> analyzeRendered(
    const CompositeTimbre& timbre, std::size_t frames, std::size_t keyoff) {
    CompositeWavRenderOptions rendering;
    rendering.midi_note = 60;
    rendering.key_off_frame = keyoff;
    rendering.frame_count = frames;
    const auto rendered = renderCompositeWav(timbre, rendering);
    require(rendered.ok() && rendered.mono_pcm.size() == frames,
        "known SCC source renders through the real engine");
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48'000;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    pcm->mono_samples = rendered.mono_pcm;
    pcm->interleaved_samples = rendered.mono_pcm;
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    const auto analyzed = analyzeCompositeWaveSource(pcm, {0, frames}, options);
    require(analyzed.completion == SourceAnalysisCompletion::Completed,
        "engine-rendered acceptance source analyzes with the manual C4 guide");
    return analyzed.analysis;
}

CompositeWavConversionResult convert(std::shared_ptr<const SourceAnalysis> analysis,
    CompositeWavConfiguration configuration, std::size_t keyoff, std::size_t bank = 8,
    CompositeWavLoopMode loop = CompositeWavLoopMode::Automatic,
    CompositeWavStrategy strategy = CompositeWavStrategy::Independent,
    std::size_t evaluationBudget = 48,
    CompositeWavSccWaveMethod waveMethod = CompositeWavSccWaveMethod::Reconstructed) {
    CompositeWavConversionOptions options;
    options.configuration = configuration;
    options.strategy = strategy;
    options.max_scc_waveforms = bank;
    options.max_evaluations = evaluationBudget;
    options.stagnation_limit = 0;
    options.key_off_position = keyoff;
    options.loop_mode = loop;
    options.scc_wave_method = waveMethod;
    auto result = convertCompositeWave(std::move(analysis), options);
    require(result.completion == CompositeWavConversionCompletion::Completed,
        "acceptance conversion completes with a valid candidate");
    require(result.composite_tone.has_value(), "conversion returns an editable composite");
    return result;
}

const SavedTimbreReference* findSnapshot(const CompositeTimbre& tone, std::uint64_t id) {
    for (const auto& layer : tone.layers)
        if (layer.base_timbre && layer.base_timbre->library_id == id) return &*layer.base_timbre;
    for (const auto& snapshot : tone.embedded_timbres)
        if (snapshot.library_id == id) return &snapshot;
    return nullptr;
}

bool hasSccLoop(const CompositeTimbre& tone) {
    return std::any_of(tone.layers.begin(), tone.layers.end(), [](const auto& layer) {
        return layer.source == TimbreSource::Scc && layer.envelope_timeline.loop_start_count
            && layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count > *layer.envelope_timeline.loop_start_count;
    });
}

void setWave(SavedTimbreReference& ref, bool highRich) {
    constexpr double tau = 2 * std::numbers::pi;
    for (std::size_t i = 0; i < ref.scc_waveform.size(); ++i) {
        const double phase = tau * static_cast<double>(i) / ref.scc_waveform.size();
        const double value = highRich
            ? 0.75 * std::sin(5 * phase) + 0.45 * std::sin(7 * phase) + 0.20 * std::sin(9 * phase)
            : 0.85 * std::sin(phase) + 0.30 * std::sin(2 * phase) + 0.18 * std::sin(3 * phase);
        const auto sample = static_cast<std::int8_t>(std::clamp(std::lround(value * 100), -127L, 127L));
        ref.scc_waveform[i] = static_cast<std::uint8_t>(sample);
    }
}

void setAsymmetricEdgeRichWave(SavedTimbreReference& ref) {
    for (std::size_t i = 0; i < ref.scc_waveform.size(); ++i) {
        const int sample = i < 4 ? 96 - static_cast<int>(i) * 4
            : i < 11 ? 80 - static_cast<int>(i - 4) * 15
            : i < 19 ? -25 - static_cast<int>(i - 11) * 5
            : i < 25 ? -65 + static_cast<int>(i - 19) * 8
            : -17 - static_cast<int>(i - 25) * 13;
        ref.scc_waveform[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(sample));
    }
}
CompositeTimbre engineSccSource() {
    auto tone = defaultCompositeTimbre();
    auto scc = tone.layers.at(1);
    seedDefaultLayerTimbre(tone, scc);
    tone.layers = {scc};
    tone.embedded_timbres.clear();
    tone.layers[0].channel = 0;
    tone.layers[0].envelope_number = 0;
    tone.layers[0].volume = 12;
    if (!tone.layers[0].base_timbre) throw std::runtime_error("constructed SCC snapshot missing");
    setAsymmetricEdgeRichWave(*tone.layers[0].base_timbre);
    return tone;
}
CompositeTimbre pairedSource(bool highRichSecond, std::int32_t secondMicro = 0) {
    auto tone = defaultCompositeTimbre();
    auto scc = tone.layers.at(1);
    seedDefaultLayerTimbre(tone, scc);
    tone.layers = {scc, scc};
    tone.embedded_timbres.clear();
    tone.layers[0].channel = 0;
    tone.layers[0].envelope_number = 0;
    tone.layers[0].relative_semitones = 0;
    tone.layers[0].micro_detune = 0;
    tone.layers[0].volume = 7;
    tone.layers[1].channel = 1;
    tone.layers[1].envelope_number = 1;
    tone.layers[1].relative_semitones = 0;
    tone.layers[1].micro_detune = secondMicro;
    tone.layers[1].volume = 7;
    tone.layers[0].envelope_timeline.length_counts = 60;
    tone.layers[1].envelope_timeline.length_counts = 60;
    tone.layers[0].envelope_timeline.loop_start_count.reset();
    tone.layers[0].envelope_timeline.loop_end_count.reset();
    tone.layers[1].envelope_timeline.loop_start_count.reset();
    tone.layers[1].envelope_timeline.loop_end_count.reset();
    if (!tone.layers[0].base_timbre) throw std::runtime_error("default SCC snapshot missing");
    auto first = *tone.layers[0].base_timbre;
    setWave(first, false);
    tone.layers[0].base_timbre = first;
    auto second = first;
    second.library_id = allocateCompositeOwnedTimbreId(tone);
    second.name = highRichSecond ? "High partial SCC" : "Detuned shared SCC";
    if (highRichSecond) setWave(second, true);
    tone.layers[1].base_timbre = second;
    return tone;
}

void testDirectPeriodicBeatsReconstructedOnEngineWave() {
    constexpr std::size_t frames = 28'800;
    constexpr std::size_t keyoff = 24'000;
    const auto source = analyzeRendered(engineSccSource(), frames, keyoff);
    const auto reconstructed = convert(source, CompositeWavConfiguration::Scc, keyoff, 8,
        CompositeWavLoopMode::None, CompositeWavStrategy::Independent, 12,
        CompositeWavSccWaveMethod::Reconstructed);
    const auto direct = convert(source, CompositeWavConfiguration::Scc, keyoff, 8,
        CompositeWavLoopMode::None, CompositeWavStrategy::Independent, 12,
        CompositeWavSccWaveMethod::DirectPeriodic);
    std::printf("L waveform methods: reconstructed=%.9f direct=%.9f direct_extract=%.6f waves=(%zu,%zu)\n",
        reconstructed.best_loss, direct.best_loss, direct.morph_search.direct_extraction_seconds,
        reconstructed.resource_plan.scc_waveforms, direct.resource_plan.scc_waveforms);
    require(direct.morph_search.direct_extraction_seconds > 0,
        "DirectPeriodic is exercised on source PCM rendered from an asymmetric SCC wave");
    require(direct.best_loss + 1e-4 < reconstructed.best_loss,
        "DirectPeriodic yields a measured full-objective improvement over Reconstructed for the EngineCore SCC source");
}
void testOffPathKeyframe() {
    const auto analysis = harmonicFixture(31'200, [](double t) {
        std::array<double, 8> a{};
        a[1] = 1.0;
        a[2] = 0.15;
        const auto bump = [](double x, double centre, double width) {
            const double z = (x - centre) / width;
            return std::exp(-z * z);
        };
        a[3] = 0.04 + 0.30 * bump(t, 0.30, 0.12);
        a[5] = 0.03 + 0.22 * bump(t, 0.30, 0.12);
        a[7] = 0.02 + 0.95 * bump(t, 0.30, 0.11);
        return a;
    });
    auto result = convert(analysis, CompositeWavConfiguration::Scc, 26'400, 8,
        CompositeWavLoopMode::None, CompositeWavStrategy::Independent, 48);
    const auto& layer = result.composite_tone->layers.front();
    require(result.morph_search.keyframe_seconds > 0,
        "off-path analysis runs when an interior timbre differs from the endpoint Morph path");
    require(layer.base_timbre.has_value(), "off-path result retains its first SCC endpoint snapshot");
    const auto firstWave = layer.base_timbre->scc_waveform;
    const auto lastEvent = std::max_element(layer.timbre_automation.begin(), layer.timbre_automation.end(),
        [](const auto& a, const auto& b) { return a.count < b.count; });
    require(lastEvent != layer.timbre_automation.end(), "off-path result retains its terminal source event");
    const auto* lastRef = findSnapshot(*result.composite_tone, lastEvent->target_library_id);
    require(lastRef != nullptr, "terminal SCC source event resolves to its snapshot");
    const auto middle = std::find_if(layer.timbre_automation.begin(), layer.timbre_automation.end(),
        [](const auto& event) { return event.kind == EnvelopeEventKind::Timbre
            && event.count >= 8 && event.count <= 25; });
    require(middle != layer.timbre_automation.end(),
        "the output retains an interior SCC keyframe for the off-path partial peak");
    const auto* middleRef = findSnapshot(*result.composite_tone, middle->target_library_id);
    require(middleRef && middleRef->scc_waveform != firstWave
            && middleRef->scc_waveform != lastRef->scc_waveform,
        "the retained interior keyframe uses a waveform distinct from both endpoint waves");
}

void testNearWaveReuseRegressionIsRejected() {
    const auto nearRepeat = harmonicFixture(28'800, [](double t) {
        std::array<double, 8> a{};
        a[1] = 1.0;
        a[2] = 0.18;
        const double bump = std::exp(-std::pow((t - 0.25) / 0.11, 2));
        a[3] = 0.08 + 0.11 * bump;
        a[5] = 0.04 + 0.06 * bump;
        return a;
    });
    auto reuse = convert(nearRepeat, CompositeWavConfiguration::Scc, 24'000, 8,
        CompositeWavLoopMode::None, CompositeWavStrategy::Independent, 48);
    const auto& reusedLayer = reuse.composite_tone->layers.front();
    std::vector<std::array<std::uint8_t, 32>> returnedWaves;
    for (const auto& event : reusedLayer.timbre_automation) {
        if (event.kind != EnvelopeEventKind::Timbre) continue;
        const auto* reference = findSnapshot(*reuse.composite_tone, event.target_library_id);
        if (reference) returnedWaves.push_back(reference->scc_waveform);
    }
    std::sort(returnedWaves.begin(), returnedWaves.end());
    const auto uniqueReturned = static_cast<std::size_t>(
        std::unique(returnedWaves.begin(), returnedWaves.end()) - returnedWaves.begin());
    std::printf("E near reuse: trials=%zu accepted=%zu evaluations=%zu selected_unique=%zu keyframes=%zu local_pcm=%.6f final_reuse=%zu\n",
        reuse.morph_search.approximate_reuse_trials, reuse.morph_search.approximate_reuse_accepted,
        reuse.evaluations, reuse.resource_plan.scc_waveforms, reusedLayer.timbre_automation.size(),
        reuse.quality.max_pcm_discontinuity, returnedWaves.size() - uniqueReturned);
    std::printf("E candidate deltas: objective=%.9f local=%.9f waveforms_saved=%d\n",
        reuse.morph_search.approximate_reuse_objective_delta,
        reuse.morph_search.approximate_reuse_local_delta,
        reuse.morph_search.approximate_reuse_waveforms_saved);
    require(reuse.morph_search.approximate_reuse_trials > 0,
        "near-repeated waveform candidate is evaluated for SCC resource reuse");
    require(reuse.morph_search.approximate_reuse_accepted == 0,
        "a near-wave replacement that worsens local PCM quality is rejected");
    require(std::isfinite(reuse.morph_search.approximate_reuse_objective_delta)
            && reuse.morph_search.approximate_reuse_objective_delta > 0,
        "the measured full objective confirms this reuse proposal is worse");
    require(std::isfinite(reuse.morph_search.approximate_reuse_local_delta)
            && reuse.morph_search.approximate_reuse_local_delta > 0,
        "the measured local-quality guard rejects this reuse proposal");
    require(reuse.morph_search.approximate_reuse_waveforms_saved > 0,
        "the rejected proposal would reduce SCC waveform resource use");
    require(returnedWaves.size() >= 2 && uniqueReturned == returnedWaves.size(),
        "the rejected near-wave proposal does not appear in the final keyframe sequence");
}

const CompositeWavConversionResult& stableLoopResult() {
    static const auto result = [] {
        const auto stable = harmonicFixture(28'800, [](double t) {
            std::array<double, 8> a{};
            a[1] = t < 0.08 ? 0.45 + 0.55 * t / 0.08 : 1.0;
            a[2] = 0.20;
            a[3] = 0.08;
            return a;
        });
        return convert(stable, CompositeWavConfiguration::Scc, 24'000, 8);
    }();
    return result;
}

void testStableLoopRetention() {
    const auto& result = stableLoopResult();
    std::printf("F loop: probes=%zu boundary=%.6f steady=%.6f selected=%d\n",
        result.loop_probe_renders, result.quality.loop_boundary, result.quality.loop_steady,
        hasSccLoop(*result.composite_tone));
    printConversionWarnings("F loop", result);
    require(hasSccLoop(*result.composite_tone),
        "a stable, periodic held portion retains an automatic SCC loop");
    require(result.loop_probe_renders > 0,
        "loop candidate is rendered past key-off to inspect repeated held-note cycles");
}

void testTwoTraversalLoopAudio() {
    const auto& result = stableLoopResult();
    printConversionWarnings("N held loop", result);
    const auto loopLayer = std::find_if(result.composite_tone->layers.begin(), result.composite_tone->layers.end(),
        [](const auto& layer) { return layer.source == TimbreSource::Scc
            && layer.envelope_timeline.loop_start_count && layer.envelope_timeline.loop_end_count; });
    require(loopLayer != result.composite_tone->layers.end(), "stable loop interval is retained");
    const auto endFrame = static_cast<std::size_t>(*loopLayer->envelope_timeline.loop_end_count) * 800;
    const auto period = static_cast<std::size_t>(*loopLayer->envelope_timeline.loop_end_count
        - *loopLayer->envelope_timeline.loop_start_count) * 800;
    CompositeWavRenderOptions held;
    held.key_off_frame = endFrame + 2 * period + 1'600;
    held.frame_count = held.key_off_frame + 800;
    const auto twoCycles = renderCompositeWav(*result.composite_tone, held);
    require(twoCycles.ok() && twoCycles.mono_pcm.size() == held.frame_count
            && endFrame + 2 * period < twoCycles.mono_pcm.size(),
        "held-note Engine rendering advances past the second complete SCC loop traversal");
    const auto cycleOne = endFrame + period;
    const auto cycleTwo = endFrame + 2 * period;
    const auto windowRms = [&](std::size_t begin) {
        const auto count = std::min<std::size_t>(400, twoCycles.mono_pcm.size() - begin);
        double energy{};
        for (std::size_t i = begin; i < begin + count; ++i)
            energy += twoCycles.mono_pcm[i] * twoCycles.mono_pcm[i];
        return std::sqrt(energy / std::max<std::size_t>(1, count));
    };
    const auto firstRms = windowRms(cycleOne);
    const auto secondRms = windowRms(cycleTwo);
    std::printf("N held loop: period_frames=%zu first_rms=%.6f second_rms=%.6f quality_steady=%.6f\n",
        period, firstRms, secondRms, result.quality.loop_steady);
    require(cycleOne < twoCycles.mono_pcm.size() && cycleTwo < twoCycles.mono_pcm.size()
            && firstRms > 1e-4 && secondRms > 1e-4,
        "audible held-note PCM persists through both successive loop traversals");
    require(std::isfinite(result.quality.loop_boundary) && std::isfinite(result.quality.loop_steady)
            && result.quality.loop_steady < 0.5,
        "the second loop seam remains bounded after a full additional cycle");
}

void testMovingVolumeRejectsLoop() {
    const auto moving = harmonicFixture(28'800, [](double t) {
        std::array<double, 8> a{};
        a[1] = 0.15 + 0.85 * std::exp(-t * 3.0);
        a[2] = 0.20;
        a[3] = 0.08;
        return a;
    });
    auto result = convert(moving, CompositeWavConfiguration::Scc, 24'000, 8);
    std::printf("G decay loop: selected=%d boundary=%.6f steady=%.6f\n",
        hasSccLoop(*result.composite_tone), result.quality.loop_boundary, result.quality.loop_steady);
    require(!hasSccLoop(*result.composite_tone),
        "automatic loop is rejected when a decaying source has no stable repeated volume state");
}

void testSameWaveDetuneIsAdoptedAndShared() {
    PsgSccModulationBase base;
    require(psgSccModulationBase(60, 0, base), "C4 has a representable SCC pitch base");
    auto plusEightCents = static_cast<std::int32_t>(std::lround(
        base.unshifted * (std::pow(2.0, -8.0 / 1200.0) - 1.0)));
    if (!plusEightCents) plusEightCents = -1;
    std::uint16_t period{};
    require(psgSccPeriodWithMicroDetune(60, plusEightCents, period) && period > 0,
        "the exact +8-cent source offset is representable by the existing SCC period table");
    constexpr std::size_t frames = 48'000;
    constexpr std::size_t keyoff = 46'400;
    const auto known = pairedSource(false, plusEightCents);
    const auto analysis = analyzeRendered(known, frames, keyoff);
    auto result = convert(analysis, CompositeWavConfiguration::SccScc, keyoff, 16,
        CompositeWavLoopMode::None, CompositeWavStrategy::Independent, 96);
    printConversionWarnings("H detune", result, 800);
    const auto& layers = result.composite_tone->layers;
    std::printf("H detune: expected_micro=%d selected=(%d,%d) shared=%d probes=%zu penalty=%.6f unique=%zu best_loss=%.6f total=%.6f\n",
        plusEightCents, layers.size() > 0 ? layers[0].micro_detune : 0,
        layers.size() > 1 ? layers[1].micro_detune : 0,
        layers.size() == 2 && layers[0].base_timbre && layers[1].base_timbre
            && layers[0].base_timbre->scc_waveform == layers[1].base_timbre->scc_waveform,
        result.morph_search.neighbour_pitch_probe_renders, result.quality.neighbour_pitch_penalty,
        result.resource_plan.scc_waveforms, result.best_loss, result.quality.total);
    require(layers.size() == 2 && layers[0].source == TimbreSource::Scc
            && layers[1].source == TimbreSource::Scc,
        "same-wave detune fit retains two SCC channels");
    require(layers[0].base_timbre && layers[1].base_timbre
            && layers[0].base_timbre->scc_waveform == layers[1].base_timbre->scc_waveform,
        "known same-wave source is represented by a shared, byte-identical SCC waveform");
    require(std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
        return layer.micro_detune == plusEightCents;
    }), "converter adopts the exact nonzero detune present in the engine-rendered source");
    require(result.morph_search.neighbour_pitch_probe_renders >= 2
            && std::isfinite(result.quality.neighbour_pitch_penalty),
        "adopted detune is checked at neighboring playable pitches through the Engine");
    const auto mgs = formatMgsComposite(*result.composite_tone);
    const auto parsed = parseMgsComposite(mgs.source);
    require(mgs.valid() && parsed.valid(), "same-wave dual-SCC MGSC output parses");
    const auto parsedScc = std::count_if(parsed.timbre.layers.begin(), parsed.timbre.layers.end(),
        [](const auto& layer) { return layer.source == TimbreSource::Scc; });
    require(parsedScc == 2 && extractMgsSourceDefinitions(mgs.source, 'e').size() == 1,
        "two shared-wave SCC tracks serialize one common @e definition");
    require(std::any_of(parsed.timbre.layers.begin(), parsed.timbre.layers.end(), [&](const auto& layer) {
        return layer.source == TimbreSource::Scc && layer.micro_detune == plusEightCents;
    }), "MGSC parse preserves the selected micro-detune");
    const auto portable = CompositeTimbreLibrary::serializeTimbreFile(*result.composite_tone, 0);
    const auto restored = CompositeTimbreLibrary::deserializeTimbreFile(portable);
    require(restored && *restored == *result.composite_tone,
        "portable timbre roundtrip preserves shared waves and adopted detune");
}

void testDistinctSccWavesRemainDistinct() {
    constexpr std::size_t frames = 38'400;
    constexpr std::size_t keyoff = 36'000;
    auto known = pairedSource(true);
    auto& low = known.layers[0];
    auto& high = known.layers[1];
    low.volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 14, .count = 0},
        {.kind = EnvelopeEventKind::Volume, .value = 11, .count = 12},
        {.kind = EnvelopeEventKind::Volume, .value = 8, .count = 28},
    };
    high.volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 3, .count = 0},
        {.kind = EnvelopeEventKind::Volume, .value = 10, .count = 10},
        {.kind = EnvelopeEventKind::Volume, .value = 13, .count = 26},
        {.kind = EnvelopeEventKind::Volume, .value = 5, .count = 38},
    };
    const auto analysis = analyzeRendered(known, frames, keyoff);
    auto result = convert(analysis, CompositeWavConfiguration::SccScc, keyoff, 16,
        CompositeWavLoopMode::None, CompositeWavStrategy::LowHigh, 64);
    const auto& layers = result.composite_tone->layers;
    std::printf("J distinct waves: waves=%zu base_distinct=%d layers=%zu eval=%zu\n",
        result.resource_plan.scc_waveforms,
        layers.size() == 2 && layers[0].base_timbre && layers[1].base_timbre
            && layers[0].base_timbre->scc_waveform != layers[1].base_timbre->scc_waveform,
        layers.size(), result.evaluations);
    require(layers.size() == 2 && layers[0].base_timbre && layers[1].base_timbre,
        "low/high SCC output has two materialized wave sources");
    require(layers[0].base_timbre->scc_waveform != layers[1].base_timbre->scc_waveform,
        "distinct low-rich and high-rich source waves remain distinct after SCC×SCC selection");
    const auto mgs = formatMgsComposite(*result.composite_tone);
    const auto parsed = parseMgsComposite(mgs.source);
    require(mgs.valid() && parsed.valid(), "distinct SCC waves export and parse");
    const auto sccDefinitions = extractMgsSourceDefinitions(mgs.source, 's');
    require(sccDefinitions.size() >= 2,
        "MGSC retains separate @s definitions for genuinely different output waves");
}
}

int main() {
    int failures{};
    const auto run = [&](const char* label, const auto& test) {
        std::printf("[ RUN      ] %s\n", label);
        try {
            test();
            std::printf("[       OK ] %s\n", label);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "[  FAILED  ] %s: %s\n", label, error.what());
        }
    };
    run("C off-path keyframe", testOffPathKeyframe);
    run("L DirectPeriodic acoustic benefit", testDirectPeriodicBeatsReconstructedOnEngineWave);
    run("E near-wave local regression rejected", testNearWaveReuseRegressionIsRejected);
    run("F stable loop retention", testStableLoopRetention);
    run("G moving-volume loop rejection", testMovingVolumeRejectsLoop);
    run("N two held loop traversals", testTwoTraversalLoopAudio);
    run("H same-wave detune adoption", testSameWaveDetuneIsAdoptedAndShared);
    run("J distinct SCC waves", testDistinctSccWavesRemainDistinct);
    std::printf("Acceptance cases: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
