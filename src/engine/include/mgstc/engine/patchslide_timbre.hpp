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

// A conflict never produces an audition or commit model. The UI must obtain
// approval for the proposed timeline and retry against the same source snapshot.
struct PatchSlideEnvelopeLoopConflict {
    std::vector<std::uint32_t> repeated_counts;
    std::vector<std::uint32_t> unreachable_counts;
    EnvelopeTimeline proposed_timeline;
};

struct PatchSlideEnvelopeApplyResult {
    std::optional<CompositeTimbre> timbre;
    std::optional<PatchSlideEnvelopeLoopConflict> loop_conflict;
    std::string error;
};

// Pure, bounded UI/worker operation. Replaces only the selected OPLL layer's
// @ and y/TL/FB authoring; all other authoring and other layers are retained.
// No history, engine, database or caller-owned model is mutated. Both preview
// and commit consume the returned model. A caller committing asynchronously
// must still compare its source snapshot and host-state generation.
[[nodiscard]] PatchSlideEnvelopeApplyResult preparePatchSlideEnvelopeApply(
    const CompositeTimbre& original, std::size_t layer_index,
    const PatchSlideCandidate& candidate,
    const std::optional<EnvelopeTimeline>& approved_timeline = std::nullopt);

}  // namespace mgstc::engine
