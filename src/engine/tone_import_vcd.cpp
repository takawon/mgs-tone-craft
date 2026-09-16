#include "tone_import_internal.hpp"

#include <algorithm>
#include <array>

namespace mgstc::engine {
namespace {

constexpr std::size_t kVcdSize = 0x10B8;
constexpr std::size_t kOpllNameOffset = 0x0000;
constexpr std::size_t kPsgNameOffset = 0x0320;
constexpr std::size_t kSccNameOffset = 0x0410;
constexpr std::size_t kOpllDataOffset = 0x05A0;
constexpr std::size_t kPsgDataOffset = 0x08C0;
constexpr std::size_t kSccDataOffset = 0x09B0;
constexpr int kOpllCount = 100;
constexpr int kPsgCount = 30;
constexpr int kSccCount = 50;

// MuSICA editor 00 is stored as F1 (wait=15, delta=1). Treat as inactive.
constexpr std::uint8_t kMusicaInactiveStage = 0xF1;
constexpr std::array<std::uint8_t, 8> kMusicaUnusedPsg{{
    0xF1, 0xF1, 0x00, 0xF1, 0x00, 0x09, 0x00, 0x00}};

[[nodiscard]] bool musicaStageActive(std::uint8_t packed) noexcept {
    if (packed == kMusicaInactiveStage) {
        return false;
    }
    const int wait = packed >> 4;
    const int delta = packed & 0x0F;
    return wait > 0 && delta > 0;
}

[[nodiscard]] std::uint8_t musicaReleaseToKeyOffHang(
    std::uint8_t release,
    std::uint8_t sustain) noexcept {
    if (!musicaStageActive(release) || sustain == 0) {
        return 0;
    }
    const int wait = release >> 4;
    const int delta = release & 0x0F;
    if (delta <= 0 || wait <= 0) {
        return 0;
    }
    return static_cast<std::uint8_t>(
        std::min(255, std::max(1, wait / delta)));
}

bool musicaEnvelopeToLayer(
    std::uint8_t attack,
    std::uint8_t decay,
    std::uint8_t sustain,
    std::uint8_t release,
    CompositeLayer& layer) {
    if (sustain > 15) {
        return false;
    }
    const auto run = [](
                         std::uint8_t packed,
                         bool raise,
                         std::uint8_t limit,
                         std::uint8_t& volume,
                         std::uint32_t& count,
                         std::vector<EnvelopeEvent>& events) {
        if (!musicaStageActive(packed)) {
            return;
        }
        const int wait = packed >> 4;
        const int delta = packed & 0x0F;
        while (true) {
            if (raise) {
                if (volume >= 15) {
                    break;
                }
            } else if (volume <= limit) {
                break;
            }
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts,
                count + static_cast<std::uint32_t>(wait));
            if (raise) {
                volume = static_cast<std::uint8_t>(
                    std::min(15, static_cast<int>(volume) + delta));
            } else {
                volume = static_cast<std::uint8_t>(
                    std::max(
                        static_cast<int>(limit),
                        static_cast<int>(volume) - delta));
            }
            events.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = volume,
                .count = count,
            });
            if (count >= EnvelopeTimeline::kMaximumLengthCounts) {
                break;
            }
        }
    };

    const bool has_ads =
        musicaStageActive(attack) || musicaStageActive(decay)
        || sustain > 0;
    if (!has_ads && !musicaStageActive(release)) {
        return false;
    }

    std::uint8_t volume = 0;
    std::uint32_t count = 0;
    std::vector<EnvelopeEvent> events;
    run(attack, true, 15, volume, count, events);
    run(decay, false, sustain, volume, count, events);
    if (volume != sustain) {
        if (count == 0 && events.empty()) {
            events.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = sustain,
                .count = 0,
            });
            volume = sustain;
        } else {
            count = std::min<std::uint32_t>(
                EnvelopeTimeline::kMaximumLengthCounts, count + 1);
            volume = sustain;
            events.push_back({
                .kind = EnvelopeEventKind::Volume,
                .value = volume,
                .count = count,
            });
        }
    }

    // Hold sustain until key-off by ending the sequence at SL. MuSICA has
    // no `[]`; a 1-step `[f]` loop is MGSDRV, not VCD. Release is after
    // key-off, so convert wait-per-step to track `k`.
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.volume_envelope.events = std::move(events);
    layer.envelope_timeline.length_counts =
        std::max<std::uint32_t>(1, count);
    layer.key_off_hang = musicaReleaseToKeyOffHang(release, sustain);
    return true;
}

bool slotNameInUse(std::span<const std::uint8_t> name) {
    return !tone_import_detail::decodeCp932Name(name).empty();
}

bool sccWaveformInUse(std::span<const std::uint8_t> data) {
    return data.size() >= 36
        && !tone_import_detail::allZero(data.subspan(4, 32));
}

bool psgSlotInUse(
    std::span<const std::uint8_t> name,
    std::span<const std::uint8_t> data) {
    if (slotNameInUse(name)) {
        return true;
    }
    if (data.size() < 8 || tone_import_detail::allZero(data)) {
        return false;
    }
    return !std::equal(
        data.begin(),
        data.begin() + 8,
        kMusicaUnusedPsg.begin());
}

}  // namespace

bool looksLikeMusicaVcd(std::span<const std::uint8_t> bytes) {
    return bytes.size() == kVcdSize;
}

