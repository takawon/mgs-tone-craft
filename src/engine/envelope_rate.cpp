#include "mgstc/engine/envelope_rate.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

RateEnvelopeTrace traceRateEnvelope(
    RateEnvelopeDefinition definition,
    std::uint8_t track_volume,
    bool software_release) noexcept {
    RateEnvelopeTrace trace{};
    RateEnvelopeRuntime runtime(definition, track_volume);
    constexpr int kTicksPerSecond = 60;
    constexpr int total_ticks = static_cast<int>(
        RateEnvelopeTrace::kDurationSeconds * kTicksPerSecond);
    constexpr int key_off_tick = static_cast<int>(
        RateEnvelopeTrace::kKeyOffSeconds * kTicksPerSecond);
    std::array<std::uint8_t, total_ticks> levels{};
    std::array<std::uint8_t, total_ticks> volumes{};
    for (int tick = 0; tick < total_ticks; ++tick) {
        if (tick == key_off_tick) {
            runtime.keyOff(software_release);
        }
        const auto event = runtime.processTick();
        levels[static_cast<std::size_t>(tick)] =
            static_cast<std::uint8_t>(event.arg0);
        volumes[static_cast<std::size_t>(tick)] =
            static_cast<std::uint8_t>(event.arg1);
    }
    const float last = static_cast<float>(
        RateEnvelopeTrace::kPointCount - 1);
    for (std::size_t index = 0; index < RateEnvelopeTrace::kPointCount;
         ++index) {
        const float seconds = last <= 0.0F
            ? 0.0F
            : (static_cast<float>(index) / last)
                * RateEnvelopeTrace::kDurationSeconds;
        const int tick = std::clamp(
            static_cast<int>(std::floor(seconds * kTicksPerSecond)),
            0,
            total_ticks - 1);
        trace.level[index] =
            static_cast<float>(levels[static_cast<std::size_t>(tick)])
            / 255.0F;
        trace.quantized[index] =
            static_cast<float>(volumes[static_cast<std::size_t>(tick)])
            / 15.0F;
    }
    trace.valid = true;
    return trace;
}

namespace {

constexpr int kRateTicksPerSecond = 60;
constexpr int kRatePreviewTicks = static_cast<int>(
    RateEnvelopeTrace::kDurationSeconds * kRateTicksPerSecond);
constexpr int kRateKeyOffTick = static_cast<int>(
    RateEnvelopeTrace::kKeyOffSeconds * kRateTicksPerSecond);

[[nodiscard]] std::uint8_t ceilRate(
    unsigned delta,
    int ticks) noexcept {
    if (ticks <= 0 || delta == 0) {
        return 0;
    }
    const auto value =
        (delta + static_cast<unsigned>(ticks) - 1U)
        / static_cast<unsigned>(ticks);
    return static_cast<std::uint8_t>(std::min(255U, value));
}

[[nodiscard]] int secondsToTick(float seconds) noexcept {
    return std::clamp(
        static_cast<int>(std::lround(
            seconds * static_cast<float>(kRateTicksPerSecond))),
        0,
        kRatePreviewTicks - 1);
}

[[nodiscard]] float tickToSeconds(int tick) noexcept {
    return static_cast<float>(tick)
        / static_cast<float>(kRateTicksPerSecond);
}

struct RateEnvelopeAnalysis {
    int peak_tick{-1};
    int decay_end_tick{-1};
    std::uint8_t level_before_key_off{};
    int release_zero_tick{-1};
};

[[nodiscard]] RateEnvelopeAnalysis analyzeRateEnvelope(
    RateEnvelopeDefinition definition,
    bool software_release) noexcept {
    RateEnvelopeAnalysis result;
    result.level_before_key_off = definition.attack_level;
    RateEnvelopeRuntime runtime(definition, 15);
    for (int tick = 0; tick < kRatePreviewTicks; ++tick) {
        if (tick == kRateKeyOffTick) {
            runtime.keyOff(software_release);
        }
        const auto event = runtime.processTick();
        const auto level = static_cast<std::uint8_t>(event.arg0);
        if (tick < kRateKeyOffTick) {
            result.level_before_key_off = level;
        }
        if (result.peak_tick < 0 && level == 255) {
            result.peak_tick = tick;
        }
        if (result.decay_end_tick < 0
            && runtime.phase() == RatePhase::Sustain) {
            result.decay_end_tick = tick;
        }
        if (software_release
            && tick >= kRateKeyOffTick
            && result.release_zero_tick < 0
            && level == 0) {
            result.release_zero_tick = tick;
        }
    }
    return result;
}

}  // namespace

