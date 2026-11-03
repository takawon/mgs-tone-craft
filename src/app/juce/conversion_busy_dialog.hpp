// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "editor_session.hpp"
#include "ui_image_io.hpp"
#include "ui_scale.hpp"
#include "mgstc/engine/wave_import.hpp"

namespace mgstc::app {
struct ConversionProgressState {
    // Optional worker-specific hooks. Installed before the worker/UI starts.
    std::function<void()> custom_cancel;
    std::function<bool()> custom_cancelled;
    std::function<double()> custom_progress;
    std::function<juce::String()> custom_stage;
    std::shared_ptr<mgstc::engine::OpllApproximationControl> control{
        std::make_shared<
            mgstc::engine::OpllApproximationControl>()};

    void requestCancellation() const noexcept {
        control->requestCancel();
        if (custom_cancel) custom_cancel();
    }

    [[nodiscard]] bool cancellationRequested() const noexcept {
        return control->cancelRequested()
            || (custom_cancelled && custom_cancelled());
    }

    [[nodiscard]] double progressFraction() const noexcept {
        if (custom_progress) return custom_progress();
        const auto current = control->progress();
        if (current.phase
            == mgstc::engine::OpllApproximationPhase::Completed) {
            return 1.0;
        }
        if (current.total == 0) {
            return 0.0;
        }
        return static_cast<double>(current.completed)
            / static_cast<double>(current.total);
    }
};

class ConversionBusyContent final
    : public juce::Component,
      private juce::Timer {
public:
    explicit ConversionBusyContent(
        std::shared_ptr<ConversionProgressState> state)
        : state_(std::move(state)),
          progress_bar_(displayed_progress_) {
        label_.setText(
            state_->custom_stage ? state_->custom_stage() : juce::String::fromUTF8("変換中…"),
            juce::dontSendNotification);
        label_.setJustificationType(juce::Justification::centred);
        label_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE6EDF3));
        addAndMakeVisible(label_);

        addAndMakeVisible(progress_bar_);
        progress_bar_.setColour(
            juce::ProgressBar::backgroundColourId,
            juce::Colour(0xFF26323C));
        progress_bar_.setColour(
            juce::ProgressBar::foregroundColourId,
            juce::Colour(0xFF53E3A6));

        gif_.loadFromMemory(
            BinaryData::mgstc_spin_gif,
            BinaryData::mgstc_spin_gifSize);
        // Source GIF is 512x512; show at about 50%.
        gif_.setDisplaySize(256, 256);
        addAndMakeVisible(gif_);

        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { requestCancellation(); };
        addAndMakeVisible(cancel_);

        startTimerHz(30);
        setSize(UiScale::sx(320), UiScale::sx(402));
    }

    ~ConversionBusyContent() override {
        stopTimer();
    }

    void requestCancellation() {
        state_->requestCancellation();
        label_.setText(
            juce::String::fromUTF8("キャンセル中…"),
            juce::dontSendNotification);
        cancel_.setEnabled(false);
    }

    void resized() override {
        auto area = getLocalBounds().reduced(16);
        label_.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        progress_bar_.setBounds(area.removeFromTop(22));
        area.removeFromTop(8);
        auto buttons = area.removeFromBottom(34);
        cancel_.setBounds(buttons.withSizeKeepingCentre(120, 34));
        area.removeFromBottom(8);
        gif_.setBounds(
            area.withSizeKeepingCentre(256, 256));
    }

private:
    void timerCallback() override {
        displayed_progress_ = juce::jlimit(
            0.0,
            1.0,
            state_->progressFraction());
        if (state_->custom_stage && !state_->cancellationRequested()) {
            label_.setText(state_->custom_stage(), juce::dontSendNotification);
        }
    }

    std::shared_ptr<ConversionProgressState> state_;
    double displayed_progress_{0.0};
    juce::Label label_;
    juce::ProgressBar progress_bar_;
    AnimatedGifComponent gif_;
    juce::TextButton cancel_;
};

class ConversionBusyDialog final : public juce::DialogWindow, private juce::Timer {
public:
    explicit ConversionBusyDialog(
        std::shared_ptr<ConversionProgressState> state, juce::Component* owner,
        const juce::String& title = juce::String::fromUTF8("変換中"))
        : juce::DialogWindow(
              title,
              juce::Colour(0xFF1B222C),
              false,
              true),
          state_(std::move(state)), owner_(owner), had_owner_(owner != nullptr) {
        if (had_owner_) startTimerHz(30);
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        auto* content = new ConversionBusyContent(state_);
        content_ = content;
        setContentOwned(content, true);
        centreWithSize(content->getWidth(), content->getHeight() + 32);
    }

    ~ConversionBusyDialog() override {
        stopTimer();
        requestCancellation();
    }

    void keepTaskAlive(std::shared_ptr<EditorBackgroundTask> task, std::function<void(bool)> completed) {
        task_ = std::move(task);
        completed_ = std::move(completed);
        startTimerHz(30);
    }

    void closeButtonPressed() override {
        requestCancellation();
    }

private:
    void requestCancellation() {
        state_->requestCancellation();
        if (content_ != nullptr) {
            content_->requestCancellation();
        }
    }

    std::shared_ptr<ConversionProgressState> state_;
    ConversionBusyContent* content_{};
    void timerCallback() override {
        if (had_owner_ && owner_ == nullptr) {
            requestCancellation();
            setVisible(false);
            // Standalone keeps the worker alive until completion. Plugin
            // editors instead destroy this dialog while the registry retains
            // its task; neither editor-close path joins ongoing work.
            exitModalState(0);
            had_owner_ = false;
        }
        if (task_ && task_->completed()) {
            stopTimer();
            const bool cancelled = state_->cancellationRequested();
            auto completed = std::move(completed_);
            exitModalState(1);
            delete this;
            if (completed) completed(cancelled);
            return;
        }
    }
    juce::Component::SafePointer<juce::Component> owner_;
    bool had_owner_{};
    std::shared_ptr<EditorBackgroundTask> task_;
    std::function<void(bool)> completed_;
};



} // namespace mgstc::app
