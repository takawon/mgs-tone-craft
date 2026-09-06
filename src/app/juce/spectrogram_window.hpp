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

class SpectrogramWindow final
    : public juce::DocumentWindow,
      private juce::Timer {
public:
    explicit SpectrogramWindow(engine::RealtimeEngineHost& engine);
    ~SpectrogramWindow() override;

    SpectrogramWindow(const SpectrogramWindow&) = delete;
    SpectrogramWindow& operator=(const SpectrogramWindow&) = delete;

    void showWindow();
    void setAnalysisMode(SpectrogramAnalysisMode mode);
    void closeButtonPressed() override;

private:
    class Content;

    void setInternalPin(bool pinned);
    void timerCallback() override;
    void applyGlobalUiScale();
    void hideWindow();
    void loadState();
    void saveState();

    Content* content_{};
    bool internal_pin_{};
};

}  // namespace mgstc::app
