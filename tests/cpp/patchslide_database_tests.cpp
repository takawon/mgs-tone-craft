// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_database.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace mgstc::engine;
namespace fs = std::filesystem;

void require(bool ok, std::string_view message) {
    if (!ok) throw std::runtime_error(std::string(message));
}

void requireNear(double actual, double expected, std::string_view context) {
    const double tolerance = 1e-12 + 1e-12 * std::abs(expected);
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(context) + ": numerical result differs");
    }
}

fs::path temporaryDirectory(std::string_view label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() / ("mgstc_patchslide_" + std::string(label) + "_" + std::to_string(nonce));
    require(fs::create_directory(path), "could not create isolated temporary directory");
    return path;
}

struct TempTree final {
    fs::path path;
    ~TempTree() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::uint16_t readU16(std::istream& in) {
    std::array<unsigned char, 2> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(in), "truncated fixture u16");
    return static_cast<std::uint16_t>(bytes[0] | (std::uint16_t(bytes[1]) << 8));
}

std::uint8_t readU8(std::istream& in) {
    const auto value = in.get();
    require(value != std::char_traits<char>::eof(), "truncated fixture u8");
    return static_cast<std::uint8_t>(value);
}

std::uint32_t readU32(std::istream& in) {
    std::array<unsigned char, 4> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(in), "truncated fixture u32");
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8)
        | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

double readF64(std::istream& in) {
    std::array<unsigned char, 8> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(in), "truncated fixture f64");
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) bits |= std::uint64_t(bytes[i]) << (8 * i);
    return std::bit_cast<double>(bits);
}

std::string readId(std::istream& in, std::size_t width) {
    std::string bytes(width, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(width));
    require(static_cast<bool>(in), "truncated fixture ID");
    const auto end = bytes.find('\0');
    require(end != std::string::npos && end != 0, "invalid fixture ID encoding");
    bytes.resize(end);
    return bytes;
}

void missingAndCorruptInputs() {
    TempTree missing{temporaryDirectory("missing")};
    std::string error;
    const auto absent = PatchSlideDatabase::load(missing.path, &error);
    require(!absent && !error.empty(), "missing database files must return a diagnostic");

    TempTree corrupt{temporaryDirectory("corrupt")};
    for (const auto* name : {"patchslide.sqlite", "patchslide_search.sqlite", "manifest.json"}) {
        std::ofstream file(corrupt.path / name, std::ios::binary);
        file << "not a valid database or manifest";
        require(static_cast<bool>(file), "could not create malformed-input fixture");
    }
    error.clear();
    const auto invalid = PatchSlideDatabase::load(corrupt.path, &error);
    require(!invalid && !error.empty(), "corrupt database files must be rejected with a diagnostic");
}

