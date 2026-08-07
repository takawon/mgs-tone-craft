#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "mgstc/engine/mamidi_rpc_client.hpp"
#include "mgstc/engine/register_write.hpp"

namespace mgstc::engine {

// MAmidiMEmo DirectAccessToChip DeviceIDs (Manual / InstrumentManager).
enum class MAmidiDeviceId : std::uint8_t {
    Scc = 7,
    Opll = 9,
    Psg = 11,
};

inline constexpr std::uint8_t kMAmidiPsgRegisterCount = 14;
inline constexpr std::uint8_t kMAmidiPsgMixerMask = 0x3F;
inline constexpr std::uint8_t kMAmidiOpllMaxAddress = 0x38;
inline constexpr std::uint8_t kMAmidiOpllChannelCount = 9;
inline constexpr std::uint8_t kMAmidiSccWaveMax = 0x7F;
inline constexpr std::uint8_t kMAmidiSccFreqCount = 10;
inline constexpr std::uint8_t kMAmidiSccVolumeCount = 5;
inline constexpr std::uint32_t kMAmidiSccFreqBase = 0x80;
inline constexpr std::uint32_t kMAmidiSccVolumeBase = 0x8A;
inline constexpr std::uint32_t kMAmidiSccKeyAddress = 0x8F;
inline constexpr std::uint32_t kMAmidiSccPlusOffset = 0x100;
inline constexpr std::size_t kMAmidiPsgSilenceWriteCount = 4;
// 9 key-offs + 9 volumes + rhythm control.
inline constexpr std::size_t kMAmidiOpllSilenceWriteCount =
    (kMAmidiOpllChannelCount * 2) + 1;
// 5 channel volumes + key mask.
inline constexpr std::size_t kMAmidiSccSilenceWriteCount =
    kMAmidiSccVolumeCount + 1;

struct MAmidiMapOptions {
    std::uint8_t unit_no{0};
    // When true, SCC addresses are offset by +0x100 (SCC+).
    bool scc_plus{false};
};

// Maps one MGSTC RegisterWrite to a DirectAccessToChip payload.
// SCC uses MAmidi absolute addresses (0x80/0x8A/0x8F), not emu2212's
// internal 0xC0/0xD0/0xE1 map.
[[nodiscard]] std::optional<MAmidiChipAccess> mapRegisterWriteToMAmidi(
    const RegisterWrite& write,
    MAmidiMapOptions options = {});

// Convenience overload kept for existing call sites (unit only, SCC).
[[nodiscard]] std::optional<MAmidiChipAccess> mapRegisterWriteToMAmidi(
    const RegisterWrite& write,
    std::uint8_t unit_no);

[[nodiscard]] std::string_view mapRegisterWriteError(
    const RegisterWrite& write);

// PSG silence: R7=0x3F (tone+noise off), R8–R10 volume=0.
void fillPsgSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiPsgSilenceWriteCount],
    std::uint8_t unit_no = 0) noexcept;

// OPLL silence: ch key-off (0x20+n=0), volume min (0x30+n=0x0F), rhythm=0.
void fillOpllSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiOpllSilenceWriteCount],
    std::uint8_t unit_no = 0) noexcept;

// SCC silence: volumes 0x8A–0x8E=0, key 0x8F=0 (+0x100 when SCC+).
void fillSccSilenceWrites(
    MAmidiChipAccess (&out)[kMAmidiSccSilenceWriteCount],
    std::uint8_t unit_no = 0,
    bool scc_plus = false) noexcept;

}  // namespace mgstc::engine
