#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"

namespace mgstc::engine {

enum class ImportedToneType : std::uint8_t {
    Opll,
    Scc,
    Composite,
};

enum class ToneImportFormat : std::uint8_t {
    Unknown,
    Mgs,
    MgsMml,
    MusicaVcd,
    SccMusixxSng,
    Vgm,
    Vgz,
};

enum class ImportRegisterAs : std::uint8_t {
    Opll,
    Scc,
    Composite,
};

struct ImportWarning {
    std::string message;
};

using ImportedToneData = std::variant<
    OpllPatchParameters,
    SccWaveform,
    CompositeTimbre>;

struct ImportedToneCandidate {
    ImportedToneType type{ImportedToneType::Opll};
    std::string name;
    bool favorite{};
    bool registered{};
    std::string source_format;
    ImportedToneData data;
    // Present when a MuSICA SCC row can also be stored as a composite.
    std::optional<CompositeTimbre> composite_alternative;
    ImportRegisterAs default_register_as{ImportRegisterAs::Opll};
    std::vector<ImportRegisterAs> register_choices;
    std::vector<ImportWarning> warnings;
};

struct ToneImportResult {
    ToneImportFormat format{ToneImportFormat::Unknown};
    std::string source_name;
    std::vector<ImportedToneCandidate> candidates;
    std::vector<std::string> errors;

    [[nodiscard]] bool valid() const noexcept {
        return errors.empty();
    }
};

// VGM candidate window: same-block writes may include unrelated chip I/O and
// short waits. Crossing these limits means the bytes are not one timbre load.
inline constexpr std::uint32_t kVgmCandidateMaxSamples = 2'940;
inline constexpr std::uint32_t kVgmCandidateMaxCommands = 48;

[[nodiscard]] const char* toneImportFormatName(ToneImportFormat format) noexcept;

[[nodiscard]] ToneImportFormat detectToneImportFormat(
    std::span<const std::uint8_t> bytes,
    std::string_view extension = {});

[[nodiscard]] ToneImportResult importTones(
    std::span<const std::uint8_t> bytes,
    std::string_view extension = {},
    std::string_view source_name = {});

[[nodiscard]] TimbreLibraryEntry makeImportedLibraryEntry(
    const ImportedToneCandidate& candidate,
    ImportRegisterAs register_as);

[[nodiscard]] std::optional<CompositeTimbre> makeImportedComposite(
    const ImportedToneCandidate& candidate,
    ImportRegisterAs register_as);

[[nodiscard]] SccWaveform sccWaveformFromBytes(
    const std::array<std::uint8_t, 32>& bytes) noexcept;

[[nodiscard]] std::array<std::uint8_t, 32> sccWaveformToBytes(
    const SccWaveform& waveform) noexcept;

}  // namespace mgstc::engine
