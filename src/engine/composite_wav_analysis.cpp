// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_analysis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

namespace mgstc::engine {
namespace {
constexpr double kTau = 2.0 * std::numbers::pi;
constexpr std::size_t kMaximumDecodedSamples = 16'000'000;
constexpr double kSilence = 1.0e-5;

bool cancelled(const std::shared_ptr<std::atomic<bool>>& control) {
    return control && control->load(std::memory_order_relaxed);
}

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

float decode(std::span<const std::uint8_t> bytes, std::size_t offset,
             std::uint16_t format, std::uint16_t bits) {
    if (format == 3) {
        const auto word = u32(bytes, offset);
        float value{};
        std::memcpy(&value, &word, sizeof(value));
        return value;
    }
    if (bits == 8) return (static_cast<float>(bytes[offset]) - 128.0F) / 128.0F;
    if (bits == 16) return static_cast<std::int16_t>(u16(bytes, offset)) / 32768.0F;
    if (bits == 24) {
        auto word = u32(std::array<std::uint8_t, 4>{
            bytes[offset], bytes[offset + 1], bytes[offset + 2], 0}, 0);
        if ((word & 0x00800000U) != 0) word |= 0xff000000U;
        return static_cast<std::int32_t>(word) / 8388608.0F;
    }
    return static_cast<float>(static_cast<std::int32_t>(u32(bytes, offset)))
        / 2147483648.0F;
}

void fft(std::vector<std::complex<double>>& values) {
    const auto count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        auto bit = count >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= count; length *= 2) {
        const auto rotation = std::polar(1.0, -kTau / static_cast<double>(length));
        for (std::size_t base = 0; base < count; base += length) {
            std::complex<double> phase{1.0, 0.0};
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto even = values[base + j];
                const auto odd = values[base + j + length / 2] * phase;
                values[base + j] = even + odd;
                values[base + j + length / 2] = even - odd;
                phase *= rotation;
            }
        }
    }
}

