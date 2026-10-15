// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/tone_library_store.hpp"

namespace mgstc::engine {

// Portable .mgstc is one of three texts. Readers of the older two headers
// stay unchanged; this header is only the set document.
enum class MgstcPortableKind {
    Unknown,
    TimbreLibrary,
    CompositeLibrary,
    ToneSet,
};

struct MgstcToneSetDocument {
    std::string name;
    std::string memo;
    std::vector<std::variant<
        TimbreLibraryEntry,
        CompositeTimbreLibraryEntry>>
        members;
};

struct MgstcToneSetImportResult {
    std::uint64_t set_id{};
    std::string set_name;
    std::size_t added_timbres{};
    std::size_t added_composites{};
};

[[nodiscard]] MgstcPortableKind detectMgstcFileKind(std::string_view text);

[[nodiscard]] std::optional<std::string> serializeMgstcToneSet(
    const MgstcToneSetDocument& document,
    std::string* error = nullptr);

[[nodiscard]] std::optional<MgstcToneSetDocument> deserializeMgstcToneSet(
    std::string_view text,
    std::string* error = nullptr);

// Append the document as new timbres and one new set. Existing rows and
// other sets are not rewritten. Names that collide gain a suffix.
[[nodiscard]] std::optional<MgstcToneSetImportResult> importMgstcToneSet(
    const MgstcToneSetDocument& document,
    TimbreLibrary& timbres,
    CompositeTimbreLibrary& composites,
    std::vector<ToneSetRecord>& sets,
    std::vector<ToneSetMemberRecord>& members,
    std::int64_t now_unix_seconds,
    std::string* error = nullptr);

// Replace destination only after the temporary file is complete.
// A refused or failed write leaves any existing destination bytes in place.
[[nodiscard]] bool replaceFileBytesAtomically(
    const std::filesystem::path& destination,
    std::string_view bytes,
    std::string* error = nullptr);

[[nodiscard]] bool writeMgstcToneSetFile(
    const std::filesystem::path& destination,
    const MgstcToneSetDocument& document,
    std::string* error = nullptr);

// Editor list filter. NamedSet matches one set. Unassigned matches a
// timbre or composite that has no membership row.
enum class ToneSetListFilter {
    All,
    Unassigned,
    NamedSet,
};

enum class ToneSetMembershipAppend {
    Added,
    AlreadyPresent,
    Rejected,
};

// Exactly one of timbre_id and composite_id is the row being tested.
[[nodiscard]] bool toneVisibleInSetFilter(
    std::uint64_t timbre_id,
    std::uint64_t composite_id,
    ToneSetListFilter filter,
    std::uint64_t set_id,
    const std::vector<ToneSetMemberRecord>& members);

// Append one membership at max(position)+1. A second append of the same
// pair is AlreadyPresent and does not change the vector.
[[nodiscard]] ToneSetMembershipAppend appendToneSetMember(
    std::vector<ToneSetMemberRecord>& members,
    std::uint64_t set_id,
    std::uint64_t timbre_id,
    std::uint64_t composite_id);

}  // namespace mgstc::engine
