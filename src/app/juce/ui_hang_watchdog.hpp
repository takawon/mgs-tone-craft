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

// Message-thread breadcrumb. A stale heartbeat alone never says which call
// blocked, so hang reports quote the innermost label plus how long it has run.
// Labels must have static storage duration (string literals).
void setUiActivity(const char* label) noexcept;
[[nodiscard]] const char* uiActivityLabel() noexcept;

// Windows DefWindowProc size/move (title-bar drag, border resize) runs a
// nested modal loop that stops JUCE timers from updating the hang heartbeat.
// That is expected OS behaviour — suppress hang reports while it is active.
void enterUiHangNativeModal() noexcept;
void exitUiHangNativeModal() noexcept;
[[nodiscard]] bool uiHangNativeModalActive() noexcept;

class ScopedUiActivity final {
public:
    explicit ScopedUiActivity(const char* label) noexcept;
    ~ScopedUiActivity();

    ScopedUiActivity(const ScopedUiActivity&) = delete;
    ScopedUiActivity& operator=(const ScopedUiActivity&) = delete;

private:
    const char* previous_label_;
    std::int64_t previous_started_ms_;
};

// One per scope; a second in the same scope is a compile error by design.
#define MGSTC_UI_ACTIVITY(label)     \
    const ::mgstc::app::ScopedUiActivity mgstc_ui_activity_scope_ { label }

// Lightweight UI hang detector: message-thread heartbeat + rare watchdog poll.
// Must never run on the audio callback path or take audio locks.
class UiHangWatchdog final : private juce::Timer {
public:
    static constexpr int kDefaultHangThresholdMs = 4000;
    static constexpr int kDefaultCheckIntervalMs = 500;
    static constexpr int kDefaultHeartbeatIntervalMs = 250;
    // A second record this much past the threshold separates a bounded stall
    // (recovers) from a real deadlock (never does).
    static constexpr int kEscalationFactor = 4;

    UiHangWatchdog();
    ~UiHangWatchdog() override;

    UiHangWatchdog(const UiHangWatchdog&) = delete;
    UiHangWatchdog& operator=(const UiHangWatchdog&) = delete;

    void start(int hang_threshold_ms = kDefaultHangThresholdMs);
    void stop() noexcept;

private:
    void timerCallback() override;
    void watchdogLoop();
    void writeHangReport(
        const char* stage,
        std::int64_t hung_ms) const noexcept;

    std::atomic<bool> running_{false};
    std::atomic<bool> hang_reported_{false};
    std::atomic<std::int64_t> last_heartbeat_ms_{0};
    std::atomic<int> hang_threshold_ms_{kDefaultHangThresholdMs};
    // Watchdog-thread only.
    bool escalation_reported_{false};
    std::int64_t peak_hang_ms_{0};
    std::thread watchdog_;
};

}  // namespace mgstc::app
