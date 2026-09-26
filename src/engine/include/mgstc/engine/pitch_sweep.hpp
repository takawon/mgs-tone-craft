// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/note_pitch.hpp"

#include <cstdint>
#include <cstdlib>

namespace mgstc::engine {

// MGSDRV 3.20 bend (MGSC `p`, opcode 52H). PSG/SCC only.
// The byte is not a signed step around 128. Low 7 bits are an unsigned
// amount v. Bit 7 selects the update (driver +1B bit 0, with no prior `h`
// delay left in the byte). Every later 60 Hz interrupt:
//   bit7 = 0: add 2*v to the pre-octave word, then logical-shift by octave.
//   bit7 = 1: add (8-octave)*v to the current output period.
// Both increase the period (pitch falls). v = 0 (p0 and p128) is silent.
// The pre-shift path outputs the shifted word alone, so a track `\` that
// was only in the key-on period is removed on the first update.
struct PitchSweepSettings {
    bool enabled{};
    std::uint8_t value{128};

    friend bool operator==(
        const PitchSweepSettings&,
        const PitchSweepSettings&) = default;
};

[[nodiscard]] constexpr int pitchSweepAmount(
    std::uint8_t value) noexcept {
    return static_cast<int>(value & 0x7F);
}

[[nodiscard]] constexpr bool pitchSweepPostShift(
    std::uint8_t value) noexcept {
    return (value & 0x80) != 0;
}

// Pitch-up offset after `tick_from_key_on` interrupts (tick 0 is the key-on
// interrupt, before the first bend). Positive raises pitch. `detune_pitch_up`
// is the editor `\` already subtracted from the key-on period.
[[nodiscard]] inline std::int32_t pitchSweepOffsetAtTick(
    const PitchSweepSettings& settings,
    std::uint32_t tick_from_key_on,
    PsgSccModulationBase base = {},
    std::int32_t detune_pitch_up = 0) noexcept {
    if (!settings.enabled || tick_from_key_on == 0) {
        return 0;
    }
    const int amount = pitchSweepAmount(settings.value);
    const auto octave = static_cast<unsigned>(base.octave & 7);
    if (pitchSweepPostShift(settings.value)) {
        const auto step = static_cast<std::int32_t>(8U - octave) * amount;
        return -step * static_cast<std::int32_t>(tick_from_key_on);
    }
    auto pre = base.unshifted;
    const auto add = static_cast<std::uint16_t>(amount * 2);
    for (std::uint32_t tick = 0; tick < tick_from_key_on; ++tick) {
        pre = static_cast<std::uint16_t>(pre + add);
    }
    const auto shifted0 = static_cast<std::int32_t>(base.unshifted >> octave);
    const auto shifted = static_cast<std::int32_t>(pre >> octave);
    return shifted0 - detune_pitch_up - shifted;
}

[[nodiscard]] inline int pitchSweepDisplayExtent(
    const PitchSweepSettings& settings,
    std::uint32_t tick_count,
    PsgSccModulationBase base = {},
    std::int32_t detune_pitch_up = 0) noexcept {
    if (!settings.enabled || tick_count == 0) {
        return 0;
    }
    const auto offset = pitchSweepOffsetAtTick(
        settings, tick_count, base, detune_pitch_up);
    return std::abs(offset);
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
        settings_.value = 128;
        resetForKeyOn();
    }

    // Advance one interrupt. Returns FrequencyDelta vs last applied offset.
    [[nodiscard]] std::int32_t advance() noexcept {
        const auto next = pitchSweepOffsetAtTick(
            settings_, tick_, base_, detune_pitch_up_);
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
    PsgSccModulationBase base_{};
    std::int32_t detune_pitch_up_{};
    std::uint32_t tick_{};
    std::int32_t applied_{};
};

}  // namespace mgstc::engine