void standaloneValueBoundaries() {
    PatchSlideCandidate c;
    c.enabled = true;
    c.auto_safe = false;
    c.manual_use_allowed = false;
    c.visibility = "visible";
    c.predicted_partition = "AAAA";
    require(PatchSlideDatabase::isRejected(c), "unsafe candidate requires explicit review permission");
    require(!PatchSlideDatabase::isVisible(c, false), "rejected candidate is hidden by default");
    require(PatchSlideDatabase::isVisible(c, true), "show-rejected exposes an enabled rejected candidate");
    c.manual_use_allowed = true;
    require(!PatchSlideDatabase::isRejected(c), "manual permission permits a non-automatic candidate");
    for (const auto* pattern : {"AAAA", "ABBB"}) {
        c.predicted_partition = pattern;
        require(!PatchSlideDatabase::isRejected(c), "AAAA/ABBB remain visible without AUTO_SAFE");
    }
    for (const auto* pattern : {"ABAB", "AABA", "ABBA", "AAAB", "ABAA", "AABB", "ABCB", "", "unknown"}) {
        c.predicted_partition = pattern;
        require(PatchSlideDatabase::isRejected(c), "unstable/unknown pattern requires explicit Reject visibility");
        require(!PatchSlideDatabase::isVisible(c) && PatchSlideDatabase::isVisible(c, true),
            "Reject switch controls pattern-based rejection");
    }
    c.predicted_partition = "ABAB";
    c.auto_safe = true;
    require(PatchSlideDatabase::isRejected(c), "AUTO_SAFE cannot override a rejected pattern");
    c.human_pattern = "AAAA";
    require(!PatchSlideDatabase::isRejected(c), "human stable pattern overrides prediction");
    c.human_pattern = "ABBB";
    require(!PatchSlideDatabase::isRejected(c), "human initial-difference pattern remains visible");
    c.predicted_partition = "AAAA";
    c.human_pattern = "ABAB";
    require(PatchSlideDatabase::isRejected(c), "human rejected pattern overrides prediction and AUTO_SAFE");
    c.human_pattern.clear();
    require(!PatchSlideDatabase::isRejected(c), "empty human pattern falls back to prediction");
    c.visibility = "hidden";
    require(PatchSlideDatabase::isRejected(c), "explicitly hidden candidate remains rejected");
    c.enabled = false;
    require(!PatchSlideDatabase::isVisible(c, true), "show-rejected does not expose disabled entries");

    std::array<double, PatchSlideDatabase::vector_dimension> zero{};
    auto one = zero;
    one.back() = 1.0;
    require(PatchSlideDatabase::squaredDistance(zero, one) == 1.0, "768-value distance boundary");
    bool wrong_dimension_rejected = false;
    try {
        (void)PatchSlideDatabase::squaredDistance(std::span<const double>(zero).first(zero.size() - 1), one);
    } catch (const std::exception&) {
        wrong_dimension_rejected = true;
    }
    require(wrong_dimension_rejected, "distance rejects a short vector");

    std::vector<PatchSlideSearchResult> pool(256);
    std::vector<double> pairs(256 * 256, 0.0);
    for (std::size_t i = 0; i < pool.size(); ++i) {
        pool[i] = {i, i + 1, i < 129 ? 0.0 : 1.0};
    }
    const auto fallback = PatchSlideDatabase::selectVariation(pool, pairs);
    require(fallback.radius == 0.0 && fallback.zero_source_distances == 129
                && fallback.positive_source_distances == 127 && fallback.candidates.size() == 3,
            "zero median retains positive-distance diagnostics");
    for (std::size_t i = 0; i < 3; ++i) {
        require(fallback.candidates[i].candidate_index == i && fallback.candidates[i].rank == i + 1
                    && fallback.candidates[i].selected_rank == i + 1 && !fallback.candidates[i].score,
                "zero-radius fallback preserves the first three global ranks");
    }
    pool[1].candidate_index = pool[0].candidate_index;
    bool duplicate_rejected = false;
    try {
        (void)PatchSlideDatabase::selectVariation(pool, pairs);
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "Variation rejects duplicate candidate identities");
}

