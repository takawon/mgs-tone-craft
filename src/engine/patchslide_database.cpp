#include "mgstc/engine/patchslide_database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#endif

namespace mgstc::engine {
namespace {

struct DatasetProfile {
    std::string_view manifest_hash;
    std::string_view source_hash;
    std::string_view search_hash;
    std::string_view ids_hash;
    std::size_t candidate_count;
    std::size_t tag_records;
    std::size_t untagged_candidates;
};

// Each profile binds one complete, verified delivery. A changed manifest does
// not authorize arbitrary files, candidate counts, or search definitions.
constexpr std::array<DatasetProfile, 3> dataset_profiles{{
    {"4b2c918887dec0c257811c34b8e886c323fc1697bdbf205af1e4e5b021ec2880",
     "27f6d7821d0ddd16caafd8397a603670977821823a21fee7146df410665d2793",
     "0f086f089c4373490988302031a6dd4545f441cd50ec3e3a24a6e43d41b8b3e1",
     "0b7e33a6cab107cac827e09835b42be86c3a53f605738b6dbebd6cb2b1dc2a10",
     PatchSlideDatabase::candidate_count, 71809, 28},
    {"839ecd1659616dc7e8731ee9e046db10da0cdbedf8f03ef401286f0c70330a10",
     "5122f57eedd251f7dac2e1184ded353b1c86090343567020421bf27d7ea641b6",
     "14b17f3b5458d3887a1aa73f53eeb71b6a9cff1f01280eb7c965dc1d10f0075f",
     "968d8bdcc5ebd56b48665d973ee30223558347c99368181632695571db9dd6f5",
     PatchSlideDatabase::pruned_candidate_count, 62654, 26},
    {"d912cf65a8f61fe4a1a3d5c07746525401038521cf48b3bf3cce51b7d6c7eb93",
     "67e3de24979e2c3fe0817653fa8fce6386ee91099b9a615d4231a7a32512d0a0",
     "a7b43f1121e2355f2023488f078cdbe7c8f952d028b6602291cc4a6616e47fa1",
     "968d8bdcc5ebd56b48665d973ee30223558347c99368181632695571db9dd6f5",
     PatchSlideDatabase::pruned_candidate_count, 62654, 26}
}};
constexpr std::string_view method_version = "B-frozen-acoustic-search-v1";
constexpr std::string_view layout_hash = "ee9425083fb7741c2941b1c5ef2974287e039b23b37905d064e28df2388695a9";
constexpr std::string_view config_hash = "0faa965fb5db348d719aba2b469874011914ed9c1b8528321849e54e2bc6ef2f";
constexpr std::string_view fingerprint_layout_hash = "393fdf6421ce99a95ba314b4d61ea98f45209eea51993d4b59d949dd2095b6f8";
constexpr std::string_view metrics_hash = "4a8d87c02f6c137d69de5cbe08b72394f73f85d1b016821ab07ed85c88555d8c";
constexpr std::string_view scales_hash = "888c9fcb86052fb41d1d0e8f845fe17265abf309ab8a24da5b80761f51f900d0";

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

class Sha256 final {
public:
    Sha256() {
#ifdef _WIN32
        require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                "Cannot initialize SHA-256 provider");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            algorithm_ = nullptr;
            throw std::runtime_error("Cannot initialize SHA-256 hash");
        }
#else
        throw std::runtime_error("PatchSlide SHA-256 requires the Windows platform provider");
#endif
    }
    ~Sha256() {
#ifdef _WIN32
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
#endif
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* bytes, std::size_t length) {
#ifdef _WIN32
        require(length <= std::numeric_limits<ULONG>::max(), "SHA-256 input block is too large");
        require(BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(bytes)),
                               static_cast<ULONG>(length), 0) >= 0, "SHA-256 update failed");
#else
        (void)bytes;
        (void)length;
#endif
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
#ifdef _WIN32
        require(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
                "SHA-256 finalization failed");
#endif
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto byte : digest) {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
        return result;
    }
private:
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
#endif
};

std::string hashBytes(std::string_view value) {
    Sha256 hash;
    hash.update(value.data(), value.size());
    return hash.finish();
}

