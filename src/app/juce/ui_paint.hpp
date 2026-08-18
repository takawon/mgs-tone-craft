// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_layout.hpp"

void clampWindowToDisplayWorkArea(juce::ResizableWindow& window);

void applyScaledContentSize(
    juce::Component& content,
    int preferred_w,
    int preferred_h,
    bool clamp_host_window);

void layoutEditorTopRightChrome(
    int host_width,
    juce::DrawableButton& settings,
    juce::Slider& master_volume,
    juce::DrawableButton* immediate_audition = nullptr);

void paintPageBackground(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds);

void fillRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds);

void strokeRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds);

void paintRoundedPanelFrame(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds);
