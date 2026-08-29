#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mgstc/engine/pitch_sweep.hpp"
#include "mgstc/engine/software_lfo.hpp"
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

enum class TimbrePick : std::uint8_t {
    Library = 0,
    OpllRom = 1,
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
    std::uint64_t target_library_id{};
    TimbrePick timbre_pick{TimbrePick::Library};
    // §6.2.3: at the loop-start count, false = before `[` (first pass only),
    // true = after `[` (re-run on each `]` return). Ignored when the event's
    // count is not the layer's loop_start_count.
    bool after_loop_start{};
    // Volume-only authoring flag. When set, the sequence compiler emits a
    // 2n cc ramp command using the interval from the preceding volume spec.
    bool automatic{};
    // Volume-only. When set with automatic, mid-span zero-time commands keep
    // the unsplit remainder-ramp tick volumes (MGSC uses 1-count / `:` holds
    // instead of restarting `=`). Ignored when automatic is false.
    bool precise{};

    friend bool operator==(const EnvelopeEvent&, const EnvelopeEvent&)
        = default;
};

struct RateEnvelope {
    std::uint8_t tone_mode{};
    std::uint8_t noise{};
    std::uint8_t attack_level{};
    std::uint8_t attack_rate{};
    std::uint8_t decay_rate{};
    std::uint8_t sustain_level{};
    std::uint8_t sustain_rate{};
    std::uint8_t release_rate{};

    friend bool operator==(const RateEnvelope&, const RateEnvelope&)
        = default;
};

[[nodiscard]] inline bool rateEnvelopeIsUnset(
    const RateEnvelope& rate) noexcept {
    return rate.tone_mode == 0
        && rate.noise == 0
        && rate.attack_level == 0
        && rate.attack_rate == 0
        && rate.decay_rate == 0
        && rate.sustain_level == 0
        && rate.sustain_rate == 0
        && rate.release_rate == 0;
}

inline void seedDefaultRateEnvelope(
    RateEnvelope& rate,
    TimbreSource source) noexcept {
    if (!rateEnvelopeIsUnset(rate)) {
        return;
    }
    rate.attack_level = 0;
    rate.attack_rate = 64;
    rate.decay_rate = 32;
    rate.sustain_level = 128;
    rate.sustain_rate = 8;
    rate.release_rate = 16;
    rate.tone_mode =
        source == TimbreSource::Psg ? std::uint8_t{1} : std::uint8_t{0};
    rate.noise = 0;
}

[[nodiscard]] inline RateEnvelope clampRateEnvelope(
    RateEnvelope rate) noexcept {
    rate.tone_mode = std::min<std::uint8_t>(rate.tone_mode, 3);
    rate.noise = std::min<std::uint8_t>(rate.noise, 31);
    return rate;
}

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

enum class OpllRegisterAutoMode : std::uint8_t {
    Off = 0,
    Rise = 1,
    Fall = 2,
    Lfo = 3,
    FreeCurve = 4,
};

enum class OpllRegisterAutoTarget : std::uint8_t {
    TotalLevel = 0,
    Feedback = 1,
};

// Authoring model for OPLL MOD TL / FB time evolution. Distinct from the
// manual `y` lane (`timbre_automation` RegisterWrite). Product output and
// audition expand this to standard `yreg,data` (§6.2.4.5 / §7.5).
// Packing: reg2 = KSL|TL, reg3 = KSL|waves|FB — only the TL/FB field is
// replaced; other co-resident bits come from the active original image.
// Unavailable while the active `@` (or base) is an OPLL ROM timbre.
struct OpllRegisterAutoLane {
    OpllRegisterAutoMode mode{OpllRegisterAutoMode::Off};
    std::uint32_t start_count{};
    std::uint8_t depth{};
    std::uint8_t change_speed{1};
    std::uint8_t coarseness{1};
    std::uint8_t stop_position{};
    std::vector<std::uint8_t> free_curve;

    [[nodiscard]] bool active() const noexcept {
        return mode != OpllRegisterAutoMode::Off;
    }

    friend bool operator==(
        const OpllRegisterAutoLane&,
        const OpllRegisterAutoLane&) = default;
};

[[nodiscard]] constexpr std::uint8_t opllRegisterAutoValueMax(
    OpllRegisterAutoTarget target) noexcept {
    return target == OpllRegisterAutoTarget::TotalLevel ? 63 : 7;
}

