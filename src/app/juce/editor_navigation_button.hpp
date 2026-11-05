// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>
#include "launcher_icons.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

namespace mgstc::app {

// An action that opens or raises another editor, never a persistent selection.
class EditorNavigationButton final : public juce::TextButton {
public:
    explicit EditorNavigationButton(int target) : target_(target) {
        setClickingTogglesState(false);
        setWantsKeyboardFocus(true);
    }

    [[nodiscard]] int preferredWidth() const {
        return juce::GlyphArrangement::getStringWidthInt(
                   UiFonts::body(), getButtonText())
            + UiLayout::textButtonH + UiLayout::sm
            + UiLayout::controlGap * 3;
    }

    void paintButton(juce::Graphics& g, bool over, bool down) override {
        const auto bounds = getLocalBounds().toFloat().reduced(0.75F);
        const bool highlighted = over || hasKeyboardFocus(true);
        const auto accent = juce::Colour(kUiHoverAccent);
        const std::array<juce::Colour, 3> source_colours{
            juce::Colour(0xFFAD8BC5), juce::Colour(0xFFD49C78),
            juce::Colour(0xFF79BD97)};
        const auto tint = source_colours[static_cast<std::size_t>(
            juce::jlimit(0, 2, target_))];
        g.setColour(tint.withAlpha(down ? 0.23F : 0.08F));
        g.fillRoundedRectangle(bounds, static_cast<float>(UiLayout::xs));
        g.setColour(highlighted ? accent : tint.withAlpha(0.65F));
        g.drawRoundedRectangle(bounds, static_cast<float>(UiLayout::xs),
            highlighted ? 1.5F : 1.0F);
        auto content = getLocalBounds().reduced(UiLayout::controlGap, UiLayout::xs);
        const auto icon = content.removeFromLeft(content.getHeight());
        g.setOpacity(isEnabled() ? 1.0F : 0.45F);
        g.drawImageWithin(launcherIcon(target_), icon.getX(), icon.getY(),
            icon.getWidth(), icon.getHeight(), juce::RectanglePlacement::centred);
        content.removeFromLeft(UiLayout::controlGap);
        auto arrow = content.removeFromRight(UiLayout::sm);
        content.removeFromRight(UiLayout::controlGap);
        g.setColour(highlighted ? accent : juce::Colour(0xFFE6EDF3));
        g.setFont(UiFonts::body());
        g.drawFittedText(getButtonText(), content,
            juce::Justification::centredLeft, 1);
        const auto x = static_cast<float>(arrow.getX());
        const auto y = static_cast<float>(arrow.getCentreY());
        const auto w = static_cast<float>(arrow.getWidth());
        juce::Path path;
        path.startNewSubPath(x, y);
        path.lineTo(x + w, y);
        path.startNewSubPath(x + w * 0.5F, y - w * 0.5F);
        path.lineTo(x + w, y);
        path.lineTo(x + w * 0.5F, y + w * 0.5F);
        g.strokePath(path, juce::PathStrokeType(1.25F));
    }

private:
    int target_{};
};

} // namespace mgstc::app
