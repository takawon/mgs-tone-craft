#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "tone_import_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mgstc::engine {
namespace {

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(
        bytes[at] | (static_cast<std::uint16_t>(bytes[at + 1]) << 8U));
}

bool isMgsMagic(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 6
        && bytes[0] == 'M'
        && bytes[1] == 'G'
        && bytes[2] == 'S'
        && std::isdigit(static_cast<unsigned char>(bytes[3]));
}

std::optional<std::size_t> findMgsEofMarker(
    std::span<const std::uint8_t> bytes) {
    for (std::size_t position = 0; position < bytes.size(); ++position) {
        if (bytes[position] == 0x1A) {
            return position;
        }
    }
    return std::nullopt;
}

// Byte after 0x1A. Present only for uncompressed MGS (that byte is 0).
std::optional<std::size_t> findMgsBinaryHeader(
    std::span<const std::uint8_t> bytes) {
    const auto marker = findMgsEofMarker(bytes);
    if (!marker || *marker + 1 >= bytes.size()) {
        return std::nullopt;
    }
    if (bytes[*marker + 1] != 0) {
        return std::nullopt;
    }
    return *marker + 1;
}

[[nodiscard]] bool lineStartsWithIgnoreCase(
    std::string_view text,
    std::string_view prefix) noexcept;

std::string stripPsgModeNoiseTokens(std::string_view body, bool& stripped) {
    stripped = false;
    std::string output;
    output.reserve(body.size());
    for (std::size_t position = 0; position < body.size();) {
        const char character = body[position];
        if ((character == 'n' || character == 'N')
            && position + 1 < body.size()
            && std::isdigit(static_cast<unsigned char>(body[position + 1]))) {
            stripped = true;
            ++position;
            while (position < body.size()
                   && std::isdigit(static_cast<unsigned char>(body[position]))) {
                ++position;
            }
            continue;
        }
        if (character == '/' || character == '*') {
            stripped = true;
            ++position;
            if (position < body.size()
                && body[position] >= '0'
                && body[position] <= '3') {
                ++position;
            }
            continue;
        }
        output.push_back(character);
        ++position;
    }
    return output;
}

