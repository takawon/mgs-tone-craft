// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"
#include "plugin_editor.hpp"
#include "plugin_editor_context.hpp"
#include "mgstc_editor_view.hpp"
#include "ui_layout.hpp"
#include "tone_library_session.hpp"
#include <rpc/server.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "plugin_state.hpp"
#include "mgstc/engine/dc_blocker.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/patchslide_timbre.hpp"
#include "mgstc/engine/register_write.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/sinc_rate_conv.hpp"

namespace mgstc::app {

struct SatelliteEditorParityTestAccess {
    static void checkSccWav(SccEditorComponent& editor) {
        const auto check = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        const auto type = [&](const juce::String& text) {
            editor.finishNumericEdit();
            editor.numeric_input_.setText(text, false);
            editor.numeric_input_.setCaretPosition(4);
            editor.numericTextChanged();
            check(editor.numeric_input_.getText() == text && editor.numeric_input_.getCaretPosition() == 4,
                "live expression evaluation preserves input text and caret");
        };
        const auto previous_wave = editor.scc_wave_;
        const juce::String periodic_text("SCCWAV L=256 127<255>127<0>127");
        editor.pasteNumericText(periodic_text);
        const mgstc::engine::SccWaveform periodic_expected{
            -1,24,48,70,90,105,117,125,127,125,117,105,90,70,48,24,
            -1,-26,-50,-72,-91,-107,-118,-126,-128,-126,-118,-107,-91,-72,-50,-26};
        check(editor.scc_wave_ == periodic_expected && editor.graph_.waveform() == periodic_expected,
            "shared editor applies all 32 nonduplicated periodic samples");
        editor.numeric_copy_.onClick();
        check(juce::SystemClipboard::getTextFromClipboard() == periodic_text,
            "endpoint sampling preserves original shared expression");
        editor.undo();
        check(editor.scc_wave_ == previous_wave, "periodic paste undo preserves prior wave");
        editor.redo();
        check(editor.scc_wave_ == periodic_expected, "periodic paste redo preserves new endpoint policy");
        const auto wave = editor.scc_wave_;
        const auto cursor = editor.history_cursor_;
        type("SCCWAV L=16");
        check(editor.scc_wave_ == wave && editor.history_cursor_ == cursor
            && editor.numeric_resolution_.getSelectedId() == 16 && !editor.numeric_copy_.isEnabled(),
            "header-only input updates selector without waveform or history");
        editor.numeric_resolution_.setSelectedId(32, juce::sendNotificationSync);
        check(editor.numeric_input_.getText() == "SCCWAV L=32" && editor.scc_wave_ == wave,
            "incomplete header tracks resolution changes");
        editor.pasteNumericText("0 31");
        check(editor.numeric_state_.resolution == 32 && editor.scc_wave_.back() == 127,
            "bare dedicated paste uses header-only selector resolution");
        type("SCCWAV L=16 0>(15 0)");
        const auto cross = editor.scc_wave_;
        const auto cross_cursor = editor.history_cursor_;
        type("SCCWAV L=16 0(>15 0)");
        check(editor.scc_wave_ == cross && editor.history_cursor_ == cross_cursor + 1,
            "equivalent cross-boundary syntax records distinct expression history");
        editor.undo();
        check(editor.numeric_input_.getText() == "SCCWAV L=16 0>(15 0)",
            "undo restores original equivalent expression");
        editor.redo();
        check(editor.numeric_input_.getText() == "SCCWAV L=16 0(>15 0)",
            "redo restores parentheses and operator placement");
        editor.numeric_copy_.onClick();
        check(juce::SystemClipboard::getTextFromClipboard() == "SCCWAV L=16 0(>15 0)",
            "copy emits a single header and preserves expression");
        editor.numeric_resolution_.setSelectedId(256, juce::sendNotificationSync);
        check(editor.numeric_input_.getText() == "SCCWAV L=256 0(>255 0)",
            "requantization rewrites literals and header only");
        editor.undo();
        check(editor.numeric_resolution_.getSelectedId() == 16, "expression resolution undo restores selector");
        const auto valid = editor.scc_wave_;
        const auto valid_cursor = editor.history_cursor_;
        type("SCCWAV L=16 0>(>15 0)");
        check(editor.scc_wave_ == valid && editor.history_cursor_ == valid_cursor,
            "duplicate interpolation preserves committed wave and history");
        type("SCCWAV L=16 0 (15 0");
        check(editor.scc_wave_ == valid && editor.history_cursor_ == valid_cursor,
            "unfinished parentheses do not commit waveform");
        type("SCCWAV L=32 (");
        editor.numeric_resolution_.setSelectedId(64, juce::sendNotificationSync);
        check(editor.numeric_resolution_.getSelectedId() == 32
            && editor.numeric_input_.getText() == "SCCWAV L=32 (",
            "invalid expression retains header resolution and text when selector change is rejected");
        editor.pasteNumericText("SCCWAV L=16 0>(15-0)");
        check(editor.numeric_input_.getText() == "SCCWAV L=16 0>(15-0)",
            "dedicated paste preserves full expression");
        const auto pasted = editor.scc_wave_;
        editor.pasteNumericText("SCCNUM/1 L=16 0 15");
        check(editor.scc_wave_ == pasted, "obsolete sharing format is rejected");
        editor.numeric_input_.insertTextAtCaret("SCCWAV L=16 (0 8)(>15 0)");
        check(editor.numeric_input_.getText() == "SCCWAV L=16 (0 8)(>15 0)",
            "shared text insertion atomically replaces full input");
        for (const int percent : {75,100,125}) {
            editor.applyEditorUiScale(percent);
            const auto bounds = editor.numeric_input_.getBounds();
            editor.showNumericHelp();
            check(editor.numeric_help_->isVisible() && !editor.numeric_help_->isCurrentlyModal()
                && editor.numeric_input_.getBounds() == bounds, "help is a non-modal overlay without relayout");
            check(editor.numeric_help_button_.getRight() < editor.numeric_copy_.getX()
                && editor.numeric_help_button_.getX() > bounds.getRight(), "help fits between input and Copy");
            const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
            auto output = juce::File::getCurrentWorkingDirectory().getChildFile(
                "sccwav-help-" + juce::String(percent) + ".png").createOutputStream();
            check(output != nullptr && juce::PNGImageFormat().writeImageToStream(image, *output),
                "SCCWAV help snapshot saved");
            check(editor.numeric_input_.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))
                && !editor.numeric_help_->isVisible(), "Escape dismisses help from numeric editor");
            editor.showNumericHelp();
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent outside(juce::Desktop::getInstance().getMainMouseSource(), {0.0F,0.0F},
                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                1.0F,0.0F,0.0F,0.0F,0.0F, &editor.numeric_copy_, &editor.numeric_copy_, now,
                {0.0F,0.0F}, now,1,false);
            editor.numeric_help_->mouseDown(outside);
            check(!editor.numeric_help_->isVisible(), "outside click dismisses non-modal help");
        }
        editor.applyEditorUiScale(100);
        {
            juce::DocumentWindow host("SCCWAV focus", juce::Colours::black, juce::DocumentWindow::allButtons);
            host.setContentNonOwned(&editor, true);
            host.setTopLeftPosition(-10000,-10000);
            host.setVisible(true);
            editor.numeric_input_.grabKeyboardFocus();
            const auto* focus = juce::Component::getCurrentlyFocusedComponent();
            const auto modal_count = juce::Component::getNumCurrentlyModalComponents();
            editor.showNumericHelp();
            check(focus == &editor.numeric_input_ && juce::Component::getCurrentlyFocusedComponent() == focus
                && juce::Component::getNumCurrentlyModalComponents() == modal_count,
                "help does not steal numeric focus or enter modal state");
            editor.closeNumericHelp();
            host.setVisible(false);
            host.clearContentComponent();
        }
    }
    static void checkNumeric(SccEditorComponent& editor) {
        const auto check = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        editor.immediate_audition_.setToggleState(false, juce::dontSendNotification);
        editor.pasteNumericText("SCCWAV L=16 0 15");
        check(editor.numeric_state_.resolution == 16 && editor.scc_wave_[0] == -128
            && editor.scc_wave_[16] == 127, "shared numeric paste applies waveform and resolution");
        const auto first = editor.history_cursor_;
        editor.numeric_input_.moveCaretToEnd();
        editor.numeric_input_.keyPressed(juce::KeyPress(' ', {}, ' '));
        editor.numeric_input_.keyPressed(juce::KeyPress('1', {}, '1'));
        editor.numeric_input_.keyPressed(juce::KeyPress('5', {}, '5'));
        check(editor.history_cursor_ == first + 1 && editor.numeric_state_.values.size() == 3
            && editor.numeric_state_.values.back() == 15, "multi-digit token is one live undo operation");
        editor.undo();
        check(editor.numeric_state_.values == std::vector<int>({0,15})
            && editor.numeric_state_.resolution == 16, "undo restores numeric sequence and resolution");
        editor.redo();
        check(editor.numeric_state_.values.size() == 3, "redo restores numeric token");
        editor.numeric_resolution_.setSelectedId(256, juce::sendNotificationSync);
        check(editor.numeric_state_.values == std::vector<int>({0,255,255}), "resolution change requantizes");
        editor.undo();
        check(editor.numeric_state_.resolution == 16, "resolution change is one undo operation");
        auto valid = editor.scc_wave_;
        auto cursor = editor.history_cursor_;
        editor.numeric_input_.setText("16 invalid", false);
        editor.numericTextChanged();
        check(editor.scc_wave_ == valid && editor.history_cursor_ == cursor
            && !editor.numeric_copy_.isEnabled(), "invalid text preserves wave and history");
        editor.pasteNumericText("SCCNUM/2 L=16 0 15");
        check(editor.scc_wave_ == valid && editor.numeric_input_.getText() == "16 invalid",
            "failed dedicated paste preserves text and waveform");
        editor.pasteNumericText("SCCWAV L=16 0 15");
        editor.commitWave(editor.scc_wave_);
        check(editor.numeric_state_.resolution == 16 && editor.numeric_state_.values.size() == 2,
            "identical external wave does not reset numeric state");
        editor.performLoadImportedScc(editor.scc_wave_, "Numeric reload", false);
        check(editor.numeric_state_.resolution == 16 && editor.numeric_state_.values.size() == 2,
            "identical imported wave preserves numeric resolution and source sequence");
        auto other = editor.scc_wave_;
        other[0] = 1;
        editor.commitWave(other);
        check(editor.numeric_state_.resolution == 256 && editor.numeric_state_.values.size() == 32
            && editor.numeric_state_.values.front() == 129, "external edit synchronizes signed samples exactly");
        editor.undo();
        check(editor.numeric_state_.resolution == 16 && editor.numeric_state_.values.size() == 2,
            "mixed external and numeric history restores compact sequence");
        editor.numeric_copy_.onClick();
        check(juce::SystemClipboard::getTextFromClipboard() == "SCCWAV L=16 0 15",
            "copy preserves compact numeric source");
        const auto first_value = editor.numeric_input_.getText().lastIndexOf("0 15");
        editor.numeric_input_.setHighlightedRegion({first_value,first_value+1});
        juce::SystemClipboard::copyTextToClipboard("15");
        editor.numeric_input_.keyPressed(juce::KeyPress('V', juce::ModifierKeys::ctrlModifier, 0));
        check(editor.numeric_state_.values == std::vector<int>({15,15}), "ordinary paste replaces selection");
        editor.undo();
        check(editor.numeric_state_.values == std::vector<int>({0,15}), "batch paste undo is atomic");
        for (const int percent : {75,100,125}) {
            editor.applyEditorUiScale(percent);
            check(!editor.random_reroll_.isVisible(), "canvas preserves hidden non-Random control");
            const auto graph = editor.graph_.graphBounds();
            check(graph.getHeight() >= 256, "scaled graph retains 256 mouse positions");
            const auto row = editor.numeric_input_.getBounds();
            check(row.getWidth() > 100 && editor.numeric_resolution_.getX() == editor.graph_.getX() + graph.getX()
                && editor.numeric_paste_.getRight() == editor.graph_.getX() + graph.getRight(),
                "numeric controls fit graph width at every scale");
            for (const int height : {256,384,graph.getHeight()}) {
                const juce::Rectangle<int> bounds(30,26,640,height);
                std::array<bool,256> reachable{};
                for (int y=bounds.getY(); y<bounds.getBottom(); ++y)
                    reachable[static_cast<int>(SccWaveGraph::sampleValueAtY(static_cast<float>(y),bounds)) + 128] = true;
                check(std::all_of(reachable.begin(),reachable.end(),[](bool v) { return v; }),
                    "all 256 signed values reachable inside graph");
                for (int value=-128; value<=127; ++value) {
                    const auto y = SccWaveGraph::sampleY(static_cast<std::int8_t>(value), bounds);
                    check(SccWaveGraph::sampleValueAtY(y,bounds) == value, "draw and edit amplitude scales agree");
                }
            }
            const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
            auto output = juce::File::getCurrentWorkingDirectory().getChildFile(
                "scc-numeric-" + juce::String(percent) + ".png").createOutputStream();
            check(output != nullptr && juce::PNGImageFormat().writeImageToStream(image, *output),
                "numeric editor snapshot saved");
            {
                juce::DocumentWindow host("SCC numeric layout", juce::Colours::black,
                    juce::DocumentWindow::allButtons);
                host.setContentNonOwned(&editor, false);
                host.setSize(UiScale::sx(1280), 800);
                editor.resized();
                check(editor.graph_.graphBounds().getHeight() >= 256,
                    "host-clamped editor preserves full mouse resolution via viewport");
                check(editor.ab_audition_.getWidth() == UiScale::sx(52),
                    "small host preserves existing preset controls through horizontal scrolling");
                editor.waveform_viewport_.setViewPosition(0,
                    editor.waveform_content_.getHeight());
                check(editor.waveform_viewport_.getViewArea().contains(editor.numeric_input_.getBounds()),
                    "numeric input row remains reachable when host is small");
                check(editor.waveform_content_.getWidth() == editor.waveform_viewport_.getMaximumVisibleWidth(),
                    "scrollbar transition keeps Paste inside content width");
                host.setTopLeftPosition(-10000, -10000);
                host.setVisible(true);
                editor.editor_viewport_.setViewPosition(0,0);
                editor.numeric_input_.grabKeyboardFocus();
                editor.editor_viewport_.focusOfChildComponentChanged(juce::Component::focusChangedDirectly);
                const auto focused_bounds = editor.editor_canvas_.getLocalArea(
                    &editor.numeric_input_, editor.numeric_input_.getLocalBounds());
                check(editor.editor_viewport_.getViewArea().contains(focused_bounds),
                    "keyboard focus reveals numeric input in clamped editor");
                host.setSize(UiLayout::editorWindowW, UiLayout::editorWindowH + UiScale::sx(180));
                check(editor.waveform_content_.getWidth() == editor.waveform_viewport_.getMaximumVisibleWidth(),
                    "expansion reconciles waveform content width");
                host.setVisible(false);
                host.clearContentComponent();
            }
        }
        editor.applyEditorUiScale(100);
    }
    template<class Editor>
    static const std::optional<mgstc::engine::SavedTimbreReference>&
    ownedSnapshot(const Editor& editor) {
        return editor.owned_edit_snapshot_;
    }
    template<class Editor>
    static void activationRefresh(Editor& editor) {
        editor.engine_ready_ = true;
        editor.refreshExternalState();
    }
    static void editOwned(SccEditorComponent& editor) {
        editor.saveSccImmediateAuditionSetting(false);
        editor.name_.setText("Edited SCC A", juce::dontSendNotification);
        auto waveform = editor.scc_wave_;
        waveform[3] = static_cast<std::int8_t>(waveform[3] ^ 0x3f);
        editor.commitWave(waveform);
    }
    static void editOwned(OpllEditorComponent& editor) {
        editor.saveOpllImmediateAuditionSetting(false);
        editor.name_.setText("Edited OPLL A", juce::dontSendNotification);
        auto registers = mgstc::engine::encodeOpllPatch(editor.patch_);
        registers[3] ^= 0x07;
        editor.commitPatch(mgstc::engine::decodeOpllPatch(registers));
    }
    static bool check(SccEditorComponent& scc, OpllEditorComponent& opll) {
        scc.saveSccImmediateAuditionSetting(false);
        opll.saveOpllImmediateAuditionSetting(false);
        scc.saveLastAuditionNoteSetting(73);
        for (int i = 0; i < 60; ++i) { scc.timerCallback(); opll.timerCallback(); }
        return !scc.immediate_audition_.getToggleState()
            && !opll.immediate_audition_.getToggleState()
            && scc.last_audition_note_ == 73 && opll.last_audition_note_ == 73;
    }
};

struct CompositeEditorComponentTestAccess {
    static const juce::String title(const CompositeEditorComponent& editor) {
        return editor.title_.getText();
    }
    static void pickerPreview(CompositeEditorComponent& editor,
        const mgstc::engine::CompositeTimbre& sound) {
        editor.manual_y_preview_timbre_ = sound;
    }
    static bool hasPickerPreview(const CompositeEditorComponent& editor) {
        return editor.manual_y_preview_timbre_.has_value();
    }
    static void commitTimelineSound(CompositeEditorComponent& editor,
        const mgstc::engine::CompositeTimbre& sound) {
        // Stage edited settings, then use the real timeline commit callback.
        editor.timeline_.setTimbre(sound);
        editor.timeline_.clearLoop(0);
    }
    static bool previewPending(const CompositeEditorComponent& editor) {
        return editor.timeline_preview_pending_;
    }
    static void fireTimelinePreview(CompositeEditorComponent& editor) {
        editor.timeline_preview_due_ms_ = 0;
        editor.timerCallback();
    }
    static void size(CompositeEditorComponent& editor, int width, int height) {
        editor.setSize(width, height);
    }
    static void immediate(CompositeEditorComponent& editor, bool enabled) {
        editor.immediate_audition_.setToggleState(enabled, juce::dontSendNotification);
        editor.saveCompositeImmediateAuditionSetting(enabled);
    }
    static bool immediate(const CompositeEditorComponent& editor) {
        return editor.immediate_audition_.getToggleState();
    }
    static void note(CompositeEditorComponent& editor, std::uint8_t note) {
        editor.saveLastAuditionNoteSetting(note);
    }
    static std::uint8_t note(const CompositeEditorComponent& editor) {
        return editor.last_audition_note_;
    }
    static const mgstc::engine::CompositeTimbre& sound(const CompositeEditorComponent& editor) { return editor.timbre_; }
    static juce::ComboBox* originalAllocationCombo(
        CompositeEditorComponent& editor, int item_count) {
        const auto find = [item_count](const auto& self, juce::Component& parent) -> juce::ComboBox* {
            for (int index = 0; index < parent.getNumChildComponents(); ++index) {
                auto* child = parent.getChildComponent(index);
                auto* combo = dynamic_cast<juce::ComboBox*>(child);
                if (combo != nullptr && combo->getNumItems() == item_count
                    && combo->getItemText(0) == (item_count == 32 ? "@0" : "@16")
                    && combo->getItemText(item_count - 1) == "@31") return combo;
                if (auto* found = self(self, *child)) return found;
            }
            return nullptr;
        };
        return find(find, editor.timeline_);
    }
    static void setOriginalAllocationStart(
        CompositeEditorComponent& editor, int item_count, int number) {
        auto* combo = originalAllocationCombo(editor, item_count);
        if (combo == nullptr)
            throw std::runtime_error("original allocation selector is missing");
        combo->setSelectedId(number + 1, juce::sendNotificationSync);
    }
    static int originalAllocationStart(
        CompositeEditorComponent& editor, int item_count) {
        auto* combo = originalAllocationCombo(editor, item_count);
        return combo == nullptr ? -1 : combo->getSelectedId() - 1;
    }
    static bool setLoopStart(CompositeEditorComponent& editor, std::size_t layer,
                             int count) {
        return editor.timeline_.setLoopStart(layer, count);
    }
    static void clearLoop(CompositeEditorComponent& editor, std::size_t layer) {
        editor.timeline_.clearLoop(layer);
    }
    static bool wavEnabled(const CompositeEditorComponent& editor) { return editor.import_wave_.isEnabled(); }
    static bool hasWaveConversionCaption(const CompositeEditorComponent& editor) {
        return editor.import_wave_.getButtonText() == juce::String::fromUTF8("WAV変換");
    }
    static std::size_t historySize(const CompositeEditorComponent& editor) { return editor.history_.size(); }
    static std::size_t historyCursor(const CompositeEditorComponent& editor) { return editor.history_cursor_; }
    static void applyWave(CompositeEditorComponent& editor, const mgstc::engine::CompositeTimbre& sound) { editor.applyWaveConversion(sound); }
    static void bindWaveAudition(CompositeEditorComponent& editor, CompositeWavConversionContent& content) {
        editor.bindWaveConversionAudition(content);
    }
    static bool hasWavePreview(const CompositeEditorComponent& editor) { return editor.wave_conversion_preview_.has_value(); }
    static bool waveAuditionPending(const CompositeEditorComponent& editor) { return editor.wave_preview_audition_pending_; }
    static const mgstc::engine::CompositeTimbre& wavePreview(const CompositeEditorComponent& editor) { return *editor.wave_conversion_preview_; }
    static void undo(CompositeEditorComponent& editor) { editor.undo(); }
    static void redo(CompositeEditorComponent& editor) { editor.redo(); }
    static void restoreHostState(CompositeEditorComponent& editor,
        const mgstc::engine::CompositeTimbre& sound) {
        editor.restoreHostState(sound);
    }
    static void recordHistory(CompositeEditorComponent& editor) {
        editor.recordHistory();
    }
    static bool matches(const CompositeEditorComponent& editor, const mgstc::engine::CompositeTimbre& snapshot) {
        return editor.waveConversionSnapshotMatches(snapshot, {});
    }
    static void editName(CompositeEditorComponent& editor, const std::string& name) { editor.timbre_.name = name; }
    static void pollOwned(CompositeEditorComponent& editor) { editor.timerCallback(); }
    static void assignBase(CompositeEditorComponent& editor, std::size_t layer,
        std::optional<std::uint64_t> id) { editor.assignLayerLibraryId(layer, id); }
    static void start(CompositeEditorComponent& editor, std::uint8_t note,
        bool one_second = false) {
        editor.startCompositeNote(note, one_second);
    }
    static void release(CompositeEditorComponent& editor, std::uint8_t note,
        double time) {
        editor.releaseCompositeNote(note, time);
    }
    static void dispatch(CompositeEditorComponent& editor, double time) {
        editor.dispatchScheduledLayerNotes(time);
    }
    static double onset(const CompositeEditorComponent& editor) {
        return editor.scheduled_layer_notes_.front().start_time_ms
            - editor.scheduled_layer_notes_.front().delay_ms;
    }
    static std::size_t scheduled(const CompositeEditorComponent& editor) {
        return editor.scheduled_layer_notes_.size();
    }
    static std::size_t sounding(const CompositeEditorComponent& editor) {
        return editor.sounding_tracks_.size();
    }
    static std::size_t voices(const CompositeEditorComponent& editor) {
        return editor.voice_allocator_.activeVoiceCount();
    }
    static void stop(CompositeEditorComponent& editor) {
        editor.silenceAuditionNotes();
    }
    static bool rebuild(CompositeEditorComponent& editor) {
        return editor.configureEngine();
    }
    static bool managerPreview(CompositeEditorComponent& editor,
        const mgstc::engine::CompositeTimbre& preview) {
        editor.library_manager_saved_timbre_ = editor.timbre_;
        editor.timbre_ = preview;
        editor.composite_program_stale_ = true;
        return editor.configureEngine();
    }
    static void setPolyphonic(CompositeEditorComponent& editor, bool enabled) {
        editor.performance_keyboard_.setPolyphonic(enabled);
    }
    static bool startsAndStopsShiftedEqually(
        const CompositeEditorComponent& editor, double hold_ms) {
        const auto origin = onset(editor);
        return std::all_of(editor.scheduled_layer_notes_.begin(),
            editor.scheduled_layer_notes_.end(),
            [origin, hold_ms](const auto& layer) {
                return layer.stop_time_ms
                    && std::fabs(layer.start_time_ms - origin - layer.delay_ms)
                        < 1.0e-6
                    && std::fabs(*layer.stop_time_ms - layer.start_time_ms
                            - hold_ms) < 1.0e-6;
            });
    }
    static bool containsNote(const CompositeEditorComponent& editor,
        std::uint8_t note) {
        return std::any_of(editor.scheduled_layer_notes_.begin(),
            editor.scheduled_layer_notes_.end(),
            [note](const auto& layer) { return layer.base_note == note; });
    }
};

struct CompositeWavConversionTestAccess {
    static bool accept(CompositeWavConversionContent& content,
        mgstc::engine::CompositeWavConversionResult result,
        std::shared_ptr<const mgstc::engine::SourcePcm> pcm, bool cancelled = false) {
        return content.acceptCompletedConversion(std::move(result), std::move(pcm), cancelled, 0.5f, 0.25f);
    }
    static const std::optional<mgstc::engine::CompositeWavConversionResult>& result(const CompositeWavConversionContent& content) { return content.result_; }
    static void source(CompositeWavConversionContent& content, std::shared_ptr<const mgstc::engine::SourcePcm> pcm) {
        content.source_ = std::move(pcm); content.analysis_.reset(); content.analysis_summary_.clear();
        content.range_.setSource(content.source_); content.syncRange(); content.refreshEnabled();
    }
    static void load(CompositeWavConversionContent& content,
        std::function<mgstc::platform::AudacityWaveImportResult()> loader) {
        content.loadWaveSource(std::move(loader), "Audacity selection", "Acquiring audio");
    }
    static bool loading(const CompositeWavConversionContent& content) { return content.busy_dialog_ != nullptr; }
    static std::shared_ptr<const mgstc::engine::SourcePcm> source(const CompositeWavConversionContent& content) { return content.source_; }
    static auto selection(const CompositeWavConversionContent& content) { return content.range_.selection(); }
    static void cancelLoad(CompositeWavConversionContent& content) { content.analysis_cancel_->store(true); }
    static void seedAnalysis(CompositeWavConversionContent& content,
        std::shared_ptr<const mgstc::engine::SourceAnalysis> analysis) { content.analysis_ = std::move(analysis); }
    static std::shared_ptr<const mgstc::engine::SourceAnalysis> analysis(const CompositeWavConversionContent& content) { return content.analysis_; }
    static bool analysisMatches(const CompositeWavConversionContent& content) { return content.analysisMatchesInputs(); }
    static void setPitch(CompositeWavConversionContent& content, const juce::String& pitch) {
        content.pitch_.setText(pitch, true);
        // TextEditor posts textChangeMessageId instead of invoking onTextChange inline.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
    static void setKeyOff(CompositeWavConversionContent& content, const juce::String& keyoff) { content.key_off_.setText(keyoff, true); }
    static void setConfiguration(CompositeWavConversionContent& content, int id) {
        content.configuration_.setSelectedId(id, juce::sendNotificationSync);
    }
    static void setBudget(CompositeWavConversionContent& content, int count) { content.budget_.setText(juce::String(count), false); }
    static void setSourceBudget(CompositeWavConversionContent& content, int count) { content.source_budget_.setSelectedId(count + 1, juce::sendNotificationSync); }
    static int sourceBudget(const CompositeWavConversionContent& content) { return content.source_budget_.getSelectedId() - 1; }
    static void clickAnalyze(CompositeWavConversionContent& content) { content.analyze_.onClick(); }
    static void clickConvert(CompositeWavConversionContent& content) { content.convert_.onClick(); }
    static void clickApply(CompositeWavConversionContent& content) { content.apply_.onClick(); }
    static void clickCancel(CompositeWavConversionContent& content) { content.cancel_.onClick(); }
    static void selectCandidate(CompositeWavConversionContent& content, int row) { content.candidates_.selectRow(row); }
    static int selectedCandidate(const CompositeWavConversionContent& content) { return content.selected_candidate_; }
    static auto selectedTone(const CompositeWavConversionContent& content) { return content.selectedCandidate()->tone; }
    static void previewSelected(CompositeWavConversionContent& content) { content.previewResult(); }
    static void previewSource(CompositeWavConversionContent& content) { content.previewSource(); }
    static void releaseRangeHandle(CompositeWavConversionContent& content, bool start) { content.range_.changed(start); }
    static void stopPreview(CompositeWavConversionContent& content) { content.stop_.onClick(); }
    static bool moveCandidate(CompositeWavConversionContent& content, int key) { return content.candidates_.keyPressed(juce::KeyPress(key)); }
    static bool previewPending(const CompositeWavConversionContent& content) { return content.preview_dialog_ != nullptr; }
    static auto alternatePcm(const CompositeWavConversionContent& content) { return content.alternate_pcm_; }
    static bool candidateLayout(const CompositeWavConversionContent& content) {
        const auto right = content.candidates_.getBounds(); const auto left = content.status_.getBounds();
        const auto width = left.getWidth() + right.getWidth();
        std::array<const juce::Component*, 12> controls{&content.begin_, &content.end_, &content.pitch_, &content.key_off_, &content.budget_, &content.source_budget_,
            &content.configuration_, &content.strategy_, &content.fixed_tone_, &content.preference_, &content.sustain_, &content.wave_method_};
        for (std::size_t i = 0; i < controls.size(); ++i) {
            const auto label = content.labels_[i].getBounds(); const auto field = controls[i]->getBounds();
            if (label.getY() != field.getY() || label.getHeight() != UiLayout::fieldH
                || field.getHeight() != UiLayout::fieldH || label.getRight() + UiLayout::controlGap != field.getX()) return false;
        }
        return content.level_match_.getX() == content.stop_.getRight() + UiLayout::controlGap
            && content.level_match_.getY() == content.stop_.getY()
            && !content.level_match_.getBounds().intersects(content.zoom_in_.getBounds())
            && content.begin_.getY() == content.stop_.getBottom() + UiLayout::sm
            && content.source_budget_.getX() == content.budget_.getX()
            && content.source_budget_.getY() == content.budget_.getBottom() + UiLayout::sm
            && left.getY() - content.source_budget_.getBottom() >= UiLayout::panelGap
            && content.candidates_.getRowHeight() == UiLayout::fieldH + UiLayout::xs * 2
            && left.getHeight() >= UiScale::sx(100) && right.getHeight() > 0
            && content.getLocalBounds().contains(right) && !left.intersects(right)
            && right.getX() == left.getRight() + UiLayout::panelGap
            && std::abs(right.getWidth() - width * 35 / 100) <= 1;
    }
    static bool candidatesEmpty(const CompositeWavConversionContent& content) { return content.candidates_empty_.isVisible(); }
    static int waveMethod(const CompositeWavConversionContent& content) { return content.wave_method_.getSelectedId(); }
    static void setWaveMethod(CompositeWavConversionContent& content, int id) { content.wave_method_.setSelectedId(id, juce::sendNotificationSync); }
    static bool hasAudacityBesideLoad(const CompositeWavConversionContent& content) {
        return content.load_.getButtonText() == juce::String::fromUTF8("WAVを開く…")
            && content.audacity_.getButtonText() == juce::String::fromUTF8("Audacityから変換")
            && content.audacity_.isEnabled()
            && content.audacity_.getX() == content.load_.getRight() + UiLayout::controlGap
            && content.audacity_.getY() == content.load_.getY()
            && content.getLocalBounds().contains(content.audacity_.getBounds())
            && content.metadata_.getX() == content.audacity_.getRight() + UiLayout::controlGap;
    }
    static bool hasComparisonButtonsWithoutAb(const CompositeWavConversionContent& content) {
        bool original = false, converted = false, stop = false;
        for (auto* child : content.getChildren()) {
            if (auto* button = dynamic_cast<juce::TextButton*>(child)) {
                const auto label = button->getButtonText();
                if (label.contains("A/B")) return false;
                original |= label == juce::String::fromUTF8("原音 ▶");
                converted |= label == juce::String::fromUTF8("変換音 ▶");
                stop |= label == juce::String::fromUTF8("停止");
            }
        }
        return original && converted && stop;
    }
    static bool hasResult(const CompositeWavConversionContent& content) { return content.result_.has_value(); }
    static juce::String status(const CompositeWavConversionContent& content) { return content.status_.getText(); }
    static void setSelection(CompositeWavConversionContent& content, std::size_t begin, std::size_t end) {
        content.range_.setSelection(begin, end);
        if (content.range_.changed) content.range_.changed(true);
    }
    static void tab(CompositeWavRange& range) { range.active_start_ = !range.active_start_; }
    static bool matchOn(const CompositeWavConversionContent& content) { return content.level_match_.getToggleState(); }
    static bool buttonsInside(const CompositeWavConversionContent& content) {
        return content.getLocalBounds().contains(content.level_match_.getBounds())
            && content.level_match_.getWidth() >= UiScale::sx(250)
            && content.getLocalBounds().contains(content.wave_method_.getBounds())
            && !content.wave_method_.getBounds().intersects(content.level_match_.getBounds())
            && content.wave_method_.getWidth() >= UiScale::sx(180)
            && content.getLocalBounds().contains(content.analyze_.getBounds())
            && content.status_.getHeight() >= UiScale::sx(100)
            && content.getLocalBounds().contains(content.apply_.getBounds());
    }
};

}  // namespace mgstc::app

namespace mgstc::plugin {

struct MgstcAudioProcessorTestAccess {
    static void notifyStateRestored(MgstcAudioProcessor& processor) {
        processor.notifyEditorStateRestored();
    }
    static std::uint64_t frame(const MgstcAudioProcessor& processor) {
        return processor.engine_frame_position_;
    }

    static const LayerDelayScheduler& scheduler(
        const MgstcAudioProcessor& processor) {
        return processor.scheduler_;
    }

    static const mgstc::engine::CompositePlaybackPlan& plan(
        const MgstcAudioProcessor& processor) {
        return processor.activePlan();
    }

    static std::size_t noteOnCount(const MgstcAudioProcessor& processor) {
        return processor.note_on_log_size_;
    }

    static std::uint64_t hostFrame(const MgstcAudioProcessor& processor) {
        return processor.host_frame_position_;
    }

    static std::uint32_t hostRate(const MgstcAudioProcessor& processor) {
        return processor.host_rate_hz_;
    }

    static bool usesSrc(const MgstcAudioProcessor& processor) {
        return processor.use_src_;
    }

    static bool logHas(
        const MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t track) {
        for (std::size_t index = 0; index < processor.note_on_log_size_;
             ++index) {
            const auto& entry = processor.note_on_log_[index];
            if (entry.frame == frame && entry.physical_track == track) {
                return true;
            }
        }
        return false;
    }

    static bool logHasTrack(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        for (std::size_t index = 0; index < processor.note_on_log_size_;
             ++index) {
            if (processor.note_on_log_[index].physical_track == track) {
                return true;
            }
        }
        return false;
    }

    static bool isInactive(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        return processor.track_state_[track]
            == MgstcAudioProcessor::TrackLayerState::Inactive;
    }

    static bool isActive(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        return processor.track_state_[track]
            == MgstcAudioProcessor::TrackLayerState::Active;
    }

    static std::uint32_t srcCallbacks(const MgstcAudioProcessor& processor) {
        return processor.src_callbacks_;
    }

    static std::uint32_t srcFillIters(const MgstcAudioProcessor& processor) {
        return processor.src_fill_iters_;
    }

    static std::uint32_t srcZeroProgress(const MgstcAudioProcessor& processor) {
        return processor.src_zero_progress_;
    }

    static std::uint32_t srcKnownEndRejects(
        const MgstcAudioProcessor& processor) {
        return processor.src_known_end_rejects_;
    }

    static std::uint32_t srcWatchdogTrips(
        const MgstcAudioProcessor& processor) {
        return processor.src_watchdog_trips_;
    }

    static std::uint64_t srcStallEngine(const MgstcAudioProcessor& processor) {
        return processor.src_stall_engine_;
    }

    static std::uint64_t srcStallMapped(const MgstcAudioProcessor& processor) {
        return processor.src_stall_mapped_;
    }

    static std::uint64_t srcStallKnownEnd(
        const MgstcAudioProcessor& processor) {
        return processor.src_stall_known_end_;
    }

    static std::uint64_t knownEngineEnd(
        const MgstcAudioProcessor& processor) {
        return processor.known_engine_end_;
    }

    static std::uint64_t srcProcessNs(const MgstcAudioProcessor& processor) {
        return processor.src_process_ns_;
    }

    static std::uint64_t srcFillNs(const MgstcAudioProcessor& processor) {
        return processor.src_fill_ns_;
    }

    static std::uint64_t srcRenderNs(const MgstcAudioProcessor& processor) {
        return processor.src_render_ns_;
    }

    static std::uint64_t copyNs(const MgstcAudioProcessor& processor) {
        return processor.copy_ns_;
    }

    static std::uint32_t renderAudioCalls(
        const MgstcAudioProcessor& processor) {
        return processor.render_audio_calls_;
    }

    static bool pluginQuiescent(const MgstcAudioProcessor& processor) {
        return processor.pluginQuiescent();
    }

    static bool hasRealtimeWork(const MgstcAudioProcessor& processor) {
        return processor.engine_.hasRealtimeWork();
    }

    static std::uint64_t processBlockCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_process_block_.load(std::memory_order_relaxed);
    }

