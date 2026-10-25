// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "editor_session.hpp"
#include "mgstc/engine/patchslide_database.hpp"
#include "mgstc/engine/patchslide_timbre.hpp"
#include "switch_look_and_feel.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"

namespace mgstc::app {

// Shared by the standalone and plugin library windows. Only the worker loads
// files or computes searches; the message thread owns selection and callbacks.
class PatchSlideBrowserComponent final : public juce::Component,
                                        private juce::ListBoxModel,
                                        private juce::Timer {
public:
    struct Callbacks {
        std::function<void(const engine::CompositeTimbre&)> preview;
        std::function<void(const engine::CompositeTimbre&)> apply;
        std::function<void(const engine::CompositeTimbre&)> registerTimbre;
        std::function<void()> stop;
    };

    explicit PatchSlideBrowserComponent(
        Callbacks callbacks, BackgroundTaskBoundary* background_tasks = nullptr)
        : PatchSlideBrowserComponent(std::move(callbacks), background_tasks, {}) {}

private:
    friend class PatchSlideBrowserTestAccess;
    PatchSlideBrowserComponent(Callbacks callbacks, BackgroundTaskBoundary* background_tasks,
                              std::filesystem::path directory)
        : callbacks_(std::move(callbacks)), tags_model_(*this),
          worker_(std::make_shared<Worker>()) {
        worker_->directory = std::move(directory);
        mode_.addItem(utf8("タグから探す"), 1);
        mode_.addItem(utf8("分類から探す"), 2);
        mode_.addItem(utf8("すべての音色"), 3);
        mode_.setSelectedId(1, juce::dontSendNotification);
        match_.addItem(utf8("AND（すべて）"), 1);
        match_.addItem(utf8("OR（いずれか）"), 2);
        match_.setSelectedId(1, juce::dontSendNotification);
        cluster_size_.addItem(utf8("16群（独立分類）"), 16);
        cluster_size_.addItem(utf8("64群（独立分類）"), 64);
        cluster_size_.setSelectedId(16, juce::dontSendNotification);
        mode_.onChange = [this] { updateMode(); requestBrowse(); };
        match_.onChange = [this] { requestBrowse(); };
        cluster_size_.onChange = [this] { rebuildGroups(); requestBrowse(); };
        group_.onChange = [this] { updateRepresentative(); requestBrowse(); };
        rejected_.setButtonText(utf8("Rejectも表示（注意）"));
        rejected_.setLookAndFeel(&switch_style_);
        rejected_.setTooltip(utf8("通常非表示の不安定候補を明示的に表示します。共有DBは変更しません。"));
        rejected_.onClick = [this] {
            if (query_kind_ == QueryKind::Browse) requestBrowse();
            else request(query_kind_, query_source_);
        };
        configure(preview_, "試聴", [this] { previewSelection(); });
        configure(stop_, "停止", [this] { stopPreview(); });
        configure(apply_, "編集へ適用", [this] {
            if (selection_ && callbacks_.apply) callbacks_.apply(*selection_);
        });
        configure(register_, "ライブラリへ登録", [this] {
            if (selection_ && callbacks_.registerTimbre) callbacks_.registerTimbre(*selection_);
        });
        configure(similar_, "Standard B", [this] {
            if (selected_index_) request(QueryKind::StandardB, *selected_index_);
        });
        configure(variation_, "Variation（3候補）", [this] {
            if (selected_index_) request(QueryKind::Variation, *selected_index_);
        });
        configure(random_, "音色発掘", [this] {
            if (!rows_.empty()) {
                const int row = juce::Random::getSystemRandom().nextInt(static_cast<int>(rows_.size()));
                list_.selectRow(row);
                list_.scrollToEnsureRowIsOnscreen(row);
                previewSelection();
            }
        });
        configure(back_, "探索へ戻る", [this] { requestBrowse(); });
        configure(reload_, "再読込", [this] { requestBrowse(); });
        configure(representative_, "代表候補を試聴", [this] {
            if (busy_ || query_kind_ != QueryKind::Browse) return;
            if (const auto* group = selectedGroup()) {
                const auto row = std::find(rows_.begin(), rows_.end(), group->representative_index);
                if (row != rows_.end()) {
                    const auto index = static_cast<int>(std::distance(rows_.begin(), row));
                    list_.selectRow(index);
                    list_.scrollToEnsureRowIsOnscreen(index);
                }
                selectCandidate(group->representative_index);
                previewSelection();
            }
        });
        list_.setModel(this);
        list_.setMultipleSelectionEnabled(false);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(UiLayout::panelFill));
        list_.setOutlineThickness(1);
        list_.setColour(juce::ListBox::outlineColourId, juce::Colour(UiLayout::panelStroke));
        tag_list_.setModel(&tags_model_);
        tag_list_.setMultipleSelectionEnabled(true);
        tag_list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(UiLayout::panelFill));
        tag_hint_.setText(utf8("タグを複数選択: Ctrl / Shift"), juce::dontSendNotification);
        group_info_.setJustificationType(juce::Justification::topLeft);
        details_.setMultiLine(true);
        details_.setReadOnly(true);
        details_.setScrollbarsShown(true);
        details_.setCaretVisible(false);
        status_.setJustificationType(juce::Justification::centredLeft);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &mode_, &match_, &cluster_size_, &group_, &rejected_, &preview_, &stop_,
                 &apply_, &register_, &similar_, &variation_, &random_, &back_, &reload_,
                 &representative_, &list_, &tag_list_, &tag_hint_, &group_info_, &details_, &status_})
            addAndMakeVisible(component);
        int focus = 100;
        for (auto* component : std::initializer_list<juce::Component*>{
                 &mode_, &match_, &tag_list_, &cluster_size_, &group_, &representative_,
                 &rejected_, &list_, &random_, &similar_, &variation_, &back_, &preview_,
                 &stop_, &apply_, &register_, &details_, &reload_})
            component->setExplicitFocusOrder(focus++);
        const bool registered = !background_tasks || background_tasks->registerTask(worker_);
        if (registered && worker_->start()) {
            requestBrowse();
            startTimerHz(30);
        } else {
            worker_->cancel();
            reload_.setEnabled(false);
            status_.setText(utf8("検索ワーカーを開始できません。画面を開き直してください。"), juce::dontSendNotification);
        }
        updateMode();
        updateActions();
    }

