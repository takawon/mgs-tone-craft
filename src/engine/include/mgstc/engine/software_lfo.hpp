// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace mgstc::engine {

// MGSDRV 3.20 software pitch LFO (pre-key-on MGSC `h` / `@p`).
// Integrating triangle: n4 / `@p` is the signed step (粗さ), not a DC bias.
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

// Pitch-offset units (same as track `\` / `@e` `\`).
// `@p != 0` replaces n4 on PSG/SCC; OPLL always uses n4.
[[nodiscard]] constexpr std::int32_t softwareLfoStep(
    const SoftwareLfoSettings& settings,
    bool apply_extra_roughness) noexcept {
    if (apply_extra_roughness && settings.extra_roughness != 0) {
        return settings.extra_roughness;
    }
    return static_cast<std::int32_t>(settings.roughness);
}

// Offset at 60 Hz tick since key-on. Delay is folded into the first speed
// countdown (stored delay + stored speed). Until the first step, offset is 0.
[[nodiscard]] inline std::int32_t softwareLfoOffsetAtTick(
    const SoftwareLfoSettings& settings,
    std::uint32_t tick_from_key_on,
    bool apply_extra_roughness = true) noexcept {
    if (!settings.enabled) {
        return 0;
    }
    auto step = softwareLfoStep(settings, apply_extra_roughness);
    unsigned counter =
        (softwareLfoStoredParam(settings.delay)
         + softwareLfoStoredParam(settings.speed))
        & 0xFFU;
    const unsigned speed_reload = softwareLfoStoredParam(settings.speed);
    unsigned depth_reload = static_cast<unsigned>(settings.depth) + 1U;
    unsigned depth_count = (depth_reload >> 1) + 1U;
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
            step = -step;
        }
        offset += step;
    }
    return offset;
}

[[nodiscard]] inline int softwareLfoDisplayExtent(
    const SoftwareLfoSettings& settings,
    bool include_extra_roughness) noexcept {
    if (!settings.enabled) {
        return 0;
    }
    auto step = softwareLfoStep(settings, include_extra_roughness);
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
            settings_, tick_, apply_extra_roughness_);
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
    bool apply_extra_roughness_{true};
    std::uint32_t tick_{};
    std::int32_t applied_{};
};

}  // namespace mgstc::engine
