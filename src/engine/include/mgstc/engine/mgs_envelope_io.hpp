#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

enum class MgsEnvelopeIssue : std::uint8_t {
    InvalidDefinitionNumber,
    IncompleteLoop,
    InvalidLoopRange,
    DefinitionLengthExceeded,
    InvalidVolume,
    InvalidPitch,
    InvalidTimbre,
    InvalidRegisterWrite,
    InvalidAutomaticVolumeDuration,
    UnrepresentableModulationLoop,
    DuplicateZeroTimeCommand,
    InvalidGeneratedCommand,
    AmbiguousImportedAutomaticVolume,
};

enum class MgsEnvelopeCommandType : std::uint8_t { Noise, ToneNoiseMode, Timbre };
enum class MgsEnvelopeCommandZone : std::uint8_t {
    Count, BeforeLoopStart, AfterLoopStart, BeforeLoopEnd, AfterLoopEnd,
};

struct MgsEnvelopeCommandConflict {
    std::uint32_t count{};
    MgsEnvelopeCommandType type{};
    MgsEnvelopeCommandZone zone{};
    bool includes_automatic_base{};
};

[[nodiscard]] std::string describeMgsEnvelopeCommandConflict(
    const MgsEnvelopeCommandConflict& conflict);

struct MgsEnvelopeFormatResult {
    std::string definition;
    std::string body;
    std::size_t compiled_bytes{};
    std::vector<std::uint8_t> bytecode;
    std::vector<std::uint32_t> playback_counts;
    std::vector<MgsEnvelopeIssue> issues;
    std::vector<MgsEnvelopeCommandConflict> command_conflicts;
    // Imported automatic-volume markers whose zero span cannot be recovered
    // from the saved authoring events. Authoring data is never rewritten.
    std::vector<std::uint32_t> ambiguous_imported_automatic_counts;
    // First excluded execution count when the complete stream exceeds the
    // byte budget. The generated definition/bytecode is a valid prefix.
    std::optional<std::uint32_t> output_cutoff_count;

    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }

    [[nodiscard]] bool hasIssue(MgsEnvelopeIssue issue) const noexcept;
};

// MGSC 1.11 rejects a command-byte count >= 254 (not source characters).
inline constexpr std::size_t kMgscEnvelopeCompiledByteLimit = 253;
// MGSC source line length (bytes). The serializer wraps at '.' when needed.
inline constexpr std::size_t kMgscEnvelopeSourceLineLimit = 255;

[[nodiscard]] MgsEnvelopeFormatResult formatMgsCompositeEnvelope(
    const CompositeLayer& layer,
    std::uint8_t definition_number,
    std::size_t compiled_byte_limit = kMgscEnvelopeCompiledByteLimit,
    const TimbreNumberResolution* numbers = nullptr,
    const TimbreLibrary* library = nullptr,
    std::string_view composite_name = {});

// Save preflight for the command-count restriction only. Other formatting
// issues do not change the authoring format's existing save/load contract.
[[nodiscard]] std::optional<std::string> mgsCompositeCommandConflict(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library = nullptr);

// Non-realtime State-restore classification only. True requires a strict
// command conflict and no other authoring/format/morph failure, across all
// layers. No relaxed definition or bytecode is exposed to output callers.
[[nodiscard]] bool mgsCompositeHasOnlyCommandConflicts(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library = nullptr);

// Save/output preflight also recognizes unresolved legacy import timing.
[[nodiscard]] std::optional<std::string> mgsCompositeAuthoringIssue(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library = nullptr);

// State-only classification: command conflicts or unresolved legacy import
// timing, with no other invalid data. Never exposes relaxed output bytes.
[[nodiscard]] bool mgsCompositeHasOnlyRepairableAuthoringIssues(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library = nullptr);

// Track-side setup MML for the layer preview (no @e body).
// Example: `9 v15 @16 @e0 \1 @\30 r8`
// An out-of-range envelope number returns an empty string and, if supplied,
// an error message. It is never silently remapped to another definition.
[[nodiscard]] std::string formatMgsCompositeTrackSetup(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr,
    std::string* error = nullptr);

// UI-only track setup preview. Uses display channel labels 1-3 (PSG),
// 4-8 (SCC), and 9,A-H (OPLL); full MGSC export keeps its track syntax.
[[nodiscard]] std::string formatMgsCompositeTrackPreview(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr);

// Product UI/END ceiling: serializer probes stay cheap and usable even when
// trailing holds collapse and sparse envelopes would otherwise grow forever.
inline constexpr std::uint32_t kMgscEnvelopeUiLengthCap = 512;

// Largest length_counts whose serialized @e compiles to compiled_byte_limit
// bytes (same serializer as product output).
[[nodiscard]] std::uint32_t maxEnvelopeLengthFittingBodyLimit(
    const CompositeLayer& layer,
    std::size_t compiled_byte_limit = kMgscEnvelopeCompiledByteLimit,
    const TimbreNumberResolution* numbers = nullptr,
    const TimbreLibrary* library = nullptr);

}  // namespace mgstc::engine
