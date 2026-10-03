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
    require(result.preview.ok() && result.preview.mono_pcm.size() == 9600, "exact preview duration");
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
    try { run(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "WAV conversion regression: %s\n", error.what()); return 1; }
}
