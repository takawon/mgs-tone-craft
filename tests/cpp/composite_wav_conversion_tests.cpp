// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_conversion.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>
#include <numbers>
#include <set>
#include <map>
#include <algorithm>
#include <functional>
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
    require(std::isfinite(result.morph_search.adaptive_candidate_seconds)
            && result.morph_search.adaptive_candidate_seconds >= 0
            && std::isfinite(result.morph_search.adaptive_refinement_seconds)
            && result.morph_search.adaptive_refinement_seconds >= 0
            && result.morph_search.adaptive_candidate_seconds
                + result.morph_search.adaptive_refinement_seconds <= result.elapsed_seconds + 1e-3,
        "disjoint adaptive-search timers are finite, nonnegative, and bounded by conversion wall time");
    require(result.resource_plan.scc_waveforms > 0 && result.resource_plan.scc_waveforms <= limit,
            "waveform budget honored");
    const auto bank = compileSccMorph(tone);
    require(bank.valid && bank.used <= limit, "shared SCC allocator accepts output");
    if (tone.layers.size() == 2 && tone.layers[0].source == TimbreSource::Scc
        && tone.layers[1].source == TimbreSource::Scc) {
        auto sharedRamBoundary = tone;
        sharedRamBoundary.layers[0].channel = 3;
        sharedRamBoundary.layers[1].channel = 4;
        const auto sharedRamCompile = compileSccMorph(sharedRamBoundary);
        require(sharedRamCompile.valid && sharedRamCompile.used <= limit,
            "converted dual-SCC timbres remain allocatable when routed through the SCC4/5 shared-RAM boundary");
    }
    require(std::isfinite(result.quality.max_pcm_discontinuity)
            && std::isfinite(result.quality.max_pcm_spectral_change)
            && std::isfinite(result.quality.max_wave_aligned)
            && std::isfinite(result.quality.max_wave_harmonic),
        "local PCM/wave continuity diagnostics remain finite");
    const auto finalizedNumbers = resolveTimbreNumbers(bank.timbre);
    for (const auto& layer : bank.timbre.layers) {
        if (layer.source != TimbreSource::Scc) continue;
        const auto code = formatMgsCompositeEnvelope(layer, layer.envelope_number,
            kMgscEnvelopeCompiledByteLimit, &finalizedNumbers).bytecode;
        std::size_t leadingPatches{}, cursor{};
        while (cursor < code.size()) {
            if (code[cursor] == 0x10 && cursor + 1 < code.size()) { ++leadingPatches; cursor += 2; }
            else if (code[cursor] == 0x40) ++cursor;
            else break;
        }
        require(leadingPatches <= 1,
            "generated SCC candidate consumes time before a second initial patch command");
    }
    const auto saved = CompositeTimbreLibrary::serializeTimbreFile(tone, 0);
    const auto loaded = CompositeTimbreLibrary::deserializeTimbreFile(saved);
    require(loaded && *loaded == tone, "portable file roundtrip");
    const auto payload = serializeCompositeSoundPayload(tone);
    const auto restored = deserializeCompositeSoundPayload(payload, kCompositeSoundPayloadVersion);
    require(restored && serializeCompositeSoundPayload(*restored) == payload, "sound payload roundtrip");
    const auto mgs = formatMgsComposite(tone);
    require(mgs.valid(), "ordinary MGSC export accepts output");
    require(parseMgsComposite(mgs.source).valid(), "MGSC export parses");
    const auto regenerated = compileSccMorph(*loaded);
    require(regenerated.valid && regenerated.plans.size() == bank.plans.size(),
        "portable WAV result regenerates its common MorphPlan set");
    for (std::size_t i = 0; i < bank.plans.size(); ++i) {
        const auto& before = bank.plans[i];
        const auto& after = regenerated.plans[i];
        require(before.layer_index == after.layer_index
                && before.destination_event_index == after.destination_event_index
                && before.result.plan == after.result.plan
                && before.result.generated.intermediate == after.result.generated.intermediate,
            "portable save/load regenerates identical plan counts and quantized SCC waves");
    }
    std::set<std::uint64_t> sccIds;
    for (const auto& layer : tone.layers)
        if (layer.source == TimbreSource::Scc && layer.base_timbre) sccIds.insert(layer.base_timbre->library_id);
    for (const auto& ref : tone.embedded_timbres)
        if (ref.source == TimbreSource::Scc) sccIds.insert(ref.library_id);
    for (const auto& layer : tone.layers)
        if (layer.source == TimbreSource::Opll && layer.base_timbre)
            require(!sccIds.contains(layer.base_timbre->library_id), "OPLL snapshot ID never aliases rebuilt SCC");
    std::set<std::uint64_t> checkedIds;
    std::set<std::array<std::uint8_t, 32>> checkedSccWaves;
    const auto checkSccWave = [&](const SavedTimbreReference& reference) {
        if (reference.source != TimbreSource::Scc || !checkedIds.insert(reference.library_id).second) return;
        require(checkedSccWaves.insert(reference.scc_waveform).second,
            "converter merges byte-identical SCC snapshots instead of spending duplicate bank slots");
    };
    for (const auto& layer : tone.layers)
        if (layer.base_timbre) checkSccWave(*layer.base_timbre);
    for (const auto& reference : tone.embedded_timbres) checkSccWave(reference);
    for (const auto& layer : tone.layers) for (const auto& event : layer.timbre_automation) {
        if (!event.scc_morph.enabled) continue;
        require(event.scc_morph.explicit_plan.has_value()
                && isValidSccMorphPlan(*event.scc_morph.explicit_plan,
                    event.scc_morph.intermediate_count),
            "WAV-selected morph interval persists a valid confirmed plan");
    }
}

