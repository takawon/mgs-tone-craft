// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "opll_conversion_dialog.hpp"
#include "opll_conversion_task.hpp"

namespace mgstc::app {

class OpllConversionSession final {
public:
    using Candidate = OpllConversionTask::Candidate;
    using Original = OpllConversionTask::Original;
    using StockId = OpllConversionDialog::StockId;
    using Audition = OpllConversionDialog::Audition;

    struct Target {
        std::function<bool()> valid;
        std::function<bool(Original)> listen_original;
        std::function<bool(const engine::OpllContinuousCandidate&)> listen_candidate;
        std::function<void(bool)> stop;
        std::function<bool(const engine::OpllPatchParameters&)> apply;
        std::function<void(const juce::String&)> status;
    };

    OpllConversionSession(juce::Component& anchor, BackgroundTaskBoundary& tasks,
                          std::shared_ptr<OpllConversionReservation> reservation,
                          OpllConversionTask::Prepare prepare,
                          std::optional<std::uint8_t> note, Target target)
        : tasks_(tasks), reservation_(std::move(reservation)),
          prepare_(std::move(prepare)), note_(note), target_(std::move(target)),
          dialog_(OpllConversionDialog::Callbacks{
              .start = [this] { start(); },
              .listen_original = [this] { listenOriginal(); },
              .listen_candidate = [this] { listenCandidate(); },
              .stop_audition = [this] { stopAudition(); },
              .add_stock = [this] { addStock(); },
              .select_stock = [this](auto id) { selectStock(id); },
              .delete_stock = [this](auto id) { deleteStock(id); },
              .apply = [this] { apply(); },
              .cancel = [this] { close(juce::String::fromUTF8("変換をキャンセルしました")); },
          }) {
        refresh();
        dialog_.showFor(anchor);
    }

    ~OpllConversionSession() {
        if (task_) task_->cancel();
        stopAudition();
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] OpllConversionDialog& dialog() noexcept { return dialog_; }
    void authorChanged() {
        close(juce::String::fromUTF8("編集中の音色が変更されたため、変換を終了しました"));
    }

    [[nodiscard]] Candidate keyboardCandidate() const {
        return active() ? selectedCandidate() : nullptr;
    }

    bool beginKeyboardAudition() {
        if (!active()) return false;
        if (!keyboard_audition_) stopAudition(false);
        keyboard_audition_ = static_cast<bool>(selectedCandidate());
        if (keyboard_audition_) {
            audition_ = Audition::Candidate;
            preview_installed_ = true;
        }
        return true;
    }

    void keyboardEnded() {
        if (!keyboard_audition_) return;
        keyboard_audition_ = false;
        audition_ = Audition::None;
    }

    void auditionEnded() { stopAudition(); }

    void poll() {
        if (closed_) return;
        if (!active()) {
            authorChanged();
            return;
        }
        if (task_ && !finished_) {
            if (const auto update = task_->poll()) {
                original_ = update->original;
                best_ = update->best;
                started_ = update->started;
                if (update->completion) finish(*update->completion, update->error);
            }
        }
        if (!closed_) refresh();
    }

private:
    struct Stock {
        StockId id{};
        Candidate candidate;
        bool same_timbre{};
    };

    [[nodiscard]] bool active() const { return !closed_ && target_.valid(); }

    [[nodiscard]] Candidate selectedCandidate() const {
        for (const auto& stock : stocks_)
            if (stock.id == selected_) return stock.candidate;
        return finished_ ? nullptr : best_;
    }

    void start() {
        if (!active() || task_) return;
        auto task = std::make_shared<OpllConversionTask>();
        if (!tasks_.registerTask(task)) {
            close(juce::String::fromUTF8("ほかの処理が終了してから変換してください"));
            return;
        }
        if (!task->start(std::move(prepare_), note_, reservation_)) {
            task->join();
            close(juce::String::fromUTF8("変換を開始できませんでした"));
            return;
        }
        task_ = std::move(task);
        refresh();
    }

    void stopAudition(bool release_keyboard = true) {
        if (!preview_installed_) return;
        preview_installed_ = false;
        audition_ = Audition::None;
        keyboard_audition_ = false;
        target_.stop(release_keyboard);
    }

    void listenOriginal() {
        if (!active() || !original_) return;
        stopAudition();
        if (target_.listen_original(original_)) {
            audition_ = Audition::Original;
            preview_installed_ = true;
        }
        refresh();
    }

    void listenCandidate() {
        if (!active()) return;
        const auto candidate = selectedCandidate();
        if (!candidate) return;
        stopAudition();
        if (target_.listen_candidate(*candidate)) {
            audition_ = Audition::Candidate;
            preview_installed_ = true;
        }
        refresh();
    }