RateEnvelopeHandleLayout rateEnvelopeHandleLayout(
    RateEnvelopeDefinition definition,
    bool software_release) noexcept {
    const auto analysis =
        analyzeRateEnvelope(definition, software_release);
    RateEnvelopeHandleLayout layout{};
    auto& start = layout.handles[0];
    start.kind = RateEnvelopeHandleKind::AttackStart;
    start.seconds = 0.0F;
    start.level = definition.attack_level;
    start.available = true;

    auto& peak = layout.handles[1];
    peak.kind = RateEnvelopeHandleKind::AttackPeak;
    if (analysis.peak_tick >= 0) {
        peak.seconds = tickToSeconds(analysis.peak_tick);
        peak.level = 255;
        peak.available = true;
    } else {
        peak.seconds = RateEnvelopeTrace::kDurationSeconds;
        peak.level = definition.attack_level;
        peak.available = false;
    }

    auto& decay = layout.handles[2];
    decay.kind = RateEnvelopeHandleKind::DecayEnd;
    if (analysis.decay_end_tick >= 0) {
        decay.seconds = tickToSeconds(analysis.decay_end_tick);
        decay.level = definition.sustain_level;
        decay.available = true;
    } else {
        decay.seconds = RateEnvelopeTrace::kDurationSeconds;
        decay.level = definition.sustain_level;
        decay.available = analysis.peak_tick >= 0;
    }

    auto& key_off = layout.handles[3];
    key_off.kind = RateEnvelopeHandleKind::KeyOff;
    key_off.seconds = RateEnvelopeTrace::kKeyOffSeconds;
    key_off.level = analysis.level_before_key_off;
    key_off.available = true;

    auto& release = layout.handles[4];
    release.kind = RateEnvelopeHandleKind::ReleaseEnd;
    release.available = software_release;
    if (software_release && analysis.release_zero_tick >= 0) {
        release.seconds = tickToSeconds(analysis.release_zero_tick);
        release.level = 0;
    } else {
        release.seconds = RateEnvelopeTrace::kDurationSeconds;
        release.level = 0;
    }
    return layout;
}

RateEnvelopeDefinition applyRateEnvelopeHandleDrag(
    RateEnvelopeDefinition definition,
    RateEnvelopeHandleKind kind,
    float seconds,
    std::uint8_t level,
    bool software_release) noexcept {
    const int tick = secondsToTick(seconds);
    switch (kind) {
    case RateEnvelopeHandleKind::AttackStart:
        definition.attack_level = level;
        break;
    case RateEnvelopeHandleKind::AttackPeak:
        if (definition.attack_level < 255) {
            const int attack_ticks = std::max(1, tick + 1);
            definition.attack_rate = std::max<std::uint8_t>(
                1,
                ceilRate(
                    255U - definition.attack_level,
                    attack_ticks));
        }
        break;
    case RateEnvelopeHandleKind::DecayEnd: {
        definition.sustain_level = level;
        const auto analysis =
            analyzeRateEnvelope(definition, software_release);
        if (definition.sustain_level >= 255) {
            definition.sustain_level = 255;
            break;
        }
        if (analysis.peak_tick >= 0) {
            const int decay_ticks = std::max(1, tick - analysis.peak_tick);
            definition.decay_rate = std::max<std::uint8_t>(
                1,
                ceilRate(
                    255U - definition.sustain_level,
                    decay_ticks));
        }
        break;
    }
    case RateEnvelopeHandleKind::KeyOff: {
        const auto analysis =
            analyzeRateEnvelope(definition, software_release);
        if (analysis.decay_end_tick >= 0
            && analysis.decay_end_tick < kRateKeyOffTick) {
            const int sustain_ticks =
                (kRateKeyOffTick - 1) - analysis.decay_end_tick;
            if (level >= definition.sustain_level || sustain_ticks <= 0) {
                definition.sustain_level = std::max(
                    definition.sustain_level, level);
                definition.sustain_rate = 0;
            } else {
                definition.sustain_rate = std::max<std::uint8_t>(
                    1,
                    ceilRate(
                        static_cast<unsigned>(
                            definition.sustain_level) - level,
                        sustain_ticks));
            }
        } else {
            definition.sustain_level = level;
            definition.sustain_rate = 0;
            if (analysis.peak_tick >= 0
                && analysis.peak_tick < kRateKeyOffTick - 1
                && level < 255) {
                const int decay_ticks = std::max(
                    1,
                    (kRateKeyOffTick - 1) - analysis.peak_tick);
                definition.decay_rate = std::max<std::uint8_t>(
                    1,
                    ceilRate(255U - level, decay_ticks));
            }
        }
        break;
    }
    case RateEnvelopeHandleKind::ReleaseEnd: {
        const auto analysis =
            analyzeRateEnvelope(definition, true);
        if (analysis.level_before_key_off == 0) {
            break;
        }
        const int release_ticks = std::max(
            1, tick - (kRateKeyOffTick - 1));
        definition.release_rate = std::max<std::uint8_t>(
            1,
            ceilRate(analysis.level_before_key_off, release_ticks));
        break;
    }
    }
    return definition;
}

}  // namespace mgstc::engine
