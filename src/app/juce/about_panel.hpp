// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#ifndef MGSTC_DOC_VERSION
#define MGSTC_DOC_VERSION "0.0"
#endif

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_scale.hpp"

class AboutPanel final : public juce::Component {
public:
    AboutPanel() {
        logo_ = juce::ImageFileFormat::loadFrom(
            BinaryData::MGSTC_logo_png,
            BinaryData::MGSTC_logo_pngSize);
        title_.setText(
            "MGS Tone Craft", juce::dontSendNotification);
        title_.setFont(UiFonts::title());
        title_.setJustificationType(juce::Justification::centred);
        title_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(title_);

        subtitle_.setText(
            "MGSTC", juce::dontSendNotification);
        subtitle_.setFont(UiFonts::body());
        subtitle_.setJustificationType(juce::Justification::centred);
        subtitle_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(subtitle_);

        detail_.setText(
            juce::String::fromUTF8(
                "MGSDRV向け複合音色エディタ\n"
                "Version ")
                + juce::String(MGSTC_DOC_VERSION),
            juce::dontSendNotification);
        detail_.setJustificationType(juce::Justification::centred);
        detail_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(detail_);

        copyright_.setText(
            "Copyright(C) 2026 Takawo. All right reserves.",
            juce::dontSendNotification);
        copyright_.setJustificationType(juce::Justification::centred);
        copyright_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(copyright_);

        github_.setJustificationType(juce::Justification::centred);
        x_.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(github_);
        addAndMakeVisible(x_);
    }

    void paint(juce::Graphics& g) override {
        if (logo_.isValid()) {
            g.drawImageWithin(
                logo_,
                logo_bounds_.getX(),
                logo_bounds_.getY(),
                logo_bounds_.getWidth(),
                logo_bounds_.getHeight(),
                juce::RectanglePlacement::centred
                    | juce::RectanglePlacement::onlyReduceInSize);
        }
    }

    void resized() override {
        auto area = getLocalBounds().reduced(UiScale::sx(12));
        const int footer_row = UiScale::sx(22);
        const int title_h = UiScale::sx(28);
        const int subtitle_h = UiScale::sx(24);
        const int gap_logo_title = UiScale::sx(8);
        const int gap_detail = UiScale::sx(8);
        const int detail_h = UiScale::sx(48);

        // Natural (100%) asset size; shrink only when the panel is too small.
        const int logo_w = logo_.isValid()
            ? juce::jmax(1, logo_.getWidth())
            : UiScale::sx(256);
        const int logo_h = logo_.isValid()
            ? juce::jmax(1, logo_.getHeight())
            : UiScale::sx(256);

        x_.setBounds(area.removeFromBottom(footer_row));
        github_.setBounds(area.removeFromBottom(footer_row));
        copyright_.setBounds(area.removeFromBottom(footer_row));

        int display_logo_w = logo_w;
        int display_logo_h = logo_h;
        const int chrome_h = title_h + subtitle_h + gap_logo_title
            + gap_detail + detail_h;
        if (logo_w > area.getWidth()
            || logo_h > juce::jmax(1, area.getHeight() - chrome_h)) {
            const float fit = juce::jmin(
                static_cast<float>(area.getWidth())
                    / static_cast<float>(logo_w),
                static_cast<float>(
                    juce::jmax(1, area.getHeight() - chrome_h))
                    / static_cast<float>(logo_h));
            display_logo_w = juce::jmax(
                1, juce::roundToInt(static_cast<float>(logo_w) * fit));
            display_logo_h = juce::jmax(
                1, juce::roundToInt(static_cast<float>(logo_h) * fit));
        }

        const int display_stack_h =
            display_logo_h + gap_logo_title + title_h + subtitle_h;
        const int display_group_h =
            display_stack_h + gap_detail + detail_h;
        const int logo_y = (area.getHeight() < display_group_h)
            ? area.getY()
            : area.getY() + (area.getHeight() - display_group_h) / 2;

        logo_bounds_ = juce::Rectangle<int>(
                           area.getX(),
                           logo_y,
                           area.getWidth(),
                           display_logo_h)
                           .withSizeKeepingCentre(
                               display_logo_w, display_logo_h);
        title_.setBounds(
            area.getX(),
            logo_y + display_logo_h + gap_logo_title,
            area.getWidth(),
            title_h);
        subtitle_.setBounds(
            area.getX(),
            logo_y + display_logo_h + gap_logo_title + title_h,
            area.getWidth(),
            subtitle_h);
        detail_.setBounds(
            area.getX(),
            logo_y + display_stack_h + gap_detail,
            area.getWidth(),
            detail_h);
    }

private:
    juce::Image logo_;
    juce::Rectangle<int> logo_bounds_;
    juce::Label title_;
    juce::Label subtitle_;
    juce::Label detail_;
    juce::Label copyright_;
    juce::HyperlinkButton github_{
        "GitHub",
        juce::URL("https://github.com/takawon/mgs-tone-craft")};
    juce::HyperlinkButton x_{
        "X",
        juce::URL("https://x.com/takawo_n")};
};
