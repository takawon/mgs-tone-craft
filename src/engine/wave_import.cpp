#include "mgstc/engine/wave_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "emu2413.h"
#include "mgstc/engine/note_pitch.hpp"

namespace mgstc::engine {

struct OpllApproximationCoordinator {
    explicit OpllApproximationCoordinator(
        const OpllApproximationOptions& options,
        std::uint64_t total)
        : control(options.control
              ? options.control
              : std::make_shared<OpllApproximationControl>()) {
        control->state_->completed.store(0, std::memory_order_relaxed);
        control->state_->total.store(total, std::memory_order_release);
        control->state_->phase.store(
            OpllApproximationPhase::Idle,
            std::memory_order_release);
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return control->state_->cancel_requested.load(
            std::memory_order_acquire);
    }

    void setPhase(OpllApproximationPhase phase) noexcept {
        control->state_->phase.store(phase, std::memory_order_release);
    }

    void advance(std::uint64_t work) noexcept {
        const auto total =
            control->state_->total.load(std::memory_order_relaxed);
        const auto previous =
            control->state_->completed.load(std::memory_order_relaxed);
        control->state_->completed.store(
            std::min(total, previous + work),
            std::memory_order_release);
    }

    void finish(bool was_cancelled) noexcept {
        if (was_cancelled) {
            control->state_->phase.store(
                OpllApproximationPhase::Cancelled,
                std::memory_order_release);
            return;
        }
        const auto total =
            control->state_->total.load(std::memory_order_relaxed);
        control->state_->completed.store(total, std::memory_order_release);
        control->state_->phase.store(
            OpllApproximationPhase::Completed,
            std::memory_order_release);
    }

    std::shared_ptr<OpllApproximationControl> control;
};

std::vector<OpllPatchParameters> searchWaveCycleWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    std::size_t evaluation_budget,
    OpllApproximationPhase phase);

namespace {

constexpr std::size_t kSteadyBatchSize = 256;
constexpr std::size_t kFullWaveBatchSize = 32;
constexpr std::size_t kStandardSteadyBatchSize = 5'000;
constexpr std::size_t kStandardFullWaveBatchSize = 360;

struct SearchBudgets {
    std::size_t representative{};
    std::size_t short_timbre{};
    std::size_t full_envelope{};
};

SearchBudgets searchBudgets(const OpllApproximationOptions& options) {
    if (options.profile == OpllApproximationProfile::Compact) {
        return SearchBudgets{384, 192, 16};
    }
    return options.effort == OpllApproximationEffort::Thorough
        ? SearchBudgets{48'000, 48'000, 6'000}
        : SearchBudgets{5'000, 2'100, 360};
}

std::uint64_t fullWaveWeight(std::size_t samples) {
    return 1U + static_cast<std::uint64_t>((samples + 4'095U) / 4'096U);
}

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        bytes[offset]
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(
        bytes[offset]
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24));
}

float normalizedSample(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::uint16_t format,
    std::uint16_t bits) {
    if (format == 3 && bits == 32) {
        float value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
    }
    switch (bits) {
    case 8:
        return (static_cast<float>(bytes[offset]) - 128.0F) / 128.0F;
    case 16: {
        const auto value = static_cast<std::int16_t>(u16(bytes, offset));
        return static_cast<float>(value) / 32768.0F;
    }
    case 24: {
        std::int32_t value = static_cast<std::int32_t>(
            bytes[offset]
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16));
        if (value & 0x00800000) {
            value |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<float>(value) / 8388608.0F;
    }
    case 32: {
        const auto value = static_cast<std::int32_t>(u32(bytes, offset));
        return static_cast<float>(value) / 2147483648.0F;
    }
    default:
        return 0.0F;
    }
}

std::vector<float> normalizedCycle(std::span<const float> input) {
    if (input.empty()) {
        return {};
    }
    std::vector<float> result(input.begin(), input.end());
    float mean{};
    for (const float sample : result) {
        mean += sample;
    }
    mean /= static_cast<float>(result.size());
    float peak{};
    for (float& sample : result) {
        sample -= mean;
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 0.000001F) {
        for (float& sample : result) {
            sample /= peak;
        }
    }
    return result;
}

float periodicDifference(
    std::span<const float> samples,
    std::size_t lag) {
    double error{};
    double energy{};
    const std::size_t count = samples.size() - lag;
    for (std::size_t index = 0; index < count; ++index) {
        const double a = samples[index];
        const double b = samples[index + lag];
        const double difference = a - b;
        error += difference * difference;
        energy += a * a + b * b;
    }
    return energy > 1.0e-12
        ? static_cast<float>(error / energy)
        : std::numeric_limits<float>::max();
}

constexpr std::uint32_t kChipClock = 3'579'545;
constexpr std::uint32_t kAuditionSampleRate = 48'000;
constexpr std::uint8_t kAuditionMidiNote = 60;
constexpr std::size_t kAuditionWarmup = 1'024;
constexpr std::size_t kAuditionSamples = 4'096;

void removeDcAndNormalize(std::vector<float>& samples);

float sccFrequencyHz(std::uint16_t period) noexcept {
    return static_cast<float>(
        static_cast<double>(kChipClock)
        / ((static_cast<double>(period) + 1.0) * 32.0));
}

std::array<float, 16> spectrum(std::span<const float> cycle) {
    std::array<float, 16> result{};
    if (cycle.empty()) {
        return result;
    }
    for (std::size_t harmonic = 1; harmonic < result.size(); ++harmonic) {
        double real{};
        double imaginary{};
        for (std::size_t index = 0; index < cycle.size(); ++index) {
            const double angle =
                2.0 * std::numbers::pi
                * static_cast<double>(harmonic * index)
                / static_cast<double>(cycle.size());
            real += static_cast<double>(cycle[index]) * std::cos(angle);
            imaginary -= static_cast<double>(cycle[index]) * std::sin(angle);
        }
        result[harmonic] = static_cast<float>(
            std::sqrt(real * real + imaginary * imaginary));
    }
    float total{};
    for (const float value : result) {
        total += value;
    }
    if (total > 0.000001F) {
        for (float& value : result) {
            value /= total;
        }
    }
    return result;
}

std::array<float, 32> resampleCycle32(
    std::span<const float> cycle) {
    std::array<float, 32> result{};
    const auto normalized = normalizedCycle(cycle);
    if (normalized.empty()) {
        return result;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * normalized.size() / result.size();
        const auto first =
            static_cast<std::size_t>(position) % normalized.size();
        const auto second = (first + 1) % normalized.size();
        const float fraction =
            static_cast<float>(position - std::floor(position));
        result[index] =
            normalized[first] * (1.0F - fraction)
            + normalized[second] * fraction;
    }
    return result;
}

std::array<float, 32> resampleWindow32(
    std::span<const float> samples,
    std::size_t begin,
    std::size_t length) {
    std::array<float, 32> result{};
    if (length < 2 || begin + length > samples.size()) {
        return result;
    }
    double mean{};
    for (std::size_t index = 0; index < length; ++index) {
        mean += samples[begin + index];
    }
    mean /= static_cast<double>(length);
    float peak{};
    for (std::size_t index = 0; index < length; ++index) {
        peak = std::max(
            peak,
            std::abs(samples[begin + index] - static_cast<float>(mean)));
    }
    const float scale = peak > 1.0e-6F ? 1.0F / peak : 1.0F;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * length / result.size();
        const auto first = static_cast<std::size_t>(position) % length;
        const auto second = (first + 1) % length;
        const float fraction =
            static_cast<float>(position - std::floor(position));
        const float a =
            (samples[begin + first] - static_cast<float>(mean)) * scale;
        const float b =
            (samples[begin + second] - static_cast<float>(mean)) * scale;
        result[index] = a * (1.0F - fraction) + b * fraction;
    }
    return result;
}

struct OpllDeleter {
    void operator()(OPLL* chip) const noexcept {
        OPLL_delete(chip);
    }
};

