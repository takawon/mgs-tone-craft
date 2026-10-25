// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <string>
#include <string_view>
#include <limits>

#include <juce_core/juce_core.h>

[[nodiscard]] inline std::string utf8String(const juce::String& text) {
    const auto utf8 = text.toUTF8();
    return {
        utf8.getAddress(),
        static_cast<std::size_t>(utf8.sizeInBytes() - 1)};
}

// A broken producer must not send invalid native bytes into text shaping.
[[nodiscard]] inline juce::String statusTextFromUtf8(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || !juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())))
        return "Invalid status text (UTF-8 required)";
    return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
}
