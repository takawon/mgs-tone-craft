#include "mgstc/engine/tone_library_store.hpp"

#include "mgstc/engine/composite_timbre_library.hpp"

#include <sqlite3.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mgstc::engine {
namespace {

void setError(std::string* error, std::string_view message) {
    if (error) {
        *error = std::string(message);
    }
}

void setSqliteError(std::string* error, sqlite3* db, std::string_view prefix) {
    std::string message(prefix);
    if (db != nullptr) {
        const auto* sqlite_message = sqlite3_errmsg(db);
        if (sqlite_message != nullptr && sqlite_message[0] != '\0') {
            message += ": ";
            message += sqlite_message;
        }
    }
    setError(error, message);
}

[[nodiscard]] bool toSqliteId(std::uint64_t id, sqlite3_int64& out) {
    if (id == 0
        || id > static_cast<std::uint64_t>(
                    std::numeric_limits<sqlite3_int64>::max())) {
        return false;
    }
    out = static_cast<sqlite3_int64>(id);
    return true;
}

[[nodiscard]] bool fromSqliteId(sqlite3_int64 id, std::uint64_t& out) {
    if (id <= 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(id);
    return true;
}

class SqliteStatement {
public:
    SqliteStatement() = default;
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    ~SqliteStatement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    [[nodiscard]] bool prepare(
        sqlite3* db,
        const char* sql,
        std::string* error) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            setSqliteError(error, db, "failed to prepare statement");
            return false;
        }
        return true;
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept {
        return stmt_;
    }

    [[nodiscard]] bool bindInt64(
        int index,
        sqlite3_int64 value,
        sqlite3* db,
        std::string* error) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            setSqliteError(error, db, "failed to bind integer");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool bindText(
        int index,
        std::string_view value,
        sqlite3* db,
        std::string* error) {
        const char* data = value.empty() ? "" : value.data();
        if (sqlite3_bind_text(
                stmt_,
                index,
                data,
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT)
            != SQLITE_OK) {
            setSqliteError(error, db, "failed to bind text");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool bindBlob(
        int index,
        std::span<const std::uint8_t> value,
        sqlite3* db,
        std::string* error) {
        if (sqlite3_bind_blob(
                stmt_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT)
            != SQLITE_OK) {
            setSqliteError(error, db, "failed to bind blob");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool bindNull(int index, sqlite3* db, std::string* error) {
        if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
            setSqliteError(error, db, "failed to bind null");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool stepRow(bool& has_row, sqlite3* db, std::string* error) {
        const auto rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            has_row = true;
            return true;
        }
        if (rc == SQLITE_DONE) {
            has_row = false;
            return true;
        }
        setSqliteError(error, db, "failed to step statement");
        return false;
    }

    [[nodiscard]] bool stepDone(sqlite3* db, std::string* error) {
        bool has_row = false;
        if (!stepRow(has_row, db, error)) {
            return false;
        }
        if (has_row) {
            setError(error, "unexpected result row");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool reset(sqlite3* db, std::string* error) {
        if (sqlite3_reset(stmt_) != SQLITE_OK
            || sqlite3_clear_bindings(stmt_) != SQLITE_OK) {
            setSqliteError(error, db, "failed to reset statement");
            return false;
        }
        return true;
    }

private:
    sqlite3_stmt* stmt_{};
};

constexpr const char* kCreateV1Sql = R"SQL(
CREATE TABLE timbres (
    id INTEGER PRIMARY KEY,
    category INTEGER NOT NULL,
    name TEXT NOT NULL,
    memo TEXT NOT NULL DEFAULT '',
    favorite INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_used_at INTEGER NOT NULL DEFAULT 0,
    use_count INTEGER NOT NULL DEFAULT 0,
    revision INTEGER NOT NULL DEFAULT 1,
    payload BLOB NOT NULL
);

CREATE TABLE composite_timbres (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    memo TEXT NOT NULL DEFAULT '',
    favorite INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    last_used_at INTEGER NOT NULL DEFAULT 0,
    use_count INTEGER NOT NULL DEFAULT 0,
    revision INTEGER NOT NULL DEFAULT 1,
    payload_version INTEGER NOT NULL,
    payload BLOB NOT NULL
);

CREATE TABLE tags (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE
);

CREATE TABLE timbre_tags (
    timbre_id INTEGER NOT NULL,
    tag_id INTEGER NOT NULL,
    PRIMARY KEY (timbre_id, tag_id),
    FOREIGN KEY (timbre_id) REFERENCES timbres(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id) REFERENCES tags(id) ON DELETE RESTRICT
);

CREATE TABLE composite_tags (
    composite_id INTEGER NOT NULL,
    tag_id INTEGER NOT NULL,
    PRIMARY KEY (composite_id, tag_id),
    FOREIGN KEY (composite_id) REFERENCES composite_timbres(id) ON DELETE CASCADE,
    FOREIGN KEY (tag_id) REFERENCES tags(id) ON DELETE RESTRICT
);
)SQL";

constexpr const char* kCreateV2Sql = R"SQL(
CREATE TABLE tone_sets (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    memo TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    revision INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE tone_set_members (
    set_id INTEGER NOT NULL REFERENCES tone_sets(id) ON DELETE CASCADE,
    timbre_id INTEGER REFERENCES timbres(id) ON DELETE CASCADE,
    composite_id INTEGER REFERENCES composite_timbres(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    CHECK ((timbre_id IS NULL) <> (composite_id IS NULL)),
    UNIQUE (set_id, timbre_id),
    UNIQUE (set_id, composite_id)
);

CREATE INDEX tone_set_members_order ON tone_set_members(set_id, position);
CREATE INDEX tone_set_members_timbre ON tone_set_members(timbre_id);
CREATE INDEX tone_set_members_composite ON tone_set_members(composite_id);
)SQL";

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.data()),
        utf8.size()));
}

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

[[nodiscard]] std::string sqlQuote(std::string_view text) {
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('\'');
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] std::string utcBackupStamp() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_s(&utc, &now);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y%m%d%H%M%S");
    return text.str();
}

class MigrationFileLock {
public:
    MigrationFileLock() = default;
    MigrationFileLock(const MigrationFileLock&) = delete;
    MigrationFileLock& operator=(const MigrationFileLock&) = delete;

    ~MigrationFileLock() {
        release();
    }

    [[nodiscard]] bool acquire(const std::string& database_utf8, std::string* error) {
        release();
        const auto wide = utf8ToWide(database_utf8 + ".migrate.lock");
        if (wide.empty()) {
            setError(error, "failed to encode tone library migration lock path");
            return false;
        }
        for (int attempt = 0; attempt < 600; ++attempt) {
            handle_ = CreateFileW(
                wide.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }
            Sleep(50);
        }
        setError(error, "timed out waiting for tone library migration lock");
        return false;
    }

private:
    void release() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] static std::wstring utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) {
            return {};
        }
        const auto chars = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            utf8.data(),
            static_cast<int>(utf8.size()),
            nullptr,
            0);
        if (chars <= 0) {
            return {};
        }
        std::wstring wide(static_cast<std::size_t>(chars), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                utf8.data(),
                static_cast<int>(utf8.size()),
                wide.data(),
                chars)
            != chars) {
            return {};
        }
        return wide;
    }

    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] bool payloadForCategory(
    const TimbreLibraryEntry& entry,
    std::span<const std::uint8_t>& payload,
    std::string* error) {
    if (entry.category == TimbreCategory::Opll) {
        payload = std::span<const std::uint8_t>(
            entry.opll_registers.data(), entry.opll_registers.size());
        return true;
    }
    if (entry.category == TimbreCategory::Scc) {
        payload = std::span<const std::uint8_t>(
            entry.scc_waveform.data(), entry.scc_waveform.size());
        return true;
    }
    setError(error, "unsupported timbre category");
    return false;
}

