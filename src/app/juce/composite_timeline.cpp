// SPDX-License-Identifier: AGPL-3.0-only

#include "composite_timeline.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "composite_envelope_compile.hpp"
#include "juce_chip_tracks.hpp"
#include "juce_utf8.hpp"
#include "last_audition_note.hpp"
#include "library_browser_chrome.hpp"
#include "opll_patch_ui.hpp"
#include "pitch_command_labels.hpp"
#include "rate_envelope_trace.hpp"
#include "shared_audio_host.hpp"
#include "switch_look_and_feel.hpp"
#include "tag_ui.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_focus.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"

#include "mgstc/engine/chip_volume_curve.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/software_lfo.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"
#include "mgstc/engine/volume.hpp"

using mgstc::app::EnvelopeTimbreCatalogItem;
using mgstc::app::EnvelopeTimbreChoice;
using mgstc::app::LayerBaseTimbreAssign;
using mgstc::app::envelopeTimbreCatalogLabel;

namespace {

// #region agent log
void dbg7ae407(
    const char* hypothesisId,
    const char* location,
    const char* message,
    const std::string& dataObject) {
    try {
        std::ofstream out(
            "debug-7ae407.log",
            std::ios::app | std::ios::binary);
        if (!out) {
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"sessionId\":\"7ae407\",\"runId\":\"pre-fix\",\"hypothesisId\":\""
            << hypothesisId << "\",\"location\":\"" << location
            << "\",\"message\":\"" << message << "\",\"data\":" << dataObject
            << ",\"timestamp\":" << ms << "}\n";
    } catch (...) {
    }
}
// #endregion

class EnvelopeNumericEntryContent final : public juce::Component {
public:
    EnvelopeNumericEntryContent(
        const juce::String& prompt,
        const juce::String& initial,
        std::function<void(int)> on_ok,
        bool show_automatic = false,
        bool initial_automatic = false,
        std::function<void(int, bool, bool)> on_volume_ok = {},
        bool initial_precise = false)
        : on_ok_(std::move(on_ok)),
          on_volume_ok_(std::move(on_volume_ok)),
          show_automatic_(show_automatic),
          initial_automatic_(initial_automatic),
          initial_precise_(initial_precise) {
        hint_.setText(prompt, juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        addAndMakeVisible(hint_);
        UiFonts::styleBodyField(editor_);
        editor_.setInputRestrictions(8, "-0123456789");
        editor_.setText(initial, false);
        editor_.setJustification(juce::Justification::centred);
        editor_.onReturnKey = [this] { accept(); };
        addAndMakeVisible(editor_);
        if (show_automatic_) {
            automatic_.setButtonText(
                juce::String::fromUTF8("自動変化"));
            automatic_.setLookAndFeel(&switch_look_and_feel_);
            automatic_.setToggleState(
                initial_automatic_, juce::dontSendNotification);
            automatic_.setEnabled(true);
            automatic_.onClick = [this] { syncPreciseEnabled(); };
            addAndMakeVisible(automatic_);
            precise_.setButtonText(juce::String::fromUTF8("詳細"));
            precise_.setLookAndFeel(&switch_look_and_feel_);
            precise_.setToggleState(
                initial_precise_ && initial_automatic_,
                juce::dontSendNotification);
            precise_.setTooltip(juce::String::fromUTF8(
                "途中のコマンドがあっても、無いときと同じ音量段で変化します。"
                "OFFは従来どおり補間で=を分割します。ONは@eが長くなりやすいです"));
            addAndMakeVisible(precise_);
            syncPreciseEnabled();
        }
        ok_.setButtonText(juce::String::fromUTF8("設定"));
        ok_.onClick = [this] { accept(); };
        addAndMakeVisible(ok_);
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { closeHost(0); };
        addAndMakeVisible(cancel_);
        setSize(
            UiLayout::libraryWidth,
            UiLayout::panelPad * 2
                + UiLayout::fieldH * 2
                + UiLayout::sm
                + (show_automatic_ ? UiLayout::fieldH + UiLayout::sm : 0)
                + UiLayout::textButtonH
                + UiLayout::sm);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        editor_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        if (show_automatic_) {
            auto row = area.removeFromTop(fieldH);
            const int switch_w = juce::jmax(
                1, (row.getWidth() - controlGap) / 2);
            automatic_.setBounds(row.removeFromLeft(switch_w));
            row.removeFromLeft(controlGap);
            precise_.setBounds(row);
            area.removeFromTop(sm);
        }
        auto buttons = area.removeFromTop(textButtonH);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons);
    }

    void visibilityChanged() override {
        if (isShowing()) {
            editor_.grabKeyboardFocus();
        }
    }

private:
    void syncPreciseEnabled() {
        const bool on = automatic_.getToggleState();
        precise_.setEnabled(on);
        if (!on) {
            precise_.setToggleState(false, juce::dontSendNotification);
        }
    }

    void accept() {
        if (on_volume_ok_) {
            const bool automatic_on = automatic_.getToggleState();
            on_volume_ok_(
                editor_.getText().getIntValue(),
                automatic_on,
                automatic_on && precise_.getToggleState());
        } else if (on_ok_) {
            on_ok_(editor_.getText().getIntValue());
        }
        closeHost(1);
    }

    void closeHost(int result) {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(result);
        }
    }

    std::function<void(int)> on_ok_;
    std::function<void(int, bool, bool)> on_volume_ok_;
    bool show_automatic_{};
    bool initial_automatic_{};
    bool initial_precise_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label hint_;
    juce::TextEditor editor_;
    juce::ToggleButton automatic_;
    juce::ToggleButton precise_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

class EnvelopeTimbrePickContent final : public juce::Component {
public:
    EnvelopeTimbrePickContent(
        mgstc::engine::TimbreSource source,
        const std::vector<EnvelopeTimbreCatalogItem>& catalog,
        const EnvelopeTimbreChoice& current,
        std::function<void(EnvelopeTimbreChoice)> on_choose,
        std::function<void(std::uint64_t)> on_edit,
        std::function<void()> on_remove,
        std::function<void()> on_manage = {},
        std::function<bool(std::uint64_t, juce::String)> on_rename = {},
        std::function<bool(std::uint64_t, std::vector<std::string>)>
            on_set_tags = {},
        std::function<bool(std::uint64_t, juce::String)> on_set_memo = {},
        bool layer_base = false)
        : source_(source),
          catalog_(catalog),
          current_(current),
          on_choose_(std::move(on_choose)),
          on_edit_(std::move(on_edit)),
          on_remove_(std::move(on_remove)),
          on_manage_(std::move(on_manage)),
          on_rename_(std::move(on_rename)),
          on_set_tags_(std::move(on_set_tags)),
          on_set_memo_(std::move(on_set_memo)),
          layer_base_(layer_base) {
        preset_label_.setText(
            juce::String::fromUTF8("プリセット音色"),
            juce::dontSendNotification);
        preset_label_.setFont(UiFonts::body());
        addAndMakeVisible(preset_label_);
        preset_.setTextWhenNothingSelected(
            juce::String::fromUTF8("プリセットを選択"));
        if (source_ == mgstc::engine::TimbreSource::Opll) {
            for (int rom = 0; rom < 15; ++rom) {
                preset_.addItem(opllRomPatchLabel(rom), rom + 1);
            }
            if (current_.pick == mgstc::engine::TimbrePick::OpllRom) {
                preset_.setSelectedId(
                    current_.rom_number + 1, juce::dontSendNotification);
            }
        } else if (source_ == mgstc::engine::TimbreSource::Scc) {
            constexpr std::array<const char*, 6> names{
                "Sine", "Square", "Triangle", "Saw",
                "Pulse 25%", "Pulse 12.5%"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                preset_.addItem(
                    juce::String(names[index]),
                    static_cast<int>(index) + 1);
            }
        }
        preset_.setTooltip(
            juce::String::fromUTF8(
                source_ == mgstc::engine::TimbreSource::Opll
                    ? (layer_base_
                           ? "OPLL ROM（@0～@14）。読込でこのチャンネルの基本音色に設定します"
                           : "OPLL ROM（@0～@14）。読込でこのカウントの@に設定します")
                    : "SCC波形プリセット。@割当はライブラリ音色を選んでください"));
        preset_.onChange = [this] {
            if (source_ == mgstc::engine::TimbreSource::Opll
                && preset_.getSelectedId() > 0) {
                list_.setSelectedId(0, juce::dontSendNotification);
            }
            syncDetailFromSelection();
        };
        addAndMakeVisible(preset_);

        edit_.setButtonText(juce::String::fromUTF8("音色を編集"));
        edit_.setTooltip(juce::String::fromUTF8(
            "選択したライブラリ／オリジナル音色を総合音色編集ウィンドウで開きます。"
            "ROMは開けません"));
        edit_.onClick = [this] {
            if (const auto id = selectedLibraryId()) {
                on_edit_(*id);
                closeHost();
            }
        };
        addAndMakeVisible(edit_);

        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { refreshList(); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);
        tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        tag_filter_.onClick = [this] { showTagFilter(); };
        addAndMakeVisible(tag_filter_);
        tag_manage_.setButtonText(
            juce::String::fromUTF8("ライブラリ管理"));
        tag_manage_.setTooltip(juce::String::fromUTF8(
            "音色ライブラリの一覧・複製・削除・タグ管理を行います"));
        tag_manage_.onClick = [this] {
            if (on_manage_) {
                on_manage_();
            }
        };
        tag_manage_.setEnabled(on_manage_ != nullptr);
        addAndMakeVisible(tag_manage_);
        favorite_only_.setButtonText(juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshList(); };
        addAndMakeVisible(favorite_only_);
        ab_.setButtonText("A/B");
        ab_.setTooltip(juce::String::fromUTF8(
            "現在の@割当(A)と一覧で選んだ保存音色(B)を切り替えます"));
        ab_.onClick = [this] { toggleAb(); };
        addAndMakeVisible(ab_);
        sort_.addItem(juce::String::fromUTF8("★優先"), 1);
        sort_.addItem(juce::String::fromUTF8("最近使った順"), 2);
        sort_.addItem(juce::String::fromUTF8("更新日時順"), 3);
        sort_.addItem(juce::String::fromUTF8("名前順"), 4);
        sort_.setSelectedId(1, juce::dontSendNotification);
        sort_.onChange = [this] { refreshList(); };
        addAndMakeVisible(sort_);

        list_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み音色を選択"));
        list_.onChange = [this] {
            if (list_.getSelectedId() > 0
                && source_ == mgstc::engine::TimbreSource::Opll) {
                preset_.setSelectedId(0, juce::dontSendNotification);
            }
            syncDetailFromSelection();
        };
        addAndMakeVisible(list_);
        load_.setButtonText(juce::String::fromUTF8("読込"));
        load_.setTooltip(juce::String::fromUTF8(
            layer_base_
                ? "選択したプリセットまたはライブラリ音色をこのチャンネルの基本音色に設定します"
                : "選択したプリセットまたはライブラリ音色をこのカウントの@に設定します"));
        load_.onClick = [this] {
            if (auto choice = selectedChoice()) {
                on_choose_(*choice);
                closeHost();
            }
        };
        addAndMakeVisible(load_);
        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.setTooltip(juce::String::fromUTF8(
            "このカウントの@イベントを削除します"));
        remove_.onClick = [this] {
            if (on_remove_) {
                on_remove_();
            }
            closeHost();
        };
        addAndMakeVisible(remove_);
        remove_.setVisible(!layer_base_);

        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        UiFonts::styleBodyField(name_);
        addAndMakeVisible(name_);
        rename_.setButtonText(juce::String::fromUTF8("名前変更"));
        rename_.onClick = [this] { renameSelected(); };
        addAndMakeVisible(rename_);
        tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        tags_.onClick = [this] { editSelectedTags(); };
        addAndMakeVisible(tags_);
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        UiFonts::styleBodyField(memo_);
        memo_.onFocusLost = [this] { commitMemo(); };
        addAndMakeVisible(memo_);

        refreshList();
        syncDetailFromSelection();
        setSize(UiLayout::compositeTimbrePickW, UiLayout::compositeTimbrePickH);
    }

    ~EnvelopeTimbrePickContent() override {
        favorite_only_.setLookAndFeel(nullptr);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto preset_row = area.removeFromTop(fieldH);
        preset_label_.setBounds(preset_row.removeFromLeft(UiScale::sx(108)));
        preset_row.removeFromLeft(controlGap);
        preset_.setBounds(preset_row);
        area.removeFromTop(sm);
        edit_.setBounds(area.removeFromTop(textButtonH));
        area.removeFromTop(sm);
        layoutLibraryBrowserChrome(
            area,
            LibraryBrowserChromeWidgets{
                filter_,
                tag_filter_,
                tag_manage_,
                favorite_only_,
                ab_,
                sort_,
                list_,
                load_,
                remove_,
                name_,
                rename_,
                tags_,
                &memo_});
    }

private:
    void closeHost() {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(1);
        }
    }

    [[nodiscard]] std::vector<const EnvelopeTimbreCatalogItem*>
    filteredCatalog() const {
        std::vector<const EnvelopeTimbreCatalogItem*> ordered;
        for (const auto& item : catalog_) {
            if (!favorite_only_.getToggleState() || item.favorite) {
                ordered.push_back(&item);
            }
        }
        const int sort_mode = sort_.getSelectedId();
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [sort_mode](const auto* left, const auto* right) {
                if (left->favorite != right->favorite) {
                    return left->favorite;
                }
                if (sort_mode == 2
                    && left->last_used_unix_seconds
                        != right->last_used_unix_seconds) {
                    return left->last_used_unix_seconds
                        > right->last_used_unix_seconds;
                }
                if (sort_mode == 3
                    && left->updated_unix_seconds
                        != right->updated_unix_seconds) {
                    return left->updated_unix_seconds
                        > right->updated_unix_seconds;
                }
                if (sort_mode == 4) {
                    return left->name < right->name;
                }
                return false;
            });
        const auto filter = filter_.getText().trim();
        std::vector<const EnvelopeTimbreCatalogItem*> visible;
        for (const auto* item : ordered) {
            if (filter.isNotEmpty()) {
                const auto tags = juce::String::fromUTF8(
                    mgstc::engine::serializeTimbreTags(item->tags).c_str());
                if (!item->name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !item->memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    item->tags, filter_tags_)) {
                continue;
            }
            visible.push_back(item);
        }
        return visible;
    }

    void refreshList() {
        const auto previous = selectedLibraryId();
        list_.clear(juce::dontSendNotification);
        visible_ids_.clear();
        int item_id = 1;
        int selected = 0;
        for (const auto* item : filteredCatalog()) {
            visible_ids_.push_back(item->library_id);
            list_.addItem(envelopeTimbreCatalogLabel(*item), item_id);
            if ((previous && *previous == item->library_id)
                || (!previous
                    && current_.library_id == item->library_id
                    && current_.pick
                        == mgstc::engine::TimbrePick::Library)) {
                selected = item_id;
            }
            ++item_id;
        }
        list_.setSelectedId(selected, juce::dontSendNotification);
        ab_.setEnabled(
            current_.library_id != 0 || selectedLibraryId().has_value());
        syncDetailFromSelection();
    }

    void syncDetailFromSelection() {
        syncing_ = true;
        const auto* item = selectedCatalogItem();
        if (item != nullptr) {
            name_.setText(item->name, juce::dontSendNotification);
            memo_.setText(item->memo, juce::dontSendNotification);
            tags_.setButtonText(
                tagSelectionSummary(
                    item->tags,
                    juce::String::fromUTF8("タグを選択")));
        } else {
            name_.clear();
            memo_.clear();
            tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        }
        const bool library_selected = item != nullptr;
        edit_.setEnabled(library_selected);
        rename_.setEnabled(library_selected && on_rename_ != nullptr);
        tags_.setEnabled(library_selected && on_set_tags_ != nullptr);
        memo_.setReadOnly(!library_selected || on_set_memo_ == nullptr);
        load_.setEnabled(selectedChoice().has_value());
        syncing_ = false;
    }

    [[nodiscard]] const EnvelopeTimbreCatalogItem* selectedCatalogItem() const {
        const auto id = list_.getSelectedId();
        if (id <= 0) {
            return nullptr;
        }
        const auto index = static_cast<std::size_t>(id - 1);
        if (index >= visible_ids_.size()) {
            return nullptr;
        }
        const auto library_id = visible_ids_[index];
        for (const auto& item : catalog_) {
            if (item.library_id == library_id) {
                return &item;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::uint64_t> selectedLibraryId() const {
        if (const auto* item = selectedCatalogItem()) {
            return item->library_id;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<EnvelopeTimbreChoice> selectedChoice() const {
        if (source_ == mgstc::engine::TimbreSource::Opll
            && preset_.getSelectedId() > 0) {
            EnvelopeTimbreChoice choice;
            choice.pick = mgstc::engine::TimbrePick::OpllRom;
            choice.rom_number = static_cast<std::uint8_t>(
                juce::jlimit(0, 14, preset_.getSelectedId() - 1));
            return choice;
        }
        if (const auto library_id = selectedLibraryId()) {
            EnvelopeTimbreChoice choice;
            choice.pick = mgstc::engine::TimbrePick::Library;
            choice.library_id = *library_id;
            return choice;
        }
        return std::nullopt;
    }

    void toggleAb() {
        if (ab_showing_b_) {
            selectLibraryId(current_.library_id);
            if (current_.pick == mgstc::engine::TimbrePick::OpllRom) {
                preset_.setSelectedId(
                    current_.rom_number + 1, juce::dontSendNotification);
            }
            ab_showing_b_ = false;
            return;
        }
        ab_b_id_ = selectedLibraryId().value_or(ab_b_id_);
        if (ab_b_id_ != 0) {
            selectLibraryId(ab_b_id_);
            preset_.setSelectedId(0, juce::dontSendNotification);
            ab_showing_b_ = true;
        }
    }

    void selectLibraryId(std::uint64_t library_id) {
        for (int index = 0; index < static_cast<int>(visible_ids_.size());
             ++index) {
            if (visible_ids_[static_cast<std::size_t>(index)] == library_id) {
                list_.setSelectedId(index + 1, juce::dontSendNotification);
                syncDetailFromSelection();
                return;
            }
        }
        list_.setSelectedId(0, juce::dontSendNotification);
        syncDetailFromSelection();
    }

    void showTagFilter() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& item : catalog_) {
            tag_sets.push_back(item.tags);
        }
        juce::Component::SafePointer<EnvelopeTimbrePickContent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("音色ライブラリのタグ検索"),
            filterTagChoices(tag_sets, filter_tags_),
            filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->filter_tags_ = std::move(selected);
                    safe->refreshList();
                }
            });
    }

    void renameSelected() {
        const auto id = selectedLibraryId();
        if (!id || on_rename_ == nullptr) {
            return;
        }
        const auto requested = name_.getText().trim();
        if (requested.isEmpty()) {
            return;
        }
        if (on_rename_(*id, requested)) {
            for (auto& item : catalog_) {
                if (item.library_id == *id) {
                    item.name = requested;
                }
            }
            refreshList();
            selectLibraryId(*id);
        }
    }

    void editSelectedTags() {
        const auto* item = selectedCatalogItem();
        if (item == nullptr || on_set_tags_ == nullptr) {
            return;
        }
        const auto id = item->library_id;
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : catalog_) {
            tag_sets.push_back(entry.tags);
        }
        juce::Component::SafePointer<EnvelopeTimbrePickContent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("タグを選択"),
            filterTagChoices(tag_sets, item->tags),
            item->tags,
            true,
            [safe, id](std::vector<std::string> selected) {
                if (safe == nullptr || !safe->on_set_tags_(id, selected)) {
                    return;
                }
                for (auto& entry : safe->catalog_) {
                    if (entry.library_id == id) {
                        entry.tags = std::move(selected);
                    }
                }
                safe->refreshList();
                safe->selectLibraryId(id);
            });
    }

    void commitMemo() {
        if (syncing_ || on_set_memo_ == nullptr) {
            return;
        }
        const auto id = selectedLibraryId();
        if (!id) {
            return;
        }
        const auto text = memo_.getText();
        if (on_set_memo_(*id, text)) {
            for (auto& item : catalog_) {
                if (item.library_id == *id) {
                    item.memo = text;
                }
            }
        }
    }

    mgstc::engine::TimbreSource source_;
    std::vector<EnvelopeTimbreCatalogItem> catalog_;
    EnvelopeTimbreChoice current_;
    std::vector<std::uint64_t> visible_ids_;
    std::vector<std::string> filter_tags_;
    std::uint64_t ab_b_id_{};
    bool ab_showing_b_{};
    bool syncing_{};
    std::function<void(EnvelopeTimbreChoice)> on_choose_;
    std::function<void(std::uint64_t)> on_edit_;
    std::function<void()> on_remove_;
    std::function<void()> on_manage_;
    std::function<bool(std::uint64_t, juce::String)> on_rename_;
    std::function<bool(std::uint64_t, std::vector<std::string>)> on_set_tags_;
    std::function<bool(std::uint64_t, juce::String)> on_set_memo_;
    bool layer_base_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label preset_label_;
    juce::ComboBox preset_;
    juce::TextButton edit_;
    juce::TextEditor filter_;
    juce::TextButton tag_filter_;
    juce::TextButton tag_manage_;
    juce::ToggleButton favorite_only_;
    juce::TextButton ab_;
    juce::ComboBox sort_;
    juce::ComboBox list_;
    juce::TextButton load_;
    juce::TextButton remove_;
    juce::TextEditor name_;
    juce::TextButton rename_;
    juce::TextButton tags_;
    juce::TextEditor memo_;
};
class EnvelopeOpllYParamContent final
    : public juce::Component,
      private juce::Timer {
public:
    EnvelopeOpllYParamContent(
        const mgstc::engine::OpllPatchParameters& baseline,
        const mgstc::engine::OpllPatchParameters& initial,
        bool editing_existing,
        std::function<void(mgstc::engine::OpllPatchParameters)> on_apply,
        SharedAudioHost* audio_service = nullptr)
        : on_apply_(std::move(on_apply)),
          audio_service_(audio_service),
          panel_(switch_look_and_feel_) {
        title_.setText(
            juce::String::fromUTF8(
                editing_existing
                    ? "手動 y（FMパラメータ）— 編集"
                    : "手動 y（FMパラメータ）— 新規"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        hint_.setText(
            juce::String::fromUTF8(
                editing_existing
                    ? "琥珀色＝このステップの手動 y 群で変わったフィールド。"
                      "表示はTL/FB自動適用後。値変更で1秒試聴。"
                      "決定時は変更レジスタごとに最大1本の y"
                    : "OPLLエディタと同じレイアウト（表示はTL/FB自動込み）。"
                      "値変更で1秒試聴。変更レジスタごとに最大1本の yreg,data"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        addAndMakeVisible(hint_);

        panel_.setDiffBaseline(baseline);
        panel_.setAuditionNote(
            static_cast<std::uint8_t>(LastAuditionNoteStore::instance().get()));
        panel_.setParameters(initial);
        panel_.onChange = [this](bool commit) {
            juce::ignoreUnused(commit);
            auditionPreview();
        };
        addAndMakeVisible(panel_);

        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            stopPreviewAndRestore();
            if (on_apply_) {
                on_apply_(panel_.parameters());
            }
            closeHost(1);
        };
        addAndMakeVisible(ok_);
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            stopPreviewAndRestore();
            closeHost(0);
        };
        addAndMakeVisible(cancel_);

        setSize(UiLayout::compositeOpllYParamW, UiLayout::compositeOpllYParamH);
    }

    ~EnvelopeOpllYParamContent() override {
        stopPreviewAndRestore();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);

        auto buttons = area.removeFromBottom(textButtonH);
        area.removeFromBottom(sm);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons.removeFromLeft(button_w));

        panel_.setBounds(area);
    }

private:
    void closeHost(int result) {
        if (auto* window =
                findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(result);
        }
    }

    void silencePreviewTracks() {
        if (audio_service_ == nullptr) {
            return;
        }
        auto& engine = audio_service_->engine();
        for (std::uint8_t channel = 0; channel < 9; ++channel) {
            const auto track = static_cast<std::uint8_t>(kOpllTrack + channel);
            static_cast<void>(engine.submit(
                mgstc::engine::EngineCommand::noteOff(track)));
            audio_service_->armOpllKeyOffForceSilence(track);
        }
    }

    void auditionPreview() {
        if (audio_service_ == nullptr || !audio_service_->running()) {
            return;
        }
        const auto note = static_cast<std::uint8_t>(
            LastAuditionNoteStore::instance().get());
        panel_.setAuditionNote(note);
        const auto patch = panel_.parameters();
        silencePreviewTracks();
        audio_service_->clearOpllKeyOffForceSilence();
        temporary_program_ = audio_service_->submitSharedEditorProgram(
            audio_service_->sharedSccWaveform(),
            patch,
            true,
            kOpllTrack,
            note);
        if (temporary_program_) {
            startTimer(1000);
        }
    }

    void stopPreviewAndRestore() {
        stopTimer();
        if (audio_service_ == nullptr || !temporary_program_) {
            temporary_program_ = false;
            return;
        }
        silencePreviewTracks();
        static_cast<void>(audio_service_->submitSharedEditorProgram(
            audio_service_->sharedSccWaveform(),
            audio_service_->sharedOpllPatch(),
            false,
            kOpllTrack,
            static_cast<std::uint8_t>(
                LastAuditionNoteStore::instance().get())));
        temporary_program_ = false;
    }

    void timerCallback() override {
        stopPreviewAndRestore();
    }

    std::function<void(mgstc::engine::OpllPatchParameters)> on_apply_;
    SharedAudioHost* audio_service_{};
    bool temporary_program_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label title_;
    juce::Label hint_;
    OpllPatchParameterPanel panel_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};


class EnvelopePsgYParamContent final : public juce::Component {
public:
    EnvelopePsgYParamContent(
        std::uint8_t reg,
        std::uint8_t data,
        std::function<void(std::uint8_t, std::uint8_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        hint_.setText(
            juce::String::fromUTF8("PSG 手動 y（yreg,data）"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        addAndMakeVisible(hint_);
        reg_label_.setText("reg", juce::dontSendNotification);
        data_label_.setText("data", juce::dontSendNotification);
        for (auto* label : {&reg_label_, &data_label_}) {
            label->setFont(UiFonts::body());
            label->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(label);
        }
        UiFonts::styleBodyField(reg_);
        UiFonts::styleBodyField(data_);
        reg_.setInputRestrictions(2, "0123456789");
        data_.setInputRestrictions(3, "0123456789");
        reg_.setText(juce::String(static_cast<int>(reg)), false);
        data_.setText(juce::String(static_cast<int>(data)), false);
        addAndMakeVisible(reg_);
        addAndMakeVisible(data_);
        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            const auto r = static_cast<std::uint8_t>(juce::jlimit(
                0, 15, reg_.getText().getIntValue()));
            const auto d = static_cast<std::uint8_t>(juce::jlimit(
                0, 255, data_.getText().getIntValue()));
            if (on_apply_) {
                on_apply_(r, d);
            }
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(1);
            }
        };
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);
        setSize(UiLayout::compositeTimbrePickW, UiLayout::compositeTimbrePickH);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        auto row = area.removeFromTop(fieldH);
        reg_label_.setBounds(row.removeFromLeft(UiScale::sx(48)));
        reg_.setBounds(row.removeFromLeft(UiScale::sx(72)));
        row.removeFromLeft(sm);
        data_label_.setBounds(row.removeFromLeft(UiScale::sx(48)));
        data_.setBounds(row.removeFromLeft(UiScale::sx(80)));
        area.removeFromTop(sm);
        auto buttons = area.removeFromTop(textButtonH);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons.removeFromLeft(button_w));
    }

private:
    std::function<void(std::uint8_t, std::uint8_t)> on_apply_;
    juce::Label hint_;
    juce::Label reg_label_;
    juce::Label data_label_;
    juce::TextEditor reg_;
    juce::TextEditor data_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

class SoftwareLfoWavePreview final : public juce::Component {
public:
    void setSettings(
        mgstc::engine::SoftwareLfoSettings settings,
        bool apply_extra_roughness) {
        settings_ = settings;
        apply_extra_roughness_ = apply_extra_roughness;
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0F);
        graphics.setColour(juce::Colour(UiLayout::panelFill));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);

        const auto graph = bounds.reduced(6.0F, 6.0F);
        constexpr int kSeconds = 4;
        const int interrupt_hz = juce::roundToInt(
            mgstc::engine::kMgscInterruptHz);
        const int tick_count = interrupt_hz * kSeconds;
        graphics.setColour(juce::Colour(0xFF586675));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX(),
            graph.getRight());
        graphics.setFont(UiFonts::dense());
        for (int second = 1; second < kSeconds; ++second) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(second * interrupt_hz)
                    / static_cast<float>(tick_count);
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                graph.getY(),
                graph.getBottom());
            graphics.setColour(juce::Colour(0xFF8896A3));
            graphics.drawText(
                juce::String(second) + "s",
                juce::roundToInt(x) + 3,
                juce::roundToInt(graph.getY()) + 1,
                28,
                14,
                juce::Justification::centredLeft,
                false);
            graphics.setColour(juce::Colour(0xFF586675));
        }

        const int extent = juce::jmax(
            1,
            mgstc::engine::softwareLfoDisplayExtent(
                settings_, apply_extra_roughness_));
        juce::Path path;
        for (int tick = 0; tick <= tick_count; ++tick) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(tick)
                    / static_cast<float>(tick_count);
            const float offset = static_cast<float>(
                mgstc::engine::softwareLfoOffsetAtTick(
                    settings_,
                    static_cast<std::uint32_t>(tick),
                    apply_extra_roughness_));
            const float y = graph.getCentreY()
                - (offset / static_cast<float>(extent))
                    * graph.getHeight() * 0.45F;
            if (tick == 0) {
                path.startNewSubPath(x, y);
            } else {
                path.lineTo(x, y);
            }
        }
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.strokePath(path, juce::PathStrokeType(1.5F));
    }

private:
    mgstc::engine::SoftwareLfoSettings settings_{};
    bool apply_extra_roughness_{true};
};

