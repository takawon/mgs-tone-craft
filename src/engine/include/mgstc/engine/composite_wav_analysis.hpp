// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mgstc::engine {

// All positions address original PCM frames. End is exclusive, including regions.
struct SampleSelection {
    std::size_t begin{};
    std::size_t end{};
};

enum class SourceSampleFormat { IntegerPcm, FloatPcm };
enum class AnalysisMonoStrategy { Average, StrongestChannel };

struct SourcePcm {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::uint16_t bit_depth{};
    SourceSampleFormat sample_format{SourceSampleFormat::IntegerPcm};
    // Original decoded values, without normalization/clamping, for preview.
    std::vector<float> interleaved_samples;
    std::vector<float> mono_samples;
    AnalysisMonoStrategy mono_strategy{AnalysisMonoStrategy::Average};
};

struct SourceMetadata {
    double duration_seconds{};
    double peak{};
    double rms{};
    std::size_t clipped_samples{};
    std::vector<SampleSelection> silent_ranges;
};

struct PitchTrajectoryFrame {
    std::size_t sample_position{};
    double frequency_hz{}; // Zero means unvoiced/unknown, never an invented pitch.
    double confidence{};
};

struct HarmonicComponent {
    std::size_t harmonic{}; // One-based harmonic order.
    double frequency_hz{};
    double amplitude{}; // Peak sinusoidal amplitude, in source PCM units.
    double phase_radians{}; // Relative to frame centre, not global oscillator phase.
};

struct HarmonicTrajectoryFrame {
    std::size_t sample_position{};
    std::vector<HarmonicComponent> harmonics;
    double residual_rms{};
    double spectral_centroid_hz{};
};

struct AmplitudeEnvelopeFrame {
    std::size_t sample_position{};
    double rms{};
    double peak{};
};

struct EstimatedSourceRegion {
    SampleSelection selection{};
    double confidence{};
};

struct EstimatedKeyOff {
    std::size_t sample_position{};
    double confidence{};
};

// Immutable result can be reused when only conversion options change.
// Source/selection/reference pitch changes require a new analysis.
struct SourceAnalysis {
    std::shared_ptr<const SourcePcm> source;
    SampleSelection selection{};
    // Selection-local stereo cancellation can differ from the whole WAV.
    // Override has original frame indexing; empty means reuse source mono.
    // Original interleaved preview data remains immutable.
    std::vector<float> analysis_mono_override;
    AnalysisMonoStrategy mono_strategy{AnalysisMonoStrategy::Average};
    SourceMetadata metadata;
    double reference_pitch_hz{};
    std::vector<PitchTrajectoryFrame> pitch_trajectory;
    std::vector<HarmonicTrajectoryFrame> harmonic_trajectory;
    std::vector<AmplitudeEnvelopeFrame> amplitude_envelope;
    EstimatedSourceRegion attack_region;
    std::optional<EstimatedSourceRegion> sustain_region;
    std::optional<EstimatedKeyOff> estimated_key_off;
    double confidence{};
    std::vector<std::string> warnings;

    [[nodiscard]] std::span<const float> analysisPcm() const noexcept {
        return !analysis_mono_override.empty() ? std::span<const float>(analysis_mono_override)
            : (source ? std::span<const float>(source->mono_samples) : std::span<const float>{});
    }
};

struct SourceAnalysisOptions {
    std::optional<double> reference_pitch_hz;
    double minimum_pitch_hz{40.0};
    double maximum_pitch_hz{2000.0};
    double hop_seconds{0.01};
    std::size_t maximum_harmonics{16};
    std::shared_ptr<std::atomic<bool>> cancel_requested;
};

enum class SourceAnalysisCompletion { Completed, Cancelled, InvalidInput };
struct SourceAnalysisResult {
    SourceAnalysisCompletion completion{SourceAnalysisCompletion::InvalidInput};
    std::shared_ptr<const SourceAnalysis> analysis;
    std::string error;
};

// Worker-thread only: allocates and performs bounded offline analysis.
// RIFF PCM formats match wave_import; unlike the legacy mono parser this retains
// stereo and rejects nonfinite float PCM rather than silently replacing it.
[[nodiscard]] bool parseCompositeWavePcm(
    std::span<const std::uint8_t> bytes, SourcePcm& output,
    std::string* error = nullptr,
    const std::shared_ptr<std::atomic<bool>>& cancel_requested = {});

[[nodiscard]] SourceAnalysisResult analyzeCompositeWaveSource(
    std::shared_ptr<const SourcePcm> source, SampleSelection selection,
    const SourceAnalysisOptions& options = {});

} // namespace mgstc::engine