    static std::uint64_t prepareToPlayCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_prepare_to_play_.load(std::memory_order_relaxed);
    }

    static std::uint64_t releaseResourcesCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_release_resources_.load(
            std::memory_order_relaxed);
    }

    static std::uint64_t resetCount(const MgstcAudioProcessor& processor) {
        return processor.diag_reset_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyCalls(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_calls_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencySame(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_same_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyChanged(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_changed_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyFromProcess(
        const MgstcAudioProcessor& processor) {
        return processor.diag_latency_from_process_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastNumSamples(const MgstcAudioProcessor& processor) {
        return processor.diag_last_num_samples_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastBufferChannels(
        const MgstcAudioProcessor& processor) {
        return processor.diag_last_buffer_channels_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastMidiEvents(const MgstcAudioProcessor& processor) {
        return processor.diag_last_midi_events_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t processSampleRateHz(
        const MgstcAudioProcessor& processor) {
        return processor.diag_process_sample_rate_hz_.load(
            std::memory_order_relaxed);
    }

    static std::uint64_t sampleRateChangesInProcess(
        const MgstcAudioProcessor& processor) {
        return processor.diag_sample_rate_changes_in_process_.load(
            std::memory_order_relaxed);
    }

    static constexpr int diagnosticMode() {
        return kVst3DiagMode;
    }

    static void setEngineFrame(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        processor.engine_frame_position_ = frame;
    }

    static std::size_t mappedIndex(const MgstcAudioProcessor& processor) {
        return processor.mapped_index_;
    }

    static std::size_t mappedCount(const MgstcAudioProcessor& processor) {
        return processor.mapped_count_;
    }

    static void resetMapped(MgstcAudioProcessor& processor) {
        processor.mapped_count_ = 0;
        processor.mapped_index_ = 0;
    }

    static bool plantMapped(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        MgstcAudioProcessor::MappedMidiType type,
        std::uint8_t note) {
        if (processor.mapped_count_ >= processor.mapped_events_.size()) {
            return false;
        }
        auto& event = processor.mapped_events_[processor.mapped_count_++];
        event.engine_frame = frame;
        event.type = type;
        event.midi_note = note;
        return true;
    }

    static bool plantNoteOn(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t note) {
        return plantMapped(
            processor, frame, MgstcAudioProcessor::MappedMidiType::NoteOn, note);
    }

    static bool plantNoteOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t note) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::NoteOff,
            note);
    }

    static bool plantAllNotesOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::AllNotesOff,
            0);
    }

    static bool plantAllSoundOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::AllSoundOff,
            0);
    }

    static void processEventsAt(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        processor.engine_frame_position_ = frame;
        processor.processEventsAt(frame);
    }

    static void bindMidiIterator(
        MgstcAudioProcessor& processor,
        juce::MidiBuffer& midi,
        std::uint64_t host_block_start,
        int host_block_samples) {
        processor.mapped_count_ = 0;
        processor.mapped_index_ = 0;
        processor.midi_it_ = midi.begin();
        processor.midi_end_ = midi.end();
        processor.src_host_block_start_ = host_block_start;
        processor.src_host_block_samples_ = host_block_samples;
        processor.use_src_ = false;
    }

    static void clearMidiIterator(MgstcAudioProcessor& processor) {
        processor.midi_it_ = {};
        processor.midi_end_ = {};
    }

    static void renderDirect(
        MgstcAudioProcessor& processor,
        juce::AudioBuffer<float>& buffer,
        std::uint64_t block_start) {
        processor.engine_frame_position_ = block_start;
        processor.known_engine_end_ =
            block_start + static_cast<std::uint64_t>(buffer.getNumSamples());
        processor.resetBlockDiagnostics();
        processor.renderDirectSpan(buffer, block_start);
    }

    static std::size_t logSize(const MgstcAudioProcessor& processor) {
        return processor.note_on_log_size_;
    }

    static std::uint64_t logFrame(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].frame;
    }

    static std::uint8_t logTrack(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].physical_track;
    }

    static std::uint8_t logNote(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].midi_note;
    }

    static std::uint64_t blockedBackendCalls(
        const MgstcAudioProcessor& processor) {
        return processor.editor_blocked_backend_calls_;
    }

    static bool scopeEnabled(const MgstcAudioProcessor& processor) {
        return processor.engine_.opllScopeEnabled();
    }
    static void keyoffSeconds(MgstcAudioProcessor& processor, int seconds) {
        processor.opll_keyoff_silence_seconds_.store(seconds);
    }
    static std::uint64_t keyoffDue(const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        return processor.opll_silence_due_[track];
    }

    static std::uint64_t stateCompileCount(
        const MgstcAudioProcessor& processor) {
        return processor.state_compile_count_.load(std::memory_order_relaxed);
    }

    static std::uint64_t stateAudioRejectCount(
        const MgstcAudioProcessor& processor) {
        return processor.state_audio_reject_count_.load(
            std::memory_order_relaxed);
    }

    static void poseAsAudioThread(MgstcAudioProcessor& processor) {
        processor.poseAsAudioThreadForTest();
    }

    static void clearAudioThread(MgstcAudioProcessor& processor) {
        processor.clearAudioThreadForTest();
    }
};

}  // namespace mgstc::plugin

namespace {

using Access = mgstc::plugin::MgstcAudioProcessorTestAccess;
using mgstc::plugin::delayMillisecondsToEngineFrames;
using mgstc::plugin::hostFrameToEngineFrame;
using mgstc::plugin::kVst3DiagOff;
using mgstc::plugin::MgstcAudioProcessor;
using mgstc::plugin::PluginEditorContext;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

double channelPeak(
    const juce::AudioBuffer<float>& buffer,
    int start,
    int count) {
    double peak = 0.0;
    const int end = std::min(start + count, buffer.getNumSamples());
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        const auto* samples = buffer.getReadPointer(channel);
        for (int i = start; i < end; ++i) {
            peak = std::max(peak, static_cast<double>(std::fabs(samples[i])));
        }
    }
    return peak;
}

void prime(MgstcAudioProcessor& processor, int block) {
    processor.prepareToPlay(48'000.0, block);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

const mgstc::engine::CompositePlaybackLayer* layerOf(
    const mgstc::engine::CompositePlaybackPlan& plan,
    mgstc::engine::TimbreSource source) {
    for (const auto& layer : plan.audible_layers) {
        if (layer.source == source) {
            return &layer;
        }
    }
    return nullptr;
}

void processEmpty(MgstcAudioProcessor& processor, int frames) {
    while (frames > 0) {
        const int chunk = std::min(frames, 512);
        juce::AudioBuffer<float> buffer(2, chunk);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        frames -= chunk;
    }
}

void loadDelayedThreeLayerProgram(MgstcAudioProcessor& processor) {
    mgstc::plugin::PluginStateDocument document;
    document.sound = mgstc::engine::defaultCompositeTimbre();
    // Keep this scheduler fixture at 200/400 ms as r% follows tempo.
    document.sound.layers[1].start_delay_form =
        mgstc::engine::StartDelayForm::NoteLength;
    document.sound.layers[1].start_delay_value = 10;
    document.sound.layers[2].start_delay_form =
        mgstc::engine::StartDelayForm::NoteLength;
    document.sound.layers[2].start_delay_value = 5;
    require(
        processor.replacePluginState(std::move(document)),
        "load delayed three-layer composite");
}

void processUntilEngine(
    MgstcAudioProcessor& processor,
    std::uint64_t target,
    int host_block = 512) {
    while (Access::frame(processor) < target) {
        juce::AudioBuffer<float> buffer(2, host_block);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
}

void processUntilFrame(MgstcAudioProcessor& processor, std::uint64_t target) {
    while (Access::frame(processor) < target) {
        const auto remaining = target - Access::frame(processor);
        const int chunk = static_cast<int>(
            std::min<std::uint64_t>(remaining, 512));
        juce::AudioBuffer<float> buffer(2, chunk);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
}

void testNoteOnAtSample128DoesNotAffectPrefix() {
    MgstcAudioProcessor processor;
    constexpr int kBlock = 2048;
    prime(processor, kBlock);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        128);
    processor.processBlock(buffer, midi);
    require(
        channelPeak(buffer, 0, 128) < 0.001,
        "samples before MIDI note on were not silent");

    double later = channelPeak(buffer, 128, kBlock - 128);
    if (later <= 0.001) {
        juce::AudioBuffer<float> more(2, kBlock);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        later = channelPeak(more, 0, kBlock);
    }
    require(later > 0.001, "note on at sample 128 produced no audio");
}

void processSilence(
    MgstcAudioProcessor& processor,
    int host_frames,
    int block) {
    while (host_frames > 0) {
        const int n = std::min(host_frames, block);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        host_frames -= n;
    }
}

std::string srcDiag(const MgstcAudioProcessor& processor) {
    char line[768];
    std::snprintf(
        line,
        sizeof(line),
        "rate=%u host=%llu engine=%llu cb=%u fill_iters=%u zero_progress=%u "
        "known_end_rej=%u watchdog=%u stall_engine=%llu stall_mapped=%llu "
        "stall_known_end=%llu",
        Access::hostRate(processor),
        static_cast<unsigned long long>(Access::hostFrame(processor)),
        static_cast<unsigned long long>(Access::frame(processor)),
        Access::srcCallbacks(processor),
        Access::srcFillIters(processor),
        Access::srcZeroProgress(processor),
        Access::srcKnownEndRejects(processor),
        Access::srcWatchdogTrips(processor),
        static_cast<unsigned long long>(Access::srcStallEngine(processor)),
        static_cast<unsigned long long>(Access::srcStallMapped(processor)),
        static_cast<unsigned long long>(Access::srcStallKnownEnd(processor)));
    return line;
}

void testCubaseSilenceThenFirstNoteOn(
    double rate,
    int block,
    int silence_host,
    int note_offset = 0) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);
    processSilence(processor, silence_host, block);

    const int offset = std::clamp(note_offset, 0, std::max(block - 1, 0));
    const auto host_before = Access::hostFrame(processor);
    const auto engine_before = Access::frame(processor);
    const auto mapped_host = host_before + static_cast<std::uint64_t>(offset);
    const auto mapped_note = hostFrameToEngineFrame(
        mapped_host, Access::hostRate(processor));

    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        offset);
    const auto started = std::chrono::steady_clock::now();
    processor.processBlock(buffer, midi);
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();

    std::fprintf(
        stderr,
        "Cubase first-note rate=%.1f block=%d silence=%d offset=%d "
        "host_before=%llu engine_before=%llu mapped=%llu "
        "known_end=%llu elapsed_ms=%.3f %s\n",
        rate,
        block,
        silence_host,
        offset,
        static_cast<unsigned long long>(host_before),
        static_cast<unsigned long long>(engine_before),
        static_cast<unsigned long long>(mapped_note),
        static_cast<unsigned long long>(Access::knownEngineEnd(processor)),
        elapsed_ms,
        srcDiag(processor).c_str());

    require(
        elapsed_ms < 250.0,
        "first Note On processBlock did not return in bounded time");
    if (Access::srcWatchdogTrips(processor) != 0) {
        throw std::runtime_error(
            std::string("SRC zero-progress infinite loop at first Note On: ")
            + srcDiag(processor));
    }
    const auto fill = Access::srcFillIters(processor);
    require(
        fill < 64,
        "source generation iterated excessively at first Note On");
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    require(
        Access::logHasTrack(processor, track),
        "first Note On after silence must dispatch PSG");
}

void measureSilenceVersusNoteOnLoad(double rate, int block) {
    MgstcAudioProcessor silent;
    silent.prepareToPlay(rate, block);
    juce::AudioBuffer<float> silence_buf(2, block);
    juce::MidiBuffer empty;
    const auto silence_started = std::chrono::steady_clock::now();
    silent.processBlock(silence_buf, empty);
    const auto silence_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - silence_started)
            .count();
    const auto silence_cb = Access::srcCallbacks(silent);

    MgstcAudioProcessor sounding;
    sounding.prepareToPlay(rate, block);
    juce::AudioBuffer<float> note_buf(2, block);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    const auto note_started = std::chrono::steady_clock::now();
    sounding.processBlock(note_buf, on);
    const auto note_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - note_started)
            .count();
    const auto note_cb = Access::srcCallbacks(sounding);

    std::fprintf(
        stderr,
        "SRC load rate=%.1f block=%d silence_ms=%.3f silence_cb=%u "
        "note_ms=%.3f note_cb=%u zero_progress=%u watchdog=%u\n",
        rate,
        block,
        silence_ms,
        silence_cb,
        note_ms,
        note_cb,
        Access::srcZeroProgress(sounding),
        Access::srcWatchdogTrips(sounding));

    require(note_cb <= silence_cb + 2, "first Note On must not explode SRC callbacks");
    require(
        Access::srcWatchdogTrips(sounding) == 0,
        "first Note On load probe tripped zero-progress watchdog");
}

void testCubaseFirstNoteOnAfterSilence() {
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 64, 44'100);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 44'100);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 64, 96'000);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 96'000);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 1);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 7);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 128);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 511);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0, 1);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0, 511);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 1, 0, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 1, 0, 0);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 32, 0, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 32, 0, 0);
    measureSilenceVersusNoteOnLoad(44'100.0, 512);
    measureSilenceVersusNoteOnLoad(96'000.0, 512);
    measureSilenceVersusNoteOnLoad(48'000.0, 512);
}

void testEngineNeverLeadsHostMapping() {
    constexpr std::array rates{44'100.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto host_rate = Access::hostRate(processor);
        int remaining = static_cast<int>(rate);
        std::uint32_t rejects = 0;
        while (remaining > 0) {
            const int n = std::min(remaining, 512);
            juce::AudioBuffer<float> buffer(2, n);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            remaining -= n;
            rejects += Access::srcKnownEndRejects(processor);
            const auto host = Access::hostFrame(processor);
            const auto engine = Access::frame(processor);
            const auto mapped = hostFrameToEngineFrame(host, host_rate);
            if (engine > mapped) {
                throw std::runtime_error(
                    "engine lead host mapping: rate="
                    + std::to_string(rate)
                    + " host=" + std::to_string(host)
                    + " engine=" + std::to_string(engine)
                    + " mapped=" + std::to_string(mapped));
            }
        }
        std::fprintf(
            stderr,
            "SRC known_end cap rate=%.1f host=%llu engine=%llu mapped=%llu "
            "rejects=%u\n",
            rate,
            static_cast<unsigned long long>(Access::hostFrame(processor)),
            static_cast<unsigned long long>(Access::frame(processor)),
            static_cast<unsigned long long>(hostFrameToEngineFrame(
                Access::hostFrame(processor), host_rate)),
            rejects);
    }
}

enum class RtLoad {
    Idle,
    OneVoice,
    Poly,
    Release,
};

const char* rtLoadName(RtLoad load) {
    switch (load) {
    case RtLoad::Idle:
        return "idle";
    case RtLoad::OneVoice:
        return "1voice";
    case RtLoad::Poly:
        return "poly";
    case RtLoad::Release:
        return "release";
    }
    return "unknown";
}

void reportRealtimeDeadlineCost(
    double rate,
    int block,
    RtLoad load) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);

    constexpr int kWarmup = 16;
    constexpr int kMeasure = 64;
    processSilence(processor, kWarmup * block, block);

    if (load == RtLoad::OneVoice || load == RtLoad::Release) {
        juce::AudioBuffer<float> on_buf(2, block);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(on_buf, on);
        processSilence(processor, 8 * block, block);
    } else if (load == RtLoad::Poly) {
        juce::AudioBuffer<float> on_buf(2, block);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        on.addEvent(
            juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)),
            1);
        on.addEvent(
            juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)),
            2);
        processor.processBlock(on_buf, on);
        processSilence(processor, 8 * block, block);
    }
    if (load == RtLoad::Release) {
        juce::AudioBuffer<float> off_buf(2, block);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(off_buf, off);
    }

    double process_sum_ms = 0.0;
    double process_max_ms = 0.0;
    double src_sum_ms = 0.0;
    double fill_sum_ms = 0.0;
    double render_sum_ms = 0.0;
    double copy_sum_ms = 0.0;
    std::uint32_t cb_sum = 0;
    std::uint32_t render_sum = 0;
    for (int i = 0; i < kMeasure; ++i) {
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        const auto started = std::chrono::steady_clock::now();
        processor.processBlock(buffer, midi);
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        process_sum_ms += elapsed_ms;
        process_max_ms = std::max(process_max_ms, elapsed_ms);
        src_sum_ms += static_cast<double>(Access::srcProcessNs(processor))
            / 1.0e6;
        fill_sum_ms += static_cast<double>(Access::srcFillNs(processor))
            / 1.0e6;
        render_sum_ms += static_cast<double>(Access::srcRenderNs(processor))
            / 1.0e6;
        copy_sum_ms += static_cast<double>(Access::copyNs(processor))
            / 1.0e6;
        cb_sum += Access::srcCallbacks(processor);
        render_sum += Access::renderAudioCalls(processor);
    }

    const double deadline_ms =
        1.0e3 * static_cast<double>(block) / rate;
    const double process_mean = process_sum_ms / kMeasure;
    const double src_mean = src_sum_ms / kMeasure;
    const double fill_mean = fill_sum_ms / kMeasure;
    const double render_mean = render_sum_ms / kMeasure;
    const double sinc_mean = std::max(0.0, src_mean - fill_mean);
    const double fill_overhead = std::max(0.0, fill_mean - render_mean);
    const double copy_mean = copy_sum_ms / kMeasure;
#ifdef NDEBUG
    const char* config = "Release";
#else
    const char* config = "Debug";
#endif
    std::fprintf(
        stderr,
        "RT cost config=%s rate=%.1f block=%d load=%s path=%s "
        "deadline_ms=%.3f process_mean_ms=%.3f process_max_ms=%.3f "
        "ratio_mean=%.3f ratio_max=%.3f src_ms=%.3f fill_ms=%.3f "
        "render_ms=%.3f copy_ms=%.3f sinc_ms=%.3f fill_overhead_ms=%.3f "
        "cb_mean=%.1f render_calls_mean=%.1f\n",
        config,
        rate,
        block,
        rtLoadName(load),
        Access::usesSrc(processor) ? "src-batch" : "direct-48k",
        deadline_ms,
        process_mean,
        process_max_ms,
        process_mean / deadline_ms,
        process_max_ms / deadline_ms,
        src_mean,
        fill_mean,
        render_mean,
        copy_mean,
        sinc_mean,
        fill_overhead,
        static_cast<double>(cb_sum) / kMeasure,
        static_cast<double>(render_sum) / kMeasure);
}

void testRealtimeDeadlineCost() {
    constexpr std::array blocks{64, 128, 256, 512, 1024};
    constexpr std::array loads{
        RtLoad::Idle,
        RtLoad::OneVoice,
        RtLoad::Poly,
        RtLoad::Release,
    };
    for (const auto block : blocks) {
        for (const auto load : loads) {
            reportRealtimeDeadlineCost(48'000.0, block, load);
        }
    }
    reportRealtimeDeadlineCost(44'100.0, 512, RtLoad::Idle);
    reportRealtimeDeadlineCost(44'100.0, 512, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(44'100.0, 256, RtLoad::Idle);
    reportRealtimeDeadlineCost(44'100.0, 256, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(96'000.0, 512, RtLoad::Idle);
    reportRealtimeDeadlineCost(96'000.0, 512, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(96'000.0, 256, RtLoad::Idle);
    reportRealtimeDeadlineCost(96'000.0, 256, RtLoad::OneVoice);
}

void testInvalidSampleRateIsSilentAndDoesNotAdvanceEngine() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(0.0, 512);
    require(Access::frame(processor) == 0, "prepare at 0 Hz must not run");
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "0 Hz output was not silent");
    require(
        Access::frame(processor) == 0,
        "invalid rate must not advance engine timeline");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "invalid rate must not schedule delayed layers");

    processor.prepareToPlay(
        std::numeric_limits<double>::quiet_NaN(), 512);
    juce::AudioBuffer<float> nan_buf(2, 512);
    juce::MidiBuffer nan_midi;
    nan_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(nan_buf, nan_midi);
    require(channelPeak(nan_buf, 0, 512) < 0.001, "NaN rate must stay silent");
    require(Access::frame(processor) == 0, "NaN rate must not advance engine");
}

double playPeakAtRate(double rate, int block) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    double peak = channelPeak(buffer, 0, block);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, block);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        peak = channelPeak(more, 0, block);
    }
    return peak;
}

