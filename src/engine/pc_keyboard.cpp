// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/pc_keyboard.hpp"

namespace mgstc::engine {

std::optional<int>
pcKeyboardSemitoneFromScanCode(std::uint16_t scan_code) {
    switch (scan_code) {
    // Lower row: Z S X D C V G B H N J M (C through B).
    case 0x2C: return 0;
    case 0x1F: return 1;
    case 0x2D: return 2;
    case 0x20: return 3;
    case 0x2E: return 4;
    case 0x2F: return 5;
    case 0x22: return 6;
    case 0x30: return 7;
    case 0x23: return 8;
    case 0x31: return 9;
    case 0x24: return 10;
    case 0x32: return 11;

    // Q begins C one octave above the lower row. Number-row accidentals
    // remain on the same physical 2, 3, 5, 6, 7, 9 and 0 keys.
    case 0x10: return 12;
    case 0x03: return 13;
    case 0x11: return 14;
    case 0x04: return 15;
    case 0x12: return 16;
    case 0x13: return 17;
    case 0x06: return 18;
    case 0x14: return 19;
    case 0x07: return 20;
    case 0x15: return 21;
    case 0x08: return 22;
    case 0x16: return 23;
    case 0x17: return 24;
    case 0x0A: return 25;
    case 0x18: return 26;
    case 0x0B: return 27;
    case 0x19: return 28;
    case 0x1A: return 29;
    case 0x0D: return 30;
    case 0x1B: return 31;
    case 0x7D: return 32;
    default: return std::nullopt;
    }
}

std::optional<std::uint8_t>
pcKeyboardMidiNoteFromScanCode(
    std::uint16_t scan_code,
    int octave) {
    const auto offset =
        pcKeyboardSemitoneFromScanCode(scan_code);
    if (!offset) {
        return std::nullopt;
    }
    const int note = (octave + 1) * 12 + *offset;
    if (note < 24 || note > 119) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(note);
}

}  // namespace mgstc::engine
