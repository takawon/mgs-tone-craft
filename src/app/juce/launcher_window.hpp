// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "launcher_icons.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_paint.hpp"

namespace mgstc::app {
namespace LauncherLayout {
inline int width() { return 3 * UiLayout::launcherCardSize + 2 * UiLayout::panelGap + 4 * UiLayout::panelPad; }
inline int height() { return UiLayout::launcherCardSize + UiLayout::titleH + UiLayout::descriptionH + UiLayout::sm + 4 * UiLayout::panelPad; }
}
class LauncherCard final : public juce::Button {
public:
    LauncherCard(int index, juce::String title, juce::String subtitle)
        : Button(title), index_(index), title_(std::move(title)), subtitle_(std::move(subtitle)) {
        setTooltip(title_ + " / " + subtitle_);
        setWantsKeyboardFocus(true);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (isEnabled() && hasKeyboardFocus(false)
            && !isCurrentlyBlockedByAnotherModalComponent()
            && key.isKeyCode(juce::KeyPress::spaceKey)
            && !key.getModifiers().isAnyModifierKeyDown()) {
            triggerClick();
            return true;
        }
        return juce::Button::keyPressed(key);
    }
    void paintButton(juce::Graphics& g, bool hover, bool down) override {
        const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
        const std::array<juce::Colour, 3> colours{juce::Colour(0xff9b86b6), juce::Colour(0xffc59474), juce::Colour(0xff7db49a)};
        const auto accent = colours[static_cast<std::size_t>(index_)];
        g.setGradientFill(juce::ColourGradient(accent.interpolatedWith(juce::Colour(0xff20262e), down ? 0.60f : 0.83f), 0, 0,
            juce::Colour(0xff1b222b), 0, bounds.getBottom(), false));
        const float radius = static_cast<float>(UiLayout::panelPad * 2);
        g.fillRoundedRectangle(bounds, radius);
        g.setColour(hover || hasKeyboardFocus(true) ? juce::Colour(kUiHoverAccent) : accent.withAlpha(0.65f));
        g.drawRoundedRectangle(bounds, radius, hover || hasKeyboardFocus(true) ? 2.0f : 1.0f);
        auto inner = getLocalBounds().reduced(UiLayout::panelPad);
        const auto body = UiFonts::body().withExtraKerningFactor(-0.025f);
        auto image_area = inner.removeFromTop(UiLayout::launcherIconSize);
        const auto icon_bounds = image_area.withSizeKeepingCentre(
            UiLayout::launcherIconSize, UiLayout::launcherIconSize);
        inner.removeFromTop(UiLayout::xs);
        auto title = inner.removeFromTop(juce::roundToInt(body.getHeight()) + UiLayout::xs);
        auto english = inner.removeFromBottom(juce::roundToInt(body.getHeight() * 2.0f));
        g.setColour(juce::Colour(0xffe6edf3));
        g.setFont(body.boldened());
        g.drawFittedText(title_, title, juce::Justification::centred, 1, 0.9f);
        g.setColour(juce::Colour(0xffadb8c6));
        g.setFont(body);
        g.drawFittedText(subtitle_, english, juce::Justification::centred, 2, 0.9f);
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImageWithin(launcherIcon(index_), icon_bounds.getX(), icon_bounds.getY(),
            icon_bounds.getWidth(), icon_bounds.getHeight(), juce::RectanglePlacement::centred);
    }
private:
    int index_;
    juce::String title_, subtitle_;
};
class LauncherContent final : public juce::Component {
public:
    LauncherContent(std::function<void(const juce::String&)> open, std::function<void()> settings)
      : cards_{LauncherCard(0, juce::String::fromUTF8("複合音色エディタ"), "Composite Timbre Editor"),
               LauncherCard(1, juce::String::fromUTF8("OPLL音色エディタ"), "OPLL Timbre Editor"),
               LauncherCard(2, juce::String::fromUTF8("SCC音色エディタ"), "SCC Timbre Editor")} {
        const std::array<juce::String, 3> targets{"main", "opll", "scc"};
        for (std::size_t i=0; i<cards_.size(); ++i) {
            addAndMakeVisible(cards_[i]);
            cards_[i].setExplicitFocusOrder(static_cast<int>(i)+1);
            cards_[i].onClick = [this, open, target=targets[i]] {
                if (opening_) return;
                opening_=true;
                open(target);
            };
        }
        configureSettingsButton(settings_, std::move(settings));
        settings_.setExplicitFocusOrder(4);
        addAndMakeVisible(settings_);
        setSize(LauncherLayout::width(), LauncherLayout::height());
    }
    void paint(juce::Graphics& g) override {
        paintPageBackground(g, getLocalBounds());
        auto heading=getLocalBounds().reduced(UiLayout::panelPad*2);
        g.setColour(juce::Colour(0xffe6edf3));
        g.setFont(UiFonts::title());
        g.drawText(juce::String::fromUTF8("エディタ選択"), heading.removeFromTop(UiLayout::titleH), juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xffadb8c6));
        g.setFont(UiFonts::body());
        g.drawText("Editor Selection", heading.removeFromTop(UiLayout::descriptionH), juce::Justification::centredLeft);
    }
    void resized() override {
        auto area=getLocalBounds().reduced(UiLayout::panelPad*2);
        auto header=area.removeFromTop(UiLayout::titleH + UiLayout::descriptionH + UiLayout::sm);
        settings_.setBounds(header.removeFromRight(UiLayout::iconButton).removeFromTop(UiLayout::iconButton));
        const int gap=UiLayout::panelGap;
        const int width=(area.getWidth()-2*gap)/3;
        for (auto& card:cards_) { card.setBounds(area.removeFromLeft(width)); area.removeFromLeft(gap); }
    }
    LauncherCard& card(int index) { return cards_[static_cast<std::size_t>(index)]; }
    juce::DrawableButton& settingsButton() { return settings_; }
private:
    std::array<LauncherCard,3> cards_;
    juce::DrawableButton settings_{"settings", juce::DrawableButton::ImageOnButtonBackground};
    bool opening_{};
    juce::TooltipWindow tooltip_{this};
};
class LauncherWindow final : public juce::DocumentWindow {
public:
    LauncherWindow(EditorSession& session, std::function<void(const juce::String&)> open, std::function<void()> close)
      : DocumentWindow("MGS Tone Craft", juce::Colour(0xff20262e), closeButton), close_(std::move(close)) {
        setUsingNativeTitleBar(true);
        setContentOwned(new LauncherContent(std::move(open), [this, &session] {
            static_cast<void>(showAppSettings(session, *this));
        }), true);
        listener_=UiScale::addGlobalListener([this] { applyScale(); });
        centreWithSize(LauncherLayout::width(), LauncherLayout::height());
        clampWindowToDisplayWorkArea(*this);
        setVisible(true);
    }
    ~LauncherWindow() override { UiScale::removeGlobalListener(listener_); }
    void closeButtonPressed() override { close_(); }
    void applyScale() {
        UiScale::forceGlobalForNonEditorUi();
        setContentComponentSize(LauncherLayout::width(), LauncherLayout::height());
        getContentComponent()->resized();
        clampWindowToDisplayWorkArea(*this);
        repaint();
    }
private:
    std::function<void()> close_;
    std::uint64_t listener_{};
};
}
