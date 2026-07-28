#pragma once

#include <array>
#include <cstdint>

#include "mgstc/engine/register_mapper.hpp"

namespace mgstc::engine {

class PsgHardwareEnvelopeState {
public:
    [[nodiscard]] MapError setPeriod(
        std::uint8_t source_track,
        std::uint16_t period,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] bool setShape(std::uint8_t shape) noexcept {
        if (shape > 15) {
            return false;
        }
        shape_ = shape;
        return true;
    }

    [[nodiscard]] std::uint16_t period() const noexcept {
        return period_;
    }

    [[nodiscard]] std::uint8_t shape() const noexcept {
        return shape_;
    }

private:
    std::uint16_t period_{1};
    std::uint8_t shape_{};
};

class PsgTrackEnvelopeMode {
public:
    [[nodiscard]] bool selectHardware(
        PsgHardwareEnvelopeState& shared,
        std::uint8_t shape) noexcept;
    void setFixedVolume(std::uint8_t volume) noexcept;

    [[nodiscard]] MapError keyOn(
        std::uint8_t track,
        const PsgHardwareEnvelopeState& shared,
        std::uint8_t master_attenuation,
        std::uint8_t track_attenuation,
        bool output_enabled,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output) const;

    [[nodiscard]] bool hardwareEnabled() const noexcept {
        return hardware_enabled_;
    }

    [[nodiscard]] std::uint8_t trackVolume() const noexcept {
        return track_volume_;
    }

private:
    bool hardware_enabled_{};
    std::uint8_t track_volume_{};
};

class NineVoiceRhythmState {
public:
    [[nodiscard]] MapError enterRhythmMode(
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError trigger(
        std::uint8_t instrument_mask,
        std::uint8_t source_track,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError applyPatchVolume(
        std::uint8_t opll_channel,
        std::uint8_t mml_patch,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError applyLowerVolume(
        std::uint8_t opll_channel,
        std::uint8_t attenuation,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError silencePatchControlledDrum(
        std::uint8_t opll_channel,
        Tick tick,
        RegisterMapper& mapper,
        RegisterWriteBuffer& output);

    [[nodiscard]] std::uint8_t rhythmRegister() const noexcept {
        return register_0e_;
    }

    [[nodiscard]] std::uint8_t volumeRegister(
        std::uint8_t address) const noexcept;

private:
    [[nodiscard]] static std::uint8_t internalTrack(
        std::uint8_t opll_channel) noexcept {
        return static_cast<std::uint8_t>(opll_channel + 7);
    }

    std::uint8_t register_0e_{};
    std::array<std::uint8_t, 3> volume_registers_{};
};

}  // namespace mgstc::engine
