#include "mgstc/engine/envelope_rate.hpp"

#include <algorithm>

namespace mgstc::engine {

RateEnvelopeRuntime::RateEnvelopeRuntime(
    RateEnvelopeDefinition definition,
    std::uint8_t track_volume) noexcept
    : definition_(definition),
      track_volume_(std::min<std::uint8_t>(track_volume, 15)) {
    resetForKeyOn();
}

void RateEnvelopeRuntime::resetForKeyOn() noexcept {
    level_ = definition_.attack_level;
    phase_ = RatePhase::Attack;
    tick_ = 0;
    key_off_pending_ = false;
}

void RateEnvelopeRuntime::keyOff(bool software_release) noexcept {
    if (software_release) {
        key_off_pending_ = true;
    }
}

void RateEnvelopeRuntime::setTrackVolume(
    std::uint8_t track_volume) noexcept {
    track_volume_ = std::min<std::uint8_t>(track_volume, 15);
}

MeaningEvent RateEnvelopeRuntime::processTick() noexcept {
    if (key_off_pending_ && phase_ == RatePhase::Sustain) {
        phase_ = RatePhase::Release;
        key_off_pending_ = false;
    }

    switch (phase_) {
    case RatePhase::Attack: {
        const auto next = static_cast<unsigned>(level_)
            + definition_.attack_rate;
        level_ = static_cast<std::uint8_t>(std::min(next, 255U));
        if (level_ == 255) {
            phase_ = RatePhase::Decay;
        }
        break;
    }
    case RatePhase::Decay: {
        const auto reduced = level_ > definition_.decay_rate
            ? static_cast<unsigned>(level_ - definition_.decay_rate)
            : 0U;
        level_ = static_cast<std::uint8_t>(std::max(
            reduced,
            static_cast<unsigned>(definition_.sustain_level)));
        if (level_ == definition_.sustain_level) {
            phase_ = RatePhase::Sustain;
        }
        break;
    }
    case RatePhase::Sustain:
        level_ = level_ > definition_.sustain_rate
            ? static_cast<std::uint8_t>(level_ - definition_.sustain_rate)
            : 0;
        break;
    case RatePhase::Release:
        level_ = level_ > definition_.release_rate
            ? static_cast<std::uint8_t>(level_ - definition_.release_rate)
            : 0;
        break;
    }

    const MeaningEvent event{
        tick_,
        MeaningEventKind::RateVolume,
        level_,
        quantizedVolume(),
    };
    ++tick_;
    return event;
}

std::uint8_t RateEnvelopeRuntime::quantizedVolume(
    std::uint8_t master_attenuation,
    std::uint8_t track_attenuation) const noexcept {
    const auto premaster = static_cast<std::uint8_t>(
        (static_cast<unsigned>(level_) * (track_volume_ + 1U)) >> 8U);
    return applyCommonAttenuation(
        premaster,
        master_attenuation,
        track_attenuation);
}

}  // namespace mgstc::engine
