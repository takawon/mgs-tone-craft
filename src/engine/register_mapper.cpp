#include "mgstc/engine/register_mapper.hpp"

#include <algorithm>
#include <cstddef>

namespace mgstc::engine {
namespace {

[[nodiscard]] std::uint8_t logicalVolume(const MeaningEvent& event) noexcept {
    const auto value = event.kind == MeaningEventKind::RateVolume
        ? event.arg1
        : event.arg0;
    return static_cast<std::uint8_t>(std::clamp(value, 0, 15));
}

// MGSDRV 3.20 OPLL `\`: 16-bit packed F-num/block (reg 10h + 20h bits 0–3),
// then at most one window snap: F-num≥0x159 → +0x153, else low<0xAC → −0x153.
constexpr int kOpllPackedLow = 0xAC;
constexpr int kOpllPackedHighNibble = 0x59;
constexpr std::uint16_t kOpllPackedOctaveUp = 0x0153;
constexpr std::uint16_t kOpllPackedOctaveDown = 0xFEAD;

[[nodiscard]] std::uint16_t addPeriodDelta(
    std::uint16_t current,
    std::int32_t mml_delta) noexcept {
    return static_cast<std::uint16_t>(static_cast<int>(current) - mml_delta);
}

void applyOpllPackedFrequencyDelta(
    std::uint16_t& packed,
    std::int32_t delta) noexcept {
    packed = static_cast<std::uint16_t>(static_cast<int>(packed) + delta);
    const auto high = static_cast<std::uint8_t>(packed >> 8);
    const auto low = static_cast<std::uint8_t>(packed & 0xFF);
    if ((high & 1) != 0) {
        if (low >= kOpllPackedHighNibble) {
            packed = static_cast<std::uint16_t>(packed + kOpllPackedOctaveUp);
        }
    } else if (low < kOpllPackedLow) {
        packed = static_cast<std::uint16_t>(packed + kOpllPackedOctaveDown);
    }
}

[[nodiscard]] std::uint16_t packOpllPitch(OpllPitch pitch) noexcept {
    return static_cast<std::uint16_t>(
        ((static_cast<unsigned>(pitch.block) << 1)
            | ((pitch.f_number >> 8) & 1U))
            << 8
        | (pitch.f_number & 0xFF));
}

}  // namespace

RegisterMapper::RegisterMapper() noexcept {
    reset();
}

void RegisterMapper::reset() noexcept {
    sequence_ = 0;
    psg_mirror_.fill(0);
    // MGSDRV initializes the PSG mixer to tone enabled / noise disabled.
    psg_mirror_[7] = 0xB8;
    scc_period_.fill(0);
    scc_key_mask_ = 0;
    opll_mirror_.fill(0);
    opll_packed_.fill(0);
    for (std::size_t address = 0x30; address <= 0x38; ++address) {
        opll_mirror_[address] = 0x0F;
    }
}

void RegisterMapper::beginTick() noexcept {
    sequence_ = 0;
}

MapError RegisterMapper::defineSccPatch(
    std::uint8_t patch,
    std::span<const std::uint8_t> wave) noexcept {
    if (wave.size() != 32) {
        return MapError::InvalidWaveLength;
    }
    std::copy(wave.begin(), wave.end(), scc_patches_[patch].begin());
    scc_patch_defined_[patch] = true;
    return MapError::None;
}

MapError RegisterMapper::defineOpllOriginalPatch(
    std::uint8_t patch,
    std::span<const std::uint8_t> registers) noexcept {
    if (registers.size() != 8) {
        return MapError::InvalidPatchLength;
    }
    std::copy(
        registers.begin(),
        registers.end(),
        opll_patches_[patch].begin());
    opll_patch_defined_[patch] = true;
    return MapError::None;
}

MapError RegisterMapper::emit(
    RegisterWriteBuffer& output,
    Tick tick,
    ChipId chip,
    std::uint8_t port,
    std::uint8_t address,
    std::uint8_t value,
    std::uint8_t track,
    WriteReason reason) {
    const RegisterWrite write{
        tick,
        sequence_,
        chip,
        port,
        address,
        value,
        track,
        reason,
    };
    if (!output.push(write)) {
        return MapError::BufferOverflow;
    }
    ++sequence_;
    return MapError::None;
}

MapError RegisterMapper::mapMeaningEvent(
    std::uint8_t track,
    const MeaningEvent& event,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track >= kTrackCount) {
        return MapError::InvalidTrack;
    }
    if (track < 3) {
        return mapPsg(track, event, tick, output);
    }
    if (track < 8) {
        return mapScc(track, event, tick, output);
    }
    return mapOpll(track, event, tick, output);
}