void validateCatalog(const PatchSlideDatabase& db) {
    const auto& candidates = db.candidates();
    const auto& tags = db.tags();
    const auto& clusters = db.clusters();
    const bool pruned = candidates.size() == PatchSlideDatabase::pruned_candidate_count;
    require(pruned || candidates.size() == PatchSlideDatabase::candidate_count, "candidate coverage");
    require(tags.size() == 94, "standard tag dictionary coverage");
    require(std::count_if(tags.begin(), tags.end(), [](const auto& tag) { return tag.candidate_count != 0; }) == 27,
            "used tag count");
    const auto edge_count = std::accumulate(tags.begin(), tags.end(), std::size_t{0},
        [](std::size_t sum, const auto& tag) { return sum + tag.candidate_count; });
    require(edge_count == (pruned ? 62654 : 71809), "candidate-tag edge count");
    require(std::count_if(candidates.begin(), candidates.end(), [](const auto& candidate) { return candidate.tags.empty(); }) == (pruned ? 26 : 28),
            "zero-tag candidate count");
    std::map<std::string, std::size_t> tag_order;
    for (std::size_t i = 0; i < tags.size(); ++i) tag_order.emplace(tags[i].tag, i);
    for (const auto& candidate : candidates) {
        require(!candidate.candidate_id.empty() && db.find(candidate.candidate_id).has_value(), "candidate identity lookup");
        std::size_t previous_tag_order = 0;
        bool first_tag = true;
        for (const auto& tag : candidate.tags) {
            const auto order = tag_order.find(tag);
            require(order != tag_order.end() && (first_tag || order->second > previous_tag_order),
                    "candidate tags use catalog order without duplicates");
            previous_tag_order = order->second;
            first_tag = false;
        }
        require(candidate.cluster16 >= 0 && candidate.cluster16 < 16
                    && candidate.cluster64 >= 0 && candidate.cluster64 < 64,
                "each candidate belongs to both independent partitions");
    }
    require(!db.find("patchslide-test-unknown-id"), "unknown candidate lookup");
    for (const auto& [source, top_n] : {
             std::pair<std::size_t, std::size_t>{candidates.size(), 1},
             std::pair<std::size_t, std::size_t>{0, 0},
             std::pair<std::size_t, std::size_t>{0, candidates.size()}}) {
        bool rejected = false;
        try {
            (void)db.standardB(source, top_n);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "Standard B rejects out-of-range source and result counts");
    }

    for (const int count : {16, 64}) {
        std::vector<std::size_t> assigned(static_cast<std::size_t>(count), 0);
        std::size_t members = 0;
        for (const auto& cluster : clusters) {
            if (cluster.cluster_count != count) continue;
            require(cluster.cluster_id >= 0 && cluster.cluster_id < count, "cluster ID range");
            require(cluster.representative_index < candidates.size(), "cluster representative exists");
            const auto& representative = candidates[cluster.representative_index];
            require((count == 16 ? representative.cluster16 : representative.cluster64) == cluster.cluster_id,
                    "cluster representative belongs to its own cluster");
            require(cluster.member_count > 0, "empty cluster");
            assigned[static_cast<std::size_t>(cluster.cluster_id)] = cluster.member_count;
            members += cluster.member_count;
        }
        require(members == candidates.size()
                    && std::all_of(assigned.begin(), assigned.end(), [](std::size_t n) { return n > 0; }),
                "cluster partitions cover every candidate exactly once");
        for (int cluster_id = 0; cluster_id < count; ++cluster_id) {
            PatchSlideFilter cluster_filter;
            cluster_filter.cluster_count = count;
            cluster_filter.cluster_id = cluster_id;
            cluster_filter.show_rejected = true;
            const auto actual = db.filter(cluster_filter);
            std::vector<std::size_t> expected;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const auto value = count == 16 ? candidates[i].cluster16 : candidates[i].cluster64;
                if (candidates[i].enabled && value == cluster_id) expected.push_back(i);
            }
            require(actual == expected, "cluster browsing returns the selected 16/64 partition members");
        }
    }

    const auto first = std::find_if(tags.begin(), tags.end(), [](const auto& tag) { return tag.candidate_count > 0; });
    require(first != tags.end(), "one active tag exists for filter checks");
    const auto second = std::find_if(std::next(first), tags.end(), [](const auto& tag) { return tag.candidate_count > 0; });
    require(second != tags.end(), "two active tags exist for filter checks");
    PatchSlideFilter filter;
    filter.tags = {first->tag, first->tag};
    filter.show_rejected = true;
    const auto single = db.filter(filter);
    filter.tags_match_all = false;
    const auto duplicate_or = db.filter(filter);
    require(single == duplicate_or, "duplicate selected tags are deduplicated for AND/OR");
    filter.tags = {first->tag, second->tag};
    filter.tags_match_all = true;
    const auto both = db.filter(filter);
    filter.tags_match_all = false;
    const auto either = db.filter(filter);
    auto expectedBoth = std::vector<std::size_t>{};
    auto expectedEither = std::vector<std::size_t>{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (!candidate.enabled) continue;
        const bool hasFirst = std::find(candidate.tags.begin(), candidate.tags.end(), first->tag) != candidate.tags.end();
        const bool hasSecond = std::find(candidate.tags.begin(), candidate.tags.end(), second->tag) != candidate.tags.end();
        if (hasFirst && hasSecond) expectedBoth.push_back(i);
        if (hasFirst || hasSecond) expectedEither.push_back(i);
    }
    require(both == expectedBoth && either == expectedEither, "tag AND/OR filtering follows candidate assignments");
    filter.tags = {"patchslide-test-unknown-tag"};
    require(db.filter(filter).empty(), "unknown tag does not silently broaden the selection");

    filter = {};
    const auto default_visible = db.filter(filter);
    filter.show_rejected = true;
    const auto show_rejected = db.filter(filter);
    std::vector<std::size_t> expectedDefault;
    std::vector<std::size_t> expectedAllEnabled;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].enabled) continue;
        expectedAllEnabled.push_back(i);
        if (!PatchSlideDatabase::isRejected(candidates[i])) expectedDefault.push_back(i);
    }
    require(default_visible == expectedDefault, "default search excludes rejected and disabled entries");
    require(show_rejected == expectedAllEnabled, "empty tag selection retains all enabled entries when requested");
    if (pruned) {
        require(default_visible.size() == 11974 && show_rejected.size() == 12180,
            "pruned catalog must hide exactly 206 unstable candidates by default");
        std::cout << "Reject OFF=11974, ON=12180 (206 rejected).\n";
    }
    filter.auto_safe_only = true;
    const auto safe_only = db.filter(filter);
    const auto expectedSafe = static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(),
        [](const auto& candidate) { return candidate.enabled && candidate.auto_safe; }));
    require(safe_only.size() == expectedSafe, "automatic-safety filtering has exact coverage");
}

