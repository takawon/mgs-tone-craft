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

// The common editor note number follows MIDI numbering (C4 == 60).
// MGSDRV/YM2413's useful shared range is C1..B8.
[[nodiscard]] bool notePitch(
    std::uint8_t midi_note,
    NotePitch& output) noexcept;

}  // namespace mgstc::engine