void testNon48kHzProducesAudio() {
    require(playPeakAtRate(44'100.0, 512) > 0.001, "44.1kHz produced silence");
    require(playPeakAtRate(48'000.0, 512) > 0.001, "48kHz produced silence");
    require(playPeakAtRate(88'200.0, 512) > 0.001, "88.2kHz produced silence");
    require(playPeakAtRate(96'000.0, 512) > 0.001, "96kHz produced silence");
}

void test48kHzBypassesSrcAndHasZeroLatency() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    require(!Access::usesSrc(processor), "48kHz must stay on the direct path");
    require(processor.getLatencySamples() == 0, "48kHz latency must be 0");
    require(Access::hostRate(processor) == 48'000, "48kHz host rate");

    processor.prepareToPlay(44'100.0, 512);
    require(processor.getLatencySamples() == 7, "44.1kHz latency is 7");
    processor.prepareToPlay(88'200.0, 512);
    require(processor.getLatencySamples() == 15, "88.2kHz latency is 15");
    processor.prepareToPlay(96'000.0, 512);
    require(processor.getLatencySamples() == 16, "96kHz latency is 16");
}

void testHostEngineMapping() {
    require(
        hostFrameToEngineFrame(0, 44'100) == 0, "host 0 -> engine 0");
    require(
        hostFrameToEngineFrame(441, 44'100) == 480,
        "10 ms at 44.1kHz is 480 engine frames");
    require(
        hostFrameToEngineFrame(512, 96'000) == 256,
        "512 host frames at 96kHz are 256 engine frames");
    require(
        hostFrameToEngineFrame(8820, 44'100) == 9'600,
        "200 ms at 44.1kHz is 9600 engine frames");
    require(
        hostFrameToEngineFrame(2'646'000, 44'100) == 2'880'000,
        "60 s at 44.1kHz maps without drift");
    require(
        hostFrameToEngineFrame(128, 48'000) == 128,
        "48kHz mapping is identity");
}

void test44100MidiOffsetMapsToEngine480() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "smoke program has PSG");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);

    juce::AudioBuffer<float> first(2, 440);
    juce::MidiBuffer empty;
    processor.processBlock(first, empty);
    require(
        Access::noteOnCount(processor) == 0,
        "no MIDI yet, so no NoteOn");
    require(
        Access::frame(processor) < 480,
        "440 host samples must not reach engine frame 480");

    juce::AudioBuffer<float> second(2, 72);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        1);
    processor.processBlock(second, midi);
    require(
        Access::logHas(processor, 480, track),
        "441 host samples at 44.1kHz must NoteOn at engine 480");
    require(
        !Access::logHas(processor, 479, track),
        "must not apply the note before engine frame 480");
}

void testLayerDelayIs9600EngineFramesAtEveryHostRate() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        loadDelayedThreeLayerProgram(processor);
        processor.prepareToPlay(rate, 512);
        const auto& plan = Access::plan(processor);
        const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        require(
            delayMillisecondsToEngineFrames(scc->start_delay_ms) == 9'600,
            "200 ms is 9600 engine frames");
        const auto scc_track = plan.physicalTrack(*scc, 0);

        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        processUntilEngine(processor, 9'600);
        require(
            Access::logHas(processor, 9'600, scc_track),
            "delayed SCC must fire at engine 9600 at every host rate");
    }
}

void testCancellationAt44100() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "note off before due cancels pending at 44.1kHz");
    processUntilEngine(processor, 9'664);
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled delayed layer must never NoteOn at 44.1kHz");
}

void testSameFrameCancelAt44100() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);

    juce::AudioBuffer<float> buffer(2, 9'000);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 8'820);
    processor.processBlock(buffer, midi);
    require(
        !Access::logHasTrack(processor, scc_track),
        "same-frame NoteOff at host 8820 must prevent SCC at engine 9600");
}

void testVoiceStealAndAllNotesOffAt44100() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> steal_buf(2, 512);
    juce::MidiBuffer steal;
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 1);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 2);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 63, static_cast<juce::uint8>(100)), 3);
    processor.processBlock(steal_buf, steal);
    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "stolen voice still has delayed SCC");
    require(event->midi_note == 63, "stolen voice carries the new note");
    const auto due_host3 = hostFrameToEngineFrame(3, 44'100) + scc_delay;
    require(event->due_frame == due_host3, "due uses mapped steal sample");

    MgstcAudioProcessor off_proc;
    loadDelayedThreeLayerProgram(off_proc);
    off_proc.prepareToPlay(44'100.0, 512);
    const auto* scc_off = layerOf(
        Access::plan(off_proc), mgstc::engine::TimbreSource::Scc);
    require(scc_off != nullptr, "SCC layer");
    const auto scc_off_track = Access::plan(off_proc).physicalTrack(*scc_off, 0);
    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    off.addEvent(juce::MidiMessage::allNotesOff(1), 64);
    off_proc.processBlock(off_buf, off);
    require(
        Access::scheduler(off_proc).pendingCount() == 0,
        "All Notes Off cancels pending at 44.1kHz");
    processUntilEngine(off_proc, 10'000);
    require(
        !Access::logHasTrack(off_proc, scc_off_track),
        "All Notes Off must not let delayed layers start later at 44.1kHz");
}

void feedMidiTimeline(
    MgstcAudioProcessor& processor,
    int block,
    const juce::MidiMessage& message,
    int host_offset,
    int total_host_frames) {
    int pos = 0;
    while (pos < total_host_frames) {
        const int n = std::min(block, total_host_frames - pos);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        if (host_offset >= pos && host_offset < pos + n) {
            midi.addEvent(message, host_offset - pos);
        }
        processor.processBlock(buffer, midi);
        pos += n;
    }
}

void testBlockSizeInvarianceAt44100() {
    const auto message =
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100));
    MgstcAudioProcessor small_blocks;
    MgstcAudioProcessor large_blocks;
    small_blocks.prepareToPlay(44'100.0, 64);
    large_blocks.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(small_blocks), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(small_blocks).physicalTrack(*psg, 0);

    feedMidiTimeline(small_blocks, 64, message, 441, 1'024);
    feedMidiTimeline(large_blocks, 512, message, 441, 1'024);
    require(
        Access::logHas(small_blocks, 480, track)
            && Access::logHas(large_blocks, 480, track),
        "block size must not change mapped MIDI engine time");
}

void testLongRunDriftAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    constexpr int kSeconds = 60;
    constexpr int kHost = 44'100 * kSeconds;
    int remaining = kHost;
    while (remaining > 0) {
        const int n = std::min(remaining, 512);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        remaining -= n;
    }
    require(Access::hostFrame(processor) == static_cast<std::uint64_t>(kHost),
        "host timeline must advance by 60 s");
    const auto mapped = hostFrameToEngineFrame(
        static_cast<std::uint64_t>(kHost), 44'100);
    const auto engine = Access::frame(processor);
    const auto delta = engine > mapped ? engine - mapped : mapped - engine;
    require(delta <= 1, "60 s at 44.1kHz must not accumulate timeline drift");
    require(
        engine <= mapped,
        "engine must not lead ceil host mapping after 60 s");

    juce::AudioBuffer<float> note_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    const auto started = std::chrono::steady_clock::now();
    processor.processBlock(note_buf, on);
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    std::fprintf(
        stderr,
        "Cubase first-note after 60s 44100 elapsed_ms=%.3f %s\n",
        elapsed_ms,
        srcDiag(processor).c_str());
    require(elapsed_ms < 250.0, "first Note On after 60 s must return");
    require(
        Access::srcWatchdogTrips(processor) == 0,
        "first Note On after 60 s must not spin in fillSrcSource");
}

void testRateChangeResetsSrcTimelineAndLatency() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(44'100.0, 512);
    require(Access::usesSrc(processor), "44.1kHz uses SRC");
    require(processor.getLatencySamples() == 7, "44.1kHz SRC latency is 7");
    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    require(Access::scheduler(processor).pendingCount() > 0, "had pending");
    require(Access::frame(processor) > 0, "44.1kHz advanced engine");

    processor.prepareToPlay(96'000.0, 512);
    require(Access::usesSrc(processor), "96kHz uses SRC");
    require(processor.getLatencySamples() == 16, "96kHz SRC latency is 16");
    require(Access::frame(processor) == 0, "rate change resets engine");
    require(Access::hostFrame(processor) == 0, "rate change resets host");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "rate change drops pending delay");
    require(Access::noteOnCount(processor) == 0, "rate change drops log");

    juce::AudioBuffer<float> silent(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(silent, empty);
    require(
        channelPeak(silent, 0, 512) < 0.001,
        "no stale audio after 96kHz prepare");

    processor.prepareToPlay(48'000.0, 512);
    require(!Access::usesSrc(processor), "48kHz returns to direct path");
    require(processor.getLatencySamples() == 0, "48kHz latency returns to 0");
    juce::AudioBuffer<float> live(2, 2'048);
    juce::MidiBuffer live_on;
    live_on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(live, live_on);
    double peak = channelPeak(live, 0, 2'048);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 2'048);
        juce::MidiBuffer rest;
        processor.processBlock(more, rest);
        peak = channelPeak(more, 0, 2'048);
    }
    require(peak > 0.001, "returning to 48kHz should play");
}

void testTwoProcessorsDoNotShareSrcState() {
    MgstcAudioProcessor a;
    MgstcAudioProcessor b;
    loadDelayedThreeLayerProgram(a);
    a.prepareToPlay(44'100.0, 512);
    b.prepareToPlay(44'100.0, 512);

    juce::AudioBuffer<float> buffer_a(2, 2'048);
    juce::AudioBuffer<float> buffer_b(2, 2'048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    juce::MidiBuffer empty;
    a.processBlock(buffer_a, on);
    b.processBlock(buffer_b, empty);

    double peak_a = channelPeak(buffer_a, 0, 2'048);
    if (peak_a <= 0.001) {
        juce::AudioBuffer<float> more(2, 2'048);
        juce::MidiBuffer rest;
        a.processBlock(more, rest);
        peak_a = channelPeak(more, 0, 2'048);
    }
    require(peak_a > 0.001, "processor A should sound at 44.1kHz");
    require(
        channelPeak(buffer_b, 0, 2'048) < 0.001,
        "processor B should stay silent");
    require(
        Access::scheduler(a).pendingCount() > 0,
        "processor A should own delayed slots");
    require(
        Access::scheduler(b).pendingCount() == 0,
        "processor B scheduler must stay empty");
}

double estimateHz(
    const juce::AudioBuffer<float>& buffer,
    int start,
    int count,
    double sample_rate) {
    const auto* samples = buffer.getReadPointer(0);
    const int end = std::min(start + count, buffer.getNumSamples());
    int crossings = 0;
    for (int i = start + 1; i < end; ++i) {
        if (samples[i - 1] <= 0.0F && samples[i] > 0.0F) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(end - start) / sample_rate;
    require(seconds > 0.0 && crossings > 4, "not enough pitch cycles");
    return static_cast<double>(crossings) / seconds;
}

double pitchAtRate(double rate) {
    MgstcAudioProcessor processor;
    const int frames = std::max(8'192, static_cast<int>(rate * 0.18));
    processor.prepareToPlay(rate, frames);
    juce::AudioBuffer<float> buffer(2, frames);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    const int start = static_cast<int>(rate * 0.04);
    const int count = static_cast<int>(rate * 0.10);
    return estimateHz(buffer, start, count, rate);
}

void testPitchIndependentOfHostRate() {
    const auto hz48 = pitchAtRate(48'000.0);
    const auto hz441 = pitchAtRate(44'100.0);
    const auto hz882 = pitchAtRate(88'200.0);
    const auto hz96 = pitchAtRate(96'000.0);
    require(std::abs(hz441 - hz48) / hz48 < 0.03, "44.1kHz pitch drifted");
    require(std::abs(hz882 - hz48) / hz48 < 0.03, "88.2kHz pitch drifted");
    require(std::abs(hz96 - hz48) / hz48 < 0.03, "96kHz pitch drifted");
}

void testDurationIndependentOfHostRate() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto host_rate = Access::hostRate(processor);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        processUntilEngine(processor, 9'600);
        const auto host_seconds = static_cast<double>(Access::hostFrame(processor))
            / static_cast<double>(host_rate);
        require(
            std::abs(host_seconds - 0.2) < 0.02,
            "200 ms delay must stay 200 ms in host time");
        require(
            Access::frame(processor) >= 9'600,
            "engine must reach the 200 ms due frame");
    }
}

void testAllSoundOffAt44100() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(44'100.0, 512);
    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Sound Off cancels pending at 44.1kHz");
    processUntilEngine(processor, 10'000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Sound Off must not let delayed layers start later at 44.1kHz");
}

void testTwoProcessorsDoNotShareVoiceState() {
    MgstcAudioProcessor a;
    MgstcAudioProcessor b;
    loadDelayedThreeLayerProgram(a);
    prime(a, 512);
    prime(b, 512);

    juce::AudioBuffer<float> buffer_a(2, 2048);
    juce::AudioBuffer<float> buffer_b(2, 2048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    juce::MidiBuffer empty;
    a.processBlock(buffer_a, on);
    b.processBlock(buffer_b, empty);

    double peak_a = channelPeak(buffer_a, 0, 2048);
    if (peak_a <= 0.001) {
        juce::AudioBuffer<float> more(2, 2048);
        juce::MidiBuffer rest;
        a.processBlock(more, rest);
        peak_a = channelPeak(more, 0, 2048);
    }
    require(peak_a > 0.001, "processor A should sound");
    require(
        channelPeak(buffer_b, 0, 2048) < 0.001,
        "processor B should stay silent");
    require(
        Access::scheduler(a).pendingCount() > 0,
        "processor A should own delayed slots");
    require(
        Access::scheduler(b).pendingCount() == 0,
        "processor B scheduler must stay empty");
}

void testZeroDelayFiresAtMidiFrame() {
    MgstcAudioProcessor processor;
    prime(processor, 512);
    const auto* psg = layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "smoke program has PSG");
    require(
        delayMillisecondsToEngineFrames(psg->start_delay_ms) == 0,
        "PSG layer must stay zero-delay");

    const auto block_start = Access::frame(processor);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        128);
    processor.processBlock(buffer, midi);

    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    require(
        Access::logHas(processor, block_start + 128, track),
        "zero-delay layer must NoteOn at the MIDI sample");
    require(
        channelPeak(buffer, 0, 128) < 0.001,
        "zero-delay audio must not start before MIDI");
}

void testDelayedLayersFireAtExactDueAndAcrossBlocks() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    const auto* opll = layerOf(plan, mgstc::engine::TimbreSource::Opll);
    require(scc != nullptr && opll != nullptr, "smoke has SCC and OPLL");
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);
    const auto opll_delay =
        delayMillisecondsToEngineFrames(opll->start_delay_ms);
    require(scc_delay == 9600, "SCC smoke delay is 200 ms");
    require(opll_delay == 19200, "OPLL smoke delay is 400 ms");

    juce::AudioBuffer<float> first(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        400);
    processor.processBlock(first, on);

    const auto note_frame = static_cast<std::uint64_t>(400);
    const auto scc_due = note_frame + scc_delay;
    const auto opll_due = note_frame + opll_delay;
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto opll_track = plan.physicalTrack(*opll, 0);

    require(Access::scheduler(processor).isPending(scc_track), "SCC pending");
    require(Access::scheduler(processor).isPending(opll_track), "OPLL pending");
    require(
        Access::scheduler(processor).event(scc_track)->due_frame == scc_due,
        "SCC due is absolute note+delay");
    require(
        !Access::logHasTrack(processor, scc_track),
        "SCC must not fire in the first block");

    processUntilFrame(processor, scc_due);
    require(Access::frame(processor) == scc_due, "landed on SCC due");
    require(
        Access::logHas(processor, scc_due, scc_track),
        "SCC fires at exact due frame");
    require(
        !Access::logHasTrack(processor, opll_track),
        "OPLL must still be waiting");

    processUntilFrame(processor, opll_due);
    require(
        Access::logHas(processor, opll_due, opll_track),
        "OPLL fires at exact due frame");
    require(Access::scheduler(processor).pendingCount() == 0, "all due consumed");
}

void testNoteOffBeforeDueCancelsDelayedLayers() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_due =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    processor.processBlock(buffer, midi);

    require(
        Access::scheduler(processor).pendingCount() == 0,
        "note off before due cancels pending");
    require(
        Access::isInactive(processor, scc_track),
        "cancelled layer is inactive");

    processUntilFrame(processor, scc_due + 64);
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled delayed layer must never NoteOn");
}

void testNoteOffAfterDueLeavesReleasePath() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_due =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    processUntilFrame(processor, scc_due);
    require(Access::logHas(processor, scc_due, scc_track), "delayed layer sounded");
    require(
        Access::isActive(processor, scc_track),
        "sounded layer is Active");

    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(off_buf, off);
    require(
        Access::isInactive(processor, scc_track),
        "note off after due clears Active");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "no pending after sounding note off");
}

void testSameFrameNoteOffCancelsDueEvent() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const int due = static_cast<int>(
        delayMillisecondsToEngineFrames(scc->start_delay_ms));

    juce::AudioBuffer<float> buffer(2, due + 64);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), due);
    processor.processBlock(buffer, midi);

    require(
        !Access::logHasTrack(processor, scc_track),
        "same-frame NoteOff must prevent delayed NoteOn");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "same-frame cancel leaves no pending");
}

void testVoiceStealCancelsOldPending() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 1);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 2);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 63, static_cast<juce::uint8>(100)), 3);
    processor.processBlock(buffer, midi);

    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "voice 0 still has a delayed SCC");
    require(event->midi_note == 63, "stolen voice carries the new note");
    require(
        event->due_frame == 3 + scc_delay,
        "new reservation uses the steal sample");

    processUntilFrame(processor, scc_delay);
    require(
        !Access::logHas(processor, scc_delay, scc_track),
        "stolen note's original due must not fire");
    processUntilFrame(processor, 3 + scc_delay);
    require(
        Access::logHas(processor, 3 + scc_delay, scc_track),
        "new note's delayed layer fires at its due");
}

void testAllNotesOffCancelsPending() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allNotesOff(1), 64);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Notes Off cancels pending");

    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    processEmpty(processor, 10000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Notes Off must not let delayed layers start later");
}

void testAllSoundOffCancelsPending() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Sound Off cancels pending");

    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    processEmpty(processor, 10000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Sound Off must not let delayed layers start later");
}

void testLifecycleResetDropsPending() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    require(Access::scheduler(processor).pendingCount() > 0, "had pending");

    processor.releaseResources();
    require(Access::scheduler(processor).pendingCount() == 0, "release clears");
    require(Access::frame(processor) == 0, "release resets timeline");

    processor.prepareToPlay(48'000.0, 512);
    require(Access::scheduler(processor).pendingCount() == 0, "prepare clears");
    processEmpty(processor, 20000);
    require(
        Access::noteOnCount(processor) == 0,
        "old reservations must not fire after prepareToPlay");
}

void testProgramResetDropsPending() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    processor.setCurrentProgram(0);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "program reset cancels pending");
    processEmpty(processor, 20000);
    require(
        Access::noteOnCount(processor) == 0,
        "old reservations must not fire after program reset");
}

std::uint64_t fnvMix(std::uint64_t hash, float sample) {
    const auto bits = static_cast<std::uint32_t>(
        std::bit_cast<std::uint32_t>(sample));
    hash ^= bits;
    hash *= 1099511628211ULL;
    return hash;
}

template <typename Render>
double timeFrames(int frames, Render&& render, std::uint64_t* hash) {
    std::uint64_t mixed = 14695981039346656037ULL;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        mixed = fnvMix(mixed, render());
    }
    if (hash != nullptr) {
        *hash = mixed;
    }
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

// Pre-ring SincRateConv: linear history shift, std::abs / std::min lookup.
// The ring buffer must match this sample for sample, including channel taps.
class LinearShiftSinc {
public:
    static constexpr std::size_t kTapCount = 16;
    static constexpr std::size_t kTableReso = 256;

    LinearShiftSinc(double input_hz, double output_hz)
        : input_hz_(input_hz)
        , output_hz_(output_hz)
        , ratio_(input_hz / output_hz) {
        constexpr double half_taps = static_cast<double>(kTapCount) / 2.0;
        const bool downsample = output_hz < input_hz;
        for (std::size_t i = 0; i < sinc_table_.size(); ++i) {
            const double x =
                static_cast<double>(i) / static_cast<double>(kTableReso);
            const double kernel = downsample
                ? windowed(x / ratio_, half_taps) / ratio_
                : windowed(x, half_taps);
            sinc_table_[i] = static_cast<float>(kernel);
        }
    }

    template <typename Generate>
    float next(Generate&& generate, std::span<float> channels = {}) noexcept {
        if (!channels.empty() && !tracking_) {
            channel_history_ = {};
        }
        tracking_ = !channels.empty();
        while (input_hz_ > time_) {
            time_ += output_hz_;
            push(static_cast<float>(generate()));
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                auto& history = channel_history_[ch];
                for (std::size_t k = 1; k < kTapCount; ++k) {
                    history[k - 1] = history[k];
                }
                history.back() = channels[ch];
            }
        }
        time_ -= input_hz_;
        const float mixed = interpolate();
        constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
        std::array<float, kTapCount> coefficients{};
        if (!channels.empty()) {
            for (std::size_t k = 0; k < kTapCount; ++k) {
                coefficients[k] = lookup(static_cast<double>(k) - center - frac_);
            }
        }
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            double sum = 0.0;
            for (std::size_t k = 0; k < kTapCount; ++k) {
                sum += static_cast<double>(channel_history_[ch][k])
                    * static_cast<double>(coefficients[k]);
            }
            channels[ch] = static_cast<float>(sum);
        }
        return mixed;
    }

    void reset() noexcept {
        history_.fill(0.0F);
        channel_history_ = {};
        tracking_ = false;
        time_ = 0.0;
        frac_ = 0.0;
    }

private:
    static double windowed(double x, double half_taps) noexcept {
        constexpr double pi = std::numbers::pi_v<double>;
        const double window = 0.42
            - 0.5 * std::cos(2.0 * pi * (0.5 + 0.5 * x / half_taps))
            + 0.08 * std::cos(4.0 * pi * (0.5 + 0.5 * x / half_taps));
        if (x == 0.0) {
            return window;
        }
        return window * (std::sin(pi * x) / (pi * x));
    }

    void push(float sample) noexcept {
        for (std::size_t i = 0; i + 1 < history_.size(); ++i) {
            history_[i] = history_[i + 1];
        }
        history_.back() = sample;
    }

    float lookup(double x) const noexcept {
        const auto index = static_cast<int>(
            std::abs(x) * static_cast<double>(kTableReso));
        const auto clamped = std::min(
            static_cast<int>(sinc_table_.size()) - 1,
            index);
        return sinc_table_[static_cast<std::size_t>(clamped)];
    }

    float interpolate() noexcept {
        frac_ += ratio_;
        frac_ -= std::floor(frac_);
        double sum = 0.0;
        constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
        for (std::size_t k = 0; k < history_.size(); ++k) {
            const double x = static_cast<double>(k) - center - frac_;
            sum += static_cast<double>(history_[k])
                * static_cast<double>(lookup(x));
        }
        return static_cast<float>(sum);
    }

    double input_hz_{};
    double output_hz_{};
    double ratio_{};
    double time_{};
    double frac_{};
    std::array<float, kTapCount> history_{};
    std::array<std::array<float, kTapCount>, 14> channel_history_{};
    bool tracking_{};
    std::array<float, kTableReso * kTapCount / 2> sinc_table_{};
};

void requireSameBits(float left, float right, const char* label) {
    require(
        std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right),
        label);
}

double sincSource(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(static_cast<std::int32_t>(state >> 16) - 32768)
        / 32768.0;
}

void compareSincSpans(
    double input_hz,
    int frames,
    int channel_count,
    bool reset_midway) {
    mgstc::engine::SincRateConv ring(input_hz, 48'000.0);
    LinearShiftSinc linear(input_hz, 48'000.0);
    std::uint32_t ring_state = 1;
    std::uint32_t linear_state = 1;
    int ring_calls = 0;
    int linear_calls = 0;
    std::array<float, 3> ring_channels{};
    std::array<float, 3> linear_channels{};
    for (int frame = 0; frame < frames; ++frame) {
        if (reset_midway && frame == frames / 2) {
            ring.reset();
            linear.reset();
        }
        const int active = (frame % 5 == 0) ? 0 : channel_count;
        for (int ch = 0; ch < active; ++ch) {
            const float sample = static_cast<float>(frame * 3 + ch) * 0.001F;
            ring_channels[static_cast<std::size_t>(ch)] = sample;
            linear_channels[static_cast<std::size_t>(ch)] = sample;
        }
        const auto ring_span = std::span<float>(
            ring_channels.data(), static_cast<std::size_t>(active));
        const auto linear_span = std::span<float>(
            linear_channels.data(), static_cast<std::size_t>(active));
        const float ring_mixed = ring.next(
            [&] {
                ++ring_calls;
                return sincSource(ring_state);
            },
            ring_span);
        const float linear_mixed = linear.next(
            [&] {
                ++linear_calls;
                return sincSource(linear_state);
            },
            linear_span);
        requireSameBits(ring_mixed, linear_mixed, "sinc ring mono");
        for (int ch = 0; ch < active; ++ch) {
            requireSameBits(
                ring_channels[static_cast<std::size_t>(ch)],
                linear_channels[static_cast<std::size_t>(ch)],
                "sinc ring channel");
        }
    }
    require(ring_calls == linear_calls, "sinc generate count");
}

void testSincRingMatchesLinearShift() {
    constexpr std::array rates{3'579'545.0 / 16.0, 3'579'545.0 / 72.0};
    for (const double rate : rates) {
        compareSincSpans(rate, 4'800, 0, false);
        compareSincSpans(rate, 4'800, 3, false);
        compareSincSpans(rate, 800, 3, true);
    }
}

void profileEngineIdleBreakdown() {
    constexpr int kFrames = 48'000;
    mgstc::engine::ChipRack rack;
    require(rack.valid(), "ChipRack must construct");
    std::uint64_t rack_hash = 0;
    const auto chip_ms = timeFrames(
        kFrames,
        [&] { return rack.renderSample().psg; },
        &rack_hash);

    std::array<float, 1024> pcm{};
    auto render_core = [&](bool scope) {
        mgstc::engine::EngineCore core;
        core.setOpllScopeEnabled(scope);
        const auto started = std::chrono::steady_clock::now();
        int remain = kFrames;
        while (remain > 0) {
            const int n = std::min(remain, 512);
            static_cast<void>(core.render(
                std::span<float>(pcm.data(), static_cast<std::size_t>(n) * 2)));
            remain -= n;
        }
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    const auto core_scope_ms = render_core(true);
    const auto core_noscope_ms = render_core(false);

    mgstc::engine::DcBlocker blocker(48'000.0F, 3.4F);
    const auto mix_ms = timeFrames(
        kFrames,
        [&] {
            return std::clamp(blocker.process(0.0F), -1.0F, 1.0F);
        },
        nullptr);

    std::fprintf(
        stderr,
        "Idle breakdown 48k/1s ChipRack_ms=%.3f EngineCore_scope_ms=%.3f "
        "EngineCore_noscope_ms=%.3f mix_dc_ms=%.3f rack_hash=%llx\n",
        chip_ms,
        core_scope_ms,
        core_noscope_ms,
        mix_ms,
        static_cast<unsigned long long>(rack_hash));

#ifdef NDEBUG
    using mgstc::engine::ChipId;
    using mgstc::engine::WriteReason;
    constexpr double kPsgNative = 3'579'545.0 / 16.0;
    constexpr double kOpllNative = 3'579'545.0 / 72.0;
    auto time_sinc = [](double input_hz) {
        mgstc::engine::SincRateConv conv(input_hz, 48'000.0);
        std::uint32_t state = 1;
        return timeFrames(
            kFrames,
            [&] {
                return conv.next([&] {
                    state = state * 1664525U + 1013904223U;
                    return static_cast<float>(
                               static_cast<int>(state >> 16) - 32768)
                        / 32768.0F;
                });
            },
            nullptr);
    };

    mgstc::engine::Ym2149Adapter psg;
    mgstc::engine::SccAdapter scc;
    mgstc::engine::Ym2413Adapter opll;
    require(psg.valid() && scc.valid() && opll.valid(), "adapters construct");
    std::uint64_t psg_idle_hash = 0;
    std::uint64_t scc_idle_hash = 0;
    std::uint64_t opll_idle_hash = 0;
    const auto psg_idle = timeFrames(
        kFrames, [&] { return psg.renderSample(); }, &psg_idle_hash);
    const auto scc_idle = timeFrames(
        kFrames, [&] { return scc.renderSample(); }, &scc_idle_hash);
    const auto opll_idle = timeFrames(
        kFrames, [&] { return opll.renderSample(); }, &opll_idle_hash);

    mgstc::engine::Ym2149Adapter psg_tone;
    require(psg_tone.write(0, 0xAB), "psg period lo");
    require(psg_tone.write(1, 0x01), "psg period hi");
    require(psg_tone.write(7, 0xBE), "psg mixer");
    require(psg_tone.write(8, 0x0F), "psg volume");
    std::uint64_t psg_tone_hash = 0;
    const auto psg_tone_ms = timeFrames(
        kFrames, [&] { return psg_tone.renderSample(); }, &psg_tone_hash);

    mgstc::engine::SccAdapter scc_tone;
    for (std::uint8_t index = 0; index < 32; ++index) {
        require(
            scc_tone.write(
                0,
                index,
                static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80)),
            "scc wave");
    }
    require(scc_tone.write(1, 0, 0xAB), "scc freq lo");
    require(scc_tone.write(1, 1, 0x01), "scc freq hi");
    require(scc_tone.write(2, 0, 0x0F), "scc volume");
    require(scc_tone.write(3, 0, 0x01), "scc key");
    std::uint64_t scc_tone_hash = 0;
    const auto scc_tone_ms = timeFrames(
        kFrames, [&] { return scc_tone.renderSample(); }, &scc_tone_hash);

    mgstc::engine::Ym2413Adapter opll_tone;
    const std::array<std::uint8_t, 8> original_patch{
        0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00,
    };
    for (std::uint8_t address = 0; address < original_patch.size(); ++address) {
        require(
            opll_tone.write(address, original_patch[address]),
            "opll patch");
    }
    require(opll_tone.write(0x30, 0x00), "opll inst");
    require(opll_tone.write(0x20, 0x16), "opll key");
    require(opll_tone.write(0x10, 0xAC), "opll freq");
    std::uint64_t opll_tone_hash = 0;
    const auto opll_tone_ms = timeFrames(
        kFrames, [&] { return opll_tone.renderSample(); }, &opll_tone_hash);

    mgstc::engine::ChipRack sounding;
    const std::array<mgstc::engine::RegisterWrite, 4> psg_setup{{
        {0, 0, ChipId::Psg, 0, 0, 0xAB, 0, WriteReason::Frequency},
        {0, 1, ChipId::Psg, 0, 1, 0x01, 0, WriteReason::Frequency},
        {0, 2, ChipId::Psg, 0, 7, 0xBE, 0, WriteReason::Patch},
        {0, 3, ChipId::Psg, 0, 8, 0x0F, 0, WriteReason::Volume},
    }};
    require(sounding.apply(psg_setup), "sounding psg");
    std::uint32_t sequence = 4;
    for (std::uint8_t index = 0; index < 32; ++index) {
        require(
            sounding.apply({
                0,
                sequence++,
                ChipId::Scc,
                0,
                index,
                static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80),
                3,
                WriteReason::Patch,
            }),
            "sounding scc wave");
    }
    const std::array<mgstc::engine::RegisterWrite, 7> rest_setup{{
        {0, sequence++, ChipId::Scc, 1, 0, 0xAB, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 1, 1, 0x01, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 2, 0, 0x0F, 3, WriteReason::Volume},
        {0, sequence++, ChipId::Scc, 3, 0, 0x01, 3, WriteReason::KeyOn},
        {0, sequence++, ChipId::Opll, 0, 0x30, 0x10, 8, WriteReason::Patch},
        {0, sequence++, ChipId::Opll, 0, 0x20, 0x16, 8, WriteReason::KeyOn},
        {0, sequence++, ChipId::Opll, 0, 0x10, 0xAC, 8, WriteReason::Frequency},
    }};
    require(sounding.apply(rest_setup), "sounding scc/opll");
    const auto rack_sound_ms = timeFrames(
        kFrames, [&] { return sounding.renderSample().psg; }, nullptr);
    const auto rack_capture_ms = timeFrames(
        kFrames,
        [&] { return sounding.renderSample(true).psg; },
        nullptr);
    mgstc::engine::ChipRack warm;
    require(warm.valid(), "warm ChipRack");
    const auto rack_warm_ms = timeFrames(
        kFrames, [&] { return warm.renderSample().psg; }, nullptr);

    std::fprintf(
        stderr,
        "Chip split 48k/1s PSG_idle_ms=%.3f PSG_tone_ms=%.3f "
        "SCC_idle_ms=%.3f SCC_tone_ms=%.3f OPLL_idle_ms=%.3f "
        "OPLL_tone_ms=%.3f sinc_psg_ratio_ms=%.3f sinc_opll_ratio_ms=%.3f "
        "rack_sound_ms=%.3f rack_capture_ms=%.3f rack_warm_ms=%.3f\n",
        psg_idle,
        psg_tone_ms,
        scc_idle,
        scc_tone_ms,
        opll_idle,
        opll_tone_ms,
        time_sinc(kPsgNative),
        time_sinc(kOpllNative),
        rack_sound_ms,
        rack_capture_ms,
        rack_warm_ms);
    auto hash_fresh = [](auto construct, int frames) {
        auto chip = construct();
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            frames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    const auto psg_10 = hash_fresh(
        [] { return mgstc::engine::Ym2149Adapter(); }, kFrames * 10);
    const auto opll_10 = hash_fresh(
        [] { return mgstc::engine::Ym2413Adapter(); }, kFrames * 10);
    const auto psg_60 = hash_fresh(
        [] { return mgstc::engine::Ym2149Adapter(); }, kFrames * 60);
    const auto opll_60 = hash_fresh(
        [] { return mgstc::engine::Ym2413Adapter(); }, kFrames * 60);
    auto hash_psg_after_idle = [](int idle) {
        mgstc::engine::Ym2149Adapter chip;
        for (int i = 0; i < idle; ++i) {
            static_cast<void>(chip.renderSample());
        }
        require(chip.write(0, 0xAB) && chip.write(1, 0x01)
                    && chip.write(7, 0xBE) && chip.write(8, 0x0F),
            "psg after idle");
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            kFrames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    auto hash_opll_after_idle = [](int idle) {
        mgstc::engine::Ym2413Adapter chip;
        for (int i = 0; i < idle; ++i) {
            static_cast<void>(chip.renderSample());
        }
        const std::array<std::uint8_t, 8> patch{
            0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00,
        };
        for (std::uint8_t address = 0; address < patch.size(); ++address) {
            require(chip.write(address, patch[address]), "opll after idle");
        }
        require(chip.write(0x30, 0x00) && chip.write(0x20, 0x16)
                    && chip.write(0x10, 0xAC),
            "opll key after idle");
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            kFrames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    const auto psg_note0 = hash_psg_after_idle(0);
    const auto psg_note1 = hash_psg_after_idle(kFrames);
    const auto psg_note10 = hash_psg_after_idle(kFrames * 10);
    const auto psg_note60 = hash_psg_after_idle(kFrames * 60);
    const auto opll_note0 = hash_opll_after_idle(0);
    const auto opll_note1 = hash_opll_after_idle(kFrames);
    const auto opll_note10 = hash_opll_after_idle(kFrames * 10);
    const auto opll_note60 = hash_opll_after_idle(kFrames * 60);
    std::fprintf(
        stderr,
        "PCM hash psg_idle_1s=%016llx scc_idle_1s=%016llx opll_idle_1s=%016llx "
        "psg_tone_1s=%016llx scc_tone_1s=%016llx opll_tone_1s=%016llx "
        "psg_idle_10s=%016llx opll_idle_10s=%016llx "
        "psg_idle_60s=%016llx opll_idle_60s=%016llx "
        "psg_note_after_0=%016llx psg_note_after_1s=%016llx "
        "psg_note_after_10s=%016llx psg_note_after_60s=%016llx "
        "opll_note_after_0=%016llx opll_note_after_1s=%016llx "
        "opll_note_after_10s=%016llx opll_note_after_60s=%016llx\n",
        static_cast<unsigned long long>(psg_idle_hash),
        static_cast<unsigned long long>(scc_idle_hash),
        static_cast<unsigned long long>(opll_idle_hash),
        static_cast<unsigned long long>(psg_tone_hash),
        static_cast<unsigned long long>(scc_tone_hash),
        static_cast<unsigned long long>(opll_tone_hash),
        static_cast<unsigned long long>(psg_10),
        static_cast<unsigned long long>(opll_10),
        static_cast<unsigned long long>(psg_60),
        static_cast<unsigned long long>(opll_60),
        static_cast<unsigned long long>(psg_note0),
        static_cast<unsigned long long>(psg_note1),
        static_cast<unsigned long long>(psg_note10),
        static_cast<unsigned long long>(psg_note60),
        static_cast<unsigned long long>(opll_note0),
        static_cast<unsigned long long>(opll_note1),
        static_cast<unsigned long long>(opll_note10),
        static_cast<unsigned long long>(opll_note60));
    require(psg_tone_hash == 0x87fb8b985f6f522bULL, "psg tone pcm");
    require(scc_tone_hash == 0xb5847d925273a925ULL, "scc tone pcm");
    require(opll_tone_hash == 0x19aafbc80013f191ULL, "opll tone pcm");
    require(psg_note0 == 0x87fb8b985f6f522bULL, "psg note after 0");
    require(psg_note1 == 0x37becf2600dcf147ULL, "psg note after 1s");
    require(psg_note10 == 0xa98dcd45456d8911ULL, "psg note after 10s");
    require(psg_note60 == 0x098def64738b3ff5ULL, "psg note after 60s");
    require(opll_note0 == 0x19aafbc80013f191ULL, "opll note after 0");
    require(opll_note1 == 0x2a9082c034cdd2d0ULL, "opll note after 1s");
    require(opll_note10 == 0x0b3d8c04f8a70453ULL, "opll note after 10s");
    require(opll_note60 == 0xebfa32d4a20a4aeaULL, "opll note after 60s");
#else
    std::fprintf(stderr, "Chip split skipped in Debug\n");
#endif
}

std::vector<float> captureNotePcmAfterSilence(int silence_frames) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    processSilence(processor, silence_frames, 512);
    juce::AudioBuffer<float> on_buf(2, 2048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    std::vector<float> pcm(
        static_cast<std::size_t>(on_buf.getNumSamples()));
    const auto* left = on_buf.getReadPointer(0);
    for (int i = 0; i < on_buf.getNumSamples(); ++i) {
        pcm[static_cast<std::size_t>(i)] = left[i];
    }
    return pcm;
}

double maxAbsDiff(
    const std::vector<float>& a,
    const std::vector<float>& b) {
    require(a.size() == b.size(), "PCM compare size");
    double peak = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        peak = std::max(
            peak,
            static_cast<double>(std::fabs(a[i] - b[i])));
    }
    return peak;
}

std::uint64_t hashPcm(const std::vector<float>& pcm) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float sample : pcm) {
        hash = fnvMix(hash, sample);
    }
    return hash;
}

void testIdleDurationChangesNextNotePcm() {
    const auto pcm0 = captureNotePcmAfterSilence(0);
    const auto pcm1s = captureNotePcmAfterSilence(48'000);
#ifdef NDEBUG
    const auto pcm10s = captureNotePcmAfterSilence(480'000);
    const auto pcm60s = captureNotePcmAfterSilence(2'880'000);
#else
    const auto pcm10s = pcm1s;
    const auto pcm60s = pcm1s;
#endif
    const auto d1 = maxAbsDiff(pcm0, pcm1s);
    const auto d10 = maxAbsDiff(pcm0, pcm10s);
    std::fprintf(
        stderr,
        "Idle PCM next-note maxdiff 0vs1s=%.6g 0vs10s=%.6g "
        "hash0=%016llx hash1s=%016llx hash10s=%016llx hash60s=%016llx\n",
        d1,
        d10,
        static_cast<unsigned long long>(hashPcm(pcm0)),
        static_cast<unsigned long long>(hashPcm(pcm1s)),
        static_cast<unsigned long long>(hashPcm(pcm10s)),
        static_cast<unsigned long long>(hashPcm(pcm60s)));
    // Chip clocks keep running while silent. Skipping them would not match
    // full render, so idle synthesis is not skipped.
#ifdef NDEBUG
    require(hashPcm(pcm0) == 0xdf5a02060aa6e92bULL, "next note after 0");
    require(hashPcm(pcm1s) == 0x2b87be7c9976c757ULL, "next note after 1s");
    require(hashPcm(pcm10s) == 0xe55be4bed683f0aeULL, "next note after 10s");
    require(hashPcm(pcm60s) == 0xf311345bfc9d6f71ULL, "next note after 60s");
#endif
}

void testNeverPlayedIsQuiescentAndReleaseStillSounds() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    processSilence(processor, 512, 512);
    require(
        Access::pluginQuiescent(processor),
        "never-played silence must be plugin-quiescent");
    require(
        !Access::hasRealtimeWork(processor),
        "never-played session has no software envelope work");

    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    require(
        !Access::pluginQuiescent(processor),
        "active/pending layers are not quiescent");

    processEmpty(processor, 20'000);
    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(off_buf, off);
    juce::AudioBuffer<float> tail(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(tail, empty);
    require(
        channelPeak(tail, 0, 512) > 0.0001,
        "release tail must keep rendering after Note Off");
}

void testBatchSourceRenderCallCount() {
    auto measure = [](double rate, int block, bool sounding) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, block);
        processSilence(processor, 8 * block, block);
        if (sounding) {
            juce::AudioBuffer<float> on_buf(2, block);
            juce::MidiBuffer on;
            on.addEvent(
                juce::MidiMessage::noteOn(
                    1, 60, static_cast<juce::uint8>(100)),
                block / 2);
            processor.processBlock(on_buf, on);
            const auto calls = Access::renderAudioCalls(processor);
            const auto cb = Access::srcCallbacks(processor);
            std::fprintf(
                stderr,
                "Batch calls rate=%.1f block=%d sounding=1 "
                "renderAudio=%u src_cb=%u fill_iters=%u\n",
                rate,
                block,
                calls,
                cb,
                Access::srcFillIters(processor));
            require(
                calls < static_cast<std::uint32_t>(block),
                "sounding block must not renderAudio per host sample");
            if (Access::usesSrc(processor)) {
                require(
                    cb <= 8,
                    "SRC must pull batched source, not one frame per callback");
            }
            return;
        }
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        const auto calls = Access::renderAudioCalls(processor);
        const auto cb = Access::srcCallbacks(processor);
        std::fprintf(
            stderr,
            "Batch calls rate=%.1f block=%d sounding=0 "
            "renderAudio=%u src_cb=%u fill_iters=%u\n",
            rate,
            block,
            calls,
            cb,
            Access::srcFillIters(processor));
        require(calls <= 4, "idle batch should be a handful of renderAudio calls");
        if (Access::usesSrc(processor)) {
            require(cb <= 4, "idle SRC callbacks should be buffer bounds, not 1-frame");
        }
    };
    measure(44'100.0, 512, false);
    measure(44'100.0, 512, true);
    measure(96'000.0, 512, false);
    measure(48'000.0, 512, false);
}

void testMidBlockNoteIsNotRoundedToBatchStart() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        256);
    processor.processBlock(buffer, midi);
    const auto expected = hostFrameToEngineFrame(256, 44'100);
    require(
        Access::logHas(processor, expected, track),
        "mid-block MIDI must keep its mapped engine frame");
    require(
        !Access::logHas(processor, 0, track),
        "batch source must not snap MIDI to the block start");
}

bool bufferAllFinite(const juce::AudioBuffer<float>& buffer) {
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        const auto* samples = buffer.getReadPointer(channel);
        const int count = buffer.getNumSamples();
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(samples[i])) {
                return false;
            }
        }
    }
    return true;
}

void testNon48kHzOutputIsFinite() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        for (int block = 0; block < 8; ++block) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            if (block == 0) {
                midi.addEvent(
                    juce::MidiMessage::noteOn(
                        1, 60, static_cast<juce::uint8>(100)),
                    0);
            } else if (block == 4) {
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            }
            processor.processBlock(buffer, midi);
            require(
                bufferAllFinite(buffer),
                "non-48kHz path produced NaN or Inf");
        }
    }
}

void testLatencyReportedOnlyFromPrepareAndRelease() {
    require(
        Access::diagnosticMode() == kVst3DiagOff,
        "normal processor tests must compile with diagnostic OFF");
    require(
        !MgstcAudioProcessor::kProcessBlockUsesScopedNoDenormals,
        "fact: processBlock has no ScopedNoDenormals");

    MgstcAudioProcessor processor;
    require(Access::latencyCalls(processor) == 0, "no latency before prepare");
    processor.prepareToPlay(44'100.0, 512);
    require(
        Access::prepareToPlayCount(processor) == 1, "one prepareToPlay");
    require(
        Access::latencyCalls(processor) == 1,
        "44.1kHz prepare reports the final SRC group delay once");
    require(
        Access::latencyChanged(processor) == 1,
        "first 44.1kHz prepare notifies once (0 -> 7)");
    require(
        Access::latencySame(processor) == 0,
        "first 44.1kHz prepare must not bounce through 0");
    require(processor.getLatencySamples() == 7, "44.1kHz latency is 7");

    const auto after_prepare = Access::latencyCalls(processor);
    for (int i = 0; i < 64; ++i) {
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        if (i == 0) {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1, 60, static_cast<juce::uint8>(100)),
                0);
        }
        processor.processBlock(buffer, midi);
        require(
            Access::lastNumSamples(processor) == 512,
            "host block size should stay 512 in this harness");
        require(
            Access::lastBufferChannels(processor) == 2,
            "host channel count should stay stereo in this harness");
        require(
            Access::processSampleRateHz(processor) == 44'100,
            "processBlock sample rate should stay 44100");
    }
    require(
        Access::processBlockCount(processor) == 64,
        "processBlock counter should track calls");
    require(
        Access::latencyCalls(processor) == after_prepare,
        "processBlock must not call setLatencySamples");
    require(
        Access::latencyFromProcess(processor) == 0,
        "Audio Thread must not notify latency");
    require(
        Access::sampleRateChangesInProcess(processor) == 0,
        "sample rate must not change per block");

    processor.prepareToPlay(48'000.0, 512);
    require(
        Access::prepareToPlayCount(processor) == 2, "second prepareToPlay");
    require(
        Access::latencyCalls(processor) == after_prepare + 1,
        "48kHz prepare reports final latency 0 once");
    require(processor.getLatencySamples() == 0, "48kHz latency returns to 0");
    juce::AudioBuffer<float> live(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(live, empty);
    require(
        Access::processSampleRateHz(processor) == 48'000,
        "48kHz processBlock should report 48000");

    const auto after_48 = Access::latencyCalls(processor);
    processor.releaseResources();
    require(
        Access::releaseResourcesCount(processor) == 1,
        "releaseResources counter");
    require(
        Access::latencyCalls(processor) == after_48,
        "releaseResources must not call setLatencySamples");
    require(
        processor.getLatencySamples() == 0,
        "release must leave the last reported latency");

    MgstcAudioProcessor bounce;
    bounce.prepareToPlay(44'100.0, 512);
    require(bounce.getLatencySamples() == 7, "re-prepare fixture starts at 7");
    const auto first_changed = Access::latencyChanged(bounce);
    const auto first_calls = Access::latencyCalls(bounce);
    bounce.releaseResources();
    require(
        bounce.getLatencySamples() == 7,
        "release at 44.1kHz must not bounce latency to 0");
    require(
        Access::latencyCalls(bounce) == first_calls,
        "release must not notify Host");
    bounce.prepareToPlay(44'100.0, 512);
    require(bounce.getLatencySamples() == 7, "second 44.1kHz prepare stays 7");
    require(
        Access::latencyChanged(bounce) == first_changed,
        "re-prepare at the same rate must not send kLatencyChanged");
}

void testHostBufferContractAcrossRates() {
    MgstcAudioProcessor processor;
    constexpr std::array rates{44'100.0, 48'000.0, 96'000.0};
    for (const auto rate : rates) {
        processor.prepareToPlay(rate, 512);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, midi);
        require(
            Access::lastNumSamples(processor) == 512, "numSamples 512");
        require(
            Access::lastBufferChannels(processor) == 2, "stereo buffer");
        require(
            Access::lastMidiEvents(processor) == 1, "one MIDI event");
        require(
            Access::sampleRateChangesInProcess(processor) == 0,
            "no per-block sample-rate rewrite");
    }
}

const mgstc::engine::CompositePlaybackLayer* psgLayer(
    const MgstcAudioProcessor& processor) {
    return layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Psg);
}

void requireLoggedNote(
    const MgstcAudioProcessor& processor,
    std::size_t index,
    std::uint64_t frame,
    std::uint8_t note) {
    require(index < Access::logSize(processor), "log index");
    require(Access::logFrame(processor, index) == frame, "log frame");
    require(Access::logNote(processor, index) == note, "log note");
}

void testOverdueMappedEventsKeepOrderAndDoNotStick() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant 100");
    require(Access::plantNoteOn(processor, 101, 61), "plant 101");
    require(Access::plantNoteOn(processor, 102, 62), "plant 102");
    Access::processEventsAt(processor, 105);
    require(Access::mappedIndex(processor) == 3, "all overdue events consumed");
    require(Access::logSize(processor) == 3, "three immediate PSG notes");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 61);
    requireLoggedNote(processor, 2, 105, 62);
    require(Access::frame(processor) == 105, "timeline does not rewind");
    const auto logged = Access::logSize(processor);
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == logged, "second pass adds nothing");
    require(Access::mappedIndex(processor) == 3, "cursor stays past the queue");
}

void testExactFrameStillConsumedAndFutureStaysQueued() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 80, 60), "plant exact");
    require(Access::plantNoteOn(processor, 80, 64), "plant same frame");
    require(Access::plantNoteOn(processor, 90, 67), "plant future");
    Access::processEventsAt(processor, 80);
    require(Access::mappedIndex(processor) == 2, "same-frame pair consumed");
    require(Access::logSize(processor) == 2, "two notes at the exact frame");
    requireLoggedNote(processor, 0, 80, 60);
    requireLoggedNote(processor, 1, 80, 64);
    Access::processEventsAt(processor, 80);
    require(Access::logSize(processor) == 2, "future event stays queued");
    Access::processEventsAt(processor, 90);
    require(Access::mappedIndex(processor) == 3, "future event consumed later");
    requireLoggedNote(processor, 2, 90, 67);
}

void testOverdueThenSameFrameKeepsQueueOrder() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 90, 60), "plant overdue");
    require(Access::plantNoteOn(processor, 105, 61), "plant same a");
    require(Access::plantNoteOn(processor, 105, 62), "plant same b");
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == 3, "overdue and same-frame together");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 61);
    requireLoggedNote(processor, 2, 105, 62);
}

void testOverdueNoteOffCancelsDueLayer() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    require(Access::scheduler(processor).isPending(scc_track), "SCC pending");

    Access::resetMapped(processor);
    require(Access::plantNoteOff(processor, 9'500, 60), "plant overdue note off");
    Access::processEventsAt(processor, 9'600);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "overdue NoteOff cancels the due layer");
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled due layer must not NoteOn");
    require(Access::frame(processor) == 9'600, "cancel does not rewind");
}

void testOverdueVoiceStealReplacesDueLayer() {
    MgstcAudioProcessor processor;
    loadDelayedThreeLayerProgram(processor);
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto delay = delayMillisecondsToEngineFrames(scc->start_delay_ms);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant 60");
    require(Access::plantNoteOn(processor, 101, 61), "plant 61");
    require(Access::plantNoteOn(processor, 102, 62), "plant 62");
    require(Access::plantNoteOn(processor, 103, 63), "plant steal");
    Access::processEventsAt(processor, 105);
    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "stolen voice keeps a delayed SCC");
    require(event->midi_note == 63, "overdue steal keeps the newest note");
    require(
        event->due_frame == 105 + delay,
        "delay is measured from the frame that applied the note");
}

void testOverdueAllNotesOffAndAllSoundOff() {
    {
        MgstcAudioProcessor processor;
        loadDelayedThreeLayerProgram(processor);
        processor.prepareToPlay(48'000.0, 512);
        const auto* scc = layerOf(
            Access::plan(processor), mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
        juce::AudioBuffer<float> buffer(2, 32);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        Access::resetMapped(processor);
        require(Access::plantAllNotesOff(processor, 10), "plant all notes off");
        Access::processEventsAt(processor, 40);
        require(
            Access::scheduler(processor).pendingCount() == 0,
            "overdue All Notes Off clears pending");
        processEmpty(processor, 10'000);
        require(
            !Access::logHasTrack(processor, scc_track),
            "overdue All Notes Off must not start the delayed layer");
    }
    {
        MgstcAudioProcessor processor;
        loadDelayedThreeLayerProgram(processor);
        processor.prepareToPlay(48'000.0, 512);
        const auto* scc = layerOf(
            Access::plan(processor), mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
        juce::AudioBuffer<float> buffer(2, 32);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        Access::resetMapped(processor);
        require(Access::plantAllSoundOff(processor, 8), "plant all sound off");
        Access::processEventsAt(processor, 40);
        require(
            Access::scheduler(processor).pendingCount() == 0,
            "overdue All Sound Off clears pending");
        require(
            Access::isInactive(processor, scc_track),
            "overdue All Sound Off leaves the layer inactive");
        processEmpty(processor, 10'000);
        require(
            !Access::logHasTrack(processor, scc_track),
            "overdue All Sound Off must not start the delayed layer");
    }
}

void testDirectSpanConsumesOverdueWithoutStalling() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 128);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant overdue");
    require(Access::plantNoteOn(processor, 140, 64), "plant inside the span");
    constexpr int kBlock = 128;
    constexpr std::uint64_t kStart = 105;
    juce::AudioBuffer<float> buffer(2, kBlock);
    Access::renderDirect(processor, buffer, kStart);
    require(
        Access::frame(processor) == kStart + static_cast<std::uint64_t>(kBlock),
        "direct span reaches the block end");
    require(Access::srcWatchdogTrips(processor) == 0, "no watchdog");
    require(Access::mappedIndex(processor) == 2, "both events consumed");
    requireLoggedNote(processor, 0, kStart, 60);
    requireLoggedNote(processor, 1, 140, 64);
    require(bufferAllFinite(buffer), "direct span PCM is finite");
}

void testIteratorOverdueIsConsumedInOrder() {
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)),
        2);
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 64);
    Access::bindMidiIterator(processor, midi, 100, 64);
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == 2, "iterator overdue notes fire");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 64);
    Access::clearMidiIterator(processor);

    juce::MidiBuffer later;
    later.addEvent(
        juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)),
        0);
    MgstcAudioProcessor span;
    span.prepareToPlay(48'000.0, 64);
    Access::bindMidiIterator(span, later, 100, 64);
    juce::AudioBuffer<float> buffer(2, 32);
    Access::renderDirect(span, buffer, 105);
    require(
        Access::frame(span) == 137,
        "iterator overdue does not stall the direct span");
    requireLoggedNote(span, 0, 105, 67);
    Access::clearMidiIterator(span);
}

void testBlockBoundaryMidiStaysOnItsFrame() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    juce::AudioBuffer<float> tail(2, 512);
    juce::MidiBuffer end_midi;
    end_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        511);
    processor.processBlock(tail, end_midi);
    require(Access::logHas(processor, 511, track), "last sample of the block");
    juce::AudioBuffer<float> next(2, 512);
    juce::MidiBuffer start_midi;
    start_midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(next, start_midi);
    require(Access::frame(processor) == 1'024, "next block continues");
    require(
        Access::isInactive(processor, track),
        "NoteOff at the next block start releases the voice");
}

void testHostRateOverdueNoteIsAppliedNow() {
    constexpr std::array rates{44'100.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto latency_calls = Access::latencyCalls(processor);
        const auto latency = processor.getLatencySamples();
        const auto* psg = psgLayer(processor);
        require(psg != nullptr, "PSG layer");
        const auto track = Access::plan(processor).physicalTrack(*psg, 0);
        Access::setEngineFrame(processor, 40);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, midi);
        require(
            Access::logHas(processor, 40, track),
            "host-mapped frame 0 is applied at the current engine frame");
        require(
            !Access::logHas(processor, 0, track),
            "overdue note must not rewind to frame 0");
        require(Access::frame(processor) > 40, "engine keeps moving forward");
        require(Access::srcWatchdogTrips(processor) == 0, "SRC watchdog quiet");
        require(
            Access::latencyCalls(processor) == latency_calls,
            "overdue handling must not notify latency");
        require(
            processor.getLatencySamples() == latency,
            "plugin latency stays at the prepared value");
        require(bufferAllFinite(buffer), "overdue host block is finite");
    }
}

juce::MemoryBlock stateBytes(MgstcAudioProcessor& processor) {
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    return block;
}

bool sameBlock(const juce::MemoryBlock& left, const juce::MemoryBlock& right) {
    if (left.getSize() != right.getSize()) {
        return false;
    }
    const auto* a = static_cast<const std::uint8_t*>(left.getData());
    const auto* b = static_cast<const std::uint8_t*>(right.getData());
    return std::equal(a, a + left.getSize(), b);
}

