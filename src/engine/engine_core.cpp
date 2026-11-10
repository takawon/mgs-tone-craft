#include "mgstc/engine/engine_core.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace mgstc::engine {

bool EngineCore::validGain(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

bool EngineCore::writeActiveRegisters(
    std::span<const RegisterWrite> writes) noexcept {
    // Keep every ordered write in the mirror even when its waveform is hidden.
    // Offline bounces can begin between notes, without a route reinitialization.
    const bool mirrored = emulator_.writeRegisters(writes);
    return mirrored && (output_backend_ == nullptr || offline_rendering_
        || output_backend_->writeRegisters(writes));
}

ChipSamples EngineCore::renderScopeSample(std::uint32_t channel_mask) noexcept {
    return emulator_.renderSampleMask(channel_mask);
}

void EngineCore::setScopeSelection(std::uint32_t mask, std::uint64_t context) noexcept {
    mask &= (1U << RuntimeSession::kTrackCount) - 1U;
    if (scope_requested_track_mask_ == mask && scope_selection_context_ == context) return;
    if (scope_selection_context_ != context) scope_waiting_track_mask_ = 0;
    scope_requested_track_mask_ = mask;
    scope_waiting_track_mask_ &= mask;
    scope_track_mask_ = mask & ~scope_waiting_track_mask_;
    scope_selection_context_ = context;
    opll_scope_position_ = 0;
    opll_scope_ready_ = false;
}

void EngineCore::awaitScopeNoteOn(std::uint8_t track) noexcept {
    if (track >= RuntimeSession::kTrackCount) return;
    const auto bit = std::uint32_t{1} << track;
    if ((scope_requested_track_mask_ & bit) == 0) return;
    scope_waiting_track_mask_ |= bit;
    scope_waiting_generations_[track] = session_.noteStartGeneration(track);
    scope_track_mask_ &= ~bit;
    resetScopeCapture();
}

bool EngineCore::setGains(MixerGains gains) noexcept {
    if (!validGain(gains.master)
        || !validGain(gains.psg)
        || !validGain(gains.scc)
        || !validGain(gains.opll)) {
        return false;
    }
    gains_ = gains;
    return true;
}

void EngineCore::hardReset() noexcept {
    session_.resetForKeyOn();
    if (output_backend_ != nullptr && !offline_rendering_) {
        output_backend_->reset();
    }
    emulator_.reset();
    clock_.reset();
    mix_dc_blocker_.reset();
    analysis_mix_dc_blocker_.reset();
    analysis_context_ = 0;
    psg_scope_work_.fill(0.0F);
    scc_scope_work_.fill(0.0F);
    opll_scope_work_.fill(0.0F);
    mixed_scope_work_.fill(0.0F);
    opll_scope_completed_ = {};
    opll_scope_position_ = 0;
    opll_scope_sequence_ = 0;
    opll_scope_ready_ = false;
    scope_track_mask_ = 0;
    scope_requested_track_mask_ = 0;
    scope_waiting_track_mask_ = 0;
    scope_selection_context_ = 0;
}

bool EngineCore::takeOpllScopeFrame(
    OpllScopeFrame& frame) noexcept {
    if (!opll_scope_ready_) {
        return false;
    }
    frame = opll_scope_completed_;
    opll_scope_ready_ = false;
    return true;
}

RenderResult EngineCore::render(
    std::span<float> interleaved_stereo, SpectrumCapture* capture) noexcept {
    RenderResult result{};
    if ((interleaved_stereo.size() % 2) != 0) {
        std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0.0F);
        result.error = RenderError::InvalidBuffer;
        return result;
    }

    const bool remote_output = output_backend_ != nullptr && !offline_rendering_;
    const bool hide_monitor = remote_output && !waveform_monitor_;
    const bool fill_scope = opll_scope_enabled_;
    const auto channel_mask = (capture != nullptr && capture->channelsEnabled())
        ? 0x3FFFFFU : (fill_scope && !hide_monitor ? scope_requested_track_mask_ : 0U);
    const auto frame_count = interleaved_stereo.size() / 2;
    for (std::size_t frame = 0; frame < frame_count; ) {
        if (clock_.tickDue()) {
            result.tick_result = session_.processTick();
            if (!result.tick_result.ok()) {
                std::fill(
                    interleaved_stereo.begin()
                        + static_cast<std::ptrdiff_t>(frame * 2),
                    interleaved_stereo.end(),
                    0.0F);
                result.error = RenderError::Runtime;
                result.frames = frame;
                return result;
            }
            if (!writeActiveRegisters(session_.writes())) {
                std::fill(
                    interleaved_stereo.begin()
                        + static_cast<std::ptrdiff_t>(frame * 2),
                    interleaved_stereo.end(),
                    0.0F);
                result.error = RenderError::ChipWrite;
                result.frames = frame;
                return result;
            }
            const auto previous_mask = scope_track_mask_;
            for (auto bits = scope_waiting_track_mask_; bits != 0; bits &= bits - 1U) {
                const auto track = std::countr_zero(bits);
                if (session_.noteStartGeneration(static_cast<std::uint8_t>(track))
                    != scope_waiting_generations_[track])
                    scope_waiting_track_mask_ &= ~(1U << track);
            }
            scope_track_mask_ = scope_requested_track_mask_ & ~scope_waiting_track_mask_;
            if (scope_track_mask_ != previous_mask) resetScopeCapture();
            static_cast<void>(clock_.beginTick());
        }

        const auto until_tick = clock_.framesUntilNextTick();
        if (until_tick == 0) {
            std::fill(
                interleaved_stereo.begin()
                    + static_cast<std::ptrdiff_t>(frame * 2),
                interleaved_stereo.end(),
                0.0F);
            result.error = RenderError::Runtime;
            result.frames = frame;
            return result;
        }
        const auto run = std::min(
            frame_count - frame,
            static_cast<std::size_t>(until_tick));
        for (std::size_t index = 0; index < run; ++index) {
            const auto out_frame = frame + index;
            const auto local_chips = renderScopeSample(channel_mask);
            const auto scope_chips = hide_monitor ? silentSample() : local_chips;
            const auto scope_raw = gains_.master * (
                local_chips.psg * gains_.psg
                + local_chips.scc * gains_.scc
                + local_chips.opll * gains_.opll);
            // Mild DC block on the mixed float bus (MSXplay/libkss-like output
            // stage with MML lpf=0: RCF off, DC filter still active).
            const auto scope_filtered = mix_dc_blocker_.process(scope_raw);
            const auto local_mixed = std::clamp(scope_filtered, -1.0F, 1.0F);
            const auto scope_mixed = hide_monitor ? 0.0F : local_mixed;
            if (capture != nullptr) {
                if (analysis_context_ != capture->context()) {
                    analysis_mix_dc_blocker_.reset();
                    analysis_context_ = capture->context();
                }
                const auto mask = capture->sourceMask();
                const auto selected_raw = gains_.master * (
                    ((mask & 1) ? scope_chips.psg * gains_.psg : 0.0F)
                    + ((mask & 2) ? scope_chips.scc * gains_.scc : 0.0F)
                    + ((mask & 4) ? scope_chips.opll * gains_.opll : 0.0F));
                const auto selected = std::clamp(
                    analysis_mix_dc_blocker_.process(selected_raw),
                    -1.0F,
                    1.0F);
                // Full context taps the audible bus exactly. A restricted editor
                // uses the same mixer stages on its selected source PCM.
                capture->append(
                    scope_chips,
                    mask == 7 ? scope_mixed : selected,
                    gains_.master,
                    gains_.psg,
                    gains_.scc,
                    gains_.opll);
            }
            if (fill_scope) {
                const auto scope_index = opll_scope_position_++;
                scope_gain_work_[scope_index] = gains_;
                psg_scope_work_[scope_index] = scope_chips.psg;
                scc_scope_work_[scope_index] = scope_chips.scc;
                opll_scope_work_[scope_index] = scope_chips.opll;
                mixed_scope_work_[scope_index] = scope_mixed;
                for (auto bits = scope_track_mask_; bits != 0; bits &= bits - 1U) {
                    const auto track = std::countr_zero(bits);
                    physical_scope_work_[track][scope_index] = scope_chips.channels[track];
                }
                if (opll_scope_position_ == opll_scope_work_.size()) {
                    opll_scope_completed_.psg_samples = psg_scope_work_;
                    opll_scope_completed_.scc_samples = scc_scope_work_;
                    opll_scope_completed_.samples = opll_scope_work_;
                    opll_scope_completed_.mixed_samples = mixed_scope_work_;
                    opll_scope_completed_.mixer_gains = scope_gain_work_;
                    opll_scope_completed_.captured_track_mask = scope_track_mask_;
                    opll_scope_completed_.selection_context = scope_selection_context_;
                    for (auto bits = scope_track_mask_; bits != 0; bits &= bits - 1U) {
                        const auto track = std::countr_zero(bits);
                        opll_scope_completed_.physical_samples[track] = physical_scope_work_[track];
                        opll_scope_completed_.playback_positions[track] = session_.playbackPosition(
                            static_cast<std::uint8_t>(track));
                    }
                    opll_scope_completed_.sequence = ++opll_scope_sequence_;
                    opll_scope_completed_.sample_end = clock_.currentTick() * TickClock::kFramesPerTick
                        + (TickClock::kFramesPerTick - until_tick) + index + 1;
                    opll_scope_position_ = 0;
                    opll_scope_ready_ = true;
                }
            }

            // Audible destination is the remote chip; keep PC mix silent.
            const auto mixed = remote_output ? 0.0F : scope_mixed;
            result.clipped = result.clipped
                || (!remote_output && scope_mixed != scope_filtered);
            interleaved_stereo[out_frame * 2] = mixed;
            interleaved_stereo[out_frame * 2 + 1] = mixed;
        }
        static_cast<void>(
            clock_.consumeFrames(static_cast<std::uint32_t>(run)));
        frame += run;
    }
    result.frames = frame_count;
    return result;
}

}  // namespace mgstc::engine
