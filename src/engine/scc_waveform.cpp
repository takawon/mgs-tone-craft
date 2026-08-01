#include "mgstc/engine/scc_waveform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mgstc::engine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSilence = 1.0e-9;

using WorkingWave = std::array<double, 32>;

double presetSample(
    SccWavePreset preset,
    double phase) noexcept {
    phase -= std::floor(phase);
    switch (preset) {
    case SccWavePreset::Sine:
        return std::sin(2.0 * kPi * phase);
    case SccWavePreset::Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case SccWavePreset::Triangle:
        return 1.0 - 4.0 * std::abs(phase - 0.5);
    case SccWavePreset::SawUp:
        return phase * 2.0 - 1.0;
    case SccWavePreset::SawDown:
        return 1.0 - phase * 2.0;
    case SccWavePreset::Pulse25:
        return phase < 0.25 ? 1.0 : -1.0;
    case SccWavePreset::Pulse12_5:
        return phase < 0.125 ? 1.0 : -1.0;
    }
    return 0.0;
}

WorkingWave generateWorking(
    SccWavePreset preset,
    double cycles) noexcept {
    WorkingWave result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = presetSample(
            preset,
            cycles * static_cast<double>(index)
                / static_cast<double>(result.size()));
    }
    return result;
}

double mean(const WorkingWave& waveform) noexcept {
    double total{};
    for (const double sample : waveform) {
        total += sample;
    }
    return total / static_cast<double>(waveform.size());
}

void removeDc(WorkingWave& waveform) noexcept {
    const double average = mean(waveform);
    for (double& sample : waveform) {
        sample -= average;
    }
}

double rms(const WorkingWave& waveform) noexcept {
    double sum{};
    for (const double sample : waveform) {
        sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<double>(waveform.size()));
}

double peak(const WorkingWave& waveform) noexcept {
    double result{};
    for (const double sample : waveform) {
        result = std::max(result, std::abs(sample));
    }
    return result;
}

WorkingWave toWorking(const SccWaveform& waveform) noexcept {
    WorkingWave result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<double>(waveform[index]);
    }
    return result;
}

SccWaveform quantizeClamped(
    const WorkingWave& waveform) noexcept {
    SccWaveform result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::int8_t>(
            std::clamp<long>(
                std::lround(waveform[index]),
                -128,
                127));
    }
    return result;
}

SccWaveform quantize(WorkingWave waveform) noexcept {
    const double maximum = peak(waveform);
    if (maximum > 127.0) {
        const double scale = 127.0 / maximum;
        for (double& sample : waveform) {
            sample *= scale;
        }
    }
    return quantizeClamped(waveform);
}

SccWaveform quantizeFullRange(const WorkingWave& waveform) noexcept {
    const auto [minimum_it, maximum_it] =
        std::minmax_element(waveform.begin(), waveform.end());
    const double minimum = *minimum_it;
    const double maximum = *maximum_it;
    const double range = maximum - minimum;
    if (range <= kSilence) {
        return {};
    }

    SccWaveform result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (std::abs(waveform[index]) <= kSilence) {
            result[index] = 0;
            continue;
        }
        const double scaled =
            -128.0 + (waveform[index] - minimum) * 255.0 / range;
        result[index] = static_cast<std::int8_t>(
            std::clamp<long>(std::lround(scaled), -128, 127));
    }
    return result;
}

WorkingWave shifted(
    const WorkingWave& waveform,
    std::size_t amount,
    bool inverted) noexcept {
    WorkingWave result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double sample =
            waveform[(index + amount) % waveform.size()];
        result[index] = inverted ? -sample : sample;
    }
    return result;
}

double normalizedCorrelation(
    const WorkingWave& left,
    const WorkingWave& right) noexcept {
    double product{};
    double left_square{};
    double right_square{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        product += left[index] * right[index];
        left_square += left[index] * left[index];
        right_square += right[index] * right[index];
    }
    const double divisor = std::sqrt(left_square * right_square);
    return divisor <= kSilence ? 0.0 : product / divisor;
}

}  // namespace

SccWaveform generateSccPreset(
    SccWavePreset preset,
    SccHarmonic harmonic) noexcept {
    WorkingWave waveform{};
    if (harmonic == SccHarmonic::OneAndHalf) {
        const auto one = generateWorking(preset, 1.0);
        const auto two = generateWorking(preset, 2.0);
        for (std::size_t index = 0; index < waveform.size(); ++index) {
            waveform[index] = (one[index] + two[index]) * 0.5;
        }
    } else {
        double cycles = 1.0;
        switch (harmonic) {
        case SccHarmonic::Two:
            cycles = 2.0;
            break;
        case SccHarmonic::Three:
            cycles = 3.0;
            break;
        case SccHarmonic::Four:
            cycles = 4.0;
            break;
        default:
            break;
        }
        waveform = generateWorking(preset, cycles);
    }
    removeDc(waveform);
    return quantizeFullRange(waveform);
}