std::string hashFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "Cannot read PatchSlide database file");
    Sha256 hash;
    std::array<char, 65536> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.update(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    }
    require(stream.eof(), "PatchSlide database read failed");
    return hash.finish();
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        require(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) == SQLITE_OK,
                "PatchSlide SQLite schema/query is invalid");
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view value) {
        require(sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                                  SQLITE_TRANSIENT) == SQLITE_OK, "PatchSlide SQLite binding failed");
    }
    void bind(int index, int value) {
        require(sqlite3_bind_int(statement_, index, value) == SQLITE_OK,
                "PatchSlide SQLite integer binding failed");
    }
    bool row() {
        const int result = sqlite3_step(statement_);
        require(result == SQLITE_ROW || result == SQLITE_DONE, "PatchSlide SQLite read failed");
        return result == SQLITE_ROW;
    }
    std::string text(int column) const {
        const auto* data = sqlite3_column_text(statement_, column);
        return data ? std::string(reinterpret_cast<const char*>(data),
                                 static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))) : std::string{};
    }
    int integer(int column) const { return sqlite3_column_int(statement_, column); }
    bool null(int column) const { return sqlite3_column_type(statement_, column) == SQLITE_NULL; }
    std::string_view blob(int column) const {
        return {static_cast<const char*>(sqlite3_column_blob(statement_, column)),
                static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
    }
private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void checkStandalone(const std::filesystem::path& path) {
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        auto sidecar = path;
        sidecar += suffix;
        require(!std::filesystem::exists(sidecar), "PatchSlide database has a live journal/WAL sidecar");
    }
}

class Database final {
public:
    explicit Database(const std::filesystem::path& path) {
        checkStandalone(path);
        const auto generic = std::filesystem::absolute(path).generic_u8string();
        std::string uri = "file:";
        constexpr char hex[] = "0123456789ABCDEF";
        for (char8_t value : generic) {
            const auto byte = static_cast<unsigned char>(value);
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9') || byte == '/' || byte == ':' || byte == '-' || byte == '_' || byte == '.') {
                uri.push_back(static_cast<char>(byte));
            } else {
                uri.push_back('%');
                uri.push_back(hex[byte >> 4]);
                uri.push_back(hex[byte & 15]);
            }
        }
        uri += "?mode=ro&immutable=1";
        if (sqlite3_open_v2(uri.c_str(), &database_, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
                            nullptr) != SQLITE_OK) {
            if (database_) sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error("Cannot open read-only PatchSlide SQLite database");
        }
        sqlite3_exec(database_, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);
    }
    ~Database() { if (database_) sqlite3_close(database_); }
    sqlite3* get() const noexcept { return database_; }
private:
    sqlite3* database_ = nullptr;
};

std::string scalar(sqlite3* database, const char* sql) {
    Statement query(database, sql);
    require(query.row(), "Missing PatchSlide metadata");
    auto result = query.text(0);
    require(!query.row(), "Duplicate PatchSlide metadata");
    return result;
}

std::map<std::string, std::string> metadata(sqlite3* database, const char* sql) {
    std::map<std::string, std::string> result;
    Statement query(database, sql);
    while (query.row()) require(result.emplace(query.text(0), query.text(1)).second, "Duplicate metadata key");
    return result;
}

std::string jsonValue(sqlite3* database, std::string_view json, std::string_view path) {
    Statement query(database, "SELECT json_extract(?1,?2)");
    query.bind(1, json);
    query.bind(2, path);
    require(query.row(), "Cannot read PatchSlide JSON metadata");
    return query.text(0);
}

void checkIntegrity(sqlite3* database) {
    require(scalar(database, "PRAGMA integrity_check") == "ok", "PatchSlide database integrity check failed");
    Statement foreign_keys(database, "PRAGMA foreign_key_check");
    require(!foreign_keys.row(), "PatchSlide database foreign-key check failed");
}

// Fixed binary64 reductions preserve the delivered ranking across CPU dispatch
// choices. Keep the two-lane norm's separately rounded products and the dot's
// explicit fused steps. Do not enable /fp:fast or use squared differences.
double normSquared(std::span<const double> values) {
    std::array<double, 2> lanes{};
    for (std::size_t block = 0; block < values.size(); block += 8) {
        for (std::size_t offset : {std::size_t{6}, std::size_t{4}, std::size_t{2}, std::size_t{0}}) {
            for (std::size_t lane = 0; lane < 2; ++lane) {
                const double value = values[block + offset + lane];
                const double product = value * value;
                lanes[lane] += product;
            }
        }
    }
    return lanes[0] + lanes[1];
}

enum class DotReduction { fused_four, separate_two, separate_four };

// The delivered full-ranking baseline freezes eight balanced output spans. This
// controls arithmetic only: no runtime CPU dispatch, worker count or parallel
// execution can change the remainder rows' separately rounded reductions.
DotReduction globalDotReduction(std::size_t row, std::size_t count) {
    constexpr std::size_t span_count = 8;
    std::size_t begin = 0;
    std::size_t remaining = count;
    for (std::size_t span = 0; span < span_count; ++span) {
        const auto left = span_count - span;
        const auto width = (remaining + left - 1) / left;
        if (row < begin + width) {
            const auto remainder = width % 4;
            const auto local = row - begin;
            const auto complete = width - remainder;
            if (local < complete) return DotReduction::fused_four;
            if ((remainder & 2) != 0 && local < complete + 2) return DotReduction::separate_two;
            return DotReduction::separate_four;
        }
        begin += width;
        remaining -= width;
    }
    throw std::runtime_error("PatchSlide dot row is outside the complete candidate set");
}

