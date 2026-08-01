#include "mgstc/engine/engine_core.hpp"

#include <algorithm>
#include <cmath>

namespace mgstc::engine {

bool EngineCore::validGain(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
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
    chips_.reset();
    clock_.reset();
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
            if (!chips_.apply(session_.writes())) {
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

        const auto chips = chips_.renderSample();
        const auto raw = gains_.master * (
            chips.psg * gains_.psg
            + chips.scc * gains_.scc
            + chips.opll * gains_.opll);
        const auto mixed = std::clamp(raw, -1.0F, 1.0F);
        const auto scope_index = opll_scope_position_++;
        psg_scope_work_[scope_index] = chips.psg;
        scc_scope_work_[scope_index] = chips.scc;
        opll_scope_work_[scope_index] = chips.opll;
        mixed_scope_work_[scope_index] = mixed;
        if (opll_scope_position_ == opll_scope_work_.size()) {
            opll_scope_completed_.psg_samples = psg_scope_work_;
            opll_scope_completed_.scc_samples = scc_scope_work_;
            opll_scope_completed_.samples = opll_scope_work_;
            opll_scope_completed_.mixed_samples = mixed_scope_work_;
            opll_scope_completed_.sequence = ++opll_scope_sequence_;
            opll_scope_position_ = 0;
            opll_scope_ready_ = true;
        }
        result.clipped = result.clipped || mixed != raw;
        interleaved_stereo[frame * 2] = mixed;
        interleaved_stereo[frame * 2 + 1] = mixed;
        static_cast<void>(clock_.consumeFrames(1));
    }
    result.frames = frame_count;
    return result;
}

}  // namespace mgstc::engine
