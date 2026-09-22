// SPDX-License-Identifier: AGPL-3.0-only

#include "layer_delay_scheduler.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

using mgstc::plugin::delayMillisecondsToEngineFrames;
using mgstc::plugin::LayerDelayScheduler;
using mgstc::plugin::PendingLayerEvent;

void testDelayConversion() {
    require(delayMillisecondsToEngineFrames(0.0) == 0, "0 ms -> 0 frames");
    require(delayMillisecondsToEngineFrames(-1.0) == 0, "negative -> 0");
    require(delayMillisecondsToEngineFrames(1.0) == 48, "1 ms -> 48");
    require(delayMillisecondsToEngineFrames(10.0) == 480, "10 ms -> 480");
    require(delayMillisecondsToEngineFrames(0.5) == 24, "0.5 ms -> 24");
    require(delayMillisecondsToEngineFrames(200.0) == 9600, "200 ms -> 9600");
    require(
        delayMillisecondsToEngineFrames(1000.0 / 60.0) == 800,
        "1/60 s tick -> 800 frames");
    require(
        delayMillisecondsToEngineFrames(1.0 / 48.0) == 1,
        "1/48 ms rounds to 1 frame");
    const auto half_up = delayMillisecondsToEngineFrames(0.0104166666667);
    require(
        half_up == 0 || half_up == 1,
        "sub-frame delay must round to 0 or 1");
    require(
        delayMillisecondsToEngineFrames(0.0104166666667)
            == static_cast<std::uint64_t>(std::llround(0.0104166666667 * 48.0)),
        "conversion matches llround(ms * 48)");
}

void testExactDueFrame() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(3, 100 + 480, 0, 60);
    require(scheduler.pendingCount() == 1, "scheduled one event");
    require(scheduler.isPending(3), "track 3 pending");
    const auto* event = scheduler.event(3);
    require(event != nullptr && event->due_frame == 580, "due = 580");

    std::array<PendingLayerEvent, 17> due{};
    require(
        scheduler.takeDueAt(579, due) == 0,
        "must not fire before due");
    require(scheduler.isPending(3), "still pending at 579");

    const auto count = scheduler.takeDueAt(580, due);
    require(count == 1, "fires at due frame");
    require(due[0].physical_track == 3, "track 3 fired");
    require(due[0].midi_note == 60, "note preserved");
    require(due[0].due_frame == 580, "due frame preserved");
    require(!scheduler.isPending(3), "slot cleared after take");
}

void testBlockBoundaryDueStaysUntilAbsoluteFrame() {
    LayerDelayScheduler scheduler;
    // Note on at block-relative 400, delay 300, block 512 => due 700.
    scheduler.schedule(5, 700, 1, 64);
    require(
        !scheduler.nextDueFrame(0, 511).has_value(),
        "due is not inside first block 0..511");
    const auto second = scheduler.nextDueFrame(511, 1023);
    require(second.has_value() && *second == 700, "due in second block");

    std::array<PendingLayerEvent, 17> due{};
    require(scheduler.takeDueAt(511, due) == 0, "not due at block end");
    require(scheduler.takeDueAt(699, due) == 0, "not due at 699");
    require(scheduler.takeDueAt(700, due) == 1, "due at absolute 700");
}

void testCancelBeforeDue() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(8, 2000, 0, 60);
    scheduler.cancelTrack(8);
    std::array<PendingLayerEvent, 17> due{};
    require(scheduler.takeDueAt(2000, due) == 0, "cancelled event must not fire");
    require(scheduler.pendingCount() == 0, "no pending after cancel");
}

void testCancelVoiceAndAll() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(0, 100, 0, 60);
    scheduler.schedule(3, 200, 0, 60);
    scheduler.schedule(8, 300, 1, 62);
    scheduler.cancelVoice(0);
    require(!scheduler.isPending(0), "voice 0 PSG cancelled");
    require(!scheduler.isPending(3), "voice 0 SCC cancelled");
    require(scheduler.isPending(8), "voice 1 still pending");
    scheduler.cancelAll();
    require(scheduler.pendingCount() == 0, "cancelAll clears remaining");
    std::array<PendingLayerEvent, 17> due{};
    require(scheduler.takeDueAt(300, due) == 0, "all-sound-off must not fire");
}

void testSameFrameCancelBeforeTake() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(3, 1500, 0, 60);
    scheduler.cancelTrack(3);
    std::array<PendingLayerEvent, 17> due{};
    require(
        scheduler.takeDueAt(1500, due) == 0,
        "same-frame cancel must win over due");
}

void testMultipleIndependentDelays() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(0, 0, 0, 60);
    scheduler.schedule(3, 480, 0, 60);
    scheduler.schedule(8, 1200, 0, 60);
    require(
        scheduler.nextDueFrame(0, 2000).value_or(0) == 480,
        "next after 0 is 480 (due 0 is not after 0)");

    std::array<PendingLayerEvent, 17> due{};
    require(scheduler.takeDueAt(0, due) == 1, "immediate-slot due 0 fires at 0");
    require(due[0].physical_track == 0, "track 0 at 0");
    require(scheduler.takeDueAt(479, due) == 0, "480 not early");
    require(scheduler.takeDueAt(480, due) == 1, "480 fires");
    require(due[0].physical_track == 3, "track 3 at 480");
    require(scheduler.isPending(8), "later slot still pending");
    require(scheduler.takeDueAt(1200, due) == 1, "1200 fires");
    require(due[0].physical_track == 8, "track 8 at 1200");
}

void testSameTrackOverwrite() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(3, 1000, 0, 60);
    scheduler.schedule(3, 2000, 1, 64);
    const auto* event = scheduler.event(3);
    require(event != nullptr, "overwritten slot still pending");
    require(event->due_frame == 2000, "new due");
    require(event->voice_index == 1, "new voice");
    require(event->midi_note == 64, "new note");
    std::array<PendingLayerEvent, 17> due{};
    require(scheduler.takeDueAt(1000, due) == 0, "old due must not fire");
    require(scheduler.takeDueAt(2000, due) == 1, "new due fires");
}

void testResetAndIsolation() {
    LayerDelayScheduler a;
    LayerDelayScheduler b;
    a.schedule(4, 50, 0, 61);
    require(b.pendingCount() == 0, "instances do not share slots");
    a.reset();
    std::array<PendingLayerEvent, 17> due{};
    require(a.takeDueAt(50, due) == 0, "reset discards pending");
    require(a.pendingCount() == 0, "reset empty");
}

void testInvalidTrackIgnored() {
    LayerDelayScheduler scheduler;
    scheduler.schedule(17, 10, 0, 60);
    scheduler.schedule(255, 10, 0, 60);
    require(scheduler.pendingCount() == 0, "out-of-range tracks ignored");
}

}  // namespace

int main() {
    try {
        testDelayConversion();
        testExactDueFrame();
        testBlockBoundaryDueStaysUntilAbsoluteFrame();
        testCancelBeforeDue();
        testCancelVoiceAndAll();
        testSameFrameCancelBeforeTake();
        testMultipleIndependentDelays();
        testSameTrackOverwrite();
        testResetAndIsolation();
        testInvalidTrackIgnored();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
