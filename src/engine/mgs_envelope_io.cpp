#include "mgstc/engine/mgs_envelope_io.hpp"

#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/composite_modulation.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/scc_morph.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <vector>

namespace mgstc::engine {
namespace {

// Author coordinates accompany the uncoalesced token stream only. Export
// formatting and bytecode remain unchanged; the display map is prepared off
// the audio thread before transformations such as the terminal-hold collapse.
struct DisplayMappedTokens : std::vector<std::string> {
    std::vector<std::uint32_t> origins;
    std::uint32_t source_count{};
};

void setTokenOrigin(std::vector<std::string>&, std::uint32_t) {}
void setTokenOrigin(DisplayMappedTokens& tokens, std::uint32_t count) {
    tokens.source_count = count;
}

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
    struct OrderedCommand {
        std::string token;
        std::uint32_t order{};
        bool after{};
        bool pitch{};
        bool leading{};
    };
    std::vector<OrderedCommand> ordered_commands;

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

[[nodiscard]] MgsEnvelopeCommandZone commandZone(
    const EnvelopeTimeline& timeline, std::uint32_t count, bool after) {
    if (timeline.loop_start_count == count)
        return after ? MgsEnvelopeCommandZone::AfterLoopStart
                     : MgsEnvelopeCommandZone::BeforeLoopStart;
    if (timeline.loop_end_count == count)
        return after ? MgsEnvelopeCommandZone::BeforeLoopEnd
                     : MgsEnvelopeCommandZone::AfterLoopEnd;
    return MgsEnvelopeCommandZone::Count;
}

using CommandConflictKey = std::tuple<std::uint32_t,
    MgsEnvelopeCommandType, MgsEnvelopeCommandZone>;
using CommandOccurrences = std::map<CommandConflictKey, std::pair<unsigned, bool>>;

void recordRestrictedCommand(CommandOccurrences& occurrences,
    const EnvelopeTimeline& timeline, std::uint32_t count,
    MgsEnvelopeCommandType type, bool after, bool implicit_base = false) {
    auto& [amount, has_base] = occurrences[{count, type, commandZone(timeline, count, after)}];
    ++amount;
    has_base = has_base || implicit_base;
}

void appendCommandConflicts(MgsEnvelopeFormatResult& result,
    const CommandOccurrences& occurrences) {
    for (const auto& [key, occurrence] : occurrences) {
        if (occurrence.first < 2) continue;
        const auto& [count, type, zone] = key;
        const auto found = std::find_if(result.command_conflicts.begin(),
            result.command_conflicts.end(), [&](const auto& conflict) {
                return conflict.count == count && conflict.type == type && conflict.zone == zone;
            });
        if (found == result.command_conflicts.end()) {
            result.command_conflicts.push_back({count, type, zone, occurrence.second});
        } else {
            found->includes_automatic_base |= occurrence.second;
        }
        addIssueOnce(result, MgsEnvelopeIssue::DuplicateZeroTimeCommand);
    }
}

void validateAuthoredRestrictedCommands(const CompositeLayer& layer,
    MgsEnvelopeFormatResult& result) {
    CommandOccurrences occurrences;
    for (const auto& event : layer.timbre_automation) {
        std::optional<MgsEnvelopeCommandType> type;
        switch (event.kind) {
        case EnvelopeEventKind::Noise: type = MgsEnvelopeCommandType::Noise; break;
        case EnvelopeEventKind::ToneNoiseMode: type = MgsEnvelopeCommandType::ToneNoiseMode; break;
        case EnvelopeEventKind::Timbre: type = MgsEnvelopeCommandType::Timbre; break;
        default: break;
        }
        if (type) recordRestrictedCommand(occurrences, layer.envelope_timeline,
            event.count, *type, event.after_loop_start);
    }
    appendCommandConflicts(result, occurrences);
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

void appendToken(DisplayMappedTokens& tokens, std::string token) {
    if (!token.empty()) {
        tokens.origins.push_back(tokens.source_count);
        tokens.push_back(std::move(token));
    }
}

template <typename Tokens>
void appendHold(
    Tokens& tokens,
    std::int32_t volume,
    std::uint32_t count) {
    while (count != 0) {
        const auto chunk = std::min<std::uint32_t>(239, count);
        std::string token(1, volumeCharacter(volume));
        if (chunk != 1) {
            token += ':' + std::to_string(chunk);
        }
        appendToken(tokens, std::move(token));
        if constexpr (std::is_same_v<Tokens, DisplayMappedTokens>)
            tokens.source_count += chunk;
        count -= chunk;
    }
}

template <typename Tokens, typename AppendZeroTime>
void emitPreciseAutomaticTokens(
    Tokens& tokens,
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
    std::uint32_t run_start = origin_count;
    const auto flush = [&]() {
        if (run_length != 0) {
            setTokenOrigin(tokens, run_start);
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
            run_start = count;
            run_volume = volume;
            run_length = 1;
        } else if (volume == run_volume) {
            ++run_length;
        } else {
            flush();
            run_start = count;
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

[[nodiscard]] std::optional<std::vector<std::uint8_t>> compileEnvelopeTokens(
    const std::vector<std::string>& tokens) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(compiledEnvelopeBytes(tokens));
    bool valid_numbers = true;
    const auto number = [&valid_numbers](std::string_view text, int minimum, int maximum) {
        int value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || value < minimum || value > maximum) valid_numbers = false;
        return value;
    };
    for (const auto& token : tokens) {
        if (token.empty()) return std::nullopt;
        if (token == "[") {
            bytes.push_back(0x40);
        } else if (token == "]") {
            bytes.push_back(0x60);
        } else if (token.front() == '@') {
            bytes.insert(bytes.end(), {0x10,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1), 0, 31))});
        } else if (token.front() == '\\') {
            bytes.insert(bytes.end(), {0x12,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1), -127, 127))});
        } else if (token.front() == 'n' || token.front() == 'N') {
            bytes.push_back(static_cast<std::uint8_t>(0x80 | number(std::string_view(token).substr(1), 0, 31)));
        } else if (token.front() == '/' || token.front() == '*') {
            bytes.push_back(static_cast<std::uint8_t>(0xA0 | number(std::string_view(token).substr(1), 0, 3)));
        } else if (token.front() == 'y' || token.front() == 'Y') {
            const auto comma = token.find(',');
            if (comma == std::string::npos) return std::nullopt;
            bytes.insert(bytes.end(), {0x11,
                static_cast<std::uint8_t>(number(std::string_view(token).substr(1, comma - 1), 0, 56)),
                static_cast<std::uint8_t>(number(std::string_view(token).substr(comma + 1), 0, 255))});
        } else if (std::isxdigit(static_cast<unsigned char>(token.front()))) {
            const auto value = static_cast<std::uint8_t>(
                std::isdigit(static_cast<unsigned char>(token.front()))
                    ? token.front() - '0'
                    : std::tolower(static_cast<unsigned char>(token.front())) - 'a' + 10);
            const auto separator = token.find_first_of(":=");
            if (separator == std::string::npos) {
                if (token.size() != 1) return std::nullopt;
                bytes.push_back(value);
            } else {
                if (separator != 1) return std::nullopt;
                bytes.push_back(static_cast<std::uint8_t>(
                    (token[separator] == ':' ? 0xE0 : 0x20) | value));
                bytes.push_back(static_cast<std::uint8_t>(
                    number(std::string_view(token).substr(separator + 1), 1, 255)));
            }
        } else {
            return std::nullopt;
        }
    }
    if (!valid_numbers) return std::nullopt;
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

