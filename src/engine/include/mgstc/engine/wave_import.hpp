#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"

namespace mgstc::engine {

struct WavePcm {
    std::uint32_t sample_rate{};
    std::vector<float> mono_samples;
};

struct WaveCycleAnalysis {
    std::vector<float> cycle;
    float estimated_frequency_hz{};
};

enum class OpllApproximationEffort {
    Standard,
    Thorough,
};

enum class OpllApproximationPhase : std::uint8_t {
    Idle,
    RepresentativeCycle,
    SteadyTimbre,
    ShortTimbre,
    FullEnvelope,
    Completed,
    Cancelled,
};

enum class OpllApproximationCompletion : std::uint8_t {
    Completed,
    Cancelled,
};

// Compact is a deterministic test/calibration plan. Production is the UI
// default and carries the accepted quality budgets.
enum class OpllApproximationProfile : std::uint8_t {
    Production,
    Compact,
};

struct OpllApproximationProgress {
    OpllApproximationPhase phase{OpllApproximationPhase::Idle};
    std::uint64_t completed{};
    std::uint64_t total{};
    bool cancel_requested{};
};

class OpllApproximationControl {
public:
    OpllApproximationControl();

    void requestCancel() noexcept;
    [[nodiscard]] bool cancelRequested() const noexcept;
    [[nodiscard]] OpllApproximationProgress progress() const noexcept;

private:
    struct State {
        std::atomic<bool> cancel_requested{false};
        std::atomic<OpllApproximationPhase> phase{
            OpllApproximationPhase::Idle};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> total{0};
    };

    std::shared_ptr<State> state_;
    friend struct OpllApproximationCoordinator;
};

struct OpllApproximationOptions {
    std::size_t max_workers{8};
    OpllApproximationEffort effort{OpllApproximationEffort::Standard};
    std::shared_ptr<OpllApproximationControl> control;
    OpllApproximationProfile profile{OpllApproximationProfile::Production};
};

struct OpllApproximationResult {
    OpllApproximationCompletion completion{
        OpllApproximationCompletion::Completed};
    std::vector<OpllPatchParameters> candidates;
};

[[nodiscard]] bool parseWavePcm(
    std::span<const std::uint8_t> bytes,
    WavePcm& output,
    std::string* error = nullptr);

[[nodiscard]] WaveCycleAnalysis analyzeWaveCycle(
    const WavePcm& pcm);

[[nodiscard]] SccWaveform waveCycleToScc(
    std::span<const float> cycle) noexcept;

[[nodiscard]] OpllPatchParameters approximateWaveCycleWithOpll(
    std::span<const float> cycle) noexcept;

// Ranked steady-state OPLL approximations for one waveform cycle.
// Deterministic: identical cycle samples yield identical register values.
[[nodiscard]] std::vector<OpllPatchParameters>
approximateWaveCycleCandidatesWithOpll(std::span<const float> cycle);
[[nodiscard]] std::vector<OpllPatchParameters>
approximateWaveCycleCandidatesWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options);
[[nodiscard]] OpllApproximationResult approximateWaveCycleWithOpllResult(
    std::span<const float> cycle,
    const OpllApproximationOptions& options = {});

// SCC 32-sample waveform → OPLL. Fits by rendering both at the same MIDI
// note and comparing audio (no envelope path; SCC single-timbre is steady).
[[nodiscard]] std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(const SccWaveform& waveform);
[[nodiscard]] std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options);
[[nodiscard]] OpllApproximationResult approximateSccWaveformWithOpllResult(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options = {});

// Uses the complete audible portion of a WAV to fit both the steady-state
// spectrum and the YM2413 hardware envelope. The search is deterministic:
// identical PCM and emu2413 versions produce identical register values.
[[nodiscard]] OpllPatchParameters approximateWavePcmWithOpll(
    const WavePcm& pcm);

// Returns the deterministic ranked alternatives retained by the search.
// The first entry is the same patch returned by approximateWavePcmWithOpll.
[[nodiscard]] std::vector<OpllPatchParameters>
approximateWavePcmCandidatesWithOpll(const WavePcm& pcm);
[[nodiscard]] std::vector<OpllPatchParameters>
approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm,
    const OpllApproximationOptions& options);
[[nodiscard]] OpllApproximationResult approximateWavePcmWithOpllResult(
    const WavePcm& pcm,
    const OpllApproximationOptions& options = {});

}  // namespace mgstc::engine
