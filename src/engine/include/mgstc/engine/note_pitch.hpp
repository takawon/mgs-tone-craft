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

}  // namespace mgstc::engine