std::vector<float> synthesizePeriodicAudition(
    std::span<const float> cycle,
    float frequency_hz,
    std::size_t sample_count) {
    std::vector<float> result(sample_count, 0.0F);
    if (cycle.size() < 2 || !(frequency_hz > 0.0F)) {
        return result;
    }
    double phase = 0.0;
    const double step =
        static_cast<double>(frequency_hz) * static_cast<double>(cycle.size())
        / static_cast<double>(kAuditionSampleRate);
    for (float& sample : result) {
        const auto first =
            static_cast<std::size_t>(phase) % cycle.size();
        const auto second = (first + 1) % cycle.size();
        const float fraction = static_cast<float>(
            phase - std::floor(phase));
        sample = cycle[first] * (1.0F - fraction)
            + cycle[second] * fraction;
        phase += step;
        while (phase >= static_cast<double>(cycle.size())) {
            phase -= static_cast<double>(cycle.size());
        }
    }
    removeDcAndNormalize(result);
    return result;
}

float cyclePairDistance(
    const std::array<float, 32>& target_cycle,
    const std::array<float, 32>& generated_cycle) {
    const auto target_spectrum = spectrum(target_cycle);
    const auto generated_spectrum = spectrum(generated_cycle);
    float distance{};
    float cumulative{};
    float wasserstein{};
    for (std::size_t index = 1; index < target_spectrum.size(); ++index) {
        const float weight = 1.0F / std::sqrt(static_cast<float>(index));
        const float difference =
            std::log1p(target_spectrum[index] * 40.0F)
            - std::log1p(generated_spectrum[index] * 40.0F);
        distance += weight * difference * difference;
        cumulative += target_spectrum[index] - generated_spectrum[index];
        wasserstein += std::abs(cumulative);
    }
    distance += wasserstein * 1.6F;
    float best_shape = std::numeric_limits<float>::max();
    for (std::size_t shift = 0; shift < target_cycle.size(); ++shift) {
        float normal_error{};
        float inverted_error{};
        for (std::size_t index = 0; index < target_cycle.size(); ++index) {
            const float generated =
                generated_cycle[(index + shift) % generated_cycle.size()];
            const float difference = target_cycle[index] - generated;
            const float inverted = target_cycle[index] + generated;
            normal_error += difference * difference;
            inverted_error += inverted * inverted;
        }
        best_shape = std::min(
            best_shape,
            std::min(normal_error, inverted_error)
                / static_cast<float>(target_cycle.size()));
    }
    distance += best_shape * 0.45F;
    return distance;
}

// Compare target vs generated using one SCC-period-length window. An octave
// error packs 2 (or 1/2) cycles into that window and scores poorly.
float periodWindowDistance(
    std::span<const float> target,
    std::span<const float> generated,
    std::size_t period_samples) {
    if (target.size() < period_samples * 2
        || generated.size() < period_samples * 2
        || period_samples < 8) {
        return 0.0F;
    }
    const std::size_t target_begin =
        (target.size() - period_samples) / 2;
    const auto target_cycle =
        resampleWindow32(target, target_begin, period_samples);
    const std::size_t generated_begin =
        (generated.size() - period_samples) / 2;
    return cyclePairDistance(
        target_cycle,
        resampleWindow32(generated, generated_begin, period_samples));
}

constexpr std::uint32_t kOpllClock = kChipClock;
constexpr std::uint32_t kOpllFitSampleRate = kAuditionSampleRate;
constexpr std::size_t kEnvelopeBins = 32;
constexpr std::size_t kSpectralBins = 80;
constexpr std::size_t kSpectralFrames = 3;
constexpr std::array<std::size_t, 3> kFftSizes{256, 1024, 4096};

using SpectralDistribution = std::array<float, kSpectralBins>;

struct TimedAudioFeatures {
    std::array<float, kEnvelopeBins> envelope{};
    std::array<
        SpectralDistribution,
        kFftSizes.size() * kSpectralFrames> spectra{};
};

struct TimedFeatureWorkspace {
    std::vector<std::complex<float>> fft_values;
};

struct OpllFitTarget {
    std::vector<float> samples;
    TimedAudioFeatures features;
    float fundamental_hz{261.625565F};
    std::size_t key_off_sample{std::numeric_limits<std::size_t>::max()};
    std::size_t render_offset_sample{};
};

OpllPitch closestOpllPitch(float frequency_hz) noexcept {
    OpllPitch best{1, 0};
    double best_error = std::numeric_limits<double>::max();
    const double target = std::max(1.0, static_cast<double>(frequency_hz));
    for (std::uint8_t block = 0; block < 8; ++block) {
        const double scale =
            static_cast<double>(kOpllClock)
            / (72.0 * static_cast<double>(1U << (19 - block)));
        const auto f_number = static_cast<std::uint16_t>(std::clamp(
            static_cast<int>(std::lround(target / scale)),
            1,
            511));
        const double rendered = static_cast<double>(f_number) * scale;
        const double error = std::abs(std::log2(rendered / target));
        if (error < best_error) {
            best_error = error;
            best = OpllPitch{f_number, block};
        }
    }
    return best;
}

std::vector<float> resampleAudio(
    std::span<const float> input,
    std::uint32_t source_rate,
    std::uint32_t destination_rate) {
    if (input.empty() || source_rate == 0 || destination_rate == 0) {
        return {};
    }
    const std::size_t size = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::llround(
                static_cast<double>(input.size()) * destination_rate
                / source_rate)));
    std::vector<float> result(size);
    for (std::size_t index = 0; index < size; ++index) {
        const double position =
            static_cast<double>(index) * source_rate / destination_rate;
        const auto first = std::min(
            static_cast<std::size_t>(position),
            input.size() - 1);
        const auto second = std::min(first + 1, input.size() - 1);
        const float fraction =
            static_cast<float>(position - static_cast<double>(first));
        result[index] =
            input[first] * (1.0F - fraction) + input[second] * fraction;
    }
    return result;
}

void removeDcAndNormalize(std::vector<float>& samples) {
    if (samples.empty()) {
        return;
    }
    double mean{};
    for (const float sample : samples) {
        mean += sample;
    }
    mean /= static_cast<double>(samples.size());
    float peak{};
    for (float& sample : samples) {
        sample -= static_cast<float>(mean);
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 1.0e-6F) {
        for (float& sample : samples) {
            sample /= peak;
        }
    }
}

std::array<float, kEnvelopeBins> amplitudeEnvelope(
    std::span<const float> samples) {
    std::array<float, kEnvelopeBins> result{};
    if (samples.empty()) {
        return result;
    }
    for (std::size_t bin = 0; bin < result.size(); ++bin) {
        const std::size_t begin = bin * samples.size() / result.size();
        const std::size_t end = std::max(
            begin + 1,
            (bin + 1) * samples.size() / result.size());
        double squared{};
        for (std::size_t index = begin;
             index < std::min(end, samples.size());
             ++index) {
            squared += static_cast<double>(samples[index]) * samples[index];
        }
        result[bin] = static_cast<float>(
            std::sqrt(squared / static_cast<double>(end - begin)));
    }
    const float maximum =
        *std::max_element(result.begin(), result.end());
    if (maximum > 1.0e-6F) {
        for (float& value : result) {
            value /= maximum;
        }
    }
    return result;
}

