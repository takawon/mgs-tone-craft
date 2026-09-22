// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_editor.hpp"

#include "mgstc_editor_view.hpp"
#include "mgstc_look_and_feel.hpp"

namespace mgstc::plugin {
namespace {

class PluginSatelliteWindow final : public juce::DocumentWindow {
public:
    PluginSatelliteWindow(
        const juce::String& title,
        juce::Component* content)
        : DocumentWindow(
            title,
            juce::Colour(0xFF1B222C),
            DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(content, true);
        setResizable(true, false);
        centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override {
        setVisible(false);
    }
};

}  // namespace

MgstcAudioProcessorEditor::MgstcAudioProcessorEditor(
    MgstcAudioProcessor& processor)
    : juce::AudioProcessorEditor(processor),
      processor_(processor),
      context_(processor) {
    look_and_feel_ = std::make_unique<MgstcLookAndFeel>();
    setLookAndFeel(look_and_feel_.get());
    setWantsKeyboardFocus(true);

    link_.owned_audition = {
        [this] {
            if (composite_ != nullptr) {
                composite_->syncOwnedEditProgram();
            }
        },
        [this] {
            if (composite_ != nullptr) {
                composite_->auditionOwnedEditOneSecond();
            }
        },
        [this](std::uint8_t note) {
            if (composite_ != nullptr) {
                composite_->ownedEditNoteOn(note);
            }
        },
        [this](std::uint8_t note) {
            if (composite_ != nullptr) {
                composite_->ownedEditNoteOff(note);
            }
        },
        [this] {
            if (composite_ != nullptr) {
                composite_->stopImportedPreview();
            }
        },
    };

    const auto state = processor_.copyPluginState();
    composite_ = std::make_unique<mgstc::app::CompositeEditorComponent>(
        context_,
        link_,
        [this](const juce::String& kind, std::optional<std::uint64_t> id) {
            openSatellite(kind, id);
        },
        [] {},
        [](std::uint8_t) {},
        [](juce::Component*) {},
        [] {},
        state.sound);
    addAndMakeVisible(composite_.get());
    setSize(composite_->getWidth(), composite_->getHeight());
}

MgstcAudioProcessorEditor::~MgstcAudioProcessorEditor() {
    setLookAndFeel(nullptr);
    scc_window_.reset();
    opll_window_.reset();
    composite_.reset();
}

void MgstcAudioProcessorEditor::resized() {
    if (composite_ != nullptr) {
        composite_->setBounds(getLocalBounds());
    }
}

void MgstcAudioProcessorEditor::openSatellite(
    const juce::String& kind,
    std::optional<std::uint64_t>) {
    const auto open_editor =
        [this](const juce::String& next, std::optional<std::uint64_t> id) {
            openSatellite(next, id);
        };
    const mgstc::app::SpectrogramOpenCallback no_spectrum = [] {};
    const mgstc::app::TagManagementCallback no_library =
        [](juce::Component*) {};
    const mgstc::app::LibrariesChangedCallback no_change = [] {};
    if (kind.equalsIgnoreCase("scc")
        || kind.equalsIgnoreCase("scc-envelope")) {
        if (scc_window_ == nullptr) {
            auto* editor = new mgstc::app::SccEditorComponent(
                context_,
                link_,
                open_editor,
                no_spectrum,
                [](std::vector<mgstc::engine::OpllPatchParameters>) {},
                no_library,
                kind.equalsIgnoreCase("scc-envelope"),
                no_change);
            scc_window_ = std::make_unique<PluginSatelliteWindow>(
                "MGS Tone Craft - SCC",
                editor);
        }
        scc_window_->setVisible(true);
        scc_window_->toFront(true);
        return;
    }
    if (kind.equalsIgnoreCase("opll")
        || kind.equalsIgnoreCase("opll-envelope")) {
        if (opll_window_ == nullptr) {
            auto* editor = new mgstc::app::OpllEditorComponent(
                context_,
                link_,
                open_editor,
                no_spectrum,
                no_library,
                kind.equalsIgnoreCase("opll-envelope"),
                no_change);
            opll_window_ = std::make_unique<PluginSatelliteWindow>(
                "MGS Tone Craft - OPLL",
                editor);
        }
        opll_window_->setVisible(true);
        opll_window_->toFront(true);
    }
}

juce::AudioProcessorEditor* MgstcAudioProcessor::createEditor() {
    return new MgstcAudioProcessorEditor(*this);
}

}  // namespace mgstc::plugin
