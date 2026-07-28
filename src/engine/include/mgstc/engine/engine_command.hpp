#pragma once

#include <cstdint>

#include "mgstc/engine/engine_core.hpp"

namespace mgstc::engine {

enum class EngineCommandType : std::uint8_t {
    LoadProgram,
    NoteOn,
    NoteOff,
    Stop,
    HardReset,
    SetMixerGains,
};

struct EngineCommand {
    EngineCommandType type{EngineCommandType::Stop};
    std::uint8_t track{};
    std::uint8_t midi_note{60};
    MixerGains gains{};
    std::uint64_t generation{};
    bool retrigger{};
    std::uint8_t program_slot{0xFF};

    [[nodiscard]] static EngineCommand noteOn(
        std::uint8_t track,
        std::uint8_t midi_note) noexcept {
        return {
            .type = EngineCommandType::NoteOn,
            .track = track,
            .midi_note = midi_note,
        };
    }

    [[nodiscard]] static EngineCommand loadProgram(
        std::uint8_t slot,
        std::uint64_t generation,
        bool retrigger,
        std::uint8_t track,
        std::uint8_t midi_note) noexcept {
        return {
            .type = EngineCommandType::LoadProgram,
            .track = track,
            .midi_note = midi_note,
            .generation = generation,
            .retrigger = retrigger,
            .program_slot = slot,
        };
    }

    [[nodiscard]] static EngineCommand noteOff(
        std::uint8_t track) noexcept {
        return {
            .type = EngineCommandType::NoteOff,
            .track = track,
        };
    }

    [[nodiscard]] static EngineCommand stop() noexcept {
        return {.type = EngineCommandType::Stop};
    }

    [[nodiscard]] static EngineCommand hardReset() noexcept {
        return {.type = EngineCommandType::HardReset};
    }

    [[nodiscard]] static EngineCommand setMixerGains(
        MixerGains gains) noexcept {
        return {
            .type = EngineCommandType::SetMixerGains,
            .gains = gains,
        };
    }

};

}  // namespace mgstc::engine
