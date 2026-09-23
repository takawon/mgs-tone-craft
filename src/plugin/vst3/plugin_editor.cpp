// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_editor.hpp"

#include <cstdio>

#include "mgstc_editor_view.hpp"
#include "mgstc_look_and_feel.hpp"
#include "editor_stall_probe.hpp"
#include "ui_layout.hpp"

namespace mgstc::plugin {
namespace {

#if !defined(NDEBUG)
void dumpEditorStallLog(const char* tag) {
    auto& probe = mgstc::app::EditorStallProbe::instance();
    probe.maybeFlushSecond();
    char report[32768]{};
    probe.formatReport(report, sizeof(report));
    auto dir = juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
                   .getChildFile("MgsToneCraft");
    if (!dir.isDirectory() && !dir.createDirectory()) {
        return;
    }
    const auto text =
        juce::String("--- ") + tag + " "
        + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S")
        + " ---\n" + juce::String(report) + "\n";
    static_cast<void>(
        dir.getChildFile("editor-stall.log").replaceWithText(text));
}

void schedulePeriodicJankDump(
    juce::Component::SafePointer<juce::Component> editor,
    int ticks) {
    juce::Timer::callAfterDelay(1000, [editor, ticks] {
        if (editor.getComponent() == nullptr) {
            return;
        }
        auto& probe = mgstc::app::EditorStallProbe::instance();
        probe.setEditorPixels(
            editor->getWidth() * editor->getHeight());
        probe.maybeFlushSecond();
        const int next = ticks + 1;
        if (next % 5 == 0) {
            dumpEditorStallLog("periodic-5s");
        }
        schedulePeriodicJankDump(editor, next);
    });
}

class JankHoverSpy final : public juce::MouseListener {
public:
    void mouseEnter(const juce::MouseEvent& event) override {
        if (event.eventComponent == last_) {
            return;
        }
        last_ = event.eventComponent;
        mgstc::app::editorJankNoteEvent("mouseEnter");
    }

    void mouseExit(const juce::MouseEvent& event) override {
        if (event.eventComponent != last_) {
            return;
        }
        last_ = nullptr;
        mgstc::app::editorJankNoteEvent("mouseExit");
    }

    void mouseMove(const juce::MouseEvent& event) override {
        if (event.eventComponent == last_) {
            return;
        }
        last_ = event.eventComponent;
        mgstc::app::editorJankNoteEvent("hoverChange");
    }

private:
    juce::Component* last_{};
};
#endif

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

[[nodiscard]] bool independentWorkbenchKind(const juce::String& kind) {
    return kind.equalsIgnoreCase("scc") || kind.equalsIgnoreCase("opll");
}

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
#if !defined(NDEBUG)
    hover_spy_ = std::make_unique<JankHoverSpy>();
    juce::Desktop::getInstance().addGlobalMouseListener(hover_spy_.get());
    auto& probe = mgstc::app::EditorStallProbe::instance();
    probe.beginSession();
    probe.setEditorPixels(getWidth() * getHeight());
    dumpEditorStallLog("open");
    schedulePeriodicJankDump(this, 0);
#endif
}

MgstcAudioProcessorEditor::~MgstcAudioProcessorEditor() {
#if !defined(NDEBUG)
    if (hover_spy_ != nullptr) {
        juce::Desktop::getInstance().removeGlobalMouseListener(
            hover_spy_.get());
    }
    dumpEditorStallLog("close");
#endif
    setLookAndFeel(nullptr);
    scc_window_.reset();
    opll_window_.reset();
    composite_.reset();
}

void MgstcAudioProcessorEditor::resized() {
    if (composite_ != nullptr) {
        composite_->setBounds(getLocalBounds());
    }
#if !defined(NDEBUG)
    mgstc::app::EditorStallProbe::instance().setEditorPixels(
        getWidth() * getHeight());
#endif
}

mgstc::engine::CompositeTimbre
MgstcAudioProcessorEditor::copyWorkingComposite() const {
    if (composite_ == nullptr) {
        return {};
    }
    return composite_->workingTimbre();
}

void MgstcAudioProcessorEditor::openSatellite(
    const juce::String& kind,
    std::optional<std::uint64_t> library_id) {
    if (independentWorkbenchKind(kind)
        && !library_id
        && !link_.owned_target) {
        return;
    }
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
                kind.equalsIgnoreCase("scc-envelope")
                    ? juce::String::fromUTF8("MGS Tone Craft - 総合音色編集（SCC）")
                    : juce::String("MGS Tone Craft - SCC"),
                editor);
        }
        if (library_id) {
            if (auto* editor = dynamic_cast<mgstc::app::SccEditorComponent*>(
                    scc_window_->getContentComponent())) {
                editor->requestLibraryEntry(*library_id);
            }
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
                kind.equalsIgnoreCase("opll-envelope")
                    ? juce::String::fromUTF8("MGS Tone Craft - 総合音色編集（OPLL）")
                    : juce::String("MGS Tone Craft - OPLL"),
                editor);
        }
        if (library_id) {
            if (auto* editor = dynamic_cast<mgstc::app::OpllEditorComponent*>(
                    opll_window_->getContentComponent())) {
                editor->requestLibraryEntry(*library_id);
            }
        }
        opll_window_->setVisible(true);
        opll_window_->toFront(true);
    }
}

