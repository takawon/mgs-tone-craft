// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"
#include "mgstc_look_and_feel.hpp"
#include "ui_fonts.hpp"
#include "ui_image_io.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"

namespace mgstc::app {

// Message-thread presentation only. The owner supplies immutable display
// values, rejects stale callbacks, and owns search/audition/commit lifetimes.
// This window never enters a modal state and never deletes itself.
class OpllConversionDialog final : public juce::DocumentWindow {
public:
    enum class Audition { None, Original, Candidate };

    struct Details {
        std::array<std::uint8_t, 8> registers{};
        std::uint8_t midi_note{60};
        bool operator==(const Details&) const = default;
    };

    struct State {
        bool confirming{true};
        bool running{};
        bool busy{};
        double elapsed_seconds{};
        double time_limit_seconds{300.0};
        juce::String status;
        bool original_available{};
        bool candidate_available{};
        bool can_apply{};
        bool can_stop_audition{};
        Audition audition{Audition::None};
        std::size_t candidate_count{};
        std::optional<std::size_t> selected_row;
        std::optional<Details> details;
        bool operator==(const State&) const = default;
    };

    struct Callbacks {
        std::function<void()> start;
        std::function<void()> listen_original;
        std::function<void()> listen_candidate;
        std::function<void()> stop_audition;
        std::function<void(std::size_t)> select_candidate;
        std::function<void()> apply;
        std::function<void()> cancel;
    };

private:
    // Non-editor windows use global scale without changing the active
    // editor's session scale when a paint/layout callback returns.
    class ScopedGlobalScale {
    public:
        ScopedGlobalScale() : previous_(UiScale::active_percent) {
            UiScale::forceGlobalForNonEditorUi();
        }
        ~ScopedGlobalScale() { UiScale::setActivePercent(previous_); }
    private:
        int previous_;
    };

    class DetailsPane final : public juce::Component, private juce::AsyncUpdater {
    public:
        ~DetailsPane() override { cancelPendingUpdate(); }

        void setDetails(const std::optional<Details>& details) {
            if (details_ == details) return;
            details_ = details;
            trace_ = {};
            if (details_) {
                patch_ = engine::decodeOpllPatch(details_->registers);
                triggerAsyncUpdate();
            } else {
                cancelPendingUpdate();
            }
            repaint();
        }

        void paint(juce::Graphics& graphics) override {
            const ScopedGlobalScale scale;
            graphics.fillAll(juce::Colour(UiLayout::panelFill));
            auto bounds = getLocalBounds().reduced(UiLayout::sm);
            graphics.setColour(findColour(juce::Label::textColourId));
            graphics.setFont(UiFonts::body());
            if (!details_) {
                graphics.drawFittedText(juce::String::fromUTF8("選択した候補の音色を表示します"),
                    bounds, juce::Justification::centred, 2);
                return;
            }
            const auto valueRow = [&](const char* name, int mod, int car) {
                auto row = bounds.removeFromTop(UiScale::sx(19));
                const auto name_width = row.getWidth() / 3;
                graphics.drawText(juce::String(name), row.removeFromLeft(name_width),
                    juce::Justification::centredLeft);
                const auto mod_cell = row.removeFromLeft(row.getWidth() / 2);
                graphics.drawText(mod < 0 ? juce::String("MOD") : juce::String(mod),
                    mod_cell, juce::Justification::centred);
                if (car >= 0 || mod < 0) {
                    graphics.drawText(car < 0 ? juce::String("CAR") : juce::String(car),
                        row, juce::Justification::centred);
                }
            };
            const auto& mod = patch_.modulator;
            const auto& car = patch_.carrier;
            valueRow("FM", -1, -1);
            valueRow("AM", mod.amplitude_modulation, car.amplitude_modulation);
            valueRow("PM", mod.pitch_modulation, car.pitch_modulation);
            valueRow("EG", mod.sustained_tone, car.sustained_tone);
            valueRow("KR", mod.key_rate_scaling, car.key_rate_scaling);
            valueRow("MULT", mod.multiplier, car.multiplier);
            valueRow("KSL", mod.key_scale_level, car.key_scale_level);
            valueRow("WS", mod.waveform, car.waveform);
            valueRow("AR", mod.attack_rate, car.attack_rate);
            valueRow("DR", mod.decay_rate, car.decay_rate);
            valueRow("SL", mod.sustain_level, car.sustain_level);
            valueRow("RR", mod.release_rate, car.release_rate);
            valueRow("TL", mod.total_level, -1);
            graphics.drawText("FB  " + juce::String(static_cast<int>(patch_.feedback)),
                bounds.removeFromTop(UiScale::sx(19)), juce::Justification::centredLeft);
            bounds.removeFromTop(UiLayout::xs);
            drawEnvelope(graphics, bounds);
        }

