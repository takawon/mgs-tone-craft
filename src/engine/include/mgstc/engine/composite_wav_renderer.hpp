// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_core.hpp"

namespace mgstc::engine {

enum class CompositeWavRenderError : std::uint8_t {
    None,
    InvalidOptions,
    InvalidNote,
    ProgramCompile,
    EngineUnavailable,
    Runtime,
    Cancelled,
    ResourceLimit,
};

struct CompositeWavRenderOptions {
    static constexpr std::uint32_t kSampleRate = TickClock::kSampleRate;
    // Offline evaluation is deliberately bounded, including output allocation.
    static constexpr std::size_t kMaximumFrameCount = kSampleRate * 120;

    std::uint8_t midi_note{60};
    std::size_t key_off_frame{24'000};
    std::size_t frame_count{48'000};
    MixerGains gains{};
    const TimbreLibrary* library{nullptr};
    const std::atomic_bool* cancel{nullptr};
};

struct CompositeWavRenderResult {
    std::vector<float> mono_pcm;
    std::vector<float> stereo_pcm;
    CompositeWavRenderError error{CompositeWavRenderError::None};
    bool clipped{};
    std::size_t requested_key_off_frame{};
    // RuntimeSession applies keys at the next 60 Hz boundary. Layer delays
    // shift both key-on and key-off, as in standalone composite audition.
    std::size_t effective_key_off_frame{};
    RenderResult engine_result{};

    [[nodiscard]] bool ok() const noexcept {
        return error == CompositeWavRenderError::None;
    }
};

// Worker-thread only. Uses a fresh local EngineCore and the ordinary composite
// compiler (including SCC morph materialization). Never changes authoring data
// or the active realtime/remote-device session. Failure/cancellation returns no
// partial PCM. The selected end frame is exact; key events retain 60 Hz timing.
[[nodiscard]] CompositeWavRenderResult renderCompositeWav(
    const CompositeTimbre& source,
    const CompositeWavRenderOptions& options = {});

}  // namespace mgstc::engine
