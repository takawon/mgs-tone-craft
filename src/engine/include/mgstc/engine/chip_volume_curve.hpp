// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "mgstc/engine/composite_timbre.hpp"

namespace mgstc::engine {

// Visual / edit mapping for @e volume 0–15 onto a lane height.
// Values are linear amplitude (0 silent … 1 loudest), matching the chip
// output tables rather than a linear nibble axis.
//
// PSG: YM2149 32-step voltbl, 4-bit volume written as (n << 1) (emu2149).
// SCC: emu2212 multiplies volume * waveform (linear).
// OPLL: MGS 15 is loudest; each step is 3 dB (YM2413 volume nibble).

[[nodiscard]] inline constexpr std::array<float, 16>
psgYm2149VolumeAmplitude() noexcept {
    // emu2149 voltbl[0] even indices 0,2,…,30 (register value << 1), / 0xD6.
    return {
        0.0F, 1.0F / 214.0F, 2.0F / 214.0F, 3.0F / 214.0F,
        5.0F / 214.0F, 7.0F / 214.0F, 11.0F / 214.0F, 15.0F / 214.0F,
        22.0F / 214.0F, 31.0F / 214.0F, 45.0F / 214.0F, 63.0F / 214.0F,
        90.0F / 214.0F, 127.0F / 214.0F, 180.0F / 214.0F, 1.0F};
}

[[nodiscard]] inline float chipVolumeAmplitude(
    TimbreSource source, int volume) noexcept {
    const int vol = std::clamp(volume, 0, 15);
    switch (source) {
    case TimbreSource::Psg: {
        constexpr auto table = psgYm2149VolumeAmplitude();
        return table[static_cast<std::size_t>(vol)];
    }
    case TimbreSource::Scc:
        return static_cast<float>(vol) / 15.0F;
    case TimbreSource::Opll: {
        // 3 dB per MGS step; volume 15 = 0 dB (loudest).
        const int atten_steps = 15 - vol;
        return std::pow(10.0F, -0.15F * static_cast<float>(atten_steps));
    }
    }
    return static_cast<float>(vol) / 15.0F;
}

[[nodiscard]] inline int chipVolumeFromAmplitude(
    TimbreSource source, float amplitude) noexcept {
    const float target = std::clamp(amplitude, 0.0F, 1.0F);
    int best = 0;
    float best_err = std::abs(chipVolumeAmplitude(source, 0) - target);
    for (int vol = 1; vol <= 15; ++vol) {
        const float err =
            std::abs(chipVolumeAmplitude(source, vol) - target);
        if (err < best_err) {
            best_err = err;
            best = vol;
        }
    }
    return best;
}

// Smallest adjacent amplitude step on a 0–15 lane (quiet end for OPLL/PSG).
[[nodiscard]] inline float chipVolumeMinAmplitudeStep(
    TimbreSource source) noexcept {
    float min_step = 1.0F;
    for (int volume = 0; volume < 15; ++volume) {
        const float step = std::abs(
            chipVolumeAmplitude(source, volume + 1)
            - chipVolumeAmplitude(source, volume));
        if (step > 0.0F) {
            min_step = std::min(min_step, step);
        }
    }
    return min_step;
}

// Lane height so the worst-case chip step is at least 1 screen pixel
// without display rounding. The editor uses a shorter lane plus
// `chipVolumeDisplayPixelsTable` instead.
[[nodiscard]] inline int chipVolumeLaneMinPixels() noexcept {
    const float worst = std::min({
        chipVolumeMinAmplitudeStep(TimbreSource::Psg),
        chipVolumeMinAmplitudeStep(TimbreSource::Scc),
        chipVolumeMinAmplitudeStep(TimbreSource::Opll)});
    return std::max(1, static_cast<int>(std::ceil(1.0F / worst)));
}

// Integer pixels from the lane bottom (0 silent … height = loudest) after
// rounding the chip curve so consecutive 0–15 volumes never share a row.
// Quiet steps that would be sub-pixel are pushed up by 1px; loud steps keep
// the true amplitude spacing when it already fits.
[[nodiscard]] inline std::array<int, 16> chipVolumeDisplayPixelsTable(
    TimbreSource source, int lane_height) noexcept {
    const int height = std::max(1, lane_height);
    std::array<int, 16> pixels{};
    for (int volume = 0; volume <= 15; ++volume) {
        const float desired =
            chipVolumeAmplitude(source, volume)
            * static_cast<float>(height);
        const int rounded = std::clamp(
            static_cast<int>(std::lround(desired)), 0, height);
        if (volume == 0) {
            pixels[0] = rounded;
        } else {
            pixels[static_cast<std::size_t>(volume)] = std::max(
                rounded, pixels[static_cast<std::size_t>(volume - 1)] + 1);
        }
    }
    if (pixels[15] > height) {
        pixels[15] = height;
        for (int volume = 14; volume >= 0; --volume) {
            const int capped = pixels[static_cast<std::size_t>(volume + 1)] - 1;
            pixels[static_cast<std::size_t>(volume)] = std::max(
                0,
                std::min(pixels[static_cast<std::size_t>(volume)], capped));
        }
    }
    return pixels;
}

[[nodiscard]] inline int chipVolumeDisplayPixelsFromBottom(
    TimbreSource source, int volume, int lane_height) noexcept {
    const auto table = chipVolumeDisplayPixelsTable(source, lane_height);
    return table[static_cast<std::size_t>(std::clamp(volume, 0, 15))];
}

[[nodiscard]] inline int chipVolumeFromDisplayPixels(
    TimbreSource source, float pixels_from_bottom, int lane_height) noexcept {
    const int height = std::max(1, lane_height);
    const float target = std::clamp(
        pixels_from_bottom, 0.0F, static_cast<float>(height));
    const auto table = chipVolumeDisplayPixelsTable(source, height);
    int best = 0;
    float best_err = std::abs(static_cast<float>(table[0]) - target);
    for (int volume = 1; volume <= 15; ++volume) {
        const float err = std::abs(
            static_cast<float>(table[static_cast<std::size_t>(volume)])
            - target);
        if (err < best_err) {
            best_err = err;
            best = volume;
        }
    }
    return best;
}

[[nodiscard]] inline int chipVolumeFromDisplayPixels(
    TimbreSource source, int pixels_from_bottom, int lane_height) noexcept {
    return chipVolumeFromDisplayPixels(
        source, static_cast<float>(pixels_from_bottom), lane_height);
}

}  // namespace mgstc::engine
