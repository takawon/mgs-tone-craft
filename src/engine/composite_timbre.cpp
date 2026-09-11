#include "mgstc/engine/composite_timbre.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "mgstc/engine/scc_waveform.hpp"

namespace mgstc::engine {
namespace {

SoftwareEnvelope defaultEnvelope() {
    SoftwareEnvelope envelope;
    envelope.events = {
        {
            .kind = EnvelopeEventKind::Volume,
            .value = 15,
        },
        {
            .kind = EnvelopeEventKind::Wait,
            .count = 60,
        },
    };
    return envelope;
}

CompositeLayer makeLayer(
    std::string name,
    TimbreSource source,
    std::uint8_t channel) {
    CompositeLayer layer;
    layer.name = std::move(name);
    layer.source = source;
    layer.channel = channel;
    layer.volume_envelope = defaultEnvelope();
    return layer;
}

std::size_t sourceIndex(TimbreSource source) noexcept {
    return static_cast<std::size_t>(source);
}

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

}  // namespace

CompositeTimbre defaultCompositeTimbre() {
    CompositeTimbre timbre;
    timbre.format_version = CompositeTimbre::kFormatVersion;
    timbre.name = "New Composite Timbre";
    timbre.layers = {
        makeLayer("PSG Layer", TimbreSource::Psg, 0),
        makeLayer("SCC Layer", TimbreSource::Scc, 0),
        makeLayer("OPLL Layer", TimbreSource::Opll, 0),
    };
    for (std::size_t index = 0; index < timbre.layers.size(); ++index) {
        timbre.layers[index].envelope_number =
            static_cast<std::uint8_t>(std::min<std::size_t>(index, 31));
    }
    return timbre;
}

void seedDefaultLayerTimbre(CompositeLayer& layer) noexcept {
    if (layer.source == TimbreSource::Opll) {
        layer.base_timbre.reset();
        layer.base_opll_rom = 0;
        return;
    }
    if (layer.source != TimbreSource::Scc) {
        return;
    }
    const auto waveform = generateSccPreset(
        SccWavePreset::Triangle, SccHarmonic::One);
    SavedTimbreReference reference;
    reference.library_id = kSccTrianglePresetLibraryId;
    reference.revision = 1;
    reference.name = "Triangle";
    reference.source = TimbreSource::Scc;
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        reference.scc_waveform[index] =
            static_cast<std::uint8_t>(waveform[index]);
    }
    layer.base_opll_rom.reset();
    layer.base_timbre = std::move(reference);
}

bool layerIsAudible(
    const CompositeTimbre& timbre,
    std::size_t layer_index) noexcept {
    if (layer_index >= timbre.layers.size()) {
        return false;
    }
    const auto& layer = timbre.layers[layer_index];
    if (!layer.enabled || layer.muted) {
        return false;
    }
    bool any_solo = false;
    for (const auto& candidate : timbre.layers) {
        any_solo = any_solo || (candidate.enabled && candidate.solo);
    }
    return !any_solo || layer.solo;
}

std::optional<std::uint8_t> layerMidiNote(
    const CompositeLayer& layer,
    std::uint8_t root_midi_note) noexcept {
    const int note = static_cast<int>(root_midi_note)
        + static_cast<int>(layer.relative_semitones);
    if (note < 24 || note > 119) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(note);
}

std::optional<std::uint8_t> firstAvailableChannel(
    const CompositeTimbre& timbre,
    TimbreSource source) noexcept {
    constexpr std::array<std::uint8_t, 3> capacities{3, 5, 9};
    const auto capacity = capacities[sourceIndex(source)];
    std::array<bool, 9> used_channels{};
    for (const auto& layer : timbre.layers) {
        if (layer.source == source && layer.channel < capacity) {
            used_channels[layer.channel] = true;
        }
    }
    const auto available = std::find(
        used_channels.begin(),
        used_channels.begin() + capacity,
        false);
    if (available == used_channels.begin() + capacity) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(
        std::distance(used_channels.begin(), available));
}

std::uint8_t nextFreeEnvelopeNumber(const CompositeTimbre& timbre) noexcept {
    std::array<bool, 32> used{};
    for (const auto& layer : timbre.layers) {
        if (layer.envelope_number < used.size()) {
            used[layer.envelope_number] = true;
        }
    }
    const auto free = std::find(used.begin(), used.end(), false);
    if (free == used.end()) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::distance(used.begin(), free));
}

