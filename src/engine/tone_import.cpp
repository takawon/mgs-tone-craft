#include "mgstc/engine/tone_import.hpp"

#include "mgstc/engine/gzip_inflate.hpp"
#include "tone_import_internal.hpp"

#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mgstc::engine {
namespace tone_import_detail {
namespace {

constexpr std::uint64_t kImportLibraryIdBase = 0x4D47535400020001ULL;

// JIS X 0201 halfwidth katakana U+FF61–FF9F → fullwidth.
constexpr char32_t kHalfwidthKatakanaToFullwidth[] = {
    0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3,
    0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,
    0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF,
    0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF,
    0x30C1, 0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD,
    0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB, 0x30DE, 0x30DF,
    0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, 0x30EA,
    0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C,
};

void appendUtf8(std::string& out, char32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

bool nextUtf8(
    std::string_view text, std::size_t& offset, char32_t& codepoint) {
    if (offset >= text.size()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t width = 1;
    char32_t value = lead;
    if (lead >= 0x80) {
        if ((lead & 0xE0) == 0xC0 && offset + 1 < text.size()) {
            width = 2;
            value = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0 && offset + 2 < text.size()) {
            width = 3;
            value = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0 && offset + 3 < text.size()) {
            width = 4;
            value = lead & 0x07;
        } else {
            ++offset;
            codepoint = lead;
            return true;
        }
        for (std::size_t index = 1; index < width; ++index) {
            const auto unit = static_cast<unsigned char>(text[offset + index]);
            if ((unit & 0xC0) != 0x80) {
                ++offset;
                codepoint = lead;
                return true;
            }
            value = (value << 6) | (unit & 0x3F);
        }
    }
    offset += width;
    codepoint = value;
    return true;
}

std::string widenMsxWindowsText(std::string text) {
    std::string out;
    out.reserve(text.size());
    std::size_t offset = 0;
    char32_t codepoint = 0;
    while (nextUtf8(text, offset, codepoint)) {
        if (codepoint >= 0xFF61 && codepoint <= 0xFF9F) {
            codepoint = kHalfwidthKatakanaToFullwidth[codepoint - 0xFF61];
            appendUtf8(out, codepoint);
            continue;
        }
        if (codepoint >= 0x2460 && codepoint <= 0x2473) {
            const int number = static_cast<int>(codepoint - 0x2460) + 1;
            if (number >= 10) {
                out.push_back(static_cast<char>('0' + (number / 10)));
            }
            out.push_back(static_cast<char>('0' + (number % 10)));
            continue;
        }
        appendUtf8(out, codepoint);
    }
    return out;
}

std::string asciiOrUtf8(std::string text) {
    while (!text.empty()
           && (text.back() == '\0' || std::isspace(
                   static_cast<unsigned char>(text.back())))) {
        text.pop_back();
    }
    std::size_t begin = 0;
    while (begin < text.size()
           && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    return widenMsxWindowsText(text.substr(begin));
}

}  // namespace

bool allZero(std::span<const std::uint8_t> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) {
        return value == 0;
    });
}

std::string trimPaddedName(std::string_view text) {
    return asciiOrUtf8(std::string(text));
}

std::string paddedImportNumber(unsigned number) {
    if (number >= 100U) {
        return std::to_string(number);
    }
    std::string text(2, '0');
    text[0] = static_cast<char>('0' + (number / 10U));
    text[1] = static_cast<char>('0' + (number % 10U));
    return text;
}

std::string collapseImportLabel(std::string text) {
    std::string collapsed;
    collapsed.reserve(text.size());
    bool pending_space = false;
    for (const unsigned char character : text) {
        if (std::isspace(character)) {
            pending_space = true;
            continue;
        }
        if (pending_space && !collapsed.empty()) {
            collapsed.push_back(' ');
        }
        pending_space = false;
        collapsed.push_back(static_cast<char>(character));
    }
    return widenMsxWindowsText(std::move(collapsed));
}

std::string decodeCp932Name(std::span<const std::uint8_t> bytes) {
    std::size_t length = bytes.size();
    while (length > 0 && bytes[length - 1] == 0) {
        --length;
    }
    if (length == 0) {
        return {};
    }
#ifdef _WIN32
    const auto wide_count = MultiByteToWideChar(
        932,
        0,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(length),
        nullptr,
        0);
    if (wide_count <= 0) {
        return trimPaddedName(std::string_view(
            reinterpret_cast<const char*>(bytes.data()), length));
    }
    std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
    MultiByteToWideChar(
        932,
        0,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(length),
        wide.data(),
        wide_count);
    const auto utf8_count = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        wide_count,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8_count <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(utf8_count), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        wide_count,
        utf8.data(),
        utf8_count,
        nullptr,
        nullptr);
    return asciiOrUtf8(std::move(utf8));
#else
    return trimPaddedName(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), length));
