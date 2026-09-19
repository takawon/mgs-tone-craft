// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

class EngineCore;

inline constexpr std::uint8_t kCompositePsgTrackBase = 0;
inline constexpr std::uint8_t kCompositeSccTrackBase = 3;
inline constexpr std::uint8_t kCompositeOpllTrackBase = 8;
inline constexpr std::array<std::uint8_t, 3> kCompositeSourceTrackBases{
    kCompositePsgTrackBase,
    kCompositeSccTrackBase,
    kCompositeOpllTrackBase};
inline constexpr std::array<std::uint8_t, 3> kCompositeSourceChannelCounts{
    3, 5, 9};

struct CompositePlaybackLayer {
    std::size_t layer_index{};
    TimbreSource source{TimbreSource::Psg};
    std::uint8_t source_ordinal{};
    std::uint8_t authoring_channel{};
    std::int8_t relative_semitones{};
    double start_delay_ms{};
};

struct CompositePlaybackPlan {
    std::uint8_t voice_capacity{1};
    std::array<std::uint8_t, 3> audible_counts{};
    std::vector<CompositePlaybackLayer> audible_layers;

    [[nodiscard]] std::uint8_t physicalTrack(
        const CompositePlaybackLayer& layer,
        std::uint8_t voice) const noexcept;

    [[nodiscard]] std::uint8_t physicalTrack(
        std::size_t layer_index,
        std::uint8_t voice) const noexcept;

    [[nodiscard]] std::uint8_t authoringTrack(
        const CompositePlaybackLayer& layer) const noexcept;
};

struct CompositeProgramCompileOptions {
    bool polyphonic{false};
    const TimbreLibrary* library{nullptr};
};

[[nodiscard]] std::array<std::uint8_t, 3> audibleLayerCounts(
    const CompositeTimbre& timbre) noexcept;

[[nodiscard]] std::uint8_t compositeVoiceCapacity(
    const std::array<std::uint8_t, 3>& counts) noexcept;

[[nodiscard]] std::uint8_t physicalTrackForVoice(
    const CompositeTimbre& timbre,
    std::size_t layer_index,
    std::uint8_t voice,
    const std::array<std::uint8_t, 3>& counts) noexcept;

[[nodiscard]] std::uint8_t authoringTrackForLayer(
    const CompositeLayer& layer) noexcept;

[[nodiscard]] CompositePlaybackPlan buildCompositePlaybackPlan(
    const CompositeTimbre& timbre);

[[nodiscard]] RateEnvelopeDefinition rateEnvelopeDefinitionFrom(
    const RateEnvelope& rate) noexcept;

[[nodiscard]] bool compileCompositeProgram(
    EngineCore& engine,
    const CompositeTimbre& timbre,
    const CompositeProgramCompileOptions& options = {},
    CompositePlaybackPlan* plan_out = nullptr);

}  // namespace mgstc::engine