MapError RegisterMapper::mapPsg(
    std::uint8_t track,
    const MeaningEvent& event,
    Tick tick,
    RegisterWriteBuffer& output) {
    const auto channel = track;

    switch (event.kind) {
    case MeaningEventKind::Volume:
    case MeaningEventKind::RateVolume: {
        const auto address = static_cast<std::uint8_t>(8 + channel);
        const auto value = logicalVolume(event);
        const auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            address,
            value,
            track,
            WriteReason::Volume);
        if (error == MapError::None) {
            psg_mirror_[address] = value;
        }
        return error;
    }
    case MeaningEventKind::RegisterWrite: {
        if (event.arg0 < 0 || event.arg0 > 15
            || event.arg1 < 0 || event.arg1 > 255) {
            return MapError::InvalidRegister;
        }
        const auto address = static_cast<std::uint8_t>(event.arg0);
        const auto value = static_cast<std::uint8_t>(event.arg1);
        const auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            address,
            value,
            track,
            WriteReason::EnvelopeY);
        if (error == MapError::None) {
            psg_mirror_[address] = value;
        }
        return error;
    }
    case MeaningEventKind::FrequencyDelta: {
        const auto low_address = static_cast<std::uint8_t>(channel * 2);
        const auto high_address = static_cast<std::uint8_t>(low_address + 1);
        const auto current = static_cast<std::uint16_t>(
            psg_mirror_[low_address]
            | (static_cast<unsigned>(psg_mirror_[high_address]) << 8));
        const auto period = addPeriodDelta(current, event.arg0);
        const auto low = static_cast<std::uint8_t>(period & 0xFF);
        const auto high = static_cast<std::uint8_t>(period >> 8);
        auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            low_address,
            low,
            track,
            WriteReason::Frequency);
        if (error != MapError::None) {
            return error;
        }
        psg_mirror_[low_address] = low;
        error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            high_address,
            high,
            track,
            WriteReason::Frequency);
        if (error == MapError::None) {
            psg_mirror_[high_address] = high;
        }
        return error;
    }
    case MeaningEventKind::Noise: {
        const auto value = static_cast<std::uint8_t>(event.arg0 & 0x1F);
        const auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            6,
            value,
            track,
            WriteReason::Patch);
        if (error == MapError::None) {
            psg_mirror_[6] = value;
        }
        return error;
    }
    case MeaningEventKind::ToneNoiseMode: {
        const auto mode = static_cast<std::uint8_t>(event.arg0 & 0x03);
        const auto tone_mask = static_cast<std::uint8_t>(1U << channel);
        const auto noise_mask = static_cast<std::uint8_t>(1U << (channel + 3));
        auto value = psg_mirror_[7];
        value = (mode & 0x01) != 0
            ? static_cast<std::uint8_t>(value & ~tone_mask)
            : static_cast<std::uint8_t>(value | tone_mask);
        value = (mode & 0x02) != 0
            ? static_cast<std::uint8_t>(value & ~noise_mask)
            : static_cast<std::uint8_t>(value | noise_mask);
        const auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            7,
            value,
            track,
            WriteReason::Patch);
        if (error == MapError::None) {
            psg_mirror_[7] = value;
        }
        return error;
    }
    case MeaningEventKind::Patch:
        return MapError::None;
    }
    return MapError::UnsupportedEvent;
}

