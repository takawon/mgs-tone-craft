#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mgstc/engine/event.hpp"

namespace mgstc::engine {

enum class SequenceError : std::uint8_t {
    None,
    EventBufferOverflow,
    InstructionBudgetExceeded,
    TruncatedCommand,
    InvalidRampCount,
    PositionOutOfRange,
    UnknownOpcode,
};

class SequenceEnvelopeRuntime {
public:
    static constexpr std::size_t kDefaultInstructionBudget = 8'192;

    explicit SequenceEnvelopeRuntime(std::vector<std::uint8_t> bytecode);

    void resetForKeyOn() noexcept;

    [[nodiscard]] SequenceError processTick(
        EventBuffer& output,
        std::size_t instruction_budget = kDefaultInstructionBudget);

    [[nodiscard]] Tick tick() const noexcept {
        return tick_;
    }

    [[nodiscard]] std::uint8_t volume() const noexcept {
        return volume_;
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

private:
    [[nodiscard]] bool emit(
        EventBuffer& output,
        MeaningEventKind kind,
        std::int32_t arg0 = 0,
        std::int32_t arg1 = 0) const;

    [[nodiscard]] bool readByte(std::uint8_t& value) noexcept;

    std::vector<std::uint8_t> bytecode_;
    std::size_t position_{};
    std::size_t loop_position_{};
    std::uint16_t wait_{1};
    std::uint16_t ramp_total_{};
    std::uint16_t ramp_magnitude_{};
    std::int8_t ramp_direction_{};
    std::uint16_t ramp_remainder_{};
    std::uint8_t volume_{};
    Tick tick_{};
};

}  // namespace mgstc::engine