double distanceWithNorms(std::span<const double> query, std::span<const double> target,
                         double query_norm, double target_norm,
                         DotReduction reduction = DotReduction::fused_four) {
    double dot = 0.0;
    if (reduction == DotReduction::separate_two) {
        std::array<double, 2> lanes{};
        for (std::size_t i = 0; i < query.size(); i += 2) {
            const double even_product = (2.0 * query[i]) * target[i];
            const double odd_product = (2.0 * query[i + 1]) * target[i + 1];
            lanes[0] += even_product;
            lanes[1] += odd_product;
        }
        dot = lanes[0] + lanes[1];
    } else {
        std::array<double, 4> lanes{};
        for (std::size_t i = 0; i < query.size(); i += 4) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                if (reduction == DotReduction::fused_four) {
                    lanes[lane] = std::fma(2.0 * query[i + lane], target[i + lane], lanes[lane]);
                } else {
                    const double product = (2.0 * query[i + lane]) * target[i + lane];
                    lanes[lane] += product;
                }
            }
        }
        dot = (lanes[0] + lanes[2]) + (lanes[1] + lanes[3]);
    }
    return std::max(query_norm + target_norm - dot, 0.0);
}

using FileStamp = std::pair<std::uintmax_t, std::filesystem::file_time_type>;
using FileStamps = std::array<FileStamp, 3>;
FileStamps fileStamps(const std::filesystem::path& directory) {
    FileStamps result{};
    std::size_t i = 0;
    std::string missing;
    for (const char* name : {"patchslide.sqlite", "patchslide_search.sqlite", "manifest.json"}) {
        auto path = directory / name;
        if (!std::filesystem::is_regular_file(path)) {
            if (!missing.empty()) missing += ", ";
            missing += name;
        } else {
            result[i] = {std::filesystem::file_size(path), std::filesystem::last_write_time(path)};
            if (i < 2) checkStandalone(path);
        }
        ++i;
    }
    require(missing.empty(), "Missing PatchSlide files: " + missing);
    return result;
}

} // namespace

std::filesystem::path PatchSlideDatabase::defaultDirectory() {
#ifdef _WIN32
    PWSTR value = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &value))) {
        std::filesystem::path directory(value);
        CoTaskMemFree(value);
        return directory / L"OPLL_PatchSlide_DB";
    }
    return std::filesystem::path(L"C:\\ProgramData\\OPLL_PatchSlide_DB");
#else
    return std::filesystem::path("/usr/local/share/OPLL_PatchSlide_DB");
#endif
}

std::shared_ptr<const PatchSlideDatabase> PatchSlideDatabase::load(std::string* error) {
    return load(defaultDirectory(), error);
}

std::shared_ptr<const PatchSlideDatabase> PatchSlideDatabase::load(
    const std::filesystem::path& directory, std::string* error) {
    struct Cached { FileStamps stamps; std::weak_ptr<const PatchSlideDatabase> database; };
    static std::mutex cache_mutex;
    static std::map<std::filesystem::path, Cached> cache;
    if (error) error->clear();
    try {
        const auto canonical = std::filesystem::weakly_canonical(directory);
        // Worker-only lock also prevents concurrent instances loading duplicate 86MB arrays.
        const std::lock_guard lock(cache_mutex);
        const auto stamps = fileStamps(canonical);
        const auto previous = cache.find(canonical);
        if (previous != cache.end() && previous->second.stamps == stamps) {
            if (auto existing = previous->second.database.lock()) return existing;
        }
        auto database = std::shared_ptr<PatchSlideDatabase>(new PatchSlideDatabase);
        database->read(canonical);
        require(fileStamps(canonical) == stamps, "PatchSlide files changed during initialization");
        cache[canonical] = {stamps, database};
        return database;
    } catch (const std::exception& exception) {
        if (error) *error = std::string(exception.what()) + "\n" + pathText(directory);
        return {};
    }
}

