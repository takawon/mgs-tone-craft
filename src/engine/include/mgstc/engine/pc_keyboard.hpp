// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>

namespace mgstc::engine {

[[nodiscard]] std::optional<int>
pcKeyboardSemitoneFromScanCode(std::uint16_t scan_code);

[[nodiscard]] std::optional<std::uint8_t>
pcKeyboardMidiNoteFromScanCode(
    std::uint16_t scan_code,
    int octave);

}  // namespace mgstc::engine