#endif
}

ImportedToneCandidate makeOpllCandidate(
    const OpllPatchParameters& patch,
    std::string name,
    std::string_view source_format) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Opll;
    candidate.name = std::move(name);
    candidate.source_format = std::string(source_format);
    candidate.data = patch;
    candidate.default_register_as = ImportRegisterAs::Opll;
    candidate.register_choices = {ImportRegisterAs::Opll};
    return candidate;
}

ImportedToneCandidate makeSccCandidate(
    const SccWaveform& waveform,
    std::string name,
    std::string_view source_format) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Scc;
    candidate.name = std::move(name);
    candidate.source_format = std::string(source_format);
    candidate.data = waveform;
    candidate.default_register_as = ImportRegisterAs::Scc;
    candidate.register_choices = {ImportRegisterAs::Scc};
    return candidate;
}

ImportedToneCandidate makeCompositeCandidate(
    CompositeTimbre timbre,
    std::string_view source_format) {
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Composite;
    candidate.name = timbre.name;
    candidate.favorite = timbre.favorite;
    candidate.source_format = std::string(source_format);
    candidate.data = std::move(timbre);
    candidate.default_register_as = ImportRegisterAs::Composite;
    candidate.register_choices = {ImportRegisterAs::Composite};
    return candidate;
}

