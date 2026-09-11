// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <functional>

#include <juce_gui_extra/juce_gui_extra.h>

namespace mgstc::engine {
class RealtimeEngineHost;
}

namespace mgstc::app {

enum class SpectrogramAnalysisMode : std::uint8_t {
    None = 0,
    PsgOnly = 1U << 0U,
    SccOnly = 1U << 1U,
    PsgAndScc = 3,
    OpllOnly = 1U << 2U,
    PsgAndOpll = 5,
    SccAndOpll = 6,
    AllSources = 7,
};

class SpectrogramWindow final : public juce::DocumentWindow {
public:
    explicit SpectrogramWindow(engine::RealtimeEngineHost& engine);
    ~SpectrogramWindow() override;

    SpectrogramWindow(const SpectrogramWindow&) = delete;
    SpectrogramWindow& operator=(const SpectrogramWindow&) = delete;

    void showWindow();
    void setAnalysisMode(SpectrogramAnalysisMode mode);
    void setSpectrumMode(bool enabled);
    [[nodiscard]] juce::Component* snapshotContent() const noexcept;
    void closeButtonPressed() override;

    // Windows HWND subclass: rewrite a sibling's WINDOWPOS so this window
    // stays above it without WS_EX_TOPMOST. window_pos is WINDOWPOS*.
    static void constrainSiblingZOrder(
        void* window_pos,
        void* caller_native_handle);
    static void raisePinnedAfterSibling(void* caller_native_handle);

private:
    class Content;

    void setInternalPin(bool pinned);
    void applyGlobalUiScale();
    void hideWindow();
    void loadState();
    void saveState();
    void updatePinZOrderHook();

    Content* content_{};
    bool internal_pin_{};
};

}  // namespace mgstc::app
