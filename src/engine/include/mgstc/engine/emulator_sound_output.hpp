#pragma once

#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/sound_output_backend.hpp"

namespace mgstc::engine {

class EmulatorSoundOutput final : public SoundOutputBackend {
public:
    EmulatorSoundOutput() = default;

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] bool writeRegister(
        const RegisterWrite& write) override;
    [[nodiscard]] bool writeRegisters(
        std::span<const RegisterWrite> writes) override;

    void reset() override;
    void allNotesOff() override;

    [[nodiscard]] SoundOutputKind kind() const override;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ChipSamples renderSample(bool capture_channels = false) noexcept;

private:
    ChipRack chips_{};
    bool open_{true};
};

}  // namespace mgstc::engine
