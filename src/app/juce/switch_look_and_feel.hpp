// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_scale.hpp"

class SwitchLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        auto bounds = button.getLocalBounds().toFloat();
        const auto enabled = button.isEnabled();
        const auto active = button.getToggleState();
        // Design tokens match UiLayout::Base::switch* (scaled via UiScale).
        // Keep magic bases identical to UiLayout::Base::{switchTrackW,H,Thumb,LabelPad}.
        const auto track_w =
            static_cast<float>(UiScale::sx(34));
        const auto track_h =
            static_cast<float>(UiScale::sx(18));
        const auto thumb =
            static_cast<float>(UiScale::sx(14));
        const auto label_pad =
            static_cast<float>(UiScale::sx(8));
        const auto track_pad =
            juce::jmax(1.0F, (track_h - thumb) * 0.5F);
        const auto switch_bounds = juce::Rectangle<float>(
            bounds.getX(),
            bounds.getCentreY() - track_h * 0.5F,
            track_w,
            track_h);
        auto track = active
            ? juce::Colour(0xFF2FA9D6)
            : juce::Colour(0xFF4C5863);
        if (should_draw_highlight || should_draw_down) {
            track = track.brighter(0.12F);
        }
        if (!enabled) {
            track = track.withMultipliedAlpha(0.45F);
        }
        graphics.setColour(track);
        graphics.fillRoundedRectangle(
            switch_bounds, track_h * 0.5F);

        const auto thumb_x = active
            ? switch_bounds.getRight() - thumb - track_pad
            : switch_bounds.getX() + track_pad;
        graphics.setColour(
            juce::Colours::white.withMultipliedAlpha(
                enabled ? 0.95F : 0.45F));
        graphics.fillEllipse(
            thumb_x,
            switch_bounds.getY() + track_pad,
            thumb,
            thumb);

        const auto label_colour =
            (should_draw_highlight && enabled)
                ? juce::Colour(kUiHoverAccent)
                : button.findColour(
                      juce::ToggleButton::textColourId);
        graphics.setColour(
            label_colour.withMultipliedAlpha(enabled ? 1.0F : 0.45F));
        const auto font = UiFonts::make(
            UiFonts::controlTextHeight(
                static_cast<float>(button.getHeight())));
        graphics.setFont(font);
        // Track | labelPad | label — same formula as UiLayout::switchControlWidth.
        const auto label_bounds = juce::Rectangle<float>(
            switch_bounds.getRight() + label_pad,
            switch_bounds.getY(),
            juce::jmax(
                0.0F,
                bounds.getRight()
                    - (switch_bounds.getRight() + label_pad)),
            switch_bounds.getHeight());
        graphics.drawFittedText(
            button.getButtonText(),
            label_bounds.toNearestInt(),
            juce::Justification::centredLeft,
            1);

        if (button.hasKeyboardFocus(true)) {
            graphics.setColour(juce::Colour(kUiHoverAccent));
            graphics.drawRoundedRectangle(
                bounds.reduced(0.5F),
                static_cast<float>(UiScale::sx(4)),
                1.0F);
        }
    }
};
