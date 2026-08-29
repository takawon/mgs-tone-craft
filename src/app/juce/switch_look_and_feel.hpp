// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_scale.hpp"

// Pill toggle: same ON/OFF fill/text language as TextButton toggles
// (e.g. 即時発音 / 1秒プレビュー設定), but fully rounded ends like a
// switch track so it stays distinct from square TextButtons.
class SwitchLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5F);
        const auto enabled = button.isEnabled();
        const auto active = button.getToggleState();
        const float corner = bounds.getHeight() * 0.5F;

        // Match MgstcLookAndFeel panel / toggle-on tokens (this LnF is separate).
        constexpr auto kPanelFill = juce::uint32{0xFF2A3540};
        constexpr auto kText = juce::uint32{0xFFF2F4F5};
        constexpr auto kOutline = juce::uint32{0xFF607080};

        auto fill = juce::Colour(active ? kUiToggleOnFill : kPanelFill);
        fill = fill.withMultipliedAlpha(enabled ? 1.0F : 0.5F);
        if (should_draw_down || should_draw_highlight) {
            fill = fill.contrasting(should_draw_down ? 0.2F : 0.05F);
        }
        graphics.setColour(fill);
        graphics.fillRoundedRectangle(bounds, corner);

        const bool focus = button.hasKeyboardFocus(true);
        graphics.setColour(
            focus ? juce::Colour(kUiHoverAccent)
                  : juce::Colour(kOutline));
        graphics.drawRoundedRectangle(
            bounds, corner, focus ? 1.5F : 1.0F);

        const auto font = UiFonts::make(
            UiFonts::controlTextHeight(
                static_cast<float>(button.getHeight())));
        graphics.setFont(font);
        const auto text_colour = active
            ? juce::Colour(kText)
            : ((should_draw_highlight && enabled)
                   ? juce::Colour(kUiHoverAccent)
                   : juce::Colour(kText));
        graphics.setColour(
            text_colour.withMultipliedAlpha(enabled ? 1.0F : 0.5F));
        graphics.drawFittedText(
            button.getButtonText(),
            bounds.toNearestInt(),
            juce::Justification::centred,
            1);
    }
};
