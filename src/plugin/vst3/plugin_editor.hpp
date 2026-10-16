// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <memory>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugin_editor_context.hpp"
#include "tone_library_session.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/tone_import.hpp"

enum class LibraryManagerKind : std::uint8_t;

namespace mgstc::app {
class CompositeEditorComponent;
class LibraryManagerWindow;
class SpectrogramWindow;
}

namespace mgstc::plugin {

class MgstcAudioProcessorEditor final : public juce::AudioProcessorEditor {
public:
    explicit MgstcAudioProcessorEditor(MgstcAudioProcessor& processor);
    ~MgstcAudioProcessorEditor() override;

    void resized() override;
    void focusGained(juce::Component::FocusChangeType cause) override;
    void focusOfChildComponentChanged(juce::Component::FocusChangeType cause) override;
    [[nodiscard]] mgstc::engine::CompositeTimbre copyWorkingComposite() const;

private:
    void openSatellite(
        const juce::String& kind,
        std::optional<std::uint64_t> library_id);
    void showLibraryManager(
        juce::Component* anchor,
        std::optional<LibraryManagerKind> replace_kind = {},
        std::function<void(std::uint64_t)> on_replace = {});
    void notifyLibrariesChanged();
    void refreshLibrariesOnFocus();
    void rewriteLibraryTag(std::string source, std::string replacement);
    void openLibraryEntry(LibraryManagerKind kind, std::uint64_t id);
    void previewLibraryEntry(
        LibraryManagerKind kind, std::uint64_t id,
        std::optional<std::uint8_t> note = {});
    void previewImportedCandidate(
        const mgstc::engine::ImportedToneCandidate& candidate,
        std::optional<std::uint8_t> note = {});
    void showSpectrogram();

    MgstcAudioProcessor& processor_;
    PluginEditorContext context_;
    mgstc::app::EditorLink link_;
    std::unique_ptr<juce::LookAndFeel> look_and_feel_;
    std::unique_ptr<mgstc::app::CompositeEditorComponent> composite_;
    std::unique_ptr<juce::DocumentWindow> scc_window_;
    std::unique_ptr<juce::DocumentWindow> opll_window_;
    std::unique_ptr<mgstc::app::LibraryManagerWindow> library_manager_window_;
    std::unique_ptr<mgstc::app::SpectrogramWindow> spectrogram_window_;
    juce::InterProcessLock library_lock_{"MgsToneCraftTimbreLibraryV1"};
    std::uint8_t spectrogram_source_mask_{};
    std::uint8_t spectrogram_input_target_{};
    bool notifying_libraries_{};
    mgstc::app::LibraryFileFingerprint library_fingerprint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MgstcAudioProcessorEditor)
};

}  // namespace mgstc::plugin
