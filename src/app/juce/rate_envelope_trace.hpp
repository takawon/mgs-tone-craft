#pragma once

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/envelope_rate.hpp"

namespace mgstc::app {

[[nodiscard]] engine::RateEnvelopeDefinition rateDefinitionFrom(
    const engine::RateEnvelope& rate);

[[nodiscard]] engine::RateEnvelopeTrace makeLayerRateTrace(
    const engine::CompositeLayer& layer);

[[nodiscard]] engine::RateEnvelopeTrace makeRateTrace(
    const engine::RateEnvelope& rate,
    std::uint8_t volume,
    engine::TimbreSource source);

}  // namespace mgstc::app
