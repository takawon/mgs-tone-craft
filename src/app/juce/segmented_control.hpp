// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ui_chrome_constants.hpp"
#include "ui_layout.hpp"

// Opt in per button; selection, callbacks and keyboard handling stay with JUCE.
inline void configureSegmentButton(juce::Button& button, int index, int count) {
    button.getProperties().set("mgstcSegment", true);
    button.setConnectedEdges((index > 0 ? juce::Button::ConnectedOnLeft : 0)
        | (index + 1 < count ? juce::Button::ConnectedOnRight : 0));
}

inline bool isSegmentButton(const juce::Button& button) {
    return static_cast<bool>(button.getProperties()["mgstcSegment"]);
}

inline void paintSegmentBackground(juce::Graphics& g, juce::Button& button) {
    const auto bounds = button.getLocalBounds().toFloat();
    const float corner = static_cast<float>(UiLayout::xs);
    juce::Path track;
    track.addRoundedRectangle(bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
        corner, corner, !button.isConnectedOnLeft(), !button.isConnectedOnRight(),
        !button.isConnectedOnLeft(), !button.isConnectedOnRight());
    g.setColour(juce::Colours::black.withAlpha(0.5F));
    g.fillPath(track);
    if (button.getToggleState()) {
        g.setColour(uiToggleOnFill().brighter(0.18F)
            .withMultipliedAlpha(button.isEnabled() ? 1.0F : 0.5F));
        g.fillRoundedRectangle(bounds.reduced(static_cast<float>(UiLayout::xs) * 0.5F), corner);
    }
    if (button.hasKeyboardFocus(true)) {
        g.setColour(uiHoverAccent());
        g.drawRoundedRectangle(bounds.reduced(1.0F), corner, 1.5F);
    }
}
