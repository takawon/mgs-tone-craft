// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_wav_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

#include "mgstc/engine/composite_program_compiler.hpp"

namespace mgstc::engine {
namespace {

constexpr std::size_t kTickFrames = TickClock::kFramesPerTick;

std::size_t nextTickFrame(std::size_t frame) noexcept {
    return ((frame + kTickFrames - 1) / kTickFrames) * kTickFrames;
}

bool cancelled(const CompositeWavRenderOptions& options) noexcept {
    return options.cancel != nullptr
        && options.cancel->load(std::memory_order_relaxed);
}

struct LayerKeys {
    std::uint8_t track{};
    std::uint8_t note{};
    std::size_t on_frame{};
    std::size_t off_frame{};
};

}  // namespace

CompositeWavRenderResult renderCompositeWav(
    const CompositeTimbre& source,
    const CompositeWavRenderOptions& options) {
    CompositeWavRenderResult result;
    result.requested_key_off_frame = options.key_off_frame;
    auto fail = [&result](CompositeWavRenderError error) {
        result.error = error;
        result.mono_pcm.clear();
        result.stereo_pcm.clear();
        return std::move(result);
    };
    if (options.frame_count == 0
        || options.frame_count > CompositeWavRenderOptions::kMaximumFrameCount
        || options.key_off_frame == 0
        || options.key_off_frame > options.frame_count) {
        return fail(CompositeWavRenderError::InvalidOptions);
    }
    result.effective_key_off_frame = nextTickFrame(options.key_off_frame);
    if (options.midi_note < 24 || options.midi_note > 119) {
        return fail(CompositeWavRenderError::InvalidNote);
    }
    if (cancelled(options)) {
        return fail(CompositeWavRenderError::Cancelled);
    }

    try {
        // A new emulator, session, clock and DC-filter history for every
        // candidate. No active device routing or preceding FM/phase state.
        EngineCore engine;
        if (!engine.valid()) {
            return fail(CompositeWavRenderError::EngineUnavailable);
        }
        if (!engine.setGains(options.gains)) {
            return fail(CompositeWavRenderError::InvalidOptions);
        }
        engine.setOpllScopeEnabled(false);
        CompositePlaybackPlan plan;
        if (!compileCompositeProgram(
                engine, source, {.library = options.library}, &plan)
            || plan.audible_layers.empty()
            || plan.audible_layers.size() > RuntimeSession::kTrackCount) {
            return fail(CompositeWavRenderError::ProgramCompile);
        }
        if (cancelled(options)) {
            return fail(CompositeWavRenderError::Cancelled);
        }
        engine.session().gateUntilNoteOn();

        std::array<LayerKeys, RuntimeSession::kTrackCount> keys{};
        std::size_t key_count = 0;
        for (const auto& binding : plan.audible_layers) {
            const auto note = layerMidiNote(
                source.layers[binding.layer_index], options.midi_note);
            // The ordinary composite audition omits transposed layers that
            // fall outside the engine's supported C1..B8 note range.
            if (!note) {
                continue;
            }
            if (!std::isfinite(binding.start_delay_ms)
                || binding.start_delay_ms < 0.0) {
                return fail(CompositeWavRenderError::InvalidOptions);
            }
            // Once a delay lies beyond the output end it never fires. Clamp
            // only this scheduling sentinel, before converting to size_t.
            const double delayed_frames = std::min(
                binding.start_delay_ms * 48.0,
                static_cast<double>(options.frame_count + kTickFrames));
            const auto delay = static_cast<std::size_t>(
                std::llround(delayed_frames));
            keys[key_count++] = {
                .track = plan.physicalTrack(binding, 0),
                .note = *note,
                .on_frame = nextTickFrame(delay),
                .off_frame = nextTickFrame(delay + options.key_off_frame),
            };
        }
        if (key_count == 0) {
            return fail(CompositeWavRenderError::InvalidNote);
        }

        result.stereo_pcm.resize(options.frame_count * 2);
        result.mono_pcm.resize(options.frame_count);
        for (std::size_t frame = 0; frame < options.frame_count; ) {
            if (cancelled(options)) {
                return fail(CompositeWavRenderError::Cancelled);
            }
            // Queue keys immediately before EngineCore's own Tick processing;
            // never process the session separately or replay final registers.
            for (std::size_t index = 0; index < key_count; ++index) {
                const auto& key = keys[index];
                if (key.on_frame == frame
                    && !engine.session().queueNoteOn(key.track, key.note)) {
                    return fail(CompositeWavRenderError::Runtime);
                }
                if (key.off_frame == frame
                    && !engine.session().queueKeyOff(key.track)) {
                    return fail(CompositeWavRenderError::Runtime);
                }
            }
            const auto count = std::min(kTickFrames, options.frame_count - frame);
            result.engine_result = engine.render(std::span<float>(
                result.stereo_pcm.data() + frame * 2, count * 2));
            result.clipped = result.clipped || result.engine_result.clipped;
            if (!result.engine_result.ok() || result.engine_result.frames != count) {
                return fail(CompositeWavRenderError::Runtime);
            }
            for (std::size_t index = 0; index < count; ++index) {
                const auto offset = (frame + index) * 2;
                const float left = result.stereo_pcm[offset];
                const float right = result.stereo_pcm[offset + 1];
                if (!std::isfinite(left) || !std::isfinite(right)) {
                    return fail(CompositeWavRenderError::Runtime);
                }
                result.mono_pcm[frame + index] = (left + right) * 0.5F;
            }
            frame += count;
        }
        if (cancelled(options)) {
            return fail(CompositeWavRenderError::Cancelled);
        }
    } catch (const std::bad_alloc&) {
        return fail(CompositeWavRenderError::ResourceLimit);
    }
    return result;
}

}  // namespace mgstc::engine
