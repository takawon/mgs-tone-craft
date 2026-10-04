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
#include <map>
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
        || reason.find("SCC global output capacity exceeded") != std::string::npos;
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
    require(hasStrictNonAdaptiveWinner(first),
        "an evolving WAV interval strictly selects Time or Tone when its Engine PCM score beats Adaptive");
    printMorphComparison("SCC comparison", first);
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
    try { run(); completionRegressions(); corpusModeRegressions(); evolvingCorpusModeRegressions(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "WAV conversion regression: %s\n", error.what()); return 1; }
}