constexpr std::array<SccMorphDistributionMode, 3> kWavMorphModes{
    SccMorphDistributionMode::AdaptiveDistribution,
    SccMorphDistributionMode::TimeDistribution,
    SccMorphDistributionMode::ToneDistribution,
};

bool isExpectedMorphConstraintRejection(const std::string& reason) {
    return reason.find("intermediate count exceeds available time slots") != std::string::npos
        || reason.find("SCC Bank requirement exceeds") != std::string::npos
        || reason.find("SCC Morph Bank overflow") != std::string::npos
        || reason.find("SCC global output capacity exceeded") != std::string::npos
        || reason.find("Wave reuse/reduction worsens maximum local PCM continuity") != std::string::npos
        || reason.find("Approximate wave reuse must reduce") != std::string::npos
        || reason.find("Dynamic OPLL contour does not improve") != std::string::npos;
}

bool validateMatchedMorphComparisons(const CompositeWavConversionResult& result,
    CompositeWavConfiguration configuration) {
    std::map<std::uint64_t, std::vector<const CompositeWavMorphCandidateDiagnostic*>> groups;
    for (const auto& candidate : result.morph_candidates) {
        require(candidate.configuration == configuration,
            "morph candidate reports the complete composite configuration");
        if (result.completion == CompositeWavConversionCompletion::Completed
            && candidate.comparison_group != 0)
            require(candidate.comparison_complete,
                "completed conversion cannot leave a partial three-mode group");
        if (candidate.selected)
            require(candidate.valid && candidate.accepted,
                "only a valid accepted whole-composite candidate may enter the final result");
        if (candidate.comparison_group != 0 && candidate.comparison_complete)
            groups[candidate.comparison_group].push_back(&candidate);
        else if (candidate.comparison_group != 0)
            require(!candidate.group_winner,
                "incomplete mode comparisons cannot claim a winner");
    }
    require(!groups.empty(), "sufficient WAV search budget forms a complete three-mode comparison");
    require(result.morph_search.complete_comparisons >= groups.size(),
        "comparison diagnostics include every retained matched group");
    bool hasPlayableModeCandidate{};
    for (const auto& [group, candidates] : groups) {
        (void)group;
        require(candidates.size() == kWavMorphModes.size(),
            "a matched comparison contains one candidate for each distribution mode");
        const CompositeWavMorphCandidateDiagnostic* byMode[kWavMorphModes.size()]{};
        for (const auto* candidate : candidates) {
            require(candidate->comparison_complete,
                "all members of an exposed triplet have a complete comparison");
            const auto index = static_cast<std::size_t>(candidate->distribution);
            require(index < kWavMorphModes.size() && byMode[index] == nullptr,
                "matched triplet uses each distribution exactly once");
            byMode[index] = candidate;
        }
        require(std::all_of(std::begin(byMode), std::end(byMode), [](const auto* item) { return item != nullptr; }),
            "matched triplet does not omit a distribution");

        const auto& reference = *byMode[0];
        for (const auto* candidate : byMode) {
            require(candidate->layer_index == reference.layer_index
                    && candidate->destination_event_index == reference.destination_event_index
                    && candidate->start_count == reference.start_count
                    && candidate->end_count == reference.end_count
                    && candidate->intermediate_count == reference.intermediate_count
                    && candidate->curve == reference.curve,
                "mode comparison holds interval, keyframes, count, and curve fixed");
        }
        std::vector<const CompositeWavMorphCandidateDiagnostic*> valid;
        for (const auto* candidate : byMode) {
            if (candidate->valid) {
                require(candidate->quality.total >= 0 && std::isfinite(candidate->quality.total),
                    "valid mode candidates have finite full-composite PCM scores");
                valid.push_back(candidate);
            } else {
                require(!candidate->group_winner && !candidate->accepted && !candidate->selected
                        && isExpectedMorphConstraintRejection(candidate->reason),
                    "rejected mode candidate has a reason and cannot win or enter the final composite");
            }
        }
        if (valid.empty()) {
            for (const auto* candidate : byMode) {
                require(isExpectedMorphConstraintRejection(candidate->reason),
                    "all-invalid matched triplet is rejected by the common time-slot or SCC Bank constraint");
            }
            continue;
        }
        for (const auto* candidate : valid) {
            const auto mode = static_cast<std::size_t>(candidate->distribution);
            require(mode < result.morph_search.mode_trials.size()
                    && result.morph_search.mode_trials[mode] > 0,
                "each playable mode candidate is backed by an actual Engine PCM trial");
        }
        require(result.morph_search.candidate_pool_peak >= valid.size(),
            "candidate-pool diagnostics retain each feasible member of a playable comparison");
        hasPlayableModeCandidate = true;
        const auto minimumScore = std::min_element(valid.begin(), valid.end(),
            [](const auto* lhs, const auto* rhs) { return lhs->quality.total < rhs->quality.total; });
        const auto winner = std::find_if(valid.begin(), valid.end(), [&](const auto* candidate) {
            return candidate->quality.total <= (*minimumScore)->quality.total + 1e-9;
        });
        require((*winner)->group_winner,
            "WAV group winner is the lowest actual full-composite score with stable mode tie-break");
        for (const auto* candidate : valid)
            if (candidate != *winner)
                require(!candidate->group_winner,
                    "only the best valid member of a completed triplet is marked its winner");
    }
    return hasPlayableModeCandidate;
}

bool hasStrictNonAdaptiveWinner(const CompositeWavConversionResult& result) {
    for (const auto& adaptive : result.morph_candidates) {
        if (!adaptive.valid || adaptive.distribution != SccMorphDistributionMode::AdaptiveDistribution
            || !adaptive.comparison_complete || adaptive.comparison_group == 0) continue;
        const auto winner = std::find_if(result.morph_candidates.begin(), result.morph_candidates.end(),
            [&](const auto& candidate) {
                return candidate.comparison_group == adaptive.comparison_group && candidate.group_winner;
            });
        if (winner != result.morph_candidates.end() && winner->valid
            && winner->distribution != SccMorphDistributionMode::AdaptiveDistribution
            && winner->quality.total + 1e-9 < adaptive.quality.total)
            return true;
    }
    return false;
}