bool envelopeBytecodeToLayer(
    std::span<const std::uint8_t> bytecode,
    CompositeLayer& layer,
    std::vector<std::string>& issues,
    bool* dropped_psg_mode_noise) {
    std::size_t position = 0;
    std::uint32_t count = 0;
    std::optional<std::uint32_t> loop_start;
    std::optional<std::uint32_t> loop_end;
    std::vector<EnvelopeEvent> volume;
    std::vector<EnvelopeEvent> pitch;
    std::vector<EnvelopeEvent> timbre;
    bool dropped_psg = false;
    bool after_loop_start = false;
    const auto fail = [&](const char* message) {
        issues.emplace_back(message);
        return false;
    };
    while (position < bytecode.size()) {
        const auto opcode = bytecode[position++];
        if (opcode <= 0x0F) {
            volume.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = opcode,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts, count + 1);
            continue;
        }
        if (opcode == 0x10) {
            if (position >= bytecode.size()) {
                return fail("truncated envelope timbre command");
            }
            timbre.push_back({
                .kind = EnvelopeEventKind::Timbre,
                .value = bytecode[position++],
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (opcode == 0x11) {
            if (position + 1 >= bytecode.size()) {
                return fail("truncated envelope register command");
            }
            const auto reg = bytecode[position++];
            const auto value = bytecode[position++];
            timbre.push_back({
                .kind = EnvelopeEventKind::RegisterWrite,
                .value = reg,
                .secondary = value,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (opcode == 0x12) {
            if (position >= bytecode.size()) {
                return fail("truncated envelope pitch command");
            }
            const auto encoded = bytecode[position++];
            const auto delta = encoded >= 0x80
                ? static_cast<std::int32_t>(encoded) - 256
                : static_cast<std::int32_t>(encoded);
            pitch.push_back({
                .kind = EnvelopeEventKind::Pitch,
                .value = delta,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            continue;
        }
        if (opcode >= 0x20 && opcode <= 0x2F) {
            if (position >= bytecode.size()) {
                return fail("truncated envelope ramp command");
            }
            const auto hold = bytecode[position++];
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts,
                count + hold);
            volume.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = opcode & 0x0F,
                .count = count,
                .after_loop_start = after_loop_start,
                .automatic = true,
            });
            continue;
        }
        if (opcode == 0x40) {
            loop_start = count;
            after_loop_start = true;
            continue;
        }
        if (opcode == 0x60) {
            loop_end = count;
            // L<: commands after `]` use after_loop_start=false (§6.2.3).
            after_loop_start = false;
            continue;
        }
        if (opcode >= 0x80 && opcode <= 0xA3) {
            dropped_psg = true;
            continue;
        }
        if (opcode >= 0xE0 && opcode <= 0xEF) {
            if (position >= bytecode.size()) {
                return fail("truncated envelope hold command");
            }
            const auto hold = bytecode[position++];
            volume.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = opcode & 0x0F,
                .count = count,
                .after_loop_start = after_loop_start,
            });
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts,
                count + hold);
            continue;
        }
        return fail("unknown envelope opcode");
    }
    if (dropped_psg_mode_noise != nullptr) {
        *dropped_psg_mode_noise = dropped_psg;
    }
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.volume_envelope.events = std::move(volume);
    layer.pitch_envelope.events = std::move(pitch);
    layer.timbre_automation = std::move(timbre);
    if (loop_start && loop_end && *loop_start >= *loop_end) {
        issues.emplace_back("zero-time envelope loop was dropped");
        loop_start.reset();
        loop_end.reset();
    } else if (static_cast<bool>(loop_start) != static_cast<bool>(loop_end)) {
        issues.emplace_back("incomplete envelope loop was dropped");
        loop_start.reset();
        loop_end.reset();
    }
    layer.envelope_timeline = {
        .length_counts = std::max<std::uint32_t>(1, count),
        .loop_start_count = loop_start,
        .loop_end_count = loop_end,
    };
    return true;
}

void dropImportedZeroTimeLoop(
    CompositeLayer& layer,
    std::vector<ImportWarning>& warnings) {
    auto& timeline = layer.envelope_timeline;
    const bool start = timeline.loop_start_count.has_value();
    const bool end = timeline.loop_end_count.has_value();
    if (start && end && *timeline.loop_start_count < *timeline.loop_end_count) {
        return;
    }
    if (!start && !end) {
        return;
    }
    warnings.push_back({"zero-time or incomplete envelope loop was dropped"});
    timeline.loop_start_count.reset();
    timeline.loop_end_count.reset();
}

CompositeTimbre makeSelfContainedComposite(
    CompositeLayer envelope_layer,
    const DefinedTones& tones,
    std::string name,
    std::vector<ImportWarning>& warnings,
    const EnvelopeChipUse& usage) {
    dropImportedZeroTimeLoop(envelope_layer, warnings);

    std::map<unsigned, SavedTimbreReference> opll_refs;
    std::map<unsigned, SavedTimbreReference> scc_refs;
    std::uint64_t next_id = kImportLibraryIdBase;
    const auto embedOpll = [&](unsigned number) -> std::optional<std::uint64_t> {
        if (const auto found = opll_refs.find(number);
            found != opll_refs.end()) {
            return found->second.library_id;
        }
        const auto defined = tones.opll.find(number);
        if (defined == tones.opll.end()) {
            return std::nullopt;
        }
        SavedTimbreReference reference;
        reference.library_id = next_id++;
        reference.source = TimbreSource::Opll;
        reference.name = envelope_layer.name;
        reference.number_mode = TimbreNumberMode::Manual;
        reference.manual_number =
            static_cast<std::uint8_t>(std::min(number, 31U));
        reference.opll_registers = encodeOpllPatch(defined->second);
        opll_refs.emplace(number, reference);
        return reference.library_id;
    };
    const auto embedScc = [&](unsigned number) -> std::optional<std::uint64_t> {
        if (const auto found = scc_refs.find(number);
            found != scc_refs.end()) {
            return found->second.library_id;
        }
        const auto defined = tones.scc.find(number);
        if (defined == tones.scc.end()) {
            return std::nullopt;
        }
        SavedTimbreReference reference;
        reference.library_id = next_id++;
        reference.source = TimbreSource::Scc;
        reference.name = envelope_layer.name;
        reference.number_mode = TimbreNumberMode::Manual;
        reference.manual_number =
            static_cast<std::uint8_t>(std::min(number, 31U));
        reference.scc_waveform = sccWaveformToBytes(defined->second);
        scc_refs.emplace(number, reference);
        return reference.library_id;
    };

    for (const auto& event : envelope_layer.timbre_automation) {
        if (event.kind != EnvelopeEventKind::Timbre) {
            continue;
        }
        const auto number = static_cast<unsigned>(event.value);
        static_cast<void>(embedOpll(number));
        static_cast<void>(embedScc(number));
    }
    for (const auto number : usage.track_opll) {
        static_cast<void>(embedOpll(number));
    }
    for (const auto number : usage.track_scc) {
        static_cast<void>(embedScc(number));
    }

    bool want_psg = usage.psg;
    bool want_scc = usage.scc;
    bool want_opll = usage.opll;
    if (!usage.anyChip()) {
        if (!opll_refs.empty() && opll_refs.size() >= scc_refs.size()) {
            want_opll = true;
        } else if (!scc_refs.empty()) {
            want_scc = true;
        } else {
            want_psg = true;
        }
    }
    if (!want_psg && !want_scc && !want_opll) {
        want_psg = true;
    }

    CompositeTimbre timbre;
    timbre.format_version = CompositeTimbre::kFormatVersion;
    timbre.name = std::move(name);

    const auto rewriteLayerEvents = [&](CompositeLayer& layer) {
        for (auto& event : layer.timbre_automation) {
            if (event.kind != EnvelopeEventKind::Timbre) {
                continue;
            }
            const auto number = static_cast<unsigned>(event.value);
            if (layer.source == TimbreSource::Opll
                && tones.scc.contains(number)
                && !tones.opll.contains(number)) {
                continue;
            }
            if (layer.source == TimbreSource::Scc
                && tones.opll.contains(number)
                && !tones.scc.contains(number)) {
                continue;
            }
            if (layer.source == TimbreSource::Opll
                && number <= 14
                && !tones.opll.contains(number)) {
                event.timbre_pick = TimbrePick::OpllRom;
                event.target_library_id = 0;
                continue;
            }
            if (layer.source == TimbreSource::Opll) {
                if (const auto id = embedOpll(number)) {
                    event.timbre_pick = TimbrePick::Library;
                    event.target_library_id = *id;
                } else {
                    warnings.push_back(
                        {"envelope references an undefined OPLL tone"});
                }
            } else if (layer.source == TimbreSource::Scc) {
                if (const auto id = embedScc(number)) {
                    event.timbre_pick = TimbrePick::Library;
                    event.target_library_id = *id;
                } else {
                    warnings.push_back(
                        {"envelope references an undefined SCC tone"});
                }
            }
        }
        if (layer.base_timbre) {
            return;
        }
        const auto firstToneNumber =
            [](const std::optional<unsigned>& primary,
               const std::set<unsigned>& track)
                -> std::optional<unsigned> {
            if (primary) {
                return primary;
            }
            if (!track.empty()) {
                return *track.begin();
            }
            return std::nullopt;
        };
        const bool envelope_has_timbre = std::any_of(
            envelope_layer.timbre_automation.begin(),
            envelope_layer.timbre_automation.end(),
            [](const EnvelopeEvent& event) {
                return event.kind == EnvelopeEventKind::Timbre;
            });
        if (!envelope_has_timbre) {
            if (layer.source == TimbreSource::Opll && usage.opll) {
                if (const auto number = firstToneNumber(
                        usage.primary_opll, usage.track_opll)) {
                    if (embedOpll(*number)) {
                        layer.base_timbre = opll_refs[*number];
                        layer.base_timbre->name = envelope_layer.name;
                        return;
                    }
                    if (*number <= 14) {
                        layer.base_opll_rom =
                            static_cast<std::uint8_t>(*number);
                        return;
                    }
                }
                if (!opll_refs.empty()) {
                    layer.base_timbre = opll_refs.begin()->second;
                    if (layer.base_timbre->name.empty()) {
                        layer.base_timbre->name = envelope_layer.name;
                    }
                    return;
                }
                seedDefaultLayerTimbre(timbre, layer);
                return;
            }
            if (layer.source == TimbreSource::Scc && usage.scc) {
                if (const auto number = firstToneNumber(
                        usage.primary_scc, usage.track_scc)) {
                    if (embedScc(*number)) {
                        layer.base_timbre = scc_refs[*number];
                        layer.base_timbre->name = envelope_layer.name;
                        return;
                    }
                }
                if (!scc_refs.empty()) {
                    layer.base_timbre = scc_refs.begin()->second;
                    if (layer.base_timbre->name.empty()) {
                        layer.base_timbre->name = envelope_layer.name;
                    }
                    return;
                }
                seedDefaultLayerTimbre(timbre, layer);
                return;
            }
        }
        if (layer.source == TimbreSource::Opll && !opll_refs.empty()) {
            layer.base_timbre = opll_refs.begin()->second;
            if (layer.base_timbre->name.empty()) {
                layer.base_timbre->name = envelope_layer.name;
            }
        } else if (layer.source == TimbreSource::Scc && !scc_refs.empty()) {
            layer.base_timbre = scc_refs.begin()->second;
            if (layer.base_timbre->name.empty()) {
                layer.base_timbre->name = envelope_layer.name;
            }
        } else {
            seedDefaultLayerTimbre(timbre, layer);
        }
    };

    const auto addLayer = [&](TimbreSource source) {
        CompositeLayer layer = envelope_layer;
        layer.source = source;
        layer.envelope_number = static_cast<std::uint8_t>(
            std::min<std::size_t>(timbre.layers.size(), 31));
        if (layer.name.empty()) {
            layer.name = "Imported";
        }
        rewriteLayerEvents(layer);
        if (const auto channel = firstAvailableChannel(timbre, source)) {
            layer.channel = *channel;
        } else {
            warnings.push_back({"no free channel for imported layer"});
            return;
        }
        timbre.layers.push_back(std::move(layer));
    };

    if (want_psg) {
        addLayer(TimbreSource::Psg);
    }
    if (want_scc) {
        addLayer(TimbreSource::Scc);
    }
    if (want_opll) {
        addLayer(TimbreSource::Opll);
    }

    for (const auto& [number, reference] : opll_refs) {
        static_cast<void>(number);
        timbre.embedded_timbres.push_back(reference);
    }
    for (const auto& [number, reference] : scc_refs) {
        static_cast<void>(number);
        timbre.embedded_timbres.push_back(reference);
    }
    return timbre;
}

[[nodiscard]] const CompositeTimbre* compositeOf(
    const ImportedToneCandidate& candidate) {
    if (const auto* timbre =
            std::get_if<CompositeTimbre>(&candidate.data)) {
        return timbre;
    }
    if (candidate.composite_alternative) {
        return &*candidate.composite_alternative;
    }
    return nullptr;
}

std::optional<SccWaveform> extractImportedScc(
    const ImportedToneCandidate& candidate) {
    if (const auto* wave = std::get_if<SccWaveform>(&candidate.data)) {
        return *wave;
    }
    const auto* timbre = compositeOf(candidate);
    if (timbre == nullptr) {
        return std::nullopt;
    }
    const auto from_reference =
        [](const SavedTimbreReference& reference)
            -> std::optional<SccWaveform> {
        if (reference.source != TimbreSource::Scc
            || allZero(reference.scc_waveform)) {
            return std::nullopt;
        }
        return sccWaveformFromBytes(reference.scc_waveform);
    };
    for (const auto& layer : timbre->layers) {
        if (layer.base_timbre) {
            if (auto wave = from_reference(*layer.base_timbre)) {
                return wave;
            }
        }
    }
    for (const auto& embedded : timbre->embedded_timbres) {
        if (auto wave = from_reference(embedded)) {
            return wave;
        }
    }
    return std::nullopt;
}

std::optional<OpllPatchParameters> extractImportedOpll(
    const ImportedToneCandidate& candidate) {
    if (const auto* patch =
            std::get_if<OpllPatchParameters>(&candidate.data)) {
        return *patch;
    }
    const auto* timbre = compositeOf(candidate);
    if (timbre == nullptr) {
        return std::nullopt;
    }
    const auto from_reference =
        [](const SavedTimbreReference& reference)
            -> std::optional<OpllPatchParameters> {
        if (reference.source != TimbreSource::Opll
            || allZero(reference.opll_registers)) {
            return std::nullopt;
        }
        return decodeOpllPatch(reference.opll_registers);
    };
    for (const auto& layer : timbre->layers) {
        if (layer.base_timbre) {
            if (auto patch = from_reference(*layer.base_timbre)) {
                return patch;
            }
        }
    }
    for (const auto& embedded : timbre->embedded_timbres) {
        if (auto patch = from_reference(embedded)) {
            return patch;
        }
    }
    return std::nullopt;
}

void decorateRegisterChoices(ImportedToneCandidate& candidate) {
    const auto add = [&](ImportRegisterAs choice) {
        if (std::find(
                candidate.register_choices.begin(),
                candidate.register_choices.end(),
                choice)
            == candidate.register_choices.end()) {
            candidate.register_choices.push_back(choice);
        }
    };
    if (candidate.register_choices.empty()) {
        candidate.register_choices.push_back(candidate.default_register_as);
    }
    if (extractImportedScc(candidate)) {
        add(ImportRegisterAs::Scc);
    }
    if (extractImportedOpll(candidate)) {
        add(ImportRegisterAs::Opll);
    }
}

void addCandidate(
    std::vector<ImportedToneCandidate>& candidates,
    ImportedToneCandidate candidate) {
    decorateRegisterChoices(candidate);
    candidates.push_back(std::move(candidate));
}

}  // namespace tone_import_detail

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

