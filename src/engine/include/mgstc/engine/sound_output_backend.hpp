#pragma once

#include <cstdint>
#include <span>

#include "mgstc/engine/register_write.hpp"

namespace mgstc::engine {

enum class SoundOutputKind : std::uint8_t {
    Emulator,
    MAmidiMemo,
};

// Pluggable destination for ordered RegisterWrite events.
// EmulatorSoundOutput renders locally; MAmidiMemoSoundOutput forwards via RPC.
// Future Direct VSIF can add another backend without changing the runtime core.
class SoundOutputBackend {
public:
    virtual ~SoundOutputBackend() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    [[nodiscard]] virtual bool writeRegister(
        const RegisterWrite& write) = 0;
    [[nodiscard]] virtual bool writeRegisters(
        std::span<const RegisterWrite> writes) = 0;

    virtual void reset() = 0;
    virtual void allNotesOff() = 0;

    [[nodiscard]] virtual SoundOutputKind kind() const = 0;
};

}  // namespace mgstc::engine
