// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_analysis.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace mgstc::engine;
constexpr double tau = 2.0 * std::numbers::pi;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void append16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void tag(std::vector<std::uint8_t>& bytes, const char* value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value[i]));
}
std::vector<std::uint8_t> floatWave(std::span<const float> samples, std::uint16_t channels = 1) {
    std::vector<std::uint8_t> bytes;
    tag(bytes, "RIFF"); append32(bytes, static_cast<std::uint32_t>(36 + samples.size() * 4));
    tag(bytes, "WAVE"); tag(bytes, "fmt "); append32(bytes, 16);
    append16(bytes, 3); append16(bytes, channels); append32(bytes, 48000);
    append32(bytes, 48000 * channels * 4); append16(bytes, channels * 4); append16(bytes, 32);
    tag(bytes, "data"); append32(bytes, static_cast<std::uint32_t>(samples.size() * 4));
    for (const auto sample : samples) append32(bytes, std::bit_cast<std::uint32_t>(sample));
    return bytes;
}
std::vector<std::uint8_t> extremaWave(std::uint16_t bits, bool extensible, bool floating = false) {
    std::vector<std::uint8_t> payload;
    if (floating) {
        for (float value : {-1.0F, 0.0F, 1.0F}) append32(payload, std::bit_cast<std::uint32_t>(value));
    } else if (bits == 8) payload = {0, 128, 255};
    else {
        const auto minimum = std::uint32_t{1} << (bits - 1);
        const auto maximum = minimum - 1;
        for (auto value : {minimum, std::uint32_t{0}, maximum})
            for (std::uint16_t byte = 0; byte < bits / 8; ++byte)
                payload.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
    }
    std::vector<std::uint8_t> bytes;
    tag(bytes, "RIFF");
    append32(bytes, static_cast<std::uint32_t>(36 + (extensible ? 24 : 0) + payload.size() + (payload.size() & 1U)));
    tag(bytes, "WAVE"); tag(bytes, "fmt "); append32(bytes, extensible ? 40 : 16);
    append16(bytes, extensible ? 0xfffe : (floating ? 3 : 1));
    append16(bytes, 1); append32(bytes, 48000); append32(bytes, 48000 * bits / 8);
    append16(bytes, bits / 8); append16(bytes, bits);
    if (extensible) {
        append16(bytes, 22); append16(bytes, bits); append32(bytes, 0);
        append32(bytes, floating ? 3 : 1);
        const std::array<std::uint8_t, 12> tail{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        bytes.insert(bytes.end(), tail.begin(), tail.end());
    }
    tag(bytes, "data"); append32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0) bytes.push_back(0);
    return bytes;
}
std::shared_ptr<SourcePcm> tone(double seconds, double f0, bool missing_fundamental = false) {
    auto source = std::make_shared<SourcePcm>();
    source->sample_rate = 48000; source->channels = 1; source->bit_depth = 32;
    source->sample_format = SourceSampleFormat::FloatPcm;
    source->mono_samples.resize(static_cast<std::size_t>(seconds * source->sample_rate));
    for (std::size_t i = 0; i < source->mono_samples.size(); ++i) {
        const auto phase = tau * f0 * i / source->sample_rate;
        source->mono_samples[i] = static_cast<float>(missing_fundamental
            ? 0.45 * std::sin(phase * 2) + 0.3 * std::sin(phase * 3)
            : 0.6 * std::sin(phase));
    }
    source->interleaved_samples = source->mono_samples;
    return source;
}

