// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

namespace mgstc::engine::detail {
struct WavRetainedScore {
    double total{};
    std::uint64_t candidate_id{};
};

// Offline only. The caller supplies distinct, already evaluated alternatives.
// Joint ordered assignment prevents a nearest +5% choice from consuming the
// only sensible +10% choice. Missing/exact target scores never cause exploration.
inline std::vector<std::size_t> selectWavScoreSpacedAlternatives(
    std::span<const WavRetainedScore> scores, double winnerScore) {
    if (!std::isfinite(winnerScore) || winnerScore < 0) return {};
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < scores.size(); ++i)
        if (std::isfinite(scores[i].total) && scores[i].total >= winnerScore) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return std::tie(scores[a].total, scores[a].candidate_id)
            < std::tie(scores[b].total, scores[b].candidate_id);
    });
    const auto count = std::min<std::size_t>(4, order.size());
    if (!count) return {};
    std::array<std::vector<double>, 5> cost;
    std::array<std::vector<bool>, 5> take;
    for (std::size_t k = 0; k <= count; ++k) {
        cost[k].assign(order.size() + 1, k ? std::numeric_limits<double>::infinity() : 0.0);
        take[k].assign(order.size() + 1, false);
    }
    for (std::size_t k = 1; k <= count; ++k) {
        const double target = winnerScore * (1.0 + .05 * static_cast<double>(k));
        for (std::size_t j = 1; j <= order.size(); ++j) {
            cost[k][j] = cost[k][j - 1];
            const double chosen = cost[k - 1][j - 1] + std::abs(scores[order[j - 1]].total - target);
            // On equal cost keep the earlier (lower score / stable ID) sequence.
            if (chosen < cost[k][j]) {
                cost[k][j] = chosen;
                take[k][j] = true;
            }
        }
    }
    if (!std::isfinite(cost[count][order.size()])) return {};
    std::vector<std::size_t> selected(count);
    for (std::size_t k = count, j = order.size(); k;) {
        if (take[k][j]) selected[--k] = order[--j];
        else --j;
    }
    return selected;
}

// Canonicalized completed keys can merge previously distinct evaluated tones.
// Filter the whole evaluated pool before assigning score targets, so a duplicate
// does not consume a slot when another evaluated alternative is available.
template<class IsWinner, class SameCandidate>
inline std::vector<std::size_t> selectDistinctWavScoreSpacedAlternatives(
    std::span<const WavRetainedScore> scores, double winnerScore,
    IsWinner isWinner, SameCandidate sameCandidate) {
    if (!std::isfinite(winnerScore) || winnerScore < 0) return {};
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < scores.size(); ++i)
        if (std::isfinite(scores[i].total) && scores[i].total >= winnerScore
            && !isWinner(i)) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return std::tie(scores[a].total, scores[a].candidate_id)
            < std::tie(scores[b].total, scores[b].candidate_id);
    });
    std::vector<std::size_t> unique;
    std::vector<WavRetainedScore> distinctScores;
    for (const auto index : order) {
        if (std::any_of(unique.begin(), unique.end(),
            [&](auto previous) { return sameCandidate(index, previous); })) continue;
        unique.push_back(index);
        distinctScores.push_back(scores[index]);
    }
    auto selected = selectWavScoreSpacedAlternatives(distinctScores, winnerScore);
    for (auto& index : selected) index = unique[index];
    return selected;
}
} // namespace mgstc::engine::detail
