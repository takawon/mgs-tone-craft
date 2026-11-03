// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_conversion.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/patchslide_database.hpp"
#include "mgstc/engine/patchslide_timbre.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace {
using namespace mgstc::engine;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::shared_ptr<const SourceAnalysis> fixture() {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48'000;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    pcm->sample_format = SourceSampleFormat::FloatPcm;
    for (std::size_t frame = 0; frame < 7200; ++frame) {
        const double seconds = double(frame) / pcm->sample_rate;
        const double phase = 2 * std::numbers::pi * 261.625565 * seconds;
        const double envelope = std::min(1.0, seconds / .012) * std::exp(-3 * seconds);
        const auto sample = float(envelope * (.22 * std::sin(phase)
            + .12 * std::exp(-4 * seconds) * std::sin(3 * phase) + .05 * std::sin(5 * phase)));
        pcm->mono_samples.push_back(sample);
        pcm->interleaved_samples.push_back(sample);
    }
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    const auto analysis = analyzeCompositeWaveSource(pcm, {0, pcm->mono_samples.size()}, options);
    require(analysis.completion == SourceAnalysisCompletion::Completed, "synthetic source analysis completes");
    return analysis.analysis;
}

bool nativeSequence(const CompositeLayer& layer, const PatchSlideDatabase& db) {
    if (layer.source != TimbreSource::Opll || layer.base_timbre) return false;
    for (const auto& candidate : db.candidates()) {
        if (!candidate.auto_safe || PatchSlideDatabase::isRejected(candidate)) continue;
        const auto expected = makePatchSlideTimbre(candidate);
        if (expected && layer.base_opll_rom == expected->layers[0].base_opll_rom
            && layer.timbre_automation == expected->layers[0].timbre_automation) return true;
    }
    return false;
}

void validate(const CompositeWavConversionResult& result, CompositeWavConfiguration configuration,
    const PatchSlideDatabase& db, std::size_t budget) {
    if (result.completion != CompositeWavConversionCompletion::Completed)
        throw std::runtime_error("PatchSlide conversion did not complete: " + result.error);
    require(result.composite_tone && result.composite_tone->layers.size() == 2,
        "every PatchSlide configuration returns exactly two authoring layers");
    require(result.preview.ok() && !result.preview.clipped && !result.preview.mono_pcm.empty(),
        "winner has non-clipped actual whole-composite Engine PCM");
    require(std::any_of(result.preview.mono_pcm.begin(), result.preview.mono_pcm.end(),
        [](float value) { return std::abs(value) > 1e-6F; }), "whole-composite PCM is audible");
    require(result.evaluations > 0 && result.evaluations <= budget && std::isfinite(result.best_loss)
        && result.best_loss <= result.initial_loss + 1e-9, "final evaluation budget and measured winner are valid");
    require(result.patchslide_search.eligible_candidates > 0
        && result.patchslide_search.prefiltered_candidates <= 32
        && result.patchslide_search.preliminary_renders <= 38
        && result.patchslide_search.max_probe_frames <= 24'000
        && result.patchslide_search.pair_trials <= budget,
        "compact acquisition and pair search respect explicit finite limits");
    const bool scc = configurationUsesScc(configuration);
    require(scc ? result.resource_plan.scc_waveforms > 0 && result.resource_plan.scc_waveforms <= 8
                : result.resource_plan.scc_waveforms == 0 && result.resource_plan.scc_numbers.empty(),
        "OPLL-only results have zero SCC resources, SCC results retain their bounded bank");
    const auto plan = buildCompositePlaybackPlan(*result.composite_tone);
    require(plan.audible_counts[static_cast<std::size_t>(TimbreSource::Opll)] == (scc ? 1 : 2)
        && (scc || plan.voice_capacity == 4), "two OPLL layers consume channels of one nine-channel chip");
    std::size_t slides{}, originals{};
    for (const auto& layer : result.composite_tone->layers) {
        if (layer.source != TimbreSource::Opll) continue;
        if (layer.base_timbre) ++originals;
        else {
            ++slides;
            require(nativeSequence(layer, db), "all ROM numbers, holds, ordering and duplicate @ events match an AUTO_SAFE DB sequence");
            require(layer.envelope_timeline.length_counts >= layer.timbre_automation.back().count + 1,
                "timeline contains the complete native sequence");
            if (layer.envelope_timeline.loop_start_count)
                require(*layer.envelope_timeline.loop_start_count >= layer.timbre_automation.back().count,
                    "automatic loops never recycle or hide the PatchSlide prefix");
        }
    }
    require(slides == (configuration == CompositeWavConfiguration::OpllPatchSlideOpllPatchSlide ? 2 : 1)
        && originals == (configuration == CompositeWavConfiguration::OpllOriginalOpllPatchSlide ? 1 : 0),
        "only the original-plus-PatchSlide configuration owns one shared custom tone definition");
    const auto portable = CompositeTimbreLibrary::serializeTimbreFile(*result.composite_tone, 0);
    const auto restored = CompositeTimbreLibrary::deserializeTimbreFile(portable);
    require(restored && *restored == *result.composite_tone, "complete DB-independent native timbre survives save/reload");
    const auto mgs = formatMgsComposite(*result.composite_tone);
    require(mgs.valid(), "whole candidate exports to MGSC");
    const auto parsed = parseMgsComposite(mgs.source);
    require(parsed.valid(), "exported MGSC parses for replay");
    CompositeWavRenderOptions replay;
    replay.midi_note = 60;
    replay.frame_count = 7200;
    replay.key_off_frame = 6400;
    require(renderCompositeWav(parsed.timbre, replay).ok(), "MGSC round trip replays through the same chip Engine");
    for (const auto& candidate : result.retained_candidates) {
        for (const auto& layer : candidate.tone.layers)
            if (layer.source == TimbreSource::Opll && !layer.base_timbre)
                require(nativeSequence(layer, db), "retained alternatives also keep every DB event intact");
    }
    for (const int note : {59, 61}) {
        replay.midi_note = static_cast<std::uint8_t>(note);
        const auto neighbour = renderCompositeWav(*restored, replay);
        require(neighbour.ok() && !neighbour.clipped
            && std::any_of(neighbour.mono_pcm.begin(), neighbour.mono_pcm.end(),
                [](float sample) { return std::abs(sample) > 1e-6F; }), "adjacent notes retain audible, finite key-off playback");
    }
}