MapError RegisterMapper::mapScc(
    std::uint8_t track,
    const MeaningEvent& event,
    Tick tick,
    RegisterWriteBuffer& output) {
    const auto channel = static_cast<std::uint8_t>(track - 3);

    switch (event.kind) {
    case MeaningEventKind::Volume:
    case MeaningEventKind::RateVolume:
        return emit(
            output,
            tick,
            ChipId::Scc,
            2,
            channel,
            logicalVolume(event),
            track,
            WriteReason::Volume);
    case MeaningEventKind::Patch: {
        if (event.arg0 < 0 || event.arg0 > 255) {
            return MapError::PatchNotFound;
        }
        const auto patch = static_cast<std::uint8_t>(event.arg0);
        if (!scc_patch_defined_[patch]) {
            return MapError::PatchNotFound;
        }
        return writeSccWave(
            track,
            scc_patches_[patch],
            tick,
            output);
    }
    case MeaningEventKind::RegisterWrite:
    case MeaningEventKind::Noise:
    case MeaningEventKind::ToneNoiseMode:
        return MapError::None;
    case MeaningEventKind::FrequencyDelta: {
        const auto current = scc_period_[channel];
        const auto period = addPeriodDelta(current, event.arg0);
        const auto low_address = static_cast<std::uint8_t>(channel * 2);
        const auto high_address = static_cast<std::uint8_t>(low_address + 1);
        auto error = emit(
            output,
            tick,
            ChipId::Scc,
            1,
            low_address,
            static_cast<std::uint8_t>(period & 0xFF),
            track,
            WriteReason::Frequency);
        if (error != MapError::None) {
            return error;
        }
        error = emit(
            output,
            tick,
            ChipId::Scc,
            1,
            high_address,
            static_cast<std::uint8_t>(period >> 8),
            track,
            WriteReason::Frequency);
        if (error == MapError::None) {
            scc_period_[channel] = period;
        }
        return error;
    }
    }
    return MapError::UnsupportedEvent;
}

MapError RegisterMapper::mapOpll(
    std::uint8_t track,
    const MeaningEvent& event,
    Tick tick,
    RegisterWriteBuffer& output) {
    const auto channel = static_cast<std::uint8_t>(track - 8);

    switch (event.kind) {
    case MeaningEventKind::Volume:
    case MeaningEventKind::RateVolume: {
        const auto address = static_cast<std::uint8_t>(0x30 + channel);
        const auto attenuation = static_cast<std::uint8_t>(
            logicalVolume(event) ^ 0x0F);
        const auto value = static_cast<std::uint8_t>(
            (opll_mirror_[address] & 0xF0) | attenuation);
        const auto error = emit(
            output,
            tick,
            ChipId::Opll,
            0,
            address,
            value,
            track,
            WriteReason::Volume);
        if (error == MapError::None) {
            opll_mirror_[address] = value;
        }
        return error;
    }
    case MeaningEventKind::Patch: {
        if (event.arg0 < 0 || event.arg0 > 255) {
            return MapError::PatchNotFound;
        }
        const auto patch = static_cast<std::uint8_t>(event.arg0);
        if (patch <= 14) {
            return writeOpllRomPatch(track, patch, tick, output);
        }
        if (!opll_patch_defined_[patch]) {
            return MapError::PatchNotFound;
        }
        return writeOpllOriginalPatch(
            track,
            opll_patches_[patch],
            tick,
            output);
    }
    case MeaningEventKind::RegisterWrite: {
        if (event.arg0 < 0 || event.arg0 > 56
            || event.arg1 < 0 || event.arg1 > 255) {
            return MapError::InvalidRegister;
        }
        const auto address = static_cast<std::uint8_t>(event.arg0);
        const auto value = static_cast<std::uint8_t>(event.arg1);
        const auto reason = address == 0x0E
            ? WriteReason::RhythmControl
            : WriteReason::EnvelopeY;
        const auto error = emit(
            output,
            tick,
            ChipId::Opll,
            0,
            address,
            value,
            track,
            reason);
        if (error == MapError::None) {
            opll_mirror_[address] = value;
        }
        return error;
    }
    case MeaningEventKind::FrequencyDelta: {
        const auto low_address = static_cast<std::uint8_t>(0x10 + channel);
        const auto high_address = static_cast<std::uint8_t>(0x20 + channel);
        auto packed = opll_packed_[channel];
        applyOpllPackedFrequencyDelta(packed, event.arg0);
        opll_packed_[channel] = packed;
        const auto low = static_cast<std::uint8_t>(packed & 0xFF);
        const auto high = static_cast<std::uint8_t>(
            (opll_mirror_[high_address] & 0x30)
            | static_cast<std::uint8_t>(packed >> 8));
        // MGSDRV writes the key/block register before the F-number low byte.
        auto error = emit(
            output,
            tick,
            ChipId::Opll,
            0,
            high_address,
            high,
            track,
            WriteReason::Frequency);
        if (error != MapError::None) {
            return error;
        }
        opll_mirror_[high_address] = high;
        error = emit(
            output,
            tick,
            ChipId::Opll,
            0,
            low_address,
            low,
            track,
            WriteReason::Frequency);
        if (error == MapError::None) {
            opll_mirror_[low_address] = low;
        }
        return error;
    }
    case MeaningEventKind::Noise:
    case MeaningEventKind::ToneNoiseMode:
        return MapError::None;
    }
    return MapError::UnsupportedEvent;
}

