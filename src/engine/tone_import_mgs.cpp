#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "tone_import_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
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
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage) {
    std::sort(
        events.begin(),
        events.end(),
        [](const MusicUseEvent& left, const MusicUseEvent& right) {
            if (left.tick != right.tick) {
                return left.tick < right.tick;
            }
            // Rhythm is chip-global. y14 and @e may sit on different of
            // OPLL ch 7/8/9, so apply every y14 at this tick before @e.
            if (left.kind != right.kind) {
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
        value = value * static_cast<unsigned>(base)
            + static_cast<unsigned>(digit);
        ++position;
    }
    if (!digits) {
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
        while (position < code.size()
               && std::isdigit(static_cast<unsigned char>(code[position]))) {
            track = track * 10U
                + static_cast<unsigned>(code[position] - '0');
            ++position;
        }
        return track;
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

void collectMmlEnvelopeUse(
    std::string_view text,
    std::map<unsigned, tone_import_detail::EnvelopeChipUse>& usage) {
    std::vector<MusicUseEvent> events;
    std::size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const auto line_end = text.find_first_of("\r\n", line_begin);
        const auto line = text.substr(
            line_begin,
            (line_end == std::string_view::npos ? text.size() : line_end)
                - line_begin);
        const auto comment = line.find(';');
        const auto code = line.substr(
            0, comment == std::string_view::npos ? line.size() : comment);
        std::size_t position = 0;
        while (position < code.size()
               && std::isspace(static_cast<unsigned char>(code[position]))) {
            ++position;
        }
        const auto track = parseMmlTrackNumber(code, position);
        if (track && sourceForMusicTrack(*track)) {
            std::uint32_t tick = 0;
            int seq = 0;
            while (position < code.size()) {
                const auto character =
                    static_cast<unsigned char>(code[position]);
                if (std::isspace(character)) {
                    ++position;
                    continue;
                }
                if (character == 'y' || character == 'Y') {
                    auto cursor = position + 1;
                    const auto reg = parseMmlInteger(code, cursor);
                    while (cursor < code.size()
                           && std::isspace(
                               static_cast<unsigned char>(code[cursor]))) {
                        ++cursor;
                    }
                    if (reg && cursor < code.size() && code[cursor] == ',') {
                        ++cursor;
                        if (const auto data = parseMmlInteger(code, cursor)) {
                            if (isOpllRhythmControlWrite(*track, *reg)) {
                                events.push_back({
                                    .tick = tick,
                                    .track = static_cast<int>(*track),
                                    .seq = seq++,
                                    .kind = MusicUseEvent::Kind::Rhythm,
                                    .rhythm_on =
                                        (*data & kOpllRhythmModeBit) != 0,
                                });
                            }
                            position = cursor;
                            continue;
                        }
                    }
                    ++position;
                    continue;
                }
                if (character == '@') {
                    auto cursor = position + 1;
                    if (cursor < code.size()
                        && (code[cursor] == 'e' || code[cursor] == 'E'
                            || code[cursor] == 'r' || code[cursor] == 'R')) {
                        ++cursor;
                        unsigned number = 0;
                        bool digits = false;
                        while (cursor < code.size()
                               && std::isdigit(
                                   static_cast<unsigned char>(code[cursor]))) {
                            digits = true;
                            number = number * 10U
                                + static_cast<unsigned>(code[cursor] - '0');
                            ++cursor;
                        }
                        if (digits) {
                            events.push_back({
                                .tick = tick,
                                .track = static_cast<int>(*track),
                                .seq = seq++,
                                .kind = MusicUseEvent::Kind::Envelope,
                                .number = number,
                            });
                        }
                        position = cursor;
                        continue;
                    }
                    if (cursor < code.size()
                        && (code[cursor] == 'p' || code[cursor] == 'P'
                            || code[cursor] == 's' || code[cursor] == 'S'
                            || code[cursor] == 'v' || code[cursor] == 'V'
                            || code[cursor] == '\\')) {
                        position = cursor;
                        continue;
                    }
                    unsigned number = 0;
                    bool digits = false;
                    while (cursor < code.size()
                           && std::isdigit(
                               static_cast<unsigned char>(code[cursor]))) {
                        digits = true;
                        number = number * 10U
                            + static_cast<unsigned>(code[cursor] - '0');
                        ++cursor;
                    }
                    if (digits) {
                        events.push_back({
                            .tick = tick,
                            .track = static_cast<int>(*track),
                            .seq = seq++,
                            .kind = MusicUseEvent::Kind::Patch,
                            .number = number,
                        });
                    }
                    position = cursor;
                    continue;
                }
                const auto note = static_cast<char>(std::tolower(character));
                if (note == 'c' || note == 'd' || note == 'e' || note == 'f'
                    || note == 'g' || note == 'a' || note == 'b'
                    || note == 'r') {
                    ++tick;
                }
                ++position;
            }
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
    applyMusicEnvelopeEvents(events, mmlHeaderRhythmMode(text), usage);
}

void addEnvelopeCandidate(
    ToneImportResult& result,
    EnvelopeDef envelope,
    const tone_import_detail::DefinedTones& tones,
    std::string_view source_format,
    const tone_import_detail::EnvelopeChipUse& usage) {
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
            return;
        }
        if (dropped_psg) {
            warnings.push_back(
                {"PSG noise and tone/noise mode commands were skipped"});
        }
        if (envelope.mode != 0 || envelope.noise != 0) {
            warnings.push_back(
                {"PSG mode/noise from the envelope header is not stored "
                 "on sequence envelopes"});
        }
        for (const auto& issue : issues) {
            warnings.push_back({issue});
        }
    }
    auto timbre = tone_import_detail::makeSelfContainedComposite(
        std::move(layer), tones, {}, warnings, usage);
    auto candidate = tone_import_detail::makeCompositeCandidate(
        std::move(timbre), source_format);
    candidate.warnings = std::move(warnings);
    tone_import_detail::addCandidate(
        result.candidates, std::move(candidate));
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

    const auto need = [&](std::size_t count) {
        return position + count <= voice_end;
    };
    while (position < voice_end) {
        const auto opcode = bytes[position++];
        if (opcode == 0xFF) {
            break;
        }
        if (opcode == 0x00) {
            if (!need(9)) {
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
                break;
            }
            position += 2;
            continue;
        }
        if (opcode == 0x02) {
            if (!need(2)) {
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
                    break;
                }
                const auto length = bytes[position++];
                if (!need(length)) {
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
                break;
            }
            position += 24;
            continue;
        }
        if (opcode == 0x06) {
            if (!need(2)) {
                break;
            }
            position += 1;
            while (position < voice_end && bytes[position] != 0) {
                ++position;
            }
            if (position < voice_end) {
                ++position;
            } else {
                break;
            }
            continue;
        }
        break;
    }

    std::map<unsigned, tone_import_detail::EnvelopeChipUse> usage_by_envelope;
    collectBinaryMusicEnvelopeUse(bytes, *header, usage_by_envelope);

    for (const auto& [number, patch] : tones.opll) {
        static_cast<void>(number);
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeOpllCandidate(patch, {}, "MGS"));
    }
    for (const auto& [number, wave] : tones.scc) {
        static_cast<void>(number);
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeSccCandidate(wave, {}, "MGS"));
    }
    for (const auto& envelope : envelopes) {
        const auto found = usage_by_envelope.find(envelope.number);
        if (found == usage_by_envelope.end() || !found->second.anyChip()) {
            continue;
        }
        addEnvelopeCandidate(result, envelope, tones, "MGS", found->second);
    }
    return result;
}

ToneImportResult importMgsMml(std::string_view text) {
    ToneImportResult result;
    result.format = ToneImportFormat::MgsMml;
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
            tone_import_detail::makeOpllCandidate(parsed->patch, {}, "MML"));
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
            tone_import_detail::makeSccCandidate(parsed->waveform, {}, "MML"));
    }
    std::map<unsigned, tone_import_detail::EnvelopeChipUse> usage_by_envelope;
    collectMmlEnvelopeUse(text, usage_by_envelope);
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
        const auto found = usage_by_envelope.find(definition.number);
        if (found == usage_by_envelope.end() || !found->second.anyChip()) {
            continue;
        }
        auto timbre = tone_import_detail::makeSelfContainedComposite(
            std::move(layer), tones, {}, warnings, found->second);
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
        const auto found = usage_by_envelope.find(definition.number);
        if (found == usage_by_envelope.end() || !found->second.anyChip()) {
            continue;
        }
        auto timbre = tone_import_detail::makeSelfContainedComposite(
            std::move(layer), tones, {}, warnings, found->second);
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeCompositeCandidate(std::move(timbre), "MML"));
    }
    return result;
}

}  // namespace mgstc::engine
