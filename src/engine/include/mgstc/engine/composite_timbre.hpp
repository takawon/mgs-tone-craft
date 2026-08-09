#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

enum class TimbreSource : std::uint8_t {
    Psg,
    Scc,
    Opll,
};

enum class EnvelopeKind : std::uint8_t {
    Sequence,
    Rate,
};

enum class EnvelopeEventKind : std::uint8_t {
    Volume,
    Wait,
    Pitch,
    Timbre,
    RegisterWrite,
    LoopStart,
    LoopEnd,
};

enum class TimbreNumberMode : std::uint8_t {
    Automatic,
    Manual,
};

struct SavedTimbreReference {
    std::uint64_t library_id{};
    std::uint32_t revision{1};
    std::string name;
    TimbreSource source{TimbreSource::Psg};
    TimbreNumberMode number_mode{TimbreNumberMode::Automatic};
    std::optional<std::uint8_t> manual_number;
    std::array<std::uint8_t, 8> opll_registers{};
    std::array<std::uint8_t, 32> scc_waveform{};

    friend bool operator==(
        const SavedTimbreReference&,
        const SavedTimbreReference&) = default;
};

struct EnvelopeEvent {
    EnvelopeEventKind kind{EnvelopeEventKind::Volume};
    std::int32_t value{};
    std::int32_t secondary{};
    std::uint32_t count{};

    friend bool operator==(const EnvelopeEvent&, const EnvelopeEvent&)
        = default;
};

struct RateEnvelope {
    std::uint8_t attack_level{};
    std::uint8_t attack_rate{};
    std::uint8_t decay_rate{};
    std::uint8_t sustain_level{};
    std::uint8_t sustain_rate{};
    std::uint8_t release_rate{};

    friend bool operator==(const RateEnvelope&, const RateEnvelope&)
        = default;
};

struct EnvelopeTimeline {
    static constexpr std::uint32_t kDefaultLengthCounts = 120;
    static constexpr std::uint32_t kMaximumLengthCounts = 65'535;

    std::uint32_t length_counts{kDefaultLengthCounts};
    std::optional<std::uint32_t> loop_start_count;
    std::optional<std::uint32_t> loop_end_count;

    friend bool operator==(
        const EnvelopeTimeline&,
        const EnvelopeTimeline&) = default;
};

struct SoftwareEnvelope {
    EnvelopeKind kind{EnvelopeKind::Sequence};
    std::vector<EnvelopeEvent> events;
    RateEnvelope rate;

    friend bool operator==(
        const SoftwareEnvelope&,
        const SoftwareEnvelope&) = default;
};

struct CompositeLayer {
    std::string name;
    TimbreSource source{TimbreSource::Psg};
    std::uint8_t channel{};
    std::optional<SavedTimbreReference> base_timbre;
    std::int8_t relative_semitones{};
    std::int16_t detune{};
    std::uint32_t start_delay_counts{};
    std::uint8_t volume{15};
    SoftwareEnvelope volume_envelope;
    SoftwareEnvelope pitch_envelope;
    std::vector<EnvelopeEvent> timbre_automation;
    EnvelopeTimeline envelope_timeline;
    bool enabled{true};
    bool muted{};
    bool solo{};

    friend bool operator==(const CompositeLayer&, const CompositeLayer&)
        = default;
};

struct CompositeTimbre {
    static constexpr std::uint32_t kFormatVersion = 5;

    std::uint32_t format_version{kFormatVersion};
    std::string name;
    std::vector<std::string> tags;
    std::string memo;
    bool favorite{};
    std::vector<CompositeLayer> layers;

    friend bool operator==(const CompositeTimbre&, const CompositeTimbre&)
        = default;
};

struct CompositeValidation {
    std::uint8_t psg_channels{};
    std::uint8_t scc_channels{};
    std::uint8_t opll_channels{};
    std::vector<std::string> warnings;

    [[nodiscard]] bool valid() const noexcept {
        return warnings.empty();
    }
};

struct ResolvedTimbreNumber {
    std::size_t layer_index{};
    std::uint64_t library_id{};
    std::uint8_t number{};
    bool manually_assigned{};

    friend bool operator==(
        const ResolvedTimbreNumber&,
        const ResolvedTimbreNumber&) = default;
};

struct TimbreNumberResolution {
    std::vector<ResolvedTimbreNumber> assignments;
    std::vector<std::string> warnings;

    [[nodiscard]] bool valid() const noexcept {
        return warnings.empty();
    }
};

struct TimbreUse {
    std::size_t composite_index{};
    std::size_t layer_index{};
    std::string composite_name;
    std::string layer_name;
};

[[nodiscard]] CompositeTimbre defaultCompositeTimbre();

[[nodiscard]] bool layerIsAudible(
    const CompositeTimbre& timbre,
    std::size_t layer_index) noexcept;

[[nodiscard]] std::optional<std::uint8_t> layerMidiNote(
    const CompositeLayer& layer,
    std::uint8_t root_midi_note) noexcept;

[[nodiscard]] std::optional<std::uint8_t> firstAvailableChannel(
    const CompositeTimbre& timbre,
    TimbreSource source) noexcept;

bool removeCompositeLayer(
    CompositeTimbre& timbre,
    std::size_t layer_index) noexcept;

void setEnvelopeTimelineRange(
    EnvelopeTimeline& timeline,
    std::uint32_t length_counts,
    std::optional<std::uint32_t> loop_start_count,
    std::optional<std::uint32_t> loop_end_count) noexcept;

[[nodiscard]] CompositeValidation validateCompositeTimbre(
    const CompositeTimbre& timbre);

[[nodiscard]] SavedTimbreReference makeSavedTimbreReference(
    const TimbreLibraryEntry& entry);

[[nodiscard]] TimbreNumberResolution resolveTimbreNumbers(
    const CompositeTimbre& timbre,
    std::uint8_t minimum_number = 15,
    std::uint8_t maximum_number = 31);

[[nodiscard]] std::vector<TimbreUse> findTimbreUses(
    std::span<const CompositeTimbre> composites,
    std::uint64_t library_id);

std::size_t updateTimbreReferences(
    std::span<CompositeTimbre> composites,
    const TimbreLibraryEntry& entry);

}  // namespace mgstc::engine