bool sameBuffer(
    const juce::AudioBuffer<float>& left,
    const juce::AudioBuffer<float>& right) {
    if (left.getNumChannels() != right.getNumChannels()
        || left.getNumSamples() != right.getNumSamples()) {
        return false;
    }
    for (int channel = 0; channel < left.getNumChannels(); ++channel) {
        const auto* a = left.getReadPointer(channel);
        const auto* b = right.getReadPointer(channel);
        if (!std::equal(a, a + left.getNumSamples(), b)) {
            return false;
        }
    }
    return true;
}

void renderMidiNote(
    MgstcAudioProcessor& processor,
    juce::AudioBuffer<float>& out) {
    processor.prepareToPlay(48'000.0, 256);
    juce::AudioBuffer<float> warm(2, 256);
    juce::MidiBuffer empty;
    processor.processBlock(warm, empty);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    out.setSize(2, 256);
    processor.processBlock(out, on);
}

juce::AudioBuffer<float> renderEngineCoreMidiNote(
    const mgstc::engine::CompositeTimbre& timbre,
    int total_frames) {
    using namespace mgstc::engine;
    constexpr int block_frames = 256;
    require(total_frames > 0 && total_frames % block_frames == 0,
        "direct EngineCore frame count uses whole blocks");
    EngineCore core;
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(core, timbre, {}, &plan),
        "direct EngineCore compiles the same composite program");
    core.hardReset();
    core.session().gateUntilNoteOn();

    std::array<float, block_frames * 2> interleaved{};
    require(core.render(interleaved).ok(), "EngineCore warmup renders");
    const auto layer = std::find_if(
        plan.audible_layers.begin(), plan.audible_layers.end(),
        [](const auto& binding) { return binding.layer_index == 0; });
    require(layer != plan.audible_layers.end(), "EngineCore test layer is audible");
    const auto note = transposedMidiNote(60, layer->relative_semitones);
    require(note.has_value(), "EngineCore test note remains in range after transpose");
    require(core.session().queueNoteOn(plan.physicalTrack(*layer, 0), *note),
        "EngineCore accepts the matching MIDI note");
    juce::AudioBuffer<float> out(2, total_frames);
    for (int block = 0; block < total_frames / block_frames; ++block) {
        interleaved.fill(0.0F);
        require(core.render(interleaved).ok(), "EngineCore note block renders");
        const auto block_start = block * block_frames;
        for (int frame = 0; frame < block_frames; ++frame) {
            const auto source = static_cast<std::size_t>(frame) * 2;
            out.setSample(0, block_start + frame, interleaved[source]);
            out.setSample(1, block_start + frame, interleaved[source + 1]);
        }
    }
    return out;
}

std::string soundBytes(const mgstc::engine::CompositeTimbre& timbre) {
    return mgstc::engine::serializeCompositeSoundPayload(timbre);
}

// Author-state restoration must retain every sound/metadata field, while
// allocation starts belong to the current editor and are never saved.
bool sameAuthoringSound(mgstc::engine::CompositeTimbre left,
                       mgstc::engine::CompositeTimbre right) {
    left.original_timbre_allocation_starts.reset();
    right.original_timbre_allocation_starts.reset();
    return left == right;
}

mgstc::engine::CompositeTimbre psgOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    timbre.name = "PSG only";
    timbre.memo = "psg";
    timbre.tags = {"psg"};
    timbre.favorite = false;
    timbre.layers = {timbre.layers[0]};
    timbre.layers[0].volume = 11;
    timbre.layers[0].relative_semitones = -2;
    mgstc::engine::EnvelopeEvent pitch;
    pitch.kind = mgstc::engine::EnvelopeEventKind::Pitch;
    pitch.value = -3;
    pitch.count = 4;
    timbre.layers[0].pitch_envelope.events.push_back(pitch);
    return timbre;
}

mgstc::engine::CompositeTimbre sccOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    auto layer = timbre.layers[1];
    mgstc::engine::seedDefaultLayerTimbre(timbre, layer);
    for (std::size_t index = 0; index < layer.base_timbre->scc_waveform.size();
         ++index) {
        layer.base_timbre->scc_waveform[index] =
            static_cast<std::uint8_t>(index * 3U + 1U);
    }
    layer.relative_semitones = 3;
    layer.volume = 12;
    layer.start_delay_form = mgstc::engine::StartDelayForm::AbsoluteTicks;
    layer.start_delay_value = 6;
    layer.software_lfo.enabled = true;
    layer.software_lfo.delay = 1;
    layer.software_lfo.depth = 4;
    layer.software_lfo.speed = 2;
    timbre.name = "SCC only";
    timbre.layers = {std::move(layer)};
    return timbre;
}

mgstc::engine::CompositeTimbre opllOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    auto layer = timbre.layers[2];
    layer.base_opll_rom = std::uint8_t{4};
    layer.volume = 10;
    layer.relative_semitones = -5;
    layer.opll_sustain = true;
    layer.start_delay_form = mgstc::engine::StartDelayForm::AbsoluteTicks;
    layer.start_delay_value = 12;
    mgstc::engine::EnvelopeEvent slide;
    slide.kind = mgstc::engine::EnvelopeEventKind::Timbre;
    slide.timbre_pick = mgstc::engine::TimbrePick::OpllRom;
    slide.value = 8;
    slide.count = 24;
    layer.timbre_automation.push_back(slide);
    timbre.name = "OPLL only";
    timbre.favorite = true;
    timbre.layers = {std::move(layer)};
    return timbre;
}

mgstc::engine::CompositeTimbre compositeTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    timbre.name = "Stage E Snapshot";
    timbre.memo = "self-contained";
    timbre.tags = {"lead", "stage-e"};
    timbre.favorite = true;
    auto scc = sccOnlyTimbre().layers[0];
    auto opll = opllOnlyTimbre().layers[0];
    timbre.layers[0] = psgOnlyTimbre().layers[0];
    timbre.layers[1] = std::move(scc);
    timbre.layers[2] = std::move(opll);
    return timbre;
}

mgstc::plugin::PluginStateDocument documentFrom(
    mgstc::engine::CompositeTimbre timbre,
    std::uint64_t library_id,
    mgstc::plugin::PluginEditorState editor) {
    mgstc::plugin::PluginStateDocument document;
    document.sound = std::move(timbre);
    document.library_id = library_id;
    document.editor = editor;
    return document;
}

void requireSoundRoundTrip(const mgstc::plugin::PluginStateDocument& document) {
    const auto bytes = mgstc::plugin::serializePluginState(document);
    require(!bytes.empty(), "plugin state serializes");
    const auto parsed = mgstc::plugin::parsePluginState(
        bytes.data(), bytes.size());
    require(
        parsed.status == mgstc::plugin::PluginStateStatus::Ok,
        "plugin state parses");
    require(
        soundBytes(parsed.document.sound) == soundBytes(document.sound),
        "sound payload round-trip");
    require(parsed.document.sound.name == document.sound.name, "name");
    require(parsed.document.sound.memo == document.sound.memo, "memo");
    require(parsed.document.sound.tags == document.sound.tags, "tags");
    require(
        parsed.document.sound.favorite == document.sound.favorite,
        "favorite");
    require(parsed.document.library_id == document.library_id, "library id");
    require(
        parsed.document.master_volume_percent
            == document.master_volume_percent,
        "master volume");
    require(parsed.document.editor == document.editor, "editor state");
}

std::size_t sourceByteIndex(const mgstc::engine::CompositeTimbre& timbre) {
    return 2U + 4U + 4U + timbre.layers[0].name.size();
}

std::vector<std::uint8_t> toBytes(std::string_view text) {
    std::vector<std::uint8_t> out(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        out[index] = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[index]));
    }
    return out;
}

std::vector<std::uint8_t> replacePayload(
    std::vector<std::uint8_t> blob,
    const std::vector<std::uint8_t>& original,
    const std::vector<std::uint8_t>& replacement) {
    const auto found = std::search(
        blob.begin(), blob.end(), original.begin(), original.end());
    require(found != blob.end(), "sound payload is inside the plugin state");
    const auto at = static_cast<std::size_t>(std::distance(blob.begin(), found));
    require(at >= 4, "payload size prefix");
    const auto new_size = static_cast<std::uint32_t>(replacement.size());
    blob[at - 4] = static_cast<std::uint8_t>(new_size & 0xFFU);
    blob[at - 3] = static_cast<std::uint8_t>((new_size >> 8U) & 0xFFU);
    blob[at - 2] = static_cast<std::uint8_t>((new_size >> 16U) & 0xFFU);
    blob[at - 1] = static_cast<std::uint8_t>((new_size >> 24U) & 0xFFU);
    blob.erase(
        blob.begin() + static_cast<std::ptrdiff_t>(at),
        blob.begin() + static_cast<std::ptrdiff_t>(at + original.size()));
    blob.insert(
        blob.begin() + static_cast<std::ptrdiff_t>(at),
        replacement.begin(),
        replacement.end());
    return blob;
}

void requireRejected(
    MgstcAudioProcessor& processor,
    const std::vector<std::uint8_t>& bytes) {
    const auto before = stateBytes(processor);
    const auto compiles = Access::stateCompileCount(processor);
    processor.setStateInformation(
        bytes.empty() ? nullptr : bytes.data(),
        static_cast<int>(bytes.size()));
    require(
        Access::stateCompileCount(processor) == compiles,
        "rejected state does not compile");
    require(sameBlock(before, stateBytes(processor)), "rejected state keeps the previous snapshot");
}

void testPluginStateFoundation() {
    require(MgstcAudioProcessor{}.hasEditor(), "plugin exposes the shared editor");

    MgstcAudioProcessor original;
    const auto original_bytes = stateBytes(original);
    require(!original_bytes.isEmpty(), "default state is not empty");
    MgstcAudioProcessor restored;
    restored.setStateInformation(
        original_bytes.getData(),
        static_cast<int>(original_bytes.getSize()));
    require(sameBlock(original_bytes, stateBytes(restored)), "default round-trip");
    const auto smoke = restored.copyPluginState().sound;
    require(smoke.layers.size() == 1, "plugin default composite is PSG x1");
    require(
        smoke.layers[0].source == mgstc::engine::TimbreSource::Psg,
        "plugin default layer is PSG");
    require(
        smoke.layers[0].start_delay_value == 0,
        "plugin default PSG has no start delay");

    const mgstc::plugin::PluginEditorState editor{
        .has_selected_layer = true,
        .selected_layer = 1,
        .editor_tab = 3,
    };
    const auto psg = documentFrom(psgOnlyTimbre(), 0, {});
    const auto scc = documentFrom(sccOnlyTimbre(), 0, editor);
    const auto opll = documentFrom(opllOnlyTimbre(), 382, editor);
    auto composite = documentFrom(compositeTimbre(), 382, editor);
    requireSoundRoundTrip(psg);
    requireSoundRoundTrip(scc);
    requireSoundRoundTrip(opll);
    requireSoundRoundTrip(composite);

    const auto scc_parsed = mgstc::plugin::parsePluginState(
        mgstc::plugin::serializePluginState(scc).data(),
        mgstc::plugin::serializePluginState(scc).size());
    require(
        scc_parsed.document.sound.layers[0].base_timbre->scc_waveform
            == scc.sound.layers[0].base_timbre->scc_waveform,
        "SCC waveform snapshot");
    require(
        scc_parsed.document.sound.layers[0].software_lfo.enabled
            && scc_parsed.document.sound.layers[0].software_lfo.depth == 4,
        "LFO snapshot");
    require(
        scc_parsed.document.sound.layers[0].start_delay_value == 6,
        "layer delay snapshot");
    const auto opll_parsed = mgstc::plugin::parsePluginState(
        mgstc::plugin::serializePluginState(opll).data(),
        mgstc::plugin::serializePluginState(opll).size());
    require(
        mgstc::engine::layerEnvelopeHasPatchSlide(
            opll_parsed.document.sound.layers[0], nullptr),
        "patch slide snapshot");
    require(
        !opll_parsed.document.sound.layers[0].pitch_envelope.events.empty()
            || opll_parsed.document.sound.layers[0].volume_envelope.events.size()
                > 0,
        "envelope snapshot");
    require(
        psg.sound.layers[0].pitch_envelope.events.back().value == -3,
        "pitch envelope value");

    MgstcAudioProcessor player_a;
    require(player_a.replacePluginState(composite), "commit composite");
    const auto committed = stateBytes(player_a);
    MgstcAudioProcessor player_b;
    player_b.setStateInformation(
        committed.getData(), static_cast<int>(committed.getSize()));
    juce::AudioBuffer<float> pcm_a;
    juce::AudioBuffer<float> pcm_b;
    renderMidiNote(player_a, pcm_a);
    renderMidiNote(player_b, pcm_b);
    require(sameBuffer(pcm_a, pcm_b), "restored program matches PCM");
    require(bufferAllFinite(pcm_a), "restored PCM is finite");

    auto library_a = composite;
    library_a.library_id = 0;
    auto library_b = composite;
    library_b.library_id = 382;
    require(
        soundBytes(library_a.sound) == soundBytes(library_b.sound),
        "library id is not part of the sound payload");
    MgstcAudioProcessor lib_left;
    MgstcAudioProcessor lib_right;
    require(lib_left.replacePluginState(library_a), "library 0");
    require(lib_right.replacePluginState(library_b), "library 382");
    require(lib_right.copyPluginState().library_id == 382, "library metadata");
    juce::AudioBuffer<float> lib_pcm_a;
    juce::AudioBuffer<float> lib_pcm_b;
    renderMidiNote(lib_left, lib_pcm_a);
    renderMidiNote(lib_right, lib_pcm_b);
    require(sameBuffer(lib_pcm_a, lib_pcm_b), "library id does not change PCM");

    MgstcAudioProcessor instance_a;
    MgstcAudioProcessor instance_b;
    require(instance_a.replacePluginState(psg), "instance A");
    require(instance_b.replacePluginState(opll), "instance B");
    const auto instance_b_bytes = stateBytes(instance_b);
    require(instance_a.replacePluginState(scc), "instance A changes");
    require(
        sameBlock(instance_b_bytes, stateBytes(instance_b)),
        "instance B is unchanged");
    require(
        instance_a.copyPluginState().sound.name
            != instance_b.copyPluginState().sound.name,
        "instances keep different programs");

    const auto valid = mgstc::plugin::serializePluginState(psg);
    require(
        mgstc::plugin::parsePluginState(nullptr, 0).status
            == mgstc::plugin::PluginStateStatus::Empty,
        "empty");
    require(
        mgstc::plugin::parsePluginState(valid.data(), 4).status
            == mgstc::plugin::PluginStateStatus::Truncated,
        "truncated header");
    std::vector<std::uint8_t> bad_magic(8, static_cast<std::uint8_t>('X'));
    require(
        mgstc::plugin::parsePluginState(bad_magic.data(), bad_magic.size()).status
            == mgstc::plugin::PluginStateStatus::BadMagic,
        "bad magic");
    auto unknown = valid;
    unknown[8] = 3;
    require(
        mgstc::plugin::parsePluginState(unknown.data(), unknown.size()).status
            == mgstc::plugin::PluginStateStatus::UnsupportedVersion,
        "unknown version");
    auto trailing = valid;
    trailing.push_back(0);
    require(
        mgstc::plugin::parsePluginState(trailing.data(), trailing.size()).status
            == mgstc::plugin::PluginStateStatus::BrokenPayload,
        "trailing bytes");

    auto payload = toBytes(soundBytes(psg.sound));
    const auto source_at = sourceByteIndex(psg.sound);
    require(payload.size() > source_at + 1, "payload has a source byte");
    require(
        payload[source_at]
            == static_cast<std::uint8_t>(psg.sound.layers[0].source),
        "source byte layout");
    auto invalid_enum = payload;
    invalid_enum[source_at] = 9;
    auto oversized = payload;
    oversized[2] = 0xFF;
    oversized[3] = 0xFF;
    auto bad_channel = payload;
    bad_channel[source_at + 1] = 255;
    auto short_wave = payload;
    short_wave.resize(short_wave.size() - 8);

    MgstcAudioProcessor guard;
    requireRejected(guard, {});
    requireRejected(guard, std::vector<std::uint8_t>(valid.begin(), valid.begin() + 12));
    requireRejected(guard, bad_magic);
    requireRejected(guard, unknown);
    requireRejected(guard, trailing);
    requireRejected(guard, replacePayload(valid, payload, invalid_enum));
    requireRejected(guard, replacePayload(valid, payload, oversized));
    requireRejected(guard, replacePayload(valid, payload, bad_channel));
    requireRejected(guard, replacePayload(valid, payload, short_wave));

    auto corrupt = guard.copyPluginState();
    corrupt.sound.layers[0].channel = 255;
    const auto compiles = Access::stateCompileCount(guard);
    const auto before_direct = stateBytes(guard);
    require(!guard.replacePluginState(corrupt), "invalid channel is rejected");
    require(Access::stateCompileCount(guard) == compiles, "invalid channel does not compile");
    require(sameBlock(before_direct, stateBytes(guard)), "direct reject keeps state");

    MgstcAudioProcessor audio_guard;
    const auto audio_before = stateBytes(audio_guard);
    const auto audio_compiles = Access::stateCompileCount(audio_guard);
    Access::poseAsAudioThread(audio_guard);
    audio_guard.setStateInformation(valid.data(), static_cast<int>(valid.size()));
    require(
        Access::stateAudioRejectCount(audio_guard) == 1,
        "audio thread rejects state");
    require(
        Access::stateCompileCount(audio_guard) == audio_compiles,
        "audio thread does not compile");
    Access::clearAudioThread(audio_guard);
    require(sameBlock(audio_before, stateBytes(audio_guard)), "audio-thread reject keeps state");

    MgstcAudioProcessor timed;
    loadDelayedThreeLayerProgram(timed);
    timed.prepareToPlay(44'100.0, 128);
    juce::AudioBuffer<float> attack(2, 128);
    juce::MidiBuffer note;
    note.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    timed.processBlock(attack, note);
    require(Access::noteOnCount(timed) > 0, "note is sounding");
    require(Access::scheduler(timed).pendingCount() > 0, "delayed layer is pending");
    require(timed.replacePluginState(psg), "replace while prepared");
    processEmpty(timed, 48'000);
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Psg) != nullptr, "PSG remains");
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Scc) == nullptr, "old SCC delay is gone");
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Opll) == nullptr, "old OPLL delay is gone");
    require(Access::scheduler(timed).pendingCount() == 0, "old pending layers are dropped");

    MgstcAudioProcessor rates;
    require(rates.replacePluginState(psg), "program before rate changes");
    rates.prepareToPlay(44'100.0, 512);
    const auto latency_calls = Access::latencyCalls(rates);
    const auto latency = rates.getLatencySamples();
    require(latency == 7, "44.1 kHz latency stays 7");
    require(rates.replacePluginState(scc), "restore does not touch latency");
    processEmpty(rates, 512);
    require(Access::latencyCalls(rates) == latency_calls, "setState does not report latency");
    require(rates.getLatencySamples() == latency, "latency value unchanged by setState");
    const auto at_441 = stateBytes(rates);
    rates.prepareToPlay(48'000.0, 512);
    require(rates.getLatencySamples() == 0, "48 kHz latency is still 0");
    require(sameBlock(at_441, stateBytes(rates)), "48 kHz keeps the sound snapshot");
    rates.prepareToPlay(96'000.0, 256);
    require(sameBlock(at_441, stateBytes(rates)), "96 kHz keeps the sound snapshot");
    rates.prepareToPlay(88'200.0, 256);
    require(sameBlock(at_441, stateBytes(rates)), "88.2 kHz keeps the sound snapshot");
    require(rates.getLatencySamples() == 15, "88.2 kHz latency is unchanged by state");

    MgstcAudioProcessor audible;
    audible.setStateInformation(valid.data(), static_cast<int>(valid.size()));
    audible.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> heard(2, 512);
    juce::MidiBuffer heard_midi;
    heard_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    audible.processBlock(heard, heard_midi);
    double peak = channelPeak(heard, 0, 512);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 512);
        juce::MidiBuffer empty;
        audible.processBlock(more, empty);
        peak = channelPeak(more, 0, 512);
    }
    require(peak > 0.001, "restored PSG is audible at 48 kHz");
    audible.prepareToPlay(44'100.0, 512);
    juce::AudioBuffer<float> heard_44(2, 512);
    juce::MidiBuffer midi_44;
    midi_44.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    audible.processBlock(heard_44, midi_44);
    peak = channelPeak(heard_44, 0, 512);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 512);
        juce::MidiBuffer empty;
        audible.processBlock(more, empty);
        peak = channelPeak(more, 0, 512);
    }
    require(peak > 0.001, "restored PSG is audible at 44.1 kHz");
}

void testWideSoftwareLfoMatchesDirectEngineCoreWithPitchEnvelope() {
    using namespace mgstc::engine;
    constexpr int block_frames = 256;
    constexpr int rendered_frames = 12'288;
    for (const auto source : {TimbreSource::Psg, TimbreSource::Scc}) {
        auto timbre = source == TimbreSource::Psg
            ? psgOnlyTimbre()
            : sccOnlyTimbre();
        auto& layer = timbre.layers.front();
        // This test drives the shared EngineCore directly after key-on; layer
        // delay scheduling has separate coverage and is not part of @p parity.
        layer.start_delay_form = StartDelayForm::AbsoluteTicks;
        layer.start_delay_value = 0;
        layer.pitch_envelope.events.clear();
        EnvelopeEvent pitch;
        pitch.kind = EnvelopeEventKind::Pitch;
        pitch.value = -3;
        pitch.count = 8;
        layer.pitch_envelope.events.push_back(pitch);
        layer.software_lfo.enabled = true;
        layer.software_lfo.delay = 0;
        layer.software_lfo.depth = 20;
        layer.software_lfo.speed = 1;
        layer.software_lfo.roughness = 0;
        layer.software_lfo.extra_roughness = 5;

        MgstcAudioProcessor processor;
        require(processor.replacePluginState(documentFrom(timbre, 0, {})),
            "plugin applies the composite program carrying ENV pitch and @p");
        processor.prepareToPlay(48'000.0, block_frames);
        juce::AudioBuffer<float> warm(2, block_frames);
        juce::MidiBuffer empty;
        processor.processBlock(warm, empty);

        juce::AudioBuffer<float> plugin_pcm(2, rendered_frames);
        for (int block = 0; block < rendered_frames / block_frames; ++block) {
            juce::AudioBuffer<float> chunk(2, block_frames);
            juce::MidiBuffer midi;
            if (block == 0) {
                midi.addEvent(
                    juce::MidiMessage::noteOn(
                        1, 60, static_cast<juce::uint8>(100)),
                    0);
            }
            processor.processBlock(chunk, midi);
            plugin_pcm.copyFrom(0, block * block_frames, chunk, 0, 0, block_frames);
            plugin_pcm.copyFrom(1, block * block_frames, chunk, 1, 0, block_frames);
        }

        const auto core_pcm = renderEngineCoreMidiNote(timbre, rendered_frames);
        require(bufferAllFinite(plugin_pcm), "@p processor output is finite");
        require(channelPeak(plugin_pcm, 0, rendered_frames) > 0.001,
            "@p processor output remains audible through the first update");
        if (!sameBuffer(plugin_pcm, core_pcm)) {
            float max_difference = 0.0F;
            int first_difference = -1;
            for (int frame = 0; frame < rendered_frames; ++frame) {
                const auto difference = std::abs(
                    plugin_pcm.getSample(0, frame)
                    - core_pcm.getSample(0, frame));
                if (difference > 0.0F && first_difference < 0) {
                    first_difference = frame;
                }
                max_difference = std::max(max_difference, difference);
            }
            std::fprintf(stderr,
                "@p PCM mismatch source=%s first_frame=%d max_abs_diff=%.9g "
                "plugin_peak=%.9g engine_peak=%.9g\n",
                source == TimbreSource::Psg ? "PSG" : "SCC",
                first_difference,
                static_cast<double>(max_difference),
                channelPeak(plugin_pcm, 0, rendered_frames),
                channelPeak(core_pcm, 0, rendered_frames));
        }
        require(sameBuffer(plugin_pcm, core_pcm),
            source == TimbreSource::Psg
                ? "PSG DAW MIDI @p+ENV output matches direct EngineCore"
                : "SCC DAW MIDI @p+ENV output matches direct EngineCore");
    }
}

void testSccMorphPluginStateCompatibility() {
    using namespace mgstc::engine;
    auto document = documentFrom(sccOnlyTimbre(), 0, {});
    document.sound.scc_morph_algorithm_version = 2;
    auto& layer = document.sound.layers.front();
    layer.volume_envelope.events.clear();
    layer.pitch_envelope.events.clear();
    layer.timbre_automation.clear();
    layer.pitch_modulation = {};
    layer.volume_modulation = {};
    layer.opll_tl_modulation = {};
    layer.opll_fb_modulation = {};
    document.sound.embedded_timbres.clear();
    // One event-free layer: v24 adds three empty profile/placement vector
    // counts before the embedded count; v22 adds two allocation bytes there.
    // No manual start or event fields are present in this legacy-mode fixture.
    layer.scc_output_allocation = SccOutputAllocationMode::LegacyBank;
    layer.scc_output_start.reset();
    const auto original = toBytes(soundBytes(document.sound));
    const auto framed = mgstc::plugin::serializePluginState(document);
    // v21 adds only the five global morph bytes.
    // v20 adds four six-byte modulation settings before the embedded count.
    for (const std::uint8_t version : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{5}}) {
        auto legacy = original;
        require(legacy.size() >= 45, "legacy sound fixture tail exists");
        legacy.erase(legacy.end() - 21, legacy.end() - 9);
        if (version <= 3) legacy.erase(legacy.end() - 11, legacy.end() - 9);
        if (version <= 2) legacy.resize(legacy.size() - 5);
        if (version == 1) {
            legacy.erase(legacy.end() - 28, legacy.end() - 4);
        }
        std::string legacy_error;
        const auto legacy_sound = deserializeCompositeSoundPayload(
            std::string(legacy.begin(), legacy.end()), version, &legacy_error);
        if (!legacy_sound) {
            std::fprintf(stderr, "legacy sound payload v%u fixture: %s\n",
                static_cast<unsigned>(version), legacy_error.c_str());
        }
        require(legacy_sound.has_value(), "legacy sound payload fixture is valid");
        auto blob = replacePayload(framed, original, legacy);
        const auto found = std::search(blob.begin(), blob.end(), legacy.begin(), legacy.end());
        require(found != blob.end(), "legacy payload framed");
        const auto at = static_cast<std::size_t>(std::distance(blob.begin(), found));
        require(at >= 8, "legacy payload version prefix");
        blob[at - 8] = version;
        blob[at - 7] = blob[at - 6] = blob[at - 5] = 0;
        const auto parsed = mgstc::plugin::parsePluginState(blob.data(), blob.size());
        if (parsed.status != mgstc::plugin::PluginStateStatus::Ok) {
            std::fprintf(stderr, "legacy plugin sound payload v%u status=%u\n",
                static_cast<unsigned>(version), static_cast<unsigned>(parsed.status));
        }
        require(parsed.status == mgstc::plugin::PluginStateStatus::Ok,
                "legacy v1/v2/v3/v4/v5 plugin sound payload readable");
        auto expected_legacy_sound = document.sound;
        if (version <= 2) expected_legacy_sound.scc_morph_algorithm_version = 1;
        require(soundBytes(parsed.document.sound) == soundBytes(expected_legacy_sound),
                "legacy plugin payload defaults preserve sound");
    }
    auto arrival = *layer.base_timbre;
    arrival.library_id = allocateCompositeOwnedTimbreId(document.sound);
    arrival.name = "Morph destination";
    for (std::size_t i = 0; i < arrival.scc_waveform.size(); ++i) {
        arrival.scc_waveform[i] = static_cast<std::uint8_t>(i < 16 ? 64 : -64);
    }
    document.sound.embedded_timbres.push_back(arrival);
    layer.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .count = 0,
         .target_library_id = layer.base_timbre->library_id},
        {.kind = EnvelopeEventKind::Timbre, .count = 20,
         .target_library_id = arrival.library_id,
         .scc_morph = {.enabled = true, .intermediate_count = 3, .curve = 2.0}},
    };
    document.sound.scc_morph_bank_base = 16;
    layer.scc_output_allocation = SccOutputAllocationMode::Contiguous;
    layer.scc_output_start = 20;
    layer.timbre_automation.back().scc_morph.curve = 0.5;
    document.sound.scc_morph_algorithm_version = 3;
    const SccMorphDistributionMode modes[] = {
        SccMorphDistributionMode::AdaptiveDistribution,
        SccMorphDistributionMode::TimeDistribution,
        SccMorphDistributionMode::ToneDistribution};
    for (const auto mode : modes) {
        layer.timbre_automation.back().scc_morph.distribution_mode = mode;
        requireSoundRoundTrip(document);
    }
    layer.timbre_automation.back().scc_morph.distribution_mode =
        SccMorphDistributionMode::AdaptiveDistribution;
    layer.timbre_automation.back().scc_morph.explicit_plan =
        SccMorphPlan{{{0.0, 0}, {0.1, 2}, {0.3, 7}, {0.8, 13}, {1.0, 20}}};
    requireSoundRoundTrip(document);
    const auto rejects = [&](const mgstc::plugin::PluginStateDocument& invalid) {
        return !mgstc::plugin::validatePluginSoundSnapshot(invalid.sound)
            && mgstc::plugin::serializePluginState(invalid).empty();
    };
    auto invalid = document;
    invalid.sound.layers.front().timbre_automation.back().scc_morph.distribution_mode =
        static_cast<SccMorphDistributionMode>(255);
    require(rejects(invalid), "plugin state rejects unknown distribution mode");
    invalid = document;
    invalid.sound.layers.front().timbre_automation.back().scc_morph.explicit_plan->points[1].morph_position =
        std::numeric_limits<double>::infinity();
    require(rejects(invalid), "plugin state rejects non-finite explicit plan");
    invalid = document;
    invalid.sound.layers.front().timbre_automation.back().scc_morph.explicit_plan->points[1].event_count = 0;
    require(rejects(invalid), "plugin state rejects duplicate explicit counts");
    MgstcAudioProcessor processor;
    require(processor.replacePluginState(document), "morph plugin state compiles");
    requireSoundRoundTrip(processor.copyPluginState());
    require(processor.copyPluginState().sound.embedded_timbres.size() == 1,
            "plugin state keeps sources without generated bank snapshots");
}

std::vector<float> captureHostNotePcm(
    MgstcAudioProcessor& processor,
    int frames = 2048) {
    juce::AudioBuffer<float> buffer(2, frames);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    std::vector<float> pcm(static_cast<std::size_t>(frames));
    const auto* left = buffer.getReadPointer(0);
    for (int index = 0; index < frames; ++index) {
        pcm[static_cast<std::size_t>(index)] = left[index];
    }
    return pcm;
}

double peakOf(const std::vector<float>& pcm) {
    double peak = 0.0;
    for (const auto sample : pcm) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    return peak;
}

void requireWorkingCompositeMatchesProcessor(
    juce::AudioProcessorEditor* editor,
    MgstcAudioProcessor& processor,
    const char* message) {
    auto* plugin_editor =
        dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(editor);
    require(plugin_editor != nullptr, "plugin editor type");
    require(
        soundBytes(plugin_editor->copyWorkingComposite())
            == soundBytes(processor.copyPluginState().sound),
        message);
}

std::vector<std::uint8_t> asLegacyV1(std::vector<std::uint8_t> v2) {
    require(v2.size() > 36, "v2 blob has master volume field");
    require(v2[8] == 2, "current serializer writes schema v2");
    v2[8] = 1;
    v2.erase(v2.begin() + 32, v2.begin() + 36);
    return v2;
}

