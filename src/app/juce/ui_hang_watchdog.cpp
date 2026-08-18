// SPDX-License-Identifier: AGPL-3.0-only

#include "ui_hang_watchdog.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>

#ifndef MGSTC_DOC_VERSION
#define MGSTC_DOC_VERSION "0.0"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace mgstc::app {
namespace {

std::atomic<std::uint8_t> g_pc_audio_hint{kUiHangHintUnknown};
std::atomic<std::uint8_t> g_sound_output_hint{kUiHangHintUnknown};

constexpr const char* kIdleActivity = "idle (message loop)";
constexpr const char* kNativeModalActivity =
    "window: native size/move modal";
std::atomic<const char*> g_ui_activity{kIdleActivity};
std::atomic<std::int64_t> g_ui_activity_started_ms{0};
std::atomic<int> g_native_modal_depth{0};
// Published by the active watchdog so EXITSIZEMOVE can refresh the heartbeat
// without holding a pointer to the Application instance.
std::atomic<std::int64_t>* g_heartbeat_ms{nullptr};
std::atomic<bool>* g_hang_reported{nullptr};

[[nodiscard]] std::int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] const char* pcAudioHintText(std::uint8_t kind) noexcept {
    switch (kind) {
        case kUiHangPcWasapi:
            return "WASAPI";
        case kUiHangPcAsio:
            return "ASIO";
        default:
            return "unknown";
    }
}

[[nodiscard]] const char* soundOutputHintText(std::uint8_t kind) noexcept {
    switch (kind) {
        case kUiHangOutEmulator:
            return "emulator";
        case kUiHangOutMAmidi:
            return "MAmidiMEmo";
        default:
            return "unknown";
    }
}

[[nodiscard]] bool ensureHangLogDirectory(wchar_t* out_dir, std::size_t cap) {
    if (out_dir == nullptr || cap < 8) {
        return false;
    }
    const DWORD got = GetEnvironmentVariableW(
        L"LOCALAPPDATA", out_dir, static_cast<DWORD>(cap));
    if (got == 0 || got >= cap) {
        return false;
    }
    const auto len = std::wcslen(out_dir);
    constexpr wchar_t kSuffix[] = L"\\MgsToneCraft";
    if (len + (sizeof(kSuffix) / sizeof(wchar_t)) >= cap) {
        return false;
    }
    if (wcscat_s(out_dir, cap, kSuffix) != 0) {
        return false;
    }
    CreateDirectoryW(out_dir, nullptr);
    return true;
}

void bumpHeartbeat() noexcept {
    if (auto* heartbeat = g_heartbeat_ms) {
        heartbeat->store(steadyNowMs(), std::memory_order_relaxed);
    }
}

}  // namespace

void publishUiHangPcAudioHint(std::uint8_t kind) noexcept {
    g_pc_audio_hint.store(kind, std::memory_order_relaxed);
}

void publishUiHangSoundOutputHint(std::uint8_t kind) noexcept {
    g_sound_output_hint.store(kind, std::memory_order_relaxed);
}

void setUiActivity(const char* label) noexcept {
    g_ui_activity.store(
        label != nullptr ? label : kIdleActivity,
        std::memory_order_relaxed);
    g_ui_activity_started_ms.store(
        steadyNowMs(), std::memory_order_relaxed);
}

const char* uiActivityLabel() noexcept {
    const auto* label = g_ui_activity.load(std::memory_order_relaxed);
    return label != nullptr ? label : kIdleActivity;
}

void enterUiHangNativeModal() noexcept {
    if (g_native_modal_depth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        setUiActivity(kNativeModalActivity);
    }
}

void exitUiHangNativeModal() noexcept {
    const auto previous =
        g_native_modal_depth.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 0) {
        // Unbalanced EXITSIZEMOVE (e.g. after process attach); keep depth ≥ 0.
        g_native_modal_depth.store(0, std::memory_order_relaxed);
        return;
    }
    if (previous == 1) {
        setUiActivity(kIdleActivity);
        bumpHeartbeat();
        if (auto* reported = g_hang_reported) {
            // Drop a half-written episode that started before ENTERSIZEMOVE
            // raced the watchdog sample.
            reported->store(false, std::memory_order_relaxed);
        }
    }
}

bool uiHangNativeModalActive() noexcept {
    return g_native_modal_depth.load(std::memory_order_acquire) > 0;
}

ScopedUiActivity::ScopedUiActivity(const char* label) noexcept
    : previous_label_(g_ui_activity.load(std::memory_order_relaxed)),
      previous_started_ms_(
          g_ui_activity_started_ms.load(std::memory_order_relaxed)) {
    setUiActivity(label);
}

ScopedUiActivity::~ScopedUiActivity() {
    // Restore the outer scope with its own start time so nesting does not
    // reset how long the outer call has already been running.
    g_ui_activity.store(
        previous_label_ != nullptr ? previous_label_ : kIdleActivity,
        std::memory_order_relaxed);
    g_ui_activity_started_ms.store(
        previous_started_ms_ > 0 ? previous_started_ms_ : steadyNowMs(),
        std::memory_order_relaxed);
}

UiHangWatchdog::UiHangWatchdog() = default;

UiHangWatchdog::~UiHangWatchdog() {
    stop();
}

