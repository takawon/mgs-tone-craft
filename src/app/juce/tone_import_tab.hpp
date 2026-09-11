// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "juce_utf8.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/tone_import.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"

class ToneImportTab final : public juce::Component {
public:
    using PreviewCallback = std::function<void(
        const mgstc::engine::ImportedToneCandidate&)>;
    using EditCallback = std::function<void(
        const mgstc::engine::ImportedToneCandidate&)>;
    using PersistCallback = std::function<bool()>;
    using ReloadCallback = std::function<bool()>;
    using LibrariesChangedCallback = std::function<void()>;

    ToneImportTab(
        PreviewCallback preview,
        EditCallback edit,
        ReloadCallback reload,
        PersistCallback persist_timbres,
        PersistCallback persist_composites,
        std::function<mgstc::engine::TimbreLibrary*()> timbres,
        std::function<mgstc::engine::CompositeTimbreLibrary*()> composites,
        LibrariesChangedCallback libraries_changed)
        : preview_(std::move(preview)),
          edit_(std::move(edit)),
          reload_(std::move(reload)),
          persist_timbres_(std::move(persist_timbres)),
          persist_composites_(std::move(persist_composites)),
          timbres_(std::move(timbres)),
          composites_(std::move(composites)),
          libraries_changed_(std::move(libraries_changed)) {
        setWantsKeyboardFocus(true);
        open_file_.setButtonText(juce::String::fromUTF8("ファイルを開く"));
        open_file_.setTooltip(
            juce::String::fromUTF8(
                "1つまたは複数の音色データを開いてリストへ追加する。"
                "リストはアプリ終了まで保持する"));
        open_file_.onClick = [this] { openFiles(); };
        addAndMakeVisible(open_file_);

        status_.setColour(
            juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
        addAndMakeVisible(status_);

        viewport_.setViewedComponent(&list_host_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);

        register_selected_.setButtonText(
            juce::String::fromUTF8("選択項目を登録"));
        register_selected_.setTooltip(
            juce::String::fromUTF8("チェックした未登録の音色を一括登録する"));
        register_selected_.onClick = [this] { registerChecked(); };
        addAndMakeVisible(register_selected_);
        register_all_.setButtonText(juce::String::fromUTF8("すべて登録"));
        register_all_.onClick = [this] { registerAll(); };
        addAndMakeVisible(register_all_);
        applyFonts();
        updateStatus();
    }

    [[nodiscard]] const mgstc::engine::ImportedToneCandidate*
    selectedCandidate() const {
        return candidateAt(selected_);
    }

    void paint(juce::Graphics& graphics) override {
        if (!list_bounds_.isEmpty()) {
            fillRoundedPanelFrame(graphics, list_bounds_);
        }
    }

    void paintOverChildren(juce::Graphics& graphics) override {
        if (!list_bounds_.isEmpty()) {
            strokeRoundedPanelFrame(graphics, list_bounds_);
        }
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto header = area.removeFromTop(textButtonH);
        open_file_.setBounds(header.removeFromLeft(UiScale::sx(140)));
        header.removeFromLeft(controlGap);
        status_.setBounds(header);
        area.removeFromTop(sm);
        auto footer = area.removeFromBottom(textButtonH);
        register_all_.setBounds(footer.removeFromRight(UiScale::sx(120)));
        footer.removeFromRight(controlGap);
        register_selected_.setBounds(footer.removeFromRight(UiScale::sx(160)));
        area.removeFromBottom(sm);
        list_bounds_ = area;
        viewport_.setBounds(area.reduced(1));
        layoutRows();
    }

    void lookAndFeelChanged() override {
        applyFonts();
        resized();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (row_keys_.empty()) {
            return false;
        }
        if (key == juce::KeyPress::downKey) {
            selectRow(selected_ + 1, true);
            return true;
        }
        if (key == juce::KeyPress::upKey) {
            selectRow(selected_ - 1, true);
            return true;
        }
        return false;
    }

private:
    struct ImportFileGroup {
        mgstc::engine::ToneImportResult result;
        std::vector<char> checked;
    };

    struct RowKey {
        std::size_t file_index{};
        std::size_t candidate_index{};
    };

    class MiniPreview final : public juce::Component {
    public:
        void setCandidate(const mgstc::engine::ImportedToneCandidate& candidate) {
            candidate_ = candidate;
            if (const auto* patch =
                    std::get_if<mgstc::engine::OpllPatchParameters>(
                        &candidate.data)) {
                trace_ = mgstc::engine::traceOpllEnvelope(*patch, 60);
            } else {
                trace_ = {};
            }
            repaint();
        }

        void paint(juce::Graphics& graphics) override {
            const auto bounds = getLocalBounds().toFloat().reduced(1.0F);
            graphics.setColour(juce::Colour(0xFF182028));
            graphics.fillRoundedRectangle(bounds, 5.0F);
            graphics.setColour(juce::Colour(0xFF435160));
            graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);
            const auto graph = bounds.reduced(4.0F, 3.0F);
            graphics.setColour(juce::Colour(0xFF586675));
            graphics.drawHorizontalLine(
                juce::roundToInt(graph.getCentreY()),
                graph.getX(),
                graph.getRight());
            if (const auto* wave =
                    std::get_if<mgstc::engine::SccWaveform>(&candidate_.data)) {
                paintWave(graphics, graph, *wave);
                return;
            }
            if (std::holds_alternative<mgstc::engine::OpllPatchParameters>(
                    candidate_.data)
                && trace_.valid) {
                paintTrace(
                    graphics, graph, trace_.modulator, juce::Colour(0xFF9AA8B5));
                paintTrace(graphics, graph, trace_.carrier, juce::Colour(kUiHoverAccent));
                return;
            }
            if (const auto* timbre =
                    std::get_if<mgstc::engine::CompositeTimbre>(
                        &candidate_.data)) {
                paintEnvelope(graphics, graph, *timbre);
                return;
            }
            if (candidate_.composite_alternative) {
                paintEnvelope(graphics, graph, *candidate_.composite_alternative);
            }
        }

    private:
        static void paintWave(
            juce::Graphics& graphics,
            juce::Rectangle<float> graph,
            const mgstc::engine::SccWaveform& waveform) {
            juce::Path path;
            for (std::size_t index = 0; index < waveform.size(); ++index) {
                const auto x = juce::jmap(
                    static_cast<float>(index),
                    0.0F,
                    static_cast<float>(waveform.size() - 1),
                    graph.getX(),
                    graph.getRight());
                const auto y = juce::jmap(
                    static_cast<float>(waveform[index]),
                    -128.0F,
                    127.0F,
                    graph.getBottom(),
                    graph.getY());
                if (index == 0) {
                    path.startNewSubPath(x, y);
                } else {
                    path.lineTo(x, y);
                }
            }
            graphics.setColour(juce::Colour(kUiHoverAccent));
            graphics.strokePath(
                path,
                juce::PathStrokeType(
                    1.2F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
        }

        static void paintTrace(
            juce::Graphics& graphics,
            juce::Rectangle<float> graph,
            const std::array<float, mgstc::engine::OpllEnvelopeTrace::kPointCount>&
                levels,
            juce::Colour colour) {
            juce::Path path;
            for (std::size_t index = 0; index < levels.size(); ++index) {
                const float x = graph.getX()
                    + graph.getWidth() * static_cast<float>(index)
                        / static_cast<float>(levels.size() - 1);
                const float y = graph.getBottom()
                    - graph.getHeight() * std::clamp(levels[index], 0.0F, 1.0F);
                if (index == 0) {
                    path.startNewSubPath(x, y);
                } else {
                    path.lineTo(x, y);
                }
            }
            graphics.setColour(colour);
            graphics.strokePath(path, juce::PathStrokeType(1.2F));
        }

        static void paintEnvelope(
            juce::Graphics& graphics,
            juce::Rectangle<float> graph,
            const mgstc::engine::CompositeTimbre& timbre) {
            if (timbre.layers.empty()) {
                return;
            }
            const auto& events = timbre.layers.front().volume_envelope.events;
            if (events.empty()) {
                return;
            }
            const auto length = std::max<std::uint32_t>(
                1, timbre.layers.front().envelope_timeline.length_counts);
            juce::Path path;
            bool started = false;
            for (const auto& event : events) {
                if (event.kind != mgstc::engine::EnvelopeEventKind::Volume) {
                    continue;
                }
                const float x = graph.getX()
                    + graph.getWidth()
                        * static_cast<float>(event.count)
                        / static_cast<float>(length);
                const float y = juce::jmap(
                    static_cast<float>(event.value),
                    0.0F,
                    15.0F,
                    graph.getBottom(),
                    graph.getY());
                if (!started) {
                    path.startNewSubPath(x, y);
                    started = true;
                } else {
                    path.lineTo(x, y);
                }
            }
            graphics.setColour(juce::Colour(kUiHoverAccent));
            graphics.strokePath(path, juce::PathStrokeType(1.2F));
        }

        mgstc::engine::ImportedToneCandidate candidate_{};
        mgstc::engine::OpllEnvelopeTrace trace_{};
    };

    class FileGroupHeader final : public juce::Component {
    public:
        FileGroupHeader() {
            name_.setInterceptsMouseClicks(false, false);
            name_.setColour(
                juce::Label::textColourId, juce::Colour(0xFFF2F4F5));
            addAndMakeVisible(name_);
            format_.setInterceptsMouseClicks(false, false);
            format_.setJustificationType(juce::Justification::centredRight);
            format_.setColour(
                juce::Label::textColourId, juce::Colour(0xFF9AA8B5));
            addAndMakeVisible(format_);
            applyFonts();
        }

        void setFile(
            const juce::String& file_name, const juce::String& format) {
            name_.setText(file_name, juce::dontSendNotification);
            format_.setText(format, juce::dontSendNotification);
        }

        void applyFonts() {
            name_.setFont(UiFonts::heading());
            format_.setFont(UiFonts::body());
        }

        void paint(juce::Graphics& graphics) override {
            graphics.setColour(juce::Colour(0x33182028));
            graphics.fillRoundedRectangle(
                getLocalBounds().toFloat().reduced(1.0F), 4.0F);
        }

        void resized() override {
            using namespace UiLayout;
            auto area = getLocalBounds().reduced(xs, xs);
            format_.setBounds(area.removeFromRight(UiScale::sx(88)));
            area.removeFromRight(controlGap);
            name_.setBounds(area);
        }

    private:
        juce::Label name_;
        juce::Label format_;
    };

    class CandidateRow final : public juce::Component,
                                 public juce::SettableTooltipClient {
    public:
        CandidateRow(
            ToneImportTab& owner,
            std::size_t file_index,
            std::size_t candidate_index,
            int flat_index)
            : owner_(owner),
              file_index_(file_index),
              candidate_index_(candidate_index),
              flat_index_(flat_index) {
            checked_.setButtonText({});
            checked_.setTooltip(
                juce::String::fromUTF8("登録対象に含める"));
            checked_.onClick = [this] {
                auto& group = owner_.files_[file_index_];
                if (candidate_index_ < group.checked.size()) {
                    group.checked[candidate_index_] =
                        checked_.getToggleState() ? 1 : 0;
                }
                owner_.selectRow(flat_index_, true);
            };
            addAndMakeVisible(checked_);
            type_.setInterceptsMouseClicks(false, false);
            type_.setFont(UiFonts::body());
            addAndMakeVisible(type_);
            UiFonts::styleBodyField(name_);
            name_.onTextChange = [this] {
                candidate().name = utf8String(name_.getText());
            };
            name_.onFocusLost = [this] {
                candidate().name = utf8String(name_.getText());
            };
            addAndMakeVisible(name_);
            addAndMakeVisible(preview_);
            favorite_.setJustificationType(juce::Justification::centred);
            favorite_.setFont(UiFonts::body());
            favorite_.setInterceptsMouseClicks(true, false);
            favorite_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
            favorite_.addMouseListener(this, false);
            addAndMakeVisible(favorite_);
            register_.onClick = [this] {
                owner_.registerAt(file_index_, candidate_index_, std::nullopt);
            };
            addAndMakeVisible(register_);
            edit_.setButtonText(juce::String::fromUTF8("編集"));
            edit_.onClick = [this] {
                owner_.editAt(file_index_, candidate_index_);
            };
            addAndMakeVisible(edit_);
            registered_.setColour(
                juce::Label::textColourId, juce::Colour(kUiHoverAccent));
            registered_.setFont(UiFonts::body());
            addAndMakeVisible(registered_);
            refreshFromModel();
        }

        void mouseDown(const juce::MouseEvent& event) override {
            if (event.eventComponent == &favorite_) {
                auto& item = candidate();
                item.favorite = !item.favorite;
                refreshFromModel();
                return;
            }
            owner_.selectRow(flat_index_, true);
        }

        void resized() override {
            using namespace UiLayout;
            auto area = getLocalBounds().reduced(xs, xs);
            checked_.setBounds(area.removeFromLeft(UiScale::sx(28)));
            area.removeFromLeft(controlGap);
            type_.setBounds(area.removeFromLeft(UiScale::sx(56)));
            area.removeFromLeft(controlGap);
            preview_.setBounds(area.removeFromLeft(UiScale::sx(110)));
            area.removeFromLeft(controlGap);
            register_.setBounds(area.removeFromRight(UiScale::sx(108)));
            registered_.setBounds(register_.getBounds());
            area.removeFromRight(controlGap);
            favorite_.setBounds(area.removeFromRight(UiScale::sx(28)));
            area.removeFromRight(controlGap);
            edit_.setBounds(area.removeFromRight(
                std::max(libraryButtonMinW, UiScale::sx(72))));
            area.removeFromRight(controlGap);
            name_.setBounds(area);
        }

        void paint(juce::Graphics& graphics) override {
            if (owner_.selected_ == flat_index_) {
                graphics.setColour(juce::Colour(0x3320A070));
                graphics.fillRoundedRectangle(
                    getLocalBounds().toFloat().reduced(1.0F), 4.0F);
            }
        }

        void refreshFromModel() {
            const auto& item = candidate();
            const auto& group = owner_.files_[file_index_];
            const bool is_checked =
                candidate_index_ < group.checked.size()
                && group.checked[candidate_index_] != 0;
            checked_.setToggleState(is_checked, juce::dontSendNotification);
            type_.setText(typeLabel(item), juce::dontSendNotification);
            name_.setText(
                juce::String::fromUTF8(item.name.c_str()),
                juce::dontSendNotification);
            preview_.setCandidate(item);
            favorite_.setText(
                juce::String::fromUTF8(item.favorite ? "★" : "☆"),
                juce::dontSendNotification);
            favorite_.setColour(
                juce::Label::textColourId,
                item.favorite
                    ? juce::Colour(0xFFFFD866)
                    : juce::Colour(0xFF8A97A4));
            const bool multi = item.register_choices.size() > 1;
            register_.setButtonText(
                juce::String::fromUTF8(multi ? "登録 ▼" : "登録"));
            if (multi) {
                register_.onClick = [this] {
                    owner_.showRegisterMenu(file_index_, candidate_index_);
                };
            } else {
                register_.onClick = [this] {
                    owner_.registerAt(
                        file_index_, candidate_index_, std::nullopt);
                };
            }
            register_.setVisible(!item.registered);
            registered_.setVisible(item.registered);
            registered_.setText(
                juce::String::fromUTF8("✓ 登録済"),
                juce::dontSendNotification);
            if (!item.warnings.empty()) {
                juce::String tip;
                for (const auto& warning : item.warnings) {
                    if (tip.isNotEmpty()) {
                        tip += "\n";
                    }
                    tip += juce::String::fromUTF8(warning.message.c_str());
                }
                setTooltip(tip);
            } else {
                setTooltip({});
            }
        }

    private:
        [[nodiscard]] mgstc::engine::ImportedToneCandidate& candidate() {
            return owner_.files_[file_index_].result.candidates[candidate_index_];
        }

        static juce::String typeLabel(
            const mgstc::engine::ImportedToneCandidate& candidate) {
            if (candidate.type == mgstc::engine::ImportedToneType::Composite) {
                return juce::String::fromUTF8("総合");
            }
            if (candidate.type == mgstc::engine::ImportedToneType::Scc) {
                return juce::String::fromUTF8("SCC");
            }
            return juce::String::fromUTF8("OPLL");
        }

        ToneImportTab& owner_;
        std::size_t file_index_{};
        std::size_t candidate_index_{};
        int flat_index_{};
        juce::ToggleButton checked_;
        juce::Label type_;
        juce::TextEditor name_;
        MiniPreview preview_;
        juce::Label favorite_;
        juce::TextButton register_;
        juce::TextButton edit_;
        juce::Label registered_;
    };

    void applyFonts() {
        open_file_.setLookAndFeel(&getLookAndFeel());
        status_.setFont(UiFonts::body());
        for (auto& header : headers_) {
            header->applyFonts();
        }
    }

    void openFiles() {
        juce::FileChooser chooser(
            juce::String::fromUTF8("音色データをインポート"),
            {},
            "*.mgs;*.mus;*.mml;*.vcd;*.sng;*.vgm;*.vgz");
        if (!chooser.browseForMultipleFilesToOpen()) {
            return;
        }
        const auto chosen = chooser.getResults();
        if (chosen.isEmpty()) {
            return;
        }

        const int previous_candidates = static_cast<int>(row_keys_.size());
        juce::StringArray errors;
        int added_candidates = 0;
        int added_files = 0;
        for (const auto& file : chosen) {
            juce::MemoryBlock block;
            if (!file.loadFileAsData(block) || block.getSize() == 0) {
                errors.add(
                    file.getFileName()
                    + juce::String::fromUTF8(" を読み込めませんでした"));
                continue;
            }
            const auto* data = static_cast<const std::uint8_t*>(block.getData());
            ImportFileGroup group;
            group.result = mgstc::engine::importTones(
                std::span<const std::uint8_t>(data, block.getSize()),
                utf8String(file.getFileExtension()),
                utf8String(file.getFileName()));
            if (!group.result.errors.empty()) {
                for (const auto& error : group.result.errors) {
                    errors.add(
                        file.getFileName()
                        + juce::String::fromUTF8(": ")
                        + juce::String::fromUTF8(error.c_str()));
                }
            }
            if (group.result.candidates.empty()) {
                continue;
            }
            group.checked.assign(group.result.candidates.size(), 1);
            added_candidates += static_cast<int>(group.result.candidates.size());
            ++added_files;
            files_.push_back(std::move(group));
        }

        rebuildRows();
        if (added_candidates > 0) {
            selectRow(previous_candidates, true);
            viewport_.setViewPosition(
                0,
                std::max(0, list_host_.getHeight() - viewport_.getHeight()));
        }
        if (errors.isEmpty()) {
            return;
        }
        juce::String message;
        const int shown = std::min(8, errors.size());
        for (int index = 0; index < shown; ++index) {
            if (message.isNotEmpty()) {
                message += "\n";
            }
            message += errors[index];
        }
        if (errors.size() > shown) {
            message += juce::String::fromUTF8("\n…他 ")
                + juce::String(errors.size() - shown)
                + juce::String::fromUTF8(" 件");
        }
        if (added_files > 0) {
            message =
                juce::String(added_files)
                + juce::String::fromUTF8(" ファイルをリストへ追加しました。\n")
                + message;
        }
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            juce::String::fromUTF8("インポート"),
            message);
    }

    void rebuildRows() {
        rows_.clear();
        headers_.clear();
        row_keys_.clear();
        list_host_.removeAllChildren();
        for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
            auto& group = files_[file_index];
            auto header = std::make_unique<FileGroupHeader>();
            auto name = juce::String::fromUTF8(group.result.source_name.c_str());
            if (name.isEmpty()) {
                name = juce::String::fromUTF8("(無名)");
            }
            header->setFile(
                name,
                juce::String(
                    mgstc::engine::toneImportFormatName(group.result.format)));
            list_host_.addAndMakeVisible(*header);
            headers_.push_back(std::move(header));
            for (std::size_t candidate_index = 0;
                 candidate_index < group.result.candidates.size();
                 ++candidate_index) {
                auto row = std::make_unique<CandidateRow>(
                    *this,
                    file_index,
                    candidate_index,
                    static_cast<int>(row_keys_.size()));
                list_host_.addAndMakeVisible(*row);
                rows_.push_back(std::move(row));
                row_keys_.push_back({file_index, candidate_index});
            }
        }
        if (row_keys_.empty()) {
            selected_ = -1;
        } else {
            selected_ = juce::jlimit(
                0, static_cast<int>(row_keys_.size()) - 1, selected_);
        }
        updateStatus();
        layoutRows();
    }