bool hasAdaptiveTieWinner(const CompositeWavConversionResult& result) {
    for (const auto& adaptive : result.morph_candidates) {
        if (!adaptive.valid || adaptive.distribution != SccMorphDistributionMode::AdaptiveDistribution
            || !adaptive.comparison_complete || adaptive.comparison_group == 0) continue;
        std::array<const CompositeWavMorphCandidateDiagnostic*, 3> modes{};
        for (const auto& candidate : result.morph_candidates) {
            if (candidate.comparison_group == adaptive.comparison_group && candidate.valid)
                modes[static_cast<std::size_t>(candidate.distribution)] = &candidate;
        }
        if (std::any_of(modes.begin(), modes.end(), [](const auto* candidate) { return candidate == nullptr; }))
            continue;
        double low = modes.front()->quality.total, high = low;
        for (const auto* candidate : modes) {
            low = std::min(low, candidate->quality.total);
            high = std::max(high, candidate->quality.total);
        }
        if (high - low <= 1e-9 && adaptive.group_winner) return true;
    }
    return false;
}

void printMorphComparison(const char* label, const CompositeWavConversionResult& result) {
    std::printf("%s: %.6f s, %zu bytes working set, cache morph %zu/%zu, render %zu/%zu\n",
        label, result.elapsed_seconds, result.morph_search.working_set_bytes,
        result.morph_search.morph_cache_hits, result.morph_search.morph_cache_misses,
        result.morph_search.render_cache_hits, result.morph_search.render_cache_misses);
    for (const auto& candidate : result.morph_candidates) {
        if (!candidate.comparison_complete || candidate.comparison_group == 0) continue;
        std::printf("  group %llu mode %u %s",
            static_cast<unsigned long long>(candidate.comparison_group),
            static_cast<unsigned>(candidate.distribution), candidate.valid ? "score" : "rejected");
        if (candidate.valid)
            std::printf(" %.8f bank %zu", candidate.quality.total, candidate.bank_waveforms);
        else
            std::printf(": %s", candidate.reason.c_str());
        std::printf("%s%s\n", candidate.group_winner ? " winner" : "",
            candidate.selected ? " selected" : "");
    }
    std::printf("  final loss %.8f; planning %.6f s, PCM %.6f s; global combinations %zu/%zu, retained pools %zu\n",
        result.best_loss, result.morph_search.planning_seconds, result.morph_search.evaluation_seconds,
        result.morph_search.global_combination_accepted, result.morph_search.global_combination_trials,
        result.morph_search.retained_segment_pools);
    std::printf("  adaptive candidate %.6f s, adaptive refinement %.6f s\n",
        result.morph_search.adaptive_candidate_seconds,
        result.morph_search.adaptive_refinement_seconds);
    if (result.composite_tone) for (std::size_t layerIndex = 0;
        layerIndex < result.composite_tone->layers.size(); ++layerIndex) {
        const auto& layer = result.composite_tone->layers[layerIndex];
        if (layer.source != TimbreSource::Scc) continue;
        for (std::size_t eventIndex = 0; eventIndex < layer.timbre_automation.size(); ++eventIndex) {
            const auto& event = layer.timbre_automation[eventIndex];
            if (event.kind != EnvelopeEventKind::Timbre || !event.scc_morph.enabled) continue;
            std::printf("  final layer %zu event %zu mode %u N %u curve %.6g\n", layerIndex, eventIndex,
                static_cast<unsigned>(event.scc_morph.distribution_mode),
                event.scc_morph.intermediate_count, event.scc_morph.curve);
        }
    }
}

