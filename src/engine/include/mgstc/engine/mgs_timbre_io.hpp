#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"

namespace mgstc::engine {

struct MgsOpllDefinition {
    std::uint16_t number{};
    OpllPatchParameters patch{};
};

struct MgsSccDefinition {
    std::uint16_t number{};
    SccWaveform waveform{};
};

[[nodiscard]] std::string formatMgsOpllDefinition(
    const OpllPatchParameters& patch,
    std::uint16_t number,
    std::string_view voice_name);

[[nodiscard]] std::string formatMgsSccDefinition(
    const SccWaveform& waveform,
    std::uint16_t number,
    std::string_view voice_name);

[[nodiscard]] std::optional<MgsOpllDefinition>
parseMgsOpllDefinition(std::string_view text);

[[nodiscard]] std::optional<MgsSccDefinition>
parseMgsSccDefinition(std::string_view text);

}  // namespace mgstc::engine
