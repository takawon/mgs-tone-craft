// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/scc_waveform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgstc::app {

inline constexpr std::array<int, 5> kSccNumericResolutions{16, 32, 64, 128, 256};
// Header and body have separate bounded budgets. Reserve three bytes per
// numeric literal so every accepted expression survives resolution changes.
inline constexpr std::size_t kSccNumericMaxInputBytes = 4096;
inline constexpr std::size_t kSccNumericMaxDepth = 16;

[[nodiscard]] inline bool isSccNumericResolution(int resolution) noexcept {
    return std::find(kSccNumericResolutions.begin(), kSccNumericResolutions.end(),
        resolution) != kSccNumericResolutions.end();
}

enum class SccNumericError {
    None, Empty, InvalidResolution, InvalidCharacter, OutOfRange, TooManyValues,
    UnsupportedFormat, InvalidSyntax, TooDeep, InputTooLong,
};

enum class SccNumericCurve { Hold, Linear, SlowFast, FastSlow, SlowEnds, FastEnds };

struct SccNumericSpan {
    std::size_t offset{};
    std::size_t length{};
    bool operator==(const SccNumericSpan&) const = default;
};

struct SccNumericItem {
    bool group{};
    std::size_t index{};
    bool operator==(const SccNumericItem&) const = default;
};

struct SccNumericGroup {
    std::vector<SccNumericItem> items;
    bool operator==(const SccNumericGroup&) const = default;
};

// Session-only editor data. Group membership controls time; incoming connections
// link successive literals independently of the brackets around them.
struct SccNumericWave {
    int resolution{256};
    std::vector<int> values;
    std::string expression;
    std::vector<SccNumericGroup> groups;
    std::vector<SccNumericCurve> incoming;
    std::vector<SccNumericSpan> literal_spans;
    std::optional<SccNumericSpan> header_span;
    std::size_t body_offset{};

    [[nodiscard]] static double curveValue(SccNumericCurve curve, double t) {
        constexpr double pi = std::numbers::pi_v<double>;
        switch (curve) {
        case SccNumericCurve::Linear: return t;
        case SccNumericCurve::SlowFast: return 1.0 - std::cos(pi * t / 2.0);
        case SccNumericCurve::FastSlow: return std::sin(pi * t / 2.0);
        case SccNumericCurve::SlowEnds: return (1.0 - std::cos(pi * t)) / 2.0;
        case SccNumericCurve::FastEnds:
            return t <= 0.5 ? std::sin(pi * t) / 2.0
                : 1.0 - std::sin(pi * (1.0 - t)) / 2.0;
        case SccNumericCurve::Hold: return 0.0;
        }
        return 0.0;
    }

    [[nodiscard]] engine::SccWaveform waveform() const {
        engine::SccWaveform result{};
        if (values.empty() || !isSccNumericResolution(resolution)) return result;
        const auto signedSample = [this](int value) {
            value = std::clamp(value, 0, resolution - 1);
            return static_cast<std::int8_t>(
                (255 * value + (resolution - 1) / 2) / (resolution - 1) - 128);
        };
        if (groups.empty()) {
            for (std::size_t i = 0; i < result.size(); ++i)
                result[i] = signedSample(values[i * values.size() / result.size()]);
            return result;
        }
        const auto boundary = [](int start, int width, std::size_t j, std::size_t count) {
            return start + static_cast<int>((j * static_cast<std::size_t>(width) + count - 1) / count);
        };
        const auto renderLiteral = [&](std::size_t literal, int start, int width, bool include_end) {
            const auto curve = incoming[literal];
            const int a = curve == SccNumericCurve::Hold ? values[literal] : values[literal - 1];
            const int b = values[literal];
            for (int i = 0; i < width; ++i) {
                const double t = width > 1
                    ? static_cast<double>(i) / (include_end ? width - 1 : width) : 0.0;
                const int value = static_cast<int>(std::lround(
                    a + (b - a) * curveValue(curve, t)));
                result[static_cast<std::size_t>(start + i)] = signedSample(value);
            }
        };
        struct SampleInterval { std::size_t literal; int start; int width; };
        std::vector<SampleInterval> intervals;
        intervals.reserve(values.size());
        const auto renderGroup = [&](const auto& self, std::size_t group_index,
                                     int start, int width) -> void {
            const auto& items = groups[group_index].items;
            struct Unit { std::size_t first; std::size_t end; };
            std::vector<Unit> units;
            units.reserve(items.size());
            for (std::size_t i = 0; i < items.size();) {
                const auto first = i++;
                if (!items[first].group) {
                    while (i < items.size() && !items[i].group
                        && incoming[items[i].index] != SccNumericCurve::Hold) ++i;
                }
                units.push_back({first, i});
            }
            for (std::size_t u = 0; u < units.size(); ++u) {
                const int left = boundary(start, width, u, units.size());
                const int right = boundary(start, width, u + 1, units.size());
                const auto unit = units[u];
                if (items[unit.first].group) {
                    self(self, items[unit.first].index, left, right - left);
                    continue;
                }
                auto first = unit.first;
                // A chain's leading value is its starting point, except when an
                // incoming connection gives that value its own preceding segment.
                if (unit.end - first > 1
                    && incoming[items[first].index] == SccNumericCurve::Hold) ++first;
                const auto segments = unit.end - first;
                for (std::size_t segment = 0; segment < segments; ++segment) {
                    const int segment_left = boundary(left, right - left, segment, segments);
                    const int segment_right = boundary(left, right - left, segment + 1, segments);
                    intervals.push_back({items[first + segment].index,
                        segment_left, segment_right - segment_left});
                }
            }
        };
        renderGroup(renderGroup, 0, 0, 32);
        for (std::size_t first = 0; first < intervals.size();) {
            const auto& initial = intervals[first];
            if (incoming[initial.literal] == SccNumericCurve::Hold) {
                renderLiteral(initial.literal, initial.start, initial.width, true);
                ++first;
                continue;
            }
            auto end = first + 1;
            while (end < intervals.size()
                && incoming[intervals[end].literal] != SccNumericCurve::Hold
                && intervals[end].literal == intervals[end - 1].literal + 1
                && intervals[end].start == intervals[end - 1].start + intervals[end - 1].width)
                ++end;
            // Literal identity, rather than equal endpoint values, determines
            // connectivity. Holds (even zero-width ones) and independent groups
            // break the run; explicit connections may cross group boundaries.
            const bool closed = values[initial.literal - 1] == values[intervals[end - 1].literal];
            for (auto segment = first; segment < end; ++segment) {
                const auto& interval = intervals[segment];
                renderLiteral(interval.literal, interval.start, interval.width,
                    !closed && segment + 1 == end);
            }
            first = end;
        }
        return result;
    }

