// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"

class SharedAudioHost {
public:
    virtual ~SharedAudioHost() = default;

    [[nodiscard]] virtual mgstc::engine::RealtimeEngineHost&
    engine() noexcept = 0;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    virtual void armOpllKeyOffForceSilence(std::uint8_t track) = 0;

    virtual void clearOpllKeyOffForceSilence() = 0;

    [[nodiscard]] virtual bool submitSharedEditorProgram(
        const mgstc::engine::SccWaveform& scc_wave,
        const mgstc::engine::OpllPatchParameters& opll_patch,
        bool retrigger,
        std::uint8_t retrigger_track,
        std::uint8_t midi_note) = 0;

    [[nodiscard]] virtual const mgstc::engine::SccWaveform&
    sharedSccWaveform() const noexcept = 0;

    [[nodiscard]] virtual const mgstc::engine::OpllPatchParameters&
    sharedOpllPatch() const noexcept = 0;
};
