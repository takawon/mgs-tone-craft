#include "mgstc/engine/mgs_envelope_io.hpp"

#include "mgstc/engine/opll_register_auto.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>

namespace mgstc::engine {
namespace {

struct CountState {
    std::vector<std::string> timbre_tokens;
    std::vector<std::string> pitch_tokens;
    std::optional<std::int32_t> volume;

    [[nodiscard]] bool boundary() const noexcept {
        return !timbre_tokens.empty()
            || !pitch_tokens.empty()
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

}  // namespace

bool MgsEnvelopeFormatResult::hasIssue(
    MgsEnvelopeIssue issue) const noexcept {
    return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

MgsEnvelopeFormatResult formatMgsCompositeEnvelope(
    const CompositeLayer& layer,
    std::uint8_t definition_number,
    std::size_t body_length_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library) {
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
        states[event.count].volume = event.value;
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
        appendToken(
            states[event.count].pitch_tokens,
            "\\" + std::to_string(event.value));
    }

    for (const auto& event : layer.timbre_automation) {
        if (event.count > effective_end
            || (looping && event.count == effective_end)) {
            continue;
        }
        if (event.kind == EnvelopeEventKind::Timbre) {
            const auto number = envelopeEventTimbreNumber(event, numbers);
            if (!number) {
                addIssueOnce(result, MgsEnvelopeIssue::InvalidTimbre);
                continue;
            }
            appendToken(
                states[event.count].timbre_tokens,
                "@" + std::to_string(*number));
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
                states[event.count].timbre_tokens,
                "y" + std::to_string(event.value)
                    + "," + std::to_string(event.secondary));
        }
    }
    // TL/FB auto authoring expands to standard yreg,data after manual y
    // at the same count (§7.5). Packs from the active original's register
    // image (base / @-slide + prior manual y). Skips ROM intervals.
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
                states[event.count].timbre_tokens,
                "y" + std::to_string(event.value)
                    + "," + std::to_string(event.secondary));
        }
    }
    // §6.5.1: when the envelope slides to another patch, prepend the
    // track-side base `@` at count 0 (unless already present).
    if (layerEnvelopeHasPatchSlide(layer, numbers)) {
        if (const auto base = layerBasePatchNumber(layer, numbers)) {
            const auto token = "@" + std::to_string(*base);
            auto& leading = states[0].timbre_tokens;
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
        states[0].volume = layer.volume;
    }

    std::vector<std::string> tokens;
    std::int32_t current_volume = layer.volume;
    std::uint32_t cursor{};
    while (cursor < effective_end) {
        if (looping && cursor == *timeline.loop_start_count) {
            appendToken(tokens, "[");
        }
        for (const auto& token : states[cursor].timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[cursor].pitch_tokens) {
            appendToken(tokens, token);
        }
        if (states[cursor].volume) {
            current_volume = *states[cursor].volume;
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
        appendToken(tokens, "]");
    } else {
        for (const auto& token : states[effective_end].timbre_tokens) {
            appendToken(tokens, token);
        }
        for (const auto& token : states[effective_end].pitch_tokens) {
            appendToken(tokens, token);
        }
        if (states[effective_end].volume) {
            appendHold(tokens, *states[effective_end].volume, 1);
        }
    }

    result.body = ",," + joinTokens(tokens);
    if (result.body.size() > body_length_limit) {
        result.issues.push_back(MgsEnvelopeIssue::DefinitionLengthExceeded);
        return result;
    }
    result.definition = "@e" + std::to_string(definition_number)
        + " = { " + result.body + " }\r\n";
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
    if (const auto number = layerBasePatchNumber(layer, numbers)) {
        line += " @" + std::to_string(static_cast<unsigned>(*number));
    }
    if (layer.detune != 0) {
        line += " \\" + std::to_string(static_cast<int>(layer.detune));
    }
    if (layer.micro_detune != 0) {
        line += " @\\"
            + std::to_string(static_cast<int>(layer.micro_detune));
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
    std::size_t body_length_limit,
    const TimbreNumberResolution* numbers,
    const TimbreLibrary* library) {
    auto probe = layer;
    // Length capacity is independent of incomplete loop authoring errors.
    probe.envelope_timeline.loop_start_count.reset();
    probe.envelope_timeline.loop_end_count.reset();
    auto fits = [&](std::uint32_t length) {
        probe.envelope_timeline.length_counts = std::max<std::uint32_t>(1, length);
        const auto formatted = formatMgsCompositeEnvelope(
            probe, 0, body_length_limit, numbers, library);
        if (formatted.body.empty()) {
            return false;
        }
        return formatted.body.size() <= body_length_limit;
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