std::string titleFromHeaderBytes(std::span<const std::uint8_t> bytes) {
    std::string decoded = tone_import_detail::decodeCp932Name(bytes);
    std::string title;
    std::size_t line_begin = 0;
    while (line_begin <= decoded.size()) {
        const auto line_end = decoded.find_first_of("\r\n", line_begin);
        const auto line = decoded.substr(
            line_begin,
            (line_end == std::string::npos ? decoded.size() : line_end)
                - line_begin);
        std::size_t start = 0;
        while (start < line.size()
               && std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        if (start < line.size() && line[start] != ';') {
            auto piece = tone_import_detail::collapseImportLabel(
                line.substr(start));
            if (!piece.empty()) {
                if (!title.empty()) {
                    title.push_back(' ');
                }
                title += piece;
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        line_begin = line_end + 1;
        if (line_end < decoded.size()
            && decoded[line_end] == '\r'
            && line_end + 1 < decoded.size()
            && decoded[line_end + 1] == '\n') {
            line_begin = line_end + 2;
        }
    }
    return title;
}

std::string extractMgsBinaryTitle(std::span<const std::uint8_t> bytes) {
    if (!isMgsMagic(bytes)) {
        return {};
    }
    const auto marker = findMgsEofMarker(bytes);
    if (!marker || *marker <= 4) {
        return {};
    }
    return titleFromHeaderBytes(bytes.subspan(4, *marker - 4));
}

std::string extractMgsMmlTitle(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    bool comment = false;
    for (const char character : text) {
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
    for (std::size_t index = 0; index + 6 <= cleaned.size(); ++index) {
        if (index > 0
            && !std::isspace(static_cast<unsigned char>(cleaned[index - 1]))) {
            continue;
        }
        if (!lineStartsWithIgnoreCase(
                std::string_view(cleaned).substr(index), "#title")) {
            continue;
        }
        auto position = index + 6;
        if (position < cleaned.size()
            && cleaned[position] != '{'
            && !std::isspace(static_cast<unsigned char>(cleaned[position]))) {
            continue;
        }
        while (position < cleaned.size()
               && std::isspace(static_cast<unsigned char>(cleaned[position]))) {
            ++position;
        }
        std::string body;
        if (position < cleaned.size() && cleaned[position] == '{') {
            ++position;
            const auto end = cleaned.find('}', position);
            body = cleaned.substr(
                position,
                end == std::string::npos ? std::string::npos : end - position);
        } else {
            const auto end = cleaned.find_first_of("\r\n", position);
            body = cleaned.substr(
                position,
                end == std::string::npos ? std::string::npos : end - position);
        }
        const auto title = titleFromHeaderBytes(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(body.data()),
                body.size()));
        if (!title.empty()) {
            return title;
        }
    }
    return {};
}

struct EnvelopeDef {
    unsigned number{};
    bool rate{};
    std::uint8_t mode{};
    std::uint8_t noise{};
    RateEnvelope rate_values{};
    std::vector<std::uint8_t> bytecode;
};

std::optional<TimbreSource> sourceForMusicTrack(unsigned track) {
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

constexpr unsigned kOpllRhythmTrackBegin = 15;
constexpr unsigned kOpllRhythmTrackEnd = 17;
constexpr std::uint8_t kOpllRhythmControlRegister = 0x0E;
constexpr std::uint8_t kOpllRhythmModeBit = 0x20;
constexpr std::uint8_t kDefaultNoteLength = 48;

[[nodiscard]] bool isOpllRhythmMusicTrack(unsigned track) noexcept {
    return track >= kOpllRhythmTrackBegin && track <= kOpllRhythmTrackEnd;
}

[[nodiscard]] bool isOpllRhythmControlWrite(
    unsigned track,
    unsigned register_number) noexcept {
    // MGS data typically writes y14 on OPLL ch 7/8/9 (tracks 15–17), not
    // melody ch 1–6. PSG y14 is a different chip and must not flip this bit.
    return isOpllRhythmMusicTrack(track)
        && register_number == kOpllRhythmControlRegister;
}

void markEnvelopeUse(
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage,
    unsigned envelope,
    TimbreSource source,
    std::optional<unsigned> patch) {
    auto& chips = usage[envelope];
    switch (source) {
    case TimbreSource::Psg:
        chips.psg = true;
        break;
    case TimbreSource::Scc:
        if (!chips.scc) {
            chips.primary_scc = patch;
        }
        chips.scc = true;
        if (patch) {
            chips.track_scc.insert(*patch);
        }
        break;
    case TimbreSource::Opll:
        if (!chips.opll) {
            chips.primary_opll = patch;
        }
        chips.opll = true;
        if (patch) {
            chips.track_opll.insert(*patch);
        }
        break;
    }
}

struct MusicUseEvent {
    enum class Kind : std::uint8_t { Rhythm, Patch, Envelope };

    std::uint32_t tick{};
    int track{};
    int seq{};
    Kind kind{Kind::Envelope};
    unsigned number{};
    bool rhythm_on{};
};

void applyMusicEnvelopeEvents(
    std::vector<MusicUseEvent>& events,
    bool rhythm_on,
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage,
    bool mml_source_order = false) {
    std::sort(
        events.begin(),
        events.end(),
        [mml_source_order](const MusicUseEvent& left, const MusicUseEvent& right) {
            if (left.tick != right.tick) {
                return left.tick < right.tick;
            }
            // Rhythm is chip-global. y14 and @e may sit on different of
            // OPLL ch 7/8/9, so apply every y14 at this tick before @e.
            if (left.kind != right.kind
                && (!mml_source_order || left.kind == MusicUseEvent::Kind::Rhythm
                    || right.kind == MusicUseEvent::Kind::Rhythm)) {
                return left.kind < right.kind;
            }
            if (left.track != right.track) {
                return left.track < right.track;
            }
            return left.seq < right.seq;
        });
    std::array<std::optional<unsigned>, 18> current_patch{};
    for (const auto& event : events) {
        if (event.kind == MusicUseEvent::Kind::Rhythm) {
            rhythm_on = event.rhythm_on;
            continue;
        }
        if (event.kind == MusicUseEvent::Kind::Patch) {
            if (event.track >= 1 && event.track <= 17) {
                current_patch[static_cast<std::size_t>(event.track)] =
                    event.number;
            }
            continue;
        }
        const auto source = sourceForMusicTrack(
            static_cast<unsigned>(event.track));
        if (!source) {
            continue;
        }
        if (isOpllRhythmMusicTrack(static_cast<unsigned>(event.track))
            && rhythm_on) {
            static_cast<void>(usage[event.number]);
            continue;
        }
        const std::optional<unsigned> patch =
            (event.track >= 1 && event.track <= 17)
                ? current_patch[static_cast<std::size_t>(event.track)]
                : std::nullopt;
        markEnvelopeUse(usage, event.number, *source, patch);
    }
}

std::size_t musicTrackCommandSize(std::uint8_t opcode) noexcept {
    if (opcode >= 0x20 && opcode <= 0x2C) {
        return 2;
    }
    if (opcode >= 0x30 && opcode <= 0x3C) {
        return 1;
    }
    if (opcode >= 0x80 && opcode <= 0x9F) {
        return 1;
    }
    if (opcode >= 0xC0 && opcode <= 0xCF) {
        return 1;
    }
    if (opcode >= 0xD0 && opcode <= 0xDF) {
        return 1;
    }
    if (opcode >= 0xE0 && opcode <= 0xE3) {
        return 1;
    }
    switch (opcode) {
    case 0x40:
        return 1;
    case 0x41:
        return 51;
    case 0x42:
    case 0x44:
    case 0x46:
    case 0x48:
    case 0x49:
    case 0x4B:
    case 0x4D:
    case 0x50:
    case 0x52:
    case 0x53:
    case 0x5A:
    case 0x5B:
    case 0x5F:
    case 0x60:
    case 0x61:
        return 2;
    case 0x4A:
    case 0x4E:
    case 0x4F:
    case 0x5D:
    case 0x63:
    case 0x64:
    case 0xFF:
        return 1;
    case 0x4C:
    case 0x55:
    case 0x58:
    case 0x5C:
        return 3;
    case 0x51:
        return 3;
    case 0x54:
        return 5;
    case 0x57:
    case 0x59:
        return 4;
    default:
        return 0;
    }
}

void collectBinaryMusicEnvelopeUse(
    std::span<const std::uint8_t> bytes,
    std::size_t header,
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage) {
    constexpr int kTrackCount = 18;
    std::array<std::size_t, kTrackCount> starts{};
    for (int track = 1; track < kTrackCount; ++track) {
        const auto offset = readU16(
            bytes, header + 4 + static_cast<std::size_t>(track) * 2);
        if (offset == 0) {
            continue;
        }
        starts[static_cast<std::size_t>(track)] =
            header + static_cast<std::size_t>(offset);
    }
    std::vector<MusicUseEvent> events;
    for (int track = 1; track < kTrackCount; ++track) {
        const auto source = sourceForMusicTrack(
            static_cast<unsigned>(track));
        const auto start = starts[static_cast<std::size_t>(track)];
        if (!source || start == 0 || start >= bytes.size()) {
            continue;
        }
        auto end = bytes.size();
        for (int later = track + 1; later < kTrackCount; ++later) {
            const auto later_start = starts[static_cast<std::size_t>(later)];
            if (later_start > start && later_start < end) {
                end = later_start;
            }
        }
        auto position = start;
        std::uint32_t tick = 0;
        std::uint8_t default_length = kDefaultNoteLength;
        int seq = 0;
        while (position < end) {
            const auto opcode = bytes[position];
            const auto size = musicTrackCommandSize(opcode);
            if (size == 0 || position + size > end) {
                break;
            }
            if (opcode >= 0x80 && opcode <= 0x9F) {
                events.push_back({
                    .tick = tick,
                    .track = track,
                    .seq = seq++,
                    .kind = MusicUseEvent::Kind::Patch,
                    .number = static_cast<unsigned>(opcode & 0x1F),
                });
            } else if (opcode == 0x49 && size >= 2) {
                events.push_back({
                    .tick = tick,
                    .track = track,
                    .seq = seq++,
                    .kind = MusicUseEvent::Kind::Envelope,
                    .number = static_cast<unsigned>(
                        bytes[position + 1] & 0x1F),
                });
            } else if (opcode == 0x5C && size >= 3
                       && isOpllRhythmControlWrite(
                           static_cast<unsigned>(track),
                           bytes[position + 1])) {
                // MGSDRV `y` (0x5C) on OPLL ch 7/8/9. Rhythm is 0x0E bit5,
                // including 9-voice songs that stay `#opll_mode 0`.
                events.push_back({
                    .tick = tick,
                    .track = track,
                    .seq = seq++,
                    .kind = MusicUseEvent::Kind::Rhythm,
                    .rhythm_on =
                        (bytes[position + 2] & kOpllRhythmModeBit) != 0,
                });
            } else if (opcode == 0x42 && size >= 2) {
                default_length = bytes[position + 1];
            } else if (opcode >= 0x20 && opcode <= 0x2C && size >= 2) {
                tick += bytes[position + 1];
            } else if (opcode >= 0x30 && opcode <= 0x3C) {
                tick += default_length;
            } else if (opcode == 0xFF) {
                break;
            }
            position += size;
        }
    }
    const bool header_rhythm =
        header + 1 < bytes.size() && (bytes[header + 1] & 0x01U) != 0U;
    applyMusicEnvelopeEvents(events, header_rhythm, usage);
}

[[nodiscard]] std::optional<unsigned> parseMmlInteger(
    std::string_view text,
    std::size_t& position) {
    while (position < text.size()
           && std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    if (position >= text.size()) {
        return std::nullopt;
    }
    int base = 10;
    if (text[position] == '$') {
        base = 16;
        ++position;
    } else if (
        position + 1 < text.size()
        && text[position] == '0'
        && (text[position + 1] == 'x' || text[position + 1] == 'X')) {
        base = 16;
        position += 2;
    }
    unsigned value = 0;
    bool digits = false;
    bool overflow = false;
    while (position < text.size()) {
        const auto character =
            static_cast<unsigned char>(text[position]);
        int digit = -1;
        if (std::isdigit(character)) {
            digit = character - '0';
        } else if (
            base == 16 && character >= 'a' && character <= 'f') {
            digit = character - 'a' + 10;
        } else if (
            base == 16 && character >= 'A' && character <= 'F') {
            digit = character - 'A' + 10;
        } else {
            break;
        }
        digits = true;
        const auto unsigned_digit = static_cast<unsigned>(digit);
        const auto unsigned_base = static_cast<unsigned>(base);
        if (!overflow) {
            if (value > (std::numeric_limits<unsigned>::max() - unsigned_digit) / unsigned_base)
                overflow = true;
            else value = value * unsigned_base + unsigned_digit;
        }
        ++position;
    }
    if (!digits || overflow) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool lineStartsWithIgnoreCase(
    std::string_view text,
    std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(text[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool mmlHeaderRhythmMode(std::string_view text) {
    std::size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const auto line_end = text.find_first_of("\r\n", line_begin);
        const auto line = text.substr(
            line_begin,
            (line_end == std::string_view::npos ? text.size() : line_end)
                - line_begin);
        const auto comment = line.find(';');
        auto code = line.substr(
            0, comment == std::string_view::npos ? line.size() : comment);
        std::size_t position = 0;
        while (position < code.size()
               && std::isspace(static_cast<unsigned char>(code[position]))) {
            ++position;
        }
        const auto directive = code.substr(position);
        if (lineStartsWithIgnoreCase(directive, "#opll_mode")) {
            position += 10;
            if (const auto mode = parseMmlInteger(code, position)) {
                return *mode == 1;
            }
            return false;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_begin = line_end + 1;
        if (line_end < text.size()
            && text[line_end] == '\r'
            && line_end + 1 < text.size()
            && text[line_end + 1] == '\n') {
            line_begin = line_end + 2;
        }
    }
    return false;
}

[[nodiscard]] std::optional<unsigned> parseMmlTrackNumber(
    std::string_view code,
    std::size_t& position) {
    if (position >= code.size()) {
        return std::nullopt;
    }
    const auto character = static_cast<unsigned char>(code[position]);
    if (std::isdigit(character)) {
        unsigned track = 0;
        bool overflow = false;
        while (position < code.size()
               && std::isdigit(static_cast<unsigned char>(code[position]))) {
            const auto digit = static_cast<unsigned>(code[position] - '0');
            if (!overflow) {
                if (track > (std::numeric_limits<unsigned>::max() - digit) / 10U) overflow = true;
                else track = track * 10U + digit;
            }
            ++position;
        }
        return overflow ? std::nullopt : std::optional<unsigned>{track};
    }
    if ((character >= 'A' && character <= 'H')
        || (character >= 'a' && character <= 'h')) {
        const auto letter = static_cast<unsigned char>(
            std::toupper(character));
        ++position;
        return 10U + static_cast<unsigned>(letter - 'A');
    }
    return std::nullopt;
}

// No binary-MGS or public interface changes are required by this reader.
struct MmlUseTrack {
    std::string code;
    std::uint32_t steps{};
    std::uint32_t default_steps{kMgscQuarterSteps};
    int seq{};
    bool uncertain{};
};

// Compare actual interrupt counts for a fixed supported tempo, including
// quantisation that can put different score positions on the same tick.
std::uint32_t mmlUseTick(std::uint32_t steps, unsigned tempo) {
    const std::uint64_t whole_ticks = 14400U / tempo;
    return static_cast<std::uint32_t>(
        (steps / kMgscWholeSteps) * whole_ticks
        + ((steps % kMgscWholeSteps) * whole_ticks + 96U) / kMgscWholeSteps);
}

bool readMmlUseLength(std::string_view code, std::size_t& position,
    std::uint32_t fallback, std::uint32_t& steps, unsigned tempo) {
    while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position]))) ++position;
    const bool absolute = position < code.size() && code[position] == '%';
    if (absolute) ++position;
    if (position < code.size() && std::isdigit(static_cast<unsigned char>(code[position]))) {
        const auto value = parseMmlInteger(code, position);
        if (!value || *value == 0 || *value > 256U) return false;
        if (!absolute && (*value > std::min<unsigned>(kMgscWholeSteps, 14400U / tempo)
            || (kMgscWholeSteps % *value) != 0U)) return false;
        steps = absolute ? *value : kMgscWholeSteps / *value;
    } else {
        if (absolute) return false;
        steps = fallback;
    }
    auto dot = steps;
    while (position < code.size() && code[position] == '.') {
        // Never invent the compiler's rounding for a fractional score step.
        if ((dot & 1U) != 0U) return false;
        dot /= 2U;
        if (dot == 0 || steps > 256U - dot) return false;
        steps += dot;
        ++position;
    }
    return steps != 0 && steps <= 256U;
}

void collectMmlEnvelopeUse(
    std::string_view text,
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage,
    std::vector<ImportWarning>& warnings) {
    std::array<MmlUseTrack, 18> tracks;
    unsigned tempo = kMgscDefaultTempo;
    bool have_tempo = false;
    bool global_uncertain = false;
    bool any_uncertain = false;
    int definition_depth = 0;
    std::set<std::string> reasons;
    const auto warn = [&](std::string reason) { reasons.insert(std::move(reason)); };
    std::size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const auto line_end = text.find_first_of("\r\n", line_begin);
        auto code = text.substr(line_begin,
            (line_end == std::string_view::npos ? text.size() : line_end) - line_begin);
        code = code.substr(0, code.find(';'));
        std::size_t position = 0;
        while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position]))) ++position;
        const auto directive = code.substr(position);
        if (definition_depth != 0 || (!directive.empty() && (directive.front() == '@' || directive.front() == '*'))) {
            for (const auto character : directive) {
                if (character == '{') ++definition_depth;
                if (character == '}') definition_depth = std::max(0, definition_depth - 1);
            }
        } else if (lineStartsWithIgnoreCase(directive, "#opll_mode")) {
            position += 10;
            const auto mode = parseMmlInteger(code, position);
            while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position]))) ++position;
            if (!mode || *mode > 1U || position != code.size()) {
                global_uncertain = true;
                warn("invalid OPLL mode directive");
            }
        } else if (lineStartsWithIgnoreCase(directive, "#tempo")) {
            position += 6;
            const auto value = parseMmlInteger(code, position);
            while (position < code.size() && std::isspace(static_cast<unsigned char>(code[position]))) ++position;
            if (!value || *value < kMgscTempoMin || *value > kMgscTempoMax
                || (have_tempo && *value != tempo) || position != code.size()) {
                global_uncertain = true;
                warn("unsupported or ambiguous tempo directive");
            } else {
                tempo = *value;
                have_tempo = true;
            }
        } else {
            const auto selector_end = code.find_first_of(" \t", position);
            const auto selector = code.substr(position,
                (selector_end == std::string_view::npos ? code.size() : selector_end) - position);
            std::set<unsigned> selected;
            auto numeric_position = position;
            const auto numeric_track = parseMmlTrackNumber(code, numeric_position);
            // Keep the existing decimal 10..17 aliases, alongside the source
            // format's documented adjacent single-character track selectors.
            if (numeric_track && sourceForMusicTrack(*numeric_track)
                && numeric_position == position + selector.size()) selected.insert(*numeric_track);
            else if (numeric_track || numeric_position != position + selector.size()) {
                for (const auto character : selector) {
                    std::size_t one = 0;
                    const auto value = parseMmlTrackNumber(std::string_view(&character, 1), one);
                    if (!value || !sourceForMusicTrack(*value)) { selected.clear(); break; }
                    selected.insert(*value);
                }
            }
            if (!selected.empty() && selector_end != std::string_view::npos)
                for (const auto track : selected) {
                    tracks[track].code.append(code.substr(selector_end));
                    tracks[track].code.push_back(' ');
                }
            else if (!directive.empty() && directive.front() == '#'
                && !lineStartsWithIgnoreCase(directive, "#opll_mode")
                && !lineStartsWithIgnoreCase(directive, "#title")
                && !lineStartsWithIgnoreCase(directive, "#composer")
                && !lineStartsWithIgnoreCase(directive, "#arranger")
                && !lineStartsWithIgnoreCase(directive, "#memo")
                && !lineStartsWithIgnoreCase(directive, "#machine_id")
                && !lineStartsWithIgnoreCase(directive, "#macro_offset")) {
                global_uncertain = true;
                warn("unsupported source directive");
            }
            else if (selected.empty() && !directive.empty() && directive.front() != '#') {
                global_uncertain = true;
                warn("invalid or unsupported track selector");
            }
        }
        if (line_end == std::string_view::npos) break;
        line_begin = line_end + 1;
        if (text[line_end] == '\r' && line_begin < text.size() && text[line_begin] == '\n') ++line_begin;
    }

    std::vector<MusicUseEvent> events;
    for (unsigned number = 1; number < tracks.size(); ++number) {
        auto& track = tracks[number];
        std::size_t position = 0;
        std::size_t commands = 0;
        struct Repeat { std::size_t start, end, after; unsigned remaining; };
        std::vector<Repeat> repeats;
        bool track_finished = false;
        const auto fail = [&](std::string reason) {
            track.uncertain = true;
            any_uncertain = true;
            warn("track " + std::to_string(number) + ": " + std::move(reason));
        };
        const auto event = [&](MusicUseEvent::Kind kind, unsigned value, bool rhythm = false) {
            events.push_back({.tick = mmlUseTick(track.steps, tempo),
                .track = static_cast<int>(number), .seq = track.seq++,
                .kind = kind, .number = value, .rhythm_on = rhythm});
        };
        while (position < track.code.size() && !track.uncertain) {
            if (++commands > 100000U) { fail("timing command budget exceeded"); break; }
            const auto c = static_cast<char>(std::tolower(static_cast<unsigned char>(track.code[position])));
            if (std::isspace(static_cast<unsigned char>(c))) { ++position; continue; }
            if (c == '!') { track_finished = true; break; }
            if (c == '[') {
                auto start = position + 1;
                while (start < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[start]))) ++start;
                std::optional<unsigned> prefix;
                if (start < track.code.size() && std::isdigit(static_cast<unsigned char>(track.code[start]))) {
                    prefix = parseMmlInteger(track.code, start);
                    if (!prefix) { fail("invalid repeat count"); continue; }
                }
                std::size_t end = start;
                unsigned depth = 1;
                while (end < track.code.size() && depth != 0) {
                    if (++commands > 100000U) break;
                    if (track.code[end] == '[') ++depth;
                    if (track.code[end] == ']') --depth;
                    if (depth != 0) ++end;
                }
                if (depth != 0 || commands > 100000U || repeats.size() >= 16) {
                    fail("unclosed repeat, nesting limit or timing budget exceeded"); continue;
                }
                auto after = end + 1;
                while (after < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[after]))) ++after;
                std::optional<unsigned> suffix;
                if (after < track.code.size() && std::isdigit(static_cast<unsigned char>(track.code[after]))) {
                    suffix = parseMmlInteger(track.code, after);
                    if (!suffix) { fail("invalid repeat count"); continue; }
                }
                if ((prefix && suffix) || (prefix && *prefix > 255U) || (suffix && *suffix > 255U)) {
                    fail("invalid repeat count"); continue;
                }
                const auto count = prefix.value_or(suffix.value_or(2U));
                if (count == 0) { fail("infinite repeat is not supported for timing analysis"); continue; }
                repeats.push_back({start, end, after, count});
                position = start;
                continue;
            }
            if (c == ']') {
                if (repeats.empty() || repeats.back().end != position) {
                    fail("unmatched repeat end"); continue;
                }
                if (--repeats.back().remaining != 0) position = repeats.back().start;
                else { position = repeats.back().after; repeats.pop_back(); }
                continue;
            }
            if (c == '|') {
                if (repeats.empty()) { fail("repeat break outside a repeat"); continue; }
                if (repeats.back().remaining == 1) {
                    position = repeats.back().after;
                    repeats.pop_back();
                } else ++position;
                continue;
            }
            if (c == 'y') {
                ++position;
                const auto reg = parseMmlInteger(track.code, position);
                while (position < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[position]))) ++position;
                if (!reg || *reg > 255U || position >= track.code.size() || track.code[position++] != ',') {
                    fail("invalid register write"); continue;
                }
                const auto value = parseMmlInteger(track.code, position);
                if (!value || *value > 255U) { fail("invalid register data"); continue; }
                if (isOpllRhythmControlWrite(number, *reg))
                    event(MusicUseEvent::Kind::Rhythm, 0, (*value & kOpllRhythmModeBit) != 0);
                continue;
            }
            if (c == '@') {
                ++position;
                if (position >= track.code.size()) { fail("incomplete instrument command"); continue; }
                const auto type = static_cast<char>(std::tolower(static_cast<unsigned char>(track.code[position])));
                if (type == 'e' || type == 'r') {
                    ++position;
                    const auto value = parseMmlInteger(track.code, position);
                    if (!value || *value > 31U) { fail("invalid envelope selection"); continue; }
                    event(MusicUseEvent::Kind::Envelope, *value);
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(type))) {
                    const auto value = parseMmlInteger(track.code, position);
                    if (!value || *value > 31U) { fail("invalid instrument selection"); continue; }
                    event(MusicUseEvent::Kind::Patch, *value);
                    continue;
                }
                if (type == 'p' || type == '\\' || type == 'l' || type == 'o') {
                    ++position;
                    if (position < track.code.size() && (track.code[position] == '+' || track.code[position] == '-')) ++position;
                    if (!parseMmlInteger(track.code, position)) fail("invalid zero-time instrument parameter");
                    continue;
                }
                if (type == 'f') { ++position; continue; }
                fail("unsupported instrument command");
                continue;
            }
            if ((c >= 'a' && c <= 'g') || c == 'r') {
                ++position;
                if (c != 'r' && position < track.code.size()
                    && (track.code[position] == '+' || track.code[position] == '-')) ++position;
                std::uint32_t length{};
                if (!readMmlUseLength(track.code, position, track.default_steps, length, tempo)
                    || track.steps > 100000000U - length) {
                    fail("unsupported or fractional note length"); continue;
                }
                while (position < track.code.size()) {
                    auto tie = position;
                    while (tie < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[tie]))) ++tie;
                    if (tie >= track.code.size() || track.code[tie] != '^') break;
                    position = tie + 1;
                    std::uint32_t extension{};
                    if (!readMmlUseLength(track.code, position, track.default_steps, extension, tempo)
                        || length > 256U - extension) {
                        fail("unsupported or excessive tied length"); break;
                    }
                    length += extension;
                }
                if (track.uncertain) continue;
                const auto next = track.steps + length;
                if (static_cast<std::uint64_t>(length) * (14400U / tempo) < kMgscWholeSteps
                    || mmlUseTick(next, tempo) <= mmlUseTick(track.steps, tempo)) {
                    fail("note length does not advance an interrupt tick"); continue;
                }
                track.steps = next;
                continue;
            }
            if (c == 'l') {
                ++position;
                std::uint32_t length{};
                if (!readMmlUseLength(track.code, position, 0, length, tempo)
                    || static_cast<std::uint64_t>(length) * (14400U / tempo) < kMgscWholeSteps)
                    fail("unsupported default note length");
                else track.default_steps = length;
                continue;
            }
            if (c == '&' || c == '<' || c == '>') { ++position; continue; }
            if ((c == 's' || c == 'k' || c == 'h') && position + 1 < track.code.size()
                && (std::tolower(static_cast<unsigned char>(track.code[position + 1])) == 'o'
                    || std::tolower(static_cast<unsigned char>(track.code[position + 1])) == 'f'
                    || (c == 'h' && std::tolower(static_cast<unsigned char>(track.code[position + 1])) == 'i'))) {
                position += 2; continue;
            }
            if (c == 'h') {
                ++position;
                for (int parameter = 0; parameter < 4 && !track.uncertain; ++parameter) {
                    while (position < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[position]))) ++position;
                    if (position < track.code.size() && (track.code[position] == '+' || track.code[position] == '-')) ++position;
                    if (!parseMmlInteger(track.code, position)) { fail("invalid zero-time LFO parameter"); break; }
                    while (position < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[position]))) ++position;
                    if (parameter < 3 && (position >= track.code.size() || track.code[position++] != ','))
                        fail("invalid zero-time LFO parameter separator");
                }
                continue;
            }
            if (c == '(' || c == ')') {
                ++position;
                while (position < track.code.size() && std::isspace(static_cast<unsigned char>(track.code[position]))) ++position;
                if (position < track.code.size()
                    && (std::isdigit(static_cast<unsigned char>(track.code[position])) || track.code[position] == '$')) {
                    const auto value = parseMmlInteger(track.code, position);
                    if (!value || *value < 1U || *value > 15U) fail("invalid relative volume parameter");
                }
                continue;
            }
            if (c == 'o' || c == 'v' || c == 'q' || c == 'k' || c == 'p' || c == 's'
                || c == 'n' || c == 'm' || c == '/' || c == '\\') {
                ++position;
                if (position < track.code.size() && (track.code[position] == '+' || track.code[position] == '-')) ++position;
                if (!parseMmlInteger(track.code, position)) fail("invalid zero-time control parameter");
                continue;
            }
            if (c == 't') global_uncertain = true;
            fail("unsupported timing syntax");
        }
        if (!track_finished && !track.uncertain && !repeats.empty()) fail("unclosed repeat");
    }
    const bool rhythm_uncertain = global_uncertain
        || tracks[15].uncertain || tracks[16].uncertain || tracks[17].uncertain;
    std::erase_if(events, [&](const MusicUseEvent& event) {
        return tracks[static_cast<std::size_t>(event.track)].uncertain
            || (rhythm_uncertain && isOpllRhythmMusicTrack(static_cast<unsigned>(event.track)));
    });
    applyMusicEnvelopeEvents(events, mmlHeaderRhythmMode(text), usage, true);
    if (any_uncertain || global_uncertain) {
        // Unknown source can hide a use. Do not reinterpret an ambiguous
        // definition as an unused PSG envelope; independently proved uses stay.
        for (const auto type : {'e', 'r'})
            for (const auto& definition : extractMgsSourceDefinitions(text, type))
                static_cast<void>(usage[definition.number]);
    }
    for (const auto& reason : reasons)
        warnings.push_back({"MML timing analysis skipped ambiguous envelope use: " + reason});
}

