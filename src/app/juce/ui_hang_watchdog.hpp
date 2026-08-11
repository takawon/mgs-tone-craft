// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include <juce_events/juce_events.h>

namespace mgstc::app {

// Cheap lock-free hints for hang reports (message thread preferred).
constexpr std::uint8_t kUiHangHintUnknown = 0;
constexpr std::uint8_t kUiHangPcWasapi = 1;
constexpr std::uint8_t kUiHangPcAsio = 2;
constexpr std::uint8_t kUiHangOutEmulator = 1;
constexpr std::uint8_t kUiHangOutMAmidi = 2;

void publishUiHangPcAudioHint(std::uint8_t kind) noexcept;
void publishUiHangSoundOutputHint(std::uint8_t kind) noexcept;

// Lightweight UI hang detector: message-thread heartbeat + rare watchdog poll.
// Must never run on the audio callback path or take audio locks.
class UiHangWatchdog final : private juce::Timer {
public:
    static constexpr int kDefaultHangThresholdMs = 4000;
    static constexpr int kDefaultCheckIntervalMs = 500;
    static constexpr int kDefaultHeartbeatIntervalMs = 250;

    UiHangWatchdog();
    ~UiHangWatchdog() override;

    UiHangWatchdog(const UiHangWatchdog&) = delete;
    UiHangWatchdog& operator=(const UiHangWatchdog&) = delete;

    void start(int hang_threshold_ms = kDefaultHangThresholdMs);
    void stop() noexcept;

private:
    void timerCallback() override;
    void watchdogLoop();
    void writeHangReport(std::int64_t hung_ms) const noexcept;

    std::atomic<bool> running_{false};
    std::atomic<bool> hang_reported_{false};
    std::atomic<std::int64_t> last_heartbeat_ms_{0};
    std::atomic<int> hang_threshold_ms_{kDefaultHangThresholdMs};
    std::thread watchdog_;
};

}  // namespace mgstc::app
