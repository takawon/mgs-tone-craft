// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Editor Open/Paste/Settings icon over; shared UI hover accent.
constexpr juce::uint32 kUiHoverAccent = 0xFF53E3A6;

[[nodiscard]] inline juce::Colour uiHoverAccent() {
    return juce::Colour(kUiHoverAccent);
}

// Darker/stronger hover of the same green (edit-tier sub-lane under the cursor).
[[nodiscard]] inline juce::Colour uiHoverAccentStrong() {
    return juce::Colour(kUiHoverAccent).darker(0.28F);
}