[[nodiscard]] std::optional<tone_import_detail::EnvelopeChipUse>
usageOrUnusedPsg(
    const std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage,
    unsigned number) {
    const auto found = usage.find(number);
    if (found != usage.end()) {
        if (found->second.anyChip()) {
            return found->second;
        }
        // Present but empty: used only while OPLL rhythm mode was on.
        return std::nullopt;
    }
    tone_import_detail::EnvelopeChipUse psg;
    psg.psg = true;
    return psg;
}

bool addEnvelopeCandidate(
    ToneImportResult& result,
    EnvelopeDef envelope,
    const tone_import_detail::DefinedTones& tones,
    std::string_view source_format,
    const std::optional<tone_import_detail::EnvelopeChipUse>& usage) {
    CompositeLayer layer;
    layer.source = TimbreSource::Psg;
    layer.channel = 0;
    std::vector<std::string> issues;
    std::vector<ImportWarning> warnings;
    if (envelope.rate) {
        layer.volume_envelope.kind = EnvelopeKind::Rate;
        layer.volume_envelope.rate = envelope.rate_values;
    } else {
        bool dropped_psg = false;
        if (!tone_import_detail::envelopeBytecodeToLayer(
                envelope.bytecode, layer, issues, &dropped_psg)) {
            return false;
        }
        if (dropped_psg) {
            warnings.push_back(
                {"PSG noise and tone/noise mode commands were skipped"});
        }
        assignSequenceEnvelopeMixer(
            layer.volume_envelope.rate, envelope.mode, envelope.noise);
        for (const auto& issue : issues) {
            warnings.push_back({issue});
        }
    }
    if (!usage) return true;
    auto timbre = tone_import_detail::makeSelfContainedComposite(
        std::move(layer),
        tones,
        tone_import_detail::paddedImportNumber(envelope.number),
        warnings,
        *usage);
    auto candidate = tone_import_detail::makeCompositeCandidate(
        std::move(timbre), source_format);
    candidate.warnings = std::move(warnings);
    tone_import_detail::addCandidate(
        result.candidates, std::move(candidate));
    return true;
}

}  // namespace

