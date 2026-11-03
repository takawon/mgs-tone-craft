// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "mgstc/engine/composite_timbre.hpp"

namespace mgstc::engine {

inline constexpr std::size_t kPatchSlideFingerprintDimension = 436;
inline constexpr std::size_t kPatchSlideSearchVectorDimension = 768;
inline constexpr std::uint32_t kPatchSlideFingerprintSampleRate = 192'000;
using PatchSlideFingerprint = std::array<float, kPatchSlideFingerprintDimension>;
using PatchSlideSearchVector = std::array<double, kPatchSlideSearchVectorDimension>;

enum class PatchSlideFeatureError : std::uint8_t {
    None,
    InvalidInput,
    InvalidPitch,
};

// A region contains only samples from one key-on through key-off. The four
// regions represent four consecutive same-pitch notes rendered without resetting
// the OPLL between notes. Input is signed 16-bit mono PCM at 192 kHz.
[[nodiscard]] PatchSlideFeatureError extractPatchSlideFingerprint(
    std::span<const std::int16_t> key_on_pcm,
    double fundamental_hz,
    PatchSlideFingerprint& output,
    const std::function<bool()>& cancelled = {});

// Applies the frozen Standard B input quantization and four-note acoustic shape
// aggregation to four ordered note fingerprints. This does not perform a DB read.
[[nodiscard]] PatchSlideFeatureError makePatchSlideSearchVector(
    const std::array<PatchSlideFingerprint, 4>& fingerprints,
    PatchSlideSearchVector& output);

// Renders the already isolated single OPLL layer through the search-only
// 192 kHz YM2413 path, then extracts and aggregates its four-note query.
// Worker-thread only. Cancellation is checked between ticks and FFT frames.
[[nodiscard]] std::optional<PatchSlideSearchVector> makePatchSlideSearchVector(
    const CompositeTimbre& isolated,
    std::string* error = nullptr,
    std::function<bool()> cancelled = {});

} // namespace mgstc::engine
