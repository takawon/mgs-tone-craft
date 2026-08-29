#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"

namespace mgstc::engine {

struct CompositeTimbreLibraryEntry {
    std::uint64_t id{};
    std::uint32_t revision{1};
    std::int64_t created_unix_seconds{};
    std::int64_t updated_unix_seconds{};
    std::int64_t last_used_unix_seconds{};
    std::uint32_t use_count{};
    CompositeTimbre timbre;

    friend bool operator==(
        const CompositeTimbreLibraryEntry&,
        const CompositeTimbreLibraryEntry&) = default;
};

class CompositeTimbreLibrary {
public:
    static constexpr std::uint32_t kSchemaVersion = 3;

    [[nodiscard]] const std::vector<CompositeTimbreLibraryEntry>&
        entries() const noexcept;
    [[nodiscard]] const CompositeTimbreLibraryEntry* find(
        std::uint64_t id) const noexcept;
    [[nodiscard]] CompositeTimbreLibraryEntry* find(
        std::uint64_t id) noexcept;

    std::uint64_t add(
        CompositeTimbre timbre,
        std::int64_t now_unix_seconds);
    bool update(
        std::uint64_t id,
        const CompositeTimbre& replacement,
        std::int64_t now_unix_seconds);
    bool erase(std::uint64_t id);
    bool touch(std::uint64_t id, std::int64_t now_unix_seconds);
    std::size_t rewriteTag(
        std::string_view source,
        std::string_view replacement,
        std::int64_t now_unix_seconds);

    [[nodiscard]] std::string uniqueName(
        std::string_view requested_name) const;
    [[nodiscard]] std::vector<TimbreUse> findTimbreUses(
        std::uint64_t timbre_library_id) const;
    std::size_t updateTimbreReferences(
        const TimbreLibraryEntry& timbre,
        std::int64_t now_unix_seconds);

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static std::optional<CompositeTimbreLibrary>
        deserialize(
            std::string_view text,
            std::string* error = nullptr);

    // Single-program .mgstc (library header + one entry). Used by the
    // composite editor Open/Save/Copy/Paste chrome.
    [[nodiscard]] static std::string serializeTimbreFile(
        const CompositeTimbre& timbre,
        std::int64_t now_unix_seconds);
    [[nodiscard]] static std::optional<CompositeTimbre>
        deserializeTimbreFile(
            std::string_view text,
            std::string* error = nullptr);

private:
    std::vector<CompositeTimbreLibraryEntry> entries_;
    std::uint64_t next_id_{1};
};

}  // namespace mgstc::engine
