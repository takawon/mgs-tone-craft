// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace mgstc::app::spectrogram {

constexpr std::size_t kMaximumHarmonicPeaks = 16;
constexpr int kHarmonicSearchRadiusBins = 2;

struct HarmonicPeak {
    float frequency{};
    std::uint8_t level{};
    std::uint8_t harmonic{};
};

struct HarmonicPeakList {
    std::array<HarmonicPeak, kMaximumHarmonicPeaks> peaks{};
    std::uint8_t count{};
};

static_assert(std::is_trivially_copyable_v<HarmonicPeak>);
static_assert(std::is_trivially_copyable_v<HarmonicPeakList>);

struct InterpolatedPeak {
    double bin{};
    float power{};
};

[[nodiscard]] inline InterpolatedPeak interpolatePeak(
    std::span<const float> power_spectrum,
    std::size_t peak_bin) noexcept {
    InterpolatedPeak result{
        static_cast<double>(peak_bin),
        power_spectrum[peak_bin],
    };
    if (peak_bin == 0 || peak_bin + 1 >= power_spectrum.size()) {
        return result;
    }

    constexpr double kMinimumPower = 1.0e-30;
    const double alpha = std::log(std::max(
        static_cast<double>(power_spectrum[peak_bin - 1]),
        kMinimumPower));
    const double beta = std::log(std::max(
        static_cast<double>(power_spectrum[peak_bin]),
        kMinimumPower));
    const double gamma = std::log(std::max(
        static_cast<double>(power_spectrum[peak_bin + 1]),
        kMinimumPower));
    const double denominator = alpha - 2.0 * beta + gamma;
    if (!std::isfinite(denominator) || std::abs(denominator) < 1.0e-12) {
        return result;
    }

    const double offset = std::clamp(
        0.5 * (alpha - gamma) / denominator,
        -0.5,
        0.5);
    if (!std::isfinite(offset)) {
        return result;
    }
    const double interpolated_log_power = beta
        - 0.25 * (alpha - gamma) * offset;
    const double interpolated_power = std::exp(interpolated_log_power);
    result.bin += offset;
    if (std::isfinite(interpolated_power) && interpolated_power > 0.0) {
        result.power = static_cast<float>(interpolated_power);
    }
    return result;
}

[[nodiscard]] inline std::uint8_t quantizePower(
    float power,
    std::size_t fft_size,
    float minimum_db) noexcept {
    if (!(power > 0.0F) || fft_size == 0 || !(minimum_db < 0.0F)) {
        return 0;
    }
    const double reference_power = static_cast<double>(fft_size)
        * static_cast<double>(fft_size);
    const double decibels = 10.0 * std::log10(
        static_cast<double>(power) / reference_power);
    if (!std::isfinite(decibels) || decibels <= minimum_db) {
        return 0;
    }
    const double normalized = std::clamp(
        (decibels - static_cast<double>(minimum_db))
            / -static_cast<double>(minimum_db),
        0.0,
        1.0);
    return static_cast<std::uint8_t>(normalized * 255.0 + 0.5);
}