public:
    ~PatchSlideBrowserComponent() override {
        stopTimer();
        stopPreview();
        worker_->cancel();
        list_.setModel(nullptr);
        tag_list_.setModel(nullptr);
        rejected_.setLookAndFeel(nullptr);
        // Plugin ownership is retained by BackgroundTaskBoundary until its
        // non-audio shutdown joins the worker, including after editor closure.
    }

    [[nodiscard]] const std::optional<engine::CompositeTimbre>& selectedTimbre() const noexcept {
        return selection_;
    }
    [[nodiscard]] const engine::PatchSlideCandidate* selectedCandidate() const noexcept {
        return db_ && selected_index_ ? &db_->candidates()[*selected_index_] : nullptr;
    }
    void stopPreview() { if (callbacks_.stop) callbacks_.stop(); }

    void visibilityChanged() override { if (!isShowing()) stopPreview(); }
    void paint(juce::Graphics& g) override { paintPageBackground(g, getLocalBounds()); }
    void resized() override {
        using namespace UiLayout;
        UiFonts::styleBodyField(details_);
        for (auto* label : {&status_, &tag_hint_, &group_info_}) label->setFont(UiFonts::body());
        list_.setRowHeight(fieldH * 2);
        tag_list_.setRowHeight(fieldH);
        auto area = getLocalBounds().reduced(panelPad);
        auto top = area.removeFromTop(fieldH);
        mode_.setBounds(top.removeFromLeft(libraryWidth));
        top.removeFromLeft(controlGap);
        reload_.setBounds(top.removeFromRight(libraryManageButtonW));
        top.removeFromRight(controlGap);
        rejected_.setBounds(top);
        area.removeFromTop(sm);
        status_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        auto bottom = area.removeFromBottom(textButtonH);
        preview_.setBounds(bottom.removeFromLeft(libraryButtonMinW));
        bottom.removeFromLeft(controlGap);
        stop_.setBounds(bottom.removeFromLeft(libraryButtonMinW));
        bottom.removeFromLeft(controlGap);
        apply_.setBounds(bottom.removeFromLeft(compositeSelectorW));
        bottom.removeFromLeft(controlGap);
        register_.setBounds(bottom.removeFromLeft(outputSwitchW + panelPad * 2));
        area.removeFromBottom(sm);
        details_.setBounds(area.removeFromBottom(libraryMemoH + fieldH));
        area.removeFromBottom(sm);
        auto filters = area.removeFromLeft(libraryWidth);
        area.removeFromLeft(panelGap);
        match_.setBounds(filters.removeFromTop(fieldH));
        filters.removeFromTop(sm);
        tag_hint_.setBounds(filters.removeFromTop(fieldH));
        filters.removeFromTop(xs);
        tag_list_.setBounds(filters);
        auto groups = getLocalBounds().reduced(panelPad);
        groups.removeFromTop(fieldH * 2 + sm * 2);
        groups.setWidth(libraryWidth);
        cluster_size_.setBounds(groups.removeFromTop(fieldH));
        groups.removeFromTop(sm);
        group_.setBounds(groups.removeFromTop(fieldH));
        groups.removeFromTop(sm);
        representative_.setBounds(groups.removeFromTop(textButtonH));
        groups.removeFromTop(sm);
        group_info_.setBounds(groups.removeFromTop(fieldH * 3));
        auto actions = area.removeFromTop(textButtonH);
        similar_.setBounds(actions.removeFromLeft(compositeSelectorW));
        actions.removeFromLeft(controlGap);
        variation_.setBounds(actions.removeFromLeft(outputSwitchW));
        actions.removeFromLeft(controlGap);
        back_.setBounds(actions.removeFromLeft(compositeSelectorW));
        area.removeFromTop(sm);
        random_.setBounds(juce::Rectangle<int>(panelPad, status_.getBottom() + sm,
            libraryWidth, textButtonH));
        list_.setBounds(area);
    }