void decoderTests() {
    for (auto bits : {8U, 16U, 24U, 32U}) {
        for (bool extensible : {false, true}) {
            SourcePcm source;
            require(parseCompositeWavePcm(extremaWave(static_cast<std::uint16_t>(bits), extensible), source),
                "supported integer PCM format rejected");
            require(source.bit_depth == bits && source.sample_format == SourceSampleFormat::IntegerPcm
                && source.mono_samples.size() == 3, "integer PCM metadata wrong");
            require(source.mono_samples[0] == -1.0F && source.mono_samples[1] == 0.0F
                && std::abs(source.mono_samples[2] - (1.0 - std::ldexp(1.0, 1 - static_cast<int>(bits)))) < 1.0e-7,
                "integer PCM extrema decoded incorrectly");
            const auto analyzed = analyzeCompositeWaveSource(std::make_shared<SourcePcm>(source), {0, 3});
            require(analyzed.analysis && analyzed.analysis->metadata.clipped_samples == 2,
                "integer full-scale clipping not counted");
        }
    }
    SourcePcm extensible_source;
    auto extensible_float = extremaWave(32, true, true);
    require(parseCompositeWavePcm(extensible_float, extensible_source)
        && extensible_source.sample_format == SourceSampleFormat::FloatPcm,
        "extensible float PCM rejected");
    auto bad_guid = extensible_float;
    bad_guid[48] = 1;
    require(!parseCompositeWavePcm(bad_guid, extensible_source), "invalid extensible GUID accepted");
    auto bad_alignment = extremaWave(24, false);
    bad_alignment[32] = 2;
    require(!parseCompositeWavePcm(bad_alignment, extensible_source), "invalid block alignment accepted");
    auto packed_bits = extremaWave(32, true);
    packed_bits[38] = 24;
    require(!parseCompositeWavePcm(packed_bits, extensible_source), "unsupported packed valid-bit format accepted");
    std::vector<float> stereo;
    for (int i = 0; i < 100; ++i) {
        const auto sample = static_cast<float>(0.5 * std::sin(tau * i / 20));
        stereo.push_back(sample); stereo.push_back(-sample);
    }
    auto bytes = floatWave(stereo, 2);
    SourcePcm source;
    std::string error;
    require(parseCompositeWavePcm(bytes, source, &error), "inverse-phase stereo decode failed");
    require(source.channels == 2 && source.interleaved_samples == stereo, "original stereo not retained");
    require(source.mono_strategy == AnalysisMonoStrategy::StrongestChannel, "inverse-phase mono fallback missing");
    require(std::abs(source.mono_samples[5] - 0.5F) < 1.0e-6F, "inverse-phase input cancelled in analysis");
    stereo[3] = std::numeric_limits<float>::quiet_NaN();
    const auto preserved = source.interleaved_samples;
    require(!parseCompositeWavePcm(floatWave(stereo, 2), source, &error), "nonfinite WAV accepted");
    require(source.interleaved_samples == preserved, "failed decode mutated prior source");
    auto clipped = floatWave(std::array<float, 4>{1.5F, -1.1F, 0.0F, 0.25F});
    require(parseCompositeWavePcm(clipped, source), "finite clipped float failed decode");
    require(source.interleaved_samples[0] == 1.5F, "original float preview PCM was clamped");
    bytes.pop_back();
    require(!parseCompositeWavePcm(bytes, source), "truncated WAV accepted");
    const auto cancellation = std::make_shared<std::atomic<bool>>(true);
    require(!parseCompositeWavePcm(clipped, source, &error, cancellation), "decode cancellation ignored");
}

void selectionLocalStereoTests() {
    auto source = tone(0.3, 220.0);
    std::vector<float> stereo;
    for (std::size_t i = 0; i < source->mono_samples.size(); ++i) {
        stereo.push_back(source->mono_samples[i]);
        stereo.push_back(i < 9600 ? source->mono_samples[i] : -source->mono_samples[i]);
    }
    SourcePcm decoded;
    require(parseCompositeWavePcm(floatWave(stereo, 2), decoded), "mixed-phase stereo decode failed");
    require(decoded.mono_strategy == AnalysisMonoStrategy::Average, "fixture should globally select averaging");
    auto original = std::make_shared<SourcePcm>(std::move(decoded));
    const auto before = original->mono_samples;
    const auto result = analyzeCompositeWaveSource(original, {9600, 14400});
    require(result.analysis && result.analysis->mono_strategy == AnalysisMonoStrategy::StrongestChannel,
        "selection-local phase cancellation was not detected");
    require(result.analysis->confidence > 0.8
        && std::abs(result.analysis->reference_pitch_hz - 220.0) < 2.0,
        "selected inverse-phase signal lost its pitch");
    require(result.analysis->analysisPcm()[9610] != 0.0F && original->mono_samples == before
        && result.analysis->source.get() == original.get(), "selection analysis mutated original preview data");
}

