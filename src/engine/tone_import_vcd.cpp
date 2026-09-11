#include "tone_import_internal.hpp"

#include <algorithm>
#include <array>

namespace mgstc::engine {
namespace {

constexpr std::size_t kVcdSize = 0x10B8;
constexpr std::size_t kOpllNameOffset = 0x0000;
constexpr std::size_t kSccNameOffset = 0x0410;
constexpr std::size_t kOpllDataOffset = 0x05A0;
constexpr std::size_t kSccDataOffset = 0x09B0;
constexpr int kOpllCount = 100;
constexpr int kSccCount = 50;

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
        const int wait = packed >> 4;
        const int delta = packed & 0x0F;
        if (wait == 0 || delta == 0) {
            return true;
        }
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
        return true;
    };

    std::uint8_t volume = 0;
    std::uint32_t count = 0;
    std::vector<EnvelopeEvent> events;
    events.push_back({
        .kind = EnvelopeEventKind::Volume,
        .value = 0,
        .count = 0,
    });
    run(attack, true, 15, volume, count, events);
    run(decay, false, sustain, volume, count, events);
    if (volume != sustain) {
        count = std::min<std::uint32_t>(
            EnvelopeTimeline::kMaximumLengthCounts, count + 1);
        volume = sustain;
        events.push_back({
            .kind = EnvelopeEventKind::Volume,
            .value = volume,
            .count = count,
        });
    }
    run(release, false, 0, volume, count, events);
    if (events.size() <= 1) {
        return false;
    }
    layer.source = TimbreSource::Scc;
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.volume_envelope.events = std::move(events);
    layer.envelope_timeline.length_counts =
        std::max<std::uint32_t>(1, count);
    return true;
}

bool slotInUse(std::span<const std::uint8_t> name, std::span<const std::uint8_t> data) {
    return !tone_import_detail::decodeCp932Name(name).empty()
        || !tone_import_detail::allZero(data);
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
        if (!slotInUse(name, data)) {
            continue;
        }
        std::array<std::uint8_t, 8> registers{};
        std::copy(data.begin(), data.end(), registers.begin());
        tone_import_detail::addCandidate(
            result.candidates,
            tone_import_detail::makeOpllCandidate(
                decodeOpllPatch(registers),
                tone_import_detail::decodeCp932Name(name),
                "VCD"));
    }
    for (int index = 0; index < kSccCount; ++index) {
        const auto name = bytes.subspan(
            kSccNameOffset + static_cast<std::size_t>(index) * 8, 8);
        const auto data = bytes.subspan(
            kSccDataOffset + static_cast<std::size_t>(index) * 36, 36);
        if (!slotInUse(name, data)) {
            continue;
        }
        std::array<std::uint8_t, 32> wave{};
        std::copy(data.begin() + 4, data.end(), wave.begin());
        const auto waveform = sccWaveformFromBytes(wave);
        auto candidate = tone_import_detail::makeSccCandidate(
            waveform,
            tone_import_detail::decodeCp932Name(name),
            "VCD");
        if (data[2] > 15) {
            candidate.warnings.push_back(
                {"MuSICA sustain exceeds 15; envelope was skipped"});
        }
        CompositeLayer layer;
        layer.name = candidate.name;
        if (musicaEnvelopeToLayer(data[0], data[1], data[2], data[3], layer)) {
            tone_import_detail::DefinedTones tones;
            tones.scc[0] = waveform;
            std::vector<ImportWarning> warnings;
            tone_import_detail::EnvelopeChipUse usage;
            usage.scc = true;
            usage.track_scc.insert(0);
            candidate.composite_alternative =
                tone_import_detail::makeSelfContainedComposite(
                    std::move(layer), tones, candidate.name, warnings, usage);
            candidate.default_register_as = ImportRegisterAs::Composite;
            candidate.register_choices = {
                ImportRegisterAs::Composite, ImportRegisterAs::Scc};
            candidate.warnings = std::move(warnings);
        }
        tone_import_detail::addCandidate(
            result.candidates, std::move(candidate));
    }
    return result;
}

}  // namespace mgstc::engine