void requireSameMorphDecision(const CompositeWavConversionResult& first,
    const CompositeWavConversionResult& second) {
    require(first.morph_candidates.size() == second.morph_candidates.size(),
        "repeat conversion retains the same number of morph alternatives");
    for (std::size_t i = 0; i < first.morph_candidates.size(); ++i) {
        const auto& a = first.morph_candidates[i];
        const auto& b = second.morph_candidates[i];
        require(a.comparison_group == b.comparison_group
                && a.distribution == b.distribution
                && a.plan == b.plan
                && a.valid == b.valid
                && a.group_winner == b.group_winner
                && a.selected == b.selected
                && a.quality.total == b.quality.total,
            "repeat WAV search produces deterministic mode scores, tie-breaks, and final choice");
    }
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

enum class MorphCorpus { FrontFast, Uniform, BackFast, Abrupt, Gradual, LongSustain,
    VolumeChange, DcOffset, SilenceGap, ShortWav };

const char* corpusName(MorphCorpus corpus) {
    switch (corpus) {
    case MorphCorpus::FrontFast: return "front-fast";
    case MorphCorpus::Uniform: return "uniform";
    case MorphCorpus::BackFast: return "back-fast";
    case MorphCorpus::Abrupt: return "abrupt-harmonic";
    case MorphCorpus::Gradual: return "gradual-harmonic";
    case MorphCorpus::LongSustain: return "long-sustain";
    case MorphCorpus::VolumeChange: return "volume-change";
    case MorphCorpus::DcOffset: return "dc-offset";
    case MorphCorpus::SilenceGap: return "silence-gap";
    case MorphCorpus::ShortWav: return "short-wav";
    }
    return "unknown";
}

std::shared_ptr<const SourceAnalysis> comparisonCorpus(MorphCorpus corpus) {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48000;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    const std::size_t frames = corpus == MorphCorpus::LongSustain ? 28800
        : corpus == MorphCorpus::ShortWav ? 3840 : 11520;
    double phase{};
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / 48000;
        const double duration = static_cast<double>(frames) / 48000;
        const double progress = std::clamp((t - .018) / (duration * .78), 0.0, 1.0);
        double morph = progress;
        if (corpus == MorphCorpus::FrontFast) morph = std::sqrt(progress);
        if (corpus == MorphCorpus::BackFast) morph = progress * progress;
        if (corpus == MorphCorpus::Abrupt) morph = progress < .48 ? 0 : 1;
        if (corpus == MorphCorpus::Gradual) morph = progress * progress * (3 - 2 * progress);
        const double frequency = 261.625565 * (1 + .006 * std::sin(2 * std::numbers::pi * 4.5 * t));
        phase += 2 * std::numbers::pi * frequency / 48000;
        double amplitude = std::min(1.0, t / .012) * .28;
        if (corpus == MorphCorpus::VolumeChange)
            amplitude *= .22 + .78 * std::clamp((t - .045) / (duration * .58), 0.0, 1.0);
        if (corpus == MorphCorpus::SilenceGap && t >= duration * .43 && t < duration * .57)
            amplitude = 0;
        const double dc = corpus == MorphCorpus::DcOffset ? .12 : 0.0;
        const auto value = static_cast<float>(dc + amplitude * (
            .8 * std::sin(phase) + .62 * (1 - morph) * std::sin(2 * phase + .2)
            + .58 * morph * std::sin(5 * phase - .3)));
        pcm->mono_samples.push_back(value);
        pcm->interleaved_samples.push_back(value);
    }
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    auto analyzed = analyzeCompositeWaveSource(pcm, {0, frames}, options);
    require(analyzed.completion == SourceAnalysisCompletion::Completed,
        "generated mode-comparison corpus analyzes");
    auto result = std::make_shared<SourceAnalysis>(*analyzed.analysis);
    if (corpus == MorphCorpus::LongSustain)
        result->sustain_region = EstimatedSourceRegion{{9600, 24000}, 1};
    return result;
}

void corpusModeRegressions() {
    constexpr std::array corpus{
        MorphCorpus::FrontFast, MorphCorpus::Uniform, MorphCorpus::BackFast,
        MorphCorpus::Abrupt, MorphCorpus::Gradual, MorphCorpus::LongSustain,
        MorphCorpus::VolumeChange, MorphCorpus::DcOffset, MorphCorpus::SilenceGap,
        MorphCorpus::ShortWav,
    };
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::Scc;
    options.max_scc_waveforms = 8;
    options.max_evaluations = 4; // seed plus one complete, fair three-mode comparison
    options.loop_mode = CompositeWavLoopMode::Automatic;
    bool observedAdaptiveTie{};
    bool corpusHasPlayableComparison{};
    for (const auto shape : corpus) {
        const auto result = convertCompositeWave(comparisonCorpus(shape), options);
        validate(result, 1, options.max_scc_waveforms);
        corpusHasPlayableComparison = validateMatchedMorphComparisons(result, options.configuration)
            || corpusHasPlayableComparison;
        if (shape == MorphCorpus::LongSustain) {
            const auto& layer = result.composite_tone->layers.front();
            require(layer.envelope_timeline.loop_start_count && layer.envelope_timeline.loop_end_count
                    && *layer.envelope_timeline.loop_end_count > *layer.envelope_timeline.loop_start_count,
                "long-sustain corpus carries the selected loop boundaries through WAV conversion");
        }
        for (const auto& candidate : result.morph_candidates) {
            if (candidate.comparison_group == 0 || !candidate.comparison_complete) continue;
            if (candidate.valid)
                require(std::isfinite(candidate.curve) && candidate.curve >= kSccMorphGammaMin
                        && candidate.curve <= kSccMorphGammaMax
                        && isValidSccMorphPlan(candidate.plan, candidate.intermediate_count),
                    "playable corpus candidate yields a valid bounded curve and playback MorphPlan");
            else
                require(isExpectedMorphConstraintRejection(candidate.reason),
                    "infeasible corpus candidate records the common planner or Bank constraint");
        }
        observedAdaptiveTie = hasAdaptiveTieWinner(result) || observedAdaptiveTie;
        printMorphComparison(corpusName(shape), result);
        std::printf("  budget %zu, evaluations %zu, pool %zu, planning %.6f s, PCM %.6f s\n",
            options.max_evaluations, result.evaluations, result.morph_search.candidate_pool_peak,
            result.morph_search.planning_seconds, result.morph_search.evaluation_seconds);
    }
    require(corpusHasPlayableComparison,
        "generated WAV corpus includes at least one feasible complete mode comparison");
    require(observedAdaptiveTie,
        "short attack-only corpus tie deterministically prefers Adaptive when all three measured scores are equal");
}