// A bounded YIN-style cumulative-mean normalized difference estimator.
// de Cheveigne/Kawahara, JASA 111 (2002), doi:10.1121/1.1458024.
// This is not pYIN and does not provide a probabilistic voicing posterior.
PitchTrajectoryFrame pitchFrame(
    std::span<const float> pcm, SampleSelection selection, std::size_t centre,
    std::uint32_t rate, const SourceAnalysisOptions& options,
    double previous_pitch) {
    PitchTrajectoryFrame result{centre, 0.0, 0.0};
    const auto stride = std::max<std::size_t>(1, (rate + 11999U) / 12000U);
    const auto analysis_rate = static_cast<double>(rate) / stride;
    const auto maximum_lag = static_cast<std::size_t>(
        std::ceil(analysis_rate / options.minimum_pitch_hz));
    const auto minimum_lag = std::max<std::size_t>(2,
        static_cast<std::size_t>(analysis_rate / options.maximum_pitch_hz));
    const auto half = maximum_lag * 2;
    const auto count = half * 2;
    const auto radius = count / 2 * stride;
    const auto latest_begin = selection.end - selection.begin >= count * stride
        ? selection.end - count * stride : selection.begin;
    const auto begin = std::min(latest_begin,
        centre > radius ? std::max(selection.begin, centre - radius) : selection.begin);
    const auto available = std::min(count, (selection.end - begin) / stride);
    if (available < maximum_lag * 2 + 2) return result;
    std::vector<double> samples(available);
    double mean{}, energy{};
    for (std::size_t i = 0; i < available; ++i) {
        double sum{};
        for (std::size_t j = 0; j < stride; ++j) sum += pcm[begin + i * stride + j];
        samples[i] = sum / stride;
        mean += samples[i];
    }
    mean /= available;
    for (auto& value : samples) { value -= mean; energy += value * value; }
    if (energy / available <= kSilence * kSilence) return result;
    const auto comparisons = available - maximum_lag;
    std::vector<double> normalized(maximum_lag + 1, 1.0);
    double cumulative{};
    for (std::size_t lag = 1; lag <= maximum_lag; ++lag) {
        if ((lag & 15U) == 0 && cancelled(options.cancel_requested)) return result;
        double difference{};
        for (std::size_t i = 0; i < comparisons; ++i) {
            const auto delta = samples[i] - samples[i + lag];
            difference += delta * delta;
        }
        cumulative += difference;
        normalized[lag] = cumulative > 1.0e-20
            ? difference * lag / cumulative : 1.0;
    }
    std::size_t best{};
    for (std::size_t lag = minimum_lag; lag < maximum_lag; ++lag) {
        if (normalized[lag] < 0.15) {
            while (lag + 1 < maximum_lag && normalized[lag + 1] < normalized[lag]) ++lag;
            best = lag;
            break;
        }
    }
    if (best == 0) {
        best = static_cast<std::size_t>(std::min_element(
            normalized.begin() + minimum_lag, normalized.end()) - normalized.begin());
        if (normalized[best] >= 0.4) return result;
    }
    // Prefer a nearby previous-period valley only when comparably periodic.
    // Never multiply/divide an otherwise accepted pitch solely for continuity.
    if (previous_pitch > 0.0) {
        const auto expected = static_cast<std::size_t>(std::lround(analysis_rate / previous_pitch));
        if (expected > minimum_lag && expected < maximum_lag) {
            auto nearby = expected;
            for (auto lag = expected > 3 ? expected - 3 : minimum_lag;
                 lag <= std::min(maximum_lag - 1, expected + 3); ++lag) {
                if (normalized[lag] < normalized[nearby]) nearby = lag;
            }
            if (normalized[nearby] < normalized[best] + 0.025
                && normalized[nearby] < 0.15) best = nearby;
        }
    }
    double lag = static_cast<double>(best);
    if (best > 0 && best < maximum_lag) {
        const auto denominator = normalized[best - 1] - 2.0 * normalized[best]
            + normalized[best + 1];
        if (std::abs(denominator) > 1.0e-15) {
            lag += std::clamp(0.5 * (normalized[best - 1] - normalized[best + 1])
                / denominator, -0.5, 0.5);
        }
    }
    const auto frequency = analysis_rate / lag;
    if (frequency < options.minimum_pitch_hz || frequency > options.maximum_pitch_hz)
        return result;
    result.frequency_hz = frequency;
    result.confidence = std::clamp(1.0 - normalized[best], 0.0, 1.0);
    return result;
}

