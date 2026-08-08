#include "mgstc/engine/engine_core.hpp"

#include <algorithm>
#include <cmath>

namespace mgstc::engine {

bool EngineCore::validGain(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

bool EngineCore::writeActiveRegisters(
    std::span<const RegisterWrite> writes) noexcept {
    if (output_backend_ == nullptr) {
        return emulator_.writeRegisters(writes);
    }
    if (!output_backend_->writeRegisters(writes)) {
        return false;
    }
    if (waveform_monitor_) {
        return emulator_.writeRegisters(writes);
    }
    return true;
}

ChipSamples EngineCore::renderScopeSample() noexcept {
    if (output_backend_ == nullptr) {
        return emulator_.renderSample();
    }
    if (waveform_monitor_) {
        return emulator_.renderSample();
    }
    return silentSample();
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
    if (output_backend_ != nullptr) {
        output_backend_->reset();
        if (waveform_monitor_) {
            emulator_.reset();
        } else {
            emulator_.allNotesOff();
        }
    } else {
        emulator_.reset();
    }
    clock_.reset();
    mix_dc_blocker_.reset();
    psg_scope_work_.fill(0.0F);
    scc_scope_work_.fill(0.0F);
    opll_scope_work_.fill(0.0F);
    mixed_scope_work_.fill(0.0F);
    opll_scope_completed_ = {};
    opll_scope_position_ = 0;
    opll_scope_sequence_ = 0;
    opll_scope_ready_ = false;
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
    std::span<float> interleaved_stereo) noexcept {
    RenderResult result{};
    if ((interleaved_stereo.size() % 2) != 0) {
        std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0.0F);
        result.error = RenderError::InvalidBuffer;
        return result;
    }

    const bool remote_output = output_backend_ != nullptr;
    const auto frame_count = interleaved_stereo.size() / 2;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
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
            static_cast<void>(clock_.beginTick());
        }

        const auto scope_chips = renderScopeSample();
        const auto scope_raw = gains_.master * (
            scope_chips.psg * gains_.psg
            + scope_chips.scc * gains_.scc
            + scope_chips.opll * gains_.opll);
        // Mild DC block on the mixed float bus (MSXplay/libkss-like output
        // stage with MML lpf=0: RCF off, DC filter still active).
        const auto scope_filtered = mix_dc_blocker_.process(scope_raw);
        const auto scope_mixed = std::clamp(scope_filtered, -1.0F, 1.0F);
        const auto scope_index = opll_scope_position_++;
        psg_scope_work_[scope_index] = scope_chips.psg;
        scc_scope_work_[scope_index] = scope_chips.scc;
        opll_scope_work_[scope_index] = scope_chips.opll;
        mixed_scope_work_[scope_index] = scope_mixed;
        if (opll_scope_position_ == opll_scope_work_.size()) {
            opll_scope_completed_.psg_samples = psg_scope_work_;
            opll_scope_completed_.scc_samples = scc_scope_work_;
            opll_scope_completed_.samples = opll_scope_work_;
            opll_scope_completed_.mixed_samples = mixed_scope_work_;
            opll_scope_completed_.sequence = ++opll_scope_sequence_;
            opll_scope_position_ = 0;
            opll_scope_ready_ = true;
        }

        // Audible destination is the remote chip; keep PC mix silent.
        const auto mixed = remote_output ? 0.0F : scope_mixed;
        result.clipped = result.clipped
            || (!remote_output && scope_mixed != scope_filtered);
        interleaved_stereo[frame * 2] = mixed;
        interleaved_stereo[frame * 2 + 1] = mixed;
        static_cast<void>(clock_.consumeFrames(1));
    }
    result.frames = frame_count;
    return result;
}

}  // namespace mgstc::engine
