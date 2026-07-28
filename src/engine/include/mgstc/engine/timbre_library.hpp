#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mgstc::engine {

enum class TimbreCategory : std::uint8_t {
    Opll,
    Scc,
};

struct TimbreLibraryEntry {
    std::uint64_t id{};
    TimbreCategory category{TimbreCategory::Opll};
    std::string name;
    std::string tags;
    std::string memo;
    bool favorite{};
    std::int64_t created_unix_seconds{};
    std::int64_t updated_unix_seconds{};
    std::uint32_t data_version{1};
    std::array<std::uint8_t, 8> opll_registers{};
    std::array<std::uint8_t, 32> scc_waveform{};
};

class TimbreLibrary {
public:
    static constexpr std::uint32_t kSchemaVersion = 1;

    [[nodiscard]] const std::vector<TimbreLibraryEntry>& entries() const
        noexcept;
    [[nodiscard]] const TimbreLibraryEntry* find(std::uint64_t id) const
        noexcept;
    [[nodiscard]] TimbreLibraryEntry* find(std::uint64_t id) noexcept;

    std::uint64_t add(
        TimbreLibraryEntry entry,
        std::int64_t now_unix_seconds);
    bool update(
        std::uint64_t id,
        const TimbreLibraryEntry& replacement,
        std::int64_t now_unix_seconds);
    bool erase(std::uint64_t id);

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] std::optional<std::string> serializeEntry(
        std::uint64_t id) const;
    [[nodiscard]] std::string uniqueName(
        TimbreCategory category,
        std::string_view requested_name) const;
    [[nodiscard]] std::optional<std::vector<std::uint64_t>>
        importSerialized(
            std::string_view text,
            std::int64_t now_unix_seconds,
            std::string* error = nullptr);
    [[nodiscard]] static std::optional<TimbreLibrary> deserialize(
        std::string_view text,
        std::string* error = nullptr);

private:
    std::vector<TimbreLibraryEntry> entries_;
    std::uint64_t next_id_{1};
};

}  // namespace mgstc::engine
