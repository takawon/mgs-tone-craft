// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/scc_waveform.hpp"

namespace mgstc::engine {

inline constexpr std::uint32_t kSccMorphAlgorithmVersion = 1;
inline constexpr double kSccMorphGammaMax = 8.0;

struct SccMorphDescriptors {
    double centroid{}, spread{}, slope{}, odd_even{}, irregularity{}, flatness{};
    double rms{}, dc{}, peak{};
};

struct SccMorphAnalysis {
    // Normalized DFT: DC and Nyquist are real, positive/negative harmonics
    // are conjugates. Positive harmonics are 1..16 (0 is DC).
    std::array<double, 17> magnitude{}, phase{};
    SccMorphDescriptors descriptors;
};

enum class SccMorphCandidate {
    Source, LogHarmonic, AlignedTime, SpectralEnvelope,
};

struct SccMorphWaveDiagnostic {
    std::size_t layer_index{};
    std::uint32_t count{};
    double u{};
    SccMorphCandidate candidate{SccMorphCandidate::Source};
    double pre_quantization_error{}, post_quantization_error{};
    SccMorphDescriptors target{}, result{};
    std::size_t phase_shift{};
    std::uint8_t output_number{};
    bool generated{};
};

struct SccMorphPairResult {
    std::vector<SccWaveform> intermediate;
    std::vector<SccMorphWaveDiagnostic> diagnostics;
};

struct SccMorphCompileResult {
    CompositeTimbre timbre;
    bool valid{true};
    std::string error;
    // Counts only byte-distinct derived waveforms. Capacity excludes source
    // slots already occupied in the selected 0..31 / 16..31 range.
    std::size_t used{}, capacity{}, required{};
    std::vector<SccMorphWaveDiagnostic> diagnostics;
};

[[nodiscard]] double morphPosition(double time, double gamma) noexcept;
[[nodiscard]] SccMorphAnalysis analyzeSccMorphWaveform(const SccWaveform& wave);
[[nodiscard]] double sccMorphTransitionCost(const SccWaveform& first,
                                          const SccWaveform& second) noexcept;
[[nodiscard]] SccMorphPairResult generateSccMorph(
    const SccWaveform& first, const SccWaveform& second,
    std::uint8_t intermediate_count, double gamma = 1.0);
// Exact 32-state dynamic programming on quantized waves. Endpoints remain
// byte-identical when fixed; all other nodes may be circularly shifted.
[[nodiscard]] std::vector<std::size_t> optimizeSccMorphBlockPhases(
    std::vector<SccWaveform>& waves, bool fixed_start, bool fixed_end);
// Non-audio-thread API. Keeps authored sources/events intact and materializes
// the derived cache into a separate CompositeTimbre for existing consumers.
[[nodiscard]] SccMorphCompileResult compileSccMorph(const CompositeTimbre& input);
// Bounded one-entry cache per preparation thread; exact model equality is the
// invalidation key, including source snapshots and output allocation settings.
[[nodiscard]] std::shared_ptr<const SccMorphCompileResult> compileSccMorphCached(
    const CompositeTimbre& input);

} // namespace mgstc::engine
