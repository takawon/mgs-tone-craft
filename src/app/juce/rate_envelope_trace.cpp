#include "rate_envelope_trace.hpp"

namespace mgstc::app {

engine::RateEnvelopeDefinition rateDefinitionFrom(
    const engine::RateEnvelope& rate) {
    engine::RateEnvelopeDefinition definition;
    definition.attack_level = rate.attack_level;
    definition.attack_rate = rate.attack_rate;
    definition.decay_rate = rate.decay_rate;
    definition.sustain_level = rate.sustain_level;
    definition.sustain_rate = rate.sustain_rate;
    definition.release_rate = rate.release_rate;
    return definition;
}

engine::RateEnvelopeTrace makeLayerRateTrace(
    const engine::CompositeLayer& layer) {
    const auto rate = engine::clampRateEnvelope(layer.volume_envelope.rate);
    return engine::traceRateEnvelope(
        rateDefinitionFrom(rate),
        layer.volume,
        layer.source != engine::TimbreSource::Opll);
}

engine::RateEnvelopeTrace makeRateTrace(
    const engine::RateEnvelope& rate,
    std::uint8_t volume,
    engine::TimbreSource source) {
    return engine::traceRateEnvelope(
        rateDefinitionFrom(engine::clampRateEnvelope(rate)),
        volume,
        source != engine::TimbreSource::Opll);
}

}  // namespace mgstc::app
