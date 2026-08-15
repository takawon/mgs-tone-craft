#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
};

struct MgsEnvelopeFormatResult {
    std::string definition;
    std::string body;
    std::vector<MgsEnvelopeIssue> issues;

    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }

    [[nodiscard]] bool hasIssue(MgsEnvelopeIssue issue) const noexcept;
};

[[nodiscard]] MgsEnvelopeFormatResult formatMgsCompositeEnvelope(
    const CompositeLayer& layer,
    std::uint8_t definition_number,
    std::size_t body_length_limit = 255,
    const TimbreNumberResolution* numbers = nullptr,
    const TimbreLibrary* library = nullptr);

// Track-side setup MML for the layer preview (no @e body).
// Example: `9 v15 @16 \1 @\30 r8`
[[nodiscard]] std::string formatMgsCompositeTrackSetup(
    const CompositeLayer& layer,
    const TimbreNumberResolution* numbers = nullptr);

// MGSC 1.11: @e Data is ~255 characters (content-dependent). Uses the same
// serializer as product output to find the largest length_counts that fits.
// Sparse holds can theoretically pack many thousands of counts into 255 chars;
// the product UI/END ceiling is capped so probes stay cheap and usable.
inline constexpr std::uint32_t kMgscEnvelopeUiLengthCap = 512;

[[nodiscard]] std::uint32_t maxEnvelopeLengthFittingBodyLimit(
    const CompositeLayer& layer,
    std::size_t body_length_limit = 255,
    const TimbreNumberResolution* numbers = nullptr,
    const TimbreLibrary* library = nullptr);

}  // namespace mgstc::engine
