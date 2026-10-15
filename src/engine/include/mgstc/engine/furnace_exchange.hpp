#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/tone_import.hpp"

namespace mgstc::engine {

// Furnace 0.6.8.3 instrument (.fui) and wavetable (.fuw) exchange.
// File layout follows Furnace's format papers. Sample scaling follows the
// SCC register write used by that version: a 0..255 level becomes the signed
// chip byte (level - 128). GPL sources are not included.

inline constexpr std::uint16_t kFurnaceEngineVersion0683 = 232;

struct FurnaceExportFile {
    std::string filename;
    std::vector<std::uint8_t> bytes;
};

struct FurnaceExportBatch {
    std::vector<FurnaceExportFile> files;
    std::vector<std::string> warnings;
    bool partial{false};
};

[[nodiscard]] ToneImportFormat detectFurnaceToneFormat(
    std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] ToneImportResult importFurnaceTones(
    std::span<const std::uint8_t> bytes,
    std::string_view source_name = {});

[[nodiscard]] std::vector<std::uint8_t> writeFurnaceWavetable(
    std::string_view name,
    const SccWaveform& waveform);

[[nodiscard]] FurnaceExportBatch exportFurnaceInstruments(
    const TimbreLibraryEntry& entry);

[[nodiscard]] FurnaceExportBatch exportFurnaceInstruments(
    const CompositeTimbre& timbre);

[[nodiscard]] FurnaceExportBatch exportFurnaceWavetables(
    const TimbreLibraryEntry& entry);

// SCC `.fuw` files in first-seen `@e` order. Morph intermediates are included
// only when the compiled envelope names them. The library is not modified.
[[nodiscard]] FurnaceExportBatch exportFurnaceWavetables(
    const CompositeTimbre& timbre);

}  // namespace mgstc::engine
