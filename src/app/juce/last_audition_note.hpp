// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <atomic>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <juce_core/juce_core.h>

#include "juce_chip_tracks.hpp"
#include "ui_paths.hpp"

// Coalesces LastAuditionNote INI writes off the note-on hot path.
class LastAuditionNoteStore final {
public:
    [[nodiscard]] static LastAuditionNoteStore& instance() {
        static LastAuditionNoteStore store;
        return store;
    }

    [[nodiscard]] std::uint8_t get() const noexcept {
        ensureLoaded();
        return static_cast<std::uint8_t>(juce::jlimit(
            24,
            119,
            cached_note_.load(std::memory_order_relaxed)));
    }

    void mark(std::uint8_t note) noexcept {
        ensureLoaded();
        const int clamped = juce::jlimit(24, 119, static_cast<int>(note));
        cached_note_.store(clamped, std::memory_order_relaxed);
        dirty_.store(true, std::memory_order_release);
    }

    void flushIfDirty() {
        if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        const auto file = mgstcApplicationDataDirectory().getChildFile(
            "settings-v1.ini");
        if (file.getParentDirectory().createDirectory().failed()) {
            dirty_.store(true, std::memory_order_release);
            return;
        }
        const auto path = file.getFullPathName();
        const auto value = juce::String(
            cached_note_.load(std::memory_order_relaxed));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"SchemaVersion",
            L"1",
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            L"Application",
            L"LastAuditionNote",
            value.toWideCharPointer(),
            path.toWideCharPointer()));
        static_cast<void>(WritePrivateProfileStringW(
            nullptr, nullptr, nullptr, path.toWideCharPointer()));
    }

private:
    LastAuditionNoteStore() = default;
    ~LastAuditionNoteStore() { flushIfDirty(); }

    void ensureLoaded() const {
        if (loaded_.load(std::memory_order_acquire)) {
            return;
        }
        const auto file = mgstcApplicationDataDirectory().getChildFile(
            "settings-v1.ini");
        int value = kPreviewNote;
        if (file.existsAsFile()) {
            value = static_cast<int>(GetPrivateProfileIntW(
                L"Application",
                L"LastAuditionNote",
                kPreviewNote,
                file.getFullPathName().toWideCharPointer()));
        }
        cached_note_.store(
            juce::jlimit(24, 119, value),
            std::memory_order_relaxed);
        loaded_.store(true, std::memory_order_release);
    }

    mutable std::atomic<bool> loaded_{false};
    mutable std::atomic<int> cached_note_{kPreviewNote};
    std::atomic<bool> dirty_{false};
};
