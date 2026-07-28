#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"

namespace mgstc::engine {

struct WavePcm {
    std::uint32_t sample_rate{};
    std::vector<float> mono_samples;
};

struct WaveCycleAnalysis {
    std::vector<float> cycle;
    float estimated_frequency_hz{};
};

[[nodiscard]] bool parseWavePcm(
    std::span<const std::uint8_t> bytes,
    WavePcm& output,
    std::string* error = nullptr);

[[nodiscard]] WaveCycleAnalysis analyzeWaveCycle(
    const WavePcm& pcm);

[[nodiscard]] SccWaveform waveCycleToScc(
    std::span<const float> cycle) noexcept;

[[nodiscard]] OpllPatchParameters approximateWaveCycleWithOpll(
    std::span<const float> cycle) noexcept;

}  // namespace mgstc::engine