HarmonicTrajectoryFrame harmonicFrame(
    std::span<const float> pcm, SampleSelection selection, std::size_t centre,
    std::uint32_t rate, double f0, bool attack, const SourceAnalysisOptions& options) {
    HarmonicTrajectoryFrame frame;
    frame.sample_position = centre;
    std::size_t length = 256;
    const auto target = attack ? static_cast<double>(rate) * 0.012
        : (f0 > 0.0 ? static_cast<double>(rate) * 4.0 / f0 : rate * 0.04);
    while (length < target && length < 8192) length *= 2;
    std::vector<double> window(length);
    std::vector<std::complex<double>> spectrum(length);
    double weight_sum{}, mean{}, weight_energy{}, sample_energy{};
    const auto start = static_cast<std::int64_t>(centre) - static_cast<std::int64_t>(length / 2);
    std::size_t valid{};
    for (std::size_t i = 0; i < length; ++i) {
        const auto position = start + static_cast<std::int64_t>(i);
        if (position < static_cast<std::int64_t>(selection.begin)
            || position >= static_cast<std::int64_t>(selection.end)) continue;
        window[i] = 0.5 - 0.5 * std::cos(kTau * i / static_cast<double>(length - 1));
        mean += pcm[static_cast<std::size_t>(position)];
        ++valid;
    }
    if (valid == 0) return frame;
    mean /= valid;
    for (std::size_t i = 0; i < length; ++i) {
        if (window[i] == 0.0) continue;
        const auto sample = static_cast<double>(pcm[static_cast<std::size_t>(
            start + static_cast<std::int64_t>(i))]) - mean;
        spectrum[i] = sample * window[i];
        weight_sum += window[i];
        weight_energy += window[i] * window[i];
        sample_energy += std::norm(spectrum[i]);
    }
    if (weight_sum <= 1.0e-12 || weight_energy <= 1.0e-12) return frame;
    fft(spectrum);
    double total_power{}, weighted_frequency{};
    for (std::size_t bin = 1; bin < length / 2; ++bin) {
        const auto power = std::norm(spectrum[bin]);
        total_power += power;
        weighted_frequency += power * static_cast<double>(bin) * rate / length;
    }
    frame.spectral_centroid_hz = total_power > 1.0e-20 ? weighted_frequency / total_power : 0.0;
    double periodic_energy{};
    if (f0 > 0.0) {
        for (std::size_t order = 1; order <= options.maximum_harmonics; ++order) {
            if (cancelled(options.cancel_requested)) return frame;
            const auto expected = f0 * order;
            if (expected >= rate * 0.5) break;
            const auto expected_bin = expected * length / rate;
            const auto radius = std::min(2.0, f0 * 0.22 * length / rate);
            const auto low = std::max<std::size_t>(1,
                static_cast<std::size_t>(std::max(1.0, std::ceil(expected_bin - radius))));
            const auto high = std::min(length / 2 - 1,
                static_cast<std::size_t>(std::max(1.0, std::floor(expected_bin + radius))));
            double frequency = expected;
            if (low <= high) {
                auto best = low;
                for (auto bin = low + 1; bin <= high; ++bin)
                    if (std::norm(spectrum[bin]) > std::norm(spectrum[best])) best = bin;
                if (best > 0 && best + 1 < length / 2) {
                    const auto left = std::log(std::max(1.0e-24, std::abs(spectrum[best - 1])));
                    const auto middle = std::log(std::max(1.0e-24, std::abs(spectrum[best])));
                    const auto right = std::log(std::max(1.0e-24, std::abs(spectrum[best + 1])));
                    const auto denominator = left - 2.0 * middle + right;
                    const auto delta = std::abs(denominator) > 1.0e-12
                        ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5) : 0.0;
                    frequency = std::clamp((best + delta) * rate / length,
                        expected - f0 * 0.22, expected + f0 * 0.22);
                }
            }
            const auto step = std::polar(1.0, -kTau * frequency / rate);
            auto phase = std::polar(1.0, kTau * frequency * (length / 2) / rate);
            std::complex<double> projection{};
            for (std::size_t i = 0; i < length; ++i) {
                if (window[i] > 0.0) {
                    projection += (static_cast<double>(pcm[static_cast<std::size_t>(
                        start + static_cast<std::int64_t>(i))])
                        - mean) * window[i] * phase;
                }
                phase *= step;
            }
            const auto amplitude = 2.0 * std::abs(projection) / weight_sum;
            periodic_energy += amplitude * amplitude * 0.5;
            frame.harmonics.push_back({order, frequency, amplitude, std::arg(projection)});
        }
    }
    // Diagnostic only: overlapping Hann lobes/inharmonic transients make this
    // an energy remainder estimate, not an SMS residual waveform decomposition.
    frame.residual_rms = std::sqrt(std::max(0.0,
        sample_energy / weight_energy - periodic_energy));
    return frame;
}
} // namespace

