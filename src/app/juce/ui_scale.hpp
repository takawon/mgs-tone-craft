// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_paths.hpp"

namespace UiLayout {
void applyScale(float factor);
}

// Discrete UI scale (View setting + optional per-editor session override).
namespace UiScale {
constexpr int kDefaultPercent = 100;
constexpr std::array<int, 3> kSteps{75, 100, 125};

inline int global_percent = kDefaultPercent;
inline int active_percent = kDefaultPercent;
inline float active_factor = 1.0F;

inline std::vector<std::function<void()>> global_listeners;

[[nodiscard]] inline juce::File settingsIniFile() {
    return mgstcApplicationDataDirectory().getChildFile("settings-v1.ini");
}

[[nodiscard]] inline int nearestStep(int percent) noexcept {
    int best = kDefaultPercent;
    int best_dist = std::abs(percent - best);
    for (const int step : kSteps) {
        const int dist = std::abs(percent - step);
        if (dist < best_dist) {
            best = step;
            best_dist = dist;
        }
    }
    return best;
}

[[nodiscard]] inline bool isStep(int percent) noexcept {
    for (const int step : kSteps) {
        if (step == percent) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline float factorFromPercent(int percent) noexcept {
    return static_cast<float>(nearestStep(percent)) / 100.0F;
}

[[nodiscard]] inline int nextLarger(int percent) noexcept {
    const int clamped = nearestStep(percent);
    for (const int step : kSteps) {
        if (step > clamped) {
            return step;
        }
    }
    return clamped;
}

[[nodiscard]] inline int nextSmaller(int percent) noexcept {
    const int clamped = nearestStep(percent);
    for (int i = static_cast<int>(kSteps.size()) - 1; i >= 0; --i) {
        if (kSteps[static_cast<std::size_t>(i)] < clamped) {
            return kSteps[static_cast<std::size_t>(i)];
        }
    }
    return clamped;
}

[[nodiscard]] inline int sx(int value) noexcept {
    return juce::jmax(
        1, juce::roundToInt(static_cast<float>(value) * active_factor));
}

void applyLayoutMetrics(float factor); // defined after UiLayout

inline void setActivePercent(int percent) {
    active_percent = nearestStep(percent);
    active_factor = factorFromPercent(active_percent);
    applyLayoutMetrics(active_factor);
}

inline void loadGlobalFromIni() {
    const auto file = settingsIniFile();
    int value = kDefaultPercent;
    if (file.existsAsFile()) {
        value = static_cast<int>(GetPrivateProfileIntW(
            L"Application",
            L"UiScalePercent",
            kDefaultPercent,
            file.getFullPathName().toWideCharPointer()));
    }
    // Only exact steps {75,100,125} are accepted. Invalid values (including
    // legacy 50) stay at default 100% for this session; do not rewrite INI.
    global_percent = isStep(value) ? value : kDefaultPercent;
    setActivePercent(global_percent);
}

inline void saveGlobalToIni() {
    const auto file = settingsIniFile();
    if (file.getParentDirectory().createDirectory().failed()) {
        return;
    }
    const auto path = file.getFullPathName();
    const auto value = juce::String(global_percent);
    static_cast<void>(WritePrivateProfileStringW(
        L"Application",
        L"SchemaVersion",
        L"1",
        path.toWideCharPointer()));
    static_cast<void>(WritePrivateProfileStringW(
        L"Application",
        L"UiScalePercent",
        value.toWideCharPointer(),
        path.toWideCharPointer()));
    static_cast<void>(WritePrivateProfileStringW(
        nullptr, nullptr, nullptr, path.toWideCharPointer()));
}

inline void addGlobalListener(std::function<void()> listener) {
    global_listeners.push_back(std::move(listener));
}

inline void notifyGlobalListeners() {
    for (auto& listener : global_listeners) {
        if (listener) {
            listener();
        }
    }
}

// Persist + apply to metrics, then refresh every registered root (editors + dialogs).
inline void setGlobalPercent(int percent, bool persist) {
    global_percent = nearestStep(percent);
    if (persist) {
        saveGlobalToIni();
    }
    setActivePercent(global_percent);
    notifyGlobalListeners();
}

// Settings / library / tags / other dialogs must never inherit an editor's
// Ctrl± session override via process-global metrics.
inline void forceGlobalForNonEditorUi() {
    setActivePercent(global_percent);
}

[[nodiscard]] inline bool modalUiBlocksEditorScale() {
    return juce::ModalComponentManager::getInstance()
               ->getNumModalComponents()
        > 0;
}

// Only the active editor window may push session scale into shared metrics.
// Background editors and any open modal must not leak Ctrl± into dialogs or
// newly created windows.
[[nodiscard]] inline bool editorMayDriveProcessScale(
    const juce::Component& editor) {
    if (modalUiBlocksEditorScale()) {
        return false;
    }
    if (auto* top = editor.getTopLevelComponent()) {
        if (auto* window =
                dynamic_cast<const juce::TopLevelWindow*>(top)) {
            return window->isActiveWindow();
        }
    }
    return false;
}

inline void syncEditorDrivenActivePercent(
    const juce::Component& editor,
    int effective_percent) {
    if (modalUiBlocksEditorScale()) {
        // Any modal (settings, library, tags, AlertWindow, …) owns the UI
        // context — keep process metrics on View/INI global, never Ctrl±.
        forceGlobalForNonEditorUi();
        return;
    }
    if (editorMayDriveProcessScale(editor)) {
        setActivePercent(effective_percent);
    }
}

[[nodiscard]] inline bool tryHandleEditorScaleKey(
    const juce::KeyPress& key,
    std::optional<int>& session_override_percent,
    const std::function<void(int effective_percent)>& apply) {
    if (!key.getModifiers().isCommandDown()
        || key.getModifiers().isAltDown()) {
        return false;
    }
    const auto code = key.getKeyCode();
    // Keep Ctrl+C/V/Z/Y for timbre edit; only handle zoom keys here.
    if (code == 'c' || code == 'C' || code == 'v' || code == 'V'
        || code == 'z' || code == 'Z' || code == 'y' || code == 'Y') {
        return false;
    }
    const int current = session_override_percent.value_or(global_percent);
    if (code == '=' || code == '+'
        || code == juce::KeyPress::numberPadAdd) {
        session_override_percent = nextLarger(current);
        apply(*session_override_percent);
        return true;
    }
    if (code == '-' || code == juce::KeyPress::numberPadSubtract) {
        session_override_percent = nextSmaller(current);
        apply(*session_override_percent);
        return true;
    }
    if (code == '0' || code == juce::KeyPress::numberPad0) {
        session_override_percent.reset();
        apply(global_percent);
        return true;
    }
    return false;
}
} // namespace UiScale
