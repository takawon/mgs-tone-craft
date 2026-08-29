#pragma once

#include <cstddef>
#include <cstdint>
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
};

struct MgsEnvelopeFormatResult {
    std::string definition;
    std::string body;
    std::size_t compiled_bytes{};
    std::vector<MgsEnvelopeIssue> issues;

    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }

    [[nodiscard]] bool hasIssue(MgsEnvelopeIssue issue) const noexcept;
};

// MGSC 1.11 compiles @e to at most 256 payload bytes (not source characters).
inline constexpr std::size_t kMgscEnvelopeCompiledByteLimit = 256;
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