bool parseCompositeWavePcm(std::span<const std::uint8_t> bytes, SourcePcm& output,
                          std::string* error,
                          const std::shared_ptr<std::atomic<bool>>& control) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (cancelled(control)) return fail("WAV decoding cancelled.");
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return fail("Not a RIFF/WAVE file.");
    const auto riff_size = static_cast<std::size_t>(u32(bytes, 4));
    if (riff_size < 4 || riff_size > bytes.size() - 8)
        return fail("Truncated RIFF/WAVE container.");
    const auto limit = riff_size + 8;
    std::span<const std::uint8_t> data;
    std::uint16_t format{}, channels{}, bits{}, alignment{};
    std::uint32_t rate{};
    bool format_found{}, data_found{};
    for (std::size_t offset = 12; offset + 8 <= limit;) {
        if (cancelled(control)) return fail("WAV decoding cancelled.");
        const auto size = static_cast<std::size_t>(u32(bytes, offset + 4));
        const auto begin = offset + 8;
        if (size > limit - begin) return fail("Truncated WAVE chunk.");
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0) {
            if (format_found || size < 16) return fail("Invalid WAVE format chunk.");
            format_found = true;
            format = u16(bytes, begin);
            channels = u16(bytes, begin + 2);
            rate = u32(bytes, begin + 4);
            alignment = u16(bytes, begin + 12);
            bits = u16(bytes, begin + 14);
            if (format == 0xfffe) {
                constexpr std::array<std::uint8_t, 12> guid_tail{
                    0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                if (size < 40 || u16(bytes, begin + 16) < 22
                    || u16(bytes, begin + 16) > size - 18
                    || !std::equal(guid_tail.begin(), guid_tail.end(), bytes.begin() + begin + 28))
                    return fail("Unsupported extensible WAVE subformat.");
                const auto valid_bits = u16(bytes, begin + 18);
                if (valid_bits != 0 && valid_bits != bits)
                    return fail("Packed extensible WAVE bit depths are unsupported.");
                const auto subtype = u32(bytes, begin + 24);
                if (subtype != 1 && subtype != 3)
                    return fail("Unsupported extensible WAVE subformat.");
                format = static_cast<std::uint16_t>(subtype);
            }
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            if (data_found) return fail("Multiple WAVE data chunks are unsupported.");
            data_found = true;
            data = bytes.subspan(begin, size);
        }
        offset = begin + size;
        if ((size & 1U) != 0 && offset < limit) ++offset;
    }
    if (!format_found || !data_found || channels == 0 || channels > 32
        || rate == 0 || rate > 384000 || data.empty()
        || (format != 1 && format != 3)
        || (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        || (format == 3 && bits != 32))
        return fail("Unsupported or incomplete PCM WAVE format.");
    const auto bytes_per_sample = bits / 8;
    if (alignment < channels * bytes_per_sample || alignment == 0
        || data.size() % alignment != 0) return fail("Invalid WAVE block alignment.");
    const auto frames = data.size() / alignment;
    if (frames < 2 || frames > static_cast<std::uint64_t>(rate) * 60
        || frames > kMaximumDecodedSamples / channels)
        return fail("WAVE exceeds analysis limits (60 seconds / 16 million channel samples).");
    SourcePcm decoded;
    decoded.sample_rate = rate;
    decoded.channels = channels;
    decoded.bit_depth = bits;
    decoded.sample_format = format == 3 ? SourceSampleFormat::FloatPcm : SourceSampleFormat::IntegerPcm;
    decoded.interleaved_samples.resize(frames * channels);
    decoded.mono_samples.resize(frames);
    std::vector<double> channel_energy(channels, 0.0);
    double average_energy{};
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if ((frame & 4095U) == 0 && cancelled(control)) return fail("WAV decoding cancelled.");
        double sum{};
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto value = decode(data, frame * alignment + channel * bytes_per_sample, format, bits);
            if (!std::isfinite(value) || std::abs(static_cast<double>(value)) > 1.0e6)
                return fail("Nonfinite or excessive floating-point WAVE sample.");
            decoded.interleaved_samples[frame * channels + channel] = value;
            channel_energy[channel] += static_cast<double>(value) * value;
            sum += value;
        }
        decoded.mono_samples[frame] = static_cast<float>(sum / channels);
        average_energy += (sum / channels) * (sum / channels);
    }
    const auto strongest = static_cast<std::size_t>(std::max_element(
        channel_energy.begin(), channel_energy.end()) - channel_energy.begin());
    if (channels > 1 && average_energy < channel_energy[strongest] * 0.25) {
        decoded.mono_strategy = AnalysisMonoStrategy::StrongestChannel;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            if ((frame & 4095U) == 0 && cancelled(control)) return fail("WAV decoding cancelled.");
            decoded.mono_samples[frame] = decoded.interleaved_samples[frame * channels + strongest];
        }
    }
    output = std::move(decoded);
    if (error) error->clear();
    return true;
}

