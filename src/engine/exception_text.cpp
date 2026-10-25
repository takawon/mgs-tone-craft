// SPDX-License-Identifier: AGPL-3.0-only
#include "exception_text.hpp"

#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mgstc::engine::detail {
namespace {

bool validUtf8(std::string_view text) noexcept {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first < 0x80) {
            ++offset;
            continue;
        }
        const std::size_t count = first >= 0xc2 && first <= 0xdf ? 2
            : first >= 0xe0 && first <= 0xef ? 3
            : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (count == 0 || count > text.size() - offset) return false;
        for (std::size_t i = 1; i < count; ++i) {
            const auto next = static_cast<unsigned char>(text[offset + i]);
            if (next < 0x80 || next > 0xbf) return false;
            if (i == 1 && ((first == 0xe0 && next < 0xa0)
                || (first == 0xed && next > 0x9f)
                || (first == 0xf0 && next < 0x90)
                || (first == 0xf4 && next > 0x8f))) return false;
        }
        offset += count;
    }
    return true;
}

std::string escapedBytes(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char byte : text) {
        if (byte >= 0x20 && byte <= 0x7e) result.push_back(static_cast<char>(byte));
        else {
            result += "\\x";
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }
    return result;
}

} // namespace

std::string normaliseExceptionTextUtf8(std::string_view text, unsigned fallback_code_page) {
    if (validUtf8(text)) return std::string(text);
#ifdef _WIN32
    // Win32 conversion lengths are signed integers. Never narrow a larger
    // exception payload or pass an unbounded NUL-terminated input to Win32.
    if (text.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        const auto byte_count = static_cast<int>(text.size());
        const int wide_count = MultiByteToWideChar(fallback_code_page, 0,
            text.data(), byte_count, nullptr, 0);
        if (wide_count > 0) {
            std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
            if (MultiByteToWideChar(fallback_code_page, 0,
                    text.data(), byte_count, wide.data(), wide_count) == wide_count) {
                const int utf8_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                    wide.data(), wide_count, nullptr, 0, nullptr, nullptr);
                if (utf8_count > 0) {
                    std::string utf8(static_cast<std::size_t>(utf8_count), '\0');
                    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            wide.data(), wide_count, utf8.data(), utf8_count, nullptr, nullptr) == utf8_count)
                        return utf8;
                }
            }
        }
    }
#else
    static_cast<void>(fallback_code_page);
#endif
    // Unsupported code pages or platforms still expose the diagnostic bytes,
    // without passing invalid text to a UTF-8 consumer.
    return escapedBytes(text);
}

} // namespace mgstc::engine::detail