bool removeCompositeLayer(
    CompositeTimbre& timbre,
    std::size_t layer_index) noexcept {
    if (layer_index >= timbre.layers.size()) {
        return false;
    }
    timbre.layers.erase(
        timbre.layers.begin() + static_cast<std::ptrdiff_t>(layer_index));
    return true;
}

std::optional<std::size_t> duplicateCompositeLayer(
    CompositeTimbre& timbre,
    std::size_t layer_index) {
    if (layer_index >= timbre.layers.size()) {
        return std::nullopt;
    }
    const auto source = timbre.layers[layer_index].source;
    const auto free_channel = firstAvailableChannel(timbre, source);
    if (!free_channel) {
        return std::nullopt;
    }
    auto layer = timbre.layers[layer_index];
    layer.channel = *free_channel;
    layer.envelope_number = nextFreeEnvelopeNumber(timbre);
    layer.name = std::string(sourceName(source)) + " Ch."
        + std::to_string(static_cast<int>(layer.channel) + 1);
    // TL/FB auto is chip-wide exclusive; the original keeps ownership.
    layer.opll_tl_auto = {};
    layer.opll_fb_auto = {};
    timbre.layers.push_back(std::move(layer));
    return timbre.layers.size() - 1;
}

void setEnvelopeTimelineRange(
    EnvelopeTimeline& timeline,
    std::uint32_t length_counts,
    std::optional<std::uint32_t> loop_start_count,
    std::optional<std::uint32_t> loop_end_count) noexcept {
    timeline.length_counts = std::clamp(
        length_counts,
        std::uint32_t{1},
        EnvelopeTimeline::kMaximumLengthCounts);
    const auto clamp_marker = [&timeline](
        std::optional<std::uint32_t> marker) {
        if (marker) {
            *marker = std::min(*marker, timeline.length_counts);
        }
        return marker;
    };
    timeline.loop_start_count = clamp_marker(loop_start_count);
    timeline.loop_end_count = clamp_marker(loop_end_count);
    if (timeline.loop_start_count
        && timeline.loop_end_count
        && *timeline.loop_start_count > *timeline.loop_end_count) {
        std::swap(
            timeline.loop_start_count,
            timeline.loop_end_count);
    }
}

CompositeValidation validateCompositeTimbre(
    const CompositeTimbre& timbre) {
    CompositeValidation result;
    std::array<std::array<bool, 16>, 3> used{};
    constexpr std::array<std::uint8_t, 3> capacities{3, 5, 9};
    for (const auto& layer : timbre.layers) {
        if (!layer.enabled) {
            continue;
        }
        switch (layer.source) {
        case TimbreSource::Psg:
            ++result.psg_channels;
            break;
        case TimbreSource::Scc:
            ++result.scc_channels;
            break;
        case TimbreSource::Opll:
            ++result.opll_channels;
            break;
        }
        const auto source = sourceIndex(layer.source);
        if (layer.channel >= capacities[source]) {
            result.warnings.push_back(
                std::string(sourceName(layer.source))
                + " layer uses an unsupported channel");
            continue;
        }
        if (used[source][layer.channel]) {
            result.warnings.push_back(
                std::string(sourceName(layer.source))
                + " channel "
                + std::to_string(
                    static_cast<unsigned int>(layer.channel) + 1)
                + " is assigned more than once");
        }
        used[source][layer.channel] = true;
    }
    std::array<int, 32> envelope_users{};
    for (const auto& layer : timbre.layers) {
        if (layer.envelope_number > 31) {
            result.warnings.push_back(
                "Envelope number is outside 0-31");
            continue;
        }
        ++envelope_users[layer.envelope_number];
    }
    for (std::size_t number = 0; number < envelope_users.size(); ++number) {
        if (envelope_users[number] > 1) {
            const auto digits = number < 10
                ? std::string("0") + std::to_string(number)
                : std::to_string(number);
            result.warnings.push_back(
                "@e" + digits
                + " is used by more than one layer");
        }
    }
    if (timbre.layers.empty()) {
        result.warnings.emplace_back(
            "Composite timbre has no layers");
    }
    return result;
}

