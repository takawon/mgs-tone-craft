#include "mgstc/engine/emulator_sound_output.hpp"

namespace mgstc::engine {
namespace {

void silencePsg(ChipRack& chips) noexcept {
    // Mixer: tone and noise off on all channels (upper bits preserved as 10xxxx).
    static_cast<void>(chips.apply(RegisterWrite{
        .chip = ChipId::Psg,
        .port = 0,
        .address = 7,
        .value = 0x3F,
        .reason = WriteReason::KeyOff,
    }));
    for (std::uint8_t channel = 0; channel < 3; ++channel) {
        static_cast<void>(chips.apply(RegisterWrite{
            .chip = ChipId::Psg,
            .port = 0,
            .address = static_cast<std::uint8_t>(8 + channel),
            .value = 0,
            .reason = WriteReason::Volume,
        }));
    }
}

void silenceScc(ChipRack& chips) noexcept {
    for (std::uint8_t channel = 0; channel < 5; ++channel) {
        static_cast<void>(chips.apply(RegisterWrite{
            .chip = ChipId::Scc,
            .port = 2,
            .address = channel,
            .value = 0,
            .reason = WriteReason::Volume,
        }));
    }
    static_cast<void>(chips.apply(RegisterWrite{
        .chip = ChipId::Scc,
        .port = 3,
        .address = 0,
        .value = 0,
        .reason = WriteReason::KeyOff,
    }));
}

void silenceOpll(ChipRack& chips) noexcept {
    for (std::uint8_t channel = 0; channel < 9; ++channel) {
        static_cast<void>(chips.apply(RegisterWrite{
            .chip = ChipId::Opll,
            .port = 0,
            .address = static_cast<std::uint8_t>(0x20 + channel),
            .value = 0,
            .reason = WriteReason::KeyOff,
        }));
        static_cast<void>(chips.apply(RegisterWrite{
            .chip = ChipId::Opll,
            .port = 0,
            .address = static_cast<std::uint8_t>(0x30 + channel),
            .value = 0x0F,
            .reason = WriteReason::Volume,
        }));
    }
    static_cast<void>(chips.apply(RegisterWrite{
        .chip = ChipId::Opll,
        .port = 0,
        .address = 0x0E,
        .value = 0,
        .reason = WriteReason::RhythmControl,
    }));
}

}  // namespace

bool EmulatorSoundOutput::open() {
    open_ = true;
    return true;
}

void EmulatorSoundOutput::close() {
    if (open_) {
        allNotesOff();
    }
    open_ = false;
}

bool EmulatorSoundOutput::isOpen() const {
    return open_;
}

bool EmulatorSoundOutput::writeRegister(const RegisterWrite& write) {
    if (!open_) {
        return false;
    }
    return chips_.apply(write);
}

bool EmulatorSoundOutput::writeRegisters(
    std::span<const RegisterWrite> writes) {
    if (!open_) {
        return false;
    }
    return chips_.apply(writes);
}

void EmulatorSoundOutput::reset() {
    chips_.reset();
    open_ = true;
}

void EmulatorSoundOutput::allNotesOff() {
    if (!chips_.valid()) {
        return;
    }
    silencePsg(chips_);
    silenceScc(chips_);
    silenceOpll(chips_);
}

SoundOutputKind EmulatorSoundOutput::kind() const {
    return SoundOutputKind::Emulator;
}

bool EmulatorSoundOutput::valid() const noexcept {
    return chips_.valid();
}

ChipSamples EmulatorSoundOutput::renderSample() noexcept {
    return chips_.renderSample();
}

}  // namespace mgstc::engine