std::size_t verifyFullRankings(const PatchSlideDatabase& db, const fs::path& fixture_dir) {
    const auto file_path = fixture_dir / "standard_b_572_full.bin";
    std::ifstream in(file_path, std::ios::binary);
    if (!in) throw std::runtime_error("572-query fixture is missing: " + file_path.string());
    std::array<char, 8> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    require(std::string_view(magic.data(), magic.size()) == "MGSTB572", "wrong full-ranking fixture signature");
    require(readU32(in) == 1, "unsupported full-ranking fixture version");
    const auto candidate_count = readU32(in);
    const auto query_count = readU32(in);
    const auto id_width = readU32(in);
    require(candidate_count == PatchSlideDatabase::candidate_count && query_count == 572 && id_width == 64,
            "full-ranking fixture dimensions");
    std::vector<std::string> ids;
    ids.reserve(candidate_count);
    for (std::uint32_t i = 0; i < candidate_count; ++i) ids.push_back(readId(in, id_width));
    require(std::is_sorted(ids.begin(), ids.end()) && std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
            "fixture candidate IDs are not a unique ordered set");
    const bool complete_database = ids.size() == db.candidates().size();
    if (complete_database) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            require(ids[i] == db.candidates()[i].candidate_id,
                    "fixture and complete database candidate order differs");
        }
    } else {
        // The pruned delivery retains a subset of the original candidate IDs.
        // The old frozen ranking remains an independent oracle: removing rows
        // preserves relative order. The reduced row/count can select a
        // different binary64 reduction path, so compare distance at the
        // dataset's documented 1e-12 absolute/relative tolerance.
        const auto profile_size = db.candidates().size();
        require(profile_size == 12180, "unrecognized PatchSlide candidate profile");
        std::map<std::string, std::size_t> original_index;
        for (std::size_t i = 0; i < ids.size(); ++i) original_index.emplace(ids[i], i);
        for (const auto& candidate : db.candidates())
            require(original_index.contains(candidate.candidate_id),
                    "pruned database contains an unknown candidate ID");
    }
    const std::size_t result_count = db.candidates().size() - 1;
    std::map<std::string, std::size_t> loaded_index;
    for (std::size_t i = 0; i < db.candidates().size(); ++i)
        loaded_index.emplace(db.candidates()[i].candidate_id, i);
    std::size_t compared_queries = 0;
    for (std::uint32_t q = 0; q < query_count; ++q) {
        const auto source_index = readU16(in);
        require(source_index < candidate_count, "query source index outside candidate table");
        const auto source = loaded_index.find(ids[source_index]);
        if (!complete_database && source == loaded_index.end()) {
            // A removed candidate cannot be queried in the pruned index.
            for (std::size_t rank = 0; rank < candidate_count - 1; ++rank) {
                static_cast<void>(readU16(in));
                static_cast<void>(readF64(in));
            }
            continue;
        }
        ++compared_queries;
        const auto actual = db.standardB(complete_database ? source_index : source->second, result_count);
        require(actual.size() == result_count, "full Standard B result count");
        std::size_t retained_rank = 0;
        for (std::size_t rank = 0; rank < candidate_count - 1; ++rank) {
            const auto expected_index = readU16(in);
            const auto expected_distance = readF64(in);
            const auto expected_id = ids[expected_index];
            const auto retained = loaded_index.find(expected_id);
            if (!complete_database && retained == loaded_index.end()) continue;
            const auto expected_loaded_index = complete_database ? expected_index : retained->second;
            const auto& row = actual[retained_rank];
            const auto expected_retained_rank = ++retained_rank;
            const bool ranking_mismatch = row.candidate_index != expected_loaded_index
                || row.rank != expected_retained_rank;
            const bool distance_mismatch = complete_database
                ? row.squared_distance != expected_distance
                : !std::isfinite(row.squared_distance)
                    || std::abs(row.squared_distance - expected_distance)
                        > 1e-12 + 1e-12 * std::abs(expected_distance);
            if (ranking_mismatch || distance_mismatch) {
                std::ostringstream message;
                message << "Standard B "
                        << (ranking_mismatch && distance_mismatch ? "ranking and distance mismatch"
                            : ranking_mismatch ? "ranking mismatch" : "distance mismatch")
                        << " at query " << (q + 1) << " (" << ids[source_index] << "), rank " << (rank + 1)
                        << "; actual_id=" << (row.candidate_index < db.candidates().size()
                            ? db.candidates()[row.candidate_index].candidate_id : "<out-of-range>")
                        << "; expected_id=" << expected_id
                        << "; actual_rank=" << row.rank << "; expected_rank=" << expected_retained_rank
                        << std::setprecision(17) << std::defaultfloat
                        << "; actual_distance=" << row.squared_distance
                        << "; expected_distance=" << expected_distance
                        << std::hexfloat
                        << "; actual_distance_hex=" << row.squared_distance
                        << "; expected_distance_hex=" << expected_distance;
                throw std::runtime_error(message.str());
            }
        }
        require(retained_rank == result_count, "projected ranking retains every database candidate");
    }
    if (!complete_database)
        require(compared_queries > 0, "pruned ranking fixture has no retained query sources");
    require(in.peek() == std::char_traits<char>::eof(), "trailing bytes in full-ranking fixture");
    return compared_queries;
}

