#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mgstc::engine {

// Inflates a gzip member (RFC 1952) into the stored payload. Multi-member
// streams are not required; VGZ is a single gzip of a VGM file.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> inflateGzip(
    std::span<const std::uint8_t> bytes,
    std::string* error = nullptr);

[[nodiscard]] bool looksLikeGzip(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace mgstc::engine