void evolvingCorpusModeRegressions() {
    constexpr std::array shapes{MorphCorpus::FrontFast, MorphCorpus::Uniform, MorphCorpus::BackFast};
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::Scc;
    options.max_scc_waveforms = 8;
    options.max_evaluations = 48;
    options.stagnation_limit = 0;
    options.loop_mode = CompositeWavLoopMode::None;
    for (const auto shape : shapes) {
        const auto result = convertCompositeWave(comparisonCorpus(shape), options);
        validate(result, 1, options.max_scc_waveforms);
        require(validateMatchedMorphComparisons(result, options.configuration),
            "extended shape corpus has at least one playable full-composite mode comparison");
        bool hasEvolvingTriplet{};
        for (const auto& candidate : result.morph_candidates) {
            if (!candidate.valid || !candidate.comparison_complete || candidate.comparison_group == 0
                || candidate.intermediate_count < 1) continue;
            require(std::isfinite(candidate.curve) && candidate.curve >= kSccMorphGammaMin
                    && candidate.curve <= kSccMorphGammaMax
                    && isValidSccMorphPlan(candidate.plan, candidate.intermediate_count),
                "evolving feasible candidate carries a valid common-engine plan and curve");
            const auto sameGroupValidModes = std::count_if(result.morph_candidates.begin(),
                result.morph_candidates.end(), [&](const auto& other) {
                    return other.comparison_group == candidate.comparison_group
                        && other.valid && other.comparison_complete
                        && other.intermediate_count >= 1;
                });
            if (sameGroupValidModes == static_cast<std::ptrdiff_t>(kWavMorphModes.size()))
                hasEvolvingTriplet = true;
        }
        require(hasEvolvingTriplet,
            "front/uniform/back corpus reaches a valid N>=1 triplet rendered by the real Engine");
        require(result.evaluations <= options.max_evaluations,
            "extended evolving-mode exploration honors the finite evaluation budget");
        printMorphComparison(corpusName(shape), result);
        if (shape == MorphCorpus::FrontFast)
            require(hasStrictNonAdaptiveWinner(result),
                "front-fast actual Engine triplet strictly selects Time or Tone when it beats Adaptive");
        std::printf("  evolving budget %zu, evaluations %zu\n", options.max_evaluations, result.evaluations);
    }
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
    require(validateMatchedMorphComparisons(optimized, options.configuration),
        "160-evaluation temporal regression contains a playable completed mode comparison");
    printMorphComparison("dual-SCC temporal comparison", optimized);
    std::printf("Temporal WAV objective %.8f -> %.8f; evaluations %zu, loop probes %zu\n",
        optimized.initial_loss, optimized.best_loss, optimized.evaluations, optimized.loop_probe_renders);
    std::printf("Temporal WAV benchmark: %.6f s, %zu SCC bank waves\n",
        optimized.elapsed_seconds, optimized.resource_plan.scc_waveforms);
    for (std::size_t family = 0; family < optimized.search.trials.size(); ++family)
        std::printf("Coordinate family %zu: %zu rendered, %zu accepted\n", family,
            optimized.search.trials[family], optimized.search.accepted[family]);
    require(optimized.evaluations <= options.max_evaluations, "joint coordinates stay within render budget");
    require(optimized.loop_probe_renders > 0 && optimized.loop_probe_renders <= 2 * optimized.evaluations,
        "short sustain uses at most two bounded held/finite loop probes per candidate");
    for (std::size_t family = 0; family < optimized.search.trials.size(); ++family) {
        const bool hasLoop = std::any_of(optimized.composite_tone->layers.begin(), optimized.composite_tone->layers.end(),
            [](const auto& layer) { return layer.envelope_timeline.loop_start_count.has_value(); });
        if (family == static_cast<std::size_t>(CompositeWavSearchFamily::Loop) && !hasLoop) continue;
        require(optimized.search.trials[family] > 0, "every applicable coordinate family reaches real Engine PCM");
    }
    require(optimized.best_loss + 1e-5 < optimized.initial_loss, "temporal corpus improves measured full-source objective");
    require(optimized.morph_search.same_wave_trials > 0 && optimized.morph_search.detune_trials > 0
            && optimized.morph_search.near_wave_trials > 0 && optimized.morph_search.mixed_role_trials > 0,
        "dual SCC compares independent, shared, detuned, nearby and mixed-role real Engine seeds");
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
    validateMatchedMorphComparisons(noisy, options.configuration);
    printMorphComparison("gliding noisy comparison", noisy);
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

void localDiscontinuityRegressions() {
    std::vector<float> source(24000);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float>(.2 * std::sin(2 * std::numbers::pi * 220 * i / 48000));
    const auto measured = fixture();
    auto movingPitch = std::make_shared<SourceAnalysis>(*measured);
    for (auto& frame : movingPitch->pitch_trajectory) {
        frame.frequency_hz = movingPitch->reference_pitch_hz
            * std::pow(2.0, 8.0 / 1200.0) * (1 + .01 * std::sin(frame.sample_position / 1200.0));
        frame.confidence = 1;
    }
    const auto nominalNote = std::round(69 + 12 * std::log2(movingPitch->reference_pitch_hz / 440));
    const double nominalFrequency = 440 * std::pow(2.0, (nominalNote - 69) / 12.0);
    const auto pitchIdentity = evaluateCompositeWavePcm(movingPitch->source->mono_samples,
        movingPitch->source->mono_samples, nominalFrequency, 720, 0, {}, nullptr, movingPitch.get());
    require(pitchIdentity.total < 1e-9,
        "main acoustic identity preserves measured source cents and glides instead of warping its target FFT");
    auto spike = source;
    spike[12000] += .7F;
    const auto identity = evaluateCompositeWavePcm(source, source, 220, 720);
    const auto tinyFrequency = evaluateCompositeWavePcm(source, source, 1e-310, 720);
    require(std::isfinite(tinyFrequency.total) && tinyFrequency.total < 1e-9,
        "tiny positive public metric frequency remains finite with a bounded local period");
    const auto discontinuous = evaluateCompositeWavePcm(source, spike, 220, 720);
    require(identity.max_pcm_discontinuity < 1e-9 && identity.max_pcm_spectral_change < 1e-9,
        "identity has zero local discontinuity");
    require(discontinuous.max_pcm_discontinuity > .1 && discontinuous.total > identity.total,
        "single local spike is exposed independently of whole-WAV mean loss");
    auto tone = defaultCompositeTimbre();
    auto layer = tone.layers.at(1);
    seedDefaultLayerTimbre(tone, layer);
    tone.layers = {layer};
    tone.embedded_timbres.clear();
    auto& scc = tone.layers.front();
    require(scc.base_timbre.has_value(), "actual switch fixture owns a valid SCC snapshot");
    scc.channel = 0;
    scc.timbre_automation.clear();
    scc.envelope_timeline.length_counts = 18;
    for (std::size_t i = 0; i < 32; ++i)
        scc.base_timbre->scc_waveform[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(
            std::lround(110 * std::sin(2 * std::numbers::pi * i / 32))));
    auto switched = tone;
    auto pulse = *switched.layers.front().base_timbre;
    pulse.library_id = allocateCompositeOwnedTimbreId(switched);
    for (std::size_t i = 0; i < 32; ++i)
        pulse.scc_waveform[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i < 8 ? 127 : -42));
    switched.embedded_timbres.push_back(pulse);
    EnvelopeEvent change;
    change.kind = EnvelopeEventKind::Timbre;
    change.count = 5;
    change.target_library_id = pulse.library_id;
    switched.layers.front().timbre_automation.push_back(change);
    CompositeWavRenderOptions render;
    render.frame_count = 14400;
    render.key_off_frame = 14400;
    const auto held = renderCompositeWav(tone, render), changed = renderCompositeWav(switched, render);
    require(held.ok() && changed.ok(), "actual waveform-switch fixtures render through real Engine");
    const auto switchQuality = evaluateCompositeWavePcm(held.mono_pcm, changed.mono_pcm, 261.625565, 720);
    require(switchQuality.max_pcm_discontinuity > .1 || switchQuality.max_pcm_spectral_change > .05,
        "local derivative floor preserves detection of an actual abrupt SCC waveform switch");
}