class SoftwareLfoDialogContent final
    : public juce::Component,
      private juce::Timer {
public:
    using ChangeCallback =
        std::function<void(const mgstc::engine::SoftwareLfoSettings&)>;
    using PreviewCallback = std::function<void()>;
    using PollKeysCallback = std::function<void()>;

    SoftwareLfoDialogContent(
        mgstc::engine::TimbreSource source,
        mgstc::engine::SoftwareLfoSettings initial,
        ChangeCallback on_change,
        PreviewCallback on_preview,
        PollKeysCallback on_poll_keys)
        : source_(source),
          on_change_(std::move(on_change)),
          on_preview_(std::move(on_preview)),
          on_poll_keys_(std::move(on_poll_keys)) {
        title_.setText(
            juce::String::fromUTF8("MGSDRV ソフトウェア LFO（h）"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        enabled_.setButtonText(juce::String::fromUTF8("有効"));
        enabled_.setTooltip(juce::String::fromUTF8(
            "発音前トラック MML の h を出す。オフでは LFO なし"));
        enabled_.setLookAndFeel(&switch_look_and_feel_);
        enabled_.onClick = [this] { commitFromEditors(); };
        addAndMakeVisible(enabled_);

        configureParam(
            delay_,
            delay_label_,
            juce::String::fromUTF8("ディレイ"),
            0.0,
            255.0,
            juce::String::fromUTF8(
                "n1。キーオンから三角波開始までの約1/60秒カウント（0～255）"));
        configureParam(
            depth_,
            depth_label_,
            juce::String::fromUTF8("深さ"),
            0.0,
            127.0,
            juce::String::fromUTF8(
                "n2。三角波の振幅（0～127）。音程オフセット単位"));
        configureParam(
            speed_,
            speed_label_,
            juce::String::fromUTF8("速度"),
            0.0,
            255.0,
            juce::String::fromUTF8(
                "n3。割り込みごとの位相進み（0～255）"));
        configureParam(
            roughness_,
            roughness_label_,
            juce::String::fromUTF8("粗さ"),
            -127.0,
            127.0,
            juce::String::fromUTF8(
                "n4。三角波の1ステップ（−127～127）。"
                "正で先に上がり、負で先に下がる"));
        configureParam(
            extra_roughness_,
            extra_roughness_label_,
            juce::String::fromUTF8("@p"),
            -32768.0,
            32767.0,
            juce::String::fromUTF8(
                "LFO の粗さを細かく設定（PSG／SCCのみ。−32768～32767）"));
        extra_roughness_.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 64, UiLayout::fieldH - 10);

        hint_.setText(
            juce::String::fromUTF8(
                "波形は MGSDRV 3.20 互換の三角波（約60Hz）。"
                "縦線は1秒。MIDI／PCキーでも当該チャンネルの音色で演奏できます"
                "（鍵盤は表示しません）。"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        addAndMakeVisible(hint_);
        addAndMakeVisible(wave_);

        preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
        preview_.onClick = [this] {
            commitFromEditors();
            if (on_preview_) {
                on_preview_();
            }
        };
        addAndMakeVisible(preview_);

        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] {
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(close_);

        assignFromSettings(initial);
        extra_roughness_.setEnabled(
            source_ != mgstc::engine::TimbreSource::Opll);
        extra_roughness_label_.setEnabled(extra_roughness_.isEnabled());
        setWantsKeyboardFocus(true);
        setSize(
            UiLayout::compositeLfoDialogW,
            UiLayout::compositeLfoDialogH);
        startTimerHz(15);
    }

    ~SoftwareLfoDialogContent() override {
        enabled_.setLookAndFeel(nullptr);
        stopTimer();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        enabled_.setBounds(
            area.removeFromTop(textButtonH).removeFromLeft(
                switchControlWidth(
                    juce::String::fromUTF8("有効"), textButtonH)));
        area.removeFromTop(sm);
        layoutParam(area.removeFromTop(fieldH), delay_label_, delay_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), depth_label_, depth_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), speed_label_, speed_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), roughness_label_, roughness_);
        area.removeFromTop(controlGap);
        if (source_ != mgstc::engine::TimbreSource::Opll) {
            layoutParam(
                area.removeFromTop(fieldH), extra_roughness_label_, extra_roughness_);
            area.removeFromTop(controlGap);
        } else {
            extra_roughness_.setVisible(false);
            extra_roughness_label_.setVisible(false);
        }
        wave_.setBounds(area.removeFromTop(compositeLfoWaveH));
        area.removeFromTop(sm);
        hint_.setBounds(area.removeFromTop(fieldH + sm));
        area.removeFromTop(sm);
        auto buttons = area.removeFromTop(textButtonH);
        preview_.setBounds(buttons.removeFromLeft(libraryButtonMinW + 24));
        buttons.removeFromLeft(controlGap);
        close_.setBounds(buttons.removeFromLeft(libraryButtonMinW));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        juce::ignoreUnused(key);
        if (on_poll_keys_) {
            on_poll_keys_();
        }
        return true;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
        return false;
    }

private:
    void timerCallback() override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
    }

    void configureParam(
        juce::Slider& slider,
        juce::Label& label,
        const juce::String& name,
        double minimum,
        double maximum,
        const juce::String& tooltip) {
        label.setText(name, juce::dontSendNotification);
        label.setFont(UiFonts::body());
        label.setJustificationType(juce::Justification::centredLeft);
        label.setTooltip(tooltip);
        addAndMakeVisible(label);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 48, UiLayout::fieldH - 10);
        slider.setTooltip(tooltip);
        slider.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors();
            }
        };
        addAndMakeVisible(slider);
    }

    static void layoutParam(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider) {
        label.setBounds(area.removeFromLeft(UiLayout::setupValueLabelW));
        area.removeFromLeft(UiLayout::xs);
        slider.setBounds(area);
    }

    void assignFromSettings(mgstc::engine::SoftwareLfoSettings settings) {
        assigning_ = true;
        const bool extra = source_ != mgstc::engine::TimbreSource::Opll;
        settings = mgstc::engine::clampSoftwareLfo(settings, extra);
        enabled_.setToggleState(settings.enabled, juce::dontSendNotification);
        delay_.setValue(settings.delay, juce::dontSendNotification);
        depth_.setValue(settings.depth, juce::dontSendNotification);
        speed_.setValue(settings.speed, juce::dontSendNotification);
        roughness_.setValue(settings.roughness, juce::dontSendNotification);
        extra_roughness_.setValue(
            settings.extra_roughness, juce::dontSendNotification);
        wave_.setSettings(settings, extra);
        assigning_ = false;
    }

    void commitFromEditors() {
        const bool extra = source_ != mgstc::engine::TimbreSource::Opll;
        mgstc::engine::SoftwareLfoSettings settings;
        settings.enabled = enabled_.getToggleState();
        settings.delay = static_cast<std::uint8_t>(delay_.getValue());
        settings.depth = static_cast<std::uint8_t>(depth_.getValue());
        settings.speed = static_cast<std::uint8_t>(speed_.getValue());
        settings.roughness = static_cast<std::int8_t>(roughness_.getValue());
        settings.extra_roughness = extra
            ? static_cast<std::int32_t>(extra_roughness_.getValue())
            : 0;
        settings = mgstc::engine::clampSoftwareLfo(settings, extra);
        wave_.setSettings(settings, extra);
        if (on_change_) {
            on_change_(settings);
        }
    }

    mgstc::engine::TimbreSource source_{};
    ChangeCallback on_change_;
    PreviewCallback on_preview_;
    PollKeysCallback on_poll_keys_;
    bool assigning_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label title_;
    juce::ToggleButton enabled_;
    juce::Label delay_label_;
    juce::Label depth_label_;
    juce::Label speed_label_;
    juce::Label roughness_label_;
    juce::Label extra_roughness_label_;
    juce::Slider delay_;
    juce::Slider depth_;
    juce::Slider speed_;
    juce::Slider roughness_;
    juce::Slider extra_roughness_;
    SoftwareLfoWavePreview wave_;
    juce::Label hint_;
    juce::TextButton preview_;
    juce::TextButton close_;
};

[[nodiscard]] juce::Rectangle<float> rateEnvelopePlotRect(
    juce::Rectangle<int> bounds) {
    return bounds.toFloat().reduced(0.5F);
}

[[nodiscard]] float rateEnvelopeXFromSeconds(
    juce::Rectangle<float> graph,
    float seconds) {
    const float duration = mgstc::engine::RateEnvelopeTrace::kDurationSeconds;
    return juce::jmap(
        juce::jlimit(0.0F, duration, seconds),
        0.0F,
        duration,
        graph.getX() + 2.0F,
        graph.getRight() - 2.0F);
}

[[nodiscard]] float rateEnvelopeYFromLevel(
    juce::Rectangle<float> graph,
    std::uint8_t level) {
    return juce::jmap(
        static_cast<float>(level),
        0.0F,
        255.0F,
        graph.getBottom() - 3.0F,
        graph.getY() + 3.0F);
}

[[nodiscard]] float rateEnvelopeSecondsFromX(
    juce::Rectangle<float> graph,
    float x) {
    return juce::jmap(
        juce::jlimit(graph.getX() + 2.0F, graph.getRight() - 2.0F, x),
        graph.getX() + 2.0F,
        graph.getRight() - 2.0F,
        0.0F,
        mgstc::engine::RateEnvelopeTrace::kDurationSeconds);
}

[[nodiscard]] std::uint8_t rateEnvelopeLevelFromY(
    juce::Rectangle<float> graph,
    float y) {
    return static_cast<std::uint8_t>(juce::jlimit(
        0,
        255,
        juce::roundToInt(
            juce::jmap(
                y,
                graph.getBottom() - 3.0F,
                graph.getY() + 3.0F,
                0.0F,
                255.0F))));
}

void paintRateEnvelopeHandles(
    juce::Graphics& graphics,
    juce::Rectangle<float> graph,
    const mgstc::engine::RateEnvelopeHandleLayout& handles,
    int hover_index,
    int active_index) {
    const float radius = static_cast<float>(UiLayout::rateEnvelopeHandleR);
    for (int index = 0;
         index < static_cast<int>(handles.handles.size());
         ++index) {
        const auto& handle = handles.handles[static_cast<std::size_t>(index)];
        if (!handle.available) {
            continue;
        }
        const auto centre = juce::Point<float>(
            rateEnvelopeXFromSeconds(graph, handle.seconds),
            rateEnvelopeYFromLevel(graph, handle.level));
        const bool hot = index == hover_index || index == active_index;
        graphics.setColour(
            hot
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(0xFFD66B6B));
        graphics.fillEllipse(
            centre.x - radius,
            centre.y - radius,
            radius * 2.0F,
            radius * 2.0F);
        graphics.setColour(juce::Colour(0xFF111920).withAlpha(0.85F));
        graphics.drawEllipse(
            centre.x - radius,
            centre.y - radius,
            radius * 2.0F,
            radius * 2.0F,
            1.2F);
    }
}

[[nodiscard]] juce::Path rateEnvelopeLevelPolyline(
    juce::Rectangle<float> graph,
    const mgstc::engine::RateEnvelopeHandleLayout& layout) {
    struct Point {
        float seconds{};
        std::uint8_t level{};
    };
    std::vector<Point> points;
    for (const auto& handle : layout.handles) {
        if (!handle.available) {
            continue;
        }
        points.push_back({handle.seconds, handle.level});
    }
    std::stable_sort(
        points.begin(),
        points.end(),
        [](const Point& left, const Point& right) {
            return left.seconds < right.seconds;
        });
    std::vector<Point> unique;
    for (const auto& point : points) {
        if (!unique.empty()
            && std::abs(unique.back().seconds - point.seconds) < 1.0e-4F
            && unique.back().level == point.level) {
            continue;
        }
        unique.push_back(point);
    }
    if (unique.empty()) {
        return {};
    }
    const float duration = mgstc::engine::RateEnvelopeTrace::kDurationSeconds;
    if (unique.front().seconds > 0.0F) {
        unique.insert(
            unique.begin(),
            {0.0F, unique.front().level});
    }
    if (unique.back().seconds < duration) {
        unique.push_back({duration, unique.back().level});
    }
    juce::Path path;
    path.startNewSubPath(
        rateEnvelopeXFromSeconds(graph, unique.front().seconds),
        rateEnvelopeYFromLevel(graph, unique.front().level));
    for (std::size_t index = 1; index < unique.size(); ++index) {
        path.lineTo(
            rateEnvelopeXFromSeconds(graph, unique[index].seconds),
            rateEnvelopeYFromLevel(graph, unique[index].level));
    }
    return path;
}

[[nodiscard]] mgstc::engine::RateEnvelopeHandleLayout
rateEnvelopeHandlesForLayer(const mgstc::engine::CompositeLayer& layer) {
    return mgstc::engine::rateEnvelopeHandleLayout(
        mgstc::app::rateDefinitionFrom(
            mgstc::engine::clampRateEnvelope(layer.volume_envelope.rate)),
        layer.source != mgstc::engine::TimbreSource::Opll);
}

void paintRateEnvelopeGraph(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds,
    const mgstc::engine::RateEnvelopeTrace& trace,
    juce::Colour colour,
    mgstc::engine::TimbreSource source,
    bool draw_labels,
    const mgstc::engine::RateEnvelopeHandleLayout* handles = nullptr,
    int hover_index = -1,
    int active_index = -1) {
    const auto graph = rateEnvelopePlotRect(bounds);
    graphics.setColour(juce::Colour(0xFF111920));
    graphics.fillRoundedRectangle(graph, 5.0F);
    graphics.setColour(juce::Colour(0xFF435160));
    graphics.drawRoundedRectangle(graph, 5.0F, 1.0F);
    const float key_off_x = rateEnvelopeXFromSeconds(
        graph, mgstc::engine::RateEnvelopeTrace::kKeyOffSeconds);
    const float dash_pattern[]{4.0F, 3.0F};
    graphics.setColour(juce::Colour(0xFFD66B6B));
    graphics.drawDashedLine(
        {key_off_x, graph.getY() + 1.0F, key_off_x, graph.getBottom() - 1.0F},
        dash_pattern,
        2,
        1.0F);
    if (trace.valid) {
        juce::Path quantized;
        const int lane_h = juce::jmax(
            1, juce::roundToInt(graph.getHeight() - 6.0F));
        const float graph_bottom = graph.getBottom() - 3.0F;
        for (std::size_t index = 0; index < trace.level.size(); ++index) {
            const float x = juce::jmap(
                static_cast<float>(index),
                0.0F,
                static_cast<float>(trace.level.size() - 1),
                graph.getX() + 2.0F,
                graph.getRight() - 2.0F);
            const int nibble = juce::jlimit(
                0,
                15,
                juce::roundToInt(trace.quantized[index] * 15.0F));
            const float qy = graph_bottom
                - static_cast<float>(
                      mgstc::engine::chipVolumeDisplayPixelsFromBottom(
                          source, nibble, lane_h));
            if (index == 0) {
                quantized.startNewSubPath(x, qy);
            } else {
                quantized.lineTo(x, qy);
            }
        }
        juce::Path level;
        if (handles != nullptr) {
            level = rateEnvelopeLevelPolyline(graph, *handles);
        }
        if (!level.isEmpty()) {
            juce::Path filled = level;
            filled.lineTo(graph.getRight() - 2.0F, graph.getBottom() - 3.0F);
            filled.lineTo(graph.getX() + 2.0F, graph.getBottom() - 3.0F);
            filled.closeSubPath();
            graphics.setColour(colour.withAlpha(0.55F));
            graphics.fillPath(filled);
        }
        juce::Path chip_fill = quantized;
        chip_fill.lineTo(graph.getRight() - 2.0F, graph.getBottom() - 3.0F);
        chip_fill.lineTo(graph.getX() + 2.0F, graph.getBottom() - 3.0F);
        chip_fill.closeSubPath();
        graphics.setColour(colour.withAlpha(0.32F));
        graphics.fillPath(chip_fill);
        graphics.setColour(colour.withAlpha(0.55F));
        graphics.strokePath(
            quantized,
            juce::PathStrokeType(
                2.2F,
                juce::PathStrokeType::mitered,
                juce::PathStrokeType::butt));
        if (!level.isEmpty()) {
            graphics.setColour(colour);
            graphics.strokePath(
                level,
                juce::PathStrokeType(
                    1.7F,
                    juce::PathStrokeType::mitered,
                    juce::PathStrokeType::butt));
        }
    }
    if (handles != nullptr) {
        paintRateEnvelopeHandles(
            graphics, graph, *handles, hover_index, active_index);
    }
    if (!draw_labels) {
        return;
    }
    graphics.setFont(UiFonts::dense());
    graphics.setColour(juce::Colour(0xFF9BA8B2));
    graphics.drawText(
        "0s",
        bounds.reduced(UiScale::sx(5)).removeFromBottom(UiScale::sx(15)),
        juce::Justification::bottomLeft);
    graphics.drawText(
        juce::String(
            juce::roundToInt(
                mgstc::engine::RateEnvelopeTrace::kDurationSeconds))
            + "s",
        bounds.reduced(UiScale::sx(5)).removeFromBottom(UiScale::sx(15)),
        juce::Justification::bottomRight);
    graphics.setColour(juce::Colour(0xFFD98A8A));
    graphics.drawText(
        "KO 1s",
        juce::Rectangle<int>(
            juce::roundToInt(key_off_x) + UiScale::sx(4),
            bounds.getBottom() - UiScale::sx(21),
            UiScale::sx(48),
            UiScale::sx(16)),
        juce::Justification::centredLeft);
}

class RateEnvelopeGraphView final : public juce::Component {
public:
    using DragCallback = std::function<void(
        mgstc::engine::RateEnvelopeHandleKind,
        float,
        std::uint8_t,
        bool)>;

    void setTrace(
        mgstc::engine::RateEnvelopeTrace trace,
        juce::Colour colour,
        mgstc::engine::TimbreSource source,
        mgstc::engine::RateEnvelopeHandleLayout handles,
        bool interactive) {
        trace_ = trace;
        colour_ = colour;
        source_ = source;
        handles_ = handles;
        interactive_ = interactive;
        if (!interactive_) {
            hover_index_ = -1;
            drag_index_ = -1;
        }
        repaint();
    }

    void setDragCallback(DragCallback callback) {
        on_drag_ = std::move(callback);
    }

    void paint(juce::Graphics& graphics) override {
        paintRateEnvelopeGraph(
            graphics,
            getLocalBounds(),
            trace_,
            colour_,
            source_,
            true,
            &handles_,
            hover_index_,
            drag_index_);
    }

    void mouseMove(const juce::MouseEvent& event) override {
        if (!interactive_) {
            return;
        }
        hover_index_ = hitHandle(event.position);
        setMouseCursor(
            hover_index_ >= 0
                ? juce::MouseCursor::DraggingHandCursor
                : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (drag_index_ < 0) {
            hover_index_ = -1;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!interactive_) {
            return;
        }
        drag_index_ = hitHandle(event.position);
        hover_index_ = drag_index_;
        if (drag_index_ >= 0) {
            applyDrag(event.position, false);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (drag_index_ < 0) {
            return;
        }
        applyDrag(event.position, false);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (drag_index_ < 0) {
            return;
        }
        applyDrag(event.position, true);
        drag_index_ = -1;
        hover_index_ = hitHandle(event.position);
        setMouseCursor(
            hover_index_ >= 0
                ? juce::MouseCursor::DraggingHandCursor
                : juce::MouseCursor::NormalCursor);
        repaint();
    }

private:
    [[nodiscard]] int hitHandle(juce::Point<float> position) const {
        const auto graph = rateEnvelopePlotRect(getLocalBounds());
        const float radius =
            static_cast<float>(UiLayout::rateEnvelopeHandleR) + 4.0F;
        int best = -1;
        float best_distance = radius * radius;
        for (int index = 0;
             index < static_cast<int>(handles_.handles.size());
             ++index) {
            const auto& handle =
                handles_.handles[static_cast<std::size_t>(index)];
            if (!handle.available) {
                continue;
            }
            const auto centre = juce::Point<float>(
                rateEnvelopeXFromSeconds(graph, handle.seconds),
                rateEnvelopeYFromLevel(graph, handle.level));
            const float distance = centre.getDistanceSquaredFrom(position);
            if (distance <= best_distance) {
                best = index;
                best_distance = distance;
            }
        }
        return best;
    }

    void applyDrag(juce::Point<float> position, bool commit) {
        if (drag_index_ < 0 || !on_drag_) {
            return;
        }
        const auto graph = rateEnvelopePlotRect(getLocalBounds());
        const auto kind =
            handles_.handles[static_cast<std::size_t>(drag_index_)].kind;
        on_drag_(
            kind,
            rateEnvelopeSecondsFromX(graph, position.x),
            rateEnvelopeLevelFromY(graph, position.y),
            commit);
    }

    mgstc::engine::RateEnvelopeTrace trace_{};
    juce::Colour colour_{0xFF53E3A6};
    mgstc::engine::TimbreSource source_{mgstc::engine::TimbreSource::Scc};
    mgstc::engine::RateEnvelopeHandleLayout handles_{};
    DragCallback on_drag_;
    bool interactive_{};
    int hover_index_{-1};
    int drag_index_{-1};
};

[[nodiscard]] juce::Colour colourForTimbreSource(
    mgstc::engine::TimbreSource source) {
    if (source == mgstc::engine::TimbreSource::Psg) {
        return juce::Colour(0xFFB990FF);
    }
    if (source == mgstc::engine::TimbreSource::Scc) {
        return juce::Colour(0xFF53E3A6);
    }
    return juce::Colour(0xFFFFA75E);
}

class RateEnvelopeEditorContent final
    : public juce::Component,
      private juce::Timer {
public:
    using ChangeCallback =
        std::function<void(const mgstc::engine::RateEnvelope&, bool)>;
    using PreviewCallback = std::function<void()>;
    using PollKeysCallback = std::function<void()>;

    RateEnvelopeEditorContent(
        const mgstc::engine::CompositeLayer& layer,
        ChangeCallback on_change,
        PreviewCallback on_preview,
        PollKeysCallback on_poll_keys)
        : source_(layer.source),
          on_change_(std::move(on_change)),
          on_preview_(std::move(on_preview)),
          on_poll_keys_(std::move(on_poll_keys)) {
        title_.setText(
            juce::String::fromUTF8("レートエンベロープ（@r）"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);
        hint_.setText(
            juce::String::fromUTF8(
                "曲線上の赤い●をドラッグして AL〜RR を同時に動かせます。"
                "AL〜RRは1割り込みあたりの8bit増減。赤い線はプレビュー用の"
                "1秒キーオフ（定義には保存しません）。"
                "PCキー／MIDIで総合ミックスを演奏できます（この窓に鍵盤は出しません）。"
                "バー・ハンドル・Mode／Noiseの確定で1秒プレビューします。"
                "「試聴（1秒）」でも1秒プレビューします。"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setMinimumHorizontalScale(1.0F);
        addAndMakeVisible(hint_);
        configureBar(attack_level_, attack_level_label_, "AL");
        configureBar(attack_rate_, attack_rate_label_, "AR");
        configureBar(decay_rate_, decay_rate_label_, "DR");
        configureBar(sustain_level_, sustain_level_label_, "SL");
        configureBar(sustain_rate_, sustain_rate_label_, "SR");
        configureBar(release_rate_, release_rate_label_, "RR");
        mode_.addItem(juce::String::fromUTF8("/0 オフ"), 1);
        mode_.addItem(juce::String::fromUTF8("/1 トーン"), 2);
        mode_.addItem(juce::String::fromUTF8("/2 ノイズ"), 3);
        mode_.addItem(juce::String::fromUTF8("/3 トーン+ノイズ"), 4);
        mode_.setTooltip(juce::String::fromUTF8(
            "PSG Tone/Noise モード（@r 定義の Mode。0～3）"));
        mode_.onChange = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(mode_);
        mode_label_.setText(
            juce::String::fromUTF8("Mode"), juce::dontSendNotification);
        mode_label_.setFont(UiFonts::body());
        addAndMakeVisible(mode_label_);
        noise_label_.setText(
            juce::String::fromUTF8("Noise"), juce::dontSendNotification);
        noise_label_.setFont(UiFonts::body());
        addAndMakeVisible(noise_label_);
        noise_.setRange(0.0, 31.0, 1.0);
        noise_.setSliderStyle(juce::Slider::LinearHorizontal);
        noise_.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 48, UiLayout::fieldH - 10);
        noise_.setTooltip(juce::String::fromUTF8(
            "PSG ノイズ周期（@r 定義の Noise。0～31）"));
        noise_.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors(false);
            }
        };
        noise_.onDragEnd = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(noise_);
        addAndMakeVisible(graph_);
        graph_.setDragCallback(
            [this](
                mgstc::engine::RateEnvelopeHandleKind kind,
                float seconds,
                std::uint8_t level,
                bool commit) {
                applyHandleDrag(kind, seconds, level, commit);
            });
        preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
        preview_.setTooltip(juce::String::fromUTF8(
            "最後の試聴音程で1秒キーオンします。離すと @r の Release が続きます"));
        preview_.onClick = [this] {
            commitFromEditors(true);
        };
        addAndMakeVisible(preview_);
        assignFromLayer(layer);
        setWantsKeyboardFocus(true);
        fitToContent();
        startTimerHz(15);
    }

    void assignFromLayer(const mgstc::engine::CompositeLayer& layer) {
        assigning_ = true;
        source_ = layer.source;
        const auto rate = mgstc::engine::clampRateEnvelope(
            layer.volume_envelope.rate);
        attack_level_.setValue(rate.attack_level, juce::dontSendNotification);
        attack_rate_.setValue(rate.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(rate.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(rate.sustain_level, juce::dontSendNotification);
        sustain_rate_.setValue(rate.sustain_rate, juce::dontSendNotification);
        release_rate_.setValue(rate.release_rate, juce::dontSendNotification);
        mode_.setSelectedId(
            static_cast<int>(rate.tone_mode) + 1, juce::dontSendNotification);
        noise_.setValue(rate.noise, juce::dontSendNotification);
        const bool psg = source_ == mgstc::engine::TimbreSource::Psg;
        mode_.setVisible(psg);
        mode_label_.setVisible(psg);
        noise_.setVisible(psg);
        noise_label_.setVisible(psg);
        volume_ = layer.volume;
        refreshGraph();
        assigning_ = false;
        fitToContent();
        if (auto* window =
                findParentComponentOfClass<juce::DocumentWindow>()) {
            window->centreWithSize(getWidth(), getHeight() + 32);
        }
        resized();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        graph_.setBounds(area.removeFromTop(rateEnvelopeGraphH));
        area.removeFromTop(sm);
        auto bars = area.removeFromTop(rateEnvelopeBarColumnH);
        const int bar_w = rateEnvelopeBarW;
        auto place_bar = [&bars, bar_w](
            juce::Label& label, juce::Slider& slider) {
            auto column = bars.removeFromLeft(bar_w);
            bars.removeFromLeft(UiLayout::controlGap);
            label.setBounds(column.removeFromTop(UiLayout::fieldH / 2));
            slider.setBounds(column);
        };
        place_bar(attack_level_label_, attack_level_);
        place_bar(attack_rate_label_, attack_rate_);
        place_bar(decay_rate_label_, decay_rate_);
        place_bar(sustain_level_label_, sustain_level_);
        place_bar(sustain_rate_label_, sustain_rate_);
        place_bar(release_rate_label_, release_rate_);
        area.removeFromTop(sm);
        if (source_ == mgstc::engine::TimbreSource::Psg) {
            auto mode_row = area.removeFromTop(fieldH);
            mode_label_.setBounds(mode_row.removeFromLeft(setupValueLabelW));
            mode_row.removeFromLeft(controlGap);
            mode_.setBounds(mode_row.removeFromLeft(setupPitchSweepW * 2));
            area.removeFromTop(controlGap);
            auto noise_row = area.removeFromTop(fieldH);
            noise_label_.setBounds(noise_row.removeFromLeft(setupValueLabelW));
            noise_row.removeFromLeft(controlGap);
            noise_.setBounds(noise_row);
            area.removeFromTop(sm);
        }
        hint_.setBounds(area.removeFromTop(descriptionH * 2 + sm));
        area.removeFromTop(sm);
        preview_.setBounds(
            area.removeFromTop(textButtonH).removeFromLeft(
                libraryButtonMinW + 24));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        juce::ignoreUnused(key);
        if (on_poll_keys_) {
            on_poll_keys_();
        }
        return true;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
        return false;
    }

private:
    void timerCallback() override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
    }

    void configureBar(
        juce::Slider& slider,
        juce::Label& label,
        const char* name) {
        label.setText(name, juce::dontSendNotification);
        label.setFont(UiFonts::body());
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
        slider.setRange(0.0, 255.0, 1.0);
        slider.setSliderStyle(juce::Slider::LinearVertical);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxBelow,
            false,
            UiLayout::rateEnvelopeBarW - UiLayout::xs,
            UiLayout::rateEnvelopeBarTextH);
        slider.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors(false);
            }
        };
        slider.onDragEnd = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(slider);
    }

    [[nodiscard]] mgstc::engine::RateEnvelope readRate() const {
        mgstc::engine::RateEnvelope rate;
        rate.attack_level = static_cast<std::uint8_t>(attack_level_.getValue());
        rate.attack_rate = static_cast<std::uint8_t>(attack_rate_.getValue());
        rate.decay_rate = static_cast<std::uint8_t>(decay_rate_.getValue());
        rate.sustain_level = static_cast<std::uint8_t>(sustain_level_.getValue());
        rate.sustain_rate = static_cast<std::uint8_t>(sustain_rate_.getValue());
        rate.release_rate = static_cast<std::uint8_t>(release_rate_.getValue());
        if (source_ == mgstc::engine::TimbreSource::Psg) {
            rate.tone_mode = static_cast<std::uint8_t>(
                juce::jlimit(0, 3, mode_.getSelectedId() - 1));
            rate.noise = static_cast<std::uint8_t>(
                juce::jlimit(0, 31, juce::roundToInt(noise_.getValue())));
        }
        return mgstc::engine::clampRateEnvelope(rate);
    }

    [[nodiscard]] bool softwareRelease() const noexcept {
        return source_ != mgstc::engine::TimbreSource::Opll;
    }

    void fitToContent() {
        setSize(
            UiLayout::rateEnvelopeDialogW,
            UiLayout::rateEnvelopeEditorHeight(
                source_ == mgstc::engine::TimbreSource::Psg));
    }