void fft(std::vector<std::complex<float>>& values) {
    const std::size_t size = values.size();
    for (std::size_t index = 1, reversed = 0; index < size; ++index) {
        std::size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const float angle =
            -2.0F * std::numbers::pi_v<float>
            / static_cast<float>(length);
        const std::complex<float> root{
            std::cos(angle),
            std::sin(angle)};
        for (std::size_t offset = 0; offset < size; offset += length) {
            std::complex<float> factor{1.0F, 0.0F};
            for (std::size_t index = 0; index < length / 2; ++index) {
                const auto even = values[offset + index];
                const auto odd =
                    values[offset + index + length / 2] * factor;
                values[offset + index] = even + odd;
                values[offset + index + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
}

const std::vector<float>& hannWindow(std::size_t fft_size) {
    static const auto windows = [] {
        std::array<std::vector<float>, kFftSizes.size()> result;
        for (std::size_t size_index = 0;
             size_index < kFftSizes.size();
             ++size_index) {
            const std::size_t size = kFftSizes[size_index];
            result[size_index].resize(size);
            for (std::size_t index = 0; index < size; ++index) {
                result[size_index][index] =
                    0.5F
                    - 0.5F * std::cos(
                        2.0F * std::numbers::pi_v<float>
                        * static_cast<float>(index)
                        / static_cast<float>(size - 1));
            }
        }
        return result;
    }();
    for (std::size_t index = 0; index < kFftSizes.size(); ++index) {
        if (kFftSizes[index] == fft_size) {
            return windows[index];
        }
    }
    return windows.back();
}

SpectralDistribution harmonicDistribution(
    std::span<const float> samples,
    std::size_t center,
    std::size_t fft_size,
    float fundamental_hz,
    TimedFeatureWorkspace& workspace) {
    workspace.fft_values.assign(
        fft_size,
        std::complex<float>{});
    auto& values = workspace.fft_values;
    const auto& window_values = hannWindow(fft_size);
    const std::size_t half = fft_size / 2;
    const std::size_t begin = center > half ? center - half : 0;
    for (std::size_t index = 0; index < fft_size; ++index) {
        const std::size_t source = begin + index;
        const float sample = source < samples.size() ? samples[source] : 0.0F;
        values[index] = sample * window_values[index];
    }
    fft(values);

    SpectralDistribution result{};
    constexpr float harmonic_step = 0.25F;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const float frequency =
            fundamental_hz * harmonic_step * static_cast<float>(index + 1);
        const float fft_position =
            frequency * static_cast<float>(fft_size)
            / static_cast<float>(kOpllFitSampleRate);
        if (fft_position >= static_cast<float>(fft_size / 2 - 1)) {
            break;
        }
        const auto first = static_cast<std::size_t>(fft_position);
        const float fraction =
            fft_position - static_cast<float>(first);
        result[index] =
            std::abs(values[first]) * (1.0F - fraction)
            + std::abs(values[first + 1]) * fraction;
    }
    float total{};
    for (const float value : result) {
        total += value;
    }
    if (total > 1.0e-6F) {
        for (float& value : result) {
            value /= total;
        }
    }
    return result;
}

TimedAudioFeatures timedFeatures(
    std::span<const float> samples,
    float fundamental_hz,
    TimedFeatureWorkspace& workspace) {
    TimedAudioFeatures result{};
    result.envelope = amplitudeEnvelope(samples);
    std::size_t output{};
    for (const std::size_t fft_size : kFftSizes) {
        for (std::size_t frame = 0; frame < kSpectralFrames; ++frame) {
            const std::size_t center =
                samples.empty()
                    ? 0
                    : (frame + 1) * samples.size()
                        / (kSpectralFrames + 1);
            result.spectra[output++] = harmonicDistribution(
                samples,
                center,
                fft_size,
                fundamental_hz,
                workspace);
        }
    }
    return result;
}

TimedAudioFeatures timedFeatures(
    std::span<const float> samples,
    float fundamental_hz) {
    TimedFeatureWorkspace workspace;
    return timedFeatures(samples, fundamental_hz, workspace);
}

float wassersteinDistance(
    const SpectralDistribution& target,
    const SpectralDistribution& generated) {
    float cumulative{};
    float distance{};
    for (std::size_t index = 0; index < target.size(); ++index) {
        cumulative += target[index] - generated[index];
        distance += std::abs(cumulative) * 0.25F;
    }
    return distance;
}

float featureDistance(
    const TimedAudioFeatures& target,
    const TimedAudioFeatures& generated) {
    float spectral{};
    for (std::size_t frame = 0; frame < target.spectra.size(); ++frame) {
        spectral +=
            wassersteinDistance(
                target.spectra[frame],
                generated.spectra[frame])
            * 2.4F;
        for (std::size_t bin = 0; bin < kSpectralBins; ++bin) {
            const float difference =
                std::log1p(target.spectra[frame][bin] * 80.0F)
                - std::log1p(generated.spectra[frame][bin] * 80.0F);
            spectral += std::abs(difference) * 0.025F;
        }
    }
    spectral /= static_cast<float>(target.spectra.size());

    float envelope{};
    for (std::size_t index = 0; index < target.envelope.size(); ++index) {
        const float difference =
            std::log1p(target.envelope[index] * 20.0F)
            - std::log1p(generated.envelope[index] * 20.0F);
        envelope += difference * difference;
    }
    envelope /= static_cast<float>(target.envelope.size());
    return spectral + envelope * 0.7F;
}

class SteadyOpllRenderer {
public:
    SteadyOpllRenderer()
        : chip_(OPLL_new(kOpllClock, kOpllFitSampleRate)) {}

    const std::vector<float>& render(
        const OpllPatchParameters& patch,
        OpllPitch pitch,
        std::size_t sample_count) {
        result_.assign(sample_count, 0.0F);
        if (!chip_) {
            return result_;
        }
        OPLL_reset(chip_.get());
        OPLL_setChipType(chip_.get(), 0);
        OPLL_resetPatch(chip_.get(), OPLL_2413_TONE);
        const auto registers = encodeOpllPatch(patch);
        for (std::size_t index = 0; index < registers.size(); ++index) {
            OPLL_writeReg(
                chip_.get(),
                static_cast<std::uint32_t>(index),
                registers[index]);
        }
        OPLL_writeReg(
            chip_.get(),
            0x10,
            static_cast<std::uint8_t>(pitch.f_number & 0xFF));
        OPLL_writeReg(chip_.get(), 0x30, 0x00);
        OPLL_writeReg(
            chip_.get(),
            0x20,
            static_cast<std::uint8_t>(
                ((pitch.f_number >> 8) & 1)
                | ((pitch.block & 7) << 1)
                | 0x10));
        for (std::size_t index = 0;
             index < kAuditionWarmup + sample_count;
             ++index) {
            const float sample = static_cast<float>(OPLL_calc(chip_.get()));
            if (index >= kAuditionWarmup) {
                result_[index - kAuditionWarmup] = sample;
            }
        }
        removeDcAndNormalize(result_);
        return result_;
    }

private:
    std::unique_ptr<OPLL, OpllDeleter> chip_;
    std::vector<float> result_;
};

// Score OPLL patches by rendering at the same MIDI note as the SCC/cycle
// reference and comparing both broadband features and one SCC-period window.
class SameMidiScoreContext {
public:
    SameMidiScoreContext(
        std::span<const float> cycle,
        float fundamental_hz,
        std::uint8_t midi_note)
        : fundamental_hz_(fundamental_hz),
          period_samples_(std::clamp<std::size_t>(
              static_cast<std::size_t>(std::llround(
                  static_cast<double>(kAuditionSampleRate)
                  / std::max(static_cast<double>(fundamental_hz), 1.0))),
              16,
              kAuditionSamples / 2)),
          target_(synthesizePeriodicAudition(
              cycle,
              fundamental_hz,
              kAuditionSamples)),
          target_features_(timedFeatures(target_, fundamental_hz_)) {
        NotePitch note_pitch{};
        if (notePitch(midi_note, note_pitch)) {
            opll_pitch_ = note_pitch.opll;
        }
    }

    float score(const OpllPatchParameters& patch) {
        const auto& generated =
            renderer_.render(patch, opll_pitch_, kAuditionSamples);
        float distance = featureDistance(
            target_features_,
            timedFeatures(generated, fundamental_hz_, feature_workspace_));
        distance += periodWindowDistance(
                        target_,
                        generated,
                        period_samples_)
            * 1.25F;
        return distance;
    }

private:
    float fundamental_hz_;
    std::size_t period_samples_;
    std::vector<float> target_;
    TimedAudioFeatures target_features_;
    OpllPitch opll_pitch_{};
    SteadyOpllRenderer renderer_;
    TimedFeatureWorkspace feature_workspace_;
};

class OpllTimedRenderer {
public:
    OpllTimedRenderer()
        : chip_(OPLL_new(kOpllClock, kOpllFitSampleRate)) {}

    const std::vector<float>& render(
        const OpllPatchParameters& patch,
        float frequency_hz,
        std::size_t sample_count,
        std::size_t key_off_sample,
        std::size_t render_offset_sample) {
        result_.assign(sample_count, 0.0F);
        if (!chip_) {
            return result_;
        }
        OPLL_reset(chip_.get());
        OPLL_setChipType(chip_.get(), 0);
        OPLL_resetPatch(chip_.get(), OPLL_2413_TONE);
        const auto registers = encodeOpllPatch(patch);
        for (std::size_t index = 0; index < registers.size(); ++index) {
            OPLL_writeReg(
                chip_.get(),
                static_cast<std::uint32_t>(index),
                registers[index]);
        }
        const auto pitch = closestOpllPitch(frequency_hz);
        OPLL_writeReg(
            chip_.get(),
            0x10,
            static_cast<std::uint8_t>(pitch.f_number & 0xFF));
        OPLL_writeReg(chip_.get(), 0x30, 0x00);
        const std::uint8_t pitch_without_key = static_cast<std::uint8_t>(
            ((pitch.f_number >> 8) & 1)
            | ((pitch.block & 7) << 1));
        OPLL_writeReg(
            chip_.get(),
            0x20,
            static_cast<std::uint8_t>(pitch_without_key | 0x10));
        for (std::size_t index = 0;
             index < render_offset_sample + sample_count;
             ++index) {
            if (index == key_off_sample) {
                OPLL_writeReg(chip_.get(), 0x20, pitch_without_key);
            }
            const float sample = static_cast<float>(OPLL_calc(chip_.get()));
            if (index >= render_offset_sample) {
                result_[index - render_offset_sample] = sample;
            }
        }
        removeDcAndNormalize(result_);
        return result_;
    }

private:
    std::unique_ptr<OPLL, OpllDeleter> chip_;
    std::vector<float> result_;
};

class TimedOpllScoreContext {
public:
    explicit TimedOpllScoreContext(const OpllFitTarget& target)
        : target_(target),
          fundamental_hz_(target.fundamental_hz) {}

    float score(const OpllPatchParameters& patch) {
        const auto& rendered = renderer_.render(
            patch,
            target_.fundamental_hz,
            target_.samples.size(),
            target_.key_off_sample,
            target_.render_offset_sample);
        float distance = featureDistance(
            target_.features,
            timedFeatures(
                rendered,
                fundamental_hz_,
                feature_workspace_));
        const auto period_samples = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(
                static_cast<double>(kOpllFitSampleRate)
                / std::max(1.0, static_cast<double>(fundamental_hz_)))),
            16,
            std::max<std::size_t>(16, target_.samples.size() / 2));
        distance += periodWindowDistance(
                        target_.samples,
                        rendered,
                        period_samples)
            * 0.35F;
        return distance;
    }

private:
    const OpllFitTarget& target_;
    float fundamental_hz_;
    OpllTimedRenderer renderer_;
    TimedFeatureWorkspace feature_workspace_;
};

struct ScoredOpllPatch {
    float distance{};
    std::array<std::uint8_t, 8> registers{};
    OpllPatchParameters patch{};
};

template <typename ContextFactory>
class OpllScoreExecutor {
public:
    using Context = decltype(std::declval<ContextFactory&>()());

    OpllScoreExecutor(ContextFactory make_context, std::size_t max_workers) {
        const unsigned hardware = std::thread::hardware_concurrency();
        worker_capacity_ = std::min<std::size_t>(
            std::clamp<std::size_t>(max_workers, 1, 8),
            hardware == 0 ? 4U : hardware);
        contexts_.reserve(worker_capacity_);
        for (std::size_t worker = 0; worker < worker_capacity_; ++worker) {
            contexts_.push_back(
                std::make_unique<Context>(make_context()));
        }
        if (worker_capacity_ == 1) {
            return;
        }
        workers_.reserve(worker_capacity_);
        try {
            for (std::size_t worker = 0; worker < worker_capacity_; ++worker) {
                workers_.emplace_back([this, worker] {
                    workerLoop(worker);
                });
            }
        } catch (const std::system_error&) {
            // The already-created workers remain usable. Missing deterministic
            // worker chunks are scored synchronously by score().
        }
    }

    OpllScoreExecutor(const OpllScoreExecutor&) = delete;
    OpllScoreExecutor& operator=(const OpllScoreExecutor&) = delete;

    ~OpllScoreExecutor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_ready_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    void score(std::span<ScoredOpllPatch> batch) {
        if (batch.empty()) {
            return;
        }
        const std::size_t active_workers =
            std::min(worker_capacity_, batch.size());
        if (active_workers == 1) {
            scoreChunk(batch, 0, active_workers);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            batch_data_ = batch.data();
            batch_size_ = batch.size();
            active_workers_ = active_workers;
            completed_workers_ = 0;
            ++generation_;
        }
        work_ready_.notify_all();

        // A failed thread launch leaves a suffix of deterministic worker
        // indices. Run exactly those chunks on the submitting thread.
        for (std::size_t worker = workers_.size();
             worker < active_workers;
             ++worker) {
            scoreChunk(batch, worker, active_workers);
        }

        std::unique_lock lock(mutex_);
        work_done_.wait(lock, [&] {
            return completed_workers_ == workers_.size();
        });
    }

private:
    void scoreChunk(
        std::span<ScoredOpllPatch> batch,
        std::size_t worker,
        std::size_t worker_count) {
        const std::size_t chunk_size =
            (batch.size() + worker_count - 1) / worker_count;
        const std::size_t begin = worker * chunk_size;
        const std::size_t end = std::min(begin + chunk_size, batch.size());
        auto& context = *contexts_[worker];
        for (std::size_t index = begin; index < end; ++index) {
            batch[index].distance = context.score(batch[index].patch);
        }
    }

    void workerLoop(std::size_t worker) {
        std::uint64_t observed_generation{};
        for (;;) {
            std::span<ScoredOpllPatch> batch;
            std::size_t active_workers{};
            {
                std::unique_lock lock(mutex_);
                work_ready_.wait(lock, [&] {
                    return stopping_ || generation_ != observed_generation;
                });
                if (stopping_) {
                    return;
                }
                observed_generation = generation_;
                batch = {batch_data_, batch_size_};
                active_workers = active_workers_;
            }
            if (worker < active_workers) {
                scoreChunk(batch, worker, active_workers);
            }
            {
                std::lock_guard lock(mutex_);
                ++completed_workers_;
            }
            work_done_.notify_one();
        }
    }

    std::size_t worker_capacity_{1};
    std::vector<std::unique_ptr<Context>> contexts_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable work_done_;
    ScoredOpllPatch* batch_data_{};
    std::size_t batch_size_{};
    std::size_t active_workers_{};
    std::size_t completed_workers_{};
    std::uint64_t generation_{};
    bool stopping_{};
};

template <typename Executor>
bool scoreOpllSubBatches(
    std::vector<ScoredOpllPatch>& candidates,
    Executor& executor,
    std::size_t batch_size,
    std::uint64_t weight,
    OpllApproximationCoordinator* coordinator) {
    for (std::size_t begin = 0; begin < candidates.size();
         begin += batch_size) {
        if (coordinator && coordinator->cancelled()) {
            return false;
        }
        const std::size_t count =
            std::min(batch_size, candidates.size() - begin);
        executor.score(
            std::span<ScoredOpllPatch>(candidates).subspan(begin, count));
        // A sub-batch is published only after every worker has completed.
        if (coordinator) {
            coordinator->advance(
                static_cast<std::uint64_t>(count) * weight);
        }
    }
    return true;
}

std::size_t detectKeyOff(
    const std::array<float, kEnvelopeBins>& envelope,
    std::size_t sample_count) {
    if (sample_count == 0) {
        return std::numeric_limits<std::size_t>::max();
    }
    const auto peak = static_cast<std::size_t>(
        std::distance(
            envelope.begin(),
            std::max_element(envelope.begin(), envelope.end())));
    for (std::size_t bin = std::max<std::size_t>(
             peak + 2,
             envelope.size() * 2 / 3);
         bin + 3 < envelope.size();
         ++bin) {
        if (envelope[bin] > 0.35F
            && envelope[bin + 3] < envelope[bin] * 0.45F
            && envelope.back() < 0.18F) {
            return bin * sample_count / envelope.size();
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

OpllFitTarget makeFitTarget(
    const WavePcm& pcm,
    const WaveCycleAnalysis& cycle_analysis) {
    OpllFitTarget target{};
    auto resampled = resampleAudio(
        pcm.mono_samples,
        pcm.sample_rate,
        kOpllFitSampleRate);
    removeDcAndNormalize(resampled);
    float peak{};
    for (const float sample : resampled) {
        peak = std::max(peak, std::abs(sample));
    }
    std::size_t first_audible{};
    const float threshold = peak * 0.0125F;
    while (first_audible < resampled.size()
           && std::abs(resampled[first_audible]) < threshold) {
        ++first_audible;
    }
    constexpr std::size_t maximum_fit_samples =
        kOpllFitSampleRate * 2;
    const std::size_t end = std::min(
        resampled.size(),
        first_audible + maximum_fit_samples);
    if (first_audible < end) {
        target.samples.assign(
            resampled.begin() + static_cast<std::ptrdiff_t>(first_audible),
            resampled.begin() + static_cast<std::ptrdiff_t>(end));
    } else {
        target.samples = std::move(resampled);
    }
    removeDcAndNormalize(target.samples);
    target.fundamental_hz =
        cycle_analysis.estimated_frequency_hz > 0.0F
            ? cycle_analysis.estimated_frequency_hz
            : 261.625565F;
    target.features = timedFeatures(
        target.samples,
        target.fundamental_hz);
    target.key_off_sample =
        detectKeyOff(target.features.envelope, target.samples.size());
    return target;
}

OpllFitTarget makeTimbreTarget(const OpllFitTarget& full_target) {
    OpllFitTarget target{};
    target.fundamental_hz = full_target.fundamental_hz;
    target.key_off_sample = std::numeric_limits<std::size_t>::max();
    if (full_target.samples.empty()) {
        return target;
    }
    constexpr std::size_t maximum_samples = 4'096;
    const std::size_t length =
        std::min(maximum_samples, full_target.samples.size());
    // Skip only a bounded onset interval. Rendering applies the same offset,
    // keeping target and candidate envelope time aligned without making the
    // cheap phase scale with a long source WAV.
    const std::size_t best_begin = std::min<std::size_t>(
        1'024,
        full_target.samples.size() - length);
    target.render_offset_sample = best_begin;
    target.samples.assign(
        full_target.samples.begin() + static_cast<std::ptrdiff_t>(best_begin),
        full_target.samples.begin()
            + static_cast<std::ptrdiff_t>(best_begin + length));
    removeDcAndNormalize(target.samples);
    target.features = timedFeatures(target.samples, target.fundamental_hz);
    return target;
}

std::vector<OpllPatchParameters> searchTimedOpll(
    const OpllFitTarget& target,
    std::span<const float> representative_cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    const SearchBudgets& budgets) {
    // Cheap timbre phase: 92 seeds + 8 * (81 + 72 + 32 + 4*4 + 16)
    // = 1,828 probes before register deduplication.
    const std::size_t evaluation_budget = budgets.short_timbre;
    const std::size_t steady_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardSteadyBatchSize
        : kSteadyBatchSize;
    const std::size_t full_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardFullWaveBatchSize
        : kFullWaveBatchSize;
    constexpr std::size_t beam_width = 8;
    constexpr std::size_t result_capacity = 6;
    const auto timbre_target = makeTimbreTarget(target);
    std::size_t evaluations{};
    std::set<std::array<std::uint8_t, 8>> seen;
    std::vector<ScoredOpllPatch> short_scored;
    // OPLL_new performs emu2413's one-time table initialization. Complete it
    // on this thread before any worker constructs its private context.
    TimedOpllScoreContext priming_context(timbre_target);
    static_cast<void>(priming_context);
    const auto make_context = [&timbre_target] {
        return TimedOpllScoreContext(timbre_target);
    };
    const auto less = [](
                          const ScoredOpllPatch& left,
                          const ScoredOpllPatch& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.registers < right.registers;
    };
    const auto admit = [&](
                           const OpllPatchParameters& patch,
                           std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (evaluations >= evaluation_budget || !seen.insert(registers).second) {
            return;
        }
        ++evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > beam_width) {
            candidates.resize(beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> seed_batch;
    const auto seed = [&](const OpllPatchParameters& patch) {
        admit(patch, seed_batch);
    };

    for (const auto& patch : searchWaveCycleWithOpll(
             representative_cycle,
             options,
             coordinator,
             budgets.representative,
             OpllApproximationPhase::RepresentativeCycle)) {
        seed(patch);
    }
    if (coordinator && coordinator->cancelled()) {
        return {};
    }
    seed(defaultOpllPatch());
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            seed(*rom);
        }
    }
    std::uint32_t random = 0x5749464DU;
    const auto next_random = [&]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    for (int index = 0; index < 64; ++index) {
        auto candidate = defaultOpllPatch();
        std::uint32_t bits = next_random();
        candidate.modulator.multiplier = bits & 15;
        candidate.carrier.multiplier = (bits >> 4) & 15;
        candidate.feedback = (bits >> 8) & 7;
        candidate.modulator.waveform = ((bits >> 11) & 1) != 0;
        candidate.carrier.waveform = ((bits >> 12) & 1) != 0;
        candidate.modulator.pitch_modulation = ((bits >> 13) & 1) != 0;
        candidate.carrier.pitch_modulation = ((bits >> 14) & 1) != 0;
        candidate.modulator.amplitude_modulation = ((bits >> 15) & 1) != 0;
        candidate.carrier.amplitude_modulation = ((bits >> 16) & 1) != 0;
        candidate.modulator.sustained_tone = ((bits >> 17) & 1) != 0;
        candidate.carrier.sustained_tone = ((bits >> 18) & 1) != 0;
        candidate.modulator.key_rate_scaling = ((bits >> 19) & 1) != 0;
        candidate.carrier.key_rate_scaling = ((bits >> 20) & 1) != 0;
        candidate.modulator.key_scale_level = (bits >> 21) & 3;
        candidate.carrier.key_scale_level = (bits >> 23) & 3;
        bits = next_random();
        candidate.modulator.total_level = bits & 63;
        candidate.modulator.attack_rate = (bits >> 6) & 15;
        candidate.modulator.decay_rate = (bits >> 10) & 15;
        candidate.modulator.sustain_level = (bits >> 14) & 15;
        candidate.modulator.release_rate = (bits >> 18) & 15;
        candidate.carrier.attack_rate = (bits >> 22) & 15;
        candidate.carrier.decay_rate = (bits >> 26) & 15;
        bits = next_random();
        candidate.carrier.sustain_level = bits & 15;
        candidate.carrier.release_rate = (bits >> 4) & 15;
        seed(candidate);
    }
    if (coordinator) {
        coordinator->setPhase(OpllApproximationPhase::ShortTimbre);
    }
    // Do not keep the short-target pool alive during the nested
    // representative-cycle search; each homogeneous phase owns one pool.
    OpllScoreExecutor short_executor(make_context, options.max_workers);
    if (!scoreOpllSubBatches(
            seed_batch,
            short_executor,
            steady_batch_size,
            1,
            coordinator)) {
        return {};
    }
    short_scored.insert(
        short_scored.end(), seed_batch.begin(), seed_batch.end());
    std::vector<ScoredOpllPatch> beam = retain(std::move(seed_batch));
    const auto run_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                short_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return false;
        }
        short_scored.insert(
            short_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = beam;
        next.insert(next.end(), batch.begin(), batch.end());
        beam = retain(std::move(next));
        return true;
    };

    constexpr std::array<std::uint8_t, 9> multipliers{
        0, 1, 2, 3, 4, 6, 8, 12, 15};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto modulator : multipliers) {
            for (const auto carrier : multipliers) {
                auto candidate = base;
                candidate.modulator.multiplier = modulator;
                candidate.carrier.multiplier = carrier;
                emit(candidate);
            }
        }
    })) return {};
    constexpr std::array<std::uint8_t, 9> levels{
        0, 8, 16, 24, 32, 40, 48, 56, 63};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto level : levels) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.total_level = level;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int waveform = 0; waveform < 4; ++waveform) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.waveform = (waveform & 1) != 0;
                candidate.carrier.waveform = (waveform & 2) != 0;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.amplitude_modulation = (value & 1) != 0;
            candidate.carrier.amplitude_modulation = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.pitch_modulation = (value & 1) != 0;
            candidate.carrier.pitch_modulation = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.sustained_tone = (value & 1) != 0;
            candidate.carrier.sustained_tone = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.key_rate_scaling = (value & 1) != 0;
            candidate.carrier.key_rate_scaling = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (std::uint8_t modulator = 0; modulator < 4; ++modulator) {
            for (std::uint8_t carrier = 0; carrier < 4; ++carrier) {
                auto candidate = base;
                candidate.modulator.key_scale_level = modulator;
                candidate.carrier.key_scale_level = carrier;
                emit(candidate);
            }
        }
    })) return {};

    // Thorough uses deterministic full-register restarts after the complete
    // Standard path. Decoding arbitrary register bytes spans the legal patch
    // space and prevents later stages from starving on duplicate mutations.
    if (options.effort == OpllApproximationEffort::Thorough) {
        std::sort(short_scored.begin(), short_scored.end(), less);
        std::vector<ScoredOpllPatch> broad_beam = short_scored;
        if (broad_beam.size() > 24) {
            broad_beam.resize(24);
        }
        std::vector<ScoredOpllPatch> pair_local_batch;
        for (const auto& parent : broad_beam) {
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = parent.patch;
                candidate.modulator.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.multiplier)
                            + delta,
                        0,
                        15));
                candidate.carrier.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(parent.patch.carrier.multiplier)
                            - delta,
                        0,
                        15));
                candidate.modulator.total_level =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.total_level)
                            + delta * 2,
                        0,
                        63));
                admit(candidate, pair_local_batch);
            }
        }
        if (!scoreOpllSubBatches(
                pair_local_batch,
                short_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return {};
        }
        short_scored.insert(
            short_scored.end(),
            pair_local_batch.begin(),
            pair_local_batch.end());
        while (evaluations < evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(steady_batch_size);
            while (batch.size() < steady_batch_size
                   && evaluations < evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit(decodeOpllPatch(registers), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    short_executor,
                    steady_batch_size,
                    1,
                    coordinator)) {
                return {};
            }
            short_scored.insert(
                short_scored.end(), batch.begin(), batch.end());
            std::vector<ScoredOpllPatch> next = beam;
            next.insert(next.end(), batch.begin(), batch.end());
            beam = retain(std::move(next));
        }
    }

    // Full-length phase: 8 promoted timbres + 3 * (4 * 16 + 8 * 5)
    // = 320 probes before register deduplication.
    const std::size_t full_evaluation_budget = budgets.full_envelope;
    constexpr std::size_t full_beam_width = 3;
    std::size_t full_evaluations{};
    std::set<std::array<std::uint8_t, 8>> full_seen;
    std::vector<ScoredOpllPatch> full_scored;
    TimedOpllScoreContext full_priming_context(target);
    static_cast<void>(full_priming_context);
    const auto make_full_context = [&target] {
        return TimedOpllScoreContext(target);
    };
    OpllScoreExecutor full_executor(make_full_context, options.max_workers);
    const auto admit_full = [&](
                                const OpllPatchParameters& patch,
                                std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (full_evaluations >= full_evaluation_budget
            || !full_seen.insert(registers).second) {
            return;
        }
        ++full_evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain_full = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > full_beam_width) {
            candidates.resize(full_beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> promoted_batch;
    for (const auto& entry : beam) {
        admit_full(entry.patch, promoted_batch);
    }
    if (coordinator) {
        coordinator->setPhase(OpllApproximationPhase::FullEnvelope);
    }
    const auto full_weight = fullWaveWeight(target.samples.size());
    if (!scoreOpllSubBatches(
            promoted_batch,
            full_executor,
            full_batch_size,
            full_weight,
            coordinator)) {
        return {};
    }
    full_scored.insert(
        full_scored.end(),
        promoted_batch.begin(),
        promoted_batch.end());
    std::vector<ScoredOpllPatch> full_beam =
        retain_full(std::move(promoted_batch));
    const auto run_full_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : full_beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit_full(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                full_executor,
                full_batch_size,
                full_weight,
                coordinator)) {
            return false;
        }
        full_scored.insert(full_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = full_beam;
        next.insert(next.end(), batch.begin(), batch.end());
        full_beam = retain_full(std::move(next));
        return true;
    };

    constexpr std::array<std::uint8_t, 4> eg{0, 5, 10, 15};
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto attack : eg) {
            for (const auto decay : eg) {
                auto candidate = base;
                candidate.modulator.attack_rate = attack;
                candidate.modulator.decay_rate = decay;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto sustain : eg) {
            for (const auto release : eg) {
                auto candidate = base;
                candidate.modulator.sustain_level = sustain;
                candidate.modulator.release_rate = release;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto attack : eg) {
            for (const auto decay : eg) {
                auto candidate = base;
                candidate.carrier.attack_rate = attack;
                candidate.carrier.decay_rate = decay;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto sustain : eg) {
            for (const auto release : eg) {
                auto candidate = base;
                candidate.carrier.sustain_level = sustain;
                candidate.carrier.release_rate = release;
                emit(candidate);
            }
        }
    });

    // Quantization-aware ±2 refinement runs only after all four joint EG
    // stages; the full-phase upper bound reserves all 120 probes.
    const auto refine_local = [&](auto member) {
        run_full_stage([&](const OpllPatchParameters& base, auto emit) {
            const int center = member(base);
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = base;
                member(candidate) = static_cast<std::uint8_t>(
                    std::clamp(center + delta, 0, 15));
                emit(candidate);
            }
        });
    };
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.attack_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.decay_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.sustain_level;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.release_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.attack_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.decay_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.sustain_level;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.release_rate;
    });

    if (coordinator && coordinator->cancelled()) {
        return {};
    }
    if (options.effort == OpllApproximationEffort::Thorough) {
        while (full_evaluations < full_evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(full_batch_size);
            while (batch.size() < full_batch_size
                   && full_evaluations < full_evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit_full(decodeOpllPatch(registers), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    full_executor,
                    full_batch_size,
                    full_weight,
                    coordinator)) {
                return {};
            }
            full_scored.insert(
                full_scored.end(), batch.begin(), batch.end());
        }
    }

    std::sort(full_scored.begin(), full_scored.end(), less);
    std::vector<OpllPatchParameters> result;
    result.reserve(result_capacity);
    for (const auto& entry : full_scored) {
        result.push_back(entry.patch);
        if (result.size() == result_capacity) {
            break;
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

}  // namespace

bool parseWavePcm(
    std::span<const std::uint8_t> bytes,
    WavePcm& output,
    std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (bytes.size() < 12
        || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return fail("Not a RIFF/WAVE file.");
    }
    std::uint16_t format{};
    std::uint16_t channels{};
    std::uint16_t bits{};
    std::uint32_t sample_rate{};
    std::uint16_t block_align{};
    std::span<const std::uint8_t> data;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::uint32_t size = u32(bytes, offset + 4);
        const std::size_t begin = offset + 8;
        if (begin + size > bytes.size()) {
            return fail("Truncated WAVE chunk.");
        }
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0
            && size >= 16) {
            format = u16(bytes, begin);
            channels = u16(bytes, begin + 2);
            sample_rate = u32(bytes, begin + 4);
            block_align = u16(bytes, begin + 12);
            bits = u16(bytes, begin + 14);
            if (format == 0xFFFE && size >= 40) {
                // WAVE_FORMAT_EXTENSIBLE stores PCM/IEEE-float as the
                // leading word of its SubFormat GUID.
                format = u16(bytes, begin + 24);
            }
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            data = bytes.subspan(begin, size);
        }
        offset = begin + size + (size & 1U);
    }
    const bool supported =
        format == 1 || (format == 3 && bits == 32);
    if (!supported || channels == 0 || channels > 32
        || sample_rate == 0 || block_align == 0 || data.empty()
        || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        return fail("Unsupported or incomplete PCM WAVE format.");
    }
    const std::size_t bytes_per_sample = bits / 8;
    if (block_align < channels * bytes_per_sample) {
        return fail("Invalid WAVE block alignment.");
    }
    const std::size_t frames = data.size() / block_align;
    if (frames < 2 || frames > sample_rate * 60ULL) {
        return fail("WAVE must contain between two samples and 60 seconds.");
    }
    output.sample_rate = sample_rate;
    output.mono_samples.assign(frames, 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum{};
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sum += normalizedSample(
                data,
                frame * block_align + channel * bytes_per_sample,
                format,
                bits);
        }
        output.mono_samples[frame] =
            sum / static_cast<float>(channels);
    }
    return true;
}

