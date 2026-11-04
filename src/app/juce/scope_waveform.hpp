// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <juce_gui_basics/juce_gui_basics.h>

namespace mgstc::app {

[[nodiscard]] inline std::uint8_t scopeDisplayNote(std::uint64_t context,
    std::uint8_t selected_note, std::uint8_t ui_note) noexcept {
    return (context & (std::uint64_t{1} << 63)) != 0 ? selected_note : ui_note;
}

[[nodiscard]] inline double scopePeriodSamples(unsigned midi_note, double sample_rate = 48000.0) noexcept {
    return sample_rate / (440.0 * std::pow(2.0, (static_cast<double>(midi_note) - 69.0) / 12.0));
}

template <typename SampleAt>
[[nodiscard]] float scopePeak(std::size_t sample_count, SampleAt sample_at, float floor = 0.0001F) {
    float peak = floor;
    for (std::size_t index = 0; index < sample_count; ++index)
        peak = std::max(peak, std::abs(sample_at(index)));
    return peak;
}

// The caller chooses its window and synchronization policy. Coordinates and
// gain are shared without changing the editor-specific sample interpolation.
template <typename SampleAt>
void buildScopePath(juce::Path& path, juce::Rectangle<float> bounds,
    std::size_t points, SampleAt sample_at, float gain, bool clamp = false) {
    path.clear();
    if (points < 2 || bounds.isEmpty()) return;
    path.preallocateSpace(static_cast<int>(points * 3 + 4));
    const float step = bounds.getWidth() / static_cast<float>(points - 1);
    for (std::size_t index = 0; index < points; ++index) {
        float value = sample_at(index) * gain;
        if (clamp) value = std::clamp(value, -1.0F, 1.0F);
        const float x = bounds.getX() + static_cast<float>(index) * step;
        const float y = bounds.getCentreY() - value * bounds.getHeight() * 0.5F;
        if (index == 0) path.startNewSubPath(x, y);
        else path.lineTo(x, y);
    }
}

} // namespace mgstc::app
