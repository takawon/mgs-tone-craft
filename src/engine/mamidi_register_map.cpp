#include "mgstc/engine/mamidi_register_map.hpp"

namespace mgstc::engine {
namespace {

[[nodiscard]] std::uint32_t psgData(const RegisterWrite& write) noexcept {
    auto value = static_cast<std::uint32_t>(write.value);
    if (write.address == 7) {
        value &= kMAmidiPsgMixerMask;
    }
    return value;
}

[[nodiscard]] std::optional<std::uint32_t> sccAbsoluteAddress(
    const RegisterWrite& write,
    bool scc_plus) noexcept {
    std::uint32_t address = 0;
    switch (write.port) {
    case 0:  // waveform RAM
        if (write.address > kMAmidiSccWaveMax) {
            return std::nullopt;
        }
        address = write.address;
        break;
    case 1:  // frequency
        if (write.address >= kMAmidiSccFreqCount) {
            return std::nullopt;
        }
        address = kMAmidiSccFreqBase + write.address;
        break;
    case 2:  // volume
        if (write.address >= kMAmidiSccVolumeCount) {
            return std::nullopt;
        }
        address = kMAmidiSccVolumeBase + write.address;
        break;
    case 3:  // key on/off mask
        if (write.address != 0) {
            return std::nullopt;
        }
        address = kMAmidiSccKeyAddress;
        break;
    default:
        return std::nullopt;
    }
    if (scc_plus) {
        address += kMAmidiSccPlusOffset;
    }
    return address;
}

}  // namespace

std::optional<MAmidiChipAccess> mapRegisterWriteToMAmidi(
    const RegisterWrite& write,
    std::uint8_t unit_no) {
    return mapRegisterWriteToMAmidi(
        write,
        MAmidiMapOptions{.unit_no = unit_no, .scc_plus = false});
}

std::optional<MAmidiChipAccess> mapRegisterWriteToMAmidi(
    const RegisterWrite& write,
    MAmidiMapOptions options) {
    switch (write.chip) {
    case ChipId::Psg:
        if (write.address >= kMAmidiPsgRegisterCount) {
            return std::nullopt;
        }
        return MAmidiChipAccess{
            static_cast<std::uint8_t>(MAmidiDeviceId::Psg),
            options.unit_no,
            write.address,
            psgData(write),
        };
    case ChipId::Opll:
        if (write.address > kMAmidiOpllMaxAddress) {
            return std::nullopt;
        }
        return MAmidiChipAccess{
            static_cast<std::uint8_t>(MAmidiDeviceId::Opll),
            options.unit_no,
            write.address,
            write.value,
        };
    case ChipId::Scc: {
        const auto address =
            sccAbsoluteAddress(write, options.scc_plus);
        if (!address.has_value()) {
            return std::nullopt;
        }
        return MAmidiChipAccess{
            static_cast<std::uint8_t>(MAmidiDeviceId::Scc),
            options.unit_no,
            *address,
            write.value,
        };
    }
    }
    return std::nullopt;
}

std::string_view mapRegisterWriteError(const RegisterWrite& write) {
    switch (write.chip) {
    case ChipId::Psg:
        if (write.address >= kMAmidiPsgRegisterCount) {
            return "PSG register address out of range";
        }
        return "PSG register mapping failed";
    case ChipId::Opll:
        if (write.address > kMAmidiOpllMaxAddress) {
            return "OPLL register address out of range";
        }
        return "OPLL register mapping failed";
    case ChipId::Scc:
        return "SCC register port/address out of range";
    }
    return "unknown chip register mapping";
}

void fillPsgSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiPsgSilenceWriteCount],
    std::uint8_t unit_no) noexcept {
    const auto device = static_cast<std::uint8_t>(MAmidiDeviceId::Psg);
    out[0] = {device, unit_no, 7, kMAmidiPsgMixerMask};
    out[1] = {device, unit_no, 8, 0};
    out[2] = {device, unit_no, 9, 0};
    out[3] = {device, unit_no, 10, 0};
}

void fillOpllSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiOpllSilenceWriteCount],
    std::uint8_t unit_no) noexcept {
    const auto device = static_cast<std::uint8_t>(MAmidiDeviceId::Opll);
    std::size_t index = 0;
    for (std::uint8_t channel = 0; channel < kMAmidiOpllChannelCount;
         ++channel) {
        out[index++] = {
            device,
            unit_no,
            static_cast<std::uint32_t>(0x20 + channel),
            0,
        };
        out[index++] = {
            device,
            unit_no,
            static_cast<std::uint32_t>(0x30 + channel),
            0x0F,
        };
    }
    out[index] = {device, unit_no, 0x0E, 0};
}

void fillSccSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiSccSilenceWriteCount],
    std::uint8_t unit_no,
    bool scc_plus) noexcept {
    const auto device = static_cast<std::uint8_t>(MAmidiDeviceId::Scc);
    const std::uint32_t offset = scc_plus ? kMAmidiSccPlusOffset : 0;
    for (std::uint8_t channel = 0; channel < kMAmidiSccVolumeCount;
         ++channel) {
        out[channel] = {
            device,
            unit_no,
            kMAmidiSccVolumeBase + channel + offset,
            0,
        };
    }
    out[kMAmidiSccVolumeCount] = {
        device,
        unit_no,
        kMAmidiSccKeyAddress + offset,
        0,
    };
}

}  // namespace mgstc::engine