    void writeRateToEditors(const mgstc::engine::RateEnvelope& rate) {
        attack_level_.setValue(rate.attack_level, juce::dontSendNotification);
        attack_rate_.setValue(rate.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(rate.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(rate.sustain_level, juce::dontSendNotification);
        sustain_rate_.setValue(rate.sustain_rate, juce::dontSendNotification);
        release_rate_.setValue(rate.release_rate, juce::dontSendNotification);
    }

    void applyHandleDrag(
        mgstc::engine::RateEnvelopeHandleKind kind,
        float seconds,
        std::uint8_t level,
        bool commit) {
        if (assigning_) {
            return;
        }
        auto rate = readRate();
        const auto next = mgstc::engine::applyRateEnvelopeHandleDrag(
            mgstc::app::rateDefinitionFrom(rate),
            kind,
            seconds,
            level,
            softwareRelease());
        rate.attack_level = next.attack_level;
        rate.attack_rate = next.attack_rate;
        rate.decay_rate = next.decay_rate;
        rate.sustain_level = next.sustain_level;
        rate.sustain_rate = next.sustain_rate;
        rate.release_rate = next.release_rate;
        assigning_ = true;
        writeRateToEditors(rate);
        assigning_ = false;
        commitFromEditors(commit);
    }

    void refreshGraph() {
        const auto rate = readRate();
        graph_.setTrace(
            mgstc::app::makeRateTrace(rate, volume_, source_),
            colourForTimbreSource(source_),
            source_,
            mgstc::engine::rateEnvelopeHandleLayout(
                mgstc::app::rateDefinitionFrom(rate),
                softwareRelease()),
            true);
    }

    void commitFromEditors(bool commit) {
        const auto rate = readRate();
        refreshGraph();
        if (on_change_) {
            on_change_(rate, commit);
        }
        if (commit && on_preview_) {
            on_preview_();
        }
    }

    mgstc::engine::TimbreSource source_{};
    std::uint8_t volume_{15};
    ChangeCallback on_change_;
    PreviewCallback on_preview_;
    PollKeysCallback on_poll_keys_;
    bool assigning_{};
    juce::Label title_;
    juce::Label hint_;
    RateEnvelopeGraphView graph_;
    juce::Label attack_level_label_;
    juce::Label attack_rate_label_;
    juce::Label decay_rate_label_;
    juce::Label sustain_level_label_;
    juce::Label sustain_rate_label_;
    juce::Label release_rate_label_;
    juce::Slider attack_level_;
    juce::Slider attack_rate_;
    juce::Slider decay_rate_;
    juce::Slider sustain_level_;
    juce::Slider sustain_rate_;
    juce::Slider release_rate_;
    juce::Label mode_label_;
    juce::ComboBox mode_;
    juce::Label noise_label_;
    juce::Slider noise_;
    juce::TextButton preview_;
};

class RateEnvelopeEditorWindow final : public juce::DocumentWindow {
public:
    RateEnvelopeEditorWindow(
        RateEnvelopeEditorContent* content,
        std::function<void()> on_close)
        : DocumentWindow(
              juce::String::fromUTF8("レートエンベロープ"),
              juce::Colour(0xFF1B222C),
              DocumentWindow::closeButton),
          on_close_(std::move(on_close)) {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        setContentOwned(content, true);
        setWantsKeyboardFocus(true);
        centreWithSize(content->getWidth(), content->getHeight() + 32);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override {
        if (on_close_) {
            on_close_();
        }
    }

    RateEnvelopeEditorContent* editor() const {
        return dynamic_cast<RateEnvelopeEditorContent*>(getContentComponent());
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (auto* content = getContentComponent()) {
            if (content->keyPressed(key)) {
                return true;
            }
        }
        return DocumentWindow::keyPressed(key);
    }

    bool keyStateChanged(bool is_key_down) override {
        if (auto* content = getContentComponent()) {
            static_cast<void>(content->keyStateChanged(is_key_down));
        }
        return DocumentWindow::keyStateChanged(is_key_down);
    }

private:
    std::function<void()> on_close_;
};

class CompositeLayerSetupView final : public juce::Component {
public:
    using ChangedCallback = std::function<void(bool)>;
    using EditCallback = std::function<void()>;
    using PickCallback = std::function<void()>;
    using LfoCallback = std::function<void()>;
    using RateEditCallback = std::function<void()>;
    using DuplicateCallback = std::function<void()>;
    using RemoveCallback = std::function<void()>;

    CompositeLayerSetupView() {
        source_.setFont(UiFonts::heading());
        addAndMakeVisible(source_);
        enabled_.setButtonText(juce::String::fromUTF8("有効"));
        mute_.setButtonText(juce::String::fromUTF8("ミュート"));
        solo_.setButtonText(juce::String::fromUTF8("ソロ"));
        for (auto* toggle : {&enabled_, &mute_, &solo_}) {
            toggle->setLookAndFeel(&switch_look_and_feel_);
            toggle->onClick = [this] {
                if (changed_) {
                    changed_(true);
                }
            };
            addAndMakeVisible(*toggle);
        }
        UiFonts::styleBodyField(name_);
        name_.onTextChange = [this] {
            if (changed_) {
                changed_(false);
            }
        };
        name_.onReturnKey = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        name_.onFocusLost = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(name_);
        envelope_number_.setRange(0.0, 31.0, 1.0);
        envelope_number_.setSliderStyle(juce::Slider::LinearBar);
        envelope_number_.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        envelope_number_.setNumDecimalPlacesToDisplay(0);
        envelope_number_.textFromValueFunction = [](double value) {
            return juce::String::formatted(
                "%02d", juce::jlimit(0, 31, juce::roundToInt(value)));
        };
        envelope_number_.valueFromTextFunction = [](const juce::String& text) {
            return static_cast<double>(
                juce::jlimit(0, 31, text.getIntValue()));
        };
        envelope_number_.setTooltip(juce::String::fromUTF8(
            "@e／@r 仮番号（0～31）。新規は00から連番。"
            "プレビュー定義とトラック @e／@r の両方に反映します。"
            "同一総合音色内で重複すると警告します"));
        envelope_number_.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        envelope_number_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(envelope_number_);
        rate_kind_.setButtonText(juce::String::fromUTF8("@r"));
        rate_kind_.setLookAndFeel(&switch_look_and_feel_);
        rate_kind_.setTooltip(juce::String::fromUTF8(
            "オフ＝シーケンス @e、オン＝レート @r。"
            "@e の命令列は残し、@r のときは使いません"));
        rate_kind_.onClick = [this] {
            rate_edit_.setEnabled(rate_kind_.getToggleState());
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(rate_kind_);
        rate_edit_.setButtonText(juce::String::fromUTF8("レート編集"));
        rate_edit_.setTooltip(juce::String::fromUTF8(
            "@r の AL〜RR（PSG は Mode/Noise）を別窓で編集します。"
            "試聴（1秒）と PCキー／MIDI 演奏が使えます"));
        rate_edit_.onClick = [this] {
            if (rate_edit_callback_) {
                rate_edit_callback_();
            }
        };
        addAndMakeVisible(rate_edit_);
        timbre_settings_.setButtonText(juce::String::fromUTF8("音色設定"));
        timbre_settings_.setTooltip(juce::String::fromUTF8(
            "基本音色を別ウィンドウで選びます（エンベロープ@と同じ選択画面）"));
        timbre_settings_.onClick = [this] {
            if (pick_) {
                pick_();
            }
        };
        addAndMakeVisible(timbre_settings_);
        channel_.onChange = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(channel_);
        duplicate_.setButtonText(juce::String::fromUTF8("複製"));
        duplicate_.setTooltip(juce::String::fromUTF8(
            "このチャンネルを同じ音源の空き番号へ複製します。"
            "SCCからOPLLなど、異なる音源へは複製できません"));
        duplicate_.onClick = [this] {
            if (duplicate_callback_) {
                duplicate_callback_();
            }
        };
        addAndMakeVisible(duplicate_);
        remove_.setButtonText(juce::String::fromUTF8("Ch.削除"));
        remove_.setTooltip(juce::String::fromUTF8(
            "このチャンネルとその独立エンベロープを削除します"));
        remove_.onClick = [this] {
            if (remove_callback_) {
                remove_callback_();
            }
        };
        addAndMakeVisible(remove_);
        number_mode_.addItem(juce::String::fromUTF8("自動"), 1);
        number_mode_.addItem(juce::String::fromUTF8("手動"), 2);
        number_mode_.setSelectedId(1, juce::dontSendNotification);
        number_mode_.setTooltip(juce::String::fromUTF8(
            "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
        number_mode_.onChange = [this] {
            syncTimbreNumberEnabled();
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(number_mode_);
        timbre_number_label_.setText(
            juce::String::fromUTF8("音色番号"),
            juce::dontSendNotification);
        timbre_number_label_.setFont(UiFonts::dense());
        timbre_number_label_.setJustificationType(
            juce::Justification::centredLeft);
        timbre_number_label_.setTooltip(juce::String::fromUTF8(
            "手動時のMGSC @番号（15～31）。自動時は無効"));
        addAndMakeVisible(timbre_number_label_);
        timbre_number_.setRange(15.0, 31.0, 1.0);
        timbre_number_.setSliderStyle(juce::Slider::LinearHorizontal);
        timbre_number_.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        timbre_number_.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        timbre_number_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(timbre_number_);
        edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
        edit_.onClick = [this] {
            if (edit_callback_) {
                edit_callback_();
            }
        };
        addAndMakeVisible(edit_);
        h_.setButtonText("h");
        h_.setClickingTogglesState(false);
        h_.setTooltip(juce::String::fromUTF8(
            "MGSDRV ソフトウェア LFO（発音前 h）。"
            "クリックで設定窓を開きます"));
        h_.onClick = [this] {
            if (changed_) {
                changed_(true);
            }
            if (lfo_) {
                lfo_();
            }
        };
        addAndMakeVisible(h_);
        p_.setButtonText("p");
        p_.setClickingTogglesState(true);
        p_.setTooltip(juce::String::fromUTF8(
            "発音前 p（PSG／SCC）。音程の自動上げ下げ。"
            "0～255（128＝変化なし。旧MGSDRVの p は +128）。"
            "LFO（h）と同時には使えません"));
        p_.onClick = [this] {
            if (assigning_) {
                return;
            }
            if (p_.getToggleState()) {
                h_.setToggleState(false, juce::dontSendNotification);
            }
            syncPitchCommandVisuals();
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(p_);
        configureValueSlider(p_value_, 0.0, 255.0, "");
        p_value_.setTooltip(p_.getTooltip());
        p_value_.onValueChange = [this] {
            if (assigning_) {
                return;
            }
            if (!p_.getToggleState()) {
                p_.setToggleState(true, juce::dontSendNotification);
                h_.setToggleState(false, juce::dontSendNotification);
                syncPitchCommandVisuals();
            }
            if (changed_) {
                changed_(false);
            }
        };
        p_value_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(p_value_);
        p_value_.setValue(128.0, juce::dontSendNotification);
        configureValueSlider(pitch_, -24.0, 24.0, " st");
        configureValueSlider(detune_, -127.0, 127.0, "");
        configureValueSlider(micro_detune_, -32768.0, 32767.0, "");
        configureValueSlider(delay_, 0.0, 256.0, "");
        configureValueSlider(volume_, 0.0, 15.0, "");
        configureValueSlider(key_off_hang_, 0.0, 255.0, "");
        delay_form_.addItem("r", 1);
        delay_form_.addItem("r%", 2);
        delay_form_.setSelectedId(2, juce::dontSendNotification);
        delay_form_.onChange = [this] {
            updateDelayValueRange();
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        delay_form_.setTooltip(juce::String::fromUTF8(
            "発音前休符の記法。r=音符長（テンポ依存）、"
            "r%=1/60秒ティック（テンポ非依存）。@e内の待ちではない"));
        addAndMakeVisible(delay_form_);
        pitch_label_.setText(
            juce::String::fromUTF8("相対音程"),
            juce::dontSendNotification);
        detune_label_.setText(juce::String::fromUTF8("\\"), juce::dontSendNotification);
        micro_detune_label_.setText(
            juce::String::fromUTF8("@\\"), juce::dontSendNotification);
        delay_label_.setText(juce::String::fromUTF8("休符"), juce::dontSendNotification);
        volume_label_.setText(juce::String::fromUTF8("v"), juce::dontSendNotification);
        key_off_hang_label_.setText(
            juce::String::fromUTF8("k"), juce::dontSendNotification);
        pitch_.setTooltip(juce::String::fromUTF8(
            "相対音程（半音）。鍵盤／PCキー／MIDI試聴の音程オフセット（§6.5.3）"));
        detune_.setTooltip(juce::String::fromUTF8(
            "トラック・デチューン（MML \\）。@e内の\\とは別。"
            "キーオン時に先に適用し、エンベロープ\\はこれに加算"));
        micro_detune_.setTooltip(juce::String::fromUTF8(
            "トラック微デチューン（MML @\\）。\\と同時利用で効果が重なる。"
            "PSG/SCC: -32768～32767（128≒半音）、FM: 0～255（255≒半音上）"));
        delay_.setTooltip(juce::String::fromUTF8(
            "キーオン前の休符ディレイ（トラックMML。エンベロープ内カウントではない）。"
            "r<n>は音符長（例 r4＝4分、実時間は複合音色のテンポ依存）。"
            "r%<n>は1/60秒ティック（例 r%30＝0.5秒、テンポ非依存）。"
            "グリッド左側の薄表示は発音前待ちの目安"));
        volume_.setTooltip(juce::String::fromUTF8(
            "トラック音量 v（発音前MML）。@eのfはこの値を最大として引き算"));
        key_off_hang_.setTooltip(juce::String::fromUTF8(
            "発音前 k（PSG／SCC）。キーオフ後に音を残す長さ 0～255。"
            "0＝即時。@r およびハードウェアEG使用時は無効（値は残る）"));
        key_off_hang_label_.setTooltip(key_off_hang_.getTooltip());
        sustain_.addItem("sf", 1);
        sustain_.addItem("so", 2);
        sustain_.setSelectedId(1, juce::dontSendNotification);
        sustain_.setTooltip(juce::String::fromUTF8(
            "FMサスティン。未指定は sf（既定）。so を選んだときだけ出力します"));
        sustain_.onChange = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(sustain_);
        delay_label_.setTooltip(delay_.getTooltip());
        for (auto* label : {
                 &pitch_label_,
                 &detune_label_,
                 &micro_detune_label_,
                 &delay_label_,
                 &volume_label_,
                 &key_off_hang_label_}) {
            label->setFont(UiFonts::dense());
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        updateDelayValueRange();
        syncTimbreNumberEnabled();
        syncPitchCommandVisuals();
    }

    ~CompositeLayerSetupView() override {
        for (auto* toggle : {&enabled_, &mute_, &solo_, &rate_kind_}) {
            toggle->setLookAndFeel(nullptr);
        }
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(UiLayout::panelFill));
    }

    void setCallbacks(
        ChangedCallback changed,
        EditCallback edit,
        PickCallback pick,
        LfoCallback lfo,
        RateEditCallback rate_edit,
        DuplicateCallback duplicate,
        RemoveCallback remove) {
        changed_ = std::move(changed);
        edit_callback_ = std::move(edit);
        pick_ = std::move(pick);
        lfo_ = std::move(lfo);
        rate_edit_callback_ = std::move(rate_edit);
        duplicate_callback_ = std::move(duplicate);
        remove_callback_ = std::move(remove);
    }

    void setDuplicateEnabled(bool enabled) {
        duplicate_.setEnabled(enabled);
        duplicate_.setTooltip(
            enabled
                ? juce::String::fromUTF8(
                      "このチャンネルを同じ音源の空き番号へ複製します。"
                      "SCCからOPLLなど、異なる音源へは複製できません")
                : juce::String::fromUTF8(
                      "この音源のチャンネルはすべて使用中のため複製できません"));
    }

    void syncFromLayer(const mgstc::engine::CompositeLayer& layer) {
        assigning_ = true;
        const char* source_name =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? "PSG"
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? "SCC" : "OPLL";
        source_.setText(source_name, juce::dontSendNotification);
        source_.setColour(
            juce::Label::textColourId,
            layer.source == mgstc::engine::TimbreSource::Psg
                ? juce::Colour(0xFFB990FF)
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? juce::Colour(0xFF53E3A6)
                    : juce::Colour(0xFFFFA75E));
        enabled_.setToggleState(layer.enabled, juce::dontSendNotification);
        mute_.setToggleState(layer.muted, juce::dontSendNotification);
        solo_.setToggleState(layer.solo, juce::dontSendNotification);
        name_.setText(juce::String::fromUTF8(layer.name.c_str()), false);
        envelope_number_.setValue(
            layer.envelope_number, juce::dontSendNotification);
        const bool rate =
            layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate;
        rate_kind_.setToggleState(rate, juce::dontSendNotification);
        rate_edit_.setEnabled(rate);
        const int channel_count =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? 3
                : layer.source == mgstc::engine::TimbreSource::Scc ? 5 : 9;
        channel_.clear(juce::dontSendNotification);
        for (int channel = 0; channel < channel_count; ++channel) {
            channel_.addItem(
                juce::String::fromUTF8("Ch.") + juce::String(channel + 1),
                channel + 1);
        }
        channel_.setSelectedId(
            static_cast<int>(layer.channel) + 1,
            juce::dontSendNotification);
        const bool has_timbre_choice =
            layer.source != mgstc::engine::TimbreSource::Psg;
        const bool rom_base = mgstc::engine::layerUsesOpllRomBase(layer);
        const bool library_base = layer.base_timbre.has_value();
        timbre_settings_.setEnabled(has_timbre_choice);
        const bool triangle_preset = library_base
            && layer.base_timbre->library_id
                == mgstc::engine::kSccTrianglePresetLibraryId;
        edit_.setEnabled(library_base && !triangle_preset);
        number_mode_.setEnabled(library_base && !rom_base);
        if (!has_timbre_choice) {
            timbre_settings_.setTooltip(juce::String::fromUTF8("PSGは@なし"));
        } else if (rom_base) {
            timbre_settings_.setTooltip(
                juce::String::fromUTF8("現在: ")
                + opllRomPatchLabel(static_cast<int>(*layer.base_opll_rom))
                + juce::String::fromUTF8(
                    "\nクリックで基本音色を別ウィンドウから選びます"));
        } else if (library_base) {
            timbre_settings_.setTooltip(
                juce::String::fromUTF8("現在: ")
                + juce::String::fromUTF8(layer.base_timbre->name.c_str())
                + juce::String::fromUTF8(
                    "\nクリックで基本音色を別ウィンドウから選びます"));
        } else {
            timbre_settings_.setTooltip(juce::String::fromUTF8(
                "基本音色を別ウィンドウで選びます（エンベロープ@と同じ選択画面）"));
        }
        if (rom_base) {
            timbre_number_.setRange(0.0, 14.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            timbre_number_.setValue(
                static_cast<double>(*layer.base_opll_rom),
                juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "ROM音色は個別エディタで開けません"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ROM音色は音色番号 0〜14 固定のため変更できません"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        } else if (library_base) {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            const bool manual =
                layer.base_timbre->number_mode
                == mgstc::engine::TimbreNumberMode::Manual;
            number_mode_.setSelectedId(
                manual ? 2 : 1, juce::dontSendNotification);
            timbre_number_.setValue(
                layer.base_timbre->manual_number.value_or(15),
                juce::dontSendNotification);
            edit_.setButtonText(
                juce::String::fromUTF8("編集: ")
                    + juce::String::fromUTF8(layer.base_timbre->name.c_str()));
            edit_.setTooltip(
                triangle_preset
                    ? juce::String::fromUTF8(
                        "初期Triangleはライブラリ未登録です。"
                        "音色設定から保存音色を選ぶと個別編集できます")
                    : juce::String::fromUTF8(
                        "割り当て音色を個別画面で開く"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
            timbre_number_label_.setTooltip(juce::String::fromUTF8(
                "手動時のMGSC @番号（15～31）。自動時は無効"));
            timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        } else {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "先に音色を割り当ててください"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ライブラリ音色を割り当てると音色番号を設定できます"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        }
        syncTimbreNumberEnabled();
        pitch_.setValue(layer.relative_semitones, juce::dontSendNotification);
        h_.setToggleState(
            layer.software_lfo.enabled, juce::dontSendNotification);
        const bool psg_scc =
            layer.source != mgstc::engine::TimbreSource::Opll;
        p_.setVisible(psg_scc);
        p_value_.setVisible(psg_scc);
        p_.setToggleState(
            psg_scc && layer.pitch_sweep.enabled,
            juce::dontSendNotification);
        p_value_.setValue(
            layer.pitch_sweep.value, juce::dontSendNotification);
        key_off_hang_.setVisible(psg_scc);
        key_off_hang_label_.setVisible(psg_scc);
        sustain_.setVisible(!psg_scc);
        key_off_hang_.setValue(
            layer.key_off_hang, juce::dontSendNotification);
        key_off_hang_.setEnabled(psg_scc && !rate);
        key_off_hang_.setAlpha(psg_scc && !rate ? 1.0F : 0.45F);
        key_off_hang_label_.setAlpha(psg_scc && !rate ? 1.0F : 0.45F);
        sustain_.setSelectedId(
            layer.opll_sustain ? 2 : 1, juce::dontSendNotification);
        syncPitchCommandVisuals();
        detune_.setValue(layer.detune, juce::dontSendNotification);
        updateMicroDetuneRange(layer.source);
        micro_detune_.setValue(
            layer.micro_detune, juce::dontSendNotification);
        delay_form_.setSelectedId(
            layer.start_delay_form
                    == mgstc::engine::StartDelayForm::NoteLength
                ? 1
                : 2,
            juce::dontSendNotification);
        updateDelayValueRange();
        delay_.setValue(
            static_cast<double>(layer.start_delay_value),
            juce::dontSendNotification);
        volume_.setValue(layer.volume, juce::dontSendNotification);
        assigning_ = false;
    }

    void applyToLayer(mgstc::engine::CompositeLayer& layer) const {
        layer.enabled = enabled_.getToggleState();
        layer.muted = mute_.getToggleState();
        layer.solo = solo_.getToggleState();
        layer.name = name_.getText().toStdString();
        layer.envelope_number = static_cast<std::uint8_t>(
            juce::jlimit(0, 31, juce::roundToInt(envelope_number_.getValue())));
        if (rate_kind_.getToggleState()) {
            if (layer.volume_envelope.kind
                != mgstc::engine::EnvelopeKind::Rate) {
                mgstc::engine::seedDefaultRateEnvelope(
                    layer.volume_envelope.rate, layer.source);
            }
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Rate;
        } else {
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        }
        layer.channel = static_cast<std::uint8_t>(
            juce::jmax(1, channel_.getSelectedId()) - 1);
        layer.relative_semitones = static_cast<std::int8_t>(pitch_.getValue());
        layer.detune = static_cast<std::int16_t>(detune_.getValue());
        {
            const int micro = static_cast<int>(micro_detune_.getValue());
            layer.micro_detune = layer.source == mgstc::engine::TimbreSource::Opll
                ? juce::jlimit(0, 255, micro)
                : juce::jlimit(-32768, 32767, micro);
        }
        layer.start_delay_form =
            delay_form_.getSelectedId() == 1
                ? mgstc::engine::StartDelayForm::NoteLength
                : mgstc::engine::StartDelayForm::AbsoluteTicks;
        layer.start_delay_value =
            static_cast<std::uint32_t>(delay_.getValue());
        layer.volume = static_cast<std::uint8_t>(volume_.getValue());
        layer.key_off_hang = static_cast<std::uint8_t>(
            key_off_hang_.getValue());
        layer.opll_sustain = layer.source == mgstc::engine::TimbreSource::Opll
            && sustain_.getSelectedId() == 2;
        if (layer.source == mgstc::engine::TimbreSource::Opll) {
            layer.pitch_sweep.enabled = false;
            layer.key_off_hang = 0;
        } else {
            layer.pitch_sweep.enabled = p_.getToggleState();
            layer.pitch_sweep.value = static_cast<std::uint8_t>(
                p_value_.getValue());
            layer.opll_sustain = false;
            if (layer.pitch_sweep.enabled) {
                layer.software_lfo.enabled = false;
            }
        }
        if (!layer.base_timbre) {
            return;
        }
        const bool manual = number_mode_.getSelectedId() == 2;
        layer.base_timbre->number_mode = manual
            ? mgstc::engine::TimbreNumberMode::Manual
            : mgstc::engine::TimbreNumberMode::Automatic;
        layer.base_timbre->manual_number = manual
            ? std::optional<std::uint8_t>{
                  static_cast<std::uint8_t>(timbre_number_.getValue())}
            : std::nullopt;
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto title = area.removeFromTop(fieldH);
        source_.setBounds(title.removeFromLeft(setupSourceLabelW));
        title.removeFromLeft(controlGap);
        auto place_switch = [&title](juce::ToggleButton& toggle) {
            const int width = switchControlWidth(toggle.getButtonText(), fieldH);
            toggle.setBounds(title.removeFromRight(width));
        };
        place_switch(solo_);
        title.removeFromRight(controlGap);
        place_switch(mute_);
        title.removeFromRight(controlGap);
        place_switch(enabled_);
        area.removeFromTop(sm);
        auto identity = area.removeFromTop(fieldH);
        remove_.setBounds(identity.removeFromRight(setupRemoveLayerW));
        identity.removeFromRight(controlGap);
        duplicate_.setBounds(identity.removeFromRight(setupDuplicateLayerW));
        identity.removeFromRight(controlGap);
        channel_.setBounds(identity.removeFromRight(setupChannelComboW));
        identity.removeFromRight(controlGap);
        envelope_number_.setBounds(
            identity.removeFromLeft(setupEnvelopeNumberW));
        identity.removeFromLeft(controlGap);
        name_.setBounds(identity);
        area.removeFromTop(sm);
        auto rate_row = area.removeFromTop(fieldH);
        rate_edit_.setBounds(rate_row.removeFromRight(setupRateEditW));
        rate_row.removeFromRight(controlGap);
        const int rate_w = switchControlWidth(
            rate_kind_.getButtonText(), fieldH);
        rate_kind_.setBounds(rate_row.removeFromRight(rate_w));
        area.removeFromTop(sm);
        auto timbre_row = area.removeFromTop(fieldH);
        edit_.setBounds(timbre_row.removeFromRight(libraryManageButtonW));
        timbre_row.removeFromRight(controlGap);
        timbre_settings_.setBounds(timbre_row);
        area.removeFromTop(sm);
        auto number_row = area.removeFromTop(fieldH);
        auto volume_area = number_row.removeFromRight(
            juce::jmax(
                editSubLaneLabelW + controlGap + setupPitchSweepW,
                number_row.getWidth() / 3));
        number_row.removeFromRight(controlGap);
        number_mode_.setBounds(number_row.removeFromLeft(setupNumberModeW));
        number_row.removeFromLeft(controlGap);
        timbre_number_label_.setBounds(
            number_row.removeFromLeft(setupValueLabelW));
        number_row.removeFromLeft(controlGap);
        timbre_number_.setBounds(number_row);
        layoutValue(volume_area, volume_label_, volume_);
        area.removeFromTop(sm);
        auto pitch_row = area.removeFromTop(fieldH);
        if (p_.isVisible()) {
            p_value_.setBounds(pitch_row.removeFromRight(setupPitchSweepW));
            pitch_row.removeFromRight(controlGap);
            p_.setBounds(pitch_row.removeFromRight(fieldH));
            pitch_row.removeFromRight(controlGap);
        } else {
            p_.setBounds({});
            p_value_.setBounds({});
        }
        h_.setBounds(pitch_row.removeFromRight(fieldH));
        pitch_row.removeFromRight(controlGap);
        layoutValue(
            pitch_row, pitch_label_, pitch_, setupValueLabelW);
        area.removeFromTop(sm);
        auto detune_row = area.removeFromTop(fieldH);
        const int half = juce::jmax(
            1, (detune_row.getWidth() - controlGap) / 2);
        layoutValue(detune_row.removeFromLeft(half), detune_label_, detune_);
        detune_row.removeFromLeft(controlGap);
        layoutValue(detune_row, micro_detune_label_, micro_detune_);
        area.removeFromTop(sm);
        auto delay_row = area.removeFromTop(fieldH);
        auto delay_half = delay_row.removeFromLeft(half);
        delay_label_.setBounds(delay_half.removeFromLeft(editSubLaneLabelW));
        delay_half.removeFromLeft(controlGap);
        delay_form_.setBounds(delay_half.removeFromLeft(setupSustainComboW));
        delay_half.removeFromLeft(controlGap);
        delay_.setBounds(delay_half);
        delay_row.removeFromLeft(controlGap);
        if (key_off_hang_.isVisible()) {
            layoutValue(delay_row, key_off_hang_label_, key_off_hang_);
            sustain_.setBounds({});
        } else {
            key_off_hang_label_.setBounds({});
            key_off_hang_.setBounds({});
            sustain_.setBounds(delay_row.removeFromLeft(setupSustainComboW));
        }
    }

    void mouseDown(const juce::MouseEvent&) override {
        if (changed_) {
            changed_(true);
        }
    }

private:
    void updateDelayValueRange() {
        const bool note_length = delay_form_.getSelectedId() == 1;
        delay_.setRange(
            0.0,
            note_length ? 192.0 : 256.0,
            1.0);
        delay_.setTextValueSuffix(note_length ? "" : "");
    }

    void updateMicroDetuneRange(mgstc::engine::TimbreSource source) {
        if (source == mgstc::engine::TimbreSource::Opll) {
            micro_detune_.setRange(0.0, 255.0, 1.0);
        } else {
            micro_detune_.setRange(-32768.0, 32767.0, 1.0);
        }
    }

    void syncTimbreNumberEnabled() {
        const bool can_assign = number_mode_.isEnabled();
        const bool manual = number_mode_.getSelectedId() == 2;
        timbre_number_.setEnabled(can_assign && manual);
        timbre_number_label_.setEnabled(can_assign && manual);
    }

    void syncPitchCommandVisuals() {
        const bool p_on = p_.isVisible() && p_.getToggleState();
        const bool h_on = h_.getToggleState();
        p_.setAlpha(h_on ? 0.45F : 1.0F);
        h_.setAlpha(p_on ? 0.45F : 1.0F);
        p_value_.setEnabled(p_on && !h_on);
        p_value_.setAlpha(p_on && !h_on ? 1.0F : 0.45F);
    }

    void configureValueSlider(
        juce::Slider& slider,
        double minimum,
        double maximum,
        const juce::String& suffix) {
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        slider.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(slider);
    }

    static void layoutValue(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider,
        int label_w = UiLayout::editSubLaneLabelW) {
        label.setBounds(area.removeFromLeft(label_w));
        area.removeFromLeft(UiLayout::controlGap);
        slider.setBounds(area);
    }

    ChangedCallback changed_;
    EditCallback edit_callback_;
    PickCallback pick_;
    LfoCallback lfo_;
    RateEditCallback rate_edit_callback_;
    DuplicateCallback duplicate_callback_;
    RemoveCallback remove_callback_;
    bool assigning_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label source_;
    juce::ToggleButton enabled_;
    juce::ToggleButton mute_;
    juce::ToggleButton solo_;
    juce::TextEditor name_;
    juce::Slider envelope_number_;
    juce::ToggleButton rate_kind_;
    juce::TextButton rate_edit_;
    juce::TextButton timbre_settings_;
    juce::ComboBox channel_;
    juce::TextButton duplicate_;
    juce::TextButton remove_;
    juce::ComboBox number_mode_;
    juce::Label timbre_number_label_;
    juce::Slider timbre_number_;
    juce::TextButton edit_;
    juce::TextButton h_;
    juce::TextButton p_;
    juce::Slider p_value_;
    juce::Label pitch_label_;
    juce::Label detune_label_;
    juce::Label micro_detune_label_;
    juce::Label delay_label_;
    juce::Label volume_label_;
    juce::Label key_off_hang_label_;
    juce::ComboBox delay_form_;
    juce::ComboBox sustain_;
    juce::Slider pitch_;
    juce::Slider detune_;
    juce::Slider micro_detune_;
    juce::Slider delay_;
    juce::Slider volume_;
    juce::Slider key_off_hang_;
};

class CountCommandStackView final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    using DeleteCallback = std::function<void(int chip_index)>;
    using MoveCallback = std::function<void(int chip_index, bool to_after)>;
    using InsertZoneCallback = std::function<void(bool insert_after)>;

    void setDeleteCallback(DeleteCallback callback) {
        delete_callback_ = std::move(callback);
    }

    void setMoveCallback(MoveCallback callback) {
        move_callback_ = std::move(callback);
    }

    void setInsertZoneCallback(InsertZoneCallback callback) {
        insert_zone_callback_ = std::move(callback);
    }

    void clearStack() {
        count_ = -1;
        has_loop_start_ = false;
        has_loop_end_ = false;
        insert_after_ = false;
        before_.clear();
        after_.clear();
        after_end_.clear();
        repaint();
    }

    void setStack(
        int count,
        bool has_loop_start,
        bool has_loop_end,
        bool insert_after,
        std::vector<juce::String> before,
        std::vector<juce::String> after,
        std::vector<juce::String> after_end) {
        count_ = count;
        has_loop_start_ = has_loop_start;
        has_loop_end_ = has_loop_end;
        insert_after_ = insert_after && has_loop_start;
        before_ = std::move(before);
        after_ = std::move(after);
        after_end_ = std::move(after_end);
        setTooltip(juce::String::fromUTF8(
            "選択中カウントのゼロ時間命令。"
            "ゾーンをクリックで追加先（ループ前／後）を切替。"
            "チップを右クリックで削除または前後へ移動。"
            "[ より前は初回のみ、後はループのたびに実行。"));
        repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (count_ < 0) {
            return;
        }
        const int index = hitChipIndex(event.getPosition());
        if (event.mods.isPopupMenu() && index >= 0) {
            const bool in_after = index >= static_cast<int>(before_.size());
            juce::PopupMenu menu;
            menu.addItem(1, juce::String::fromUTF8("削除"));
            if (has_loop_start_) {
                if (in_after) {
                    menu.addItem(
                        2, juce::String::fromUTF8("ループ前へ移動"));
                } else {
                    menu.addItem(
                        3, juce::String::fromUTF8("ループ後へ移動"));
                }
            }
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(this),
                [this, index, in_after](int result) {
                    if (result == 1 && delete_callback_ != nullptr) {
                        delete_callback_(index);
                    } else if (
                        result == 2 && in_after && move_callback_ != nullptr) {
                        move_callback_(index, false);
                    } else if (
                        result == 3 && !in_after && move_callback_ != nullptr) {
                        move_callback_(index, true);
                    }
                });
            return;
        }
        if (event.mods.isLeftButtonDown()) {
            const auto zone = hitInsertZone(event.getPosition());
            if (zone && insert_zone_callback_ != nullptr) {
                insert_zone_callback_(*zone);
            }
        }
    }

    void paint(juce::Graphics& graphics) override {
        auto bounds = getLocalBounds().toFloat();
        graphics.setColour(juce::Colour(0xFF26313B));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);

        auto area = getLocalBounds().reduced(UiLayout::sm, UiLayout::xs);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::dense(true));
        auto header = area.removeFromTop(16);
        if (count_ < 0) {
            graphics.drawText(
                juce::String::fromUTF8("チャンネルを選択すると命令スタックを表示"),
                header,
                juce::Justification::centredLeft,
                false);
            return;
        }
        graphics.drawText(
            juce::String::fromUTF8("カウント ")
                + juce::String(count_)
                + juce::String::fromUTF8(" の命令"),
            header,
            juce::Justification::centredLeft,
            false);

        drawZone(
            graphics,
            area.removeFromTop(20),
            juce::String::fromUTF8("ループ前"),
            before_,
            juce::Colour(kUiHoverAccent),
            false,
            has_loop_start_ && !insert_after_);
        drawDivider(
            graphics,
            area.removeFromTop(16),
            has_loop_start_
                ? juce::String::fromUTF8("[  ループ開始")
                : juce::String::fromUTF8("[  なし（この列）"),
            has_loop_start_);
        drawZone(
            graphics,
            area.removeFromTop(20),
            juce::String::fromUTF8("ループ後"),
            after_,
            juce::Colour(kUiHoverAccent),
            !has_loop_start_,
            has_loop_start_ && insert_after_);
        drawDivider(
            graphics,
            area.removeFromTop(16),
            has_loop_end_
                ? juce::String::fromUTF8("]  ループ終了")
                : juce::String::fromUTF8("]  なし（この列）"),
            has_loop_end_);
        drawZone(
            graphics,
            area,
            juce::String::fromUTF8("]以降"),
            after_end_,
            juce::Colour(0xFF8896A3),
            true,
            false);
    }

private:
    void drawDivider(
        juce::Graphics& graphics,
        juce::Rectangle<int> row,
        const juce::String& text,
        bool active) const {
        graphics.setColour(
            active
                ? juce::Colour(kUiHoverAccent).withAlpha(0.35F)
                : juce::Colour(0xFF40505E));
        graphics.fillRect(row.withHeight(1).withY(row.getCentreY()));
        graphics.setFont(UiFonts::dense(true));
        graphics.setColour(
            active ? juce::Colour(kUiHoverAccent) : juce::Colour(0xFF8896A3));
        graphics.drawText(
            text, row.reduced(UiLayout::xs, 0),
            juce::Justification::centredLeft, false);
    }

    void drawZone(
        juce::Graphics& graphics,
        juce::Rectangle<int> row,
        const juce::String& title,
        const std::vector<juce::String>& chips,
        juce::Colour chip_colour,
        bool dimmed,
        bool insert_target) const {
        if (insert_target) {
            graphics.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.12F));
            graphics.fillRoundedRectangle(row.toFloat(), 3.0F);
        }
        auto label = row.removeFromLeft(64);
        graphics.setFont(UiFonts::dense());
        graphics.setColour(
            insert_target
                ? juce::Colour(kUiHoverAccent)
                : (dimmed ? juce::Colour(0xFF6B7785) : juce::Colour(0xFFC9D1D9)));
        graphics.drawText(
            title, label, juce::Justification::centredLeft, false);
        if (chips.empty()) {
            graphics.setColour(juce::Colour(0xFF6B7785));
            graphics.drawText(
                juce::String::fromUTF8(
                    insert_target ? "（追加先）" : "（なし）"),
                row,
                juce::Justification::centredLeft,
                false);
            return;
        }
        int x = row.getX();
        graphics.setFont(UiFonts::dense(true));
        for (const auto& chip : chips) {
            const int width = juce::jmax(
                28,
                juce::GlyphArrangement::getStringWidthInt(
                    UiFonts::dense(true), chip)
                    + UiLayout::sm);
            if (x + width > row.getRight()) {
                break;
            }
            auto chip_bounds = juce::Rectangle<int>(
                x, row.getY() + 1, width - 2, row.getHeight() - 2);
            graphics.setColour(chip_colour.withAlpha(dimmed ? 0.18F : 0.28F));
            graphics.fillRoundedRectangle(chip_bounds.toFloat(), 3.0F);
            graphics.setColour(
                dimmed ? juce::Colour(0xFF8896A3) : juce::Colour(0xFFE6EDF3));
            graphics.drawText(
                chip, chip_bounds, juce::Justification::centred, false);
            x += width;
        }
    }

    [[nodiscard]] std::optional<bool> hitInsertZone(
        juce::Point<int> point) const {
        if (!has_loop_start_) {
            return std::nullopt;
        }
        auto area = getLocalBounds().reduced(UiLayout::sm, UiLayout::xs);
        area.removeFromTop(16);  // header
        auto before_row = area.removeFromTop(20);
        area.removeFromTop(16);  // [ divider
        auto after_row = area.removeFromTop(20);
        if (before_row.contains(point)) {
            return false;
        }
        if (after_row.contains(point)) {
            return true;
        }
        return std::nullopt;
    }

    [[nodiscard]] int hitChipIndex(juce::Point<int> point) const {
        auto area = getLocalBounds().reduced(UiLayout::sm, UiLayout::xs);
        area.removeFromTop(16);  // header
        auto before_row = area.removeFromTop(20);
        before_row.removeFromLeft(64);
        int x = before_row.getX();
        for (int index = 0; index < static_cast<int>(before_.size()); ++index) {
            const auto& chip = before_[static_cast<std::size_t>(index)];
            const int width = juce::jmax(
                28,
                juce::GlyphArrangement::getStringWidthInt(
                    UiFonts::dense(true), chip)
                    + UiLayout::sm);
            auto chip_bounds = juce::Rectangle<int>(
                x, before_row.getY() + 1, width - 2, before_row.getHeight() - 2);
            if (chip_bounds.contains(point)) {
                return index;
            }
            x += width;
            if (x > before_row.getRight()) {
                break;
            }
        }
        area.removeFromTop(16);  // [ divider
        auto after_row = area.removeFromTop(20);
        after_row.removeFromLeft(64);
        x = after_row.getX();
        const int before_count = static_cast<int>(before_.size());
        for (int index = 0; index < static_cast<int>(after_.size()); ++index) {
            const auto& chip = after_[static_cast<std::size_t>(index)];
            const int width = juce::jmax(
                28,
                juce::GlyphArrangement::getStringWidthInt(
                    UiFonts::dense(true), chip)
                    + UiLayout::sm);
            auto chip_bounds = juce::Rectangle<int>(
                x, after_row.getY() + 1, width - 2, after_row.getHeight() - 2);
            if (chip_bounds.contains(point)) {
                return before_count + index;
            }
            x += width;
            if (x > after_row.getRight()) {
                break;
            }
        }
        return -1;
    }

    DeleteCallback delete_callback_;
    MoveCallback move_callback_;
    InsertZoneCallback insert_zone_callback_;
    int count_{-1};
    bool has_loop_start_{};
    bool has_loop_end_{};
    bool insert_after_{};
    std::vector<juce::String> before_;
    std::vector<juce::String> after_;
    std::vector<juce::String> after_end_;
};


}  // namespace

class mgstc::app::CompositeTimeline::Impl final
    : public juce::Component,
      private juce::ScrollBar::Listener {
public:
    using EditCallback = std::function<void(
        const mgstc::engine::CompositeTimbre&, bool, bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using CatalogCallback = std::function<
        std::vector<EnvelopeTimbreCatalogItem>(mgstc::engine::TimbreSource)>;
    using OpenTimbreCallback = std::function<void(
        const juce::String&, std::optional<std::uint64_t>)>;
    using AssignTimbreCallback = std::function<void(
        std::size_t, LayerBaseTimbreAssign)>;
    using TimbreNameCallback = std::function<juce::String(std::uint64_t)>;
    using TimbreLibraryCallback =
        std::function<const mgstc::engine::TimbreLibrary*()>;
    using ManageTagsCallback = std::function<void()>;
    using MutateTimbreNameCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using MutateTimbreTagsCallback =
        std::function<bool(std::uint64_t, std::vector<std::string>)>;
    using MutateTimbreMemoCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using LfoSessionCallback =
        std::function<void(std::optional<std::size_t>, bool, bool)>;
    using LfoPollKeysCallback = std::function<void()>;

    Impl()
        : horizontal_scroll_(false), vertical_scroll_(true) {
        parameter_.addItem(juce::String::fromUTF8("音量"), 1);
        parameter_.addItem(juce::String::fromUTF8("音程"), 2);
        parameter_.addItem(juce::String::fromUTF8("@音色"), 3);
        parameter_.addItem(juce::String::fromUTF8("手動y"), 6);
        parameter_.addItem(juce::String::fromUTF8("TL自動"), 4);
        parameter_.addItem(juce::String::fromUTF8("FB自動"), 5);
        parameter_.setSelectedId(1, juce::dontSendNotification);
        parameter_.setTooltip(
            juce::String::fromUTF8(
                "時間軸へ描画するパラメーター。"
                "手動yはOPLLオリジナル音色のレジスタ編集／PSGのyreg,data。"
                "TL/FB自動はOPLL専用（手動yとは別）"));
        parameter_.onChange = [this] {
            syncPointEditors();
            syncRegisterAutoEditors();
            syncInspector();
            repaint();
        };
        addAndMakeVisible(parameter_);

        auto_mode_.addItem(juce::String::fromUTF8("オフ"), 1);
        auto_mode_.addItem(juce::String::fromUTF8("上昇"), 2);
        auto_mode_.addItem(juce::String::fromUTF8("下降"), 3);
        auto_mode_.addItem("LFO", 4);
        auto_mode_.addItem(juce::String::fromUTF8("自由曲線"), 5);
        auto_mode_.setSelectedId(1, juce::dontSendNotification);
        auto_mode_.setTooltip(juce::String::fromUTF8(
            "TL/FB自動の時間変化モード。出力は展開したyレジスタ書き込み"));
        auto_mode_.onChange = [this] {
            applyRegisterAutoFromEditors(false);
        };
        addAndMakeVisible(auto_mode_);

        configureAutoParamEditor(
            auto_depth_, juce::String::fromUTF8("深さ（開始値またはLFO振幅）"));
        configureAutoParamEditor(
            auto_speed_, juce::String::fromUTF8("変化量／変化スピード"));
        configureAutoParamEditor(
            auto_coarseness_,
            juce::String::fromUTF8("粗さ（何カウントごとに書き込むか）"));
        configureAutoParamEditor(
            auto_stop_, juce::String::fromUTF8("停止位置（到達値／LFO中心）"));
        auto_depth_label_.setText(
            juce::String::fromUTF8("深さ"), juce::dontSendNotification);
        auto_speed_label_.setText(
            juce::String::fromUTF8("変化"), juce::dontSendNotification);
        auto_coarseness_label_.setText(
            juce::String::fromUTF8("粗さ"), juce::dontSendNotification);
        auto_stop_label_.setText(
            juce::String::fromUTF8("停止"), juce::dontSendNotification);
        for (auto* label : {
                 &auto_depth_label_, &auto_speed_label_,
                 &auto_coarseness_label_, &auto_stop_label_}) {
            label->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*label);
        }

        psg_add_.setButtonText(juce::String::fromUTF8("PSG追加"));
        scc_add_.setButtonText(juce::String::fromUTF8("SCC追加"));
        opll_add_.setButtonText(juce::String::fromUTF8("OPLL追加"));
        psg_add_.setTooltip(juce::String::fromUTF8(
            "未使用のPSGチャンネルを追加します（最大3ch）"));
        scc_add_.setTooltip(juce::String::fromUTF8(
            "未使用のSCCチャンネルを追加します（最大5ch）"));
        opll_add_.setTooltip(juce::String::fromUTF8(
            "未使用のOPLLチャンネルを追加します（最大9ch）"));
        psg_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Psg);
        };
        scc_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Scc);
        };
        opll_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Opll);
        };
        addAndMakeVisible(psg_add_);
        addAndMakeVisible(scc_add_);
        addAndMakeVisible(opll_add_);

        tempo_label_.setText(
            juce::String::fromUTF8("テンポ"),
            juce::dontSendNotification);
        tempo_label_.setFont(UiFonts::body());
        tempo_label_.setJustificationType(juce::Justification::centredRight);
        tempo_label_.setTooltip(juce::String::fromUTF8(
            "この複合音色の演奏テンポ（MGSC #tempo 57～2047）。ライブラリ保存対象"));
        addAndMakeVisible(tempo_label_);
        UiFonts::styleBodyField(tempo_);
        tempo_.setInputRestrictions(4, "0123456789");
        tempo_.setJustification(juce::Justification::centred);
        tempo_.setText(
            juce::String(playback_tempo_),
            juce::dontSendNotification);
        tempo_.setTooltip(tempo_label_.getTooltip());
        tempo_.onReturnKey = [this] { commitPlaybackTempoFromEditor(); };
        tempo_.onFocusLost = [this] { commitPlaybackTempoFromEditor(); };
        addAndMakeVisible(tempo_);

        edit_mode_.addItem(juce::String::fromUTF8("描画"), 1);
        edit_mode_.addItem(juce::String::fromUTF8("終端"), 2);
        edit_mode_.addItem(juce::String::fromUTF8("L開始"), 3);
        edit_mode_.addItem(juce::String::fromUTF8("L終了"), 4);
        edit_mode_.setSelectedId(1, juce::dontSendNotification);
        edit_mode_.setTooltip(juce::String::fromUTF8(
            "編集モード: 描画／終端／ループ開始(L開始)／ループ終了(L終了)"));
        addAndMakeVisible(edit_mode_);

        value_.setInputRestrictions(5, "-0123456789");
        value_.setText("15", false);
        value_.setTooltip(
            juce::String::fromUTF8("選択パラメーターの値"));
        UiFonts::styleBodyField(value_);
        addAndMakeVisible(value_);
        position_.setText("ct 0", juce::dontSendNotification);
        position_.setJustificationType(juce::Justification::centredRight);
        position_.setTooltip(juce::String::fromUTF8(
            "ホバーまたはクリック中のカウント（数値入力では変更しません）"));
        value_label_.setText(
            juce::String::fromUTF8("値"), juce::dontSendNotification);
        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(position_);
        addAndMakeVisible(value_label_);
        apply_.setButtonText(juce::String::fromUTF8("設定"));
        apply_.setTooltip(
            juce::String::fromUTF8(
                "表示中のカウントへ入力値を設定します。"
                "TL/FB自動では開始ステップとパラメーターを確定します"));
        apply_.onClick = [this] {
            if (selectedEditMode() != EditMode::Draw) {
                return;
            }
            if (isRegisterAutoParameter(selectedParameter())) {
                applyRegisterAutoFromEditors(true);
                return;
            }
            if (selectedParameter() == Parameter::Timbre) {
                pickTimbreAt(selected_count_, false);
                return;
            }
            if (selectedParameter() == Parameter::RegisterWrite) {
                pickRegisterAt(selected_count_);
                return;
            }
            if (selectedParameter() == Parameter::Pitch) {
                addPitchCommand(
                    selected_count_, value_.getText().getIntValue(), true);
                return;
            }
            editPoint(selected_count_, value_.getText().getIntValue(), true);
        };
        addAndMakeVisible(apply_);

        inspector_selection_.setJustificationType(
            juce::Justification::centredLeft);
        inspector_selection_.setTooltip(juce::String::fromUTF8(
            "選択中のチャンネルとエンベロープ種類"));
        addAndMakeVisible(inspector_selection_);

        configureInspectorEditor(
            length_editor_, juce::String::fromUTF8("終端カウント"));
        configureInspectorEditor(
            loop_start_editor_, juce::String::fromUTF8("ループ開始カウント"));
        configureInspectorEditor(
            loop_end_editor_, juce::String::fromUTF8("ループ終了カウント"));
        configureInspectorLabel(length_label_, "END");
        configureInspectorLabel(loop_start_label_, "L>");
        configureInspectorLabel(loop_end_label_, "<L");

        inspector_apply_.setButtonText(juce::String::fromUTF8("反映"));
        inspector_apply_.setTooltip(juce::String::fromUTF8(
            "表示中の終端とループ位置を選択エンベロープへ反映します"));
        inspector_apply_.onClick = [this] { applyInspector(); };
        addAndMakeVisible(inspector_apply_);

        clear_loop_.setButtonText(juce::String::fromUTF8("ループ解除"));
        clear_loop_.setTooltip(juce::String::fromUTF8(
            "選択エンベロープのループ開始と終了を両方解除します"));
        clear_loop_.onClick = [this] { clearSelectedLoop(); };
        addAndMakeVisible(clear_loop_);

        command_stack_.setInterceptsMouseClicks(true, false);
        command_stack_.setDeleteCallback([this](int chip_index) {
            deleteStackChipAt(chip_index);
        });
        command_stack_.setMoveCallback(
            [this](int chip_index, bool to_after) {
                moveStackChipAt(chip_index, to_after);
            });
        command_stack_.setInsertZoneCallback([this](bool insert_after) {
            loop_insert_after_ = insert_after;
            if (selected_layer_ >= 0
                && selected_layer_
                    < static_cast<int>(timbre_.layers.size())) {
                refreshCommandStack(
                    timbre_.layers[static_cast<std::size_t>(
                        selected_layer_)]);
            }
        });
        addAndMakeVisible(command_stack_);

        envelope_mml_preview_.setMultiLine(true);
        envelope_mml_preview_.setReadOnly(true);
        envelope_mml_preview_.setWantsKeyboardFocus(false);
        envelope_mml_preview_.setScrollbarsShown(true);
        envelope_mml_preview_.setFont(UiFonts::mono());
        envelope_mml_preview_.setColour(
            juce::TextEditor::backgroundColourId,
            juce::Colour(0xFF151A20));
        envelope_mml_preview_.setColour(
            juce::TextEditor::textColourId,
            juce::Colour(0xFF9BE9C3));
        envelope_mml_preview_.setColour(
            juce::TextEditor::outlineColourId,
            juce::Colour(0xFF40505E));
        envelope_mml_preview_.setColour(
            juce::TextEditor::focusedOutlineColourId,
            juce::Colour(kUiHoverAccent));
        envelope_mml_preview_.setTooltip(juce::String::fromUTF8(
            "定義=@e（長いときは複数行）、その下=発音前トラックMML。"
            "マウスで範囲選択して Ctrl+C でコピーできます"));
        addAndMakeVisible(envelope_mml_preview_);

        graph_clip_.setInterceptsMouseClicks(false, true);
        addAndMakeVisible(graph_clip_);
        for (auto* c : std::initializer_list<juce::Component*>{
                 &edit_mode_, &parameter_, &position_, &value_label_,
                 &value_, &apply_, &inspector_selection_,
                 &length_label_, &length_editor_, &loop_start_label_,
                 &loop_start_editor_, &loop_end_label_, &loop_end_editor_,
                 &inspector_apply_, &clear_loop_, &command_stack_,
                 &envelope_mml_preview_,
                 &auto_mode_, &auto_depth_, &auto_speed_,
                 &auto_coarseness_, &auto_stop_, &auto_depth_label_,
                 &auto_speed_label_, &auto_coarseness_label_,
                 &auto_stop_label_}) {
            graph_clip_.addAndMakeVisible(*c);
        }

        horizontal_scroll_.setRangeLimits(
            0.0, static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
        horizontal_scroll_.setCurrentRange(
            0.0, kDefaultVisibleCounts, juce::dontSendNotification);
        horizontal_scroll_.setSingleStepSize(1.0);
        horizontal_scroll_.setAutoHide(false);
        horizontal_scroll_.addListener(this);
        vertical_scroll_.setSingleStepSize(
            static_cast<double>(UiLayout::sm));
        vertical_scroll_.setAutoHide(false);
        vertical_scroll_.addListener(this);
        addAndMakeVisible(horizontal_scroll_);
        addAndMakeVisible(vertical_scroll_);
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }

    ~Impl() override {
        closeRateEditor(false);
        horizontal_scroll_.removeListener(this);
        vertical_scroll_.removeListener(this);
    }

    void setEditCallback(EditCallback callback) {
        edit_callback_ = std::move(callback);
    }

    void setStatusCallback(StatusCallback callback) {
        status_callback_ = std::move(callback);
    }

    void setTimbreCatalogCallback(CatalogCallback callback) {
        catalog_callback_ = std::move(callback);
    }

    void setOpenTimbreCallback(OpenTimbreCallback callback) {
        open_timbre_callback_ = std::move(callback);
    }

    void setAssignTimbreCallback(AssignTimbreCallback callback) {
        assign_timbre_callback_ = std::move(callback);
    }

    void setTimbreNameCallback(TimbreNameCallback callback) {
        timbre_name_callback_ = std::move(callback);
    }

    void setTimbreLibraryCallback(TimbreLibraryCallback callback) {
        timbre_library_callback_ = std::move(callback);
    }

    void setManageTagsCallback(ManageTagsCallback callback) {
        manage_tags_callback_ = std::move(callback);
    }

    void setMutateTimbreNameCallback(MutateTimbreNameCallback callback) {
        mutate_name_callback_ = std::move(callback);
    }

    void setMutateTimbreTagsCallback(MutateTimbreTagsCallback callback) {
        mutate_tags_callback_ = std::move(callback);
    }

    void setMutateTimbreMemoCallback(MutateTimbreMemoCallback callback) {
        mutate_memo_callback_ = std::move(callback);
    }

    void setLfoSessionCallback(LfoSessionCallback callback) {
        lfo_session_callback_ = std::move(callback);
    }

    void setLfoPollKeysCallback(LfoPollKeysCallback callback) {
        lfo_poll_keys_callback_ = std::move(callback);
    }

    void closeRateEditor(bool notify_session) {
        rate_editor_layer_.reset();
        rate_window_.reset();
        if (notify_session && lfo_session_callback_) {
            lfo_session_callback_(std::nullopt, false, false);
        }
    }

    void openRateEditor(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Rate) {
            mgstc::engine::seedDefaultRateEnvelope(
                layer.volume_envelope.rate, layer.source);
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Rate;
            if (static_cast<int>(layer_index) < layer_setups_.size()) {
                layer_setups_[static_cast<int>(layer_index)]
                    ->syncFromLayer(layer);
            }
            if (edit_callback_) {
                edit_callback_(timbre_, true, false);
            }
        }
        selected_layer_ = static_cast<int>(layer_index);
        UiScale::forceGlobalForNonEditorUi();
        if (rate_window_ != nullptr
            && rate_editor_layer_ == layer_index) {
            rate_window_->toFront(true);
            if (auto* editor = rate_window_->editor()) {
                editor->assignFromLayer(layer);
            }
            if (lfo_session_callback_) {
                lfo_session_callback_(layer_index, false, false);
            }
            return;
        }
        closeRateEditor(false);
        rate_editor_layer_ = layer_index;
        auto* content = new RateEnvelopeEditorContent(
            layer,
            [this, layer_index](
                const mgstc::engine::RateEnvelope& rate, bool commit) {
                if (layer_index >= timbre_.layers.size()) {
                    return;
                }
                timbre_.layers[layer_index].volume_envelope.rate = rate;
                timbre_.layers[layer_index].volume_envelope.kind =
                    mgstc::engine::EnvelopeKind::Rate;
                if (static_cast<int>(layer_index) < layer_setups_.size()) {
                    layer_setups_[static_cast<int>(layer_index)]
                        ->syncFromLayer(timbre_.layers[layer_index]);
                }
                updateEnvelopeMmlPreview();
                repaint();
                if (edit_callback_) {
                    edit_callback_(timbre_, commit, false);
                }
            },
            [this, layer_index] {
                if (lfo_session_callback_) {
                    lfo_session_callback_(layer_index, true, false);
                }
            },
            [this] {
                if (lfo_poll_keys_callback_) {
                    lfo_poll_keys_callback_();
                }
            });
        rate_window_ = std::make_unique<RateEnvelopeEditorWindow>(
            content,
            [this] {
                closeRateEditor(true);
            });
        if (lfo_session_callback_) {
            lfo_session_callback_(layer_index, false, false);
        }
    }

    void syncRateEditorFromModel() {
        if (!rate_editor_layer_
            || *rate_editor_layer_ >= timbre_.layers.size()
            || rate_window_ == nullptr) {
            closeRateEditor(false);
            return;
        }
        const auto& layer = timbre_.layers[*rate_editor_layer_];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Rate) {
            closeRateEditor(true);
            return;
        }
        if (auto* editor = rate_window_->editor()) {
            editor->assignFromLayer(layer);
        }
    }

