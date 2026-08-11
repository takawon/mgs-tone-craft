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

}  // namespace

void publishUiHangPcAudioHint(std::uint8_t kind) noexcept {
    g_pc_audio_hint.store(kind, std::memory_order_relaxed);
}

void publishUiHangSoundOutputHint(std::uint8_t kind) noexcept {
    g_sound_output_hint.store(kind, std::memory_order_relaxed);
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
    last_heartbeat_ms_.store(steadyNowMs(), std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    startTimer(kDefaultHeartbeatIntervalMs);
    watchdog_ = std::thread([this] { watchdogLoop(); });
}

void UiHangWatchdog::stop() noexcept {
    running_.store(false, std::memory_order_release);
    stopTimer();
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
        const auto now = steadyNowMs();
        const auto last =
            last_heartbeat_ms_.load(std::memory_order_relaxed);
        const auto age_ms = now - last;
        const auto threshold =
            static_cast<std::int64_t>(
                hang_threshold_ms_.load(std::memory_order_relaxed));
        if (age_ms >= threshold) {
            if (!hang_reported_.exchange(true, std::memory_order_acq_rel)) {
                writeHangReport(age_ms);
            }
        } else {
            hang_reported_.store(false, std::memory_order_relaxed);
        }
    }
}

void UiHangWatchdog::writeHangReport(std::int64_t hung_ms) const noexcept {
    wchar_t dir[MAX_PATH]{};
    if (!ensureHangLogDirectory(dir, std::size(dir))) {
        return;
    }

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    wchar_t path[MAX_PATH]{};
    if (_snwprintf_s(
            path,
            _TRUNCATE,
            L"%s\\hang-%04u%02u%02u-%02u%02u%02u.log",
            dir,
            static_cast<unsigned>(utc.wYear),
            static_cast<unsigned>(utc.wMonth),
            static_cast<unsigned>(utc.wDay),
            static_cast<unsigned>(utc.wHour),
            static_cast<unsigned>(utc.wMinute),
            static_cast<unsigned>(utc.wSecond))
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

    char body[768]{};
    const int written = _snprintf_s(
        body,
        _TRUNCATE,
        "MGS Tone Craft UI hang diagnostic\r\n"
        "timestamp_utc: %04u-%02u-%02uT%02u:%02u:%02uZ\r\n"
        "hang_duration_ms: %lld\r\n"
        "threshold_ms: %d\r\n"
        "note: message thread did not pump (heartbeat stale)\r\n"
        "pc_audio: %s\r\n"
        "sound_output: %s\r\n"
        "doc_version: " MGSTC_DOC_VERSION "\r\n",
        static_cast<unsigned>(utc.wYear),
        static_cast<unsigned>(utc.wMonth),
        static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour),
        static_cast<unsigned>(utc.wMinute),
        static_cast<unsigned>(utc.wSecond),
        static_cast<long long>(hung_ms),
        hang_threshold_ms_.load(std::memory_order_relaxed),
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
