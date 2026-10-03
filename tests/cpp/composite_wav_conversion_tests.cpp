// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_conversion.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>
#include <numbers>
#include <set>
#include <algorithm>
#include <numeric>
#include <limits>
#include <stdexcept>

namespace {
using namespace mgstc::engine;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::shared_ptr<const SourceAnalysis> fixture() {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48000;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    for (std::size_t i = 0; i < 9600; ++i) {
        const double t = static_cast<double>(i) / 48000;
        const double envelope = std::min(1.0, t / .015) * std::exp(-t * 4);
        const double phase = 2 * std::numbers::pi * 261.625565 * t;
        const auto value = static_cast<float>(envelope * (.22 * std::sin(phase)
            + .12 * std::exp(-t * 12) * std::sin(phase * 3)));
        pcm->mono_samples.push_back(value);
        pcm->interleaved_samples.push_back(value);
    }
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    const auto result = analyzeCompositeWaveSource(pcm, {0, pcm->mono_samples.size()}, options);
    require(result.completion == SourceAnalysisCompletion::Completed, "fixture analysis");
    return result.analysis;
}
void validate(const CompositeWavConversionResult& result, std::size_t layers, std::size_t limit) {
    if (result.completion != CompositeWavConversionCompletion::Completed)
        throw std::runtime_error("conversion failed: " + result.error);
    require(result.composite_tone.has_value(), "ordinary composite output exists");
    const auto& tone = *result.composite_tone;
    require(tone.layers.size() == layers, "requested chip configuration");
    const auto& analysis = *result.analysis_reference;
    const auto frames = static_cast<std::size_t>(std::llround((analysis.selection.end - analysis.selection.begin)
        * 48000.0 / analysis.source->sample_rate));
    require(result.preview.ok() && result.preview.mono_pcm.size() == frames, "exact preview duration");
    require(std::isfinite(result.best_loss) && result.best_loss <= result.initial_loss + 1e-10,
            "best objective never regresses");
    require(result.resource_plan.scc_waveforms > 0 && result.resource_plan.scc_waveforms <= limit,
            "waveform budget honored");
    const auto bank = compileSccMorph(tone);
    require(bank.valid && bank.used <= limit, "shared SCC allocator accepts output");
    const auto saved = CompositeTimbreLibrary::serializeTimbreFile(tone, 0);
    const auto loaded = CompositeTimbreLibrary::deserializeTimbreFile(saved);
    require(loaded && *loaded == tone, "portable file roundtrip");
    const auto payload = serializeCompositeSoundPayload(tone);
    const auto restored = deserializeCompositeSoundPayload(payload, kCompositeSoundPayloadVersion);
    require(restored && serializeCompositeSoundPayload(*restored) == payload, "sound payload roundtrip");
    const auto mgs = formatMgsComposite(tone);
    require(mgs.valid(), "ordinary MGSC export accepts output");
    require(parseMgsComposite(mgs.source).valid(), "MGSC export parses");
    std::set<std::uint64_t> sccIds;
    for (const auto& layer : tone.layers)
        if (layer.source == TimbreSource::Scc && layer.base_timbre) sccIds.insert(layer.base_timbre->library_id);
    for (const auto& ref : tone.embedded_timbres)
        if (ref.source == TimbreSource::Scc) sccIds.insert(ref.library_id);
    for (const auto& layer : tone.layers)
        if (layer.source == TimbreSource::Opll && layer.base_timbre)
            require(!sccIds.contains(layer.base_timbre->library_id), "OPLL snapshot ID never aliases rebuilt SCC");
}

std::shared_ptr<const SourceAnalysis> temporalFixture(bool noisy, bool glide) {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48000; pcm->channels = 1; pcm->bit_depth = 32;
    double phase{};
    std::uint32_t random = 0x1458;
    for (std::size_t i = 0; i < 24000; ++i) {
        const double t = i / 48000.0;
        phase += 2 * std::numbers::pi * 261.625565 * (glide ? 1 + .025 * std::sin(t * 35) : 1) / 48000;
        const double attack = std::min(1.0, t / .025);
        const double release = t < .4 ? 1 : std::exp(-(t - .4) * 30);
        const double morph = std::clamp((t - .07) / .22, 0.0, 1.0);
        random = random * 1664525U + 1013904223U;
        const double noise = noisy ? (static_cast<double>(random >> 8) / 16777215.0 - .5) * .045 : 0;
        const float value = static_cast<float>(attack * release * (.17 * std::sin(phase)
            + .10 * (1 - morph * morph) * std::sin(2 * phase + .35)
            + .11 * morph * morph * std::sin(5 * phase - .2) + noise));
        pcm->mono_samples.push_back(value); pcm->interleaved_samples.push_back(value);
    }
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    auto result = analyzeCompositeWaveSource(pcm, {0, pcm->mono_samples.size()}, options);
    require(result.completion == SourceAnalysisCompletion::Completed, "temporal corpus analysis");
    auto analysis = std::make_shared<SourceAnalysis>(*result.analysis);
    // A deterministic manual sustain selection exercises the same editor path,
    // without depending on a detector's optional steady-region confidence.
    analysis->sustain_region = EstimatedSourceRegion{{4800, 14400}, 1};
    return analysis;
}

double projection(std::span<const float> pcm, double frequency) {
    double real{}, imaginary{};
    const auto start = std::min<std::size_t>(2400, pcm.size() / 4);
    for (auto i = start; i < pcm.size(); ++i) {
        const double phase = 2 * std::numbers::pi * frequency * i / 48000;
        real += pcm[i] * std::cos(phase); imaginary += pcm[i] * std::sin(phase);
    }
    return std::hypot(real, imaginary);
}

void completionRegressions() {
    const auto analysis = temporalFixture(false, false);
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::SccScc;
    options.strategy = CompositeWavStrategy::Independent;
    options.key_off_position = 19200;
    options.max_scc_waveforms = 16;
    options.max_evaluations = 160;
    options.stagnation_limit = 0;
    const auto optimized = convertCompositeWave(analysis, options);
    validate(optimized, 2, 16);
    std::printf("Temporal WAV objective %.8f -> %.8f; evaluations %zu, loop probes %zu\n",
        optimized.initial_loss, optimized.best_loss, optimized.evaluations, optimized.loop_probe_renders);
    for (std::size_t family = 0; family < optimized.search.trials.size(); ++family)
        std::printf("Coordinate family %zu: %zu rendered, %zu accepted\n", family,
            optimized.search.trials[family], optimized.search.accepted[family]);
    require(optimized.evaluations <= options.max_evaluations, "joint coordinates stay within render budget");
    require(optimized.loop_probe_renders > 0 && optimized.loop_probe_renders <= optimized.evaluations,
        "short sustain uses at most one bounded additional loop probe per candidate");
    for (std::size_t family = 0; family < optimized.search.trials.size(); ++family)
        require(optimized.search.trials[family] > 0, "every applicable coordinate family reaches real Engine PCM");
    require(optimized.best_loss + 1e-5 < optimized.initial_loss, "temporal corpus improves measured full-source objective");
    const auto& first = optimized.composite_tone->layers[0];
    const auto& second = optimized.composite_tone->layers[1];
    require(first.base_timbre->scc_waveform != second.base_timbre->scc_waveform,
        "independent dual SCC does not normalize identical copies of the source spectrum");
    require(optimized.search.accepted[static_cast<std::size_t>(CompositeWavSearchFamily::WaveShape)] > 0,
        "render-feedback wave refinement improves a real temporal corpus candidate");
    require(optimized.search.accepted[static_cast<std::size_t>(CompositeWavSearchFamily::Envelope)] > 0,
        "render-feedback ENV refinement improves a real temporal corpus candidate");
    const auto bank = compileSccMorph(*optimized.composite_tone);
    require(bank.valid && bank.used <= 16, "all independently refined intervals share finite bank allocation");
    options.configuration = CompositeWavConfiguration::Scc;
    options.max_evaluations = 36;
    options.loop_mode = CompositeWavLoopMode::None;
    const auto noisy = convertCompositeWave(temporalFixture(true, true), options);
    validate(noisy, 1, 16);
    require(noisy.quality.volume < 1 && std::isfinite(noisy.quality.attack),
        "residual-aware objective retains audible amplitude and attack constraints");

    // Strong measured inharmonic partials remain residual instead of being
    // silently moved into an integer SCC harmonic during seed generation.
    auto detuned = std::make_shared<SourceAnalysis>(*analysis);
    for (auto& frame : detuned->harmonic_trajectory) for (auto& h : frame.harmonics)
        if (h.harmonic == 2) { h.frequency_hz = 2.2 * detuned->reference_pitch_hz; h.amplitude = .5; }
    options.max_evaluations = 1;
    const auto detunedResult = convertCompositeWave(detuned, options);
    validate(detunedResult, 1, 16);
    const auto& wave = detunedResult.composite_tone->layers.front().base_timbre->scc_waveform;
    double h2Real{}, h2Imaginary{};
    for (std::size_t i = 0; i < wave.size(); ++i) {
        const auto value = static_cast<std::int8_t>(wave[i]);
        h2Real += value * std::cos(4 * std::numbers::pi * i / wave.size());
        h2Imaginary += value * std::sin(4 * std::numbers::pi * i / wave.size());
    }
    require(std::hypot(h2Real, h2Imaginary) < 32, "inharmonic residual is not forced into SCC harmonic2");

    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48000; pcm->channels = 1; pcm->bit_depth = 32;
    for (std::size_t i = 0; i < 9600; ++i) {
        const auto value = static_cast<float>(.2 * std::sin(2 * std::numbers::pi * 220 * i / 48000));
        pcm->mono_samples.push_back(value); pcm->interleaved_samples.push_back(value);
    }
    SourceAnalysisOptions pitchOptions;
    pitchOptions.reference_pitch_hz = 110;
    const auto subharmonic = analyzeCompositeWaveSource(pcm, {0, 9600}, pitchOptions);
    require(subharmonic.completion == SourceAnalysisCompletion::Completed, "manual octave guide analysis");
    options.key_off_position = 9600;
    const auto guided = convertCompositeWave(subharmonic.analysis, options);
    validate(guided, 1, 16);
    require(projection(guided.preview.mono_pcm, 220) > 5 * projection(guided.preview.mono_pcm, 110),
        "manual subharmonic guide retains physical220Hz as harmonic2 rather than transpose to110Hz");
    SourceAnalysis empty;
    auto invalidAnalysis = std::make_shared<SourceAnalysis>(*analysis);
    invalidAnalysis->periodic_confidence = std::numeric_limits<double>::quiet_NaN();
    require(convertCompositeWave(invalidAnalysis, options).completion == CompositeWavConversionCompletion::InvalidInput,
        "nonfinite periodic summary is rejected before rendering");
    invalidAnalysis = std::make_shared<SourceAnalysis>(*analysis);
    invalidAnalysis->harmonic_trajectory.front().periodic_confidence = 1.1;
    require(convertCompositeWave(invalidAnalysis, options).completion == CompositeWavConversionCompletion::InvalidInput,
        "out-of-range frame confidence is rejected before rendering");
    require(std::isfinite(evaluateCompositeWavePcm(pcm->mono_samples, pcm->mono_samples, 220, 720,
        0, {}, nullptr, &empty).total), "metric API safely ignores incomplete optional analysis");
}
void run() {
    const auto analysis = fixture();
    const auto sourceBefore = analysis->source->interleaved_samples;
    CompositeWavConversionOptions options;
    options.max_evaluations = 18;
    options.max_scc_waveforms = 8;
    options.loop_mode = CompositeWavLoopMode::None;
    options.key_off_position = 7200;
    const auto first = convertCompositeWave(analysis, options);
    validate(first, 1, 8);
    const auto again = convertCompositeWave(analysis, options);
    require(first.composite_tone == again.composite_tone && first.preview.mono_pcm == again.preview.mono_pcm,
            "repeat conversion is deterministic with fresh runtime");
    options.max_evaluations = 4;
    for (const auto strategy : {CompositeWavStrategy::Automatic, CompositeWavStrategy::FundamentalResidual,
            CompositeWavStrategy::LowHigh, CompositeWavStrategy::AttackSustain, CompositeWavStrategy::Independent}) {
        options.configuration = CompositeWavConfiguration::SccScc;
        options.strategy = strategy;
        validate(convertCompositeWave(analysis, options), 2, 8);
    }
    options.configuration = CompositeWavConfiguration::SccOpllRom;
    options.strategy = CompositeWavStrategy::Automatic;
    options.fixed_opll_tone = 7;
    const auto rom = convertCompositeWave(analysis, options);
    validate(rom, 2, 8);
    require(rom.composite_tone->layers.back().base_opll_rom == 7, "manual ROM choice retained");
    options.fixed_opll_tone.reset();
    options.max_evaluations = 15;
    const auto automaticRom = convertCompositeWave(analysis, options);
    validate(automaticRom, 2, 8);
    require(automaticRom.evaluations == 15 && automaticRom.composite_tone->layers.back().base_opll_rom <= 14,
            "automatic selection evaluates every ROM within its explicit budget");
    options.configuration = CompositeWavConfiguration::SccOpllOriginal;
    options.max_evaluations = 24;
    validate(convertCompositeWave(analysis, options), 2, 8);
    options.configuration = CompositeWavConfiguration::Scc;
    options.max_scc_waveforms = 1;
    options.scc_start_number = 31;
    validate(convertCompositeWave(analysis, options), 1, 1);
    options.max_scc_waveforms = 33;
    require(convertCompositeWave(analysis, options).completion == CompositeWavConversionCompletion::InvalidInput,
            "invalid global capacity rejected");
    options.max_scc_waveforms = 8;
    options.control = std::make_shared<CompositeWavConversionControl>();
    options.control->cancel_requested = true;
    const auto cancelled = convertCompositeWave(analysis, options);
    require(cancelled.completion == CompositeWavConversionCompletion::Cancelled && cancelled.evaluations == 0,
            "pre-cancel does no candidate work");
    options.scc_start_number.reset();
    options.max_evaluations = 4096;
    options.control = std::make_shared<CompositeWavConversionControl>();
    std::jthread cancelWorker([control = options.control](std::stop_token stop) {
        while (!stop.stop_requested() && control->evaluations.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!stop.stop_requested()) control->cancel_requested = true;
    });
    const auto interrupted = convertCompositeWave(analysis, options);
    cancelWorker.request_stop();
    require(interrupted.completion == CompositeWavConversionCompletion::Cancelled
            && interrupted.evaluations > 0, "active candidate evaluation cooperatively cancels");
    require(analysis->source->interleaved_samples == sourceBefore, "source PCM remains immutable");
    const auto identical = evaluateCompositeWavePcm(sourceBefore, sourceBefore, 261.625565, 720);
    require(identical.total < 1e-9, "objective identity has zero error");
    const std::vector<float> silence(sourceBefore.size());
    require(evaluateCompositeWavePcm(sourceBefore, silence, 261.625565, 720).total > identical.total,
            "objective penalizes missing audible output");
}
}
int main() {
    try { run(); completionRegressions(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "WAV conversion regression: %s\n", error.what()); return 1; }
}
