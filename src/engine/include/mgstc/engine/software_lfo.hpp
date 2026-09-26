// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/note_pitch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace mgstc::engine {

// MGSDRV 3.20 software pitch LFO (pre-key-on MGSC `h` / `@p`).
// Integrating triangle: n4 is the signed pitch-up step. Non-zero `@p` on
// PSG/SCC is a signed add to the pre-octave period instead (pitch falls
// when the value is positive).
// Applied as period/F-number FrequencyDelta each 60 Hz interrupt.
// Not chip hardware AM/PM, and not OPLL built-in vibrato.
// Delay/speed use the public MGS stored-byte encoding (param+1, 0 ⇒ 256).
struct SoftwareLfoSettings {
    bool enabled{};
    std::uint8_t delay{};              // n1 0–255
    std::uint8_t depth{};              // n2 0–127
    std::uint8_t speed{};              // n3 0–255
    std::int8_t roughness{};           // n4 −127–127; sign = first direction
    std::int32_t extra_roughness{};    // `@p` −32768–32767; ignored on OPLL

    friend bool operator==(
        const SoftwareLfoSettings&,
        const SoftwareLfoSettings&) = default;
};

// Public MGS encoding: delay/speed bytes are (param + 1) & 0xFF (0 ⇒ 256).
[[nodiscard]] constexpr unsigned softwareLfoStoredParam(
    std::uint8_t param) noexcept {
    return (static_cast<unsigned>(param) + 1U) & 0xFFU;
}

[[nodiscard]] constexpr unsigned softwareLfoDelayTicks(
    std::uint8_t delay) noexcept {
    const auto stored = softwareLfoStoredParam(delay);
    return stored == 0U ? 256U : stored - 1U;
}

[[nodiscard]] constexpr unsigned softwareLfoSpeedPeriod(
    std::uint8_t speed) noexcept {
    const auto stored = softwareLfoStoredParam(speed);
    return stored == 0U ? 256U : stored;
}

// First 60 Hz tick (0-based from key-on) that applies a step. Offset stays 0
// until this tick. Matches the 8-bit (stored delay + stored speed) countdown.
[[nodiscard]] constexpr unsigned softwareLfoFirstUpdateTick(
    std::uint8_t delay,
    std::uint8_t speed) noexcept {
    const unsigned initial =
        (softwareLfoStoredParam(delay) + softwareLfoStoredParam(speed))
        & 0xFFU;
    return (initial == 0U ? 256U : initial) - 1U;
}

// Dialog preview Y range in pitch-offset units. Not Hz. Settings that exceed
// this are clipped in the graph and labelled with the real min/max.
constexpr int kSoftwareLfoPreviewExtent = 127;

[[nodiscard]] inline SoftwareLfoSettings clampSoftwareLfo(
    SoftwareLfoSettings settings,
    bool allow_extra_roughness) noexcept {
    settings.depth = std::min<std::uint8_t>(settings.depth, 127);
    settings.roughness = static_cast<std::int8_t>(
        std::clamp(static_cast<int>(settings.roughness), -127, 127));
    if (!allow_extra_roughness) {
        settings.extra_roughness = 0;
    } else {
        settings.extra_roughness = std::clamp(
            settings.extra_roughness,
            static_cast<std::int32_t>(-32768),
            static_cast<std::int32_t>(32767));
    }
    return settings;
}

// 8-bit n4, added to the current output as a pitch-up step.
// `@p != 0` does not use this step (see softwareLfoOffsetAtTick).
[[nodiscard]] constexpr std::int32_t softwareLfoStep(
    const SoftwareLfoSettings& settings,
    bool apply_extra_roughness) noexcept {
    if (apply_extra_roughness && settings.extra_roughness != 0) {
        return 0;
    }
    return static_cast<std::int32_t>(settings.roughness);
}

[[nodiscard]] constexpr std::int32_t softwareLfoNegate16(
    std::int32_t value) noexcept {
    if (value == static_cast<std::int32_t>(-32768)) {
        return value;
    }
    return -value;
}

