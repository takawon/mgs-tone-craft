// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>

#include <juce_core/juce_core.h>

[[nodiscard]] inline juce::String midiNoteName(std::uint8_t midi_note) {
    constexpr std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"};
    const auto name =
        names[static_cast<std::size_t>(midi_note % 12)];
    const int octave = static_cast<int>(midi_note / 12) - 1;
    return juce::String(name) + juce::String(octave);
}