    void layoutRows() {
        using namespace UiLayout;
        const int row_h = fieldH + sm;
        const int header_h = fieldH;
        const int width = std::max(1, viewport_.getWidth());
        int y = 0;
        std::size_t row_index = 0;
        for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
            if (file_index < headers_.size()) {
                headers_[file_index]->setBounds(0, y, width, header_h);
                y += header_h;
            }
            const auto count = files_[file_index].result.candidates.size();
            for (std::size_t candidate = 0; candidate < count; ++candidate) {
                if (row_index < rows_.size()) {
                    rows_[row_index]->setBounds(0, y, width, row_h);
                    y += row_h;
                }
                ++row_index;
            }
        }
        list_host_.setSize(width, std::max(row_h, y));
    }

    void updateStatus() {
        if (files_.empty()) {
            status_.setText(
                juce::String::fromUTF8(
                    "ファイルを開くと候補をリストへ追加します（アプリ終了まで保持）"),
                juce::dontSendNotification);
            return;
        }
        status_.setText(
            juce::String(static_cast<int>(files_.size()))
                + juce::String::fromUTF8(" ファイル / ")
                + juce::String(static_cast<int>(row_keys_.size()))
                + juce::String::fromUTF8(" 件の音色"),
            juce::dontSendNotification);
    }

    [[nodiscard]] int flatIndex(
        std::size_t file_index, std::size_t candidate_index) const {
        for (int index = 0; index < static_cast<int>(row_keys_.size()); ++index) {
            if (row_keys_[static_cast<std::size_t>(index)].file_index == file_index
                && row_keys_[static_cast<std::size_t>(index)].candidate_index
                    == candidate_index) {
                return index;
            }
        }
        return -1;
    }

    [[nodiscard]] const mgstc::engine::ImportedToneCandidate* candidateAt(
        int flat) const {
        if (flat < 0 || static_cast<std::size_t>(flat) >= row_keys_.size()) {
            return nullptr;
        }
        const auto& key = row_keys_[static_cast<std::size_t>(flat)];
        if (key.file_index >= files_.size()) {
            return nullptr;
        }
        const auto& candidates = files_[key.file_index].result.candidates;
        if (key.candidate_index >= candidates.size()) {
            return nullptr;
        }
        return &candidates[key.candidate_index];
    }

    void selectRow(int index, bool preview) {
        if (row_keys_.empty()) {
            return;
        }
        index = juce::jlimit(0, static_cast<int>(row_keys_.size()) - 1, index);
        selected_ = index;
        for (auto& row : rows_) {
            row->repaint();
        }
        if (preview && preview_) {
            if (const auto* candidate = candidateAt(index)) {
                preview_(*candidate);
            }
        }
    }

    void showRegisterMenu(std::size_t file_index, std::size_t candidate_index) {
        auto* item = mutableCandidate(file_index, candidate_index);
        if (item == nullptr) {
            return;
        }
        const int flat = flatIndex(file_index, candidate_index);
        if (flat < 0) {
            return;
        }
        juce::PopupMenu menu;
        for (const auto choice : item->register_choices) {
            const int id = static_cast<int>(choice) + 1;
            menu.addItem(id, registerChoiceLabel(choice));
        }
        juce::Component::SafePointer<ToneImportTab> safe(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(
                rows_[static_cast<std::size_t>(flat)].get()),
            [safe, file_index, candidate_index](int result) {
                if (safe == nullptr || result <= 0) {
                    return;
                }
                safe->registerAt(
                    file_index,
                    candidate_index,
                    static_cast<mgstc::engine::ImportRegisterAs>(result - 1));
            });
    }

    static juce::String registerChoiceLabel(
        mgstc::engine::ImportRegisterAs register_as) {
        switch (register_as) {
        case mgstc::engine::ImportRegisterAs::Composite:
            return juce::String::fromUTF8("総合音色として登録");
        case mgstc::engine::ImportRegisterAs::Scc:
            return juce::String::fromUTF8("SCC音色として登録");
        case mgstc::engine::ImportRegisterAs::Opll:
            return juce::String::fromUTF8("OPLL音色として登録");
        }
        return juce::String::fromUTF8("登録");
    }

    void registerChecked() {
        int registered = 0;
        for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
            auto& group = files_[file_index];
            for (std::size_t candidate_index = 0;
                 candidate_index < group.result.candidates.size();
                 ++candidate_index) {
                if (candidate_index >= group.checked.size()
                    || group.checked[candidate_index] == 0) {
                    continue;
                }
                if (group.result.candidates[candidate_index].registered) {
                    continue;
                }
                registerAt(file_index, candidate_index, std::nullopt);
                ++registered;
            }
        }
        if (registered == 0) {
            status_.setText(
                juce::String::fromUTF8("チェックした未登録の音色がありません"),
                juce::dontSendNotification);
        }
    }

    void editAt(std::size_t file_index, std::size_t candidate_index) {
        const auto* item = mutableCandidate(file_index, candidate_index);
        if (item == nullptr || !edit_) {
            return;
        }
        edit_(*item);
    }

    void registerAll() {
        for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
            auto& group = files_[file_index];
            for (std::size_t candidate_index = 0;
                 candidate_index < group.result.candidates.size();
                 ++candidate_index) {
                if (!group.result.candidates[candidate_index].registered) {
                    registerAt(file_index, candidate_index, std::nullopt);
                }
            }
        }
    }

    [[nodiscard]] mgstc::engine::ImportedToneCandidate* mutableCandidate(
        std::size_t file_index, std::size_t candidate_index) {
        if (file_index >= files_.size()) {
            return nullptr;
        }
        auto& candidates = files_[file_index].result.candidates;
        if (candidate_index >= candidates.size()) {
            return nullptr;
        }
        return &candidates[candidate_index];
    }

    void registerAt(
        std::size_t file_index,
        std::size_t candidate_index,
        std::optional<mgstc::engine::ImportRegisterAs> register_as) {
        auto* item = mutableCandidate(file_index, candidate_index);
        if (item == nullptr) {
            return;
        }
        auto& candidate = *item;
        const auto chosen = register_as.value_or(candidate.default_register_as);
        if (!reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("インポート"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み込めませんでした"));
            return;
        }
        const auto now = static_cast<std::int64_t>(
            juce::Time::getCurrentTime().toMilliseconds() / 1000);
        if (chosen == mgstc::engine::ImportRegisterAs::Composite) {
            auto* library = composites_ ? composites_() : nullptr;
            auto timbre = mgstc::engine::makeImportedComposite(candidate, chosen);
            if (library == nullptr || !timbre) {
                return;
            }
            if (!timbre->name.empty()) {
                timbre->name = library->uniqueName(timbre->name);
            }
            library->add(std::move(*timbre), now);
            if (!persist_composites_ || !persist_composites_()) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    juce::String::fromUTF8("インポート"),
                    juce::String::fromUTF8("ライブラリへ保存できませんでした"));
                return;
            }
        } else {
            auto* library = timbres_ ? timbres_() : nullptr;
            if (library == nullptr) {
                return;
            }
            auto entry =
                mgstc::engine::makeImportedLibraryEntry(candidate, chosen);
            if (!entry.name.empty()) {
                entry.name = library->uniqueName(entry.category, entry.name);
            }
            library->add(std::move(entry), now);
            if (!persist_timbres_ || !persist_timbres_()) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    juce::String::fromUTF8("インポート"),
                    juce::String::fromUTF8("ライブラリへ保存できませんでした"));
                return;
            }
        }
        candidate.registered = true;
        const int flat = flatIndex(file_index, candidate_index);
        if (flat >= 0 && static_cast<std::size_t>(flat) < rows_.size()) {
            rows_[static_cast<std::size_t>(flat)]->refreshFromModel();
        }
        if (libraries_changed_) {
            libraries_changed_();
        }
    }

    PreviewCallback preview_;
    EditCallback edit_;
    ReloadCallback reload_;
    PersistCallback persist_timbres_;
    PersistCallback persist_composites_;
    std::function<mgstc::engine::TimbreLibrary*()> timbres_;
    std::function<mgstc::engine::CompositeTimbreLibrary*()> composites_;
    LibrariesChangedCallback libraries_changed_;
    juce::TextButton open_file_;
    juce::Label status_;
    juce::Viewport viewport_;
    juce::Component list_host_;
    juce::TextButton register_selected_;
    juce::TextButton register_all_;
    juce::Rectangle<int> list_bounds_;
    std::vector<ImportFileGroup> files_;
    std::vector<std::unique_ptr<FileGroupHeader>> headers_;
    std::vector<std::unique_ptr<CandidateRow>> rows_;
    std::vector<RowKey> row_keys_;
    int selected_{-1};
};