void PatchSlideDatabase::read(const std::filesystem::path& directory) {
    const auto source_path = directory / "patchslide.sqlite";
    const auto search_path = directory / "patchslide_search.sqlite";
    const auto manifest_path = directory / "manifest.json";
    require(std::filesystem::file_size(manifest_path) <= 4 * 1024 * 1024, "PatchSlide manifest is too large");
    std::ifstream stream(manifest_path, std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    require(stream.eof() || stream.good(), "Cannot read PatchSlide manifest");
    const auto manifest_hash = hashBytes(manifest);
    const auto selected = std::find_if(dataset_profiles.begin(), dataset_profiles.end(),
        [&](const auto& profile) { return profile.manifest_hash == manifest_hash; });
    require(selected != dataset_profiles.end(), "PatchSlide manifest SHA-256 mismatch");
    const auto& profile = *selected;
    const auto source_hash = profile.source_hash;
    const auto search_hash = profile.search_hash;
    const auto ids_hash = profile.ids_hash;
    const auto candidate_count = profile.candidate_count;
    const auto candidate_count_text = std::to_string(candidate_count);
    require(hashFile(source_path) == source_hash, "PatchSlide source database SHA-256 mismatch");
    require(hashFile(search_path) == search_hash, "PatchSlide search database SHA-256 mismatch");
    Database source(source_path);
    Database search(search_path);
    auto* db = search.get();
    const auto manifest_value = [&](std::string_view path) { return jsonValue(db, manifest, path); };
    require(scalar(db, "PRAGMA user_version") == "2", "PatchSlide search schema must be version 2");
    require(scalar(source.get(), "PRAGMA user_version") == "1", "PatchSlide source schema must be version 1");
    require(manifest_value("$.manifest_version") == "patchslide-search-manifest-v2"
            && manifest_value("$.schema_version") == "2"
            && manifest_value("$.method_version") == method_version
            && manifest_value("$.vector_codec") == "f64le_v1"
            && manifest_value("$.vector_dimension") == "768"
            && manifest_value("$.candidate_count") == candidate_count_text
            && manifest_value("$.search_db_sha256") == search_hash
            && manifest_value("$.source_db_sha256") == source_hash,
            "PatchSlide manifest schema/method/database binding mismatch");
    checkIntegrity(db);
    checkIntegrity(source.get());
    const auto meta = metadata(db, "SELECT key,value FROM search_meta");
    const auto source_meta = metadata(source.get(), "SELECT key,value FROM db_meta");
    require(meta.at("schema_version") == "2" && source_meta.at("schema_version") == "1",
            "PatchSlide schema metadata mismatch");
    const auto& binding = meta.at("source_binding_json");
    require(hashBytes(binding) == meta.at("source_binding_sha256"), "PatchSlide source binding hash mismatch");
    const std::array<std::pair<const char*, const char*>, 4> source_fields{{
        {"source_build_id", "build_id"}, {"source_schema_version", "schema_version"},
        {"source_manifest_sha256", "source_manifest_sha256"},
        {"source_analysis_manifest_sha256", "source_analysis_manifest_sha256"}}};
    for (const auto& [field, source_field] : source_fields) {
        const auto expected = source_meta.at(source_field);
        require(jsonValue(db, binding, std::string("$.") + field) == expected
                && manifest_value(std::string("$.source_binding.") + field) == expected,
                "PatchSlide source/search identity mismatch");
    }
    for (const auto& [field, expected] : std::array<std::pair<const char*, std::string_view>, 6>{{
             {"source_db_sha256", source_hash}, {"candidate_ids_sha256", ids_hash},
             {"fingerprint_layout_sha256", fingerprint_layout_hash}, {"fingerprint_version", "fp436_v1"},
             {"fingerprint_codec", "fp436_q16le_v1"}, {"candidate_count", candidate_count_text}}}) {
        require(jsonValue(db, binding, std::string("$.") + field) == expected
                && manifest_value(std::string("$.source_binding.") + field) == expected,
                "PatchSlide source binding contract mismatch");
    }
    require(meta.at("runtime_manifest_sha256") == manifest_value("$.runtime_manifest_sha256")
            && meta.at("schema_sql_sha256") == manifest_value("$.schema_sql_sha256"),
            "PatchSlide manifest metadata mismatch");
    require(manifest_value("$.candidate_ids_sha256") == ids_hash, "PatchSlide manifest candidate IDs mismatch");
    Statement layout(source.get(), "SELECT fingerprint_version,codec,element_count,layout_json,layout_sha256 FROM fingerprint_layouts");
    require(layout.row() && layout.text(0) == "fp436_v1" && layout.text(1) == "fp436_q16le_v1"
            && layout.integer(2) == 436 && hashBytes(layout.text(3)) == layout.text(4)
            && layout.text(4) == fingerprint_layout_hash && !layout.row(), "PatchSlide fingerprint layout mismatch");
    Statement policy(source.get(), "SELECT policy_version,rule_text,config_json,config_sha256 FROM decision_policy");
    require(policy.row() && policy.text(0) == "partition-AAAA-raw6same-v1"
            && policy.text(1) == "predicted_partition == \"AAAA\" AND raw_different_count == 0"
            && hashBytes(policy.text(2)) == policy.text(3) && !policy.row(), "PatchSlide decision policy mismatch");

    Statement method(db, "SELECT method_id,method_version,dimension,vector_codec,config_json,config_sha256,layout_json,layout_sha256,implementation_sha256,generator_version,generator_sha256 FROM search_methods");
    require(method.row() && method.text(0) == "standard_b" && method.text(1) == method_version
            && method.integer(2) == 768 && method.text(3) == "f64le_v1", "Unsupported PatchSlide search method");
    require(hashBytes(method.text(4)) == config_hash && method.text(5) == config_hash
            && manifest_value("$.method_config_sha256") == config_hash
            && hashBytes(method.text(6)) == layout_hash && method.text(7) == layout_hash
            && manifest_value("$.vector_layout_sha256") == layout_hash
            && method.text(8) == metrics_hash && manifest_value("$.frozen_metrics_sha256") == metrics_hash
            && manifest_value("$.frozen_scales_sha256") == scales_hash
            && method.text(9) == manifest_value("$.generator_version")
            && method.text(10) == manifest_value("$.generator_sha256") && !method.row(),
            "PatchSlide frozen search method/layout metadata mismatch");

    candidates_.reserve(candidate_count);
    Statement candidates(source.get(), "SELECT candidate_id,candidate_no,patch_a,patch_b,patch_c,change_count,hold_pattern,hold_a_steps,hold_b_steps,auto_safe,manual_use_allowed,enabled,visibility,predicted_partition,human_pattern,raw_different_count FROM candidates ORDER BY candidate_id COLLATE BINARY");
    std::string canonical_ids = "[";
    while (candidates.row()) {
        PatchSlideCandidate candidate;
        candidate.candidate_id = candidates.text(0);
        candidate.candidate_no = candidates.integer(1);
        candidate.patch_a = candidates.integer(2);
        candidate.patch_b = candidates.integer(3);
        if (!candidates.null(4)) candidate.patch_c = candidates.integer(4);
        candidate.change_count = candidates.integer(5);
        candidate.hold_pattern = candidates.text(6);
        candidate.hold_a_steps = candidates.integer(7);
        if (!candidates.null(8)) candidate.hold_b_steps = candidates.integer(8);
        candidate.auto_safe = candidates.integer(9) != 0;
        candidate.manual_use_allowed = candidates.integer(10) != 0;
        candidate.enabled = candidates.integer(11) != 0;
        candidate.visibility = candidates.text(12);
        candidate.predicted_partition = candidates.text(13);
        candidate.human_pattern = candidates.text(14);
        candidate.raw_different_count = candidates.integer(15);
        require(candidate.auto_safe == (candidate.predicted_partition == "AAAA" && candidate.raw_different_count == 0),
                "PatchSlide AUTO_SAFE differs from its decision policy");
        require(candidate.candidate_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos,
                "Invalid PatchSlide candidate ID");
        if (!candidates_.empty()) canonical_ids += ',';
        canonical_ids += '"' + candidate.candidate_id + '"';
        candidates_.push_back(std::move(candidate));
    }
    canonical_ids += ']';
    require(candidates_.size() == candidate_count && hashBytes(canonical_ids) == ids_hash,
            "PatchSlide requires the complete candidate ID set for its delivery");
    vectors_.resize(candidate_count * vector_dimension);
    norms_.resize(candidate_count);
    Statement vectors(db, "SELECT candidate_id,method_id,dimension,codec,vector_blob,vector_sha256 FROM search_vectors ORDER BY candidate_id COLLATE BINARY");
    std::size_t index = 0;
    while (vectors.row()) {
        require(index < candidates_.size() && vectors.text(0) == candidates_[index].candidate_id
                && vectors.text(1) == "standard_b" && vectors.integer(2) == 768 && vectors.text(3) == "f64le_v1",
                "PatchSlide vector candidate/method correspondence mismatch");
        const auto blob = vectors.blob(4);
        require(blob.size() == vector_dimension * sizeof(double) && hashBytes(blob) == vectors.text(5),
                "PatchSlide vector payload size/SHA-256 mismatch");
        auto values = std::span<double>(vectors_.data() + index * vector_dimension, vector_dimension);
        for (std::size_t element = 0; element < vector_dimension; ++element) {
            std::uint64_t bits = 0;
            for (unsigned byte = 0; byte < 8; ++byte)
                bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(blob[element * 8 + byte])) << (byte * 8);
            values[element] = std::bit_cast<double>(bits);
            require(std::isfinite(values[element]), "PatchSlide vector contains nonfinite values");
        }
        norms_[index] = normSquared(values);
        ++index;
    }
    require(index == candidate_count, "PatchSlide vector coverage mismatch");
    require(scalar(db, "SELECT count(*) FROM tagging_candidates") == candidate_count_text
            && scalar(db, "SELECT count(*) FROM tags") == "94"
            && scalar(db, "SELECT count(*) FROM candidate_tags") == std::to_string(profile.tag_records)
            && scalar(db, "SELECT count(*) FROM tagging_candidates c WHERE NOT EXISTS(SELECT 1 FROM candidate_tags t WHERE t.candidate_id=c.candidate_id)") == std::to_string(profile.untagged_candidates),
            "PatchSlide tagging coverage/count mismatch");
    Statement tags(db, "SELECT t.tag,t.status,count(ct.candidate_id),t.ordinal FROM tags t LEFT JOIN candidate_tags ct USING(tag) GROUP BY t.tag ORDER BY t.ordinal");
    while (tags.row()) {
        require(tags.integer(3) == static_cast<int>(tags_.size()), "PatchSlide tag order mismatch");
        tags_.push_back({tags.text(0), tags.text(1), static_cast<std::size_t>(tags.integer(2))});
    }
    Statement edges(db, "SELECT ct.candidate_id,t.tag FROM candidate_tags ct JOIN tags t USING(tag) ORDER BY ct.candidate_id COLLATE BINARY,t.ordinal");
    while (edges.row()) {
        const auto candidate_index = find(edges.text(0));
        require(candidate_index.has_value(), "PatchSlide tag references an unknown candidate");
        candidates_[*candidate_index].tags.push_back(edges.text(1));
    }
    Statement processed(db, "SELECT candidate_id FROM tagging_candidates ORDER BY candidate_id COLLATE BINARY");
    index = 0;
    while (processed.row()) {
        require(index < candidates_.size() && processed.text(0) == candidates_[index++].candidate_id,
                "PatchSlide tagging candidate ID coverage mismatch");
    }
    require(index == candidate_count, "Incomplete PatchSlide tagging processing records");

    Statement sets(db, "SELECT cluster_set_id,cluster_count,distance_method,provenance_json,provenance_sha256 FROM cluster_sets ORDER BY cluster_count");
    int set_index = 0;
    while (sets.row()) {
        const int count = set_index == 0 ? 16 : 64;
        const std::string set_id = "sampled_medoids_k" + std::to_string(count);
        require(set_index < 2 && sets.text(0) == set_id && sets.integer(1) == count
                && sets.text(2) == "D-frozen-acoustic-search-v1"
                && hashBytes(sets.text(3)) == sets.text(4)
                && jsonValue(db, sets.text(3), "$.independent_partition") == "1"
                && jsonValue(db, sets.text(3), "$.dimension") == "1544",
                "PatchSlide independent cluster set provenance mismatch");
        Statement members(db, "SELECT candidate_id,cluster_id FROM cluster_memberships WHERE cluster_set_id=?1 ORDER BY candidate_id COLLATE BINARY");
        members.bind(1, set_id);
        index = 0;
        while (members.row()) {
            require(index < candidates_.size() && members.text(0) == candidates_[index].candidate_id,
                    "PatchSlide cluster candidate ID coverage mismatch");
            (count == 16 ? candidates_[index].cluster16 : candidates_[index].cluster64) = members.integer(1);
            ++index;
        }
        require(index == candidate_count, "Incomplete PatchSlide cluster membership");
        Statement clusters(db, "SELECT cluster_id,representative_candidate_id,member_count,statistics_json,statistics_sha256 FROM clusters WHERE cluster_set_id=?1 ORDER BY cluster_id");
        clusters.bind(1, set_id);
        int cluster_rows = 0;
        std::size_t total = 0;
        while (clusters.row()) {
            const int cluster_id = clusters.integer(0);
            const auto representative = find(clusters.text(1));
            const auto member_count = static_cast<std::size_t>(clusters.integer(2));
            require(representative.has_value() && hashBytes(clusters.text(3)) == clusters.text(4),
                    "PatchSlide cluster representative/statistics mismatch");
            const auto cluster_of = [count](const PatchSlideCandidate& candidate) {
                return count == 16 ? candidate.cluster16 : candidate.cluster64;
            };
            require(cluster_of(candidates_[*representative]) == cluster_id
                    && static_cast<std::size_t>(std::count_if(candidates_.begin(), candidates_.end(),
                         [&](const auto& candidate) { return cluster_of(candidate) == cluster_id; })) == member_count,
                    "PatchSlide cluster member count/representative membership mismatch");
            clusters_.push_back({count, cluster_id, member_count, *representative});
            total += member_count;
            ++cluster_rows;
        }
        require(cluster_rows == count && total == candidate_count, "PatchSlide cluster count mismatch");
        ++set_index;
    }
    require(set_index == 2, "PatchSlide requires independent 16 and 64 cluster sets");
}