const char* toneImportFormatName(ToneImportFormat format) noexcept {
    switch (format) {
    case ToneImportFormat::Mgs:
        return "MGS";
    case ToneImportFormat::MgsMml:
        return "MML";
    case ToneImportFormat::MusicaVcd:
        return "VCD";
    case ToneImportFormat::SccMusixxSng:
        return "SNG";
    case ToneImportFormat::Vgm:
        return "VGM";
    case ToneImportFormat::Vgz:
        return "VGZ";
    case ToneImportFormat::Unknown:
        break;
    }
    return "";
}

namespace {

std::string normalizedExtension(std::string_view extension) {
    std::string text(extension);
    if (!text.empty() && text.front() == '.') {
        text.erase(text.begin());
    }
    for (auto& character : text) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

std::string importFileStem(std::string_view source_name) {
    std::string text(source_name);
    const auto slash = text.find_last_of("/\\");
    if (slash != std::string::npos) {
        text.erase(0, slash + 1);
    }
    const auto dot = text.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        text.resize(dot);
    }
    return tone_import_detail::trimPaddedName(std::move(text));
}

void applyDefaultImportNames(ToneImportResult& result) {
    const auto prefix = result.title.empty()
        ? importFileStem(result.source_name)
        : result.title;
    unsigned sequential = 0;
    for (auto& candidate : result.candidates) {
        std::string id = candidate.name;
        if (id.empty()) {
            id = tone_import_detail::paddedImportNumber(sequential++);
        }
        if (prefix.empty()) {
            candidate.name = std::move(id);
        } else {
            candidate.name = prefix + " - " + id;
        }
    }
}

}  // namespace