private:
    enum class QueryKind { Browse, StandardB, Variation };
    struct Query {
        std::uint64_t generation{};
        QueryKind kind{QueryKind::Browse};
        engine::PatchSlideFilter filter;
        std::size_t source{};
    };
    struct Result {
        std::uint64_t generation{};
        std::shared_ptr<const engine::PatchSlideDatabase> db;
        std::shared_ptr<const std::vector<juce::String>> names;
        std::vector<std::size_t> rows;
        std::vector<engine::PatchSlideSearchResult> ranks;
        std::size_t total{}, rejected{}, other_hidden{};
        std::string error;
    };

    struct Worker final : EditorBackgroundTask {
        ~Worker() override { cancel(); join(); }
        void cancel() noexcept override {
            {
                std::lock_guard lock(mutex);
                cancelled.store(true, std::memory_order_release);
            }
            wake.notify_all();
        }
        void join() noexcept override {
            std::lock_guard lock(start_mutex);
            shutdown = true;
            if (thread.joinable()) thread.join();
        }
        bool completed() const noexcept override { return done.load(std::memory_order_acquire); }
        bool start() {
            std::lock_guard lock(start_mutex);
            if (shutdown || cancelled.load(std::memory_order_acquire)) return false;
            thread = std::jthread([this] { run(); });
            return true;
        }
        void submit(Query query) {
            std::lock_guard lock(mutex);
            newest.store(query.generation, std::memory_order_release);
            pending = std::move(query);
            wake.notify_one();
        }
        void run() noexcept {
            std::shared_ptr<const engine::PatchSlideDatabase> db;
            std::shared_ptr<const std::vector<juce::String>> names;
            for (;;) {
                Query query;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [this] { return cancelled.load(std::memory_order_acquire) || pending.has_value(); });
                    if (cancelled.load(std::memory_order_acquire)) break;
                    query = std::move(*pending);
                    pending.reset();
                }
                Result result;
                result.generation = query.generation;
                try {
                    if (!db) db = directory.empty() ? engine::PatchSlideDatabase::load(&result.error)
                        : engine::PatchSlideDatabase::load(directory, &result.error);
                    if (db && !names) {
                        auto formatted = std::make_shared<std::vector<juce::String>>();
                        formatted->reserve(db->candidates().size());
                        for (const auto& candidate : db->candidates())
                            formatted->push_back(utf8(engine::patchSlideDisplayName(candidate).c_str()));
                        names = std::move(formatted);
                    }
                    if (cancelled.load(std::memory_order_acquire)) break;
                    if (query.generation != newest.load(std::memory_order_acquire)) continue;
                    result.db = db;
                    result.names = names;
                    if (db) {
                        if (query.kind == QueryKind::Browse) {
                            auto all = query.filter;
                            all.show_rejected = true;
                            const auto candidates = db->filter(all);
                            result.total = candidates.size();
                            for (const auto index : candidates) appendVisible(result, index, query.filter.show_rejected);
                        } else {
                            if (query.kind == QueryKind::StandardB)
                                result.ranks = db->standardB(query.source, db->candidates().size() - 1);
                            else {
                                const auto variation = db->variation(query.source);
                                for (const auto& row : variation.candidates) result.ranks.push_back(row);
                            }
                            result.total = result.ranks.size();
                            for (const auto& row : result.ranks) appendVisible(result, row.candidate_index, query.filter.show_rejected);
                        }
                    }
                } catch (const std::exception& error) { result.error = error.what(); }
                  catch (...) { result.error = "PatchSlide search failed"; }
                if (cancelled.load(std::memory_order_acquire)) break;
                {
                    std::lock_guard lock(mutex);
                    if (query.generation == newest.load(std::memory_order_acquire)) ready = std::move(result);
                }
            }
            done.store(true, std::memory_order_release);
        }
        static void appendVisible(Result& result, std::size_t index, bool show_rejected) {
            const auto& candidate = result.db->candidates()[index];
            if (engine::PatchSlideDatabase::isVisible(candidate, show_rejected)) result.rows.push_back(index);
            else if (engine::PatchSlideDatabase::isRejected(candidate)) ++result.rejected;
            else ++result.other_hidden;
        }
        std::mutex start_mutex, mutex;
        bool shutdown{};
        std::condition_variable wake;
        std::jthread thread;
        std::atomic<bool> cancelled{false}, done{false};
        std::atomic<std::uint64_t> newest{0};
        std::optional<Query> pending;
        std::optional<Result> ready;
        std::filesystem::path directory;
    };

    class TagsModel final : public juce::ListBoxModel {
    public:
        explicit TagsModel(PatchSlideBrowserComponent& owner) : owner_(owner) {}
        int getNumRows() override { return static_cast<int>(owner_.used_tags_.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
            if (row < 0 || row >= getNumRows()) return;
            if (selected) g.fillAll(uiToggleOnFill());
            g.setColour(juce::Colours::white);
            g.setFont(UiFonts::body());
            const auto& tag = owner_.used_tags_[static_cast<std::size_t>(row)];
            g.drawFittedText(utf8(tag.tag.c_str()) + " (" + juce::String(static_cast<int>(tag.candidate_count)) + ")",
                UiLayout::xs, 0, width - UiLayout::xs * 2, height, juce::Justification::centredLeft, 1);
        }
        void selectedRowsChanged(int) override { if (!owner_.updating_) owner_.requestBrowse(); }
    private:
        PatchSlideBrowserComponent& owner_;
    };

    static juce::String utf8(const char* value) { return juce::String::fromUTF8(value); }
    void configure(juce::TextButton& button, const char* text, std::function<void()> action) {
        button.setButtonText(utf8(text));
        button.onClick = std::move(action);
    }
    void requestBrowse() { request(QueryKind::Browse, 0); }
    void request(QueryKind kind, std::size_t source) {
        if (worker_->cancelled.load(std::memory_order_acquire)) return;
        stopPreview();
        selection_.reset();
        selected_index_.reset();
        busy_ = true;
        query_kind_ = kind;
        query_source_ = source;
        Query query;
        query.generation = ++generation_;
        query.kind = kind;
        query.source = source;
        query.filter.show_rejected = rejected_.getToggleState();
        if (kind == QueryKind::Browse) {
            if (mode_.getSelectedId() == 1) {
                query.filter.tags_match_all = match_.getSelectedId() == 1;
                const auto selected = tag_list_.getSelectedRows();
                for (int i = 0; i < selected.size(); ++i) {
                    const int row = selected[i];
                    if (row >= 0 && static_cast<std::size_t>(row) < used_tags_.size())
                        query.filter.tags.push_back(used_tags_[static_cast<std::size_t>(row)].tag);
                }
            } else if (mode_.getSelectedId() == 2 && group_.getSelectedId() > 1) {
                query.filter.cluster_count = cluster_size_.getSelectedId();
                query.filter.cluster_id = group_.getSelectedId() - 2;
            }
        }
        status_.setText(utf8("読込・検索中…"), juce::dontSendNotification);
        details_.clear();
        updateActions();
        worker_->submit(std::move(query));
    }
    void timerCallback() override {
        std::optional<Result> result;
        {
            std::lock_guard lock(worker_->mutex);
            result.swap(worker_->ready);
        }
        if (!result || result->generation != generation_) return;
        const juce::ScopedValueSetter<bool> adopting(adopting_result_, true);
        busy_ = false;
        const bool first_load = !db_ && result->db;
        db_ = std::move(result->db);
        names_ = std::move(result->names);
        rows_ = std::move(result->rows);
        ranks_ = std::move(result->ranks);
        updating_ = true;
        list_.deselectAllRows();
        list_.updateContent();
        if (first_load) {
            for (const auto& tag : db_->tags()) if (tag.candidate_count > 0) used_tags_.push_back(tag);
            tag_list_.updateContent();
            rebuildGroups();
        }
        updating_ = false;
        if (!result->error.empty()) {
            status_.setText(utf8("パッチスライドを利用できません"), juce::dontSendNotification);
            details_.setText(utf8(result->error.c_str()) + utf8("\n正しい3ファイルを配置後、再読込またはMGSTCを再起動してください。"), false);
        } else {
            auto text = query_kind_ == QueryKind::Variation ? utf8("Variation（選択順を保持）: ")
                : query_kind_ == QueryKind::StandardB ? utf8("Standard B（距離順位）: ") : utf8("探索: ");
            text += utf8("表示 ") + juce::String(static_cast<int>(rows_.size())) + " / " + juce::String(static_cast<int>(result->total));
            text += utf8("　除外 Reject ") + juce::String(static_cast<int>(result->rejected));
            text += utf8(" / 表示不可 ") + juce::String(static_cast<int>(result->other_hidden));
            status_.setText(text, juce::dontSendNotification);
            if (!rows_.empty()) list_.selectRow(0);
        }
        updateMode();
        updateActions();
    }
    void updateMode() {
        const int mode = mode_.getSelectedId();
        for (auto* component : std::initializer_list<juce::Component*>{&match_, &tag_list_, &tag_hint_}) component->setVisible(mode == 1);
        for (auto* component : std::initializer_list<juce::Component*>{&cluster_size_, &group_, &representative_, &group_info_}) component->setVisible(mode == 2);
        random_.setVisible(mode == 3 && query_kind_ == QueryKind::Browse);
        back_.setVisible(query_kind_ != QueryKind::Browse);
        resized();
    }
    void rebuildGroups() {
        const int previous = group_.getSelectedId();
        group_.clear(juce::dontSendNotification);
        group_.addItem(utf8("すべての群"), 1);
        // Actual cluster IDs can include zero. Use id+2, reserving 1 for all.
        if (db_) for (const auto& group : db_->clusters()) {
            if (group.cluster_count == cluster_size_.getSelectedId())
                group_.addItem(utf8("グループ ") + juce::String(group.cluster_id) + " (" + juce::String(static_cast<int>(group.member_count)) + ")", group.cluster_id + 2);
        }
        group_.setSelectedId(previous > 0 && previous <= group_.getNumItems() ? previous : 1, juce::dontSendNotification);
        updateRepresentative();
    }
    [[nodiscard]] const engine::PatchSlideCluster* selectedGroup() const {
        if (!db_ || group_.getSelectedId() <= 1) return nullptr;
        for (const auto& group : db_->clusters())
            if (group.cluster_count == cluster_size_.getSelectedId() && group.cluster_id == group_.getSelectedId() - 2) return &group;
        return nullptr;
    }
    void updateRepresentative() {
        const auto* group = selectedGroup();
        representative_.setEnabled(group && !busy_ && query_kind_ == QueryKind::Browse
            && engine::PatchSlideDatabase::isVisible(db_->candidates()[group->representative_index], rejected_.getToggleState()));
        group_info_.setText(group && names_ ? utf8("DB指定の代表候補:\n") + (*names_)[group->representative_index] : utf8("16群と64群は独立した分類です。\n群を選ぶと代表候補を確認できます。"), juce::dontSendNotification);
    }
    void updateActions() {
        const bool selected = selection_.has_value() && !busy_;
        preview_.setEnabled(selected && static_cast<bool>(callbacks_.preview));
        apply_.setEnabled(selected && static_cast<bool>(callbacks_.apply));
        register_.setEnabled(selected && static_cast<bool>(callbacks_.registerTimbre));
        similar_.setEnabled(selected);
        variation_.setEnabled(selected);
        random_.setEnabled(!busy_ && !rows_.empty());
        list_.setEnabled(!busy_);
        updateRepresentative();
    }
    void selectCandidate(std::size_t index) {
        // The request already stopped its audition. Its asynchronous initial
        // selection must not stop notes begun elsewhere while it was pending.
        if (!adopting_result_) stopPreview();
        selected_index_ = index;
        std::string error;
        const auto& candidate = db_->candidates()[index];
        selection_ = engine::makePatchSlideTimbre(candidate, &error);
        juce::String text = (*names_)[index];
        text += "\n" + utf8(candidate.candidate_id.c_str());
        text += utf8("　AUTO_SAFE: ") + juce::String(candidate.auto_safe ? "Yes" : "No");
        text += utf8("　手動利用: ") + juce::String(candidate.manual_use_allowed ? "Yes" : "No");
        text += "\nvisibility: " + utf8(candidate.visibility.c_str()) + " / partition: " + utf8(candidate.predicted_partition.c_str());
        text += " / human: " + (candidate.human_pattern.empty() ? utf8("未判定") : utf8(candidate.human_pattern.c_str()));
        if ((candidate.human_pattern.empty() ? candidate.predicted_partition : candidate.human_pattern) == "ABBB")
            text += utf8("\n注意: ABBBは初回発声と以降の発声で音が異なる場合があります。");
        if (engine::PatchSlideDatabase::isRejected(candidate))
            text += utf8("\n判定: Reject（通常非表示）");
        text += utf8("\nタグ: ");
        for (std::size_t i = 0; i < candidate.tags.size(); ++i) {
            if (i != 0) text += " / ";
            text += utf8(candidate.tags[i].c_str());
        }
        if (candidate.tags.empty()) text += utf8("なし");
        if (!error.empty()) text += "\n" + utf8(error.c_str());
        details_.setText(text, false);
        updateActions();
    }
    void previewSelection() { if (selection_ && !busy_ && callbacks_.preview) callbacks_.preview(*selection_); }
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void selectedRowsChanged(int row) override {
        if (!updating_ && !busy_ && db_ && row >= 0 && row < getNumRows()) selectCandidate(rows_[static_cast<std::size_t>(row)]);
    }
    void returnKeyPressed(int) override { previewSelection(); }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { previewSelection(); }
    [[nodiscard]] static juce::String candidateStateText(const engine::PatchSlideCandidate& candidate) {
        auto state = engine::PatchSlideDatabase::isRejected(candidate) ? juce::String("Reject")
            : candidate.auto_safe ? juce::String("AUTO_SAFE")
            : candidate.manual_use_allowed ? utf8("手動利用候補") : utf8("利用判定を確認");
        state += " / " + utf8(candidate.visibility.c_str());
        if ((candidate.human_pattern.empty() ? candidate.predicted_partition : candidate.human_pattern) == "ABBB")
            state += utf8(" / ABBB: 初回発声差に注意");
        return state;
    }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (!db_ || !names_ || row < 0 || row >= getNumRows()) return;
        if (selected) g.fillAll(uiToggleOnFill());
        g.setColour(juce::Colours::white);
        g.setFont(UiFonts::body());
        const auto index = rows_[static_cast<std::size_t>(row)];
        const auto& candidate = db_->candidates()[index];
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(UiLayout::xs);
        g.drawFittedText((*names_)[index], area.removeFromTop(area.getHeight() / 2), juce::Justification::centredLeft, 1);
        g.drawFittedText(candidateStateText(candidate), area, juce::Justification::centredLeft, 1);
    }

    Callbacks callbacks_;
    SwitchLookAndFeel switch_style_;
    juce::ComboBox mode_, match_, cluster_size_, group_;
    juce::ToggleButton rejected_;
    juce::TextButton preview_, stop_, apply_, register_, similar_, variation_, random_, back_, reload_, representative_;
    juce::Label status_, tag_hint_, group_info_;
    juce::TextEditor details_;
    juce::ListBox list_, tag_list_;
    TagsModel tags_model_;
    std::shared_ptr<Worker> worker_;
    std::shared_ptr<const engine::PatchSlideDatabase> db_;
    std::shared_ptr<const std::vector<juce::String>> names_;
    std::vector<engine::PatchSlideTag> used_tags_;
    std::vector<std::size_t> rows_;
    std::vector<engine::PatchSlideSearchResult> ranks_;
    std::optional<std::size_t> selected_index_;
    std::optional<engine::CompositeTimbre> selection_;
    std::uint64_t generation_{};
    QueryKind query_kind_{QueryKind::Browse};
    std::size_t query_source_{};
    bool busy_{false}, updating_{false}, adopting_result_{false};
};

} // namespace mgstc::app
