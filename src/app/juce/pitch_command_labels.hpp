// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>

#include <juce_core/juce_core.h>

[[nodiscard]] inline juce::String pitchCommandLabel(std::int32_t value) {
    if (value >= 0) {
        return juce::String::fromUTF8("\\+") + juce::String(value);
    }
    return juce::String::fromUTF8("\\") + juce::String(value);
}

[[nodiscard]] inline juce::String opllRomPatchLabel(int rom_number) {
    static constexpr std::array<const char*, 15> names{
        "@0  Violin",
        "@1  Guitar",
        "@2  Piano",
        "@3  Flute",
        "@4  Clarinet",
        "@5  Oboe",
        "@6  Trumpet",
        "@7  Organ",
        "@8  Horn",
        "@9  Synthesizer",
        "@10 Harpsichord",
        "@11 Vibraphone",
        "@12 Synthesizer Bass",
        "@13 Acoustic Bass",
        "@14 Electric Guitar"};
    if (rom_number < 0 || rom_number >= static_cast<int>(names.size())) {
        return "@" + juce::String(rom_number);
    }
    return juce::String::fromUTF8(names[static_cast<std::size_t>(rom_number)]);
}