#if !defined(NDEBUG) && !defined(MGSTC_SKIP_MINIMAL_EDITOR)
namespace {

int readJankIniInt(
    const wchar_t* section,
    const wchar_t* key,
    int fallback) {
    wchar_t path[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH) == 0) {
        return fallback;
    }
    wcsncat_s(path, MAX_PATH, L"\\MgsToneCraft\\editor-jank.ini", _TRUNCATE);
    return static_cast<int>(
        GetPrivateProfileIntW(section, key, fallback, path));
}

class ProbeTextButton final : public juce::TextButton {
public:
    using juce::TextButton::TextButton;

    void mouseEnter(const juce::MouseEvent& event) override {
        MGSTC_JANK_EVENT("button mouseEnter");
        juce::TextButton::mouseEnter(event);
    }

    void mouseExit(const juce::MouseEvent& event) override {
        MGSTC_JANK_EVENT("button mouseExit");
        juce::TextButton::mouseExit(event);
    }

    void paint(juce::Graphics& graphics) override {
        MGSTC_JANK_PAINT(
            mgstc::app::JankRepaintKind::Other, graphics, *this);
        juce::TextButton::paint(graphics);
    }
};

class NestedPanel final : public juce::Component {
public:
    explicit NestedPanel(juce::Colour fill)
        : fill_(fill) {}

    void paint(juce::Graphics& graphics) override {
        MGSTC_JANK_PAINT(
            mgstc::app::JankRepaintKind::Other, graphics, *this);
        graphics.fillAll(fill_);
    }

private:
    juce::Colour fill_;
};

