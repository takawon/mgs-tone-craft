// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <juce_graphics/juce_graphics.h>
#include <BinaryData.h>
namespace mgstc::app {
inline const juce::Image& launcherIcon(int index) {
    static const std::array<juce::Image, 3> images{
        juce::ImageFileFormat::loadFrom(BinaryData::composite_transparent_512_png, BinaryData::composite_transparent_512_pngSize),
        juce::ImageFileFormat::loadFrom(BinaryData::opll_transparent_512_png, BinaryData::opll_transparent_512_pngSize),
        juce::ImageFileFormat::loadFrom(BinaryData::scc_transparent_512_png, BinaryData::scc_transparent_512_pngSize)};
    return images[static_cast<std::size_t>(juce::jlimit(0, 2, index))];
}
}