bool looksLikeCompressedMgs(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 6 || bytes[0] != 'M' || bytes[1] != 'G'
        || bytes[2] != 'S') {
        return false;
    }
    if (bytes[3] == 'A') {
        return true;
    }
    const auto marker = findMgsEofMarker(bytes);
    if (!marker || *marker + 1 >= bytes.size()) {
        return false;
    }
    const auto base = *marker + 1;
    if (bytes[base] != 0) {
        return true;
    }
    if (base + 1 >= bytes.size()) {
        return false;
    }
    return (bytes[base + 1] & 0x80U) != 0U;
}

bool looksLikeMgsBinary(std::span<const std::uint8_t> bytes) {
    if (!isMgsMagic(bytes) || looksLikeCompressedMgs(bytes)) {
        return false;
    }
    const auto header = findMgsBinaryHeader(bytes);
    if (!header || *header + 0x28 > bytes.size()) {
        return false;
    }
    const auto voice = readU16(bytes, *header + 4);
    return voice >= 0x28
        && *header + static_cast<std::size_t>(voice) < bytes.size();
}

bool looksLikeMgsMml(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || looksLikeMgsBinary(bytes) || looksLikeVgm(bytes)) {
        return false;
    }
    const std::string_view text(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (const char kind : {'v', 's', 'e', 'r'}) {
        if (!extractMgsSourceDefinitions(text, kind).empty()) {
            return true;
        }
    }
    return false;
}