WaveCycleAnalysis analyzeWaveCycle(const WavePcm& pcm) {
    WaveCycleAnalysis result{};
    if (pcm.mono_samples.size() < 2 || pcm.sample_rate == 0) {
        return result;
    }
    const auto samples = normalizedCycle(pcm.mono_samples);
    const std::size_t minimum =
        std::max<std::size_t>(8, pcm.sample_rate / 2000);
    const std::size_t maximum = std::min<std::size_t>(
        pcm.sample_rate / 40,
        samples.size() / 2);
    std::size_t period = samples.size();
    if (samples.size() > pcm.sample_rate / 20 && maximum > minimum) {
        const std::size_t probe_size = std::min<std::size_t>(
            samples.size(),
            std::max<std::size_t>(
                pcm.sample_rate / 2,
                maximum * 2));
        const std::size_t probe_begin =
            (samples.size() - probe_size) / 2;
        const std::span<const float> probe(
            samples.data() + probe_begin,
            probe_size);
        float best = std::numeric_limits<float>::max();
        for (std::size_t lag = minimum; lag <= maximum; ++lag) {
            const float difference = periodicDifference(probe, lag);
            if (difference < best) {
                best = difference;
                period = lag;
            }
        }
        // Non-integer periods often score 2T slightly better than T (sine).
        // Collapse octave-up locks while the half-period remains competitive.
        while (period / 2 >= minimum) {
            const std::size_t half = period / 2;
            const float half_difference = periodicDifference(probe, half);
            if (half_difference
                <= std::max(best * 1.08F, best + 0.002F)) {
                period = half;
                best = half_difference;
            } else {
                break;
            }
        }
    }
    period = std::clamp<std::size_t>(period, 2, samples.size());
    result.cycle.assign(period, 0.0F);
    const std::size_t cycle_count =
        std::max<std::size_t>(1, samples.size() / period);
    const std::size_t used_cycles = std::min<std::size_t>(cycle_count, 16);
    const std::size_t start_cycle =
        cycle_count > used_cycles ? (cycle_count - used_cycles) / 2 : 0;
    for (std::size_t cycle = 0; cycle < used_cycles; ++cycle) {
        for (std::size_t index = 0; index < period; ++index) {
            result.cycle[index] +=
                samples[(start_cycle + cycle) * period + index];
        }
    }
    for (float& sample : result.cycle) {
        sample /= static_cast<float>(used_cycles);
    }
    result.cycle = normalizedCycle(result.cycle);
    result.estimated_frequency_hz =
        static_cast<float>(pcm.sample_rate)
        / static_cast<float>(period);
    return result;
}

