// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"

namespace mgstc::engine {

enum class CompositeStepEditLane : std::uint8_t {
    All, Volume, Pitch, Timbre, RegisterWrite, Tl, Fb,
};

struct CompositeStepEditResult {
    bool accepted{};
    CompositeLayer layer;
    int applied_delta{};
    std::string error;
};

// Authoring/UI-thread operation. Positive delta inserts immediately before
// start_count; negative delta removes the interval immediately before it.
// Rejection returns the unchanged source. No saved format or runtime changes.
[[nodiscard]] CompositeStepEditResult editCompositeSteps(
    const CompositeLayer& source, CompositeStepEditLane lane,
    std::uint32_t start_count, int delta,
    std::uint32_t maximum_length = kMgscEnvelopeUiLengthCap);

} // namespace mgstc::engine
