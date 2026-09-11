#include "mgstc/engine/mgs_composite_io.hpp"

#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/opll_patch.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgstc::engine {
namespace {

using Definition = MgsSourceDefinition;

struct ParsedTrack {
    CompositeLayer layer;
    unsigned track{};
    unsigned envelope_number{};
    bool envelope_is_rate{};
    bool has_envelope{};
    std::optional<unsigned> base_number;
};

struct LayerComment {
    std::string name;
    std::optional<int> relative;
    std::optional<std::uint32_t> length;
    std::optional<bool> enabled;
    std::optional<bool> muted;
    std::optional<bool> solo;
};

using TimbreKey = std::pair<TimbreSource, unsigned>;

const char* sourceName(TimbreSource source) noexcept {
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

SccWaveform sccWaveformFromBytes(
    const std::array<std::uint8_t, 32>& bytes) noexcept {
    SccWaveform waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        waveform[index] = static_cast<std::int8_t>(bytes[index]);
    }
    return waveform;
}

std::array<std::uint8_t, 32> sccWaveformToBytes(
    const SccWaveform& waveform) noexcept {
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(waveform[index]);
    }
    return bytes;
}

std::string withoutComments(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool comment = false;
    for (const char character : text) {
        if (comment) {
            if (character == '\r' || character == '\n') {
                comment = false;
                result.push_back(character);
            }
            continue;
        }
        if (character == ';') {
            comment = true;
            result.push_back(' ');
        } else {
            result.push_back(character);
        }
    }
    return result;
}

}  // namespace

std::vector<MgsSourceDefinition> extractMgsSourceDefinitions(
    std::string_view source,
    char wanted_kind) {
    std::string cleaned;
    cleaned.reserve(source.size());
    bool comment = false;
    for (const char character : source) {
        if (comment) {
            if (character == '\r' || character == '\n') {
                comment = false;
                cleaned.push_back(character);
            }
            continue;
        }
        if (character == ';') {
            comment = true;
            cleaned.push_back(' ');
        } else {
            cleaned.push_back(character);
        }
    }
    std::vector<MgsSourceDefinition> result;
    for (std::size_t index = 0; index + 3 < cleaned.size(); ++index) {
        if (cleaned[index] != '@'
            || static_cast<char>(std::tolower(
                   static_cast<unsigned char>(cleaned[index + 1])))
                != wanted_kind) {
            continue;
        }
        std::size_t number_end = index + 2;
        while (number_end < cleaned.size()
               && std::isdigit(static_cast<unsigned char>(
                   cleaned[number_end]))) {
            ++number_end;
        }
        if (number_end == index + 2) {
            continue;
        }
        unsigned number{};
        const auto parsed = std::from_chars(
            cleaned.data() + index + 2,
            cleaned.data() + number_end,
            number);
        if (parsed.ec != std::errc{}) {
            continue;
        }
        std::size_t position = number_end;
        while (position < cleaned.size()
               && std::isspace(static_cast<unsigned char>(
                   cleaned[position]))) {
            ++position;
        }
        if (position >= cleaned.size() || cleaned[position] != '=') {
            continue;
        }
        position++;
        while (position < cleaned.size()
               && std::isspace(static_cast<unsigned char>(
                   cleaned[position]))) {
            ++position;
        }
        if (position >= cleaned.size() || cleaned[position] != '{') {
            continue;
        }
        const auto body_begin = ++position;
        const auto body_end = cleaned.find('}', body_begin);
        if (body_end == std::string::npos) {
            continue;
        }
        result.push_back({
            .kind = wanted_kind,
            .number = number,
            .body = cleaned.substr(body_begin, body_end - body_begin),
        });
        index = body_end;
    }
    return result;
}