void verifyVariations(const PatchSlideDatabase* db, const fs::path& fixture_dir) {
    const auto file_path = fixture_dir / "variation_v2_golden.bin";
    std::ifstream in(file_path, std::ios::binary);
    if (!in) throw std::runtime_error("Variation golden fixture is missing: " + file_path.string());
    std::ifstream vectors(fixture_dir / "variation_vectors.f64le", std::ios::binary);
    std::ifstream pairs_file(fixture_dir / "variation_pairs.f64le", std::ios::binary);
    require(static_cast<bool>(vectors) && static_cast<bool>(pairs_file), "Variation numeric fixtures are missing");
    std::array<char, 8> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    require(std::string_view(magic.data(), magic.size()) == "MGSTVAR2", "wrong Variation fixture signature");
    require(readU32(in) == 1 && readU32(in) == 6, "Variation fixture version or case count");
    require(PatchSlideDatabase::variationConfigurationSha256() == "d5a8a0a551dad3e8856f17de91896ed68790f351c283a452b4783a0dbf4572d6",
            "Variation configuration differs from the frozen mode");
    struct ExpectedRow {
        std::string id;
        std::size_t selected_rank = 0;
        std::size_t base_rank = 0;
        double source = 0.0;
        double relevance = 0.0;
        double novelty = 0.0;
        double diversity = 0.0;
        double score = 0.0;
        bool has_score = false;
    };
    for (int case_index = 0; case_index < 6; ++case_index) {
        const auto query_id = readId(in, 64);
        const double expected_radius = readF64(in);
        const double expected_q10 = readF64(in);
        const double expected_q50 = readF64(in);
        const auto zero_count = readU16(in);
        const auto positive_count = readU16(in);
        std::vector<PatchSlideSearchResult> pool(256);
        std::vector<std::string> pool_ids;
        pool_ids.reserve(256);
        for (std::size_t i = 0; i < pool.size(); ++i) {
            pool_ids.push_back(readId(in, 64));
            pool[i] = {i, readU16(in), readF64(in)};
        }
        std::array<ExpectedRow, 3> expected_rows;
        for (std::size_t i = 0; i < 3; ++i) {
            auto& expected = expected_rows[i];
            expected.id = readId(in, 64);
            expected.selected_rank = readU16(in);
            expected.base_rank = readU16(in);
            expected.source = readF64(in);
            expected.relevance = readF64(in);
            expected.novelty = readF64(in);
            expected.diversity = readF64(in);
            expected.score = readF64(in);
            expected.has_score = readU8(in) != 0;
        }

        constexpr std::size_t dimension = PatchSlideDatabase::vector_dimension;
        std::vector<double> matrix((pool.size() + 1) * dimension);
        for (double& value : matrix) value = readF64(vectors);
        std::vector<double> pair_squared(pool.size() * pool.size());
        for (double& value : pair_squared) value = readF64(pairs_file);
        const std::span<const double> source_vector(matrix.data(), dimension);
        for (std::size_t i = 0; i < pool.size(); ++i) {
            const std::span<const double> target(matrix.data() + (i + 1) * dimension, dimension);
            requireNear(PatchSlideDatabase::squaredDistance(source_vector, target), pool[i].squared_distance,
                        "Variation pool source distance");
            for (std::size_t j = 0; j < pool.size(); ++j) {
                const std::span<const double> other(matrix.data() + (j + 1) * dimension, dimension);
                requireNear(PatchSlideDatabase::squaredDistance(target, other), pair_squared[i * pool.size() + j],
                            "Variation pair distance");
            }
        }
        const auto result = PatchSlideDatabase::selectVariation(pool, pair_squared);
        require(result.candidates.size() == 3 && result.zero_source_distances == zero_count
                    && result.positive_source_distances == positive_count,
                "Variation result cardinality and source-distance diagnostics");
        requireNear(result.radius, expected_radius, "Variation radius");
        requireNear(result.q10, expected_q10, "Variation q10");
        requireNear(result.q50, expected_q50, "Variation q50");
        for (std::size_t i = 0; i < expected_rows.size(); ++i) {
            const auto& expected = expected_rows[i];
            const auto& actual = result.candidates[i];
            require(actual.candidate_index < pool_ids.size() && pool_ids[actual.candidate_index] == expected.id
                        && actual.selected_rank == expected.selected_rank && actual.rank == expected.base_rank,
                    "Variation selected IDs and order match the golden result");
            requireNear(actual.squared_distance, expected.source, "Variation source distance");
            requireNear(actual.relevance, expected.relevance, "Variation relevance");
            requireNear(actual.novelty, expected.novelty, "Variation novelty");
            requireNear(actual.diversity, expected.diversity, "Variation diversity");
            require(actual.score.has_value() == expected.has_score, "Variation score nullability");
            if (expected.has_score) requireNear(*actual.score, expected.score, "Variation score");
        }

        if (db) {
            const auto source = db->find(query_id);
            require(source.has_value(), "Variation golden source candidate exists");
            const auto integrated = db->variation(*source);
            require(integrated.candidates.size() == expected_rows.size()
                        && integrated.zero_source_distances == zero_count
                        && integrated.positive_source_distances == positive_count,
                    "database Variation result cardinality and source diagnostics");
            requireNear(integrated.radius, expected_radius, "database Variation radius");
            requireNear(integrated.q10, expected_q10, "database Variation q10");
            requireNear(integrated.q50, expected_q50, "database Variation q50");
            for (std::size_t i = 0; i < expected_rows.size(); ++i) {
                const auto& expected = expected_rows[i];
                const auto& actual = integrated.candidates[i];
                require(db->candidates()[actual.candidate_index].candidate_id == expected.id
                            && actual.selected_rank == expected.selected_rank && actual.rank == expected.base_rank,
                        "database Variation IDs and order match the golden result");
                requireNear(actual.squared_distance, expected.source, "database Variation source distance");
                requireNear(actual.relevance, expected.relevance, "database Variation relevance");
                requireNear(actual.novelty, expected.novelty, "database Variation novelty");
                requireNear(actual.diversity, expected.diversity, "database Variation diversity");
                require(actual.score.has_value() == expected.has_score, "database Variation score nullability");
                if (expected.has_score) requireNear(*actual.score, expected.score, "database Variation score");
            }
        }
    }
    require(in.peek() == std::char_traits<char>::eof(), "trailing bytes in Variation fixture");
    require(vectors.peek() == std::char_traits<char>::eof() && pairs_file.peek() == std::char_traits<char>::eof(),
            "trailing bytes in Variation numeric fixtures");
}

