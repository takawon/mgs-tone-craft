// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>

#include <juce_core/juce_core.h>

[[nodiscard]] inline std::string utf8String(const juce::String& text) {
    const auto utf8 = text.toUTF8();
    return {
        utf8.getAddress(),
        static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
}
