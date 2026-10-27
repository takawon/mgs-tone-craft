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
#include <stdexcept>
#include <thread>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "editor_session.hpp"
#include "patchslide_stock.hpp"
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
        Callbacks callbacks, BackgroundTaskBoundary* background_tasks = nullptr,
        std::shared_ptr<PatchSlideStock> stock = {})
        : PatchSlideBrowserComponent(std::move(callbacks), background_tasks, {}, std::move(stock)) {}

    [[nodiscard]] static int minimumContentWidth() { return UiScale::sx(1100); }

private:
    friend class PatchSlideBrowserTestAccess;
    PatchSlideBrowserComponent(Callbacks callbacks, BackgroundTaskBoundary* background_tasks,
                              std::filesystem::path directory, std::shared_ptr<PatchSlideStock> stock = {})
        : callbacks_(std::move(callbacks)), tags_model_(*this), stock_model_(*this),
          stock_(stock ? std::move(stock) : std::make_shared<PatchSlideStock>()),
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
        two_changes_.setButtonText(utf8("2回変更"));
        two_changes_.setLookAndFeel(&switch_style_);
        two_changes_.setTooltip(utf8("OFF: 1回変更のみ / ON: 1回変更と2回変更"));
        two_changes_.onClick = rejected_.onClick;
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
        configure(reload_, "再読込", [this] { requestBrowse(true); });
        configure(representative_, "代表候補を試聴", [this] {
            if (busy_ || query_kind_ != QueryKind::Browse) return;
            if (const auto* group = selectedGroup()) {
                const auto row = std::find(rows_.begin(), rows_.end(), group->representative_index);
                if (row != rows_.end()) {
                    const auto index = static_cast<int>(std::distance(rows_.begin(), row));
                    list_.selectRow(index);
                    list_.scrollToEnsureRowIsOnscreen(index);
                    previewSelection();
                }
            }
        });
        list_.setModel(this);
        list_.setMultipleSelectionEnabled(false);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(UiLayout::panelFill));
        list_.setOutlineThickness(1);
        list_.setName(utf8("パッチスライド候補（Spaceでストック切替）"));
        list_.onSpace = [this] {
            const auto row = list_.getSelectedRow();
            if (db_ && row >= 0 && row < getNumRows()) toggleStock(db_->candidates()[rows_[static_cast<std::size_t>(row)]].candidate_id);
        };
        stock_list_.setModel(&stock_model_);
        stock_list_.setMultipleSelectionEnabled(false);
        stock_list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(UiLayout::panelFill));
        stock_list_.setColour(juce::ListBox::outlineColourId, juce::Colour(UiLayout::panelStroke));
        stock_list_.setOutlineThickness(1);
        stock_list_.setName(utf8("ストック（Deleteで解除）"));
        stock_list_.onDelete = [this] {
            const auto row = stock_list_.getSelectedRow();
            if (row >= 0 && static_cast<std::size_t>(row) < stock_ids_.size()) removeStock(stock_ids_[static_cast<std::size_t>(row)]);
        };
        list_.setColour(juce::ListBox::outlineColourId, juce::Colour(UiLayout::panelStroke));
        tag_list_.setModel(&tags_model_);
        tag_list_.setMultipleSelectionEnabled(true);
        tag_list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(UiLayout::panelFill));
        tag_hint_.setText(utf8("タグを複数選択: Ctrl / Shift"), juce::dontSendNotification);
        group_info_.setJustificationType(juce::Justification::topLeft);
        details_.setMultiLine(true, false);
        details_.setReadOnly(true);
        details_.setScrollbarsShown(true);
        details_.setCaretVisible(false);
        status_.setJustificationType(juce::Justification::centredLeft);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &mode_, &match_, &cluster_size_, &group_, &rejected_, &preview_, &stop_,
                 &apply_, &register_, &similar_, &variation_, &random_, &back_, &reload_,
                 &representative_, &list_, &stock_list_, &stock_header_, &two_changes_, &tag_list_, &tag_hint_, &group_info_, &details_, &status_})
            addAndMakeVisible(component);
        int focus = 100;
        for (auto* component : std::initializer_list<juce::Component*>{
                 &mode_, &match_, &tag_list_, &cluster_size_, &group_, &representative_,
                 &rejected_, &list_, &stock_list_, &random_, &similar_, &variation_, &two_changes_, &back_, &preview_,
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
        refreshStock();
        updateActions();
    }

public:
    ~PatchSlideBrowserComponent() override {
        stopTimer();
        stopPreview();
        worker_->cancel();
        list_.setModel(nullptr);
        stock_list_.setModel(nullptr);
        tag_list_.setModel(nullptr);
        rejected_.setLookAndFeel(nullptr);
        two_changes_.setLookAndFeel(nullptr);
        // Plugin ownership is retained by BackgroundTaskBoundary until its
        // non-audio shutdown joins the worker, including after editor closure.
    }

    [[nodiscard]] const std::optional<engine::CompositeTimbre>& selectedTimbre() const noexcept {
        return busy_ && selection_origin_ != SelectionOrigin::Stock ? no_selection_ : selection_;
    }
    [[nodiscard]] const engine::PatchSlideCandidate* selectedCandidate() const noexcept {
        return db_ && selected_index_ && (!busy_ || selection_origin_ == SelectionOrigin::Stock)
            ? &db_->candidates()[*selected_index_] : nullptr;
    }
    void stopPreview() { if (callbacks_.stop) callbacks_.stop(); }

    void visibilityChanged() override { if (!isShowing()) stopPreview(); }
    void paint(juce::Graphics& g) override { paintPageBackground(g, getLocalBounds()); }
    void resized() override {
        using namespace UiLayout;
        UiFonts::styleBodyField(details_);
        for (auto* label : {&status_, &tag_hint_, &group_info_, &stock_header_}) label->setFont(UiFonts::body());
        list_.setRowHeight(fieldH * 3);
        stock_list_.setRowHeight(fieldH * 3);
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
        auto left = area.removeFromLeft(libraryWidth);
        details_.setBounds(left.removeFromBottom(libraryMemoH + fieldH));
        left.removeFromBottom(sm);
        auto tags = left;
        match_.setBounds(tags.removeFromTop(fieldH));
        tags.removeFromTop(sm);
        tag_hint_.setBounds(tags.removeFromTop(fieldH));
        tags.removeFromTop(xs);
        tag_list_.setBounds(tags);
        auto groups = left;
        cluster_size_.setBounds(groups.removeFromTop(fieldH));
        groups.removeFromTop(sm);
        group_.setBounds(groups.removeFromTop(fieldH));
        groups.removeFromTop(sm);
        representative_.setBounds(groups.removeFromTop(textButtonH));
        groups.removeFromTop(sm);
        group_info_.setBounds(groups);
        random_.setBounds(left.removeFromTop(textButtonH));
        area.removeFromLeft(panelGap);
        auto stockArea = area.removeFromRight(juce::jmax(UiScale::sx(220), getWidth() / 3));
        area.removeFromRight(panelGap);
        stock_header_.setBounds(stockArea.removeFromTop(textButtonH));
        stockArea.removeFromTop(sm);
        stock_list_.setBounds(stockArea);
        auto first = area.removeFromTop(textButtonH);
        similar_.setBounds(first.removeFromLeft(compositeSelectorW));
        first.removeFromLeft(controlGap);
        variation_.setBounds(first.removeFromLeft(outputSwitchW));
        first.removeFromLeft(controlGap);
        const bool fits = first.getWidth() >= UiScale::sx(112);
        if (fits) two_changes_.setBounds(first.removeFromLeft(UiScale::sx(112)));
        area.removeFromTop(sm);
        auto second = area.removeFromTop(textButtonH);
        if (!fits) {
            two_changes_.setBounds(second.removeFromLeft(UiScale::sx(112)));
            second.removeFromLeft(controlGap);
        }
        back_.setBounds(second.removeFromLeft(juce::jmin(second.getWidth(), compositeSelectorW)));
        area.removeFromTop(sm);
        list_.setBounds(area);
    }

private:
    enum class QueryKind { Browse, StandardB, Variation };
    enum class SelectionOrigin { None, Candidate, Stock };
    struct Query {
        std::uint64_t generation{};
        QueryKind kind{QueryKind::Browse};
        engine::PatchSlideFilter filter;
        std::size_t source{};
        std::string source_id;
        bool reload{};
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
            if (pending && pending->reload) query.reload = true;
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
                    if (query.reload) { db.reset(); names.reset(); }
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
                            const auto source = db->find(query.source_id);
                            if (!source) throw std::runtime_error("The selected PatchSlide candidate is no longer available");
                            if (query.kind == QueryKind::StandardB)
                                result.ranks = db->standardB(*source, db->candidates().size() - 1);
                            else {
                                const auto variation = db->variation(*source, query.filter.max_change_count);
                                for (const auto& row : variation.candidates) result.ranks.push_back(row);
                            }
                            for (const auto& row : result.ranks) {
                                if (db->candidates()[row.candidate_index].change_count > query.filter.max_change_count) continue;
                                ++result.total;
                                appendVisible(result, row.candidate_index, query.filter.show_rejected);
                            }
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

    class KeyboardList final : public juce::ListBox {
    public:
        std::function<void()> onSpace, onDelete;
        bool keyPressed(const juce::KeyPress& key) override {
            if (key == juce::KeyPress::spaceKey && onSpace) { onSpace(); return true; }
            if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && onDelete) { onDelete(); return true; }
            return juce::ListBox::keyPressed(key);
        }
    };
    class TrashButton final : public juce::Button {
    public:
        TrashButton() : juce::Button(utf8("ストックから削除")) {}
        void paintButton(juce::Graphics& g, bool over, bool down) override {
            g.setColour(over || down || hasKeyboardFocus(true) ? juce::Colour(kUiHoverAccent) : juce::Colours::lightgrey);
            auto box = getLocalBounds().toFloat().reduced(static_cast<float>(UiLayout::sm));
            g.drawLine(box.getX(), box.getY() + box.getHeight() * 0.2f, box.getRight(), box.getY() + box.getHeight() * 0.2f, 1.5f);
            g.drawRect(box.reduced(box.getWidth() * 0.15f, 0).withTrimmedTop(box.getHeight() * 0.3f), 1.5f);
            g.drawLine(box.getCentreX() - box.getWidth() * 0.15f, box.getY(), box.getCentreX() + box.getWidth() * 0.15f, box.getY(), 1.5f);
        }
    };
    class RowControls final : public juce::Component {
    public:
        RowControls() { setInterceptsMouseClicks(false, true); addAndMakeVisible(check); addAndMakeVisible(trash); }
        void bind(PatchSlideBrowserComponent& owner, const std::string& id, bool stockRow) {
            check.setVisible(!stockRow);
            trash.setVisible(stockRow);
            check.setToggleState(owner.stock_->contains(id), juce::dontSendNotification);
            check.setTitle(utf8("ストック: ") + utf8(id.c_str()));
            check.setTooltip(utf8("ストックに追加／解除。行選択とは独立した操作です。"));
            trash.setTooltip(utf8("一時ストックから解除します。DBやライブラリは変更しません。"));
            juce::Component::SafePointer<PatchSlideBrowserComponent> safe(&owner);
            check.onClick = [safe, id] { if (safe) safe->toggleStock(id); };
            trash.onClick = [safe, id] { if (safe) safe->removeStock(id); };
        }
        void resized() override {
            check.setBounds(0, 0, UiLayout::fieldH, getHeight());
            trash.setBounds(getWidth() - UiLayout::fieldH, (getHeight() - UiLayout::fieldH) / 2, UiLayout::fieldH, UiLayout::fieldH);
        }
        juce::ToggleButton check;
        TrashButton trash;
    };
    class StockModel final : public juce::ListBoxModel {
    public:
        explicit StockModel(PatchSlideBrowserComponent& owner) : owner_(owner) {}
        int getNumRows() override { return static_cast<int>(owner_.stock_ids_.size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
            if (row < 0 || row >= getNumRows()) return;
            const auto& id = owner_.stock_ids_[static_cast<std::size_t>(row)];
            if (selected && owner_.selection_origin_ == SelectionOrigin::Stock) g.fillAll(uiToggleOnFill());
            g.setColour(juce::Colours::white);
            g.setFont(UiFonts::body());
            auto area = juce::Rectangle<int>(0, 0, width - UiLayout::fieldH, height).reduced(UiLayout::xs);
            const auto index = owner_.db_ ? owner_.db_->find(id) : std::optional<std::size_t>{};
            const auto name = index && owner_.names_ ? (*owner_.names_)[*index] : utf8(id.c_str());
            g.drawFittedText(name, area.removeFromTop(area.getHeight() / 2), juce::Justification::centredLeft, 2, 1.0f);
            g.drawFittedText(index ? candidateStateText(owner_.db_->candidates()[*index]) : utf8("参照不可・再読込で確認"),
                area, juce::Justification::centredLeft, 2, 1.0f);
        }
        juce::Component* refreshComponentForRow(int row, bool, juce::Component* component) override {
            if (row < 0 || row >= getNumRows()) { delete component; return nullptr; }
            auto* controls = dynamic_cast<RowControls*>(component);
            if (!controls) { delete component; controls = new RowControls; }
            controls->bind(owner_, owner_.stock_ids_[static_cast<std::size_t>(row)], true);
            return controls;
        }
        void selectedRowsChanged(int row) override {
            if (owner_.updating_) return;
            if (row < 0) {
                if (owner_.selection_origin_ == SelectionOrigin::Stock) owner_.clearSelection();
                return;
            }
            if (row >= getNumRows()) return;
            const auto id = owner_.stock_ids_[static_cast<std::size_t>(row)];
            if (const auto index = owner_.db_ ? owner_.db_->find(id) : std::optional<std::size_t>{}) owner_.selectCandidate(*index, true);
            else {
                owner_.clearSelection();
                const juce::ScopedValueSetter<bool> updating(owner_.updating_, true);
                owner_.stock_list_.selectRow(row);
                owner_.details_.setText(utf8(id.c_str()) + utf8("\n参照不可。再読込で確認するか、ストックから解除してください。"), false);
            }
        }
        void returnKeyPressed(int) override { owner_.previewSelection(); }
        void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { owner_.previewSelection(); }
    private:
        PatchSlideBrowserComponent& owner_;
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
    void clearSelection() {
        if (selection_ && !adopting_result_) stopPreview();
        selection_.reset();
        selected_index_.reset();
        active_id_.clear();
        selection_origin_ = SelectionOrigin::None;
        details_.clear();
        const juce::ScopedValueSetter<bool> updating(updating_, true);
        list_.deselectAllRows();
        stock_list_.deselectAllRows();
        updateActions();
    }
    void reconcileSelection() {
        if (selection_origin_ == SelectionOrigin::None) return;
        const auto index = db_ ? db_->find(active_id_) : std::optional<std::size_t>{};
        if (!index) { clearSelection(); return; }
        if (selection_origin_ == SelectionOrigin::Candidate) {
            const auto row = std::find(rows_.begin(), rows_.end(), *index);
            if (row == rows_.end()) { clearSelection(); return; }
            {
                const juce::ScopedValueSetter<bool> updating(updating_, true);
                list_.selectRow(static_cast<int>(std::distance(rows_.begin(), row)));
            }
            selectCandidate(*index);
        } else {
            if (!stock_->contains(active_id_)) { clearSelection(); return; }
            selectCandidate(*index, true);
        }
    }
    void refreshStock() {
        const auto selected_row = stock_list_.getSelectedRow();
        const auto previous_id = selected_row >= 0 && static_cast<std::size_t>(selected_row) < stock_ids_.size()
            ? stock_ids_[static_cast<std::size_t>(selected_row)] : std::string{};
        stock_revision_ = stock_->revision();
        stock_ids_ = stock_->ids();
        {
            const juce::ScopedValueSetter<bool> updating(updating_, true);
            stock_list_.deselectAllRows();
            stock_list_.updateContent();
            list_.updateContent();
            if (selection_origin_ == SelectionOrigin::Stock) {
                const auto row = std::find(stock_ids_.begin(), stock_ids_.end(), active_id_);
                if (row != stock_ids_.end()) stock_list_.selectRow(static_cast<int>(std::distance(stock_ids_.begin(), row)));
            }
        }
        stock_header_.setText(utf8("ストック: ") + juce::String(static_cast<int>(stock_ids_.size())), juce::dontSendNotification);
        if (selection_origin_ == SelectionOrigin::Stock && !stock_->contains(active_id_)) clearSelection();
        if (selection_origin_ == SelectionOrigin::None && !previous_id.empty() && !stock_->contains(previous_id)) details_.clear();
        stock_list_.repaint();
        list_.repaint();
    }
    void toggleStock(std::string id) {
        if (stock_->contains(id)) removeStock(std::move(id));
        else if (db_ && db_->find(id)) { stock_->add(id); refreshStock(); }
    }
    void removeStock(std::string id) {
        stock_->remove(id);
        refreshStock();
    }
    void requestBrowse(bool reload = false) { request(QueryKind::Browse, 0, reload); }
    void request(QueryKind kind, std::size_t source, bool reload = false) {
        if (worker_->cancelled.load(std::memory_order_acquire)) return;
        stopPreview();
        busy_ = true;
        query_kind_ = kind;
        query_source_ = source;
        Query query;
        query.generation = ++generation_;
        query.kind = kind;
        query.source = source;
        if (db_ && source < db_->candidates().size()) query.source_id = db_->candidates()[source].candidate_id;
        query.reload = reload;
        query.filter.show_rejected = rejected_.getToggleState();
        query.filter.max_change_count = two_changes_.getToggleState() ? 2 : 1;
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
        if (selection_origin_ != SelectionOrigin::Stock) details_.clear();
        updateActions();
        worker_->submit(std::move(query));
    }
    void timerCallback() override {
        if (stock_revision_ != stock_->revision()) refreshStock();
        std::optional<Result> result;
        {
            std::lock_guard lock(worker_->mutex);
            result.swap(worker_->ready);
        }
        if (!result || result->generation != generation_) return;
        const juce::ScopedValueSetter<bool> adopting(adopting_result_, true);
        busy_ = false;
        const bool catalog_changed = db_ != result->db;
        std::vector<std::string> selected_tags;
        const auto tag_rows = tag_list_.getSelectedRows();
        for (int i = 0; i < tag_rows.size(); ++i)
            if (tag_rows[i] >= 0 && static_cast<std::size_t>(tag_rows[i]) < used_tags_.size())
                selected_tags.push_back(used_tags_[static_cast<std::size_t>(tag_rows[i])].tag);
        db_ = std::move(result->db);
        names_ = std::move(result->names);
        rows_ = std::move(result->rows);
        ranks_ = std::move(result->ranks);
        updating_ = true;
        list_.deselectAllRows();
        list_.updateContent();
        if (catalog_changed) {
            used_tags_.clear();
            if (db_) for (const auto& tag : db_->tags()) if (tag.candidate_count > 0) used_tags_.push_back(tag);
            tag_list_.deselectAllRows();
            tag_list_.updateContent();
            for (std::size_t i = 0; i < used_tags_.size(); ++i)
                if (std::find(selected_tags.begin(), selected_tags.end(), used_tags_[i].tag) != selected_tags.end())
                    tag_list_.selectRow(static_cast<int>(i), true, false);
            rebuildGroups();
        }
        updating_ = false;
        if (!result->error.empty()) {
            clearSelection();
            status_.setText(utf8("パッチスライドを利用できません"), juce::dontSendNotification);
            details_.setText(utf8(result->error.c_str()) + utf8("\n正しい3ファイルを配置後、再読込またはMGSTCを再起動してください。"), false);
        } else {
            auto text = query_kind_ == QueryKind::Variation ? utf8("Variation（選択順を保持）: ")
                : query_kind_ == QueryKind::StandardB ? utf8("Standard B（距離順位）: ") : utf8("探索: ");
            text += utf8("表示 ") + juce::String(static_cast<int>(rows_.size())) + " / " + juce::String(static_cast<int>(result->total));
            text += utf8("　除外 Reject ") + juce::String(static_cast<int>(result->rejected));
            text += utf8(" / 表示不可 ") + juce::String(static_cast<int>(result->other_hidden));
            status_.setText(text, juce::dontSendNotification);
        }
        if (result->error.empty()) reconcileSelection();
        refreshStock();
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
            && std::find(rows_.begin(), rows_.end(), group->representative_index) != rows_.end());
        group_info_.setText(group && names_ ? utf8("DB指定の代表候補:\n") + (*names_)[group->representative_index] : utf8("16群と64群は独立した分類です。\n群を選ぶと代表候補を確認できます。"), juce::dontSendNotification);
    }
    void updateActions() {
        const bool selected = selection_.has_value() && (!busy_ || selection_origin_ == SelectionOrigin::Stock);
        preview_.setEnabled(selected && static_cast<bool>(callbacks_.preview));
        apply_.setEnabled(selected && static_cast<bool>(callbacks_.apply));
        register_.setEnabled(selected && static_cast<bool>(callbacks_.registerTimbre));
        similar_.setEnabled(selected);
        variation_.setEnabled(selected);
        random_.setEnabled(!busy_ && !rows_.empty());
        list_.setEnabled(!busy_);
        updateRepresentative();
    }
    void selectCandidate(std::size_t index, bool from_stock = false) {
        if (!db_ || index >= db_->candidates().size() || !db_->candidates()[index].enabled) {
            clearSelection();
            return;
        }
        // The request already stopped its audition. Its asynchronous initial
        // selection must not stop notes begun elsewhere while it was pending.
        if (!adopting_result_) stopPreview();
        selected_index_ = index;
        std::string error;
        const auto& candidate = db_->candidates()[index];
        active_id_ = candidate.candidate_id;
        selection_origin_ = from_stock ? SelectionOrigin::Stock : SelectionOrigin::Candidate;
        {
            const juce::ScopedValueSetter<bool> updating(updating_, true);
            if (from_stock) list_.deselectAllRows(); else stock_list_.deselectAllRows();
        }
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
    void previewSelection() { if (selection_ && (!busy_ || selection_origin_ == SelectionOrigin::Stock) && callbacks_.preview) callbacks_.preview(*selection_); }
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void selectedRowsChanged(int row) override {
        if (!updating_ && row < 0 && selection_origin_ == SelectionOrigin::Candidate) { clearSelection(); return; }
        if (!updating_ && !busy_ && db_ && row >= 0 && row < getNumRows()) selectCandidate(rows_[static_cast<std::size_t>(row)]);
    }
    juce::Component* refreshComponentForRow(int row, bool, juce::Component* component) override {
        if (!db_ || row < 0 || row >= getNumRows()) { delete component; return nullptr; }
        auto* controls = dynamic_cast<RowControls*>(component);
        if (!controls) { delete component; controls = new RowControls; }
        controls->bind(*this, db_->candidates()[rows_[static_cast<std::size_t>(row)]].candidate_id, false);
        return controls;
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
        area.removeFromLeft(UiLayout::fieldH);
        g.drawFittedText((*names_)[index], area.removeFromTop(area.getHeight() / 2), juce::Justification::centredLeft, 2, 1.0f);
        g.drawFittedText(candidateStateText(candidate), area, juce::Justification::centredLeft, 2, 1.0f);
    }

    Callbacks callbacks_;
    SwitchLookAndFeel switch_style_;
    juce::ComboBox mode_, match_, cluster_size_, group_;
    juce::ToggleButton rejected_, two_changes_;
    juce::TextButton preview_, stop_, apply_, register_, similar_, variation_, random_, back_, reload_, representative_;
    juce::Label status_, tag_hint_, group_info_, stock_header_;
    juce::TextEditor details_;
    KeyboardList list_, stock_list_;
    juce::ListBox tag_list_;
    TagsModel tags_model_;
    StockModel stock_model_;
    std::shared_ptr<PatchSlideStock> stock_;
    std::vector<std::string> stock_ids_;
    std::uint64_t stock_revision_{};
    std::shared_ptr<Worker> worker_;
    std::shared_ptr<const engine::PatchSlideDatabase> db_;
    std::shared_ptr<const std::vector<juce::String>> names_;
    std::vector<engine::PatchSlideTag> used_tags_;
    std::vector<std::size_t> rows_;
    std::vector<engine::PatchSlideSearchResult> ranks_;
    std::optional<std::size_t> selected_index_;
    std::optional<engine::CompositeTimbre> selection_;
    const std::optional<engine::CompositeTimbre> no_selection_;
    SelectionOrigin selection_origin_{SelectionOrigin::None};
    std::string active_id_;
    std::uint64_t generation_{};
    QueryKind query_kind_{QueryKind::Browse};
    std::size_t query_source_{};
    bool busy_{false}, updating_{false}, adopting_result_{false};
};

} // namespace mgstc::app
