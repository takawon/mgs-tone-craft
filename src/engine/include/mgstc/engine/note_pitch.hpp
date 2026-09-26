#pragma once

#include <cstdint>

namespace mgstc::engine {

struct OpllPitch {
    std::uint16_t f_number{};
    std::uint8_t block{};

    friend bool operator==(const OpllPitch&, const OpllPitch&) = default;
};

struct NotePitch {
    std::uint16_t psg_scc_period{};
    OpllPitch opll{};

    friend bool operator==(const NotePitch&, const NotePitch&) = default;
};

// The editor uses MIDI-like integer positions (C1 == 24, C4 == 60), while
// octave labels and chip pitches follow MGSDRV o1..o8 rather than concert-
// pitch correction. The shared range contains all 96 MGSDRV notes.
[[nodiscard]] bool notePitch(
    std::uint8_t midi_note,
    NotePitch& output) noexcept;

// Unshifted PSG/SCC table word (plus `@\`) and the internal octave index.
// `p` / `@p` accumulate on `unshifted`, then logical-shift `octave` times.
struct PsgSccModulationBase {
    std::uint16_t unshifted{};
    std::uint8_t octave{};
};

[[nodiscard]] bool psgSccModulationBase(
    std::uint8_t midi_note,
    std::int32_t micro,
    PsgSccModulationBase& output) noexcept;

// Track MML `@\`: add the 16-bit value to the unshifted PSG/SCC table word,
// then logical-shift by the internal octave (o1=0 … o8=7).
[[nodiscard]] bool psgSccPeriodWithMicroDetune(
    std::uint8_t midi_note,
    std::int32_t micro,
    std::uint16_t& period) noexcept;

// Track MML `@\` on OPLL: 0 skips; otherwise floor(d*(n+1)/256) using the
// per-note F-number span table. n is clamped to 0..255.
[[nodiscard]] std::int32_t opllMicroDetuneDelta(
    std::uint8_t midi_note,
    std::int32_t micro) noexcept;

}  // namespace mgstc::engine
