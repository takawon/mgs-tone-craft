// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include <juce_gui_basics/juce_gui_basics.h>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "ui_chrome_constants.hpp"

namespace mgstc::app {

inline void paintToneMiniWave(
    juce::Graphics& graphics,
    juce::Rectangle<float> graph,
    const mgstc::engine::SccWaveform& waveform) {
    if (graph.isEmpty() || waveform.size() < 2) {
        return;
    }
    juce::Path path;
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        const auto x = juce::jmap(
            static_cast<float>(index),
            0.0F,
            static_cast<float>(waveform.size() - 1),
            graph.getX(),
            graph.getRight());
        const auto y = juce::jmap(
            static_cast<float>(waveform[index]),
            -128.0F,
            127.0F,
            graph.getBottom(),
            graph.getY());
        if (index == 0) {
            path.startNewSubPath(x, y);
        } else {
            path.lineTo(x, y);
        }
    }
    graphics.setColour(juce::Colour(kUiHoverAccent));
    graphics.strokePath(
        path,
        juce::PathStrokeType(
            1.2F,
            juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded));
}

inline void paintToneMiniLevels(
    juce::Graphics& graphics,
    juce::Rectangle<float> graph,
    const std::array<float, mgstc::engine::OpllEnvelopeTrace::kPointCount>&
        levels,
    juce::Colour colour) {
    if (graph.isEmpty() || levels.size() < 2) {
        return;
    }
    juce::Path path;
    for (std::size_t index = 0; index < levels.size(); ++index) {
        const float x = graph.getX()
            + graph.getWidth() * static_cast<float>(index)
                / static_cast<float>(levels.size() - 1);
        const float y = graph.getBottom()
            - graph.getHeight() * std::clamp(levels[index], 0.0F, 1.0F);
        if (index == 0) {
            path.startNewSubPath(x, y);
        } else {
            path.lineTo(x, y);
        }
    }
    graphics.setColour(colour);
    graphics.strokePath(path, juce::PathStrokeType(1.2F));
}

inline void paintToneMiniEnvelope(
    juce::Graphics& graphics,
    juce::Rectangle<float> graph,
    const mgstc::engine::CompositeTimbre& timbre) {
    if (graph.isEmpty() || timbre.layers.empty()) {
        return;
    }
    const auto& events = timbre.layers.front().volume_envelope.events;
    if (events.empty()) {
        return;
    }
    const auto length = std::max<std::uint32_t>(
        1, timbre.layers.front().envelope_timeline.length_counts);
    juce::Path path;
    bool started = false;
    for (const auto& event : events) {
        if (event.kind != mgstc::engine::EnvelopeEventKind::Volume) {
            continue;
        }
        const float x = graph.getX()
            + graph.getWidth() * static_cast<float>(event.count)
                / static_cast<float>(length);
        const float y = juce::jmap(
            static_cast<float>(event.value),
            0.0F,
            15.0F,
            graph.getBottom(),
            graph.getY());
        if (!started) {
            path.startNewSubPath(x, y);
            started = true;
        } else {
            path.lineTo(x, y);
        }
    }
    if (!started) {
        return;
    }
    graphics.setColour(juce::Colour(kUiHoverAccent));
    graphics.strokePath(path, juce::PathStrokeType(1.2F));
}

}  // namespace mgstc::app
