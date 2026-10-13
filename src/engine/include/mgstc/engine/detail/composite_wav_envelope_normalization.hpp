// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/composite_timbre.hpp"

#include <algorithm>
#include <utility>

namespace mgstc::engine::detail {

// Only call on completed WAV converter output, after search/retention selection.
// Authored/imported sparse sequences keep their existing semantics.
inline bool normalizeWavConstantEnvelopes(CompositeTimbre& tone) {
    bool changed = false;
    for (auto& layer : tone.layers) {
        if ((layer.source != TimbreSource::Scc && layer.source != TimbreSource::Opll)
            || !layer.base_timbre || layer.base_timbre->source != layer.source
            || layer.base_opll_rom || layer.volume > 15
            || layer.volume_envelope.kind != EnvelopeKind::Sequence
            || layer.envelope_timeline.loop_start_count || layer.envelope_timeline.loop_end_count
            || !layer.pitch_envelope.events.empty()
            || !layer.modulation_profiles.empty() || !layer.register_auto_profiles.empty()
            || !layer.derived_command_placements.empty()
            || layer.pitch_modulation.enabled || layer.volume_modulation.enabled
            || layer.opll_tl_modulation.enabled || layer.opll_fb_modulation.enabled
            || layer.opll_tl_auto.active() || layer.opll_fb_auto.active()
            || layer.software_lfo.enabled || layer.pitch_sweep.enabled
            || layer.volume_envelope.events.size() != 1
            || layer.timbre_automation.size() > 1) continue;

        const auto& volume = layer.volume_envelope.events.front();
        if (volume.kind != EnvelopeEventKind::Volume || volume.count != 0
            || volume.automatic || volume.value < 0 || volume.value > 15) continue;
        if (!layer.timbre_automation.empty()) {
            const auto& initial = layer.timbre_automation.front();
            if (initial.kind != EnvelopeEventKind::Timbre || initial.count != 0
                || initial.timbre_pick != TimbrePick::Library
                || initial.target_library_id != layer.base_timbre->library_id
                || initial.scc_morph.enabled
                || initial.scc_morph.explicit_plan) continue;
        }
        if (!layerUsesSequenceEnvelope(layer)) continue;

        auto normalized = layer;
        normalized.volume = static_cast<std::uint8_t>(
            std::max(0, volume.value + static_cast<int>(layer.volume) - 15));
        normalized.timbre_automation.clear();
        // Full-scale is an identity marker, not an exported/active @e. Keeping
        // it avoids the authored sparse-sequence fallback that seeds ENV with v.
        normalized.volume_envelope.events = {
            EnvelopeEvent{.kind = EnvelopeEventKind::Volume, .value = 15, .count = 0}};
        if (layerUsesSequenceEnvelope(normalized)) continue;
        layer = std::move(normalized);
        changed = true;
    }
    return changed;
}

} // namespace mgstc::engine::detail