SavedTimbreReference makeSavedTimbreReference(
    const TimbreLibraryEntry& entry) {
    SavedTimbreReference reference;
    reference.library_id = entry.id;
    reference.revision = entry.revision;
    reference.name = entry.name;
    reference.source =
        entry.category == TimbreCategory::Scc
        ? TimbreSource::Scc
        : TimbreSource::Opll;
    reference.opll_registers = entry.opll_registers;
    reference.scc_waveform = entry.scc_waveform;
    return reference;
}

const SavedTimbreReference* findEmbeddedTimbreSnapshot(
    const CompositeTimbre& timbre,
    std::uint64_t library_id) noexcept {
    if (library_id == 0) {
        return nullptr;
    }
    for (const auto& layer : timbre.layers) {
        if (layer.base_timbre
            && layer.base_timbre->library_id == library_id) {
            return &*layer.base_timbre;
        }
    }
    for (const auto& reference : timbre.embedded_timbres) {
        if (reference.library_id == library_id) {
            return &reference;
        }
    }
    return nullptr;
}

TimbreNumberResolution resolveTimbreNumbers(
    const CompositeTimbre& timbre,
    std::uint8_t minimum_number,
    std::uint8_t maximum_number) {
    TimbreNumberResolution result;
    if (minimum_number > maximum_number) {
        result.warnings.emplace_back(
            "Timbre number range is invalid");
        return result;
    }

    using TimbreKey = std::pair<TimbreSource, std::uint64_t>;
    std::map<TimbreKey, std::uint8_t> assigned;
    std::array<std::map<std::uint8_t, std::uint64_t>, 3> occupied;

    const auto assign = [&](std::size_t layer_index,
                            const SavedTimbreReference& reference,
                            std::uint8_t number,
                            bool manual) {
        const TimbreKey key{reference.source, reference.library_id};
        assigned[key] = number;
        occupied[sourceIndex(reference.source)][number] =
            reference.library_id;
        result.assignments.push_back(
            {
                .layer_index = layer_index,
                .library_id = reference.library_id,
                .number = number,
                .manually_assigned = manual,
            });
    };

    for (std::size_t index = 0;
         index < timbre.layers.size();
         ++index) {
        const auto& layer = timbre.layers[index];
        if (!layer.base_timbre
            || layer.base_timbre->number_mode
                != TimbreNumberMode::Manual) {
            continue;
        }
        const auto& reference = *layer.base_timbre;
        if (reference.source != layer.source) {
            result.warnings.push_back(
                layer.name + " references a different sound source");
            continue;
        }
        if (!reference.manual_number) {
            result.warnings.push_back(
                layer.name + " has no manual timbre number");
            continue;
        }
        const auto number = *reference.manual_number;
        if (number < minimum_number || number > maximum_number) {
            result.warnings.push_back(
                layer.name + " uses an out-of-range timbre number");
            continue;
        }
        const TimbreKey key{
            reference.source, reference.library_id};
        if (const auto existing = assigned.find(key);
            existing != assigned.end()) {
            if (existing->second != number) {
                result.warnings.push_back(
                    layer.name
                    + " assigns two numbers to the same timbre");
            } else {
                result.assignments.push_back(
                    {
                        .layer_index = index,
                        .library_id = reference.library_id,
                        .number = number,
                        .manually_assigned = true,
                    });
            }
            continue;
        }
        const auto source = sourceIndex(reference.source);
        if (const auto collision = occupied[source].find(number);
            collision != occupied[source].end()
            && collision->second != reference.library_id) {
            result.warnings.push_back(
                layer.name + " duplicates timbre number "
                + std::to_string(number));
            continue;
        }
        assign(index, reference, number, true);
    }

    for (std::size_t index = 0;
         index < timbre.layers.size();
         ++index) {
        const auto& layer = timbre.layers[index];
        if (!layer.base_timbre) {
            continue;
        }
        const auto& reference = *layer.base_timbre;
        if (reference.source != layer.source) {
            if (reference.number_mode
                == TimbreNumberMode::Automatic) {
                result.warnings.push_back(
                    layer.name
                    + " references a different sound source");
            }
            continue;
        }
        const TimbreKey key{
            reference.source, reference.library_id};
        if (const auto existing = assigned.find(key);
            existing != assigned.end()) {
            if (std::none_of(
                    result.assignments.begin(),
                    result.assignments.end(),
                    [index](const auto& item) {
                        return item.layer_index == index;
                    })) {
                result.assignments.push_back(
                    {
                        .layer_index = index,
                        .library_id = reference.library_id,
                        .number = existing->second,
                        .manually_assigned =
                            reference.number_mode
                            == TimbreNumberMode::Manual,
                    });
            }
            continue;
        }
        if (reference.number_mode == TimbreNumberMode::Manual) {
            continue;
        }
        const auto source = sourceIndex(reference.source);
        std::optional<std::uint8_t> available;
        for (unsigned int candidate = minimum_number;
             candidate <= maximum_number;
             ++candidate) {
            const auto number =
                static_cast<std::uint8_t>(candidate);
            if (!occupied[source].contains(number)) {
                available = number;
                break;
            }
        }
        if (!available) {
            result.warnings.push_back(
                std::string(sourceName(reference.source))
                + " has no free timbre numbers");
            continue;
        }
        assign(index, reference, *available, false);
    }

    for (std::size_t index = 0;
         index < timbre.layers.size();
         ++index) {
        const auto& layer = timbre.layers[index];
        if (layer.source == TimbreSource::Psg) {
            continue;
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != EnvelopeEventKind::Timbre
                || event.timbre_pick != TimbrePick::Library
                || event.target_library_id == 0) {
                continue;
            }
            const TimbreKey key{
                layer.source, event.target_library_id};
            if (assigned.contains(key)) {
                continue;
            }
            const auto source = sourceIndex(layer.source);
            std::optional<std::uint8_t> available;
            for (unsigned int candidate = minimum_number;
                 candidate <= maximum_number;
                 ++candidate) {
                const auto number =
                    static_cast<std::uint8_t>(candidate);
                if (!occupied[source].contains(number)) {
                    available = number;
                    break;
                }
            }
            if (!available) {
                result.warnings.push_back(
                    std::string(sourceName(layer.source))
                    + " has no free timbre numbers");
                continue;
            }
            SavedTimbreReference reference;
            reference.library_id = event.target_library_id;
            reference.source = layer.source;
            assign(index, reference, *available, false);
        }
    }

    std::sort(
        result.assignments.begin(),
        result.assignments.end(),
        [](const auto& left, const auto& right) {
            return left.layer_index < right.layer_index;
        });
    return result;
}