    private:
        void handleAsyncUpdate() override {
            if (!details_) return;
            trace_ = engine::traceOpllEnvelope(patch_, details_->midi_note);
            repaint();
        }

        void drawEnvelope(juce::Graphics& graphics, juce::Rectangle<int> bounds) const {
            const auto carrier = findColour(juce::Label::textColourId);
            const auto modulator = uiHoverAccent();
            auto legend = bounds.removeFromTop(UiLayout::fieldH);
            graphics.drawText("EG", legend.removeFromLeft(UiScale::sx(32)),
                juce::Justification::centredLeft);
            graphics.setColour(modulator);
            graphics.drawText("MOD", legend.removeFromLeft(UiScale::sx(60)),
                juce::Justification::centred);
            graphics.setColour(carrier);
            graphics.drawText("CAR", legend.removeFromLeft(UiScale::sx(60)),
                juce::Justification::centred);
            auto time = bounds.removeFromBottom(UiLayout::fieldH);
            const auto plot = bounds.toFloat().reduced(1.0F);
            if (plot.isEmpty()) return;
            constexpr auto duration = engine::OpllEnvelopeTrace::kDurationSeconds;
            constexpr auto key_off = engine::OpllEnvelopeTrace::kKeyOffSeconds;
            const auto off_x = plot.getX() + plot.getWidth() * key_off / duration;
            graphics.setColour(juce::Colour(UiLayout::panelStroke));
            graphics.drawRect(plot);
            graphics.drawVerticalLine(juce::roundToInt(off_x), plot.getY(), plot.getBottom());
            graphics.setColour(carrier);
            const int label_width = time.getWidth() / 3;
            graphics.drawFittedText("ON 0 s", time.removeFromLeft(label_width),
                juce::Justification::centredLeft, 1);
            graphics.drawFittedText("OFF " + juce::String(key_off, 0) + " s",
                time.removeFromLeft(label_width), juce::Justification::centred, 1);
            graphics.drawFittedText(juce::String(duration, 0) + " s", time,
                juce::Justification::centredRight, 1);
            if (!trace_.valid) return;
            const auto draw = [&](const auto& samples, juce::Colour colour) {
                juce::Path path;
                for (std::size_t i = 0; i < samples.size(); ++i) {
                    const auto x = plot.getX() + plot.getWidth() * static_cast<float>(i)
                        / static_cast<float>(samples.size() - 1);
                    const auto y = plot.getBottom() - plot.getHeight() * samples[i];
                    if (i == 0) path.startNewSubPath(x, y);
                    else path.lineTo(x, y);
                }
                graphics.setColour(colour);
                graphics.strokePath(path, juce::PathStrokeType(1.5F));
            };
            draw(trace_.modulator, modulator);
            draw(trace_.carrier, carrier);
        }

        std::optional<Details> details_;
        engine::OpllPatchParameters patch_{};
        engine::OpllEnvelopeTrace trace_{};
    };

