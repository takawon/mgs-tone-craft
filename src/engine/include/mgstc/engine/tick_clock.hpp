#pragma once

#include <algorithm>
#include <cstdint>

#include "mgstc/engine/event.hpp"

namespace mgstc::engine {

class TickClock {
public:
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::uint32_t kTicksPerSecond = 60;
    static constexpr std::uint32_t kFramesPerTick =
        kSampleRate / kTicksPerSecond;

    [[nodiscard]] bool tickDue() const noexcept {
        return tick_due_;
    }

    [[nodiscard]] Tick currentTick() const noexcept {
        return tick_;
    }

    [[nodiscard]] std::uint32_t framesUntilNextTick() const noexcept {
        return frames_remaining_;
    }

    bool beginTick() noexcept {
        if (!tick_due_) {
            return false;
        }
        frames_remaining_ = kFramesPerTick;
        tick_due_ = false;
        return true;
    }

    std::uint32_t consumeFrames(std::uint32_t requested) noexcept {
        if (tick_due_ || requested == 0) {
            return 0;
        }
        const auto consumed = std::min(requested, frames_remaining_);
        frames_remaining_ -= consumed;
        if (frames_remaining_ == 0) {
            ++tick_;
            tick_due_ = true;
        }
        return consumed;
    }

    void reset() noexcept {
        tick_ = 0;
        frames_remaining_ = 0;
        tick_due_ = true;
    }

private:
    Tick tick_{};
    std::uint32_t frames_remaining_{};
    bool tick_due_{true};
};

static_assert(TickClock::kFramesPerTick == 800);

}  // namespace mgstc::engine
