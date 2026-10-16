// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

#include "mgstc/engine/engine_core.hpp"

namespace mgstc::app {

// Message thread only. One engine queue consumer publishes bounded history;
// each view keeps its own cursor, including a separate cursor for latest-only
// scope display. The engine's sequence may restart after a program change, so
// publication uses an independent monotonically increasing sequence.
class ScopeFrameDistributor final {
public:
    static constexpr std::size_t kHistoryFrames = 32;
    static constexpr std::size_t kMaxDrainFrames = 8;

    template<class Drain>
    [[nodiscard]] bool poll(engine::OpllScopeFrame& frame,
        std::uint64_t& cursor, Drain&& drain) noexcept {
        receive(drain);
        const auto next = std::max(cursor + 1, oldest_);
        if (next > published_) return false;
        frame = (*history_)[(next - 1) % kHistoryFrames];
        cursor = next;
        return true;
    }

    template<class Drain>
    [[nodiscard]] bool pollLatest(engine::OpllScopeFrame& frame,
        std::uint64_t& cursor, Drain&& drain) noexcept {
        receive(drain);
        if (published_ < oldest_ || cursor >= published_) return false;
        cursor = published_;
        frame = (*history_)[(cursor - 1) % kHistoryFrames];
        return true;
    }

    void clear() noexcept { oldest_ = published_ + 1; }

private:
    template<class Drain>
    void receive(Drain& drain) noexcept {
        engine::OpllScopeFrame frame{};
        for (std::size_t i = 0; i < kMaxDrainFrames; ++i) {
            if (!drain(frame)) break;
            ++published_;
            (*history_)[(published_ - 1) % kHistoryFrames] = frame;
            if (published_ >= kHistoryFrames)
                oldest_ = std::max(oldest_, published_ - kHistoryFrames + 1);
        }
    }

    std::unique_ptr<std::array<engine::OpllScopeFrame, kHistoryFrames>> history_{
        std::make_unique<std::array<engine::OpllScopeFrame, kHistoryFrames>>()};
    std::uint64_t published_{};
    std::uint64_t oldest_{1};
};

}  // namespace mgstc::app
