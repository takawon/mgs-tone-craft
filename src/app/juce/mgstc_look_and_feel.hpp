// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

class MgstcLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    MgstcLookAndFeel() {
        // Window chrome peeking at edges: solid page top colour (content paints gradient).
        const auto background = juce::Colour(UiLayout::pageFill);
        const auto panel = juce::Colour(0xFF2A3540);
        const auto border = juce::Colour(0xFF607080);
        const auto text = juce::Colour(0xFFF2F4F5);
        const auto blue = juce::Colour(0xFF0072B2);
        const auto cyan = juce::Colour(0xFF56B4E9);
        // Focus rings / hover accent / progress / SCC confirmed waveform.
        const auto accent_green = juce::Colour(kUiHoverAccent);
        // Toggle-on button fill: darker so #F2F4F5 text stays readable.
        const auto selected_green = juce::Colour(kUiToggleOnFill);
        setDefaultSansSerifTypefaceName(UiFonts::windowsMessageFaceName());
        setColour(juce::ResizableWindow::backgroundColourId, background);
        setColour(juce::TextEditor::backgroundColourId, panel);
        setColour(juce::TextEditor::outlineColourId, border);
        setColour(juce::TextEditor::focusedOutlineColourId, accent_green);
        setColour(juce::TextEditor::textColourId, text);
        setColour(juce::Label::textColourId, text);
        setColour(juce::ComboBox::backgroundColourId, panel);
        setColour(juce::ComboBox::outlineColourId, border);
        setColour(juce::ComboBox::focusedOutlineColourId, accent_green);
        setColour(juce::ComboBox::textColourId, text);
        setColour(juce::Slider::backgroundColourId,
                  juce::Colour(0xFF465562));
        setColour(juce::Slider::trackColourId, cyan);
        setColour(juce::Slider::thumbColourId, text);
        setColour(juce::Slider::rotarySliderFillColourId, blue);
        setColour(juce::Slider::rotarySliderOutlineColourId, border);
        setColour(juce::TextButton::buttonColourId, panel);
        setColour(juce::TextButton::buttonOnColourId, selected_green);
        setColour(juce::TextButton::textColourOffId, text);
        setColour(juce::TextButton::textColourOnId, text);
        // ImageOnButtonBackground uses TextButton::buttonOnColourId when
        // toggled; set backgroundOn too for other DrawableButton styles.
        setColour(juce::DrawableButton::backgroundColourId, panel);
        setColour(
            juce::DrawableButton::backgroundOnColourId, selected_green);
        setColour(juce::TabbedButtonBar::tabTextColourId, text);
        setColour(juce::TabbedButtonBar::frontTextColourId, text);
    }

    void drawButtonBackground(
        juce::Graphics& graphics,
        juce::Button& button,
        const juce::Colour& backgroundColour,
        bool should_draw_highlighted,
        bool should_draw_down) override {
        constexpr float corner = 6.0F;
        const auto bounds =
            button.getLocalBounds().toFloat().reduced(0.5F, 0.5F);
        const bool on = button.getToggleState();
        // V4 desaturates fills by 0.9x; keep toggle-on green as authored.
        auto base = on
            ? findColour(juce::TextButton::buttonOnColourId)
            : backgroundColour.withMultipliedSaturation(
                  button.hasKeyboardFocus(true) ? 1.3F : 0.9F);
        base = base.withMultipliedAlpha(button.isEnabled() ? 1.0F : 0.5F);
        if (should_draw_down || should_draw_highlighted) {
            base = base.contrasting(should_draw_down ? 0.2F : 0.05F);
        }
        graphics.setColour(base);
        graphics.fillRoundedRectangle(bounds, corner);

        const bool focus = button.hasKeyboardFocus(true);
        graphics.setColour(
            focus ? juce::Colour(kUiHoverAccent)
                  : button.findColour(juce::ComboBox::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds, corner, focus ? 1.5F : 1.0F);
    }

    void drawButtonText(
        juce::Graphics& graphics,
        juce::TextButton& button,
        bool should_draw_highlighted,
        bool /*should_draw_down*/) override {
        const auto font = getTextButtonFont(button, button.getHeight());
        graphics.setFont(font);

        // Toggle-on uses dark green fill — keep light text for contrast.
        const bool toggle_on = button.getToggleState();
        const auto base_colour = toggle_on
            ? button.findColour(juce::TextButton::textColourOnId)
            : ((should_draw_highlighted && button.isEnabled())
                   ? juce::Colour(kUiHoverAccent)
                   : button.findColour(juce::TextButton::textColourOffId));
        graphics.setColour(
            base_colour.withMultipliedAlpha(
                button.isEnabled() ? 1.0F : 0.5F));

        const int y_indent = juce::jmin(
            4, button.proportionOfHeight(0.3F));
        const int corner_size =
            juce::jmin(button.getHeight(), button.getWidth()) / 2;
        const int font_height =
            juce::roundToInt(font.getHeight() * 0.6F);
        const int left_indent = juce::jmin(
            font_height,
            2 + corner_size / (button.isConnectedOnLeft() ? 4 : 2));
        const int right_indent = juce::jmin(
            font_height,
            2 + corner_size / (button.isConnectedOnRight() ? 4 : 2));
        const int text_width =
            button.getWidth() - left_indent - right_indent;
        if (text_width > 0) {
            graphics.drawFittedText(
                button.getButtonText(),
                left_indent,
                y_indent,
                text_width,
                button.getHeight() - y_indent * 2,
                juce::Justification::centred,
                2);
        }
    }

    juce::Font getTextButtonFont(
        juce::TextButton&,
        int buttonHeight) override {
        return UiFonts::body().withHeight(
            UiFonts::controlTextHeight(
                static_cast<float>(buttonHeight)));
    }

    juce::Font getLabelFont(juce::Label& label) override {
        // JUCE Label default height is 15px; treat that as unset → body.
        if (std::abs(label.getFont().getHeight() - 15.0F) < 0.05F) {
            return UiFonts::body();
        }
        return label.getFont();
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override {
        return UiFonts::body().withHeight(
            UiFonts::controlTextHeight(
                static_cast<float>(box.getHeight())));
    }

    // LookAndFeel_V4::drawComboBox ignores focusedOutlineColourId; draw the
    // same bright-green focus ring as TextEditor / focused buttons.
    void drawComboBox(
        juce::Graphics& graphics,
        int width,
        int height,
        bool,
        int,
        int,
        int,
        int,
        juce::ComboBox& box) override {
        constexpr float corner = 3.0F;
        const auto bounds =
            juce::Rectangle<int>(0, 0, width, height)
                .toFloat()
                .reduced(0.5F, 0.5F);

        graphics.setColour(
            box.findColour(juce::ComboBox::backgroundColourId));
        graphics.fillRoundedRectangle(bounds, corner);

        const bool focus = box.hasKeyboardFocus(true);
        graphics.setColour(
            focus ? box.findColour(
                        juce::ComboBox::focusedOutlineColourId)
                  : box.findColour(juce::ComboBox::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds, corner, focus ? 1.5F : 1.0F);

        juce::Rectangle<int> arrow_zone(width - 30, 0, 20, height);
        juce::Path path;
        path.startNewSubPath(
            static_cast<float>(arrow_zone.getX()) + 3.0F,
            static_cast<float>(arrow_zone.getCentreY()) - 2.0F);
        path.lineTo(
            static_cast<float>(arrow_zone.getCentreX()),
            static_cast<float>(arrow_zone.getCentreY()) + 3.0F);
        path.lineTo(
            static_cast<float>(arrow_zone.getRight()) - 3.0F,
            static_cast<float>(arrow_zone.getCentreY()) - 2.0F);
        const bool arrow_hover =
            box.isEnabled() && box.isMouseOverOrDragging(true);
        graphics.setColour(
            arrow_hover
                ? juce::Colour(kUiHoverAccent).withAlpha(0.9F)
                : box.findColour(juce::ComboBox::arrowColourId)
                      .withAlpha(box.isEnabled() ? 0.9F : 0.2F));
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
    }

    juce::Font getPopupMenuFont() override {
        return UiFonts::body();
    }

    // LookAndFeel_V4/V3 draws tabs via drawTabButton (not drawTabButtonText),
    // and hardcodes depth*0.5 font. Keep getTabButtonFont + drawTabButtonText
    // for the V2 path; override drawTabButton so settings tabs use body + hover.
    juce::Font getTabButtonFont(
        juce::TabBarButton&,
        float /*height*/) override {
        return UiFonts::body();
    }

    void drawTabButtonText(
        juce::TabBarButton& button,
        juce::Graphics& graphics,
        bool is_mouse_over,
        bool is_mouse_down) override {
        auto area = button.getTextArea().toFloat();
        auto length = area.getWidth();
        auto depth = area.getHeight();
        if (button.getTabbedButtonBar().isVertical()) {
            std::swap(length, depth);
        }

        auto font = getTabButtonFont(button, depth);
        font.setUnderline(button.hasKeyboardFocus(false));

        juce::AffineTransform transform;
        switch (button.getTabbedButtonBar().getOrientation()) {
            case juce::TabbedButtonBar::TabsAtLeft:
                transform = transform
                    .rotated(juce::MathConstants<float>::pi * -0.5F)
                    .translated(area.getX(), area.getBottom());
                break;
            case juce::TabbedButtonBar::TabsAtRight:
                transform = transform
                    .rotated(juce::MathConstants<float>::pi * 0.5F)
                    .translated(area.getRight(), area.getY());
                break;
            case juce::TabbedButtonBar::TabsAtTop:
            case juce::TabbedButtonBar::TabsAtBottom:
                transform = transform.translated(area.getX(), area.getY());
                break;
            default:
                jassertfalse;
                break;
        }

        const float alpha = button.isEnabled()
            ? ((is_mouse_over || is_mouse_down) ? 1.0F : 0.8F)
            : 0.3F;
        juce::Colour colour;
        if (button.isEnabled() && (is_mouse_over || is_mouse_down)) {
            colour = juce::Colour(kUiHoverAccent);
        } else if (
            button.isFrontTab()
            && (button.isColourSpecified(
                    juce::TabbedButtonBar::frontTextColourId)
                || isColourSpecified(
                    juce::TabbedButtonBar::frontTextColourId))) {
            colour = findColour(juce::TabbedButtonBar::frontTextColourId);
        } else if (
            button.isColourSpecified(juce::TabbedButtonBar::tabTextColourId)
            || isColourSpecified(juce::TabbedButtonBar::tabTextColourId)) {
            colour = findColour(juce::TabbedButtonBar::tabTextColourId);
        } else {
            colour = button.getTabBackgroundColour().contrasting();
        }

        graphics.setColour(colour.withMultipliedAlpha(alpha));
        graphics.setFont(font);
        graphics.addTransform(transform);
        graphics.drawFittedText(
            button.getButtonText().trim(),
            0,
            0,
            static_cast<int>(length),
            static_cast<int>(depth),
            juce::Justification::centred,
            juce::jmax(1, static_cast<int>(depth) / 12));
    }

    void drawTabButton(
        juce::TabBarButton& button,
        juce::Graphics& graphics,
        bool is_mouse_over,
        bool is_mouse_down) override {
        const auto active_area = button.getActiveArea();
        const auto orientation =
            button.getTabbedButtonBar().getOrientation();
        const auto background = button.getTabBackgroundColour();

        if (button.getToggleState()) {
            graphics.setColour(background);
        } else {
            juce::Point<int> p1;
            juce::Point<int> p2;
            switch (orientation) {
                case juce::TabbedButtonBar::TabsAtBottom:
                    p1 = active_area.getBottomLeft();
                    p2 = active_area.getTopLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtTop:
                    p1 = active_area.getTopLeft();
                    p2 = active_area.getBottomLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtRight:
                    p1 = active_area.getTopRight();
                    p2 = active_area.getTopLeft();
                    break;
                case juce::TabbedButtonBar::TabsAtLeft:
                    p1 = active_area.getTopLeft();
                    p2 = active_area.getTopRight();
                    break;
                default:
                    jassertfalse;
                    break;
            }
            graphics.setGradientFill(juce::ColourGradient(
                background.brighter(0.2F),
                p1.toFloat(),
                background.darker(0.1F),
                p2.toFloat(),
                false));
        }
        graphics.fillRect(active_area);

        graphics.setColour(
            button.findColour(juce::TabbedButtonBar::tabOutlineColourId));
        auto outline = active_area;
        if (orientation != juce::TabbedButtonBar::TabsAtBottom) {
            graphics.fillRect(outline.removeFromTop(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtTop) {
            graphics.fillRect(outline.removeFromBottom(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtRight) {
            graphics.fillRect(outline.removeFromLeft(1));
        }
        if (orientation != juce::TabbedButtonBar::TabsAtLeft) {
            graphics.fillRect(outline.removeFromRight(1));
        }

        drawTabButtonText(
            button, graphics, is_mouse_over, is_mouse_down);
    }

    juce::Rectangle<int> getTooltipBounds(
        const juce::String& tipText,
        juce::Point<int> screenPos,
        juce::Rectangle<int> parentArea) override {
        juce::AttributedString attributed;
        attributed.setWordWrap(
            juce::AttributedString::WordWrap::byWord);
        attributed.setJustification(juce::Justification::centred);
        attributed.append(
            tipText, UiFonts::body(), juce::Colours::black);
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(
            attributed, 400.0F);
        const auto width =
            static_cast<int>(layout.getWidth() + 14.0F);
        const auto height =
            static_cast<int>(layout.getHeight() + 8.0F);
        return juce::Rectangle<int>(
                   screenPos.x > parentArea.getCentreX()
                       ? screenPos.x - (width + 12)
                       : screenPos.x + 24,
                   screenPos.y > parentArea.getCentreY()
                       ? screenPos.y - (height + 6)
                       : screenPos.y + 6,
                   width,
                   height)
            .constrainedWithin(parentArea);
    }

    void drawTooltip(
        juce::Graphics& graphics,
        const juce::String& text,
        int width,
        int height) override {
        auto bounds = juce::Rectangle<int>(width, height).toFloat();
        constexpr float corner = 5.0F;
        graphics.setColour(
            findColour(juce::TooltipWindow::backgroundColourId));
        graphics.fillRoundedRectangle(bounds, corner);
        graphics.setColour(
            findColour(juce::TooltipWindow::outlineColourId));
        graphics.drawRoundedRectangle(
            bounds.reduced(0.5F), corner, 1.0F);

        juce::AttributedString attributed;
        attributed.setWordWrap(
            juce::AttributedString::WordWrap::byWord);
        attributed.setJustification(juce::Justification::centred);
        attributed.append(
            text,
            UiFonts::body(),
            findColour(juce::TooltipWindow::textColourId));
        juce::TextLayout layout;
        layout.createLayoutWithBalancedLineLengths(
            attributed, static_cast<float>(width) - 8.0F);
        layout.draw(
            graphics,
            juce::Rectangle<float>(
                static_cast<float>(width),
                static_cast<float>(height)));
    }

    juce::Font getAlertWindowTitleFont() override {
        return UiFonts::heading();
    }

    juce::Font getAlertWindowMessageFont() override {
        return UiFonts::body();
    }

    juce::Font getAlertWindowFont() override {
        return UiFonts::body();
    }

    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
        // Param numerics = body (dense reserved for SCC cells / graph chrome).
        label->setFont(UiFonts::body());
        return label;
    }

    // Default checkbox toggles (tick + label). SwitchLookAndFeel owns switch-styled ones.
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        const auto font = UiFonts::body();
        const auto tick_width = font.getHeight() * 1.1F;

        drawTickBox(
            graphics,
            button,
            4.0F,
            (static_cast<float>(button.getHeight()) - tick_width) * 0.5F,
            tick_width,
            tick_width,
            button.getToggleState(),
            button.isEnabled(),
            should_draw_highlight,
            should_draw_down);

        const auto label_colour =
            (should_draw_highlight && button.isEnabled())
                ? juce::Colour(kUiHoverAccent)
                : button.findColour(juce::ToggleButton::textColourId);
        graphics.setColour(label_colour);
        graphics.setFont(font);

        if (!button.isEnabled()) {
            graphics.setOpacity(0.5F);
        }

        graphics.drawFittedText(
            button.getButtonText(),
            button.getLocalBounds()
                .withTrimmedLeft(juce::roundToInt(tick_width) + 10)
                .withTrimmedRight(2),
            juce::Justification::centredLeft,
            10);
    }

    void drawLinearSlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float slider_position,
        float minimum_position,
        float maximum_position,
        juce::Slider::SliderStyle style,
        juce::Slider& slider) override {
        const auto draw = [&] {
            juce::LookAndFeel_V4::drawLinearSlider(
                graphics,
                x,
                y,
                width,
                height,
                slider_position,
                minimum_position,
                maximum_position,
                style,
                slider);
        };
        if (slider.isEnabled()) {
            draw();
            return;
        }
        graphics.beginTransparencyLayer(kDisabledOpacity);
        draw();
        graphics.endTransparencyLayer();
    }

    void drawRotarySlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float slider_position,
        float start_angle,
        float end_angle,
        juce::Slider& slider) override {
        const auto draw = [&] {
            juce::LookAndFeel_V4::drawRotarySlider(
                graphics,
                x,
                y,
                width,
                height,
                slider_position,
                start_angle,
                end_angle,
                slider);
        };
        if (slider.isEnabled()) {
            draw();
            return;
        }
        graphics.beginTransparencyLayer(kDisabledOpacity);
        draw();
        graphics.endTransparencyLayer();
    }

private:
    // 無効なバーはスイッチ（位相自動・音量維持）と同じ薄さへ揃える。
    static constexpr float kDisabledOpacity = 0.45F;
};
