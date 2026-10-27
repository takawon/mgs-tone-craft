#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mgstc::engine {

struct PatchSlideCandidate {
    std::string candidate_id;
    int candidate_no = 0;
    int patch_a = 0;
    int patch_b = 0;
    std::optional<int> patch_c;
    int change_count = 1;
    std::string hold_pattern;
    int hold_a_steps = 1;
    std::optional<int> hold_b_steps;
    bool auto_safe = false;
    bool manual_use_allowed = false;
    bool enabled = false;
    std::string visibility;
    std::string predicted_partition;
    std::string human_pattern; // Empty means SQL NULL; never inferred.
    int raw_different_count = 0;
    std::vector<std::string> tags;
    int cluster16 = -1;
    int cluster64 = -1;
};

struct PatchSlideTag {
    std::string tag;
    std::string status;
    std::size_t candidate_count = 0;
};

struct PatchSlideCluster {
    int cluster_count = 16;
    int cluster_id = 0;
    std::size_t member_count = 0;
    std::size_t representative_index = 0;
};

struct PatchSlideFilter {
    std::vector<std::string> tags;
    bool tags_match_all = true;
    int cluster_count = 0; // 0: no classification filter; otherwise 16 or 64.
    int cluster_id = -1;
    bool show_rejected = false;
    bool auto_safe_only = false;
    // One-change only (1), or both supported sequence lengths (2).
    int max_change_count = 2;
};

struct PatchSlideSearchResult {
    std::size_t candidate_index = 0;
    std::size_t rank = 0;
    double squared_distance = 0.0;
};

struct PatchSlideVariationRow : PatchSlideSearchResult {
    std::size_t selected_rank = 0;
    double relevance = 0.0;
    double novelty = 0.0;
    double diversity = 0.0;
    std::optional<double> score;
};

struct PatchSlideVariationResult {
    std::vector<PatchSlideVariationRow> candidates;
    double radius = 0.0;
    double q10 = 0.0;
    double q50 = 0.0;
    std::size_t zero_source_distances = 0;
    std::size_t positive_source_distances = 0;
};

// Loading, filtering and searches belong on a non-audio worker. Immutable metadata
// accessors are safe on the UI thread. After loading SQLite handles are closed;
// concurrent queries need no shared mutable state. Never use this API in audio.
class PatchSlideDatabase final {
public:
    // Verified delivery sizes. Use candidates().size() for the loaded data set.
    static constexpr std::size_t candidate_count = 13950;
    static constexpr std::size_t pruned_candidate_count = 12180;
    static constexpr std::size_t vector_dimension = 768;
    [[nodiscard]] static std::filesystem::path defaultDirectory();
    // Instances in one process share an immutable data set by canonical directory.
    // Errors include missing filenames and the deployment directory. No dialogs.
    [[nodiscard]] static std::shared_ptr<const PatchSlideDatabase> load(
        const std::filesystem::path& directory, std::string* error = nullptr);
    [[nodiscard]] static std::shared_ptr<const PatchSlideDatabase> load(
        std::string* error = nullptr);

    [[nodiscard]] const std::vector<PatchSlideCandidate>& candidates() const noexcept;
    [[nodiscard]] const std::vector<PatchSlideTag>& tags() const noexcept;
    [[nodiscard]] const std::vector<PatchSlideCluster>& clusters() const noexcept;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view candidate_id) const;
    [[nodiscard]] static bool isRejected(const PatchSlideCandidate& candidate) noexcept;
    [[nodiscard]] static bool isVisible(const PatchSlideCandidate& candidate,
                                        bool show_rejected = false) noexcept;
    [[nodiscard]] std::vector<std::size_t> filter(const PatchSlideFilter& filter) const;
    [[nodiscard]] std::vector<PatchSlideSearchResult> standardB(
        std::size_t source_index, std::size_t top_n) const;
    [[nodiscard]] PatchSlideVariationResult variation(std::size_t source_index) const;
    // Restrict sequence lengths before choosing the eligible Standard B pool256.
    // The score expression and tie ordering are identical to the unfiltered mode.
    [[nodiscard]] PatchSlideVariationResult variation(
        std::size_t source_index, int max_change_count) const;
    // Portable fixture seam: exactly the same binary64 expression used in search.
    [[nodiscard]] static double squaredDistance(std::span<const double> query,
                                                 std::span<const double> target);
    [[nodiscard]] static PatchSlideVariationResult selectVariation(
        std::span<const PatchSlideSearchResult> pool, std::span<const double> pair_squared);
    [[nodiscard]] static std::string_view variationConfiguration() noexcept;
    [[nodiscard]] static std::string_view variationConfigurationSha256() noexcept;

private:
    PatchSlideDatabase() = default;
    std::vector<PatchSlideCandidate> candidates_;
    std::vector<PatchSlideTag> tags_;
    std::vector<PatchSlideCluster> clusters_;
    std::vector<double> vectors_;
    std::vector<double> norms_;
    [[nodiscard]] PatchSlideVariationResult variationFromPool(
        std::span<const PatchSlideSearchResult> pool) const;
    void read(const std::filesystem::path& directory);
};

} // namespace mgstc::engine
