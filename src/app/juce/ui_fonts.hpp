// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_scale.hpp"

namespace UiFonts {
[[nodiscard]] inline juce::String windowsMessageFaceName() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0)
        != FALSE) {
        return juce::String(metrics.lfMessageFont.lfFaceName);
    }
    return "Segoe UI";
}

[[nodiscard]] inline float unscaledBodyHeight() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    float pixels = 18.0F;
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0)
        != FALSE) {
        pixels = static_cast<float>(
            std::abs(metrics.lfMessageFont.lfHeight));
    }
    // One step above classic JUCE body (~15–16px) / prior MGSTC body (16).
    return juce::jmax(18.0F, pixels);
}

[[nodiscard]] inline float bodyHeight() {
    // Floor so 75% remains usable where possible.
    return juce::jmax(12.0F, unscaledBodyHeight() * UiScale::active_factor);
}

[[nodiscard]] inline float headingHeight() {
    return juce::jmax(13.0F, bodyHeight() * 1.15F);
}

[[nodiscard]] inline float titleHeight() {
    return juce::jmax(
        14.0F,
        juce::jmax(26.0F, unscaledBodyHeight() * 1.45F)
            * UiScale::active_factor);
}

[[nodiscard]] inline float denseHeight() {
    // ~14.6px at body=18; timeline/OPLL EG frames are ≥16px at 100%.
    return juce::jmax(10.0F, bodyHeight() * 0.8125F);
}

// Match LookAndFeel_V4 TextButton: min(heading, height×0.6), never below body.
[[nodiscard]] inline float controlTextHeight(float control_height_px) {
    const auto sized = juce::jmin(
        headingHeight(),
        control_height_px * 0.6F);
    return juce::jmax(bodyHeight(), sized);
}

[[nodiscard]] inline juce::Font make(
    float height_px,
    bool bold = false) {
    return juce::Font(
        juce::FontOptions(
            windowsMessageFaceName(),
            height_px,
            bold ? juce::Font::bold : juce::Font::plain));
}

[[nodiscard]] inline juce::Font title() {
    return make(titleHeight(), true);
}

[[nodiscard]] inline juce::Font heading() {
    return make(headingHeight(), true);
}

[[nodiscard]] inline juce::Font body(bool bold = false) {
    return make(bodyHeight(), bold);
}

[[nodiscard]] inline juce::Font dense(bool bold = false) {
    return make(denseHeight(), bold);
}

[[nodiscard]] inline juce::Font mono() {
    return juce::Font(
        juce::FontOptions(
            juce::Font::getDefaultMonospacedFontName(),
            denseHeight(),
            juce::Font::plain));
}

inline void setMgscPreviewText(
    juce::TextEditor& editor,
    const juce::String& text) {
    const auto font = mono();
    editor.setFont(font);
    editor.setText(text, false);
    editor.applyFontToAllText(font);
}

inline void refreshMgscPreviewFont(juce::TextEditor& editor) {
    const auto font = mono();
    editor.setFont(font);
    editor.applyFontToAllText(font);
}

// Compatibility aliases used across the UI.
[[nodiscard]] inline float height(float relative = 1.0F) {
    return bodyHeight() * relative;
}

[[nodiscard]] inline float parameterHeight() {
    return bodyHeight();
}

[[nodiscard]] inline float windowsMessageHeightPx() {
    return bodyHeight();
}

[[nodiscard]] inline juce::Font ui(
    float relative = 1.0F,
    bool bold = false) {
    if (std::abs(relative - 1.0F) < 0.001F) {
        return body(bold);
    }
    return make(bodyHeight() * relative, bold);
}

[[nodiscard]] inline juce::Font uiBold(float relative = 1.0F) {
    return ui(relative, true);
}

[[nodiscard]] inline juce::Font section() {
    return heading();
}

[[nodiscard]] inline juce::Font parameter(bool bold = false) {
    return body(bold);
}

[[nodiscard]] inline juce::Font compact() {
    return body();
}

inline void styleBodyField(juce::TextEditor& editor) {
    editor.setFont(body());
    editor.applyFontToAllText(body());
}

inline void styleDenseField(juce::TextEditor& editor) {
    editor.setFont(dense());
    editor.applyFontToAllText(dense());
}
} // namespace UiFonts
