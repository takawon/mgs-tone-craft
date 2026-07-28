#include "mgstc/engine/shared_state.hpp"

#include <algorithm>
#include <cstddef>

namespace mgstc::engine {

MapError PsgHardwareEnvelopeState::setPeriod(
    std::uint8_t source_track,
    std::uint16_t period,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    if (period == 0) {
        return MapError::InvalidValue;
    }
    const auto error = mapper.writePsgHardwareEnvelopePeriod(
        source_track,
        period,
        tick,
        output);
    if (error == MapError::None) {
        period_ = period;
    }
    return error;
}

bool PsgTrackEnvelopeMode::selectHardware(
    PsgHardwareEnvelopeState& shared,
    std::uint8_t shape) noexcept {
    if (!shared.setShape(shape)) {
        return false;
    }
    hardware_enabled_ = true;
    track_volume_ = 15;
    return true;
}

void PsgTrackEnvelopeMode::setFixedVolume(std::uint8_t volume) noexcept {
    track_volume_ = std::min<std::uint8_t>(volume, 15);
    hardware_enabled_ = false;
}

MapError PsgTrackEnvelopeMode::keyOn(
    std::uint8_t track,
    const PsgHardwareEnvelopeState& shared,
    std::uint8_t master_attenuation,
    std::uint8_t track_attenuation,
    bool output_enabled,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) const {
    if (track >= 3) {
        return MapError::InvalidTrack;
    }
    if (master_attenuation > 15 || track_attenuation > 15) {
        return MapError::InvalidValue;
    }
    if (!hardware_enabled_ || !output_enabled) {
        return MapError::None;
    }
    const auto restart = static_cast<unsigned>(master_attenuation)
        + track_attenuation < 8U;
    return mapper.writePsgHardwareEnvelopeKeyOn(
        track,
        shared.shape(),
        restart,
        tick,
        output);
}

MapError NineVoiceRhythmState::enterRhythmMode(
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    register_0e_ = static_cast<std::uint8_t>(register_0e_ | 0x20);
    return mapper.writeOpllRegister(
        14,
        0x0E,
        register_0e_,
        WriteReason::RhythmControl,
        tick,
        output);
}

MapError NineVoiceRhythmState::trigger(
    std::uint8_t instrument_mask,
    std::uint8_t source_track,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    if (instrument_mask > 0x1F) {
        return MapError::InvalidValue;
    }
    if (source_track < 14 || source_track > 16) {
        return MapError::InvalidTrack;
    }
    if (instrument_mask == 0) {
        return MapError::None;
    }
    const auto off = static_cast<std::uint8_t>(
        register_0e_ & ~instrument_mask);
    const auto off_with_mode = static_cast<std::uint8_t>(off | 0x20);
    const auto on = static_cast<std::uint8_t>(
        off_with_mode | instrument_mask);
    auto error = mapper.writeOpllRegister(
        source_track,
        0x0E,
        off_with_mode,
        WriteReason::RhythmControl,
        tick,
        output);
    if (error != MapError::None) {
        return error;
    }
    error = mapper.writeOpllRegister(
        source_track,
        0x0E,
        on,
        WriteReason::RhythmControl,
        tick,
        output);
    if (error == MapError::None) {
        register_0e_ = on;
    }
    return error;
}

MapError NineVoiceRhythmState::applyPatchVolume(
    std::uint8_t opll_channel,
    std::uint8_t mml_patch,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    if (opll_channel < 8 || opll_channel > 9) {
        return MapError::InvalidTrack;
    }
    if (mml_patch > 14) {
        return MapError::InvalidValue;
    }
    const auto index = static_cast<std::size_t>(opll_channel - 7);
    const auto address = static_cast<std::uint8_t>(0x36 + index);
    const auto value = static_cast<std::uint8_t>(
        ((mml_patch + 1) << 4)
        | (volume_registers_[index] & 0x0F));
    const auto error = mapper.writeOpllRegister(
        internalTrack(opll_channel),
        address,
        value,
        WriteReason::Patch,
        tick,
        output);
    if (error == MapError::None) {
        volume_registers_[index] = value;
    }
    return error;
}

MapError NineVoiceRhythmState::applyLowerVolume(
    std::uint8_t opll_channel,
    std::uint8_t attenuation,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    if (opll_channel < 7 || opll_channel > 9) {
        return MapError::InvalidTrack;
    }
    if (attenuation > 15) {
        return MapError::InvalidValue;
    }
    const auto index = static_cast<std::size_t>(opll_channel - 7);
    const auto address = static_cast<std::uint8_t>(0x36 + index);
    const auto value = static_cast<std::uint8_t>(
        (volume_registers_[index] & 0xF0) | attenuation);
    const auto error = mapper.writeOpllRegister(
        internalTrack(opll_channel),
        address,
        value,
        WriteReason::Volume,
        tick,
        output);
    if (error == MapError::None) {
        volume_registers_[index] = value;
    }
    return error;
}

MapError NineVoiceRhythmState::silencePatchControlledDrum(
    std::uint8_t opll_channel,
    Tick tick,
    RegisterMapper& mapper,
    RegisterWriteBuffer& output) {
    if (opll_channel < 8 || opll_channel > 9) {
        return MapError::InvalidTrack;
    }
    const auto index = static_cast<std::size_t>(opll_channel - 7);
    const auto address = static_cast<std::uint8_t>(0x36 + index);
    const auto value = static_cast<std::uint8_t>(
        volume_registers_[index] | 0xF0);
    const auto error = mapper.writeOpllRegister(
        internalTrack(opll_channel),
        address,
        value,
        WriteReason::RhythmControl,
        tick,
        output);
    if (error == MapError::None) {
        volume_registers_[index] = value;
    }
    return error;
}

std::uint8_t NineVoiceRhythmState::volumeRegister(
    std::uint8_t address) const noexcept {
    if (address < 0x36 || address > 0x38) {
        return 0;
    }
    return volume_registers_[address - 0x36];
}

}  // namespace mgstc::engine
