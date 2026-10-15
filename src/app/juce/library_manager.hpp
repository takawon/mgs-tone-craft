// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "editor_icons.hpp"
#include "juce_utf8.hpp"
#include "library_set_model.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"
#include "tone_mini_preview.hpp"
#include "switch_look_and_feel.hpp"
#include "tag_ui.hpp"
#include "tone_library_session.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"

using mgstc::app::LibrarySetDragItem;
using mgstc::app::LibrarySetDropPlace;
using mgstc::app::LibrarySetDropTarget;
using mgstc::app::applyLibrarySetDrop;
using mgstc::app::librarySetDelete;
using mgstc::app::librarySetNameTaken;
using mgstc::app::librarySetNextId;
using mgstc::app::librarySetRemoveMembership;
using mgstc::app::librarySetUniqueName;
using mgstc::app::paintToneMiniEnvelope;
using mgstc::app::paintToneMiniLevels;
using mgstc::app::paintToneMiniWave;

enum class LibraryManagerKind : std::uint8_t {
    Scc,
    Opll,
    Composite,
};

enum class LibraryManagerRowRole : std::uint8_t {
    Timbre,
    SetHeader,
    UnassignedHeader,
    Member,
};

struct LibraryManagerRow {
    LibraryManagerKind kind{};
    std::uint64_t id{};
    std::string name;
    std::vector<std::string> tags;
    std::string memo;
    bool favorite{};
    std::uint32_t revision{1};
    std::int64_t updated_unix_seconds{};
    std::int64_t last_used_unix_seconds{};
    LibraryManagerRowRole role{LibraryManagerRowRole::Timbre};
    std::uint64_t set_id{};
    bool expanded{false};
};

[[nodiscard]] inline bool libraryManagerRowIsBody(
    const LibraryManagerRow& row) {
    return row.role == LibraryManagerRowRole::Timbre
        || row.role == LibraryManagerRowRole::Member;
}

[[nodiscard]] inline bool libraryManagerRowIsHeader(
    const LibraryManagerRow& row) {
    return row.role == LibraryManagerRowRole::SetHeader
        || row.role == LibraryManagerRowRole::UnassignedHeader;
}

[[nodiscard]] inline juce::String formatLibraryManagerTime(
    std::int64_t unix_seconds) {
    if (unix_seconds <= 0) {
        return juce::String::fromUTF8("—");
    }
    return juce::Time(unix_seconds * 1000)
        .formatted("%Y-%m-%d %H:%M");
}

class LibraryManagerTableModel final
    : public juce::TableListBoxModel {
public:
    enum Column {
        kPreview = 1,
        kName = 2,
        kTags = 3,
        kUpdated = 4,
        kFavorite = 5,
        kMini = 6,
    };

    using PreviewHandler =
        std::function<void(const LibraryManagerRow&)>;
    using FavoriteHandler =
        std::function<void(int, const LibraryManagerRow&)>;
    using SelectionHandler = std::function<void()>;
    using SortHandler = std::function<void()>;
    using RowHandler = std::function<void(const LibraryManagerRow&)>;
    using MiniPaintHandler = std::function<void(
        juce::Graphics&,
        juce::Rectangle<float>,
        const LibraryManagerRow&)>;

    void setTable(juce::TableListBox* table) {
        table_ = table;
    }

    void setRows(std::vector<LibraryManagerRow> rows) {
        rows_ = std::move(rows);
    }

    [[nodiscard]] LibraryManagerRow* rowAtMutable(int index) {
        if (index < 0
            || static_cast<std::size_t>(index) >= rows_.size()) {
            return nullptr;
        }
        return &rows_[static_cast<std::size_t>(index)];
    }

    void setPreviewHandler(PreviewHandler handler) {
        preview_handler_ = std::move(handler);
    }

    void setFavoriteHandler(FavoriteHandler handler) {
        favorite_handler_ = std::move(handler);
    }

    void setSelectionHandler(SelectionHandler handler) {
        selection_handler_ = std::move(handler);
    }

    void setSortHandler(SortHandler handler) {
        sort_handler_ = std::move(handler);
    }

    void setExpandHandler(RowHandler handler) {
        expand_handler_ = std::move(handler);
    }

    void setContextMenuHandler(RowHandler handler) {
        context_menu_handler_ = std::move(handler);
    }

    void setMiniPaintHandler(MiniPaintHandler handler) {
        mini_paint_handler_ = std::move(handler);
    }

    void setRecentUsedMode(bool enabled) {
        recent_used_mode_ = enabled;
    }

    [[nodiscard]] bool recentUsedMode() const {
        return recent_used_mode_;
    }

    [[nodiscard]] int sortColumnId() const {
        return sort_column_id_;
    }

    [[nodiscard]] bool sortForwards() const {
        return sort_forwards_;
    }

    [[nodiscard]] const LibraryManagerRow* rowAt(int index) const {
        if (index < 0
            || static_cast<std::size_t>(index) >= rows_.size()) {
            return nullptr;
        }
        return &rows_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] static juce::String cellText(
        const LibraryManagerRow& row,
        int column_id) {
        if (column_id == kName) {
            auto text = juce::String::fromUTF8(row.name.c_str());
            if (libraryManagerRowIsHeader(row)) {
                const auto mark =
                    row.expanded
                        ? juce::String::fromUTF8("▼ ")
                        : juce::String::fromUTF8("▶ ");
                return mark + text;
            }
            if (row.kind != LibraryManagerKind::Composite) {
                text += " (r"
                    + juce::String(static_cast<int>(row.revision))
                    + ")";
            }
            return text;
        }
        if (libraryManagerRowIsHeader(row)) {
            if (column_id == kTags) {
                return juce::String(static_cast<int>(row.revision))
                    + juce::String::fromUTF8(" 件");
            }
            return {};
        }
        if (column_id == kTags) {
            if (row.tags.empty()) {
                return juce::String::fromUTF8("（タグなし）");
            }
            juce::StringArray parts;
            for (const auto& tag : row.tags) {
                parts.add(juce::String::fromUTF8(tag.c_str()));
            }
            return parts.joinIntoString(" ");
        }
        if (column_id == kUpdated) {
            return formatLibraryManagerTime(row.updated_unix_seconds);
        }
        return {};
    }

    [[nodiscard]] static bool isTooltipColumn(int column_id) {
        return column_id == kName || column_id == kTags
            || column_id == kUpdated;
    }

    int getNumRows() override {
        return static_cast<int>(rows_.size());
    }

    void paintRowBackground(
        juce::Graphics& graphics,
        int row_number,
        int width,
        int height,
        bool row_is_selected) override {
        juce::ignoreUnused(width);
        if (row_is_selected) {
            graphics.fillAll(juce::Colour(0xFF34404A));
        } else if (row_number % 2 == 0) {
            graphics.fillAll(juce::Colour(0xFF232B34));
        } else {
            graphics.fillAll(juce::Colour(0xFF1E252D));
        }
        graphics.setColour(juce::Colour(UiLayout::panelStroke));
        graphics.drawHorizontalLine(
            height - 1, 0.0F, static_cast<float>(width));
    }

    void paintCell(
        juce::Graphics& graphics,
        int row_number,
        int column_id,
        int width,
        int height,
        bool row_is_selected) override {
        juce::ignoreUnused(row_is_selected);
        const auto* row = rowAt(row_number);
        if (row == nullptr) {
            return;
        }
        graphics.setFont(UiFonts::body());
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        if (libraryManagerRowIsHeader(*row)) {
            if (column_id == kName) {
                graphics.setColour(juce::Colour(0xFFF2F4F5));
                graphics.setFont(UiFonts::body().withHeight(
                    UiFonts::controlTextHeight(
                        static_cast<float>(UiLayout::textButtonH))));
            } else if (column_id != kTags) {
                return;
            }
        }
        if (column_id == kMini) {
            if (mini_paint_handler_ && libraryManagerRowIsBody(*row)) {
                mini_paint_handler_(
                    graphics,
                    juce::Rectangle<float>(
                        1.0F,
                        1.0F,
                        static_cast<float>(width - 2),
                        static_cast<float>(height - 2)),
                    *row);
            }
            return;
        }
        if (column_id == kPreview) {
            if (libraryManagerRowIsHeader(*row)) {
                return;
            }
            if (auto icon = mgstc::app::makeEditorIcon(
                    mgstc::app::EditorIcon::Audition,
                    juce::Colour(0xFF9AA8B5))) {
                const int icon_size =
                    juce::jmin(width, height) - UiLayout::xs * 2;
                icon->drawWithin(
                    graphics,
                    juce::Rectangle<float>(
                        static_cast<float>((width - icon_size) / 2),
                        static_cast<float>((height - icon_size) / 2),
                        static_cast<float>(icon_size),
                        static_cast<float>(icon_size)),
                    juce::RectanglePlacement::centred,
                    1.0F);
            }
            return;
        }
        if (column_id == kFavorite) {
            if (libraryManagerRowIsHeader(*row)) {
                return;
            }
            graphics.setColour(
                row->favorite ? juce::Colour(0xFFFFD866)
                              : juce::Colour(0xFF68737E));
            graphics.drawText(
                juce::String::fromUTF8("★"),
                0,
                0,
                width,
                height,
                juce::Justification::centred,
                false);
            return;
        }
        const auto text = cellText(*row, column_id);
        const int indent = row->role == LibraryManagerRowRole::Member
            ? UiLayout::md
            : 0;
        graphics.drawText(
            text,
            UiLayout::xs + indent,
            0,
            width - UiLayout::xs * 2 - indent,
            height,
            juce::Justification::centredLeft,
            true);
    }

    void sortOrderChanged(
        int new_sort_column_id,
        bool is_forwards) override {
        if (recent_used_mode_) {
            if (table_ != nullptr) {
                table_->getHeader().setSortColumnId(0, true);
            }
            return;
        }
        if (new_sort_column_id == 0
            || new_sort_column_id == kPreview
            || new_sort_column_id == kMini) {
            return;
        }
        sort_column_id_ = new_sort_column_id;
        sort_forwards_ = is_forwards;
        if (sort_handler_) {
            sort_handler_();
        }
    }

    void cellClicked(
        int row_number,
        int column_id,
        const juce::MouseEvent& event) override {
        const auto* row = rowAt(row_number);
        if (row == nullptr) {
            return;
        }
        if (event.mods.isPopupMenu()) {
            if (context_menu_handler_) {
                context_menu_handler_(*row);
            }
            return;
        }
        if (libraryManagerRowIsHeader(*row)) {
            if (expand_handler_) {
                expand_handler_(*row);
            }
            return;
        }
        if (column_id == kPreview && preview_handler_) {
            preview_handler_(*row);
            return;
        }
        if (column_id == kFavorite && favorite_handler_) {
            if (!favoriteGlyphContains(row_number, event)) {
                return;
            }
            favorite_handler_(row_number, *row);
        }
    }

    void selectedRowsChanged(int) override {
        if (selection_handler_) {
            selection_handler_();
        }
    }

    juce::String getCellTooltip(
        int row_number,
        int column_id) override {
        if (!isTooltipColumn(column_id)) {
            return {};
        }
        const auto* row = rowAt(row_number);
        if (row == nullptr || table_ == nullptr) {
            return {};
        }
        const auto text = cellText(*row, column_id);
        const int column_width =
            table_->getHeader().getColumnWidth(column_id);
        const int available_width =
            column_width - UiLayout::xs * 2;
        if (UiLayout::isTextTruncated(
                UiFonts::body(), text, available_width)) {
            return text;
        }
        return {};
    }

private:
    [[nodiscard]] bool favoriteGlyphContains(
        int row_number,
        const juce::MouseEvent& event) const {
        if (table_ == nullptr) {
            return false;
        }
        const int cell_w =
            table_->getHeader().getColumnWidth(kFavorite);
        const int cell_h = table_->getRowHeight();
        if (cell_w <= 0 || cell_h <= 0) {
            return false;
        }
        const auto font = UiFonts::body();
        const auto star = juce::String::fromUTF8("★");
        const float glyph_w = static_cast<float>(
            juce::GlyphArrangement::getStringWidthInt(font, star));
        const float glyph_h = font.getHeight();
        const auto glyph = juce::Rectangle<float>(
                               (static_cast<float>(cell_w) - glyph_w)
                                   * 0.5F,
                               (static_cast<float>(cell_h) - glyph_h)
                                   * 0.5F,
                               glyph_w,
                               glyph_h)
                               .expanded(1.0F);
        auto local = event.position;
        const auto cell = table_->getCellPosition(
            kFavorite, row_number, true);
        if (!cell.isEmpty()) {
            const auto table_pos =
                event.getEventRelativeTo(table_).getPosition();
            if (cell.contains(table_pos)) {
                local = (table_pos - cell.getPosition()).toFloat();
            }
        }
        return glyph.contains(local);
    }

    juce::TableListBox* table_{};
    std::vector<LibraryManagerRow> rows_;
    PreviewHandler preview_handler_;
    FavoriteHandler favorite_handler_;
    SelectionHandler selection_handler_;
    SortHandler sort_handler_;
    RowHandler expand_handler_;
    RowHandler context_menu_handler_;
    MiniPaintHandler mini_paint_handler_;
    bool recent_used_mode_{};
    int sort_column_id_{kName};
    bool sort_forwards_{true};
};