std::optional<std::uint8_t> assignedNumberForLibraryId(
    const TimbreNumberResolution& numbers,
    std::uint64_t library_id) noexcept {
    if (library_id == 0) {
        return std::nullopt;
    }
    for (const auto& assignment : numbers.assignments) {
        if (assignment.library_id == library_id) {
            return assignment.number;
        }
    }
    return std::nullopt;
}

std::optional<std::uint8_t> envelopeEventTimbreNumber(
    const EnvelopeEvent& event,
    const TimbreNumberResolution* numbers) noexcept {
    if (event.kind != EnvelopeEventKind::Timbre) {
        return std::nullopt;
    }
    if (event.timbre_pick == TimbrePick::OpllRom) {
        if (event.value < 0 || event.value > 14) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(event.value);
    }
    if (numbers != nullptr && event.target_library_id != 0) {
        if (const auto assigned = assignedNumberForLibraryId(
                *numbers, event.target_library_id)) {
            return assigned;
        }
    }
    if (event.value < 0 || event.value > 31) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(event.value);
}

std::optional<std::uint8_t> layerBasePatchNumber(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers) noexcept {
    if (layerUsesOpllRomBase(layer)) {
        return *layer.base_opll_rom;
    }
    if (!layer.base_timbre) {
        return std::nullopt;
    }
    if (numbers != nullptr) {
        if (const auto assigned = assignedNumberForLibraryId(
                *numbers, layer.base_timbre->library_id)) {
            return assigned;
        }
    }
    if (layer.base_timbre->manual_number) {
        return layer.base_timbre->manual_number;
    }
    return std::nullopt;
}

