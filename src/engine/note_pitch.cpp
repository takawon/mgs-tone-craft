#include "mgstc/engine/note_pitch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace mgstc::engine {
namespace {

// Tables copied from MGSDRV 3.20 at driver addresses 7737H and 774FH.
// MGSDRV octave commands o1..o8 map to table octave indices 0..7.
constexpr std::array<std::uint16_t, 12> kMgsdrvPsgSccOctaveOne{
    0x0D5D, 0x0C9C, 0x0BE7, 0x0B3C,
    0x0A9B, 0x0A02, 0x0973, 0x08EB,
    0x086B, 0x07F2, 0x0780, 0x0714,
};

constexpr std::array<std::uint16_t, 12> kMgsdrvOpllFNumbers{
    0x0AC, 0x0B6, 0x0C2, 0x0CD,
    0x0D9, 0x0E6, 0x0F4, 0x102,
    0x111, 0x122, 0x133, 0x145,
};

constexpr std::size_t kNotesPerOctave = 12;
constexpr std::size_t kMgsdrvOctaveCount = 8;
constexpr std::size_t kMgsdrvNoteCount =
    kNotesPerOctave * kMgsdrvOctaveCount;

[[nodiscard]] constexpr std::array<NotePitch, kMgsdrvNoteCount>
makeMgsdrvNoteTable() noexcept {
    std::array<NotePitch, kMgsdrvNoteCount> table{};
    for (std::size_t octave = 0; octave < kMgsdrvOctaveCount; ++octave) {
        for (std::size_t semitone = 0;
             semitone < kNotesPerOctave;
             ++semitone) {
            table[octave * kNotesPerOctave + semitone] = {
                static_cast<std::uint16_t>(
                    kMgsdrvPsgSccOctaveOne[semitone] >> octave),
                {
                    kMgsdrvOpllFNumbers[semitone],
                    static_cast<std::uint8_t>(octave),
                },
            };
        }
    }
    return table;
}

constexpr auto kMgsdrvNoteTable = makeMgsdrvNoteTable();

static_assert(kMgsdrvNoteTable.front().psg_scc_period == 0x0D5D);
static_assert(kMgsdrvNoteTable.front().opll.block == 0);
static_assert(kMgsdrvNoteTable.back().psg_scc_period == 0x000E);
static_assert(kMgsdrvNoteTable.back().opll.f_number == 0x0145);
static_assert(kMgsdrvNoteTable.back().opll.block == 7);

}  // namespace

bool notePitch(std::uint8_t midi_note, NotePitch& output) noexcept {
    constexpr std::uint8_t kFirstMidiNote = 24;   // C1
    constexpr std::uint8_t kLastMidiNote = 119;  // B8
    if (midi_note < kFirstMidiNote || midi_note > kLastMidiNote) {
        return false;
    }

    const auto index = static_cast<std::size_t>(
        midi_note - kFirstMidiNote);
    output = kMgsdrvNoteTable[index];
    return true;
}

}  // namespace mgstc::engine