// Offset at 60 Hz tick since key-on. Delay is folded into the first speed
// countdown (stored delay + stored speed). Until the first step, offset is 0.
// Pitch-up offset. n4 adds to the current output. Non-zero `@p` on PSG/SCC
// instead adds the signed 16-bit value to the pre-octave word and re-shifts.
// The first such update also drops track `\` (`detune_pitch_up`).
[[nodiscard]] inline std::int32_t softwareLfoOffsetAtTick(
    const SoftwareLfoSettings& settings,
    std::uint32_t tick_from_key_on,
    bool apply_extra_roughness = true,
    PsgSccModulationBase base = {},
    std::int32_t detune_pitch_up = 0) noexcept {
    if (!settings.enabled) {
        return 0;
    }
    const bool wide =
        apply_extra_roughness && settings.extra_roughness != 0;
    auto step = wide
        ? settings.extra_roughness
        : static_cast<std::int32_t>(settings.roughness);
    unsigned counter =
        (softwareLfoStoredParam(settings.delay)
         + softwareLfoStoredParam(settings.speed))
        & 0xFFU;
    const unsigned speed_reload = softwareLfoStoredParam(settings.speed);
    unsigned depth_reload = static_cast<unsigned>(settings.depth) + 1U;
    unsigned depth_count = (depth_reload >> 1) + 1U;
    const auto octave = static_cast<unsigned>(base.octave & 7);
    auto pre = base.unshifted;
    const auto shifted0 = static_cast<std::int32_t>(base.unshifted >> octave);
    std::int32_t offset = 0;
    for (std::uint32_t tick = 0; tick <= tick_from_key_on; ++tick) {
        counter = (counter - 1U) & 0xFFU;
        if (counter != 0U) {
            continue;
        }
        counter = speed_reload;
        --depth_count;
        if (depth_count == 0U) {
            depth_count = depth_reload;
            step = wide ? softwareLfoNegate16(step) : -step;
        }
        if (!wide) {
            offset += step;
            continue;
        }
        pre = static_cast<std::uint16_t>(
            static_cast<int>(pre) + static_cast<int>(step));
        const auto shifted = static_cast<std::int32_t>(pre >> octave);
        offset = shifted0 - detune_pitch_up - shifted;
    }
    return offset;
}

[[nodiscard]] inline int softwareLfoDisplayExtent(
    const SoftwareLfoSettings& settings,
    bool include_extra_roughness,
    PsgSccModulationBase base = {},
    std::int32_t detune_pitch_up = 0) noexcept {
    if (!settings.enabled) {
        return 0;
    }
    if (include_extra_roughness && settings.extra_roughness != 0) {
        auto step = settings.extra_roughness;
        const unsigned depth_reload =
            static_cast<unsigned>(settings.depth) + 1U;
        unsigned depth_count = (depth_reload >> 1) + 1U;
        const auto octave = static_cast<unsigned>(base.octave & 7);
        auto pre = base.unshifted;
        const auto shifted0 =
            static_cast<std::int32_t>(base.unshifted >> octave);
        int extent = 0;
        const unsigned updates = (depth_reload + 2U) * 4U;
        for (unsigned i = 0; i < updates; ++i) {
            --depth_count;
            if (depth_count == 0U) {
                depth_count = depth_reload;
                step = softwareLfoNegate16(step);
            }
            pre = static_cast<std::uint16_t>(
                static_cast<int>(pre) + static_cast<int>(step));
            const auto shifted = static_cast<std::int32_t>(pre >> octave);
            extent = std::max(
                extent,
                std::abs(shifted0 - detune_pitch_up - shifted));
        }
        return extent;
    }
    auto step = static_cast<std::int32_t>(settings.roughness);
    if (step == 0) {
        return 0;
    }
    const unsigned depth_reload = static_cast<unsigned>(settings.depth) + 1U;
    unsigned depth_count = (depth_reload >> 1) + 1U;
    std::int32_t offset = 0;
    int extent = 0;
    const unsigned updates = (depth_reload + 2U) * 8U;
    for (unsigned i = 0; i < updates; ++i) {
        --depth_count;
        if (depth_count == 0U) {
            depth_count = depth_reload;
            step = -step;
        }
        offset += step;
        extent = std::max(extent, std::abs(offset));
    }
    return extent;
}

class SoftwareLfoRuntime {
public:
    void setSettings(
        SoftwareLfoSettings settings,
        bool apply_extra_roughness) noexcept {
        settings_ = settings;
        apply_extra_roughness_ = apply_extra_roughness;
        if (!settings_.enabled) {
            settings_ = {};
        }
    }

    void bindPitchBase(
        PsgSccModulationBase base,
        std::int32_t detune_pitch_up) noexcept {
        base_ = base;
        detune_pitch_up_ = detune_pitch_up;
    }

    void resetForKeyOn() noexcept {
        tick_ = 0;
        applied_ = 0;
    }

    void clear() noexcept {
        settings_ = {};
        apply_extra_roughness_ = true;
        resetForKeyOn();
    }

    // Advance one interrupt. Returns FrequencyDelta vs last applied offset.
    [[nodiscard]] std::int32_t advance() noexcept {
        const auto next = softwareLfoOffsetAtTick(
            settings_, tick_, apply_extra_roughness_, base_, detune_pitch_up_);
        ++tick_;
        const auto delta = next - applied_;
        applied_ = next;
        return delta;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return settings_.enabled;
    }

    [[nodiscard]] std::int32_t applied() const noexcept {
        return applied_;
    }

private:
    SoftwareLfoSettings settings_{};
    PsgSccModulationBase base_{};
    std::int32_t detune_pitch_up_{};
    bool apply_extra_roughness_{true};
    std::uint32_t tick_{};
    std::int32_t applied_{};
};

}  // namespace mgstc::engine