void verifyManifestBinding(const fs::path& source_dir) {
    TempTree temp{temporaryDirectory("manifest")};
    for (const auto* name : {"patchslide.sqlite", "patchslide_search.sqlite"}) {
        std::error_code error;
        fs::create_hard_link(source_dir / name, temp.path / name, error);
        require(!error, "could not create read-only database hard link for manifest mismatch test");
    }
    std::ifstream source_manifest(source_dir / "manifest.json", std::ios::binary);
    require(static_cast<bool>(source_manifest), "loaded database manifest is readable");
    std::string manifest((std::istreambuf_iterator<char>(source_manifest)), std::istreambuf_iterator<char>());
    const std::string key = "\"source_db_sha256\": \"";
    const auto value = manifest.find(key);
    require(value != std::string::npos, "manifest contains source database identity");
    const auto hash_start = value + key.size();
    require(hash_start + 64 <= manifest.size(), "manifest source hash is complete");
    manifest.replace(hash_start, 64, 64, '0');
    std::ofstream target_manifest(temp.path / "manifest.json", std::ios::binary);
    target_manifest << manifest;
    target_manifest.close();
    std::string error;
    const auto rejected = PatchSlideDatabase::load(temp.path, &error);
    require(!rejected && !error.empty(), "manifest/source mismatch is rejected");
}

