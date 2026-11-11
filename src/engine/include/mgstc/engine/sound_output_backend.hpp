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

    // Audio producer only. Called once per runtime tick with newly started
    // physical tracks; implementations must not wait, allocate or perform I/O.
    virtual void beginTick(std::uint32_t /*new_note_tracks*/) noexcept {}
    // Atomic-only capture is also called when control-side commands enter the
    // input queue. Delivery keeps that token instead of sampling it later.
    [[nodiscard]] virtual std::uint64_t captureNoteToken() const noexcept { return 0; }
    virtual void noteQueued(std::uint8_t /*track*/, std::uint64_t /*token*/) noexcept {}
    // A remote transport may defer delivery while the local mirror continues.
    [[nodiscard]] virtual bool recoverableWriteFailure() const noexcept { return false; }

    virtual void reset() = 0;
    virtual void allNotesOff() = 0;

    [[nodiscard]] virtual SoundOutputKind kind() const = 0;
};

}  // namespace mgstc::engine
