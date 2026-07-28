#include "mgstc/engine/chip_rack.hpp"

#include <cstdint>
#include <utility>

extern "C" {
#include "emu2149.h"
#include "emu2212.h"
#include "emu2413.h"
}

namespace mgstc::engine {
namespace {

constexpr std::uint32_t kMasterClock = 3'579'545;
constexpr std::uint32_t kSccClock = 1'789'773;
constexpr std::uint32_t kSampleRate = 48'000;
constexpr float kInt16Scale = 1.0F / 32768.0F;

}  // namespace

struct Ym2149Adapter::Impl {
    PSG* chip{};

    Impl()
        : chip(PSG_new(kMasterClock, kSampleRate)) {}

    ~Impl() {
        PSG_delete(chip);
    }
};

Ym2149Adapter::Ym2149Adapter()
    : impl_(std::make_unique<Impl>()) {
    if (valid()) {
        PSG_setClockDivider(impl_->chip, 1);
        PSG_setVolumeMode(impl_->chip, 1);
        PSG_setQuality(impl_->chip, 1);
        PSG_reset(impl_->chip);
    }
}

Ym2149Adapter::~Ym2149Adapter() = default;
Ym2149Adapter::Ym2149Adapter(Ym2149Adapter&&) noexcept = default;
Ym2149Adapter& Ym2149Adapter::operator=(Ym2149Adapter&&) noexcept = default;

bool Ym2149Adapter::valid() const noexcept {
    return impl_ && impl_->chip;
}

void Ym2149Adapter::reset() noexcept {
    if (valid()) {
        PSG_reset(impl_->chip);
    }
}

bool Ym2149Adapter::write(
    std::uint8_t address,
    std::uint8_t value) noexcept {
    if (!valid() || address > 15) {
        return false;
    }
    PSG_writeReg(impl_->chip, address, value);
    return true;
}

float Ym2149Adapter::renderSample() noexcept {
    return valid()
        ? static_cast<float>(PSG_calc(impl_->chip)) * kInt16Scale
        : 0.0F;
}

struct SccAdapter::Impl {
    SCC* chip{};

    Impl()
        : chip(SCC_new(kSccClock, kSampleRate)) {}

    ~Impl() {
        SCC_delete(chip);
    }
};

SccAdapter::SccAdapter()
    : impl_(std::make_unique<Impl>()) {
    if (valid()) {
        SCC_set_type(impl_->chip, SCC_STANDARD);
        SCC_set_quality(impl_->chip, 1);
        SCC_reset(impl_->chip);
    }
}

SccAdapter::~SccAdapter() = default;
SccAdapter::SccAdapter(SccAdapter&&) noexcept = default;
SccAdapter& SccAdapter::operator=(SccAdapter&&) noexcept = default;

bool SccAdapter::valid() const noexcept {
    return impl_ && impl_->chip;
}

void SccAdapter::reset() noexcept {
    if (valid()) {
        SCC_reset(impl_->chip);
        SCC_set_type(impl_->chip, SCC_STANDARD);
    }
}

bool SccAdapter::write(
    std::uint8_t port,
    std::uint8_t address,
    std::uint8_t value) noexcept {
    if (!valid()) {
        return false;
    }
    std::uint16_t mapped{};
    switch (port) {
    case 0:
        if (address > 0x7F) {
            return false;
        }
        mapped = address;
        break;
    case 1:
        if (address > 9) {
            return false;
        }
        mapped = static_cast<std::uint16_t>(0xC0 + address);
        break;
    case 2:
        if (address > 4) {
            return false;
        }
        mapped = static_cast<std::uint16_t>(0xD0 + address);
        break;
    case 3:
        if (address != 0) {
            return false;
        }
        mapped = 0xE1;
        break;
    default:
        return false;
    }
    SCC_writeReg(impl_->chip, mapped, value);
    return true;
}

float SccAdapter::renderSample() noexcept {
    return valid()
        ? static_cast<float>(SCC_calc(impl_->chip)) * kInt16Scale
        : 0.0F;
}

struct Ym2413Adapter::Impl {
    OPLL* chip{};

    Impl()
        : chip(OPLL_new(kMasterClock, kSampleRate)) {}

    ~Impl() {
        OPLL_delete(chip);
    }
};

Ym2413Adapter::Ym2413Adapter()
    : impl_(std::make_unique<Impl>()) {
    if (valid()) {
        OPLL_reset(impl_->chip);
        OPLL_setChipType(impl_->chip, 0);
        OPLL_resetPatch(impl_->chip, OPLL_2413_TONE);
    }
}

Ym2413Adapter::~Ym2413Adapter() = default;
Ym2413Adapter::Ym2413Adapter(Ym2413Adapter&&) noexcept = default;
Ym2413Adapter& Ym2413Adapter::operator=(Ym2413Adapter&&) noexcept = default;

bool Ym2413Adapter::valid() const noexcept {
    return impl_ && impl_->chip;
}

void Ym2413Adapter::reset() noexcept {
    if (valid()) {
        OPLL_reset(impl_->chip);
        OPLL_setChipType(impl_->chip, 0);
        OPLL_resetPatch(impl_->chip, OPLL_2413_TONE);
    }
}

bool Ym2413Adapter::write(
    std::uint8_t address,
    std::uint8_t value) noexcept {
    if (!valid() || address > 0x38) {
        return false;
    }
    OPLL_writeReg(impl_->chip, address, value);
    return true;
}

float Ym2413Adapter::renderSample() noexcept {
    return valid()
        ? static_cast<float>(OPLL_calc(impl_->chip)) * kInt16Scale
        : 0.0F;
}

bool ChipRack::valid() const noexcept {
    return psg_.valid() && scc_.valid() && opll_.valid();
}

void ChipRack::reset() noexcept {
    psg_.reset();
    scc_.reset();
    opll_.reset();
}

bool ChipRack::apply(const RegisterWrite& write) noexcept {
    switch (write.chip) {
    case ChipId::Psg:
        return write.port == 0
            && psg_.write(write.address, write.value);
    case ChipId::Scc:
        return scc_.write(write.port, write.address, write.value);
    case ChipId::Opll:
        return write.port == 0
            && opll_.write(write.address, write.value);
    }
    return false;
}

bool ChipRack::apply(
    std::span<const RegisterWrite> writes) noexcept {
    for (const auto& write : writes) {
        if (!apply(write)) {
            return false;
        }
    }
    return true;
}

ChipSamples ChipRack::renderSample() noexcept {
    return {
        psg_.renderSample(),
        scc_.renderSample(),
        opll_.renderSample(),
    };
}

}  // namespace mgstc::engine
