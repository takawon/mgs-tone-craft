#include "mgstc/engine/mgs_envelope_io.hpp"

#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/opll_register_auto.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace mgstc::engine {
namespace {

struct CountState {
    struct VolumeSpec {
        std::int32_t value{};
        bool automatic{};
        bool precise{};
        std::optional<std::uint32_t> imported_ramp_start;
    };

    std::vector<std::string> before_timbre_tokens;
    std::vector<std::string> before_pitch_tokens;
    std::vector<std::string> after_timbre_tokens;
    std::vector<std::string> after_pitch_tokens;
    std::optional<VolumeSpec> volume;

    [[nodiscard]] bool boundary() const noexcept {
        return !before_timbre_tokens.empty()
            || !before_pitch_tokens.empty()
            || !after_timbre_tokens.empty()
            || !after_pitch_tokens.empty()
            || volume.has_value();
    }
};

void addIssueOnce(
    MgsEnvelopeFormatResult& result,
    MgsEnvelopeIssue issue) {
    if (!result.hasIssue(issue)) {
        result.issues.push_back(issue);
    }
}

[[nodiscard]] char volumeCharacter(std::int32_t value) noexcept {
    constexpr std::string_view digits = "0123456789abcdef";
    return digits[static_cast<std::size_t>(value)];
}

[[nodiscard]] std::int32_t interpolatedVolume(
    std::int32_t start_value,
    std::uint32_t start_count,
    std::int32_t end_value,
    std::uint32_t end_count,
    std::uint32_t at) noexcept {
    if (at <= start_count) {
        return start_value;
    }
    if (at >= end_count) {
        return end_value;
    }
    const auto span = static_cast<std::int32_t>(end_count - start_count);
    const auto num = start_value * static_cast<std::int32_t>(end_count - at)
        + end_value * static_cast<std::int32_t>(at - start_count);
    if (num >= 0) {
        return (num + span / 2) / span;
    }
    return (num - span / 2) / span;
}

void appendToken(std::vector<std::string>& tokens, std::string token) {
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
}

void appendHold(
    std::vector<std::string>& tokens,
    std::int32_t volume,
    std::uint32_t count) {
    while (count != 0) {
        const auto chunk = std::min<std::uint32_t>(239, count);
        std::string token(1, volumeCharacter(volume));
        if (chunk != 1) {
            token += ':' + std::to_string(chunk);
        }
        appendToken(tokens, std::move(token));
        count -= chunk;
    }
}

template <typename AppendZeroTime>
void emitPreciseAutomaticTokens(
    std::vector<std::string>& tokens,
    const std::vector<std::uint8_t>& vols,
    std::uint32_t origin_count,
    const std::vector<std::uint32_t>& cuts,
    AppendZeroTime&& append_zero_time) {
    if (vols.empty()) {
        return;
    }
    const auto is_cut = [&cuts](std::uint32_t count) {
        return std::find(cuts.begin(), cuts.end(), count) != cuts.end();
    };
    std::int32_t run_volume = static_cast<std::int32_t>(vols.front());
    std::uint32_t run_length = 0;
    const auto flush = [&]() {
        if (run_length != 0) {
            appendHold(tokens, run_volume, run_length);
            run_length = 0;
        }
    };
    for (std::uint32_t i = 0; i < vols.size(); ++i) {
        const auto count = origin_count + i;
        if (i > 0 && is_cut(count)) {
            flush();
            append_zero_time(count);
        }
        const auto volume = static_cast<std::int32_t>(vols[i]);
        if (run_length == 0) {
            run_volume = volume;
            run_length = 1;
        } else if (volume == run_volume) {
            ++run_length;
        } else {
            flush();
            run_volume = volume;
            run_length = 1;
        }
    }
    flush();
}

[[nodiscard]] std::string joinTokens(
    const std::vector<std::string>& tokens) {
    std::ostringstream output;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (index != 0) {
            output << '.';
        }
        output << tokens[index];
    }
    return output.str();
}

struct ParsedHold {
    std::int32_t volume{};
    std::uint32_t count{1};
};

[[nodiscard]] std::optional<ParsedHold> parseHoldToken(
    std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(token.front());
    if (!std::isxdigit(first)) {
        return std::nullopt;
    }
    const int volume = std::isdigit(first)
        ? token.front() - '0'
        : std::tolower(first) - 'a' + 10;
    if (volume < 0 || volume > 15) {
        return std::nullopt;
    }
    if (token.size() == 1) {
        return ParsedHold{volume, 1};
    }
    if (token[1] != ':') {
        return std::nullopt;
    }
    if (token.size() < 3) {
        return std::nullopt;
    }
    int parsed{};
    const auto result = std::from_chars(
        token.data() + 2, token.data() + token.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != token.data() + token.size()
        || parsed < 1) {
        return std::nullopt;
    }
    return ParsedHold{volume, static_cast<std::uint32_t>(parsed)};
}

