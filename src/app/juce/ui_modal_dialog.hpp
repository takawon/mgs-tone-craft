// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "ui_scale.hpp"

// juce::DialogWindow の既定の閉じるボタンは非表示にするだけでモーダルを
// 解除しないため、タイトルバーの✘でも閉じられるようにする。
class ModalDialogWindow final : public juce::DialogWindow {
public:
    ModalDialogWindow(
        const juce::String& name,
        juce::Colour background)
        : juce::DialogWindow(name, background, true, true) {}

    void closeButtonPressed() override {
        exitModalState(0);
    }

    void activeWindowStatusChanged() override {
        juce::DialogWindow::activeWindowStatusChanged();
        if (!isActiveWindow()) {
            return;
        }
        // Keep LookAndFeel / UiFonts / UiLayout on View (global) scale while
        // any non-editor dialog is the active surface.
        UiScale::forceGlobalForNonEditorUi();
        if (auto* content = getContentComponent()) {
            content->sendLookAndFeelChange();
            content->resized();
            content->repaint();
        }
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        juce::DialogWindow::resized();
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        juce::DialogWindow::paint(graphics);
    }
};