std::string describeMgsEnvelopeCommandConflict(
    const MgsEnvelopeCommandConflict& conflict) {
    const auto command = conflict.type == MgsEnvelopeCommandType::Noise ? "n"
        : conflict.type == MgsEnvelopeCommandType::ToneNoiseMode ? "/（*）" : "@";
    const auto zone = [&] {
        switch (conflict.zone) {
        case MgsEnvelopeCommandZone::BeforeLoopStart: return "[ より前（初回のみ）";
        case MgsEnvelopeCommandZone::AfterLoopStart: return "[ より後（毎周）";
        case MgsEnvelopeCommandZone::BeforeLoopEnd: return "] 直前（ループ内）";
        case MgsEnvelopeCommandZone::AfterLoopEnd: return "] 以降";
        default: return "通常カウント";
        }
    }();
    auto text = "ct " + std::to_string(conflict.count) + " / " + zone + " の " + command
        + " は1件だけ指定できます。重複を解消してください。";
    if (conflict.includes_automatic_base)
        text += " 自動追加する先頭の基準音色 @ と衝突しています。";
    return text;
}

static MgsEnvelopeFormatResult formatMgsCompositeEnvelopeImpl(
    const CompositeLayer& source_layer,
    std::uint8_t definition_number,
    std::size_t compiled_byte_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library,
    std::string_view composite_name,
    bool optimize_loop,
    bool allow_prefix,
    bool validate_commands = true) {
    MgsEnvelopeFormatResult result;
    if (validate_commands) {
        if (source_layer.volume_envelope.kind == EnvelopeKind::Sequence) {
            for (const auto& event : source_layer.volume_envelope.events) {
                if (event.kind == EnvelopeEventKind::Volume && event.automatic
                    && event.secondary > 0
                    && event.count == static_cast<std::uint32_t>(event.secondary - 1)
                    && std::find(result.ambiguous_imported_automatic_counts.begin(),
                        result.ambiguous_imported_automatic_counts.end(), event.count)
                            == result.ambiguous_imported_automatic_counts.end())
                    result.ambiguous_imported_automatic_counts.push_back(event.count);
            }
            if (!result.ambiguous_imported_automatic_counts.empty()) {
                addIssueOnce(result, MgsEnvelopeIssue::AmbiguousImportedAutomaticVolume);
                return result;
            }
        }
        // Validate authoring before modulation expansion can replace commands.
        validateAuthoredRestrictedCommands(source_layer, result);
        if (!result.valid()) return result;
    }
    if (source_layer.volume_envelope.kind == EnvelopeKind::Sequence
        && compositeHasModulation(source_layer, CompositeModulationTarget::Volume)) {
        // Tremolo replaces the authored volume events with sampled values.
        // Validate that source first, so malformed values/ramps cannot turn
        // into a valid silent sample and disappear during the replacement.
        // Disable modulation to terminate this preflight recursion, and use
        // an unlimited budget: capacity alone is recoverable after expansion.
        auto base_layer = source_layer;
        disableCompositeModulation(base_layer, CompositeModulationTarget::Pitch);
        disableCompositeModulation(base_layer, CompositeModulationTarget::Volume);
        disableCompositeModulation(base_layer, CompositeModulationTarget::TotalLevel);
        disableCompositeModulation(base_layer, CompositeModulationTarget::Feedback);
        const auto base = formatMgsCompositeEnvelopeImpl(
            base_layer, definition_number,
            std::numeric_limits<std::size_t>::max(), numbers, library,
            composite_name, false, false, validate_commands);
        if (!base.valid()) {
            return base;
        }
    }
    const auto expanded = expandCompositeLayerModulations(
        source_layer, library, optimize_loop);
    const auto& layer = expanded;
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

    // Authored [] repeats the first traversal's command stream. MGSDRV's
    // jump preserves accumulated pitch and the last volume/register state;
    // matching an indefinitely continuous authoring LFO is not required.
    // The phase classification remains available to the editor's graph.
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
        states[event.count].ordered_commands.push_back({
            "\\" + std::to_string(event.value), event.command_order,
            event.after_loop_start, true});
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
            states[event.count].ordered_commands.push_back({
                "@" + std::to_string(*number), event.command_order,
                event.after_loop_start, false});
        } else if (event.kind == EnvelopeEventKind::Noise || event.kind == EnvelopeEventKind::ToneNoiseMode) {
            const bool noise = event.kind == EnvelopeEventKind::Noise;
            if (layer.source != TimbreSource::Psg || event.value < 0 || event.value > (noise ? 31 : 3)) {
                addIssueOnce(result, MgsEnvelopeIssue::InvalidRegisterWrite);
                continue;
            }
            const auto token = std::string(noise ? "n" : "/") + std::to_string(event.value);
            appendToken(tokens, token);
            states[event.count].ordered_commands.push_back({token,
                event.command_order, event.after_loop_start, false});
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
            states[event.count].ordered_commands.push_back({
                "y" + std::to_string(event.value) + "," + std::to_string(event.secondary),
                event.command_order, event.after_loop_start, false});
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
                || (looping && event.count == effective_end && !event.after_loop_start)
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
            states[event.count].ordered_commands.push_back({
                "y" + std::to_string(event.value) + "," + std::to_string(event.secondary),
                event.command_order, event.after_loop_start, false});
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
                states[0].ordered_commands.insert(states[0].ordered_commands.begin(),
                    {token, 0, false, false, true});
            }
        }
    }
    if (validate_commands) {
        CommandOccurrences occurrences;
        for (std::uint32_t count = 0; count < states.size(); ++count) {
            for (const auto& command : states[count].ordered_commands) {
                std::optional<MgsEnvelopeCommandType> type;
                switch (command.token.front()) {
                case 'n': type = MgsEnvelopeCommandType::Noise; break;
                case '/': case '*': type = MgsEnvelopeCommandType::ToneNoiseMode; break;
                case '@': type = MgsEnvelopeCommandType::Timbre; break;
                default: break;
                }
                if (type) recordRestrictedCommand(occurrences, timeline, count,
                    *type, command.after, command.leading);
            }
        }
        appendCommandConflicts(result, occurrences);
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

    DisplayMappedTokens tokens;
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
    const auto append_zero_time = [&](std::uint32_t count, bool inside_only = false) {
        setTokenOrigin(tokens, count);
        auto commands = states[count].ordered_commands;
        if (inside_only) std::erase_if(commands, [](const auto& command) { return !command.after; });
        const bool anchor = looping && count == *timeline.loop_start_count;
        const auto default_priority = [](const auto& command) {
            switch (command.token.front()) {
            case 'n': return 0;
            case '/': case '*': return 1;
            case '\\': return 2;
            case '@': return 3;
            default: return 4; // y
            }
        };
        std::stable_sort(commands.begin(), commands.end(),
            [anchor, default_priority](const auto& a, const auto& b) {
                if (anchor && a.after != b.after) return !a.after;
                if (a.leading != b.leading) return a.leading;
                if (a.order && b.order) return a.order < b.order;
                if (a.order != b.order) return a.order != 0;
                return default_priority(a) < default_priority(b);
            });
        bool bracket = false;
        for (const auto& command : commands) {
            if (anchor && command.after && !bracket) {
                appendToken(tokens, "[");
                bracket = true;
            }
            appendToken(tokens, command.token);
        }
        if (anchor && !bracket) appendToken(tokens, "[");
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
                        || *target_count - *imported_start == 240) {
                        addIssueOnce(result,
                            MgsEnvelopeIssue::InvalidAutomaticVolumeDuration);
                        return result;
                    }
                    if (precise_span && waypoints.size() > 1
                        && *target_count - *imported_start > 1) {
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
                            setTokenOrigin(tokens, from);
                            if (from < *imported_start) {
                                const auto hold_end = std::min(to, *imported_start);
                                appendHold(tokens, origin_value, hold_end - from);
                                from = hold_end;
                                setTokenOrigin(tokens, from);
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
                    setTokenOrigin(tokens, from);
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
                        // A direct target letter executes at the destination;
                        // only an actual ramp repeats the origin count.
                        setTokenOrigin(tokens, to);
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
            current_volume = spec.value;
            previous_volume_count = cursor;
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
        append_zero_time(effective_end, true);
        appendToken(tokens, "]");
    } else {
        append_zero_time(effective_end);
        if (states[effective_end].volume) {
            const auto spec = *states[effective_end].volume;
            appendHold(tokens, spec.value, 1);
        }
    }

    // Timed tokens retain their exact authored origin, including the repeated
    // origin sample of a compact automatic ramp. Zero-time commands consume
    // no display entries, just as they consume no execution ticks.
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (const auto duration = tokenDuration(tokens[index])) {
            for (std::uint32_t tick = 0; tick < *duration; ++tick)
                result.playback_counts.push_back(tokens.origins[index] + tick);
        }
    }
    if (!looping) {
        // END is a visible author boundary even when its terminal hold is
        // shortened in the exported program. Keep one final END position.
        while (result.playback_counts.size() > 1
            && result.playback_counts.back() == effective_end
            && result.playback_counts[result.playback_counts.size() - 2] == effective_end)
            result.playback_counts.pop_back();
        if (result.playback_counts.empty() || result.playback_counts.back() < effective_end)
            result.playback_counts.push_back(effective_end);
    }
    coalesceAdjacentHolds(tokens);
    collapseTrailingHold(tokens);
    result.body = formatSequenceEnvelopeHeader(layer)
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
            numbers, library, composite_name, false, false, validate_commands);
        if (finite.compiled_bytes <= result.compiled_bytes) {
            if (allow_prefix
                && finite.hasIssue(MgsEnvelopeIssue::DefinitionLengthExceeded)) {
                return formatMgsCompositeEnvelopeImpl(
                    source_layer, definition_number, compiled_byte_limit,
                    numbers, library, composite_name, false, true, validate_commands);
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
        // A byte-budget prefix cannot display the excluded remainder. This is
        // an execution-tick cutoff, so retain the corresponding map prefix.
        result.playback_counts.resize(std::min<std::size_t>(
            result.playback_counts.size(), cutoff_count));
        result.body = formatSequenceEnvelopeHeader(layer)
            + joinTokens(tokens);
        result.compiled_bytes = compiledEnvelopeBytes(tokens);
    }
    auto compiled_tokens = compileEnvelopeTokens(tokens);
    if (!compiled_tokens) {
        addIssueOnce(result, MgsEnvelopeIssue::InvalidGeneratedCommand);
        result.body.clear();
        result.definition.clear();
        result.playback_counts.clear();
        return result;
    }
    result.bytecode = std::move(*compiled_tokens);
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

namespace {
std::optional<std::string> commandConflictInLayers(
    const CompositeTimbre& timbre, const TimbreLibrary* library,
    bool include_ambiguous_import = false) {
    const auto numbers = resolveTimbreNumbers(timbre);
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        const auto& layer = timbre.layers[index];
        if (!layerUsesSequenceEnvelope(layer)) continue;
        const auto formatted = formatMgsCompositeEnvelope(layer, layer.envelope_number,
            std::numeric_limits<std::size_t>::max(), &numbers, library, timbre.name);
        if (!formatted.command_conflicts.empty()) {
            return "layer " + std::to_string(index + 1) + ": "
                + describeMgsEnvelopeCommandConflict(formatted.command_conflicts.front());
        }
        if (include_ambiguous_import && !formatted.ambiguous_imported_automatic_counts.empty()) {
            return "layer " + std::to_string(index + 1) + ": ct "
                + std::to_string(formatted.ambiguous_imported_automatic_counts.front())
                + ": imported zero-count volume has ambiguous saved timing; re-enter this volume step";
        }
    }
    return std::nullopt;
}
} // namespace

