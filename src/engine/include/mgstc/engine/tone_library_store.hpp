#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/timbre_library.hpp"

struct sqlite3;

namespace mgstc::engine {

struct ToneSetRecord {
    std::uint64_t id = 0;
    std::string name;
    std::string memo;
    std::int64_t created_unix_seconds = 0;
    std::int64_t updated_unix_seconds = 0;
    std::int64_t revision = 1;
};

struct ToneSetMemberRecord {
    std::uint64_t set_id = 0;
    std::uint64_t timbre_id = 0;
    std::uint64_t composite_id = 0;
    int position = 0;
};

// SQLite-backed user tone library (`tone_library.sqlite`).
// In-memory TimbreLibrary / CompositeTimbreLibrary remain the editing model;
// this class is the only place that issues SQL.
class ToneLibraryDatabase {
public:
    static constexpr int kSchemaVersion = 2;
    // Four-byte magic 'MGTL' (MGSTC Tone Library). Distinct from
    // patchslide.sqlite, which will receive its own application_id.
    static constexpr int kApplicationId = 0x4D47544C;

    ToneLibraryDatabase() = default;
    ToneLibraryDatabase(const ToneLibraryDatabase&) = delete;
    ToneLibraryDatabase& operator=(const ToneLibraryDatabase&) = delete;
    ~ToneLibraryDatabase();

    [[nodiscard]] bool open(
        const std::string& utf8_path,
        std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] int userVersion() const noexcept;

    [[nodiscard]] bool load(
        TimbreLibrary& timbres,
        CompositeTimbreLibrary& composites,
        std::string* error = nullptr);
    // Startup recovery only. Publish all readable rows from both tables;
    // false means some data failed and the caller must prohibit persistence.
    // Strict load() retains its all-or-nothing semantics.
    [[nodiscard]] bool loadAvailable(
        TimbreLibrary& timbres,
        CompositeTimbreLibrary& composites,
        std::string* error = nullptr);
    // Timbres table + timbre_tags only. Does not garbage-collect `tags`
    // (composites may still reference the same names).
    [[nodiscard]] bool replaceTimbres(
        const TimbreLibrary& timbres,
        std::string* error = nullptr);
    // Composite table + composite_tags only. Does not garbage-collect `tags`.
    [[nodiscard]] bool replaceComposites(
        const CompositeTimbreLibrary& composites,
        std::string* error = nullptr);
    // Both sides, then delete tags unreferenced by either junction table.
    [[nodiscard]] bool replaceAll(
        const TimbreLibrary& timbres,
        const CompositeTimbreLibrary& composites,
        std::string* error = nullptr);
    // Replaces set rows and memberships only. Timbre and composite rows stay.
    [[nodiscard]] bool loadToneSets(
        std::vector<ToneSetRecord>& sets,
        std::vector<ToneSetMemberRecord>& members,
        std::string* error = nullptr);
    [[nodiscard]] bool replaceToneSets(
        const std::vector<ToneSetRecord>& sets,
        const std::vector<ToneSetMemberRecord>& members,
        std::string* error = nullptr);
    // Timbres, composites, and set membership in one transaction.
    [[nodiscard]] bool replaceAllAndToneSets(
        const TimbreLibrary& timbres,
        const CompositeTimbreLibrary& composites,
        const std::vector<ToneSetRecord>& sets,
        const std::vector<ToneSetMemberRecord>& members,
        std::string* error = nullptr);

private:
    [[nodiscard]] bool migrate(std::string* error);
    [[nodiscard]] bool backupExistingV1(std::string* error);
    [[nodiscard]] bool toneSetsReadable(std::string* error);
    [[nodiscard]] bool enableForeignKeys(std::string* error);
    [[nodiscard]] bool readUserVersion(int& version, std::string* error) const;
    [[nodiscard]] bool writeUserVersion(int version, std::string* error);
    [[nodiscard]] bool readApplicationId(int& application_id, std::string* error) const;
    [[nodiscard]] bool writeApplicationId(int application_id, std::string* error);
    [[nodiscard]] bool migrateToV1(std::string* error);
    [[nodiscard]] bool exec(const char* sql, std::string* error);
    [[nodiscard]] bool beginImmediate(std::string* error);
    [[nodiscard]] bool commit(std::string* error);
    void rollback() noexcept;

    [[nodiscard]] bool writeTimbres(
        const TimbreLibrary& timbres,
        std::string* error);
    [[nodiscard]] bool writeComposites(
        const CompositeTimbreLibrary& composites,
        std::string* error);
    [[nodiscard]] bool pruneUnusedTags(std::string* error);
    [[nodiscard]] bool validateToneSets(
        const std::vector<ToneSetRecord>& sets,
        const std::vector<ToneSetMemberRecord>& members,
        std::string* error) const;
    [[nodiscard]] bool writeToneSets(
        const std::vector<ToneSetRecord>& sets,
        const std::vector<ToneSetMemberRecord>& members,
        std::string* error);
    [[nodiscard]] bool loadTimbres(
        TimbreLibrary& timbres,
        std::string* error,
        bool allow_partial = false);
    [[nodiscard]] bool loadComposites(
        CompositeTimbreLibrary& composites,
        std::string* error,
        bool allow_partial = false);

    sqlite3* db_{};
    int user_version_{};
    std::string utf8_path_;
};

}  // namespace mgstc::engine