void run() {
    const auto analysis = fixture();
    const auto originalPcm = analysis->source->mono_samples;
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::OpllPatchSlideOpllPatchSlide;
    options.strategy = CompositeWavStrategy::Independent;
    options.max_scc_waveforms = 0;
    options.max_scc_source_waveforms = 0;
    options.max_evaluations = 8;
    options.preference = CompositeWavPreference::Compact;
    options.key_off_position = analysis->selection.end;
    options.control = std::make_shared<CompositeWavConversionControl>();
    options.control->cancel_requested = true;
    const auto precancel = convertCompositeWave(analysis, options);
    require(precancel.completion == CompositeWavConversionCompletion::Cancelled
        && !precancel.composite_tone && !precancel.evaluations, "pre-cancel performs no DB acquisition or applied candidate work");
    options.control.reset();
    options.patchslide_database_directory = std::filesystem::temp_directory_path()
        / ("mgstc_missing_patchslide_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto missing = convertCompositeWave(analysis, options);
    require(missing.completion == CompositeWavConversionCompletion::NoValidCandidate
        && !missing.composite_tone && missing.error.find("patchslide.sqlite") != std::string::npos,
        "explicit missing DB is an explanatory failure without an applied tone");
    const auto* configured = std::getenv("MGSTC_PATCHSLIDE_TEST_DB_DIR");
    const auto directory = configured ? std::filesystem::path(configured) : PatchSlideDatabase::defaultDirectory();
    if (!configured && !std::filesystem::exists(directory / "patchslide.sqlite")
        && !std::filesystem::exists(directory / "patchslide_search.sqlite") && !std::filesystem::exists(directory / "manifest.json")) {
        std::cout << "PatchSlide WAV DB integration skipped (optional deployment absent); missing/cancel boundaries passed.\n";
        return;
    }
    std::string error;
    const auto db = PatchSlideDatabase::load(directory, &error);
    if (!db) throw std::runtime_error("Configured/installed DB is required for integration: " + error);
    options.patchslide_database_directory = directory;
    options.loop_mode = CompositeWavLoopMode::None;
    std::array<std::vector<EnvelopeEvent>, 2> roleEnvelopes;
    for (const auto configuration : {CompositeWavConfiguration::SccOpllPatchSlide,
             CompositeWavConfiguration::OpllOriginalOpllPatchSlide, CompositeWavConfiguration::OpllPatchSlideOpllPatchSlide}) {
        options.configuration = configuration;
        const bool scc = configurationUsesScc(configuration);
        options.max_scc_waveforms = scc ? 8 : 0;
        options.max_scc_source_waveforms = scc ? 2 : 0;
        options.scc_start_number = scc ? 0 : 255;
        options.scc_wave_method = scc ? CompositeWavSccWaveMethod::Reconstructed : static_cast<CompositeWavSccWaveMethod>(999);
        for (const auto strategy : {CompositeWavStrategy::Automatic, CompositeWavStrategy::FundamentalResidual,
                 CompositeWavStrategy::LowHigh, CompositeWavStrategy::AttackSustain, CompositeWavStrategy::Independent}) {
            options.strategy = strategy;
            const auto result = convertCompositeWave(analysis, options);
            validate(result, configuration, *db, options.max_evaluations);
            if (configuration == CompositeWavConfiguration::OpllPatchSlideOpllPatchSlide) {
                if (strategy == CompositeWavStrategy::FundamentalResidual) roleEnvelopes[0] = result.composite_tone->layers[1].volume_envelope.events;
                if (strategy == CompositeWavStrategy::LowHigh) roleEnvelopes[1] = result.composite_tone->layers[1].volume_envelope.events;
                if (strategy == CompositeWavStrategy::AttackSustain)
                    require(result.composite_tone->layers[1].volume_envelope.events.front().value == 0,
                        "attack/sustain role starts the sustain-side channel silently");
            }
            std::cout << "PatchSlide config=" << unsigned(configuration) << " strategy=" << unsigned(strategy)
                << " eval=" << result.evaluations << " probes=" << result.patchslide_search.preliminary_renders
                << " loss=" << result.best_loss << " seconds=" << result.elapsed_seconds
                << " local_bytes=" << result.patchslide_search.working_set_bytes << '\n';
        }
    }
    require(roleEnvelopes[0] != roleEnvelopes[1], "fundamental/residual and low/high have distinct actual OPLL-only role envelopes");
    options.configuration = CompositeWavConfiguration::OpllPatchSlideOpllPatchSlide;
    options.strategy = CompositeWavStrategy::Automatic;
    options.max_evaluations = 3;
    require(convertCompositeWave(analysis, options).completion == CompositeWavConversionCompletion::InvalidInput,
        "automatic search refuses a budget that cannot cover all four role proposals");
    options.max_evaluations = 8;
    options.key_off_position = analysis->selection.begin + 1;
    const auto tooShort = convertCompositeWave(analysis, options);
    require(tooShort.completion == CompositeWavConversionCompletion::NoValidCandidate && !tooShort.composite_tone,
        "zero eligible candidates before key-off never truncate a DB sequence");
    options.key_off_position = analysis->selection.end;
    options.max_evaluations = 24;
    options.loop_mode = CompositeWavLoopMode::Automatic;
    auto loopAnalysis = std::make_shared<SourceAnalysis>(*analysis);
    loopAnalysis->sustain_region = EstimatedSourceRegion{{2400, 6400}, 1};
    const auto looped = convertCompositeWave(loopAnalysis, options);
    validate(looped, options.configuration, *db, options.max_evaluations);
    require(looped.search.trials[static_cast<std::size_t>(CompositeWavSearchFamily::Loop)] > 0,
        "automatic looping is evaluated with real held-note traversal PCM");
    options.loop_mode = CompositeWavLoopMode::None;
    options.max_evaluations = 4096;
    options.control = std::make_shared<CompositeWavConversionControl>();
    std::jthread canceller([control = options.control](std::stop_token stop) {
        while (!stop.stop_requested() && !control->evaluations.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!stop.stop_requested()) control->cancel_requested = true;
    });
    const auto interrupted = convertCompositeWave(analysis, options);
    canceller.request_stop();
    require(interrupted.completion == CompositeWavConversionCompletion::Cancelled
        && interrupted.evaluations > 0 && !interrupted.composite_tone && !interrupted.preview.ok()
        && interrupted.retained_candidates.empty(),
        "active whole-composite evaluation cancellation exposes no applicable partial list");
    require(analysis->source->mono_samples == originalPcm, "all modes/cancel paths preserve original source audio");
    std::cout << "PatchSlide WAV 3 configurations x 5 strategies, zero SCC, shared chip, DB timings, persistence/export and cancellation passed\n";
}
} // namespace

int main() {
    try { run(); return 0; }
    catch (const std::exception& error) { std::cerr << "PatchSlide WAV regression: " << error.what() << '\n'; return 1; }
}
