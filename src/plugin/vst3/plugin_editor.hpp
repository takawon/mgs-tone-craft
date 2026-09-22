// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <memory>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin_editor_context.hpp"

namespace mgstc::app {
class CompositeEditorComponent;
}

namespace mgstc::plugin {

class MgstcAudioProcessorEditor final : public juce::AudioProcessorEditor {
public:
    explicit MgstcAudioProcessorEditor(MgstcAudioProcessor& processor);
    ~MgstcAudioProcessorEditor() override;

    void resized() override;

private:
    void openSatellite(
        const juce::String& kind,
        std::optional<std::uint64_t> library_id);

    MgstcAudioProcessor& processor_;
    PluginEditorContext context_;
    mgstc::app::EditorLink link_;
    std::unique_ptr<juce::LookAndFeel> look_and_feel_;
    std::unique_ptr<mgstc::app::CompositeEditorComponent> composite_;
    std::unique_ptr<juce::DocumentWindow> scc_window_;
    std::unique_ptr<juce::DocumentWindow> opll_window_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MgstcAudioProcessorEditor)
};

}  // namespace mgstc::plugin
