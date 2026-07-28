#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mgstc/engine/event.hpp"

namespace mgstc::engine {

enum class ChipId : std::uint8_t {
    Psg,
    Scc,
    Opll,
};

enum class WriteReason : std::uint8_t {
    KeyOn,
    KeyOff,
    Frequency,
    Volume,
    Patch,
    EnvelopeY,
    HardwareEnvelope,
    RhythmControl,
};

struct RegisterWrite {
    Tick tick{};
    std::uint32_t sequence{};
    ChipId chip{};
    std::uint8_t port{};
    std::uint8_t address{};
    std::uint8_t value{};
    std::uint8_t track{};
    WriteReason reason{};

    friend bool operator==(const RegisterWrite&, const RegisterWrite&) = default;
};

class RegisterWriteBuffer {
public:
    explicit RegisterWriteBuffer(std::size_t max_writes)
        : max_writes_(max_writes) {
        writes_.reserve(max_writes_);
    }

    void clear() noexcept {
        writes_.clear();
    }

    [[nodiscard]] bool push(RegisterWrite write) {
        if (writes_.size() >= max_writes_) {
            return false;
        }
        writes_.push_back(write);
        return true;
    }

    [[nodiscard]] std::span<const RegisterWrite> writes() const noexcept {
        return writes_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return writes_.size();
    }

    [[nodiscard]] std::size_t maxWrites() const noexcept {
        return max_writes_;
    }

private:
    std::size_t max_writes_{};
    std::vector<RegisterWrite> writes_;
};

}  // namespace mgstc::engine