[[nodiscard]] constexpr std::uint8_t opllRegisterAutoRegisterNumber(
    OpllRegisterAutoTarget target) noexcept {
    return target == OpllRegisterAutoTarget::TotalLevel ? 2 : 3;
}

enum class StartDelayForm : std::uint8_t {
    // Track MML `r<n>` — note-length rest (tempo-dependent wall-clock).
    NoteLength = 0,
    // Track MML `r%<n>` — absolute 1/60s ticks (tempo-independent wall-clock).
    // Product rule (Takao): changing global tempo must NOT change r% wait;
    // only `r` scales. MGSC `%` step *identity* (a4=a%48) is still documented
    // separately; audition maps r% to driver 1/60s ticks for the editor grid.
    AbsoluteTicks = 1,
};

// MGSC 1.11: `a4 = a%48` (quarter = 48 steps, whole = 192).
// `#tempo` 57～2047, default 120. Shortest playable note denom ≈
// floor(14400/tempo) (capped at 192) lasts one ≤1/60s interrupt.
constexpr int kMgscTempoMin = 57;
constexpr int kMgscTempoMax = 2047;
constexpr int kMgscDefaultTempo = 120;
constexpr int kMgscWholeSteps = 192;
constexpr int kMgscQuarterSteps = 48;
constexpr double kMgscInterruptHz = 60.0;

// Wall-clock delay before key-on (keyboard / PC / MIDI / 1s preview).
// `r%<n>`: n / 60 s (absolute tick; tempo ignored).
// `r<n>`:  240000/(tempo*n) ms — quarter (n=4) = 60000/tempo ms
//         (= same 14400/tempo family as MGSC shortest-note note).
[[nodiscard]] inline double startDelayMilliseconds(
    StartDelayForm form,
    std::uint32_t value,
    int tempo_bpm) noexcept {
    if (value == 0) {
        return 0.0;
    }
    if (form == StartDelayForm::AbsoluteTicks) {
        return static_cast<double>(value) * (1000.0 / kMgscInterruptHz);
    }
    const int tempo = std::clamp(
        tempo_bpm, kMgscTempoMin, kMgscTempoMax);
    return 240000.0
        / (static_cast<double>(tempo) * static_cast<double>(value));
}

// Shared-timeline tint counts (≈1/60s). `r%` exact; `r` rounds from ms.
[[nodiscard]] inline std::uint32_t startDelayGridCounts(
    StartDelayForm form,
    std::uint32_t value,
    int tempo_bpm) noexcept {
    if (value == 0) {
        return 0;
    }
    if (form == StartDelayForm::AbsoluteTicks) {
        return value;
    }
    const double ms =
        startDelayMilliseconds(form, value, tempo_bpm);
    return static_cast<std::uint32_t>(
        std::llround(ms * (kMgscInterruptHz / 1000.0)));
}

struct CompositeLayer {
    std::string name;
    TimbreSource source{TimbreSource::Psg};
    std::uint8_t channel{};
    // Library snapshot for SCC / OPLL original. Mutually exclusive with
    // `base_opll_rom` on OPLL layers.
    std::optional<SavedTimbreReference> base_timbre;
    // OPLL ROM patch `@0`–`@14` as the track-side base timbre.
    std::optional<std::uint8_t> base_opll_rom;
    std::int8_t relative_semitones{};
    std::int16_t detune{};
    // Track MML `@\` (stacks with `detune` / `\`). PSG/SCC signed; OPLL 0..255.
    std::int32_t micro_detune{};
    // Track-side rest before key-on (MML `r` / `r%`), not an @e wait.
    StartDelayForm start_delay_form{StartDelayForm::AbsoluteTicks};
    std::uint32_t start_delay_value{};
    std::uint8_t volume{15};
    // `@e` / `@r` definition number (0–31). New layers start at 00 sequential.
    std::uint8_t envelope_number{};
    // Track MML `k` (PSG/SCC). 0 = immediate key-off. Ignored with `@r` / HW EG.
    std::uint8_t key_off_hang{};
    // Track MML `p` (PSG/SCC). Mutually exclusive with `software_lfo`.
    PitchSweepSettings pitch_sweep{};
    // Track MML `so` when true. Unspecified / `sf` when false (OPLL only).
    bool opll_sustain{};
    // MGSDRV software LFO (pre-key-on `h` / `@p`). Disabled ⇒ no `h`.
    SoftwareLfoSettings software_lfo{};
    SoftwareEnvelope volume_envelope;
    SoftwareEnvelope pitch_envelope;
    std::vector<EnvelopeEvent> timbre_automation;
    EnvelopeTimeline envelope_timeline;
    OpllRegisterAutoLane opll_tl_auto;
    OpllRegisterAutoLane opll_fb_auto;
    bool enabled{true};
    bool muted{};
    bool solo{};