bool layerEnvelopeHasPatchSlide(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers) noexcept {
    if (layer.source == TimbreSource::Psg) {
        return false;
    }
    const auto base = layerBasePatchNumber(layer, numbers);
    for (const auto& event : layer.timbre_automation) {
        if (event.kind != EnvelopeEventKind::Timbre) {
            continue;
        }
        const auto number = envelopeEventTimbreNumber(event, numbers);
        if (!number) {
            continue;
        }
        if (!base || *number != *base) {
            return true;
        }
        // Same number but different pick kind still counts as a slide.
        if (layerUsesOpllRomBase(layer)) {
            if (event.timbre_pick != TimbrePick::OpllRom) {
                return true;
            }
        } else if (layer.base_timbre
                   && event.timbre_pick == TimbrePick::OpllRom) {
            return true;
        } else if (layer.base_timbre
                   && event.timbre_pick == TimbrePick::Library
                   && event.target_library_id != 0
                   && event.target_library_id
                       != layer.base_timbre->library_id) {
            return true;
        }
    }
    return false;
}

bool layerEnvelopeNeedsOriginalToneRestore(
    const CompositeLayer& layer) noexcept {
    if (layer.source != TimbreSource::Opll) {
        return false;
    }
    // ROM `@n` selects an instrument number; it does not reload original
    // timbre registers 0–7. TL/FB auto / original y are already restricted
    // on ROM-base layers for this channel's sound.
    if (layerUsesOpllRomBase(layer) || !layer.base_timbre) {
        return false;
    }
    if (layer.opll_tl_auto.active() || layer.opll_fb_auto.active()) {
        return true;
    }
    for (const auto& event : layer.timbre_automation) {
        if (event.kind == EnvelopeEventKind::RegisterWrite
            && event.value >= 0
            && event.value <= 7) {
            return true;
        }
    }
    return false;
}

bool layerEnvelopeNeedsLeadingBasePatch(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers) noexcept {
    return layerEnvelopeHasPatchSlide(layer, numbers)
        || layerEnvelopeNeedsOriginalToneRestore(layer);
}

bool envelopeEventReferencesLibrary(
    const EnvelopeEvent& event,
    std::uint64_t library_id) noexcept {
    return event.kind == EnvelopeEventKind::Timbre
        && event.timbre_pick == TimbrePick::Library
        && event.target_library_id != 0
        && event.target_library_id == library_id;
}

std::vector<TimbreUse> findTimbreUses(
    std::span<const CompositeTimbre> composites,
    std::uint64_t library_id) {
    std::vector<TimbreUse> uses;
    for (std::size_t composite_index = 0;
         composite_index < composites.size();
         ++composite_index) {
        const auto& composite = composites[composite_index];
        for (std::size_t layer_index = 0;
             layer_index < composite.layers.size();
             ++layer_index) {
            const auto& layer = composite.layers[layer_index];
            const bool base_match = layer.base_timbre
                && layer.base_timbre->library_id == library_id;
            const bool envelope_match = std::any_of(
                layer.timbre_automation.begin(),
                layer.timbre_automation.end(),
                [library_id](const EnvelopeEvent& event) {
                    return envelopeEventReferencesLibrary(
                        event, library_id);
                });
            if (!base_match && !envelope_match) {
                continue;
            }
            uses.push_back(
                {
                    .composite_index = composite_index,
                    .layer_index = layer_index,
                    .composite_name = composite.name,
                    .layer_name = layer.name,
                });
        }
    }
    return uses;
}

std::size_t updateTimbreReferences(
    std::span<CompositeTimbre> composites,
    const TimbreLibraryEntry& entry) {
    const auto replacement = makeSavedTimbreReference(entry);
    std::size_t updated{};
    for (auto& composite : composites) {
        for (auto& layer : composite.layers) {
            if (layer.base_timbre
                && layer.base_timbre->library_id == entry.id
                && layer.source == replacement.source) {
                const auto number_mode =
                    layer.base_timbre->number_mode;
                const auto manual_number =
                    layer.base_timbre->manual_number;
                layer.base_timbre = replacement;
                layer.base_timbre->number_mode = number_mode;
                layer.base_timbre->manual_number = manual_number;
                ++updated;
            }
            for (const auto& event : layer.timbre_automation) {
                if (envelopeEventReferencesLibrary(event, entry.id)) {
                    ++updated;
                    break;
                }
            }
        }
    }
    return updated;
}

}  // namespace mgstc::engine