[[nodiscard]] inline HarmonicPeakList findHarmonicPeaks(
    std::span<const float> power_spectrum,
    double fundamental_frequency,
    double sample_rate,
    std::size_t fft_size,
    float minimum_power,
    float minimum_db) noexcept {
    HarmonicPeakList result{};
    if (power_spectrum.size() < 5
        || !(fundamental_frequency > 0.0)
        || !std::isfinite(fundamental_frequency)
        || !(sample_rate > 0.0)
        || !std::isfinite(sample_rate)
        || fft_size == 0) {
        return result;
    }

    std::array<std::size_t, kMaximumHarmonicPeaks> used_bins{};
    std::size_t used_count = 0;
    const double nyquist = sample_rate * 0.5;
    for (std::size_t harmonic = 1;
         harmonic <= kMaximumHarmonicPeaks;
         ++harmonic) {
        const double expected_frequency = fundamental_frequency
            * static_cast<double>(harmonic);
        if (!(expected_frequency < nyquist)) {
            break;
        }
        const double expected_bin = expected_frequency
            * static_cast<double>(fft_size) / sample_rate;
        const auto centre_bin = static_cast<std::ptrdiff_t>(
            std::llround(expected_bin));
        const auto first_bin = static_cast<std::size_t>(std::max<
            std::ptrdiff_t>(
            2,
            centre_bin - kHarmonicSearchRadiusBins));
        const auto last_bin = static_cast<std::size_t>(std::min<
            std::ptrdiff_t>(
            static_cast<std::ptrdiff_t>(power_spectrum.size()) - 2,
            centre_bin + kHarmonicSearchRadiusBins));
        if (first_bin > last_bin) {
            continue;
        }

        std::size_t best_bin = power_spectrum.size();
        double best_distance = std::numeric_limits<double>::max();
        float best_power = 0.0F;
        const auto used_end = used_bins.begin()
            + static_cast<std::ptrdiff_t>(used_count);
        for (std::size_t bin = first_bin; bin <= last_bin; ++bin) {
            const float power = power_spectrum[bin];
            if (!(power >= minimum_power)
                || !(power > power_spectrum[bin - 1])
                || !(power >= power_spectrum[bin + 1])
                || std::find(used_bins.begin(), used_end, bin) != used_end) {
                continue;
            }
            const double distance = std::abs(
                static_cast<double>(bin) - expected_bin);
            if (distance < best_distance
                || (distance == best_distance && power > best_power)) {
                best_bin = bin;
                best_distance = distance;
                best_power = power;
            }
        }
        if (best_bin == power_spectrum.size()) {
            continue;
        }

        const auto interpolated = interpolatePeak(power_spectrum, best_bin);
        const double frequency = interpolated.bin * sample_rate
            / static_cast<double>(fft_size);
        const auto level = quantizePower(
            interpolated.power, fft_size, minimum_db);
        if (!(frequency > 0.0) || !std::isfinite(frequency) || level == 0) {
            continue;
        }
        result.peaks[result.count++] = {
            static_cast<float>(frequency),
            level,
            static_cast<std::uint8_t>(harmonic),
        };
        used_bins[used_count++] = best_bin;
    }
    return result;
}

[[nodiscard]] inline std::uint8_t composeHarmonicRidgeLevel(
    std::uint8_t original_level,
    std::uint8_t peak_level,
    double distance_pixels,
    double lobe_radius_pixels,
    double strength) noexcept {
    strength = std::clamp(strength, 0.0, 1.0);
    distance_pixels = std::abs(distance_pixels);
    if (strength <= 0.0
        || !(lobe_radius_pixels > 0.0)
        || distance_pixels > lobe_radius_pixels) {
        return original_level;
    }

    const double broad_sigma = std::max(1.0, lobe_radius_pixels * 0.45);
    const double ridge_sigma = broad_sigma
        + strength * (0.45 - broad_sigma);
    const double ridge_weight = std::exp(
        -0.5 * distance_pixels * distance_pixels
        / (ridge_sigma * ridge_sigma));
    const double lobe_weight = std::max(
        0.0, 1.0 - distance_pixels / lobe_radius_pixels);
    const double original = static_cast<double>(original_level);
    const double suppressed = original
        * (1.0 - strength * 0.28 * lobe_weight * (1.0 - ridge_weight));
    const double boosted_peak = static_cast<double>(peak_level)
        + (255.0 - static_cast<double>(peak_level)) * 0.35 * strength;
    const double emphasized = suppressed
        + strength * ridge_weight
            * std::max(0.0, boosted_peak - suppressed);
    return static_cast<std::uint8_t>(
        std::clamp(emphasized, 0.0, 255.0) + 0.5);
}

}  // namespace mgstc::app::spectrogram