    friend bool operator==(const CompositeLayer&, const CompositeLayer&)
        = default;
};

struct CompositeTimbre {
    static constexpr std::uint32_t kFormatVersion = 18;
    static constexpr std::uint32_t kMinimumReadableFormatVersion = 5;

    std::uint32_t format_version{kFormatVersion};
    std::string name;
    std::vector<std::string> tags;
    std::string memo;
    bool favorite{};
    // MGSC #tempo for this composite (audition + library save). Not app-global.
    int playback_tempo{kMgscDefaultTempo};
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

// Snapshot id for the Triangle waveform assigned when adding an SCC channel.
// Not a library entry; the dedicated SCC editor stays closed until a saved
// timbre is chosen.
inline constexpr std::uint64_t kSccTrianglePresetLibraryId =
    0x4D47534300010001ULL;

void seedDefaultLayerTimbre(CompositeLayer& layer) noexcept;

[[nodiscard]] bool layerIsAudible(
    const CompositeTimbre& timbre,
    std::size_t layer_index) noexcept;

[[nodiscard]] std::optional<std::uint8_t> layerMidiNote(
    const CompositeLayer& layer,
    std::uint8_t root_midi_note) noexcept;

[[nodiscard]] std::optional<std::uint8_t> firstAvailableChannel(
    const CompositeTimbre& timbre,
    TimbreSource source) noexcept;

[[nodiscard]] std::uint8_t nextFreeEnvelopeNumber(
    const CompositeTimbre& timbre) noexcept;

bool removeCompositeLayer(
    CompositeTimbre& timbre,
    std::size_t layer_index) noexcept;

// Copies a layer onto the lowest free channel of the same source.
// Cross-source copies (e.g. SCC → OPLL) are not performed. Returns the
// new layer index, or nullopt if the source has no free channel.
[[nodiscard]] std::optional<std::size_t> duplicateCompositeLayer(
    CompositeTimbre& timbre,
    std::size_t layer_index);

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

[[nodiscard]] std::optional<std::uint8_t> assignedNumberForLibraryId(
    const TimbreNumberResolution& numbers,
    std::uint64_t library_id) noexcept;

[[nodiscard]] std::optional<std::uint8_t> envelopeEventTimbreNumber(
    const EnvelopeEvent& event,
    const TimbreNumberResolution* numbers = nullptr) noexcept;

// Track-side / §6.5.1 helpers for the layer base patch (`@n` or ROM).
[[nodiscard]] inline bool layerUsesOpllRomBase(
    const CompositeLayer& layer) noexcept {
    return layer.source == TimbreSource::Opll
        && layer.base_opll_rom.has_value()
        && *layer.base_opll_rom <= 14;
}

[[nodiscard]] std::optional<std::uint8_t> layerBasePatchNumber(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr) noexcept;

[[nodiscard]] bool layerEnvelopeHasPatchSlide(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr) noexcept;

// True when the OPLL layer mutates original-tone regs 0–7 via manual `y`
// and/or active TL/FB auto, so `@e` must re-establish the base original at
// count 0 (§6.5.1). ROM-base layers are excluded (`@` ROM does not reload
// original regs). Requires a library `base_timbre` to restore against.
[[nodiscard]] bool layerEnvelopeNeedsOriginalToneRestore(
    const CompositeLayer& layer) noexcept;

// §6.5.1: patch slide and/or original-tone y / TL/FB auto → leading base `@`.
[[nodiscard]] bool layerEnvelopeNeedsLeadingBasePatch(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr) noexcept;

[[nodiscard]] bool envelopeEventReferencesLibrary(
    const EnvelopeEvent& event,
    std::uint64_t library_id) noexcept;

[[nodiscard]] std::vector<TimbreUse> findTimbreUses(
    std::span<const CompositeTimbre> composites,
    std::uint64_t library_id);

std::size_t updateTimbreReferences(
    std::span<CompositeTimbre> composites,
    const TimbreLibraryEntry& entry);

}  // namespace mgstc::engine