[[nodiscard]] bool applyTimbrePayload(
    TimbreLibraryEntry& entry,
    const void* blob,
    int size,
    std::string* error) {
    if (entry.category == TimbreCategory::Opll) {
        if (size != 8 || blob == nullptr) {
            setError(error, "OPLL payload must be 8 bytes");
            return false;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(blob);
        std::copy(bytes, bytes + 8, entry.opll_registers.begin());
        return true;
    }
    if (entry.category == TimbreCategory::Scc) {
        if (size != 32 || blob == nullptr) {
            setError(error, "SCC payload must be 32 bytes");
            return false;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(blob);
        std::copy(bytes, bytes + 32, entry.scc_waveform.begin());
        return true;
    }
    setError(error, "unsupported timbre category");
    return false;
}

}  // namespace

ToneLibraryDatabase::~ToneLibraryDatabase() {
    close();
}

bool ToneLibraryDatabase::open(
    const std::string& utf8_path,
    std::string* error) {
    close();
    utf8_path_ = utf8_path;
    if (sqlite3_open_v2(
            utf8_path.c_str(),
            &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr)
        != SQLITE_OK) {
        setSqliteError(error, db_, "failed to open tone library");
        close();
        return false;
    }
    if (!enableForeignKeys(error) || !migrate(error)) {
        close();
        return false;
    }
    return true;
}

void ToneLibraryDatabase::close() noexcept {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    user_version_ = 0;
    utf8_path_.clear();
}

bool ToneLibraryDatabase::isOpen() const noexcept {
    return db_ != nullptr;
}

int ToneLibraryDatabase::userVersion() const noexcept {
    return user_version_;
}

bool ToneLibraryDatabase::enableForeignKeys(std::string* error) {
    return exec("PRAGMA foreign_keys = ON;", error);
}

bool ToneLibraryDatabase::exec(const char* sql, std::string* error) {
    char* sqlite_error = nullptr;
    const auto rc = sqlite3_exec(db_, sql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        if (sqlite_error != nullptr) {
            setError(error, sqlite_error);
            sqlite3_free(sqlite_error);
        } else {
            setSqliteError(error, db_, "SQL failed");
        }
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::beginImmediate(std::string* error) {
    return exec("BEGIN IMMEDIATE;", error);
}

bool ToneLibraryDatabase::commit(std::string* error) {
    return exec("COMMIT;", error);
}

void ToneLibraryDatabase::rollback() noexcept {
    if (db_ != nullptr) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
}

bool ToneLibraryDatabase::readUserVersion(
    int& version,
    std::string* error) const {
    SqliteStatement stmt;
    if (!stmt.prepare(db_, "PRAGMA user_version;", error)) {
        return false;
    }
    bool has_row = false;
    if (!stmt.stepRow(has_row, db_, error) || !has_row) {
        setError(error, "failed to read schema version");
        return false;
    }
    version = static_cast<int>(sqlite3_column_int64(stmt.get(), 0));
    return true;
}

bool ToneLibraryDatabase::writeUserVersion(int version, std::string* error) {
    const auto sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    return exec(sql.c_str(), error);
}

bool ToneLibraryDatabase::readApplicationId(
    int& application_id,
    std::string* error) const {
    SqliteStatement stmt;
    if (!stmt.prepare(db_, "PRAGMA application_id;", error)) {
        return false;
    }
    bool has_row = false;
    if (!stmt.stepRow(has_row, db_, error) || !has_row) {
        setError(error, "failed to read application id");
        return false;
    }
    application_id = static_cast<int>(sqlite3_column_int64(stmt.get(), 0));
    return true;
}

bool ToneLibraryDatabase::writeApplicationId(
    int application_id,
    std::string* error) {
    const auto sql =
        "PRAGMA application_id = " + std::to_string(application_id) + ";";
    return exec(sql.c_str(), error);
}

bool ToneLibraryDatabase::migrateToV1(std::string* error) {
    return exec(kCreateV1Sql, error);
}

bool ToneLibraryDatabase::backupExistingV1(std::string* error) {
    const auto database = pathFromUtf8(utf8_path_);
    const auto preferred_name = database.stem().u8string();
    const auto preferred_suffix = std::string(".v1-backup.sqlite");
    auto preferred_text = preferred_name;
    preferred_text.append(
        reinterpret_cast<const char8_t*>(preferred_suffix.data()),
        preferred_suffix.size());
    const auto preferred = database.parent_path() / preferred_text;
    std::filesystem::path chosen;
    if (!std::filesystem::exists(preferred)) {
        chosen = preferred;
    } else {
        const auto stamp = utcBackupStamp();
        const auto stem = database.stem().u8string();
        for (int index = 0; index < 50 && chosen.empty(); ++index) {
            auto name = stem;
            const auto suffix = std::string(".v1-backup-") + stamp
                + (index > 0 ? "-" + std::to_string(index) : "")
                + ".sqlite";
            name.append(
                reinterpret_cast<const char8_t*>(suffix.data()),
                suffix.size());
            const auto candidate = database.parent_path() / name;
            if (!std::filesystem::exists(candidate)) {
                chosen = candidate;
            }
        }
    }
    if (chosen.empty()) {
        setError(error, "failed to choose a tone library backup path");
        return false;
    }
    const auto sql = "VACUUM INTO " + sqlQuote(pathToUtf8(chosen)) + ";";
    if (!exec(sql.c_str(), error)) {
        std::error_code remove_error;
        std::filesystem::remove(chosen, remove_error);
        setError(error, "failed to back up the tone library before migration");
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::migrate(std::string* error) {
    int application_id = 0;
    int version = 0;
    if (!readApplicationId(application_id, error)
        || !readUserVersion(version, error)) {
        return false;
    }
    const bool uninitialized = application_id == 0 && version == 0;
    if (!uninitialized && application_id != kApplicationId) {
        setError(error, "file is not an MGSTC tone library");
        return false;
    }
    if (application_id == kApplicationId) {
        if (version > kSchemaVersion) {
            setError(
                error,
                "tone library schema is newer than this MGSTC build");
            return false;
        }
        if (version == kSchemaVersion) {
            user_version_ = version;
            return true;
        }
    }

    MigrationFileLock migration_lock;
    const bool needs_upgrade = uninitialized || version < kSchemaVersion;
    if (needs_upgrade) {
        if (!migration_lock.acquire(utf8_path_, error)
            || !readApplicationId(application_id, error)
            || !readUserVersion(version, error)) {
            return false;
        }
        const bool still_uninitialized = application_id == 0 && version == 0;
        if (!still_uninitialized && application_id != kApplicationId) {
            setError(error, "file is not an MGSTC tone library");
            return false;
        }
        if (version > kSchemaVersion) {
            setError(
                error,
                "tone library schema is newer than this MGSTC build");
            return false;
        }
        if (version == kSchemaVersion) {
            user_version_ = version;
            return true;
        }
        if (version == 1 && !backupExistingV1(error)) {
            return false;
        }
    }

    if (!beginImmediate(error)) {
        return false;
    }
    if (!readApplicationId(application_id, error)
        || !readUserVersion(version, error)) {
        rollback();
        return false;
    }
    const bool still_uninitialized = application_id == 0 && version == 0;
    if (!still_uninitialized && application_id != kApplicationId) {
        rollback();
        setError(error, "file is not an MGSTC tone library");
        return false;
    }
    if (version > kSchemaVersion) {
        rollback();
        setError(
            error,
            "tone library schema is newer than this MGSTC build");
        return false;
    }
    if (version == kSchemaVersion) {
        if (!commit(error)) {
            rollback();
            return false;
        }
        user_version_ = version;
        return true;
    }
    if (version < 1 && !migrateToV1(error)) {
        rollback();
        return false;
    }
    if (version < 2 && !exec(kCreateV2Sql, error)) {
        rollback();
        return false;
    }
    if (!writeApplicationId(kApplicationId, error)
        || !writeUserVersion(kSchemaVersion, error)
        || !commit(error)) {
        rollback();
        return false;
    }
    user_version_ = kSchemaVersion;
    return true;
}

bool ToneLibraryDatabase::pruneUnusedTags(std::string* error) {
    // Shared `tags` rows must survive if either junction still references them.
    return exec(
        "DELETE FROM tags "
        "WHERE NOT EXISTS ("
        "SELECT 1 FROM timbre_tags WHERE timbre_tags.tag_id = tags.id"
        ") AND NOT EXISTS ("
        "SELECT 1 FROM composite_tags WHERE composite_tags.tag_id = tags.id"
        ");",
        error);
}

bool ToneLibraryDatabase::writeTimbres(
    const TimbreLibrary& timbres,
    std::string* error) {
    if (!exec(
            "CREATE TEMP TABLE IF NOT EXISTS mgstc_keep_timbre_ids("
            "id INTEGER PRIMARY KEY);",
            error)
        || !exec("DELETE FROM mgstc_keep_timbre_ids;", error)
        || !exec("DELETE FROM timbre_tags;", error)) {
        return false;
    }

    SqliteStatement insert_timbre;
    SqliteStatement keep_id;
    SqliteStatement insert_tag;
    SqliteStatement select_tag;
    SqliteStatement insert_link;
    if (!insert_timbre.prepare(
            db_,
            "INSERT INTO timbres ("
            "id, category, name, memo, favorite, created_at, updated_at, "
            "last_used_at, use_count, revision, payload"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "category = excluded.category, name = excluded.name, "
            "memo = excluded.memo, favorite = excluded.favorite, "
            "created_at = excluded.created_at, updated_at = excluded.updated_at, "
            "last_used_at = excluded.last_used_at, use_count = excluded.use_count, "
            "revision = excluded.revision, payload = excluded.payload;",
            error)
        || !keep_id.prepare(
            db_,
            "INSERT INTO mgstc_keep_timbre_ids(id) VALUES (?);",
            error)
        || !insert_tag.prepare(
            db_,
            "INSERT INTO tags(name) VALUES (?) ON CONFLICT(name) DO NOTHING;",
            error)
        || !select_tag.prepare(
            db_,
            "SELECT id FROM tags WHERE name = ?;",
            error)
        || !insert_link.prepare(
            db_,
            "INSERT INTO timbre_tags(timbre_id, tag_id) VALUES (?, ?);",
            error)) {
        return false;
    }

    for (const auto& entry : timbres.entries()) {
        sqlite3_int64 id = 0;
        std::span<const std::uint8_t> payload;
        if (!toSqliteId(entry.id, id)
            || !payloadForCategory(entry, payload, error)) {
            if (error && error->empty()) {
                setError(error, "invalid timbre id");
            }
            return false;
        }
        if (!insert_timbre.bindInt64(1, id, db_, error)
            || !insert_timbre.bindInt64(
                2,
                static_cast<sqlite3_int64>(
                    static_cast<std::uint8_t>(entry.category)),
                db_,
                error)
            || !insert_timbre.bindText(3, entry.name, db_, error)
            || !insert_timbre.bindText(4, entry.memo, db_, error)
            || !insert_timbre.bindInt64(5, entry.favorite ? 1 : 0, db_, error)
            || !insert_timbre.bindInt64(6, entry.created_unix_seconds, db_, error)
            || !insert_timbre.bindInt64(7, entry.updated_unix_seconds, db_, error)
            || !insert_timbre.bindInt64(
                8, entry.last_used_unix_seconds, db_, error)
            || !insert_timbre.bindInt64(9, entry.use_count, db_, error)
            || !insert_timbre.bindInt64(10, entry.revision, db_, error)
            || !insert_timbre.bindBlob(11, payload, db_, error)
            || !insert_timbre.stepDone(db_, error)
            || !insert_timbre.reset(db_, error)
            || !keep_id.bindInt64(1, id, db_, error)
            || !keep_id.stepDone(db_, error)
            || !keep_id.reset(db_, error)) {
            return false;
        }
        for (const auto& tag : entry.tags) {
            if (!insert_tag.bindText(1, tag, db_, error)
                || !insert_tag.stepDone(db_, error)
                || !insert_tag.reset(db_, error)
                || !select_tag.bindText(1, tag, db_, error)) {
                return false;
            }
            bool has_row = false;
            if (!select_tag.stepRow(has_row, db_, error) || !has_row) {
                setError(error, "failed to resolve tag id");
                return false;
            }
            const auto tag_id = sqlite3_column_int64(select_tag.get(), 0);
            if (!select_tag.reset(db_, error)
                || !insert_link.bindInt64(1, id, db_, error)
                || !insert_link.bindInt64(2, tag_id, db_, error)
                || !insert_link.stepDone(db_, error)
                || !insert_link.reset(db_, error)) {
                return false;
            }
        }
    }
    return exec(
        "DELETE FROM timbres WHERE id NOT IN ("
        "SELECT id FROM mgstc_keep_timbre_ids);",
        error);
}

bool ToneLibraryDatabase::writeComposites(
    const CompositeTimbreLibrary& composites,
    std::string* error) {
    if (!exec(
            "CREATE TEMP TABLE IF NOT EXISTS mgstc_keep_composite_ids("
            "id INTEGER PRIMARY KEY);",
            error)
        || !exec("DELETE FROM mgstc_keep_composite_ids;", error)
        || !exec("DELETE FROM composite_tags;", error)) {
        return false;
    }

    SqliteStatement insert_row;
    SqliteStatement keep_id;
    SqliteStatement insert_tag;
    SqliteStatement select_tag;
    SqliteStatement insert_link;
    if (!insert_row.prepare(
            db_,
            "INSERT INTO composite_timbres ("
            "id, name, memo, favorite, created_at, updated_at, last_used_at, "
            "use_count, revision, payload_version, payload"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "name = excluded.name, memo = excluded.memo, "
            "favorite = excluded.favorite, created_at = excluded.created_at, "
            "updated_at = excluded.updated_at, "
            "last_used_at = excluded.last_used_at, "
            "use_count = excluded.use_count, revision = excluded.revision, "
            "payload_version = excluded.payload_version, "
            "payload = excluded.payload;",
            error)
        || !keep_id.prepare(
            db_,
            "INSERT INTO mgstc_keep_composite_ids(id) VALUES (?);",
            error)
        || !insert_tag.prepare(
            db_,
            "INSERT INTO tags(name) VALUES (?) ON CONFLICT(name) DO NOTHING;",
            error)
        || !select_tag.prepare(
            db_,
            "SELECT id FROM tags WHERE name = ?;",
            error)
        || !insert_link.prepare(
            db_,
            "INSERT INTO composite_tags(composite_id, tag_id) VALUES (?, ?);",
            error)) {
        return false;
    }

    for (const auto& entry : composites.entries()) {
        sqlite3_int64 id = 0;
        if (!toSqliteId(entry.id, id)) {
            setError(error, "invalid composite timbre id");
            return false;
        }
        const auto payload = serializeCompositeSoundPayload(entry.timbre);
        const auto payload_bytes = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(payload.data()),
            payload.size());
        if (!insert_row.bindInt64(1, id, db_, error)
            || !insert_row.bindText(2, entry.timbre.name, db_, error)
            || !insert_row.bindText(3, entry.timbre.memo, db_, error)
            || !insert_row.bindInt64(
                4, entry.timbre.favorite ? 1 : 0, db_, error)
            || !insert_row.bindInt64(5, entry.created_unix_seconds, db_, error)
            || !insert_row.bindInt64(6, entry.updated_unix_seconds, db_, error)
            || !insert_row.bindInt64(
                7, entry.last_used_unix_seconds, db_, error)
            || !insert_row.bindInt64(8, entry.use_count, db_, error)
            || !insert_row.bindInt64(9, entry.revision, db_, error)
            || !insert_row.bindInt64(
                10,
                static_cast<sqlite3_int64>(kCompositeSoundPayloadVersion),
                db_,
                error)
            || !insert_row.bindBlob(11, payload_bytes, db_, error)
            || !insert_row.stepDone(db_, error)
            || !insert_row.reset(db_, error)
            || !keep_id.bindInt64(1, id, db_, error)
            || !keep_id.stepDone(db_, error)
            || !keep_id.reset(db_, error)) {
            return false;
        }
        for (const auto& tag : entry.timbre.tags) {
            if (!insert_tag.bindText(1, tag, db_, error)
                || !insert_tag.stepDone(db_, error)
                || !insert_tag.reset(db_, error)
                || !select_tag.bindText(1, tag, db_, error)) {
                return false;
            }
            bool has_row = false;
            if (!select_tag.stepRow(has_row, db_, error) || !has_row) {
                setError(error, "failed to resolve tag id");
                return false;
            }
            const auto tag_id = sqlite3_column_int64(select_tag.get(), 0);
            if (!select_tag.reset(db_, error)
                || !insert_link.bindInt64(1, id, db_, error)
                || !insert_link.bindInt64(2, tag_id, db_, error)
                || !insert_link.stepDone(db_, error)
                || !insert_link.reset(db_, error)) {
                return false;
            }
        }
    }
    return exec(
        "DELETE FROM composite_timbres WHERE id NOT IN ("
        "SELECT id FROM mgstc_keep_composite_ids);",
        error);
}

bool ToneLibraryDatabase::loadTimbres(
    TimbreLibrary& timbres,
    std::string* error,
    bool allow_partial) {
    SqliteStatement select_rows;
    SqliteStatement select_tags;
    if (!select_rows.prepare(
            db_,
            "SELECT id, category, name, memo, favorite, created_at, "
            "updated_at, last_used_at, use_count, revision, payload "
            "FROM timbres ORDER BY id;",
            error)) {
        return false;
    }
    bool read_tags = select_tags.prepare(
            db_,
            "SELECT tags.name FROM timbre_tags "
            "JOIN tags ON tags.id = timbre_tags.tag_id "
            "WHERE timbre_tags.timbre_id = ? "
            "ORDER BY tags.name COLLATE NOCASE, tags.id;",
            error);
    if (!read_tags && !allow_partial) {
        return false;
    }
    bool complete = read_tags;

    std::vector<TimbreLibraryEntry> entries;
    for (;;) {
        bool has_row = false;
        if (!select_rows.stepRow(has_row, db_, error)) {
            if (!allow_partial) { return false; }
            complete = false;
            break;
        }
        if (!has_row) {
            break;
        }
        TimbreLibraryEntry entry;
        if (!fromSqliteId(sqlite3_column_int64(select_rows.get(), 0), entry.id)) {
            setError(error, "invalid timbre id");
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        const auto category = sqlite3_column_int64(select_rows.get(), 1);
        if (category != 0 && category != 1) {
            setError(error, "invalid timbre category");
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        entry.category = static_cast<TimbreCategory>(category);
        const auto* name = sqlite3_column_text(select_rows.get(), 2);
        const auto* memo = sqlite3_column_text(select_rows.get(), 3);
        entry.name = name != nullptr
            ? reinterpret_cast<const char*>(name)
            : "";
        entry.memo = memo != nullptr
            ? reinterpret_cast<const char*>(memo)
            : "";
        entry.favorite = sqlite3_column_int64(select_rows.get(), 4) != 0;
        entry.created_unix_seconds = sqlite3_column_int64(select_rows.get(), 5);
        entry.updated_unix_seconds = sqlite3_column_int64(select_rows.get(), 6);
        entry.last_used_unix_seconds =
            sqlite3_column_int64(select_rows.get(), 7);
        entry.use_count = static_cast<std::uint32_t>(
            sqlite3_column_int64(select_rows.get(), 8));
        entry.revision = static_cast<std::uint32_t>(
            sqlite3_column_int64(select_rows.get(), 9));
        entry.data_version = 1;
        if (!applyTimbrePayload(
                entry,
                sqlite3_column_blob(select_rows.get(), 10),
                sqlite3_column_bytes(select_rows.get(), 10),
                error)) {
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        if (read_tags) {
            sqlite3_int64 sqlite_id = 0;
            if (!toSqliteId(entry.id, sqlite_id)
                || !select_tags.bindInt64(1, sqlite_id, db_, error)) {
                if (!allow_partial) { return false; }
                complete = false;
                read_tags = false;
            } else {
                for (;;) {
                    bool has_tag = false;
                    if (!select_tags.stepRow(has_tag, db_, error)) {
                        if (!allow_partial) { return false; }
                        complete = false;
                        break;
                    }
                    if (!has_tag) { break; }
                    const auto* tag = sqlite3_column_text(select_tags.get(), 0);
                    if (tag != nullptr) {
                        entry.tags.emplace_back(
                            reinterpret_cast<const char*>(tag));
                    }
                }
                if (!select_tags.reset(db_, error)) {
                    if (!allow_partial) { return false; }
                    complete = false;
                    read_tags = false;
                }
            }
        }
        entries.push_back(std::move(entry));
    }
    timbres.installEntries(std::move(entries));
    return complete;
}

bool ToneLibraryDatabase::loadComposites(
    CompositeTimbreLibrary& composites,
    std::string* error,
    bool allow_partial) {
    SqliteStatement select_rows;
    SqliteStatement select_tags;
    if (!select_rows.prepare(
            db_,
            "SELECT id, name, memo, favorite, created_at, updated_at, "
            "last_used_at, use_count, revision, payload_version, payload "
            "FROM composite_timbres ORDER BY id;",
            error)) {
        return false;
    }
    bool read_tags = select_tags.prepare(
            db_,
            "SELECT tags.name FROM composite_tags "
            "JOIN tags ON tags.id = composite_tags.tag_id "
            "WHERE composite_tags.composite_id = ? "
            "ORDER BY tags.name COLLATE NOCASE, tags.id;",
            error);
    if (!read_tags && !allow_partial) {
        return false;
    }
    bool complete = read_tags;

    std::vector<CompositeTimbreLibraryEntry> entries;
    for (;;) {
        bool has_row = false;
        if (!select_rows.stepRow(has_row, db_, error)) {
            if (!allow_partial) { return false; }
            complete = false;
            break;
        }
        if (!has_row) {
            break;
        }
        CompositeTimbreLibraryEntry entry;
        if (!fromSqliteId(
                sqlite3_column_int64(select_rows.get(), 0), entry.id)) {
            setError(error, "invalid composite timbre id");
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        const auto* name = sqlite3_column_text(select_rows.get(), 1);
        const auto* memo = sqlite3_column_text(select_rows.get(), 2);
        const auto payload_version = sqlite3_column_int64(select_rows.get(), 9);
        const auto* blob = sqlite3_column_blob(select_rows.get(), 10);
        const auto blob_size = sqlite3_column_bytes(select_rows.get(), 10);
        if (blob == nullptr || blob_size <= 0) {
            setError(error, "missing composite sound payload");
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        if (payload_version != 1 && payload_version != 2 && payload_version != 3
            && payload_version != 4 && payload_version != 5
            && payload_version
                != static_cast<sqlite3_int64>(kCompositeSoundPayloadVersion)) {
            setError(error, "unsupported composite sound payload version");
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        const auto payload = std::string_view(
            static_cast<const char*>(blob),
            static_cast<std::size_t>(blob_size));
        auto timbre = deserializeCompositeSoundPayload(
            payload,
            static_cast<std::uint32_t>(payload_version),
            error);
        if (!timbre) {
            if (!allow_partial) { return false; }
            complete = false;
            continue;
        }
        entry.timbre = std::move(*timbre);
        entry.timbre.name = name != nullptr
            ? reinterpret_cast<const char*>(name)
            : "";
        entry.timbre.memo = memo != nullptr
            ? reinterpret_cast<const char*>(memo)
            : "";
        entry.timbre.favorite = sqlite3_column_int64(select_rows.get(), 3) != 0;
        entry.created_unix_seconds = sqlite3_column_int64(select_rows.get(), 4);
        entry.updated_unix_seconds = sqlite3_column_int64(select_rows.get(), 5);
        entry.last_used_unix_seconds =
            sqlite3_column_int64(select_rows.get(), 6);
        entry.use_count = static_cast<std::uint32_t>(
            sqlite3_column_int64(select_rows.get(), 7));
        entry.revision = static_cast<std::uint32_t>(
            sqlite3_column_int64(select_rows.get(), 8));

        entry.timbre.tags.clear();
        if (read_tags) {
            sqlite3_int64 sqlite_id = 0;
            if (!toSqliteId(entry.id, sqlite_id)
                || !select_tags.bindInt64(1, sqlite_id, db_, error)) {
                if (!allow_partial) { return false; }
                complete = false;
                read_tags = false;
            } else {
                for (;;) {
                    bool has_tag = false;
                    if (!select_tags.stepRow(has_tag, db_, error)) {
                        if (!allow_partial) { return false; }
                        complete = false;
                        break;
                    }
                    if (!has_tag) { break; }
                    const auto* tag = sqlite3_column_text(select_tags.get(), 0);
                    if (tag != nullptr) {
                        entry.timbre.tags.emplace_back(
                            reinterpret_cast<const char*>(tag));
                    }
                }
                if (!select_tags.reset(db_, error)) {
                    if (!allow_partial) { return false; }
                    complete = false;
                    read_tags = false;
                }
            }
        }
        entries.push_back(std::move(entry));
    }
    composites.installEntries(std::move(entries));
    return complete;
}

bool ToneLibraryDatabase::load(
    TimbreLibrary& timbres,
    CompositeTimbreLibrary& composites,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!enableForeignKeys(error)) {
        return false;
    }
    TimbreLibrary loaded_timbres;
    CompositeTimbreLibrary loaded_composites;
    if (!loadTimbres(loaded_timbres, error)
        || !loadComposites(loaded_composites, error)
        || !toneSetsReadable(error)) {
        return false;
    }
    timbres = std::move(loaded_timbres);
    composites = std::move(loaded_composites);
    return true;
}

bool ToneLibraryDatabase::loadAvailable(
    TimbreLibrary& timbres,
    CompositeTimbreLibrary& composites,
    std::string* error) {
    timbres = {};
    composites = {};
    if (error) { error->clear(); }
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!enableForeignKeys(error)) {
        return false;
    }
    // Do not short-circuit: a broken table must not discard the other one.
    const bool timbres_complete = loadTimbres(timbres, error, true);
    const bool composites_complete = loadComposites(composites, error, true);
    const bool sets_complete = toneSetsReadable(error);
    return timbres_complete && composites_complete && sets_complete;
}

bool ToneLibraryDatabase::replaceTimbres(
    const TimbreLibrary& timbres,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!enableForeignKeys(error) || !beginImmediate(error)) {
        return false;
    }
    if (!writeTimbres(timbres, error) || !commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::replaceComposites(
    const CompositeTimbreLibrary& composites,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!enableForeignKeys(error) || !beginImmediate(error)) {
        return false;
    }
    if (!writeComposites(composites, error) || !commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::replaceAll(
    const TimbreLibrary& timbres,
    const CompositeTimbreLibrary& composites,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!enableForeignKeys(error) || !beginImmediate(error)) {
        return false;
    }
    if (!writeTimbres(timbres, error)
        || !writeComposites(composites, error)
        || !pruneUnusedTags(error)
        || !commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::toneSetsReadable(std::string* error) {
    SqliteStatement sets;
    SqliteStatement members;
    bool has_row = false;
    if (!sets.prepare(db_, "SELECT id FROM tone_sets LIMIT 1;", error)
        || !sets.stepRow(has_row, db_, error)
        || !members.prepare(
            db_,
            "SELECT set_id, timbre_id, composite_id, position "
            "FROM tone_set_members LIMIT 1;",
            error)
        || !members.stepRow(has_row, db_, error)) {
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::loadToneSets(
    std::vector<ToneSetRecord>& sets,
    std::vector<ToneSetMemberRecord>& members,
    std::string* error) {
    sets.clear();
    members.clear();
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    SqliteStatement select_sets;
    SqliteStatement select_members;
    if (!select_sets.prepare(
            db_,
            "SELECT id, name, memo, created_at, updated_at, revision "
            "FROM tone_sets ORDER BY name, id;",
            error)
        || !select_members.prepare(
            db_,
            "SELECT set_id, timbre_id, composite_id, position "
            "FROM tone_set_members ORDER BY set_id, position, rowid;",
            error)) {
        return false;
    }
    for (;;) {
        bool has_row = false;
        if (!select_sets.stepRow(has_row, db_, error)) {
            return false;
        }
        if (!has_row) {
            break;
        }
        ToneSetRecord set;
        if (!fromSqliteId(sqlite3_column_int64(select_sets.get(), 0), set.id)) {
            setError(error, "invalid tone set id");
            return false;
        }
        const auto* name = sqlite3_column_text(select_sets.get(), 1);
        const auto* memo = sqlite3_column_text(select_sets.get(), 2);
        set.name = name != nullptr ? reinterpret_cast<const char*>(name) : "";
        set.memo = memo != nullptr ? reinterpret_cast<const char*>(memo) : "";
        set.created_unix_seconds = sqlite3_column_int64(select_sets.get(), 3);
        set.updated_unix_seconds = sqlite3_column_int64(select_sets.get(), 4);
        set.revision = sqlite3_column_int64(select_sets.get(), 5);
        sets.push_back(std::move(set));
    }
    for (;;) {
        bool has_row = false;
        if (!select_members.stepRow(has_row, db_, error)) {
            return false;
        }
        if (!has_row) {
            break;
        }
        ToneSetMemberRecord member;
        if (!fromSqliteId(
                sqlite3_column_int64(select_members.get(), 0),
                member.set_id)) {
            setError(error, "invalid tone set member");
            return false;
        }
        if (sqlite3_column_type(select_members.get(), 1) != SQLITE_NULL) {
            if (!fromSqliteId(
                    sqlite3_column_int64(select_members.get(), 1),
                    member.timbre_id)) {
                setError(error, "invalid tone set timbre id");
                return false;
            }
        }
        if (sqlite3_column_type(select_members.get(), 2) != SQLITE_NULL) {
            if (!fromSqliteId(
                    sqlite3_column_int64(select_members.get(), 2),
                    member.composite_id)) {
                setError(error, "invalid tone set composite id");
                return false;
            }
        }
        member.position = sqlite3_column_int(select_members.get(), 3);
        members.push_back(member);
    }
    return true;
}

bool ToneLibraryDatabase::validateToneSets(
    const std::vector<ToneSetRecord>& sets,
    const std::vector<ToneSetMemberRecord>& members,
    std::string* error) const {
    std::vector<std::uint64_t> set_ids;
    set_ids.reserve(sets.size());
    for (const auto& set : sets) {
        if (set.id == 0 || set.name.empty()) {
            setError(error, "tone set requires an id and a name");
            return false;
        }
        if (std::find(set_ids.begin(), set_ids.end(), set.id) != set_ids.end()) {
            setError(error, "duplicate tone set id");
            return false;
        }
        set_ids.push_back(set.id);
    }
    for (const auto& member : members) {
        const bool has_timbre = member.timbre_id != 0;
        const bool has_composite = member.composite_id != 0;
        if (has_timbre == has_composite
            || std::find(set_ids.begin(), set_ids.end(), member.set_id)
                == set_ids.end()
            || member.position < 0) {
            setError(error, "invalid tone set member");
            return false;
        }
    }
    return true;
}

bool ToneLibraryDatabase::replaceToneSets(
    const std::vector<ToneSetRecord>& sets,
    const std::vector<ToneSetMemberRecord>& members,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!validateToneSets(sets, members, error)) {
        return false;
    }
    if (!enableForeignKeys(error) || !beginImmediate(error)) {
        return false;
    }
    if (!writeToneSets(sets, members, error) || !commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::replaceAllAndToneSets(
    const TimbreLibrary& timbres,
    const CompositeTimbreLibrary& composites,
    const std::vector<ToneSetRecord>& sets,
    const std::vector<ToneSetMemberRecord>& members,
    std::string* error) {
    if (db_ == nullptr) {
        setError(error, "tone library is not open");
        return false;
    }
    if (!validateToneSets(sets, members, error)) {
        return false;
    }
    if (!enableForeignKeys(error) || !beginImmediate(error)) {
        return false;
    }
    if (!writeTimbres(timbres, error)
        || !writeComposites(composites, error)
        || !pruneUnusedTags(error)
        || !writeToneSets(sets, members, error)
        || !commit(error)) {
        rollback();
        return false;
    }
    return true;
}

bool ToneLibraryDatabase::writeToneSets(
    const std::vector<ToneSetRecord>& sets,
    const std::vector<ToneSetMemberRecord>& members,
    std::string* error) {
    if (!exec(
            "CREATE TEMP TABLE IF NOT EXISTS mgstc_keep_set_ids("
            "id INTEGER PRIMARY KEY);",
            error)
        || !exec("DELETE FROM mgstc_keep_set_ids;", error)) {
        rollback();
        return false;
    }
    SqliteStatement upsert_set;
    SqliteStatement keep_set;
    SqliteStatement insert_member;
    if (!upsert_set.prepare(
            db_,
            "INSERT INTO tone_sets("
            "id, name, memo, created_at, updated_at, revision"
            ") VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(id) DO UPDATE SET "
            "name = excluded.name, memo = excluded.memo, "
            "created_at = excluded.created_at, "
            "updated_at = excluded.updated_at, "
            "revision = excluded.revision;",
            error)
        || !keep_set.prepare(
            db_,
            "INSERT INTO mgstc_keep_set_ids(id) VALUES (?);",
            error)
        || !insert_member.prepare(
            db_,
            "INSERT INTO tone_set_members("
            "set_id, timbre_id, composite_id, position"
            ") VALUES (?, ?, ?, ?);",
            error)) {
        rollback();
        return false;
    }
    for (const auto& set : sets) {
        sqlite3_int64 id = 0;
        if (!toSqliteId(set.id, id)
            || !upsert_set.bindInt64(1, id, db_, error)
            || !upsert_set.bindText(2, set.name, db_, error)
            || !upsert_set.bindText(3, set.memo, db_, error)
            || !upsert_set.bindInt64(4, set.created_unix_seconds, db_, error)
            || !upsert_set.bindInt64(5, set.updated_unix_seconds, db_, error)
            || !upsert_set.bindInt64(6, set.revision, db_, error)
            || !upsert_set.stepDone(db_, error)
            || !upsert_set.reset(db_, error)
            || !keep_set.bindInt64(1, id, db_, error)
            || !keep_set.stepDone(db_, error)
            || !keep_set.reset(db_, error)) {
            rollback();
            return false;
        }
    }
    if (!exec(
            "DELETE FROM tone_sets WHERE id NOT IN ("
            "SELECT id FROM mgstc_keep_set_ids);",
            error)
        || !exec(
            "DELETE FROM tone_set_members WHERE set_id IN ("
            "SELECT id FROM mgstc_keep_set_ids);",
            error)) {
        rollback();
        return false;
    }
    for (const auto& member : members) {
        sqlite3_int64 set_id = 0;
        sqlite3_int64 timbre_id = 0;
        sqlite3_int64 composite_id = 0;
        if (!toSqliteId(member.set_id, set_id)
            || !insert_member.bindInt64(1, set_id, db_, error)) {
            rollback();
            return false;
        }
        bool bound = true;
        if (member.timbre_id != 0) {
            bound = toSqliteId(member.timbre_id, timbre_id)
                && insert_member.bindInt64(2, timbre_id, db_, error)
                && insert_member.bindNull(3, db_, error);
        } else {
            bound = toSqliteId(member.composite_id, composite_id)
                && insert_member.bindNull(2, db_, error)
                && insert_member.bindInt64(3, composite_id, db_, error);
        }
        if (!bound
            || !insert_member.bindInt64(4, member.position, db_, error)
            || !insert_member.stepDone(db_, error)
            || !insert_member.reset(db_, error)) {
            rollback();
            return false;
        }
    }
    return true;
}

}  // namespace mgstc::engine
