// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_scale.hpp"

namespace UiFonts {
namespace detail {
struct MetricsCache {
    juce::String face{"Segoe UI"};
    float unscaled_body{18.0F};
    int scale_percent{-1};
    juce::Font body;
    juce::Font body_bold;
    juce::Font dense;
    juce::Font dense_bold;
    juce::Font heading;
    juce::Font title;
    juce::Font mono;
};

inline MetricsCache& metricsCache() {
    static MetricsCache cache;
    return cache;
}

inline void readSystemMetrics(MetricsCache& cache) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS,
            sizeof(metrics),
            &metrics,
            0)
        != FALSE) {
        cache.face = juce::String(metrics.lfMessageFont.lfFaceName);
        cache.unscaled_body = juce::jmax(
            18.0F,
            static_cast<float>(
                std::abs(metrics.lfMessageFont.lfHeight)));
    }
}

inline juce::Font makeNamed(float height_px, bool bold) {
    auto& cache = metricsCache();
    return juce::Font(
        juce::FontOptions(
            cache.face,
            height_px,
            bold ? juce::Font::bold : juce::Font::plain));
}

inline void refreshMetricsIfNeeded() {
    auto& cache = metricsCache();
    if (cache.scale_percent == UiScale::active_percent
        && cache.face.isNotEmpty()) {
        return;
    }
    readSystemMetrics(cache);
    cache.scale_percent = UiScale::active_percent;
    const float body_h = juce::jmax(
        12.0F, cache.unscaled_body * UiScale::active_factor);
    const float dense_h = juce::jmax(10.0F, body_h * 0.8125F);
    const float heading_h = juce::jmax(13.0F, body_h * 1.15F);
    const float title_h = juce::jmax(
        14.0F,
        juce::jmax(26.0F, cache.unscaled_body * 1.45F)
            * UiScale::active_factor);
    cache.body = makeNamed(body_h, false);
    cache.body_bold = makeNamed(body_h, true);
    cache.dense = makeNamed(dense_h, false);
    cache.dense_bold = makeNamed(dense_h, true);
    cache.heading = makeNamed(heading_h, true);
    cache.title = makeNamed(title_h, true);
    cache.mono = juce::Font(
        juce::FontOptions(
            juce::Font::getDefaultMonospacedFontName(),
            dense_h,
            juce::Font::plain));
}
}  // namespace detail

[[nodiscard]] inline juce::String windowsMessageFaceName() {
    detail::refreshMetricsIfNeeded();
    return detail::metricsCache().face;
}

[[nodiscard]] inline float unscaledBodyHeight() {
    detail::refreshMetricsIfNeeded();
    return detail::metricsCache().unscaled_body;
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
    detail::refreshMetricsIfNeeded();
    return detail::makeNamed(height_px, bold);
}

[[nodiscard]] inline juce::Font title() {
    detail::refreshMetricsIfNeeded();
    return detail::metricsCache().title;
}

[[nodiscard]] inline juce::Font heading() {
    detail::refreshMetricsIfNeeded();
    return detail::metricsCache().heading;
}

[[nodiscard]] inline juce::Font body(bool bold = false) {
    detail::refreshMetricsIfNeeded();
    return bold
        ? detail::metricsCache().body_bold
        : detail::metricsCache().body;
}

[[nodiscard]] inline juce::Font dense(bool bold = false) {
    detail::refreshMetricsIfNeeded();
    return bold
        ? detail::metricsCache().dense_bold
        : detail::metricsCache().dense;
}

[[nodiscard]] inline juce::Font mono() {
    detail::refreshMetricsIfNeeded();
    return detail::metricsCache().mono;
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