ToneImportResult importMusicaVcd(std::span<const std::uint8_t> bytes) {
    ToneImportResult result;
    result.format = ToneImportFormat::MusicaVcd;
    if (!looksLikeMusicaVcd(bytes)) {
        result.errors.emplace_back("VCD size is not a MuSICA voice file");
        return result;
    }
    for (int index = 0; index < kOpllCount; ++index) {
        const auto name = bytes.subspan(
            kOpllNameOffset + static_cast<std::size_t>(index) * 8, 8);
        const auto data = bytes.subspan(
            kOpllDataOffset + static_cast<std::size_t>(index) * 8, 8);
        if (tone_import_detail::allZero(data)) {
            continue;
        }
        std::array<std::uint8_t, 8> registers{};
        std::copy(data.begin(), data.end(), registers.begin());
        auto label = tone_import_detail::decodeCp932Name(name);
        if (label.empty()) {
            label = tone_import_detail::paddedImportNumber(
                static_cast<unsigned>(index));
        }
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeOpllCandidate(
                decodeOpllPatch(registers),
                std::move(label),
                "VCD"));
    }
    for (int index = 0; index < kPsgCount; ++index) {
        const auto name = bytes.subspan(
            kPsgNameOffset + static_cast<std::size_t>(index) * 8, 8);
        const auto data = bytes.subspan(
            kPsgDataOffset + static_cast<std::size_t>(index) * 8, 8);
        if (!psgSlotInUse(name, data)) {
            continue;
        }
        auto label = tone_import_detail::decodeCp932Name(name);
        if (label.empty()) {
            label = tone_import_detail::paddedImportNumber(
                static_cast<unsigned>(index));
        }
        CompositeLayer layer;
        layer.name = label;
        layer.source = TimbreSource::Psg;
        std::vector<ImportWarning> warnings;
        if (data[2] > 15) {
            warnings.push_back(
                {"MuSICA sustain exceeds 15; envelope was skipped"});
        } else if (!musicaEnvelopeToLayer(
                       data[0], data[1], data[2], data[3], layer)) {
            layer.volume_envelope.kind = EnvelopeKind::Sequence;
            layer.volume_envelope.events = {{
                .kind = EnvelopeEventKind::Volume,
                .value = 15,
                .count = 0,
            }};
            layer.envelope_timeline.length_counts = 1;
        }
        // VCD +4 = noise period 0–31, +5 = mixer (bit0 tone off, bit3
        // noise off). Padding +6/+7 is unused.
        const auto noise = static_cast<std::uint8_t>(data[4] & 0x1F);
        const bool tone_on = (data[5] & 0x01) == 0;
        const bool noise_on = (data[5] & 0x08) == 0;
        std::uint8_t mode = 0;
        if (tone_on && noise_on) {
            mode = 3;
        } else if (noise_on) {
            mode = 2;
        } else if (tone_on) {
            mode = 1;
        }
        assignSequenceEnvelopeMixer(layer.volume_envelope.rate, mode, noise);
        tone_import_detail::EnvelopeChipUse usage;
        usage.psg = true;
        auto candidate = tone_import_detail::makeCompositeCandidate(
            tone_import_detail::makeSelfContainedComposite(
                std::move(layer),
                {},
                label,
                warnings,
                usage),
            "VCD");
        candidate.name = std::move(label);
        candidate.warnings = std::move(warnings);
        tone_import_detail::addCandidate(
            result.candidates, std::move(candidate));
    }
    for (int index = 0; index < kSccCount; ++index) {
        const auto name = bytes.subspan(
            kSccNameOffset + static_cast<std::size_t>(index) * 8, 8);
        const auto data = bytes.subspan(
            kSccDataOffset + static_cast<std::size_t>(index) * 36, 36);
        const bool named = slotNameInUse(name);
        const bool has_wave = sccWaveformInUse(data);
        if (!named && !has_wave) {
            continue;
        }
        std::array<std::uint8_t, 32> wave{};
        std::copy(data.begin() + 4, data.end(), wave.begin());
        const auto waveform = sccWaveformFromBytes(wave);
        auto label = tone_import_detail::decodeCp932Name(name);
        if (label.empty()) {
            label = tone_import_detail::paddedImportNumber(
                static_cast<unsigned>(index));
        }
        auto candidate = tone_import_detail::makeSccCandidate(
            waveform,
            std::move(label),
            "VCD");
        CompositeLayer layer;
        layer.name = candidate.name;
        layer.source = TimbreSource::Scc;
        if (data[2] > 15) {
            candidate.warnings.push_back(
                {"MuSICA sustain exceeds 15; envelope was skipped"});
        } else if (musicaEnvelopeToLayer(
                       data[0], data[1], data[2], data[3], layer)) {
            tone_import_detail::DefinedTones tones;
            tones.scc[0] = waveform;
            std::vector<ImportWarning> warnings;
            tone_import_detail::EnvelopeChipUse usage;
            usage.scc = true;
            usage.primary_scc = 0;
            usage.track_scc.insert(0);
            candidate.composite_alternative =
                tone_import_detail::makeSelfContainedComposite(
                    std::move(layer), tones, candidate.name, warnings, usage);
            candidate.type = ImportedToneType::Composite;
            candidate.default_register_as = ImportRegisterAs::Composite;
            candidate.register_choices = {
                ImportRegisterAs::Composite, ImportRegisterAs::Scc};
            candidate.warnings.insert(
                candidate.warnings.end(),
                warnings.begin(),
                warnings.end());
        }
        tone_import_detail::addCandidate(
            result.candidates, std::move(candidate));
    }
    return result;
}

}  // namespace mgstc::engine