ToneImportResult importMgsBinary(std::span<const std::uint8_t> bytes) {
    ToneImportResult result;
    result.format = ToneImportFormat::Mgs;
    if (looksLikeCompressedMgs(bytes)) {
        result.errors.emplace_back("compressed MGS is not supported");
        return result;
    }
    result.title = extractMgsBinaryTitle(bytes);
    const auto header = findMgsBinaryHeader(bytes);
    if (!header || *header + 0x28 > bytes.size()) {
        result.errors.emplace_back("invalid MGS header");
        return result;
    }
    const auto voice_offset = readU16(bytes, *header + 4);
    if (voice_offset < 0x28) {
        result.errors.emplace_back("invalid MGS header");
        return result;
    }
    auto position = *header + static_cast<std::size_t>(voice_offset);
    auto voice_end = bytes.size();
    for (int track = 1; track < 18; ++track) {
        const auto offset = readU16(
            bytes, *header + 4 + static_cast<std::size_t>(track) * 2);
        if (offset == 0) {
            continue;
        }
        const auto start = *header + static_cast<std::size_t>(offset);
        if (start > position && start < voice_end) {
            voice_end = start;
        }
    }
    tone_import_detail::DefinedTones tones;
    std::vector<EnvelopeDef> envelopes;
    std::vector<ImportWarning> voice_warnings;
    bool terminated = false;
    const auto warn_voice = [&](std::string message) {
        voice_warnings.push_back({"MGS voice definitions: " + std::move(message)});
    };

    const auto need = [&](std::size_t count) {
        return position <= voice_end && count <= voice_end - position;
    };
    while (position < voice_end) {
        const auto opcode_position = position;
        const auto opcode = bytes[position++];
        if (opcode == 0xFF) {
            terminated = true;
            break;
        }
        if (opcode == 0x00) {
            if (!need(9)) {
                warn_voice("truncated OPLL definition");
                break;
            }
            const auto number = static_cast<unsigned>(bytes[position++] & 0x1F);
            std::array<std::uint8_t, 8> registers{};
            for (auto& value : registers) {
                value = bytes[position++];
            }
            tones.opll[number] = decodeOpllPatch(registers);
            continue;
        }
        if (opcode == 0x01) {
            if (!need(2)) {
                warn_voice("truncated OPLL parameter definition");
                break;
            }
            position += 2;
            continue;
        }
        if (opcode == 0x02) {
            if (!need(2)) {
                warn_voice("truncated envelope header");
                break;
            }
            const auto number = static_cast<unsigned>(bytes[position++] & 0x1F);
            const auto nn = bytes[position++];
            EnvelopeDef envelope;
            envelope.number = number;
            envelope.noise = static_cast<std::uint8_t>(nn & 0x1F);
            envelope.mode = static_cast<std::uint8_t>((nn >> 5) & 0x03);
            envelope.rate = (nn & 0x80U) != 0U;
            if (envelope.rate) {
                if (!need(6)) {
                    warn_voice("truncated rate envelope definition");
                    break;
                }
                envelope.rate_values = clampRateEnvelope({
                    .tone_mode = envelope.mode,
                    .noise = envelope.noise,
                    .attack_level = bytes[position],
                    .attack_rate = bytes[position + 1],
                    .decay_rate = bytes[position + 2],
                    .sustain_level = bytes[position + 3],
                    .sustain_rate = bytes[position + 4],
                    .release_rate = bytes[position + 5],
                });
                position += 6;
            } else {
                if (!need(1)) {
                    warn_voice("truncated sequence envelope length");
                    break;
                }
                const auto length = bytes[position++];
                if (!need(length)) {
                    warn_voice("truncated sequence envelope definition");
                    break;
                }
                envelope.bytecode.assign(
                    bytes.begin() + static_cast<std::ptrdiff_t>(position),
                    bytes.begin()
                        + static_cast<std::ptrdiff_t>(position + length));
                position += length;
            }
            envelopes.push_back(std::move(envelope));
            continue;
        }
        if (opcode == 0x03) {
            if (!need(33)) {
                warn_voice("truncated SCC definition");
                break;
            }
            const auto number = static_cast<unsigned>(bytes[position++] & 0x1F);
            std::array<std::uint8_t, 32> wave{};
            for (auto& value : wave) {
                value = bytes[position++];
            }
            tones.scc[number] = sccWaveformFromBytes(wave);
            continue;
        }
        if (opcode == 0x04 || opcode == 0x05) {
            if (!need(24)) {
                warn_voice("truncated pitch table definition");
                break;
            }
            position += 24;
            continue;
        }
        if (opcode == 0x06) {
            if (!need(2)) {
                warn_voice("truncated control string definition");
                break;
            }
            position += 1;
            while (position < voice_end && bytes[position] != 0) {
                ++position;
            }
            if (position < voice_end) {
                ++position;
            } else {
                warn_voice("control string has no NUL terminator");
                break;
            }
            continue;
        }
        warn_voice("unknown definition command " + std::to_string(opcode)
            + " at byte " + std::to_string(opcode_position) + "; remaining definitions were not scanned");
        break;
    }
    if (!terminated && voice_warnings.empty())
        warn_voice("missing voice-track terminator");

    std::map<unsigned, tone_import_detail::EnvelopeChipUse> usage_by_envelope;
    collectBinaryMusicEnvelopeUse(bytes, *header, usage_by_envelope);

    for (const auto& [number, patch] : tones.opll) {
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeOpllCandidate(
                patch,
                tone_import_detail::paddedImportNumber(number),
                "MGS"));
    }
    for (const auto& [number, wave] : tones.scc) {
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeSccCandidate(
                wave,
                tone_import_detail::paddedImportNumber(number),
                "MGS"));
    }
    for (const auto& envelope : envelopes) {
        const auto usage = usageOrUnusedPsg(
            usage_by_envelope, envelope.number);
        if (!addEnvelopeCandidate(result, envelope, tones, "MGS", usage))
            warn_voice("broken envelope definition " + std::to_string(envelope.number) + " was skipped");
    }
    if (!voice_warnings.empty()) {
        for (auto& candidate : result.candidates)
            candidate.warnings.insert(candidate.warnings.end(), voice_warnings.begin(), voice_warnings.end());
        if (result.candidates.empty())
            for (const auto& warning : voice_warnings) result.errors.push_back(warning.message);
    }
    return result;
}

