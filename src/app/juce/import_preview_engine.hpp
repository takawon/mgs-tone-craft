// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "shared_audio_host.hpp"

namespace mgstc::app {

namespace import_preview_detail {

inline void drainProgramEdits(
    mgstc::engine::RealtimeEngineHost& engine,
    SharedAudioHost& audio_service) {
    std::array<float, 256> drain{};
    engine.drainPendingCommands(
        drain,
        250,
        !audio_service.running());
}

}  // namespace import_preview_detail

// Build and activate a composite program for import-list preview without
// touching an editor's editing model.
[[nodiscard]] inline bool configureCompositeImportProgram(
    SharedAudioHost& audio_service,
    const mgstc::engine::CompositeTimbre& program_timbre_in,
    const mgstc::engine::TimbreLibrary* timbre_library) {
    auto& engine = audio_service.engine();
    import_preview_detail::drainProgramEdits(engine, audio_service);
    auto edit = engine.beginProgramEdit();
    if (!edit.valid()) {
        engine.discardStuckProgramEdits();
        edit = engine.beginProgramEdit();
    }
    if (!edit.valid()) {
        return false;
    }

    if (!mgstc::engine::compileCompositeProgram(
            *edit.engine,
            program_timbre_in,
            {
                .polyphonic = true,
                .library = timbre_library,
            })) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return false;
    }
    if (!engine.submitProgram(edit)) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return false;
    }
    audio_service.invalidateSharedEditorProgram();
    if (audio_service.running()) {
        if (!engine.waitForPendingProgramActivation()) {
            return false;
        }
    } else {
        import_preview_detail::drainProgramEdits(engine, audio_service);
    }
    return true;
}

[[nodiscard]] inline bool startCompositeImportNotes(
    mgstc::engine::RealtimeEngineHost& engine,
    const mgstc::engine::CompositeTimbre& timbre,
    std::uint8_t midi_note,
    std::vector<std::uint8_t>& sounding_tracks) {
    sounding_tracks.clear();
    const auto plan = mgstc::engine::buildCompositePlaybackPlan(timbre);
    for (const auto& binding : plan.audible_layers) {
        const auto& layer = timbre.layers[binding.layer_index];
        const auto note = mgstc::engine::layerMidiNote(layer, midi_note);
        if (!note) {
            continue;
        }
        const auto track = plan.physicalTrack(binding, 0);
        if (engine.submit(mgstc::engine::EngineCommand::noteOn(track, *note))) {
            sounding_tracks.push_back(track);
        }
    }
    return !sounding_tracks.empty();
}

[[nodiscard]] inline void stopCompositeImportNotes(
    mgstc::engine::RealtimeEngineHost& engine,
    std::vector<std::uint8_t>& sounding_tracks) {
    for (const auto track : sounding_tracks) {
        static_cast<void>(
            engine.submit(mgstc::engine::EngineCommand::noteOff(track)));
    }
    sounding_tracks.clear();
}

}  // namespace mgstc::app