MapError RegisterMapper::writeSccWave(
    std::uint8_t track,
    std::span<const std::uint8_t> wave,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 3 || track >= 8) {
        return MapError::InvalidTrack;
    }
    if (wave.size() != 32) {
        return MapError::InvalidWaveLength;
    }
    const auto channel = static_cast<std::uint8_t>(track - 3);
    const auto base = static_cast<std::uint8_t>(
        std::min<std::uint8_t>(channel, 3) * 32);
    for (std::size_t index = 0; index < wave.size(); ++index) {
        const auto error = emit(
            output,
            tick,
            ChipId::Scc,
            0,
            static_cast<std::uint8_t>(base + index),
            wave[index],
            track,
            WriteReason::Patch);
        if (error != MapError::None) {
            return error;
        }
    }
    return MapError::None;
}

MapError RegisterMapper::writeOpllOriginalPatch(
    std::uint8_t track,
    std::span<const std::uint8_t> registers,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 8 || track >= kTrackCount) {
        return MapError::InvalidTrack;
    }
    if (registers.size() != 8) {
        return MapError::InvalidPatchLength;
    }
    for (std::size_t address = 0; address < registers.size(); ++address) {
        const auto error = emit(
            output,
            tick,
            ChipId::Opll,
            0,
            static_cast<std::uint8_t>(address),
            registers[address],
            track,
            WriteReason::Patch);
        if (error != MapError::None) {
            return error;
        }
        opll_mirror_[address] = registers[address];
    }
    const auto volume_address = static_cast<std::uint8_t>(
        0x30 + (track - 8));
    const auto error = emit(
        output,
        tick,
        ChipId::Opll,
        0,
        volume_address,
        0x0F,
        track,
        WriteReason::Patch);
    if (error == MapError::None) {
        opll_mirror_[volume_address] = 0x0F;
    }
    return error;
}

MapError RegisterMapper::writeOpllRomPatch(
    std::uint8_t track,
    std::uint8_t mml_patch,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 8 || track >= kTrackCount) {
        return MapError::InvalidTrack;
    }
    if (mml_patch > 14) {
        return MapError::PatchNotFound;
    }
    const auto address = static_cast<std::uint8_t>(0x30 + (track - 8));
    const auto value = static_cast<std::uint8_t>(
        ((mml_patch + 1) << 4) | (opll_mirror_[address] & 0x0F));
    const auto error = emit(
        output,
        tick,
        ChipId::Opll,
        0,
        address,
        value,
        track,
        WriteReason::Patch);
    if (error == MapError::None) {
        opll_mirror_[address] = value;
    }
    return error;
}

