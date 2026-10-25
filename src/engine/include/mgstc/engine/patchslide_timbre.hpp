// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <optional>
#include <string>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/patchslide_database.hpp"

namespace mgstc::engine {

[[nodiscard]] std::string patchSlideDisplayName(const PatchSlideCandidate& candidate);

// Expands the complete sound into ordinary editable data. No database, runtime
// program, or external library reference is retained by the returned timbre.
// Call from a UI or worker thread, before preparing an audition program.
[[nodiscard]] std::optional<CompositeTimbre> makePatchSlideTimbre(
    const PatchSlideCandidate& candidate, std::string* error = nullptr);

}  // namespace mgstc::engine