SourceAnalysisResult analyzeCompositeWaveSource(std::shared_ptr<const SourcePcm> source,
    SampleSelection selection, const SourceAnalysisOptions& options) {
    const auto invalid = [](const char* message) {
        return SourceAnalysisResult{SourceAnalysisCompletion::InvalidInput, {}, message};
    };
    const auto stopped = [] {
        return SourceAnalysisResult{SourceAnalysisCompletion::Cancelled, {}, {}};
    };
    if (cancelled(options.cancel_requested)) return stopped();
    if (!source || source->sample_rate == 0 || source->sample_rate > 384000
        || source->channels == 0 || source->channels > 32
        || source->mono_samples.size() > kMaximumDecodedSamples / source->channels
        || source->mono_samples.size() > static_cast<std::uint64_t>(source->sample_rate) * 60
        || source->interleaved_samples.size() != source->mono_samples.size() * source->channels
        || selection.begin >= selection.end || selection.end > source->mono_samples.size())
        return invalid("Invalid source PCM or end-exclusive selection.");
    if (!std::isfinite(options.minimum_pitch_hz) || !std::isfinite(options.maximum_pitch_hz)
        || options.minimum_pitch_hz < 20 || options.maximum_pitch_hz <= options.minimum_pitch_hz
        || options.maximum_pitch_hz > std::min(4000.0, source->sample_rate * 0.25)
        || !std::isfinite(options.hop_seconds) || options.hop_seconds < 0.005
        || options.hop_seconds > 0.1 || options.maximum_harmonics == 0
        || options.maximum_harmonics > 64
        || (options.reference_pitch_hz && (!std::isfinite(*options.reference_pitch_hz)
            || *options.reference_pitch_hz < options.minimum_pitch_hz
            || *options.reference_pitch_hz > options.maximum_pitch_hz)))
        return invalid("Invalid or excessive source analysis options.");
    auto result = std::make_shared<SourceAnalysis>();
    result->source = std::move(source);
    result->selection = selection;
    const auto rate = result->source->sample_rate;
    const auto channels = result->source->channels;
    const auto hop = std::max<std::size_t>(1,
        static_cast<std::size_t>(std::lround(rate * options.hop_seconds)));
    double original_energy{}, average_energy{};
    std::vector<double> selected_channel_energy(channels, 0.0);
    for (std::size_t frame = selection.begin; frame < selection.end; ++frame) {
        if ((frame & 4095U) == 0 && cancelled(options.cancel_requested)) return stopped();
        if (!std::isfinite(result->source->mono_samples[frame])
            || std::abs(static_cast<double>(result->source->mono_samples[frame])) > 1.0e6)
            return invalid("Nonfinite or excessive analysis PCM sample.");
        double sum{};
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto value = static_cast<double>(result->source->interleaved_samples[frame * channels + channel]);
            if (!std::isfinite(value) || std::abs(value) > 1.0e6)
                return invalid("Nonfinite or excessive original PCM sample.");
            original_energy += value * value;
            selected_channel_energy[channel] += value * value;
            sum += value;
            result->metadata.peak = std::max(result->metadata.peak, std::abs(value));
            const auto positive_full_scale = result->source->sample_format == SourceSampleFormat::IntegerPcm
                && result->source->bit_depth >= 8 && result->source->bit_depth <= 32
                ? 1.0 - std::ldexp(1.0, 1 - result->source->bit_depth) : 1.0;
            if (value <= -1.0 || value >= positive_full_scale) ++result->metadata.clipped_samples;
        }
        average_energy += (sum / channels) * (sum / channels);
    }
    const auto strongest = static_cast<std::size_t>(std::max_element(
        selected_channel_energy.begin(), selected_channel_energy.end()) - selected_channel_energy.begin());
    result->mono_strategy = channels > 1 && average_energy < selected_channel_energy[strongest] * 0.25
        ? AnalysisMonoStrategy::StrongestChannel : AnalysisMonoStrategy::Average;
    // Re-evaluate the actual selected region, including a different strongest
    // channel or an in-phase region of a globally anti-phase recording.
    if (channels > 1) {
        result->analysis_mono_override = result->source->mono_samples;
        for (auto frame = selection.begin; frame < selection.end; ++frame) {
            if ((frame & 4095U) == 0 && cancelled(options.cancel_requested)) return stopped();
            if (result->mono_strategy == AnalysisMonoStrategy::StrongestChannel) {
                result->analysis_mono_override[frame] = result->source->interleaved_samples[frame * channels + strongest];
            } else {
                double sum{};
                for (std::size_t channel = 0; channel < channels; ++channel)
                    sum += result->source->interleaved_samples[frame * channels + channel];
                result->analysis_mono_override[frame] = static_cast<float>(sum / channels);
            }
        }
    }
    const auto pcm = result->analysisPcm();
    result->metadata.duration_seconds = static_cast<double>(selection.end - selection.begin) / rate;
    result->metadata.rms = std::sqrt(original_energy / (selection.end - selection.begin) / channels);
    std::optional<std::size_t> silence_start;
    double previous_pitch{};
    std::vector<double> voiced_pitches;
    double confidence_sum{};
    for (auto position = selection.begin; position < selection.end;) {
        if (cancelled(options.cancel_requested)) return stopped();
        const auto end = position + std::min(hop, selection.end - position);
        double energy{}, peak{};
        for (auto i = position; i < end; ++i) {
            energy += static_cast<double>(pcm[i]) * pcm[i];
            peak = std::max(peak, std::abs(static_cast<double>(pcm[i])));
        }
        const auto rms = std::sqrt(energy / (end - position));
        const auto centre = position + (end - position) / 2;
        result->amplitude_envelope.push_back({centre, rms, peak});
        if (rms <= kSilence) { if (!silence_start) silence_start = position; }
        else if (silence_start) {
            result->metadata.silent_ranges.push_back({*silence_start, position});
            silence_start.reset();
        }
        auto pitch = pitchFrame(pcm, selection, centre, rate, options, previous_pitch);
        if (cancelled(options.cancel_requested)) return stopped();
        if (rms <= kSilence) pitch = {centre, 0.0, 0.0};
        if (pitch.frequency_hz > 0.0) {
            voiced_pitches.push_back(pitch.frequency_hz);
            confidence_sum += pitch.confidence;
            previous_pitch = pitch.frequency_hz;
        } else previous_pitch = 0.0;
        result->pitch_trajectory.push_back(pitch);
        position = end;
    }
    if (silence_start) result->metadata.silent_ranges.push_back({*silence_start, selection.end});
    if (!voiced_pitches.empty()) {
        std::sort(voiced_pitches.begin(), voiced_pitches.end());
        result->reference_pitch_hz = voiced_pitches[voiced_pitches.size() / 2];
        result->confidence = confidence_sum / result->pitch_trajectory.size();
    }
    if (options.reference_pitch_hz) result->reference_pitch_hz = *options.reference_pitch_hz;
    if (result->confidence < 0.65)
        result->warnings.emplace_back("Pitch is uncertain; check the reference pitch manually.");
    if (result->mono_strategy == AnalysisMonoStrategy::StrongestChannel)
        result->warnings.emplace_back("Channel averaging caused cancellation; strongest channel used for analysis.");
    if (result->metadata.clipped_samples > 0)
        result->warnings.emplace_back("Source contains full-scale/clipped samples.");

    const auto& envelope = result->amplitude_envelope;
    const auto peak_it = std::max_element(envelope.begin(), envelope.end(),
        [](const auto& a, const auto& b) { return a.rms < b.rms; });
    const auto peak_index = static_cast<std::size_t>(peak_it - envelope.begin());
    const auto threshold = std::max(kSilence, peak_it->rms * 0.05);
    std::size_t onset{};
    while (onset < peak_index && envelope[onset].rms < threshold) ++onset;
    // Initial local maximum separates attack from later swells.
    auto attack_end = onset;
    while (attack_end < peak_index && attack_end + 1 < envelope.size()
        && envelope[attack_end + 1].rms >= envelope[attack_end].rms * 0.98) ++attack_end;
    result->attack_region = {{std::max(selection.begin,
        envelope[onset].sample_position > hop / 2 ? envelope[onset].sample_position - hop / 2 : selection.begin),
        std::min(selection.end, envelope[attack_end].sample_position + hop / 2 + 1)},
        peak_it->rms > kSilence ? 0.6 : 0.0};
    for (std::size_t i = 0; i < result->pitch_trajectory.size(); ++i) {
        if (cancelled(options.cancel_requested)) return stopped();
        const auto& pitch = result->pitch_trajectory[i];
        const auto f0 = pitch.frequency_hz > 0.0 ? pitch.frequency_hz
            : (options.reference_pitch_hz ? *options.reference_pitch_hz : 0.0);
        result->harmonic_trajectory.push_back(harmonicFrame(pcm, selection,
            pitch.sample_position, rate, f0,
            pitch.sample_position < result->attack_region.selection.end, options));
        if (cancelled(options.cancel_requested)) return stopped();
    }
    // Hop RMS includes phase-dependent ripple when a window contains a
    // noninteger number of periods. Smooth energy only for region detection;
    // retain the original hop envelope for 60Hz control and attack analysis.
    const auto sustainRms = [&](std::size_t index) {
        const auto first = index > 2 ? index - 2 : 0;
        const auto last = std::min(envelope.size(), index + 3);
        double energy{};
        for (auto frame = first; frame < last; ++frame)
            energy += envelope[frame].rms * envelope[frame].rms;
        return std::sqrt(energy / (last - first));
    };
    // Sustain requires amplitude AND normalized harmonic stability for >=100 ms.
    std::size_t best_begin{}, best_end{}, run_begin = attack_end + 1;
    for (std::size_t i = attack_end + 2; i < envelope.size(); ++i) {
        const auto ratio = sustainRms(i) / std::max(kSilence, sustainRms(i - 1));
        const auto& a = result->harmonic_trajectory[i - 1].harmonics;
        const auto& b = result->harmonic_trajectory[i].harmonics;
        double a_sum{}, b_sum{}, change{};
        for (const auto& h : a) a_sum += h.amplitude;
        for (const auto& h : b) b_sum += h.amplitude;
        for (std::size_t j = 0; j < std::min(a.size(), b.size()); ++j)
            change += std::abs(a[j].amplitude / std::max(1.0e-12, a_sum)
                - b[j].amplitude / std::max(1.0e-12, b_sum));
        const bool stable = sustainRms(i) > threshold && ratio > 0.94 && ratio < 1.06
            && !a.empty() && a.size() == b.size() && change < 0.15;
        if (!stable) run_begin = i;
        if (stable && i - run_begin > best_end - best_begin) {
            best_begin = run_begin; best_end = i;
        }
    }
    if (best_end > best_begin && (best_end - best_begin) * hop >= rate / 10) {
        result->sustain_region = EstimatedSourceRegion{{
            std::max(selection.begin, envelope[best_begin].sample_position - hop / 2),
            std::min(selection.end, envelope[best_end].sample_position + hop / 2 + 1)}, 0.65};
        // A sustained amplitude followed by several falling hops can indicate a
        // release. The acoustic evidence cannot establish the physical keyoff.
        const auto search_begin = std::max(best_begin + 1, attack_end + 1);
        for (auto i = search_begin; i + 3 < envelope.size(); ++i) {
            if (envelope[i].rms > threshold
                && envelope[i + 1].rms < envelope[i].rms * 0.96
                && envelope[i + 2].rms < envelope[i + 1].rms * 0.96
                && envelope[i + 3].rms < envelope[i + 2].rms * 0.96
                && envelope.back().rms < envelope[i].rms * 0.5) {
                result->estimated_key_off = EstimatedKeyOff{envelope[i].sample_position, 0.45};
                break;
            }
        }
    }
    result->warnings.emplace_back(result->estimated_key_off
        ? "Keyoff is an acoustic heuristic with low confidence; adjust manually."
        : "Keyoff cannot be established from this WAV; set it manually.");
    result->warnings.emplace_back("Harmonic/residual analysis is a bounded estimate; noise and inharmonic detail may be unresolved.");
    return {SourceAnalysisCompletion::Completed, std::move(result), {}};
}

} // namespace mgstc::engine
