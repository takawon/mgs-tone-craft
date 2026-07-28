#include "mgstc/engine/note_pitch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mgstc::engine {
namespace {

// Tables copied from MGSDRV 3.20 at driver addresses 7737H and 774FH.
// PSG/SCC values are the octave-1 periods; higher octaves shift right.
constexpr std::array<std::uint16_t, 12> kPsgSccOctaveOne{
    0x0D5D, 0x0C9C, 0x0BE7, 0x0B3C,
    0x0A9B, 0x0A02, 0x0973, 0x08EB,
    0x086B, 0x07F2, 0x0780, 0x0714,
};

constexpr std::array<std::uint16_t, 12> kOpllFNumbers{
    0x0AC, 0x0B6, 0x0C2, 0x0CD,
    0x0D9, 0x0E6, 0x0F4, 0x102,
    0x111, 0x122, 0x133, 0x145,
};

}  // namespace

bool notePitch(std::uint8_t midi_note, NotePitch& output) noexcept {
    constexpr std::uint8_t kFirstMidiNote = 24;   // C1
    constexpr std::uint8_t kLastMidiNote = 119;  // B8
    if (midi_note < kFirstMidiNote || midi_note > kLastMidiNote) {
        return false;
    }

    const auto octave = static_cast<std::uint8_t>(midi_note / 12 - 1);
    const auto semitone = static_cast<std::size_t>(midi_note % 12);
    const auto shift = static_cast<unsigned>(octave - 1);
    output.psg_scc_period = std::max<std::uint16_t>(
        1,
        static_cast<std::uint16_t>(kPsgSccOctaveOne[semitone] >> shift));
    output.opll = {
        kOpllFNumbers[semitone],
        static_cast<std::uint8_t>(octave - 1),
    };
    return true;
}

}  // namespace mgstc::engine
