#pragma once

#include <cstdint>

namespace mgstc::engine {

class EngineCore;

struct ProgramEdit {
    static constexpr std::uint8_t kInvalidSlot = 0xFF;

    std::uint8_t slot{kInvalidSlot};
    std::uint64_t generation{};
    EngineCore* engine{};

    [[nodiscard]] bool valid() const noexcept {
        return slot != kInvalidSlot && engine != nullptr;
    }
};

struct ProgramAudition {
    bool retrigger{};
    std::uint8_t track{};
    std::uint8_t midi_note{60};
};

}  // namespace mgstc::engine