void testPluginProjectRestore() {
    auto composite = documentFrom(compositeTimbre(), 7, {});
    composite.master_volume_percent = 40;
    composite.sound.layers[1].enabled = false;
    requireSoundRoundTrip(composite);

    MgstcAudioProcessor saved;
    require(saved.replacePluginState(composite), "commit E4 composite");
    require(saved.editorMasterVolumePercent() == 40, "processor stores master 40");
    const auto blob = stateBytes(saved);

    MgstcAudioProcessor before_prepare;
    before_prepare.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    require(
        before_prepare.editorMasterVolumePercent() == 40,
        "restore before prepareToPlay keeps master volume");
    require(
        soundBytes(before_prepare.copyPluginState().sound)
            == soundBytes(composite.sound),
        "restore before prepareToPlay keeps sound");
    require(
        !before_prepare.copyPluginState().sound.layers[1].enabled,
        "disabled SCC layer round-trips");
    before_prepare.prepareToPlay(48'000.0, 512);
    const auto restored_pcm = captureHostNotePcm(before_prepare);
    require(peakOf(restored_pcm) > 0.001, "editor-less restore is audible");

    MgstcAudioProcessor after_prepare;
    after_prepare.prepareToPlay(48'000.0, 512);
    after_prepare.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    require(
        after_prepare.editorMasterVolumePercent() == 40,
        "restore after prepareToPlay keeps master volume");
    processEmpty(after_prepare, 512);
    processEmpty(after_prepare, 512);
    const auto live_pcm = captureHostNotePcm(after_prepare);
    require(peakOf(live_pcm) > 0.001, "restore after prepareToPlay is audible");

    MgstcAudioProcessor reference;
    require(reference.replacePluginState(composite), "reference 40%");
    reference.prepareToPlay(48'000.0, 512);
    const auto reference_pcm = captureHostNotePcm(reference);
    require(
        std::abs(peakOf(restored_pcm) / peakOf(reference_pcm) - 1.0) < 0.05,
        "restored master volume matches a fresh processor");

    MgstcAudioProcessor hydrate;
    hydrate.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    const auto compiles = Access::stateCompileCount(hydrate);
    auto* editor = hydrate.createEditor();
    requireWorkingCompositeMatchesProcessor(
        editor,
        hydrate,
        "createEditor after restore hydrates Processor composite");
    require(
        hydrate.copyPluginState().sound.name == composite.sound.name,
        "editor open does not replace restored sound");
    require(
        hydrate.editorMasterVolumePercent() == 40,
        "editor open does not reset restored master volume");
    require(
        Access::stateCompileCount(hydrate) == compiles,
        "editor open after restore does not compile");
    delete editor;

    const auto v1 = asLegacyV1(
        mgstc::plugin::serializePluginState(composite));
    const auto v1_parsed = mgstc::plugin::parsePluginState(
        v1.data(), v1.size());
    require(
        v1_parsed.status == mgstc::plugin::PluginStateStatus::Ok,
        "schema v1 still parses");
    require(
        v1_parsed.document.master_volume_percent == 100,
        "schema v1 restores master volume as 100");
    require(
        soundBytes(v1_parsed.document.sound) == soundBytes(composite.sound),
        "schema v1 keeps the sound payload");

    MgstcAudioProcessor v1_host;
    v1_host.editorSetMasterVolumePercent(25);
    v1_host.setStateInformation(v1.data(), static_cast<int>(v1.size()));
    require(
        v1_host.editorMasterVolumePercent() == 100,
        "v1 project restore applies default master 100");

    const auto* blob_begin =
        static_cast<const std::uint8_t*>(blob.getData());
    auto future = std::vector<std::uint8_t>(
        blob_begin, blob_begin + blob.getSize());
    future[8] = 3;
    requireRejected(saved, future);

    auto impossible = future;
    impossible[8] = 2;
    impossible[32] = 101;
    impossible[33] = 0;
    impossible[34] = 0;
    impossible[35] = 0;
    require(
        saved.editorMasterVolumePercent() == 40, "guard still at 40");
    requireRejected(saved, impossible);
    require(
        saved.editorMasterVolumePercent() == 40,
        "impossible master volume does not partial-apply");

    MgstcAudioProcessor instance_a;
    MgstcAudioProcessor instance_b;
    auto left = documentFrom(psgOnlyTimbre(), 0, {});
    left.master_volume_percent = 30;
    auto right = documentFrom(opllOnlyTimbre(), 0, {});
    right.master_volume_percent = 70;
    require(instance_a.replacePluginState(left), "instance A E4");
    require(instance_b.replacePluginState(right), "instance B E4");
    const auto a_bytes = stateBytes(instance_a);
    const auto b_bytes = stateBytes(instance_b);
    MgstcAudioProcessor restore_a;
    MgstcAudioProcessor restore_b;
    restore_a.setStateInformation(
        a_bytes.getData(), static_cast<int>(a_bytes.getSize()));
    restore_b.setStateInformation(
        b_bytes.getData(), static_cast<int>(b_bytes.getSize()));
    require(
        restore_a.editorMasterVolumePercent() == 30
            && restore_b.editorMasterVolumePercent() == 70,
        "multi-instance master volumes stay independent");
    require(
        restore_a.copyPluginState().sound.name
            != restore_b.copyPluginState().sound.name,
        "multi-instance composites stay independent");

    MgstcAudioProcessor at_441;
    at_441.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    at_441.prepareToPlay(44'100.0, 512);
    require(
        peakOf(captureHostNotePcm(at_441)) > 0.001,
        "44.1 kHz restore is audible");
    require(
        at_441.getLatencySamples() == 7,
        "44.1 kHz restore does not touch latency");
}

// Reproduce Standalone's ordering: the composite posts target B, window
// activation refreshes the old editor A, then requestLibraryEntry loads B.
// Plugin uses the same satellite components, with no SQLite fallback available.
void testOwnedEditorPendingTargetKeepsSnapshot() {
    using Access = mgstc::app::SatelliteEditorParityTestAccess;
    using mgstc::engine::SavedTimbreReference;
    using mgstc::engine::TimbreSource;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    const auto open = [](const juce::String&, std::optional<std::uint64_t>) {};
    mgstc::app::SccEditorComponent scc(context, link, open, [] {},
        [](std::vector<mgstc::engine::OpllPatchParameters>) {},
        [](juce::Component*) {}, true, [] {});
    mgstc::app::OpllEditorComponent opll(context, link, open, [] {},
        [](juce::Component*) {}, true, [] {});
    std::size_t synced = 0;
    link.owned_audition = {
        [&] {
            require(link.owned_published.has_value(), "owned sync has snapshot");
            require(mgstc::engine::replaceOwnedTimbreSnapshot(
                timbre, *link.owned_published), "owned snapshot remains in composite");
            ++synced;
        }, [] {}, [](std::uint8_t) {}, [](std::uint8_t) {}, [] {}};
    const auto exercise = [&](auto& editor, TimbreSource source,
                              std::size_t layer_index) {
        SavedTimbreReference a;
        a.library_id = 0xff00000000000000ULL + layer_index * 2;
        a.revision = 7;
        a.name = source == TimbreSource::Scc ? "SCC A" : "OPLL A";
        a.source = source;
        a.number_mode = mgstc::engine::TimbreNumberMode::Manual;
        a.manual_number = 23;
        a.opll_registers = {0x71, 0x61, 0x1e, 0x17, 0xd0, 0x78, 0, 0x17};
        for (std::size_t i = 0; i < a.scc_waveform.size(); ++i)
            a.scc_waveform[i] = static_cast<std::uint8_t>(i * 3 + 1);
        auto b = a;
        ++b.library_id;
        b.revision = 11;
        b.name = source == TimbreSource::Scc ? "SCC B" : "OPLL B";
        b.manual_number = 24;
        b.opll_registers[3] ^= 0x04;
        b.scc_waveform[3] ^= 0x7f;
        timbre.layers[layer_index].base_opll_rom.reset();
        timbre.layers[layer_index].base_timbre = a;
        timbre.embedded_timbres.push_back(b);
        link.owned_target = a;
        editor.requestLibraryEntry(a.library_id);
        Access::editOwned(editor);
        require(link.owned_published.has_value(), "edit publishes owned A");
        const auto edited_a = *link.owned_published;
        require(edited_a.name != a.name
            && edited_a.library_id == a.library_id
            && edited_a.number_mode == a.number_mode
            && edited_a.manual_number == a.manual_number
            && edited_a.revision == a.revision,
            "editing preserves A assignment metadata");
        require(a.name == (source == TimbreSource::Scc ? "SCC A" : "OPLL A")
            && a.opll_registers[3] == 0x17 && a.scc_waveform[3] == 10,
            "owned editing preserves original source A");
        link.owned_target = b;
        const auto before_refresh = synced;
        Access::activationRefresh(editor);
        require(synced > before_refresh, "activation refresh publishes current owned tone");
        require(link.owned_target == b,
            "activation of A preserves pending embedded B request");
        require(link.owned_published == edited_a,
            "activation keeps A payload and assignment metadata");
        editor.requestLibraryEntry(b.library_id);
        require(Access::ownedSnapshot(editor) == b,
            "same-channel switch loads embedded B without library fallback");
        require(*mgstc::engine::findEmbeddedTimbreSnapshot(timbre, b.library_id) == b,
            "switch preserves original B payload and assignment");
        link.owned_target = edited_a;
        Access::activationRefresh(editor);
        require(link.owned_target == edited_a,
            "activation of B preserves pending edited A request");
        editor.requestLibraryEntry(a.library_id);
        require(Access::ownedSnapshot(editor) == edited_a,
            "A to B to A retains edited waveform or patch and name");
        require(*mgstc::engine::findEmbeddedTimbreSnapshot(timbre, b.library_id) == b,
            "reopening A leaves B unchanged");
    };
    exercise(scc, TimbreSource::Scc, 1);
    exercise(opll, TimbreSource::Opll, 2);
}

void testEditorParityCorrections() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    MgstcAudioProcessor other;
    auto original = mgstc::engine::defaultCompositeTimbre();
    require(processor.replacePluginState(documentFrom(original, 0, {})), "parity seed");
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent component(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {}, [](std::uint8_t) {},
        [](juce::Component*) {}, [] {}, original);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto bytes = stateBytes(processor);
    EditorAccess::immediate(component, false);
    EditorAccess::note(component, 73);
    for (int i = 0; i < 60; ++i) EditorAccess::pollOwned(component);
    require(!EditorAccess::immediate(component) && EditorAccess::note(component) == 73,
        "timer retains plugin immediate OFF and last audition note");
    require(sameBlock(bytes, stateBytes(processor)), "UI audition preferences do not change sound state");
    require(other.editorAuditionPreferences().composite_immediate
        && other.editorAuditionPreferences().last_note == 60, "audition preferences are per instance");
    {
        mgstc::plugin::PluginEditorContext reopened(processor);
        require(!reopened.auditionPreferences().composite_immediate
            && reopened.auditionPreferences().last_note == 73, "editor reopen retains audition preferences");
    }
    const auto history = EditorAccess::historySize(component);
    const auto scc = EditorAccess::sound(component).layers[1];
    auto patch = mgstc::engine::decodeOpllPatch(std::array<std::uint8_t, 8>{0x71,0x61,0x1e,0x17,0xd0,0x78,0,0x17});
    require(component.applyOpllConversionCandidate(patch), "conversion candidate applies");
    require(processor.copyPluginState().sound == EditorAccess::sound(component),
        "conversion candidate reflects into processor sound authority");
    require(EditorAccess::sound(component).layers[1] == scc, "SCC candidate source is preserved");
    require(EditorAccess::sound(component).layers[2].base_timbre->opll_registers
        == mgstc::engine::encodeOpllPatch(patch), "OPLL destination gets converted bytes");
    require(EditorAccess::historySize(component) == history + 1, "conversion is one undo transaction");
    EditorAccess::undo(component);
    require(sameAuthoringSound(EditorAccess::sound(component), original), "conversion undo restores original");
    auto no_opll = sccOnlyTimbre();
    no_opll.layers[0].envelope_number = 2;
    component.restoreHostState(no_opll);
    require(component.applyOpllConversionCandidate(patch), "conversion adds missing OPLL destination");
    require(processor.copyPluginState().sound == EditorAccess::sound(component),
        "added OPLL conversion reflects into processor sound authority");
    require(EditorAccess::sound(component).layers.size() == no_opll.layers.size() + 1
        && EditorAccess::sound(component).layers[0] == no_opll.layers[0], "add preserves SCC channel");
    require(EditorAccess::sound(component).layers.back().envelope_number != 2,
        "added OPLL channel uses a free envelope number");
    require(EditorAccess::historySize(component) == 2, "add and candidate replacement are one undo transaction");
    EditorAccess::undo(component);
    require(sameAuthoringSound(EditorAccess::sound(component), no_opll), "one undo removes added OPLL channel");
    {
        const auto open = [](const juce::String&, std::optional<std::uint64_t>) {};
        mgstc::app::SccEditorComponent scc_editor(context, link, open, [] {},
            [](std::vector<mgstc::engine::OpllPatchParameters>) {}, [](juce::Component*) {}, true, [] {});
        mgstc::app::SatelliteEditorParityTestAccess::checkNumeric(scc_editor);
        mgstc::app::SatelliteEditorParityTestAccess::checkSccWav(scc_editor);
        mgstc::app::OpllEditorComponent opll_editor(context, link, open, [] {},
            [](juce::Component*) {}, true, [] {});
        require(mgstc::app::SatelliteEditorParityTestAccess::check(scc_editor, opll_editor),
            "owned SCC and OPLL timers retain immediate OFF and shared instance note");
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    auto restored = original;
    restored.name = "Host restore while editor open";
    restored.layers[1].enabled = false;
    const auto compiles = Access::stateCompileCount(processor);
    MgstcAudioProcessor source;
    require(source.replacePluginState(documentFrom(restored, 0, {})), "restore source");
    const auto blob = stateBytes(source);
    processor.setStateInformation(blob.getData(), static_cast<int>(blob.getSize()));
    requireWorkingCompositeMatchesProcessor(editor.get(), processor, "host restore hydrates existing editor");
    require(Access::stateCompileCount(processor) == compiles + 1,
        "host hydration does not compile or resubmit old sound");
    const auto revision = processor.editorStateRestoreRevision();
    processor.setStateInformation("invalid", 7);
    require(processor.editorStateRestoreRevision() == revision, "invalid state does not notify UI");
    requireWorkingCompositeMatchesProcessor(editor.get(), processor, "invalid state leaves UI intact");
    const auto old_revision = processor.editorStateRestoreRevision();
    context.acknowledgeStateRestore(old_revision);
    restored.name = "Background host restore";
    require(source.replacePluginState(documentFrom(restored, 0, {})), "background restore source");
    const auto background_blob = stateBytes(source);
    std::thread restore_thread([&] {
        processor.setStateInformation(background_blob.getData(), static_cast<int>(background_blob.getSize()));
    });
    restore_thread.join();
    std::uint8_t capacity{};
    const auto background_compiles = Access::stateCompileCount(processor);
    require(!processor.replaceEditorComposite(original, true, capacity, old_revision),
        "old UI document cannot overwrite background host restore before dispatch");
    require(!processor.previewEditorComposite(original, true, capacity, old_revision),
        "old temporary preview cannot overwrite restored runtime");
    require(!context.audition().noteOn(9, 73)
        && !context.audition().noteOff(9),
        "old UI notes and cleanup cannot affect restored host runtime");
    context.acknowledgeStateRestore(processor.editorStateRestoreRevision());
    require(context.audition().noteOn(9, 73)
        && context.audition().noteOff(9),
        "hydrated editor can audition the restored runtime");
    require(Access::stateCompileCount(processor) == background_compiles,
        "stale editor submissions reject before compiling");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    requireWorkingCompositeMatchesProcessor(editor.get(), processor, "background restore hydrates on message thread");
    for (auto* child : editor->getChildren()) {
        if (auto* composite = dynamic_cast<mgstc::app::CompositeEditorComponent*>(child))
        {
            require(EditorAccess::historySize(*composite) == 1,
                "host restore resets old undo history");
            EditorAccess::editName(*composite, "New edit after restore");
            Access::notifyStateRestored(processor);
            require(EditorAccess::sound(*composite).name == "New edit after restore",
                "late duplicate restore notification preserves subsequent edit");
        }
    }
}

void testPluginEditor() {
    MgstcAudioProcessor processor;
    const auto before = stateBytes(processor);
    const auto compiles = Access::stateCompileCount(processor);
    const auto latency = processor.getLatencySamples();
    require(!processor.producesMidi(), "plugin does not produce MIDI");

    auto* editor = processor.createEditor();
    require(editor != nullptr, "createEditor");
    requireWorkingCompositeMatchesProcessor(
        editor,
        processor,
        "first open hydrates processor composite into editor working state");
    require(
        processor.copyPluginState().sound.layers.size() == 1
            && processor.copyPluginState().sound.layers[0].source
                == mgstc::engine::TimbreSource::Psg,
        "first open shows PSG x1, matching DAW MIDI");
    require(
        Access::stateCompileCount(processor) == compiles,
        "opening the editor does not compile");
    require(sameBlock(before, stateBytes(processor)), "opening the editor keeps state");
    require(
        processor.getLatencySamples() == latency,
        "opening the editor keeps latency");
    require(
        Access::blockedBackendCalls(processor) == 0,
        "opening the editor does not enumerate devices");
    require(
        Access::scopeEnabled(processor),
        "composite playback scope runs while editor is open");

    juce::Button* library_toggle = nullptr;
    juce::Button* count_tab = nullptr;
    juce::Button* focus_button = nullptr;
    juce::Button* compact_button = nullptr;
    juce::Button* file_menu = nullptr;
    bool spectrum_enabled = false;
    juce::Button* spectrum_button = nullptr;
    mgstc::app::SoundOutputSwitch* output_switch = nullptr;
    bool library_enabled = false;
    juce::Button* library_manage = nullptr;
    bool scc_hidden = false;
    bool opll_hidden = false;
    int emu_buttons{}, hardware_buttons{};
    const auto walk = [&](auto&& self, juce::Component* component) -> void {
        if (auto* control = dynamic_cast<mgstc::app::SoundOutputSwitch*>(component))
            output_switch = control;
        if (auto* button = dynamic_cast<juce::Button*>(component)) {
            const auto text = button->getButtonText();
            if (text == juce::String::fromUTF8("ライブラリ")) library_toggle = button;
            if (text == juce::String::fromUTF8("カウント編集")) count_tab = button;
            if (text == juce::String::fromUTF8("編集に集中")) focus_button = button;
            if (text == juce::String::fromUTF8("概要を縮小")) compact_button = button;
            if (text == juce::String::fromUTF8("ファイル")) file_menu = button;
            if (text == juce::String::fromUTF8("エミュ") && button->isVisible()) ++emu_buttons;
            if (text == juce::String::fromUTF8("実機") && button->isVisible()) ++hardware_buttons;
            if (text == juce::String::fromUTF8("スペアナ")) {
                require(button->isEnabled() && button->isVisible(), "shared spectrum control is available");
                spectrum_enabled = true;
                spectrum_button = button;
            }
            if (text == juce::String::fromUTF8("ライブラリ管理")) {
                require(button->isEnabled(), "shared tone library manager is available");
                library_manage = button;
                library_enabled = true;
            }
            if (text == juce::String::fromUTF8("SCC音色")) {
                require(!button->isVisible(), "independent SCC workbench button is hidden");
                scc_hidden = true;
            }
            if (text == juce::String::fromUTF8("OPLL音色")) {
                require(!button->isVisible(), "independent OPLL workbench button is hidden");
                opll_hidden = true;
            }
        }
        for (int index = 0; index < component->getNumChildComponents(); ++index) {
            self(self, component->getChildComponent(index));
        }
    };
    walk(walk, editor);
    require(spectrum_enabled, "spectrum button is present");
    require(spectrum_button && output_switch
            && spectrum_button->getRight() + UiLayout::controlGap == output_switch->getX(),
        "hidden independent workbench controls leave no gap in plugin chrome");
    require(library_enabled, "library button is present");
    require(scc_hidden, "SCC workbench button is retained but hidden");
    require(opll_hidden, "OPLL workbench button is retained but hidden");
    require(emu_buttons == 1 && hardware_buttons == 1,
        "composite view has one visible shared emulator/hardware switch");
    require(library_toggle && count_tab && focus_button, "main workspace navigation is present");
    const auto captures = juce::File::getCurrentWorkingDirectory().getChildFile("composite-ui-captures");
    require(captures.createDirectory().wasOk(), "create workspace snapshot directory");
    const auto capture = [&](const char* name) {
        auto stream = captures.getChildFile(name).createOutputStream();
        require(stream != nullptr, "open workspace snapshot");
        require(stream->setPosition(0) && stream->truncate().wasOk(), "truncate workspace snapshot");
        juce::PNGImageFormat format;
        require(format.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()), *stream),
            "render shared main screen");
    };
    capture("main-editor.png");
    require(compact_button && file_menu, "density and file menu controls are present");
    file_menu->onClick();
    juce::PopupMenu::dismissAllActiveMenus();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    compact_button->setToggleState(true, juce::dontSendNotification);
    compact_button->onClick();
    capture("main-compact.png");
    compact_button->setToggleState(false, juce::dontSendNotification);
    compact_button->onClick();
    library_toggle->setToggleState(true, juce::dontSendNotification);
    library_toggle->onClick();
    require(library_manage && library_manage->isVisible(),
        "library manager becomes visible when the library panel opens");
    capture("main-library.png");
    library_toggle->setToggleState(false, juce::dontSendNotification);
    library_toggle->onClick();
    count_tab->onClick();
    focus_button->setToggleState(true, juce::dontSendNotification);
    focus_button->onClick();
    capture("main-focus.png");
    require(sameBlock(before, stateBytes(processor)), "workspace navigation keeps processor state");
    requireWorkingCompositeMatchesProcessor(editor, processor, "workspace navigation keeps editor sound");
    require(Access::stateCompileCount(processor) == compiles, "workspace navigation does not compile");
    require(Access::blockedBackendCalls(processor) == 0, "workspace navigation does not access backend");
    delete editor;
    require(sameBlock(before, stateBytes(processor)), "closing the editor keeps state");
    require(
        Access::stateCompileCount(processor) == compiles,
        "closing the editor does not recompile");
    require(
        !Access::scopeEnabled(processor),
        "composite playback scope stops when editor closes");
    require(
        Access::blockedBackendCalls(processor) == 0,
        "closing the editor does not enumerate devices");

    MgstcAudioProcessor other;
    const auto other_before = stateBytes(other);
    {
        PluginEditorContext context(processor);
        auto timbre = processor.copyPluginState().sound;
        timbre.name = "instance-a";
        mgstc::app::CompositeAuditionRequest request;
        request.timbre = &timbre;
        request.polyphonic = true;
        const auto applied = context.audition().submitComposite(request);
        require(applied.ok, "composite edit reaches this instance");
        require(
            processor.copyPluginState().sound.name == "instance-a",
            "edited name is stored on this instance");
        require(
            sameBlock(other_before, stateBytes(other)),
            "another instance is unchanged");
        require(
            context.snapshot().capabilities.spectrum_analyzer
                && context.snapshot().capabilities.tone_library
                && context.snapshot().capabilities.waveform_scope
                && context.snapshot().capabilities.composite_playback_waveform
                && context.snapshot().capabilities.independent_scc_opll_workbench
                    == false
                && context.snapshot().capabilities.on_screen_keyboard,
            "shared plugin features are enabled while independent workbenches stay hidden");
        require(
            context.output().availableAsioDrivers().empty(),
            "plugin does not list ASIO drivers");
    }
    require(
        Access::blockedBackendCalls(processor) == 1,
        "ASIO enumeration is the rejected backend call");

    const auto edited = stateBytes(processor);
    editor = processor.createEditor();
    requireWorkingCompositeMatchesProcessor(
        editor,
        processor,
        "reopen hydrates the committed composite, not an empty default");
    require(sameBlock(edited, stateBytes(processor)), "reopen does not revert the edit");
    delete editor;

    processor.prepareToPlay(48'000.0, 512);
    const auto daw_notes = Access::noteOnCount(processor);
    {
        PluginEditorContext context(processor);
        require(context.audition().noteOn(0, 60), "editor keyboard note");
        double peak = 0.0;
        for (int block = 0; block < 8 && peak <= 0.001; ++block) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            require(midi.isEmpty(), "editor keyboard does not write host MIDI");
            peak = std::max(peak, channelPeak(buffer, 0, 512));
        }
        require(peak > 0.001, "editor keyboard is audible");
        require(
            Access::noteOnCount(processor) == daw_notes,
            "editor keyboard does not use the DAW note path");
    }

    MgstcAudioProcessor headless;
    headless.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> heard(2, 512);
    juce::MidiBuffer host_midi;
    host_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    headless.processBlock(heard, host_midi);
    double headless_peak = channelPeak(heard, 0, 512);
    if (headless_peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 2048);
        juce::MidiBuffer empty;
        headless.processBlock(more, empty);
        headless_peak = channelPeak(more, 0, 2048);
    }
    require(headless_peak > 0.001, "audio works with no editor");
}

void testCompositePlaybackWaveform() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 2048);
    require(
        !Access::scopeEnabled(processor),
        "composite playback scope is off without an editor");
    mgstc::engine::OpllScopeFrame frame{};
    require(
        !processor.editorPollCompositeScope(frame),
        "no composite waveform frames without an editor");

    auto* editor = processor.createEditor();
    require(editor != nullptr, "waveform test createEditor");
    require(
        Access::scopeEnabled(processor),
        "composite playback scope is on while the editor is open");

    bool saw_wave = false;
    for (int block = 0; block < 8 && !saw_wave; ++block) {
        juce::AudioBuffer<float> buffer(2, 2048);
        juce::MidiBuffer midi;
        if (block == 0) {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1, 60, static_cast<juce::uint8>(100)),
                0);
        }
        processor.processBlock(buffer, midi);
        while (processor.editorPollCompositeScope(frame)) {
            for (const float sample : frame.mixed_samples) {
                if (std::fabs(sample) > 1.0e-4F) {
                    saw_wave = true;
                    break;
                }
            }
            if (saw_wave) {
                break;
            }
            for (const float sample : frame.psg_samples) {
                if (std::fabs(sample) > 1.0e-4F) {
                    saw_wave = true;
                    break;
                }
            }
        }
    }
    require(saw_wave, "composite editor receives instance PCM frames");
    delete editor;
    require(
        !Access::scopeEnabled(processor),
        "composite playback scope stops when the editor closes");
    require(
        !processor.editorPollCompositeScope(frame),
        "no composite waveform frames after the editor closes");
}

void testScopeFrameDistributorReaders() {
    mgstc::app::ScopeFrameDistributor distributor;
    std::uint64_t first_cursor{}, second_cursor{}, latest_cursor{};
    int pending = 3;
    std::uint64_t sequence{};
    auto drain = [&](mgstc::engine::OpllScopeFrame& frame) {
        if (pending == 0) return false;
        --pending;
        frame.sequence = ++sequence;
        frame.mixed_samples[0] = static_cast<float>(sequence);
        return true;
    };
    mgstc::engine::OpllScopeFrame first{}, second{}, latest{};
    require(distributor.poll(first, first_cursor, drain) && first.sequence == 1,
        "first waveform view gets full frame history");
    require(distributor.pollLatest(latest, latest_cursor, drain) && latest.sequence == 3,
        "OPLL scope gets the latest frame independently");
    require(distributor.poll(second, second_cursor, drain) && second.sequence == 1,
        "second history view does not steal first view frames");
    require(!distributor.pollLatest(latest, latest_cursor, drain),
        "latest scope does not append the same captured frame twice");
    require(distributor.poll(first, first_cursor, drain) && first.sequence == 2,
        "latest scope does not consume composite waveform history");
    distributor.clear();
    require(!distributor.poll(first, first_cursor, drain),
        "released capture cannot replay stale history");
    pending = 1;
    require(distributor.poll(first, first_cursor, drain) && first.sequence == 4,
        "reopened capture starts at fresh history");
    // A producer that remains busy cannot trap the message thread in receive.
    int calls{};
    auto busy = [&](mgstc::engine::OpllScopeFrame& frame) {
        frame.sequence = static_cast<std::uint64_t>(++calls);
        return true;
    };
    require(distributor.pollLatest(latest, latest_cursor, busy)
            && calls == static_cast<int>(mgstc::app::ScopeFrameDistributor::kMaxDrainFrames),
        "scope distribution drains at most one bounded queue per poll");
}

void testProcessorOfflineAndTransport() {
    struct PlayHead final : juce::AudioPlayHead {
        PositionInfo position;
        juce::Optional<PositionInfo> getPosition() const override { return position; }
    } play_head;
    MgstcAudioProcessor processor;
    processor.setPlayHead(&play_head);
    processor.prepareToPlay(48'000.0, 512);
    processor.setNonRealtime(true);
    play_head.position.setIsPlaying(true);
    play_head.position.setTimeInSamples(0);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 128);
    processor.processBlock(buffer, midi);
    double peak = channelPeak(buffer, 0, 512);
    for (int block = 1; block < 5; ++block) {
        play_head.position.setTimeInSamples(block * 512);
        processor.processBlock(buffer, midi);
        peak = std::max(peak, channelPeak(buffer, 0, 512));
    }
    require(peak > 0.001, "non realtime host processing produces emulator PCM");
    mgstc::app::EditorSnapshot snap;
    processor.editorPopulateOutputSnapshot(snap);
    require(!snap.hardware_output && !snap.desired_hardware_output && !snap.hardware_owned,
        "offline processing cannot acquire hardware");
    play_head.position.setTimeInSamples(0); // loop/rewind
    processor.processBlock(buffer, midi);
    for (std::uint8_t track = 0; track < mgstc::plugin::LayerDelayScheduler::kPhysicalTrackCount; ++track)
        require(Access::isInactive(processor, track),
            "transport discontinuity stops old voices and delayed starts");
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
    play_head.position.setTimeInSamples(512);
    processor.processBlock(buffer, midi);
    bool active = false;
    for (std::uint8_t track = 0; track < mgstc::plugin::LayerDelayScheduler::kPhysicalTrackCount; ++track)
        active = active || Access::isActive(processor, track);
    require(active, "new notes work after transport loop");
    play_head.position.setIsPlaying(false);
    play_head.position.setTimeInSamples(1024);
    processor.processBlock(buffer, midi);
    for (std::uint8_t track = 0; track < mgstc::plugin::LayerDelayScheduler::kPhysicalTrackCount; ++track)
        require(Access::isInactive(processor, track), "DAW stop ends every active voice");
    processor.setNonRealtime(false);
    processor.processBlock(buffer, midi);
    processor.editorPopulateOutputSnapshot(snap);
    require(!snap.hardware_output && !snap.hardware_owned,
        "leaving offline processing preserves manual output selection");
    processor.setPlayHead(nullptr);
    processor.setNonRealtime(true);
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, midi);
    bool offline_note_active = false;
    for (std::uint8_t track = 0; track < mgstc::plugin::LayerDelayScheduler::kPhysicalTrackCount; ++track)
        offline_note_active = offline_note_active || Access::isActive(processor, track);
    require(offline_note_active, "offline render plays MIDI without a host playhead");
    processor.setNonRealtime(false);
    processor.processBlock(buffer, midi);
    for (std::uint8_t track = 0; track < mgstc::plugin::LayerDelayScheduler::kPhysicalTrackCount; ++track)
        require(Access::isInactive(processor, track),
            "offline to realtime edge clears old voices even without transport metadata");
}

void testLibraryPreviewsPreserveCommittedPluginState() {
    using namespace mgstc::engine;
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    processEmpty(processor, 512); // activate the initial live program before previewing
    std::unique_ptr<juce::AudioProcessorEditor> view(processor.createEditor());
    auto* plugin_editor = dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(view.get());
    require(plugin_editor != nullptr, "library preview test opens shared plugin editor");
    const auto find = [](const auto& self, juce::Component& parent)
        -> mgstc::app::CompositeEditorComponent* {
        if (auto* editor = dynamic_cast<mgstc::app::CompositeEditorComponent*>(&parent))
            return editor;
        for (int index = 0; index < parent.getNumChildComponents(); ++index)
            if (auto* found = self(self, *parent.getChildComponent(index))) return found;
        return nullptr;
    };
    auto* editor = find(find, *view);
    require(editor != nullptr, "library preview test finds shared composite component");
    const auto committed = processor.copyPluginState().sound;
    const auto saved = stateBytes(processor);
    auto preview = sccOnlyTimbre();
    preview.name = "temporary manager preview";
    require(EditorAccess::managerPreview(*editor, preview), "manager preview compiles as temporary sound");
    processEmpty(processor, 512);
    require(Access::plan(processor).audible_layers.front().source == TimbreSource::Scc,
        "manager preview reaches the audio runtime");
    require(sameBlock(saved, stateBytes(processor)), "manager preview does not enter Plugin State");
    require(sameAuthoringSound(plugin_editor->copyWorkingComposite(), committed),
        "working snapshot returns committed sound during manager preview");
    editor->libraryManagerAllNotesOff();
    processEmpty(processor, 512);
    require(Access::plan(processor).audible_layers.front().source == TimbreSource::Psg,
        "manager stop restores committed runtime without another editor action");
    ImportedToneCandidate candidate;
    candidate.type = ImportedToneType::Composite;
    candidate.default_register_as = ImportRegisterAs::Composite;
    candidate.data = preview;
    editor->auditionImportedCandidate(candidate);
    processEmpty(processor, 512);
    require(Access::plan(processor).audible_layers.front().source == TimbreSource::Scc,
        "import preview reaches the audio runtime");
    require(sameBlock(saved, stateBytes(processor)), "import candidate preview does not enter Plugin State");
    require(sameAuthoringSound(plugin_editor->copyWorkingComposite(), committed),
        "working snapshot returns committed sound during import preview");
    editor->stopImportedPreview();
    processEmpty(processor, 512);
    require(Access::plan(processor).audible_layers.front().source == TimbreSource::Psg,
        "import stop restores committed runtime without another editor action");
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, midi);
    require(Access::logTrack(processor, 0) == 0 && channelPeak(buffer, 0, 512) > 0.001,
        "DAW MIDI after preview stop audibly plays the committed PSG sound");
    require(sameAuthoringSound(processor.copyPluginState().sound, committed),
        "ending library previews restores committed sound");
}