void waveformMethodRegressions() {
    const auto analysis = fixture();
    CompositeWavConversionOptions options;
    options.max_evaluations = 12;
    options.stagnation_limit = 0;
    options.max_scc_waveforms = 8;
    options.loop_mode = CompositeWavLoopMode::None;
    const auto reconstructed = convertCompositeWave(analysis, options);
    options.scc_wave_method = CompositeWavSccWaveMethod::DirectPeriodic;
    const auto direct = convertCompositeWave(analysis, options);
    validate(direct, 1, 8);
    require(direct.morph_search.direct_extraction_seconds > 0
            && direct.morph_search.source_wave_cache_misses > 0,
        "direct method executes local period/boundary/phase extraction through shared SCC quantizer");
    require(direct.composite_tone != reconstructed.composite_tone,
        "direct extraction provides a genuinely distinct useful candidate family");
    require(direct.morph_search.source_spectrum_cache_hits > 0
            && direct.morph_search.source_cache_bytes <= 9 * 1024 * 1024,
        "exact source FFT/wave caches reuse immutable features with finite storage");
    require(std::any_of(direct.composite_tone->layers.front().timbre_automation.begin(),
            direct.composite_tone->layers.front().timbre_automation.end(),
            [](const auto& event) { return event.scc_morph.enabled; }),
        "direct keyframes still use ordinary common Morph intervals");
    const auto again = convertCompositeWave(analysis, options);
    require(direct.composite_tone == again.composite_tone && direct.preview.mono_pcm == again.preview.mono_pcm,
        "direct source caches preserve repeatable byte waves, plans and real Engine PCM");
    options.scc_wave_method = static_cast<CompositeWavSccWaveMethod>(9);
    require(convertCompositeWave(analysis, options).completion == CompositeWavConversionCompletion::InvalidInput,
        "invalid waveform method is rejected before candidate work");
}

void latentZeroWaveRegressions() {
    auto analysis = std::make_shared<SourceAnalysis>(*fixture());
    auto pcm = std::make_shared<SourcePcm>(*analysis->source);
    // A valid DC source has audible energy but no coherent periodic wave. The
    // inverse level estimate must saturate before conversion to Windows long.
    std::fill(pcm->mono_samples.begin(), pcm->mono_samples.end(), .5F);
    std::fill(pcm->interleaved_samples.begin(), pcm->interleaved_samples.end(), .5F);
    analysis->source = pcm;
    analysis->analysis_mono_override.clear();
    analysis->periodic_rms = 0;
    analysis->periodic_confidence = 0;
    for (auto& frame : analysis->harmonic_trajectory) {
        for (auto& harmonic : frame.harmonics) harmonic.amplitude = 0;
        frame.periodic_rms = 0;
        frame.periodic_confidence = 0;
        frame.residual_rms = .5;
    }
    for (auto& frame : analysis->amplitude_envelope) frame.rms = frame.peak = .5;
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::SccScc;
    options.strategy = CompositeWavStrategy::Independent;
    options.loop_mode = CompositeWavLoopMode::None;
    options.key_off_position = analysis->selection.end;
    options.max_evaluations = 48;
    options.stagnation_limit = 0;
    const auto converted = convertCompositeWave(analysis, options);
    validate(converted, 2, options.max_scc_waveforms);
    require(converted.morph_search.same_wave_trials >= 2,
        "zero periodic wave reaches bounded latent two-channel level calibration");
    for (const auto& layer : converted.composite_tone->layers)
        for (const auto& event : layer.volume_envelope.events)
            if (event.kind == EnvelopeEventKind::Volume)
                require(event.value >= 0 && event.value <= 15,
                    "zero-wave inverse level calibration retains representable SCC volume");
}

