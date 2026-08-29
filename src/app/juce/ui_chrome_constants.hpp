// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Editor Open/Paste/Settings icon over; shared UI hover accent.
constexpr juce::uint32 kUiHoverAccent = 0xFF53E3A6;
// Toggle-on fill (TextButton / switch pill). Darker so light text stays readable.
constexpr juce::uint32 kUiToggleOnFill = 0xFF1F8A5C;

[[nodiscard]] inline juce::Colour uiHoverAccent() {
    return juce::Colour(kUiHoverAccent);
}

// Darker/stronger hover of the same green (edit-tier sub-lane under the cursor).
[[nodiscard]] inline juce::Colour uiHoverAccentStrong() {
    return juce::Colour(kUiHoverAccent).darker(0.28F);
}

[[nodiscard]] inline juce::Colour uiToggleOnFill() {
    return juce::Colour(kUiToggleOnFill);
}