fs::path fixtureDirectory() {
    if (const auto* value = std::getenv("MGSTC_PATCHSLIDE_TEST_FIXTURE_DIR")) return fs::path(value);
#ifdef MGSTC_SOURCE_DIR
    return fs::path(MGSTC_SOURCE_DIR) / "tests" / "fixtures" / "patchslide";
#else
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / "tests" / "fixtures" / "patchslide";
#endif
}

fs::path databaseDirectory(int argc, char** argv) {
    if (argc > 1) return fs::path(argv[1]);
    if (const auto* value = std::getenv("MGSTC_PATCHSLIDE_TEST_DB_DIR")) return fs::path(value);
    return PatchSlideDatabase::defaultDirectory();
}

bool optionalDeploymentAbsent(const fs::path& directory, bool explicitly_selected) {
    if (explicitly_selected) return false;
    for (const auto* name : {"patchslide.sqlite", "patchslide_search.sqlite", "manifest.json"})
        if (fs::exists(directory / name)) return false;
    return true;
}

void deploymentSkipBoundaries() {
    TempTree temp{temporaryDirectory("skip_policy")};
    const auto absent = temp.path / "not_installed";
    require(optionalDeploymentAbsent(absent, false), "uninstalled optional deployment may skip");
    require(!optionalDeploymentAbsent(absent, true), "explicit missing deployment must fail");
    for (const auto* name : {"patchslide.sqlite", "patchslide_search.sqlite", "manifest.json"}) {
        const auto file = temp.path / name;
        std::ofstream(file, std::ios::binary) << "invalid deployment";
        require(!optionalDeploymentAbsent(temp.path, false), "a partial or malformed deployment must not skip");
        std::string error;
        require(!PatchSlideDatabase::load(temp.path, &error), "partial or malformed deployment unexpectedly loaded");
        require(fs::remove(file), "could not remove isolated partial deployment");
    }
    require(optionalDeploymentAbsent(temp.path, false), "empty optional deployment directory may skip");
}
} // namespace