MapError RegisterMapper::writePsgHardwareEnvelopePeriod(
    std::uint8_t track,
    std::uint16_t period,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track >= 3) {
        return MapError::InvalidTrack;
    }
    if (period == 0) {
        return MapError::InvalidValue;
    }
    const auto low = static_cast<std::uint8_t>(period & 0xFF);
    const auto high = static_cast<std::uint8_t>(period >> 8);
    auto error = emit(
        output,
        tick,
        ChipId::Psg,
        0,
        11,
        low,
        track,
        WriteReason::HardwareEnvelope);
    if (error != MapError::None) {
        return error;
    }
    psg_mirror_[11] = low;
    error = emit(
        output,
        tick,
        ChipId::Psg,
        0,
        12,
        high,
        track,
        WriteReason::HardwareEnvelope);
    if (error == MapError::None) {
        psg_mirror_[12] = high;
    }
    return error;
}

MapError RegisterMapper::writePsgToneNoise(
    std::uint8_t track,
    std::uint8_t mode,
    std::uint8_t noise,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track >= 3) {
        return MapError::InvalidTrack;
    }
    if (mode > 3 || noise > 31) {
        return MapError::InvalidValue;
    }
    auto error = mapPsg(
        track,
        {tick, MeaningEventKind::ToneNoiseMode, mode, 0},
        tick,
        output);
    if (error != MapError::None) {
        return error;
    }
    return mapPsg(
        track,
        {tick, MeaningEventKind::Noise, noise, 0},
        tick,
        output);
}

MapError RegisterMapper::writePsgPeriod(
    std::uint8_t track,
    std::uint16_t period,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track >= 3) {
        return MapError::InvalidTrack;
    }
    if (period == 0 || period > 0x0FFF) {
        return MapError::InvalidValue;
    }
    const auto low_address = static_cast<std::uint8_t>(track * 2);
    const auto high_address = static_cast<std::uint8_t>(low_address + 1);
    auto error = emit(
        output,
        tick,
        ChipId::Psg,
        0,
        low_address,
        static_cast<std::uint8_t>(period & 0xFF),
        track,
        WriteReason::Frequency);
    if (error != MapError::None) {
        return error;
    }
    psg_mirror_[low_address] = static_cast<std::uint8_t>(period & 0xFF);
    error = emit(
        output,
        tick,
        ChipId::Psg,
        0,
        high_address,
        static_cast<std::uint8_t>((period >> 8) & 0x0F),
        track,
        WriteReason::Frequency);
    if (error == MapError::None) {
        psg_mirror_[high_address] =
            static_cast<std::uint8_t>((period >> 8) & 0x0F);
    }
    return error;
}

MapError RegisterMapper::writeSccPeriod(
    std::uint8_t track,
    std::uint16_t period,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 3 || track >= 8) {
        return MapError::InvalidTrack;
    }
    if (period == 0 || period > 0x0FFF) {
        return MapError::InvalidValue;
    }
    const auto channel = static_cast<std::uint8_t>(track - 3);
    const auto low_address = static_cast<std::uint8_t>(channel * 2);
    const auto high_address = static_cast<std::uint8_t>(low_address + 1);
    auto error = emit(
        output,
        tick,
        ChipId::Scc,
        1,
        low_address,
        static_cast<std::uint8_t>(period & 0xFF),
        track,
        WriteReason::Frequency);
    if (error != MapError::None) {
        return error;
    }
    error = emit(
        output,
        tick,
        ChipId::Scc,
        1,
        high_address,
        static_cast<std::uint8_t>((period >> 8) & 0x0F),
        track,
        WriteReason::Frequency);
    if (error == MapError::None) {
        scc_period_[channel] = period;
    }
    return error;
}

MapError RegisterMapper::writeSccKey(
    std::uint8_t track,
    bool key_on,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 3 || track >= 8) {
        return MapError::InvalidTrack;
    }
    const auto channel = static_cast<std::uint8_t>(track - 3);
    const auto mask = static_cast<std::uint8_t>(1U << channel);
    const auto value = key_on
        ? static_cast<std::uint8_t>(scc_key_mask_ | mask)
        : static_cast<std::uint8_t>(scc_key_mask_ & ~mask);
    const auto error = emit(
        output,
        tick,
        ChipId::Scc,
        3,
        0,
        value,
        track,
        key_on ? WriteReason::KeyOn : WriteReason::KeyOff);
    if (error == MapError::None) {
        scc_key_mask_ = value;
    }
    return error;
}

