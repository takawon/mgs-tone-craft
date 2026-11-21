// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "editor_session.hpp"
#include "mgstc/engine/composite_wav_analysis.hpp"
#include "mgstc/engine/wave_import.hpp"

#include <mutex>
#include <thread>

namespace mgstc::app {

// The session's task boundary owns the worker independently of the editor.
// Only PCM and immutable results cross between the worker and message thread.
class OpllConversionTask final : public EditorBackgroundTask {
public:
    using Candidate = std::shared_ptr<const engine::OpllContinuousCandidate>;
    using Original = std::shared_ptr<const engine::SourcePcm>;
    using Prepare = std::function<bool(engine::WavePcm&, Original&, std::string&)>;

    struct Update {
        Original original;
        Candidate best;
        std::optional<engine::OpllContinuousCompletion> completion;
        std::string error;
        std::chrono::steady_clock::time_point started{};
    };

    ~OpllConversionTask() override { join(); }

    bool start(Prepare prepare, std::optional<std::uint8_t> note,
               std::shared_ptr<OpllConversionReservation> reservation) {
        std::lock_guard lock(lifetime_mutex_);
        if (worker_.joinable() || control_->cancelRequested()) return false;
        try {
            worker_ = std::jthread([this, prepare = std::move(prepare), note,
                                    reservation = std::move(reservation)]() mutable {
                run(prepare, note);
                reservation.reset();
                complete_.store(true, std::memory_order_release);
            });
        } catch (const std::system_error&) {
            complete_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    void cancel() noexcept override { control_->requestCancel(); }

    void join() noexcept override {
        std::lock_guard lock(lifetime_mutex_);
        cancel();
        if (worker_.joinable()) worker_.join();
        complete_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool completed() const noexcept override {
        return complete_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<Update> poll() {
        std::unique_lock lock(inbox_mutex_, std::try_to_lock);
        if (!lock) return std::nullopt;
        return update_;
    }

    static bool decodeWave(std::span<const std::uint8_t> bytes,
                           engine::WavePcm& pcm, Original& original, std::string& error) {
        if (!engine::parseWavePcm(bytes, pcm, &error)) return false;
        auto decoded = std::make_shared<engine::SourcePcm>();
        // Preserve the OPLL importer's WAV acceptance even when the stricter
        // stereo decoder cannot retain the original channel layout.
        if (!engine::parseCompositeWavePcm(bytes, *decoded)) {
            decoded->sample_rate = pcm.sample_rate;
            decoded->channels = 1;
            decoded->bit_depth = 32;
            decoded->sample_format = engine::SourceSampleFormat::FloatPcm;
            decoded->mono_samples = pcm.mono_samples;
            decoded->interleaved_samples = pcm.mono_samples;
        }
        original = std::move(decoded);
        return true;
    }

private:
    void finish(engine::OpllContinuousCompletion completion, std::string error) {
        std::lock_guard lock(inbox_mutex_);
        update_.completion = completion;
        update_.error = std::move(error);
    }

    void run(const Prepare& prepare, std::optional<std::uint8_t> note) {
        try {
            engine::WavePcm pcm;
            Original original;
            std::string error;
            if (control_->cancelRequested()) {
                finish(engine::OpllContinuousCompletion::Cancelled, {});
                return;
            }
            if (!prepare(pcm, original, error)) {
                finish(engine::OpllContinuousCompletion::Failed, std::move(error));
                return;
            }
            {
                std::lock_guard lock(inbox_mutex_);
                update_.original = std::move(original);
                update_.started = std::chrono::steady_clock::now();
            }
            engine::OpllContinuousOptions options;
            options.control = control_;
            options.fixed_midi_note = note;
            options.candidate_ready = [this](Candidate best) {
                std::lock_guard lock(inbox_mutex_);
                update_.best = std::move(best);
            };
            auto result = engine::approximateWavePcmContinuouslyWithOpll(pcm, options);
            {
                std::lock_guard lock(inbox_mutex_);
                if (!result.ranked.empty()) update_.best = std::move(result.ranked.front());
                update_.completion = result.completion;
                update_.error = std::move(result.error);
            }
        } catch (...) {
            finish(engine::OpllContinuousCompletion::Failed, {});
        }
    }

    std::shared_ptr<engine::OpllApproximationControl> control_{
        std::make_shared<engine::OpllApproximationControl>()};
    std::mutex lifetime_mutex_;
    std::mutex inbox_mutex_;
    Update update_;
    std::atomic<bool> complete_{false};
    // Joins before the inbox and cancellation state are destroyed.
    std::jthread worker_;
};

} // namespace mgstc::app
