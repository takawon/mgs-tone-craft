// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace mgstc::plugin {

// 48 kHz engine frames from layer start_delay_ms. 1 ms = 48 frames.
[[nodiscard]] std::uint64_t delayMillisecondsToEngineFrames(
    double start_delay_ms) noexcept;

struct PendingLayerEvent {
    bool pending{};
    std::uint64_t due_frame{};
    std::uint8_t voice_index{};
    std::uint8_t physical_track{};
    std::uint8_t midi_note{};
};

// Fixed-capacity delayed-layer scheduler. One slot per physical track
// (PSG 0–2, SCC 3–7, OPLL 8–16). Audio-thread safe: no heap traffic.
class LayerDelayScheduler {
public:
    static constexpr std::size_t kPhysicalTrackCount = 17;

    void schedule(
        std::uint8_t physical_track,
        std::uint64_t due_frame,
        std::uint8_t voice_index,
        std::uint8_t midi_note) noexcept;

    void cancelTrack(std::uint8_t physical_track) noexcept;
    void cancelVoice(std::uint8_t voice_index) noexcept;
    void cancelAll() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool isPending(std::uint8_t physical_track) const noexcept;
    [[nodiscard]] std::size_t pendingCount() const noexcept;
    [[nodiscard]] const PendingLayerEvent* event(
        std::uint8_t physical_track) const noexcept;

    // Smallest due in (after_exclusive, end_inclusive], if any.
    [[nodiscard]] std::optional<std::uint64_t> nextDueFrame(
        std::uint64_t after_exclusive,
        std::uint64_t end_inclusive) const noexcept;

    // Takes still-pending events with due_frame <= frame (track order).
    std::size_t takeDueAt(
        std::uint64_t frame,
        std::span<PendingLayerEvent> out) noexcept;

private:
    [[nodiscard]] static bool validTrack(std::uint8_t physical_track) noexcept {
        return physical_track < kPhysicalTrackCount;
    }

    std::array<PendingLayerEvent, kPhysicalTrackCount> slots_{};
};

}  // namespace mgstc::plugin
