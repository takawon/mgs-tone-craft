#include "mgstc/engine/wave_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <system_error>
#include <utility>
#include <vector>

#include "emu2413.h"
#include "mgstc/engine/note_pitch.hpp"

namespace mgstc::engine {
namespace {

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

std::vector<float> renderOpllAudition(
    const OpllPatchParameters& patch,
    std::uint8_t midi_note,
    std::size_t sample_count) {
    std::vector<float> result(sample_count, 0.0F);
    NotePitch pitch{};
    if (!notePitch(midi_note, pitch)) {
        return result;
    }
    std::unique_ptr<OPLL, OpllDeleter> chip(
        OPLL_new(kChipClock, kAuditionSampleRate));
    if (!chip) {
        return result;
    }
    OPLL_reset(chip.get());
    OPLL_setChipType(chip.get(), 0);
    OPLL_resetPatch(chip.get(), OPLL_2413_TONE);
    const auto registers = encodeOpllPatch(patch);
    for (std::size_t index = 0; index < registers.size(); ++index) {
        OPLL_writeReg(
            chip.get(),
            static_cast<std::uint32_t>(index),
            registers[index]);
    }
    OPLL_writeReg(
        chip.get(),
        0x10,
        static_cast<std::uint8_t>(pitch.opll.f_number & 0xFF));
    OPLL_writeReg(chip.get(), 0x30, 0x00);
    const auto pitch_register = static_cast<std::uint8_t>(
        ((pitch.opll.f_number >> 8) & 1)
        | ((pitch.opll.block & 7) << 1)
        | 0x10);
    OPLL_writeReg(chip.get(), 0x20, pitch_register);
    for (std::size_t index = 0;
         index < kAuditionWarmup + sample_count;
         ++index) {
        const float sample = static_cast<float>(OPLL_calc(chip.get()));
        if (index >= kAuditionWarmup) {
            result[index - kAuditionWarmup] = sample;
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
    std::vector<float> target_period(
        target.begin() + static_cast<std::ptrdiff_t>(target_begin),
        target.begin()
            + static_cast<std::ptrdiff_t>(target_begin + period_samples));
    const auto target_cycle = resampleCycle32(target_period);
    float best = std::numeric_limits<float>::max();
    const std::size_t max_shift = std::min(period_samples, generated.size() / 2);
    for (std::size_t shift = 0; shift < max_shift; ++shift) {
        if (shift + period_samples > generated.size()) {
            break;
        }
        std::vector<float> generated_period(
            generated.begin() + static_cast<std::ptrdiff_t>(shift),
            generated.begin()
                + static_cast<std::ptrdiff_t>(shift + period_samples));
        best = std::min(
            best,
            cyclePairDistance(target_cycle, resampleCycle32(generated_period)));
    }
    return best;
}

constexpr std::uint32_t kOpllClock = kChipClock;
constexpr std::uint32_t kOpllFitSampleRate = kAuditionSampleRate;
constexpr std::size_t kEnvelopeBins = 32;
constexpr std::size_t kSpectralBins = 64;
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
    std::uint8_t midi_note{60};
    std::size_t key_off_sample{std::numeric_limits<std::size_t>::max()};
};

std::uint8_t nearestMidiNote(float frequency_hz) {
    if (!(frequency_hz > 0.0F) || !std::isfinite(frequency_hz)) {
        return 60;
    }
    const double note =
        69.0 + 12.0 * std::log2(static_cast<double>(frequency_hz) / 440.0);
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(note)),
        24,
        119));
}

