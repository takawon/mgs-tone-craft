#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "mgstc/engine/event.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/register_write.hpp"

namespace mgstc::engine {

enum class MapError : std::uint8_t {
    None,
    InvalidTrack,
    InvalidRegister,
    InvalidWaveLength,
    InvalidPatchLength,
    PatchNotFound,
    InvalidValue,
    BufferOverflow,
    UnsupportedEvent,
};

class RegisterMapper {
public:
    static constexpr std::uint8_t kTrackCount = 17;

    RegisterMapper() noexcept;

    void reset() noexcept;
    void beginTick() noexcept;

    [[nodiscard]] MapError mapMeaningEvent(
        std::uint8_t track,
        const MeaningEvent& event,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError defineSccPatch(
        std::uint8_t patch,
        std::span<const std::uint8_t> wave) noexcept;

    [[nodiscard]] MapError defineOpllOriginalPatch(
        std::uint8_t patch,
        std::span<const std::uint8_t> registers) noexcept;

    [[nodiscard]] MapError writeSccWave(
        std::uint8_t track,
        std::span<const std::uint8_t> wave,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeOpllOriginalPatch(
        std::uint8_t track,
        std::span<const std::uint8_t> registers,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeOpllRomPatch(
        std::uint8_t track,
        std::uint8_t mml_patch,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writePsgHardwareEnvelopePeriod(
        std::uint8_t track,
        std::uint16_t period,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writePsgHardwareEnvelopeKeyOn(
        std::uint8_t track,
        std::uint8_t shape,
        bool restart_shape,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writePsgToneNoise(
        std::uint8_t track,
        std::uint8_t mode,
        std::uint8_t noise,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writePsgPeriod(
        std::uint8_t track,
        std::uint16_t period,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeSccPeriod(
        std::uint8_t track,
        std::uint16_t period,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeSccKey(
        std::uint8_t track,
        bool key_on,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeOpllPitch(
        std::uint8_t track,
        OpllPitch pitch,
        bool key_on,
        Tick tick,
        RegisterWriteBuffer& output);

    [[nodiscard]] MapError writeOpllRegister(
        std::uint8_t track,
        std::uint8_t address,
        std::uint8_t value,
        WriteReason reason,
        Tick tick,
        RegisterWriteBuffer& output);

    void setSccPeriod(std::uint8_t channel, std::uint16_t period) noexcept;

    [[nodiscard]] const std::array<std::uint8_t, 16>& psgMirror()
        const noexcept {
        return psg_mirror_;
    }

    [[nodiscard]] const std::array<std::uint8_t, 0x39>& opllMirror()
        const noexcept {
        return opll_mirror_;
    }

private:
    [[nodiscard]] MapError emit(
        RegisterWriteBuffer& output,
        Tick tick,
        ChipId chip,
        std::uint8_t port,
        std::uint8_t address,
        std::uint8_t value,
        std::uint8_t track,
        WriteReason reason);

    [[nodiscard]] MapError mapPsg(
        std::uint8_t track,
        const MeaningEvent& event,
        Tick tick,
        RegisterWriteBuffer& output);
    [[nodiscard]] MapError mapScc(
        std::uint8_t track,
        const MeaningEvent& event,
        Tick tick,
        RegisterWriteBuffer& output);
    [[nodiscard]] MapError mapOpll(
        std::uint8_t track,
        const MeaningEvent& event,
        Tick tick,
        RegisterWriteBuffer& output);

    std::uint32_t sequence_{};
    std::array<std::uint8_t, 16> psg_mirror_{};
    std::array<std::uint16_t, 5> scc_period_{};
    std::uint8_t scc_key_mask_{};
    std::array<std::uint8_t, 0x39> opll_mirror_{};
    std::array<std::array<std::uint8_t, 32>, 256> scc_patches_{};
    std::array<bool, 256> scc_patch_defined_{};
    std::array<std::array<std::uint8_t, 8>, 256> opll_patches_{};
    std::array<bool, 256> opll_patch_defined_{};
};

}  // namespace mgstc::engine