void pitchAndHarmonicTests() {
    for (bool missing_fundamental : {false, true}) {
        auto source = tone(0.4, 220.0, missing_fundamental);
        const auto first = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()});
        const auto second = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()});
        require(first.completion == SourceAnalysisCompletion::Completed && first.analysis, "tonal analysis failed");
        require(std::abs(first.analysis->reference_pitch_hz - 220.0) < 2.0, "F0 octave/missing-fundamental error");
        require(first.analysis->confidence > 0.8, "periodic source confidence low");
        require(first.analysis->reference_pitch_hz == second.analysis->reference_pitch_hz, "analysis not deterministic");
        require(first.analysis->pitch_trajectory.size() == second.analysis->pitch_trajectory.size(), "frame count changed");
        const auto& frame = first.analysis->harmonic_trajectory[20];
        require(!frame.harmonics.empty(), "harmonics absent");
        const auto harmonic = missing_fundamental ? 1U : 0U;
        const auto expected = missing_fundamental ? 0.45 : 0.6;
        require(std::abs(frame.harmonics[harmonic].amplitude - expected) < 0.04, "harmonic amplitude inaccurate");
        require(std::isfinite(frame.harmonics[harmonic].phase_radians), "harmonic phase not finite");
        require(std::abs(frame.harmonics[harmonic].frequency_hz - (harmonic + 1) * 220.0) < 4.0,
            "tracked harmonic frequency inaccurate");
        require(first.analysis->sustain_region.has_value(), "stable tonal sustain not detected");
        require(!first.analysis->estimated_key_off, "keyoff invented for stable held tone");
    }
}

void selectionAndFailureTests() {
    auto source = tone(0.3, 330.0);
    std::fill(source->mono_samples.begin(), source->mono_samples.begin() + 4800, 0.0F);
    source->interleaved_samples = source->mono_samples;
    const auto result = analyzeCompositeWaveSource(source, {0, 12000});
    require(result.analysis && std::abs(result.analysis->metadata.duration_seconds - 0.25) < 1.0e-10,
        "end-exclusive selection duration incorrect");
    require(!result.analysis->metadata.silent_ranges.empty()
        && result.analysis->metadata.silent_ranges.front().begin == 0
        && result.analysis->metadata.silent_ranges.front().end == 4800, "silent range incorrect");
    require(result.analysis->source.get() == source.get(), "analysis did not retain reusable source");
    require(result.analysis->pitch_trajectory.back().sample_position < 12000, "selection end exceeded");
    require(analyzeCompositeWaveSource(source, {12000, 0}).completion == SourceAnalysisCompletion::InvalidInput,
        "reversed selection accepted");
    require(analyzeCompositeWaveSource(source, {0, 20000}).completion == SourceAnalysisCompletion::InvalidInput,
        "out-of-bounds selection accepted");
    require(analyzeCompositeWaveSource(source, {0, 0}).completion == SourceAnalysisCompletion::InvalidInput,
        "empty selection accepted");
    SourceAnalysisOptions cancelled_options;
    cancelled_options.cancel_requested = std::make_shared<std::atomic<bool>>(true);
    const auto cancelled = analyzeCompositeWaveSource(source, {0, 12000}, cancelled_options);
    require(cancelled.completion == SourceAnalysisCompletion::Cancelled && !cancelled.analysis,
        "cancel returned a partial usable result");
    SourceAnalysisOptions invalid_options;
    invalid_options.minimum_pitch_hz = std::numeric_limits<double>::quiet_NaN();
    require(analyzeCompositeWaveSource(source, {0, 12000}, invalid_options).completion
        == SourceAnalysisCompletion::InvalidInput, "nonfinite analysis option accepted");
    source->mono_samples[5000] = std::numeric_limits<float>::infinity();
    require(analyzeCompositeWaveSource(source, {0, 12000}).completion == SourceAnalysisCompletion::InvalidInput,
        "nonfinite source accepted");
    auto silence = tone(0.2, 220.0);
    std::fill(silence->mono_samples.begin(), silence->mono_samples.end(), 0.0F);
    silence->interleaved_samples = silence->mono_samples;
    const auto silent = analyzeCompositeWaveSource(silence, {0, silence->mono_samples.size()});
    require(silent.analysis && silent.analysis->reference_pitch_hz == 0.0 && silent.analysis->confidence == 0.0,
        "pitch invented for silence");
    require(!silent.analysis->sustain_region && !silent.analysis->estimated_key_off, "regions invented for silence");
    require(silent.analysis->periodic_rms == 0.0 && silent.analysis->periodic_confidence == 0.0,
        "silence assigned periodic evidence");
}

