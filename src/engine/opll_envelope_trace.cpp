#include "mgstc/engine/opll_envelope_trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "emu2413.h"
#include "mgstc/engine/note_pitch.hpp"

namespace mgstc::engine {
namespace {

constexpr std::uint32_t kMasterClock = 3'579'545;
constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint32_t kEgMute = 127;
constexpr std::uint32_t kEgMaximum = kEgMute - 4;
constexpr float kEgStepDecibels = 0.375F;

struct OpllDeleter {
    void operator()(OPLL* chip) const noexcept {
        OPLL_delete(chip);
    }
};

float effectiveLevel(
    const OPLL& chip,
    const OPLL_SLOT& slot) noexcept {
    if (slot.eg_out > kEgMaximum) {
        return 0.0F;
    }
    const std::uint32_t am =
        slot.patch->AM ? chip.lfo_am : 0;
    const std::uint32_t attenuation = std::min<std::uint32_t>(
        kEgMute,
        slot.eg_out + slot.tll + am);
    return std::pow(
        10.0F,
        -(static_cast<float>(attenuation)
            * kEgStepDecibels)
            / 20.0F);
}

}  // namespace

OpllEnvelopeTrace traceOpllEnvelope(
    const OpllPatchParameters& patch,
    std::uint8_t midi_note) noexcept {
    OpllEnvelopeTrace trace{};
    trace.midi_note = midi_note;
    NotePitch pitch{};
    if (!notePitch(midi_note, pitch)) {
        return trace;
    }

    std::unique_ptr<OPLL, OpllDeleter> chip(
        OPLL_new(kMasterClock, kSampleRate));
    if (!chip) {
        return trace;
    }
    OPLL_reset(chip.get());
    OPLL_setChipType(chip.get(), 0);
    OPLL_resetPatch(chip.get(), OPLL_2413_TONE);

    const auto registers = encodeOpllPatch(patch);
    for (std::size_t index = 0; index < registers.size(); ++index) {
        OPLL_writeReg(
            chip.get(),
            static_cast<std::uint32_t>(index),
            registers[index]);
    }
    OPLL_writeReg(
        chip.get(),
        0x10,
        static_cast<std::uint8_t>(pitch.opll.f_number & 0xFF));
    OPLL_writeReg(chip.get(), 0x30, 0x00);
    const std::uint8_t pitch_register =
        static_cast<std::uint8_t>(
            ((pitch.opll.f_number >> 8) & 0x01)
            | ((pitch.opll.block & 0x07) << 1));
    OPLL_writeReg(
        chip.get(),
        0x20,
        static_cast<std::uint8_t>(pitch_register | 0x10));

    constexpr std::uint32_t internal_rate =
        kMasterClock / 72;
    constexpr std::uint32_t total_ticks =
        static_cast<std::uint32_t>(
            OpllEnvelopeTrace::kDurationSeconds * internal_rate);
    constexpr std::uint32_t key_off_tick =
        static_cast<std::uint32_t>(
            OpllEnvelopeTrace::kKeyOffSeconds * internal_rate);
    std::size_t point{};
    for (std::uint32_t tick = 0;
         tick < total_ticks;
         ++tick) {
        if (tick == key_off_tick) {
            OPLL_writeReg(chip.get(), 0x20, pitch_register);
        }
        OPLL_advanceEnvelope(chip.get());
        while (point < OpllEnvelopeTrace::kPointCount
               && tick
                   >= point * static_cast<std::uint64_t>(
                       total_ticks - 1)
                       / (OpllEnvelopeTrace::kPointCount - 1)) {
            trace.modulator[point] =
                effectiveLevel(*chip, chip->slot[0]);
            trace.carrier[point] =
                effectiveLevel(*chip, chip->slot[1]);
            ++point;
        }
    }
    trace.valid = point == OpllEnvelopeTrace::kPointCount;
    return trace;
}

}  // namespace mgstc::engine
