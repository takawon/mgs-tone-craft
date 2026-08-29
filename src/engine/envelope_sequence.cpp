#include "mgstc/engine/envelope_sequence.hpp"

#include <algorithm>
#include <cstdlib>
#include <span>
#include <utility>

namespace mgstc::engine {

SequenceEnvelopeRuntime::SequenceEnvelopeRuntime(
    std::vector<std::uint8_t> bytecode,
    bool emit_volume_events)
    : bytecode_(std::move(bytecode)),
      emit_volume_events_(emit_volume_events) {}

void SequenceEnvelopeRuntime::resetForKeyOn() noexcept {
    wait_ = 1;
    ramp_total_ = 0;
    position_ = 0;
    loop_position_ = 0;
    volume_ = 0;
    tick_ = 0;
}

void SequenceEnvelopeRuntime::resetForKeyOn(
    std::uint8_t initial_volume) noexcept {
    resetForKeyOn();
    volume_ = initial_volume;
}

std::vector<std::uint8_t> sampleSequenceEnvelopeVolumes(
    std::span<const std::uint8_t> bytecode,
    std::size_t ticks,
    std::uint8_t initial_volume) {
    std::vector<std::uint8_t> copy(bytecode.begin(), bytecode.end());
    SequenceEnvelopeRuntime runtime(std::move(copy), false);
    runtime.resetForKeyOn(initial_volume);
    EventBuffer buffer(8);
    std::vector<std::uint8_t> volumes(ticks, 0);
    for (std::size_t i = 0; i < ticks; ++i) {
        buffer.clear();
        static_cast<void>(runtime.processTick(buffer));
        volumes[i] = runtime.volume();
    }
    return volumes;
}

std::vector<std::uint8_t> sampleAutomaticRampVolumes(
    std::uint8_t origin,
    std::uint8_t target,
    std::uint32_t duration,
    bool include_origin_letter) {
    if (duration == 0) {
        return {};
    }
    const auto clipped = std::min<std::uint32_t>(duration, 255);
    std::vector<std::uint8_t> bytecode;
    bytecode.reserve(3);
    if (include_origin_letter) {
        bytecode.push_back(static_cast<std::uint8_t>(origin & 0x0F));
    }
    bytecode.push_back(
        static_cast<std::uint8_t>(0x20 | (target & 0x0F)));
    bytecode.push_back(static_cast<std::uint8_t>(clipped));
    const auto initial = include_origin_letter
        ? std::uint8_t{0}
        : origin;
    return sampleSequenceEnvelopeVolumes(bytecode, clipped, initial);
}

bool SequenceEnvelopeRuntime::emit(
    EventBuffer& output,
    MeaningEventKind kind,
    std::int32_t arg0,
    std::int32_t arg1) const {
    if (kind == MeaningEventKind::Volume && !emit_volume_events_) {
        return true;
    }
    return output.push(MeaningEvent{tick_, kind, arg0, arg1});
}

bool SequenceEnvelopeRuntime::readByte(std::uint8_t& value) noexcept {
    if (position_ >= bytecode_.size()) {
        return false;
    }
    value = bytecode_[position_++];
    return true;
}

SequenceError SequenceEnvelopeRuntime::processTick(
    EventBuffer& output,
    std::size_t instruction_budget) {
    if (wait_ != 0) {
        if (ramp_total_ != 0) {
            // MGSDRV adds these values in the Z80 A register and discards
            // carry before its 8-by-8-bit division.  Counts near 255 expose
            // this wraparound, so do not widen the addition.
            const auto numerator = static_cast<std::uint8_t>(
                ramp_remainder_ + ramp_magnitude_);
            const auto quotient = numerator / ramp_total_;
            ramp_remainder_ = static_cast<std::uint16_t>(
                numerator % ramp_total_);
            const auto next = static_cast<std::int32_t>(volume_)
                + static_cast<std::int32_t>(ramp_direction_) *
                    static_cast<std::int32_t>(quotient);
            volume_ = static_cast<std::uint8_t>(next);
            if (!emit(output, MeaningEventKind::Volume, volume_)) {
                return SequenceError::EventBufferOverflow;
            }
        }

        --wait_;
        if (wait_ != 0) {
            ++tick_;
            return SequenceError::None;
        }
        ramp_total_ = 0;
    }

    std::size_t executed = 0;
    while (true) {
        if (++executed > instruction_budget) {
            return SequenceError::InstructionBudgetExceeded;
        }

        if (position_ == bytecode_.size()) {
            if (!emit(output, MeaningEventKind::Volume, volume_)) {
                return SequenceError::EventBufferOverflow;
            }
            break;
        }
        if (position_ > bytecode_.size()) {
            return SequenceError::PositionOutOfRange;
        }

        const auto opcode = bytecode_[position_++];
        if (opcode <= 0x0F) {
            volume_ = opcode;
            wait_ = 1;
            if (!emit(output, MeaningEventKind::Volume, volume_)) {
                return SequenceError::EventBufferOverflow;
            }
            break;
        }

        if (opcode == 0x10) {
            std::uint8_t patch = 0;
            if (!readByte(patch)) {
                return SequenceError::TruncatedCommand;
            }
            if (!emit(output, MeaningEventKind::Patch, patch)) {
                return SequenceError::EventBufferOverflow;
            }
            continue;
        }

        if (opcode == 0x11) {
            std::uint8_t reg = 0;
            std::uint8_t value = 0;
            if (!readByte(reg) || !readByte(value)) {
                return SequenceError::TruncatedCommand;
            }
            if (!emit(
                    output,
                    MeaningEventKind::RegisterWrite,
                    reg,
                    value)) {
                return SequenceError::EventBufferOverflow;
            }
            continue;
        }

        if (opcode == 0x12) {
            std::uint8_t encoded = 0;
            if (!readByte(encoded)) {
                return SequenceError::TruncatedCommand;
            }
            const auto delta = encoded >= 0x80
                ? static_cast<std::int32_t>(encoded) - 0x100
                : static_cast<std::int32_t>(encoded);
            if (!emit(
                    output,
                    MeaningEventKind::FrequencyDelta,
                    delta)) {
                return SequenceError::EventBufferOverflow;
            }
            continue;
        }

        if (opcode >= 0x20 && opcode <= 0x2F) {
            std::uint8_t count = 0;
            if (!readByte(count)) {
                return SequenceError::TruncatedCommand;
            }
            const auto target = static_cast<std::int32_t>(opcode & 0x0F);
            const auto delta = target - static_cast<std::int32_t>(volume_);
            ramp_total_ = count;
            ramp_magnitude_ = static_cast<std::uint16_t>(std::abs(delta));
            ramp_direction_ = delta >= 0 ? 1 : -1;
            ramp_remainder_ = 0;
            wait_ = count;
            if (!emit(output, MeaningEventKind::Volume, volume_)) {
                return SequenceError::EventBufferOverflow;
            }
            break;
        }

        if (opcode == 0x40) {
            loop_position_ = position_;
            continue;
        }

        if (opcode == 0x60) {
            position_ = loop_position_;
            continue;
        }

        if (opcode >= 0x80 && opcode <= 0x9F) {
            if (!emit(
                    output,
                    MeaningEventKind::Noise,
                    opcode & 0x1F)) {
                return SequenceError::EventBufferOverflow;
            }
            continue;
        }

        if (opcode >= 0xA0 && opcode <= 0xA3) {
            if (!emit(
                    output,
                    MeaningEventKind::ToneNoiseMode,
                    opcode & 0x03)) {
                return SequenceError::EventBufferOverflow;
            }
            continue;
        }

        if (opcode >= 0xE0 && opcode <= 0xEF) {
            std::uint8_t count = 0;
            if (!readByte(count)) {
                return SequenceError::TruncatedCommand;
            }
            volume_ = opcode & 0x0F;
            wait_ = count;
            if (!emit(output, MeaningEventKind::Volume, volume_)) {
                return SequenceError::EventBufferOverflow;
            }
            break;
        }

        return SequenceError::UnknownOpcode;
    }

    ++tick_;
    return SequenceError::None;
}

}  // namespace mgstc::engine
