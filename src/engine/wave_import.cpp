#include "mgstc/engine/wave_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>

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

std::array<float, 32> renderOpllCycle(
    const OpllPatchParameters& patch) {
    constexpr std::uint32_t clock = 3'579'545;
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr std::uint8_t midi_note = 60;
    std::array<float, 32> result{};
    NotePitch pitch{};
    if (!notePitch(midi_note, pitch)) {
        return result;
    }
    std::unique_ptr<OPLL, OpllDeleter> chip(
        OPLL_new(clock, sample_rate));
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
        chip.get(), 0x10,
        static_cast<std::uint8_t>(pitch.opll.f_number & 0xFF));
    OPLL_writeReg(chip.get(), 0x30, 0x00);
    const auto pitch_register = static_cast<std::uint8_t>(
        ((pitch.opll.f_number >> 8) & 1)
        | ((pitch.opll.block & 7) << 1)
        | 0x10);
    OPLL_writeReg(chip.get(), 0x20, pitch_register);
    constexpr std::size_t warmup = 768;
    constexpr double period = 48000.0 / 261.625565;
    std::array<float, 192> source{};
    for (std::size_t index = 0; index < warmup + source.size(); ++index) {
        const float sample = static_cast<float>(OPLL_calc(chip.get()));
        if (index >= warmup) {
            source[index - warmup] = sample;
        }
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * period / result.size();
        const auto first = static_cast<std::size_t>(position);
        const auto second = std::min(first + 1, source.size() - 1);
        const float fraction =
            static_cast<float>(position - static_cast<double>(first));
        result[index] =
            source[first] * (1.0F - fraction) + source[second] * fraction;
    }
    const auto normalized = normalizedCycle(result);
    std::copy(normalized.begin(), normalized.end(), result.begin());
    return result;
}

float candidateDistance(
    const std::array<float, 16>& target_spectrum,
    const std::array<float, 32>& target_cycle,
    const OpllPatchParameters& patch) {
    const auto generated_cycle = renderOpllCycle(patch);
    const auto generated_spectrum = spectrum(generated_cycle);
    float distance{};
    for (std::size_t index = 1; index < target_spectrum.size(); ++index) {
        const float weight = 1.0F / std::sqrt(static_cast<float>(index));
        const float difference =
            std::log1p(target_spectrum[index] * 40.0F)
            - std::log1p(generated_spectrum[index] * 40.0F);
        distance += weight * difference * difference;
    }
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
        float best = std::numeric_limits<float>::max();
        for (std::size_t lag = minimum; lag <= maximum; ++lag) {
            const float difference = periodicDifference(samples, lag);
            if (difference < best) {
                best = difference;
                period = lag;
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
    const auto normalized = normalizedCycle(cycle);
    const auto target_spectrum = spectrum(normalized);
    const auto target_cycle = resampleCycle32(normalized);
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
        return candidateDistance(
            target_spectrum,
            target_cycle,
            patch);
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
        candidate.carrier.multiplier =
            static_cast<std::uint8_t>(1 + ((bits >> 4) % 15));
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
            for (std::uint8_t value = 1; value < 16; ++value) {
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
    return leaders[0].patch;
}

}  // namespace mgstc::engine