std::optional<std::string> mgsCompositeCommandConflict(
    const CompositeTimbre& timbre, const TimbreLibrary* library) {
    if (auto conflict = commandConflictInLayers(timbre, library)) return conflict;
    const auto morph = compileSccMorphCached(timbre);
    return morph->valid ? commandConflictInLayers(morph->timbre, library) : std::nullopt;
}

std::optional<std::string> mgsCompositeAuthoringIssue(
    const CompositeTimbre& timbre, const TimbreLibrary* library) {
    if (auto issue = commandConflictInLayers(timbre, library, true)) return issue;
    const auto morph = compileSccMorphCached(timbre);
    return morph->valid ? commandConflictInLayers(morph->timbre, library, true) : std::nullopt;
}

namespace {
bool hasOnlyRepairableAuthoringIssues(
    const CompositeTimbre& timbre, const TimbreLibrary* library,
    bool include_ambiguous_import) {
    const auto morph = compileSccMorphCached(timbre);
    if (!morph->valid) return false;
    bool found_conflict = false;
    const auto inspect = [&](const CompositeTimbre& candidate, bool authored) {
        if (!validateCompositeTimbre(candidate).valid()) return false;
        auto numbering_input = candidate;
        if (authored) {
            for (auto& layer : numbering_input.layers) {
                if (layer.source == TimbreSource::Scc
                    && layer.scc_output_allocation == SccOutputAllocationMode::Contiguous) {
                    // Source aliases are relocated by the existing channel allocator.
                    layer.base_timbre.reset();
                    layer.timbre_automation.clear();
                }
            }
        }
        auto numbers = resolveTimbreNumbers(numbering_input);
        if (!numbers.valid()) return false;
        if (authored) {
            for (std::size_t index = 0; index < candidate.layers.size(); ++index) {
                const auto& layer = candidate.layers[index];
                if (layer.source != TimbreSource::Scc
                    || layer.scc_output_allocation != SccOutputAllocationMode::Contiguous) continue;
                const auto inspect_reference = [&](std::uint64_t id) {
                    const auto* snapshot = findEmbeddedTimbreSnapshot(candidate, id);
                    if (!snapshot || snapshot->source != TimbreSource::Scc) return false;
                    if (!assignedNumberForLibraryId(numbers, id))
                        numbers.assignments.push_back({index, id, 0, false});
                    return true;
                };
                if (layer.base_timbre && !inspect_reference(layer.base_timbre->library_id)) return false;
                if (layer.volume_envelope.kind != EnvelopeKind::Sequence) continue;
                for (const auto& event : layer.timbre_automation) {
                    if (event.kind == EnvelopeEventKind::Timbre
                        && event.timbre_pick == TimbrePick::Library && event.target_library_id
                        && !inspect_reference(event.target_library_id)) return false;
                }
            }
        }
        // The source-only assignments above validate author values and references;
        // they are never emitted. The materialized pass validates real numbers,
        // byte capacity and conflicts after the channel allocator has run.
        // Number resolution can provision an assignment for an unresolved
        // library ID. Program compilation still needs its actual snapshot.
        // Keep this classification gate aligned with its embedded-first,
        // then supplied-live-library lookup; a dummy number is not a patch.
        for (const auto& assignment : numbers.assignments) {
            if (findEmbeddedTimbreSnapshot(candidate, assignment.library_id) != nullptr)
                continue;
            if (library == nullptr || library->find(assignment.library_id) == nullptr)
                return false;
        }
        // Inspect inactive layers too: retaining the author model is not an
        // exception for another invalid layer that is currently inaudible.
        for (const auto& layer : candidate.layers) {
            if (!layerUsesSequenceEnvelope(layer)) continue;
            auto unexpanded = layer;
            for (const auto target : {CompositeModulationTarget::Pitch,
                    CompositeModulationTarget::Volume, CompositeModulationTarget::TotalLevel,
                    CompositeModulationTarget::Feedback, CompositeModulationTarget::Noise})
                disableCompositeModulation(unexpanded, target);
            for (const auto target : {OpllRegisterAutoTarget::Noise,
                    OpllRegisterAutoTarget::TotalLevel, OpllRegisterAutoTarget::Feedback})
                disableCompositeRegisterAuto(unexpanded, target);
            // Generated commands may replace or clamp source values. Inspect
            // the authored values too, without imposing a byte budget on this
            // non-output preflight (expansion can legitimately change size).
            const auto authored_checks = formatMgsCompositeEnvelopeImpl(unexpanded,
                layer.envelope_number, std::numeric_limits<std::size_t>::max(),
                &numbers, library, candidate.name, false, false, false);
            if (!authored_checks.valid()) return false;
            const auto strict = formatMgsCompositeEnvelope(layer, layer.envelope_number,
                kMgscEnvelopeCompiledByteLimit, &numbers, library, candidate.name);
            for (const auto issue : strict.issues) {
                if (issue != MgsEnvelopeIssue::DuplicateZeroTimeCommand
                    && !(include_ambiguous_import
                        && issue == MgsEnvelopeIssue::AmbiguousImportedAutomaticVolume)) return false;
            }
            found_conflict |= strict.hasIssue(MgsEnvelopeIssue::DuplicateZeroTimeCommand)
                || (include_ambiguous_import
                    && strict.hasIssue(MgsEnvelopeIssue::AmbiguousImportedAutomaticVolume));
            // A strict conflict returns before later checks. Re-run those
            // checks privately without this one rule so an invalid loop/value
            // cannot be concealed by an earlier duplicate. Nothing generated
            // by this classification pass is returned or sent to an engine.
            const auto other_checks = formatMgsCompositeEnvelopeImpl(layer,
                layer.envelope_number, kMgscEnvelopeCompiledByteLimit,
                &numbers, library, candidate.name, true, true, false);
            if (!other_checks.valid()) return false;
        }
        return true;
    };
    return inspect(timbre, true) && inspect(morph->timbre, false) && found_conflict;
}
} // namespace

