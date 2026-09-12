#include "mgstc/engine/tone_import.hpp"

#include "mgstc/engine/gzip_inflate.hpp"
#include "tone_import_internal.hpp"

#include "mgstc/engine/opll_patch.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

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
    return text.substr(begin);
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
            });
            continue;
        }
        if (opcode >= 0x20 && opcode <= 0x2F) {
            if (position >= bytecode.size()) {
                return fail("truncated envelope ramp command");
            }
            const auto hold = bytecode[position++];
            auto origin = count;
            if (!volume.empty()
                && volume.back().kind == EnvelopeEventKind::Volume
                && volume.back().count + 1 == count
                && !volume.back().automatic) {
                origin = volume.back().count;
            }
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts,
                origin + hold);
            volume.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = opcode & 0x0F,
                .count = count,
                .automatic = true,
            });
            continue;
        }
        if (opcode == 0x40) {
            loop_start = count;
            continue;
        }
        if (opcode == 0x60) {
            loop_end = count;
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
        reference.number_mode = TimbreNumberMode::Automatic;
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
        reference.number_mode = TimbreNumberMode::Automatic;
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
                    event.value = 0;
                } else {
                    warnings.push_back(
                        {"envelope references an undefined OPLL tone"});
                }
            } else if (layer.source == TimbreSource::Scc) {
                if (const auto id = embedScc(number)) {
                    event.timbre_pick = TimbrePick::Library;
                    event.target_library_id = *id;
                    event.value = 0;
                } else {
                    warnings.push_back(
                        {"envelope references an undefined SCC tone"});
                }
            }
        }
        if (layer.base_timbre) {
            return;
        }
        const bool envelope_has_timbre = std::any_of(
            envelope_layer.timbre_automation.begin(),
            envelope_layer.timbre_automation.end(),
            [](const EnvelopeEvent& event) {
                return event.kind == EnvelopeEventKind::Timbre;
            });
        if (!envelope_has_timbre) {
            if (layer.source == TimbreSource::Opll && usage.opll) {
                if (usage.primary_opll) {
                    const auto number = *usage.primary_opll;
                    if (embedOpll(number)) {
                        layer.base_timbre = opll_refs[number];
                        return;
                    }
                    if (number <= 14) {
                        layer.base_opll_rom =
                            static_cast<std::uint8_t>(number);
                        return;
                    }
                }
                seedDefaultLayerTimbre(layer);
                return;
            }
            if (layer.source == TimbreSource::Scc && usage.scc) {
                if (usage.primary_scc) {
                    const auto number = *usage.primary_scc;
                    if (embedScc(number)) {
                        layer.base_timbre = scc_refs[number];
                        return;
                    }
                }
                seedDefaultLayerTimbre(layer);
                return;
            }
        }
        if (layer.source == TimbreSource::Opll && !opll_refs.empty()) {
            layer.base_timbre = opll_refs.begin()->second;
        } else if (layer.source == TimbreSource::Scc && !scc_refs.empty()) {
            layer.base_timbre = scc_refs.begin()->second;
        } else {
            seedDefaultLayerTimbre(layer);
        }
    };

    CompositeTimbre timbre;
    timbre.format_version = CompositeTimbre::kFormatVersion;
    timbre.name = std::move(name);

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

void addCandidate(
    std::vector<ImportedToneCandidate>& candidates,
    ImportedToneCandidate candidate) {
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
        }
    } else {
        entry.category = TimbreCategory::Scc;
        if (const auto* wave = std::get_if<SccWaveform>(&candidate.data)) {
            entry.scc_waveform = sccWaveformToBytes(*wave);
        }
    }
    return entry;
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
        return copy;
    }
    if (candidate.composite_alternative) {
        auto copy = *candidate.composite_alternative;
        copy.name = candidate.name;
        copy.favorite = candidate.favorite;
        return copy;
    }
    return std::nullopt;
}

}  // namespace mgstc::engine
