#include "mgstc/engine/mgs_envelope_io.hpp"

#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/opll_register_auto.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
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

    std::string first = prefix + ",,";
    if (!comment.empty()) {
        first += " ; " + comment;
    }
    std::string result = first;
    result += "\r\n";

    std::string commands = body;
    if (commands.size() >= 2 && commands[0] == ',' && commands[1] == ',') {
        commands.erase(0, 2);
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

// #region agent log
void agentDbg(
    const char* hypothesisId,
    const char* location,
    const char* message,
    const std::string& dataObject) {
    try {
        static std::atomic<int> remaining{400};
        if (remaining.fetch_sub(1) <= 0) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream line;
        line << "{\"sessionId\":\"6045ae\",\"runId\":\"post-fix\",\"hypothesisId\":\""
             << hypothesisId << "\",\"location\":\"" << location
             << "\",\"message\":\"" << message << "\",\"data\":" << dataObject
             << ",\"timestamp\":" << ms << "}\n";
        const auto payload = line.str();
        auto append = [&payload](const std::filesystem::path& path) {
            std::ofstream out(path, std::ios::app | std::ios::binary);
            if (out) {
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
        };
        append(std::filesystem::path(u8"debug-6045ae.log"));
        append(std::filesystem::path(
            u8"debug-6045ae.log"));
    } catch (...) {
    }
}
// #endregion

}  // namespace

bool MgsEnvelopeFormatResult::hasIssue(
    MgsEnvelopeIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

MgsEnvelopeFormatResult formatMgsCompositeEnvelope(
    const CompositeLayer& layer,
    std::uint8_t definition_number,
    std::size_t compiled_byte_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library,
    std::string_view composite_name) {
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

    const bool looping = has_loop_start;
    const auto effective_end = looping
        ? *timeline.loop_end_count
        : timeline.length_counts;
    std::vector<CountState> states(
        static_cast<std::size_t>(effective_end) + 1);

    const auto add_volume = [&](const EnvelopeEvent& event) {
        if (event.kind != EnvelopeEventKind::Volume
            || event.count > effective_end
            || (looping && event.count == effective_end)) {
            return;
        }
        if (event.value < 0 || event.value > 15) {
            addIssueOnce(result, MgsEnvelopeIssue::InvalidVolume);
            return;
        }
        states[event.count].volume = CountState::VolumeSpec{
            event.value, event.automatic, event.precise};
    };
    for (const auto& event : layer.volume_envelope.events) {
        add_volume(event);
    }

    for (const auto& event : layer.pitch_envelope.events) {
        if (event.kind != EnvelopeEventKind::Pitch
            || event.count > effective_end
            || (looping && event.count == effective_end)) {
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
        if (event.count > effective_end
            || (looping && event.count == effective_end)) {
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
    // Autos always land in the before-`[` zone (first-pass only).
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
            appendToken(
                states[event.count].before_timbre_tokens,
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

    // #region agent log
    {
        bool any_automatic = false;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.automatic) {
                any_automatic = true;
                break;
            }
        }
        if (any_automatic) {
            std::ostringstream data;
            data << "{\"vols\":[";
            bool first = true;
            for (const auto& event : layer.volume_envelope.events) {
                if (event.kind != EnvelopeEventKind::Volume) {
                    continue;
                }
                if (!first) {
                    data << ',';
                }
                first = false;
                data << "{\"c\":" << event.count << ",\"v\":" << event.value
                     << ",\"a\":" << (event.automatic ? "true" : "false")
                     << "}";
            }
            data << "],\"pitch\":[";
            first = true;
            for (const auto& event : layer.pitch_envelope.events) {
                if (event.kind != EnvelopeEventKind::Pitch) {
                    continue;
                }
                if (!first) {
                    data << ',';
                }
                first = false;
                data << "{\"c\":" << event.count << ",\"v\":" << event.value
                     << "}";
            }
            data << "],\"timbre\":" << layer.timbre_automation.size()
                 << ",\"len\":" << timeline.length_counts << "}";
            agentDbg(
                "H1",
                "mgs_envelope_io.cpp:events",
                "envelope events before format",
                data.str());
        }
    }
    // #endregion

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
                    const bool emit_junction = !first_segment;
                    first_segment = false;
                    std::string token(1, volumeCharacter(segment_volume));
                    if (duration == 1) {
                        appendToken(tokens, std::move(token));
                    } else {
                        token += '=' + std::to_string(duration);
                        appendToken(tokens, std::move(token));
                    }
                    // #region agent log
                    {
                        std::ostringstream data;
                        data << "{\"from\":" << from << ",\"to\":" << to
                             << ",\"duration\":" << duration
                             << ",\"segVol\":" << segment_volume
                             << ",\"junctionVol\":" << junction_volume
                             << ",\"emitOriginNow\":"
                             << (emit_origin_now ? "true" : "false")
                             << ",\"emitJunctionOrigin\":"
                             << (emit_junction ? "true" : "false")
                             << ",\"afterCmd\":"
                             << (to != *target_count ? "true" : "false")
                             << ",\"token\":\""
                             << volumeCharacter(segment_volume);
                        if (duration != 1) {
                            data << "=" << duration;
                        }
                        data << "\"}";
                        agentDbg(
                            "H3",
                            "mgs_envelope_io.cpp:segment",
                            "ramp segment emitted",
                            data.str());
                    }
                    // #endregion
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
        // #region agent log
        if (states[cursor].volume
            && (states[cursor].volume->automatic
                || (next < states.size()
                    && states[next].volume
                    && states[next].volume->automatic))) {
            std::ostringstream data;
            data << "{\"cursor\":" << cursor
                 << ",\"next\":" << next
                 << ",\"hold\":" << (next - cursor)
                 << ",\"currentVol\":" << current_volume
                 << ",\"cursorAuto\":"
                 << (states[cursor].volume->automatic ? "true" : "false")
                 << "}";
            agentDbg(
                "H5",
                "mgs_envelope_io.cpp:append_hold",
                "hold consumed span that may include later automatic",
                data.str());
        }
        // #endregion
        appendHold(tokens, current_volume, next - cursor);
        cursor = next;
    }

    if (looping) {
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
    result.body = ",," + joinTokens(tokens);
    result.compiled_bytes = compiledEnvelopeBytes(tokens);
    // #region agent log
    {
        bool any_automatic = false;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.automatic) {
                any_automatic = true;
                break;
            }
        }
        if (any_automatic) {
            std::string escaped = result.body;
            for (auto& ch : escaped) {
                if (ch == '"') {
                    ch = '\'';
                }
            }
            std::ostringstream data;
            data << "{\"body\":\"" << escaped
                 << "\",\"len\":" << timeline.length_counts
                 << ",\"def\":" << static_cast<int>(definition_number)
                 << "}";
            agentDbg(
                "H1",
                "mgs_envelope_io.cpp:result",
                "formatted @e body",
                data.str());
        }
    }
    // #endregion
    if (result.compiled_bytes > compiled_byte_limit) {
        result.issues.push_back(MgsEnvelopeIssue::DefinitionLengthExceeded);
        return result;
    }
    result.definition = wrapEnvelopeDefinition(
        definition_number,
        result.body,
        formatEnvelopePlaybackComment(composite_name, layer));
    return result;
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
    {
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
        const auto formatted = formatMgsCompositeEnvelope(
            probe, 0, compiled_byte_limit, numbers, library);
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