void harmonicPitchCorrectionAndResidualTests() {
    auto weak = tone(0.4, 220.0);
    for (std::size_t i = 0; i < weak->mono_samples.size(); ++i) {
        const auto phase = tau * 220.0 * i / weak->sample_rate;
        // The YIN first-dip threshold can accept the strong second partial's
        // half period. Odd harmonic evidence must recover the true period.
        weak->mono_samples[i] = static_cast<float>(0.015 * std::sin(phase)
            + 0.7 * std::sin(phase * 2.0) + 0.12 * std::sin(phase * 3.0));
    }
    weak->interleaved_samples = weak->mono_samples;
    std::vector<SourceAnalysisStage> stages;
    SourceAnalysisOptions options;
    options.stage_changed = [&](SourceAnalysisStage stage) { stages.push_back(stage); };
    const auto corrected = analyzeCompositeWaveSource(weak, {0, weak->mono_samples.size()}, options);
    require(corrected.analysis && std::abs(corrected.analysis->reference_pitch_hz - 220.0) < 3.0,
        "spectral odd harmonics did not correct weak-fundamental octave ambiguity");
    require(stages == std::vector<SourceAnalysisStage>{SourceAnalysisStage::PitchAndAmplitude,
        SourceAnalysisStage::Harmonics, SourceAnalysisStage::Regions}, "analysis stage order wrong");
    for (std::size_t frame = 4; frame + 4 < corrected.analysis->pitch_trajectory.size(); ++frame)
        require(std::abs(corrected.analysis->pitch_trajectory[frame].frequency_hz - 220.0) < 3.0,
            "weak-fundamental correction unstable between frames");

    // A visible fundamental with a much stronger second must remain the pitch.
    for (std::size_t i = 0; i < weak->mono_samples.size(); ++i) {
        const auto phase = tau * 220.0 * i / weak->sample_rate;
        weak->mono_samples[i] = static_cast<float>(0.12 * std::sin(phase)
            + 0.7 * std::sin(phase * 2.0) + 0.06 * std::sin(phase * 3.0));
    }
    weak->interleaved_samples = weak->mono_samples;
    const auto visible = analyzeCompositeWaveSource(weak, {0, weak->mono_samples.size()});
    require(visible.analysis && std::abs(visible.analysis->reference_pitch_hz - 220.0) < 3.0,
        "stronger second partial displaced supported fundamental");

    auto chirp = tone(0.4, 220.0);
    double chirp_phase{};
    for (std::size_t i = 0; i < chirp->mono_samples.size(); ++i) {
        chirp_phase += tau * (220.0 + 220.0 * i / chirp->mono_samples.size()) / chirp->sample_rate;
        chirp->mono_samples[i] = static_cast<float>(0.6 * std::sin(chirp_phase));
    }
    chirp->interleaved_samples = chirp->mono_samples;
    const auto swept = analyzeCompositeWaveSource(chirp, {0, chirp->mono_samples.size()});
    require(static_cast<bool>(swept.analysis), "chirp analysis failed");
    double previous{}, first{}, last{};
    for (const auto& pitch : swept.analysis->pitch_trajectory) {
        if (pitch.frequency_hz <= 0.0) continue;
        if (first == 0.0) first = pitch.frequency_hz;
        if (previous > 0.0)
            require(std::abs(std::log2(pitch.frequency_hz / previous)) < 0.2,
                "harmonic correction caused chirp octave discontinuity");
        previous = last = pitch.frequency_hz;
    }
    require(last - first > 90.0, "chirp pitch motion was flattened");

    const auto pure = tone(0.4, 440.0);
    const auto periodic = analyzeCompositeWaveSource(pure, {0, pure->mono_samples.size()});
    require(periodic.analysis && std::abs(periodic.analysis->reference_pitch_hz - 440.0) < 3.0,
        "unheard pure-tone subharmonic invented");
    const auto& clean_frame = periodic.analysis->harmonic_trajectory[20];
    require(clean_frame.periodic_confidence > 0.9 && clean_frame.periodic_rms > 0.35
        && clean_frame.residual_rms < 0.08, "coherent periodic reconstruction inaccurate");
    require(periodic.analysis->periodic_confidence > 0.9 && periodic.analysis->periodic_rms > 0.35,
        "periodic selection summary inaccurate");

    for (bool inharmonic : {false, true}) {
        auto mixture = tone(0.4, 440.0);
        std::uint32_t random_state = 123456789U;
        for (std::size_t i = 0; i < mixture->mono_samples.size(); ++i) {
            random_state = random_state * 1664525U + 1013904223U;
            const auto component = inharmonic
                ? 0.35 * std::sin(tau * 440.0 * std::sqrt(2.0) * i / mixture->sample_rate)
                : 0.7 * (static_cast<double>(random_state >> 8) / 16777215.0 * 2.0 - 1.0);
            mixture->mono_samples[i] = static_cast<float>(mixture->mono_samples[i] * 0.6 + component);
        }
        mixture->interleaved_samples = mixture->mono_samples;
        SourceAnalysisOptions manual;
        manual.reference_pitch_hz = 440.0;
        const auto analyzed = analyzeCompositeWaveSource(mixture, {0, mixture->mono_samples.size()}, manual);
        require(static_cast<bool>(analyzed.analysis), "residual fixture analysis failed");
        const auto& frame = analyzed.analysis->harmonic_trajectory[20];
        require(frame.residual_rms > 0.12
            && frame.periodic_confidence < clean_frame.periodic_confidence - 0.15,
            "noise/inharmonic energy was misclassified as representable periodic energy");
        require(std::isfinite(frame.periodic_rms) && frame.periodic_confidence >= 0.0
            && frame.periodic_confidence <= 1.0, "residual confidence invalid");
    }
}

