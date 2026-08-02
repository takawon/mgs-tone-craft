#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"

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
    std::size_t body_length_limit = 255);

}  // namespace mgstc::engine
