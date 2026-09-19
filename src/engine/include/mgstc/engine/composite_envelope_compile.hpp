// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

struct CompositeEnvelopePrograms {
    std::vector<std::uint8_t> volume;
    std::vector<std::uint8_t> pitch;
    std::vector<std::uint8_t> timbre;
};

enum class CompositeEnvelopeLane : std::uint8_t {
    Volume,
    Pitch,
    Timbre,
};

[[nodiscard]] std::vector<std::uint8_t> compileCompositeEnvelopeLane(
    const CompositeLayer& layer,
    CompositeEnvelopeLane lane,
    const TimbreNumberResolution& numbers,
    const TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true,
    bool include_loop = true);

[[nodiscard]] CompositeEnvelopePrograms compileCompositeEnvelopes(
    const CompositeLayer& layer,
    const TimbreNumberResolution& numbers,
    const TimbreLibrary* library = nullptr,
    bool include_original_tone_y = true,
    bool expand_tl_auto = true,
    bool expand_fb_auto = true);

[[nodiscard]] std::vector<std::uint8_t> sampleCompositeVolumeLane(
    const CompositeLayer& layer,
    int ticks);

}  // namespace mgstc::engine
