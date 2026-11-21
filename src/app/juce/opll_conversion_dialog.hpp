// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"
#include "mgstc_look_and_feel.hpp"
#include "ui_fonts.hpp"
#include "ui_image_io.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"

namespace mgstc::app {

// Message-thread presentation only. The owner supplies immutable display
// values, rejects stale callbacks, and owns search/audition/commit lifetimes.
// This window never enters a modal state and never deletes itself.
class OpllConversionDialog final : public juce::DocumentWindow {
public:
    using StockId = std::uint64_t;
    enum class Audition { None, Original, Candidate };

    struct StockRow {
        StockId id{};
        juce::String label;
        juce::String detail;
        bool same_timbre{};
        bool operator==(const StockRow&) const = default;
    };

    struct State {
        bool confirming{true};
        bool running{};
        bool busy{};
        double elapsed_seconds{};
        double time_limit_seconds{300.0};
        juce::String status;
        juce::String candidate_description;
        bool original_available{};
        bool candidate_available{};
        bool can_add_stock{};
        bool can_apply{};
        bool can_delete_stock{};
        bool can_stop_audition{};
        Audition audition{Audition::None};
        std::vector<StockRow> stocks;
        // No stock ID means the current fully evaluated candidate.
        std::optional<StockId> selected_stock;
        bool operator==(const State&) const = default;
    };

    struct Callbacks {
        std::function<void()> start;
        std::function<void()> listen_original;
        std::function<void()> listen_candidate;
        std::function<void()> stop_audition;
        std::function<void()> add_stock;
        std::function<void(std::optional<StockId>)> select_stock;
        std::function<void(StockId)> delete_stock;
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

