// SPDX-License-Identifier: AGPL-3.0-only

#include "layer_delay_scheduler.hpp"

#include <cmath>
#include <limits>

namespace mgstc::plugin {

std::uint64_t delayMillisecondsToEngineFrames(
    double start_delay_ms) noexcept {
    if (!(start_delay_ms > 0.0) || !std::isfinite(start_delay_ms)) {
        return 0;
    }
    const double frames = start_delay_ms * 48.0;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    if (frames >= static_cast<double>(kMax)) {
        return kMax;
    }
    const auto rounded = std::llround(frames);
    if (rounded <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(rounded);
}

void LayerDelayScheduler::schedule(
    std::uint8_t physical_track,
    std::uint64_t due_frame,
    std::uint8_t voice_index,
    std::uint8_t midi_note) noexcept {
    if (!validTrack(physical_track)) {
        return;
    }
    auto& slot = slots_[physical_track];
    slot.pending = true;
    slot.due_frame = due_frame;
    slot.voice_index = voice_index;
    slot.physical_track = physical_track;
    slot.midi_note = midi_note;
}

void LayerDelayScheduler::cancelTrack(
    std::uint8_t physical_track) noexcept {
    if (!validTrack(physical_track)) {
        return;
    }
    slots_[physical_track] = {};
}

void LayerDelayScheduler::cancelVoice(std::uint8_t voice_index) noexcept {
    for (auto& slot : slots_) {
        if (slot.pending && slot.voice_index == voice_index) {
            slot = {};
        }
    }
}

void LayerDelayScheduler::cancelAll() noexcept {
    slots_ = {};
}

void LayerDelayScheduler::reset() noexcept {
    cancelAll();
}

bool LayerDelayScheduler::isPending(
    std::uint8_t physical_track) const noexcept {
    if (!validTrack(physical_track)) {
        return false;
    }
    return slots_[physical_track].pending;
}

std::size_t LayerDelayScheduler::pendingCount() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : slots_) {
        if (slot.pending) {
            ++count;
        }
    }
    return count;
}

const PendingLayerEvent* LayerDelayScheduler::event(
    std::uint8_t physical_track) const noexcept {
    if (!validTrack(physical_track) || !slots_[physical_track].pending) {
        return nullptr;
    }
    return &slots_[physical_track];
}

std::optional<std::uint64_t> LayerDelayScheduler::nextDueFrame(
    std::uint64_t after_exclusive,
    std::uint64_t end_inclusive) const noexcept {
    std::optional<std::uint64_t> found;
    for (const auto& slot : slots_) {
        if (!slot.pending) {
            continue;
        }
        if (slot.due_frame <= after_exclusive
            || slot.due_frame > end_inclusive) {
            continue;
        }
        if (!found || slot.due_frame < *found) {
            found = slot.due_frame;
        }
    }
    return found;
}

std::size_t LayerDelayScheduler::takeDueAt(
    std::uint64_t frame,
    std::span<PendingLayerEvent> out) noexcept {
    std::size_t written = 0;
    for (auto& slot : slots_) {
        if (!slot.pending || slot.due_frame > frame) {
            continue;
        }
        if (written < out.size()) {
            out[written] = slot;
        }
        ++written;
        slot = {};
    }
    return written;
}

}  // namespace mgstc::plugin