namespace {

std::vector<Definition> definitions(
    std::string_view source,
    char wanted_kind) {
    return extractMgsSourceDefinitions(source, wanted_kind);
}

bool decimal(
    std::string_view text,
    int minimum,
    int maximum,
    int& value) {
    if (text.empty()) {
        return false;
    }
    int parsed{};
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != text.data() + text.size()
        || parsed < minimum
        || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

std::string hexText(std::string_view text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size() * 2);
    for (const unsigned char character : text) {
        result.push_back(digits[character >> 4]);
        result.push_back(digits[character & 15]);
    }
    return result;
}

std::optional<std::string> unhexText(std::string_view text) {
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(text.size() / 2);
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = digit(text[index]);
        const int low = digit(text[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::string trim(std::string text) {
    const auto not_space = [](unsigned char character) {
        return !std::isspace(character);
    };
    text.erase(
        text.begin(),
        std::find_if(text.begin(), text.end(), not_space));
    text.erase(
        std::find_if(
            text.rbegin(), text.rend(), not_space).base(),
        text.end());
    return text;
}

std::optional<int> integerAfter(
    std::string_view token,
    std::string_view prefix,
    int minimum,
    int maximum) {
    if (!token.starts_with(prefix)) {
        return std::nullopt;
    }
    int value{};
    if (!decimal(
            token.substr(prefix.size()), minimum, maximum, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<LayerComment> layerCommentFor(
    std::string_view line) {
    const auto marker = line.find("; MGSTC-LAYER");
    if (marker == std::string_view::npos) {
        return std::nullopt;
    }
    std::istringstream tokens{
        std::string(line.substr(marker + 13))};
    LayerComment result;
    std::string token;
    while (tokens >> token) {
        const auto separator = token.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = std::string_view(token).substr(0, separator);
        const auto value =
            std::string_view(token).substr(separator + 1);
        if (key == "name") {
            result.name = unhexText(value).value_or(std::string{});
        } else if (key == "relative") {
            int parsed{};
            if (decimal(value, -127, 127, parsed)) {
                result.relative = parsed;
            }
        } else if (key == "length") {
            int parsed{};
            if (decimal(
                    value,
                    1,
                    static_cast<int>(EnvelopeTimeline::kMaximumLengthCounts),
                    parsed)) {
                result.length = static_cast<std::uint32_t>(parsed);
            }
        } else if (key == "enabled" || key == "muted" || key == "solo") {
            int parsed{};
            if (decimal(value, 0, 1, parsed)) {
                const bool flag = parsed != 0;
                if (key == "enabled") {
                    result.enabled = flag;
                } else if (key == "muted") {
                    result.muted = flag;
                } else {
                    result.solo = flag;
                }
            }
        }
    }
    return result;
}

std::map<unsigned, LayerComment> layerComments(
    std::string_view source) {
    std::map<unsigned, LayerComment> result;
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const auto end = source.find('\n', begin);
        const auto line = source.substr(
            begin,
            end == std::string_view::npos
                ? source.size() - begin
                : end - begin);
        const auto marker = line.find("; MGSTC-LAYER");
        if (marker != std::string_view::npos) {
            std::istringstream tokens{
                std::string(line.substr(marker + 13))};
            std::string token;
            unsigned track{};
            bool has_track = false;
            while (tokens >> token) {
                const auto separator = token.find('=');
                if (separator == std::string::npos) {
                    continue;
                }
                if (token.substr(0, separator) == "track") {
                    int parsed{};
                    if (decimal(
                            std::string_view(token).substr(
                                separator + 1),
                            1, 17, parsed)) {
                        track = static_cast<unsigned>(parsed);
                        has_track = true;
                    }
                }
            }
            if (has_track) {
                if (const auto parsed = layerCommentFor(line)) {
                    result[track] = *parsed;
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

std::optional<TimbreSource> sourceForTrack(unsigned track) {
    if (track >= 1 && track <= 3) {
        return TimbreSource::Psg;
    }
    if (track >= 4 && track <= 8) {
        return TimbreSource::Scc;
    }
    if (track >= 9 && track <= 17) {
        return TimbreSource::Opll;
    }
    return std::nullopt;
}

std::uint8_t channelForTrack(unsigned track) {
    if (track <= 3) {
        return static_cast<std::uint8_t>(track - 1);
    }
    if (track <= 8) {
        return static_cast<std::uint8_t>(track - 4);
    }
    return static_cast<std::uint8_t>(track - 9);
}

std::uint64_t syntheticLibraryId(
    TimbreSource source,
    unsigned number) noexcept {
    return 0x4D47534300000000ULL
        | (static_cast<std::uint64_t>(source) << 16)
        | number + 1;
}

void addIssue(
    std::vector<std::string>& issues,
    std::string issue) {
    if (std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(std::move(issue));
    }
}

std::optional<SavedTimbreReference> findReference(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library,
    TimbreSource source,
    std::uint64_t id) {
    if (const auto* embedded = findEmbeddedTimbreSnapshot(timbre, id);
        embedded != nullptr && embedded->source == source) {
        return *embedded;
    }
    for (const auto& layer : timbre.layers) {
        if (layer.base_timbre
            && layer.base_timbre->library_id == id
            && layer.base_timbre->source == source) {
            return layer.base_timbre;
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == EnvelopeEventKind::Timbre
                && event.timbre_pick == TimbrePick::Library
                && event.target_library_id == id
                && layer.source == source) {
                if (library != nullptr) {
                    if (const auto* entry = library->find(id);
                        entry != nullptr
                        && ((source == TimbreSource::Scc
                             && entry->category == TimbreCategory::Scc)
                            || (source == TimbreSource::Opll
                                && entry->category
                                    == TimbreCategory::Opll))) {
                        return makeSavedTimbreReference(*entry);
                    }
                }
            }
        }
    }
    if (library != nullptr) {
        if (const auto* entry = library->find(id);
            entry != nullptr
            && ((source == TimbreSource::Scc
                 && entry->category == TimbreCategory::Scc)
                || (source == TimbreSource::Opll
                    && entry->category == TimbreCategory::Opll))) {
            return makeSavedTimbreReference(*entry);
        }
    }
    return std::nullopt;
}

std::string issueName(MgsEnvelopeIssue issue) {
    switch (issue) {
    case MgsEnvelopeIssue::InvalidDefinitionNumber:
        return "invalid envelope definition number";
    case MgsEnvelopeIssue::IncompleteLoop:
        return "incomplete envelope loop";
    case MgsEnvelopeIssue::InvalidLoopRange:
        return "invalid envelope loop range";
    case MgsEnvelopeIssue::DefinitionLengthExceeded:
        return "envelope definition exceeds MGSC length";
    case MgsEnvelopeIssue::InvalidVolume:
        return "invalid envelope volume";
    case MgsEnvelopeIssue::InvalidPitch:
        return "invalid envelope pitch";
    case MgsEnvelopeIssue::InvalidTimbre:
        return "unresolved envelope timbre";
    case MgsEnvelopeIssue::InvalidRegisterWrite:
        return "invalid envelope register write";
    case MgsEnvelopeIssue::InvalidAutomaticVolumeDuration:
        return "automatic envelope ramp exceeds one-byte duration";
    }
    return "invalid envelope";
}

std::string singleLineName(std::string_view text) {
    std::string result(text);
    std::replace(result.begin(), result.end(), '\r', ' ');
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
}

std::string rateVoiceName(const CompositeLayer& layer) {
    if (!layer.name.empty()) {
        return singleLineName(layer.name);
    }
    if (layer.base_timbre && !layer.base_timbre->name.empty()) {
        return singleLineName(layer.base_timbre->name);
    }
    return {};
}

std::string rateDefinition(
    const CompositeLayer& layer,
    unsigned number) {
    const auto rate = clampRateEnvelope(layer.volume_envelope.rate);
    const bool psg = layer.source == TimbreSource::Psg;
    std::ostringstream output;
    output << "@r" << number
           << " = { "
           << static_cast<unsigned>(psg ? rate.tone_mode : 0) << ", "
           << static_cast<unsigned>(psg ? rate.noise : 0) << ", "
           << static_cast<unsigned>(rate.attack_level) << ", "
           << static_cast<unsigned>(rate.attack_rate) << ", "
           << static_cast<unsigned>(rate.decay_rate) << ", "
           << static_cast<unsigned>(rate.sustain_level) << ", "
           << static_cast<unsigned>(rate.sustain_rate) << ", "
           << static_cast<unsigned>(rate.release_rate)
           << " }";
    const auto name = rateVoiceName(layer);
    if (!name.empty()) {
        output << " ; " << name;
    }
    output << "\r\n";
    return output.str();
}

bool parseRate(
    const Definition& definition,
    RateEnvelope& rate) {
    std::string body = definition.body;
    std::replace(body.begin(), body.end(), ',', ' ');
    std::istringstream stream(body);
    std::array<int, 8> values{};
    for (auto& value : values) {
        if (!(stream >> value) || value < 0 || value > 255) {
            return false;
        }
    }
    std::string extra;
    if (stream >> extra) {
        return false;
    }
    rate = clampRateEnvelope({
        .tone_mode = static_cast<std::uint8_t>(values[0]),
        .noise = static_cast<std::uint8_t>(values[1]),
        .attack_level = static_cast<std::uint8_t>(values[2]),
        .attack_rate = static_cast<std::uint8_t>(values[3]),
        .decay_rate = static_cast<std::uint8_t>(values[4]),
        .sustain_level = static_cast<std::uint8_t>(values[5]),
        .sustain_rate = static_cast<std::uint8_t>(values[6]),
        .release_rate = static_cast<std::uint8_t>(values[7]),
    });
    return true;
}

bool parseSequence(
    const Definition& definition,
    CompositeLayer& layer,
    std::vector<std::string>& issues) {
    const auto body = definition.body;
    std::size_t position = 0;
    std::uint32_t count = 0;
    std::optional<std::uint32_t> loop_start;
    std::optional<std::uint32_t> loop_end;
    bool after_loop_start = false;
    std::optional<std::uint32_t> last_single_volume_count;
    std::vector<EnvelopeEvent> volume;
    std::vector<EnvelopeEvent> pitch;
    std::vector<EnvelopeEvent> timbre;
    const auto skip = [&] {
        while (position < body.size()
               && (std::isspace(static_cast<unsigned char>(
                       body[position]))
                   || body[position] == '.'
                   || body[position] == ',')) {
            ++position;
        }
    };
    while (position < body.size()) {
        skip();
        if (position >= body.size()) {
            break;
        }
        const char character = body[position];
        if (character == '[') {
            if (loop_start) {
                addIssue(issues, "duplicate envelope loop start");
            } else {
                loop_start = count;
                after_loop_start = true;
            }
            ++position;
            continue;
        }
        if (character == ']') {
            if (loop_end) {
                addIssue(issues, "duplicate envelope loop end");
            } else {
                loop_end = count;
            }
            ++position;
            continue;
        }
        if (character == '@') {
            ++position;
            const auto begin = position;
            while (position < body.size()
                   && std::isdigit(static_cast<unsigned char>(
                       body[position]))) {
                ++position;
            }
            int number{};
            if (begin == position
                || !decimal(
                    std::string_view(body).substr(
                        begin, position - begin),
                    0, 31, number)) {
                addIssue(issues, "invalid envelope timbre command");
                continue;
            }
            timbre.push_back({
                .kind = EnvelopeEventKind::Timbre,
                .value = number,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (character == '\\') {
            ++position;
            const auto begin = position;
            if (position < body.size()
                && (body[position] == '+' || body[position] == '-')) {
                ++position;
            }
            while (position < body.size()
                   && std::isdigit(static_cast<unsigned char>(
                       body[position]))) {
                ++position;
            }
            int value{};
            if (begin == position
                || !decimal(
                    std::string_view(body).substr(
                        begin, position - begin),
                    -127, 127, value)) {
                addIssue(issues, "invalid envelope pitch command");
                continue;
            }
            pitch.push_back({
                .kind = EnvelopeEventKind::Pitch,
                .value = value,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (character == 'y' || character == 'Y') {
            ++position;
            const auto register_begin = position;
            while (position < body.size()
                   && std::isdigit(static_cast<unsigned char>(
                       body[position]))) {
                ++position;
            }
            if (position >= body.size() || body[position] != ',') {
                addIssue(issues, "invalid envelope register command");
                continue;
            }
            int reg{};
            if (!decimal(
                    std::string_view(body).substr(
                        register_begin,
                        position - register_begin),
                    0, 56, reg)) {
                addIssue(issues, "invalid envelope register command");
                continue;
            }
            ++position;
            const auto value_begin = position;
            while (position < body.size()
                   && std::isdigit(static_cast<unsigned char>(
                       body[position]))) {
                ++position;
            }
            int value{};
            if (!decimal(
                    std::string_view(body).substr(
                        value_begin, position - value_begin),
                    0, 255, value)) {
                addIssue(issues, "invalid envelope register command");
                continue;
            }
            timbre.push_back({
                .kind = EnvelopeEventKind::RegisterWrite,
                .value = reg,
                .secondary = value,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (std::isxdigit(static_cast<unsigned char>(character))) {
            const int value = std::isdigit(static_cast<unsigned char>(
                                                character))
                ? character - '0'
                : std::tolower(static_cast<unsigned char>(character))
                    - 'a' + 10;
            ++position;
            std::uint32_t hold = 1;
            bool automatic = false;
            if (position < body.size()
                && (body[position] == ':' || body[position] == '=')) {
                automatic = body[position] == '=';
                ++position;
                const auto begin = position;
                while (position < body.size()
                       && std::isdigit(static_cast<unsigned char>(
                           body[position]))) {
                    ++position;
                }
                int parsed{};
                if (!decimal(
                        std::string_view(body).substr(
                            begin, position - begin),
                        1, 65'535, parsed)) {
                    addIssue(issues, "invalid envelope hold");
                    continue;
                }
                hold = static_cast<std::uint32_t>(parsed);
                if (hold == 240) {
                    addIssue(issues, "invalid envelope hold");
                    continue;
                }
                if (automatic && hold > 255) {
                    addIssue(
                        issues,
                        "automatic envelope ramp exceeds one-byte duration");
                    continue;
                }
            }
            if (automatic) {
                // `f=cc` starts at the current execution position and
                // reaches its target after `cc` ticks.  A preceding 1-count
                // volume (`f.8=10`) is the ramp origin, so the authoring
                // model keeps the target at origin + cc (not origin + 1 + cc).
                auto origin = count;
                if (last_single_volume_count
                    && count == *last_single_volume_count + 1) {
                    origin = *last_single_volume_count;
                }
                count = std::min<std::uint32_t>(
                    EnvelopeTimeline::kMaximumLengthCounts,
                    origin + hold);
                volume.push_back({
                    .kind = EnvelopeEventKind::Volume,
                    .value = value,
                    .count = count,
                    .automatic = true,
                });
                last_single_volume_count.reset();
            } else {
                const bool redundant_ramp_target =
                    !volume.empty()
                    && volume.back().kind
                        == EnvelopeEventKind::Volume
                    && volume.back().count == count
                    && volume.back().automatic
                    && volume.back().value == value;
                const auto hold_start = count;
                if (!redundant_ramp_target) {
                    volume.push_back({
                        .kind = EnvelopeEventKind::Volume,
                        .value = value,
                        .count = count,
                    });
                }
                count = std::min<std::uint32_t>(
                    EnvelopeTimeline::kMaximumLengthCounts,
                    count + hold);
                if (hold == 1) {
                    last_single_volume_count = hold_start;
                } else {
                    last_single_volume_count.reset();
                }
            }
            continue;
        }
        addIssue(issues, "unknown MGSC envelope token");
        ++position;
    }
    if (loop_start.has_value() != loop_end.has_value()
        || (loop_start && loop_end && *loop_start >= *loop_end)) {
        addIssue(issues, "invalid envelope loop");
    }
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.volume_envelope.events = std::move(volume);
    layer.pitch_envelope.events = std::move(pitch);
    layer.timbre_automation = std::move(timbre);
    layer.envelope_timeline = {
        .length_counts = std::max<std::uint32_t>(1, count),
        .loop_start_count = loop_start,
        .loop_end_count = loop_end,
    };
    return issues.empty();
}

std::optional<ParsedTrack> parseTrackLine(
    std::string_view line,
    std::vector<std::string>& issues) {
    const auto comment = line.find(';');
    const auto code = trim(std::string(line.substr(
        0, comment == std::string_view::npos ? line.size() : comment)));
    if (code.empty() || !std::isdigit(static_cast<unsigned char>(code[0]))) {
        return std::nullopt;
    }
    std::istringstream stream(code);
    unsigned track{};
    if (!(stream >> track)) {
        return std::nullopt;
    }
    const auto source = sourceForTrack(track);
    if (!source) {
        addIssue(issues, "track number is outside 1-17");
        return std::nullopt;
    }
    ParsedTrack result;
    result.track = track;
    result.layer.source = *source;
    result.layer.channel = channelForTrack(track);
    result.layer.enabled = true;
    result.layer.name =
        std::string(sourceName(*source)) + " Ch."
        + std::to_string(result.layer.channel + 1);
    std::string token;
    while (stream >> token) {
        if (const auto volume_number = integerAfter(token, "v", 0, 15)) {
            result.layer.volume =
                static_cast<std::uint8_t>(*volume_number);
        } else if (const auto key_off_hang_number = integerAfter(
                       token, "k", 0, 255)) {
            result.layer.key_off_hang =
                static_cast<std::uint8_t>(*key_off_hang_number);
        } else if (token == "so") {
            result.layer.opll_sustain = true;
        } else if (token == "sf") {
            result.layer.opll_sustain = false;
        } else if (const auto sequence_number = integerAfter(
                       token, "@e", 0, 31)) {
            result.envelope_number =
                static_cast<unsigned>(*sequence_number);
            result.envelope_is_rate = false;
            result.has_envelope = true;
        } else if (const auto rate_number = integerAfter(
                       token, "@r", 0, 31)) {
            result.envelope_number =
                static_cast<unsigned>(*rate_number);
            result.envelope_is_rate = true;
            result.has_envelope = true;
        } else if (token.starts_with("@\\")) {
            int parsed_value{};
            if (decimal(
                    token.substr(2), -32768, 32767, parsed_value)) {
                result.layer.micro_detune = parsed_value;
            } else {
                addIssue(issues, "invalid track micro-detune");
            }
        } else if (const auto extra_roughness_number = integerAfter(
                       token, "@p", -32768, 32767)) {
            result.layer.software_lfo.extra_roughness =
                *extra_roughness_number;
        } else if (token.starts_with("@")) {
            if (const auto track_timbre_number = integerAfter(
                    token, "@", 0, 31)) {
                result.base_number =
                    static_cast<unsigned>(*track_timbre_number);
            } else {
                addIssue(issues, "invalid track timbre command");
            }
        } else if (token.starts_with("\\")) {
            int parsed_value{};
            if (decimal(
                    token.substr(1), -32768, 32767, parsed_value)) {
                result.layer.detune =
                    static_cast<std::int16_t>(parsed_value);
            } else {
                addIssue(issues, "invalid track detune");
            }
        } else if (const auto pitch_sweep_number = integerAfter(
                       token, "p", 0, 255)) {
            result.layer.pitch_sweep = {
                .enabled = true,
                .value = static_cast<std::uint8_t>(*pitch_sweep_number),
            };
        } else if (token.starts_with("h")) {
            std::string values = token.substr(1);
            std::replace(values.begin(), values.end(), ',', ' ');
            std::istringstream parameters(values);
            std::array<int, 4> parsed{};
            bool valid = true;
            for (auto& component : parsed) {
                valid = valid
                    && static_cast<bool>(parameters >> component);
            }
            if (!valid || parsed[0] < 0 || parsed[0] > 255
                || parsed[1] < 0 || parsed[1] > 127
                || parsed[2] < 0 || parsed[2] > 255
                || parsed[3] < -127 || parsed[3] > 127) {
                addIssue(issues, "invalid track LFO");
            } else {
                result.layer.software_lfo = {
                    .enabled = true,
                    .delay = static_cast<std::uint8_t>(parsed[0]),
                    .depth = static_cast<std::uint8_t>(parsed[1]),
                    .speed = static_cast<std::uint8_t>(parsed[2]),
                    .roughness = static_cast<std::int8_t>(parsed[3]),
                };
            }
        } else if (token.starts_with("r%")) {
            if (const auto absolute_rest_number = integerAfter(
                    token, "r%", 0, 65535)) {
                result.layer.start_delay_form = StartDelayForm::AbsoluteTicks;
                result.layer.start_delay_value =
                    static_cast<std::uint32_t>(*absolute_rest_number);
            } else {
                addIssue(issues, "invalid absolute track rest");
            }
        } else if (token.starts_with("r")) {
            if (const auto note_rest_number = integerAfter(
                    token, "r", 0, 65535)) {
                result.layer.start_delay_form = StartDelayForm::NoteLength;
                result.layer.start_delay_value =
                    static_cast<std::uint32_t>(*note_rest_number);
            } else {
                addIssue(issues, "invalid track rest");
            }
        } else {
            addIssue(issues, "unknown MGSC track token");
        }
    }
    if (!result.has_envelope) {
        addIssue(issues, "track has no @e or @r envelope");
    }
    return result;
}

}  // namespace

bool parseMgsRateBody(std::string_view body, RateEnvelope& rate) {
    Definition definition;
    definition.body = std::string(body);
    return parseRate(definition, rate);
}

bool parseMgsSequenceBody(
    std::string_view body,
    CompositeLayer& layer,
    std::vector<std::string>& issues) {
    Definition definition;
    definition.body = std::string(body);
    return parseSequence(definition, layer, issues);
}

std::string formatMgsRateDefinition(
    const CompositeLayer& layer,
    unsigned number) {
    return rateDefinition(layer, number);
}

MgsCompositeIoResult formatMgsComposite(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library) {
    MgsCompositeIoResult result;
    const auto validation = validateCompositeTimbre(timbre);
    for (const auto& warning : validation.warnings) {
        result.issues.push_back(warning);
    }
    const auto numbers = resolveTimbreNumbers(timbre);
    for (const auto& warning : numbers.warnings) {
        addIssue(result.issues, warning);
    }
    if (!result.valid()) {
        return result;
    }

    std::ostringstream output;
    output << "; #opll_mode 0\r\n"
           << "; #tempo " << std::clamp(
               timbre.playback_tempo,
               kMgscTempoMin,
               kMgscTempoMax)
           << "\r\n"
           << "; MGSTC-COMPOSITE name=" << hexText(timbre.name)
           << "\r\n\r\n";

    std::set<TimbreKey> emitted;
    for (const auto& assignment : numbers.assignments) {
        TimbreSource source = TimbreSource::Psg;
        const auto find_source = [&](
                                      const CompositeLayer& layer) {
            if (layer.base_timbre
                && layer.base_timbre->library_id
                    == assignment.library_id) {
                source = layer.source;
                return true;
            }
            for (const auto& event : layer.timbre_automation) {
                if (event.kind == EnvelopeEventKind::Timbre
                    && event.target_library_id
                        == assignment.library_id) {
                    source = layer.source;
                    return true;
                }
            }
            return false;
        };
        for (const auto& layer : timbre.layers) {
            if (find_source(layer)) {
                break;
            }
        }
        if (source == TimbreSource::Psg) {
            addIssue(result.issues, "PSG cannot have a custom MGSC timbre");
            continue;
        }
        const TimbreKey key{source, assignment.number};
        if (!emitted.insert(key).second) {
            continue;
        }
        const auto reference = findReference(
            timbre, library, source, assignment.library_id);
        if (!reference) {
            addIssue(
                result.issues,
                "unresolved library timbre "
                    + std::to_string(assignment.library_id));
            continue;
        }
        if (source == TimbreSource::Scc) {
            output << formatMgsSccDefinition(
                sccWaveformFromBytes(reference->scc_waveform),
                assignment.number,
                reference->name);
        } else {
            output << formatMgsOpllDefinition(
                decodeOpllPatch(reference->opll_registers),
                assignment.number,
                reference->name);
        }
    }
    if (!result.valid()) {
        return result;
    }

    output << "\r\n";
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        const auto& layer = timbre.layers[index];
        const auto number = layer.envelope_number;
        if (layer.volume_envelope.kind == EnvelopeKind::Rate) {
            output << rateDefinition(layer, number);
        } else {
            const auto formatted = formatMgsCompositeEnvelope(
                layer, number, kMgscEnvelopeCompiledByteLimit, &numbers, library,
                timbre.name);
            if (!formatted.valid()) {
                for (const auto issue : formatted.issues) {
                    addIssue(
                        result.issues,
                        "layer " + std::to_string(index + 1)
                            + ": " + issueName(issue));
                }
            } else {
                output << formatted.definition;
            }
        }
    }
    if (!result.valid()) {
        return result;
    }

    output << "\r\n";
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        const auto& layer = timbre.layers[index];
        const unsigned track =
            layer.source == TimbreSource::Psg
                ? 1U + layer.channel
            : layer.source == TimbreSource::Scc
                ? 4U + layer.channel
                : 9U + layer.channel;
        output << formatMgsCompositeTrackSetup(layer, &numbers)
               << " ; MGSTC-LAYER track=" << track
               << " name=" << hexText(layer.name)
               << " relative=" << static_cast<int>(
                   layer.relative_semitones)
               << " length=" << layer.envelope_timeline.length_counts
               << " enabled=" << (layer.enabled ? 1 : 0)
               << " muted=" << (layer.muted ? 1 : 0)
               << " solo=" << (layer.solo ? 1 : 0)
               << "\r\n";
    }
    result.source = output.str();
    return result;
}

ParsedMgsComposite parseMgsComposite(std::string_view source) {
    ParsedMgsComposite result;
    result.timbre = defaultCompositeTimbre();
    result.timbre.layers.clear();

    const auto composite_marker = source.find(
        "; MGSTC-COMPOSITE name=");
    if (composite_marker != std::string_view::npos) {
        const auto begin = composite_marker + 23;
        const auto end = source.find_first_of("\r\n", begin);
        if (const auto name = unhexText(source.substr(
                begin,
                end == std::string_view::npos
                    ? source.size() - begin
                    : end - begin))) {
            result.timbre.name = *name;
        }
    }
    std::size_t line_begin = 0;
    while (line_begin <= source.size()) {
        const auto line_end = source.find('\n', line_begin);
        const auto line = source.substr(
            line_begin,
            line_end == std::string_view::npos
                ? source.size() - line_begin
                : line_end - line_begin);
        const auto code = trim(std::string(line));
        auto directive = code;
        if (directive.starts_with(';')) {
            directive = trim(directive.substr(1));
        }
        if (directive.starts_with("#tempo")) {
            std::istringstream stream(directive.substr(6));
            int tempo{};
            if (!(stream >> tempo)
                || tempo < kMgscTempoMin
                || tempo > kMgscTempoMax) {
                addIssue(result.issues, "invalid MGSC tempo");
            } else {
                result.timbre.playback_tempo = tempo;
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
    }

    std::map<TimbreKey, SavedTimbreReference> timbres;
    for (const auto& definition : definitions(source, 's')) {
        if (definition.number > 31
            || timbres.contains({TimbreSource::Scc, definition.number})) {
            addIssue(result.issues, "duplicate or invalid @s definition");
            continue;
        }
        const auto parsed = parseMgsSccDefinition(
            "@s" + std::to_string(definition.number)
            + " = {" + definition.body + "}");
        if (!parsed) {
            addIssue(result.issues, "invalid @s definition");
            continue;
        }
        SavedTimbreReference reference;
        reference.library_id = syntheticLibraryId(
            TimbreSource::Scc, definition.number);
        reference.name = "MGSC SCC @"
            + std::to_string(definition.number);
        reference.source = TimbreSource::Scc;
        reference.number_mode = TimbreNumberMode::Manual;
        reference.manual_number =
            static_cast<std::uint8_t>(definition.number);
        reference.scc_waveform =
            sccWaveformToBytes(parsed->waveform);
        timbres.emplace(
            TimbreKey{TimbreSource::Scc, definition.number},
            std::move(reference));
    }
    for (const auto& definition : definitions(source, 'v')) {
        if (definition.number > 31
            || timbres.contains({TimbreSource::Opll, definition.number})) {
            addIssue(result.issues, "duplicate or invalid @v definition");
            continue;
        }
        const auto parsed = parseMgsOpllDefinition(
            "@v" + std::to_string(definition.number)
            + " = {" + definition.body + "}");
        if (!parsed) {
            addIssue(result.issues, "invalid @v definition");
            continue;
        }
        SavedTimbreReference reference;
        reference.library_id = syntheticLibraryId(
            TimbreSource::Opll, definition.number);
        reference.name = "MGSC OPLL @"
            + std::to_string(definition.number);
        reference.source = TimbreSource::Opll;
        reference.number_mode = TimbreNumberMode::Manual;
        reference.manual_number =
            static_cast<std::uint8_t>(definition.number);
        reference.opll_registers = encodeOpllPatch(parsed->patch);
        timbres.emplace(
            TimbreKey{TimbreSource::Opll, definition.number},
            std::move(reference));
    }

    std::map<unsigned, Definition> envelopes;
    for (const auto& definition : definitions(source, 'e')) {
        if (definition.number > 31
            || envelopes.contains(definition.number)) {
            addIssue(result.issues, "duplicate or invalid @e definition");
            continue;
        }
        envelopes.emplace(definition.number, definition);
    }
    for (const auto& definition : definitions(source, 'r')) {
        if (definition.number > 31
            || envelopes.contains(definition.number)) {
            addIssue(result.issues, "duplicate or invalid @r definition");
            continue;
        }
        envelopes.emplace(definition.number, definition);
    }

    const auto comments = layerComments(source);
    std::set<unsigned> used_tracks;
    bool in_definition = false;
    line_begin = 0;
    while (line_begin <= source.size()) {
        const auto line_end = source.find('\n', line_begin);
        const auto line = source.substr(
            line_begin,
            line_end == std::string_view::npos
                ? source.size() - line_begin
                : line_end - line_begin);
        const auto code = trim(std::string(line));
        if (in_definition) {
            if (code.find('}') != std::string::npos) {
                in_definition = false;
            }
        } else if (
            code.starts_with("@")
            && code.find('=') != std::string::npos
            && code.find('{') != std::string::npos) {
            in_definition =
                code.find('}') == std::string::npos;
        } else {
            const auto parsed = parseTrackLine(line, result.issues);
            if (parsed) {
                if (!used_tracks.insert(parsed->track).second) {
                    addIssue(result.issues, "duplicate MGSC track");
                } else {
                    auto track = *parsed;
                    if (const auto comment = comments.find(track.track);
                        comment != comments.end()) {
                        const auto& metadata = comment->second;
                        if (!metadata.name.empty()) {
                            track.layer.name = metadata.name;
                        }
                        if (metadata.relative) {
                            track.layer.relative_semitones =
                                static_cast<std::int8_t>(*metadata.relative);
                        }
                        if (metadata.enabled) {
                            track.layer.enabled = *metadata.enabled;
                        }
                        if (metadata.muted) {
                            track.layer.muted = *metadata.muted;
                        }
                        if (metadata.solo) {
                            track.layer.solo = *metadata.solo;
                        }
                    }
                    const auto envelope = envelopes.find(
                        track.envelope_number);
                    if (envelope == envelopes.end()) {
                        addIssue(
                            result.issues,
                            "track references undefined envelope");
                    } else if (track.envelope_is_rate) {
                        track.layer.volume_envelope.kind =
                            EnvelopeKind::Rate;
                        if (!parseRate(
                                envelope->second,
                                track.layer.volume_envelope.rate)) {
                            addIssue(result.issues, "invalid @r definition");
                        }
                    } else {
                        parseSequence(
                            envelope->second,
                            track.layer,
                            result.issues);
                    }
                    if (const auto metadata = comments.find(track.track);
                        metadata != comments.end()
                        && metadata->second.length) {
                        track.layer.envelope_timeline.length_counts =
                            *metadata->second.length;
                    }
                    if (track.base_number) {
                        if (track.layer.source == TimbreSource::Opll
                            && *track.base_number <= 14) {
                            track.layer.base_opll_rom =
                                static_cast<std::uint8_t>(*track.base_number);
                        } else {
                            const TimbreKey key{
                                track.layer.source, *track.base_number};
                            const auto timbre = timbres.find(key);
                            if (timbre == timbres.end()) {
                                addIssue(
                                    result.issues,
                                    "track references undefined timbre");
                            } else {
                                track.layer.base_timbre = timbre->second;
                            }
                        }
                    }
                    for (auto& event : track.layer.timbre_automation) {
                        if (event.kind != EnvelopeEventKind::Timbre) {
                            continue;
                        }
                        if (track.layer.source == TimbreSource::Opll
                            && event.value <= 14) {
                            event.timbre_pick = TimbrePick::OpllRom;
                            continue;
                        }
                        const auto timbre = timbres.find({
                            track.layer.source,
                            static_cast<unsigned>(event.value)});
                        if (timbre == timbres.end()) {
                            addIssue(
                                result.issues,
                                "envelope references undefined timbre");
                        } else {
                            event.timbre_pick = TimbrePick::Library;
                            event.target_library_id =
                                timbre->second.library_id;
                        }
                    }
                    result.timbre.layers.push_back(std::move(track.layer));
                }
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
    }
    if (result.timbre.layers.empty()) {
        addIssue(result.issues, "MGSC source has no numbered tracks");
    }
    if (result.issues.empty()) {
        result.timbre.format_version = CompositeTimbre::kFormatVersion;
    }
    return result;
}

}  // namespace mgstc::engine