    void openSoftwareLfoDialog(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        selected_layer_ = static_cast<int>(layer_index);
        if (lfo_session_callback_) {
            lfo_session_callback_(layer_index, true, true);
        }
        UiScale::forceGlobalForNonEditorUi();
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("ソフトウェア LFO"),
            juce::Colour(0xFF1B222C));
        auto* content = new SoftwareLfoDialogContent(
            layer.source,
            layer.software_lfo,
            [this, layer_index](
                const mgstc::engine::SoftwareLfoSettings& settings) {
                if (layer_index >= timbre_.layers.size()) {
                    return;
                }
                timbre_.layers[layer_index].software_lfo = settings;
                if (settings.enabled) {
                    timbre_.layers[layer_index].pitch_sweep.enabled = false;
                }
                if (static_cast<int>(layer_index) < layer_setups_.size()) {
                    layer_setups_[static_cast<int>(layer_index)]
                        ->syncFromLayer(timbre_.layers[layer_index]);
                }
                repaint();
                if (edit_callback_) {
                    edit_callback_(timbre_, true, false);
                }
            },
            [this, layer_index] {
                if (lfo_session_callback_) {
                    lfo_session_callback_(layer_index, true, true);
                }
            },
            [this] {
                if (lfo_poll_keys_callback_) {
                    lfo_poll_keys_callback_();
                }
            });
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this](int) {
                    if (lfo_session_callback_) {
                        lfo_session_callback_(std::nullopt, false, false);
                    }
                }),
            true);
    }

    void setAudioService(SharedAudioHost* audio_service) noexcept {
        audio_service_ = audio_service;
    }

    void setPlaybackTempo(int tempo_bpm) {
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_bpm);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        repaint();
    }

    void setTimbre(
        const mgstc::engine::CompositeTimbre& timbre,
        bool reset_scroll_extent = false) {
        timbre_ = timbre;
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            timbre_.playback_tempo);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        clampAllLayersToBodyLimit();
        selected_layer_ = timbre_.layers.empty()
            ? -1
            : juce::jlimit(
                  0,
                  static_cast<int>(timbre_.layers.size()) - 1,
                  selected_layer_);
        refreshCountCeilingCache();
        if (reset_scroll_extent || !scroll_extent_initialized_) {
            resetScrollExtentToContent();
        } else {
            ensureScrollExtentIncludesContent();
        }
        updateScrollRanges();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        syncRateEditorFromModel();
        resized();
        repaint();
    }

    void refreshUiScaleFonts() {
        tempo_label_.setFont(UiFonts::body());
        UiFonts::styleBodyField(tempo_);
        UiFonts::styleBodyField(value_);
        UiFonts::refreshMgscPreviewFont(envelope_mml_preview_);
        repaint();
    }

    [[nodiscard]] bool appendScopeFrame(
        const mgstc::engine::OpllScopeFrame& frame,
        std::uint8_t midi_note) {
        constexpr float kVisibleAmplitude = 1.0e-5F;
        const bool was_visible = scope_visible_sample_count_ != 0;
        audition_note_ = midi_note;
        for (std::size_t index = 0;
             index < mgstc::engine::OpllScopeFrame::kSampleCount;
             ++index) {
            const std::array<float, 4> samples{
                frame.psg_samples[index],
                frame.scc_samples[index],
                frame.samples[index],
                frame.mixed_samples[index],
            };
            for (std::size_t channel = 0; channel < samples.size(); ++channel) {
                auto& cached = scope_history_[channel][scope_write_position_];
                if (std::abs(cached) >= kVisibleAmplitude) {
                    --scope_visible_sample_count_;
                }
                cached = samples[channel];
                if (std::abs(cached) >= kVisibleAmplitude) {
                    ++scope_visible_sample_count_;
                }
            }
            scope_write_position_ =
                (scope_write_position_ + 1) % kScopeHistorySize;
            scope_size_ = juce::jmin(kScopeHistorySize, scope_size_ + 1);
        }
        return was_visible || scope_visible_sample_count_ != 0;
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        // Single chrome row: help+tempo (paint) and channel add buttons.
        auto header = area.removeFromTop(textButtonH);
        tempo_.setBounds(header.removeFromRight(compositeTempoFieldW));
        tempo_label_.setBounds(header.removeFromRight(compositeTempoLabelW));
        header.removeFromRight(sm);
        opll_add_.setBounds(header.removeFromRight(compositeAddOpllW));
        header.removeFromRight(controlGap);
        scc_add_.setBounds(header.removeFromRight(compositeAddSccW));
        header.removeFromRight(controlGap);
        psg_add_.setBounds(header.removeFromRight(compositeAddPsgW));
        header.removeFromRight(sm);

        auto graph = graphArea();
        graph_clip_.setBounds(graph);
        vertical_scroll_.setBounds(
            graph.getRight(), graph.getY(), kScrollBarSize, graph.getHeight());
        horizontal_scroll_.setBounds(
            graph.getX(), graph.getBottom(), graph.getWidth(), kScrollBarSize);
        updateScrollRanges();
        applyCountColumnWidth();
        layoutLayerSetups();
        layoutEditToolsDock();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (event.mods.isPopupMenu()) {
            deleteCommandAt(event.getPosition());
            return;
        }
        const int layer = layerAt(event.getPosition());
        if (layer < 0) {
            return;
        }
        const bool selection_changed = selected_layer_ != layer;
        selected_layer_ = layer;
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const bool in_graph = graph.contains(event.getPosition());
        if (in_graph) {
            auto count_bounds = timelinePlotBounds(graph);
            selected_count_ = countAtX(count_bounds, event.getPosition().x);
            position_.setText(
                "ct " + juce::String(selected_count_),
                juce::dontSendNotification);
        }
        bool edit_tier = false;
        if (!selection_changed && in_graph) {
            edit_tier = hitEditLane(
                timbre_.layers[static_cast<std::size_t>(layer)],
                graph,
                event.getPosition());
        } else {
            edit_draw_bounds_.reset();
        }
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        if (selection_changed) {
            refreshCountCeilingCache();
            updateScrollRanges();
            ensureSelectedLaneVisible();
            drawing_ = false;
            resized();
            repaint();
            return;
        }
        if (!selection_changed && in_graph
            && selectedEditMode() == EditMode::Draw) {
            const auto& selected_layer =
                timbre_.layers[static_cast<std::size_t>(layer)];
            const auto slots = makeEditLaneSlots(graph, selected_layer.source);
            if (!slots.timbre.isEmpty()
                && slots.timbre.contains(event.getPosition())
                && selected_layer.source != mgstc::engine::TimbreSource::Psg) {
                parameter_.setSelectedId(
                    static_cast<int>(Parameter::Timbre),
                    juce::dontSendNotification);
                pickTimbreAt(selected_count_, false);
                drawing_ = false;
                repaint();
                return;
            }
            if (!slots.register_write.isEmpty()
                && slots.register_write.contains(event.getPosition())) {
                parameter_.setSelectedId(
                    static_cast<int>(Parameter::RegisterWrite),
                    juce::dontSendNotification);
                pickRegisterAt(selected_count_);
                drawing_ = false;
                repaint();
                return;
            }
        }
        if (selectedEditMode() == EditMode::Draw) {
            if (!in_graph) {
                drawing_ = false;
            } else if (edit_tier) {
                drawing_ = edit_draw_bounds_.has_value();
                if (drawing_) {
                    editFromMouse(event, false);
                    updateDrawHoverPopup(event.getPosition());
                }
            } else {
                // Selected edit-tier uses sublane bounds only; never map
                // Y against the full lane (wrong volume/pitch values).
                drawing_ = false;
            }
        } else if (in_graph) {
            setTimelineMarker(selected_count_, true);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (!drawing_) {
            return;
        }
        editFromMouse(event, false);
        // inspectFromMouse skips while drawing_; keep the idle-hover popup
        // at the pointer (SCC waveform mouseDrag does the same).
        updateDrawHoverPopup(event.getPosition());
        repaint();
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (!drawing_) {
            return;
        }
        editFromMouse(event, true);
        drawing_ = false;
        hover_mouse_pos_ = event.getPosition();
        inspectFromMouse(event.getPosition());
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override {
        const int layer = layerAt(event.getPosition());
        if (layer < 0
            || layer != selected_layer_
            || layer >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(layer)];
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const auto slots = makeEditLaneSlots(graph, selected.source);
        if (!slots.volume.isEmpty()
            && slots.volume.contains(event.getPosition())) {
            drawing_ = false;
            promptVolumeNumeric(event.getPosition());
            return;
        }
        if (!slots.pitch.isEmpty()
            && slots.pitch.contains(event.getPosition())) {
            drawing_ = false;
            promptPitchNumeric(event.getPosition());
        }
    }

    void mouseMove(const juce::MouseEvent& event) override {
        hover_mouse_pos_ = event.getPosition();
        inspectFromMouse(event.getPosition());
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (drawing_) {
            return;
        }
        hover_preview_value_.reset();
        hover_mouse_pos_.reset();
        hover_edit_parameter_.reset();
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        syncPointEditors();
        repaint();
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override {
        if (event.mods.isCtrlDown()) {
            const double factor = wheel.deltaY > 0.0 ? 0.85 : 1.18;
            zoomCountColumns(event.getPosition().x, factor);
            return;
        }
        const double delta = wheel.deltaY != 0.0
            ? static_cast<double>(wheel.deltaY)
            : static_cast<double>(wheel.deltaX);
        if (event.mods.isShiftDown()
            || (wheel.deltaY == 0.0 && wheel.deltaX != 0.0)) {
            const double requested_start =
                horizontal_scroll_.getCurrentRangeStart()
                - delta * 4.0;
            maybeExpandScrollExtent(static_cast<int>(std::ceil(
                requested_start
                + horizontal_scroll_.getCurrentRangeSize())));
            horizontal_scroll_.setCurrentRangeStart(
                requested_start,
                juce::dontSendNotification);
        } else {
            vertical_scroll_.setCurrentRangeStart(
                vertical_scroll_.getCurrentRangeStart()
                    - delta * static_cast<double>(UiLayout::xl * 2),
                juce::dontSendNotification);
        }
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        // #region agent log
        thread_local int paint_depth = 0;
        const int depth = ++paint_depth;
        const auto paint_t0 = juce::Time::getMillisecondCounterHiRes();
        dbg7ae407(
            "H1",
            "composite_timeline.cpp:CompositeTimeline::paint",
            "enter",
            std::string("{\"depth\":") + std::to_string(depth)
                + ",\"drawing\":" + (drawing_ ? "true" : "false") + "}");
        // #endregion
        graphics.fillAll(juce::Colour(0xFF182028));
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        auto header = area.removeFromTop(UiLayout::textButtonH);
        // Leave room for tempo + add buttons on the right (resized).
        header.removeFromRight(UiLayout::compositeHeaderControlsW);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(UiFonts::body(true));
        graphics.drawText(
            juce::String::fromUTF8(
                "共通時間軸  ホバーで値確認、枠クリックでch切替、ドラッグで編集"
                "（ホイールで縦スクロール、Shift＋ホイールで横、Ctrl＋ホイールでズーム）。"
                "音程は累積オフセット、\\は複数可（Shift＋ドラッグで同一ctへ追加）"),
            header,
            juce::Justification::centredLeft);
        area = graphArea();
        graphics.reduceClipRegion(area);

        // Keep draw bounds across mouseUp after mouseDown's repaint();
        // clearing here made Y map fall back to the full lane height.
        if (!drawing_) {
            edit_draw_bounds_.reset();
        }
        const std::size_t lane_count = timbre_.layers.size() + 1;
        const int scroll_y = juce::roundToInt(
            vertical_scroll_.getCurrentRangeStart());
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
            graph_bounds_.assign(timbre_.layers.size(), {});
            lane_frames_.assign(timbre_.layers.size(), {});
            int lane_y = area.getY() - scroll_y;
            for (int lane_index = 0;
                 lane_index < static_cast<int>(lane_count);
                 ++lane_index) {
                const int lane_height = laneHeightForIndex(lane_index);
                auto frame = juce::Rectangle<int>(
                    area.getX(),
                    lane_y,
                    area.getWidth(),
                    lane_height).reduced(0, UiLayout::editLaneFramePadV);
                lane_y += lane_height;
                const bool mixed_lane =
                    lane_index == static_cast<int>(timbre_.layers.size());
                const std::size_t index = static_cast<std::size_t>(lane_index);
                if (!mixed_lane) {
                    lane_frames_[index] = frame;
                }
                if (!frame.intersects(area)) {
                    continue;
                }
                const auto colour = mixed_lane
                    ? juce::Colour(0xFF35C4ED)
                    : sourceColour(timbre_.layers[index].source);
                graphics.setColour(juce::Colour(0xFF26313B));
                graphics.fillRoundedRectangle(frame.toFloat(), 5.0F);
                // Veil behind left setup/dock so scrolled envelope cannot show
                // through the controls (same family as frame fill).
                {
                    auto veil = frame;
                    veil.setWidth(UiLayout::compositeLaneLabelW);
                    graphics.setColour(juce::Colour(0xFF26313B).withAlpha(0.92F));
                    graphics.fillRect(veil);
                }
                const bool edit_tier = !mixed_lane
                    && static_cast<int>(index) == selected_layer_;
                if (edit_tier) {
                    const int setup_h = UiLayout::compositeSetupContentH;
                    auto dock = frame;
                    dock.setWidth(UiLayout::compositeLaneLabelW);
                    dock.removeFromTop(setup_h);
                    graphics.setColour(juce::Colour(0xFF1B222A));
                    graphics.fillRect(dock);
                    graphics.setColour(juce::Colour(0xFF8B9BAB));
                    graphics.setFont(UiFonts::dense(true));
                    graphics.drawText(
                        juce::String::fromUTF8("エンベロープ編集"),
                        dock.removeFromTop(UiLayout::editMarkerLaneH).reduced(
                            UiLayout::panelPad, 0),
                        juce::Justification::centredLeft,
                        false);
                }
                graphics.setColour(
                    !mixed_lane && static_cast<int>(index) == selected_layer_
                        ? colour
                        : colour.withAlpha(0.72F));
                graphics.drawRoundedRectangle(
                    frame.toFloat(), 5.0F,
                    !mixed_lane && static_cast<int>(index) == selected_layer_
                        ? 2.0F : 1.0F);

                    auto lane = frame;
                auto label = lane.removeFromLeft(UiLayout::compositeLaneLabelW)
                    .reduced(UiLayout::sm, 0);
            if (mixed_lane) {
            graphics.setColour(colour);
            graphics.setFont(UiFonts::body(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("MIX / 合成出力"),
                label, juce::Justification::centredLeft, 2);
            }

            lane.reduce(UiLayout::sm, UiLayout::editLaneInnerPadV);
            if (!mixed_lane) {
                lane_frames_[index] = frame;
                graph_bounds_[index] = lane;
            }
            // Same count X for every tier/ch (label gutter excluded).
            const auto plot = timelinePlotBounds(lane);
            drawCountGrid(graphics, plot);
            drawSelectedCountBand(graphics, plot);
            if (!mixed_lane) {
                const auto& layer = timbre_.layers[index];
                // Rest is pre-key-on (not an @e count). Tint the left wait
                // band only; envelope counts still start at ct0.
                const int delay_counts = static_cast<int>(
                    mgstc::engine::startDelayGridCounts(
                        layer.start_delay_form,
                        layer.start_delay_value,
                        playback_tempo_));
                if (delay_counts > 0) {
                    const int start_x = xForCount(plot, delay_counts);
                    const int wait_w = juce::jmax(0, start_x - plot.getX());
                    if (wait_w > 0) {
                        graphics.setColour(juce::Colours::black.withAlpha(
                            mgstc::engine::layerIsAudible(timbre_, index)
                                ? 0.28F : 0.14F));
                        graphics.fillRect(
                            plot.getX(),
                            plot.getY() + UiLayout::xs,
                            wait_w,
                            juce::jmax(1, plot.getHeight() - UiLayout::xs * 2));
                    }
                }
            }
            if (edit_tier) {
                paintEditTier(graphics, lane, index, colour);
                drawHoverValuePopup(graphics);
            } else {
                drawScope(graphics, plot, index, colour.withAlpha(0.48F));
                if (!mixed_lane) {
                    drawTimelineMarkers(graphics, plot, index, colour);
                    const auto& layer = timbre_.layers[index];
                    if (layer.volume_envelope.kind
                        == mgstc::engine::EnvelopeKind::Rate) {
                        const auto layout = rateEnvelopeHandlesForLayer(layer);
                        paintRateEnvelopeGraph(
                            graphics,
                            plot,
                            mgstc::app::makeLayerRateTrace(layer),
                            colour,
                            layer.source,
                            false,
                            &layout);
                    } else {
                        drawAutomation(
                            graphics, plot, index, colour, Parameter::Volume);
                        drawAutomation(
                            graphics,
                            plot,
                            index,
                            colour.withAlpha(0.75F),
                            Parameter::Pitch);
                    }
                }
            }
        }
        juce::ignoreUnused(numbers);
        // #region agent log
        const auto layout_t0 = juce::Time::getMillisecondCounterHiRes();
        // #endregion
        layoutLayerSetups();
        layoutEditToolsDock();
        // #region agent log
        const auto paint_t1 = juce::Time::getMillisecondCounterHiRes();
        dbg7ae407(
            "H1",
            "composite_timeline.cpp:CompositeTimeline::paint",
            "exit",
            std::string("{\"depth\":") + std::to_string(depth)
                + ",\"graph_ms\":"
                + std::to_string(layout_t0 - paint_t0)
                + ",\"layout_ms\":"
                + std::to_string(paint_t1 - layout_t0)
                + ",\"total_ms\":"
                + std::to_string(paint_t1 - paint_t0) + "}");
        --paint_depth;
        // #endregion
    }

private:
    enum class Parameter : int {
        Volume = 1,
        Pitch = 2,
        Timbre = 3,
        OpllTlAuto = 4,
        OpllFbAuto = 5,
        RegisterWrite = 6,
    };

    enum class EditMode : int {
        Draw = 1,
        End = 2,
        LoopStart = 3,
        LoopEnd = 4,
    };

    static constexpr int kDefaultVisibleCounts = 32;
    static constexpr int kScrollBarSize = 15;
    static constexpr int kPitchCommandMin = -127;
    static constexpr int kPitchCommandMax = 127;

    [[nodiscard]] static bool isRegisterAutoParameter(
        Parameter parameter) noexcept {
        return parameter == Parameter::OpllTlAuto
            || parameter == Parameter::OpllFbAuto;
    }

    [[nodiscard]] bool selectedLayerShowsRegisterAuto() const noexcept {
        return selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)].source
                == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoLane* selectedRegisterAutoLane() {
        if (!selectedLayerShowsRegisterAuto()
            || !isRegisterAutoParameter(selectedParameter())) {
            return nullptr;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        return selectedParameter() == Parameter::OpllTlAuto
            ? &layer.opll_tl_auto
            : &layer.opll_fb_auto;
    }

    [[nodiscard]] const mgstc::engine::OpllRegisterAutoLane*
    selectedRegisterAutoLane() const {
        return const_cast<Impl*>(this)
            ->selectedRegisterAutoLane();
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoTarget
    selectedRegisterAutoTarget() const noexcept {
        return selectedParameter() == Parameter::OpllFbAuto
            ? mgstc::engine::OpllRegisterAutoTarget::Feedback
            : mgstc::engine::OpllRegisterAutoTarget::TotalLevel;
    }

    void configureAutoParamEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
        editor.setInputRestrictions(3, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] {
            applyRegisterAutoFromEditors(true);
        };
        addAndMakeVisible(editor);
    }

    [[nodiscard]] const mgstc::engine::TimbreLibrary* timbreLibrary() const {
        return timbre_library_callback_ != nullptr
            ? timbre_library_callback_()
            : nullptr;
    }

    [[nodiscard]] bool registerAutoAvailableAtSelectedCount() const {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return false;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source != mgstc::engine::TimbreSource::Opll) {
            return false;
        }
        return mgstc::engine::opllRegisterAutoAvailableAt(
            layer, static_cast<std::uint32_t>(juce::jmax(0, selected_count_)));
    }

    void syncRegisterAutoEditors() {
        const auto* lane = selectedRegisterAutoLane();
        const bool selected =
            selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        const bool enabled = lane != nullptr;
        const auto target = selectedRegisterAutoTarget();
        const bool owned = enabled
            && selected
            && mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_,
                   static_cast<std::size_t>(selected_layer_),
                   target);
        const bool count_ok =
            enabled && owned && registerAutoAvailableAtSelectedCount();
        // Other OPLL layers may view Off, but cannot take ownership.
        auto_mode_.setEnabled(enabled && (owned || !lane->active()));
        auto_depth_.setEnabled(enabled && owned);
        auto_speed_.setEnabled(enabled && owned);
        auto_coarseness_.setEnabled(enabled && owned);
        auto_stop_.setEnabled(enabled && owned);
        if (!enabled) {
            return;
        }
        auto_mode_.setSelectedId(
            static_cast<int>(lane->mode) + 1,
            juce::dontSendNotification);
        auto_depth_.setText(juce::String(static_cast<int>(lane->depth)), false);
        auto_speed_.setText(
            juce::String(static_cast<int>(lane->change_speed)), false);
        auto_coarseness_.setText(
            juce::String(static_cast<int>(lane->coarseness)), false);
        auto_stop_.setText(
            juce::String(static_cast<int>(lane->stop_position)), false);
        if (!owned) {
            const auto owner = mgstc::engine::opllRegisterAutoOwnerLayer(
                timbre_, target);
            juce::String owner_label = juce::String::fromUTF8("別のOPLL ch");
            if (owner.has_value()
                && *owner < timbre_.layers.size()) {
                owner_label = juce::String::fromUTF8("OPLL Ch.")
                    + juce::String(
                          static_cast<int>(
                              timbre_.layers[*owner].channel) + 1);
            }
            const auto kind = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? juce::String::fromUTF8("TL自動")
                : juce::String::fromUTF8("FB自動");
            auto_mode_.setTooltip(
                kind + juce::String::fromUTF8("はチップ全体で1本のみ。")
                + owner_label
                + juce::String::fromUTF8("が使用中のため、このchでは設定できません"
                                         "（TLとFBを別chへ分けるのは可）"));
        } else if (!count_ok && isRegisterAutoParameter(selectedParameter())) {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "選択カウントはOPLL ROM音色区間のため、TL/FB自動のyは出力されません"));
        } else {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "TL/FB自動の時間変化モード。出力は展開したyレジスタ書き込み。"
                "TL・FBはそれぞれOPLL全体で1本（別ch分担可）"));
        }
    }

    void applyRegisterAutoFromEditors(bool set_start_count) {
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto target = selectedRegisterAutoTarget();
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto next_mode =
            static_cast<mgstc::engine::OpllRegisterAutoMode>(
                juce::jlimit(0, 4, auto_mode_.getSelectedId() - 1));
        if (next_mode != mgstc::engine::OpllRegisterAutoMode::Off
            && !mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_, layer_index, target)) {
            reportStatus(juce::String::fromUTF8(
                "TL/FB自動はそれぞれOPLL全体で1チャンネルのみ設定できます"
                "（例: 1ch=FB・2ch=TL の分担は可）"));
            syncRegisterAutoEditors();
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        lane->mode = next_mode;
        if (set_start_count) {
            if (!registerAutoAvailableAtSelectedCount()) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB自動の開始ステップを置けません"));
                syncRegisterAutoEditors();
                return;
            }
            lane->start_count = static_cast<std::uint32_t>(
                juce::jmax(0, selected_count_));
        }
        lane->depth = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_depth_.getText().getIntValue()));
        lane->change_speed = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_speed_.getText().getIntValue()));
        lane->coarseness = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_coarseness_.getText().getIntValue()));
        lane->stop_position = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_stop_.getText().getIntValue()));
        if (lane->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve
            && lane->free_curve.empty()
            && set_start_count) {
            lane->free_curve.push_back(lane->depth);
        }
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        syncRegisterAutoEditors();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, true);
        }
    }

    void editRegisterAutoFreeCurve(
        int timeline_count,
        int parameter_value,
        bool commit) {
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || lane->mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
            return;
        }
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto target = selectedRegisterAutoTarget();
        if (!mgstc::engine::opllRegisterAutoOwnedByLayer(
                timbre_, layer_index, target)) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "このTL/FB自動は他のOPLLチャンネルが使用中です"));
            }
            return;
        }
        const auto& layer = timbre_.layers[layer_index];
        if (!mgstc::engine::opllRegisterAutoAvailableAt(
                layer,
                static_cast<std::uint32_t>(juce::jmax(0, timeline_count)))) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB自動を描けません"));
            }
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        parameter_value = juce::jlimit(0, maximum, parameter_value);
        timeline_count = juce::jmax(0, timeline_count);
        const auto step = juce::jmax<std::uint8_t>(1, lane->coarseness);
        if (lane->free_curve.empty()) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
        }
        if (static_cast<std::uint32_t>(timeline_count) < lane->start_count) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
            lane->free_curve.clear();
        }
        const auto offset =
            (static_cast<std::uint32_t>(timeline_count) - lane->start_count)
            / step;
        if (lane->free_curve.size() <= offset) {
            const auto fill = lane->free_curve.empty()
                ? static_cast<std::uint8_t>(parameter_value)
                : lane->free_curve.back();
            lane->free_curve.resize(offset + 1, fill);
        }
        lane->free_curve[offset] =
            static_cast<std::uint8_t>(parameter_value);
        syncPointEditors();
        if (commit && edit_callback_) {
            edit_callback_(timbre_, true, true);
        }
        repaint();
    }

    [[nodiscard]] int laneHeightForIndex(int lane_index) const noexcept {
        if (lane_index == static_cast<int>(timbre_.layers.size())) {
            return UiLayout::compositeMixLaneH;
        }
        if (lane_index == selected_layer_) {
            if (lane_index >= 0
                && lane_index < static_cast<int>(timbre_.layers.size())
                && timbre_.layers[static_cast<std::size_t>(lane_index)].source
                    == mgstc::engine::TimbreSource::Opll) {
                return UiLayout::compositeEditLaneH
                    + UiLayout::compositeEditLaneOpllExtraH;
            }
            return UiLayout::compositeEditLaneH;
        }
        return UiLayout::compositeChannelLaneH;
    }

    [[nodiscard]] int totalLaneStackHeight() const noexcept {
        const int lane_count = juce::jmax(
            1, static_cast<int>(timbre_.layers.size()) + 1);
        int total = 0;
        for (int index = 0; index < lane_count; ++index) {
            total += laneHeightForIndex(index);
        }
        return juce::jmax(1, total);
    }

    [[nodiscard]] int lanePixelOffset(int lane_index) const noexcept {
        int y = 0;
        const int last = juce::jmax(0, lane_index);
        for (int index = 0; index < last; ++index) {
            y += laneHeightForIndex(index);
        }
        return y;
    }

    [[nodiscard]] int maximumCount() const noexcept {
        return cached_count_ceiling_;
    }

    [[nodiscard]] int maximumCountForSelectedLayer() const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return cached_count_ceiling_;
        }
        return cached_layer_fit_;
    }

    [[nodiscard]] int contentHorizonCount() const noexcept {
        std::uint64_t horizon = 1;
        const auto include_events =
            [&horizon](const std::vector<mgstc::engine::EnvelopeEvent>& events) {
                for (const auto& event : events) {
                    horizon = std::max<std::uint64_t>(
                        horizon, event.count);
                }
            };
        const auto include_auto =
            [&horizon](const mgstc::engine::OpllRegisterAutoLane& lane) {
                if (!lane.active()) {
                    return;
                }
                std::uint64_t last = lane.start_count;
                if (!lane.free_curve.empty()) {
                    last += static_cast<std::uint64_t>(
                                lane.free_curve.size() - 1)
                        * juce::jmax<std::uint8_t>(1, lane.coarseness);
                }
                horizon = std::max(horizon, last);
            };
        for (const auto& layer : timbre_.layers) {
            const auto& timeline = layer.envelope_timeline;
            horizon = std::max<std::uint64_t>(
                horizon, timeline.length_counts);
            if (timeline.loop_start_count) {
                horizon = std::max<std::uint64_t>(
                    horizon, *timeline.loop_start_count);
            }
            if (timeline.loop_end_count) {
                horizon = std::max<std::uint64_t>(
                    horizon, *timeline.loop_end_count);
            }
            include_events(layer.volume_envelope.events);
            include_events(layer.pitch_envelope.events);
            include_events(layer.timbre_automation);
            include_auto(layer.opll_tl_auto);
            include_auto(layer.opll_fb_auto);
        }
        return static_cast<int>(std::min<std::uint64_t>(
            horizon, static_cast<std::uint64_t>(maximumCount())));
    }

    [[nodiscard]] int scrollExpansionCounts() const noexcept {
        return juce::jmax(
            kDefaultVisibleCounts,
            static_cast<int>(std::ceil(
                horizontal_scroll_.getCurrentRangeSize())));
    }

    [[nodiscard]] int initialScrollExtentForContent() const noexcept {
        return juce::jlimit(
            1,
            maximumCount(),
            contentHorizonCount() + scrollExpansionCounts());
    }

    void resetScrollExtentToContent() {
        scroll_extent_counts_ = initialScrollExtentForContent();
        scroll_extent_initialized_ = true;
        horizontal_scroll_.setCurrentRangeStart(
            0.0, juce::dontSendNotification);
    }

    void ensureScrollExtentIncludesContent() {
        scroll_extent_counts_ = juce::jlimit(
            1,
            maximumCount(),
            juce::jmax(
                scroll_extent_counts_,
                initialScrollExtentForContent()));
        scroll_extent_initialized_ = true;
    }

    void maybeExpandScrollExtent(int target_count) {
        if (scroll_extent_counts_ >= maximumCount()
            || target_count < scroll_extent_counts_ - 1) {
            return;
        }
        const int expanded = juce::jlimit(
            1,
            maximumCount(),
            juce::jmax(
                target_count,
                scroll_extent_counts_ + scrollExpansionCounts()));
        if (expanded == scroll_extent_counts_) {
            return;
        }
        scroll_extent_counts_ = expanded;
        horizontal_scroll_.setRangeLimits(
            0.0,
            static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
    }

    void refreshCountCeilingCache() {
        std::uint32_t max_fit = 1;
        std::uint32_t selected_fit = 1;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto* library = timbreLibrary();
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            const auto fit = mgstc::engine::maxEnvelopeLengthFittingBodyLimit(
                layer,
                mgstc::engine::kMgscEnvelopeCompiledByteLimit,
                &numbers,
                library);
            max_fit = std::max(max_fit, fit);
            max_fit = std::max(max_fit, layer.envelope_timeline.length_counts);
            if (static_cast<int>(index) == selected_layer_) {
                selected_fit = fit;
            }
        }
        cached_count_ceiling_ = static_cast<int>(std::min(
            max_fit, mgstc::engine::kMgscEnvelopeUiLengthCap));
        cached_layer_fit_ = selected_layer_ >= 0
            ? static_cast<int>(std::min(
                  selected_fit, mgstc::engine::kMgscEnvelopeUiLengthCap))
            : cached_count_ceiling_;
    }

    void clampLayerToBodyLimit(mgstc::engine::CompositeLayer& layer) {
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto fit = mgstc::engine::maxEnvelopeLengthFittingBodyLimit(
            layer,
            mgstc::engine::kMgscEnvelopeCompiledByteLimit,
            &numbers,
            timbreLibrary());
        auto& timeline = layer.envelope_timeline;
        if (timeline.length_counts > fit) {
            timeline.length_counts = juce::jmax<std::uint32_t>(1, fit);
        }
        if (timeline.loop_start_count
            && *timeline.loop_start_count > timeline.length_counts) {
            timeline.loop_start_count = timeline.length_counts;
        }
        if (timeline.loop_end_count
            && *timeline.loop_end_count > timeline.length_counts) {
            timeline.loop_end_count = timeline.length_counts;
        }
    }

    void clampAllLayersToBodyLimit() {
        for (auto& layer : timbre_.layers) {
            clampLayerToBodyLimit(layer);
        }
        refreshCountCeilingCache();
    }

    [[nodiscard]] EditMode selectedEditMode() const noexcept {
        return static_cast<EditMode>(edit_mode_.getSelectedId());
    }

    [[nodiscard]] Parameter selectedParameter() const noexcept {
        return static_cast<Parameter>(parameter_.getSelectedId());
    }

    [[nodiscard]] juce::String selectedParameterName() const {
        switch (selectedParameter()) {
        case Parameter::Volume:
            return juce::String::fromUTF8("音量");
        case Parameter::Pitch:
            return juce::String::fromUTF8("音程");
        case Parameter::Timbre:
            return juce::String::fromUTF8("音色番号");
        case Parameter::RegisterWrite:
            return juce::String::fromUTF8("手動y");
        case Parameter::OpllTlAuto:
            return juce::String::fromUTF8("TL自動");
        case Parameter::OpllFbAuto:
            return juce::String::fromUTF8("FB自動");
        }
        return {};
    }

    void configureInspectorEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
        editor.setInputRestrictions(5, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTextToShowWhenEmpty(
            juce::String::fromUTF8("なし"), juce::Colour(0xFF8896A3));
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] { applyInspector(); };
        addAndMakeVisible(editor);
    }

    void configureInspectorLabel(
        juce::Label& label,
        const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
    }

    [[nodiscard]] std::optional<std::uint32_t> inspectorCount(
        const juce::TextEditor& editor,
        std::uint32_t length) const {
        const auto text = editor.getText().trim();
        if (text.isEmpty()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(juce::jlimit(
            0, static_cast<int>(length), text.getIntValue()));
    }

    void syncInspector() {
        const bool has_selection = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        length_editor_.setEnabled(has_selection);
        loop_start_editor_.setEnabled(has_selection);
        loop_end_editor_.setEnabled(has_selection);
        inspector_apply_.setEnabled(has_selection);
        clear_loop_.setEnabled(has_selection);
        if (!has_selection) {
            inspector_selection_.setText(
                juce::String::fromUTF8("チャンネル未選択"),
                juce::dontSendNotification);
            length_editor_.setText({}, false);
            loop_start_editor_.setText({}, false);
            loop_end_editor_.setText({}, false);
            command_stack_.clearStack();
            return;
        }

        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto& timeline = layer.envelope_timeline;
        inspector_selection_.setText(
            juce::String::fromUTF8(layer.name.c_str())
                + juce::String::fromUTF8(" / ソフトウェアEG"),
            juce::dontSendNotification);
        length_editor_.setText(
            juce::String(static_cast<int>(timeline.length_counts)), false);
        loop_start_editor_.setText(
            timeline.loop_start_count
                ? juce::String(static_cast<int>(*timeline.loop_start_count))
                : juce::String{},
            false);
        loop_end_editor_.setText(
            timeline.loop_end_count
                ? juce::String(static_cast<int>(*timeline.loop_end_count))
                : juce::String{},
            false);
        refreshCommandStack(layer);
    }

    void applyInspector() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto length = static_cast<std::uint32_t>(juce::jlimit(
            1,
            maximumCountForSelectedLayer(),
            length_editor_.getText().getIntValue()));
        auto loop_start = inspectorCount(
            loop_start_editor_, length);
        auto loop_end = inspectorCount(
            loop_end_editor_, length);
        mgstc::engine::setEnvelopeTimelineRange(
            timeline, length, loop_start, loop_end);
        refreshCountCeilingCache();
        maybeExpandScrollExtent(static_cast<int>(length));
        syncInspector();
        updateScrollRanges();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, true);
        }
    }

    void clearSelectedLoop() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        timeline.loop_start_count.reset();
        timeline.loop_end_count.reset();
        syncInspector();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, true);
        }
    }

    [[nodiscard]] std::pair<int, int> valueRange() const noexcept {
        return valueRangeFor(selectedParameter());
    }

    [[nodiscard]] static std::pair<int, int> valueRangeFor(
        Parameter parameter) noexcept {
        switch (parameter) {
        case Parameter::Volume:
            return {0, 15};
        case Parameter::Pitch:
            return {kPitchCommandMin, kPitchCommandMax};
        case Parameter::Timbre:
            return {0, 31};
        case Parameter::RegisterWrite:
            return {0, 255};
        case Parameter::OpllTlAuto:
            return {0, 63};
        case Parameter::OpllFbAuto:
            return {0, 7};
        }
        return {0, 15};
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    selectedEvents(const mgstc::engine::CompositeLayer& layer) const {
        return eventsFor(layer, selectedParameter());
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    eventsFor(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter) const {
        if (parameter == Parameter::Volume) {
            return layer.volume_envelope.events;
        }
        if (parameter == Parameter::Pitch) {
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    [[nodiscard]] mgstc::engine::EnvelopeEventKind selectedEventKind() const {
        return eventKindFor(selectedParameter());
    }

    [[nodiscard]] static mgstc::engine::EnvelopeEventKind eventKindFor(
        Parameter parameter) noexcept {
        if (parameter == Parameter::Volume) {
            return mgstc::engine::EnvelopeEventKind::Volume;
        }
        if (parameter == Parameter::Pitch) {
            return mgstc::engine::EnvelopeEventKind::Pitch;
        }
        if (parameter == Parameter::RegisterWrite) {
            return mgstc::engine::EnvelopeEventKind::RegisterWrite;
        }
        return mgstc::engine::EnvelopeEventKind::Timbre;
    }

    [[nodiscard]] const mgstc::engine::EnvelopeTimeline& selectedTimeline(
        const mgstc::engine::CompositeLayer& layer) const {
        return layer.envelope_timeline;
    }

    [[nodiscard]] mgstc::engine::EnvelopeTimeline& selectedTimeline(
        mgstc::engine::CompositeLayer& layer) {
        return const_cast<mgstc::engine::EnvelopeTimeline&>(
            std::as_const(*this).selectedTimeline(layer));
    }

    void updateAddButtons() {
        psg_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Psg).has_value());
        scc_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Scc).has_value());
        opll_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Opll).has_value());
        for (int index = 0; index < layer_setups_.size(); ++index) {
            if (index >= static_cast<int>(timbre_.layers.size())) {
                break;
            }
            layer_setups_[index]->setDuplicateEnabled(
                mgstc::engine::firstAvailableChannel(
                    timbre_, timbre_.layers[static_cast<std::size_t>(index)]
                                 .source)
                    .has_value());
        }
    }

    void addLayer(mgstc::engine::TimbreSource source) {
        const auto free_channel = mgstc::engine::firstAvailableChannel(
            timbre_, source);
        if (!free_channel) {
            return;
        }
        const auto defaults = mgstc::engine::defaultCompositeTimbre();
        const auto template_layer = std::find_if(
            defaults.layers.begin(), defaults.layers.end(),
            [source](const auto& layer) {
                return layer.source == source;
            });
        if (template_layer == defaults.layers.end()) {
            return;
        }
        auto layer = *template_layer;
        layer.channel = *free_channel;
        const char* source_name = source == mgstc::engine::TimbreSource::Psg
            ? "PSG"
            : source == mgstc::engine::TimbreSource::Scc ? "SCC" : "OPLL";
        layer.name = std::string(source_name) + " Ch."
            + std::to_string(static_cast<int>(layer.channel) + 1);
        layer.envelope_number =
            mgstc::engine::nextFreeEnvelopeNumber(timbre_);
        mgstc::engine::seedDefaultLayerTimbre(layer);
        timbre_.layers.push_back(std::move(layer));
        selected_layer_ = static_cast<int>(timbre_.layers.size()) - 1;
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, false);
        }
    }

    void requestRemoveLayer(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        const auto layer_name = juce::String::fromUTF8(
            timbre_.layers[layer_index].name.c_str());
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle(juce::String::fromUTF8("チャンネル削除"))
                .withMessage(
                    layer_name
                    + juce::String::fromUTF8(
                        " とそのエンベロープを削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル")),
            [safe = juce::Component::SafePointer<Impl>(this), layer_index](
                int result) {
                if (safe != nullptr && result == 1) {
                    safe->removeLayerAt(layer_index);
                }
            });
    }

    void removeLayerAt(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        const int removed = static_cast<int>(layer_index);
        if (!mgstc::engine::removeCompositeLayer(timbre_, layer_index)) {
            return;
        }
        if (timbre_.layers.empty()) {
            selected_layer_ = -1;
        } else if (selected_layer_ == removed) {
            selected_layer_ = juce::jmin(
                selected_layer_,
                static_cast<int>(timbre_.layers.size()) - 1);
        } else if (selected_layer_ > removed) {
            --selected_layer_;
        }
        selected_count_ = 0;
        position_.setText("ct 0", juce::dontSendNotification);
        refreshCountCeilingCache();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        closeRateEditor(true);
        resized();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, false);
        }
    }

    void duplicateLayerAt(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        if (layer_index < static_cast<std::size_t>(layer_setups_.size())) {
            layer_setups_[static_cast<int>(layer_index)]->applyToLayer(
                timbre_.layers[layer_index]);
        }
        const auto copied = mgstc::engine::duplicateCompositeLayer(
            timbre_, layer_index);
        if (!copied) {
            updateAddButtons();
            return;
        }
        selected_layer_ = static_cast<int>(*copied);
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, false);
        }
    }

    std::vector<mgstc::engine::EnvelopeEvent>& selectedEvents(
        mgstc::engine::CompositeLayer& layer) {
        if (selectedParameter() == Parameter::Volume) {
            if (layer.volume_envelope.kind
                != mgstc::engine::EnvelopeKind::Rate) {
                layer.volume_envelope.kind =
                    mgstc::engine::EnvelopeKind::Sequence;
            }
            return layer.volume_envelope.events;
        }
        if (selectedParameter() == Parameter::Pitch) {
            layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    void editFromMouse(const juce::MouseEvent& event, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto point = event.getPosition();
        const auto bounds = edit_draw_bounds_.value_or(
            timelinePlotBounds(
                graph_bounds_[static_cast<std::size_t>(selected_layer_)]));
        if (bounds.isEmpty()) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter == Parameter::Timbre
            || parameter == Parameter::RegisterWrite) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            return;
        }
        const int count = countAtX(bounds, point.x);
        if (isRegisterAutoParameter(parameter)) {
            const auto [minimum, maximum] = valueRangeFor(parameter);
            editRegisterAutoFreeCurve(
                count,
                valueFromY(bounds, point.y, minimum, maximum),
                commit);
            return;
        }
        if (parameter == Parameter::Pitch) {
            const auto range = pitchDisplayRange(layer);
            editPitchCumulative(
                count,
                pitchValueFromY(
                    bounds, point.y, range.second),
                commit,
                event.mods.isShiftDown());
            return;
        }
        editPoint(
            count,
            volumeValueFromY(layer.source, bounds, point.y),
            commit);
    }

    void updateDrawHoverPopup(juce::Point<int> point) {
        hover_mouse_pos_ = point;
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            hover_preview_value_.reset();
            return;
        }
        if (!edit_draw_bounds_ || edit_draw_bounds_->isEmpty()
            || selected_layer_ < 0
            || selected_layer_
                >= static_cast<int>(timbre_.layers.size())) {
            hover_preview_value_.reset();
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (parameter == Parameter::Pitch) {
            const auto range = pitchDisplayRange(layer);
            hover_preview_value_ = pitchValueFromY(
                *edit_draw_bounds_,
                point.y,
                range.second);
        } else if (parameter == Parameter::Volume) {
            hover_preview_value_ = volumeValueFromY(
                layer.source, *edit_draw_bounds_, point.y);
        } else {
            hover_preview_value_ = valueFromY(
                *edit_draw_bounds_, point.y, parameter);
        }
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        int minimum,
        int maximum) noexcept {
        if (bounds.isEmpty()) {
            return minimum;
        }
        return juce::jlimit(
            minimum, maximum,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - y)
                * (maximum - minimum) / juce::jmax(1, bounds.getHeight())
                + minimum));
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        Parameter parameter) noexcept {
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return valueFromY(bounds, y, minimum, maximum);
    }

    [[nodiscard]] static int volumeValueFromY(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int y) noexcept {
        juce::ignoreUnused(source);
        if (bounds.isEmpty()) {
            return 0;
        }
        const int height = juce::jmax(1, bounds.getHeight());
        const int y_clamped = juce::jlimit(
            bounds.getY(), bounds.getBottom() - 1, y);
        return juce::jlimit(
            0,
            15,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - y_clamped)
                * 15.0
                / static_cast<double>(height)));
    }

    [[nodiscard]] static float volumeYFromValue(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int volume) noexcept {
        juce::ignoreUnused(source);
        const int height = juce::jmax(1, bounds.getHeight());
        return static_cast<float>(
            bounds.getBottom()
            - juce::roundToInt(
                  static_cast<double>(juce::jlimit(0, 15, volume))
                  * static_cast<double>(height) / 15.0));
    }

    [[nodiscard]] static float chipTransferYFromValue(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int envelope_volume,
        int track_volume) noexcept {
        const int height = juce::jmax(1, bounds.getHeight());
        const auto output_volume =
            mgstc::engine::sequenceOutputVolume(
                static_cast<std::uint8_t>(juce::jlimit(
                    0, 15, envelope_volume)),
                static_cast<std::uint8_t>(juce::jlimit(
                    0, 15, track_volume)));
        const int pixels =
            mgstc::engine::chipVolumeDisplayPixelsFromBottom(
                source, output_volume, height);
        return static_cast<float>(bounds.getBottom() - pixels);
    }

    // First editable frequency unit is 1 screen pixel from the 0 line.
    // Remaining |value| maps onto the rest of the half-lane (existing extent scale).
    [[nodiscard]] static int pitchValueFromY(
        juce::Rectangle<int> bounds,
        int y,
        int extent) noexcept {
        if (bounds.isEmpty()) {
            return 0;
        }
        const float centre = static_cast<float>(bounds.getCentreY());
        const float half = static_cast<float>(bounds.getHeight()) * 0.5F;
        const float dy = centre - static_cast<float>(y);
        const float first_px = 1.0F;
        if (std::abs(dy) <= first_px) {
            return juce::roundToInt(dy);
        }
        const int mag_extent = juce::jmax(1, extent);
        const float remaining = juce::jmax(0.0F, half - first_px);
        if (remaining <= 0.0F) {
            return dy >= 0.0F ? mag_extent : -mag_extent;
        }
        const float extra = std::abs(dy) - first_px;
        const float t = juce::jlimit(0.0F, 1.0F, extra / remaining);
        const int mag = 1
            + juce::roundToInt(t * static_cast<float>(mag_extent - 1));
        return dy >= 0.0F ? mag : -mag;
    }

    [[nodiscard]] static float pitchYFromValue(
        juce::Rectangle<int> bounds,
        int value,
        int extent) noexcept {
        const float centre = static_cast<float>(bounds.getCentreY());
        const float half = static_cast<float>(bounds.getHeight()) * 0.5F;
        const float first_px = 1.0F;
        if (std::abs(value) <= 1) {
            return centre - static_cast<float>(value) * first_px;
        }
        const int mag_extent = juce::jmax(1, extent);
        const float remaining = juce::jmax(0.0F, half - first_px);
        const float t = mag_extent <= 1
            ? 1.0F
            : static_cast<float>(std::abs(value) - 1)
                / static_cast<float>(mag_extent - 1);
        const float pixels = first_px + juce::jlimit(0.0F, 1.0F, t) * remaining;
        return centre - std::copysign(pixels, static_cast<float>(value));
    }

    [[nodiscard]] int storedValueAt(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter,
        int timeline_count) const {
        if (isRegisterAutoParameter(parameter)) {
            const auto target = parameter == Parameter::OpllTlAuto
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                : mgstc::engine::OpllRegisterAutoTarget::Feedback;
            const auto count = static_cast<std::uint32_t>(
                juce::jmax(0, timeline_count));
            // Prefer effective field after @ / manual y / TL·FB auto (§7.5).
            if (const auto regs = mgstc::engine::opllOriginalRegisterImageAt(
                    layer, count, timbreLibrary(), true, true)) {
                return target
                        == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                    ? static_cast<int>((*regs)[2] & 0x3F)
                    : static_cast<int>((*regs)[3] & 0x07);
            }
            const auto& auto_lane = parameter == Parameter::OpllTlAuto
                ? layer.opll_tl_auto
                : layer.opll_fb_auto;
            if (const auto value = mgstc::engine::opllRegisterAutoValueAt(
                    auto_lane,
                    target,
                    count,
                    layer.envelope_timeline.length_counts)) {
                return *value;
            }
            return 0;
        }
        if (parameter == Parameter::Pitch) {
            return cumulativePitchAt(layer, timeline_count);
        }
        if (parameter == Parameter::RegisterWrite
            || parameter == Parameter::Timbre) {
            return 0;
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        int current = parameter == Parameter::Volume ? layer.volume : 0;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count <= static_cast<std::uint32_t>(timeline_count)) {
                current = event.value;
            }
        }
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return juce::jlimit(minimum, maximum, current);
    }

    [[nodiscard]] int lastPitchCommandAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        int value = 0;
        bool found = false;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)) {
                value = event.value;
                found = true;
            }
        }
        juce::ignoreUnused(found);
        return value;
    }

    [[nodiscard]] int layerAt(juce::Point<int> point) const noexcept {
        if (!graphArea().contains(point)) {
            return -1;
        }
        for (std::size_t index = 0; index < lane_frames_.size(); ++index) {
            if (lane_frames_[index].contains(point)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void inspectFromMouse(juce::Point<int> point) {
        if (drawing_) {
            return;
        }
        const int layer = layerAt(point);
        hover_edit_parameter_.reset();
        if (layer < 0) {
            hover_preview_value_.reset();
            setMouseCursor(juce::MouseCursor::CrosshairCursor);
            return;
        }
        if (layer != selected_layer_) {
            hover_preview_value_.reset();
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
            return;
        }
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const bool in_graph = graph.contains(point);
        const int previous_count = selected_count_;
        bool parameter_changed = false;
        std::optional<int> mapped_value;
        auto count_bounds = timelinePlotBounds(graph);
        if (in_graph) {
            const auto slots = makeEditLaneSlots(graph, selected.source);
            const struct LaneHit {
                Parameter parameter;
                juce::Rectangle<int> slot;
                bool drawable;
            } hits[] = {
                {Parameter::Volume, slots.volume, true},
                {Parameter::Pitch, slots.pitch, true},
                {Parameter::Timbre, slots.timbre, false},
                {Parameter::RegisterWrite, slots.register_write, false},
                {Parameter::OpllTlAuto, slots.tl_auto, true},
                {Parameter::OpllFbAuto, slots.fb_auto, true},
            };
            for (const auto& hit : hits) {
                if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                    continue;
                }
                hover_edit_parameter_ = hit.parameter;
                if (selectedParameter() != hit.parameter) {
                    parameter_.setSelectedId(
                        static_cast<int>(hit.parameter),
                        juce::dontSendNotification);
                    syncRegisterAutoEditors();
                    parameter_changed = true;
                }
                if (hit.drawable) {
                    if (hit.parameter == Parameter::Pitch) {
                        const auto range = pitchDisplayRange(selected);
                        mapped_value = pitchValueFromY(
                            graphStrip(hit.slot),
                            point.y,
                            range.second);
                    } else if (hit.parameter == Parameter::Volume) {
                        mapped_value = volumeValueFromY(
                            selected.source,
                            graphStrip(hit.slot),
                            point.y);
                    } else {
                        mapped_value = valueFromY(
                            graphStrip(hit.slot), point.y, hit.parameter);
                    }
                }
                break;
            }
            selected_count_ = countAtX(count_bounds, point.x);
        }
        const int shown = mapped_value.value_or(
            storedValueAt(selected, selectedParameter(), selected_count_));
        const bool count_changed = selected_count_ != previous_count;
        hover_preview_value_ = mapped_value;
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        if (selectedParameter() == Parameter::Timbre && !mapped_value) {
            value_.setText(
                timbreInspectLabel(selected, selected_count_), false);
        } else if (
            selectedParameter() == Parameter::RegisterWrite
            && !mapped_value) {
            value_.setText(
                registerInspectLabel(selected, selected_count_), false);
        } else {
            value_.setText(juce::String(shown), false);
        }
        if (selectedParameter() == Parameter::RegisterWrite) {
            const auto tip = registerInspectTooltip(selected, selected_count_);
            value_.setTooltip(tip);
        } else if (selectedParameter() == Parameter::Pitch) {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の累積オフセット（未確定）。"
                          "「設定」は1個の\\命令を追加します")
                    : juce::String::fromUTF8(
                          "元音域からの累積オフセット。"
                          "「設定」は入力値を1個の\\として追加。"
                          "Shift＋ドラッグでも同一カウントへ追加"));
        } else if (isRegisterAutoParameter(selectedParameter())) {
            const bool available = mgstc::engine::opllRegisterAutoAvailableAt(
                selected,
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)));
            value_.setTooltip(
                available
                    ? juce::String::fromUTF8(
                          "TL/FB自動。展開時はアクティブなオリジナル音色の"
                          "レジスタ2/3をベースにフィールドだけ書き換えます")
                    : juce::String::fromUTF8(
                          "ROM音色区間ではTL/FB自動は使えません（yを出力しません）"));
            if (!available && (count_changed || parameter_changed)) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間 — TL/FB自動は無効"));
            }
        } else {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の値（未確定）。クリックまたは「設定」で反映")
                    : juce::String::fromUTF8("選択パラメーターの値"));
        }
        if (count_changed || parameter_changed) {
            refreshCommandStack(selected);
            if (parameter_changed) {
                syncInspector();
            }
            repaint();
        }
    }

    void editPoint(
        int timeline_count,
        int parameter_value,
        bool commit,
        std::optional<bool> automatic_override = std::nullopt,
        std::optional<bool> precise_override = std::nullopt) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        if (selectedParameter() == Parameter::Pitch) {
            addPitchCommand(timeline_count, parameter_value, commit);
            return;
        }
        if (selectedParameter() == Parameter::Timbre) {
            return;
        }
        if (selectedParameter() == Parameter::RegisterWrite) {
            return;
        }
        const auto [minimum, maximum] = valueRange();
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        parameter_value = juce::jlimit(minimum, maximum, parameter_value);
        auto& events = selectedEvents(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto kind = selectedEventKind();
        bool automatic = false;
        bool precise = false;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count
                    == static_cast<std::uint32_t>(timeline_count)) {
                automatic = event.automatic;
                precise = event.precise;
                break;
            }
        }
        if (automatic_override.has_value()
            && kind == mgstc::engine::EnvelopeEventKind::Volume) {
            automatic = *automatic_override;
        }
        if (precise_override.has_value()
            && kind == mgstc::engine::EnvelopeEventKind::Volume) {
            precise = *precise_override;
        }
        if (!automatic) {
            precise = false;
        }
        std::erase_if(events, [kind, timeline_count](const auto& event) {
            return event.kind == kind
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        events.push_back({
            .kind = kind,
            .value = parameter_value,
            .count = static_cast<std::uint32_t>(timeline_count),
            .automatic = automatic,
            .precise = precise,
        });
        std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            return left.count < right.count;
        });
        notifyLayerEdit(timeline_count, parameter_value, commit);
    }

    void syncPointEditors() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        if (selectedParameter() == Parameter::Pitch) {
            value_.setText(
                juce::String(lastPitchCommandAt(layer, selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "次に追加する\\のオペランド（-127～127）。"
                "グラフのYは累積オフセットです"));
        } else if (selectedParameter() == Parameter::Timbre) {
            value_.setText(
                timbreInspectLabel(layer, selected_count_),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "このカウントの@切替先。ライブラリから選びます"));
        } else if (selectedParameter() == Parameter::RegisterWrite) {
            value_.setText(
                registerInspectLabel(layer, selected_count_),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "手動y。OPLLはオリジナル音色全フィールド（regs 0～7）を"
                "ウィンドウで編集し、変更レジスタだけ y を出します。"
                "同ステップの y 群はまとめて編集。ROM区間は不可"));
        } else {
            value_.setText(
                juce::String(
                    storedValueAt(layer, selectedParameter(), selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8("選択パラメーターの値"));
        }
        // Drawing path restores the popup after edit; do not clear mid-drag.
        if (!drawing_) {
            hover_preview_value_.reset();
        }
    }

    void setTimelineMarker(int timeline_count, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto count = static_cast<std::uint32_t>(
            juce::jlimit(0, maximumCount(), timeline_count));
        switch (selectedEditMode()) {
        case EditMode::Draw:
            return;
        case EditMode::End: {
            timeline.length_counts = juce::jmax<std::uint32_t>(1, count);
            const auto fit = static_cast<std::uint32_t>(
                maximumCountForSelectedLayer());
            if (timeline.length_counts > fit) {
                timeline.length_counts = fit;
                reportStatus(
                    juce::String::fromUTF8(
                        "@e はコンパイル後256バイトまでです。END を ")
                    + juce::String(static_cast<int>(fit))
                    + juce::String::fromUTF8(" に制限しました"));
            }
            if (timeline.loop_start_count
                && *timeline.loop_start_count > timeline.length_counts) {
                timeline.loop_start_count = timeline.length_counts;
            }
            if (timeline.loop_end_count
                && *timeline.loop_end_count > timeline.length_counts) {
                timeline.loop_end_count = timeline.length_counts;
            }
            break;
        }
        case EditMode::LoopStart:
            timeline.loop_start_count = juce::jmin(count, timeline.length_counts);
            if (timeline.loop_end_count
                && *timeline.loop_end_count < *timeline.loop_start_count) {
                timeline.loop_end_count = timeline.loop_start_count;
            }
            break;
        case EditMode::LoopEnd:
            timeline.loop_end_count = juce::jmin(count, timeline.length_counts);
            if (timeline.loop_start_count
                && *timeline.loop_start_count > *timeline.loop_end_count) {
                timeline.loop_start_count = timeline.loop_end_count;
            }
            break;
        }
        refreshCountCeilingCache();
        maybeExpandScrollExtent(static_cast<int>(count));
        syncInspector();
        updateScrollRanges();
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit, commit);
        }
    }

    [[nodiscard]] int timelineChromeHeight() const noexcept {
        using namespace UiLayout;
        // Add/help/tempo row only; envelope edit tools live in the edit-tier dock.
        return textButtonH + sm;
    }

    [[nodiscard]] juce::Rectangle<int> graphArea() const {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        area.removeFromTop(timelineChromeHeight());
        area.removeFromRight(kScrollBarSize);
        area.removeFromBottom(kScrollBarSize);
        return area;
    }

    [[nodiscard]] juce::Range<double> visibleCountRange() const {
        return horizontal_scroll_.getCurrentRange();
    }

    [[nodiscard]] int countAtX(
        juce::Rectangle<int> bounds, int x) const {
        const auto visible = visibleCountRange();
        return juce::jlimit(
            0, maximumCount(), static_cast<int>(std::floor(
                visible.getStart()
                + static_cast<double>(x - bounds.getX())
                    * visible.getLength()
                    / juce::jmax(1, bounds.getWidth()))));
    }

    [[nodiscard]] int xForCount(
        juce::Rectangle<int> bounds, int count) const {
        const auto visible = visibleCountRange();
        return bounds.getX() + juce::roundToInt(
            (static_cast<double>(count) - visible.getStart())
            * bounds.getWidth() / juce::jmax(1.0, visible.getLength()));
    }

    void updateScrollRanges() {
        const auto graph = graphArea();
        const double total = juce::jmax(
            1.0, static_cast<double>(totalLaneStackHeight()));
        const double view = juce::jmax(
            1.0, static_cast<double>(graph.getHeight()));
        const double visible = juce::jmin(view, total);
        const double max_start = juce::jmax(0.0, total - visible);
        const double start = juce::jlimit(
            0.0, max_start, vertical_scroll_.getCurrentRangeStart());
        vertical_scroll_.setRangeLimits(
            0.0, total, juce::dontSendNotification);
        vertical_scroll_.setCurrentRange(
            start, visible, juce::dontSendNotification);
        scroll_extent_counts_ = juce::jlimit(
            1, maximumCount(), scroll_extent_counts_);
        horizontal_scroll_.setRangeLimits(
            0.0,
            static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
        applyCountColumnWidth();
    }

    [[nodiscard]] int timelineGraphWidth() const {
        return juce::jmax(
            1,
            graphArea().getWidth()
                - UiLayout::compositeLaneLabelW
                - UiLayout::panelGap
                - UiLayout::editSubLaneLabelW);
    }

    [[nodiscard]] double countColumnWidthPx() const {
        return static_cast<double>(timelineGraphWidth())
            / juce::jmax(1.0, visibleCountRange().getLength());
    }

    void applyCountColumnWidth() {
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double current = horizontal_scroll_.getCurrentRangeSize();
        const double visible = juce::jlimit(min_visible, max_visible, current);
        horizontal_scroll_.setCurrentRange(
            horizontal_scroll_.getCurrentRangeStart(),
            visible,
            juce::dontSendNotification);
    }

    void zoomCountColumns(int anchor_x, double length_factor) {
        const auto graph = graphArea();
        auto bounds = graph;
        bounds.removeFromLeft(UiLayout::compositeLaneLabelW);
        bounds.reduce(UiLayout::sm, 0);
        bounds.removeFromLeft(UiLayout::editSubLaneLabelW);
        const int count = countAtX(bounds, anchor_x);
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double visible = juce::jlimit(
            min_visible,
            max_visible,
            visibleCountRange().getLength() * length_factor);
        const double start = juce::jmax(
            0.0,
            static_cast<double>(count)
                - visible * juce::jlimit(
                    0.0, 1.0,
                    static_cast<double>(anchor_x - bounds.getX())
                        / juce::jmax(1.0, static_cast<double>(bounds.getWidth()))));
        maybeExpandScrollExtent(
            static_cast<int>(std::ceil(start + visible)));
        horizontal_scroll_.setCurrentRange(
            start, visible, juce::dontSendNotification);
        repaint();
    }

    void drawCountGrid(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        const double column_w = countColumnWidthPx();
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x = xForCount(bounds, timeline_count);
            const bool major = timeline_count % 10 == 0;
            graphics.setColour(
                major
                    ? juce::Colour(0xFF52616E)
                    : juce::Colour(0xFF35404A).withAlpha(
                        column_w >= static_cast<double>(UiLayout::countColumnMinW)
                            ? 0.95F : 0.45F));
            graphics.drawVerticalLine(
                x, static_cast<float>(bounds.getY()),
                static_cast<float>(bounds.getBottom()));
        }
    }

    void scrollBarMoved(juce::ScrollBar* scroll_bar, double) override {
        if (scroll_bar == &horizontal_scroll_) {
            maybeExpandScrollExtent(static_cast<int>(std::ceil(
                horizontal_scroll_.getCurrentRange().getEnd())));
        }
        repaint();
    }

    void drawTimelineMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour) const {
        const auto& timeline = selectedTimeline(timbre_.layers[layer_index]);
        const auto draw_marker = [&](std::uint32_t count,
                                     juce::Colour marker_colour,
                                     const juce::String& label) {
            const auto visible = visibleCountRange();
            if (count < visible.getStart() || count > visible.getEnd()) {
                return;
            }
            const int x = xForCount(bounds, static_cast<int>(count));
            graphics.setColour(marker_colour);
            graphics.drawVerticalLine(
                x, static_cast<float>(bounds.getY()),
                static_cast<float>(bounds.getBottom()));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawText(
                label, x + 2, bounds.getY(), 34, 16,
                juce::Justification::centredLeft, false);
        };
        draw_marker(timeline.length_counts,
                    juce::Colour(0xFFFFD166), "END");
        if (timeline.loop_start_count) {
            draw_marker(*timeline.loop_start_count,
                        colour.brighter(0.35F), "L>");
        }
        if (timeline.loop_end_count) {
            draw_marker(*timeline.loop_end_count,
                        colour.brighter(0.35F), "<L");
        }
    }

    void drawAutomation(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour,
        std::optional<Parameter> forced_parameter = std::nullopt) const {
        const auto parameter = forced_parameter.value_or(selectedParameter());
        const auto& layer = timbre_.layers[layer_index];
        if (parameter == Parameter::Pitch) {
            drawPitchLane(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::Timbre) {
            drawTimbreMarkers(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::RegisterWrite) {
            return;
        }
        if (parameter == Parameter::Volume) {
            drawVolumeBars(graphics, bounds, layer, colour);
            return;
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        const auto [minimum, maximum] = valueRangeFor(parameter);
        std::vector<std::pair<int, int>> points;
        for (const auto& event : events) {
            if (event.kind == kind) {
                points.emplace_back(
                    static_cast<int>(event.count),
                    juce::jlimit(minimum, maximum, event.value));
            }
        }
        if (points.empty()) {
            points.emplace_back(0, 0);
        }
        std::stable_sort(points.begin(), points.end());
        drawSteppedValuePath(graphics, bounds, points, minimum, maximum, colour);
    }

    void drawSteppedValuePath(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const std::vector<std::pair<int, int>>& points,
        int minimum,
        int maximum,
        juce::Colour colour) const {
        if (points.empty() || bounds.isEmpty()) {
            return;
        }
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                static_cast<float>(minimum),
                static_cast<float>(maximum),
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY()));
        };
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().first));
        float y = map_y(points.front().second);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].first));
            const float next_y = map_y(points[index].second);
            // Hold horizontally until the next step, then jump vertically.
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
        for (const auto& [timeline_count, parameter_value] : points) {
            const float px = static_cast<float>(
                xForCount(bounds, timeline_count));
            const float py = map_y(parameter_value);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
    }

    void drawVolumeBars(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        struct Point {
            int count{};
            int volume{};
            bool automatic{};
        };
        std::vector<Point> points;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
                points.push_back({
                    static_cast<int>(event.count),
                    juce::jlimit(0, 15, event.value),
                    event.automatic});
            }
        }
        std::stable_sort(
            points.begin(),
            points.end(),
            [](const Point& left, const Point& right) {
                return left.count < right.count;
            });
        if (points.empty() || points.front().count > 0) {
            points.insert(
                points.begin(),
                {
                    0,
                    juce::jlimit(0, 15, static_cast<int>(layer.volume)),
                    false});
        }
        const int end_count = static_cast<int>(
            layer.envelope_timeline.length_counts);
        const auto sampled = sampleCompositeVolumeLane(layer, end_count);
        const auto draw_output_span = [&](int start, int stop) {
            const int last = juce::jmin(stop, end_count);
            for (int count = start; count < last; ++count) {
                const int x0 = xForCount(bounds, count);
                const int x1 = xForCount(bounds, count + 1);
                const auto actual_top = chipTransferYFromValue(
                    layer.source,
                    bounds,
                    sampled[static_cast<std::size_t>(count)],
                    layer.volume);
                graphics.setColour(colour.withAlpha(0.32F));
                graphics.fillRect(
                    x0,
                    juce::roundToInt(actual_top),
                    juce::jmax(1, x1 - x0),
                    juce::jmax(
                        1,
                        bounds.getBottom() - juce::roundToInt(actual_top)));
            }
        };
        for (std::size_t index = 0; index < points.size(); ++index) {
            const int start = points[index].count;
            const int stop = index + 1 < points.size()
                ? points[index + 1].count
                : juce::jmax(start + 1, end_count);
            const int x0 = xForCount(bounds, start);
            const int x1 = xForCount(bounds, juce::jmax(start + 1, stop));
            const int width = juce::jmax(2, x1 - x0);
            const bool ramp_to_next = index + 1 < points.size()
                && points[index + 1].automatic
                && points[index + 1].count > start;
            if (ramp_to_next) {
                const float y0 = volumeYFromValue(
                    layer.source, bounds, points[index].volume);
                const float y1 = volumeYFromValue(
                    layer.source, bounds, points[index + 1].volume);
                juce::Path bar;
                bar.startNewSubPath(
                    static_cast<float>(x0),
                    static_cast<float>(bounds.getBottom()));
                bar.lineTo(static_cast<float>(x0), y0);
                bar.lineTo(static_cast<float>(x1), y1);
                bar.lineTo(
                    static_cast<float>(x1),
                    static_cast<float>(bounds.getBottom()));
                bar.closeSubPath();
                graphics.setColour(colour.withAlpha(0.55F));
                graphics.fillPath(bar);
                graphics.setColour(juce::Colour(kUiHoverAccent));
                graphics.drawLine(
                    static_cast<float>(x0),
                    y0,
                    static_cast<float>(x1),
                    y1,
                    2.0F);
            } else {
                const float top = volumeYFromValue(
                    layer.source, bounds, points[index].volume);
                graphics.setColour(colour.withAlpha(0.55F));
                graphics.fillRect(
                    x0,
                    juce::roundToInt(top),
                    width,
                    juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
                graphics.setColour(colour);
                graphics.drawRect(
                    x0,
                    juce::roundToInt(top),
                    width,
                    juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
            }
            draw_output_span(start, juce::jmax(start + 1, stop));
        }
    }

    void drawScope(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t channel,
        juce::Colour colour) const {
        const float centre_y = static_cast<float>(bounds.getCentreY());
        graphics.setColour(juce::Colour(0xFF52616E).withAlpha(0.65F));
        graphics.drawHorizontalLine(
            bounds.getCentreY(),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        if (scope_size_ < 2 || bounds.getWidth() < 2) {
            return;
        }
        constexpr double sample_rate = 48000.0;
        const double frequency = 440.0 * std::pow(
            2.0,
            (static_cast<double>(audition_note_) - 69.0) / 12.0);
        const int sample_count = juce::jlimit(
            2,
            scope_size_,
            juce::roundToInt(2.0 * sample_rate / frequency));
        const int oldest =
            (scope_write_position_ + kScopeHistorySize - scope_size_)
            % kScopeHistorySize;
        const int logical_start = scope_size_ - sample_count;
        const std::size_t scope_channel = channel >= timbre_.layers.size()
            ? 3
            : static_cast<std::size_t>(timbre_.layers[channel].source);
        const auto sample_at = [&](int logical_index) {
            return scope_history_[scope_channel][static_cast<std::size_t>(
                (oldest + logical_start + logical_index)
                % kScopeHistorySize)];
        };
        float peak = 0.0001F;
        for (int index = 0; index < sample_count; ++index) {
            peak = juce::jmax(peak, std::abs(sample_at(index)));
        }
        const float scale =
            static_cast<float>(bounds.getHeight()) * 0.43F / peak;
        juce::Path path;
        for (int x = 0; x < bounds.getWidth(); ++x) {
            const int sample_index = juce::jlimit(
                0,
                sample_count - 1,
                juce::roundToInt(
                    static_cast<double>(x)
                    * static_cast<double>(sample_count - 1)
                    / static_cast<double>(bounds.getWidth() - 1)));
            const float point_x =
                static_cast<float>(bounds.getX() + x);
            const float point_y = centre_y - sample_at(sample_index) * scale;
            if (x == 0) {
                path.startNewSubPath(point_x, point_y);
            } else {
                path.lineTo(point_x, point_y);
            }
        }
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(1.4F));
    }

    struct EditLaneSlots {
        juce::Rectangle<int> summary;
        juce::Rectangle<int> volume;
        juce::Rectangle<int> pitch;
        juce::Rectangle<int> timbre;
        juce::Rectangle<int> register_write;
        juce::Rectangle<int> tl_auto;
        juce::Rectangle<int> fb_auto;
        juce::Rectangle<int> preview;
    };

    [[nodiscard]] bool showsRegisterLane(
        mgstc::engine::TimbreSource source) const noexcept {
        return source != mgstc::engine::TimbreSource::Scc;
    }

    [[nodiscard]] bool showsRegisterAutoLanes(
        mgstc::engine::TimbreSource source) const noexcept {
        return source == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] EditLaneSlots makeEditLaneSlots(
        juce::Rectangle<int> content,
        mgstc::engine::TimbreSource source) const {
        EditLaneSlots slots;
        slots.preview = content.removeFromBottom(UiLayout::envelopePreviewH);
        slots.summary = content.removeFromTop(UiLayout::commandSummaryH);
        if (showsRegisterAutoLanes(source)) {
            slots.fb_auto = content.removeFromBottom(UiLayout::registerAutoLaneH);
            slots.tl_auto = content.removeFromBottom(UiLayout::registerAutoLaneH);
        }
        const bool has_register = showsRegisterLane(source);
        const int marker_h = UiLayout::editMarkerLaneH;
        auto value_area = content;
        if (has_register) {
            auto markers = value_area.removeFromBottom(marker_h * 2);
            slots.timbre = markers.removeFromTop(marker_h);
            slots.register_write = markers;
        } else {
            slots.timbre = value_area.removeFromBottom(marker_h);
            slots.register_write = {};
        }
        // Shared height (UiLayout::editVolumeLaneH). Quiet chip steps that
        // would be sub-pixel are rounded so each 0–15 unit still has ≥1px.
        const int volume_h = juce::jmin(
            value_area.getHeight(),
            juce::jmax(UiLayout::editMarkerLaneH, UiLayout::editVolumeLaneH));
        slots.volume = value_area.removeFromTop(volume_h);
        slots.pitch = value_area;
        return slots;
    }

    [[nodiscard]] juce::Rectangle<int> timelinePlotBounds(
        juce::Rectangle<int> lane_content) const {
        if (lane_content.isEmpty()) {
            return {};
        }
        lane_content.removeFromLeft(UiLayout::editSubLaneLabelW);
        return lane_content;
    }

    [[nodiscard]] juce::Rectangle<int> graphStrip(juce::Rectangle<int> slot) const {
        if (slot.isEmpty()) {
            return {};
        }
        slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        return slot;
    }

    bool hitEditLane(
        const mgstc::engine::CompositeLayer& layer,
        juce::Rectangle<int> content,
        juce::Point<int> point) {
        edit_draw_bounds_.reset();
        const auto slots = makeEditLaneSlots(content, layer.source);
        if (slots.summary.contains(point) || slots.preview.contains(point)) {
            return true;
        }
        const struct LaneHit {
            Parameter parameter;
            juce::Rectangle<int> slot;
            bool drawable;
        } hits[] = {
            {Parameter::Volume, slots.volume, true},
            {Parameter::Pitch, slots.pitch, true},
            {Parameter::Timbre, slots.timbre, false},
            {Parameter::RegisterWrite, slots.register_write, false},
            {Parameter::OpllTlAuto, slots.tl_auto, true},
            {Parameter::OpllFbAuto, slots.fb_auto, true},
        };
        for (const auto& hit : hits) {
            if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                continue;
            }
            parameter_.setSelectedId(
                static_cast<int>(hit.parameter),
                juce::dontSendNotification);
            syncRegisterAutoEditors();
            if (hit.drawable) {
                const bool free_curve = isRegisterAutoParameter(hit.parameter)
                    && selectedRegisterAutoLane() != nullptr
                    && selectedRegisterAutoLane()->mode
                        == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
                const bool rise_fall_params =
                    isRegisterAutoParameter(hit.parameter);
                // Free-curve is mouse-traced; rise/fall/LFO are param-driven
                // lines (still selectable). Only free-curve starts a draw.
                if (!isRegisterAutoParameter(hit.parameter) || free_curve) {
                    edit_draw_bounds_ = graphStrip(hit.slot);
                } else {
                    edit_draw_bounds_.reset();
                    juce::ignoreUnused(rise_fall_params);
                }
            }
            return true;
        }
        return true;
    }

    void ensureSelectedLaneVisible() {
        if (selected_layer_ < 0) {
            return;
        }
        const int y0 = lanePixelOffset(selected_layer_);
        const int y1 = y0 + laneHeightForIndex(selected_layer_);
        const int view0 = juce::roundToInt(
            vertical_scroll_.getCurrentRangeStart());
        const int view_h = juce::jmax(
            1, juce::roundToInt(vertical_scroll_.getCurrentRangeSize()));
        const int view1 = view0 + view_h;
        if (y1 > view0 && y0 < view1) {
            return;
        }
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(y0), juce::dontSendNotification);
    }

    void refreshCommandStack(const mgstc::engine::CompositeLayer& layer) {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        std::vector<juce::String> before;
        std::vector<juce::String> after;
        std::vector<juce::String> after_end;
        const bool loop_start = layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
        const bool loop_end = layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count == count;
        if (!loop_start) {
            loop_insert_after_ = false;
        }
        const auto push_label =
            [&](const juce::String& label, bool after_loop) {
                if (loop_start && after_loop) {
                    after.push_back(label);
                } else {
                    before.push_back(label);
                }
            };
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == count) {
                push_label(
                    pitchCommandLabel(event.value), event.after_loop_start);
            }
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.count != count) {
                continue;
            }
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre) {
                push_label(
                    timbreCommandLabel(event), event.after_loop_start);
            } else if (event.kind
                       == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                push_label(
                    "y" + juce::String(event.value) + ","
                        + juce::String(event.secondary),
                    event.after_loop_start);
            }
        }
        command_stack_.setStack(
            selected_count_,
            loop_start,
            loop_end,
            loop_insert_after_,
            std::move(before),
            std::move(after),
            std::move(after_end));
    }

    [[nodiscard]] bool newCommandAfterLoopStart(
        std::uint32_t count) const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return false;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        return loop_insert_after_
            && layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
    }

    void drawSelectedCountBand(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        if (selected_count_ < visible.getStart()
            || selected_count_ > visible.getEnd()) {
            return;
        }
        const int x0 = xForCount(bounds, selected_count_);
        const int x1 = xForCount(bounds, selected_count_ + 1);
        const int width = juce::jmax(3, x1 - x0);
        graphics.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.12F));
        graphics.fillRect(x0, bounds.getY(), width, bounds.getHeight());
    }

    void paintEditTier(
        juce::Graphics& graphics,
        juce::Rectangle<int> content,
        std::size_t layer_index,
        juce::Colour colour) {
        const auto& layer = timbre_.layers[layer_index];
        const auto slots = makeEditLaneSlots(content, layer.source);
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            const auto rate = mgstc::engine::clampRateEnvelope(
                layer.volume_envelope.rate);
            auto summary = slots.summary;
            auto label = summary.removeFromLeft(UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("@r"),
                label,
                juce::Justification::centredLeft,
                1);
            graphics.drawFittedText(
                juce::String::formatted(
                    "AL %d  AR %d  DR %d  SL %d  SR %d  RR %d",
                    static_cast<int>(rate.attack_level),
                    static_cast<int>(rate.attack_rate),
                    static_cast<int>(rate.decay_rate),
                    static_cast<int>(rate.sustain_level),
                    static_cast<int>(rate.sustain_rate),
                    static_cast<int>(rate.release_rate)),
                summary,
                juce::Justification::centredLeft,
                1);
            auto graph_slot = slots.volume;
            graph_slot = graph_slot.getUnion(slots.pitch);
            if (!slots.timbre.isEmpty()) {
                graph_slot = graph_slot.getUnion(slots.timbre);
            }
            if (!slots.register_write.isEmpty()) {
                graph_slot = graph_slot.getUnion(slots.register_write);
            }
            auto graph_label = graph_slot.removeFromLeft(
                UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.drawFittedText(
                juce::String::fromUTF8("曲線"),
                graph_label,
                juce::Justification::centredLeft,
                1);
            const auto layout = rateEnvelopeHandlesForLayer(layer);
            paintRateEnvelopeGraph(
                graphics,
                graph_slot,
                mgstc::app::makeLayerRateTrace(layer),
                colour,
                layer.source,
                true,
                &layout);
            {
                auto preview = slots.preview;
                auto preview_label = preview.removeFromLeft(
                    UiLayout::editSubLaneLabelW);
                graphics.setColour(juce::Colour(0xFFC9D1D9));
                graphics.setFont(UiFonts::dense(true));
                graphics.drawFittedText(
                    "MML", preview_label, juce::Justification::centredLeft, 1);
            }
            return;
        }
        drawScope(
            graphics,
            timelinePlotBounds(content),
            layer_index,
            colour.withAlpha(0.32F));
        {
            auto summary = slots.summary;
            auto label = summary.removeFromLeft(UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("命令"),
                label,
                juce::Justification::centredLeft,
                1);
            drawCommandSummary(graphics, summary, layer, colour);
        }
        paintEditSubLane(
            graphics, slots.volume, layer_index, colour,
            Parameter::Volume, juce::String::fromUTF8("音量"));
        paintEditSubLane(
            graphics, slots.pitch, layer_index, colour,
            Parameter::Pitch, juce::String::fromUTF8("音程"));
        paintEditSubLane(
            graphics, slots.timbre, layer_index, colour,
            Parameter::Timbre, juce::String::fromUTF8("@"));
        if (!slots.register_write.isEmpty()) {
            paintRegisterLane(graphics, slots.register_write, layer, colour);
        }
        if (!slots.tl_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.tl_auto, layer, colour,
                Parameter::OpllTlAuto, juce::String::fromUTF8("TL自動"));
        }
        if (!slots.fb_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.fb_auto, layer, colour,
                Parameter::OpllFbAuto, juce::String::fromUTF8("FB自動"));
        }
        drawTimelineMarkers(graphics, graphStrip(slots.volume), layer_index, colour);
        {
            auto preview = slots.preview;
            auto label = preview.removeFromLeft(UiLayout::editSubLaneLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                "MML", label, juce::Justification::centredLeft, 1);
        }
    }

    void updateEnvelopeMmlPreview() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            if (last_envelope_mml_preview_.isNotEmpty()) {
                last_envelope_mml_preview_.clear();
                envelope_mml_preview_.clear();
            }
            envelope_mml_preview_.setVisible(false);
            return;
        }
        // #region agent log
        const auto preview_t0 = juce::Time::getMillisecondCounterHiRes();
        // #endregion
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        juce::String definition;
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            definition = juce::String::fromUTF8(
                mgstc::engine::formatMgsRateDefinition(
                    layer, layer.envelope_number)
                    .c_str())
                .trimEnd();
        } else {
            const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
                layer,
                layer.envelope_number,
                mgstc::engine::kMgscEnvelopeCompiledByteLimit,
                &numbers,
                timbreLibrary(),
                timbre_.name);
            definition = juce::String::fromUTF8(formatted.definition.c_str())
                              .trimEnd();
            if (definition.isEmpty()) {
                definition = "@e"
                    + juce::String(static_cast<int>(layer.envelope_number))
                    + juce::String::fromUTF8(" = { }");
            }
        }
        const auto track = juce::String::fromUTF8(
            mgstc::engine::formatMgsCompositeTrackSetup(layer, &numbers)
                .c_str());
        const auto preview_text = definition + "\n" + track;
        if (preview_text == last_envelope_mml_preview_) {
            return;
        }
        last_envelope_mml_preview_ = preview_text;
        // #region agent log
        const auto preview_t1 = juce::Time::getMillisecondCounterHiRes();
        // #endregion
        UiFonts::setMgscPreviewText(
            envelope_mml_preview_, preview_text);
        // #region agent log
        const auto preview_t2 = juce::Time::getMillisecondCounterHiRes();
        {

            juce::String vols;
            for (const auto& event : layer.volume_envelope.events) {
                if (event.kind != mgstc::engine::EnvelopeEventKind::Volume) {
                    continue;
                }
                if (vols.isNotEmpty()) {
                    vols += ",";
                }
                vols += juce::String(static_cast<int>(event.count)) + ":"
                    + juce::String(event.value) + (event.automatic ? "a" : "h");
            }
            auto def = definition.replace("\\", "/").replace("\"", "'");
            const auto payload =
                juce::String("{\"sessionId\":\"6045ae\",\"runId\":\"post-fix-gui\",")
                + "\"hypothesisId\":\"H3\",\"location\":\"main.cpp:preview\","
                + "\"message\":\"envelope mml preview\","
                + "\"data\":{\"def\":\"" + def
                + "\",\"vols\":\"" + vols + "\"},\"timestamp\":"
                + juce::String(juce::Time::currentTimeMillis()) + "}\n";
            juce::File(juce::CharPointer_UTF8(
                "debug-6045ae.log"))
                .appendText(payload, false, false);
        }
        const auto preview_t3 = juce::Time::getMillisecondCounterHiRes();
        dbg7ae407(
            "H2",
            "main.cpp:updateEnvelopeMmlPreview",
            "preview cost",
            std::string("{\"format_ms\":")
                + std::to_string(preview_t1 - preview_t0)
                + ",\"setText_ms\":"
                + std::to_string(preview_t2 - preview_t1)
                + ",\"dbgfile_ms\":"
                + std::to_string(preview_t3 - preview_t2)
                + ",\"total_ms\":"
                + std::to_string(preview_t3 - preview_t0) + "}");
        // #endregion
    }

    [[nodiscard]] bool isEditSubLaneHovered(Parameter parameter) const noexcept {
        return hover_edit_parameter_.has_value()
            && *hover_edit_parameter_ == parameter;
    }

    void paintEditSubLaneChrome(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        Parameter parameter,
        const juce::String& title) const {
        const bool selected = parameter == selectedParameter();
        const bool hovered = isEditSubLaneHovered(parameter);
        if (hovered) {
            graphics.setColour(uiHoverAccentStrong().withAlpha(0.28F));
            graphics.fillRect(slot);
        } else if (selected) {
            graphics.setColour(uiHoverAccent().withAlpha(0.12F));
            graphics.fillRect(slot);
        }
        auto label = slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        graphics.setColour(
            hovered
                ? uiHoverAccentStrong()
                : selected ? uiHoverAccent() : juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::dense(true));
        graphics.drawFittedText(
            title, label, juce::Justification::centredLeft, 1);
        graphics.setColour(juce::Colour(0xFF40505E));
        graphics.drawHorizontalLine(
            slot.getBottom() - 1,
            static_cast<float>(slot.getX()),
            static_cast<float>(slot.getRight()));
    }

    void paintRegisterAutoLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        paintEditSubLaneChrome(graphics, slot, parameter, title);
        slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        const auto& auto_lane = parameter == Parameter::OpllTlAuto
            ? layer.opll_tl_auto
            : layer.opll_fb_auto;
        const auto target = parameter == Parameter::OpllTlAuto
            ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
            : mgstc::engine::OpllRegisterAutoTarget::Feedback;
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(
            0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        // Dim ROM intervals — TL/FB auto cannot change ROM patches.
        {
            bool in_rom = false;
            int rom_start = begin;
            for (int timeline_count = begin; timeline_count <= end + 1;
                 ++timeline_count) {
                const bool rom_now = timeline_count <= end
                    && !mgstc::engine::opllRegisterAutoAvailableAt(
                        layer,
                        static_cast<std::uint32_t>(timeline_count));
                if (rom_now && !in_rom) {
                    in_rom = true;
                    rom_start = timeline_count;
                } else if (!rom_now && in_rom) {
                    in_rom = false;
                    const int x0 = xForCount(slot, rom_start);
                    const int x1 = xForCount(slot, timeline_count);
                    graphics.setColour(juce::Colour(0x33202830));
                    graphics.fillRect(
                        x0, slot.getY(), juce::jmax(1, x1 - x0),
                        slot.getHeight());
                }
            }
        }
        if (auto_lane.mode == mgstc::engine::OpllRegisterAutoMode::Off) {
            graphics.setColour(juce::Colour(0xFF6B7785));
            graphics.setFont(UiFonts::dense());
            graphics.drawText(
                juce::String::fromUTF8("オフ"),
                slot.reduced(UiLayout::xs, 0),
                juce::Justification::centredLeft,
                false);
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        const auto* library = timbreLibrary();
        const auto packed_events =
            mgstc::engine::expandOpllLayerRegisterAutos(layer, library);
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                0.0F,
                static_cast<float>(maximum),
                static_cast<float>(slot.getBottom() - 1),
                static_cast<float>(slot.getY() + 1));
        };
        // Polyline = effective TL/FB at each count (base/@/manual y + auto).
        juce::Path path;
        bool started = false;
        std::optional<float> last_x;
        std::optional<float> last_y;
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const auto count = static_cast<std::uint32_t>(timeline_count);
            if (!mgstc::engine::opllRegisterAutoAvailableAt(layer, count)) {
                started = false;
                last_x.reset();
                last_y.reset();
                continue;
            }
            const auto regs = mgstc::engine::opllOriginalRegisterImageAt(
                layer, count, library, true, true);
            if (!regs) {
                started = false;
                last_x.reset();
                last_y.reset();
                continue;
            }
            const int field = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? static_cast<int>((*regs)[2] & 0x3F)
                : static_cast<int>((*regs)[3] & 0x07);
            const float x = static_cast<float>(xForCount(slot, timeline_count));
            const float y = map_y(field);
            if (!started) {
                path.startNewSubPath(x, y);
                started = true;
            } else if (last_x && last_y) {
                path.lineTo(x, *last_y);
                path.lineTo(x, y);
            }
            last_x = x;
            last_y = y;
        }
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(1.6F));
        graphics.setFont(UiFonts::dense());
        const auto reg_number =
            mgstc::engine::opllRegisterAutoRegisterNumber(target);
        const auto visible_counts = visibleCountRange();
        for (const auto& event : packed_events) {
            if (event.value != reg_number
                || event.count < visible_counts.getStart()
                || event.count > visible_counts.getEnd()) {
                continue;
            }
            const int value = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? (event.secondary & 0x3F)
                : (event.secondary & 0x07);
            const int x = xForCount(slot, static_cast<int>(event.count));
            graphics.fillEllipse(
                static_cast<float>(x - 2),
                map_y(value) - 2.0F,
                4.0F,
                4.0F);
        }
    }

    void paintEditSubLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        std::size_t layer_index,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        paintEditSubLaneChrome(graphics, slot, parameter, title);
        slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        drawAutomation(graphics, slot, layer_index, colour, parameter);
    }

    [[nodiscard]] static juce::String formatRegisterWriteLabel(
        const mgstc::engine::EnvelopeEvent& event) {
        return "y" + juce::String(event.value) + ","
            + juce::String(event.secondary);
    }

    [[nodiscard]] static juce::String formatRegisterGroupLabel(
        const std::vector<const mgstc::engine::EnvelopeEvent*>& events) {
        if (events.empty()) {
            return {};
        }
        juce::StringArray parts;
        parts.ensureStorageAllocated(static_cast<int>(events.size()));
        for (const auto* event : events) {
            if (event != nullptr) {
                parts.add(formatRegisterWriteLabel(*event));
            }
        }
        // Side-by-side tokens (no y×N collapse) so each yreg,data stays readable.
        return parts.joinIntoString(" ");
    }

    [[nodiscard]] static juce::String formatRegisterGroupTooltip(
        const std::vector<const mgstc::engine::EnvelopeEvent*>& events) {
        juce::StringArray lines;
        for (const auto* event : events) {
            if (event != nullptr) {
                lines.add(formatRegisterWriteLabel(*event));
            }
        }
        return lines.joinIntoString("\n");
    }

    [[nodiscard]] static std::map<
        std::uint32_t,
        std::vector<const mgstc::engine::EnvelopeEvent*>>
    registerWritesByCount(const mgstc::engine::CompositeLayer& layer) {
        std::map<
            std::uint32_t,
            std::vector<const mgstc::engine::EnvelopeEvent*>>
            grouped;
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                grouped[event.count].push_back(&event);
            }
        }
        return grouped;
    }

    void paintRegisterLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        paintEditSubLaneChrome(
            graphics, slot, Parameter::RegisterWrite, "y");
        slot.removeFromLeft(UiLayout::editSubLaneLabelW);
        graphics.setFont(UiFonts::dense(true));
        const auto visible = visibleCountRange();
        for (const auto& [count, events] : registerWritesByCount(layer)) {
            if (count < visible.getStart() || count > visible.getEnd()
                || events.empty()) {
                continue;
            }
            const int x = xForCount(slot, static_cast<int>(count));
            graphics.setColour(colour);
            graphics.fillEllipse(
                static_cast<float>(x - 3),
                static_cast<float>(slot.getCentreY() - 3),
                6.0F,
                6.0F);
            const auto text = formatRegisterGroupLabel(events);
            const int text_w = juce::jmax(
                28,
                juce::jmin(
                    juce::jmax(
                        48,
                        juce::GlyphArrangement::getStringWidthInt(
                            UiFonts::dense(true), text)
                            + 4),
                    slot.getRight() - x - 4));
            graphics.drawText(
                text,
                x + 4,
                slot.getY(),
                text_w,
                slot.getHeight(),
                juce::Justification::centredLeft,
                true);
        }
    }

    void drawCommandSummary(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        graphics.setColour(juce::Colour(0xFF1B222A));
        graphics.fillRoundedRectangle(bounds.toFloat(), 3.0F);
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        graphics.setFont(UiFonts::dense());
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x0 = xForCount(bounds, timeline_count);
            const int x1 = xForCount(bounds, timeline_count + 1);
            const int width = juce::jmax(1, x1 - x0);
            graphics.setColour(
                timeline_count == selected_count_
                    ? juce::Colour(kUiHoverAccent)
                    : juce::Colour(0xFF6B7785));
            graphics.drawText(
                juce::String(timeline_count),
                x0,
                bounds.getBottom() - 13,
                width,
                12,
                juce::Justification::centred,
                false);
        }
        graphics.setFont(UiFonts::dense(true));
        struct SummaryItem {
            std::uint32_t count{};
            juce::String text;
            bool dimmed{};
        };
        std::vector<SummaryItem> items;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch) {
                items.push_back({event.count, pitchCommandLabel(event.value), false});
            }
        }
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre) {
                items.push_back(
                    {event.count, timbreCommandLabel(event), false});
            }
        }
        // Same-count y writes are listed side-by-side (y2,a y3,b), not y×N.
        for (const auto& [count, events] : registerWritesByCount(layer)) {
            items.push_back(
                {count, formatRegisterGroupLabel(events), false});
        }
        if (layer.envelope_timeline.loop_start_count) {
            items.push_back(
                {*layer.envelope_timeline.loop_start_count, "[", false});
        }
        if (layer.envelope_timeline.loop_end_count) {
            items.push_back(
                {*layer.envelope_timeline.loop_end_count, "]", false});
        }
        // Keep a stable visual order: pitch/@/y/[ /] by count, then kind.
        std::stable_sort(
            items.begin(), items.end(),
            [](const SummaryItem& left, const SummaryItem& right) {
                return left.count < right.count;
            });
        const int label_row_h = 12;
        const int count_row_h = 13;
        const int max_stack = juce::jmax(
            1,
            (bounds.getHeight() - count_row_h) / label_row_h);
        std::vector<int> stack_at_count(
            static_cast<std::size_t>(juce::jmax(1, end - begin + 1)), 0);
        for (const auto& item : items) {
            if (item.count < visible.getStart()
                || item.count > visible.getEnd()) {
                continue;
            }
            const int x0 = xForCount(bounds, static_cast<int>(item.count));
            const int x1 = xForCount(bounds, static_cast<int>(item.count) + 1);
            const int width = juce::jmax(8, x1 - x0 - 2);
            const auto slot = static_cast<std::size_t>(
                juce::jlimit(0, end - begin, static_cast<int>(item.count) - begin));
            const int stack = stack_at_count[slot]++;
            if (stack >= max_stack) {
                // Overflow: replace last visible slot with "+N" once.
                if (stack == max_stack) {
                    graphics.setColour(
                        item.count == static_cast<std::uint32_t>(selected_count_)
                            ? juce::Colour(kUiHoverAccent)
                            : colour);
                    graphics.drawText(
                        "+" + juce::String(stack_at_count[slot] - max_stack),
                        x0 + 1,
                        bounds.getY() + (max_stack - 1) * label_row_h,
                        width,
                        label_row_h,
                        juce::Justification::centred,
                        false);
                }
                continue;
            }
            graphics.setColour(
                item.count == static_cast<std::uint32_t>(selected_count_)
                    ? juce::Colour(kUiHoverAccent)
                    : colour);
            graphics.drawText(
                item.text,
                x0 + 1,
                bounds.getY() + stack * label_row_h,
                width,
                label_row_h,
                juce::Justification::centred,
                false);
        }
    }

    [[nodiscard]] static juce::Colour sourceColour(
        mgstc::engine::TimbreSource source) {
        switch (source) {
        case mgstc::engine::TimbreSource::Psg:
            return juce::Colour(0xFFB990FF);
        case mgstc::engine::TimbreSource::Scc:
            return juce::Colour(0xFF53E3A6);
        case mgstc::engine::TimbreSource::Opll:
            return juce::Colour(0xFFFFA75E);
        }
        return juce::Colours::white;
    }

    [[nodiscard]] static int clampPitchCommand(int value) noexcept {
        return juce::jlimit(kPitchCommandMin, kPitchCommandMax, value);
    }

    [[nodiscard]] static int cumulativePitchAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count <= static_cast<std::uint32_t>(
                       juce::jmax(0, timeline_count))) {
                acc += event.value;
            }
        }
        return acc;
    }

    [[nodiscard]] static int pitchCommandCountAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int count = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] static std::vector<int> splitPitchDelta(int delta) {
        std::vector<int> parts;
        while (delta > kPitchCommandMax) {
            parts.push_back(kPitchCommandMax);
            delta -= kPitchCommandMax;
        }
        while (delta < kPitchCommandMin) {
            parts.push_back(kPitchCommandMin);
            delta -= kPitchCommandMin;
        }
        if (delta != 0) {
            parts.push_back(delta);
        }
        return parts;
    }

    [[nodiscard]] std::pair<int, int> pitchDisplayRange(
        const mgstc::engine::CompositeLayer& layer) const {
        int extent = kPitchCommandMax;
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            extent = juce::jmax(extent, std::abs(acc));
        }
        if (hover_preview_value_) {
            extent = juce::jmax(extent, std::abs(*hover_preview_value_));
        }
        const bool extra =
            layer.source != mgstc::engine::TimbreSource::Opll;
        if (layer.pitch_sweep.enabled
            && layer.source != mgstc::engine::TimbreSource::Opll) {
            extent = juce::jmax(
                extent,
                mgstc::engine::pitchSweepDisplayExtent(
                    layer.pitch_sweep,
                    static_cast<std::uint32_t>(
                        juce::jmax(1, maximumCount()))));
        } else {
            extent = juce::jmax(
                extent,
                mgstc::engine::softwareLfoDisplayExtent(
                    layer.software_lfo, extra));
        }
        return {-extent, extent};
    }

    void notifyLayerEdit(int timeline_count, int shown_value, bool commit) {
        maybeExpandScrollExtent(timeline_count);
        if (commit
            && selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            clampLayerToBodyLimit(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
            refreshCountCeilingCache();
            updateScrollRanges();
        }
        selected_count_ = timeline_count;
        position_.setText(
            "ct " + juce::String(timeline_count),
            juce::dontSendNotification);
        value_.setText(juce::String(shown_value), false);
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        }
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, commit, commit);
        }
    }

    void addPitchCommand(int timeline_count, int operand, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        operand = clampPitchCommand(operand);
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .pitch_envelope.events;
        timbre_.layers[static_cast<std::size_t>(selected_layer_)]
            .pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        events.push_back({
            .kind = mgstc::engine::EnvelopeEventKind::Pitch,
            .value = operand,
            .count = static_cast<std::uint32_t>(timeline_count),
            .after_loop_start = newCommandAfterLoopStart(
                static_cast<std::uint32_t>(timeline_count)),
        });
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, operand, commit);
    }

    void editPitchCumulative(
        int timeline_count,
        int desired_cumulative,
        bool commit,
        bool append = false) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto& events = layer.pitch_envelope.events;
        layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        if (append) {
            const int previous = cumulativePitchAt(layer, timeline_count);
            const int desired_delta = desired_cumulative - previous;
            const bool after_loop = newCommandAfterLoopStart(
                static_cast<std::uint32_t>(timeline_count));
            for (const int part : splitPitchDelta(desired_delta)) {
                events.push_back({
                    .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                    .value = part,
                    .count = static_cast<std::uint32_t>(timeline_count),
                    .after_loop_start = after_loop,
                });
            }
            std::stable_sort(
                events.begin(), events.end(),
                [](const auto& left, const auto& right) {
                    return left.count < right.count;
                });
            notifyLayerEdit(timeline_count, desired_cumulative, commit);
            return;
        }
        const int previous = timeline_count <= 0
            ? 0
            : cumulativePitchAt(layer, timeline_count - 1);
        const int desired_delta = desired_cumulative - previous;
        // Preserve zone of erased siblings when redrawing the count.
        bool after_loop = newCommandAfterLoopStart(
            static_cast<std::uint32_t>(timeline_count));
        for (const auto& event : events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && event.after_loop_start) {
                after_loop = true;
                break;
            }
        }
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        for (const int part : splitPitchDelta(desired_delta)) {
            events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                .value = part,
                .count = static_cast<std::uint32_t>(timeline_count),
                .after_loop_start = after_loop,
            });
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, desired_cumulative, commit);
    }

    void drawPitchLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        const auto [minimum, maximum] = pitchDisplayRange(layer);
        juce::ignoreUnused(minimum);
        const int extent = juce::jmax(1, maximum);
        const auto map_y = [&](int value) {
            return pitchYFromValue(bounds, value, extent);
        };
        graphics.setColour(juce::Colour(0xFFE6EDF3).withAlpha(0.55F));
        graphics.drawHorizontalLine(
            juce::roundToInt(map_y(0)),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        graphics.setFont(UiFonts::dense());
        graphics.drawText(
            "0",
            bounds.getX() + 2,
            juce::roundToInt(map_y(0)) - 12,
            24,
            12,
            juce::Justification::centredLeft,
            false);

        struct Point {
            int count{};
            int cumulative{};
            int sibling{};
        };
        std::vector<Point> points;
        points.push_back({0, 0, 0});
        int acc = 0;
        std::map<int, int> siblings;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            const int count = static_cast<int>(event.count);
            points.push_back({count, acc, siblings[count]++});
        }
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().count));
        float y = map_y(points.front().cumulative);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].count));
            const float next_y = map_y(points[index].cumulative);
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
        if (layer.pitch_sweep.enabled
            && layer.source != mgstc::engine::TimbreSource::Opll) {
            const auto visible = visibleCountRange();
            const int start = juce::jmax(
                0, juce::roundToInt(std::floor(visible.getStart())));
            const int end = juce::jmin(
                maximumCount(),
                juce::roundToInt(std::ceil(visible.getEnd())));
            juce::Path sweep_path;
            bool started = false;
            for (int count = start; count <= end; ++count) {
                const int value = cumulativePitchAt(layer, count)
                    + static_cast<int>(
                        mgstc::engine::pitchSweepOffsetAtTick(
                            layer.pitch_sweep,
                            static_cast<std::uint32_t>(count)));
                const float sx = static_cast<float>(xForCount(bounds, count));
                const float sy = map_y(value);
                if (!started) {
                    sweep_path.startNewSubPath(sx, sy);
                    started = true;
                } else {
                    sweep_path.lineTo(sx, sy);
                }
            }
            if (started) {
                graphics.setColour(
                    juce::Colour(kUiHoverAccent).withAlpha(0.85F));
                graphics.strokePath(sweep_path, juce::PathStrokeType(1.0F));
            }
        } else if (layer.software_lfo.enabled) {
            const bool extra =
                layer.source != mgstc::engine::TimbreSource::Opll;
            const auto visible = visibleCountRange();
            const int start = juce::jmax(
                0, juce::roundToInt(std::floor(visible.getStart())));
            const int end = juce::jmin(
                maximumCount(),
                juce::roundToInt(std::ceil(visible.getEnd())));
            juce::Path lfo_path;
            bool started = false;
            for (int count = start; count <= end; ++count) {
                const int value = cumulativePitchAt(layer, count)
                    + static_cast<int>(
                        mgstc::engine::softwareLfoOffsetAtTick(
                            layer.software_lfo,
                            static_cast<std::uint32_t>(count),
                            extra));
                const float lx = static_cast<float>(xForCount(bounds, count));
                const float ly = map_y(value);
                if (!started) {
                    lfo_path.startNewSubPath(lx, ly);
                    started = true;
                } else {
                    lfo_path.lineTo(lx, ly);
                }
            }
            if (started) {
                graphics.setColour(
                    juce::Colour(kUiHoverAccent).withAlpha(0.85F));
                graphics.strokePath(lfo_path, juce::PathStrokeType(1.0F));
            }
        }
        for (const auto& point : points) {
            if (point.count == 0 && point.cumulative == 0 && point.sibling == 0
                && layer.pitch_envelope.events.empty()) {
                continue;
            }
            const float column = static_cast<float>(
                xForCount(bounds, point.count));
            const float next_column = static_cast<float>(
                xForCount(bounds, point.count + 1));
            const float span = juce::jmax(6.0F, next_column - column);
            const float px = column
                + span * (0.2F + 0.15F * static_cast<float>(point.sibling));
            const float py = map_y(point.cumulative);
            graphics.setColour(colour);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
    }

    void drawTimbreMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        graphics.setFont(UiFonts::dense(true));
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Timbre) {
                continue;
            }
            const int x = xForCount(bounds, static_cast<int>(event.count));
            graphics.setColour(colour);
            graphics.fillEllipse(
                static_cast<float>(x - 4),
                static_cast<float>(bounds.getCentreY() - 4),
                8.0F,
                8.0F);
            graphics.drawText(
                timbreCommandLabel(event),
                x + 6,
                bounds.getY(),
                juce::jmax(48, bounds.getRight() - x - 8),
                bounds.getHeight(),
                juce::Justification::centredLeft,
                false);
        }
    }

    [[nodiscard]] juce::String timbreCommandLabel(
        const mgstc::engine::EnvelopeEvent& event) const {
        if (event.timbre_pick == mgstc::engine::TimbrePick::OpllRom) {
            return opllRomPatchLabel(event.value);
        }
        if (event.target_library_id != 0 && timbre_name_callback_) {
            auto name = timbre_name_callback_(event.target_library_id);
            if (name.isNotEmpty()) {
                return name;
            }
        }
        return "@" + juce::String(event.value);
    }

    [[nodiscard]] juce::String timbreInspectLabel(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        if (const auto* event = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count))) {
            return timbreCommandLabel(*event);
        }
        return juce::String::fromUTF8("（なし）");
    }

    [[nodiscard]] static const mgstc::engine::EnvelopeEvent* timbreEventAt(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count) noexcept {
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == count) {
                return &event;
            }
        }
        return nullptr;
    }

    void pickTimbreAt(int timeline_count, bool) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        selected_count_ = timeline_count;
        std::vector<EnvelopeTimbreCatalogItem> catalog;
        if (catalog_callback_) {
            catalog = catalog_callback_(layer.source);
        }
        EnvelopeTimbreChoice current;
        if (const auto* existing = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count))) {
            current.pick = existing->timbre_pick;
            current.library_id = existing->target_library_id;
            current.rom_number = static_cast<std::uint8_t>(
                juce::jlimit(0, 14, existing->value));
        }
        const auto count = timeline_count;
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音色（@）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeTimbrePickContent(
            layer.source,
            catalog,
            current,
            [this, count](EnvelopeTimbreChoice choice) {
                applyTimbreChoice(count, choice);
            },
            [this, source = layer.source](std::uint64_t library_id) {
                if (open_timbre_callback_) {
                    open_timbre_callback_(
                        source == mgstc::engine::TimbreSource::Scc
                            ? "scc-envelope" : "opll-envelope",
                        library_id);
                }
            },
            [this, count] {
                removeTimbreAt(count);
            },
            [this] {
                if (manage_tags_callback_) {
                    manage_tags_callback_();
                }
            },
            [this](std::uint64_t id, juce::String name) {
                return mutate_name_callback_
                    ? mutate_name_callback_(id, std::move(name))
                    : false;
            },
            [this](std::uint64_t id, std::vector<std::string> tags) {
                return mutate_tags_callback_
                    ? mutate_tags_callback_(id, std::move(tags))
                    : false;
            },
            [this](std::uint64_t id, juce::String memo) {
                return mutate_memo_callback_
                    ? mutate_memo_callback_(id, std::move(memo))
                    : false;
            });
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void pickBaseTimbre(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        selected_layer_ = static_cast<int>(layer_index);
        std::vector<EnvelopeTimbreCatalogItem> catalog;
        if (catalog_callback_) {
            catalog = catalog_callback_(layer.source);
        }
        EnvelopeTimbreChoice current;
        if (mgstc::engine::layerUsesOpllRomBase(layer) && layer.base_opll_rom) {
            current.pick = mgstc::engine::TimbrePick::OpllRom;
            current.rom_number = *layer.base_opll_rom;
        } else if (layer.base_timbre) {
            current.pick = mgstc::engine::TimbrePick::Library;
            current.library_id = layer.base_timbre->library_id;
        }
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音色設定"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeTimbrePickContent(
            layer.source,
            catalog,
            current,
            [this, layer_index](EnvelopeTimbreChoice choice) {
                applyLayerBaseChoice(layer_index, choice);
            },
            [this, source = layer.source](std::uint64_t library_id) {
                if (open_timbre_callback_) {
                    open_timbre_callback_(
                        source == mgstc::engine::TimbreSource::Scc
                            ? "scc" : "opll",
                        library_id);
                }
            },
            {},
            [this] {
                if (manage_tags_callback_) {
                    manage_tags_callback_();
                }
            },
            [this](std::uint64_t id, juce::String name) {
                return mutate_name_callback_
                    ? mutate_name_callback_(id, std::move(name))
                    : false;
            },
            [this](std::uint64_t id, std::vector<std::string> tags) {
                return mutate_tags_callback_
                    ? mutate_tags_callback_(id, std::move(tags))
                    : false;
            },
            [this](std::uint64_t id, juce::String memo) {
                return mutate_memo_callback_
                    ? mutate_memo_callback_(id, std::move(memo))
                    : false;
            },
            true);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyLayerBaseChoice(
        std::size_t layer_index, const EnvelopeTimbreChoice& choice) {
        if (!assign_timbre_callback_ || layer_index >= timbre_.layers.size()) {
            return;
        }
        selected_layer_ = static_cast<int>(layer_index);
        LayerBaseTimbreAssign assign;
        if (choice.pick == mgstc::engine::TimbrePick::OpllRom) {
            assign.opll_rom = choice.rom_number;
        } else {
            assign.library_id = choice.library_id;
        }
        assign_timbre_callback_(layer_index, assign);
    }

    void promptVolumeNumeric(juce::Point<int> point) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto bounds = graphStrip(slots.volume);
        if (!bounds.isEmpty()) {
            selected_count_ = countAtX(bounds, point.x);
        }
        parameter_.setSelectedId(
            static_cast<int>(Parameter::Volume), juce::dontSendNotification);
        const int current = storedValueAt(
            layer, Parameter::Volume, selected_count_);
        bool automatic = false;
        bool precise = false;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume
                && event.count
                    == static_cast<std::uint32_t>(selected_count_)) {
                automatic = event.automatic;
                precise = event.precise;
                break;
            }
        }
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音量"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeNumericEntryContent(
            juce::String::fromUTF8(
                "音量（0～15）を入力。範囲外はクランプします"),
            juce::String(current),
            [this, automatic, precise](int typed) {
                editPoint(
                    selected_count_,
                    juce::jlimit(0, 15, typed),
                    true,
                    automatic,
                    precise);
            },
            true,
            automatic,
            [this](int typed, bool automatic_mode, bool precise_mode) {
                editPoint(
                    selected_count_,
                    juce::jlimit(0, 15, typed),
                    true,
                    automatic_mode,
                    precise_mode);
            },
            precise);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void promptPitchNumeric(juce::Point<int> point) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto bounds = graphStrip(slots.pitch);
        if (!bounds.isEmpty()) {
            selected_count_ = countAtX(bounds, point.x);
        }
        parameter_.setSelectedId(
            static_cast<int>(Parameter::Pitch), juce::dontSendNotification);
        const int current = lastPitchCommandAt(layer, selected_count_);
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音程（\\）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeNumericEntryContent(
            juce::String::fromUTF8(
                "周波数オフセットを入力。-200 は -127 -73 に展開します"),
            juce::String(current),
            [this](int typed) {
                applyPitchTypedDelta(selected_count_, typed);
            });
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyPitchTypedDelta(int timeline_count, int typed_delta) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto& events = layer.pitch_envelope.events;
        layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        bool after_loop = newCommandAfterLoopStart(
            static_cast<std::uint32_t>(timeline_count));
        for (const auto& event : events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && event.after_loop_start) {
                after_loop = true;
                break;
            }
        }
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        auto parts = splitPitchDelta(typed_delta);
        if (parts.empty() && typed_delta == 0) {
            parts.push_back(0);
        }
        for (const int part : parts) {
            events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                .value = part,
                .count = static_cast<std::uint32_t>(timeline_count),
                .after_loop_start = after_loop,
            });
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(
            timeline_count,
            cumulativePitchAt(layer, timeline_count),
            true);
    }

    [[nodiscard]] bool countHasManualOriginalY(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count) const noexcept {
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                && event.count == count
                && event.value >= 0
                && event.value <= 7) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] juce::String registerInspectLabel(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, timeline_count));
        const auto grouped = registerWritesByCount(layer);
        const auto it = grouped.find(count);
        if (it == grouped.end() || it->second.empty()) {
            return juce::String::fromUTF8("（なし）");
        }
        return formatRegisterGroupLabel(it->second);
    }

    [[nodiscard]] juce::String registerInspectTooltip(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, timeline_count));
        const auto grouped = registerWritesByCount(layer);
        const auto it = grouped.find(count);
        if (it == grouped.end() || it->second.empty()) {
            return juce::String::fromUTF8(
                "このカウントに手動 y はありません");
        }
        return formatRegisterGroupTooltip(it->second);
    }

    void pickRegisterAt(int timeline_count) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        selected_count_ = timeline_count;
        const auto count = static_cast<std::uint32_t>(timeline_count);
        const auto* library = timbreLibrary();

        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            std::uint8_t reg = 0;
            std::uint8_t data = 0;
            for (const auto& event : layer.timbre_automation) {
                if (event.kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && event.count == count) {
                    reg = static_cast<std::uint8_t>(
                        juce::jlimit(0, 15, event.value));
                    data = static_cast<std::uint8_t>(
                        juce::jlimit(0, 255, event.secondary));
                }
            }
            auto* dialog = new ModalDialogWindow(
                juce::String::fromUTF8("手動 y（PSG）"),
                juce::Colour(0xFF1B222C));
            auto* content = new EnvelopePsgYParamContent(
                reg,
                data,
                [this, count](std::uint8_t r, std::uint8_t d) {
                    applyPsgManualY(count, r, d);
                });
            dialog->setUsingNativeTitleBar(true);
            dialog->setResizable(false, false);
            dialog->setContentOwned(content, true);
            dialog->centreAroundComponent(
                this, content->getWidth(), content->getHeight() + 32);
            dialog->enterModalState(true, nullptr, true);
            return;
        }

        // OPLL: original-tone regs 0–7 only; ROM intervals unavailable.
        if (mgstc::engine::opllActiveTimbreAt(layer, count).kind
            == mgstc::engine::OpllActiveTimbreKind::Rom) {
            reportStatus(juce::String::fromUTF8(
                "ROM音色区間ではオリジナル音色の手動yは使えません"));
            return;
        }
        // Display / diff include TL/FB auto at this count (§7.5 after manual y).
        // Apply baseline uses the same auto-inclusive image so unchanged auto
        // fields are not re-emitted as manual y.
        const auto baseline_regs = mgstc::engine::opllOriginalRegisterImageAt(
            layer, count, library, false, true);
        if (!baseline_regs) {
            reportStatus(juce::String::fromUTF8(
                "このステップにオリジナル音色画像がありません"
                "（ベース／@ライブラリを設定してください）"));
            return;
        }
        const bool editing = countHasManualOriginalY(layer, count);
        const auto current_regs = editing
            ? mgstc::engine::opllOriginalRegisterImageAt(
                  layer, count, library, true, true)
                  .value_or(*baseline_regs)
            : *baseline_regs;
        const auto baseline_patch =
            mgstc::engine::decodeOpllPatch(*baseline_regs);
        const auto initial_patch =
            mgstc::engine::decodeOpllPatch(current_regs);

        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("手動 y（FMパラメータ）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeOpllYParamContent(
            baseline_patch,
            initial_patch,
            editing,
            [this, count, baseline = *baseline_regs](
                mgstc::engine::OpllPatchParameters patch) {
                applyOpllManualYPatch(count, baseline, patch);
            },
            audio_service_);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyOpllManualYPatch(
        std::uint32_t count,
        const std::array<std::uint8_t, 8>& baseline_regs,
        const mgstc::engine::OpllPatchParameters& patch) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        // Replace the whole same-step original-tone y group (regs 0–7).
        // Preserve non-original y (reg > 7) at this count.
        bool after_loop = newCommandAfterLoopStart(count);
        for (const auto& event : events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                && event.count == count
                && event.value >= 0
                && event.value <= 7
                && event.after_loop_start) {
                after_loop = true;
                break;
            }
        }
        std::erase_if(events, [count](const auto& event) {
            return event.kind
                    == mgstc::engine::EnvelopeEventKind::RegisterWrite
                && event.count == count
                && event.value >= 0
                && event.value <= 7;
        });
        const auto next = mgstc::engine::encodeOpllPatch(patch);
        int emitted = 0;
        for (int reg = 0; reg < 8; ++reg) {
            if (next[static_cast<std::size_t>(reg)]
                == baseline_regs[static_cast<std::size_t>(reg)]) {
                continue;
            }
            mgstc::engine::EnvelopeEvent event;
            event.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite;
            event.count = count;
            event.value = reg;
            event.secondary = next[static_cast<std::size_t>(reg)];
            event.after_loop_start = after_loop;
            events.push_back(event);
            ++emitted;
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                if (left.count != right.count) {
                    return left.count < right.count;
                }
                return false;
            });
        notifyLayerEdit(static_cast<int>(count), emitted, true);
        reportStatus(
            emitted == 0
                ? juce::String::fromUTF8(
                      "変更なし（このステップの手動y群をクリア）")
                : juce::String::fromUTF8("手動yを ")
                      + juce::String(emitted)
                      + juce::String::fromUTF8(" 本設定しました"));
    }

    void applyPsgManualY(
        std::uint32_t count, std::uint8_t reg, std::uint8_t data) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        mgstc::engine::EnvelopeEvent event;
        event.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite;
        event.count = count;
        event.value = reg;
        event.secondary = data;
        event.after_loop_start = newCommandAfterLoopStart(count);
        events.push_back(event);
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(static_cast<int>(count), reg, true);
        reportStatus(
            "y" + juce::String(static_cast<int>(reg)) + ","
            + juce::String(static_cast<int>(data))
            + juce::String::fromUTF8(" を追加しました"));
    }

    void applyTimbreChoice(int timeline_count, const EnvelopeTimbreChoice& choice) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        mgstc::engine::EnvelopeEvent event;
        event.kind = mgstc::engine::EnvelopeEventKind::Timbre;
        event.count = static_cast<std::uint32_t>(timeline_count);
        event.timbre_pick = choice.pick;
        event.target_library_id = choice.library_id;
        event.value = choice.pick == mgstc::engine::TimbrePick::OpllRom
            ? static_cast<std::int32_t>(choice.rom_number)
            : 0;
        event.after_loop_start = newCommandAfterLoopStart(event.count);
        events.push_back(event);
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, event.value, true);
    }

    void removeTimbreAt(int timeline_count) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        notifyLayerEdit(timeline_count, 0, true);
    }

    void reportStatus(const juce::String& text) {
        if (status_callback_) {
            status_callback_(text);
        }
    }

    void notifyDelete(const juce::String& what) {
        syncPointEditors();
        syncRegisterAutoEditors();
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        }
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, true);
        }
        reportStatus(what);
    }

    void deleteCommandAt(juce::Point<int> point) {
        const int layer_index = layerAt(point);
        if (layer_index < 0
            || layer_index >= static_cast<int>(timbre_.layers.size())
            || layer_index >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        selected_layer_ = layer_index;
        auto& layer = timbre_.layers[static_cast<std::size_t>(layer_index)];
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer_index)];
        if (!graph.contains(point)) {
            return;
        }
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto count_bounds = graph;
        count_bounds.removeFromLeft(UiLayout::editSubLaneLabelW);
        selected_count_ = countAtX(count_bounds, point.x);
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));

        if (slots.volume.contains(point)) {
            auto& events = layer.volume_envelope.events;
            const auto before = events.size();
            std::erase_if(events, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Volume
                    && event.count == count;
            });
            if (events.size() != before) {
                notifyDelete(juce::String::fromUTF8("音量イベントを削除しました"));
            }
            return;
        }
        if (slots.pitch.contains(point)) {
            auto& events = layer.pitch_envelope.events;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::Pitch
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("\\命令を削除しました"));
            }
            return;
        }
        if (slots.timbre.contains(point)) {
            const auto before = layer.timbre_automation.size();
            std::erase_if(layer.timbre_automation, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                    && event.count == count;
            });
            if (layer.timbre_automation.size() != before) {
                notifyDelete(juce::String::fromUTF8("@切替を削除しました"));
            }
            return;
        }
        if (slots.register_write.contains(point)) {
            auto& events = layer.timbre_automation;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("y命令を削除しました"));
            }
            return;
        }
        if (slots.tl_auto.contains(point) || slots.fb_auto.contains(point)) {
            auto& auto_lane = slots.tl_auto.contains(point)
                ? layer.opll_tl_auto
                : layer.opll_fb_auto;
            // Right-click clear applies only to free-curve drawings.
            if (auto_lane.mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
                reportStatus(juce::String::fromUTF8(
                    "TL/FB自動の右クリック削除は自由曲線のみです"
                    "（上昇／下降／LFOはモードやパラメーターで変更）"));
                parameter_.setSelectedId(
                    slots.tl_auto.contains(point)
                        ? static_cast<int>(Parameter::OpllTlAuto)
                        : static_cast<int>(Parameter::OpllFbAuto),
                    juce::dontSendNotification);
                syncRegisterAutoEditors();
                return;
            }
            if (auto_lane.free_curve.empty()) {
                return;
            }
            const auto step = juce::jmax<std::uint8_t>(1, auto_lane.coarseness);
            if (count < auto_lane.start_count) {
                return;
            }
            const auto offset =
                (count - auto_lane.start_count) / step;
            if (offset >= auto_lane.free_curve.size()) {
                // Clear entire free curve when clicking past end.
                auto_lane.free_curve.clear();
                notifyDelete(juce::String::fromUTF8(
                    "自由曲線をクリアしました"));
                return;
            }
            auto_lane.free_curve.erase(
                auto_lane.free_curve.begin()
                + static_cast<std::ptrdiff_t>(offset));
            if (auto_lane.free_curve.empty()) {
                auto_lane.start_count = 0;
            }
            notifyDelete(juce::String::fromUTF8(
                "自由曲線の点を削除しました"));
            return;
        }
    }

    void deleteStackChipAt(int chip_index) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || chip_index < 0) {
            return;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        const bool loop_start = layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
        int cursor = 0;
        const auto matches_zone = [loop_start](
                                      const mgstc::engine::EnvelopeEvent& event,
                                      bool want_after) {
            if (!loop_start) {
                return !want_after;
            }
            return event.after_loop_start == want_after;
        };
        for (bool want_after : {false, true}) {
            for (auto it = layer.pitch_envelope.events.begin();
                 it != layer.pitch_envelope.events.end();
                 ++it) {
                if (it->kind == mgstc::engine::EnvelopeEventKind::Pitch
                    && it->count == count
                    && matches_zone(*it, want_after)) {
                    if (cursor == chip_index) {
                        layer.pitch_envelope.events.erase(it);
                        notifyDelete(juce::String::fromUTF8(
                            "命令スタックから\\を削除"));
                        return;
                    }
                    ++cursor;
                }
            }
            for (auto it = layer.timbre_automation.begin();
                 it != layer.timbre_automation.end();
                 ++it) {
                if (it->count != count || !matches_zone(*it, want_after)) {
                    continue;
                }
                if (it->kind == mgstc::engine::EnvelopeEventKind::Timbre
                    || it->kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                    if (cursor == chip_index) {
                        const bool was_y = it->kind
                            == mgstc::engine::EnvelopeEventKind::RegisterWrite;
                        layer.timbre_automation.erase(it);
                        notifyDelete(
                            was_y
                                ? juce::String::fromUTF8(
                                      "命令スタックからyを削除")
                                : juce::String::fromUTF8(
                                      "命令スタックから@を削除"));
                        return;
                    }
                    ++cursor;
                }
            }
        }
    }

    void moveStackChipAt(int chip_index, bool to_after) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || chip_index < 0) {
            return;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        if (!layer.envelope_timeline.loop_start_count
            || *layer.envelope_timeline.loop_start_count != count) {
            return;
        }
        int cursor = 0;
        // Walk in the same order as refreshCommandStack / deleteStackChipAt.
        for (bool want_after : {false, true}) {
            for (auto& event : layer.pitch_envelope.events) {
                if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                    && event.count == count
                    && event.after_loop_start == want_after) {
                    if (cursor == chip_index) {
                        event.after_loop_start = to_after;
                        notifyDelete(
                            to_after
                                ? juce::String::fromUTF8(
                                      "命令をループ後へ移動しました")
                                : juce::String::fromUTF8(
                                      "命令をループ前へ移動しました"));
                        return;
                    }
                    ++cursor;
                }
            }
            for (auto& event : layer.timbre_automation) {
                if (event.count != count
                    || event.after_loop_start != want_after) {
                    continue;
                }
                if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                    || event.kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                    if (cursor == chip_index) {
                        event.after_loop_start = to_after;
                        notifyDelete(
                            to_after
                                ? juce::String::fromUTF8(
                                      "命令をループ後へ移動しました")
                                : juce::String::fromUTF8(
                                      "命令をループ前へ移動しました"));
                        return;
                    }
                    ++cursor;
                }
            }
        }
    }

    void rebuildLayerSetups() {
        while (layer_setups_.size()
               > static_cast<int>(timbre_.layers.size())) {
            layer_setups_.removeLast();
        }
        while (layer_setups_.size()
               < static_cast<int>(timbre_.layers.size())) {
            auto* setup = layer_setups_.add(new CompositeLayerSetupView());
            graph_clip_.addAndMakeVisible(setup);
        }
        for (int index = 0; index < layer_setups_.size(); ++index) {
            auto* setup = layer_setups_[index];
            const auto layer_index = static_cast<std::size_t>(index);
            setup->setCallbacks(
                [this, layer_index](bool commit) {
                    selected_layer_ = static_cast<int>(layer_index);
                    if (layer_index < timbre_.layers.size()
                        && static_cast<int>(layer_index) < layer_setups_.size()) {
                        layer_setups_[static_cast<int>(layer_index)]
                            ->applyToLayer(timbre_.layers[layer_index]);
                    }
                    syncPointEditors();
                    syncRegisterAutoEditors();
                    syncInspector();
                    updateScrollRanges();
                    resized();
                    repaint();
                    if (edit_callback_) {
                        edit_callback_(timbre_, commit, false);
                    }
                    syncRateEditorFromModel();
                },
                [this, layer_index] {
                    if (layer_index >= timbre_.layers.size()
                        || !open_timbre_callback_) {
                        return;
                    }
                    const auto& layer = timbre_.layers[layer_index];
                    if (!layer.base_timbre
                        || layer.base_timbre->library_id
                            == mgstc::engine::kSccTrianglePresetLibraryId) {
                        return;
                    }
                    open_timbre_callback_(
                        layer.source == mgstc::engine::TimbreSource::Scc
                            ? "scc" : "opll",
                        layer.base_timbre->library_id);
                },
                [this, layer_index] {
                    pickBaseTimbre(layer_index);
                },
                [this, layer_index] {
                    openSoftwareLfoDialog(layer_index);
                },
                [this, layer_index] {
                    openRateEditor(layer_index);
                },
                [this, layer_index] {
                    duplicateLayerAt(layer_index);
                },
                [this, layer_index] {
                    juce::MessageManager::callAsync(
                        [safe = juce::Component::SafePointer<Impl>(this),
                         layer_index] {
                            if (safe != nullptr) {
                                safe->requestRemoveLayer(layer_index);
                            }
                        });
                });
            setup->syncFromLayer(timbre_.layers[layer_index]);
        }
        updateAddButtons();
        layoutLayerSetups();
    }

    void layoutLayerSetups() {
        using namespace UiLayout;
        for (int index = 0; index < layer_setups_.size(); ++index) {
            if (index >= static_cast<int>(lane_frames_.size())
                || lane_frames_[static_cast<std::size_t>(index)].isEmpty()) {
                layer_setups_[index]->setVisible(false);
                continue;
            }
            auto bounds = lane_frames_[static_cast<std::size_t>(index)];
            bounds.setWidth(compositeLaneLabelW);
            const bool edit_tier = index == selected_layer_;
            if (edit_tier) {
                bounds.setHeight(
                    juce::jmin(bounds.getHeight(), compositeSetupContentH));
            }
            bounds = bounds.translated(
                -graph_clip_.getX(), -graph_clip_.getY());
            if (!bounds.intersects(graph_clip_.getLocalBounds())) {
                layer_setups_[index]->setVisible(false);
                continue;
            }
            layer_setups_[index]->setBounds(bounds);
            layer_setups_[index]->setVisible(true);
            layer_setups_[index]->toFront(false);
        }
    }

    void layoutEditToolsDock() {
        using namespace UiLayout;
        const bool show = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(lane_frames_.size())
            && !lane_frames_[static_cast<std::size_t>(selected_layer_)]
                    .isEmpty();
        const bool show_rate = show
            && timbre_.layers[
                static_cast<std::size_t>(selected_layer_)]
                    .volume_envelope.kind
                == mgstc::engine::EnvelopeKind::Rate;
        const bool show_auto = show && !show_rate
            && selectedLayerShowsRegisterAuto();

        auto set_tools_visible = [this, show, show_auto, show_rate](
            bool visible) {
            const bool sequence_tools = visible && !show_rate;
            for (auto* c : std::initializer_list<juce::Component*>{
                     &edit_mode_, &parameter_, &position_, &value_label_,
                     &value_, &apply_, &inspector_selection_,
                     &length_label_, &length_editor_, &loop_start_label_,
                     &loop_start_editor_, &loop_end_label_, &loop_end_editor_,
                     &inspector_apply_, &clear_loop_, &command_stack_}) {
                c->setVisible(sequence_tools);
            }
            envelope_mml_preview_.setVisible(visible);
            auto_mode_.setVisible(visible && show_auto);
            auto_depth_.setVisible(visible && show_auto);
            auto_speed_.setVisible(visible && show_auto);
            auto_coarseness_.setVisible(visible && show_auto);
            auto_stop_.setVisible(visible && show_auto);
            auto_depth_label_.setVisible(visible && show_auto);
            auto_speed_label_.setVisible(visible && show_auto);
            auto_coarseness_label_.setVisible(visible && show_auto);
            auto_stop_label_.setVisible(visible && show_auto);
        };

        if (!show) {
            set_tools_visible(false);
            return;
        }
        set_tools_visible(true);
        updateEnvelopeMmlPreview();

        auto frame = lane_frames_[static_cast<std::size_t>(selected_layer_)];
        frame = frame.translated(-graph_clip_.getX(), -graph_clip_.getY());
        const int setup_h = compositeSetupContentH;
        auto dock = frame;
        dock.setWidth(compositeLaneLabelW);
        dock.removeFromTop(setup_h);
        dock = dock.reduced(panelPad);
        dock.removeFromTop(editMarkerLaneH); // title painted in paint()

        if (!show_rate) {
            auto row1 = dock.removeFromTop(fieldH);
            apply_.setBounds(row1.removeFromRight(compositeDockApplyW));
            row1.removeFromRight(controlGap);
            value_.setBounds(row1.removeFromRight(compositeDockFieldW));
            value_label_.setBounds(row1.removeFromRight(compositeDockLabelW));
            row1.removeFromRight(controlGap);
            position_.setBounds(row1.removeFromRight(compositeDockCountW));
            row1.removeFromRight(controlGap);
            parameter_.setBounds(row1.removeFromRight(compositeDockParamW));
            row1.removeFromRight(controlGap);
            edit_mode_.setBounds(row1);

            if (show_auto) {
                dock.removeFromTop(sm);
                auto auto_row = dock.removeFromTop(fieldH);
                auto_stop_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoValueW));
                auto_stop_label_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoLabelW));
                auto_row.removeFromRight(controlGap);
                auto_coarseness_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoValueW));
                auto_coarseness_label_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoLabelW));
                auto_row.removeFromRight(controlGap);
                auto_speed_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoValueW));
                auto_speed_label_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoLabelW));
                auto_row.removeFromRight(controlGap);
                auto_depth_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoValueW));
                auto_depth_label_.setBounds(
                    auto_row.removeFromRight(compositeDockAutoLabelW));
                auto_row.removeFromRight(controlGap);
                auto_mode_.setBounds(auto_row);
            }

            dock.removeFromTop(sm);
            auto loop_row = dock.removeFromTop(fieldH);
            clear_loop_.setBounds(
                loop_row.removeFromRight(compositeDockClearLoopW));
            loop_row.removeFromRight(controlGap);
            inspector_apply_.setBounds(
                loop_row.removeFromRight(compositeDockApplyW));
            loop_row.removeFromRight(controlGap);
            loop_end_editor_.setBounds(
                loop_row.removeFromRight(compositeDockFieldW));
            loop_end_label_.setBounds(
                loop_row.removeFromRight(compositeDockLabelW));
            loop_row.removeFromRight(controlGap);
            loop_start_editor_.setBounds(
                loop_row.removeFromRight(compositeDockFieldW));
            loop_start_label_.setBounds(
                loop_row.removeFromRight(compositeDockLabelW));
            loop_row.removeFromRight(controlGap);
            length_editor_.setBounds(
                loop_row.removeFromRight(compositeDockFieldW));
            length_label_.setBounds(
                loop_row.removeFromRight(compositeDockLengthLabelW));
            inspector_selection_.setBounds(loop_row);

            dock.removeFromTop(sm);
            command_stack_.setBounds(dock.removeFromTop(
                juce::jmin(
                    commandStackH, juce::jmax(fieldH, dock.getHeight()))));
        }

        // Position MGSC preview over the edit-tier preview strip.
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(graph_bounds_.size())
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            const auto graph =
                graph_bounds_[static_cast<std::size_t>(selected_layer_)];
            const auto slots = makeEditLaneSlots(
                graph,
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                    .source);
            auto preview = slots.preview;
            preview.removeFromLeft(editSubLaneLabelW);
            preview = preview.translated(
                -graph_clip_.getX(), -graph_clip_.getY());
            envelope_mml_preview_.setBounds(preview);
            envelope_mml_preview_.toFront(false);
        } else {
            envelope_mml_preview_.setVisible(false);
        }

        for (auto* c : std::initializer_list<juce::Component*>{
                 &edit_mode_, &parameter_, &position_, &value_label_, &value_,
                 &apply_, &auto_mode_, &auto_depth_, &auto_speed_,
                 &auto_coarseness_, &auto_stop_, &auto_depth_label_,
                 &auto_speed_label_, &auto_coarseness_label_, &auto_stop_label_,
                 &inspector_selection_, &length_label_, &length_editor_,
                 &loop_start_label_, &loop_start_editor_, &loop_end_label_,
                 &loop_end_editor_, &inspector_apply_, &clear_loop_,
                 &command_stack_}) {
            if (c->isVisible()) {
                c->toFront(false);
            }
        }
        if (envelope_mml_preview_.isVisible()) {
            envelope_mml_preview_.toFront(false);
        }
    }

    void commitPlaybackTempoFromEditor() {
        const int next = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_.getText().getIntValue() == 0
                ? mgstc::engine::kMgscDefaultTempo
                : tempo_.getText().getIntValue());
        tempo_.setText(juce::String(next), juce::dontSendNotification);
        if (next == playback_tempo_ && next == timbre_.playback_tempo) {
            return;
        }
        playback_tempo_ = next;
        timbre_.playback_tempo = next;
        repaint();
        if (edit_callback_) {
            edit_callback_(timbre_, true, false);
        }
    }

    mgstc::engine::CompositeTimbre timbre_;
    juce::ComboBox parameter_;
    juce::ComboBox edit_mode_;
    juce::ComboBox auto_mode_;
    juce::TextButton psg_add_;
    juce::TextButton scc_add_;
    juce::TextButton opll_add_;
    juce::TextEditor value_;
    juce::Label position_;
    juce::Label value_label_;
    juce::TextButton apply_;
    juce::Label auto_depth_label_;
    juce::Label auto_speed_label_;
    juce::Label auto_coarseness_label_;
    juce::Label auto_stop_label_;
    juce::TextEditor auto_depth_;
    juce::TextEditor auto_speed_;
    juce::TextEditor auto_coarseness_;
    juce::TextEditor auto_stop_;
    juce::Label inspector_selection_;
    juce::Label length_label_;
    juce::Label loop_start_label_;
    juce::Label loop_end_label_;
    juce::TextEditor length_editor_;
    juce::TextEditor loop_start_editor_;
    juce::TextEditor loop_end_editor_;
    juce::TextButton inspector_apply_;
    juce::TextButton clear_loop_;
    CountCommandStackView command_stack_;
    juce::TextEditor envelope_mml_preview_;
    juce::String last_envelope_mml_preview_;
    juce::ScrollBar horizontal_scroll_;
    juce::ScrollBar vertical_scroll_;
    EditCallback edit_callback_;
    StatusCallback status_callback_;
    CatalogCallback catalog_callback_;
    OpenTimbreCallback open_timbre_callback_;
    AssignTimbreCallback assign_timbre_callback_;
    TimbreNameCallback timbre_name_callback_;
    TimbreLibraryCallback timbre_library_callback_;
    ManageTagsCallback manage_tags_callback_;
    MutateTimbreNameCallback mutate_name_callback_;
    MutateTimbreTagsCallback mutate_tags_callback_;
    MutateTimbreMemoCallback mutate_memo_callback_;
    LfoSessionCallback lfo_session_callback_;
    LfoPollKeysCallback lfo_poll_keys_callback_;
    SharedAudioHost* audio_service_{};
    std::unique_ptr<RateEnvelopeEditorWindow> rate_window_;
    std::optional<std::size_t> rate_editor_layer_;
    juce::OwnedArray<CompositeLayerSetupView> layer_setups_;
    juce::Component graph_clip_;
    juce::Label tempo_label_;
    juce::TextEditor tempo_;
    std::vector<juce::Rectangle<int>> graph_bounds_;
    std::vector<juce::Rectangle<int>> lane_frames_;
    std::optional<juce::Rectangle<int>> edit_draw_bounds_;
    std::optional<int> hover_preview_value_;
    std::optional<juce::Point<int>> hover_mouse_pos_;
    std::optional<Parameter> hover_edit_parameter_;
    int selected_layer_{};
    int selected_count_{};
    bool loop_insert_after_{};
    int playback_tempo_{mgstc::engine::kMgscDefaultTempo};
    int cached_count_ceiling_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    int cached_layer_fit_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    int scroll_extent_counts_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    bool scroll_extent_initialized_{};
    bool drawing_{};
    static constexpr int kScopeHistorySize = 4096;
    std::array<std::array<float, kScopeHistorySize>, 4>
        scope_history_{};
    int scope_write_position_{};
    int scope_size_{};
    std::size_t scope_visible_sample_count_{};
    std::uint8_t audition_note_{kPreviewNote};

    void drawHoverValuePopup(juce::Graphics& graphics) const {
        if (!hover_preview_value_ || !hover_mouse_pos_) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            return;
        }
        const auto text = juce::String(*hover_preview_value_);
        graphics.setFont(UiFonts::dense(true));
        const int text_w = juce::jmax(
            24, text.length() * 8 + 12);
        const int text_h = 18;
        auto box = juce::Rectangle<int>(
            hover_mouse_pos_->x + 12,
            hover_mouse_pos_->y - text_h - 8,
            text_w,
            text_h);
        if (auto area = graphArea(); !area.isEmpty()) {
            box.setX(juce::jlimit(
                area.getX(), area.getRight() - text_w, box.getX()));
            box.setY(juce::jlimit(
                area.getY(), area.getBottom() - text_h, box.getY()));
        }
        graphics.setColour(juce::Colour(0xEE151A20));
        graphics.fillRoundedRectangle(box.toFloat(), 3.0F);
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.drawRoundedRectangle(box.toFloat(), 3.0F, 1.0F);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.drawText(
            text, box, juce::Justification::centred, false);
    }
};