void UiHangWatchdog::start(int hang_threshold_ms) {
    stop();
    const auto threshold = hang_threshold_ms > 0
        ? hang_threshold_ms
        : kDefaultHangThresholdMs;
    hang_threshold_ms_.store(threshold, std::memory_order_relaxed);
    hang_reported_.store(false, std::memory_order_relaxed);
    escalation_reported_ = false;
    peak_hang_ms_ = 0;
    last_heartbeat_ms_.store(steadyNowMs(), std::memory_order_relaxed);
    g_heartbeat_ms = &last_heartbeat_ms_;
    g_hang_reported = &hang_reported_;
    // Seed idle activity start so ui_activity_ms is meaningful.
    if (g_ui_activity_started_ms.load(std::memory_order_relaxed) == 0) {
        setUiActivity(kIdleActivity);
    }
    running_.store(true, std::memory_order_release);
    startTimer(kDefaultHeartbeatIntervalMs);
    watchdog_ = std::thread([this] { watchdogLoop(); });
}

void UiHangWatchdog::stop() noexcept {
    running_.store(false, std::memory_order_release);
    stopTimer();
    if (g_heartbeat_ms == &last_heartbeat_ms_) {
        g_heartbeat_ms = nullptr;
    }
    if (g_hang_reported == &hang_reported_) {
        g_hang_reported = nullptr;
    }
    if (watchdog_.joinable()) {
        try {
            watchdog_.join();
        } catch (...) {
        }
    }
}

void UiHangWatchdog::timerCallback() {
    last_heartbeat_ms_.store(steadyNowMs(), std::memory_order_relaxed);
}

void UiHangWatchdog::watchdogLoop() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDefaultCheckIntervalMs));
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (uiHangNativeModalActive()) {
            // Expected: DefWindowProc size/move owns the message thread.
            continue;
        }
        const auto now = steadyNowMs();
        const auto last =
            last_heartbeat_ms_.load(std::memory_order_relaxed);
        const auto age_ms = now - last;
        const auto threshold =
            static_cast<std::int64_t>(
                hang_threshold_ms_.load(std::memory_order_relaxed));
        if (age_ms >= threshold) {
            peak_hang_ms_ = age_ms;
            if (!hang_reported_.exchange(true, std::memory_order_acq_rel)) {
                writeHangReport("detected", age_ms);
            } else if (
                !escalation_reported_
                && age_ms >= threshold * kEscalationFactor) {
                escalation_reported_ = true;
                writeHangReport("still-hung", age_ms);
            }
        } else if (hang_reported_.exchange(false, std::memory_order_acq_rel)) {
            // Recovery record: a bounded stall ends here, a deadlock never
            // reaches this branch.
            writeHangReport("recovered", peak_hang_ms_);
            escalation_reported_ = false;
            peak_hang_ms_ = 0;
        }
    }
}

void UiHangWatchdog::writeHangReport(
    const char* stage,
    std::int64_t hung_ms) const noexcept {
    wchar_t dir[MAX_PATH]{};
    if (!ensureHangLogDirectory(dir, std::size(dir))) {
        return;
    }

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t path[MAX_PATH]{};
    wchar_t stage_suffix[32]{};
    static_cast<void>(_snwprintf_s(
        stage_suffix, _TRUNCATE, L"%hs", stage != nullptr ? stage : "detected"));
    if (_snwprintf_s(
            path,
            _TRUNCATE,
            L"%s\\hang-%04u%02u%02u-%02u%02u%02u-%s.log",
            dir,
            static_cast<unsigned>(utc.wYear),
            static_cast<unsigned>(utc.wMonth),
            static_cast<unsigned>(utc.wDay),
            static_cast<unsigned>(utc.wHour),
            static_cast<unsigned>(utc.wMinute),
            static_cast<unsigned>(utc.wSecond),
            stage_suffix)
        < 0) {
        return;
    }

    const HANDLE file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const auto* activity = uiActivityLabel();
    const auto started =
        g_ui_activity_started_ms.load(std::memory_order_relaxed);
    const auto now = steadyNowMs();
    const auto activity_ms = started > 0 ? (now - started) : 0;

    char body[1024]{};
    const int written = _snprintf_s(
        body,
        _TRUNCATE,
        "MGS Tone Craft UI hang diagnostic\r\n"
        "timestamp_utc: %04u-%02u-%02uT%02u:%02u:%02uZ\r\n"
        "stage: %s\r\n"
        "hang_duration_ms: %lld\r\n"
        "threshold_ms: %d\r\n"
        "note: message thread did not pump (heartbeat stale)\r\n"
        "ui_activity: %s\r\n"
        "ui_activity_ms: %lld\r\n"
        "pc_audio: %s\r\n"
        "sound_output: %s\r\n"
        "doc_version: " MGSTC_DOC_VERSION "\r\n",
        static_cast<unsigned>(utc.wYear),
        static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute),
        static_cast<unsigned>(utc.wSecond),
        stage != nullptr ? stage : "detected",
        static_cast<long long>(hung_ms),
        hang_threshold_ms_.load(std::memory_order_relaxed),
        activity,
        static_cast<long long>(activity_ms),
        pcAudioHintText(
            g_pc_audio_hint.load(std::memory_order_relaxed)),
        soundOutputHintText(
            g_sound_output_hint.load(std::memory_order_relaxed)));

    if (written > 0) {
        DWORD bytes = 0;
        static_cast<void>(WriteFile(
            file,
            body,
            static_cast<DWORD>(written),
            &bytes,
            nullptr));
    }
    CloseHandle(file);
}

}  // namespace mgstc::app