void countGridRegressions() {
    CompositeWavConversionOptions options;
    options.max_evaluations = 96;
    options.stagnation_limit = 0;
    options.max_scc_waveforms = 16;
    options.loop_mode = CompositeWavLoopMode::None;
    const auto result = convertCompositeWave(temporalFixture(false, false), options);
    validate(result, 1, 16);
    std::set<unsigned> tried;
    for (const auto& diagnostic : result.morph_candidates)
        if (diagnostic.comparison_complete) tried.insert(diagnostic.intermediate_count);
    for (unsigned n : {0, 1, 2, 4, 8, 12}) require(tried.contains(n),
        "bounded diagnostic search compares N=0,1,2,4,8,12 without a fixed minimum N");
    require(std::any_of(tried.begin(), tried.end(), [](unsigned n) { return n >= 12; }),
        "diagnostic sweep reaches a waveform/time-capacity-near-maximum proposal");
    require(result.evaluations <= options.max_evaluations, "N grid preserves complete-candidate evaluation cap");
    require(result.quality.complexity >= 0 && std::isfinite(result.best_loss),
        "resource complexity is separate from acoustic fidelity objective");
    std::printf("N grid selected %zu unique waves, aligned %.6f harmonic %.6f PCM local %.6f\n",
        result.resource_plan.scc_waveforms, result.quality.max_wave_aligned,
        result.quality.max_wave_harmonic, result.quality.max_pcm_discontinuity);
}

std::shared_ptr<const SourceAnalysis> dynamicOpllFixture(bool feedback) {
    CompositeTimbre source;
    CompositeLayer opll;
    opll.source = TimbreSource::Opll;
    opll.envelope_timeline.length_counts = 24;
    SavedTimbreReference patch;
    patch.library_id = 1;
    patch.source = TimbreSource::Opll;
    auto parameters = defaultOpllPatch();
    parameters.modulator.total_level = 12;
    parameters.feedback = feedback ? 0 : 5;
    parameters.modulator.attack_rate = 15;
    parameters.carrier.attack_rate = 15;
    patch.opll_registers = encodeOpllPatch(parameters);
    opll.base_timbre = patch;
    auto& lane = feedback ? opll.opll_fb_auto : opll.opll_tl_auto;
    lane.mode = OpllRegisterAutoMode::FreeCurve;
    lane.start_count = 0;
    lane.change_speed = 1;
    lane.coarseness = 3;
    lane.free_curve = feedback ? std::vector<std::uint8_t>{0, 0, 2, 4, 6, 7, 4, 1}
        : std::vector<std::uint8_t>{12, 12, 20, 28, 36, 44, 36, 24};
    source.layers.push_back(opll);
    CompositeWavRenderOptions render;
    render.frame_count = 19200;
    render.key_off_frame = 19200;
    const auto audible = renderCompositeWav(source, render);
    require(audible.ok(), "known TL contour generates fixture through real Engine");
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48000; pcm->channels = 1; pcm->bit_depth = 32;
    pcm->mono_samples = audible.mono_pcm;
    pcm->interleaved_samples = audible.mono_pcm;
    SourceAnalysisOptions options;
    options.reference_pitch_hz = 261.625565;
    // Calibrate this known-engine fixture to the public evaluator's documented
    // SCC/OPLL diagnostic cap, so both counterfactuals use identical support.
    options.maximum_harmonics = 15;
    const auto analyzed = analyzeCompositeWaveSource(pcm, {0, pcm->mono_samples.size()}, options);
    require(analyzed.analysis != nullptr, "known TL contour analysis");
    return analyzed.analysis;
}