namespace mgstc::app {

CompositeTimeline::CompositeTimeline() {
    impl_ = std::make_unique<Impl>();
    addAndMakeVisible(*impl_);
}

CompositeTimeline::~CompositeTimeline() = default;

void CompositeTimeline::resized() {
    if (impl_ != nullptr) {
        impl_->setBounds(getLocalBounds());
    }
}

void CompositeTimeline::setEditCallback(EditCallback callback) {
    impl_->setEditCallback(std::move(callback));
}

void CompositeTimeline::setStatusCallback(StatusCallback callback) {
    impl_->setStatusCallback(std::move(callback));
}

void CompositeTimeline::setTimbreCatalogCallback(CatalogCallback callback) {
    impl_->setTimbreCatalogCallback(std::move(callback));
}

void CompositeTimeline::setOpenTimbreCallback(OpenTimbreCallback callback) {
    impl_->setOpenTimbreCallback(std::move(callback));
}

void CompositeTimeline::setAssignTimbreCallback(AssignTimbreCallback callback) {
    impl_->setAssignTimbreCallback(std::move(callback));
}

void CompositeTimeline::setTimbreNameCallback(TimbreNameCallback callback) {
    impl_->setTimbreNameCallback(std::move(callback));
}

void CompositeTimeline::setTimbreLibraryCallback(TimbreLibraryCallback callback) {
    impl_->setTimbreLibraryCallback(std::move(callback));
}

void CompositeTimeline::setManageTagsCallback(ManageTagsCallback callback) {
    impl_->setManageTagsCallback(std::move(callback));
}

void CompositeTimeline::setMutateTimbreNameCallback(
    MutateTimbreNameCallback callback) {
    impl_->setMutateTimbreNameCallback(std::move(callback));
}

void CompositeTimeline::setMutateTimbreTagsCallback(
    MutateTimbreTagsCallback callback) {
    impl_->setMutateTimbreTagsCallback(std::move(callback));
}

void CompositeTimeline::setMutateTimbreMemoCallback(
    MutateTimbreMemoCallback callback) {
    impl_->setMutateTimbreMemoCallback(std::move(callback));
}

void CompositeTimeline::setLfoSessionCallback(LfoSessionCallback callback) {
    impl_->setLfoSessionCallback(std::move(callback));
}

void CompositeTimeline::setLfoPollKeysCallback(LfoPollKeysCallback callback) {
    impl_->setLfoPollKeysCallback(std::move(callback));
}

void CompositeTimeline::setAudioService(SharedAudioHost* audio_service) noexcept {
    impl_->setAudioService(audio_service);
}

void CompositeTimeline::setPlaybackTempo(int tempo_bpm) {
    impl_->setPlaybackTempo(tempo_bpm);
}

void CompositeTimeline::setTimbre(
    const engine::CompositeTimbre& timbre,
    bool reset_scroll_extent) {
    impl_->setTimbre(timbre, reset_scroll_extent);
}

void CompositeTimeline::refreshUiScaleFonts() {
    impl_->refreshUiScaleFonts();
}

bool CompositeTimeline::appendScopeFrame(
    const engine::OpllScopeFrame& frame,
    std::uint8_t midi_note) {
    return impl_->appendScopeFrame(frame, midi_note);
}

}  // namespace mgstc::app
