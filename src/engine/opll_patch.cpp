#include "mgstc/engine/opll_patch.hpp"

#include "emu2413.h"

namespace mgstc::engine {
namespace {

OpllOperatorParameters decodeOperator(
    std::uint8_t flags,
    std::uint8_t attack_decay,
    std::uint8_t sustain_release) noexcept {
    return {
        .amplitude_modulation = (flags & 0x80) != 0,
        .pitch_modulation = (flags & 0x40) != 0,
        .sustained_tone = (flags & 0x20) != 0,
        .key_rate_scaling = (flags & 0x10) != 0,
        .multiplier = static_cast<std::uint8_t>(flags & 0x0F),
        .attack_rate =
            static_cast<std::uint8_t>((attack_decay >> 4) & 0x0F),
        .decay_rate = static_cast<std::uint8_t>(attack_decay & 0x0F),
        .sustain_level =
            static_cast<std::uint8_t>((sustain_release >> 4) & 0x0F),
        .release_rate =
            static_cast<std::uint8_t>(sustain_release & 0x0F),
    };
}

std::uint8_t encodeFlags(
    const OpllOperatorParameters& parameters) noexcept {
    return static_cast<std::uint8_t>(
        (parameters.amplitude_modulation ? 0x80 : 0)
        | (parameters.pitch_modulation ? 0x40 : 0)
        | (parameters.sustained_tone ? 0x20 : 0)
        | (parameters.key_rate_scaling ? 0x10 : 0)
        | (parameters.multiplier & 0x0F));
}

}  // namespace

OpllPatchParameters decodeOpllPatch(
    std::span<const std::uint8_t, 8> registers) noexcept {
    auto modulator = decodeOperator(
        registers[0],
        registers[4],
        registers[6]);
    auto carrier = decodeOperator(
        registers[1],
        registers[5],
        registers[7]);
    modulator.key_scale_level =
        static_cast<std::uint8_t>((registers[2] >> 6) & 0x03);
    modulator.total_level =
        static_cast<std::uint8_t>(registers[2] & 0x3F);
    carrier.key_scale_level =
        static_cast<std::uint8_t>((registers[3] >> 6) & 0x03);
    modulator.waveform = (registers[3] & 0x08) != 0;
    carrier.waveform = (registers[3] & 0x10) != 0;
    return {
        .modulator = modulator,
        .carrier = carrier,
        .feedback = static_cast<std::uint8_t>(registers[3] & 0x07),
    };
}

std::array<std::uint8_t, 8> encodeOpllPatch(
    const OpllPatchParameters& parameters) noexcept {
    return {
        encodeFlags(parameters.modulator),
        encodeFlags(parameters.carrier),
        static_cast<std::uint8_t>(
            ((parameters.modulator.key_scale_level & 0x03) << 6)
            | (parameters.modulator.total_level & 0x3F)),
        static_cast<std::uint8_t>(
            ((parameters.carrier.key_scale_level & 0x03) << 6)
            | (parameters.carrier.waveform ? 0x10 : 0)
            | (parameters.modulator.waveform ? 0x08 : 0)
            | (parameters.feedback & 0x07)),
        static_cast<std::uint8_t>(
            ((parameters.modulator.attack_rate & 0x0F) << 4)
            | (parameters.modulator.decay_rate & 0x0F)),
        static_cast<std::uint8_t>(
            ((parameters.carrier.attack_rate & 0x0F) << 4)
            | (parameters.carrier.decay_rate & 0x0F)),
        static_cast<std::uint8_t>(
            ((parameters.modulator.sustain_level & 0x0F) << 4)
            | (parameters.modulator.release_rate & 0x0F)),
        static_cast<std::uint8_t>(
            ((parameters.carrier.sustain_level & 0x0F) << 4)
            | (parameters.carrier.release_rate & 0x0F)),
    };
}

OpllPatchParameters defaultOpllPatch() noexcept {
    OpllPatchParameters patch{};
    patch.modulator.sustained_tone = true;
    patch.modulator.multiplier = 1;
    patch.modulator.attack_rate = 15;
    patch.modulator.release_rate = 15;
    patch.carrier.sustained_tone = true;
    patch.carrier.multiplier = 1;
    patch.carrier.attack_rate = 15;
    patch.carrier.release_rate = 15;
    return patch;
}

std::optional<OpllPatchParameters> ym2413RomPatch(
    std::uint8_t instrument) noexcept {
    if (instrument < 1 || instrument > 15) {
        return std::nullopt;
    }
    OPLL_PATCH patch[2]{};
    OPLL_getDefaultPatch(
        OPLL_2413_TONE,
        static_cast<std::int32_t>(instrument),
        patch);
    std::array<std::uint8_t, 8> registers{};
    OPLL_patchToDump(patch, registers.data());
    return decodeOpllPatch(registers);
}

}  // namespace mgstc::engine