void variedSignalTests() {
    // Generated fixtures, with deliberately different transient and periodic
    // structures. Numeric safety is distinct from guaranteeing perceptual fit.
    for (int kind = 0; kind < 8; ++kind) {
        auto source = std::make_shared<SourcePcm>();
        source->sample_rate = 12000; source->channels = 1; source->bit_depth = 32;
        source->sample_format = SourceSampleFormat::FloatPcm;
        source->mono_samples.resize(3600);
        double phase{};
        for (std::size_t i = 0; i < source->mono_samples.size(); ++i) {
            const auto time = static_cast<double>(i) / source->sample_rate;
            const auto f0 = kind == 5 ? 220.0 + 8.0 * std::sin(tau * 4.0 * time) : 220.0;
            phase += tau * f0 / source->sample_rate;
            const auto cycle = std::fmod(phase / tau, 1.0);
            double value{};
            switch (kind) {
            case 0: value = std::sin(phase) >= 0.0 ? 0.5 : -0.5; break; // square
            case 1: value = cycle - 0.5; break; // saw
            case 2: value = cycle < 0.25 ? 0.5 : -0.25; break; // pulse
            case 3: value = 0.5 * std::sin(phase + 2.0 * std::sin(phase * 2.0)); break; // FM
            case 4: value = 0.7 * std::exp(-time * 12.0) * std::sin(phase); break; // decay
            case 5: value = 0.5 * std::sin(phase); break; // vibrato
            case 6: value = 0.3 * std::sin(phase) + 0.25 * std::sin(phase * 1.41421356); break;
            case 7: value = time < 0.015 ? 0.8 * std::sin(tau * 3700.0 * time)
                : 0.5 * std::sin(phase); break; // short attack
            }
            source->mono_samples[i] = static_cast<float>(value);
        }
        source->interleaved_samples = source->mono_samples;
        const auto result = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()});
        require(result.analysis && result.completion == SourceAnalysisCompletion::Completed,
            "generated signal analysis failed");
        require(std::isfinite(result.analysis->reference_pitch_hz)
            && std::isfinite(result.analysis->confidence), "generated signal produced nonfinite pitch");
        if (kind <= 2 || kind == 4 || kind == 7)
            require(std::abs(result.analysis->reference_pitch_hz - 220.0) < 5.0,
                "generated periodic signal had F0 octave error");
        for (const auto& frame : result.analysis->harmonic_trajectory) {
            require(std::isfinite(frame.residual_rms) && std::isfinite(frame.spectral_centroid_hz),
                "spectral diagnostic nonfinite");
            for (const auto& harmonic : frame.harmonics)
                require(std::isfinite(harmonic.frequency_hz) && std::isfinite(harmonic.amplitude)
                    && std::isfinite(harmonic.phase_radians), "generated harmonic nonfinite");
        }
        if (kind == 5) {
            double previous{}, minimum = 1.0e9, maximum{};
            for (const auto& frame : result.analysis->pitch_trajectory) {
                if (frame.frequency_hz <= 0.0) continue;
                minimum = std::min(minimum, frame.frequency_hz);
                maximum = std::max(maximum, frame.frequency_hz);
                if (previous > 0.0)
                    require(std::abs(std::log2(frame.frequency_hz / previous)) < 0.1,
                        "vibrato continuity jumped an octave");
                previous = frame.frequency_hz;
            }
            require(maximum - minimum > 3.0, "vibrato trajectory flattened");
        }
    }
    auto short_source = tone(0.001, 440.0);
    const auto short_result = analyzeCompositeWaveSource(short_source, {0, short_source->mono_samples.size()});
    require(short_result.analysis && short_result.analysis->reference_pitch_hz == 0.0,
        "short source assigned an unsupported pitch");
    auto excessive_source = tone(0.01, 440.0);
    excessive_source->sample_rate = 1;
    require(analyzeCompositeWaveSource(excessive_source,
        {0, excessive_source->mono_samples.size()}).completion == SourceAnalysisCompletion::InvalidInput,
        "duration resource bound ignored");
}

