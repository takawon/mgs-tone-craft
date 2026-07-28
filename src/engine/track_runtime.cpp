#include "mgstc/engine/track_runtime.hpp"

#include <algorithm>
#include <utility>

namespace mgstc::engine {

bool TrackRuntime::setSequenceEnvelope(
    std::vector<std::uint8_t> bytecode) {
    envelope_.emplace<SequenceEnvelopeRuntime>(std::move(bytecode));
    return true;
}

bool TrackRuntime::setRateEnvelope(
    RateEnvelopeDefinition definition,
    std::uint8_t track_volume) {
    track_volume_ = std::min<std::uint8_t>(track_volume, 15);
    envelope_.emplace<RateEnvelopeRuntime>(definition, track_volume_);
    return true;
}

void TrackRuntime::clear() noexcept {
    envelope_.emplace<std::monostate>();
}

void TrackRuntime::resetForKeyOn() noexcept {
    if (auto* sequence = std::get_if<SequenceEnvelopeRuntime>(&envelope_)) {
        sequence->resetForKeyOn();
    } else if (auto* rate = std::get_if<RateEnvelopeRuntime>(&envelope_)) {
        rate->resetForKeyOn();
    }
}

void TrackRuntime::keyOff(bool software_release) noexcept {
    if (auto* rate = std::get_if<RateEnvelopeRuntime>(&envelope_)) {
        rate->keyOff(software_release);
    }
}

void TrackRuntime::setTrackVolume(std::uint8_t track_volume) noexcept {
    track_volume_ = std::min<std::uint8_t>(track_volume, 15);
    if (auto* rate = std::get_if<RateEnvelopeRuntime>(&envelope_)) {
        rate->setTrackVolume(track_volume_);
    }
}

SequenceError TrackRuntime::processTick(EventBuffer& output) {
    if (auto* sequence = std::get_if<SequenceEnvelopeRuntime>(&envelope_)) {
        return sequence->processTick(output);
    }
    if (auto* rate = std::get_if<RateEnvelopeRuntime>(&envelope_)) {
        if (!output.push(rate->processTick())) {
            return SequenceError::EventBufferOverflow;
        }
    }
    return SequenceError::None;
}

}  // namespace mgstc::engine
