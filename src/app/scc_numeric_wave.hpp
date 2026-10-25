// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/scc_waveform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgstc::app {

inline constexpr std::array<int, 5> kSccNumericResolutions{16, 32, 64, 128, 256};

[[nodiscard]] inline bool isSccNumericResolution(int resolution) noexcept {
    return std::find(kSccNumericResolutions.begin(), kSccNumericResolutions.end(),
        resolution) != kSccNumericResolutions.end();
}

enum class SccNumericError {
    None,
    Empty,
    InvalidResolution,
    InvalidCharacter,
    OutOfRange,
    TooManyValues,
    UnsupportedFormat,
};

// Editor-session data only. SCC sound and persistent formats remain 32 signed bytes.
// Values are valid when obtained from the parser, fromWaveform, or requantized.
struct SccNumericWave {
    int resolution{256};
    std::vector<int> values;

    [[nodiscard]] engine::SccWaveform waveform() const {
        engine::SccWaveform result{};
        if (values.empty() || !isSccNumericResolution(resolution)) return result;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const auto v = std::clamp(values[i * values.size() / result.size()],
                0, resolution - 1);
            // All operands are nonnegative; integer rounding matches round().
            const int sample = (255 * v + (resolution - 1) / 2)
                / (resolution - 1) - 128;
            result[i] = static_cast<std::int8_t>(sample);
        }
        return result;
    }

    [[nodiscard]] std::string text() const {
        std::string result;
        for (const auto value : values) {
            if (!result.empty()) result += ' ';
            result += std::to_string(value);
        }
        return result;
    }

    [[nodiscard]] std::string sharedText() const {
        return "SCCNUM/1 L=" + std::to_string(resolution) + " " + text();
    }

    [[nodiscard]] SccNumericWave requantized(int new_resolution) const {
        auto result = *this;
        if (!isSccNumericResolution(resolution)
            || !isSccNumericResolution(new_resolution)) return result;
        for (auto& value : result.values) {
            value = (std::clamp(value, 0, resolution - 1) * (new_resolution - 1)
                + (resolution - 1) / 2) / (resolution - 1);
        }
        result.resolution = new_resolution;
        return result;
    }

    [[nodiscard]] static SccNumericWave fromWaveform(
        const engine::SccWaveform& waveform) {
        SccNumericWave result;
        result.values.reserve(waveform.size());
        for (const auto value : waveform) result.values.push_back(value + 128);
        return result;
    }

    bool operator==(const SccNumericWave&) const = default;
};

struct SccNumericParseResult {
    std::optional<SccNumericWave> value;
    SccNumericError error{SccNumericError::None};
    bool shared{};
};

[[nodiscard]] inline SccNumericParseResult parseSccNumericWave(
    std::string_view text, int resolution = 256, bool allow_shared = false) {
    SccNumericParseResult result;
    const auto fail = [&result](SccNumericError error) {
        result.error = error;
        return result;
    };
    const auto trim = [](std::string_view value) {
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
        return value;
    };
    text = trim(text);
    if (text.empty()) return fail(SccNumericError::Empty);

    if (allow_shared && text.starts_with("SCCNUM")) {
        result.shared = true;
        const auto separator = text.find(' ');
        if (separator == std::string_view::npos
            || text.substr(0, separator) != "SCCNUM/1") {
            return fail(SccNumericError::UnsupportedFormat);
        }
        text = trim(text.substr(separator + 1));
        const auto resolution_end = text.find(' ');
        const auto resolution_text = text.substr(0, resolution_end);
        if (!resolution_text.starts_with("L=") || resolution_text.size() <= 2)
            return fail(SccNumericError::InvalidResolution);
        resolution = 0;
        for (const auto digit : resolution_text.substr(2)) {
            if (digit < '0' || digit > '9' || resolution > 256)
                return fail(SccNumericError::InvalidResolution);
            resolution = resolution * 10 + digit - '0';
        }
        if (!isSccNumericResolution(resolution))
            return fail(SccNumericError::InvalidResolution);
        if (resolution_end == std::string_view::npos)
            return fail(SccNumericError::Empty);
        text = trim(text.substr(resolution_end + 1));
        if (text.empty()) return fail(SccNumericError::Empty);
    }
    if (!isSccNumericResolution(resolution))
        return fail(SccNumericError::InvalidResolution);

    SccNumericWave wave{resolution, {}};
    while (!text.empty()) {
        const auto separator = text.find(' ');
        const auto token = text.substr(0, separator);
        if (wave.values.size() == 32) return fail(SccNumericError::TooManyValues);
        int value = 0;
        for (const auto digit : token) {
            if (digit < '0' || digit > '9')
                return fail(SccNumericError::InvalidCharacter);
            if (value > (resolution - 1 - (digit - '0')) / 10)
                return fail(SccNumericError::OutOfRange);
            value = value * 10 + digit - '0';
            if (value >= resolution) return fail(SccNumericError::OutOfRange);
        }
        wave.values.push_back(value);
        if (separator == std::string_view::npos) break;
        text = trim(text.substr(separator + 1));
    }
    result.value = std::move(wave);
    return result;
}

} // namespace mgstc::app
