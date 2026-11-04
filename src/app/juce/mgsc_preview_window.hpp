// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "mgstc_look_and_feel.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_scale.hpp"

namespace mgstc::app {

class MgscPreviewWindow final : public juce::DocumentWindow {
    // Floating previews follow View/INI scale and must not overwrite the
    // editor's Ctrl+/- session metrics while servicing its callbacks.
    class ScopedGlobalScale {
    public:
        ScopedGlobalScale() : previous_(UiScale::active_percent) {
            UiScale::forceGlobalForNonEditorUi();
        }
        ~ScopedGlobalScale() { UiScale::setActivePercent(previous_); }
        ScopedGlobalScale(const ScopedGlobalScale&) = delete;
        ScopedGlobalScale& operator=(const ScopedGlobalScale&) = delete;
    private:
        int previous_;
    };
public:
    class Content final : public juce::Component {
        class Divider final : public juce::StretchableLayoutResizerBar {
        public:
            explicit Divider(juce::StretchableLayoutManager& layout)
                : StretchableLayoutResizerBar(&layout, 1, false), layout_(layout) {
                setWantsKeyboardFocus(true);
                setExplicitFocusOrder(150);
                setTitle(juce::String::fromUTF8("上下の表示枠の境界"));
                setDescription(juce::String::fromUTF8("ドラッグまたは上下矢印キーで高さを変更"));
            }
            bool keyPressed(const juce::KeyPress& key) override {
                const ScopedGlobalScale scale;
                if (key.getKeyCode() != juce::KeyPress::upKey && key.getKeyCode() != juce::KeyPress::downKey) return false;
                layout_.setItemPosition(1, getY() + (key.getKeyCode() == juce::KeyPress::upKey ? -UiLayout::fieldH : UiLayout::fieldH) - UiLayout::panelPad);
                if (auto* parent = getParentComponent()) parent->resized();
                return true;
            }
        private:
            juce::StretchableLayoutManager& layout_;
        };
    public:
        Content() : divider_(layout_) {
            const ScopedGlobalScale scale;
            selected_label_.setText(juce::String::fromUTF8("選択レイヤーのMML"), juce::dontSendNotification);
            composite_label_.setText(juce::String::fromUTF8("複合音色全体のMML"), juce::dontSendNotification);
            for (auto* label : {&selected_label_, &composite_label_}) addAndMakeVisible(*label);
            for (auto* editor : {&selected_, &composite_}) {
                editor->setMultiLine(true);
                editor->setReadOnly(true);
                editor->setScrollbarsShown(true);
                editor->setWantsKeyboardFocus(true);
                editor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF151A20));
                editor->setColour(juce::TextEditor::textColourId, juce::Colour(0xFF9BE9C3));
                editor->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF40505E));
                addAndMakeVisible(*editor);
            }
            selected_.setExplicitFocusOrder(100);
            composite_.setExplicitFocusOrder(200);
            addAndMakeVisible(divider_);
            refreshFonts();
        }

        void setTexts(const juce::String& selected, const juce::String& composite) {
            const ScopedGlobalScale scale;
            if (selected_.getText() != selected) UiFonts::setMgscPreviewText(selected_, selected);
            if (composite_.getText() != composite) UiFonts::setMgscPreviewText(composite_, composite);
        }
        [[nodiscard]] juce::String selectedText() const { return selected_.getText(); }
        [[nodiscard]] juce::String compositeText() const { return composite_.getText(); }
        void refreshFonts() {
            const ScopedGlobalScale scale;
            for (auto* label : {&selected_label_, &composite_label_}) label->setFont(UiFonts::body(true));
            for (auto* editor : {&selected_, &composite_}) UiFonts::refreshMgscPreviewFont(*editor);
            layout_.setItemLayout(0, -0.15, -0.85, -split_fraction_);
            layout_.setItemLayout(1, UiLayout::controlGap, UiLayout::controlGap, UiLayout::controlGap);
            layout_.setItemLayout(2, -0.15, -0.85, -(1.0 - split_fraction_));
            resized();
        }
        void resized() override {
            const ScopedGlobalScale scale;
            auto area = getLocalBounds().reduced(UiLayout::panelPad);
            juce::Component* items[]{&selected_area_, &divider_, &composite_area_};
            layout_.layOutComponents(items, 3, area.getX(), area.getY(), area.getWidth(), area.getHeight(), true, true);
            const int pane_height = area.getHeight() - divider_.getHeight();
            if (pane_height > 0)
                split_fraction_ = std::clamp(static_cast<double>(selected_area_.getHeight()) / pane_height, 0.15, 0.85);
            auto top = selected_area_.getBounds(), bottom = composite_area_.getBounds();
            selected_label_.setBounds(top.removeFromTop(UiLayout::fieldH));
            composite_label_.setBounds(bottom.removeFromTop(UiLayout::fieldH));
            selected_.setBounds(top); composite_.setBounds(bottom);
        }
        void paint(juce::Graphics& graphics) override { graphics.fillAll(juce::Colour(UiLayout::panelFill)); }
        [[nodiscard]] bool verifySplitAndResize() {
            const ScopedGlobalScale scale;
            const auto first = selected_.getHeight();
            const auto origin = divider_.getLocalBounds().getCentre();
            const auto now = juce::Time::getCurrentTime();
            const auto event = [&](juce::Point<int> point, bool dragged) {
                return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
                    juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                    1.0F, 0.0F, 0.0F, 0.0F, 0.0F, &divider_, &divider_, now, origin.toFloat(), now, 1, dragged);
            };
            divider_.mouseDown(event(origin, false));
            divider_.mouseDrag(event(origin.translated(0, UiLayout::fieldH * 2), true));
            const bool moved = selected_.getHeight() > first
                && selected_.getBottom() <= divider_.getY()
                && composite_.getY() >= divider_.getBottom()
                && getLocalBounds().contains(composite_.getBounds());
            setSize(getWidth() + UiLayout::panelPad, getHeight() + UiLayout::fieldH);
            const bool key_handled = divider_.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
            const int retained_height = selected_area_.getHeight();
            for (int refresh = 0; refresh < 8; ++refresh) refreshFonts();
            return moved && selected_.getHeight() > 0 && composite_.getHeight() > 0
                && key_handled && std::abs(selected_area_.getHeight() - retained_height) <= 1;
        }
        [[nodiscard]] bool verifyScaleIsolation() {
            const int saved_global = UiScale::global_percent;
            const int saved_active = UiScale::active_percent;
            UiScale::global_percent = 100; // Session-only fixture, never saved.
            float global_mono{}, global_body{};
            {
                const ScopedGlobalScale scale;
                global_mono = UiFonts::mono().getHeight();
                global_body = UiFonts::body(true).getHeight();
            }
            bool valid = true;
            for (const int foreign : {75, 125}) {
                UiScale::setActivePercent(foreign);
                const auto caller_pad = UiLayout::panelPad;
                const auto caller_field = UiLayout::fieldH;
                const auto caller_gap = UiLayout::controlGap;
                const auto caller_factor = UiScale::active_factor;
                const auto preserved = [&] {
                    return UiScale::active_percent == foreign && UiScale::active_factor == caller_factor
                        && UiLayout::panelPad == caller_pad && UiLayout::fieldH == caller_field
                        && UiLayout::controlGap == caller_gap;
                };
                Content isolated;
                valid = preserved() && valid;
                isolated.setTexts("selected before", "whole before");
                valid = preserved() && valid;
                isolated.setTexts("selected " + juce::String(foreign), "whole " + juce::String(foreign));
                valid = preserved() && valid;
                isolated.refreshFonts();
                valid = preserved() && valid;
                isolated.setSize(720, 480);
                valid = preserved() && valid;
                isolated.resized();
                valid = preserved() && valid;
                valid = std::abs(isolated.selected_.getFont().getHeight() - global_mono) < 0.01F
                    && std::abs(isolated.composite_.getFont().getHeight() - global_mono) < 0.01F
                    && std::abs(isolated.selected_label_.getFont().getHeight() - global_body) < 0.01F
                    && isolated.selected_label_.getHeight() == UiLayout::Base::fieldH
                    && isolated.selected_label_.getX() == UiLayout::Base::panelPad
                    && isolated.divider_.getHeight() == UiLayout::Base::controlGap && valid;
                const int before_key = isolated.selected_area_.getHeight();
                const bool handled = isolated.divider_.keyPressed(juce::KeyPress(juce::KeyPress::downKey));
                valid = handled && preserved()
                    && std::abs(isolated.selected_area_.getHeight() - before_key - UiLayout::Base::fieldH) <= 1
                    && valid;
            }
            UiScale::global_percent = saved_global;
            UiScale::setActivePercent(saved_active);
            return valid;
        }
    private:
        juce::StretchableLayoutManager layout_;
        Divider divider_;
        juce::Component selected_area_, composite_area_;
        juce::Label selected_label_, composite_label_;
        juce::TextEditor selected_, composite_;
        double split_fraction_{0.5};
    };

    explicit MgscPreviewWindow(std::function<void()> on_close)
        : DocumentWindow(juce::String::fromUTF8("MGS Tone Craft - MGSCプレビュー"),
              juce::Colour(UiLayout::panelFill), DocumentWindow::closeButton, false),
          on_close_(std::move(on_close)) {
        const ScopedGlobalScale scale;
        setLookAndFeel(&look_and_feel_);
        setUsingNativeTitleBar(true);
        content_ = new Content;
        setContentOwned(content_, true);
        setResizable(true, false);
        setResizeLimits(UiScale::sx(480), UiScale::sx(320), UiScale::sx(2400), UiScale::sx(1600));
        centreWithSize(UiScale::sx(960), UiScale::sx(640));
        juce::Component::SafePointer<MgscPreviewWindow> safe(this);
        scale_listener_ = UiScale::addGlobalListener([safe] {
            if (safe) safe->refreshFonts();
        });
        setVisible(false);
    }
    ~MgscPreviewWindow() override {
        UiScale::removeGlobalListener(scale_listener_);
        clearContentComponent();
        setLookAndFeel(nullptr);
    }
    void showFor(juce::Component& anchor) {
        const ScopedGlobalScale scale;
        if (auto* parent_peer = anchor.getTopLevelComponent()->getPeer()) {
            if (!getPeer()) addToDesktop(getDesktopWindowStyleFlags());
#if JUCE_WINDOWS
            const auto parent = static_cast<HWND>(parent_peer->getNativeHandle());
            const auto owner = parent ? GetAncestor(parent, GA_ROOT) : nullptr;
            const auto native = getPeer() ? static_cast<HWND>(getPeer()->getNativeHandle()) : nullptr;
            // A hosted editor can already be a child of a bridged host in
            // another process. Follow that actual ancestry, not its PID.
            if (native && IsWindow(native) && owner && IsWindow(owner))
                SetWindowLongPtrW(native, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
#else
            juce::ignoreUnused(parent_peer);
#endif
            refreshFonts();
            setVisible(true);
            toFront(true);
        }
    }
    void closeButtonPressed() override {
        setVisible(false);
        if (on_close_) on_close_();
    }
    void refreshFonts() {
        const ScopedGlobalScale scale;
        content_->refreshFonts();
        setResizeLimits(UiScale::sx(480), UiScale::sx(320), UiScale::sx(2400), UiScale::sx(1600));
    }
    void resized() override {
        const ScopedGlobalScale scale;
        DocumentWindow::resized();
    }
    [[nodiscard]] Content& content() const { return *content_; }
private:
    MgstcLookAndFeel look_and_feel_;
    Content* content_{};
    std::function<void()> on_close_;
    std::uint64_t scale_listener_{};
};

} // namespace mgstc::app