void testProcessorHardwareRoutingWithMockRpc() {
    using namespace mgstc::engine;
    constexpr std::uint16_t port = 39118;
    std::atomic<std::uint64_t> writes{0};
    rpc::server server(port);
    server.bind(MAmidiRpcClient::kMethodName,
        [&](unsigned char device, unsigned char, unsigned int, unsigned int) {
            if (device != 0) writes.fetch_add(1, std::memory_order_release);
        });
    server.async_run(1);
    {
        MgstcAudioProcessor a, b;
        a.prepareToPlay(48'000.0, 512);
        b.prepareToPlay(48'000.0, 512);
        const auto original = stateBytes(a);
        mgstc::app::HardwareOutputSettings settings;
        settings.host = "127.0.0.1"; settings.port = port; settings.unit_no = 1;
        require(a.editorApplyHardwareSettings(settings, 20), "processor accepts hardware settings without mode change");
        settings.unit_no = 2;
        require(b.editorApplyHardwareSettings(settings, 20), "second processor configures shared RPC endpoint");
        require(a.editorRequestHardwareMode(true), "processor explicitly acquires hardware");
        processEmpty(a, 512); // finish route-edge cleanup before starting PCM preview
        auto pcm = std::make_shared<SourcePcm>();
        pcm->sample_rate = 48000; pcm->channels = 2;
        pcm->mono_samples.assign(4096, 0.25f);
        pcm->interleaved_samples.assign(8192, 0.25f);
        require(a.editorPlayPcmPreview(pcm, 0, 4096, 1.0f), "hardware owner starts comparison PCM preview");
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
        a.processBlock(buffer, midi);
        require(channelPeak(buffer, 0, 512) == 0 && channelPeak(buffer, 1, 512) == 0,
            "realtime hardware owner clears final DAW PCM including comparison preview");
        a.setNonRealtime(true);
        processEmpty(a, 512); // suppress queued generations and retire the mode edge
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto before_offline = writes.load(std::memory_order_acquire);
        midi.addEvent(juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 0);
        a.processBlock(buffer, midi);
        require(channelPeak(buffer, 0, 512) > 0.001, "offline hardware selection generates emulator PCM");
        for (int block = 0; block < 10; ++block) processEmpty(a, 512);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(writes.load(std::memory_order_acquire) == before_offline,
            "offline Processor emits no newly generated RPC commands");
        mgstc::app::EditorSnapshot snap;
        a.editorPopulateOutputSnapshot(snap);
        require(snap.hardware_output && snap.hardware_owned, "offline rendering preserves selected hardware and lease");
        require(sameBlock(original, stateBytes(a)), "hardware and offline state never enter Plugin State");
        a.setNonRealtime(false);
        processEmpty(a, 512); // physical reset independent of host transport
        require(b.editorRequestHardwareMode(true), "second Processor manually takes hardware");
        a.editorPopulateOutputSnapshot(snap);
        require(!snap.hardware_output && !snap.hardware_owned, "previous Processor returns to emulator");
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 0);
        a.processBlock(buffer, midi);
        require(channelPeak(buffer, 0, 512) > 0.001, "nonowner Processor continues normal emulator MIDI");
        {
            std::unique_ptr<juce::AudioProcessorEditor> view(b.createEditor());
        }
        b.editorPopulateOutputSnapshot(snap);
        require(snap.hardware_owned, "closing Editor retains Processor hardware ownership");
        MgstcAudioProcessor restored;
        restored.setStateInformation(original.getData(), static_cast<int>(original.getSize()));
        restored.editorPopulateOutputSnapshot(snap);
        require(!snap.hardware_output && !snap.hardware_owned, "restored project does not acquire hardware");
        require(b.editorRequestHardwareMode(false), "explicit release closes hardware safely");
        require(writes.load(std::memory_order_acquire) > 0, "realtime processor reaches mock RPC server");
    }
    server.stop();
}

void testOpllKeyoffSilenceDeadline() {
    MgstcAudioProcessor processor;
    auto sound = opllOnlyTimbre();
    sound.layers[0].start_delay_value = 0;
    require(processor.replacePluginState(documentFrom(sound, 0, {})),
        "keyoff test loads an OPLL program");
    processor.prepareToPlay(48'000.0, 512);
    Access::keyoffSeconds(processor, 1);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 128);
    processor.processBlock(buffer, midi);
    constexpr std::uint8_t opll_track = 8;
    require(Access::keyoffDue(processor, opll_track) == 48'128,
        "DAW OPLL noteoff arms the configured engine-frame deadline");
    midi.addEvent(juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(buffer, midi);
    require(Access::keyoffDue(processor, opll_track) == 0,
        "a reused OPLL voice cancels the old force-silence deadline");
    processor.editorArmOpllKeyoffSilence(opll_track);
    processor.processBlock(buffer, midi);
    require(Access::keyoffDue(processor, opll_track) != 0,
        "editor boundary arms the same processor-owned deadline");
    for (int block = 0; block < 96; ++block) processor.processBlock(buffer, midi);
    require(Access::keyoffDue(processor, opll_track) == 0,
        "OPLL force silence expires without UI timers or editor lifetime");
    processor.editorArmOpllKeyoffSilence(opll_track);
    Access::keyoffSeconds(processor, 0);
    processor.processBlock(buffer, midi);
    require(Access::keyoffDue(processor, opll_track) == 0,
        "disabled force-silence setting does not arm a deadline");
    Access::keyoffSeconds(processor, 1);
    PluginEditorContext stale_context(processor);
    stale_context.acknowledgeStateRestore(processor.editorStateRestoreRevision());
    require(processor.replacePluginState(documentFrom(sound, 0, {})),
        "restore another OPLL snapshot while the old editor still exists");
    processEmpty(processor, 512);
    stale_context.audition().armOpllKeyOffSilence(opll_track);
    processEmpty(processor, 512);
    require(Access::keyoffDue(processor, opll_track) == 0,
        "old editor destruction cannot arm a force-silence timer on restored sound");
    processor.editorArmOpllKeyoffSilence(opll_track);
    stale_context.audition().clearOpllKeyOffSilence();
    processEmpty(processor, 512);
    const auto restored_due = Access::keyoffDue(processor, opll_track);
    require(restored_due != 0, "current-generation keyoff request still works after restore");
    stale_context.audition().cancelOpllKeyOffSilence(opll_track);
    processEmpty(processor, 512);
    require(Access::keyoffDue(processor, opll_track) == restored_due,
        "old editor cannot cancel a restored sound's timer");
    stale_context.audition().clearOpllKeyOffSilence();
    processEmpty(processor, 512);
    require(Access::keyoffDue(processor, opll_track) == restored_due,
        "old editor cannot clear restored sound timers");

}

void testCommittedTimelineEditSupersedesPickerPreview() {
    using namespace mgstc::engine;
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    auto sound = sccOnlyTimbre();
    auto endpoint = *sound.layers[0].base_timbre;
    endpoint.library_id = allocateCompositeOwnedTimbreId(sound);
    endpoint.number_mode = TimbreNumberMode::Automatic;
    endpoint.manual_number.reset();
    endpoint.scc_waveform = sccWaveformToBytes(generateSccPreset(SccWavePreset::Square, SccHarmonic::One));
    sound.embedded_timbres.push_back(endpoint);
    EnvelopeEvent arrival;
    arrival.kind = EnvelopeEventKind::Timbre;
    arrival.count = 20;
    arrival.target_library_id = endpoint.library_id;
    arrival.scc_morph.enabled = true;
    arrival.scc_morph.intermediate_count = 2;
    sound.layers[0].envelope_timeline.length_counts = 40;
    sound.layers[0].timbre_automation = {arrival};
    require(processor.replacePluginState(documentFrom(sound, 0, {})), "seed morph preview state");
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {},
        [] {}, [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, sound);
    const auto title = EditorAccess::title(editor);
    require(title.contains(MGSTC_DOC_VERSION), "composite header shows the compiled version");
#if JUCE_DEBUG
    require(title.endsWith(" (Debug Build)"), "Debug title identifies its build");
#else
    require(!title.contains("Debug Build"), "Release title has no Debug suffix");
#endif
    const bool old_immediate = EditorAccess::immediate(editor);
    EditorAccess::immediate(editor, true);
    processEmpty(processor, 512);
    EditorAccess::pickerPreview(editor, sound);
    require(EditorAccess::rebuild(editor), "submit old picker preview");
    processEmpty(processor, 512);
    auto changed = sound;
    changed.layers[0].timbre_automation[0].scc_morph.curve = 0.125;
    EditorAccess::commitTimelineSound(editor, changed);
    require(!EditorAccess::hasPickerPreview(editor), "committed morph settings discard stale picker preview");
    require(EditorAccess::previewPending(editor), "committed edit schedules one-second preview");
    require(sameAuthoringSound(processor.copyPluginState().sound, EditorAccess::sound(editor)),
        "new morph authoring reaches plugin state instead of being submitted as stale temporary preview");
    require(EditorAccess::sound(editor).layers[0].timbre_automation[0].scc_morph.curve == 0.125,
        "new morph curve survives the timeline commit");
    processEmpty(processor, 512);
    EditorAccess::fireTimelinePreview(editor);
    require(EditorAccess::scheduled(editor) == 1 && EditorAccess::startsAndStopsShiftedEqually(editor, 1000.0),
        "updated morph triggers the existing one-second note schedule");
    EditorAccess::stop(editor);
    EditorAccess::immediate(editor, old_immediate);
}

void testCompositeEditorDelayedKeyOff() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    auto sound = mgstc::engine::defaultCompositeTimbre();
    sound.layers[1].start_delay_form = mgstc::engine::StartDelayForm::NoteLength;
    sound.layers[1].start_delay_value = 10; // r10 = 200 ms at tempo 120
    sound.layers[2].start_delay_form = mgstc::engine::StartDelayForm::AbsoluteTicks;
    sound.layers[2].start_delay_value = 24; // r%24 = 250 ms at tempo 120
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {},
        [] {}, [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, sound);
    require(EditorAccess::rebuild(editor),
        "submit the three-layer test program before checking voice capacity");
    // Let the audio side consume the submitted program, as a running host
    // does, so the later rebuild can reuse the retired program slot.
    juce::AudioBuffer<float> initial_audio(2, 512);
    juce::MidiBuffer initial_midi;
    processor.processBlock(initial_audio, initial_midi);

    EditorAccess::start(editor, 60);
    const auto key_on = EditorAccess::onset(editor);
    require(EditorAccess::scheduled(editor) == 3
            && EditorAccess::sounding(editor) == 1,
        "editor initially starts only the undelayed layer");
    EditorAccess::release(editor, 60, key_on + 50.0);
    require(EditorAccess::scheduled(editor) == 2
            && EditorAccess::sounding(editor) == 0
            && EditorAccess::voices(editor) == 1,
        "early release keeps delayed layers and their allocated voice");
    require(EditorAccess::startsAndStopsShiftedEqually(editor, 50.0),
        "r and r% shift key-on and key-off equally");
    EditorAccess::dispatch(editor, key_on + 199.0);
    require(EditorAccess::sounding(editor) == 0,
        "released key still waits for delayed onset");
    EditorAccess::dispatch(editor, key_on + 200.0);
    require(EditorAccess::sounding(editor) == 1,
        "r delayed layer sounds after physical key release");
    EditorAccess::dispatch(editor, key_on + 250.0);
    require(EditorAccess::scheduled(editor) == 1
            && EditorAccess::sounding(editor) == 1
            && EditorAccess::voices(editor) == 1,
        "r layer releases while r% layer starts with original held duration");
    EditorAccess::dispatch(editor, key_on + 300.0);
    require(EditorAccess::scheduled(editor) == 0
            && EditorAccess::sounding(editor) == 0
            && EditorAccess::voices(editor) == 0,
        "last delayed release frees the voice");

    EditorAccess::start(editor, 60, true);
    const auto preview_on = EditorAccess::onset(editor);
    require(EditorAccess::startsAndStopsShiftedEqually(editor, 1000.0),
        "one-second preview holds every layer for one second after its onset");
    EditorAccess::dispatch(editor, preview_on + 1000.0);
    require(EditorAccess::scheduled(editor) == 2
            && EditorAccess::sounding(editor) == 2,
        "preview ends undelayed layer before delayed layers");
    EditorAccess::dispatch(editor, preview_on + 1200.0);
    require(EditorAccess::scheduled(editor) == 1,
        "preview r layer lasts exactly one second");
    EditorAccess::dispatch(editor, preview_on + 1250.0);
    require(EditorAccess::scheduled(editor) == 0
            && EditorAccess::voices(editor) == 0,
        "preview r% layer lasts exactly one second");

    EditorAccess::setPolyphonic(editor, true);
    EditorAccess::start(editor, 60);
    EditorAccess::release(editor, 60, EditorAccess::onset(editor) + 50.0);
    EditorAccess::start(editor, 60);
    require(EditorAccess::scheduled(editor) == 3
            && EditorAccess::voices(editor) == 1,
        "same-note retrigger cancels the previous delayed release");
    EditorAccess::start(editor, 62);
    EditorAccess::start(editor, 64);
    EditorAccess::start(editor, 65);
    const auto voice_diagnostic = "voice stealing capacity: expected 3, got "
        + std::to_string(EditorAccess::voices(editor));
    require(EditorAccess::voices(editor) == 3, voice_diagnostic.c_str());
    const auto schedule_diagnostic = "voice stealing schedules: expected 9, got "
        + std::to_string(EditorAccess::scheduled(editor));
    require(EditorAccess::scheduled(editor) == 9, schedule_diagnostic.c_str());
    require(!EditorAccess::containsNote(editor, 60),
        "voice stealing leaves an old scheduled base note");
    EditorAccess::release(editor, 60, EditorAccess::onset(editor) + 10.0);
    require(EditorAccess::scheduled(editor) == 9,
        "late note-off for a stolen note cannot release its replacement");
    EditorAccess::stop(editor);
    require(EditorAccess::scheduled(editor) == 0
            && EditorAccess::sounding(editor) == 0
            && EditorAccess::voices(editor) == 0,
        "all-stop cancels sounding notes and future delayed starts/releases");

    EditorAccess::start(editor, 60);
    EditorAccess::release(editor, 60, EditorAccess::onset(editor) + 50.0);
    EditorAccess::setPolyphonic(editor, false);
    require(EditorAccess::scheduled(editor) == 0
            && EditorAccess::voices(editor) == 0,
        "mode change cancels delayed starts/releases before voice reuse");

    EditorAccess::start(editor, 60);
    EditorAccess::release(editor, 60, EditorAccess::onset(editor) + 50.0);
    require(EditorAccess::rebuild(editor), "editor rebuild succeeds");
    require(EditorAccess::scheduled(editor) == 0
            && EditorAccess::sounding(editor) == 0
            && EditorAccess::voices(editor) == 0,
        "program rebuild cancels previous delayed starts and releases");
    EditorAccess::start(editor, 62);
    require(EditorAccess::scheduled(editor) == 3
            && !EditorAccess::containsNote(editor, 60),
        "rebuilt program does not inherit an old release on a reused voice");
}

#include "vst3_pcm_preview_tests.inc"

void testConversionOwnerClose() {
    auto owner = std::make_unique<juce::Component>();
    auto progress = std::make_shared<mgstc::app::ConversionProgressState>();
    auto finish = std::make_shared<std::atomic_bool>(false);
    bool done = false, cancelled = false;
    mgstc::app::runWithConversionBusyDialog(owner.get(),
        [finish](mgstc::app::ConversionProgressState&) {
            while (!finish->load(std::memory_order_acquire)) juce::Thread::sleep(1);
            return 42;
        }, [&](int result, bool was_cancelled) {
            require(result == 42, "owner close retains worker result");
            cancelled = was_cancelled; done = true;
        }, progress);
    owner.reset();
    for (int wait = 0; wait < 100 && !progress->cancellationRequested(); ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const bool cancellation_requested = progress->cancellationRequested();
    const bool dialog_hidden = juce::Component::getNumCurrentlyModalComponents() == 0;
    const bool worker_retained = !done;
    finish->store(true, std::memory_order_release);
    for (int wait = 0; wait < 100 && !done; ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(cancellation_requested && dialog_hidden && worker_retained,
            "owner close cancels and hides dialog without waiting for worker");
    require(done && cancelled, "closed owner cannot publish successful conversion");
}
void testConversionCompletionCancellation() {
    for (const bool cancel : {false, true}) {
        auto progress = std::make_shared<mgstc::app::ConversionProgressState>();
        auto ready = std::make_shared<std::atomic_bool>(false);
        bool done = false;
        bool observed_cancel = false;
        mgstc::app::runWithConversionBusyDialog(nullptr,
            [ready](mgstc::app::ConversionProgressState&) {
                ready->store(true, std::memory_order_release);
                return 42;
            }, [&done, &observed_cancel](int result, bool cancelled) {
                require(result == 42, "busy completion result preserved");
                observed_cancel = cancelled;
                done = true;
            }, progress);
        for (int wait = 0; wait < 500 && !ready->load(std::memory_order_acquire); ++wait)
            juce::Thread::sleep(10);
        require(ready->load(), "conversion worker finishes within timeout");
        // Keep message dispatch paused until the worker has posted completion.
        juce::Thread::sleep(20);
        if (cancel) progress->requestCancellation();
        for (int wait = 0; wait < 100 && !done; ++wait)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        require(done && observed_cancel == cancel,
                "late cancellation checked before publication; normal completion remains successful");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
}
void testCompositeWavSourceImportTransactions() {
    using WavAccess = mgstc::app::CompositeWavConversionTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    const auto authoring = processor.copyPluginState().sound;
    const auto message_thread = std::this_thread::get_id();
    // PCM16, mono, 48 kHz, two explicit samples. No filesystem/Audacity fixture.
    const std::vector<std::uint8_t> wave{
        'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,128,187,0,0,0,119,1,0,2,0,16,0,'d','a','t','a',4,0,0,0,
        0,32,0,224};
    bool applied = false;
    auto content = std::make_unique<mgstc::app::CompositeWavConversionContent>(
        [&](const mgstc::engine::CompositeTimbre&, std::function<void()>) { applied = true; },
        context.pcmPreview(), context.backgroundTasks());
    auto finish = [&] {
        for (int wait = 0; wait < 250 && WavAccess::loading(*content); ++wait)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        require(!WavAccess::loading(*content), "source import worker finishes within bounded wait");
    };
    std::atomic<bool> worker_thread{false};
    WavAccess::load(*content, [&] {
        worker_thread.store(std::this_thread::get_id() != message_thread);
        return mgstc::platform::AudacityWaveImportResult{wave, {}};
    });
    finish();
    const auto loaded = WavAccess::source(*content);
    require(worker_thread.load() && loaded && loaded->mono_samples.size() == 2,
        "Audacity/file source provider and common WAVE decoder run on conversion worker");
    const auto range = WavAccess::selection(*content);
    require(range.begin == 0 && range.end == 2 && loaded->sample_rate == 48000
            && loaded->mono_samples[0] == 0.25f && loaded->mono_samples[1] == -0.25f,
        "source import populates exact PCM and full selection without conversion/apply");
    WavAccess::load(*content, [] {
        return mgstc::platform::AudacityWaveImportResult{{}, L"pipe unavailable"};
    });
    finish();
    require(WavAccess::source(*content) == loaded && WavAccess::status(*content).contains("pipe unavailable"),
        "failed Audacity acquisition retains previous valid source and reports its error");
    struct Pending { std::atomic<bool> started{false}; std::atomic<bool> released{false}; };
    auto pending = std::make_shared<Pending>();
    auto provider = [pending, wave] {
        pending->started.store(true);
        for (int wait = 0; wait < 5000 && !pending->released.load(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return mgstc::platform::AudacityWaveImportResult{wave, {}};
    };
    WavAccess::load(*content, provider);
    for (int wait = 0; wait < 100 && !pending->started.load(); ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    WavAccess::cancelLoad(*content);
    pending->released.store(true);
    finish();
    require(WavAccess::source(*content) == loaded, "cancelled source acquisition does not publish PCM");
    pending = std::make_shared<Pending>();
    WavAccess::load(*content, [pending, wave] {
        pending->started.store(true);
        for (int wait = 0; wait < 5000 && !pending->released.load(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return mgstc::platform::AudacityWaveImportResult{wave, {}};
    });
    for (int wait = 0; wait < 100 && !pending->started.load(); ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    content.reset();
    pending->released.store(true);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    require(!applied && sameAuthoringSound(processor.copyPluginState().sound, authoring),
        "source inspection/failure/cancellation/editor close never applies or changes VST3 authoring state");
}

void testCompositeWavUiTransactions() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    using WavAccess = mgstc::app::CompositeWavConversionTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    auto original = mgstc::engine::defaultCompositeTimbre();
    original.name = "Full original";
    original.tags = {"Preserved"};
    original.favorite = true;
    require(processor.replacePluginState(documentFrom(original, 0, {})),
        "seed VST3 state with the editor's initial authoring snapshot");
    // A Live commit stays unacknowledged until the host processes an audio block.
    processEmpty(processor, 512);
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {}, [](std::uint8_t) {},
        [](juce::Component*) {}, [] {}, original);
    // Let editor hydration and its posted text-control notifications settle
    // before taking the authoring-state baseline for this transaction test.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto before = EditorAccess::sound(editor);
    require(sameAuthoringSound(processor.copyPluginState().sound, before),
        "editor baseline begins from the committed VST3 Plugin State");
    auto converted = sccOnlyTimbre(); converted.name = "WAV result";
    const auto history_before = EditorAccess::historySize(editor);
    require(EditorAccess::wavEnabled(editor), "VST shared editor enables WAV conversion button");
    require(EditorAccess::hasWaveConversionCaption(editor), "shared editor uses WAV conversion caption");
    require(context.pcmPreview() != nullptr, "VST converter uses host PCM boundary");
    {
        mgstc::app::CompositeWavConversionContent content(
            [&](const mgstc::engine::CompositeTimbre&, std::function<void()>) { throw std::runtime_error("Apply must be explicit"); }, context.pcmPreview(), context.backgroundTasks());
        auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
        pcm->sample_rate = 48000; pcm->channels = 2;
        pcm->mono_samples.assign(128, 0.1f); pcm->interleaved_samples.assign(256, 0.1f);
        WavAccess::source(content, pcm);
        const auto original_pcm = pcm->interleaved_samples;
        auto analysis = std::make_shared<mgstc::engine::SourceAnalysis>();
        analysis->source = pcm; analysis->selection = {0, 128}; analysis->reference_pitch_hz = 440.0;
        WavAccess::seedAnalysis(content, analysis);
        require(WavAccess::analysisMatches(content), "analysis cache matches source, selection and automatic F0 input");
        WavAccess::setConfiguration(content, 2);
        WavAccess::setKeyOff(content, "0.001");
        require(WavAccess::analysis(content) == analysis && WavAccess::analysisMatches(content),
            "conversion options and key-off override reuse immutable source analysis");
        WavAccess::setPitch(content, "441");
        require(!WavAccess::analysis(content) && !WavAccess::analysisMatches(content),
            "changing reference F0 invalidates analysis before conversion");
        WavAccess::setPitch(content, "auto");
        WavAccess::seedAnalysis(content, analysis);
        WavAccess::setSelection(content, 8, 120);
        require(!WavAccess::analysis(content), "changing selected source frames invalidates analysis cache");
        WavAccess::setSelection(content, 0, 128);
        require(pcm->interleaved_samples == original_pcm, "inspection and control changes never mutate original preview PCM");
        mgstc::engine::CompositeWavConversionResult valid;
        valid.completion = mgstc::engine::CompositeWavConversionCompletion::Completed;
        valid.composite_tone = converted;
        require(WavAccess::accept(content, valid, pcm), "UI accepts completed conversion");
        require(WavAccess::matchOn(content), "comparison level matching defaults ON");
        require(!WavAccess::accept(content, {}, {}), "failed reconversion rejected");
        require(WavAccess::result(content)->composite_tone == converted, "failed reconversion retains old full tone");
        require(!WavAccess::accept(content, valid, pcm, true), "cancelled reconversion rejected");
        require(WavAccess::result(content)->composite_tone == converted, "cancelled reconversion retains old full tone");
        const auto old_scale = UiScale::active_percent;
        for (const int scale : {75, 100, 125}) {
            UiScale::setActivePercent(scale);
            content.setSize(UiScale::sx(900), UiScale::sx(900)); content.resized();
            require(WavAccess::buttonsInside(content), "converter minimum layout preserves comparison and result controls");
            require(WavAccess::hasAudacityBesideLoad(content), "Audacity conversion sits beside WAV open inside morph window at every UI scale");
            auto image = content.createComponentSnapshot(content.getLocalBounds());
            const auto folder = juce::File::getCurrentWorkingDirectory().getChildFile("build/composite-wav-ui-captures");
            require(folder.createDirectory().wasOk(), "converter capture directory");
            const auto stream = folder.getChildFile("converter-" + juce::String(scale) + ".png").createOutputStream();
            require(stream && stream->openedOk(), "converter capture output");
            stream->setPosition(0); stream->truncate();
            require(juce::PNGImageFormat().writeImageToStream(image, *stream), "converter UI capture PNG");
        }
        UiScale::setActivePercent(old_scale);
    }
    require(sameAuthoringSound(EditorAccess::sound(editor), before) && EditorAccess::historySize(editor) == history_before
            && sameAuthoringSound(processor.copyPluginState().sound, before),
        "closing converter without Apply preserves complete authoring state and history");
    require(EditorAccess::matches(editor, before), "unchanged conversion snapshot accepted");
    EditorAccess::editName(editor, "Changed while converting");
    processEmpty(processor, 512);
    require(!EditorAccess::matches(editor, before), "stale editor snapshot requires confirmation");
    EditorAccess::editName(editor, before.name);
    processEmpty(processor, 512);
    EditorAccess::applyWave(editor, converted);
    require(sameAuthoringSound(EditorAccess::sound(editor), converted) && EditorAccess::sound(editor).layers.size() == 1,
        "WAV Apply fully replaces all old channels");
    require(sameAuthoringSound(processor.copyPluginState().sound, converted),
        "shared editor Apply publishes converted sound into VST3 Plugin State");
    processEmpty(processor, 512);
    require(EditorAccess::historySize(editor) == history_before + 1, "WAV replacement is one history operation");
    EditorAccess::undo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), before), "Undo restores all previous authoring data");
    require(sameAuthoringSound(processor.copyPluginState().sound, before),
        "Undo republishes the previous complete sound into VST3 Plugin State");
    processEmpty(processor, 512);
    EditorAccess::redo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), converted), "Redo restores entire converted data");
    require(sameAuthoringSound(processor.copyPluginState().sound, converted),
        "Redo republishes the converted sound into VST3 Plugin State");
    processEmpty(processor, 512);
}

void testCompositeStepSlideVstTransactions() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);

    auto original = mgstc::engine::defaultCompositeTimbre();
    auto& edited_layer = original.layers.front();
    edited_layer.envelope_timeline.length_counts = 24;
    edited_layer.envelope_timeline.loop_start_count = 6;
    edited_layer.envelope_timeline.loop_end_count = 18;
    edited_layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
    edited_layer.volume_envelope.events = {
        {mgstc::engine::EnvelopeEventKind::Volume, 15, 0, 0},
        {mgstc::engine::EnvelopeEventKind::Volume, 9, 0, 6},
        {mgstc::engine::EnvelopeEventKind::Volume, 3, 0, 12}};
    edited_layer.pitch_envelope.events.clear();
    mgstc::engine::EnvelopeEvent pitch;
    pitch.kind = mgstc::engine::EnvelopeEventKind::Pitch;
    pitch.value = -3;
    pitch.count = 10;
    edited_layer.pitch_envelope.events.push_back(pitch);
    require(processor.replacePluginState(documentFrom(original, 0, {})),
        "seed VST step-slide transaction state");
    processEmpty(processor, 512);

    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {},
        [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, original);
    EditorAccess::size(editor, 1600, 1050);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    std::function<juce::TextButton*(juce::Component&, const juce::String&)>
        findButton = [&](juce::Component& root,
                         const juce::String& text) -> juce::TextButton* {
        for (int index = 0; index < root.getNumChildComponents(); ++index) {
            auto* child = root.getChildComponent(index);
            if (auto* button = dynamic_cast<juce::TextButton*>(child);
                button != nullptr && button->getButtonText() == text)
                return button;
            if (auto* nested = findButton(*child, text)) return nested;
        }
        return nullptr;
    };
    const auto findHorizontalScrollBar = [](juce::Component& root) -> juce::ScrollBar* {
        for (int index = 0; index < root.getNumChildComponents(); ++index) {
            auto* child = root.getChildComponent(index);
            if (auto* bar = dynamic_cast<juce::ScrollBar*>(child);
                bar != nullptr && !bar->isVertical()) return bar;
        }
        return nullptr;
    };
    const auto findVerticalScrollBar = [](juce::Component& root) -> juce::ScrollBar* {
        for (int index = 0; index < root.getNumChildComponents(); ++index) {
            auto* child = root.getChildComponent(index);
            if (auto* bar = dynamic_cast<juce::ScrollBar*>(child);
                bar != nullptr && bar->isVertical()) return bar;
        }
        return nullptr;
    };
    auto* step_button = findButton(editor,
        juce::String::fromUTF8("削除/挿入"));
    require(step_button != nullptr && step_button->isVisible(),
        "VST shared editor exposes the step edit toggle");
    auto* timeline_impl = step_button->getParentComponent();
    require(timeline_impl != nullptr,
        "step edit toggle is owned by the shared timeline implementation");
    auto* horizontal_scroll = findHorizontalScrollBar(*timeline_impl);
    require(horizontal_scroll != nullptr,
        "shared timeline exposes its horizontal count range");
    auto* vertical_scroll = findVerticalScrollBar(*timeline_impl);
    require(vertical_scroll != nullptr,
        "shared timeline exposes its graph boundary");
    require(!step_button->getToggleState(),
        "step edit mode defaults off in the VST editor");

    const auto initial_state = stateBytes(processor);
    const auto initial_sound = EditorAccess::sound(editor);
    const auto initial_history = EditorAccess::historySize(editor);
    const auto initial_cursor = EditorAccess::historyCursor(editor);
    step_button->setToggleState(true, juce::dontSendNotification);
    step_button->onClick();
    require(step_button->getToggleState()
            && sameAuthoringSound(EditorAccess::sound(editor), initial_sound)
            && EditorAccess::historySize(editor) == initial_history
            && sameBlock(initial_state, stateBytes(processor)),
        "toggling step edit mode changes no authoring data, history, or Plugin State");
    step_button->setToggleState(false, juce::dontSendNotification);
    step_button->onClick();
    require(!step_button->getToggleState()
            && EditorAccess::historyCursor(editor) == initial_cursor,
        "step edit mode can return to off without an authoring transaction");
    step_button->setToggleState(true, juce::dontSendNotification);
    step_button->onClick();
    require(step_button->getToggleState()
            && sameBlock(initial_state, stateBytes(processor))
            && sameAuthoringSound(EditorAccess::sound(editor), initial_sound),
        "step edit mode can be enabled without changing authoring data");

    const auto pointFor = [&](int count, int lane_y) {
        const auto bounds = step_button->getBounds();
        const int graph_x = bounds.getX() - UiLayout::textButtonH
            - UiLayout::controlGap * 2 - UiLayout::xs;
        const int plot_x = graph_x + UiLayout::compositeOverviewLabelW;
        const int plot_width = vertical_scroll->getX() - plot_x;
        const double start = horizontal_scroll->getCurrentRangeStart();
        const double visible = juce::jmax(1.0,
            horizontal_scroll->getCurrentRangeSize());
        const auto x_for = [&](int at) {
            return plot_x + juce::roundToInt((at - start) * plot_width / visible);
        };
        return juce::Point<int>((x_for(count) + x_for(count + 1)) / 2, lane_y);
    };
    const auto eventAt = [&](juce::Component* target, juce::Point<int> point,
                             juce::Point<int> down, bool ctrl, bool dragged) {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(
            juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                | (ctrl ? juce::ModifierKeys::ctrlModifier : 0)),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, target, target, now,
            down.toFloat(), now, 1, dragged);
    };
    const auto drag = [&](juce::Point<int> from, juce::Point<int> to,
                          bool ctrl, bool commit) {
        timeline_impl->mouseDown(eventAt(timeline_impl, from, from, ctrl, false));
        timeline_impl->mouseDrag(eventAt(timeline_impl, to, from, ctrl, true));
        if (!commit) return;
        timeline_impl->mouseUp(eventAt(timeline_impl, to, from, ctrl, true));
    };

    const auto command_y = step_button->getBounds().getCentreY();
    const auto command_from = pointFor(6, command_y);
    const auto command_to = pointFor(8, command_y);
    const auto untouched_layer = initial_sound.layers.at(1);
    drag(command_from, command_to, false, false);
    require(sameAuthoringSound(EditorAccess::sound(editor), initial_sound)
            && sameBlock(initial_state, stateBytes(processor))
            && EditorAccess::historySize(editor) == initial_history,
        "step slide candidate remains local to the timeline until mouse-up");
    timeline_impl->mouseUp(eventAt(timeline_impl, command_to, command_from,
        false, true));
    // Exercise this as a running host: each Live edit consumes a bounded
    // program slot until the next audio block retires it.
    processEmpty(processor, 512);
    const auto inserted_state = stateBytes(processor);
    const auto inserted = EditorAccess::sound(editor);
    require(inserted.layers[0].envelope_timeline.length_counts
                == initial_sound.layers[0].envelope_timeline.length_counts + 2
            && inserted.layers[0].envelope_timeline.loop_start_count == 8
            && inserted.layers[0].envelope_timeline.loop_end_count == 20
            && inserted.layers[0].pitch_envelope.events.front().count == 12
            && inserted.layers[0].volume_envelope.events.back().count == 14
            && inserted.layers[1] == untouched_layer,
        "命令 row commits a two-step slide for only the selected channel, including END and loop");
    require(EditorAccess::historySize(editor) == initial_history + 1
            && processor.copyPluginState().sound == EditorAccess::sound(editor)
            && !sameBlock(initial_state, inserted_state),
        "one completed 命令-row slide publishes one VST Plugin State transaction");
    EditorAccess::undo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), initial_sound)
            && sameBlock(initial_state, stateBytes(processor)),
        "Undo restores the entire pre-slide VST sound and state");
    EditorAccess::redo(editor);
    processEmpty(processor, 512);
    const auto redone_parent_sound = EditorAccess::sound(editor);
    const auto redone_processor_sound = processor.copyPluginState().sound;
    const auto redone_state = stateBytes(processor);
    const auto describeLayer = [](const mgstc::engine::CompositeTimbre& sound,
                                  std::size_t index) {
        if (index >= sound.layers.size()) return std::string("<missing>");
        const auto& layer = sound.layers[index];
        std::string description = "END="
            + std::to_string(layer.envelope_timeline.length_counts)
            + " loop="
            + std::to_string(layer.envelope_timeline.loop_start_count.value_or(~0u))
            + "," + std::to_string(layer.envelope_timeline.loop_end_count.value_or(~0u))
            + " volume=";
        for (const auto& event : layer.volume_envelope.events)
            description += std::to_string(event.count) + ":"
                + std::to_string(event.value) + ",";
        description += " pitch=";
        for (const auto& event : layer.pitch_envelope.events)
            description += std::to_string(event.count) + ":"
                + std::to_string(event.value) + ",";
        return description;
    };
    const auto firstDifferentLayer = [](const mgstc::engine::CompositeTimbre& a,
                                        const mgstc::engine::CompositeTimbre& b) {
        const auto common = std::min(a.layers.size(), b.layers.size());
        for (std::size_t index = 0; index < common; ++index)
            if (a.layers[index] != b.layers[index]) return index;
        return common;
    };
    const bool parent_sound_matches = redone_parent_sound == inserted;
    const bool processor_sound_matches = redone_processor_sound == inserted;
    const bool state_matches = sameBlock(inserted_state, redone_state);
    if (!parent_sound_matches || !processor_sound_matches || !state_matches) {
        const auto parent_layer = firstDifferentLayer(inserted, redone_parent_sound);
        const auto processor_layer = firstDifferentLayer(inserted, redone_processor_sound);
        std::fprintf(stderr,
            "step-slide redo diagnostic: parent=%d processor=%d bytes=%d "
            "history=%zu/%zu stateBytes=%zu/%zu parentLayer[%zu]=[%s] expected=[%s] "
            "processorLayer[%zu]=[%s] expected=[%s]\n",
            parent_sound_matches, processor_sound_matches, state_matches,
            EditorAccess::historyCursor(editor), EditorAccess::historySize(editor),
            redone_state.getSize(), inserted_state.getSize(), parent_layer,
            describeLayer(redone_parent_sound, parent_layer).c_str(),
            describeLayer(inserted, parent_layer).c_str(), processor_layer,
            describeLayer(redone_processor_sound, processor_layer).c_str(),
            describeLayer(inserted, processor_layer).c_str());
    }
    require(parent_sound_matches,
        "Redo restores the slid sound in the shared editor history");
    require(processor_sound_matches,
        "Redo republishes the slid sound into VST Plugin State");
    require(state_matches,
        "Redo restores the serialized VST Plugin State snapshot");
    step_button->setToggleState(false, juce::dontSendNotification);
    step_button->onClick();
    require(!step_button->getToggleState(),
        "Ctrl-drag coverage starts with step edit mode off");

    const auto command_top = step_button->getBounds().getY() - UiLayout::xs;
    const auto source = inserted.layers[0].source;
    const int marker_lanes = source == mgstc::engine::TimbreSource::Scc ? 1 : 2;
    const int volume_y = command_top + UiLayout::commandSummaryH
        + marker_lanes * UiLayout::editMarkerLaneH
        + UiLayout::editVolumeLaneH / 2;
    const auto volume_from = pointFor(6, volume_y);
    const auto volume_to = pointFor(8, volume_y);
    const auto before_cancel = stateBytes(processor);
    const auto cancel_sound = EditorAccess::sound(editor);
    const auto cancel_history = EditorAccess::historySize(editor);
    drag(volume_from, volume_to, true, false);
    require(sameAuthoringSound(EditorAccess::sound(editor), cancel_sound)
            && sameBlock(before_cancel, stateBytes(processor))
            && EditorAccess::historySize(editor) == cancel_history,
        "Ctrl-drag works with mode off while its individual-row candidate remains local");
    require(timeline_impl->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)),
        "Escape is consumed while cancelling a provisional step slide");
    timeline_impl->mouseUp(eventAt(timeline_impl, volume_to, volume_from,
        true, true));
    require(sameAuthoringSound(EditorAccess::sound(editor), cancel_sound)
            && sameBlock(before_cancel, stateBytes(processor))
            && EditorAccess::historySize(editor) == cancel_history,
        "Escape cancels Ctrl-drag without changing VST state or history");

    drag(volume_from, volume_to, true, true);
    processEmpty(processor, 512);
    const auto volume_edited = EditorAccess::sound(editor);
    require(volume_edited.layers[0].envelope_timeline
                == cancel_sound.layers[0].envelope_timeline
            && volume_edited.layers[0].pitch_envelope
                == cancel_sound.layers[0].pitch_envelope
            && volume_edited.layers[0].volume_envelope.events
                != cancel_sound.layers[0].volume_envelope.events
            && volume_edited.layers[1] == cancel_sound.layers[1]
            && EditorAccess::historySize(editor) == cancel_history + 1
            && sameAuthoringSound(processor.copyPluginState().sound, volume_edited),
        "Ctrl-drag commits one individual-row edit without moving END, pitch, or another channel");

    MgstcAudioProcessor restored;
    restored.prepareToPlay(48'000.0, 512);
    const auto saved_document = processor.copyPluginState();
    require(restored.replacePluginState(saved_document),
        "saved VST Plugin State accepts a step-edited sound");
    mgstc::plugin::PluginEditorContext restored_context(restored);
    mgstc::app::EditorLink restored_link;
    mgstc::app::CompositeEditorComponent reopened(restored_context, restored_link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {},
        [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, saved_document.sound);
    EditorAccess::size(reopened, 1600, 1050);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(sameAuthoringSound(EditorAccess::sound(reopened), volume_edited)
            && sameAuthoringSound(restored.copyPluginState().sound, volume_edited),
        "VST Plugin State and a recreated shared editor preserve step-slide results");
}

void testCompositeLoopVstTransactions() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);

    auto original = mgstc::engine::defaultCompositeTimbre();
    original.layers = {opllOnlyTimbre().layers.front()};
    auto& layer = original.layers.front();
    layer.envelope_timeline.length_counts = 24;
    layer.envelope_timeline.loop_start_count.reset();
    layer.envelope_timeline.loop_end_count.reset();
    layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
    using Event = mgstc::engine::EnvelopeEvent;
    using Kind = mgstc::engine::EnvelopeEventKind;
    const auto pitch = [](std::uint32_t count, std::int32_t value, bool after,
                          std::uint32_t order = 0) {
        Event event;
        event.kind = Kind::Pitch;
        event.count = count;
        event.value = value;
        event.after_loop_start = after;
        event.command_order = order;
        return event;
    };
    const auto write = [](std::uint32_t count, std::int32_t reg,
                          std::int32_t data, bool after,
                          std::uint32_t order = 0) {
        Event event;
        event.kind = Kind::RegisterWrite;
        event.count = count;
        event.value = reg;
        event.secondary = data;
        event.after_loop_start = after;
        event.command_order = order;
        return event;
    };
    layer.pitch_envelope.events = {
        pitch(4, -2, false), pitch(4, 3, true),
        pitch(10, 5, false), pitch(10, -7, true),
    };
    layer.timbre_automation = {
        write(4, 0x20, 0x11, false), write(4, 0x20, 0x12, true),
        write(10, 0x20, 0x21, false), write(10, 0x20, 0x22, true),
    };

    require(processor.replacePluginState(documentFrom(original, 0, {})),
        "seed VST loop transaction state");
    processEmpty(processor, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {},
        [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, original);
    EditorAccess::size(editor, 1600, 1050);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    const auto no_loop = EditorAccess::sound(editor);
    const auto no_loop_state = stateBytes(processor);
    const auto history_before = EditorAccess::historySize(editor);
    require(EditorAccess::setLoopStart(editor, 0, 4),
        "VST shared timeline accepts a loop start at an authored step");
    processEmpty(processor, 512);
    const auto started = EditorAccess::sound(editor);
    const auto start_state = stateBytes(processor);
    require(started.layers[0].envelope_timeline.loop_start_count == 4
            && started.layers[0].envelope_timeline.loop_end_count == 24,
        "setting the first loop start defaults the end marker to END");
    require(EditorAccess::historySize(editor) == history_before + 1
            && sameAuthoringSound(processor.copyPluginState().sound, started)
            && !sameBlock(no_loop_state, start_state),
        "loop start is one committed VST authoring and Plugin State transaction");
    EditorAccess::undo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), no_loop)
            && sameAuthoringSound(processor.copyPluginState().sound, no_loop)
            && sameBlock(no_loop_state, stateBytes(processor)),
        "Undo restores the complete pre-loop VST sound and serialized state");
    EditorAccess::redo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), started)
            && sameAuthoringSound(processor.copyPluginState().sound, started)
            && sameBlock(start_state, stateBytes(processor)),
        "Redo restores the auto-END loop start and serialized VST state");
    const auto history_after_start = EditorAccess::historySize(editor);

    require(EditorAccess::setLoopStart(editor, 0, 10),
        "VST shared timeline accepts moving the loop start");
    processEmpty(processor, 512);
    const auto moved = EditorAccess::sound(editor);
    const auto moved_state = stateBytes(processor);
    require(moved.layers[0].envelope_timeline.loop_start_count == 10
            && moved.layers[0].envelope_timeline.loop_end_count == 24,
        "moving loop start preserves the existing END marker");
    require(moved.layers[0].pitch_envelope.events == std::vector<Event>{
                pitch(4, -2, false, 2), pitch(4, 3, false, 4),
                pitch(10, 5, true, 2), pitch(10, -7, true, 4)}
            && moved.layers[0].timbre_automation == std::vector<Event>{
                write(4, 0x20, 0x11, false, 1), write(4, 0x20, 0x12, false, 3),
                write(10, 0x20, 0x21, true, 1), write(10, 0x20, 0x22, true, 3)},
        "moving the start puts every destination pitch and y command inside without merging or reordering");
    require(sameAuthoringSound(processor.copyPluginState().sound, moved),
        "moved loop and command zones are published to the VST processor");
    require(EditorAccess::historySize(editor) == history_after_start + 1,
        "moving the loop start is one VST Undo transaction");
    EditorAccess::undo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), started)
            && sameAuthoringSound(processor.copyPluginState().sound, started)
            && sameBlock(start_state, stateBytes(processor)),
        "Undo restores the prior loop boundary and destination command zones");
    EditorAccess::redo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), moved)
            && sameAuthoringSound(processor.copyPluginState().sound, moved)
            && sameBlock(moved_state, stateBytes(processor)),
        "Redo restores the moved boundary and exact command order");
    const auto history_after_move = EditorAccess::historySize(editor);

    EditorAccess::clearLoop(editor, 0);
    processEmpty(processor, 512);
    const auto cleared = EditorAccess::sound(editor);
    const auto cleared_state = stateBytes(processor);
    require(!cleared.layers[0].envelope_timeline.loop_start_count
            && !cleared.layers[0].envelope_timeline.loop_end_count,
        "clearing the loop removes both markers");
    require(cleared.layers[0].pitch_envelope.events == std::vector<Event>{
                pitch(4, -2, false, 2), pitch(4, 3, false, 4),
                pitch(10, 5, false, 2), pitch(10, -7, false, 4)}
            && cleared.layers[0].timbre_automation == std::vector<Event>{
                write(4, 0x20, 0x11, false, 1), write(4, 0x20, 0x12, false, 3),
                write(10, 0x20, 0x21, false, 1), write(10, 0x20, 0x22, false, 3)},
        "clearing the loop flattens before/after in order and preserves relative pitch and y commands");
    require(sameAuthoringSound(processor.copyPluginState().sound, cleared),
        "cleared loop and flattened commands are committed to VST Plugin State");
    require(EditorAccess::historySize(editor) == history_after_move + 1,
        "clearing the loop is one VST Undo transaction");
    EditorAccess::undo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), moved)
            && sameAuthoringSound(processor.copyPluginState().sound, moved)
            && sameBlock(moved_state, stateBytes(processor)),
        "Undo restores the loop and its ordered command zones after clear");
    EditorAccess::redo(editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(EditorAccess::sound(editor), cleared)
            && sameAuthoringSound(processor.copyPluginState().sound, cleared)
            && sameBlock(cleared_state, stateBytes(processor)),
        "Redo restores the flattened no-loop VST state");

    MgstcAudioProcessor restored;
    restored.prepareToPlay(48'000.0, 512);
    restored.setStateInformation(cleared_state.getData(),
        static_cast<int>(cleared_state.getSize()));
    processEmpty(restored, 512);
    require(sameAuthoringSound(restored.copyPluginState().sound, cleared),
        "VST state save/restore preserves flattened pitch and y command order");
    std::unique_ptr<juce::AudioProcessorEditor> reopened(restored.createEditor());
    require(reopened != nullptr, "VST editor recreates after loop state restore");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(sameAuthoringSound(restored.copyPluginState().sound, cleared),
        "reopening the VST editor keeps the restored loop-cleared author state");
    requireWorkingCompositeMatchesProcessor(reopened.get(), restored,
        "reopened VST editor hydrates the restored loop-cleared author state");
}

