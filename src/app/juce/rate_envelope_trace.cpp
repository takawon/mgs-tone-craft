#include "rate_envelope_trace.hpp"

namespace mgstc::app {

engine::RateEnvelopeTrace makeLayerRateTrace(
    const engine::CompositeLayer& layer) {
    const auto rate = engine::clampRateEnvelope(layer.volume_envelope.rate);
    return engine::traceRateEnvelope(
        engine::rateEnvelopeDefinitionFrom(rate),
        layer.volume,
        layer.source != engine::TimbreSource::Opll);
}

engine::RateEnvelopeTrace makeRateTrace(
    const engine::RateEnvelope& rate,
    std::uint8_t volume,
    engine::TimbreSource source) {
    return engine::traceRateEnvelope(
        engine::rateEnvelopeDefinitionFrom(engine::clampRateEnvelope(rate)),
        volume,
        source != engine::TimbreSource::Opll);
}

}  // namespace mgstc::app
