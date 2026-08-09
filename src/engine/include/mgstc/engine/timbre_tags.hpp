#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mgstc::engine {

struct TimbreTagUsage {
    std::string name;
    std::size_t count{};

    friend bool operator==(const TimbreTagUsage&, const TimbreTagUsage&)
        = default;
};

[[nodiscard]] std::span<const std::string_view>
presetTimbreTags() noexcept;

[[nodiscard]] bool isPresetTimbreTag(
    std::string_view tag);

[[nodiscard]] bool rewriteTimbreTag(
    std::vector<std::string>& tags,
    std::string_view source,
    std::string_view replacement);

[[nodiscard]] std::vector<std::string> parseTimbreTags(
    std::string_view serialized);

[[nodiscard]] std::string serializeTimbreTags(
    std::span<const std::string> tags);

[[nodiscard]] std::string normalizeTimbreTags(
    std::string_view serialized);

[[nodiscard]] bool containsAllTimbreTags(
    std::string_view serialized,
    std::span<const std::string> required_tags);

[[nodiscard]] bool containsAllTimbreTags(
    std::span<const std::string> available_tags,
    std::span<const std::string> required_tags);

[[nodiscard]] std::vector<TimbreTagUsage> collectTimbreTagUsage(
    std::span<const std::string> serialized_tag_sets);

[[nodiscard]] std::vector<TimbreTagUsage> collectTimbreTagUsage(
    std::span<const std::vector<std::string>> tag_sets);

}  // namespace mgstc::engine