void testPhaseIncompatibleModulationLoopRunsInVst() {
    using namespace mgstc::engine;
    using Access = mgstc::plugin::MgstcAudioProcessorTestAccess;

    auto sound = defaultCompositeTimbre();
    for (auto& layer : sound.layers) layer.enabled = false;
    auto& layer = sound.layers[2];
    layer.enabled = true;
    // defaultCompositeTimbre() intentionally leaves the OPLL base unset.
    // Give the audio assertion an explicit audible ROM patch instead of
    // relying on the emulator's reset patch state.
    layer.base_opll_rom = std::uint8_t{4};
    layer.base_timbre.reset();
    layer.volume = 15;
    layer.start_delay_value = 0;
    layer.opll_sustain = true;
    layer.envelope_timeline = {
        .length_counts = 3,
        .loop_start_count = 0,
        .loop_end_count = 2,
    };
    layer.pitch_modulation = {
        .enabled = true,
        .mode = ModulationMode::StepUp,
        .delay = 0,
        .depth = 8,
        .speed = 0,
        .roughness = 1,
    };
    layer.volume_modulation = {
        .enabled = true,
        .mode = ModulationMode::StepDown,
        .delay = 0,
        .depth = 8,
        .speed = 0,
        .roughness = 1,
    };
    require(!compositeModulationLoopRepresentable(layer),
        "fixture exercises modulation whose later loop phase differs");

    const auto formatted = formatMgsCompositeEnvelope(layer, 0);
    require(formatted.valid() && formatted.body.find('[') != std::string::npos
            && formatted.body.find(']') != std::string::npos,
        "MGSC preview formats a phase-incompatible authored loop");

    const TimbreNumberResolution numbers{};
    const auto sampleCommands = [&](CompositeEnvelopeLane lane_kind,
                                    MeaningEventKind command_kind) {
        SequenceEnvelopeRuntime runtime(compileCompositeEnvelopeLane(
            layer, lane_kind, numbers));
        runtime.resetForKeyOn(12);
        EventBuffer events(32);
        std::vector<std::vector<std::int32_t>> commands;
        for (int tick = 0; tick < 24; ++tick) {
            events.clear();
            require(runtime.processTick(events) == SequenceError::None,
                "phase-incompatible loop command stream executes");
            std::vector<std::int32_t> current;
            for (const auto& event : events.events()) {
                if (event.kind == command_kind) current.push_back(event.arg0);
            }
            commands.push_back(std::move(current));
        }
        return commands;
    };
    const auto pitch_commands = sampleCommands(
        CompositeEnvelopeLane::Pitch, MeaningEventKind::FrequencyDelta);
    const auto volume_commands = sampleCommands(
        CompositeEnvelopeLane::Volume, MeaningEventKind::Volume);
    require(pitch_commands[0] == pitch_commands[2]
            && pitch_commands[1] == pitch_commands[3]
            && volume_commands[0] == volume_commands[2]
            && volume_commands[1] == volume_commands[3],
        "the first loop's pitch and absolute tremolo commands repeat in the next pass");
    const auto pitchDelta = [](const auto& commands, std::size_t begin,
                               std::size_t end) {
        std::int64_t sum = 0;
        for (auto tick = begin; tick < end; ++tick)
            for (const auto value : commands[tick]) sum += value;
        return sum;
    };
    const auto first_pass_delta = pitchDelta(pitch_commands, 0, 2);
    const auto second_pass_delta = pitchDelta(pitch_commands, 2, 4);
    const auto accumulated_after_first = first_pass_delta;
    const auto accumulated_after_second = first_pass_delta + second_pass_delta;
    require(first_pass_delta > 0 && second_pass_delta == first_pass_delta,
        "repeated relative Pitch commands add the same positive drift each loop pass");
    require(accumulated_after_second > accumulated_after_first,
        "relative Pitch frequency state accumulates across repeated loop passes");

    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto compiles_before = Access::stateCompileCount(processor);
    require(processor.replacePluginState(documentFrom(sound, 0, {})),
        "VST accepts a phase-incompatible modulation loop for audition");
    require(Access::stateCompileCount(processor) == compiles_before + 1,
        "VST submits the edited loop through the shared composite compiler");
    processEmpty(processor, 512);
    require(Access::plan(processor).audible_layers.size() == 1,
        "VST active program contains the looped OPLL layer");
    juce::AudioBuffer<float> audio(2, 2048);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(
        1, 60, static_cast<juce::uint8>(100)), 0);
    processor.processBlock(audio, midi);
    require(channelPeak(audio, 0, audio.getNumSamples()) > 0.001,
        "VST produces audio from the phase-incompatible loop");
}

void testVstOpllStepPickerCreatesAndOpensOriginalTimbre() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    using Source = mgstc::engine::TimbreSource;
    using Kind = mgstc::engine::EnvelopeEventKind;

    auto sound = opllOnlyTimbre();
    auto& layer = sound.layers.front();
    layer.envelope_timeline.length_counts = 8;
    layer.timbre_automation.clear();
    layer.base_timbre.reset();
    layer.base_opll_rom = 4;

    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    require(processor.replacePluginState(documentFrom(sound, 0, {})),
        "seed an OPLL ROM base with an empty step @ lane");
    processEmpty(processor, 512);

    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    std::optional<std::uint64_t> opened_timbre;
    juce::String opened_context;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [&](const juce::String& scope, std::optional<std::uint64_t> id) {
            opened_context = scope;
            opened_timbre = id;
        }, [] {}, [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, sound);
    EditorAccess::size(editor, 1600, 1050);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    std::function<juce::Component*(juce::Component&, const juce::String&)>
        findComponentWithText = [&](juce::Component& root,
                                    const juce::String& text) -> juce::Component* {
        if (auto* button = dynamic_cast<juce::Button*>(&root);
            button != nullptr && button->getButtonText() == text) {
            return &root;
        }
        if (auto* combo = dynamic_cast<juce::ComboBox*>(&root)) {
            for (int id = 1; id <= combo->getNumItems(); ++id) {
                if (combo->getItemText(id - 1) == text) return combo;
            }
        }
        for (int index = 0; index < root.getNumChildComponents(); ++index) {
            if (auto* found = findComponentWithText(*root.getChildComponent(index), text))
                return found;
        }
        return nullptr;
    };
    auto* parameter = dynamic_cast<juce::ComboBox*>(
        findComponentWithText(editor, juce::String::fromUTF8("@音色")));
    auto* step_button = dynamic_cast<juce::TextButton*>(
        findComponentWithText(editor, juce::String::fromUTF8("削除/挿入")));
    require(parameter != nullptr && step_button != nullptr,
        "VST shared editor exposes the OPLL @ lane and timeline controls");

    auto* timeline_impl = step_button->getParentComponent();
    const auto findScrollBar = [](juce::Component& root, bool vertical,
                                  const auto& self) -> juce::ScrollBar* {
        for (int index = 0; index < root.getNumChildComponents(); ++index) {
            auto* child = root.getChildComponent(index);
            if (auto* bar = dynamic_cast<juce::ScrollBar*>(child);
                bar != nullptr && bar->isVertical() == vertical) return bar;
            if (auto* found = self(*child, vertical, self)) return found;
        }
        return nullptr;
    };
    auto* horizontal = findScrollBar(*timeline_impl, false, findScrollBar);
    auto* vertical = findScrollBar(*timeline_impl, true, findScrollBar);
    require(horizontal != nullptr && vertical != nullptr,
        "OPLL timeline exposes count and graph scroll boundaries");

    parameter->setSelectedId(3, juce::sendNotificationSync);
    const auto step_bounds = step_button->getBounds();
    const int graph_x = step_bounds.getX() - UiLayout::textButtonH
        - UiLayout::controlGap * 2 - UiLayout::xs;
    const int plot_x = graph_x + UiLayout::compositeOverviewLabelW;
    const int plot_width = vertical->getX() - plot_x;
    const double count_start = horizontal->getCurrentRangeStart();
    const double visible_counts = juce::jmax(1.0,
        horizontal->getCurrentRangeSize());
    const int x = plot_x + juce::roundToInt(
        (0.5 - count_start) * plot_width / visible_counts);
    const int y = step_bounds.getY() - UiLayout::xs
        + UiLayout::commandSummaryH + UiLayout::editMarkerLaneH / 2;
    const auto now = juce::Time::getCurrentTime();
    const juce::Point<int> point{x, y};
    const juce::MouseEvent click(
        juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, timeline_impl, timeline_impl,
        now, point.toFloat(), now, 1, false);
    timeline_impl->mouseDown(click);
    timeline_impl->mouseUp(click);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    juce::TopLevelWindow* picker_window = nullptr;
    for (int index = 0; index < juce::TopLevelWindow::getNumTopLevelWindows(); ++index) {
        auto* window = juce::TopLevelWindow::getTopLevelWindow(index);
        if (window != nullptr
            && findComponentWithText(*window,
                juce::String::fromUTF8("オリジナルを追加")) != nullptr) {
            picker_window = window;
            break;
        }
    }
    require(picker_window != nullptr,
        "clicking an OPLL step opens a picker that can create an original timbre");
    auto* add_original = dynamic_cast<juce::Button*>(
        findComponentWithText(*picker_window,
            juce::String::fromUTF8("オリジナルを追加")));
    require(add_original != nullptr && add_original->isEnabled(),
        "the step picker enables original OPLL timbre creation");

    const auto history_before = EditorAccess::historySize(editor);
    add_original->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    processEmpty(processor, 512);
    const auto edited = processor.copyPluginState().sound;
    require(edited.layers.size() == 1
            && edited.layers[0].base_opll_rom == 4
            && edited.layers[0].timbre_automation.size() == 1,
        "adding a step original keeps the channel base ROM and creates one @ event");
    const auto& event = edited.layers[0].timbre_automation.front();
    require(event.kind == Kind::Timbre && event.count == 0
            && event.timbre_pick == mgstc::engine::TimbrePick::Library
            && event.target_library_id != 0,
        "the new original is assigned to the clicked envelope step");
    const auto* embedded = mgstc::engine::findEmbeddedTimbreSnapshot(
        edited, event.target_library_id);
    require(embedded != nullptr && embedded->source == Source::Opll
            && embedded->opll_registers == mgstc::engine::encodeOpllPatch(
                mgstc::engine::defaultOpllPatch()),
        "the step assignment owns editable original OPLL registers in Plugin State");
    require(EditorAccess::historySize(editor) == history_before + 1
            && sameAuthoringSound(EditorAccess::sound(editor), edited),
        "creating the step original is published as one VST authoring edit");

    auto* edit_original = dynamic_cast<juce::Button*>(
        findComponentWithText(*picker_window,
            juce::String::fromUTF8("音色を編集")));
    require(edit_original != nullptr && edit_original->isEnabled(),
        "the newly assigned original remains editable from its step picker");
    edit_original->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(opened_context == "opll-envelope"
            && opened_timbre == event.target_library_id,
        "step picker opens the editor for the exact original OPLL snapshot");
}

void testOriginalTimbreAllocationSessionControls() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    using NumberMode = mgstc::engine::TimbreNumberMode;
    using Source = mgstc::engine::TimbreSource;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);

    auto original = mgstc::engine::defaultCompositeTimbre();
    auto scc_auto = sccOnlyTimbre().layers.front();
    scc_auto.channel = 0;
    scc_auto.base_timbre->number_mode = NumberMode::Automatic;
    scc_auto.base_timbre->manual_number.reset();
    auto scc_manual = scc_auto;
    scc_manual.channel = 1;
    scc_manual.base_timbre->library_id = 0xff00000000000031ULL;
    scc_manual.base_timbre->number_mode = NumberMode::Manual;
    scc_manual.base_timbre->manual_number = 7;
    scc_manual.scc_output_start = 7;
    scc_manual.base_timbre->scc_waveform[0] ^= 1;

    auto opll_rom = opllOnlyTimbre().layers.front();
    opll_rom.channel = 1;
    auto opll_auto = opll_rom;
    opll_auto.channel = 0;
    opll_auto.base_opll_rom.reset();
    mgstc::engine::SavedTimbreReference opll_reference;
    opll_reference.library_id = 0xff00000000000032ULL;
    opll_reference.name = "Automatic OPLL original";
    opll_reference.source = Source::Opll;
    opll_reference.number_mode = NumberMode::Automatic;
    opll_reference.opll_registers = {0x71, 0x61, 0x1e, 0x17,
        0xd0, 0x78, 0, 0x17};
    opll_auto.base_timbre = opll_reference;
    original.layers = {scc_auto, scc_manual, opll_auto, opll_rom};

    require(processor.replacePluginState(documentFrom(original, 0, {})),
        "seed automatic and fixed timbre allocation inputs");
    processEmpty(processor, 512);
    const auto authored_payload = soundBytes(original);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {},
        [](std::uint8_t) {}, [](juce::Component*) {}, [] {}, original);
    EditorAccess::size(editor, 1600, 1050);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto initial_state = stateBytes(processor);

    auto* scc_start = EditorAccess::originalAllocationCombo(editor, 32);
    auto* opll_start = EditorAccess::originalAllocationCombo(editor, 16);
    require(scc_start != nullptr && scc_start->getItemText(0) == "@0"
            && scc_start->getItemText(31) == "@31"
            && opll_start != nullptr && opll_start->getItemText(0) == "@16"
            && opll_start->getItemText(15) == "@31",
        "session selectors expose the full SCC @0..@31 and OPLL @16..@31 ranges");

    const auto checkAssignments = [&](std::uint8_t expected_scc,
                                      std::uint8_t expected_opll) {
        const auto sound = processor.copyPluginState().sound;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(sound);
        const auto find = [&](std::size_t layer) ->
                const mgstc::engine::ResolvedTimbreNumber* {
            const auto it = std::find_if(numbers.assignments.begin(),
                numbers.assignments.end(), [layer](const auto& item) {
                    return item.layer_index == layer;
                });
            return it == numbers.assignments.end() ? nullptr : &*it;
        };
        const auto* automatic_scc = find(0);
        const auto* manual_scc = find(1);
        const auto* automatic_opll = find(2);
        const bool matches = numbers.valid() && sound.original_timbre_allocation_starts
            && sound.original_timbre_allocation_starts->scc == expected_scc
            && sound.original_timbre_allocation_starts->opll == expected_opll
            && automatic_scc != nullptr && automatic_scc->number == expected_scc
            && !automatic_scc->manually_assigned
            && manual_scc != nullptr && manual_scc->number == 7
            && manual_scc->manually_assigned
            && automatic_opll != nullptr && automatic_opll->number == expected_opll
            && !automatic_opll->manually_assigned
            && sound.layers[3].base_opll_rom == 4;
        if (!matches) {
            std::fprintf(stderr, "Allocation publication expected=%u,%u editor=%d,%d processor=%d,%d valid=%d assignments=%zu\n",
                expected_scc, expected_opll, EditorAccess::originalAllocationStart(editor, 32),
                EditorAccess::originalAllocationStart(editor, 16),
                sound.original_timbre_allocation_starts ? sound.original_timbre_allocation_starts->scc : -1,
                sound.original_timbre_allocation_starts ? sound.original_timbre_allocation_starts->opll : -1,
                numbers.valid(), numbers.assignments.size());
            for (const auto& assignment : numbers.assignments)
                std::fprintf(stderr, "  layer=%zu number=%u manual=%d\n", assignment.layer_index, assignment.number, assignment.manually_assigned);
        }
        return matches;
    };

    EditorAccess::setOriginalAllocationStart(editor, 32, 31);
    EditorAccess::setOriginalAllocationStart(editor, 16, 31);
    processEmpty(processor, 512);
    require(checkAssignments(31, 31),
        "upper-bound session starts reach published core assignments while preserving manual and ROM bases");
    require(EditorAccess::historySize(editor) == 1
            && EditorAccess::historyCursor(editor) == 0
            && sameBlock(initial_state, stateBytes(processor))
            && soundBytes(processor.copyPluginState().sound) == authored_payload,
        "changing allocation starts updates the working program without saving or creating tone Undo entries");

    EditorAccess::setOriginalAllocationStart(editor, 32, 0);
    EditorAccess::setOriginalAllocationStart(editor, 16, 16);
    processEmpty(processor, 512);
    require(checkAssignments(0, 16),
        "lower-bound session starts allocate SCC @0 and OPLL @16");
    require(EditorAccess::historySize(editor) == 1
            && sameBlock(initial_state, stateBytes(processor)),
        "changing between allocation starts leaves saved Plugin State unchanged");

    EditorAccess::editName(editor, "Authored edit after allocation");
    EditorAccess::recordHistory(editor);
    processEmpty(processor, 512);
    require(EditorAccess::historySize(editor) == 2
            && EditorAccess::historyCursor(editor) == 1,
        "authored edits still create an Undo transaction beside session-only settings");
    EditorAccess::undo(editor);
    processEmpty(processor, 512);
    require(EditorAccess::historyCursor(editor) == 0
            && EditorAccess::originalAllocationStart(editor, 32) == 0
            && EditorAccess::originalAllocationStart(editor, 16) == 16
            && EditorAccess::sound(editor).original_timbre_allocation_starts
            && EditorAccess::sound(editor).original_timbre_allocation_starts->scc == 0
            && EditorAccess::sound(editor).original_timbre_allocation_starts->opll == 16,
        "Undo restores authored tone state and retains the current editor allocation starts");
    EditorAccess::recordHistory(editor); // unchanged name-field focus loss
    require(EditorAccess::historySize(editor) == 2
            && EditorAccess::historyCursor(editor) == 0,
        "a no-op history notification after a session change keeps Redo available");
    EditorAccess::redo(editor);
    processEmpty(processor, 512);
    require(EditorAccess::sound(editor).name == "Authored edit after allocation"
            && EditorAccess::originalAllocationStart(editor, 32) == 0
            && checkAssignments(0, 16),
        "Redo restores the authored edit and reapplies the current session numbering");

    EditorAccess::restoreHostState(editor, original);
    require(EditorAccess::sound(editor).name == original.name,
        "host load replaces the authored composite in the editor");
    require(EditorAccess::rebuild(editor),
        "restored sound can rebuild with the retained session allocation starts");
    processEmpty(processor, 512);
    require(EditorAccess::historySize(editor) == 1
            && EditorAccess::originalAllocationStart(editor, 32) == 0
            && EditorAccess::originalAllocationStart(editor, 16) == 16
            && EditorAccess::sound(editor).original_timbre_allocation_starts
            && checkAssignments(0, 16),
        "loading a host sound keeps the editor's current allocation starts out of the saved sound");
}

void testCompositeWavCandidateAudition() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    using WavAccess = mgstc::app::CompositeWavConversionTestAccess;
    using ProcessorAccess = mgstc::plugin::MgstcAudioProcessorTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    auto original = mgstc::engine::defaultCompositeTimbre();
    original.name = "WAV audition original";
    require(processor.replacePluginState(documentFrom(original, 0, {})), "seed candidate audition state");
    processEmpty(processor, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {}, [](std::uint8_t) {},
        [](juce::Component*) {}, [] {}, original);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto before = EditorAccess::sound(editor);
    const auto saved_state = stateBytes(processor);
    const auto history_before = EditorAccess::historySize(editor);
    auto candidate = sccOnlyTimbre();
    candidate.name = "Temporary WAV candidate";
    auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
    pcm->sample_rate = 48000; pcm->channels = 1;
    pcm->mono_samples.assign(128, 0.1f); pcm->interleaved_samples = pcm->mono_samples;
    mgstc::engine::CompositeWavConversionResult valid;
    valid.completion = mgstc::engine::CompositeWavConversionCompletion::Completed;
    valid.composite_tone = candidate;
    {
        mgstc::app::CompositeWavConversionContent content(
            [&](const mgstc::engine::CompositeTimbre& converted, std::function<void()> close) {
                EditorAccess::applyWave(editor, converted);
                if (close) close();
            }, context.pcmPreview(), context.backgroundTasks());
        EditorAccess::bindWaveAudition(editor, content);
        require(WavAccess::hasComparisonButtonsWithoutAb(content), "Original/Converted/Stop remain and A/B is absent");
        require(WavAccess::waveMethod(content) == 1, "SCC wave generation defaults to reconstructed");
        WavAccess::setWaveMethod(content, 2);
        require(WavAccess::waveMethod(content) == 2, "direct periodic generation is keyboard-selectable in shared UI");
        // Windows WM_CHAR supplies a printable text character; the one-argument
        // KeyPress constructor intentionally supplies no text character.
        const juce::KeyPress pc_z('Z', {}, 'z');
        require(!content.keyPressed(pc_z), "no candidate does not capture PC performance keys");
        require(WavAccess::accept(content, valid, pcm), "install completed WAV audition candidate");
        processEmpty(processor, 512);
        require(EditorAccess::hasWavePreview(editor) && sameAuthoringSound(EditorAccess::wavePreview(editor), candidate),
            "actual dialog callback binding publishes a separate temporary candidate");
        const auto& plan = ProcessorAccess::plan(processor);
        require(plan.audible_layers.size() == 1
                && plan.audible_layers.front().source == mgstc::engine::TimbreSource::Scc
                && plan.audible_layers.front().relative_semitones == candidate.layers.front().relative_semitones,
            "host MIDI uses the candidate's layers and transposition before Apply");
        require(EditorAccess::scheduled(editor) == 1 && EditorAccess::startsAndStopsShiftedEqually(editor, 1000.0)
            && !EditorAccess::waveAuditionPending(editor), "initial candidate selection starts the existing one-second note audition");
        WavAccess::stopPreview(content);
        require(EditorAccess::scheduled(editor) == 0 && !EditorAccess::waveAuditionPending(editor)
            && sameAuthoringSound(EditorAccess::wavePreview(editor), candidate), "Stop cancels one-second notes without clearing selected performance tone");
        require(content.keyPressed(pc_z), "WAV window forwards PC performance keys through existing mapping");
        require(!content.keyPressed(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 'z')),
            "WAV window preserves modified editing shortcuts");
        EditorAccess::start(editor, 72);
        require(EditorAccess::scheduled(editor) == 1 && EditorAccess::containsNote(editor, 72),
            "shared PC/MIDI UI note path uses candidate scheduling rather than original three layers");
        const auto onset = EditorAccess::onset(editor);
        EditorAccess::release(editor, 72, onset + 25.0);
        require(EditorAccess::startsAndStopsShiftedEqually(editor, 25.0),
            "candidate audition preserves layer delays and key release timing");
        require(peakOf(captureHostNotePcm(processor, 8192)) > 0.001,
            "actual DAW MIDI input sounds the temporary candidate through EngineCore");
        require(sameAuthoringSound(EditorAccess::sound(editor), before) && EditorAccess::historySize(editor) == history_before
                && sameBlock(saved_state, stateBytes(processor)),
            "candidate keyboard and host MIDI audition preserve authoring data, Undo and serialized Plugin State");
        auto selected_list = valid;
        auto alternate_tone = candidate; alternate_tone.layers.front().relative_semitones += 7;
        selected_list.retained_candidates = {{candidate, {}, {}, 1}, {alternate_tone, {}, {}, 2}};
        require(WavAccess::accept(content, selected_list, pcm), "install existing evaluated alternates into performance dialog");
        processEmpty(processor, 512); WavAccess::selectCandidate(content, 1); processEmpty(processor, 512);
        require(EditorAccess::scheduled(editor) == 1 && EditorAccess::startsAndStopsShiftedEqually(editor, 1000.0),
            "alternate selection restarts the existing one-second audition with a clean note lifetime");
        require(sameAuthoringSound(EditorAccess::wavePreview(editor), alternate_tone)
            && ProcessorAccess::plan(processor).audible_layers.front().relative_semitones == alternate_tone.layers.front().relative_semitones
            && peakOf(captureHostNotePcm(processor, 8192)) > 0.001
            && EditorAccess::historySize(editor) == history_before && sameBlock(saved_state, stateBytes(processor)),
            "alternate selection changes actual host MIDI plan/audio without changing Undo or Plugin State");
        WavAccess::selectCandidate(content, 0); processEmpty(processor, 512);
        require(!WavAccess::accept(content, {}, {}) && !WavAccess::accept(content, valid, pcm, true),
            "failed or cancelled reconversion is not a candidate replacement");
        require(sameAuthoringSound(EditorAccess::wavePreview(editor), candidate), "failed reconversion retains prior performance candidate");
        // A stopped host has no audio consumer to retire pending programs.
        // Multiple valid conversions must leave one slot for Cancel restoration.
        processEmpty(processor, 512);
        const auto stopped_compiles = ProcessorAccess::stateCompileCount(processor);
        auto latest = valid;
        latest.composite_tone->layers.front().relative_semitones += 7;
        auto list = valid;
        list.retained_candidates = {{candidate, {}, {}, 1}, {*latest.composite_tone, {}, {}, 2}};
        require(WavAccess::accept(content, list, pcm), "publish stopped-host list winner");
        WavAccess::selectCandidate(content, 1);
        WavAccess::selectCandidate(content, 0);
        WavAccess::selectCandidate(content, 1);
        require(sameAuthoringSound(EditorAccess::wavePreview(editor), *latest.composite_tone)
                && sameAuthoringSound(EditorAccess::sound(editor), before) && EditorAccess::historySize(editor) == history_before
                && sameBlock(saved_state, stateBytes(processor)),
            "rapid list selection uses latest pending candidate and preserves authoring, Undo and state");
        require(EditorAccess::waveAuditionPending(editor) && EditorAccess::scheduled(editor) == 0,
            "deferred latest candidate does not start a note on the old runtime program");
        EditorAccess::pollOwned(editor);
        require(ProcessorAccess::stateCompileCount(processor) == stopped_compiles + 1,
            "capacity wait neither compiles nor consumes restoration slot");
        WavAccess::clickCancel(content);
        processEmpty(processor, 512);
        EditorAccess::pollOwned(editor);
        require(!EditorAccess::hasWavePreview(editor) && !EditorAccess::waveAuditionPending(editor) && EditorAccess::scheduled(editor) == 0
                && EditorAccess::voices(editor) == 0 && ProcessorAccess::plan(processor).audible_layers.size() == 3,
            "Cancel stops candidate notes and restores the original host MIDI playback plan");
        require(sameBlock(saved_state, stateBytes(processor)) && sameAuthoringSound(EditorAccess::sound(editor), before),
            "Cancel restores only runtime program and leaves state bytes unchanged");
        require(WavAccess::accept(content, valid, pcm), "candidate can be installed again after cancellation");
        require(WavAccess::accept(content, latest, pcm), "retain latest candidate for resumed host");
        processEmpty(processor, 512);
        EditorAccess::pollOwned(editor);
        processEmpty(processor, 512);
        require(ProcessorAccess::plan(processor).audible_layers.front().relative_semitones
                == latest.composite_tone->layers.front().relative_semitones,
            "timer publishes the latest candidate when actual program capacity returns");
        require(!EditorAccess::waveAuditionPending(editor) && EditorAccess::scheduled(editor) == 1
            && EditorAccess::startsAndStopsShiftedEqually(editor, 1000.0), "latest deferred candidate starts one-second audition after publication succeeds");
        require(WavAccess::accept(content, selected_list, pcm), "restore Apply candidate list");
        processEmpty(processor, 512); WavAccess::selectCandidate(content, 1);
        WavAccess::clickApply(content);
        require(!EditorAccess::hasWavePreview(editor) && sameAuthoringSound(processor.copyPluginState().sound, alternate_tone)
                && EditorAccess::historySize(editor) == history_before + 1,
            "Apply clears temporary preview before committing exactly one Undo transaction");
        processEmpty(processor, 512);
        EditorAccess::undo(editor);
        require(sameAuthoringSound(EditorAccess::sound(editor), before) && sameAuthoringSound(processor.copyPluginState().sound, before),
            "Undo after candidate audition and Apply restores complete original sound");
        processEmpty(processor, 512);
        require(WavAccess::accept(content, valid, pcm), "candidate is available for host-restore lifecycle test");
        processEmpty(processor, 512);
        auto restored = before; restored.name = "DAW state restored during WAV preview";
        require(processor.replacePluginState(documentFrom(restored, 0, {})), "host restores a sound while candidate is installed");
        processEmpty(processor, 512);
        editor.restoreHostState(processor.copyPluginState().sound);
        require(!EditorAccess::hasWavePreview(editor), "host restore drops stale candidate without resubmission");
        WavAccess::clickCancel(content);
        require(sameAuthoringSound(processor.copyPluginState().sound, restored) && sameAuthoringSound(EditorAccess::sound(editor), restored),
            "old converter cancellation cannot overwrite a newer DAW state");
    }
    processEmpty(processor, 512);
    const auto closed_state = stateBytes(processor);
    {
        mgstc::app::CompositeWavConversionContent closing_content(
            [](const mgstc::engine::CompositeTimbre&, std::function<void()>) {},
            context.pcmPreview(), context.backgroundTasks());
        EditorAccess::bindWaveAudition(editor, closing_content);
        require(WavAccess::accept(closing_content, valid, pcm), "close test publishes first candidate");
        auto newest = valid;
        newest.composite_tone->layers.front().relative_semitones += 7;
        require(WavAccess::accept(closing_content, newest, pcm), "close test retains newest candidate");
        // Destruction closes the converter without an audio block between candidates.
    }
    processEmpty(processor, 512);
    EditorAccess::pollOwned(editor);
    require(!EditorAccess::hasWavePreview(editor)
            && ProcessorAccess::plan(processor).audible_layers.size() == 3
            && sameBlock(closed_state, stateBytes(processor)),
        "closing stopped-host converter restores author program and discards deferred candidate");
}

void testCompositeWavRetainedSelectionAndPreview() {
    using Access = mgstc::app::CompositeWavConversionTestAccess;
    struct RecordingPcm final : mgstc::app::PcmPreviewBoundary {
        std::shared_ptr<const mgstc::engine::SourcePcm> played;
        int plays{}, stops{};
        bool play(std::shared_ptr<const mgstc::engine::SourcePcm> pcm, std::size_t, std::size_t, float) override {
            played = std::move(pcm); ++plays; return true;
        }
        void stop() noexcept override { played.reset(); ++stops; }
    } playback;
    MgstcAudioProcessor processor;
    mgstc::plugin::PluginEditorContext context(processor);
    std::optional<mgstc::engine::CompositeTimbre> published, applied;
    mgstc::app::CompositeWavConversionContent content(
        [&](const mgstc::engine::CompositeTimbre& tone, std::function<void()>) { applied = tone; },
        &playback, context.backgroundTasks());
    int single_note_previews = 0, single_note_stops = 0;
    std::optional<mgstc::engine::CompositeTimbre> preview_tone;
    content.setAuditionCallbacks([&](const auto* tone) {
        if (tone) published = *tone; else published.reset();
    }, [](const juce::KeyPress&) { return true; }, [] {}, [&] {
        preview_tone = published; ++single_note_previews;
    }, [&] { ++single_note_stops; });
    require(Access::candidatesEmpty(content), "candidate region is present before conversion");
    auto winner = sccOnlyTimbre(); winner.name = "winner";
    auto alternate = winner; alternate.name = "alternate"; alternate.layers.front().relative_semitones = 7;
    auto third = alternate; third.name = "third"; third.layers.front().relative_semitones = -5;
    mgstc::engine::CompositeWavConversionResult result;
    result.completion = mgstc::engine::CompositeWavConversionCompletion::Completed;
    result.composite_tone = winner;
    result.preview.requested_key_off_frame = 4000;
    result.retained_candidates = {{winner, {}, {}, 10}, {alternate, {}, {}, 11}, {third, {}, {}, 12}};
    result.retained_candidates[0].resources.scc_waveforms = 1;
    result.retained_candidates[1].resources.scc_waveforms = 3;
    auto winnerPcm = std::make_shared<mgstc::engine::SourcePcm>();
    winnerPcm->sample_rate = 48000; winnerPcm->channels = 2;
    winnerPcm->mono_samples.assign(4800, 0.0f); winnerPcm->interleaved_samples.assign(9600, 0.0f);
    auto completedAnalysis = std::make_shared<mgstc::engine::SourceAnalysis>();
    completedAnalysis->source = winnerPcm; completedAnalysis->selection = {0, 4800};
    completedAnalysis->reference_pitch_hz = 523.251130601;
    result.analysis_reference = completedAnalysis;
    require(Access::accept(content, result, winnerPcm) && Access::selectedCandidate(content) == 0
        && published == winner, "completion initially selects and publishes unchanged automatic winner");
    require(single_note_previews == 1 && preview_tone == winner && !playback.played && !Access::previewPending(content),
        "initial row selection triggers one-second note callback without source-duration PCM rendering");
    Access::previewSelected(content);
    require(playback.played == winnerPcm && !Access::previewPending(content) && single_note_stops == 1,
        "converted play stops one-second notes before playing existing winner PCM directly");
    require(Access::moveCandidate(content, juce::KeyPress::downKey) && Access::selectedCandidate(content) == 1,
        "list supports down-arrow selection through ordinary JUCE keyboard handling");
    require(Access::moveCandidate(content, juce::KeyPress::upKey) && Access::selectedCandidate(content) == 0,
        "list supports up-arrow selection");
    Access::selectCandidate(content, 1);
    require(published == alternate && preview_tone == alternate && single_note_previews == 4
        && !playback.played && !Access::alternatePcm(content),
        "selection stops old PCM, publishes alternate before one-second audition and retains no PCM");
    Access::selectCandidate(content, 1);
    require(single_note_previews == 4, "selecting the same row does not duplicate one-second audition");
    require(content.keyPressed(juce::KeyPress('Z', {}, 'z')), "selected candidate retains PC-key routing");
    require(!Access::accept(content, {}, {}) && !Access::accept(content, result, winnerPcm, true)
        && Access::selectedCandidate(content) == 1 && published == alternate,
        "failed and cancelled reconversion preserve prior list, selection and audition");
    Access::previewSelected(content);
    require(Access::previewPending(content), "alternate PCM starts only on explicit converted-play request");
    Access::selectCandidate(content, 2);
    Access::previewSelected(content); // Queue latest explicit request behind cancelled worker.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Access::previewPending(content) && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    require(!Access::previewPending(content) && Access::alternatePcm(content)
        && playback.played == Access::alternatePcm(content) && published == third,
        "cancelled old preview cannot overwrite newest selection; latest request renders and plays");
    mgstc::engine::CompositeWavRenderOptions options;
    options.frame_count = 4800; options.key_off_frame = 4000; options.midi_note = 72;
    const auto expected = mgstc::engine::renderCompositeWav(third, options);
    require(expected.ok() && playback.played->mono_samples == expected.mono_pcm,
        "converted PCM is exactly the selected completed tone's Engine rendering");
    const auto cached = playback.played; Access::previewSelected(content);
    require(playback.played == cached && !Access::previewPending(content), "current alternate cache replays without another render");
    Access::clickApply(content);
    require(applied == third, "Apply passes currently selected completed tone");
    Access::selectCandidate(content, 0);
    require(!Access::alternatePcm(content), "switching away discards alternate PCM cache");
    require(Access::accept(content, result, winnerPcm) && Access::selectedCandidate(content) == 0 && published == winner,
        "successful reconversion reselects candidate one even when old row zero was selected");
    const auto savedScale = UiScale::active_percent;
    for (const int scale : {75, 100, 125}) {
        UiScale::setActivePercent(scale);
        content.setSize(UiScale::sx(1080), UiScale::sx(900)); content.resized();
        require(Access::candidateLayout(content), "source limit below total limit, level match beside Stop, compact settings and 65/35 result layout hold at supported scales");
    }
    UiScale::setActivePercent(savedScale);
    Access::source(content, winnerPcm);
    Access::selectCandidate(content, 1);
    const auto handle_note_stops = single_note_stops;
    Access::releaseRangeHandle(content, true);
    require(single_note_stops == handle_note_stops + 1 && playback.played == winnerPcm && published == alternate,
        "start-handle source preview stops one-second notes while preserving the selected candidate");
    Access::releaseRangeHandle(content, false);
    require(single_note_stops == handle_note_stops + 2 && playback.played == winnerPcm && published == alternate,
        "end-handle source preview stops one-second notes while preserving the selected candidate");
    Access::selectCandidate(content, 1); Access::previewSelected(content); Access::previewSource(content);
    const auto source_plays = playback.plays;
    const auto source_note_stops = single_note_stops;
    require(source_note_stops > 1, "Original stops the candidate one-second audition");
    while (Access::previewPending(content) && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    require(playback.played == winnerPcm && playback.plays == source_plays && !Access::alternatePcm(content),
        "Original cancels pending converted render and prevents late autoplay");
    Access::previewSelected(content); Access::stopPreview(content);
    require(single_note_stops >= source_note_stops + 2, "Converted and Stop both route to existing note-stop callback");
    const auto stopped_plays = playback.plays;
    while (Access::previewPending(content) && std::chrono::steady_clock::now() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    require(!playback.played && playback.plays == stopped_plays && !Access::alternatePcm(content),
        "Stop cancels pending render without late autoplay");
    Access::previewSelected(content); Access::clickCancel(content);
    require(!published && !Access::hasResult(content) && !Access::alternatePcm(content),
        "Cancel discards temporary candidates and cache and restores performance audition");
}

void testCompositeWavRangeInteractions() {
    using WavAccess = mgstc::app::CompositeWavConversionTestAccess;
    auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
    pcm->sample_rate = 48000; pcm->channels = 2;
    pcm->mono_samples.assign(1000, 0); pcm->interleaved_samples.assign(2000, 0.25f);
    mgstc::app::CompositeWavRange range; range.setSize(1000, 150); range.setSource(pcm);
    require(range.selection().begin == 0 && range.selection().end == 1000, "range end is original frame count exclusive");
    range.setSelection(100, 900);
    int callbacks = 0; bool last_start = false;
    range.changed = [&](bool start) { ++callbacks; last_start = start; };
    auto event = [&](float x, bool pressed, bool dragged) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {x, 60.0f},
            juce::ModifierKeys(pressed ? juce::ModifierKeys::leftButtonModifier : 0), 1, 0, 0, 0, 0,
            &range, &range, juce::Time::getCurrentTime(), {100.0f, 60.0f}, juce::Time::getCurrentTime(), 1, dragged);
    };
    range.mouseDown(event(100, true, false)); range.mouseDrag(event(200, true, true));
    require(callbacks == 0 && range.selection().begin == 200, "handle dragging changes sample range without preview retrigger");
    range.mouseUp(event(200, false, true));
    require(callbacks == 1 && last_start, "start release triggers exactly one preview request");
    range.mouseDown(event(900, true, false)); range.mouseDrag(event(1000, true, true)); range.mouseUp(event(1000, false, true));
    require(callbacks == 2 && !last_start && range.selection().end == 1000, "end release preserves exclusive final frame");
    require(range.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)), "range handles support keyboard adjustment");
    require(range.selection().end == 999, "keyboard adjusts end by one original sample");
    range.setSelection(950, 900);
    require(range.selection().end > range.selection().begin, "range prevents reversed/empty selection");
}