ToneImportFormat detectToneImportFormat(
    std::span<const std::uint8_t> bytes,
    std::string_view extension) {
    if (looksLikeGzip(bytes) && [&] {
            std::string error;
            const auto inflated = inflateGzip(bytes, &error);
            return inflated && looksLikeVgm(*inflated);
        }()) {
        return ToneImportFormat::Vgz;
    }
    if (looksLikeVgm(bytes)) {
        return ToneImportFormat::Vgm;
    }
    if (looksLikeCompressedMgs(bytes)) {
        return ToneImportFormat::Unknown;
    }
    if (looksLikeMgsBinary(bytes)) {
        return ToneImportFormat::Mgs;
    }
    if (looksLikeMusicaVcd(bytes)) {
        return ToneImportFormat::MusicaVcd;
    }
    if (looksLikeSccMusixxSng(bytes)) {
        return ToneImportFormat::SccMusixxSng;
    }
    if (looksLikeMgsMml(bytes)) {
        return ToneImportFormat::MgsMml;
    }
    const auto ext = normalizedExtension(extension);
    if (ext == "vgz" && looksLikeGzip(bytes)) {
        return ToneImportFormat::Vgz;
    }
    if (ext == "vgm") {
        return ToneImportFormat::Unknown;
    }
    if (ext == "mgs") {
        return ToneImportFormat::Unknown;
    }
    if (ext == "vcd") {
        return ToneImportFormat::Unknown;
    }
    if (ext == "sng") {
        return ToneImportFormat::Unknown;
    }
    if (ext == "mus" || ext == "mml") {
        return looksLikeMgsMml(bytes)
            ? ToneImportFormat::MgsMml
            : ToneImportFormat::Unknown;
    }
    return ToneImportFormat::Unknown;
}

