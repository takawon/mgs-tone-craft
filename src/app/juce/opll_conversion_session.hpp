// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "opll_conversion_dialog.hpp"
#include "opll_conversion_task.hpp"

namespace mgstc::app {

class OpllConversionSession final {
public:
    using Candidate = OpllConversionTask::Candidate;
    using Original = OpllConversionTask::Original;
    using Audition = OpllConversionDialog::Audition;

    struct Target {
        std::function<bool()> valid;
        std::function<bool(Original)> listen_original;
        std::function<bool(const engine::OpllContinuousCandidate&)> listen_candidate;
        std::function<std::uint8_t()> preview_note;
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
              .select_candidate = [this](auto row) { selectCandidate(row); },
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
                acceptCandidate(update->best);
                started_ = update->started;
                if (update->completion) finish(*update->completion, update->error);
            }
        }
        if (!closed_) refresh();
    }

private:
    friend struct OpllConversionSessionTestAccess;

    [[nodiscard]] bool active() const { return !closed_ && target_.valid(); }

    [[nodiscard]] std::optional<std::size_t> selectedRow() const {
        if (history_.empty()) return std::nullopt;
        return selected_.value_or(history_.size() - 1);
    }

    [[nodiscard]] Candidate selectedCandidate() const {
        const auto row = selectedRow();
        return row ? history_[*row] : nullptr;
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

    void acceptCandidate(const Candidate& candidate) {
        if (finished_ || closed_ || !candidate || candidate == last_received_) return;
        last_received_ = candidate;
        if (candidate->full_evaluation_index == 0 || !std::isfinite(candidate->full_score)
            || std::any_of(history_.begin(), history_.end(), [&](const auto& entry) {
                return entry->registers == candidate->registers;
            })) return;
        history_.push_back(candidate);
    }

    void selectCandidate(std::size_t row) {
        if (!active() || row >= history_.size() || selected_ == row) return;
        if (selectedRow() != row) stopAudition();
        selected_ = row;
        refresh();
    }

    void finish(engine::OpllContinuousCompletion completion, const std::string& error) {
        using Completion = engine::OpllContinuousCompletion;
        if (completion != Completion::TimedOut || history_.empty()) {
            auto message = juce::String::fromUTF8("適用できる候補が得られなかったため、元の音色を保持しました");
            if (!error.empty()) message += "\n" + juce::String::fromUTF8(error.c_str());
            close(message);
            return;
        }
        if (history_.size() == 1) {
            apply();
            return;
        }
        finished_ = true;
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
        state.status = finished_ ? juce::String::fromUTF8("探索を終了しました。音色を選んでください。")
            : started_ == std::chrono::steady_clock::time_point{}
                ? juce::String::fromUTF8("原音を準備しています…")
                : juce::String::fromUTF8("音色を探しています…");
        const auto candidate = selectedCandidate();
        state.original_available = static_cast<bool>(original_);
        state.candidate_available = state.can_apply = static_cast<bool>(candidate);
        state.can_stop_audition = audition_ != Audition::None;
        state.audition = audition_;
        state.candidate_count = history_.size();
        state.selected_row = selectedRow();
        if (candidate) {
            const auto audition_note = target_.preview_note ? target_.preview_note()
                : note_.value_or(60);
            state.details = OpllConversionDialog::Details{
                candidate->registers, candidate->fixed_midi_note.value_or(audition_note)};
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
    Candidate last_received_;
    std::vector<Candidate> history_;
    std::optional<std::size_t> selected_;
    Audition audition_{Audition::None};
    std::chrono::steady_clock::time_point started_{};
    bool finished_{};
    bool closed_{};
    bool keyboard_audition_{};
    bool preview_installed_{};
};

} // namespace mgstc::app