const std::vector<PatchSlideCandidate>& PatchSlideDatabase::candidates() const noexcept { return candidates_; }
const std::vector<PatchSlideTag>& PatchSlideDatabase::tags() const noexcept { return tags_; }
const std::vector<PatchSlideCluster>& PatchSlideDatabase::clusters() const noexcept { return clusters_; }

std::optional<std::size_t> PatchSlideDatabase::find(std::string_view id) const {
    const auto item = std::lower_bound(candidates_.begin(), candidates_.end(), id,
                                      [](const auto& candidate, auto value) { return candidate.candidate_id < value; });
    if (item == candidates_.end() || item->candidate_id != id) return {};
    return static_cast<std::size_t>(item - candidates_.begin());
}

bool PatchSlideDatabase::isRejected(const PatchSlideCandidate& candidate) noexcept {
    const auto& pattern = candidate.human_pattern.empty()
        ? candidate.predicted_partition : candidate.human_pattern;
    return candidate.visibility == "hidden"
        || (!candidate.manual_use_allowed && !candidate.auto_safe)
        || (pattern != "AAAA" && pattern != "ABBB");
}

bool PatchSlideDatabase::isVisible(const PatchSlideCandidate& candidate, bool show_rejected) noexcept {
    return candidate.enabled && (show_rejected || !isRejected(candidate));
}