MapError RegisterMapper::writeOpllPitch(
    std::uint8_t track,
    OpllPitch pitch,
    bool key_on,
    Tick tick,
    RegisterWriteBuffer& output,
    bool sustain) {
    if (track < 8 || track >= kTrackCount) {
        return MapError::InvalidTrack;
    }
    if (pitch.f_number > 0x01FF || pitch.block > 7) {
        return MapError::InvalidValue;
    }
    const auto channel = static_cast<std::uint8_t>(track - 8);
    const auto low_address = static_cast<std::uint8_t>(0x10 + channel);
    const auto high_address = static_cast<std::uint8_t>(0x20 + channel);
    const auto high = static_cast<std::uint8_t>(
        ((pitch.f_number >> 8) & 0x01)
        | static_cast<std::uint8_t>(pitch.block << 1)
        | (key_on ? 0x10 : 0x00)
        | (sustain ? 0x20 : 0x00));

    // MGSDRV writes the key/block register before the F-number low byte.
    auto error = emit(
        output,
        tick,
        ChipId::Opll,
        0,
        high_address,
        high,
        track,
        key_on ? WriteReason::KeyOn : WriteReason::KeyOff);
    if (error != MapError::None) {
        return error;
    }
    opll_mirror_[high_address] = high;
    opll_packed_[channel] = packOpllPitch(pitch);
    if (!key_on) {
        return MapError::None;
    }
    error = emit(
        output,
        tick,
        ChipId::Opll,
        0,
        low_address,
        static_cast<std::uint8_t>(pitch.f_number & 0xFF),
        track,
        WriteReason::Frequency);
    if (error == MapError::None) {
        opll_mirror_[low_address] =
            static_cast<std::uint8_t>(pitch.f_number & 0xFF);
    }
    return error;
}

MapError RegisterMapper::writePsgHardwareEnvelopeKeyOn(
    std::uint8_t track,
    std::uint8_t shape,
    bool restart_shape,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track >= 3) {
        return MapError::InvalidTrack;
    }
    if (shape > 15) {
        return MapError::InvalidValue;
    }
    if (restart_shape) {
        const auto error = emit(
            output,
            tick,
            ChipId::Psg,
            0,
            13,
            shape,
            track,
            WriteReason::HardwareEnvelope);
        if (error != MapError::None) {
            return error;
        }
        psg_mirror_[13] = shape;
    }
    const auto address = static_cast<std::uint8_t>(8 + track);
    const auto error = emit(
        output,
        tick,
        ChipId::Psg,
        0,
        address,
        0x10,
        track,
        WriteReason::HardwareEnvelope);
    if (error == MapError::None) {
        psg_mirror_[address] = 0x10;
    }
    return error;
}

MapError RegisterMapper::writeOpllRegister(
    std::uint8_t track,
    std::uint8_t address,
    std::uint8_t value,
    WriteReason reason,
    Tick tick,
    RegisterWriteBuffer& output) {
    if (track < 8 || track >= kTrackCount) {
        return MapError::InvalidTrack;
    }
    if (address > 56) {
        return MapError::InvalidRegister;
    }
    const auto error = emit(
        output,
        tick,
        ChipId::Opll,
        0,
        address,
        value,
        track,
        reason);
    if (error == MapError::None) {
        opll_mirror_[address] = value;
    }
    return error;
}

void RegisterMapper::setSccPeriod(
    std::uint8_t channel,
    std::uint16_t period) noexcept {
    if (channel < scc_period_.size()) {
        scc_period_[channel] = static_cast<std::uint16_t>(
            std::min<std::uint16_t>(period, 0x0FFF));
    }
}

}  // namespace mgstc::engine
