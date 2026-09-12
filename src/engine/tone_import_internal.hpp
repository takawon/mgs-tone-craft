#pragma once

#include "mgstc/engine/tone_import.hpp"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace mgstc::engine {
namespace tone_import_detail {

[[nodiscard]] bool allZero(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] std::string trimPaddedName(std::string_view text);

[[nodiscard]] std::string decodeCp932Name(std::span<const std::uint8_t> bytes);

[[nodiscard]] ImportedToneCandidate makeOpllCandidate(
    const OpllPatchParameters& patch,
    std::string name,
    std::string_view source_format);

[[nodiscard]] ImportedToneCandidate makeSccCandidate(
    const SccWaveform& waveform,
    std::string name,
    std::string_view source_format);

[[nodiscard]] ImportedToneCandidate makeCompositeCandidate(
    CompositeTimbre timbre,
    std::string_view source_format);

struct DefinedTones {
    std::map<unsigned, OpllPatchParameters> opll;
    std::map<unsigned, SccWaveform> scc;
};

[[nodiscard]] bool envelopeBytecodeToLayer(
    std::span<const std::uint8_t> bytecode,
    CompositeLayer& layer,
    std::vector<std::string>& issues,
    bool* dropped_psg_mode_noise = nullptr);

struct EnvelopeChipUse {
    bool psg{};
    bool scc{};
    bool opll{};
    std::set<unsigned> track_scc;
    std::set<unsigned> track_opll;
    // First music-track use: sounding @n, or nullopt for the chip default.
    std::optional<unsigned> primary_scc;
    std::optional<unsigned> primary_opll;

    [[nodiscard]] bool anyChip() const noexcept {
        return psg || scc || opll;
    }
};

[[nodiscard]] CompositeTimbre makeSelfContainedComposite(
    CompositeLayer layer,
    const DefinedTones& tones,
    std::string name,
    std::vector<ImportWarning>& warnings,
    const EnvelopeChipUse& usage = {});

void addCandidate(
    std::vector<ImportedToneCandidate>& candidates,
    ImportedToneCandidate candidate);

}  // namespace tone_import_detail

[[nodiscard]] ToneImportResult importMgsBinary(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] ToneImportResult importMgsMml(std::string_view text);

[[nodiscard]] ToneImportResult importMusicaVcd(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] ToneImportResult importSccMusixxSng(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] ToneImportResult importVgm(std::span<const std::uint8_t> bytes);

[[nodiscard]] bool looksLikeMgsBinary(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool looksLikeCompressedMgs(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool looksLikeMgsMml(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool looksLikeMusicaVcd(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool looksLikeSccMusixxSng(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool looksLikeVgm(std::span<const std::uint8_t> bytes);

}  // namespace mgstc::engine