void manualPitchGuideTests() {
    const auto source = tone(0.4, 220.0);
    const auto automatic = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()});
    SourceAnalysisOptions manual;
    manual.reference_pitch_hz = 110.0;
    const auto guided = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()}, manual);
    require(automatic.analysis && guided.analysis && guided.analysis->reference_pitch_hz == 110.0,
        "manual reference summary incorrect");
    const auto& harmonics = guided.analysis->harmonic_trajectory[20].harmonics;
    require(harmonics.size() >= 2 && harmonics[0].amplitude < 0.05
        && std::abs(harmonics[1].frequency_hz - 220.0) < 4.0
        && std::abs(harmonics[1].amplitude - 0.6) < 0.05,
        "manual missing fundamental did not change source220 into harmonic2");
    for (std::size_t i = 0; i < guided.analysis->pitch_trajectory.size(); ++i)
        require(std::abs(guided.analysis->pitch_trajectory[i].frequency_hz
            - automatic.analysis->pitch_trajectory[i].frequency_hz * 0.5) < 1.0e-10,
            "manual octave guide not reflected consistently in source trajectory");

    manual.reference_pitch_hz = 230.0;
    const auto nearby = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()}, manual);
    require(static_cast<bool>(nearby.analysis), "nearby manual reference analysis failed");
    for (std::size_t i = 0; i < nearby.analysis->pitch_trajectory.size(); ++i)
        require(nearby.analysis->pitch_trajectory[i].frequency_hz
            == automatic.analysis->pitch_trajectory[i].frequency_hz,
            "non-octave manual guide transposed measured source pitch");

    auto moving = tone(0.4, 220.0);
    double phase{};
    for (std::size_t i = 0; i < moving->mono_samples.size(); ++i) {
        const auto time = static_cast<double>(i) / moving->sample_rate;
        const auto f0 = 220.0 + 330.0 * i / moving->mono_samples.size()
            + 8.0 * std::sin(tau * 4.0 * time);
        phase += tau * f0 / moving->sample_rate;
        moving->mono_samples[i] = static_cast<float>(0.6 * std::sin(phase));
    }
    moving->interleaved_samples = moving->mono_samples;
    const auto measured = analyzeCompositeWaveSource(moving, {0, moving->mono_samples.size()});
    manual.reference_pitch_hz = 190.0;
    const auto aligned = analyzeCompositeWaveSource(moving, {0, moving->mono_samples.size()}, manual);
    require(measured.analysis && aligned.analysis, "manual chirp/vibrato guide analysis failed");
    double previous{}, minimum = 1.0e9, maximum{};
    for (std::size_t i = 0; i < aligned.analysis->pitch_trajectory.size(); ++i) {
        const auto actual = measured.analysis->pitch_trajectory[i].frequency_hz;
        const auto adjusted = aligned.analysis->pitch_trajectory[i].frequency_hz;
        require(std::abs(adjusted - actual * 0.5) < 1.0e-10,
            "manual guide factor changed across chirp/vibrato");
        if (adjusted <= 0.0) continue;
        minimum = std::min(minimum, adjusted); maximum = std::max(maximum, adjusted);
        if (previous > 0.0)
            require(std::abs(std::log2(adjusted / previous)) < 0.2,
                "manual guide introduced an octave discontinuity");
        previous = adjusted;
    }
    require(maximum - minimum > 100.0, "manual guide flattened measured pitch variation");
}
} // namespace

int main() {
    try {
        decoderTests(); selectionLocalStereoTests(); pitchAndHarmonicTests(); selectionAndFailureTests();
        harmonicPitchCorrectionAndResidualTests(); variedSignalTests(); manualPitchGuideTests();
        std::cout << "Composite WAV analysis tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
