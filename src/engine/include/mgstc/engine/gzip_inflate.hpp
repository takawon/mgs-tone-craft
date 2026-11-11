#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mgstc::engine {

// Inflates a gzip member (RFC 1952) into the stored payload. Multi-member
// streams are not required; VGZ is a single gzip of a VGM file.
// Appends and reserve requests are limited to max_output_bytes; an excessive
// resulting capacity is rejected. With the supported Windows vector reserve
// implementation, growth uses at most twice the limit plus fixed decoder state.
// Failure never returns a partial payload.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> inflateGzip(
    std::span<const std::uint8_t> bytes,
    std::size_t max_output_bytes,
    std::string* error = nullptr);

[[nodiscard]] bool looksLikeGzip(std::span<const std::uint8_t> bytes) noexcept;

}  // namespace mgstc::engine
