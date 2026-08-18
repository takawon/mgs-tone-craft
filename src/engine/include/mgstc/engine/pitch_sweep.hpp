// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace mgstc::engine {

// MGSDRV 3.20 software pitch slide (pre-key-on MGSC `p`).
// Public MGS value is 0–255. Offset per 60 Hz interrupt is (n − 128) in the
// same units as `\`. Old MGSDRV `p` maps to n+128 (SPEC §6.5).
// PSG/SCC only. Mutually exclusive with software LFO (`h`).
struct PitchSweepSettings {
    bool enabled{};
    std::uint8_t value{128};

    friend bool operator==(
        const PitchSweepSettings&,
        const PitchSweepSettings&) = default;
};

[[nodiscard]] constexpr std::int32_t pitchSweepStep(
    std::uint8_t value) noexcept {
    return static_cast<std::int32_t>(value) - 128;
}

[[nodiscard]] constexpr std::int32_t pitchSweepOffsetAtTick(
    const PitchSweepSettings& settings,
    std::uint32_t tick_from_key_on) noexcept {
    if (!settings.enabled) {
        return 0;
    }
    return pitchSweepStep(settings.value)
        * static_cast<std::int32_t>(tick_from_key_on);
}

[[nodiscard]] inline int pitchSweepDisplayExtent(
    const PitchSweepSettings& settings,
    std::uint32_t tick_count) noexcept {
    if (!settings.enabled || tick_count == 0) {
        return 0;
    }
    const auto step = pitchSweepStep(settings.value);
    if (step == 0) {
        return 0;
    }
    return std::abs(step) * static_cast<int>(tick_count);
}

class PitchSweepRuntime {
public:
    void setSettings(PitchSweepSettings settings) noexcept {
        settings_ = settings;
        if (!settings_.enabled) {
            settings_ = {};
            settings_.value = 128;
        }
    }

    void resetForKeyOn() noexcept {
        tick_ = 0;
        applied_ = 0;
    }

    void clear() noexcept {
        settings_ = {};
        settings_.value = 128;
        resetForKeyOn();
    }

    // Advance one interrupt. Returns FrequencyDelta vs last applied offset.
    [[nodiscard]] std::int32_t advance() noexcept {
        const auto next = pitchSweepOffsetAtTick(settings_, tick_);
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
    PitchSweepSettings settings_{};
    std::uint32_t tick_{};
    std::int32_t applied_{};
};

}  // namespace mgstc::engine