float midiFrequency(std::uint8_t midi_note) {
    return static_cast<float>(
        440.0 * std::pow(
            2.0,
            (static_cast<double>(midi_note) - 69.0) / 12.0));
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

// Score OPLL patches by rendering at the same MIDI note as the SCC/cycle
// reference and comparing both broadband features and one SCC-period window.
class SameMidiScoreContext {
public:
    SameMidiScoreContext(
        std::span<const float> cycle,
        float fundamental_hz,
        std::uint8_t midi_note)
        : fundamental_hz_(fundamental_hz),
          midi_note_(midi_note),
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
          target_features_(timedFeatures(target_, fundamental_hz_)) {}

    float score(const OpllPatchParameters& patch) {
        const auto registers = encodeOpllPatch(patch);
        if (const auto found = cache_.find(registers);
            found != cache_.end()) {
            return found->second;
        }
        const auto generated =
            renderOpllAudition(patch, midi_note_, kAuditionSamples);
        TimedFeatureWorkspace workspace;
        float distance = featureDistance(
            target_features_,
            timedFeatures(generated, fundamental_hz_, workspace));
        distance += periodWindowDistance(
                        target_,
                        generated,
                        period_samples_)
            * 1.25F;
        cache_.emplace(registers, distance);
        return distance;
    }

private:
    float fundamental_hz_;
    std::uint8_t midi_note_;
    std::size_t period_samples_;
    std::vector<float> target_;
    TimedAudioFeatures target_features_;
    std::map<std::array<std::uint8_t, 8>, float> cache_;
};

class OpllTimedRenderer {
public:
    OpllTimedRenderer()
        : chip_(OPLL_new(kOpllClock, kOpllFitSampleRate)) {}

    const std::vector<float>& render(
        const OpllPatchParameters& patch,
        std::uint8_t midi_note,
        std::size_t sample_count,
        std::size_t key_off_sample) {
        result_.assign(sample_count, 0.0F);
        NotePitch pitch{};
        if (!chip_ || !notePitch(midi_note, pitch)) {
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
            static_cast<std::uint8_t>(pitch.opll.f_number & 0xFF));
        OPLL_writeReg(chip_.get(), 0x30, 0x00);
        const std::uint8_t pitch_without_key = static_cast<std::uint8_t>(
            ((pitch.opll.f_number >> 8) & 1)
            | ((pitch.opll.block & 7) << 1));
        OPLL_writeReg(
            chip_.get(),
            0x20,
            static_cast<std::uint8_t>(pitch_without_key | 0x10));
        for (std::size_t index = 0; index < sample_count; ++index) {
            if (index == key_off_sample) {
                OPLL_writeReg(chip_.get(), 0x20, pitch_without_key);
            }
            result_[index] =
                static_cast<float>(OPLL_calc(chip_.get()));
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

    void remember(
        const OpllPatchParameters& patch,
        float distance) {
        cache_.insert_or_assign(encodeOpllPatch(patch), distance);
    }

    float score(const OpllPatchParameters& patch) {
        const auto registers = encodeOpllPatch(patch);
        if (const auto found = cache_.find(registers);
            found != cache_.end()) {
            return found->second;
        }
        const auto& rendered = renderer_.render(
            patch,
            target_.midi_note,
            target_.samples.size(),
            target_.key_off_sample);
        const float distance = featureDistance(
            target_.features,
            timedFeatures(
                rendered,
                fundamental_hz_,
                feature_workspace_));
        cache_.emplace(registers, distance);
        return distance;
    }

private:
    const OpllFitTarget& target_;
    float fundamental_hz_;
    OpllTimedRenderer renderer_;
    TimedFeatureWorkspace feature_workspace_;
    std::map<std::array<std::uint8_t, 8>, float> cache_;
};

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
    target.midi_note = nearestMidiNote(target.fundamental_hz);
    target.features = timedFeatures(
        target.samples,
        target.fundamental_hz);
    target.key_off_sample =
        detectKeyOff(target.features.envelope, target.samples.size());
    return target;
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
    const auto normalized = normalizedCycle(cycle);
    if (normalized.size() < 2) {
        return {defaultOpllPatch()};
    }
    NotePitch pitch{};
    if (!notePitch(kAuditionMidiNote, pitch)) {
        return {defaultOpllPatch()};
    }
    const float fundamental_hz = sccFrequencyHz(pitch.psg_scc_period);
    SameMidiScoreContext scoring(
        normalized,
        fundamental_hz,
        kAuditionMidiNote);
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
    struct ScoredPatch {
        float distance{std::numeric_limits<float>::max()};
        OpllPatchParameters patch{};
    };
    std::array<ScoredPatch, 8> leaders{};
    std::size_t leader_count{};
    const auto score = [&](const OpllPatchParameters& patch) {
        return scoring.score(patch);
    };
    auto consider = [&](const OpllPatchParameters& patch) {
        const ScoredPatch entry{score(patch), patch};
        std::size_t position{};
        while (position < leader_count
               && leaders[position].distance <= entry.distance) {
            ++position;
        }
        if (position >= leaders.size()) {
            return;
        }
        const std::size_t new_count =
            std::min(leaders.size(), leader_count + 1);
        for (std::size_t index = new_count - 1;
             index > position;
             --index) {
            leaders[index] = leaders[index - 1];
        }
        leaders[position] = entry;
        leader_count = new_count;
    };

    consider(prepare(defaultOpllPatch()));
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            consider(prepare(*rom));
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
        consider(candidate);
    }

    // Insurance: always evaluate MULTI 1 and 2 with quiet modulator.
    for (const std::uint8_t car_mul : {1, 2}) {
        auto candidate = prepare(defaultOpllPatch());
        candidate.modulator.multiplier = 0;
        candidate.carrier.multiplier = car_mul;
        candidate.modulator.total_level = 63;
        candidate.feedback = 0;
        consider(candidate);
    }

    const auto initial_leaders = leaders;
    const std::size_t initial_count = leader_count;
    for (std::size_t seed = 0;
         seed < std::min<std::size_t>(initial_count, 4);
         ++seed) {
        auto best = initial_leaders[seed].patch;
        float best_distance = initial_leaders[seed].distance;
        auto improve = [&](OpllPatchParameters candidate) {
            const float distance = score(candidate);
            if (distance < best_distance) {
                best_distance = distance;
                best = candidate;
            }
        };
        for (int round = 0; round < 1; ++round) {
            for (std::uint8_t value = 0; value < 16; ++value) {
                auto candidate = best;
                candidate.modulator.multiplier = value;
                improve(candidate);
            }
            for (std::uint8_t value = 0; value < 16; ++value) {
                auto candidate = best;
                candidate.carrier.multiplier = value;
                improve(candidate);
            }
            for (std::uint8_t value = 0; value < 64; ++value) {
                auto candidate = best;
                candidate.modulator.total_level = value;
                improve(candidate);
            }
            for (std::uint8_t value = 0; value < 8; ++value) {
                auto candidate = best;
                candidate.feedback = value;
                improve(candidate);
            }
            for (int value = 0; value < 4; ++value) {
                auto candidate = best;
                candidate.modulator.waveform = (value & 1) != 0;
                candidate.carrier.waveform = (value & 2) != 0;
                improve(candidate);
            }
            for (int value = 0; value < 4; ++value) {
                auto candidate = best;
                candidate.modulator.pitch_modulation = (value & 1) != 0;
                candidate.carrier.pitch_modulation = (value & 2) != 0;
                improve(candidate);
            }
        }
        consider(best);
    }

    std::vector<OpllPatchParameters> result;
    result.reserve(leader_count);
    for (std::size_t index = 0; index < leader_count; ++index) {
        const auto registers = encodeOpllPatch(leaders[index].patch);
        const bool duplicate = std::any_of(
            result.begin(),
            result.end(),
            [&](const OpllPatchParameters& existing) {
                return encodeOpllPatch(existing) == registers;
            });
        if (!duplicate) {
            result.push_back(leaders[index].patch);
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(const SccWaveform& waveform) {
    std::array<float, 32> cycle{};
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        cycle[index] = static_cast<float>(waveform[index]) / 128.0F;
    }
    return approximateWaveCycleCandidatesWithOpll(cycle);
}

std::vector<OpllPatchParameters> approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm) {
    const auto cycle_analysis = analyzeWaveCycle(pcm);
    if (cycle_analysis.cycle.size() < 2) {
        return {defaultOpllPatch()};
    }
    const auto target = makeFitTarget(pcm, cycle_analysis);
    if (target.samples.size() < 2) {
        return {approximateWaveCycleWithOpll(cycle_analysis.cycle)};
    }

    struct ScoredPatch {
        float distance{std::numeric_limits<float>::max()};
        OpllPatchParameters patch{};
    };
    constexpr std::size_t leader_capacity = 6;
    std::array<ScoredPatch, leader_capacity> leaders{};
    std::size_t leader_count{};
    // Besides caching the global candidates, constructing this context before
    // worker launch completes emu2413's one-time global table initialization.
    TimedOpllScoreContext initial_scoring(target);
    const auto score = [&](const OpllPatchParameters& patch) {
        return initial_scoring.score(patch);
    };
    auto considerScored = [&](const ScoredPatch& entry) {
        std::size_t position{};
        while (position < leader_count
               && leaders[position].distance <= entry.distance) {
            ++position;
        }
        if (position >= leaders.size()) {
            return;
        }
        const std::size_t new_count =
            std::min(leaders.size(), leader_count + 1);
        for (std::size_t index = new_count - 1;
             index > position;
             --index) {
            leaders[index] = leaders[index - 1];
        }
        leaders[position] = entry;
        leader_count = new_count;
    };
    const auto consider = [&](const OpllPatchParameters& patch) {
        considerScored(ScoredPatch{score(patch), patch});
    };

    std::vector<OpllPatchParameters> seed_patches;
    seed_patches.reserve(64);
    seed_patches.push_back(
        approximateWaveCycleWithOpll(cycle_analysis.cycle));
    seed_patches.push_back(defaultOpllPatch());
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            seed_patches.push_back(*rom);
        }
    }

    // A fixed generator makes the global coverage reproducible while sampling
    // every category of the YM2413 custom patch, including its envelope.
    std::uint32_t random = 0x5749464DU;  // "WIFM"
    const auto next_random = [&]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    for (int index = 0; index < 32; ++index) {
        OpllPatchParameters candidate = defaultOpllPatch();
        std::uint32_t bits = next_random();
        candidate.modulator.multiplier =
            static_cast<std::uint8_t>(bits & 15);
        candidate.carrier.multiplier =
            static_cast<std::uint8_t>((bits >> 4) & 15);
        candidate.feedback =
            static_cast<std::uint8_t>((bits >> 8) & 7);
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
        candidate.modulator.key_scale_level =
            static_cast<std::uint8_t>((bits >> 21) & 3);
        candidate.carrier.key_scale_level =
            static_cast<std::uint8_t>((bits >> 23) & 3);

        bits = next_random();
        candidate.modulator.total_level =
            static_cast<std::uint8_t>(bits & 63);
        candidate.modulator.attack_rate =
            static_cast<std::uint8_t>((bits >> 6) & 15);
        candidate.modulator.decay_rate =
            static_cast<std::uint8_t>((bits >> 10) & 15);
        candidate.modulator.sustain_level =
            static_cast<std::uint8_t>((bits >> 14) & 15);
        candidate.modulator.release_rate =
            static_cast<std::uint8_t>((bits >> 18) & 15);
        candidate.carrier.attack_rate =
            static_cast<std::uint8_t>((bits >> 22) & 15);
        candidate.carrier.decay_rate =
            static_cast<std::uint8_t>((bits >> 26) & 15);

        bits = next_random();
        candidate.carrier.sustain_level =
            static_cast<std::uint8_t>(bits & 15);
        candidate.carrier.release_rate =
            static_cast<std::uint8_t>((bits >> 4) & 15);
        seed_patches.push_back(candidate);
    }

    // Score seeds in parallel, then merge in seed order so ties stay
    // deterministic (identical to sequential consider() order).
    std::vector<ScoredPatch> scored_seeds(seed_patches.size());
    std::vector<std::future<void>> seed_jobs;
    seed_jobs.reserve(seed_patches.size());
    std::size_t launched = 0;
    try {
        for (std::size_t index = 0; index < seed_patches.size(); ++index) {
            seed_jobs.push_back(std::async(
                std::launch::async,
                [&target, &seed_patches, &scored_seeds, index] {
                    TimedOpllScoreContext local_scoring(target);
                    scored_seeds[index] = ScoredPatch{
                        local_scoring.score(seed_patches[index]),
                        seed_patches[index]};
                }));
            ++launched;
        }
    } catch (const std::system_error&) {
        // Keep already-launched jobs; remainder scored below.
    }
    for (auto& job : seed_jobs) {
        job.get();
    }
    for (std::size_t index = 0; index < launched; ++index) {
        considerScored(scored_seeds[index]);
        initial_scoring.remember(
            scored_seeds[index].patch,
            scored_seeds[index].distance);
    }
    for (std::size_t index = launched;
         index < seed_patches.size();
         ++index) {
        consider(seed_patches[index]);
    }

    const auto initial_leaders = leaders;
    const std::size_t initial_count = leader_count;
    constexpr std::array<std::uint8_t, 6> envelope_coarse{
        0, 3, 6, 9, 12, 15};
    const auto refineSeed = [&](ScoredPatch initial) {
        TimedOpllScoreContext scoring(target);
        scoring.remember(initial.patch, initial.distance);
        auto best = initial.patch;
        float best_distance = initial.distance;
        const auto improve = [&](OpllPatchParameters candidate) {
            const float distance = scoring.score(candidate);
            if (distance < best_distance) {
                best_distance = distance;
                best = candidate;
            }
        };
        const auto searchByte = [&](
            auto member,
            std::span<const std::uint8_t> values) {
            for (const std::uint8_t value : values) {
                auto candidate = best;
                member(candidate) = value;
                improve(candidate);
            }
        };

        // First fit the time-varying hardware envelope at coarse resolution.
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.attack_rate;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.decay_rate;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.sustain_level;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.release_rate;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.attack_rate;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.decay_rate;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.sustain_level;
            },
            envelope_coarse);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.release_rate;
            },
            envelope_coarse);

        std::array<std::uint8_t, 16> values16{};
        for (std::uint8_t value = 0; value < values16.size(); ++value) {
            values16[value] = value;
        }
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.multiplier;
            },
            values16);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.multiplier;
            },
            values16);

        constexpr std::array<std::uint8_t, 9> level_coarse{
            0, 8, 16, 24, 32, 40, 48, 56, 63};
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.total_level;
            },
            level_coarse);
        constexpr std::array<std::uint8_t, 8> feedback_values{
            0, 1, 2, 3, 4, 5, 6, 7};
        searchByte(
            [](auto& patch) -> auto& {
                return patch.feedback;
            },
            feedback_values);

        for (int value = 0; value < 4; ++value) {
            auto candidate = best;
            candidate.modulator.waveform = (value & 1) != 0;
            candidate.carrier.waveform = (value & 2) != 0;
            improve(candidate);
        }
        for (int value = 0; value < 4; ++value) {
            auto candidate = best;
            candidate.modulator.pitch_modulation = (value & 1) != 0;
            candidate.carrier.pitch_modulation = (value & 2) != 0;
            improve(candidate);
        }
        for (int value = 0; value < 4; ++value) {
            auto candidate = best;
            candidate.modulator.amplitude_modulation = (value & 1) != 0;
            candidate.carrier.amplitude_modulation = (value & 2) != 0;
            improve(candidate);
        }
        for (int value = 0; value < 4; ++value) {
            auto candidate = best;
            candidate.modulator.sustained_tone = (value & 1) != 0;
            candidate.carrier.sustained_tone = (value & 2) != 0;
            improve(candidate);
        }
        for (int value = 0; value < 4; ++value) {
            auto candidate = best;
            candidate.modulator.key_rate_scaling = (value & 1) != 0;
            candidate.carrier.key_rate_scaling = (value & 2) != 0;
            improve(candidate);
        }
        constexpr std::array<std::uint8_t, 4> ksl_values{0, 1, 2, 3};
        searchByte(
            [](auto& patch) -> auto& {
                return patch.modulator.key_scale_level;
            },
            ksl_values);
        searchByte(
            [](auto& patch) -> auto& {
                return patch.carrier.key_scale_level;
            },
            ksl_values);

        // Quantization-aware final refinement around every envelope register.
        const auto refine = [&](auto member) {
            const int center = member(best);
            std::array<std::uint8_t, 5> nearby{};
            for (int index = 0; index < 5; ++index) {
                nearby[static_cast<std::size_t>(index)] =
                    static_cast<std::uint8_t>(
                        std::clamp(center + index - 2, 0, 15));
            }
            searchByte(member, nearby);
        };
        refine([](auto& patch) -> auto& {
            return patch.modulator.attack_rate;
        });
        refine([](auto& patch) -> auto& {
            return patch.modulator.decay_rate;
        });
        refine([](auto& patch) -> auto& {
            return patch.modulator.sustain_level;
        });
        refine([](auto& patch) -> auto& {
            return patch.modulator.release_rate;
        });
        refine([](auto& patch) -> auto& {
            return patch.carrier.attack_rate;
        });
        refine([](auto& patch) -> auto& {
            return patch.carrier.decay_rate;
        });
        refine([](auto& patch) -> auto& {
            return patch.carrier.sustain_level;
        });
        refine([](auto& patch) -> auto& {
            return patch.carrier.release_rate;
        });
        return ScoredPatch{best_distance, best};
    };

    const std::size_t seed_count =
        std::min<std::size_t>(initial_count, 3);
    std::vector<std::future<ScoredPatch>> refinements;
    refinements.reserve(seed_count);
    try {
        for (std::size_t seed = 0; seed < seed_count; ++seed) {
            refinements.push_back(std::async(
                std::launch::async,
                refineSeed,
                initial_leaders[seed]));
        }
    } catch (const std::system_error&) {
        // A restricted runtime may refuse to create all worker threads.
        // Already-started seeds are joined below and the remainder use the
        // identical refinement routine synchronously.
    }
    // Merge in the original seed order so equal-distance tie handling and
    // candidate ordering stay deterministic regardless of thread completion.
    for (auto& refinement : refinements) {
        considerScored(refinement.get());
    }
    for (std::size_t seed = refinements.size();
         seed < seed_count;
         ++seed) {
        considerScored(refineSeed(initial_leaders[seed]));
    }
    std::vector<OpllPatchParameters> result;
    result.reserve(leader_count);
    for (std::size_t index = 0; index < leader_count; ++index) {
        const auto registers = encodeOpllPatch(leaders[index].patch);
        const bool duplicate = std::any_of(
            result.begin(),
            result.end(),
            [&](const OpllPatchParameters& existing) {
                return encodeOpllPatch(existing) == registers;
            });
        if (!duplicate) {
            result.push_back(leaders[index].patch);
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

OpllPatchParameters approximateWavePcmWithOpll(
    const WavePcm& pcm) {
    auto candidates = approximateWavePcmCandidatesWithOpll(pcm);
    return candidates.front();
}

}  // namespace mgstc::engine