void dynamicOpllRegressions(bool feedback) {
    CompositeWavConversionOptions options;
    options.configuration = CompositeWavConfiguration::SccOpllOriginal;
    options.strategy = CompositeWavStrategy::Independent;
    options.loop_mode = CompositeWavLoopMode::None;
    options.max_scc_waveforms = 8;
    options.max_evaluations = 96;
    options.stagnation_limit = 0;
    {
        const auto analysis = dynamicOpllFixture(feedback);
        // The fixture's actual Engine key is held to the selection end. A TL
        // fall is intentional color evolution rather than a gate estimate.
        options.key_off_position = analysis->selection.end;
        const auto result = convertCompositeWave(analysis, options);
        validate(result, 2, 8);
        std::printf("Known OPLL %s proposal diagnostics: trials %zu accepted %zu evaluations %zu loss %.8f keyoff %zu\n",
            feedback ? "FB" : "TL", result.morph_search.dynamic_opll_trials,
            result.morph_search.dynamic_opll_accepted, result.evaluations, result.best_loss,
            result.preview.effective_key_off_frame);
        for (const auto& layer : result.composite_tone->layers) if (layer.source == TimbreSource::Opll) {
            std::printf("  retained OPLL volume %u, patch", static_cast<unsigned>(layer.volume));
            if (layer.base_timbre) for (const auto value : layer.base_timbre->opll_registers)
                std::printf(" %02x", static_cast<unsigned>(value));
            std::printf("; ENV");
            for (const auto& event : layer.volume_envelope.events)
                std::printf(" %u:%d%s", event.count, event.value, event.automatic ? "a" : "");
            std::printf("\n");
        }
        require(result.morph_search.dynamic_opll_trials > 0
                && result.morph_search.dynamic_opll_accepted > 0,
            "known TL and FB fixtures must adopt a spectral contour through combined Engine evaluation");
        bool dynamic{};
        for (const auto& layer : result.composite_tone->layers) if (layer.source == TimbreSource::Opll) {
            for (const auto* lane : {&layer.opll_tl_auto, &layer.opll_fb_auto})
                dynamic = dynamic || (lane->mode == OpllRegisterAutoMode::FreeCurve
                    && std::adjacent_find(lane->free_curve.begin(), lane->free_curve.end(),
                        std::not_equal_to<>{}) != lane->free_curve.end());
        }
        require(dynamic, "adopted dynamic contour remains in final ordinary CompositeTone");
        const auto source = formatMgsComposite(*result.composite_tone);
        require(source.source.find("y2,") != std::string::npos || source.source.find("y3,") != std::string::npos,
            "accepted original contour survives CompositeTone into MGSC y2/y3 commands");
        auto constant = *result.composite_tone;
        for (auto& layer : constant.layers) {
            layer.opll_tl_auto = {};
            layer.opll_fb_auto = {};
        }
        CompositeWavRenderOptions render;
        render.frame_count = result.preview.mono_pcm.size();
        render.key_off_frame = result.preview.effective_key_off_frame;
        const auto constantPcm = renderCompositeWav(constant, render);
        require(constantPcm.ok(), "constant counterfactual renders through real Engine");
        CompositeWavQualityWeights weights;
        weights.complexity = 0;
        const auto attackFrame = static_cast<std::size_t>((analysis->attack_region.selection.end
            - analysis->selection.begin) * 48000.0 / analysis->source->sample_rate);
        const auto dynamicQuality = evaluateCompositeWavePcm(analysis->source->mono_samples,
            result.preview.mono_pcm, 261.625565, attackFrame, 0, weights, nullptr, analysis.get());
        const auto constantQuality = evaluateCompositeWavePcm(analysis->source->mono_samples,
            constantPcm.mono_pcm, 261.625565, attackFrame, 0, weights, nullptr, analysis.get());
        std::printf("Known OPLL %s fixture: dynamic trials %zu, accepted %zu, acoustic %.8f vs constant %.8f\n",
            feedback ? "FB" : "TL", result.morph_search.dynamic_opll_trials,
            result.morph_search.dynamic_opll_accepted, dynamicQuality.total, constantQuality.total);
        require(dynamicQuality.total + 1e-7 < constantQuality.total,
            "final dynamic contour improves actual combined PCM over identical constant original");
    }
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
    validateMatchedMorphComparisons(first, options.configuration);
    // This generated 0.2-second waveform has an evolving interval with one
    // intermediate. Its measured complete triplet is a strict Tone winner;
    // keep this as the regression against a fixed Adaptive bonus.
    printMorphComparison("SCC comparison", first);
    // The local-max objective legitimately makes Adaptive win this historical
    // fixture. A measured strict Time winner is asserted in the FrontFast corpus.
    const auto again = convertCompositeWave(analysis, options);
    require(first.composite_tone == again.composite_tone && first.preview.mono_pcm == again.preview.mono_pcm,
            "repeat conversion is deterministic with fresh runtime");
    requireSameMorphDecision(first, again);
    options.max_evaluations = 4;
    for (const auto strategy : {CompositeWavStrategy::Automatic, CompositeWavStrategy::FundamentalResidual,
            CompositeWavStrategy::LowHigh, CompositeWavStrategy::AttackSustain, CompositeWavStrategy::Independent}) {
        options.configuration = CompositeWavConfiguration::SccScc;
        options.strategy = strategy;
        // Preserve the four-evaluation strategy smoke cases. The separate
        // explicit-Independent fairness check needs room for a seed plus the
        // complete Adaptive/Time/Tone triplet.
        options.max_evaluations = strategy == CompositeWavStrategy::Independent ? 12 : 4;
        const auto dual = convertCompositeWave(analysis, options);
        validate(dual, 2, 8);
        if (strategy == CompositeWavStrategy::Independent) {
            validateMatchedMorphComparisons(dual, options.configuration);
            printMorphComparison("independent dual-SCC comparison", dual);
        }
    }
    options.configuration = CompositeWavConfiguration::SccOpllRom;
    options.strategy = CompositeWavStrategy::Automatic;
    options.max_evaluations = 12; // Four source-allocation seeds plus a fair mode triplet.
    options.fixed_opll_tone = 7;
    const auto rom = convertCompositeWave(analysis, options);
    validate(rom, 2, 8);
    validateMatchedMorphComparisons(rom, options.configuration);
    printMorphComparison("fixed ROM SCC+OPLL comparison", rom);
    require(rom.composite_tone->layers.back().base_opll_rom == 7, "manual ROM choice retained");
    options.fixed_opll_tone.reset();
    options.max_evaluations = 15;
    const auto automaticRom = convertCompositeWave(analysis, options);
    validate(automaticRom, 2, 8);
    require(automaticRom.evaluations == 15 && automaticRom.composite_tone->layers.back().base_opll_rom <= 14,
            "automatic selection evaluates every ROM within its explicit budget");
    options.configuration = CompositeWavConfiguration::SccOpllOriginal;
    options.max_evaluations = 24;
    const auto original = convertCompositeWave(analysis, options);
    validate(original, 2, 8);
    validateMatchedMorphComparisons(original, options.configuration);
    printMorphComparison("original OPLL SCC+OPLL comparison", original);
    options.configuration = CompositeWavConfiguration::Scc;
    options.max_scc_waveforms = 1;
    options.scc_start_number = 31;
    const auto lastBankSlot = convertCompositeWave(analysis, options);
    validate(lastBankSlot, 1, 1);
    require(lastBankSlot.resource_plan.first_scc_number == 31
            && lastBankSlot.resource_plan.scc_numbers == std::vector<std::uint8_t>{31},
        "manual SCC bank offset preserves the final legal single-wave slot");
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
    std::size_t failures{};
    const auto check = [&](const char* name, const std::function<void()>& regression) {
        try { regression(); }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "WAV conversion regression [%s]: %s\n", name, error.what());
        }
    };
    check("existing conversion", run);
    check("local discontinuity", localDiscontinuityRegressions);
    check("waveform methods", waveformMethodRegressions);
    check("latent zero wave", latentZeroWaveRegressions);
    check("N grid", countGridRegressions);
    check("dynamic TL", [] { dynamicOpllRegressions(false); });
    check("dynamic FB", [] { dynamicOpllRegressions(true); });
    check("search completion", completionRegressions);
    check("corpus modes", corpusModeRegressions);
    check("evolving modes", evolvingCorpusModeRegressions);
    return failures ? 1 : 0;
}