SccWaveform waveCycleToScc(std::span<const float> cycle) noexcept {
    SccWaveform result{};
    const auto normalized = normalizedCycle(cycle);
    if (normalized.empty()) {
        return result;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * normalized.size() / result.size();
        const auto first =
            static_cast<std::size_t>(position) % normalized.size();
        const auto second = (first + 1) % normalized.size();
        const float fraction =
            static_cast<float>(position - std::floor(position));
        const float sample =
            normalized[first] * (1.0F - fraction)
            + normalized[second] * fraction;
        result[index] = static_cast<std::int8_t>(
            std::clamp(
                static_cast<int>(std::lround(sample * 127.0F)),
                -128,
                127));
    }
    return result;
}

OpllPatchParameters approximateWaveCycleWithOpll(
    std::span<const float> cycle) noexcept {
    const auto candidates = approximateWaveCycleCandidatesWithOpll(cycle);
    return candidates.empty() ? defaultOpllPatch() : candidates.front();
}

std::vector<OpllPatchParameters> approximateWaveCycleCandidatesWithOpll(
    std::span<const float> cycle) {
    return approximateWaveCycleCandidatesWithOpll(
        cycle,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters> searchWaveCycleWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    std::size_t evaluation_budget,
    OpllApproximationPhase phase) {
    const std::size_t steady_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardSteadyBatchSize
        : kSteadyBatchSize;
    const auto normalized = normalizedCycle(cycle);
    if (normalized.size() < 2) {
        return {defaultOpllPatch()};
    }
    NotePitch pitch{};
    if (!notePitch(kAuditionMidiNote, pitch)) {
        return {defaultOpllPatch()};
    }
    const float fundamental_hz = sccFrequencyHz(pitch.psg_scc_period);
    SameMidiScoreContext priming_context(
        normalized,
        fundamental_hz,
        kAuditionMidiNote);
    static_cast<void>(priming_context);
    const auto make_context = [&normalized, fundamental_hz] {
        return SameMidiScoreContext(
            normalized,
            fundamental_hz,
            kAuditionMidiNote);
    };
    OpllScoreExecutor score_executor(make_context, options.max_workers);
    const auto prepare = [](OpllPatchParameters patch) {
        patch.modulator.amplitude_modulation = false;
        patch.carrier.amplitude_modulation = false;
        patch.modulator.sustained_tone = true;
        patch.carrier.sustained_tone = true;
        patch.modulator.key_rate_scaling = false;
        patch.carrier.key_rate_scaling = false;
        patch.modulator.key_scale_level = 0;
        patch.carrier.key_scale_level = 0;
        patch.modulator.attack_rate = 15;
        patch.carrier.attack_rate = 15;
        patch.modulator.decay_rate = 0;
        patch.carrier.decay_rate = 0;
        patch.modulator.sustain_level = 0;
        patch.carrier.sustain_level = 0;
        patch.modulator.release_rate = 8;
        patch.carrier.release_rate = 8;
        return patch;
    };
    // Upper bound before register deduplication:
    // 402 seeds + 12 * (256 + 72 + 32 + 4 + 19) = 4,998.
    constexpr std::size_t beam_width = 12;
    std::size_t evaluations{};
    std::set<std::array<std::uint8_t, 8>> seen;
    std::vector<ScoredOpllPatch> all_scored;
    const auto less = [](
                          const ScoredOpllPatch& left,
                          const ScoredOpllPatch& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.registers < right.registers;
    };
    const auto admit = [&](
                           const OpllPatchParameters& patch,
                           std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (evaluations >= evaluation_budget || !seen.insert(registers).second) {
            return;
        }
        ++evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > beam_width) {
            candidates.resize(beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> seed_batch;
    const auto seed = [&](const OpllPatchParameters& patch) {
        admit(patch, seed_batch);
    };

    seed(prepare(defaultOpllPatch()));
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            seed(prepare(*rom));
        }
    }

    std::uint32_t random = 0x2413C0DEU;
    auto next_random = [&]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    for (int index = 0; index < 384; ++index) {
        OpllPatchParameters candidate =
            prepare(defaultOpllPatch());
        std::uint32_t bits = next_random();
        candidate.modulator.multiplier =
            static_cast<std::uint8_t>(bits & 15);
        // Prefer MULTI that can match SCC pitch on the shared keyboard;
        // still allow the full legal set for timbre, scored by same-MIDI audio.
        candidate.carrier.multiplier =
            static_cast<std::uint8_t>(bits >> 4) & 15;
        candidate.feedback =
            static_cast<std::uint8_t>((bits >> 8) & 7);
        candidate.modulator.waveform = ((bits >> 11) & 1) != 0;
        candidate.carrier.waveform = ((bits >> 12) & 1) != 0;
        bits = next_random();
        candidate.modulator.total_level =
            static_cast<std::uint8_t>(bits & 63);
        candidate.modulator.pitch_modulation =
            ((bits >> 6) & 3) == 1;
        candidate.carrier.pitch_modulation =
            ((bits >> 8) & 3) == 1;
        seed(candidate);
    }

    // Insurance: always evaluate MULTI 1 and 2 with quiet modulator.
    for (const std::uint8_t car_mul :
         std::array<std::uint8_t, 2>{1, 2}) {
        auto candidate = prepare(defaultOpllPatch());
        candidate.modulator.multiplier = 0;
        candidate.carrier.multiplier = car_mul;
        candidate.modulator.total_level = 63;
        candidate.feedback = 0;
        seed(candidate);
    }
    if (coordinator) {
        coordinator->setPhase(phase);
    }
    if (!scoreOpllSubBatches(
            seed_batch,
            score_executor,
            steady_batch_size,
            1,
            coordinator)) {
        return {};
    }
    all_scored.insert(
        all_scored.end(),
        seed_batch.begin(),
        seed_batch.end());
    std::vector<ScoredOpllPatch> beam = retain(std::move(seed_batch));
    const auto run_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                score_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return false;
        }
        all_scored.insert(all_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = beam;
        next.insert(next.end(), batch.begin(), batch.end());
        beam = retain(std::move(next));
        return true;
    };

    // Joint operator-ratio search preserves alternatives that greedy
    // one-register-at-a-time refinement cannot reach.
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (std::uint8_t mod = 0; mod < 16; ++mod) {
            for (std::uint8_t car = 0; car < 16; ++car) {
                auto candidate = base;
                candidate.modulator.multiplier = mod;
                candidate.carrier.multiplier = car;
                emit(candidate);
            }
        }
    })) return {};
    constexpr std::array<std::uint8_t, 9> levels{
        0, 8, 16, 24, 32, 40, 48, 56, 63};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto level : levels) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.total_level = level;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int waveforms = 0; waveforms < 4; ++waveforms) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.waveform = (waveforms & 1) != 0;
                candidate.carrier.waveform = (waveforms & 2) != 0;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int modulation = 0; modulation < 4; ++modulation) {
            auto candidate = base;
            candidate.modulator.pitch_modulation = (modulation & 1) != 0;
            candidate.carrier.pitch_modulation = (modulation & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        const int center = base.modulator.total_level;
        for (int delta = -4; delta <= 4; ++delta) {
            auto candidate = base;
            candidate.modulator.total_level = static_cast<std::uint8_t>(
                std::clamp(center + delta, 0, 63));
            emit(candidate);
        }
        for (int delta = -2; delta <= 2; ++delta) {
            auto mod = base;
            mod.modulator.multiplier = static_cast<std::uint8_t>(
                std::clamp(
                    static_cast<int>(base.modulator.multiplier) + delta,
                    0,
                    15));
            emit(mod);
            auto car = base;
            car.carrier.multiplier = static_cast<std::uint8_t>(
                std::clamp(
                    static_cast<int>(base.carrier.multiplier) + delta,
                    0,
                    15));
            emit(car);
        }
    })) return {};

    if (options.effort == OpllApproximationEffort::Thorough) {
        // Re-open the beam after the complete Standard path, then perform a
        // joint ratio/level neighborhood before fixed-seed restarts.
        std::sort(all_scored.begin(), all_scored.end(), less);
        std::vector<ScoredOpllPatch> broad_beam = all_scored;
        if (broad_beam.size() > 48) {
            broad_beam.resize(48);
        }
        std::vector<ScoredOpllPatch> pair_local_batch;
        for (const auto& parent : broad_beam) {
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = parent.patch;
                candidate.modulator.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.multiplier)
                            + delta,
                        0,
                        15));
                candidate.carrier.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(parent.patch.carrier.multiplier)
                            - delta,
                        0,
                        15));
                candidate.modulator.total_level =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.total_level)
                            + delta * 2,
                        0,
                        63));
                admit(candidate, pair_local_batch);
            }
        }
        if (!scoreOpllSubBatches(
                pair_local_batch,
                score_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return {};
        }
        all_scored.insert(
            all_scored.end(),
            pair_local_batch.begin(),
            pair_local_batch.end());
        while (evaluations < evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(steady_batch_size);
            while (batch.size() < steady_batch_size
                   && evaluations < evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit(prepare(decodeOpllPatch(registers)), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    score_executor,
                    steady_batch_size,
                    1,
                    coordinator)) {
                return {};
            }
            all_scored.insert(
                all_scored.end(), batch.begin(), batch.end());
            std::vector<ScoredOpllPatch> next = beam;
            next.insert(next.end(), batch.begin(), batch.end());
            beam = retain(std::move(next));
        }
    }

    std::sort(all_scored.begin(), all_scored.end(), less);
    std::vector<OpllPatchParameters> result;
    result.reserve(beam_width);
    for (const auto& entry : all_scored) {
        result.push_back(entry.patch);
        if (result.size() == beam_width) {
            break;
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

OpllApproximationControl::OpllApproximationControl()
    : state_(std::make_shared<State>()) {}

void OpllApproximationControl::requestCancel() noexcept {
    state_->cancel_requested.store(true, std::memory_order_release);
}

bool OpllApproximationControl::cancelRequested() const noexcept {
    return state_->cancel_requested.load(std::memory_order_acquire);
}

OpllApproximationProgress OpllApproximationControl::progress() const noexcept {
    OpllApproximationProgress result;
    result.phase = state_->phase.load(std::memory_order_acquire);
    result.completed = state_->completed.load(std::memory_order_acquire);
    result.total = state_->total.load(std::memory_order_acquire);
    result.cancel_requested =
        state_->cancel_requested.load(std::memory_order_acquire);
    return result;
}

OpllApproximationResult approximateWaveCycleWithOpllResult(
    std::span<const float> cycle,
    const OpllApproximationOptions& options) {
    const std::size_t budget =
        options.profile == OpllApproximationProfile::Compact
        ? 384U
        : (options.effort == OpllApproximationEffort::Thorough
               ? 160'000U
               : 5'000U);
    OpllApproximationCoordinator coordinator(options, budget);
    if (coordinator.cancelled()) {
        coordinator.finish(true);
        return {OpllApproximationCompletion::Cancelled, {}};
    }
    auto candidates = searchWaveCycleWithOpll(
        cycle,
        options,
        &coordinator,
        budget,
        OpllApproximationPhase::SteadyTimbre);
    const bool cancelled = coordinator.cancelled();
    coordinator.finish(cancelled);
    return {
        cancelled
            ? OpllApproximationCompletion::Cancelled
            : OpllApproximationCompletion::Completed,
        cancelled ? std::vector<OpllPatchParameters>{}
                  : std::move(candidates)};
}

std::vector<OpllPatchParameters> approximateWaveCycleCandidatesWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options) {
    return approximateWaveCycleWithOpllResult(cycle, options).candidates;
}

std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(const SccWaveform& waveform) {
    return approximateSccWaveformCandidatesWithOpll(
        waveform,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options) {
    return approximateSccWaveformWithOpllResult(waveform, options).candidates;
}

OpllApproximationResult approximateSccWaveformWithOpllResult(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options) {
    std::array<float, 32> cycle{};
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        cycle[index] = static_cast<float>(waveform[index]) / 128.0F;
    }
    return approximateWaveCycleWithOpllResult(cycle, options);
}

std::vector<OpllPatchParameters> approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm) {
    return approximateWavePcmCandidatesWithOpll(
        pcm,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters> approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm,
    const OpllApproximationOptions& options) {
    return approximateWavePcmWithOpllResult(pcm, options).candidates;
}

OpllApproximationResult approximateWavePcmWithOpllResult(
    const WavePcm& pcm,
    const OpllApproximationOptions& options) {
    const auto cycle_analysis = analyzeWaveCycle(pcm);
    if (cycle_analysis.cycle.size() < 2) {
        OpllApproximationCoordinator coordinator(options, 0);
        const bool cancelled = coordinator.cancelled();
        coordinator.finish(cancelled);
        return {
            cancelled
                ? OpllApproximationCompletion::Cancelled
                : OpllApproximationCompletion::Completed,
            cancelled ? std::vector<OpllPatchParameters>{}
                      : std::vector<OpllPatchParameters>{defaultOpllPatch()}};
    }
    const auto target = makeFitTarget(pcm, cycle_analysis);
    if (target.samples.size() < 2) {
        return approximateWaveCycleWithOpllResult(
            cycle_analysis.cycle, options);
    }
    const auto budgets = searchBudgets(options);
    const std::uint64_t total =
        budgets.representative + budgets.short_timbre
        + static_cast<std::uint64_t>(budgets.full_envelope)
            * fullWaveWeight(target.samples.size());
    OpllApproximationCoordinator coordinator(options, total);
    if (coordinator.cancelled()) {
        coordinator.finish(true);
        return {OpllApproximationCompletion::Cancelled, {}};
    }
    auto candidates = searchTimedOpll(
        target, cycle_analysis.cycle, options, &coordinator, budgets);
    const bool cancelled = coordinator.cancelled();
    coordinator.finish(cancelled);
    return {
        cancelled
            ? OpllApproximationCompletion::Cancelled
            : OpllApproximationCompletion::Completed,
        cancelled ? std::vector<OpllPatchParameters>{}
                  : std::move(candidates)};
}

OpllPatchParameters approximateWavePcmWithOpll(
    const WavePcm& pcm) {
    auto candidates = approximateWavePcmCandidatesWithOpll(pcm);
    return candidates.empty() ? defaultOpllPatch() : candidates.front();
}

}  // namespace mgstc::engine