bool mgsCompositeHasOnlyCommandConflicts(
    const CompositeTimbre& timbre, const TimbreLibrary* library) {
    return hasOnlyRepairableAuthoringIssues(timbre, library, false);
}

bool mgsCompositeHasOnlyRepairableAuthoringIssues(
    const CompositeTimbre& timbre, const TimbreLibrary* library) {
    return hasOnlyRepairableAuthoringIssues(timbre, library, true);
}

std::string formatMgsCompositeTrackSetup(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers,
    std::string* error) {
    if (error != nullptr) error->clear();
    if ((layer.volume_envelope.kind == EnvelopeKind::Rate
            || layerUsesSequenceEnvelope(layer))
        && layer.envelope_number > 31) {
        if (error != nullptr) *error = "invalid envelope definition number";
        return {};
    }
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
    if (layer.source == TimbreSource::Psg) {
        if (layer.psg_tone_mode) line += " /" + std::to_string(*layer.psg_tone_mode);
        if (layer.psg_noise) line += " n" + std::to_string(*layer.psg_noise);
    }
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
        const auto envelope_number = layer.envelope_number;
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
    if (line.empty()) return line;
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
            // This is a byte-capacity probe, not output: clearing [] above
            // must not invalidate legal one-per-zone commands at its anchor.
            probe, 0, compiled_byte_limit, numbers, library, {}, true, false, false);
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