class MinimalPluginEditor final : public juce::AudioProcessorEditor {
public:
    explicit MinimalPluginEditor(MgstcAudioProcessor& processor)
        : juce::AudioProcessorEditor(processor) {
        stage_ = juce::jlimit(1, 5, readJankIniInt(L"Minimal", L"stage", 1));
        opaque_root_ =
            readJankIniInt(L"Minimal", L"opaque_root", 0) != 0;
        setSize(UiLayout::editorWindowW, UiLayout::editorWindowH);
        setOpaque(opaque_root_);

        if (stage_ >= 5) {
            page_ = std::make_unique<NestedPanel>(
                juce::Colour(UiLayout::pageFill));
            panel_ = std::make_unique<NestedPanel>(
                juce::Colour(0xFF2A3540));
            content_ = std::make_unique<NestedPanel>(
                juce::Colour(0xFF182028));
            page_->setOpaque(opaque_root_);
            panel_->setOpaque(opaque_root_);
            content_->setOpaque(opaque_root_);
            addAndMakeVisible(*page_);
            page_->addAndMakeVisible(*panel_);
            panel_->addAndMakeVisible(*content_);
        }

        if (stage_ >= 2) {
            button_ = std::make_unique<ProbeTextButton>("Test");
            auto* parent = content_ != nullptr
                ? static_cast<juce::Component*>(content_.get())
                : static_cast<juce::Component*>(this);
            parent->addAndMakeVisible(*button_);
        }
        if (stage_ >= 3) {
            look_and_feel_ = std::make_unique<MgstcLookAndFeel>();
            setLookAndFeel(look_and_feel_.get());
        }
        if (stage_ >= 4) {
            tooltip_ = std::make_unique<juce::TooltipWindow>(this);
            if (button_ != nullptr) {
                button_->setTooltip("minimal editor tooltip");
            }
        }

        layoutChildren();

        static char open_tag[80]{};
        std::snprintf(
            open_tag,
            sizeof(open_tag),
            "minimal stage=%d opaque_root=%d",
            stage_,
            opaque_root_ ? 1 : 0);
        hover_spy_ = std::make_unique<JankHoverSpy>();
        juce::Desktop::getInstance().addGlobalMouseListener(hover_spy_.get());
        auto& probe = mgstc::app::EditorStallProbe::instance();
        probe.beginSession();
        probe.setEditorPixels(getWidth() * getHeight());
        MGSTC_JANK_EVENT(open_tag);
        dumpEditorStallLog("minimal-open");
        schedulePeriodicJankDump(this, 0);
    }

    ~MinimalPluginEditor() override {
        if (hover_spy_ != nullptr) {
            juce::Desktop::getInstance().removeGlobalMouseListener(
                hover_spy_.get());
        }
        dumpEditorStallLog("minimal-close");
        setLookAndFeel(nullptr);
        tooltip_.reset();
        button_.reset();
        content_.reset();
        panel_.reset();
        page_.reset();
        look_and_feel_.reset();
    }

    void paint(juce::Graphics& graphics) override {
        MGSTC_JANK_PAINT(
            mgstc::app::JankRepaintKind::FullEditor, graphics, *this);
        graphics.fillAll(juce::Colour(UiLayout::pageFill));
    }

    void resized() override {
        layoutChildren();
        mgstc::app::EditorStallProbe::instance().setEditorPixels(
            getWidth() * getHeight());
    }

    void mouseEnter(const juce::MouseEvent& event) override {
        MGSTC_JANK_EVENT("root mouseEnter");
        juce::AudioProcessorEditor::mouseEnter(event);
    }

    void mouseExit(const juce::MouseEvent& event) override {
        MGSTC_JANK_EVENT("root mouseExit");
        juce::AudioProcessorEditor::mouseExit(event);
    }

private:
    void layoutChildren() {
        if (page_ != nullptr) {
            page_->setBounds(getLocalBounds());
            panel_->setBounds(page_->getLocalBounds().reduced(24));
            content_->setBounds(panel_->getLocalBounds().reduced(16));
        }
        if (button_ != nullptr) {
            button_->setBounds(40, 40, 160, 32);
        }
    }

    int stage_{1};
    bool opaque_root_{};
    std::unique_ptr<MgstcLookAndFeel> look_and_feel_;
    std::unique_ptr<NestedPanel> page_;
    std::unique_ptr<NestedPanel> panel_;
    std::unique_ptr<NestedPanel> content_;
    std::unique_ptr<ProbeTextButton> button_;
    std::unique_ptr<juce::TooltipWindow> tooltip_;
    std::unique_ptr<juce::MouseListener> hover_spy_;
};

}  // namespace
#endif

juce::AudioProcessorEditor* MgstcAudioProcessor::createEditor() {
#if !defined(NDEBUG) && !defined(MGSTC_SKIP_MINIMAL_EDITOR)
    if (readJankIniInt(L"Minimal", L"enable", 0) != 0) {
        return new MinimalPluginEditor(*this);
    }
#endif
    return new MgstcAudioProcessorEditor(*this);
}

}  // namespace mgstc::plugin
