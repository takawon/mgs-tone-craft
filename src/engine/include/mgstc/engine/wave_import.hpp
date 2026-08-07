#pragma once

#include <cstdint>
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

// SCC 32-sample waveform → OPLL. Fits by rendering both at the same MIDI
// note and comparing audio (no envelope path; SCC single-timbre is steady).
[[nodiscard]] std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(const SccWaveform& waveform);

// Uses the complete audible portion of a WAV to fit both the steady-state
// spectrum and the YM2413 hardware envelope. The search is deterministic:
// identical PCM and emu2413 versions produce identical register values.
[[nodiscard]] OpllPatchParameters approximateWavePcmWithOpll(
    const WavePcm& pcm);

// Returns the deterministic ranked alternatives retained by the search.
// The first entry is the same patch returned by approximateWavePcmWithOpll.
[[nodiscard]] std::vector<OpllPatchParameters>
approximateWavePcmCandidatesWithOpll(const WavePcm& pcm);

}  // namespace mgstc::engine