    [[nodiscard]] std::string text() const {
        if (!expression.empty()) return expression;
        std::string result;
        for (const auto value : values) {
            if (!result.empty()) result += ' ';
            result += std::to_string(value);
        }
        return result;
    }

    [[nodiscard]] std::string sharedText() const {
        const auto body = header_span ? expression.substr(body_offset) : text();
        return "SCCWAV L=" + std::to_string(resolution) + " " + body;
    }

    [[nodiscard]] SccNumericWave requantized(int new_resolution) const;

    [[nodiscard]] static SccNumericWave fromWaveform(const engine::SccWaveform& waveform) {
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
    bool incomplete{};
    std::optional<int> header_resolution;
    std::size_t header_resolution_offset{};
    std::size_t header_resolution_length{};
};

[[nodiscard]] inline SccNumericParseResult parseSccNumericWave(
    std::string_view text, int resolution = 256, bool allow_shared = false) {
    SccNumericParseResult result;
    const auto fail = [&result](SccNumericError error) {
        result.error = error;
        return result;
    };
    if (text.size() > 2 * kSccNumericMaxInputBytes) return fail(SccNumericError::InputTooLong);
    std::size_t position = 0;
    const auto skipSpaces = [&] { while (position < text.size() && text[position] == ' ') ++position; };
    skipSpaces();
    if (position == text.size()) return fail(SccNumericError::Empty);
    SccNumericWave wave;
    wave.expression = std::string(text);
    if (allow_shared && text.substr(position).starts_with("SCC")) {
        result.shared = true;
        const auto word_end = text.find(' ', position);
        if (word_end == std::string_view::npos || text.substr(position, word_end - position) != "SCCWAV")
            return fail(SccNumericError::UnsupportedFormat);
        position = word_end;
        skipSpaces();
        if (!text.substr(position).starts_with("L=")) return fail(SccNumericError::InvalidResolution);
        position += 2;
        const auto digit_start = position;
        resolution = 0;
        while (position < text.size() && text[position] != ' ') {
            const auto digit = text[position++];
            if (digit < '0' || digit > '9' || resolution > 256)
                return fail(SccNumericError::InvalidResolution);
            resolution = resolution * 10 + digit - '0';
        }
        if (position == digit_start || !isSccNumericResolution(resolution))
            return fail(SccNumericError::InvalidResolution);
        result.header_resolution = resolution;
        result.header_resolution_offset = digit_start;
        result.header_resolution_length = position - digit_start;
        wave.header_span = SccNumericSpan{digit_start, position - digit_start};
        wave.body_offset = position < text.size() ? position + 1 : position;
        if (wave.body_offset > kSccNumericMaxInputBytes
            || wave.body_offset - wave.header_span->length + 3 > kSccNumericMaxInputBytes
            || text.size() - wave.body_offset > kSccNumericMaxInputBytes)
            return fail(SccNumericError::InputTooLong);
        skipSpaces();
        if (position == text.size()) {
            result.incomplete = true;
            return result;
        }
    }
    if (!isSccNumericResolution(resolution)) return fail(SccNumericError::InvalidResolution);
    const auto body_size = text.size() - wave.body_offset;
    if (body_size > kSccNumericMaxInputBytes) return fail(SccNumericError::InputTooLong);
    wave.resolution = resolution;

    enum class TokenKind { Number, Operator, LeftParen, RightParen };
    struct Token { TokenKind kind; std::size_t literal{}; SccNumericCurve curve{}; };
    std::vector<Token> tokens;
    while (position < text.size()) {
        skipSpaces();
        if (position == text.size()) break;
        const auto c = text[position];
        if (c >= '0' && c <= '9') {
            if (wave.values.size() == 32) return fail(SccNumericError::TooManyValues);
            const auto start = position;
            int value = 0;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
                const int digit = text[position++] - '0';
                if (value > (resolution - 1 - digit) / 10) return fail(SccNumericError::OutOfRange);
                value = value * 10 + digit;
                if (value >= resolution) return fail(SccNumericError::OutOfRange);
            }
            tokens.push_back({TokenKind::Number, wave.values.size(), {}});
            wave.values.push_back(value);
            wave.literal_spans.push_back({start, position - start});
        } else if (c == '(' || c == ')') {
            tokens.push_back({c == '(' ? TokenKind::LeftParen : TokenKind::RightParen, 0, {}});
            ++position;
        } else {
            SccNumericCurve curve;
            if (text.substr(position).starts_with("><")) { curve = SccNumericCurve::SlowEnds; position += 2; }
            else if (text.substr(position).starts_with("<>")) { curve = SccNumericCurve::FastEnds; position += 2; }
            else if (c == '-') { curve = SccNumericCurve::Linear; ++position; }
            else if (c == '>') { curve = SccNumericCurve::SlowFast; ++position; }
            else if (c == '<') { curve = SccNumericCurve::FastSlow; ++position; }
            else if (text.substr(position).starts_with("\xE2\x80\x90")) {
                curve = SccNumericCurve::Linear; position += 3;
            } else return fail(SccNumericError::InvalidCharacter);
            tokens.push_back({TokenKind::Operator, 0, curve});
        }
    }
    if (wave.values.empty()) return fail(SccNumericError::InvalidSyntax);
    auto body_budget = body_size;
    for (const auto span : wave.literal_spans) body_budget -= span.length;
    body_budget += 3 * wave.values.size();
    if (body_budget > kSccNumericMaxInputBytes) return fail(SccNumericError::InputTooLong);
    wave.incoming.resize(wave.values.size(), SccNumericCurve::Hold);
    wave.groups.emplace_back();
    std::size_t token_index = 0;
    std::size_t numbers_seen = 0;
    SccNumericCurve pending = SccNumericCurve::Hold;
    SccNumericError syntax_error = SccNumericError::None;
    const auto parseGroup = [&](const auto& self, std::size_t group, std::size_t depth) -> bool {
        while (token_index < tokens.size()) {
            const auto token = tokens[token_index++];
            if (token.kind == TokenKind::Number) {
                wave.groups[group].items.push_back({false, token.literal});
                wave.incoming[token.literal] = pending;
                pending = SccNumericCurve::Hold;
                ++numbers_seen;
            } else if (token.kind == TokenKind::Operator) {
                if (numbers_seen == 0 || pending != SccNumericCurve::Hold) return false;
                pending = token.curve;
            } else if (token.kind == TokenKind::LeftParen) {
                if (depth == kSccNumericMaxDepth) { syntax_error = SccNumericError::TooDeep; return false; }
                const auto child = wave.groups.size();
                wave.groups.emplace_back();
                wave.groups[group].items.push_back({true, child});
                if (!self(self, child, depth + 1)) return false;
            } else {
                return depth > 0 && !wave.groups[group].items.empty();
            }
        }
        return depth == 0 && !wave.groups[group].items.empty();
    };
    if (!parseGroup(parseGroup, 0, 0) || token_index != tokens.size()
        || pending != SccNumericCurve::Hold)
        return fail(syntax_error == SccNumericError::None ? SccNumericError::InvalidSyntax : syntax_error);
    result.value = std::move(wave);
    return result;
}

inline SccNumericWave SccNumericWave::requantized(int new_resolution) const {
    if (!isSccNumericResolution(resolution) || !isSccNumericResolution(new_resolution)) return *this;
    const auto converted = [&](int value) {
        return (std::clamp(value, 0, resolution - 1) * (new_resolution - 1)
            + (resolution - 1) / 2) / (resolution - 1);
    };
    if (expression.empty()) {
        auto result = *this;
        for (auto& value : result.values) value = converted(value);
        result.resolution = new_resolution;
        return result;
    }
    std::string rewritten;
    std::size_t copied = 0;
    const auto replaceSpan = [&](SccNumericSpan span, int value) {
        rewritten.append(expression, copied, span.offset - copied);
        rewritten += std::to_string(value);
        copied = span.offset + span.length;
    };
    if (header_span) replaceSpan(*header_span, new_resolution);
    for (std::size_t i = 0; i < literal_spans.size(); ++i)
        replaceSpan(literal_spans[i], converted(values[i]));
    rewritten.append(expression, copied, expression.size() - copied);
    const auto parsed = parseSccNumericWave(rewritten, new_resolution, true);
    return parsed.value ? *parsed.value : *this;
}

} // namespace mgstc::app