SccMergeResult mergeSccWaveforms(
    const SccWaveform& current,
    const SccWaveform& added,
    SccMergeOptions options) noexcept {
    options.amount = std::clamp(options.amount, 0.0, 1.0);
    WorkingWave left = toWorking(current);
    WorkingWave right = toWorking(added);
    removeDc(left);
    removeDc(right);

    std::size_t best_shift{};
    bool best_inverted{};
    double best_correlation = -std::numeric_limits<double>::infinity();
    const std::size_t shift_count = options.auto_phase ? right.size() : 1;
    const int polarity_count =
        options.allow_polarity_inversion ? 2 : 1;
    for (std::size_t shift = 0; shift < shift_count; ++shift) {
        for (int polarity = 0; polarity < polarity_count; ++polarity) {
            const auto candidate = shifted(
                right,
                shift,
                polarity != 0);
            const double correlation =
                normalizedCorrelation(left, candidate);
            if (correlation > best_correlation) {
                best_correlation = correlation;
                best_shift = shift;
                best_inverted = polarity != 0;
            }
        }
    }
    right = shifted(right, best_shift, best_inverted);
    const auto aligned_original = shifted(
        toWorking(added),
        best_shift,
        best_inverted);
    const auto current_original = toWorking(current);
    const bool identical_after_alignment = std::equal(
        current_original.begin(),
        current_original.end(),
        aligned_original.begin(),
        [](double left_sample, double right_sample) {
            return std::abs(left_sample - right_sample) <= kSilence;
        });
    if (identical_after_alignment) {
        return {
            .waveform = current,
            .circular_shift = best_shift,
            .polarity_inverted = best_inverted,
        };
    }

    const double left_rms = rms(left);
    const double right_rms = rms(right);
    double target_rms{};
    if (options.preserve_volume) {
        target_rms =
            left_rms > kSilence ? left_rms : right_rms;
        if (right_rms > kSilence && target_rms > kSilence) {
            const double scale = target_rms / right_rms;
            for (double& sample : right) {
                sample *= scale;
            }
        }
    }

    WorkingWave mixed{};
    for (std::size_t index = 0; index < mixed.size(); ++index) {
        mixed[index] =
            left[index] * (1.0 - options.amount)
            + right[index] * options.amount;
    }
    removeDc(mixed);
    if (options.preserve_volume && target_rms > kSilence) {
        const double mixed_rms = rms(mixed);
        if (mixed_rms > kSilence) {
            const double scale = target_rms / mixed_rms;
            for (double& sample : mixed) {
                sample *= scale;
            }
        }
    }
    return {
        .waveform = quantize(mixed),
        .circular_shift = best_shift,
        .polarity_inverted = best_inverted,
    };
}

SccWaveform averageSccWaveform(
    const SccWaveform& waveform) noexcept {
    SccWaveform result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        const int sum =
            static_cast<int>(
                waveform[(index + waveform.size() - 1)
                    % waveform.size()])
            + static_cast<int>(waveform[index])
            + static_cast<int>(
                waveform[(index + 1) % waveform.size()]);
        result[index] = static_cast<std::int8_t>(
            std::clamp<long>(std::lround(sum / 3.0), -128, 127));
    }
    return result;
}

SccWaveform normalizeSccWaveform(
    const SccWaveform& waveform) noexcept {
    WorkingWave result = toWorking(waveform);
    removeDc(result);
    const auto [minimum, maximum] = std::minmax_element(
        result.begin(), result.end());
    const double low = *minimum;
    const double high = *maximum;
    const double span = high - low;
    if (span <= kSilence) {
        return quantize(result);
    }
    for (double& sample : result) {
        sample = -128.0 + (sample - low) * 255.0 / span;
    }
    return quantizeClamped(result);
}

SccWaveform invertSccWaveform(
    const SccWaveform& waveform) noexcept {
    SccWaveform result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::int8_t>(
            std::clamp(
                -static_cast<int>(waveform[index]),
                -128,
                127));
    }
    return result;
}

SccWaveform rotateSccWaveform(
    const SccWaveform& waveform,
    int samples) noexcept {
    SccWaveform result{};
    constexpr int size = static_cast<int>(SccWaveform{}.size());
    const int normalized = ((samples % size) + size) % size;
    for (int index = 0; index < size; ++index) {
        result[static_cast<std::size_t>(index)] =
            waveform[static_cast<std::size_t>(
                (index - normalized + size) % size)];
    }
    return result;
}

SccWaveform shiftSccWaveformVertically(
    const SccWaveform& waveform,
    int amount) noexcept {
    const auto [minimum, maximum] = std::minmax_element(
        waveform.begin(),
        waveform.end());
    const int limited = std::clamp(
        amount,
        -128 - static_cast<int>(*minimum),
        127 - static_cast<int>(*maximum));
    SccWaveform result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::int8_t>(
            static_cast<int>(waveform[index]) + limited);
    }
    return result;
}

SccWaveform applySccWaveformRange(
    const SccWaveform& current,
    const SccWaveform& candidate,
    SccApplyRange range) noexcept {
    if (range == SccApplyRange::All) {
        return candidate;
    }
    SccWaveform result = current;
    const std::size_t first =
        range == SccApplyRange::LeftHalf ? 0 : result.size() / 2;
    const std::size_t last =
        range == SccApplyRange::LeftHalf
        ? result.size() / 2
        : result.size();
    std::copy(
        candidate.begin() + static_cast<std::ptrdiff_t>(first),
        candidate.begin() + static_cast<std::ptrdiff_t>(last),
        result.begin() + static_cast<std::ptrdiff_t>(first));
    return result;
}

SccWaveform scaleSccWaveformVertically(
    const SccWaveform& waveform,
    int percent) noexcept {
    const int clamped_percent = std::clamp(percent, 0, 200);
    SccWaveform result{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        const double scaled =
            static_cast<double>(waveform[index])
            * static_cast<double>(clamped_percent)
            / 100.0;
        result[index] = static_cast<std::int8_t>(
            std::clamp<long>(std::lround(scaled), -128, 127));
    }
    return result;
}

}  // namespace mgstc::engine
