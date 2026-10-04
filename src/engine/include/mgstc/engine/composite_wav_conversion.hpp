// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mgstc/engine/composite_wav_analysis.hpp"
#include "mgstc/engine/composite_wav_renderer.hpp"

namespace mgstc::engine {

enum class CompositeWavConfiguration { Scc, SccScc, SccOpllRom, SccOpllOriginal };
enum class CompositeWavStrategy { Automatic, FundamentalResidual, LowHigh, AttackSustain, Independent };
enum class CompositeWavPreference { Balanced, Quality, Compact };
enum class CompositeWavLoopMode { Automatic, None };
enum class CompositeWavStage { Idle, Candidates, Morph, Envelope, Evaluation, Completed, Cancelled };

struct CompositeWavConversionControl {
    std::atomic_bool cancel_requested{false};
    std::atomic<CompositeWavStage> stage{CompositeWavStage::Idle};
    std::atomic_size_t evaluations{0};
};

struct CompositeWavConversionOptions {
    CompositeWavConfiguration configuration{CompositeWavConfiguration::Scc};
    CompositeWavStrategy strategy{CompositeWavStrategy::Automatic};
    CompositeWavPreference preference{CompositeWavPreference::Balanced};
    CompositeWavLoopMode loop_mode{CompositeWavLoopMode::Automatic};
    std::size_t max_scc_waveforms{16};
    // Selected by the composite editor's allocation control, not duplicated UI.
    std::optional<std::uint8_t> scc_start_number;
    std::optional<std::uint8_t> fixed_opll_tone; // MGSTC ROM @0..14.
    std::optional<std::size_t> key_off_position; // Original PCM frame.
    std::size_t max_evaluations{96};
    double minimum_improvement{1e-5};
    std::size_t stagnation_limit{24};
    std::shared_ptr<CompositeWavConversionControl> control;
};

struct CompositeWavResourcePlan {
    std::uint8_t first_scc_number{};
    std::size_t scc_waveforms{};
    std::vector<std::uint8_t> scc_numbers;
};

struct CompositeWavQualityMetrics {
    double multi_resolution_stft{};
    double harmonic{};
    double erb{};
    double attack{};
    double volume{};
    double transition{};
    double complexity{};
    double total{};
};

// Centralized objective weights; metrics are normalized dimensionless errors.
struct CompositeWavQualityWeights {
    double multi_resolution_stft{1.0};
    double harmonic{0.6};
    double erb{0.4};
    double attack{0.8};
    double volume{0.5};
    double transition{0.02};
    double complexity{0.003};
};

enum class CompositeWavConversionCompletion { Completed, Cancelled, InvalidInput, NoValidCandidate };

// Offline search diagnostics, never part of timbre/plugin persistence. A trial
// is counted only when the complete candidate has valid actual Engine PCM,
// freshly rendered or reused by exact materialized sound equality.
enum class CompositeWavSearchFamily { MorphInterval, KeyTime, WaveShape, Envelope, Loop, Reduction, Count };
struct CompositeWavSearchDiagnostics {
    std::array<std::size_t, static_cast<std::size_t>(CompositeWavSearchFamily::Count)> trials{};
    std::array<std::size_t, static_cast<std::size_t>(CompositeWavSearchFamily::Count)> accepted{};
};

// Worker-only evidence. A nonzero group compares exactly the same full sound,
// changing only the indicated interval's distribution. Invalid members retain
// their rejection reason. `selected` identifies the final complete composite;
// `group_winner` identifies a matched three-way comparison's local winner.
struct CompositeWavMorphCandidateDiagnostic {
    CompositeWavConfiguration configuration{CompositeWavConfiguration::Scc};
    std::uint64_t candidate_id{}, comparison_group{};
    std::size_t layer_index{}, destination_event_index{};
    std::uint32_t start_count{}, end_count{};
    SccMorphDistributionMode distribution{SccMorphDistributionMode::AdaptiveDistribution};
    std::uint8_t intermediate_count{};
    double curve{1.0};
    SccMorphPlan plan;
    CompositeWavQualityMetrics quality;
    std::size_t bank_waveforms{};
    bool valid{}, accepted{}, comparison_complete{}, group_winner{}, selected{};
    std::string reason;
};

struct CompositeWavMorphSearchDiagnostics {
    // Mode order is Adaptive, Time, Tone. Trials include evaluations of cached
    // real Engine PCM; cache hits never substitute ideal float waveforms.
    std::array<std::size_t, 3> mode_trials{}, mode_accepted{};
    std::size_t complete_comparisons{}, incomplete_comparisons{}, adaptive_plan_trials{};
    std::size_t global_combination_trials{}, global_combination_accepted{}, retained_segment_pools{};
    std::size_t morph_cache_hits{}, morph_cache_misses{}, render_cache_hits{}, render_cache_misses{};
    std::size_t candidate_pool_peak{};
    // Peak logical owned storage plus the common planner's scratch estimate;
    // excludes allocator overhead, Engine internals and shared source analysis.
    std::size_t working_set_bytes{};
    // All-mode candidate Morph/cache preparation and Engine/objective time.
    // External WAV u/count proposal preparation is outside these two scopes.
    double planning_seconds{}, evaluation_seconds{};
    // Initial structural Adaptive candidates and matched triplet mode 0 only;
    // covers their evaluation's Morph preparation, real PCM/cache and objective
    // work. Structural source acquisition/keyframe/curve fitting is excluded.
    // Mixed-mode global combinations and ordinary six-family edits are excluded.
    double adaptive_candidate_seconds{};
    // External WAV-guided u/count branch, including its common planner,
    // feature proposal, complete candidate evaluation and early-return work.
    // Disjoint from adaptive_candidate_seconds; both are worker wall times.
    double adaptive_refinement_seconds{};
};

struct CompositeWavConversionResult {
    CompositeWavConversionCompletion completion{CompositeWavConversionCompletion::InvalidInput};
    std::optional<CompositeTimbre> composite_tone;
    CompositeWavResourcePlan resource_plan;
    CompositeWavRenderResult preview;
    std::shared_ptr<const SourceAnalysis> analysis_reference;
    std::size_t evaluations{}; // Candidate objectives, including exact cached real Engine PCM.
    std::size_t loop_probe_renders{}; // Bounded held-note renders, separate from candidate evaluations.
    double elapsed_seconds{};
    double initial_loss{};
    double best_loss{};
    CompositeWavQualityMetrics quality;
    CompositeWavSearchDiagnostics search;
    CompositeWavMorphSearchDiagnostics morph_search;
    std::vector<CompositeWavMorphCandidateDiagnostic> morph_candidates;
    std::vector<std::string> warnings;
    std::string error;
};

[[nodiscard]] CompositeWavQualityMetrics evaluateCompositeWavePcm(
    std::span<const float> reference, std::span<const float> rendered,
    double reference_frequency_hz, std::size_t attack_end_frame,
    std::size_t waveform_count = 0,
    const CompositeWavQualityWeights& weights = {},
    const std::atomic_bool* cancel = nullptr,
    const SourceAnalysis* source_analysis = nullptr);

// Worker-thread only. Cache misses render through a fresh real EngineCore;
// bounded exact caches reuse common Morph preparation and actual Engine PCM.
// Input analysis/library/editor state are never modified. Cancelled results
// retain an already evaluated best candidate for diagnostics only.
[[nodiscard]] CompositeWavConversionResult convertCompositeWave(
    std::shared_ptr<const SourceAnalysis> analysis,
    const CompositeWavConversionOptions& options = {});

} // namespace mgstc::engine