int main(int argc, char** argv) {
    try {
        missingAndCorruptInputs();
        standaloneValueBoundaries();
        deploymentSkipBoundaries();
        const auto fixture_dir = fixtureDirectory();
        std::string error;
        const auto directory = databaseDirectory(argc, argv);
        const bool explicitly_selected = argc > 1 || std::getenv("MGSTC_PATCHSLIDE_TEST_DB_DIR") != nullptr;
        // Classify before loading: an installed input that disappears during
        // verification must fail rather than become an optional-assets skip.
        const bool may_skip = optionalDeploymentAbsent(directory, explicitly_selected);
        const auto db = PatchSlideDatabase::load(directory, &error);
        const bool have_full_fixture = fs::is_regular_file(fixture_dir / "standard_b_572_full.bin");
        const auto variation_fixture_dir = db && db->candidates().size() == 12180
            ? fixture_dir / "pruned_v1" : fixture_dir;
        const bool have_variation_fixture = fs::is_regular_file(variation_fixture_dir / "variation_v2_golden.bin")
            && fs::is_regular_file(variation_fixture_dir / "variation_vectors.f64le")
            && fs::is_regular_file(variation_fixture_dir / "variation_pairs.f64le");
        if (!db) {
            require(may_skip, "configured or installed PatchSlide database failed to load: " + error);
            if (have_variation_fixture) {
                verifyVariations(nullptr, fixture_dir);
                std::cout << "PatchSlide Variation fixture checks passed (6/6). ";
            } else {
                std::cout << "PatchSlide Variation fixture checks skipped (local fixture absent). ";
            }
            std::cout << "Database integration skipped (full 572-query ranking regression not run): "
                      << error << '\n';
            return 0;
        }
        validateCatalog(*db);
        std::size_t compared_queries = 0;
        if (have_full_fixture) compared_queries = verifyFullRankings(*db, fixture_dir);
        else std::cout << "PatchSlide full 572-query ranking regression skipped (local fixture absent).\n";
        if (have_variation_fixture) verifyVariations(db.get(), variation_fixture_dir);
        else std::cout << "PatchSlide six-case Variation regression skipped (local fixture absent).\n";
        verifyManifestBinding(directory);
        std::cout << "PatchSlide database catalog/filter/manifest checks passed";
        if (have_full_fixture) {
            if (db->candidates().size() == PatchSlideDatabase::candidate_count)
                std::cout << "; 572/572 full rankings passed";
            else
                std::cout << "; projected frozen rankings passed for "
                          << compared_queries << "/572 retained query sources";
        }
        if (have_variation_fixture) std::cout << "; Variation fixture checks passed (6/6)";
        std::cout << ".\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PatchSlide database test failure: " << error.what() << '\n';
        return 1;
    }
}
