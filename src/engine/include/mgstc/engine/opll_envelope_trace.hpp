#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "mgstc/engine/opll_patch.hpp"

namespace mgstc::engine {

struct OpllEnvelopeTrace {
    static constexpr std::size_t kPointCount = 512;
    static constexpr float kDurationSeconds = 4.0F;
    static constexpr float kKeyOffSeconds = 2.0F;

    std::array<float, kPointCount> modulator{};
    std::array<float, kPointCount> carrier{};
    std::uint8_t midi_note{60};
    bool valid{};
};

[[nodiscard]] OpllEnvelopeTrace traceOpllEnvelope(
    const OpllPatchParameters& patch,
    std::uint8_t midi_note) noexcept;

}  // namespace mgstc::engine