std::vector<std::size_t> PatchSlideDatabase::filter(const PatchSlideFilter& conditions) const {
    require(conditions.cluster_count == 0 || conditions.cluster_count == 16 || conditions.cluster_count == 64,
            "Invalid PatchSlide classification size");
    std::set<std::string> selected(conditions.tags.begin(), conditions.tags.end());
    for (const auto& tag : selected) {
        if (std::none_of(tags_.begin(), tags_.end(), [&](const auto& item) { return item.tag == tag; })) return {};
    }
    std::vector<std::size_t> result;
    result.reserve(candidates_.size());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const auto& candidate = candidates_[index];
        if (!isVisible(candidate, conditions.show_rejected) || (conditions.auto_safe_only && !candidate.auto_safe)) continue;
        if (conditions.cluster_count != 0 && conditions.cluster_id != (conditions.cluster_count == 16 ? candidate.cluster16 : candidate.cluster64)) continue;
        if (!selected.empty()) {
            const auto hits = std::count_if(selected.begin(), selected.end(), [&](const auto& tag) {
                return std::find(candidate.tags.begin(), candidate.tags.end(), tag) != candidate.tags.end();
            });
            if (conditions.tags_match_all ? static_cast<std::size_t>(hits) != selected.size() : hits == 0) continue;
        }
        result.push_back(index);
    }
    return result;
}