    void addStock() {
        if (!active() || finished_ || stocks_.size() >= 16) return;
        auto candidate = selectedCandidate();
        if (!candidate || hasRegisters(*candidate)) return;
        stocks_.push_back({next_id_++, std::move(candidate)});
        refresh();
    }

    [[nodiscard]] bool hasRegisters(const engine::OpllContinuousCandidate& candidate) const {
        return std::any_of(stocks_.begin(), stocks_.end(), [&](const auto& stock) {
            return stock.candidate->registers == candidate.registers;
        });
    }

    void selectStock(std::optional<StockId> id) {
        if (!active() || selected_ == id) return;
        stopAudition();
        selected_ = id;
        refresh();
    }

    void deleteStock(StockId id) {
        if (!active()) return;
        if (selected_ == id) stopAudition();
        std::erase_if(stocks_, [id](const auto& stock) { return stock.id == id; });
        if (selected_ == id) {
            selected_.reset();
            if (finished_ && !stocks_.empty()) selected_ = stocks_.front().id;
        }
        refresh();
    }

    void finish(engine::OpllContinuousCompletion completion, const std::string& error) {
        using Completion = engine::OpllContinuousCompletion;
        if (completion != Completion::TimedOut || !best_) {
            auto message = juce::String::fromUTF8("適用できる候補が得られなかったため、元の音色を保持しました");
            if (!error.empty()) message += "\n" + juce::String::fromUTF8(error.c_str());
            close(message);
            return;
        }
        if (stocks_.empty()) {
            apply();
            return;
        }
        if (stocks_.size() == 1) {
            const bool same = hasRegisters(*best_);
            stocks_.front().same_timbre = same;
            stocks_.push_back({next_id_++, best_, same});
        }
        finished_ = true;
        if (!selected_) selected_ = stocks_.front().id;
    }

    void apply() {
        if (!active()) return;
        const auto candidate = selectedCandidate();
        if (!candidate) return;
        if (task_) task_->cancel();
        // The target stops its audition as part of commit so plugin program
        // activation need not reserve a separate intermediate restore slot.
        audition_ = Audition::None;
        keyboard_audition_ = false;
        preview_installed_ = false;
        close(target_.apply(candidate->patch)
            ? juce::String::fromUTF8("変換した音色を適用しました")
            : juce::String::fromUTF8("変換した音色を適用できませんでした"));
    }

    void close(const juce::String& message) {
        if (closed_) return;
        closed_ = true;
        if (task_) task_->cancel();
        stopAudition();
        dialog_.setVisible(false);
        target_.status(message);
    }

    void refresh() {
        OpllConversionDialog::State state;
        state.confirming = !task_;
        state.running = task_ && !finished_;
        if (started_ != std::chrono::steady_clock::time_point{}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_);
            state.elapsed_seconds = static_cast<double>(std::min(elapsed, std::chrono::seconds{300}).count());
        }
        state.status = finished_ ? juce::String::fromUTF8("ストックから音色を選んでください")
            : started_ == std::chrono::steady_clock::time_point{}
                ? juce::String::fromUTF8("原音を準備しています…")
                : juce::String::fromUTF8("音色を探しています…");
        const auto candidate = selectedCandidate();
        state.original_available = static_cast<bool>(original_);
        state.candidate_available = state.can_apply = static_cast<bool>(candidate);
        state.can_add_stock = candidate && !finished_ && stocks_.size() < 16 && !hasRegisters(*candidate);
        state.can_delete_stock = selected_.has_value();
        state.can_stop_audition = audition_ != Audition::None;
        state.audition = audition_;
        state.selected_stock = selected_;
        for (const auto& stock : stocks_) {
            state.stocks.push_back({stock.id, juce::String::fromUTF8("ストック ")
                + juce::String(static_cast<juce::int64>(stock.id)), {}, stock.same_timbre});
        }
        if (displayed_ != state) {
            dialog_.setState(state);
            displayed_ = std::move(state);
        }
    }

    BackgroundTaskBoundary& tasks_;
    std::shared_ptr<OpllConversionReservation> reservation_;
    OpllConversionTask::Prepare prepare_;
    std::optional<std::uint8_t> note_;
    Target target_;
    OpllConversionDialog dialog_;
    std::optional<OpllConversionDialog::State> displayed_;
    std::shared_ptr<OpllConversionTask> task_;
    Original original_;
    Candidate best_;
    std::vector<Stock> stocks_;
    std::optional<StockId> selected_;
    StockId next_id_{1};
    Audition audition_{Audition::None};
    std::chrono::steady_clock::time_point started_{};
    bool finished_{};
    bool closed_{};
    bool keyboard_audition_{};
    bool preview_installed_{};
};

} // namespace mgstc::app
