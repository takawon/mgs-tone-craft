// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_editor.hpp"

#include "mgstc_editor_view.hpp"
#include "mgstc_look_and_feel.hpp"
#include "spectrogram_window.hpp"

namespace mgstc::plugin {
namespace {

class PluginSatelliteWindow final : public juce::DocumentWindow {
public:
    PluginSatelliteWindow(
        const juce::String& title,
        juce::Component* content)
        : DocumentWindow(
            mgstc::app::editorWindowTitle(title),
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

// The shared composite audition owns temporary programs and restores the
// edited sound afterward. A plain single timbre needs a one-layer model to
// enter that path; this copy never changes the editor or the library.
[[nodiscard]] mgstc::engine::ImportedToneCandidate compositePreviewCandidate(
    const mgstc::engine::ImportedToneCandidate& candidate) {
    using namespace mgstc::engine;
    if (candidate.default_register_as == ImportRegisterAs::Composite)
        return candidate;
    auto result = candidate;
    const auto entry = makeImportedLibraryEntry(candidate, candidate.default_register_as);
    auto sound = defaultCompositeTimbre();
    auto layer = sound.layers[entry.category == TimbreCategory::Scc ? 1 : 2];
    sound.layers = {std::move(layer)};
    sound.layers.front().base_opll_rom.reset();
    sound.layers.front().base_timbre = adoptLibraryTimbre(sound, entry);
    sound.name = candidate.name;
    sound.tags = candidate.tags;
    sound.memo = candidate.memo;
    sound.favorite = candidate.favorite;
    result.data = std::move(sound);
    result.default_register_as = ImportRegisterAs::Composite;
    result.type = ImportedToneType::Composite;
    return result;
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

    // The editor is created on the message thread. Keep SQLite startup and
    // recovery here, outside processor construction and processBlock.
    static const bool library_available =
        mgstc::app::initialiseToneLibrarySession(library_lock_);

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
        [this](const mgstc::engine::SavedTimbreReference& snapshot,
               std::optional<std::uint8_t> note, bool one_second) {
            return composite_ && composite_->previewOwnedTimbre(snapshot, note, one_second);
        },
    };

    const auto revision = context_.stateRestoreRevision();
    const auto state = processor_.copyPluginState();
    composite_ = std::make_unique<mgstc::app::CompositeEditorComponent>(
        context_,
        link_,
        [this](const juce::String& kind, std::optional<std::uint64_t> id) {
            openSatellite(kind, id);
        },
        [this] { showSpectrogram(); },
        [this](std::uint8_t mask) {
            spectrogram_source_mask_ = mask;
            if (spectrogram_window_)
                spectrogram_window_->setAnalysisMode(
                    static_cast<mgstc::app::SpectrogramAnalysisMode>(mask));
        },
        [this](juce::Component* anchor) { showLibraryManager(anchor); },
        [this] { notifyLibrariesChanged(); },
        state.sound);
    composite_->setLibraryReplaceOpener(
        [this](mgstc::engine::TimbreSource source,
               std::function<void(std::uint64_t)> replace) {
            showLibraryManager(composite_.get(),
                source == mgstc::engine::TimbreSource::Scc
                    ? LibraryManagerKind::Scc : LibraryManagerKind::Opll,
                std::move(replace));
        });
    addAndMakeVisible(composite_.get());
    setSize(composite_->getWidth(), composite_->getHeight());
    context_.acknowledgeStateRestore(revision);
    library_fingerprint_ = mgstc::app::libraryFileFingerprint(mgstc::app::toneLibraryFile());
    static bool library_warning_shown = false;
    if (!library_available && !library_warning_shown) {
        library_warning_shown = true;
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8("ライブラリ管理"),
            juce::String::fromUTF8("ライブラリの読込に問題がありました。読み取れた音色のみを表示し、このセッションの保存を停止します。"));
    }
    processor_.setEditorStateRestoredCallback([this, hydrated_revision = revision]() mutable {
        const auto restored_revision = context_.stateRestoreRevision();
        if (restored_revision == hydrated_revision) return;
        library_manager_window_.reset();
        spectrogram_input_target_ = 0;
        scc_window_.reset();
        opll_window_.reset();
        composite_->restoreHostState(processor_.copyPluginState().sound);
        context_.acknowledgeStateRestore(restored_revision);
        hydrated_revision = restored_revision;
    });
}

MgstcAudioProcessorEditor::~MgstcAudioProcessorEditor() {
    processor_.setEditorStateRestoredCallback({});
    spectrogram_window_.reset();
    library_manager_window_.reset();
    scc_window_.reset();
    opll_window_.reset();
    composite_.reset();
    setLookAndFeel(nullptr);
}

void MgstcAudioProcessorEditor::resized() {
    if (composite_ != nullptr) {
        composite_->setBounds(getLocalBounds());
    }
}

void MgstcAudioProcessorEditor::focusGained(juce::Component::FocusChangeType cause) {
    juce::AudioProcessorEditor::focusGained(cause);
    refreshLibrariesOnFocus();
}

void MgstcAudioProcessorEditor::focusOfChildComponentChanged(juce::Component::FocusChangeType cause) {
    juce::AudioProcessorEditor::focusOfChildComponentChanged(cause);
    if (hasKeyboardFocus(true)) refreshLibrariesOnFocus();
}

void MgstcAudioProcessorEditor::refreshLibrariesOnFocus() {
    if (!composite_) return;
    const auto fingerprint = mgstc::app::libraryFileFingerprint(mgstc::app::toneLibraryFile());
    if (fingerprint == library_fingerprint_) return;
    notifyLibrariesChanged();
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
    if (kind.equalsIgnoreCase("main")) {
        if (library_id) composite_->requestLibraryEntry(*library_id);
        return;
    }
    const bool scc = kind.equalsIgnoreCase("scc")
        || kind.equalsIgnoreCase("scc-envelope");
    const bool opll = kind.equalsIgnoreCase("opll")
        || kind.equalsIgnoreCase("opll-envelope");
    if (!scc && !opll) return;
    const auto source = scc ? mgstc::engine::TimbreSource::Scc
                            : mgstc::engine::TimbreSource::Opll;
    const bool owned = library_id && link_.owned_target
        && link_.owned_target->library_id == *library_id
        && link_.owned_target->source == source;
    if (library_id && !owned) {
        composite_->requestSingleTimbreLibraryEntry(source, *library_id);
        return;
    }
    if (!owned) {
        return;
    }
    const auto open_editor =
        [this](const juce::String& next, std::optional<std::uint64_t> id) {
            openSatellite(next, id);
        };
    const mgstc::app::SpectrogramOpenCallback open_spectrum =
        [this] { showSpectrogram(); };
    const mgstc::app::TagManagementCallback open_library =
        [this](juce::Component* anchor) { showLibraryManager(anchor); };
    const mgstc::app::LibrariesChangedCallback library_changed =
        [this] { notifyLibrariesChanged(); };
    if (kind.equalsIgnoreCase("scc")
        || kind.equalsIgnoreCase("scc-envelope")) {
        if (scc_window_ == nullptr) {
            auto* editor = new mgstc::app::SccEditorComponent(
                context_,
                link_,
                open_editor,
                open_spectrum,
                [this](std::vector<mgstc::engine::OpllPatchParameters> candidates) {
                    if (candidates.empty()) return;
                    const auto revision = context_.stateRestoreRevision();
                    juce::Component::SafePointer<MgstcAudioProcessorEditor> safe(this);
                    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                        .withTitle(juce::String::fromUTF8("SCC → OPLL"))
                        .withMessage(juce::String::fromUTF8("変換候補を総合音色の最初のOPLLチャンネルのベースへ適用します。OPLLがない場合は追加します。現在のOPLLベースは置き換わります。SCCチャンネルは保持します。"))
                        .withButton(juce::String::fromUTF8("適用"))
                        .withButton(juce::String::fromUTF8("キャンセル"))
                        .withAssociatedComponent(this),
                        [safe, revision, candidates = std::move(candidates)](int result) mutable {
                            if (!safe || result != 1
                                || safe->context_.stateRestoreRevision() != revision) return;
                            if (!safe->composite_->applyOpllConversionCandidate(candidates.front())) return;
                            if (safe->opll_window_)
                                if (auto* editor = dynamic_cast<mgstc::app::OpllEditorComponent*>(
                                        safe->opll_window_->getContentComponent()))
                                    editor->receiveConversionCandidates(std::move(candidates));
                        });
                },
                open_library,
                true,
                library_changed);
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
                open_spectrum,
                open_library,
                true,
                library_changed);
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

void MgstcAudioProcessorEditor::showSpectrogram() {
    spectrogram_input_target_ = scc_window_ && scc_window_->isActiveWindow() ? 1
        : opll_window_ && opll_window_->isActiveWindow() ? 2 : 0;
    if (!spectrogram_window_)
        spectrogram_window_ = std::make_unique<mgstc::app::SpectrogramWindow>(
            processor_.editorEngineForAnalysis(), [this](bool active) {
                if (composite_) composite_->setSpectrogramInputActive(active && spectrogram_input_target_ == 0);
                if (scc_window_)
                    if (auto* editor = dynamic_cast<mgstc::app::SccEditorComponent*>(scc_window_->getContentComponent()))
                        editor->setSpectrogramInputActive(active && spectrogram_input_target_ == 1);
                if (opll_window_)
                    if (auto* editor = dynamic_cast<mgstc::app::OpllEditorComponent*>(opll_window_->getContentComponent()))
                        editor->setSpectrogramInputActive(active && spectrogram_input_target_ == 2);
            });
    auto mode = static_cast<mgstc::app::SpectrogramAnalysisMode>(spectrogram_source_mask_);
    if (scc_window_ && scc_window_->isActiveWindow())
        mode = mgstc::app::SpectrogramAnalysisMode::SccOnly;
    else if (opll_window_ && opll_window_->isActiveWindow())
        mode = mgstc::app::SpectrogramAnalysisMode::OpllOnly;
    spectrogram_window_->setAnalysisMode(mode);
    spectrogram_window_->showWindow();
}

void MgstcAudioProcessorEditor::showLibraryManager(
    juce::Component* anchor, std::optional<LibraryManagerKind> replace_kind,
    std::function<void(std::uint64_t)> on_replace) {
    if (!library_manager_window_) {
        std::optional<mgstc::app::ToneLibrarySnapshot> loaded;
        {
            mgstc::app::ScopedLibraryIpcLock lock(library_lock_);
            if (lock.isLocked()) loaded = mgstc::app::loadToneLibraries();
        }
        if (!loaded) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("共有ライブラリを読み込めませんでした"));
            return;
        }
        auto initial = replace_kind.value_or(LibraryManagerKind::Composite);
        if (!replace_kind && dynamic_cast<mgstc::app::SccEditorComponent*>(anchor))
            initial = LibraryManagerKind::Scc;
        if (!replace_kind && dynamic_cast<mgstc::app::OpllEditorComponent*>(anchor))
            initial = LibraryManagerKind::Opll;
        UiScale::forceGlobalForNonEditorUi();
        auto content = std::make_unique<mgstc::app::LibraryManagerContent>(
            initial, std::move(loaded->timbres), std::move(loaded->composites),
            context_, library_lock_,
            [this](std::string source, std::string replacement) {
                rewriteLibraryTag(std::move(source), std::move(replacement));
            },
            [this](LibraryManagerKind kind, std::uint64_t id) { previewLibraryEntry(kind, id); },
            [this](LibraryManagerKind kind, std::uint64_t id) { openLibraryEntry(kind, id); },
            [this] { notifyLibrariesChanged(); },
            [this](LibraryManagerKind kind, std::uint64_t id, std::uint8_t note) {
                previewLibraryEntry(kind, id, note);
            },
            [this](LibraryManagerKind, std::uint8_t note) { composite_->libraryManagerNoteOff(note); },
            [this] { composite_->libraryManagerAllNotesOff(); },
            [this](const mgstc::engine::ImportedToneCandidate& candidate) {
                previewImportedCandidate(candidate);
            },
            [this](const mgstc::engine::ImportedToneCandidate& candidate) {
                composite_->libraryManagerAllNotesOff();
                composite_->stopImportedPreview();
                composite_->loadSingleImportedCandidate(candidate);
            },
            [this](const mgstc::engine::ImportedToneCandidate& candidate, std::uint8_t note) {
                previewImportedCandidate(candidate, note);
            },
            [this](LibraryManagerKind) { return composite_->polyphonic(); });
        library_manager_window_ = std::make_unique<mgstc::app::LibraryManagerWindow>(std::move(content));
        library_manager_window_->centreAroundComponent(anchor ? anchor : this,
            library_manager_window_->getWidth(), library_manager_window_->getHeight());
        clampWindowToDisplayWorkArea(*library_manager_window_);
    } else {
        library_manager_window_->reloadFromExternalChange();
    }
    library_manager_window_->setReplaceMode(replace_kind, std::move(on_replace));
    library_manager_window_->setVisible(true);
    library_manager_window_->toFront(true);
    if (auto* content = library_manager_window_->getContentComponent()) content->grabKeyboardFocus();
}

void MgstcAudioProcessorEditor::openLibraryEntry(LibraryManagerKind kind, std::uint64_t id) {
    composite_->libraryManagerAllNotesOff();
    composite_->stopImportedPreview();
    if (kind == LibraryManagerKind::Composite) composite_->requestLibraryEntry(id);
    else composite_->requestSingleTimbreLibraryEntry(
        kind == LibraryManagerKind::Scc ? mgstc::engine::TimbreSource::Scc
                                       : mgstc::engine::TimbreSource::Opll, id);
}

void MgstcAudioProcessorEditor::previewImportedCandidate(
    const mgstc::engine::ImportedToneCandidate& candidate,
    std::optional<std::uint8_t> note) {
    const auto preview = compositePreviewCandidate(candidate);
    if (note) composite_->libraryManagerImportedNoteOn(preview, *note);
    else {
        composite_->libraryManagerAllNotesOff();
        composite_->stopImportedPreview();
        composite_->auditionImportedCandidate(preview);
    }
}

void MgstcAudioProcessorEditor::previewLibraryEntry(
    LibraryManagerKind kind, std::uint64_t id, std::optional<std::uint8_t> note) {
    if (kind == LibraryManagerKind::Composite) {
        if (note) composite_->libraryManagerNoteOn(id, *note);
        else {
            composite_->libraryManagerAllNotesOff();
            composite_->stopImportedPreview();
            composite_->auditionLibraryPreview(id);
        }
        return;
    }
    mgstc::engine::ImportedToneCandidate candidate;
    {
        mgstc::app::ScopedLibraryIpcLock lock(library_lock_);
        if (!lock.isLocked()) return;
        auto loaded = mgstc::app::loadSharedTimbreLibrary();
        const auto* entry = loaded ? loaded->find(id) : nullptr;
        if (!entry) return;
        const bool scc = kind == LibraryManagerKind::Scc;
        if ((entry->category == mgstc::engine::TimbreCategory::Scc) != scc) return;
        candidate.name = entry->name;
        candidate.favorite = entry->favorite;
        candidate.tags = entry->tags;
        candidate.memo = entry->memo;
        candidate.type = scc ? mgstc::engine::ImportedToneType::Scc : mgstc::engine::ImportedToneType::Opll;
        candidate.default_register_as = scc ? mgstc::engine::ImportRegisterAs::Scc : mgstc::engine::ImportRegisterAs::Opll;
        if (scc) {
            mgstc::engine::SccWaveform wave;
            for (std::size_t i = 0; i < wave.size(); ++i)
                wave[i] = static_cast<std::int8_t>(entry->scc_waveform[i]);
            candidate.data = wave;
        } else candidate.data = mgstc::engine::decodeOpllPatch(entry->opll_registers);
        static_cast<void>(loaded->touch(id, mgstc::app::currentUnixTime()));
        static_cast<void>(mgstc::app::persistTimbreLibrary(*loaded));
    }
    previewImportedCandidate(candidate, note);
}

void MgstcAudioProcessorEditor::notifyLibrariesChanged() {
    if (notifying_libraries_) return;
    const juce::ScopedValueSetter<bool> updating(notifying_libraries_, true);
    composite_->refreshExternalState();
    if (scc_window_)
        if (auto* editor = dynamic_cast<mgstc::app::SccEditorComponent*>(scc_window_->getContentComponent()))
            editor->refreshExternalState();
    if (opll_window_)
        if (auto* editor = dynamic_cast<mgstc::app::OpllEditorComponent*>(opll_window_->getContentComponent()))
            editor->refreshExternalState();
    if (library_manager_window_ && library_manager_window_->isVisible())
        library_manager_window_->reloadFromExternalChange();
    library_fingerprint_ = mgstc::app::libraryFileFingerprint(mgstc::app::toneLibraryFile());
}

void MgstcAudioProcessorEditor::rewriteLibraryTag(std::string source, std::string replacement) {
    bool saved = false;
    {
        mgstc::app::ScopedLibraryIpcLock lock(library_lock_);
        if (lock.isLocked()) {
            auto loaded = mgstc::app::loadToneLibraries();
            if (loaded) {
                const auto now = mgstc::app::currentUnixTime();
                loaded->timbres.rewriteTag(source, replacement, now);
                loaded->composites.rewriteTag(source, replacement, now);
                saved = mgstc::app::persistToneLibraries(loaded->timbres, loaded->composites);
            }
        }
    }
    if (!saved) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8("ライブラリ管理"),
            juce::String::fromUTF8("共有ライブラリを更新できませんでした"));
        return;
    }
    composite_->applyTagRewrite(source, replacement);
    if (scc_window_)
        if (auto* editor = dynamic_cast<mgstc::app::SccEditorComponent*>(scc_window_->getContentComponent()))
            editor->applyTagRewrite(source, replacement);
    if (opll_window_)
        if (auto* editor = dynamic_cast<mgstc::app::OpllEditorComponent*>(opll_window_->getContentComponent()))
            editor->applyTagRewrite(source, replacement);
    notifyLibrariesChanged();
}

juce::AudioProcessorEditor* MgstcAudioProcessor::createEditor() {
    return new MgstcAudioProcessorEditor(*this);
}

}  // namespace mgstc::plugin