class LibraryManagerListTab final
    : public juce::Component,
      public juce::DragAndDropContainer {
public:
    using PreviewCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using OpenEditorCallback =
        std::function<void(LibraryManagerKind, std::uint64_t)>;
    using PersistCallback = std::function<bool()>;
    using ReloadCallback = std::function<bool()>;
    using PerformanceTargetChangedCallback = std::function<void()>;
    using LibrariesChangedCallback = std::function<void()>;

    LibraryManagerListTab(
        LibraryManagerKind initial_kind,
        PreviewCallback preview,
        OpenEditorCallback open_editor,
        ReloadCallback reload,
        PersistCallback persist,
        std::function<mgstc::engine::TimbreLibrary*()> timbres,
        std::function<mgstc::engine::CompositeTimbreLibrary*()>
            composites,
        PerformanceTargetChangedCallback
            performance_target_changed = {},
        LibrariesChangedCallback libraries_changed = {},
        juce::InterProcessLock* shared_library_lock = nullptr,
        std::function<std::vector<mgstc::engine::ToneSetRecord>*()>
            tone_sets = {},
        std::function<std::vector<mgstc::engine::ToneSetMemberRecord>*()>
            tone_members = {},
        std::function<bool()> persist_sets = {})
        : preview_(std::move(preview)),
          open_editor_(std::move(open_editor)),
          reload_(std::move(reload)),
          persist_(std::move(persist)),
          timbres_(std::move(timbres)),
          composites_(std::move(composites)),
          performance_target_changed_(
              std::move(performance_target_changed)),
          libraries_changed_(std::move(libraries_changed)),
          shared_library_lock_(shared_library_lock),
          tone_sets_(std::move(tone_sets)),
          tone_members_(std::move(tone_members)),
          persist_sets_(std::move(persist_sets)),
          table_(*this),
          tooltip_window_(this, 450) {
        category_.addItem(juce::String::fromUTF8("すべて"), 4);
        category_.addItem(juce::String::fromUTF8("SCC"), 1);
        category_.addItem(juce::String::fromUTF8("OPLL"), 2);
        category_.addItem(juce::String::fromUTF8("複合"), 3);
        const int initial_id =
            initial_kind == LibraryManagerKind::Opll
                ? 2
            : initial_kind == LibraryManagerKind::Composite ? 3
                                                           : 1;
        category_.setSelectedId(initial_id, juce::dontSendNotification);
        category_.onChange = [this] { refreshTable(true); };
        addAndMakeVisible(category_);

        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { refreshTable(true); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);

        tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        tag_filter_.setTooltip(
            juce::String::fromUTF8(
                "保存済み音色で使われているタグを複数選択します"
                "（すべて含むAND）"));
        tag_filter_.onClick = [this] { showTagFilter(); };
        addAndMakeVisible(tag_filter_);

        favorite_only_.setButtonText(
            juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshTable(true); };
        addAndMakeVisible(favorite_only_);

        recent_used_.setButtonText(
            juce::String::fromUTF8("最近使った順"));
        recent_used_.setLookAndFeel(&switch_look_and_feel_);
        recent_used_.setTooltip(
            juce::String::fromUTF8(
                "ONのあいだは最終使用日時で並べ替え、"
                "列ヘッダでの昇順／降順は無効になります"));
        recent_used_.onClick = [this] { applyRecentUsedMode(); };
        addAndMakeVisible(recent_used_);

        tree_mode_.setButtonText(juce::String::fromUTF8("ツリー"));
        tree_mode_.setToggleState(true, juce::dontSendNotification);
        tree_mode_.setTooltip(juce::String::fromUTF8(
            "セットごとにまとめます。OFF で音色だけの一覧に戻します"));
        tree_mode_.onClick = [this] { refreshTable(false); };
        addAndMakeVisible(tree_mode_);

        new_set_.setButtonText(juce::String::fromUTF8("セット"));
        new_set_.setTooltip(juce::String::fromUTF8(
            "空のセットを追加します。音色自体は作りません"));
        new_set_.onClick = [this] { createSet(); };
        addAndMakeVisible(new_set_);

        detail_toggle_.setButtonText(juce::String::fromUTF8("詳細"));
        detail_toggle_.setTooltip(juce::String::fromUTF8(
            "右の詳細ペインを開閉します"));
        detail_toggle_.onClick = [this] {
            detail_open_ = !detail_open_;
            resized();
        };
        addAndMakeVisible(detail_toggle_);

        table_model_.setTable(&table_);
        table_model_.setPreviewHandler(
            [this](const LibraryManagerRow& row) {
                if (preview_ && libraryManagerRowIsBody(row)) {
                    preview_(row.kind, row.id);
                }
            });
        table_model_.setFavoriteHandler(
            [this](int row_number, const LibraryManagerRow& row) {
                if (!libraryManagerRowIsBody(row)) {
                    return;
                }
                toggleFavoriteAt(row_number, row);
            });
        table_model_.setSortHandler(
            [this] { refreshTable(false); });
        table_model_.setExpandHandler(
            [this](const LibraryManagerRow& row) { toggleExpanded(row); });
        table_model_.setContextMenuHandler(
            [this](const LibraryManagerRow& row) {
                showRowContextMenu(row);
            });
        table_model_.setMiniPaintHandler(
            [this](
                juce::Graphics& graphics,
                juce::Rectangle<float> area,
                const LibraryManagerRow& row) {
                paintRowMiniPreview(graphics, area, row);
            });
        table_.setModel(&table_model_);
        table_.setMultipleSelectionEnabled(true);
        auto& header = table_.getHeader();
        header.addColumn(
            juce::String{},
            LibraryManagerTableModel::kPreview,
            34,
            28,
            -1,
            juce::TableHeaderComponent::notSortable);
        header.addColumn(
            juce::String::fromUTF8("波形"),
            LibraryManagerTableModel::kMini,
            72,
            56,
            96,
            juce::TableHeaderComponent::notSortable);
        header.addColumn(
            juce::String::fromUTF8("名前"),
            LibraryManagerTableModel::kName,
            200);
        header.addColumn(
            juce::String::fromUTF8("タグ"),
            LibraryManagerTableModel::kTags,
            160);
        header.addColumn(
            juce::String::fromUTF8("更新日時"),
            LibraryManagerTableModel::kUpdated,
            130);
        header.addColumn(
            juce::String::fromUTF8("★"),
            LibraryManagerTableModel::kFavorite,
            36);
        header.setStretchToFitActive(true);
        header.setSortColumnId(
            LibraryManagerTableModel::kName, true);
        table_model_.setSelectionHandler([this] { updateDetail(); });
        table_.setColour(
            juce::ListBox::backgroundColourId,
            juce::Colour(UiLayout::panelFill));
        table_.setColour(
            juce::ListBox::outlineColourId,
            juce::Colours::transparentBlack);
        addAndMakeVisible(table_);

        detail_viewport_.setViewedComponent(&detail_host_, false);
        detail_viewport_.setScrollBarsShown(true, false);
        detail_viewport_.setScrollOnDragMode(
            juce::Viewport::ScrollOnDragMode::never);
        addAndMakeVisible(detail_viewport_);

        configureDetailEditable(detail_name_, false);
        detail_name_.setTooltip(
            juce::String::fromUTF8("音色名（空にはできません）"));
        detail_name_.onReturnKey = [this] { commitDetailName(); };
        detail_name_.onFocusLost = [this] { commitDetailName(); };
        detail_host_.addAndMakeVisible(detail_name_);
        detail_category_.setFont(UiFonts::body());
        detail_category_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        detail_host_.addAndMakeVisible(detail_category_);
        detail_favorite_.setFont(UiFonts::body());
        detail_favorite_.setInterceptsMouseClicks(true, false);
        detail_favorite_.setMouseCursor(
            juce::MouseCursor::PointingHandCursor);
        detail_favorite_.addMouseListener(this, false);
        detail_host_.addAndMakeVisible(detail_favorite_);
        detail_tags_empty_.setFont(UiFonts::body());
        detail_tags_empty_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        detail_tags_empty_.setText(
            juce::String::fromUTF8("（タグなし）"),
            juce::dontSendNotification);
        detail_host_.addAndMakeVisible(detail_tags_empty_);
        detail_host_.addAndMakeVisible(detail_tags_host_);
        configureDetailEditable(detail_memo_, true);
        detail_memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        detail_memo_.onFocusLost = [this] { commitDetailMemo(); };
        detail_memo_.onTextChange = [this] {
            if (!syncing_detail_) {
                layoutDetailHost();
            }
        };
        detail_host_.addAndMakeVisible(detail_memo_);

        duplicate_.setButtonText(juce::String::fromUTF8("複製"));
        duplicate_.onClick = [this] { duplicateSelected(); };
        addAndMakeVisible(duplicate_);
        remove_.setButtonText(
            juce::String::fromUTF8("選択を削除"));
        remove_.onClick = [this] { deleteSelected(); };
        addAndMakeVisible(remove_);
        assign_tags_.setButtonText(
            juce::String::fromUTF8("タグを付与"));
        assign_tags_.onClick = [this] { assignTagsToSelected(); };
        addAndMakeVisible(assign_tags_);
        load_edit_.setButtonText(
            juce::String::fromUTF8("読込して編集"));
        load_edit_.onClick = [this] { loadSelectedForEdit(); };
        addAndMakeVisible(load_edit_);
        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] { closeParentDialog(); };
        addAndMakeVisible(close_);
        replace_.setButtonText(juce::String::fromUTF8("差し替え"));
        replace_.setTooltip(juce::String::fromUTF8(
            "選択した音色を、開いたチャンネルの基本音色へコピーします"));
        replace_.onClick = [this] { replaceSelected(); };
        replace_.setVisible(false);
        addAndMakeVisible(replace_);

        refreshTable(true);
    }

    ~LibraryManagerListTab() override {
        favorite_only_.setLookAndFeel(nullptr);
        recent_used_.setLookAndFeel(nullptr);
    }

    void reloadFromParent() {
        refreshTable(true);
    }

    void setExportHandler(
        std::function<void(const std::vector<LibraryManagerRow>&)> handler) {
        export_add_ = std::move(handler);
    }

    void setReplaceMode(
        std::optional<LibraryManagerKind> kind,
        std::function<void(std::uint64_t)> on_replace) {
        replace_mode_ = kind.has_value();
        on_replace_ = std::move(on_replace);
        if (kind == LibraryManagerKind::Opll) {
            category_.setSelectedId(2, juce::dontSendNotification);
        } else if (kind == LibraryManagerKind::Scc) {
            category_.setSelectedId(1, juce::dontSendNotification);
        }
        category_.setEnabled(!replace_mode_);
        tree_mode_.setEnabled(!replace_mode_);
        if (replace_mode_) {
            tree_mode_.setToggleState(false, juce::dontSendNotification);
        }
        new_set_.setVisible(!replace_mode_);
        duplicate_.setVisible(!replace_mode_);
        remove_.setVisible(!replace_mode_);
        assign_tags_.setVisible(!replace_mode_);
        load_edit_.setVisible(!replace_mode_);
        replace_.setVisible(replace_mode_);
        setDetailFieldsEditable(!replace_mode_);
        refreshTable(true);
        resized();
    }

    [[nodiscard]] std::optional<LibraryManagerRow>
    performanceTarget() const {
        if (detail_row_ && libraryManagerRowIsBody(*detail_row_)) {
            return detail_row_;
        }
        const auto selected = selectedRows();
        for (const auto& row : selected) {
            if (libraryManagerRowIsBody(row)) {
                return row;
            }
        }
        return std::nullopt;
    }

    void paint(juce::Graphics& graphics) override {
        // 塗りだけ。枠線はリストの矩形塗りに角を隠されないよう
        // paintOverChildren で重ねる。
        if (!table_bounds_.isEmpty()) {
            fillRoundedPanelFrame(graphics, table_bounds_);
        }
        if (!detail_bounds_.isEmpty()) {
            fillRoundedPanelFrame(graphics, detail_bounds_);
        }
    }

    void paintOverChildren(juce::Graphics& graphics) override {
        if (!table_bounds_.isEmpty()) {
            strokeRoundedPanelFrame(graphics, table_bounds_);
        }
        if (!detail_bounds_.isEmpty()) {
            strokeRoundedPanelFrame(graphics, detail_bounds_);
            graphics.setColour(juce::Colour(UiLayout::panelStroke));
            graphics.fillRect(splitterBounds());
        }
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto controls = area.removeFromTop(fieldH);
        category_.setBounds(controls.removeFromLeft(110));
        controls.removeFromLeft(controlGap);
        recent_used_.setBounds(controls.removeFromRight(150));
        controls.removeFromRight(controlGap);
        favorite_only_.setBounds(controls.removeFromRight(100));
        controls.removeFromRight(controlGap);
        const int tag_w = juce::jlimit(
            libraryManageButtonW,
            220,
            juce::jmax(libraryManageButtonW, controls.getWidth() / 2));
        tag_filter_.setBounds(controls.removeFromRight(tag_w));
        controls.removeFromRight(controlGap);
        filter_.setBounds(controls);
        area.removeFromTop(sm);

        auto buttons = area.removeFromBottom(textButtonH);
        tree_mode_.setBounds(buttons.removeFromLeft(libraryButtonMinW));
        buttons.removeFromLeft(controlGap);
        new_set_.setBounds(buttons.removeFromLeft(libraryButtonMinW));
        buttons.removeFromLeft(controlGap);
        detail_toggle_.setBounds(buttons.removeFromLeft(libraryButtonMinW));
        buttons.removeFromLeft(controlGap);
        close_.setBounds(buttons.removeFromRight(libraryButtonMinW));
        buttons.removeFromRight(controlGap);
        if (replace_.isVisible()) {
            replace_.setBounds(
                buttons.removeFromRight(libraryManageButtonW));
        } else {
            load_edit_.setBounds(
                buttons.removeFromRight(libraryManageButtonW + xs));
            buttons.removeFromRight(controlGap);
            assign_tags_.setBounds(
                buttons.removeFromRight(libraryManageButtonW));
            buttons.removeFromRight(controlGap);
            remove_.setBounds(
                buttons.removeFromRight(libraryManageButtonW));
            buttons.removeFromRight(controlGap);
            duplicate_.setBounds(
                buttons.removeFromRight(libraryButtonMinW));
        }
        area.removeFromBottom(sm);

        detail_bounds_ = {};
        if (detail_open_) {
            detail_width_ = juce::jlimit(200, 480, detail_width_);
            detail_bounds_ = area.removeFromRight(detail_width_);
            area.removeFromRight(sm);
        }
        table_bounds_ = area;
        table_.setBounds(table_bounds_.reduced(1));

        detail_viewport_.setBounds(detail_bounds_.reduced(1));
        layoutDetailHost();
    }

    void mouseMove(const juce::MouseEvent& event) override {
        setMouseCursor(
            splitterHit(event.getPosition())
                ? juce::MouseCursor::LeftRightResizeCursor
                : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& event) override {
        resizing_detail_ = splitterHit(event.getPosition());
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (!resizing_detail_ || !detail_open_) {
            return;
        }
        const int right = getWidth() - UiLayout::panelPad;
        detail_width_ = juce::jlimit(200, 480, right - event.x);
        resized();
    }

    void mouseUp(const juce::MouseEvent& event) override {
        resizing_detail_ = false;
        if (event.eventComponent == &detail_favorite_
            && detail_row_.has_value()
            && libraryManagerRowIsBody(*detail_row_)) {
            const int index = indexOfDisplayedRow(*detail_row_);
            if (index >= 0) {
                toggleFavoriteAt(index, *detail_row_);
            }
        }
    }

private:
    static void configureDetailEditable(
        juce::TextEditor& editor,
        bool multiline) {
        editor.setMultiLine(multiline, true);
        editor.setReturnKeyStartsNewLine(multiline);
        editor.setReadOnly(false);
        editor.setCaretVisible(true);
        editor.setScrollbarsShown(false);
        editor.setPopupMenuEnabled(true);
        editor.setWantsKeyboardFocus(true);
        UiFonts::styleBodyField(editor);
    }

    void setDetailFieldsEditable(bool editable) {
        detail_name_.setReadOnly(!editable);
        detail_name_.setCaretVisible(editable);
        detail_name_.setEnabled(editable);
        detail_name_.setWantsKeyboardFocus(editable);
        detail_memo_.setReadOnly(!editable);
        detail_memo_.setCaretVisible(editable);
        detail_memo_.setEnabled(editable);
        detail_memo_.setWantsKeyboardFocus(editable);
    }

    void layoutDetailHost() {
        using namespace UiLayout;
        if (detail_bounds_.isEmpty()) {
            return;
        }
        const int host_w = juce::jmax(1, detail_viewport_.getWidth());
        const int inner_w = juce::jmax(1, host_w - panelPad * 2);
        int y = panelPad;

        detail_name_.setBounds(panelPad, y, inner_w, fieldH);
        y += fieldH + xs;
        detail_category_.setBounds(
            panelPad, y, inner_w, libraryTitleH - xs);
        y += libraryTitleH - xs + xs;
        detail_favorite_.setBounds(
            panelPad, y, inner_w, libraryTitleH - xs);
        y += libraryTitleH - xs + sm;

        const auto font = UiFonts::body();
        const int line_h = juce::jmax(
            18, juce::roundToInt(font.getHeight()) + 4);
        if (detail_tags_empty_.isVisible()) {
            detail_tags_empty_.setBounds(panelPad, y, inner_w, line_h);
            y += line_h + xs;
        } else {
            juce::Array<juce::Component*> views;
            for (auto& chip : detail_tag_chips_) {
                views.add(chip.get());
            }
            layoutTagChipsFlow(
                detail_tags_host_, views, inner_w, 28, controlGap);
            detail_tags_host_.setBounds(
                panelPad,
                y,
                inner_w,
                detail_tags_host_.getHeight());
            y += detail_tags_host_.getHeight() + xs;
        }

        juce::AttributedString memo_text;
        memo_text.append(
            detail_memo_.getText().isNotEmpty()
                ? detail_memo_.getText()
                : juce::String::fromUTF8("メモ"),
            font,
            juce::Colour(0xFFB9C6D2));
        juce::TextLayout memo_layout;
        memo_layout.createLayout(
            memo_text, static_cast<float>(inner_w));
        const int memo_h = juce::jmax(
            fieldH * 2,
            juce::roundToInt(std::ceil(memo_layout.getHeight())) + 8);
        detail_memo_.setBounds(panelPad, y, inner_w, memo_h);
        y += memo_h + panelPad;

        detail_host_.setSize(host_w, y);
    }

    void applyRecentUsedMode() {
        const bool recent = recent_used_.getToggleState();
        table_model_.setRecentUsedMode(recent);
        if (recent) {
            table_.getHeader().setSortColumnId(0, true);
        } else {
            table_.getHeader().setSortColumnId(
                table_model_.sortColumnId() == 0
                    ? LibraryManagerTableModel::kName
                    : table_model_.sortColumnId(),
                table_model_.sortForwards());
        }
        refreshTable(false);
    }

    [[nodiscard]] int indexOfDisplayedRow(
        const LibraryManagerRow& row) const {
        for (std::size_t i = 0; i < displayed_rows_.size(); ++i) {
            if (displayed_rows_[i].role == row.role
                && displayed_rows_[i].set_id == row.set_id
                && displayed_rows_[i].kind == row.kind
                && displayed_rows_[i].id == row.id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] bool showsAllCategories() const {
        return category_.getSelectedId() == 4;
    }

    [[nodiscard]] bool categoryAccepts(LibraryManagerKind kind) const {
        if (showsAllCategories()) {
            return true;
        }
        return kind == currentKind();
    }

    [[nodiscard]] LibraryManagerKind currentKind() const {
        switch (category_.getSelectedId()) {
        case 2:
            return LibraryManagerKind::Opll;
        case 3:
            return LibraryManagerKind::Composite;
        case 1:
            return LibraryManagerKind::Scc;
        default:
            return LibraryManagerKind::Scc;
        }
    }

    [[nodiscard]] bool treeMode() const {
        return tree_mode_.getToggleState() && !replace_mode_;
    }

    [[nodiscard]] juce::Rectangle<int> splitterBounds() const {
        if (!detail_open_ || detail_bounds_.isEmpty()) {
            return {};
        }
        return {
            detail_bounds_.getX() - UiLayout::sm,
            detail_bounds_.getY(),
            UiLayout::sm,
            detail_bounds_.getHeight()};
    }

    [[nodiscard]] bool splitterHit(juce::Point<int> position) const {
        return splitterBounds().expanded(2, 0).contains(position);
    }

    [[nodiscard]] static int compareRows(
        const LibraryManagerRow& left,
        const LibraryManagerRow& right,
        bool recent_used,
        int sort_column_id,
        bool sort_forwards) {
        auto finish = [sort_forwards](int cmp) {
            return sort_forwards ? cmp : -cmp;
        };
        if (recent_used) {
            if (left.last_used_unix_seconds
                != right.last_used_unix_seconds) {
                return left.last_used_unix_seconds
                        > right.last_used_unix_seconds
                    ? -1
                    : 1;
            }
            return left.name < right.name
                ? -1
                : (left.name > right.name ? 1 : 0);
        }
        switch (sort_column_id) {
        case LibraryManagerTableModel::kTags: {
            const auto left_tags =
                mgstc::engine::serializeTimbreTags(left.tags);
            const auto right_tags =
                mgstc::engine::serializeTimbreTags(right.tags);
            if (left_tags != right_tags) {
                return finish(left_tags < right_tags ? -1 : 1);
            }
            break;
        }
        case LibraryManagerTableModel::kUpdated:
            if (left.updated_unix_seconds
                != right.updated_unix_seconds) {
                return finish(
                    left.updated_unix_seconds
                            < right.updated_unix_seconds
                        ? -1
                        : 1);
            }
            break;
        case LibraryManagerTableModel::kFavorite:
            if (left.favorite != right.favorite) {
                return finish(left.favorite ? -1 : 1);
            }
            break;
        case LibraryManagerTableModel::kName:
        default:
            if (left.name != right.name) {
                return finish(left.name < right.name ? -1 : 1);
            }
            break;
        }
        return 0;
    }

    [[nodiscard]] std::vector<std::vector<std::string>>
    currentCategoryTagSets() const {
        std::vector<std::vector<std::string>> tag_sets;
        const auto kind = currentKind();
        if (showsAllCategories()
            || kind == LibraryManagerKind::Composite) {
            const auto* library = composites_();
            if (library == nullptr) {
                return tag_sets;
            }
            for (const auto& entry : library->entries()) {
                tag_sets.push_back(entry.timbre.tags);
            }
            if (!showsAllCategories()) {
                return tag_sets;
            }
        }
        if (!showsAllCategories()
            && kind == LibraryManagerKind::Composite) {
            return tag_sets;
        }
        const auto* library = timbres_();
        if (library == nullptr) {
            return tag_sets;
        }
        const auto category = kind == LibraryManagerKind::Scc
            ? mgstc::engine::TimbreCategory::Scc
            : mgstc::engine::TimbreCategory::Opll;
        for (const auto& entry : library->entries()) {
            if (showsAllCategories() || entry.category == category) {
                tag_sets.push_back(entry.tags);
            }
        }
        return tag_sets;
    }

    void updateTagFilterButton() {
        tag_filter_.setButtonText(tagSelectionSummary(
            selected_filter_tags_,
            juce::String::fromUTF8("タグで絞り込み")));
    }

    void showTagFilter() {
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("ライブラリのタグ検索"),
            filterTagChoices(
                currentCategoryTagSets(), selected_filter_tags_),
            selected_filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe == nullptr) {
                    return;
                }
                safe->selected_filter_tags_ = std::move(selected);
                safe->updateTagFilterButton();
                safe->refreshTable(true);
            });
    }

    [[nodiscard]] bool textMatches(
        const juce::String& name,
        const juce::String& tags,
        const juce::String& memo,
        const juce::String& filter) const {
        return filter.isEmpty()
            || name.containsIgnoreCase(filter)
            || tags.containsIgnoreCase(filter)
            || memo.containsIgnoreCase(filter);
    }

    [[nodiscard]] bool setIsExpanded(std::uint64_t set_id) const {
        return std::find(
                   expanded_sets_.begin(),
                   expanded_sets_.end(),
                   set_id)
            != expanded_sets_.end();
    }

    [[nodiscard]] std::vector<LibraryManagerRow> buildTreeRows() const {
        std::vector<LibraryManagerRow> rows;
        if (!tone_sets_ || !tone_members_) {
            return rows;
        }
        const auto* sets = tone_sets_();
        const auto* members = tone_members_();
        const auto* timbres = timbres_();
        const auto* composites = composites_();
        if (sets == nullptr || members == nullptr) {
            return rows;
        }
        if (!expansion_ready_) {
            for (const auto& set : *sets) {
                expanded_sets_.push_back(set.id);
            }
            expansion_ready_ = true;
        }
        const auto filter = filter_.getText().trim();
        const bool favorites_only = favorite_only_.getToggleState();
        const bool recent_used = recent_used_.getToggleState();
        const int sort_column_id = table_model_.sortColumnId();
        const bool sort_forwards = table_model_.sortForwards();
        const bool searching = filter.isNotEmpty()
            || favorites_only
            || !selected_filter_tags_.empty();

        auto accept_body = [&](LibraryManagerKind kind,
                                bool favorite,
                                const std::vector<std::string>& tags,
                                const std::string& name,
                                const std::string& memo) {
            if (!categoryAccepts(kind)) {
                return false;
            }
            if (favorites_only && !favorite) {
                return false;
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    tags, selected_filter_tags_)) {
                return false;
            }
            return textMatches(
                juce::String::fromUTF8(name.c_str()),
                juce::String::fromUTF8(
                    mgstc::engine::serializeTimbreTags(tags).c_str()),
                juce::String::fromUTF8(memo.c_str()),
                filter);
        };

        std::vector<mgstc::engine::ToneSetRecord> ordered = *sets;
        std::stable_sort(
            ordered.begin(),
            ordered.end(),
            [](const auto& left, const auto& right) {
                const auto left_name =
                    juce::String::fromUTF8(left.name.c_str());
                const auto right_name =
                    juce::String::fromUTF8(right.name.c_str());
                return left_name.compareIgnoreCase(right_name) < 0;
            });

        auto append_member = [&](std::uint64_t set_id,
                                  bool composite,
                                  std::uint64_t body_id) {
            if (composite) {
                const auto* entry =
                    composites ? composites->find(body_id) : nullptr;
                if (entry == nullptr
                    || !accept_body(
                        LibraryManagerKind::Composite,
                        entry->timbre.favorite,
                        entry->timbre.tags,
                        entry->timbre.name,
                        entry->timbre.memo)) {
                    return false;
                }
                LibraryManagerRow row{
                    LibraryManagerKind::Composite,
                    entry->id,
                    entry->timbre.name,
                    entry->timbre.tags,
                    entry->timbre.memo,
                    entry->timbre.favorite,
                    entry->revision,
                    entry->updated_unix_seconds,
                    entry->last_used_unix_seconds};
                row.role = LibraryManagerRowRole::Member;
                row.set_id = set_id;
                rows.push_back(std::move(row));
                return true;
            }
            const auto* entry = timbres ? timbres->find(body_id) : nullptr;
            if (entry == nullptr) {
                return false;
            }
            const auto kind = entry->category
                    == mgstc::engine::TimbreCategory::Scc
                ? LibraryManagerKind::Scc
                : LibraryManagerKind::Opll;
            if (!accept_body(
                    kind,
                    entry->favorite,
                    entry->tags,
                    entry->name,
                    entry->memo)) {
                return false;
            }
            LibraryManagerRow row{
                kind,
                entry->id,
                entry->name,
                entry->tags,
                entry->memo,
                entry->favorite,
                entry->revision,
                entry->updated_unix_seconds,
                entry->last_used_unix_seconds};
            row.role = LibraryManagerRowRole::Member;
            row.set_id = set_id;
            rows.push_back(std::move(row));
            return true;
        };

        for (const auto& set : ordered) {
            std::vector<mgstc::engine::ToneSetMemberRecord> owned;
            for (const auto& member : *members) {
                if (member.set_id == set.id) {
                    owned.push_back(member);
                }
            }
            std::stable_sort(
                owned.begin(),
                owned.end(),
                [](const auto& left, const auto& right) {
                    return left.position < right.position;
                });
            const auto saved_size = rows.size();
            const bool open = searching || setIsExpanded(set.id);
            if (open) {
                for (const auto& member : owned) {
                    if (member.composite_id != 0) {
                        append_member(set.id, true, member.composite_id);
                    } else if (member.timbre_id != 0) {
                        append_member(set.id, false, member.timbre_id);
                    }
                }
            }
            const auto visible = static_cast<std::uint32_t>(
                rows.size() - saved_size);
            const bool name_matches = textMatches(
                juce::String::fromUTF8(set.name.c_str()),
                {},
                juce::String::fromUTF8(set.memo.c_str()),
                filter);
            if (searching && visible == 0 && !name_matches) {
                continue;
            }
            LibraryManagerRow header;
            header.role = LibraryManagerRowRole::SetHeader;
            header.id = set.id;
            header.set_id = set.id;
            header.name = set.name;
            header.memo = set.memo;
            header.revision = open
                ? visible
                : static_cast<std::uint32_t>(owned.size());
            header.updated_unix_seconds = set.updated_unix_seconds;
            header.expanded = open;
            if (visible > 0) {
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(saved_size), header);
            } else {
                rows.push_back(header);
            }
        }

        std::vector<LibraryManagerRow> unassigned;
        auto consider = [&](LibraryManagerRow row) {
            const bool owned = std::any_of(
                members->begin(),
                members->end(),
                [&](const mgstc::engine::ToneSetMemberRecord& member) {
                    return row.kind == LibraryManagerKind::Composite
                        ? member.composite_id == row.id
                        : member.timbre_id == row.id;
                });
            if (owned) {
                return;
            }
            row.role = LibraryManagerRowRole::Timbre;
            unassigned.push_back(std::move(row));
        };
        if (composites != nullptr && categoryAccepts(LibraryManagerKind::Composite)) {
            for (const auto& entry : composites->entries()) {
                if (!accept_body(
                        LibraryManagerKind::Composite,
                        entry.timbre.favorite,
                        entry.timbre.tags,
                        entry.timbre.name,
                        entry.timbre.memo)) {
                    continue;
                }
                consider({
                    LibraryManagerKind::Composite,
                    entry.id,
                    entry.timbre.name,
                    entry.timbre.tags,
                    entry.timbre.memo,
                    entry.timbre.favorite,
                    entry.revision,
                    entry.updated_unix_seconds,
                    entry.last_used_unix_seconds});
            }
        }
        if (timbres != nullptr) {
            for (const auto& entry : timbres->entries()) {
                const auto kind = entry.category
                        == mgstc::engine::TimbreCategory::Scc
                    ? LibraryManagerKind::Scc
                    : LibraryManagerKind::Opll;
                if (!accept_body(
                        kind,
                        entry.favorite,
                        entry.tags,
                        entry.name,
                        entry.memo)) {
                    continue;
                }
                consider({
                    kind,
                    entry.id,
                    entry.name,
                    entry.tags,
                    entry.memo,
                    entry.favorite,
                    entry.revision,
                    entry.updated_unix_seconds,
                    entry.last_used_unix_seconds});
            }
        }
        std::stable_sort(
            unassigned.begin(),
            unassigned.end(),
            [&](const LibraryManagerRow& left, const LibraryManagerRow& right) {
                return compareRows(
                           left,
                           right,
                           recent_used,
                           sort_column_id,
                           sort_forwards)
                    < 0;
            });
        if (!searching || !unassigned.empty()) {
            LibraryManagerRow header;
            header.role = LibraryManagerRowRole::UnassignedHeader;
            header.name = "未所属";
            header.revision = static_cast<std::uint32_t>(unassigned.size());
            header.expanded = searching || unassigned_expanded_;
            rows.push_back(header);
            if (header.expanded) {
                rows.insert(rows.end(), unassigned.begin(), unassigned.end());
            }
        }
        return rows;
    }

    [[nodiscard]] std::vector<LibraryManagerRow> buildRows() const {
        if (treeMode()) {
            return buildTreeRows();
        }
        std::vector<LibraryManagerRow> rows;
        const auto kind = currentKind();
        const auto filter = filter_.getText().trim();
        const bool favorites_only = favorite_only_.getToggleState();
        const bool recent_used = recent_used_.getToggleState();
        const int sort_column_id = table_model_.sortColumnId();
        const bool sort_forwards = table_model_.sortForwards();
        if (showsAllCategories()
            || kind == LibraryManagerKind::Composite) {
            const auto* library = composites_();
            if (library == nullptr) {
                return rows;
            }
            for (const auto& entry : library->entries()) {
                if (favorites_only && !entry.timbre.favorite) {
                    continue;
                }
                if (!mgstc::engine::containsAllTimbreTags(
                        entry.timbre.tags, selected_filter_tags_)) {
                    continue;
                }
                if (filter.isNotEmpty()) {
                    const auto name =
                        juce::String::fromUTF8(entry.timbre.name.c_str());
                    const auto tags =
                        juce::String::fromUTF8(
                            mgstc::engine::serializeTimbreTags(
                                entry.timbre.tags)
                                .c_str());
                    const auto memo =
                        juce::String::fromUTF8(entry.timbre.memo.c_str());
                    if (!name.containsIgnoreCase(filter)
                        && !tags.containsIgnoreCase(filter)
                        && !memo.containsIgnoreCase(filter)) {
                        continue;
                    }
                }
                rows.push_back(
                    {LibraryManagerKind::Composite,
                     entry.id,
                     entry.timbre.name,
                     entry.timbre.tags,
                     entry.timbre.memo,
                     entry.timbre.favorite,
                     entry.revision,
                     entry.updated_unix_seconds,
                     entry.last_used_unix_seconds});
            }
        }
        if (showsAllCategories()
            || kind != LibraryManagerKind::Composite) {
            const auto* library = timbres_();
            if (library == nullptr) {
                return rows;
            }
            const auto category = kind == LibraryManagerKind::Scc
                ? mgstc::engine::TimbreCategory::Scc
                : mgstc::engine::TimbreCategory::Opll;
            for (const auto& entry : library->entries()) {
                if (!showsAllCategories() && entry.category != category) {
                    continue;
                }
                if (favorites_only && !entry.favorite) {
                    continue;
                }
                if (!mgstc::engine::containsAllTimbreTags(
                        entry.tags, selected_filter_tags_)) {
                    continue;
                }
                if (filter.isNotEmpty()) {
                    const auto name =
                        juce::String::fromUTF8(entry.name.c_str());
                    const auto tags =
                        juce::String::fromUTF8(
                            mgstc::engine::serializeTimbreTags(entry.tags)
                                .c_str());
                    const auto memo =
                        juce::String::fromUTF8(entry.memo.c_str());
                    if (!name.containsIgnoreCase(filter)
                        && !tags.containsIgnoreCase(filter)
                        && !memo.containsIgnoreCase(filter)) {
                        continue;
                    }
                }
                rows.push_back(
                    {kind,
                     entry.id,
                     entry.name,
                     entry.tags,
                     entry.memo,
                     entry.favorite,
                     entry.revision,
                     entry.updated_unix_seconds,
                     entry.last_used_unix_seconds});
            }
        }
        std::stable_sort(
            rows.begin(),
            rows.end(),
            [recent_used, sort_column_id, sort_forwards](
                const LibraryManagerRow& left,
                const LibraryManagerRow& right) {
                return compareRows(
                           left,
                           right,
                           recent_used,
                           sort_column_id,
                           sort_forwards)
                    < 0;
            });
        return rows;
    }

    void refreshTable(bool reload_from_disk) {
        if (reload_from_disk) {
            if (!reload_ || !reload_()) {
                return;
            }
        }
        const auto previously_selected = selectedRows();
        displayed_rows_ = buildRows();
        table_model_.setRows(displayed_rows_);
        table_.updateContent();
        if (!previously_selected.empty()) {
            juce::SparseSet<int> selection;
            for (int index = 0;
                 index < static_cast<int>(displayed_rows_.size());
                 ++index) {
                const auto& row = displayed_rows_[static_cast<std::size_t>(
                    index)];
                for (const auto& key : previously_selected) {
                    if (row.role == key.role
                        && row.set_id == key.set_id
                        && row.kind == key.kind
                        && row.id == key.id) {
                        selection.addRange({index, index + 1});
                        break;
                    }
                }
            }
            table_.setSelectedRows(selection, juce::dontSendNotification);
        }
        updateDetail();
        table_.repaint();
        repaint();
    }

    [[nodiscard]] std::vector<LibraryManagerRow> selectedRows() const {
        std::vector<LibraryManagerRow> selected;
        const auto& indices = table_.getSelectedRows();
        for (int i = 0; i < indices.size(); ++i) {
            const auto index = indices[i];
            if (const auto* row = table_model_.rowAt(index)) {
                selected.push_back(*row);
            }
        }
        return selected;
    }

    void updateDetail() {
        const auto previous = detail_row_;
        const auto selected = selectedRows();
        syncing_detail_ = true;
        if (selected.size() != 1) {
            detail_row_.reset();
            setDetailFieldsEditable(false);
            detail_name_.setText(
                selected.empty()
                    ? juce::String::fromUTF8("音色を選択してください")
                    : juce::String::fromUTF8("複数選択中")
                      + " ("
                      + juce::String(static_cast<int>(selected.size()))
                      + ")",
                juce::dontSendNotification);
            detail_category_.setText({}, juce::dontSendNotification);
            detail_favorite_.setText({}, juce::dontSendNotification);
            rebuildDetailTagChips();
            detail_memo_.setText({}, juce::dontSendNotification);
            syncing_detail_ = false;
            layoutDetailHost();
            detail_viewport_.setViewPosition(0, 0);
            notifyPerformanceTargetChanged(previous);
            return;
        }
        detail_row_ = selected.front();
        const auto& row = *detail_row_;
        if (row.role == LibraryManagerRowRole::UnassignedHeader) {
            setDetailFieldsEditable(false);
            detail_name_.setText(
                juce::String::fromUTF8("未所属"),
                juce::dontSendNotification);
            detail_category_.setText(
                juce::String::fromUTF8(
                    "どのセットにも入っていない音色です"),
                juce::dontSendNotification);
            detail_favorite_.setText({}, juce::dontSendNotification);
            rebuildDetailTagChips();
            detail_memo_.setText({}, juce::dontSendNotification);
            syncing_detail_ = false;
            layoutDetailHost();
            detail_viewport_.setViewPosition(0, 0);
            notifyPerformanceTargetChanged(previous);
            return;
        }
        if (row.role == LibraryManagerRowRole::SetHeader) {
            setDetailFieldsEditable(!replace_mode_);
            detail_name_.setText(
                juce::String::fromUTF8(row.name.c_str()),
                juce::dontSendNotification);
            detail_category_.setText(
                juce::String::fromUTF8("セット  ")
                    + juce::String(static_cast<int>(row.revision))
                    + juce::String::fromUTF8(" 件"),
                juce::dontSendNotification);
            detail_favorite_.setText({}, juce::dontSendNotification);
            rebuildDetailTagChips();
            detail_memo_.setText(
                juce::String::fromUTF8(row.memo.c_str()),
                juce::dontSendNotification);
            syncing_detail_ = false;
            layoutDetailHost();
            detail_viewport_.setViewPosition(0, 0);
            notifyPerformanceTargetChanged(previous);
            return;
        }
        setDetailFieldsEditable(!replace_mode_);
        detail_name_.setText(
            juce::String::fromUTF8(row.name.c_str()),
            juce::dontSendNotification);
        juce::String category;
        switch (row.kind) {
        case LibraryManagerKind::Scc:
            category = juce::String::fromUTF8("SCC");
            break;
        case LibraryManagerKind::Opll:
            category = juce::String::fromUTF8("OPLL");
            break;
        case LibraryManagerKind::Composite:
            category = juce::String::fromUTF8("複合");
            break;
        }
        if (row.kind != LibraryManagerKind::Composite) {
            category += juce::String::fromUTF8("  r")
                + juce::String(static_cast<int>(row.revision));
        }
        detail_category_.setText(category, juce::dontSendNotification);
        detail_favorite_.setText(
            row.favorite
                ? juce::String::fromUTF8("★ お気に入り")
                : juce::String::fromUTF8("☆ お気に入りに追加"),
            juce::dontSendNotification);
        detail_favorite_.setColour(
            juce::Label::textColourId,
            row.favorite ? juce::Colour(0xFFFFD866)
                         : juce::Colour(0xFFB9C6D2));
        rebuildDetailTagChips();
        detail_memo_.setText(
            juce::String::fromUTF8(row.memo.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
        layoutDetailHost();
        detail_viewport_.setViewPosition(0, 0);
        notifyPerformanceTargetChanged(previous);
    }

    void rebuildDetailTagChips() {
        detail_tags_host_.removeAllChildren();
        detail_tag_chips_.clear();
        if (!detail_row_) {
            detail_tags_empty_.setVisible(false);
            detail_tags_host_.setVisible(false);
            return;
        }
        if (detail_row_->tags.empty()) {
            detail_tags_empty_.setText(
                juce::String::fromUTF8("（タグなし）"),
                juce::dontSendNotification);
            detail_tags_empty_.setVisible(true);
            detail_tags_host_.setVisible(false);
            return;
        }
        detail_tags_empty_.setVisible(false);
        detail_tags_host_.setVisible(true);
        for (const auto& tag : detail_row_->tags) {
            const auto tag_copy = tag;
            auto chip = std::make_unique<RemovableDetailTagChip>(
                juce::String::fromUTF8(tag.c_str()),
                [this, tag_copy] { confirmRemoveDetailTag(tag_copy); });
            detail_tags_host_.addAndMakeVisible(*chip);
            detail_tag_chips_.push_back(std::move(chip));
        }
    }

    void confirmRemoveDetailTag(std::string tag) {
        if (!detail_row_) {
            return;
        }
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(
                    juce::String::fromUTF8("「")
                    + juce::String::fromUTF8(tag.c_str())
                    + juce::String::fromUTF8(
                        "」をこの音色から外しますか？"))
                .withButton(juce::String::fromUTF8("外す"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, tag = std::move(tag)](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                safe->removeDetailTag(tag);
            });
    }

    class ExclusiveLibraryGuard {
    public:
        explicit ExclusiveLibraryGuard(juce::InterProcessLock* lock)
            : lock_(
                lock != nullptr
                    ? std::make_unique<mgstc::app::ScopedLibraryIpcLock>(*lock)
                    : nullptr) {}

        [[nodiscard]] bool locked() const {
            return lock_ == nullptr || lock_->isLocked();
        }

    private:
        std::unique_ptr<mgstc::app::ScopedLibraryIpcLock> lock_;
    };

    void removeDetailTag(const std::string& tag) {
        if (!detail_row_ || !libraryManagerRowIsBody(*detail_row_)) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto& tags = entry->timbre.tags;
                tags.erase(
                    std::remove(tags.begin(), tags.end(), tag),
                    tags.end());
                ok = library->update(id, entry->timbre, now)
                    && persist_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.tags.erase(
                    std::remove(
                        updated.tags.begin(),
                        updated.tags.end(),
                        tag),
                    updated.tags.end());
                ok = library->update(id, updated, now)
                    && persist_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "タグを外した結果を保存できませんでした"));
            return;
        }
        refreshTable(false);
        notifyLibrariesChanged();
    }

    void restoreDetailNameField() {
        if (!detail_row_) {
            return;
        }
        syncing_detail_ = true;
        detail_name_.setText(
            juce::String::fromUTF8(detail_row_->name.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
    }

    void restoreDetailMemoField() {
        if (!detail_row_) {
            return;
        }
        syncing_detail_ = true;
        detail_memo_.setText(
            juce::String::fromUTF8(detail_row_->memo.c_str()),
            juce::dontSendNotification);
        syncing_detail_ = false;
        layoutDetailHost();
    }

    void commitDetailName() {
        if (syncing_detail_ || !detail_row_) {
            return;
        }
        const auto requested = utf8String(detail_name_.getText().trim());
        if (requested.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("新しい名前を入力してください"));
            restoreDetailNameField();
            return;
        }
        if (requested == detail_row_->name) {
            return;
        }
        if (detail_row_->role == LibraryManagerRowRole::SetHeader) {
            commitSetName(detail_row_->id, requested);
            return;
        }
        if (!libraryManagerRowIsBody(*detail_row_)) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            restoreDetailNameField();
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                entry->timbre.name = requested;
                ok = library->update(id, entry->timbre, now)
                    && persist_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.name = requested;
                ok = library->update(id, updated, now)
                    && persist_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "名前の変更を保存できませんでした"));
            restoreDetailNameField();
            return;
        }
        refreshTable(false);
        notifyLibrariesChanged();
    }

    void commitDetailMemo() {
        if (syncing_detail_ || !detail_row_) {
            return;
        }
        const auto requested = utf8String(detail_memo_.getText());
        if (requested == detail_row_->memo) {
            return;
        }
        if (detail_row_->role == LibraryManagerRowRole::SetHeader) {
            commitSetMemo(detail_row_->id, requested);
            return;
        }
        if (!libraryManagerRowIsBody(*detail_row_)) {
            return;
        }
        const auto kind = detail_row_->kind;
        const auto id = detail_row_->id;
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !reload_ || !reload_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "共有ライブラリを読み直せませんでした"));
            restoreDetailMemoField();
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        bool ok = false;
        if (kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                entry->timbre.memo = requested;
                ok = library->update(id, entry->timbre, now)
                    && persist_();
            }
        } else {
            auto* library = timbres_();
            auto* entry = library ? library->find(id) : nullptr;
            if (entry != nullptr) {
                auto updated = *entry;
                updated.memo = requested;
                ok = library->update(id, updated, now)
                    && persist_();
            }
        }
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "メモの変更を保存できませんでした"));
            restoreDetailMemoField();
            return;
        }
        refreshTable(false);
        notifyLibrariesChanged();
    }

    void notifyLibrariesChanged() {
        if (libraries_changed_) {
            libraries_changed_();
        }
    }

    void notifyPerformanceTargetChanged(
        const std::optional<LibraryManagerRow>& previous) {
        const auto current = performanceTarget();
        const bool changed =
            static_cast<bool>(previous) != static_cast<bool>(current)
            || (previous && current
                && (previous->kind != current->kind
                    || previous->id != current->id));
        if (changed && performance_target_changed_) {
            performance_target_changed_();
        }
    }

    void toggleFavoriteAt(
        int row_number,
        const LibraryManagerRow& row) {
        auto* mutable_row = table_model_.rowAtMutable(row_number);
        if (mutable_row == nullptr
            || mutable_row->role != row.role
            || mutable_row->set_id != row.set_id
            || mutable_row->id != row.id
            || mutable_row->kind != row.kind) {
            return;
        }
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !reload_ || !reload_()) {
            refreshTable(true);
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        const bool next_favorite = !mutable_row->favorite;
        if (row.kind == LibraryManagerKind::Composite) {
            auto* library = composites_();
            if (library == nullptr) {
                return;
            }
            auto* entry = library->find(row.id);
            if (entry == nullptr) {
                refreshTable(true);
                return;
            }
            entry->timbre.favorite = next_favorite;
            if (!library->update(row.id, entry->timbre, now)
                || !persist_()) {
                refreshTable(true);
                return;
            }
        } else {
            auto* library = timbres_();
            if (library == nullptr) {
                return;
            }
            auto* entry = library->find(row.id);
            if (entry == nullptr) {
                refreshTable(true);
                return;
            }
            auto updated = *entry;
            updated.favorite = next_favorite;
            if (!library->update(row.id, updated, now)
                || !persist_()) {
                refreshTable(true);
                return;
            }
        }
        mutable_row->favorite = next_favorite;
        if (static_cast<std::size_t>(row_number)
            < displayed_rows_.size()) {
            displayed_rows_[static_cast<std::size_t>(row_number)]
                .favorite = next_favorite;
        }
        if (detail_row_
            && detail_row_->id == row.id
            && detail_row_->kind == row.kind) {
            detail_row_->favorite = next_favorite;
            detail_favorite_.setText(
                next_favorite
                    ? juce::String::fromUTF8("★ お気に入り")
                    : juce::String::fromUTF8("☆ お気に入り解除"),
                juce::dontSendNotification);
            detail_favorite_.setColour(
                juce::Label::textColourId,
                next_favorite ? juce::Colour(0xFFFFD866)
                              : juce::Colour(0xFF9AA8B5));
        }
        table_.repaintRow(row_number);
        notifyLibrariesChanged();
    }

    void duplicateSelected() {
        const auto selected = selectedRows();
        if (selected.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "複製する音色を選択してください"));
            return;
        }
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !reload_ || !reload_()) {
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        for (const auto& row : selected) {
            if (!libraryManagerRowIsBody(row)) {
                continue;
            }
            if (row.kind == LibraryManagerKind::Composite) {
                auto* library = composites_();
                const auto* source = library ? library->find(row.id) : nullptr;
                if (source == nullptr) {
                    continue;
                }
                auto copy = source->timbre;
                copy.name = library->uniqueName(copy.name);
                library->add(std::move(copy), now);
            } else {
                auto* library = timbres_();
                const auto* source = library ? library->find(row.id) : nullptr;
                if (source == nullptr) {
                    continue;
                }
                auto copy = *source;
                copy.name = library->uniqueName(
                    row.kind == LibraryManagerKind::Scc
                        ? mgstc::engine::TimbreCategory::Scc
                        : mgstc::engine::TimbreCategory::Opll,
                    copy.name);
                library->add(std::move(copy), now);
            }
        }
        if (!persist_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("複製結果を保存できませんでした"));
        }
        refreshTable(true);
        notifyLibrariesChanged();
    }

    void deleteSelected() {
        std::vector<LibraryManagerRow> bodies;
        std::vector<LibraryManagerRow> members;
        std::vector<LibraryManagerRow> sets;
        for (const auto& row : selectedRows()) {
            if (row.role == LibraryManagerRowRole::Member) {
                members.push_back(row);
            } else if (row.role == LibraryManagerRowRole::SetHeader) {
                sets.push_back(row);
            } else if (row.role == LibraryManagerRowRole::Timbre) {
                bodies.push_back(row);
            }
        }
        const int groups = (bodies.empty() ? 0 : 1)
            + (members.empty() ? 0 : 1)
            + (sets.empty() ? 0 : 1);
        if (groups == 0) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "削除する音色またはセットを選択してください"));
            return;
        }
        if (groups > 1) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "セットの削除、所属の解除、音色の削除は"
                    "同時には実行できません"));
            return;
        }
        if (!members.empty()) {
            confirmRemoveMemberships(members);
            return;
        }
        if (!sets.empty()) {
            confirmDeleteSets(sets);
            return;
        }
        const auto selected = bodies;
        juce::String message =
            juce::String::fromUTF8("選択した ")
            + juce::String(static_cast<int>(selected.size()))
            + juce::String::fromUTF8(
                " 件の音色をライブラリから削除しますか？");
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(
                    juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(message)
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, selected](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                ExclusiveLibraryGuard guard(safe->shared_library_lock_);
                if (!guard.locked()
                    || !safe->reload_
                    || !safe->reload_()) {
                    return;
                }
                for (const auto& row : selected) {
                    if (row.kind == LibraryManagerKind::Composite) {
                        if (auto* library = safe->composites_()) {
                            library->erase(row.id);
                        }
                    } else if (auto* library = safe->timbres_()) {
                        library->erase(row.id);
                    }
                }
                if (!safe->persist_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                }
                safe->refreshTable(true);
                safe->notifyLibrariesChanged();
            });
    }

    void assignTagsToSelected() {
        const auto selected = selectedRows();
        if (selected.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "タグを付与する音色を選択してください"));
            return;
        }
        std::vector<std::vector<std::string>> tag_sets;
        if (auto* timbres = timbres_()) {
            for (const auto& entry : timbres->entries()) {
                tag_sets.push_back(entry.tags);
            }
        }
        if (auto* composites = composites_()) {
            for (const auto& entry : composites->entries()) {
                tag_sets.push_back(entry.timbre.tags);
            }
        }
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("タグを付与"),
            editorTagChoices(tag_sets, {}),
            {},
            true,
            [safe, selected](std::vector<std::string> additions) {
                if (safe == nullptr || additions.empty()) {
                    return;
                }
                ExclusiveLibraryGuard guard(safe->shared_library_lock_);
                if (!guard.locked()
                    || !safe->reload_
                    || !safe->reload_()) {
                    return;
                }
                const auto now = mgstc::app::currentUnixTime();
                for (const auto& row : selected) {
                    if (!libraryManagerRowIsBody(row)) {
                        continue;
                    }
                    if (row.kind == LibraryManagerKind::Composite) {
                        auto* library = safe->composites_();
                        auto* entry =
                            library ? library->find(row.id) : nullptr;
                        if (entry == nullptr) {
                            continue;
                        }
                        auto tags = entry->timbre.tags;
                        tags.insert(
                            tags.end(),
                            additions.begin(),
                            additions.end());
                        entry->timbre.tags =
                            mgstc::engine::parseTimbreTags(
                                mgstc::engine::serializeTimbreTags(
                                    tags));
                        library->update(row.id, entry->timbre, now);
                    } else {
                        auto* library = safe->timbres_();
                        auto* entry =
                            library ? library->find(row.id) : nullptr;
                        if (entry == nullptr) {
                            continue;
                        }
                        auto updated = *entry;
                        updated.tags.insert(
                            updated.tags.end(),
                            additions.begin(),
                            additions.end());
                        updated.tags =
                            mgstc::engine::parseTimbreTags(
                                mgstc::engine::serializeTimbreTags(
                                    updated.tags));
                        library->update(row.id, updated, now);
                    }
                }
                if (!safe->persist_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "タグ付与結果を保存できませんでした"));
                }
                safe->refreshTable(true);
                safe->notifyLibrariesChanged();
            });
    }

    void loadSelectedForEdit() {
        const auto selected = selectedRows();
        if (selected.size() != 1
            || !libraryManagerRowIsBody(selected.front())) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "編集する音色を1件だけ選択してください"));
            return;
        }
        if (open_editor_) {
            open_editor_(selected.front().kind, selected.front().id);
        }
    }

    void replaceSelected() {
        const auto selected = selectedRows();
        if (selected.size() != 1
            || !libraryManagerRowIsBody(selected.front())
            || !on_replace_) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "差し替える音色を1件選択してください"));
            return;
        }
        if (selected.front().kind == LibraryManagerKind::Composite) {
            return;
        }
        on_replace_(selected.front().id);
    }

    void closeParentDialog() {
        if (auto* window =
                findParentComponentOfClass<juce::DocumentWindow>()) {
            window->closeButtonPressed();
        }
    }

    [[nodiscard]] bool lockAndReload() {
        return reload_ && reload_();
    }

    void toggleExpanded(const LibraryManagerRow& row) {
        if (row.role == LibraryManagerRowRole::UnassignedHeader) {
            unassigned_expanded_ = !unassigned_expanded_;
            refreshTable(false);
            return;
        }
        if (row.role != LibraryManagerRowRole::SetHeader) {
            return;
        }
        const auto found = std::find(
            expanded_sets_.begin(), expanded_sets_.end(), row.id);
        if (found == expanded_sets_.end()) {
            expanded_sets_.push_back(row.id);
        } else {
            expanded_sets_.erase(found);
        }
        refreshTable(false);
    }

    void createSet() {
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload() || !tone_sets_
            || !persist_sets_) {
            return;
        }
        auto* sets = tone_sets_();
        if (sets == nullptr) {
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        mgstc::engine::ToneSetRecord created;
        created.id = librarySetNextId(*sets);
        created.name = librarySetUniqueName(*sets, "新しいセット");
        created.created_unix_seconds = now;
        created.updated_unix_seconds = now;
        sets->push_back(created);
        expanded_sets_.push_back(created.id);
        if (!persist_sets_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("セットを保存できませんでした"));
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void commitSetName(std::uint64_t set_id, const std::string& name) {
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload() || !tone_sets_
            || !persist_sets_) {
            restoreDetailNameField();
            return;
        }
        auto* sets = tone_sets_();
        if (sets == nullptr) {
            return;
        }
        if (librarySetNameTaken(*sets, name, set_id)) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "同じ名前のセットが既にあります"));
            restoreDetailNameField();
            return;
        }
        const auto found = std::find_if(
            sets->begin(),
            sets->end(),
            [&](const mgstc::engine::ToneSetRecord& set) {
                return set.id == set_id;
            });
        if (found == sets->end()) {
            restoreDetailNameField();
            return;
        }
        found->name = name;
        found->updated_unix_seconds = mgstc::app::currentUnixTime();
        ++found->revision;
        if (!persist_sets_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "セット名を保存できませんでした"));
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void commitSetMemo(std::uint64_t set_id, const std::string& memo) {
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload() || !tone_sets_
            || !persist_sets_) {
            return;
        }
        auto* sets = tone_sets_();
        if (sets == nullptr) {
            return;
        }
        const auto found = std::find_if(
            sets->begin(),
            sets->end(),
            [&](const mgstc::engine::ToneSetRecord& set) {
                return set.id == set_id;
            });
        if (found == sets->end()) {
            return;
        }
        found->memo = memo;
        found->updated_unix_seconds = mgstc::app::currentUnixTime();
        ++found->revision;
        if (!persist_sets_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8(
                    "セットのメモを保存できませんでした"));
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void confirmRemoveMemberships(
        std::vector<LibraryManagerRow> members) {
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(juce::String::fromUTF8(
                    "選択した音色を、そのセットから外します。"
                    "音色自体と、ほかのセットの所属は残します。"))
                .withButton(juce::String::fromUTF8("セットから外す"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, members](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                ExclusiveLibraryGuard guard(safe->shared_library_lock_);
                if (!guard.locked() || !safe->lockAndReload()
                    || !safe->tone_members_ || !safe->persist_sets_) {
                    return;
                }
                auto* stored = safe->tone_members_();
                if (stored == nullptr) {
                    return;
                }
                for (const auto& row : members) {
                    librarySetRemoveMembership(
                        *stored,
                        row.set_id,
                        row.kind == LibraryManagerKind::Composite,
                        row.id);
                }
                if (!safe->persist_sets_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "所属の変更を保存できませんでした"));
                }
                safe->refreshTable(true);
            });
    }

    void confirmDeleteSets(std::vector<LibraryManagerRow> sets) {
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(
                    juce::String::fromUTF8("選択した ")
                    + juce::String(static_cast<int>(sets.size()))
                    + juce::String::fromUTF8(
                        " 件のセットを削除します。"
                        "音色自体は残ります。"))
                .withButton(juce::String::fromUTF8("セットを削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, sets](int result) {
                if (safe == nullptr || result != 1) {
                    return;
                }
                ExclusiveLibraryGuard guard(safe->shared_library_lock_);
                if (!guard.locked() || !safe->lockAndReload()
                    || !safe->tone_sets_ || !safe->tone_members_
                    || !safe->persist_sets_) {
                    return;
                }
                auto* stored_sets = safe->tone_sets_();
                auto* stored_members = safe->tone_members_();
                if (stored_sets == nullptr || stored_members == nullptr) {
                    return;
                }
                for (const auto& row : sets) {
                    librarySetDelete(*stored_sets, *stored_members, row.id);
                }
                if (!safe->persist_sets_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "セットの削除を保存できませんでした"));
                }
                safe->refreshTable(true);
            });
    }

    void duplicateSet(std::uint64_t set_id) {
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload() || !tone_sets_
            || !tone_members_ || !persist_sets_) {
            return;
        }
        auto* sets = tone_sets_();
        auto* members = tone_members_();
        if (sets == nullptr || members == nullptr) {
            return;
        }
        const auto found = std::find_if(
            sets->begin(),
            sets->end(),
            [&](const mgstc::engine::ToneSetRecord& set) {
                return set.id == set_id;
            });
        if (found == sets->end()) {
            return;
        }
        const auto now = mgstc::app::currentUnixTime();
        mgstc::engine::ToneSetRecord copy = *found;
        copy.id = librarySetNextId(*sets);
        copy.name = librarySetUniqueName(*sets, found->name);
        copy.created_unix_seconds = now;
        copy.updated_unix_seconds = now;
        copy.revision = 1;
        const auto source_id = found->id;
        sets->push_back(copy);
        const auto snapshot = *members;
        for (const auto& member : snapshot) {
            if (member.set_id != source_id) {
                continue;
            }
            auto cloned = member;
            cloned.set_id = copy.id;
            members->push_back(cloned);
        }
        expanded_sets_.push_back(copy.id);
        if (!persist_sets_()) {
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void paintRowMiniPreview(
        juce::Graphics& graphics,
        juce::Rectangle<float> area,
        const LibraryManagerRow& row) {
        if (!libraryManagerRowIsBody(row) || area.isEmpty()) {
            return;
        }
        graphics.setColour(juce::Colour(0xFF182028));
        graphics.fillRoundedRectangle(area, 3.0F);
        const auto graph = area.reduced(3.0F, 2.0F);
        graphics.setColour(juce::Colour(0xFF586675));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX(),
            graph.getRight());
        if (row.kind == LibraryManagerKind::Composite) {
            const auto* library = composites_();
            const auto* entry = library ? library->find(row.id) : nullptr;
            if (entry != nullptr) {
                paintToneMiniEnvelope(graphics, graph, entry->timbre);
            }
            return;
        }
        const auto* library = timbres_();
        const auto* entry = library ? library->find(row.id) : nullptr;
        if (entry == nullptr) {
            return;
        }
        if (row.kind == LibraryManagerKind::Scc) {
            mgstc::engine::SccWaveform wave{};
            for (std::size_t index = 0; index < wave.size(); ++index) {
                wave[index] = std::bit_cast<std::int8_t>(
                    entry->scc_waveform[index]);
            }
            paintToneMiniWave(graphics, graph, wave);
            return;
        }
        const auto trace = mgstc::engine::traceOpllEnvelope(
            mgstc::engine::decodeOpllPatch(entry->opll_registers), 60);
        if (trace.valid) {
            paintToneMiniLevels(
                graphics, graph, trace.carrier, juce::Colour(kUiHoverAccent));
        }
    }

    [[nodiscard]] std::vector<LibrarySetDragItem> dragItemsForRow(
        int row_number) const {
        std::vector<LibraryManagerRow> chosen;
        const auto selected = selectedRows();
        const auto* clicked = table_model_.rowAt(row_number);
        if (clicked == nullptr || !libraryManagerRowIsBody(*clicked)) {
            return {};
        }
        const bool clicked_selected = std::any_of(
            selected.begin(),
            selected.end(),
            [&](const LibraryManagerRow& row) {
                return row.role == clicked->role
                    && row.set_id == clicked->set_id
                    && row.kind == clicked->kind
                    && row.id == clicked->id;
            });
        if (clicked_selected) {
            for (const auto& row : selected) {
                if (libraryManagerRowIsBody(row)) {
                    chosen.push_back(row);
                }
            }
        } else {
            chosen.push_back(*clicked);
        }
        std::vector<LibrarySetDragItem> items;
        for (const auto& row : chosen) {
            LibrarySetDragItem item;
            item.source_set_id =
                row.role == LibraryManagerRowRole::Member ? row.set_id : 0;
            item.composite = row.kind == LibraryManagerKind::Composite;
            item.body_id = row.id;
            items.push_back(item);
        }
        return items;
    }

    [[nodiscard]] bool canDragRow(int row_number) const {
        return !dragItemsForRow(row_number).empty();
    }

    void startRowDrag(int row_number) {
        drag_items_ = dragItemsForRow(row_number);
        if (drag_items_.empty()) {
            return;
        }
        startDragging(
            juce::var("mgstc-tone-membership"),
            &table_,
            juce::ScaledImage{},
            false);
    }

    [[nodiscard]] bool dropInterested(const juce::var& description) const {
        return description == juce::var("mgstc-tone-membership")
            && !drag_items_.empty();
    }

    void completeDrop(int row_number) {
        const auto items = drag_items_;
        drag_items_.clear();
        if (items.empty() || !tone_members_) {
            return;
        }
        const auto* row = table_model_.rowAt(row_number);
        if (row == nullptr) {
            return;
        }
        LibrarySetDropTarget target;
        if (row->role == LibraryManagerRowRole::UnassignedHeader
            || row->role == LibraryManagerRowRole::Timbre) {
            target.place = LibrarySetDropPlace::Unassigned;
        } else if (row->role == LibraryManagerRowRole::SetHeader) {
            target.place = LibrarySetDropPlace::SetHeader;
            target.set_id = row->id;
        } else if (row->role == LibraryManagerRowRole::Member) {
            target.place = LibrarySetDropPlace::Member;
            target.set_id = row->set_id;
            target.composite = row->kind == LibraryManagerKind::Composite;
            target.body_id = row->id;
        } else {
            return;
        }
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload()) {
            return;
        }
        auto* members = tone_members_();
        if (members == nullptr || !persist_sets_) {
            return;
        }
        const bool ctrl =
            juce::ModifierKeys::getCurrentModifiers().isCtrlDown();
        if (!applyLibrarySetDrop(*members, items, target, ctrl)) {
            refreshTable(false);
            return;
        }
        if (!persist_sets_()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8("ライブラリ管理"),
                juce::String::fromUTF8("所属の変更を保存できませんでした"));
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void addMembersToSet(
        std::vector<LibraryManagerRow> rows,
        std::uint64_t set_id,
        bool move_from_source) {
        ExclusiveLibraryGuard guard(shared_library_lock_);
        if (!guard.locked() || !lockAndReload() || !tone_members_
            || !persist_sets_) {
            return;
        }
        auto* members = tone_members_();
        if (members == nullptr) {
            return;
        }
        std::vector<LibrarySetDragItem> items;
        for (const auto& row : rows) {
            if (!libraryManagerRowIsBody(row)) {
                continue;
            }
            LibrarySetDragItem item;
            item.source_set_id =
                row.role == LibraryManagerRowRole::Member ? row.set_id : 0;
            item.composite = row.kind == LibraryManagerKind::Composite;
            item.body_id = row.id;
            items.push_back(item);
        }
        LibrarySetDropTarget target;
        target.place = LibrarySetDropPlace::SetHeader;
        target.set_id = set_id;
        if (!applyLibrarySetDrop(
                *members, items, target, !move_from_source)) {
            return;
        }
        if (!persist_sets_()) {
            refreshTable(true);
            return;
        }
        refreshTable(false);
    }

    void showRowContextMenu(const LibraryManagerRow& anchor) {
        juce::PopupMenu menu;
        menu.addItem(1, juce::String::fromUTF8("セットを追加"));
        const bool header =
            anchor.role == LibraryManagerRowRole::SetHeader;
        const bool member =
            anchor.role == LibraryManagerRowRole::Member;
        const bool body = libraryManagerRowIsBody(anchor);
        if (header) {
            menu.addItem(2, juce::String::fromUTF8("セットを複製"));
            menu.addItem(3, juce::String::fromUTF8("セットを削除"));
            menu.addItem(
                4,
                anchor.expanded
                    ? juce::String::fromUTF8("折りたたむ")
                    : juce::String::fromUTF8("展開する"));
        }
        if (member) {
            menu.addItem(5, juce::String::fromUTF8("このセットから外す"));
        }
        if (body) {
            menu.addItem(
                6, juce::String::fromUTF8("ライブラリから音色を削除"));
        }
        if (header || body) {
            menu.addItem(
                7, juce::String::fromUTF8("エクスポート対象に追加"));
        }
        juce::PopupMenu add_to;
        juce::PopupMenu move_to;
        if (body && tone_sets_) {
            if (const auto* sets = tone_sets_()) {
                int id = 100;
                for (const auto& set : *sets) {
                    add_to.addItem(
                        id,
                        juce::String::fromUTF8(set.name.c_str()));
                    move_to.addItem(
                        id + 1000,
                        juce::String::fromUTF8(set.name.c_str()));
                    ++id;
                }
            }
        }
        if (add_to.getNumItems() > 0) {
            menu.addSubMenu(
                juce::String::fromUTF8("セットへ追加"), add_to);
            if (member) {
                menu.addSubMenu(
                    juce::String::fromUTF8("セットへ移動"), move_to);
            }
        }
        const auto selected = selectedRows();
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&table_),
            [safe, anchor, selected](int result) {
                if (safe == nullptr || result == 0) {
                    return;
                }
                if (result == 1) {
                    safe->createSet();
                } else if (result == 2) {
                    safe->duplicateSet(anchor.id);
                } else if (result == 3) {
                    safe->confirmDeleteSets({anchor});
                } else if (result == 4) {
                    safe->toggleExpanded(anchor);
                } else if (result == 5) {
                    safe->confirmRemoveMemberships({anchor});
                } else if (result == 6) {
                    safe->deleteBodies({anchor});
                } else if (result == 7) {
                    if (safe->export_add_) {
                        auto rows = selected;
                        if (rows.empty()) {
                            rows.push_back(anchor);
                        }
                        safe->export_add_(rows);
                    }
                } else if (result >= 100 && result < 1000) {
                    const auto set_index = result - 100;
                    if (safe->tone_sets_) {
                        if (const auto* sets = safe->tone_sets_()) {
                            if (set_index >= 0
                                && set_index
                                    < static_cast<int>(sets->size())) {
                                const auto rows = selected.empty()
                                    ? std::vector<LibraryManagerRow>{anchor}
                                    : selected;
                                safe->addMembersToSet(
                                    rows,
                                    (*sets)[static_cast<std::size_t>(
                                        set_index)]
                                        .id,
                                    false);
                            }
                        }
                    }
                } else if (result >= 1100) {
                    const auto set_index = result - 1100;
                    if (safe->tone_sets_) {
                        if (const auto* sets = safe->tone_sets_()) {
                            if (set_index >= 0
                                && set_index
                                    < static_cast<int>(sets->size())) {
                                const auto rows = selected.empty()
                                    ? std::vector<LibraryManagerRow>{anchor}
                                    : selected;
                                safe->addMembersToSet(
                                    rows,
                                    (*sets)[static_cast<std::size_t>(
                                        set_index)]
                                        .id,
                                    true);
                            }
                        }
                    }
                }
            });
    }

    void deleteBodies(std::vector<LibraryManagerRow> bodies) {
        juce::Component::SafePointer<LibraryManagerListTab> safe(this);
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle(juce::String::fromUTF8("ライブラリ管理"))
                .withMessage(juce::String::fromUTF8(
                    "選択した音色をライブラリから削除します。"
                    "セットの所属も一緒に消えます。"))
                .withButton(juce::String::fromUTF8("音色を削除"))
                .withButton(juce::String::fromUTF8("キャンセル"))
                .withAssociatedComponent(this),
            [safe, bodies](int result) {
                if (safe == nullptr || result != 1 || !safe->persist_) {
                    return;
                }
                ExclusiveLibraryGuard guard(safe->shared_library_lock_);
                if (!guard.locked() || !safe->lockAndReload()) {
                    return;
                }
                for (const auto& row : bodies) {
                    if (row.kind == LibraryManagerKind::Composite) {
                        if (auto* library = safe->composites_()) {
                            library->erase(row.id);
                        }
                    } else if (auto* library = safe->timbres_()) {
                        library->erase(row.id);
                    }
                }
                if (!safe->persist_()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        juce::String::fromUTF8("ライブラリ管理"),
                        juce::String::fromUTF8(
                            "削除結果を保存できませんでした"));
                }
                safe->refreshTable(true);
                safe->notifyLibrariesChanged();
            });
    }

    class SetTable final
        : public juce::TableListBox,
          public juce::DragAndDropTarget {
    public:
        explicit SetTable(LibraryManagerListTab& owner)
            : owner_(owner) {
            setWantsKeyboardFocus(true);
        }

        void mouseDown(const juce::MouseEvent& event) override {
            drag_row_ = rowUnder(event);
            drag_started_ = false;
            juce::TableListBox::mouseDown(event);
        }

        void mouseDrag(const juce::MouseEvent& event) override {
            if (!drag_started_
                && event.getDistanceFromDragStart() >= 8
                && owner_.canDragRow(drag_row_)) {
                drag_started_ = true;
                owner_.startRowDrag(drag_row_);
                return;
            }
            if (!drag_started_) {
                juce::TableListBox::mouseDrag(event);
            }
        }

        bool keyPressed(const juce::KeyPress& key) override {
            if (key == juce::KeyPress::deleteKey
                || key == juce::KeyPress::backspaceKey) {
                owner_.deleteSelected();
                return true;
            }
            if (key.getKeyCode() == juce::KeyPress::F10Key
                && key.getModifiers().isShiftDown()) {
                const auto chosen = owner_.selectedRows();
                if (!chosen.empty()) {
                    owner_.showRowContextMenu(chosen.front());
                }
                return true;
            }
            if (key == juce::KeyPress::rightKey
                || key == juce::KeyPress::leftKey) {
                const auto chosen = owner_.selectedRows();
                if (chosen.size() == 1
                    && libraryManagerRowIsHeader(chosen.front())) {
                    const bool expand =
                        key == juce::KeyPress::rightKey;
                    if (chosen.front().expanded != expand) {
                        owner_.toggleExpanded(chosen.front());
                    }
                    return true;
                }
            }
            return juce::TableListBox::keyPressed(key);
        }

        bool isInterestedInDragSource(
            const SourceDetails& details) override {
            return owner_.dropInterested(details.description);
        }

        void itemDropped(const SourceDetails& details) override {
            juce::ignoreUnused(details);
            owner_.completeDrop(rowUnder(details.localPosition));
        }

    private:
        [[nodiscard]] int rowUnder(juce::Point<int> position) const {
            return getRowContainingPosition(position.x, position.y);
        }

        [[nodiscard]] int rowUnder(const juce::MouseEvent& event) {
            const auto position =
                event.getEventRelativeTo(this).getPosition();
            return rowUnder(position);
        }

        LibraryManagerListTab& owner_;
        int drag_row_{-1};
        bool drag_started_{false};
    };

    PreviewCallback preview_;
    OpenEditorCallback open_editor_;
    ReloadCallback reload_;
    PersistCallback persist_;
    std::function<mgstc::engine::TimbreLibrary*()> timbres_;
    std::function<mgstc::engine::CompositeTimbreLibrary*()>
        composites_;
    PerformanceTargetChangedCallback performance_target_changed_;
    LibrariesChangedCallback libraries_changed_;
    juce::InterProcessLock* shared_library_lock_{};
    std::function<std::vector<mgstc::engine::ToneSetRecord>*()> tone_sets_;
    std::function<std::vector<mgstc::engine::ToneSetMemberRecord>*()>
        tone_members_;
    std::function<bool()> persist_sets_;
    std::function<void(const std::vector<LibraryManagerRow>&)> export_add_;
    juce::TooltipWindow tooltip_window_;
    SwitchLookAndFeel switch_look_and_feel_;
    juce::ComboBox category_;
    juce::TextEditor filter_;
    juce::TextButton tag_filter_;
    juce::ToggleButton favorite_only_;
    juce::ToggleButton recent_used_;
    juce::ToggleButton tree_mode_;
    juce::TextButton new_set_;
    juce::TextButton detail_toggle_;
    bool detail_open_{true};
    int detail_width_{280};
    bool resizing_detail_{false};
    mutable std::vector<std::uint64_t> expanded_sets_;
    mutable bool expansion_ready_{false};
    mutable bool unassigned_expanded_{true};
    std::vector<LibrarySetDragItem> drag_items_;
    LibraryManagerTableModel table_model_;
    SetTable table_;
    juce::Rectangle<int> table_bounds_;
    juce::Rectangle<int> detail_bounds_;
    std::vector<LibraryManagerRow> displayed_rows_;
    std::vector<std::string> selected_filter_tags_;
    std::optional<LibraryManagerRow> detail_row_;
    bool syncing_detail_{false};
    juce::Viewport detail_viewport_;
    juce::Component detail_host_;
    juce::TextEditor detail_name_;
    juce::Label detail_category_;
    juce::Label detail_favorite_;
    juce::Label detail_tags_empty_;
    juce::Component detail_tags_host_;
    std::vector<std::unique_ptr<RemovableDetailTagChip>>
        detail_tag_chips_;
    juce::TextEditor detail_memo_;
    juce::TextButton duplicate_;
    juce::TextButton remove_;
    juce::TextButton assign_tags_;
    juce::TextButton load_edit_;
    juce::TextButton replace_;
    juce::TextButton close_;
    bool replace_mode_{};
    std::function<void(std::uint64_t)> on_replace_;
};
