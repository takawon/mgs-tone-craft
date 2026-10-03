// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
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

struct CompositeWavConversionResult {
    CompositeWavConversionCompletion completion{CompositeWavConversionCompletion::InvalidInput};
    std::optional<CompositeTimbre> composite_tone;
    CompositeWavResourcePlan resource_plan;
    CompositeWavRenderResult preview;
    std::shared_ptr<const SourceAnalysis> analysis_reference;
    std::size_t evaluations{};
    double elapsed_seconds{};
    double initial_loss{};
    double best_loss{};
    CompositeWavQualityMetrics quality;
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

// Worker-thread only. Each complete candidate is rendered through a fresh real
// EngineCore; input analysis/library/editor state are never modified. Cancelled
// results retain an already evaluated best candidate for diagnostics only.
[[nodiscard]] CompositeWavConversionResult convertCompositeWave(
    std::shared_ptr<const SourceAnalysis> analysis,
    const CompositeWavConversionOptions& options = {});

} // namespace mgstc::engine