void coalesceAdjacentHolds(std::vector<std::string>& tokens) {
    std::vector<std::string> merged;
    merged.reserve(tokens.size());
    std::optional<std::int32_t> run_volume;
    std::uint32_t run_count = 0;
    const auto flush = [&]() {
        if (run_volume) {
            appendHold(merged, *run_volume, run_count);
            run_volume.reset();
            run_count = 0;
        }
    };
    for (const auto& token : tokens) {
        if (const auto hold = parseHoldToken(token)) {
            if (run_volume && *run_volume == hold->volume) {
                run_count += hold->count;
            } else {
                flush();
                run_volume = hold->volume;
                run_count = hold->count;
            }
        } else {
            flush();
            merged.push_back(token);
        }
    }
    flush();
    tokens = std::move(merged);
}

void collapseTrailingHold(std::vector<std::string>& tokens) {
    if (tokens.empty() || tokens.back() == "]") {
        return;
    }
    std::optional<std::int32_t> volume;
    std::size_t begin = tokens.size();
    while (begin > 0) {
        const auto hold = parseHoldToken(tokens[begin - 1]);
        if (!hold) {
            break;
        }
        if (volume && *volume != hold->volume) {
            break;
        }
        volume = hold->volume;
        --begin;
    }
    if (!volume || begin == tokens.size()) {
        return;
    }
    tokens.resize(begin);
    appendToken(tokens, std::string(1, volumeCharacter(*volume)));
}

[[nodiscard]] std::size_t tokenCompiledBytes(std::string_view token) {
    if (token.empty()) {
        return 0;
    }
    if (token == "[" || token == "]") {
        return 1;
    }
    const auto first = token.front();
    if (first == 'y' || first == 'Y') {
        return 3;
    }
    if (first == '@' || first == '\\') {
        return 2;
    }
    if (first == 'n' || first == 'N'
        || first == '/' || first == '*') {
        return 1;
    }
    if (token.find('=') != std::string_view::npos
        || token.find(':') != std::string_view::npos) {
        return 2;
    }
    return 1;
}

[[nodiscard]] std::size_t compiledEnvelopeBytes(
    const std::vector<std::string>& tokens) {
    std::size_t total = 0;
    for (const auto& token : tokens) {
        total += tokenCompiledBytes(token);
    }
    return total;
}

[[nodiscard]] std::optional<std::uint32_t> tokenDuration(
    std::string_view token) {
    if (const auto hold = parseHoldToken(token)) {
        return hold->count;
    }
    const auto separator = token.find('=');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    std::uint32_t count{};
    const auto parsed = std::from_chars(
        token.data() + separator + 1, token.data() + token.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return count;
}

// Keep complete same-tick command groups from the canonical stream. A ramp
// remains intact: shortening the author model would change its interpolation.
// Over-budget loops become a finite traversal, never an unmatched or shortened
// repeating body. The runtime then applies MGSDRV's ordinary terminal hold.
[[nodiscard]] bool retainEnvelopePrefix(
    std::vector<std::string>& tokens,
    std::size_t compiled_byte_limit,
    std::uint32_t& cutoff_count) {
    std::vector<std::string> prefix;
    std::vector<std::string> pending;
    std::size_t prefix_bytes{};
    std::size_t pending_bytes{};
    std::uint32_t count{};
    bool consumed_time = false;
    const auto commit = [&]() {
        if (prefix_bytes + pending_bytes > compiled_byte_limit) {
            return false;
        }
        for (auto& token : pending) {
            prefix.push_back(std::move(token));
        }
        prefix_bytes += pending_bytes;
        pending.clear();
        pending_bytes = 0;
        return true;
    };
    for (const auto& token : tokens) {
        if (token == "[" || token == "]") {
            continue;
        }
        pending.push_back(token);
        pending_bytes += tokenCompiledBytes(token);
        if (const auto duration = tokenDuration(token)) {
            if (!commit()) {
                cutoff_count = count;
                if (consumed_time) {
                    tokens = std::move(prefix);
                }
                return consumed_time;
            }
            consumed_time = true;
            count += *duration;
        }
    }
    // Final zero-time commands also form one indivisible group. If only the
    // loop markers exceeded the budget, the finite traversal ends here.
    static_cast<void>(commit());
    cutoff_count = count;
    if (consumed_time) {
        tokens = std::move(prefix);
    }
    return consumed_time;
}

[[nodiscard]] std::vector<std::uint8_t> compileEnvelopeTokens(
    const std::vector<std::string>& tokens) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(compiledEnvelopeBytes(tokens));
    const auto number = [](std::string_view text) {
        int value{};
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    };
    for (const auto& token : tokens) {
        if (token == "[") {
            bytes.push_back(0x40);
        } else if (token == "]") {
            bytes.push_back(0x60);
        } else if (token.front() == '@') {
            bytes.insert(bytes.end(), {0x10,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1)))});
        } else if (token.front() == '\\') {
            bytes.insert(bytes.end(), {0x12,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1)))});
        } else if (token.front() == 'y') {
            const auto comma = token.find(',');
            bytes.insert(bytes.end(), {0x11,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1, comma - 1))),
                static_cast<std::uint8_t>(number(std::string_view(token).substr(comma + 1)))});
        } else {
            const auto value = static_cast<std::uint8_t>(
                std::isdigit(static_cast<unsigned char>(token.front()))
                    ? token.front() - '0' : token.front() - 'a' + 10);
            const auto separator = token.find_first_of(":=");
            if (separator == std::string::npos) {
                bytes.push_back(value);
            } else {
                bytes.push_back(static_cast<std::uint8_t>(
                    (token[separator] == ':' ? 0xE0 : 0x20) | value));
                bytes.push_back(static_cast<std::uint8_t>(
                    number(std::string_view(token).substr(separator + 1))));
            }
        }
    }
    return bytes;
}

