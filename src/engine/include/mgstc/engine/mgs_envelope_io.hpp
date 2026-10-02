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
};

struct MgsEnvelopeFormatResult {
    std::string definition;
    std::string body;
    std::size_t compiled_bytes{};
    std::vector<std::uint8_t> bytecode;
    std::vector<MgsEnvelopeIssue> issues;
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

// Track-side setup MML for the layer preview (no @e body).
// Example: `9 v15 @16 @e0 \1 @\30 r8`
[[nodiscard]] std::string formatMgsCompositeTrackSetup(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr);

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
