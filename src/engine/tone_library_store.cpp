#include "mgstc/engine/tone_library_store.hpp"

#include "mgstc/engine/composite_timbre_library.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
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
    // Sequential per-generation upgrades. v1 is CREATE; later versions
    // append `if (version < 2) migrateToV2();` here.
    if (version < 1 && !migrateToV1(error)) {
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
    if (!exec("DELETE FROM timbre_tags;", error)
        || !exec("DELETE FROM timbres;", error)) {
        return false;
    }

    SqliteStatement insert_timbre;
    SqliteStatement insert_tag;
    SqliteStatement select_tag;
    SqliteStatement insert_link;
    if (!insert_timbre.prepare(
            db_,
            "INSERT INTO timbres ("
            "id, category, name, memo, favorite, created_at, updated_at, "
            "last_used_at, use_count, revision, payload"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
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
            || !insert_timbre.reset(db_, error)) {
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
    return true;
}

bool ToneLibraryDatabase::writeComposites(
    const CompositeTimbreLibrary& composites,
    std::string* error) {
    if (!exec("DELETE FROM composite_tags;", error)
        || !exec("DELETE FROM composite_timbres;", error)) {
        return false;
    }

    SqliteStatement insert_row;
    SqliteStatement insert_tag;
    SqliteStatement select_tag;
    SqliteStatement insert_link;
    if (!insert_row.prepare(
            db_,
            "INSERT INTO composite_timbres ("
            "id, name, memo, favorite, created_at, updated_at, last_used_at, "
            "use_count, revision, payload_version, payload"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
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
            || !insert_row.reset(db_, error)) {
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
    return true;
}

bool ToneLibraryDatabase::loadTimbres(
    TimbreLibrary& timbres,
    std::string* error) {
    SqliteStatement select_rows;
    SqliteStatement select_tags;
    if (!select_rows.prepare(
            db_,
            "SELECT id, category, name, memo, favorite, created_at, "
            "updated_at, last_used_at, use_count, revision, payload "
            "FROM timbres ORDER BY id;",
            error)
        || !select_tags.prepare(
            db_,
            "SELECT tags.name FROM timbre_tags "
            "JOIN tags ON tags.id = timbre_tags.tag_id "
            "WHERE timbre_tags.timbre_id = ? "
            "ORDER BY tags.name COLLATE NOCASE, tags.id;",
            error)) {
        return false;
    }

    std::vector<TimbreLibraryEntry> entries;
    for (;;) {
        bool has_row = false;
        if (!select_rows.stepRow(has_row, db_, error)) {
            return false;
        }
        if (!has_row) {
            break;
        }
        TimbreLibraryEntry entry;
        if (!fromSqliteId(sqlite3_column_int64(select_rows.get(), 0), entry.id)) {
            setError(error, "invalid timbre id");
            return false;
        }
        const auto category = sqlite3_column_int64(select_rows.get(), 1);
        if (category != 0 && category != 1) {
            setError(error, "invalid timbre category");
            return false;
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
            return false;
        }
        sqlite3_int64 sqlite_id = 0;
        if (!toSqliteId(entry.id, sqlite_id)
            || !select_tags.bindInt64(1, sqlite_id, db_, error)) {
            return false;
        }
        for (;;) {
            bool has_tag = false;
            if (!select_tags.stepRow(has_tag, db_, error)) {
                return false;
            }
            if (!has_tag) {
                break;
            }
            const auto* tag = sqlite3_column_text(select_tags.get(), 0);
            if (tag != nullptr) {
                entry.tags.emplace_back(
                    reinterpret_cast<const char*>(tag));
            }
        }
        if (!select_tags.reset(db_, error)) {
            return false;
        }
        entries.push_back(std::move(entry));
    }
    timbres.installEntries(std::move(entries));
    return true;
}

bool ToneLibraryDatabase::loadComposites(
    CompositeTimbreLibrary& composites,
    std::string* error) {
    SqliteStatement select_rows;
    SqliteStatement select_tags;
    if (!select_rows.prepare(
            db_,
            "SELECT id, name, memo, favorite, created_at, updated_at, "
            "last_used_at, use_count, revision, payload_version, payload "
            "FROM composite_timbres ORDER BY id;",
            error)
        || !select_tags.prepare(
            db_,
            "SELECT tags.name FROM composite_tags "
            "JOIN tags ON tags.id = composite_tags.tag_id "
            "WHERE composite_tags.composite_id = ? "
            "ORDER BY tags.name COLLATE NOCASE, tags.id;",
            error)) {
        return false;
    }

    std::vector<CompositeTimbreLibraryEntry> entries;
    for (;;) {
        bool has_row = false;
        if (!select_rows.stepRow(has_row, db_, error)) {
            return false;
        }
        if (!has_row) {
            break;
        }
        CompositeTimbreLibraryEntry entry;
        if (!fromSqliteId(
                sqlite3_column_int64(select_rows.get(), 0), entry.id)) {
            setError(error, "invalid composite timbre id");
            return false;
        }
        const auto* name = sqlite3_column_text(select_rows.get(), 1);
        const auto* memo = sqlite3_column_text(select_rows.get(), 2);
        const auto payload_version = sqlite3_column_int64(select_rows.get(), 9);
        const auto* blob = sqlite3_column_blob(select_rows.get(), 10);
        const auto blob_size = sqlite3_column_bytes(select_rows.get(), 10);
        if (blob == nullptr || blob_size <= 0) {
            setError(error, "missing composite sound payload");
            return false;
        }
        if (payload_version
            != static_cast<sqlite3_int64>(kCompositeSoundPayloadVersion)) {
            setError(error, "unsupported composite sound payload version");
            return false;
        }
        const auto payload = std::string_view(
            static_cast<const char*>(blob),
            static_cast<std::size_t>(blob_size));
        auto timbre = deserializeCompositeSoundPayload(
            payload,
            static_cast<std::uint32_t>(payload_version),
            error);
        if (!timbre) {
            return false;
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

        sqlite3_int64 sqlite_id = 0;
        if (!toSqliteId(entry.id, sqlite_id)
            || !select_tags.bindInt64(1, sqlite_id, db_, error)) {
            return false;
        }
        entry.timbre.tags.clear();
        for (;;) {
            bool has_tag = false;
            if (!select_tags.stepRow(has_tag, db_, error)) {
                return false;
            }
            if (!has_tag) {
                break;
            }
            const auto* tag = sqlite3_column_text(select_tags.get(), 0);
            if (tag != nullptr) {
                entry.timbre.tags.emplace_back(
                    reinterpret_cast<const char*>(tag));
            }
        }
        if (!select_tags.reset(db_, error)) {
            return false;
        }
        entries.push_back(std::move(entry));
    }
    composites.installEntries(std::move(entries));
    return true;
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
        || !loadComposites(loaded_composites, error)) {
        return false;
    }
    timbres = std::move(loaded_timbres);
    composites = std::move(loaded_composites);
    return true;
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

}  // namespace mgstc::engine