[[nodiscard]] const char* envelopeSourceName(TimbreSource source) noexcept {
    switch (source) {
    case TimbreSource::Psg:
        return "PSG";
    case TimbreSource::Scc:
        return "SCC";
    case TimbreSource::Opll:
        return "OPLL";
    }
    return "Unknown";
}

[[nodiscard]] std::string formatEnvelopePlaybackComment(
    std::string_view composite_name,
    const CompositeLayer& layer) {
    std::string name(composite_name);
    for (auto& character : name) {
        if (character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    std::string comment;
    if (!name.empty()) {
        comment = name;
        comment += ' ';
    }
    comment += '(';
    comment += envelopeSourceName(layer.source);
    comment += " Ch.";
    comment += std::to_string(static_cast<unsigned>(layer.channel) + 1);
    comment += ')';
    if (layer.relative_semitones > 0) {
        comment += " ※";
        comment += std::to_string(
            static_cast<int>(layer.relative_semitones));
        comment += "度高い音階で演奏させる";
    } else if (layer.relative_semitones < 0) {
        comment += " ※";
        comment += std::to_string(
            -static_cast<int>(layer.relative_semitones));
        comment += "度低い音階で演奏させる";
    }
    return comment;
}

[[nodiscard]] std::string_view envelopeHeaderPrefix(
    std::string_view body) noexcept {
    if (body.size() >= 2 && body[0] == ',' && body[1] == ',') {
        return body.substr(0, 2);
    }
    const auto comma = body.find(',');
    if (comma == std::string_view::npos) {
        return {};
    }
    const auto second = body.find(',', comma + 1);
    if (second == std::string_view::npos) {
        return {};
    }
    return body.substr(0, second + 1);
}

[[nodiscard]] std::string wrapEnvelopeDefinition(
    std::uint8_t definition_number,
    const std::string& body,
    const std::string& comment) {
    const auto prefix = "@e" + std::to_string(definition_number) + " = { ";
    std::string single = prefix + body + " }";
    if (!comment.empty()) {
        single += " ; " + comment;
    }
    if (single.size() <= kMgscEnvelopeSourceLineLimit) {
        return single + "\r\n";
    }

    const auto header = envelopeHeaderPrefix(body);
    std::string first = prefix + std::string(header);
    if (!comment.empty()) {
        first += " ; " + comment;
    }
    std::string result = first;
    result += "\r\n";

    std::string commands = body;
    if (!header.empty()) {
        commands.erase(0, header.size());
    }
    std::vector<std::string> pieces;
    std::size_t cursor = 0;
    while (cursor < commands.size()) {
        const auto dot = commands.find('.', cursor);
        if (dot == std::string::npos) {
            pieces.push_back(commands.substr(cursor));
            break;
        }
        pieces.push_back(commands.substr(cursor, dot - cursor));
        cursor = dot + 1;
    }
    if (pieces.empty()) {
        result += "\t}\r\n";
        return result;
    }

    std::size_t index = 0;
    while (index < pieces.size()) {
        std::string line = "\t";
        bool first_token = true;
        while (index < pieces.size()) {
            const auto& piece = pieces[index];
            const bool last = index + 1 == pieces.size();
            const std::string addition =
                (first_token ? std::string() : std::string(".")) + piece;
            const std::string closing = last ? " }" : "";
            if (!first_token
                && line.size() + addition.size() + closing.size()
                    > kMgscEnvelopeSourceLineLimit) {
                break;
            }
            line += addition;
            first_token = false;
            ++index;
        }
        if (index == pieces.size()
            && line.find('}') == std::string::npos) {
            line += " }";
        }
        result += line;
        result += "\r\n";
    }
    return result;
}

}  // namespace

bool MgsEnvelopeFormatResult::hasIssue(
    MgsEnvelopeIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

static MgsEnvelopeFormatResult formatMgsCompositeEnvelopeImpl(
    const CompositeLayer& source_layer,
    std::uint8_t definition_number,
    std::size_t compiled_byte_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library,
    std::string_view composite_name,
    bool optimize_loop,
    bool allow_prefix) {
    if (source_layer.volume_envelope.kind == EnvelopeKind::Sequence
        && source_layer.volume_modulation.enabled) {
        // Tremolo replaces the authored volume events with sampled values.
        // Validate that source first, so malformed values/ramps cannot turn
        // into a valid silent sample and disappear during the replacement.
        // Disable modulation to terminate this preflight recursion, and use
        // an unlimited budget: capacity alone is recoverable after expansion.
        auto base_layer = source_layer;
        base_layer.pitch_modulation.enabled = false;
        base_layer.volume_modulation.enabled = false;
        base_layer.opll_tl_modulation.enabled = false;
        base_layer.opll_fb_modulation.enabled = false;
        const auto base = formatMgsCompositeEnvelopeImpl(
            base_layer, definition_number,
            std::numeric_limits<std::size_t>::max(), numbers, library,
            composite_name, false, false);
        if (!base.valid()) {
            return base;
        }
    }
    const auto expanded = expandCompositeLayerModulations(
        source_layer, library, optimize_loop);
    const auto& layer = expanded;
    MgsEnvelopeFormatResult result;
    if (definition_number > 31) {
        result.issues.push_back(MgsEnvelopeIssue::InvalidDefinitionNumber);
    }

    const auto& timeline = layer.envelope_timeline;
    const bool has_loop_start = timeline.loop_start_count.has_value();
    const bool has_loop_end = timeline.loop_end_count.has_value();
    if (has_loop_start != has_loop_end) {
        result.issues.push_back(MgsEnvelopeIssue::IncompleteLoop);
    } else if (has_loop_start
               && (*timeline.loop_start_count >= *timeline.loop_end_count
                   || *timeline.loop_end_count > timeline.length_counts)) {
        result.issues.push_back(MgsEnvelopeIssue::InvalidLoopRange);
    }
    if (!result.issues.empty()) {
        return result;
    }

    if (!compositeModulationLoopRepresentable(source_layer, library)) {
        result.issues.push_back(
            MgsEnvelopeIssue::UnrepresentableModulationLoop);
        return result;
    }
    const bool looping = has_loop_start;
    const auto effective_end = looping
        ? *timeline.loop_end_count
        : timeline.length_counts;
    std::vector<CountState> states(
        static_cast<std::size_t>(effective_end) + 1);

    const auto add_volume = [&](const EnvelopeEvent& event) {
        if (event.kind != EnvelopeEventKind::Volume
            || event.count > effective_end
            || (looping && event.count == effective_end
                && event.after_loop_start && !event.automatic)) {
            return;
        }
        if (event.value < 0 || event.value > 15) {
            addIssueOnce(result, MgsEnvelopeIssue::InvalidVolume);
            return;
        }
        states[event.count].volume = CountState::VolumeSpec{
            event.value, event.automatic, event.precise,
            event.automatic && event.secondary > 0
                ? std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(event.secondary - 1)}
                : std::nullopt};
    };
    for (const auto& event : layer.volume_envelope.events) {
        add_volume(event);
    }

    for (const auto& event : layer.pitch_envelope.events) {
        if (event.kind != EnvelopeEventKind::Pitch
            || event.count > effective_end) {
            continue;
        }
        if (event.value < -127 || event.value > 127) {
            addIssueOnce(result, MgsEnvelopeIssue::InvalidPitch);
            continue;
        }
        auto& tokens = event.after_loop_start
            ? states[event.count].after_pitch_tokens
            : states[event.count].before_pitch_tokens;
        appendToken(tokens, "\\" + std::to_string(event.value));
    }

    for (const auto& event : layer.timbre_automation) {
        if (event.count > effective_end) {
            continue;
        }
        auto& tokens = event.after_loop_start
            ? states[event.count].after_timbre_tokens
            : states[event.count].before_timbre_tokens;
        if (event.kind == EnvelopeEventKind::Timbre) {
            const auto number = envelopeEventTimbreNumber(event, numbers);
            if (!number) {
                addIssueOnce(result, MgsEnvelopeIssue::InvalidTimbre);
                continue;
            }
            appendToken(tokens, "@" + std::to_string(*number));
        } else if (event.kind == EnvelopeEventKind::RegisterWrite) {
            const auto maximum_register =
                layer.source == TimbreSource::Psg ? 15 : 56;
            if (layer.source == TimbreSource::Scc
                || event.value < 0
                || event.value > maximum_register
                || event.secondary < 0
                || event.secondary > 255) {
                addIssueOnce(
                    result, MgsEnvelopeIssue::InvalidRegisterWrite);
                continue;
            }
            appendToken(
                tokens,
                "y" + std::to_string(event.value)
                    + "," + std::to_string(event.secondary));
        }
    }
    // TL/FB auto authoring expands to standard yreg,data after manual y
    // at the same count (§7.5). Packs from the active original's register
    // image (base / @-slide + prior manual y). Skips ROM intervals.
    // A derived modulation update at the loop anchor belongs in the body.
    // Legacy auto events retain the before-`[` zone.
    if (layer.source == TimbreSource::Opll) {
        for (const auto& event :
             expandOpllLayerRegisterAutos(layer, library)) {
            if (event.count > effective_end
                || (looping && event.count == effective_end)
                || event.kind != EnvelopeEventKind::RegisterWrite) {
                continue;
            }
            if (event.value < 0 || event.value > 56
                || event.secondary < 0 || event.secondary > 255) {
                addIssueOnce(
                    result, MgsEnvelopeIssue::InvalidRegisterWrite);
                continue;
            }
            auto& tokens = event.after_loop_start
                ? states[event.count].after_timbre_tokens
                : states[event.count].before_timbre_tokens;
            appendToken(tokens,
                "y" + std::to_string(event.value)
                    + "," + std::to_string(event.secondary));
        }
    }
    // §6.5.1: prepend track-side base `@` at count 0 when the envelope
    // slides to another patch and/or mutates original-tone regs (manual y /
    // TL·FB auto). `@` reloads regs 0–7 so note/envelope start restores the
    // base original. Skip if already leading.
    if (layerEnvelopeNeedsLeadingBasePatch(layer, numbers)) {
        if (const auto base = layerBasePatchNumber(layer, numbers)) {
            const auto token = "@" + std::to_string(*base);
            auto& leading = states[0].before_timbre_tokens;
            const bool already = !leading.empty() && leading.front() == token;
            if (!already) {
                leading.insert(leading.begin(), token);
            }
        }
    }
    if (layer.volume > 15) {
        addIssueOnce(result, MgsEnvelopeIssue::InvalidVolume);
    }
    if (!result.issues.empty()) {
        return result;
    }
    if (!states[0].volume) {
        states[0].volume = CountState::VolumeSpec{
            static_cast<std::int32_t>(layer.volume), false, false};
    }

    std::vector<std::string> tokens;
    std::int32_t current_volume = layer.volume;
    std::uint32_t previous_volume_count = 0;
    std::uint32_t cursor{};
    std::vector<bool> automatic_ramp_started(states.size(), false);
    const auto next_automatic_volume = [&states, effective_end](
                                           std::uint32_t start)
        -> std::optional<std::uint32_t> {
        for (auto count = start + 1; count <= effective_end; ++count) {
            if (!states[count].volume) {
                continue;
            }
            if (states[count].volume->automatic) {
                return count;
            }
            return std::nullopt;
        }
        return std::nullopt;
    };
    const auto append_zero_time = [&](std::uint32_t count) {
        // §6.2.3: before-`[` zero-time commands, then `[`, then after-`[`.
        for (const auto& token : states[count].before_timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[count].before_pitch_tokens) {
            appendToken(tokens, token);
        }
        if (looping && count == *timeline.loop_start_count) {
            appendToken(tokens, "[");
        }
        for (const auto& token : states[count].after_timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[count].after_pitch_tokens) {
            appendToken(tokens, token);
        }
    };
    while (cursor < effective_end) {
        append_zero_time(cursor);
        if (states[cursor].volume) {
            const auto target_count = next_automatic_volume(cursor);
            const bool at_auto = states[cursor].volume->automatic;
            const bool ramp_started = automatic_ramp_started[cursor];
            if (target_count) {
                const auto origin_value = states[cursor].volume->value;
                const auto target_value =
                    states[*target_count].volume->value;
                std::vector<std::uint32_t> waypoints;
                for (auto count = cursor + 1; count < *target_count;
                     ++count) {
                    if (states[count].boundary()
                        || (looping
                            && count == *timeline.loop_start_count)) {
                        waypoints.push_back(count);
                    }
                }
                waypoints.push_back(*target_count);
                const bool emit_origin =
                    cursor == previous_volume_count
                    && !(at_auto && ramp_started);
                const bool precise_span =
                    states[*target_count].volume->precise;
                const auto imported_start =
                    states[*target_count].volume->imported_ramp_start;
                // An edit inside the imported ramp may add a newer origin.
                // In that case use the ordinary authored control-point rule.
                if (imported_start && *imported_start >= cursor
                    && *imported_start < *target_count) {
                    if (*target_count - *imported_start > 255
                        || *target_count - *imported_start == 240
                        || *target_count - *imported_start == 1) {
                        addIssueOnce(result,
                            MgsEnvelopeIssue::InvalidAutomaticVolumeDuration);
                        return result;
                    }
                    if (precise_span && waypoints.size() > 1) {
                        std::vector<std::uint8_t> volumes(
                            *imported_start - cursor,
                            static_cast<std::uint8_t>(origin_value));
                        auto ramp = sampleAutomaticRampVolumes(
                            static_cast<std::uint8_t>(origin_value),
                            static_cast<std::uint8_t>(target_value),
                            *target_count - *imported_start, false);
                        volumes.insert(volumes.end(),
                            ramp.begin(), ramp.end());
                        std::vector<std::uint32_t> cuts;
                        for (std::size_t i = 0; i + 1 < waypoints.size(); ++i) {
                            cuts.push_back(waypoints[i]);
                        }
                        emitPreciseAutomaticTokens(tokens, volumes, cursor,
                            cuts, append_zero_time);
                    } else {
                        std::uint32_t from = cursor;
                        for (const auto to : waypoints) {
                            if (from < *imported_start) {
                                const auto hold_end = std::min(to, *imported_start);
                                appendHold(tokens, origin_value, hold_end - from);
                                from = hold_end;
                            }
                            if (to > from) {
                                const auto duration = to - from;
                                if (duration == 240) {
                                    addIssueOnce(result,
                                        MgsEnvelopeIssue::InvalidAutomaticVolumeDuration);
                                    return result;
                                }
                                const auto segment_value = to == *target_count
                                    ? target_value
                                    : interpolatedVolume(origin_value,
                                          *imported_start, target_value,
                                          *target_count, to);
                                std::string token(1,
                                    volumeCharacter(segment_value));
                                if (duration > 1) {
                                    token += '=' + std::to_string(duration);
                                }
                                appendToken(tokens, std::move(token));
                                from = to;
                            }
                            if (to != *target_count) {
                                append_zero_time(to);
                            }
                        }
                    }
                    current_volume = target_value;
                    previous_volume_count = *target_count;
                    automatic_ramp_started[*target_count] = true;
                    cursor = *target_count;
                    continue;
                }
                if (precise_span && waypoints.size() > 1) {
                    const auto duration = *target_count - cursor;
                    if (duration > 255) {
                        addIssueOnce(
                            result,
                            MgsEnvelopeIssue::InvalidAutomaticVolumeDuration);
                        return result;
                    }
                    std::vector<std::uint32_t> cuts;
                    cuts.reserve(waypoints.size() - 1);
                    for (std::size_t i = 0; i + 1 < waypoints.size(); ++i) {
                        cuts.push_back(waypoints[i]);
                    }
                    const auto vols = sampleAutomaticRampVolumes(
                        static_cast<std::uint8_t>(origin_value),
                        static_cast<std::uint8_t>(target_value),
                        duration,
                        emit_origin);
                    emitPreciseAutomaticTokens(
                        tokens,
                        vols,
                        cursor,
                        cuts,
                        append_zero_time);
                    current_volume = target_value;
                    previous_volume_count = *target_count;
                    automatic_ramp_started[*target_count] = true;
                    cursor = *target_count;
                    continue;
                }
                std::uint32_t from = cursor;
                bool first_segment = true;
                std::int32_t junction_volume = origin_value;
                for (const auto to : waypoints) {
                    const auto duration = to - from;
                    if (duration == 0) {
                        from = to;
                        continue;
                    }
                    if (duration > 255 || duration == 240) {
                        addIssueOnce(
                            result,
                            MgsEnvelopeIssue::InvalidAutomaticVolumeDuration);
                        return result;
                    }
                    const auto segment_volume = to == *target_count
                        ? target_value
                        : interpolatedVolume(
                              origin_value,
                              cursor,
                              target_value,
                              *target_count,
                              to);
                    const bool emit_origin_now = first_segment && emit_origin;
                    if (emit_origin_now) {
                        appendToken(
                            tokens,
                            std::string(1, volumeCharacter(origin_value)));
                    }
                    first_segment = false;
                    std::string token(1, volumeCharacter(segment_volume));
                    if (duration == 1) {
                        appendToken(tokens, std::move(token));
                    } else {
                        token += '=' + std::to_string(duration);
                        appendToken(tokens, std::move(token));
                    }
                    junction_volume = segment_volume;
                    if (to != *target_count) {
                        append_zero_time(to);
                    }
                    from = to;
                }
                current_volume = target_value;
                previous_volume_count = *target_count;
                automatic_ramp_started[*target_count] = true;
                cursor = *target_count;
                continue;
            }
        }
        if (states[cursor].volume) {
            const auto spec = *states[cursor].volume;
            if (spec.automatic
                && automatic_ramp_started[cursor]) {
                current_volume = spec.value;
                previous_volume_count = cursor;
            } else {
                current_volume = spec.value;
                previous_volume_count = cursor;
            }
        }

        std::uint32_t next = cursor + 1;
        while (next < effective_end
               && !(looping && next == *timeline.loop_start_count)
               && !states[next].boundary()) {
            ++next;
        }
        appendHold(tokens, current_volume, next - cursor);
        cursor = next;
    }

    if (looping) {
        // L<: only `[`内（`]`直前）の after_* を `]` の前へ。`]以降`は §6.2.3 で出力しない。
        for (const auto& token : states[effective_end].after_timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[effective_end].after_pitch_tokens) {
            appendToken(tokens, token);
        }
        appendToken(tokens, "]");
    } else {
        for (const auto& token : states[effective_end].before_timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[effective_end].before_pitch_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[effective_end].after_timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[effective_end].after_pitch_tokens) {
            appendToken(tokens, token);
        }
        if (states[effective_end].volume) {
            const auto spec = *states[effective_end].volume;
            appendHold(tokens, spec.value, 1);
        }
    }

    coalesceAdjacentHolds(tokens);
    collapseTrailingHold(tokens);
    result.body = formatSequenceEnvelopeHeader(
                      layer.source, layer.volume_envelope.rate)
        + joinTokens(tokens);
    result.compiled_bytes = compiledEnvelopeBytes(tokens);
    if (optimize_loop && looping
        && !source_layer.envelope_timeline.loop_start_count
        && !source_layer.envelope_timeline.loop_end_count) {
        // Adopt an automatic loop only when the finalized byte stream is
        // smaller. Absolute Step targets stay finite; only relative pitch
        // can repeat its directional change inside the body.
        const auto finite = formatMgsCompositeEnvelopeImpl(
            source_layer, definition_number, compiled_byte_limit,
            numbers, library, composite_name, false, false);
        if (finite.compiled_bytes <= result.compiled_bytes) {
            if (allow_prefix
                && finite.hasIssue(MgsEnvelopeIssue::DefinitionLengthExceeded)) {
                return formatMgsCompositeEnvelopeImpl(
                    source_layer, definition_number, compiled_byte_limit,
                    numbers, library, composite_name, false, true);
            }
            return finite;
        }
    }
    if (result.compiled_bytes > compiled_byte_limit) {
        std::uint32_t cutoff_count{};
        if (!allow_prefix
            || !retainEnvelopePrefix(tokens, compiled_byte_limit, cutoff_count)) {
            result.issues.push_back(MgsEnvelopeIssue::DefinitionLengthExceeded);
            return result;
        }
        result.output_cutoff_count = cutoff_count;
        result.body = formatSequenceEnvelopeHeader(
                          layer.source, layer.volume_envelope.rate)
            + joinTokens(tokens);
        result.compiled_bytes = compiledEnvelopeBytes(tokens);
    }
    result.bytecode = compileEnvelopeTokens(tokens);
    result.definition = wrapEnvelopeDefinition(
        definition_number,
        result.body,
        formatEnvelopePlaybackComment(composite_name, layer));
    return result;
}

MgsEnvelopeFormatResult formatMgsCompositeEnvelope(
    const CompositeLayer& source_layer,
    std::uint8_t definition_number,
    std::size_t compiled_byte_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library,
    std::string_view composite_name) {
    return formatMgsCompositeEnvelopeImpl(
        source_layer, definition_number, compiled_byte_limit,
        numbers, library, composite_name, true, true);
}

std::string formatMgsCompositeTrackSetup(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers) {
    unsigned track_number = 1;
    switch (layer.source) {
    case TimbreSource::Psg:
        track_number = 1U + layer.channel;
        break;
    case TimbreSource::Scc:
        track_number = 4U + layer.channel;
        break;
    case TimbreSource::Opll:
        track_number = 9U + layer.channel;
        break;
    }
    std::string line = std::to_string(track_number);
    line += " v" + std::to_string(static_cast<unsigned>(layer.volume));
    if (layer.source != TimbreSource::Opll && layer.key_off_hang != 0) {
        line += " k" + std::to_string(
            static_cast<unsigned>(layer.key_off_hang));
    }
    if (layer.source == TimbreSource::Opll && layer.opll_sustain) {
        line += " so";
    }
    if (const auto number = layerBasePatchNumber(layer, numbers)) {
        line += " @" + std::to_string(static_cast<unsigned>(*number));
    }
    if (layer.volume_envelope.kind == EnvelopeKind::Rate
        || layerUsesSequenceEnvelope(layer)) {
        const auto envelope_number = std::min<std::uint8_t>(
            layer.envelope_number, 31);
        const bool rate =
            layer.volume_envelope.kind == EnvelopeKind::Rate;
        line += rate ? " @r" : " @e";
        line += std::to_string(static_cast<unsigned>(envelope_number));
    }
    if (layer.detune != 0) {
        line += " \\" + std::to_string(static_cast<int>(layer.detune));
    }
    if (layer.micro_detune != 0) {
        line += " @\\"
            + std::to_string(static_cast<int>(layer.micro_detune));
    }
    const bool pitch_sweep =
        layer.pitch_sweep.enabled
        && layer.source != TimbreSource::Opll;
    if (pitch_sweep) {
        line += " p" + std::to_string(
            static_cast<unsigned>(layer.pitch_sweep.value));
    } else if (layer.software_lfo.enabled) {
        const auto lfo = clampSoftwareLfo(
            layer.software_lfo, layer.source != TimbreSource::Opll);
        line += " h"
            + std::to_string(static_cast<unsigned>(lfo.delay)) + ","
            + std::to_string(static_cast<unsigned>(lfo.depth)) + ","
            + std::to_string(static_cast<unsigned>(lfo.speed)) + ","
            + std::to_string(static_cast<int>(lfo.roughness));
        if (layer.source != TimbreSource::Opll && lfo.extra_roughness != 0) {
            line += " @p"
                + std::to_string(static_cast<int>(lfo.extra_roughness));
        }
    }
    if (layer.start_delay_value != 0) {
        if (layer.start_delay_form == StartDelayForm::NoteLength) {
            line += " r"
                + std::to_string(
                    static_cast<unsigned>(layer.start_delay_value));
        } else {
            line += " r%"
                + std::to_string(
                    static_cast<unsigned>(layer.start_delay_value));
        }
    }
    return line;
}

std::string formatMgsCompositeTrackPreview(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers) {
    auto line = formatMgsCompositeTrackSetup(layer, numbers);
    char channel = '\0';
    switch (layer.source) {
    case TimbreSource::Psg:
        if (layer.channel < 3) {
            channel = static_cast<char>('1' + layer.channel);
        }
        break;
    case TimbreSource::Scc:
        if (layer.channel < 5) {
            channel = static_cast<char>('4' + layer.channel);
        }
        break;
    case TimbreSource::Opll:
        if (layer.channel < 9) {
            channel = "9ABCDEFGH"[layer.channel];
        }
        break;
    }
    if (channel != '\0') {
        line.replace(0, line.find(' '), 1, channel);
    }
    return line;
}

std::uint32_t maxEnvelopeLengthFittingBodyLimit(
    const CompositeLayer& layer,
    std::size_t compiled_byte_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library) {
    auto probe = layer;
    // Length capacity is independent of incomplete loop authoring errors.
    probe.envelope_timeline.loop_start_count.reset();
    probe.envelope_timeline.loop_end_count.reset();
    auto fits = [&](std::uint32_t length) {
        probe.envelope_timeline.length_counts = std::max<std::uint32_t>(1, length);
        const auto formatted = formatMgsCompositeEnvelopeImpl(
            probe, 0, compiled_byte_limit, numbers, library, {}, true, false);
        if (formatted.body.empty()) {
            return false;
        }
        return formatted.compiled_bytes <= compiled_byte_limit;
    };

    if (!fits(1)) {
        return 1;
    }
    // Cap the probe ceiling: format allocates O(length) state, and sparse
    // holds can "fit" far beyond useful @e authoring ranges.
    std::uint32_t low = 1;
    std::uint32_t high = std::min(
        kMgscEnvelopeUiLengthCap, EnvelopeTimeline::kMaximumLengthCounts);
    if (fits(high)) {
        return high;
    }
    while (low + 1 < high) {
        const auto mid = low + (high - low) / 2;
        if (fits(mid)) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

}  // namespace mgstc::engine
