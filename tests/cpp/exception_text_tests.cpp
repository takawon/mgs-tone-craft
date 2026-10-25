// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/engine/exception_text.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#if MGSTC_HAVE_JUCE_UTF8_TESTS
#include "../../src/app/juce/juce_utf8.hpp"
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        using mgstc::engine::detail::normaliseExceptionTextUtf8;
        const std::string japanese = "\xe3\x81\x82";
        require(normaliseExceptionTextUtf8("").empty(), "Empty error changed");
        require(normaliseExceptionTextUtf8("RPC failed") == "RPC failed", "ASCII changed");
        require(normaliseExceptionTextUtf8(japanese, 999999) == japanese, "Valid UTF-8 changed");
        const std::string emoji = "\xf0\x9f\x8e\xb8";
        require(normaliseExceptionTextUtf8(emoji, 932) == emoji, "Supplementary UTF-8 changed");
        std::string raw, expected;
        for (int i = 0; i < 16; ++i) { raw += "\x82\xa0"; expected += japanese; }
        const auto normalized = normaliseExceptionTextUtf8(raw, 932);
#ifdef _WIN32
        require(normalized == expected, "CP932 did not become exact UTF-8");
#else
        require(normalized.find("\\x82\\xA0") == 0, "Invalid native bytes were not escaped");
#endif
        require(normaliseExceptionTextUtf8(raw, 999999).find("\\x82\\xA0") == 0,
            "Unsupported code page leaked invalid bytes");

        // Captured connection-refused text from Japanese Windows. This is a
        // protocol diagnostic, without user data or an external server.
        const std::string captured = "RPC connect failed: "
            "\x91\xce\x8f\xdb\x82\xcc\x83\x52\x83\x93\x83\x73\x83\x85\x81\x5b\x83\x5e\x81\x5b"
            "\x82\xc9\x82\xe6\x82\xc1\x82\xc4\x8b\x91\x94\xdb\x82\xb3\x82\xea\x82\xbd\x82\xbd"
            "\x82\xdf\x81\x41\x90\xda\x91\xb1\x82\xc5\x82\xab\x82\xdc\x82\xb9\x82\xf1\x82\xc5"
            "\x82\xb5\x82\xbd\x81\x42";
        const auto captured_utf8 = normaliseExceptionTextUtf8(captured, 932);
#ifdef _WIN32
        require(captured_utf8.find("RPC connect failed: \xe5\xaf\xbe\xe8\xb1\xa1") == 0,
            "Captured native message lost its Japanese prefix");
#endif
#if MGSTC_HAVE_JUCE_UTF8_TESTS
        require(!juce::CharPointer_UTF8::isValidString(raw.data(), static_cast<int>(raw.size())),
            "Regression fixture must be invalid UTF-8");
        auto cursor = juce::CharPointer_UTF8(raw.c_str());
        const auto fast_length = cursor.length();
        int decoded = 0;
        while (!cursor.isEmpty()) { static_cast<void>(cursor.getAndAdvance()); ++decoded; }
        std::cout << "Invalid raw bytes: fast length=" << fast_length << ", decoded=" << decoded << '\n';
        // Measure the historical count mismatch without performing its unsafe
        // UTF-32 write. Only normalized/rejected text reaches String conversion.
        require(statusTextFromUtf8(raw) == "Invalid status text (UTF-8 required)",
            "UI accepted invalid native status bytes");
        for (const auto& text : {normalized, captured_utf8, japanese, emoji}) {
            require(juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())),
                "Normalizer produced invalid UTF-8");
            const auto safe = statusTextFromUtf8(text);
            const auto* utf32 = safe.toUTF32().getAddress();
            const auto roundtrip = juce::String(juce::CharPointer_UTF32(utf32));
            require(utf8String(roundtrip) == text, "UTF-32 shaping input changed text");
        }
#endif
        std::cout << "Exception encoding and safe status conversion passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