double PatchSlideDatabase::squaredDistance(std::span<const double> query, std::span<const double> target) {
    require(query.size() == vector_dimension && target.size() == vector_dimension,
            "PatchSlide distance requires 768-dimensional binary64 vectors");
    return distanceWithNorms(query, target, normSquared(query), normSquared(target));
}

std::vector<PatchSlideSearchResult> PatchSlideDatabase::standardB(std::size_t source_index, std::size_t top_n) const {
    require(source_index < candidates_.size() && top_n > 0 && top_n < candidates_.size(), "Invalid PatchSlide search source/count");
    const auto source = std::span<const double>(vectors_.data() + source_index * vector_dimension, vector_dimension);
    std::vector<PatchSlideSearchResult> result;
    result.reserve(candidates_.size());
    for (std::size_t index = 0; index < candidates_.size(); ++index) {
        const auto target = std::span<const double>(vectors_.data() + index * vector_dimension, vector_dimension);
        const double distance = distanceWithNorms(source, target, norms_[source_index], norms_[index],
                                                  globalDotReduction(index, candidates_.size()));
        require(std::isfinite(distance), "Nonfinite PatchSlide search distance");
        result.push_back({index, 0, index == source_index ? std::numeric_limits<double>::infinity() : distance});
    }
    // Input order is candidate_id COLLATE BINARY, so exact ties retain that order.
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.squared_distance < right.squared_distance;
    });
    result.resize(top_n);
    for (std::size_t index = 0; index < result.size(); ++index) result[index].rank = index + 1;
    return result;
}

PatchSlideVariationResult PatchSlideDatabase::variation(std::size_t source_index) const {
    const auto pool = standardB(source_index, 256);
    std::vector<double> pairs(256 * 256);
    for (std::size_t i = 0; i < 256; ++i) {
        const auto left = std::span<const double>(vectors_.data() + pool[i].candidate_index * vector_dimension, vector_dimension);
        for (std::size_t j = 0; j < 256; ++j) {
            const auto right = std::span<const double>(vectors_.data() + pool[j].candidate_index * vector_dimension, vector_dimension);
            pairs[i * 256 + j] = distanceWithNorms(left, right, norms_[pool[i].candidate_index], norms_[pool[j].candidate_index]);
        }
    }
    return selectVariation(pool, pairs);
}

