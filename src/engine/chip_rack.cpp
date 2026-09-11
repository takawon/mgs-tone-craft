#include "mgstc/engine/chip_rack.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

#include "mgstc/engine/sinc_rate_conv.hpp"

extern "C" {
#include "emu2149.h"
#include "emu2212.h"
#include "emu2413.h"
}

namespace mgstc::engine {
namespace {

constexpr std::uint32_t kMasterClock = 3'579'545;
// Konami SCC uses the MSX master clock; f = clock / (32 * (TP + 1)).
// Using master/2 here drops every note by one octave vs MGSDRV period tables.
constexpr std::uint32_t kSccClock = kMasterClock;
constexpr std::uint32_t kSampleRate = 48'000;
constexpr float kInt16Scale = 1.0F / 32768.0F;

// YM2149 with internal /2 clock divider advances the tone generator at
// master/16. Match that cadence, then sinc-downsample to the engine rate
// (closer to MSXplay/libkss high-quality PSG path with MML lpf=0).
constexpr double kPsgNativeRate =
    static_cast<double>(kMasterClock) / 16.0;
// YM2413 native output is master/72. Feed that into the same external
// sinc path as PSG so OPLL is not left on emu2413's built-in converter alone.
constexpr double kOpllNativeRate =
    static_cast<double>(kMasterClock) / 72.0;

}  // namespace

struct Ym2149Adapter::Impl {
    PSG* chip{};
    std::optional<SincRateConv> rate_conv;

    Impl()
        : chip(PSG_new(
              kMasterClock,
              static_cast<std::uint32_t>(std::lround(kPsgNativeRate))))
        , rate_conv(
              chip != nullptr
                  ? std::optional<SincRateConv>(
                        SincRateConv(kPsgNativeRate, kSampleRate))
                  : std::nullopt) {}

    ~Impl() {
        PSG_delete(chip);
    }
};

Ym2149Adapter::Ym2149Adapter()
    : impl_(std::make_unique<Impl>()) {
    if (valid()) {
        PSG_setClockDivider(impl_->chip, 1);
        PSG_setVolumeMode(impl_->chip, 1);
        // Bypass emu2149's light internal converter; external sinc owns AA.
        PSG_setQuality(impl_->chip, 0);
        PSG_reset(impl_->chip);
        if (impl_->rate_conv) {
            impl_->rate_conv->reset();
        }
    }
}

Ym2149Adapter::~Ym2149Adapter() = default;
Ym2149Adapter::Ym2149Adapter(Ym2149Adapter&&) noexcept = default;
Ym2149Adapter& Ym2149Adapter::operator=(Ym2149Adapter&&) noexcept = default;

bool Ym2149Adapter::valid() const noexcept {
    return impl_ && impl_->chip && impl_->rate_conv.has_value();
}

void Ym2149Adapter::reset() noexcept {
    if (valid()) {
        PSG_reset(impl_->chip);
        impl_->rate_conv->reset();
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

float Ym2149Adapter::renderSample(std::span<float> channels) noexcept {
    if (!valid()) {
        return 0.0F;
    }
    return impl_->rate_conv->next([this, channels] {
        const float mixed = static_cast<float>(PSG_calc(impl_->chip)) * kInt16Scale;
        for (std::size_t ch = 0; ch < channels.size(); ++ch)
            channels[ch] = static_cast<float>(impl_->chip->ch_out[ch]) * kInt16Scale;
        return mixed;
    }, channels);
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
        // Match MSXplay live playback (kss-decoder-worker): scc quality off.
        SCC_set_quality(impl_->chip, 0);
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

float SccAdapter::renderSample(std::span<float> channels) noexcept {
    if (!valid()) return 0.0F;
    const float mixed = static_cast<float>(SCC_calc(impl_->chip)) * kInt16Scale;
    for (std::size_t ch = 0; ch < channels.size(); ++ch)
        channels[ch] = static_cast<float>(impl_->chip->ch_out[ch]) * kInt16Scale;
    return mixed;
}

struct Ym2413Adapter::Impl {
    OPLL* chip{};
    std::optional<SincRateConv> rate_conv;

    Impl()
        : chip(OPLL_new(
              kMasterClock,
              static_cast<std::uint32_t>(std::lround(kOpllNativeRate))))
        , rate_conv(
              chip != nullptr
                  ? std::optional<SincRateConv>(
                        SincRateConv(kOpllNativeRate, kSampleRate))
                  : std::nullopt) {}

    ~Impl() {
        OPLL_delete(chip);
    }
};

Ym2413Adapter::Ym2413Adapter()
    : impl_(std::make_unique<Impl>()) {
    if (valid()) {
        // Native rate disables emu2413's internal RateConv; external sinc
        // owns anti-alias filtering (same family as the PSG path).
        OPLL_reset(impl_->chip);
        OPLL_setChipType(impl_->chip, 0);
        OPLL_resetPatch(impl_->chip, OPLL_2413_TONE);
        impl_->rate_conv->reset();
    }
}

Ym2413Adapter::~Ym2413Adapter() = default;
Ym2413Adapter::Ym2413Adapter(Ym2413Adapter&&) noexcept = default;
Ym2413Adapter& Ym2413Adapter::operator=(Ym2413Adapter&&) noexcept = default;

bool Ym2413Adapter::valid() const noexcept {
    return impl_ && impl_->chip && impl_->rate_conv.has_value();
}

void Ym2413Adapter::reset() noexcept {
    if (valid()) {
        OPLL_reset(impl_->chip);
        OPLL_setChipType(impl_->chip, 0);
        OPLL_resetPatch(impl_->chip, OPLL_2413_TONE);
        impl_->rate_conv->reset();
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

float Ym2413Adapter::renderSample(std::span<float> channels) noexcept {
    if (!valid()) {
        return 0.0F;
    }
    return impl_->rate_conv->next([this, channels] {
        const float mixed = static_cast<float>(OPLL_calc(impl_->chip)) * kInt16Scale;
        for (std::size_t ch = 0; ch < channels.size(); ++ch)
            channels[ch] = static_cast<float>(impl_->chip->ch_out[ch]) * kInt16Scale;
        return mixed;
    }, channels);
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

ChipSamples ChipRack::renderSample(bool capture_channels) noexcept {
    ChipSamples result{};
    auto channels = std::span<float>(result.channels);
    result.psg = psg_.renderSample(capture_channels ? channels.first(3) : std::span<float>{});
    result.scc = scc_.renderSample(capture_channels ? channels.subspan(3, 5) : std::span<float>{});
    result.opll = opll_.renderSample(capture_channels ? channels.subspan(8, 14) : std::span<float>{});
    return result;
}

}  // namespace mgstc::engine
