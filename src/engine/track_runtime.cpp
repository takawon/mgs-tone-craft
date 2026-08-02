#include "mgstc/engine/track_runtime.hpp"

#include <algorithm>
#include <utility>

namespace mgstc::engine {

CompositeSequenceRuntime::CompositeSequenceRuntime(
    std::vector<std::uint8_t> volume_bytecode,
    std::vector<std::uint8_t> pitch_bytecode,
    std::vector<std::uint8_t> timbre_bytecode)
    : timbre_(std::move(timbre_bytecode), false),
      pitch_(std::move(pitch_bytecode), false),
      volume_(std::move(volume_bytecode)) {}

void CompositeSequenceRuntime::resetForKeyOn() noexcept {
    timbre_.resetForKeyOn();
    pitch_.resetForKeyOn();
    volume_.resetForKeyOn();
}

SequenceError CompositeSequenceRuntime::processTick(
    EventBuffer& output) {
    for (auto* runtime : {&timbre_, &pitch_, &volume_}) {
        const auto error = runtime->processTick(output);
        if (error != SequenceError::None) {
            return error;
        }
    }
    return SequenceError::None;
}

bool TrackRuntime::setSequenceEnvelope(
    std::vector<std::uint8_t> bytecode) {
    envelope_.emplace<SequenceEnvelopeRuntime>(std::move(bytecode));
    return true;
}

bool TrackRuntime::setCompositeSequenceEnvelopes(
    std::vector<std::uint8_t> volume_bytecode,
    std::vector<std::uint8_t> pitch_bytecode,
    std::vector<std::uint8_t> timbre_bytecode) {
    envelope_.emplace<CompositeSequenceRuntime>(
        std::move(volume_bytecode),
        std::move(pitch_bytecode),
        std::move(timbre_bytecode));
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
    } else if (auto* composite =
                   std::get_if<CompositeSequenceRuntime>(&envelope_)) {
        composite->resetForKeyOn();
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
    if (auto* composite =
            std::get_if<CompositeSequenceRuntime>(&envelope_)) {
        return composite->processTick(output);
    }
    if (auto* rate = std::get_if<RateEnvelopeRuntime>(&envelope_)) {
        if (!output.push(rate->processTick())) {
            return SequenceError::EventBufferOverflow;
        }
    }
    return SequenceError::None;
}

}  // namespace mgstc::engine