ToneImportResult importTones(
    std::span<const std::uint8_t> bytes,
    std::string_view extension,
    std::string_view source_name) {
    ToneImportResult result;
    result.source_name = std::string(source_name);
    if (bytes.empty()) {
        result.errors.emplace_back("file is empty");
        return result;
    }
    result.format = detectToneImportFormat(bytes, extension);
    if (result.format == ToneImportFormat::Unknown) {
        if (looksLikeCompressedMgs(bytes)) {
            result.errors.emplace_back("compressed MGS is not supported");
        } else {
            result.errors.emplace_back("unrecognized timbre file format");
        }
        return result;
    }
    switch (result.format) {
    case ToneImportFormat::Vgz: {
        std::string error;
        const auto inflated = inflateGzip(bytes, &error);
        if (!inflated) {
            result.errors.push_back(
                error.empty() ? "failed to expand VGZ" : error);
            return result;
        }
        result = importVgm(*inflated);
        result.format = ToneImportFormat::Vgz;
        break;
    }
    case ToneImportFormat::Vgm:
        result = importVgm(bytes);
        break;
    case ToneImportFormat::Mgs:
        result = importMgsBinary(bytes);
        break;
    case ToneImportFormat::MgsMml: {
        const std::string text(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        result = importMgsMml(text);
        break;
    }
    case ToneImportFormat::MusicaVcd:
        result = importMusicaVcd(bytes);
        break;
    case ToneImportFormat::SccMusixxSng:
        result = importSccMusixxSng(bytes);
        break;
    case ToneImportFormat::Unknown:
        break;
    }
    result.source_name = std::string(source_name);
    if (result.format != ToneImportFormat::Unknown && result.errors.empty()
        && result.candidates.empty()) {
        result.errors.emplace_back("no complete tones were found");
    }
    if (!result.candidates.empty()) {
        applyDefaultImportNames(result);
    }
    return result;
}

TimbreLibraryEntry makeImportedLibraryEntry(
    const ImportedToneCandidate& candidate,
    ImportRegisterAs register_as) {
    TimbreLibraryEntry entry;
    entry.name = candidate.name;
    entry.favorite = candidate.favorite;
    if (register_as == ImportRegisterAs::Opll) {
        entry.category = TimbreCategory::Opll;
        if (const auto* patch =
                std::get_if<OpllPatchParameters>(&candidate.data)) {
            entry.opll_registers = encodeOpllPatch(*patch);
        } else if (
            const auto extracted =
                tone_import_detail::extractImportedOpll(candidate)) {
            entry.opll_registers = encodeOpllPatch(*extracted);
        }
    } else {
        entry.category = TimbreCategory::Scc;
        if (const auto* wave = std::get_if<SccWaveform>(&candidate.data)) {
            entry.scc_waveform = sccWaveformToBytes(*wave);
        } else if (
            const auto extracted =
                tone_import_detail::extractImportedScc(candidate)) {
            entry.scc_waveform = sccWaveformToBytes(*extracted);
        }
    }
    return entry;
}

void finalizeImportedComposite(CompositeTimbre& timbre) {
    const auto ensureSnapshot = [&](std::uint64_t library_id,
                                    TimbreSource source) {
        if (library_id == 0
            || findEmbeddedTimbreSnapshot(timbre, library_id) != nullptr) {
            return;
        }
        for (const auto& layer : timbre.layers) {
            if (layer.base_timbre
                && layer.base_timbre->library_id == library_id
                && layer.base_timbre->source == source) {
                timbre.embedded_timbres.push_back(*layer.base_timbre);
                return;
            }
        }
        for (const auto& embedded : timbre.embedded_timbres) {
            if (embedded.library_id == library_id
                && embedded.source == source) {
                return;
            }
        }
    };

    for (const auto& layer : timbre.layers) {
        if (layer.base_timbre) {
            ensureSnapshot(
                layer.base_timbre->library_id, layer.base_timbre->source);
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != EnvelopeEventKind::Timbre
                || event.timbre_pick != TimbrePick::Library
                || event.target_library_id == 0) {
                continue;
            }
            ensureSnapshot(event.target_library_id, layer.source);
        }
    }
    static_cast<void>(enforceOpllRegisterAutoExclusivity(timbre));
}

std::optional<CompositeTimbre> makeImportedComposite(
    const ImportedToneCandidate& candidate,
    ImportRegisterAs register_as) {
    if (register_as != ImportRegisterAs::Composite) {
        return std::nullopt;
    }
    if (const auto* timbre = std::get_if<CompositeTimbre>(&candidate.data)) {
        auto copy = *timbre;
        copy.name = candidate.name;
        copy.favorite = candidate.favorite;
        finalizeImportedComposite(copy);
        return copy;
    }
    if (candidate.composite_alternative) {
        auto copy = *candidate.composite_alternative;
        copy.name = candidate.name;
        copy.favorite = candidate.favorite;
        finalizeImportedComposite(copy);
        return copy;
    }
    return std::nullopt;
}

}  // namespace mgstc::engine