    class Content final : public juce::Component,
                          private juce::ListBoxModel {
    public:
        explicit Content(Callbacks callbacks)
            : callbacks_(std::move(callbacks)), progress_(elapsed_fraction_) {
            const ScopedGlobalScale scale;
            for (auto* label : {&status_, &time_, &candidate_, &stock_heading_}) {
                label->setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(*label);
            }
            status_.setJustificationType(juce::Justification::centred);
            time_.setJustificationType(juce::Justification::centred);
            stock_heading_.setText(juce::String::fromUTF8("ストック"),
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
                "現在選択している音色を聴きます。ストックには追加しません。");
            configureButton(stop_, "停止", 120, "試聴を停止します。");
            configureButton(add_, "ストックに追加", 130,
                "現在の候補をこの変換中だけ保持します。");
            configureButton(apply_, "この音色を使う", 140,
                "探索を終了し、選択した音色を適用します。");
            configureButton(remove_, "選択を削除", 160,
                "選択したストックを削除します。");
            configureButton(start_, "変換開始", 170,
                "原音からOPLL音色への変換を開始します。");
            configureButton(cancel_, "キャンセル", 180,
                "音色を適用せずに閉じます。");

            original_.onClick = [this] { invoke(callbacks_.listen_original); };
            candidate_play_.onClick = [this] { invoke(callbacks_.listen_candidate); };
            stop_.onClick = [this] { invoke(callbacks_.stop_audition); };
            add_.onClick = [this] { invoke(callbacks_.add_stock); };
            apply_.onClick = [this] { invoke(callbacks_.apply); };
            start_.onClick = [this] { invoke(callbacks_.start); };
            cancel_.onClick = [this] { invoke(callbacks_.cancel); };
            remove_.onClick = [this] {
                const auto id = state_.selected_stock;
                const auto callback = callbacks_.delete_stock;
                if (id && callback) callback(*id);
            };

            stocks_.setModel(this);
            stocks_.setMultipleSelectionEnabled(false);
            stocks_.setColour(juce::ListBox::backgroundColourId,
                              juce::Colour(UiLayout::panelFill));
            stocks_.setColour(juce::ListBox::outlineColourId,
                              juce::Colour(UiLayout::panelStroke));
            stocks_.setOutlineThickness(1);
            stocks_.setWantsKeyboardFocus(true);
            stocks_.setExplicitFocusOrder(150);
            stocks_.setTitle(juce::String::fromUTF8("候補とストック"));
            stocks_.setDescription(juce::String::fromUTF8(
                "矢印キーで音色を選択できます。選択だけでは適用されません。"));
            stocks_.setTooltip(juce::String::fromUTF8(
                "現在の候補またはストックを選択し、試聴してから適用できます。"));
            addAndMakeVisible(stocks_);
            setFocusContainerType(FocusContainerType::keyboardFocusContainer);
            refreshFonts();
            setState({});
        }

        ~Content() override { stocks_.setModel(nullptr); }

        void setState(const State& state) {
            const ScopedGlobalScale scale;
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
            candidate_.setText(state_.candidate_description.isNotEmpty()
                ? state_.candidate_description
                : juce::String::fromUTF8(state_.candidate_available
                    ? "試聴する音色を選べます" : "試聴できる候補を準備しています…"),
                juce::dontSendNotification);
            gif_.setVisible(state_.running && !state_.confirming);
            progress_.setVisible(!state_.confirming);
            time_.setVisible(!state_.confirming);
            candidate_.setVisible(!state_.confirming);
            for (auto* control : {static_cast<juce::Component*>(&original_),
                 static_cast<juce::Component*>(&candidate_play_),
                 static_cast<juce::Component*>(&stop_), static_cast<juce::Component*>(&add_),
                 static_cast<juce::Component*>(&apply_), static_cast<juce::Component*>(&remove_),
                 static_cast<juce::Component*>(&stocks_), static_cast<juce::Component*>(&stock_heading_)})
                control->setVisible(!state_.confirming);
            const bool interactive = !state_.confirming && !state_.busy;
            original_.setEnabled(interactive && state_.original_available);
            candidate_play_.setEnabled(interactive && state_.candidate_available);
            stop_.setEnabled(!state_.confirming && state_.can_stop_audition);
            original_.setToggleState(state_.audition == Audition::Original,
                                     juce::dontSendNotification);
            candidate_play_.setToggleState(state_.audition == Audition::Candidate,
                                           juce::dontSendNotification);
            add_.setEnabled(interactive && state_.can_add_stock);
            apply_.setEnabled(interactive && state_.can_apply);
            remove_.setEnabled(interactive && state_.can_delete_stock
                               && selectedStockExists());
            stocks_.setEnabled(interactive);
            start_.setVisible(state_.confirming);
            start_.setEnabled(state_.confirming && !state_.busy);
            cancel_.setEnabled(!state_.busy);
            cancel_.setButtonText(juce::String::fromUTF8(
                !state_.confirming && !state_.running ? "閉じる" : "キャンセル"));
            stock_heading_.setText(juce::String::fromUTF8("ストック")
                + " (" + juce::String(static_cast<int>(state_.stocks.size())) + ")",
                juce::dontSendNotification);
            updating_selection_ = true;
            stocks_.updateContent();
            int selected = state_.selected_stock ? -1 : 0;
            for (std::size_t i = 0; i < state_.stocks.size(); ++i)
                if (state_.selected_stock == state_.stocks[i].id)
                    selected = static_cast<int>(i) + 1;
            if (selected >= 0) stocks_.selectRow(selected, false, true);
            else stocks_.deselectAllRows();
            updating_selection_ = false;
            resized();
            repaint();
        }

        void refreshFonts() {
            const ScopedGlobalScale scale;
            status_.setFont(UiFonts::body());
            time_.setFont(UiFonts::body());
            candidate_.setFont(UiFonts::body());
            stock_heading_.setFont(UiFonts::heading());
            stocks_.setRowHeight(UiLayout::fieldH * 2);
            stocks_.repaint();
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
            remove_.setBounds(footer.removeFromLeft(UiScale::sx(144)));
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
            candidate_.setBounds(area.removeFromTop(UiLayout::fieldH));
            area.removeFromTop(UiLayout::sm);
            auto audition = area.removeFromTop(UiLayout::textButtonH);
            const int audition_width = (audition.getWidth() - UiLayout::controlGap * 2) / 3;
            original_.setBounds(audition.removeFromLeft(audition_width));
            audition.removeFromLeft(UiLayout::controlGap);
            candidate_play_.setBounds(audition.removeFromLeft(audition_width));
            audition.removeFromLeft(UiLayout::controlGap);
            stop_.setBounds(audition);
            area.removeFromTop(UiLayout::controlGap);
            auto actions = area.removeFromTop(UiLayout::textButtonH);
            add_.setBounds(actions.removeFromLeft(
                (actions.getWidth() - UiLayout::controlGap) / 2));
            actions.removeFromLeft(UiLayout::controlGap);
            apply_.setBounds(actions);
            area.removeFromTop(UiLayout::sm);
            stock_heading_.setBounds(area.removeFromTop(UiLayout::titleH));
            area.removeFromTop(UiLayout::xs);
            stocks_.setBounds(area);
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

        bool selectedStockExists() const {
            return state_.selected_stock && std::any_of(
                state_.stocks.begin(), state_.stocks.end(), [&](const auto& stock) {
                    return stock.id == *state_.selected_stock;
                });
        }

        int getNumRows() override { return static_cast<int>(state_.stocks.size()) + 1; }

        void paintListBoxItem(int row, juce::Graphics& graphics, int width,
                              int height, bool selected) override {
            const ScopedGlobalScale scale;
            if (row < 0 || row >= getNumRows()) return;
            if (selected) graphics.fillAll(uiToggleOnFill());
            auto bounds = juce::Rectangle<int>(width, height)
                .reduced(UiLayout::panelPad, UiLayout::xs);
            juce::String title;
            juce::String detail;
            if (row == 0) {
                title = juce::String::fromUTF8("現在の候補");
                detail = state_.candidate_available
                    ? juce::String::fromUTF8("探索で見つかった最新の音色")
                    : juce::String::fromUTF8("候補を準備しています…");
            } else {
                const auto& stock = state_.stocks[static_cast<std::size_t>(row - 1)];
                title = stock.label.isNotEmpty() ? stock.label
                    : juce::String::fromUTF8("ストック ") + juce::String(row);
                if (stock.same_timbre) title += juce::String::fromUTF8("　同じ音色");
                detail = stock.detail;
            }
            graphics.setColour(findColour(juce::Label::textColourId));
            graphics.setFont(UiFonts::body(true));
            graphics.drawFittedText(title, bounds.removeFromTop(bounds.getHeight() / 2),
                                    juce::Justification::centredLeft, 1);
            graphics.setFont(UiFonts::body());
            graphics.drawFittedText(detail, bounds, juce::Justification::centredLeft, 1);
        }

        void selectedRowsChanged(int row) override {
            if (updating_selection_ || state_.busy || state_.confirming
                || row < 0 || row >= getNumRows()) return;
            const auto id = row == 0 ? std::optional<StockId>{}
                : std::optional<StockId>{state_.stocks[static_cast<std::size_t>(row - 1)].id};
            if (id == state_.selected_stock) return;
            const auto callback = callbacks_.select_stock;
            if (callback) callback(id);
        }

        State state_;
        Callbacks callbacks_;
        bool updating_selection_{};
        double elapsed_fraction_{};
        juce::Label status_, time_, candidate_, stock_heading_;
        juce::ProgressBar progress_;
        AnimatedGifComponent gif_;
        juce::TextButton original_, candidate_play_, stop_, add_, apply_;
        juce::TextButton remove_, start_, cancel_;
        juce::ListBox stocks_;
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
