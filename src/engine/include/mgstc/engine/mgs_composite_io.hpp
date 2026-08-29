#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

struct MgsCompositeIoResult {
    std::string source;
    std::vector<std::string> issues;

    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }
};

struct ParsedMgsComposite {
    CompositeTimbre timbre;
    std::vector<std::string> issues;

    [[nodiscard]] bool valid() const noexcept {
        return issues.empty();
    }
};

// Produces a complete, standalone MGSC source.  The optional library is used
// only to resolve envelope timbre-change IDs that are not present in a layer's
// SavedTimbreReference snapshot.
[[nodiscard]] MgsCompositeIoResult formatMgsComposite(
    const CompositeTimbre& timbre,
    const TimbreLibrary* library = nullptr);

// Reads the source emitted above and ordinary MGSC sources containing the same
// @s/@v/@e/@r definitions and numbered track setup lines.
[[nodiscard]] ParsedMgsComposite parseMgsComposite(
    std::string_view source);

[[nodiscard]] std::string formatMgsRateDefinition(
    const CompositeLayer& layer,
    unsigned number);

}  // namespace mgstc::engine
