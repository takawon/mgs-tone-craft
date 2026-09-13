// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "spectrogram_emphasis.hpp"
#include "mgstc/engine/spectrum_capture.hpp"

namespace mgstc::app::spectrum {
constexpr std::size_t fftSize = 2048, hop = 512, bins = fftSize / 2 + 1;
constexpr std::size_t channels = engine::kSpectrumChannels;
constexpr double sampleRate = 48000.0, minFrequency = 30.0, maxFrequency = 24000.0;
constexpr float minDb = -100.0F;
constexpr std::size_t historyCapacity = static_cast<std::size_t>(5.0 * sampleRate / hop) + 3;
constexpr std::size_t historyLines = 24;
inline float decibels(float power) noexcept {
    return power > 0 && std::isfinite(power) ? 10.0F * std::log10(power) : -160.0F;
}
inline double frequencyU(double frequency) noexcept {
    return std::log(frequency / minFrequency) / std::log(maxFrequency / minFrequency);
}
inline double frequencyAt(double u) noexcept {
    return minFrequency * std::pow(maxFrequency / minFrequency, std::clamp(u, 0.0, 1.0));
}
struct Peak { float frequency{}, db{}; };
struct Trace {
    // Calibrated one-sided squared peak amplitude; not band power or PSD.
    std::array<float, bins> power{};
    std::array<Peak, fftSize / 4> peaks{};
    std::uint16_t peak_count{};
    bool audible{};
};
inline void findPeaks(Trace& trace) noexcept {
    trace.peak_count = 0;
    trace.audible = false;
    for (float p : trace.power) trace.audible |= p >= 1.0e-10F;
    for (std::size_t i = 1; i + 1 < bins; ++i) {
        if (!(trace.power[i] > trace.power[i - 1]
            && trace.power[i] >= trace.power[i + 1] && trace.power[i] >= 1.0e-10F)) continue;
        const auto peak = spectrogram::interpolatePeak(trace.power, i);
        const auto f = peak.bin * sampleRate / fftSize;
        if (f < minFrequency || f >= maxFrequency) continue;
        if (trace.peak_count < trace.peaks.size())
            trace.peaks[trace.peak_count++] = {static_cast<float>(f), decibels(peak.power)};
    }
}
struct FrameInfo {
    std::uint64_t id{}, sample{}, context{}, pcm_epoch{};
    engine::SpectrumGuideNote guide{};
    bool simulated_remote{};
};
struct Column {
    std::array<std::uint8_t, 256> psg{}, scc{}, opll{};
    spectrogram::HarmonicPeakList psg_harmonics{}, scc_harmonics{}, opll_harmonics{};
    std::uint64_t sequence{}, sample_position{}, context{};
    std::uint8_t guide_note{60}, guide_track{};
    bool note_active{};
};
struct HistoryFrame {
    FrameInfo info{};
    Trace mixed{};
    std::uint64_t interval_samples{hop};
};
struct Frame {
    FrameInfo info{};
    Trace mixed{};
    std::array<Trace, channels> channel{};
    Column column{};
    HistoryFrame finalized_history{};
    bool channels_available{true};
    bool history_finalized{};
};

class History {
public:
    History() : frames_(historyCapacity) {}
    void clear() noexcept { count_ = write_ = 0; }
    void append(const HistoryFrame& f) {
        frames_[write_] = f;
        write_ = (write_ + 1) % frames_.size();
        count_ = std::min(count_ + 1, frames_.size());
        constexpr auto maximum_history_samples = static_cast<std::uint64_t>(5.0 * sampleRate);
        while (count_ > 1 && latest()->info.sample - at(0).info.sample > maximum_history_samples)
            --count_;
    }
    std::size_t size() const noexcept { return count_; }
    const HistoryFrame& at(std::size_t chronological) const noexcept {
        return frames_[(write_ + frames_.size() - count_ + chronological) % frames_.size()];
    }
    const HistoryFrame* latest() const noexcept { return count_ ? &at(count_ - 1) : nullptr; }
    const HistoryFrame* nearest(double sample) const noexcept {
        if (!count_ || !std::isfinite(sample)) return nullptr;
        std::size_t lo = 0, hi = count_;
        while (lo < hi) {
            auto mid = (lo + hi) / 2;
            if (static_cast<double>(at(mid).info.sample) < sample) lo = mid + 1;
            else hi = mid;
        }
        const HistoryFrame* best = lo < count_ ? &at(lo) : &at(count_ - 1);
        if (lo > 0 && std::abs(static_cast<double>(at(lo - 1).info.sample) - sample)
                      < std::abs(static_cast<double>(best->info.sample) - sample))
            best = &at(lo - 1);
        // Averaged frames represent one complete history interval. Never
        // stretch one across a wider acquisition gap.
        const auto tolerance = static_cast<double>(best->interval_samples) / 2.0 + hop / 2.0 + 1.0;
        return std::abs(static_cast<double>(best->info.sample) - sample) <= tolerance
            ? best : nullptr;
    }
private:
    std::vector<HistoryFrame> frames_;
    std::size_t count_{}, write_{};
};

class Model {
public:
    Model() : latest_(std::make_unique<Frame>()), stopped_(std::make_unique<Frame>()) {}
    void ingest(const Frame& frame) {
        if (context_ != frame.info.context) {
            clearAcquisition();
            context_ = frame.info.context;
        }
        if (frame.history_finalized) live.append(frame.finalized_history);
        *latest_ = frame;
        current_ = {frame.info, frame.mixed, hop};
        current_valid_ = true;
    }
    void clearAcquisition() {
        live.clear(); frozen.clear(); paused = false; selected_seconds = 0;
        current_valid_ = stopped_current_valid_ = false;
    }
    void clearHistory() noexcept { live.clear(); frozen.clear(); selected_seconds = 0; }
    void setPaused(bool value) {
        if (paused == value || (value && !current_valid_)) return;
        paused = value;
        selected_seconds = 0;
        if (paused) {
            frozen = live; // Both vectors already have identical preallocated sizes.
            *stopped_ = *latest_;
            stopped_current_ = current_;
            stopped_current_valid_ = true;
        }
    }
    const History& history() const noexcept { return paused ? frozen : live; }
    const HistoryFrame* front() const noexcept {
        if (!paused) return current_valid_ ? &current_ : nullptr;
        if (!stopped_current_valid_) return nullptr;
        if (selected_seconds <= 0) return &stopped_current_;
        return history().nearest(static_cast<double>(stopped_current_.info.sample)
                                 - selected_seconds * sampleRate);
    }
    const Frame* channelFrame() const noexcept {
        if (!front() || (paused && front()->info.id != stopped_->info.id)) return nullptr;
        const auto* f = paused ? stopped_.get() : latest_.get();
        return f->channels_available ? f : nullptr;
    }
    double availableSeconds() const noexcept {
        const auto& h = history();
        const auto* current = paused ? (stopped_current_valid_ ? &stopped_current_ : nullptr)
                                     : (current_valid_ ? &current_ : nullptr);
        return h.size() && current
            ? std::min(5.0, static_cast<double>(current->info.sample - h.at(0).info.sample) / sampleRate)
            : 0.0;
    }
    double selectedAge() const noexcept {
        const auto* f = front();
        return paused && f && stopped_current_valid_
            ? static_cast<double>(stopped_current_.info.sample - f->info.sample) / sampleRate
            : selected_seconds;
    }
    void hold() {
        if (const auto* f = front()) {
            reference = *f;
            reference_selection_seconds = paused ? selectedAge() : 0;
            reference_was_paused = paused;
            reference_visible = true;
        }
    }
    History live, frozen;
    std::optional<HistoryFrame> reference;
    bool paused{}, reference_visible{true}, reference_was_paused{};
    double selected_seconds{}, reference_selection_seconds{};
private:
    std::unique_ptr<Frame> latest_, stopped_;
    HistoryFrame current_{}, stopped_current_{};
    bool current_valid_{}, stopped_current_valid_{};
    std::uint64_t context_{};
};
struct GuideState {
    double fixed_frequency{};
    bool fixed() const noexcept { return fixed_frequency > 0; }
    void release() noexcept { fixed_frequency = 0; }
    void click(double frequency) noexcept {
        if (fixed()) release();
        else if (frequency > 0 && std::isfinite(frequency)) fixed_frequency = frequency;
    }
};
} // namespace mgstc::app::spectrum