    class Content final : public juce::Component,
                          private juce::ListBoxModel {
    public:
        explicit Content(Callbacks callbacks)
            : callbacks_(std::move(callbacks)), progress_(elapsed_fraction_) {
            const ScopedGlobalScale scale;
            for (auto* label : {&status_, &time_, &history_heading_}) {
                label->setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(*label);
            }
            status_.setJustificationType(juce::Justification::centred);
            time_.setJustificationType(juce::Justification::centred);
            history_heading_.setText(juce::String::fromUTF8("候補一覧"),
                                   juce::dontSendNotification);
            progress_.setPercentageDisplay(false);
            progress_.setColour(juce::ProgressBar::backgroundColourId,
                                juce::Colour(UiLayout::panelFill));
            progress_.setColour(juce::ProgressBar::foregroundColourId, uiHoverAccent());
            addAndMakeVisible(progress_);
            gif_.loadFromMemory(BinaryData::mgstc_spin_gif,
                                BinaryData::mgstc_spin_gifSize);
            addAndMakeVisible(gif_);

            configureButton(original_, "原音を聴く", 100,
                "変換前の原音を聴きます。候補の再生中は切り替わります。");
            configureButton(candidate_play_, "候補を聴く", 110,
                "選択している候補を聴きます。");
            configureButton(stop_, "停止", 120, "試聴を停止します。");
            configureButton(apply_, "この音色を使う", 140,
                "探索を終了し、選択した音色を適用します。");
            configureButton(start_, "変換開始", 170,
                "原音からOPLL音色への変換を開始します。");
            configureButton(cancel_, "キャンセル", 180,
                "音色を適用せずに閉じます。");

            original_.onClick = [this] { invoke(callbacks_.listen_original); };
            candidate_play_.onClick = [this] { invoke(callbacks_.listen_candidate); };
            stop_.onClick = [this] { invoke(callbacks_.stop_audition); };
            apply_.onClick = [this] { invoke(callbacks_.apply); };
            start_.onClick = [this] { invoke(callbacks_.start); };
            cancel_.onClick = [this] { invoke(callbacks_.cancel); };
            details_scroll_.setViewedComponent(&details_, false);
            details_scroll_.setScrollBarsShown(true, false);
            addAndMakeVisible(details_scroll_);
            history_.setModel(this);
            history_.setMultipleSelectionEnabled(false);
            history_.setColour(juce::ListBox::backgroundColourId,
                              juce::Colour(UiLayout::panelFill));
            history_.setColour(juce::ListBox::outlineColourId,
                              juce::Colour(UiLayout::panelStroke));
            history_.setOutlineThickness(1);
            history_.setWantsKeyboardFocus(true);
            history_.setExplicitFocusOrder(150);
            history_.setTitle(juce::String::fromUTF8("候補一覧"));
            history_.setDescription(juce::String::fromUTF8(
                "矢印キーで音色を選択できます。選択だけでは適用されません。"));
            addAndMakeVisible(history_);
            setFocusContainerType(FocusContainerType::keyboardFocusContainer);
            refreshFonts();
            setState({});
        }

        ~Content() override { history_.setModel(nullptr); }

        void setState(const State& state) {
            const ScopedGlobalScale scale;
            const auto previous_count = state_.candidate_count;
            const auto previous_row = state_.selected_row;
            state_ = state;
            const auto usable_limit = std::isfinite(state_.time_limit_seconds)
                && state_.time_limit_seconds > 0.0 ? state_.time_limit_seconds : 300.0;
            const auto elapsed = std::isfinite(state_.elapsed_seconds)
                ? std::max(0.0, state_.elapsed_seconds) : 0.0;
            elapsed_fraction_ = std::clamp(elapsed / usable_limit, 0.0, 1.0);
            if (state_.confirming) {
                status_.setText(juce::String::fromUTF8(
                    "OPLL音色に変換します。最大約5分かかります。\n途中で試聴や選択もできます。"),
                    juce::dontSendNotification);
            } else {
                status_.setText(state_.status.isNotEmpty() ? state_.status
                    : juce::String::fromUTF8(state_.running
                        ? "音色を探しています…" : "音色を選んでください"),
                    juce::dontSendNotification);
            }
            time_.setText(juce::String::fromUTF8("経過 ")
                + formatTime(std::min(elapsed, usable_limit))
                + juce::String::fromUTF8(" / ") + formatTime(usable_limit)
                + juce::String::fromUTF8("　残りの目安 ")
                + formatTime(std::max(0.0, usable_limit - elapsed)),
                juce::dontSendNotification);
            gif_.setVisible(state_.running && !state_.confirming);
            progress_.setVisible(!state_.confirming);
            time_.setVisible(!state_.confirming);
            for (auto* control : {static_cast<juce::Component*>(&original_),
                 static_cast<juce::Component*>(&candidate_play_),
                 static_cast<juce::Component*>(&stop_), static_cast<juce::Component*>(&apply_),
                 static_cast<juce::Component*>(&history_), static_cast<juce::Component*>(&history_heading_),
                 static_cast<juce::Component*>(&details_scroll_)})
                control->setVisible(!state_.confirming);
            const bool interactive = !state_.confirming && !state_.busy;
            original_.setEnabled(interactive && state_.original_available);
            candidate_play_.setEnabled(interactive && state_.candidate_available);
            stop_.setEnabled(!state_.confirming && state_.can_stop_audition);
            original_.setToggleState(state_.audition == Audition::Original,
                                     juce::dontSendNotification);
            candidate_play_.setToggleState(state_.audition == Audition::Candidate,
                                           juce::dontSendNotification);
            apply_.setEnabled(interactive && state_.can_apply);
            history_.setEnabled(interactive);
            start_.setVisible(state_.confirming);
            start_.setEnabled(state_.confirming && !state_.busy);
            cancel_.setEnabled(!state_.busy);
            cancel_.setButtonText(juce::String::fromUTF8(
                !state_.confirming && !state_.running ? "閉じる" : "キャンセル"));
            details_.setDetails(state_.details);
            updating_selection_ = true;
            if (previous_count != state_.candidate_count) history_.updateContent();
            if (previous_row != state_.selected_row) {
                if (state_.selected_row)
                    history_.selectRow(static_cast<int>(*state_.selected_row), false, true);
                else
                    history_.deselectAllRows();
            }
            updating_selection_ = false;
            resized();
            repaint();
        }

        void refreshFonts() {
            const ScopedGlobalScale scale;
            status_.setFont(UiFonts::body());
            time_.setFont(UiFonts::body());
            history_heading_.setFont(UiFonts::heading());
            history_.setRowHeight(UiLayout::textButtonH);
            history_.repaint();
            resized();
        }

        void paint(juce::Graphics& graphics) override {
            const ScopedGlobalScale scale;
            paintPageBackground(graphics, getLocalBounds());
        }

        void resized() override {
            const ScopedGlobalScale scale;
            auto area = getLocalBounds().reduced(UiLayout::panelPad);
            auto footer = area.removeFromBottom(UiLayout::textButtonH);
            cancel_.setBounds(footer.removeFromRight(UiScale::sx(128)));
            footer.removeFromRight(UiLayout::controlGap);
            if (state_.confirming)
                start_.setBounds(footer.removeFromRight(UiScale::sx(144)));
            else
                apply_.setBounds(footer.removeFromRight(UiScale::sx(180)));
            area.removeFromBottom(UiLayout::sm);
            if (state_.confirming) {
                status_.setBounds(area);
                return;
            }
            status_.setBounds(area.removeFromTop(UiLayout::fieldH * 2));
            area.removeFromTop(UiLayout::sm);
            const int image_height = UiLayout::fieldH * 3;
            auto banner = area.removeFromTop(image_height);
            if (state_.running && !state_.confirming) {
                gif_.setBounds(banner.removeFromLeft(image_height));
                banner.removeFromLeft(UiLayout::panelGap);
            }
            const int progress_height = UiLayout::fieldH;
            const int banner_padding = std::max(0,
                (banner.getHeight() - progress_height - UiLayout::fieldH - UiLayout::sm) / 2);
            banner.removeFromTop(banner_padding);
            progress_.setBounds(banner.removeFromTop(progress_height));
            banner.removeFromTop(UiLayout::sm);
            time_.setBounds(banner.removeFromTop(UiLayout::fieldH));
            area.removeFromTop(UiLayout::sm);
            auto audition = area.removeFromTop(UiLayout::textButtonH);
            const int audition_width = (audition.getWidth() - UiLayout::controlGap * 2) / 3;
            original_.setBounds(audition.removeFromLeft(audition_width));
            audition.removeFromLeft(UiLayout::controlGap);
            candidate_play_.setBounds(audition.removeFromLeft(audition_width));
            audition.removeFromLeft(UiLayout::controlGap);
            stop_.setBounds(audition);
            area.removeFromTop(UiLayout::controlGap);
            auto left = area.removeFromLeft(area.getWidth() * 55 / 100);
            details_scroll_.setBounds(left);
            const int pane_height = state_.details ? UiScale::sx(420) : left.getHeight();
            details_.setSize(std::max(0, left.getWidth() - details_scroll_.getScrollBarThickness()),
                std::max(left.getHeight(), pane_height));
            area.removeFromLeft(UiLayout::controlGap);
            history_heading_.setBounds(area.removeFromTop(UiLayout::titleH));
            history_.setBounds(area);
        }

        bool keyPressed(const juce::KeyPress& key) override {
            if (key.getKeyCode() != juce::KeyPress::escapeKey || state_.busy)
                return false;
            invoke(callbacks_.cancel);
            return true;
        }

    private:
        // Copy before invocation: an owner callback may destroy this window.
        static void invoke(std::function<void()> callback) {
            if (callback) callback();
        }

        static juce::String formatTime(double seconds) {
            const auto count = static_cast<int>(std::clamp(std::ceil(seconds), 0.0, 86'400.0));
            return juce::String(count / 60) + ":" + juce::String(count % 60).paddedLeft('0', 2);
        }

        void configureButton(juce::TextButton& button, const char* title,
                             int order, const char* tooltip) {
            button.setButtonText(juce::String::fromUTF8(title));
            button.setTooltip(juce::String::fromUTF8(tooltip));
            button.setWantsKeyboardFocus(true);
            button.setExplicitFocusOrder(order);
            addAndMakeVisible(button);
        }

        int getNumRows() override { return static_cast<int>(state_.candidate_count); }

        juce::String getNameForRow(int row) override {
            return juce::String::fromUTF8("候補") + juce::String(row + 1);
        }

        void paintListBoxItem(int row, juce::Graphics& graphics, int width,
                              int height, bool selected) override {
            const ScopedGlobalScale scale;
            if (row < 0 || row >= getNumRows()) return;
            if (selected) graphics.fillAll(uiToggleOnFill());
            auto bounds = juce::Rectangle<int>(width, height)
                .reduced(UiLayout::panelPad, UiLayout::xs);
            graphics.setColour(findColour(juce::Label::textColourId));
            graphics.setFont(UiFonts::body(true));
            graphics.drawFittedText(getNameForRow(row), bounds,
                                    juce::Justification::centredLeft, 1);
        }

        void selectedRowsChanged(int row) override {
            chooseRow(row);
        }

        void listBoxItemClicked(int row, const juce::MouseEvent&) override {
            // Clicking the already highlighted row makes the selection explicit.
            chooseRow(row);
        }

        void returnKeyPressed(int row) override { chooseRow(row); }

        void chooseRow(int row) {
            if (updating_selection_ || state_.busy || state_.confirming
                || row < 0 || row >= getNumRows()) return;
            const auto callback = callbacks_.select_candidate;
            if (callback) callback(static_cast<std::size_t>(row));
        }

        State state_;
        Callbacks callbacks_;
        bool updating_selection_{};
        double elapsed_fraction_{};
        juce::Label status_, time_, history_heading_;
        juce::ProgressBar progress_;
        AnimatedGifComponent gif_;
        juce::TextButton original_, candidate_play_, stop_, apply_, start_, cancel_;
        DetailsPane details_;
        juce::Viewport details_scroll_;
        juce::ListBox history_;
        juce::TooltipWindow tooltip_{this};
    };

public:
    explicit OpllConversionDialog(Callbacks callbacks)
        : DocumentWindow(juce::String::fromUTF8("OPLL音色に変換"),
              juce::Colour(UiLayout::panelFill), DocumentWindow::closeButton, false),
          on_cancel_(callbacks.cancel) {
        const ScopedGlobalScale scale;
        setLookAndFeel(&look_and_feel_);
        setUsingNativeTitleBar(true);
        setResizable(true, false);
        content_ = new Content(std::move(callbacks));
        setContentOwned(content_, true);
        setResizeLimits(UiScale::sx(620), UiScale::sx(280),
                        UiScale::sx(1200), UiScale::sx(1100));
        centreWithSize(UiScale::sx(700), UiScale::sx(300));
        const juce::Component::SafePointer<OpllConversionDialog> safe(this);
        scale_listener_ = UiScale::addGlobalListener([safe] {
            if (safe) safe->refreshScale();
        });
    }

    ~OpllConversionDialog() override {
        UiScale::removeGlobalListener(scale_listener_);
        clearContentComponent();
        setLookAndFeel(nullptr);
    }

    void setState(const State& state) {
        const ScopedGlobalScale scale;
        if (confirming_ != state.confirming) {
            confirming_ = state.confirming;
            setResizeLimits(UiScale::sx(620), UiScale::sx(confirming_ ? 280 : 620),
                            UiScale::sx(1200), UiScale::sx(1100));
            setSize(getWidth(), UiScale::sx(confirming_ ? 300 : 700));
            clampWindowToDisplayWorkArea(*this);
        }
        content_->setState(state);
    }

    void showFor(juce::Component& anchor) {
        const ScopedGlobalScale scale;
        if (!getPeer()) addToDesktop(getDesktopWindowStyleFlags());
#if JUCE_WINDOWS
        if (auto* parent_peer = anchor.getTopLevelComponent()->getPeer()) {
            const auto parent = static_cast<HWND>(parent_peer->getNativeHandle());
            const auto owner = parent ? GetAncestor(parent, GA_ROOT) : nullptr;
            const auto native = getPeer() ? static_cast<HWND>(getPeer()->getNativeHandle()) : nullptr;
            if (native && IsWindow(native) && owner && IsWindow(owner))
                SetWindowLongPtrW(native, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
        }
#endif
        centreAroundComponent(&anchor, getWidth(), getHeight());
        clampWindowToDisplayWorkArea(*this);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override {
        const auto callback = on_cancel_;
        if (callback) callback();
    }

    void resized() override {
        const ScopedGlobalScale scale;
        DocumentWindow::resized();
    }

private:
    void refreshScale() {
        const ScopedGlobalScale scale;
        const auto ratio = static_cast<double>(UiScale::global_percent) / display_scale_percent_;
        const int width = juce::roundToInt(getWidth() * ratio);
        const int height = juce::roundToInt(getHeight() * ratio);
        display_scale_percent_ = UiScale::global_percent;
        content_->refreshFonts();
        setResizeLimits(UiScale::sx(620), UiScale::sx(confirming_ ? 280 : 620),
                        UiScale::sx(1200), UiScale::sx(1100));
        setSize(width, height);
        clampWindowToDisplayWorkArea(*this);
        resized();
        repaint();
    }

    MgstcLookAndFeel look_and_feel_;
    Content* content_{};
    std::function<void()> on_cancel_;
    std::uint64_t scale_listener_{};
    bool confirming_{true};
    int display_scale_percent_{UiScale::global_percent};
};

} // namespace mgstc::app