void testCompositeWavActualUiWorkerAndApply() {
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    using WavAccess = mgstc::app::CompositeWavConversionTestAccess;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    auto original = mgstc::engine::defaultCompositeTimbre();
    original.name = "Before actual conversion";
    require(processor.replacePluginState(documentFrom(original, 0, {})),
        "seed actual conversion test with matching VST3 program state");
    processEmpty(processor, 512);
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {}, [](std::uint8_t) {},
        [](juce::Component*) {}, [] {}, original);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto previous = EditorAccess::sound(editor);
    require(sameAuthoringSound(processor.copyPluginState().sound, previous),
        "actual conversion editor baseline matches committed VST3 state");
    auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
    pcm->sample_rate = 48'000; pcm->channels = 1; pcm->bit_depth = 32;
    pcm->sample_format = mgstc::engine::SourceSampleFormat::FloatPcm;
    pcm->mono_samples.resize(9'600);
    pcm->interleaved_samples.resize(9'600);
    for (std::size_t i = 0; i < pcm->mono_samples.size(); ++i) {
        const auto value = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * 220.0 * i / pcm->sample_rate));
        pcm->mono_samples[i] = value;
        pcm->interleaved_samples[i] = value;
    }
    bool applied = false;
    mgstc::app::CompositeWavConversionContent content(
        [&](const mgstc::engine::CompositeTimbre& converted, std::function<void()> close) {
            applied = true;
            EditorAccess::applyWave(editor, converted);
            if (close) close();
        }, context.pcmPreview(), context.backgroundTasks());
    EditorAccess::bindWaveAudition(editor, content);
    WavAccess::source(content, pcm);
    WavAccess::setWaveMethod(content, 2);
    WavAccess::setBudget(content, 8);
    require(WavAccess::sourceBudget(content) == 0, "source limit defaults to automatic");
    WavAccess::setSourceBudget(content, 1);
    WavAccess::clickAnalyze(content);
    for (int wait = 0; wait < 400 && !WavAccess::analysis(content); ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(WavAccess::analysis(content) != nullptr, "Analyze button completes bounded source analysis on worker");
    require(WavAccess::analysisMatches(content)
            && WavAccess::status(content).contains("解析結果")
            && WavAccess::status(content).contains("信頼度"),
        "worker completion exposes inspectable F0/key-off confidence before conversion");
    WavAccess::setKeyOff(content, "0.200");
    require(WavAccess::analysisMatches(content), "manual key-off correction keeps pitch/source analysis reusable");
    WavAccess::clickConvert(content);
    // Unoptimized Debug PCM candidate evaluation can exceed 30 seconds.
    // Keep a finite worker-completion wait without imposing a Release-speed
    // requirement on this functional transaction test.
    for (int wait = 0; wait < 15000 && !WavAccess::hasResult(content); ++wait)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    require(WavAccess::hasResult(content), "Convert button runs actual optimizer and publishes its completed result");
    std::set<std::array<std::uint8_t, 32>> source_waves;
    const auto& converted_tone = *WavAccess::result(content)->composite_tone;
    for (const auto& layer : converted_tone.layers) {
        if (layer.source != mgstc::engine::TimbreSource::Scc) continue;
        if (layer.base_timbre) source_waves.insert(layer.base_timbre->scc_waveform);
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Timbre) continue;
            const auto* ref = mgstc::engine::findEmbeddedTimbreSnapshot(converted_tone, event.target_library_id);
            require(ref != nullptr, "converted UI source resolves");
            source_waves.insert(ref->scc_waveform);
        }
    }
    require(source_waves.size() == 1, "UI source limit reaches optimizer independently of total Bank limit");
    require(EditorAccess::hasWavePreview(editor) && sameAuthoringSound(EditorAccess::sound(editor), previous)
            && sameAuthoringSound(processor.copyPluginState().sound, previous),
        "actual direct-periodic conversion publishes candidate for performance without committing it");
    // Deliberately keep the candidate pending: Apply must commit directly even
    // when the host has not consumed another block after worker completion.
    const auto apply_compiles = Access::stateCompileCount(processor);
    WavAccess::clickApply(content);
    require(Access::stateCompileCount(processor) == apply_compiles + 1,
        "Apply submits one converted program without an obsolete original-program restore");
    require(applied && EditorAccess::sound(editor) != previous,
        "Apply control sends actual converted result through shared editor transaction");
    require(processor.copyPluginState().sound == EditorAccess::sound(editor),
        "actual WAV conversion is reflected in VST3 Plugin State");
    processEmpty(processor, 512);
    EditorAccess::undo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), previous) && sameAuthoringSound(processor.copyPluginState().sound, previous),
        "Undo restores pre-conversion authoring and VST3 program state");
    processEmpty(processor, 512);
    EditorAccess::redo(editor);
    require(processor.copyPluginState().sound == EditorAccess::sound(editor),
        "Redo restores actual converted sound in VST3 Plugin State");
    processEmpty(processor, 512);
}

void testWavMorphPlanVstStateAndReediting() {
    using namespace mgstc::engine;
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = 48'000; pcm->channels = 1; pcm->bit_depth = 32;
    pcm->sample_format = SourceSampleFormat::FloatPcm;
    // Use the measured 0.25s benchmark source/budget that actually retains a
    // nonzero Morph plan. A static winner is legal for the old low-budget source.
    constexpr std::size_t fixture_frames = 12'000;
    constexpr std::size_t fixture_key_off = 8'640;
    constexpr double fixture_frequency = 261.625565;
    for (std::size_t i = 0; i < fixture_frames; ++i) {
        const double t = static_cast<double>(i) / 48'000;
        const double attack = std::min(1.0, t / .012);
        const double release = i < fixture_key_off ? 1.0
            : std::exp(-static_cast<double>(i - fixture_key_off) / (48'000 * .055));
        const double phase = 2 * std::numbers::pi * fixture_frequency * t;
        const auto value = static_cast<float>(.34 * attack * release
            * (.72 * std::sin(phase) + .21 * std::sin(2 * phase + .17)
                + .07 * std::sin(3 * phase + .41)));
        pcm->mono_samples.push_back(value); pcm->interleaved_samples.push_back(value);
    }
    SourceAnalysisOptions analysis_options; analysis_options.reference_pitch_hz = fixture_frequency;
    auto analysis = analyzeCompositeWaveSource(pcm, {0, pcm->mono_samples.size()}, analysis_options);
    require(analysis.analysis != nullptr, "WAV plan VST fixture analysis completes");
    CompositeWavConversionOptions options;
    options.max_scc_waveforms = 8; options.max_evaluations = 96;
    options.key_off_position = fixture_key_off;
    options.stagnation_limit = 0;
    options.loop_mode = CompositeWavLoopMode::Automatic;
    auto converted = convertCompositeWave(analysis.analysis, options);
    require(converted.composite_tone.has_value() && converted.morph_search.complete_comparisons > 0,
        "actual VST fixture conversion completes fair three-mode comparison");
    const auto confirmed = *converted.composite_tone;
    std::size_t confirmed_intervals{};
    for (const auto& layer : confirmed.layers)
        for (const auto& event : layer.timbre_automation)
            if (event.scc_morph.enabled && event.scc_morph.explicit_plan) ++confirmed_intervals;
    require(confirmed_intervals > 0, "actual converter freezes final mode and u/count placement in author data");
    const auto compiled = compileSccMorph(confirmed);
    const auto exported = formatMgsComposite(confirmed);
    require(compiled.valid && exported.valid(), "confirmed converter plan compiles for audition and MGSC");
    CompositeWavRenderOptions rendering;
    rendering.midi_note = 60; rendering.frame_count = fixture_frames; rendering.key_off_frame = fixture_key_off;
    const auto original_pcm = renderCompositeWav(confirmed, rendering);
    require(original_pcm.ok(), "confirmed converter plan uses actual Engine PCM for state parity");

    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto original = processor.copyPluginState().sound;
    mgstc::plugin::PluginEditorContext context(processor);
    mgstc::app::EditorLink link;
    mgstc::app::CompositeEditorComponent editor(context, link,
        [](const juce::String&, std::optional<std::uint64_t>) {}, [] {}, [](std::uint8_t) {},
        [](juce::Component*) {}, [] {}, original);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    EditorAccess::applyWave(editor, confirmed);
    require(sameAuthoringSound(EditorAccess::sound(editor), confirmed) && sameAuthoringSound(processor.copyPluginState().sound, confirmed),
        "actual WAV three-mode output applies through shared editor and VST state");
    processEmpty(processor, 512);
    const auto stored = stateBytes(processor);
    // Remove every original-WAV/analysis/preview owner before reopening.
    analysis.analysis.reset(); pcm.reset(); converted = {};
    MgstcAudioProcessor restored;
    restored.prepareToPlay(48'000.0, 512);
    restored.setStateInformation(stored.getData(), static_cast<int>(stored.getSize()));
    processEmpty(restored, 512);
    const auto restored_sound = restored.copyPluginState().sound;
    require(sameAuthoringSound(restored_sound, confirmed), "VST state preserves confirmed plan without original WAV");
    require(compileSccMorph(restored_sound).timbre == compiled.timbre
            && formatMgsComposite(restored_sound).source == exported.source,
        "WAV-free VST load generates bit-identical SCC waves and MGSC output");
    require(renderCompositeWav(restored_sound, rendering).stereo_pcm == original_pcm.stereo_pcm,
        "WAV-free VST state regenerates bit-identical actual Engine PCM");
    {
        std::unique_ptr<juce::AudioProcessorEditor> recreated(restored.createEditor());
        require(recreated != nullptr, "VST editor recreation succeeds with confirmed WAV morph plan");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
        require(sameAuthoringSound(restored.copyPluginState().sound, confirmed),
            "VST editor open/close hydration preserves saved u/count plan");
    }
    EditorAccess::undo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), original) && sameAuthoringSound(processor.copyPluginState().sound, original),
        "Undo of actual WAV mode selection restores complete prior VST author state");
    processEmpty(processor, 512);
    EditorAccess::redo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), confirmed) && sameAuthoringSound(processor.copyPluginState().sound, confirmed),
        "Redo restores converter-confirmed plans exactly");
    processEmpty(processor, 512);

    const auto original_base = *confirmed.layers.front().base_timbre;
    // The optimizer may retain a redundant embedded copy of the layer's base.
    // Exercise the separate base-only picker precondition without changing any
    // saved Morph plan, layer base or other source waveform.
    auto picker_source = confirmed;
    std::erase_if(picker_source.embedded_timbres, [&](const auto& snapshot) {
        return snapshot.library_id == original_base.library_id;
    });
    require(renderCompositeWav(picker_source, rendering).stereo_pcm == original_pcm.stereo_pcm,
        "base-only picker setup preserves the converter-confirmed actual Engine PCM");
    EditorAccess::applyWave(editor, picker_source);
    processEmpty(processor, 512);
    const auto replacement = std::find_if(confirmed.embedded_timbres.begin(), confirmed.embedded_timbres.end(),
        [&](const auto& snapshot) {
            return snapshot.source == TimbreSource::Scc && snapshot.library_id != original_base.library_id;
        });
    require(replacement != confirmed.embedded_timbres.end(), "base reassignment fixture has a distinct existing SCC source");
    require(std::none_of(picker_source.embedded_timbres.begin(), picker_source.embedded_timbres.end(),
        [&](const auto& snapshot) { return snapshot.library_id == original_base.library_id; }),
        "old WAV base exists only in its layer before the actual assignment");
    const auto base_history = EditorAccess::historySize(editor);
    EditorAccess::assignBase(editor, 0, replacement->library_id);
    const auto reassigned = EditorAccess::sound(editor);
    const auto* retained_base = findEmbeddedTimbreSnapshot(reassigned, original_base.library_id);
    require(reassigned.layers.front().base_timbre->library_id == replacement->library_id
            && retained_base && *retained_base == original_base,
        "actual base picker retains the displaced snapshot still referenced by explicit @0");
    require(reassigned.layers.front().timbre_automation == confirmed.layers.front().timbre_automation,
        "base picker preserves explicit @ author intent and unchanged confirmed WAV plans");
    require(compileSccMorph(reassigned).valid && formatMgsComposite(reassigned).valid()
            && sameAuthoringSound(processor.copyPluginState().sound, reassigned),
        "base reassignment keeps all WAV event sources resolvable in common DSP and VST state");
    require(EditorAccess::historySize(editor) == base_history + 1,
        "actual base source reassignment creates one author history transaction");
    processEmpty(processor, 512);
    const auto reassigned_blob = stateBytes(processor);
    const auto reassigned_parse = mgstc::plugin::parsePluginState(
        reassigned_blob.getData(), reassigned_blob.getSize());
    require(reassigned_parse.status == mgstc::plugin::PluginStateStatus::Ok
            && sameAuthoringSound(reassigned_parse.document.sound, reassigned),
        "retained old base and confirmed plans survive WAV-free Plugin State serialization");
    EditorAccess::undo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), picker_source) && sameAuthoringSound(processor.copyPluginState().sound, picker_source),
        "Undo of base selection removes the retained copy and restores original confirmed WAV state");
    processEmpty(processor, 512);
    EditorAccess::redo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), reassigned) && sameAuthoringSound(processor.copyPluginState().sound, reassigned),
        "Redo restores the referenced old base snapshot and unchanged plans");
    processEmpty(processor, 512);
    EditorAccess::assignBase(editor, 0, replacement->library_id);
    require(sameAuthoringSound(EditorAccess::sound(editor), reassigned)
            && EditorAccess::historySize(editor) == base_history + 1,
        "reselecting the same base neither duplicates retained sources nor adds history");
    processEmpty(processor, 512);
    EditorAccess::assignBase(editor, 0, {});
    const auto reset_base = EditorAccess::sound(editor);
    require(!reset_base.layers.front().base_timbre
            && findEmbeddedTimbreSnapshot(reset_base, original_base.library_id)
            && reset_base.layers.front().timbre_automation == confirmed.layers.front().timbre_automation
            && compileSccMorph(reset_base).valid && sameAuthoringSound(processor.copyPluginState().sound, reset_base),
        "base reset keeps all explicit WAV keyframe sources and confirmed plans intact in VST state");
    processEmpty(processor, 512);
    EditorAccess::undo(editor); // reset -> reassigned
    processEmpty(processor, 512);
    EditorAccess::undo(editor); // reassigned -> base-only picker source
    require(sameAuthoringSound(EditorAccess::sound(editor), picker_source) && sameAuthoringSound(processor.copyPluginState().sound, picker_source),
        "Undo of base reset and reassignment restores the complete base-only picker input");
    processEmpty(processor, 512);
    if (picker_source != confirmed) EditorAccess::undo(editor); // remove redundant-copy setup
    require(sameAuthoringSound(EditorAccess::sound(editor), confirmed) && sameAuthoringSound(processor.copyPluginState().sound, confirmed),
        "Undo of base reset and reassignment restores complete WAV-free conversion state");
    processEmpty(processor, 512);

    const EnvelopeEvent* selected = nullptr;
    for (const auto& event : confirmed.layers.front().timbre_automation)
        if (event.scc_morph.explicit_plan) { selected = &event; break; }
    require(selected != nullptr, "owned-wave edit test has a confirmed destination");
    auto changed_wave = *findEmbeddedTimbreSnapshot(confirmed, selected->target_library_id);
    changed_wave.scc_waveform[5] ^= 1;
    const auto history_cursor_before = EditorAccess::historyCursor(editor);
    link.publishOwned(changed_wave);
    EditorAccess::pollOwned(editor);
    const auto changed = EditorAccess::sound(editor);
    std::size_t retained_intervals{};
    for (const auto& layer : changed.layers)
        for (const auto& event : layer.timbre_automation)
            if (event.scc_morph.explicit_plan) ++retained_intervals;
    require(retained_intervals < confirmed_intervals && changed != confirmed
            && sameAuthoringSound(processor.copyPluginState().sound, changed),
        "actual owned SCC waveform edit invalidates dependent WAV plans and republishes VST state");
    require(compileSccMorph(changed).valid
            && EditorAccess::historyCursor(editor) == history_cursor_before + 1
            && EditorAccess::historySize(editor) == history_cursor_before + 2,
        "edited WAV endpoints regenerate through common DSP with one Undo transaction and truncate the old Redo branch");
    processEmpty(processor, 512);
    EditorAccess::undo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), confirmed) && sameAuthoringSound(processor.copyPluginState().sound, confirmed),
        "Undo of waveform editing restores original waves and confirmed WAV plans");
    processEmpty(processor, 512);
    EditorAccess::redo(editor);
    require(sameAuthoringSound(EditorAccess::sound(editor), changed) && sameAuthoringSound(processor.copyPluginState().sound, changed),
        "Redo preserves invalidated plan state after waveform editing");
    processEmpty(processor, 512);
}

void testTemporaryCompositePreviewPreservesState() {
    MgstcAudioProcessor processor;
    const auto before = stateBytes(processor);
    auto preview = processor.copyPluginState().sound;
    preview.name = "temporary-manual-y-preview";
    std::uint8_t capacity = 0;
    require(processor.previewEditorComposite(preview, true, capacity),
        "temporary composite preview compiles");
    require(capacity >= 1, "temporary preview has a voice");
    require(sameBlock(before, stateBytes(processor)),
        "temporary preview does not change plugin state");
    require(processor.copyPluginState().sound.name != preview.name,
        "temporary preview does not replace processor sound");
    require(processor.editorRestoreCommittedProgram(),
        "temporary preview restores committed program");
    require(sameBlock(before, stateBytes(processor)),
        "restoring temporary preview preserves plugin state");
}

void testPatchSlideSharedEditorTransactions() {
    using namespace mgstc::engine;
    using EditorAccess = mgstc::app::CompositeEditorComponentTestAccess;
    const auto makeSound = [](bool second) {
        PatchSlideCandidate candidate;
        candidate.candidate_id = second ? "slide_14_0_h2" : "slide_0_5_14_h12";
        candidate.patch_a = second ? 14 : 0;
        candidate.patch_b = second ? 0 : 5;
        candidate.hold_a_steps = second ? 2 : 1;
        candidate.hold_pattern = second ? "h2" : "h12";
        if (!second) {
            candidate.change_count = 2;
            candidate.patch_c = 14;
            candidate.hold_b_steps = 2;
        }
        candidate.tags = {"Bright", "Metallic"};
        auto sound = makePatchSlideTimbre(candidate);
        require(sound.has_value(), "PatchSlide test sound expands without opening a database");
        return *sound;
    };
    const auto slide = makeSound(false);
    const auto other_slide = makeSound(true);
    const auto find = [](const auto& self, juce::Component& parent)
        -> mgstc::app::CompositeEditorComponent* {
        if (auto* editor = dynamic_cast<mgstc::app::CompositeEditorComponent*>(&parent))
            return editor;
        for (int index = 0; index < parent.getNumChildComponents(); ++index)
            if (auto* found = self(self, *parent.getChildComponent(index))) return found;
        return nullptr;
    };
    MgstcAudioProcessor processor;
    auto original = defaultCompositeTimbre();
    original.name = "PatchSlide destination";
    original.tags = {"Keep original metadata"};
    original.memo = "Keep SCC, PSG and unrelated channel settings";
    original.favorite = true;
    original.layers[2].channel = 3;
    original.layers[2].envelope_number = 9;
    require(processor.replacePluginState(documentFrom(original, 0, {})), "Seed PatchSlide destination");
    processor.prepareToPlay(48'000.0, 512);
    processEmpty(processor, 512);
    MgstcAudioProcessor independent;
    independent.prepareToPlay(48'000.0, 512);
    const auto independent_state = stateBytes(independent);
    std::unique_ptr<juce::AudioProcessorEditor> view(processor.createEditor());
    auto* plugin_editor = dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(view.get());
    require(plugin_editor != nullptr, "PatchSlide uses the real shared VST3 editor");
    auto* editor = find(find, *view);
    require(editor != nullptr, "PatchSlide shared composite editor is connected");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto before = editor->workingTimbre();
    const auto before_state = stateBytes(processor);
    const auto history_before = EditorAccess::historySize(*editor);
    const auto cursor_before = EditorAccess::historyCursor(*editor);
    require(before.layers.size() == 3, "PatchSlide fixture retains all three chip sources");

    const auto preview = [&](const CompositeTimbre& sound) {
        ImportedToneCandidate imported;
        imported.source_format = "OPLL PatchSlide";
        imported.type = ImportedToneType::Composite;
        imported.default_register_as = ImportRegisterAs::Composite;
        imported.data = sound;
        editor->auditionImportedCandidate(imported);
        processEmpty(processor, 512);
        require(Access::plan(processor).audible_layers.size() == 1
                && Access::plan(processor).audible_layers.front().source == TimbreSource::Opll,
                "PatchSlide preview reaches the existing OPLL runtime");
        require(sameBlock(before_state, stateBytes(processor))
                && sameAuthoringSound(editor->workingTimbre(), before)
                && sameAuthoringSound(plugin_editor->copyWorkingComposite(), before),
                "PatchSlide preview preserves working sound and Plugin State");
        require(EditorAccess::historySize(*editor) == history_before
                && EditorAccess::historyCursor(*editor) == cursor_before,
                "Changing PatchSlide preview creates no Undo transaction");
    };
    preview(slide);
    preview(other_slide);
    editor->stopImportedPreview();
    processEmpty(processor, 512);
    require(sameBlock(before_state, stateBytes(processor))
            && Access::plan(processor).audible_layers.size() == 3,
            "Stopping PatchSlide preview restores the full committed program");
    require(!editor->applyPatchSlideAt(slide, 0)
            && !editor->applyPatchSlideAt(slide, before.layers.size() + 1),
            "PatchSlide application cannot overwrite PSG or use an invalid layer index");
    require(sameBlock(before_state, stateBytes(processor))
            && EditorAccess::historySize(*editor) == history_before,
            "Rejected PatchSlide targets leave state and history intact");
    preview(other_slide);
    require(editor->applyPatchSlideAt(slide, 2), "Confirmed PatchSlide replaces the selected OPLL channel");
    processEmpty(processor, 512);
    const auto applied = editor->workingTimbre();
    require(applied.layers.size() == before.layers.size()
            && applied.layers[0] == before.layers[0] && applied.layers[1] == before.layers[1],
            "PatchSlide replacement preserves SCC and PSG layers");
    auto expected_layer = slide.layers.front();
    expected_layer.channel = before.layers[2].channel;
    expected_layer.envelope_number = before.layers[2].envelope_number;
    require(applied.layers[2] == expected_layer,
            "PatchSlide replacement retains selected channel and envelope allocation");
    require(applied.name == before.name && applied.tags == before.tags
            && applied.memo == before.memo && applied.favorite == before.favorite,
            "PatchSlide replacement preserves the edited composite metadata");
    require(EditorAccess::historySize(*editor) == history_before + 1
            && EditorAccess::historyCursor(*editor) == cursor_before + 1
            && sameAuthoringSound(processor.copyPluginState().sound, applied),
            "One PatchSlide apply is one Undo transaction and one committed native sound");
    const auto saved = stateBytes(processor);
    EditorAccess::undo(*editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(editor->workingTimbre(), before)
            && sameAuthoringSound(processor.copyPluginState().sound, before),
            "Undo restores pre-application composite in editor and Processor");
    EditorAccess::redo(*editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(editor->workingTimbre(), applied)
            && sameAuthoringSound(processor.copyPluginState().sound, applied),
            "Redo restores every PatchSlide event in editor and Processor");

    const auto append_history = EditorAccess::historyCursor(*editor);
    require(editor->applyPatchSlideAt(other_slide, applied.layers.size()), "PatchSlide can append an OPLL layer");
    processEmpty(processor, 512);
    const auto appended = editor->workingTimbre();
    require(appended.layers.size() == applied.layers.size() + 1
            && std::equal(applied.layers.begin(), applied.layers.end(), appended.layers.begin())
            && appended.layers.back().source == TimbreSource::Opll
            && appended.layers.back().channel != applied.layers[2].channel
            && appended.layers.back().envelope_number != applied.layers[2].envelope_number,
            "Append preserves existing layers and allocates a free OPLL channel and ENV");
    require(EditorAccess::historyCursor(*editor) == append_history + 1,
            "Appending a PatchSlide is one Undo transaction");
    EditorAccess::undo(*editor);
    processEmpty(processor, 512);
    require(sameAuthoringSound(editor->workingTimbre(), applied), "Undo removes only the appended PatchSlide layer");
    require(sameBlock(independent_state, stateBytes(independent)), "PatchSlide editor operations are instance-local");

    view.reset();
    processEmpty(processor, 512);
    view.reset(processor.createEditor());
    requireWorkingCompositeMatchesProcessor(view.get(), processor,
        "Recreated VST3 editor hydrates the committed PatchSlide sound");
    require(sameAuthoringSound(processor.copyPluginState().sound, applied),
            "Closing and reopening the editor retains native PatchSlide data");
    view.reset();

    MgstcAudioProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    require(sameAuthoringSound(restored.copyPluginState().sound, applied),
            "Plugin State restores PatchSlide events without a DB lookup");
    restored.prepareToPlay(48'000.0, 512);
    const auto pcm = captureHostNotePcm(restored);
    require(peakOf(pcm) > 0.001, "Restored native PatchSlide composite plays through DAW MIDI without an editor");
    std::unique_ptr<juce::AudioProcessorEditor> restored_view(restored.createEditor());
    requireWorkingCompositeMatchesProcessor(restored_view.get(), restored,
        "Editor created after DB-independent state restore has the full PatchSlide timeline");
    require(sameBlock(independent_state, stateBytes(independent)),
            "Restoring another PatchSlide instance leaves the original independent instance unchanged");
    std::unique_ptr<juce::AudioProcessorEditor> independent_view(independent.createEditor());
    auto* independent_editor = find(find, *independent_view);
    require(independent_editor != nullptr, "Independent instance uses the same shared editor");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto without_opll = independent_editor->workingTimbre();
    require(std::none_of(without_opll.layers.begin(), without_opll.layers.end(),
                [](const CompositeLayer& layer) { return layer.source == TimbreSource::Opll; }),
            "Fresh instance has no OPLL destination layer");
    require(independent_editor->applyPatchSlideAt(slide, without_opll.layers.size()),
            "PatchSlide appends when the composite has no OPLL layer");
    processEmpty(independent, 512);
    const auto first_opll = independent_editor->workingTimbre();
    require(first_opll.layers.size() == without_opll.layers.size() + 1
            && std::equal(without_opll.layers.begin(), without_opll.layers.end(), first_opll.layers.begin())
            && first_opll.layers.back().source == TimbreSource::Opll
            && first_opll.layers.back().channel == 0,
            "First OPLL application retains every existing layer and uses channel zero");
    require(sameAuthoringSound(processor.copyPluginState().sound, applied),
            "Applying PatchSlide in the second instance leaves the first instance unchanged");
}

void testPluginSoundAuthority() {
    MgstcAudioProcessor reference;
    MgstcAudioProcessor shared;
    reference.prepareToPlay(48'000.0, 512);
    shared.prepareToPlay(48'000.0, 512);
    const auto baseline = captureHostNotePcm(reference);
    {
        PluginEditorContext context(shared);
        mgstc::app::SharedAuditionRequest request;
        const auto wave = mgstc::engine::generateSccPreset(
            mgstc::engine::SccWavePreset::Square,
            mgstc::engine::SccHarmonic::One);
        const auto patch = mgstc::engine::defaultOpllPatch();
        request.scc = &wave;
        request.opll = &patch;
        request.commit_scc = true;
        request.commit_opll = true;
        require(
            !context.audition().submitShared(request),
            "plugin shared audition does not replace the composite program");
        require(
            !shared.editorSharedProgramActive(),
            "shared audition is not plugin sound authority");
    }
    const auto after_shared = captureHostNotePcm(shared);
    require(
        hashPcm(baseline) == hashPcm(after_shared),
        "DAW MIDI still plays Composite after submitShared");

    MgstcAudioProcessor instance_a;
    MgstcAudioProcessor instance_b;
    instance_a.editorSetMasterVolumePercent(25);
    {
        PluginEditorContext context(instance_a);
        auto sound = instance_a.copyPluginState().sound;
        sound.name = "isolated-a";
        mgstc::app::CompositeAuditionRequest request;
        request.timbre = &sound;
        request.polyphonic = true;
        require(
            context.audition().submitComposite(request).ok,
            "instance A commits");
    }
    require(
        instance_a.copyPluginState().sound.name == "isolated-a",
        "A owns its composite");
    require(
        instance_b.copyPluginState().sound.name != "isolated-a",
        "instance B composite is independent");
    require(
        instance_b.editorMasterVolumePercent() == 100,
        "instance B master volume is independent");
    require(
        Access::scopeEnabled(instance_a) == false
            && Access::scopeEnabled(instance_b) == false,
        "scope is off without editors");

    auto* editor_a = instance_a.createEditor();
    auto* editor_b = instance_b.createEditor();
    require(
        Access::scopeEnabled(instance_a) && Access::scopeEnabled(instance_b),
        "each open editor enables only its instance scope");
    requireWorkingCompositeMatchesProcessor(
        editor_a,
        instance_a,
        "instance A editor rehydrates from A");
    requireWorkingCompositeMatchesProcessor(
        editor_b,
        instance_b,
        "instance B editor rehydrates from B");
    auto* plugin_a =
        dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(editor_a);
    auto* plugin_b =
        dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(editor_b);
    require(plugin_a != nullptr && plugin_b != nullptr, "plugin editor types");
    require(
        plugin_a->copyWorkingComposite().name == "isolated-a"
            && plugin_b->copyWorkingComposite().name != "isolated-a",
        "instances rehydrate independently");
    delete editor_a;
    require(
        !Access::scopeEnabled(instance_a) && Access::scopeEnabled(instance_b),
        "closing A leaves B scope on");
    delete editor_b;
    require(
        !Access::scopeEnabled(instance_a)
            && !Access::scopeEnabled(instance_b),
        "closing both editors stops both scopes");
}

void testToneLibrarySessionFallback() {
    // Run last: startup fallback deliberately lasts until process exit.
    const auto lock_name = "MGSTCStartupLibraryLockTest_" + juce::Uuid().toString();
    juce::WaitableEvent holder_ready;
    juce::WaitableEvent release_holder;
    std::atomic<bool> holder_locked{};
    std::jthread holder([&] {
        juce::InterProcessLock held_lock(lock_name);
        holder_locked.store(held_lock.enter(1000));
        holder_ready.signal();
        if (holder_locked.load()) {
            static_cast<void>(release_holder.wait(10000));
            held_lock.exit();
        }
    });
    require(holder_ready.wait(2000) && holder_locked.load(),
            "startup recovery lock test holder acquires shared lock");
    juce::InterProcessLock startup_lock(lock_name);
    std::string error;
    const auto begin = std::chrono::steady_clock::now();
    const bool loaded = mgstc::app::initialiseToneLibrarySession(startup_lock, &error);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    release_holder.signal();
    holder.join();
    require(!loaded && error == "failed to acquire shared tone library lock",
            "startup does not read the DB while another instance owns the library lock");
    require(elapsed_ms >= mgstc::app::kLibraryIpcLockTimeoutMs - 300
                && elapsed_ms < mgstc::app::kLibraryIpcLockTimeoutMs + 4000,
            "startup library lock uses the existing bounded timeout");
    require(startup_lock.enter(0), "startup lock is available after holder releases it");
    startup_lock.exit();
    error.clear();
    const auto snapshot = mgstc::app::loadToneLibraries(&error);
    const auto timbres = mgstc::app::loadSharedTimbreLibrary(&error);
    const auto composites =
        mgstc::app::loadSharedCompositeTimbreLibrary(&error);
    require(snapshot && snapshot->timbres.entries().empty()
                && snapshot->composites.entries().empty(),
            "disabled DB yields one empty combined library snapshot");
    require(timbres && timbres->entries().empty()
                && composites && composites->entries().empty(),
            "disabled DB yields empty libraries in all editors without reload errors");
    require(error.empty(), "disabled DB fallback does not repeat startup errors");
    require(!mgstc::app::persistTimbreLibrary(*timbres)
                && !mgstc::app::persistCompositeTimbreLibrary(*composites)
                && !mgstc::app::persistToneLibraries(*timbres, *composites),
            "disabled DB rejects every persistence path instead of replacing unreadable data");
    mgstc::app::ToneLibrarySnapshot available;
    mgstc::engine::TimbreLibraryEntry single;
    single.name = "Recovered OPLL";
    single.opll_registers = {0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26};
    const auto single_id = available.timbres.add(single, 1);
    auto sound = mgstc::engine::defaultCompositeTimbre();
    sound.name = "Recovered Composite";
    const auto composite_id = available.composites.add(sound, 1);
    mgstc::app::disableToneLibraryForSession(std::move(available));
    const auto recovered = mgstc::app::loadToneLibraries(&error);
    const auto recovered_singles = mgstc::app::loadSharedTimbreLibrary(&error);
    const auto recovered_composites =
        mgstc::app::loadSharedCompositeTimbreLibrary(&error);
    require(recovered && recovered->timbres.find(single_id)
                && recovered->composites.find(composite_id)
                && recovered_singles && recovered_singles->find(single_id)
                && recovered_composites && recovered_composites->find(composite_id),
            "partial startup snapshot remains available through every editor load path");
    require(recovered_singles->find(single_id)->opll_registers == single.opll_registers
                && recovered_composites->find(composite_id)->timbre.name == sound.name,
            "partial startup snapshot retains readable sound data and metadata");
    require(!mgstc::app::persistTimbreLibrary(*recovered_singles)
                && !mgstc::app::persistCompositeTimbreLibrary(*recovered_composites)
                && !mgstc::app::persistToneLibraries(*recovered_singles, *recovered_composites),
            "partial startup snapshot cannot overwrite rows that failed to load");
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce_init;
    try {
        if (std::getenv("MGSTC_PATCHSLIDE_UI_ONLY")) {
            testPatchSlideSharedEditorTransactions();
            return 0;
        }
        if (std::getenv("MGSTC_COMMON_HOST_UI_ONLY")) {
            testPluginStateFoundation();
            testPluginProjectRestore();
            testPluginEditor();
            testCompositePlaybackWaveform();
            testScopeFrameDistributorReaders();
            testProcessorOfflineAndTransport();
            testLibraryPreviewsPreserveCommittedPluginState();
            testProcessorHardwareRoutingWithMockRpc();
            testOpllKeyoffSilenceDeadline();
            testVstPcmPreviewStereoRangeGainAndHostRate();
            testVstPcmPreviewMuteClampAndInvalidRequests();
            testVstPcmPreviewReplaceStopAndLifecycle();
            testPluginSoundAuthority();
            testTemporaryCompositePreviewPreservesState();
            testPatchSlideSharedEditorTransactions();
            return 0;
        }
        if (std::getenv("MGSTC_WAV_CANDIDATE_UI_ONLY")) {
            testConversionOwnerClose();
            testConversionCompletionCancellation();
            testCompositeWavUiTransactions();
            testCompositeStepSlideVstTransactions();
            testCompositeWavCandidateAudition();
            testCompositeWavRetainedSelectionAndPreview();
            testCompositeWavRangeInteractions();
            testToneLibrarySessionFallback();
            return 0;
        }
        testSincRingMatchesLinearShift();
        testNoteOnAtSample128DoesNotAffectPrefix();
        testInvalidSampleRateIsSilentAndDoesNotAdvanceEngine();
        testHostEngineMapping();
        testCubaseFirstNoteOnAfterSilence();
        testEngineNeverLeadsHostMapping();
        testRealtimeDeadlineCost();
        profileEngineIdleBreakdown();
        testIdleDurationChangesNextNotePcm();
        testNeverPlayedIsQuiescentAndReleaseStillSounds();
        testBatchSourceRenderCallCount();
        testMidBlockNoteIsNotRoundedToBatchStart();
        testNon48kHzProducesAudio();
        testNon48kHzOutputIsFinite();
        testLatencyReportedOnlyFromPrepareAndRelease();
        testHostBufferContractAcrossRates();
        test48kHzBypassesSrcAndHasZeroLatency();
        test44100MidiOffsetMapsToEngine480();
        testLayerDelayIs9600EngineFramesAtEveryHostRate();
        testCancellationAt44100();
        testSameFrameCancelAt44100();
        testVoiceStealAndAllNotesOffAt44100();
        testAllSoundOffAt44100();
        testBlockSizeInvarianceAt44100();
        testLongRunDriftAt44100();
        testRateChangeResetsSrcTimelineAndLatency();
        testTwoProcessorsDoNotShareSrcState();
        testPitchIndependentOfHostRate();
        testDurationIndependentOfHostRate();
        testTwoProcessorsDoNotShareVoiceState();
        testZeroDelayFiresAtMidiFrame();
        testDelayedLayersFireAtExactDueAndAcrossBlocks();
        testNoteOffBeforeDueCancelsDelayedLayers();
        testNoteOffAfterDueLeavesReleasePath();
        testSameFrameNoteOffCancelsDueEvent();
        testVoiceStealCancelsOldPending();
        testAllNotesOffCancelsPending();
        testAllSoundOffCancelsPending();
        testLifecycleResetDropsPending();
        testProgramResetDropsPending();
        testOverdueMappedEventsKeepOrderAndDoNotStick();
        testExactFrameStillConsumedAndFutureStaysQueued();
        testOverdueThenSameFrameKeepsQueueOrder();
        testOverdueNoteOffCancelsDueLayer();
        testOverdueVoiceStealReplacesDueLayer();
        testOverdueAllNotesOffAndAllSoundOff();
        testDirectSpanConsumesOverdueWithoutStalling();
        testIteratorOverdueIsConsumedInOrder();
        testBlockBoundaryMidiStaysOnItsFrame();
        testHostRateOverdueNoteIsAppliedNow();
        testPluginStateFoundation();
        testWideSoftwareLfoMatchesDirectEngineCoreWithPitchEnvelope();
        testSccMorphPluginStateCompatibility();
        testPluginProjectRestore();
        testPluginEditor();
        testOwnedEditorPendingTargetKeepsSnapshot();
        testEditorParityCorrections();
        testCompositePlaybackWaveform();
        testScopeFrameDistributorReaders();
        testProcessorOfflineAndTransport();
        testLibraryPreviewsPreserveCommittedPluginState();
        testProcessorHardwareRoutingWithMockRpc();
        testOpllKeyoffSilenceDeadline();
        testCommittedTimelineEditSupersedesPickerPreview();
        testCompositeEditorDelayedKeyOff();
        testPluginSoundAuthority();
        testTemporaryCompositePreviewPreservesState();
        testPatchSlideSharedEditorTransactions();
        testVstPcmPreviewStereoRangeGainAndHostRate();
        testVstPcmPreviewMuteClampAndInvalidRequests();
        testVstPcmPreviewReplaceStopAndLifecycle();
        testVstSharedSccMorphHostAudio();
        testVstConverterWorkerQuiescesAtProcessorTeardown();
        testConversionOwnerClose();
        testConversionCompletionCancellation();
        testCompositeWavSourceImportTransactions();
        testCompositeWavUiTransactions();
        testCompositeStepSlideVstTransactions();
        testCompositeLoopVstTransactions();
        testPhaseIncompatibleModulationLoopRunsInVst();
        testVstOpllStepPickerCreatesAndOpensOriginalTimbre();
        testOriginalTimbreAllocationSessionControls();
        testCompositeWavCandidateAudition();
        testCompositeWavRetainedSelectionAndPreview();
        testCompositeWavRangeInteractions();
        testCompositeWavActualUiWorkerAndApply();
        testWavMorphPlanVstStateAndReediting();
        testToneLibrarySessionFallback();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