ToneImportResult importMgsMml(std::string_view text) {
    ToneImportResult result;
    result.format = ToneImportFormat::MgsMml;
    result.title = extractMgsMmlTitle(text);
    tone_import_detail::DefinedTones tones;
    for (const auto& definition : extractMgsSourceDefinitions(text, 'v')) {
        const auto parsed = parseMgsOpllDefinition(
            "@v" + std::to_string(definition.number)
            + " = {" + definition.body + "}");
        if (!parsed) {
            continue;
        }
        tones.opll[definition.number] = parsed->patch;
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeOpllCandidate(
                parsed->patch,
                tone_import_detail::paddedImportNumber(definition.number),
                "MML"));
    }
    for (const auto& definition : extractMgsSourceDefinitions(text, 's')) {
        const auto parsed = parseMgsSccDefinition(
            "@s" + std::to_string(definition.number)
            + " = {" + definition.body + "}");
        if (!parsed) {
            continue;
        }
        tones.scc[definition.number] = parsed->waveform;
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeSccCandidate(
                parsed->waveform,
                tone_import_detail::paddedImportNumber(definition.number),
                "MML"));
    }
    std::map<unsigned, tone_import_detail::EnvelopeChipUse> usage_by_envelope;
    std::vector<ImportWarning> timing_warnings;
    collectMmlEnvelopeUse(text, usage_by_envelope, timing_warnings);
    for (const auto& definition : extractMgsSourceDefinitions(text, 'e')) {
        bool stripped = false;
        const auto body = stripPsgModeNoiseTokens(definition.body, stripped);
        CompositeLayer layer;
        layer.source = TimbreSource::Psg;
        std::vector<std::string> issues;
        if (!parseMgsSequenceBody(body, layer, issues)) {
            continue;
        }
        std::vector<ImportWarning> warnings;
        if (stripped) {
            warnings.push_back(
                {"PSG noise and tone/noise mode commands were skipped"});
        }
        const auto usage = usageOrUnusedPsg(
            usage_by_envelope, definition.number);
        if (!usage) {
            continue;
        }
        auto timbre = tone_import_detail::makeSelfContainedComposite(
            std::move(layer),
            tones,
            tone_import_detail::paddedImportNumber(definition.number),
            warnings,
            *usage);
        auto candidate = tone_import_detail::makeCompositeCandidate(
            std::move(timbre), "MML");
        candidate.warnings = std::move(warnings);
        tone_import_detail::addCandidate(
            result.candidates, std::move(candidate));
    }
    for (const auto& definition : extractMgsSourceDefinitions(text, 'r')) {
        RateEnvelope rate{};
        if (!parseMgsRateBody(definition.body, rate)) {
            continue;
        }
        CompositeLayer layer;
        layer.source = TimbreSource::Psg;
        layer.volume_envelope.kind = EnvelopeKind::Rate;
        layer.volume_envelope.rate = rate;
        std::vector<ImportWarning> warnings;
        const auto usage = usageOrUnusedPsg(
            usage_by_envelope, definition.number);
        if (!usage) {
            continue;
        }
        auto timbre = tone_import_detail::makeSelfContainedComposite(
            std::move(layer),
            tones,
            tone_import_detail::paddedImportNumber(definition.number),
            warnings,
            *usage);
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeCompositeCandidate(std::move(timbre), "MML"));
    }
    if (!timing_warnings.empty()) {
        for (auto& candidate : result.candidates)
            candidate.warnings.insert(candidate.warnings.end(), timing_warnings.begin(), timing_warnings.end());
        if (result.candidates.empty())
            for (const auto& warning : timing_warnings) result.errors.push_back(warning.message);
    }
    return result;
}

}  // namespace mgstc::engine