PatchSlideVariationResult PatchSlideDatabase::selectVariation(
    std::span<const PatchSlideSearchResult> pool, std::span<const double> pairs) {
    require(pool.size() == 256 && pairs.size() == 256 * 256, "Variation requires the complete global pool256 pair matrix");
    std::set<std::size_t> identities;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        require(pool[i].rank == i + 1 && std::isfinite(pool[i].squared_distance)
                && pool[i].squared_distance >= 0 && (i == 0 || pool[i].squared_distance >= pool[i - 1].squared_distance)
                && identities.insert(pool[i].candidate_index).second, "Invalid Variation Standard B pool");
    }
    require(std::all_of(pairs.begin(), pairs.end(), [](double value) { return std::isfinite(value) && value >= 0; }),
            "Invalid Variation pair matrix");
    const auto quantile = [&](double probability) {
        const double h = static_cast<double>(pool.size() - 1) * probability;
        const auto low = static_cast<std::size_t>(std::floor(h));
        const auto high = static_cast<std::size_t>(std::ceil(h));
        const double a = pool[low].squared_distance;
        const double b = pool[high].squared_distance;
        const double fraction = h - static_cast<double>(low);
        // NumPy linear quantile's two-sided lerp avoids loss near the upper end.
        return fraction < 0.5 ? a + (b - a) * fraction : b - (b - a) * (1.0 - fraction);
    };
    PatchSlideVariationResult result;
    result.q10 = quantile(0.1);
    result.q50 = quantile(0.5);
    result.radius = result.q50;
    for (const auto& entry : pool) {
        if (entry.squared_distance == 0) ++result.zero_source_distances;
        else ++result.positive_source_distances;
    }
    std::array<bool, 256> selected{};
    std::array<double, 256> relevance{};
    std::array<double, 256> novelty{};
    if (result.radius != 0) {
        for (std::size_t i = 0; i < 256; ++i) {
            relevance[i] = std::exp(-pool[i].squared_distance / result.radius);
            const double normalized = (pool[i].squared_distance - result.q50) / result.radius;
            novelty[i] = std::exp(-(normalized * normalized));
        }
    }
    for (std::size_t step = 0; step < 3; ++step) {
        std::size_t best = 0;
        double best_score = -std::numeric_limits<double>::infinity();
        double best_diversity = 0;
        if (result.radius == 0) best = step;
        else {
            for (std::size_t i = 0; i < 256; ++i) {
                if (selected[i]) continue;
                double diversity = 0;
                if (step > 0) {
                    double minimum = std::numeric_limits<double>::infinity();
                    for (std::size_t j = 0; j < 256; ++j) if (selected[j]) minimum = std::min(minimum, pairs[i * 256 + j]);
                    diversity = std::min(minimum / result.radius, 1.0);
                }
                const double score = (0.50 * relevance[i] + 0.25 * novelty[i]) + 0.25 * diversity;
                // Consecutive B ranks make rank the decisive exact-score tie key.
                if (score > best_score) { best = i; best_score = score; best_diversity = diversity; }
            }
        }
        selected[best] = true;
        PatchSlideVariationRow row;
        static_cast<PatchSlideSearchResult&>(row) = pool[best];
        row.selected_rank = step + 1;
        row.relevance = result.radius == 0 ? 1 : relevance[best];
        row.novelty = result.radius == 0 ? 0 : novelty[best];
        row.diversity = best_diversity;
        if (result.radius != 0) row.score = best_score;
        result.candidates.push_back(row);
    }
    return result;
}

std::string_view PatchSlideDatabase::variationConfiguration() noexcept {
    return R"json({"distance_units":"frozen B squared Euclidean","diversity":"min(min_selected_candidate_pair_squared/radius,1); first selection0","mode":"V2","novelty":"exp(-((source_squared-linear_q50)/radius)^2)","novelty_target_quantile":0.5,"perceptual_claim":false,"pool_grid":[32,64,128],"pool_size":256,"radius":"median pool source squared distance, linear quantile","relevance":"exp(-source_squared/radius)","score":"weights dot [relevance,novelty,diversity], descending then original B rank then candidate_id","show_count":3,"version":"V2-pool256-variation-research-v3.1","weight_grid":[[0.5,0.25,0.25],[0.6,0.2,0.2],[0.4,0.3,0.3]],"weights":[0.5,0.25,0.25],"zero_radius":"relevance convention1, novelty0, diversity0; original B order; record positive-distance count; zero median does not prove whole pool identical; no novelty claim"})json";
}

std::string_view PatchSlideDatabase::variationConfigurationSha256() noexcept {
    return "d5a8a0a551dad3e8856f17de91896ed68790f351c283a452b4783a0dbf4572d6";
}

} // namespace mgstc::engine
