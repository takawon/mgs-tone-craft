// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <atomic>
#include <juce_dsp/juce_dsp.h>
#include "spectrum_data.hpp"
#include "mgstc/engine/dc_blocker.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"

namespace mgstc::app::spectrum {
inline double noteFrequency(std::uint8_t note, std::size_t source) noexcept {
    engine::NotePitch pitch{};
    if (!engine::notePitch(note, pitch)) return 0;
    constexpr double clock = 3579545.0;
    return source == 2 ? pitch.opll.f_number * clock / (72.0 * (1U << (19 - pitch.opll.block)))
                      : clock / ((pitch.psg_scc_period + 1.0) * 32.0);
}
class Analyzer final : private juce::Thread {
public:
    static constexpr std::size_t queueCapacity = 32;
    explicit Analyzer(engine::RealtimeEngineHost& host)
        : Thread("MGSTC spectrum analyzer"), host_(host),
          queue_(std::make_unique<engine::SpscQueue<Frame, queueCapacity>>()),
          input_(std::make_unique<engine::SpectrumCaptureBlock>()),
          frame_(std::make_unique<Frame>()),
          window_(fftSize, juce::dsp::WindowingFunction<float>::hann, true) {
        for (std::size_t i = 0; i <= 256; ++i)
            edges_[i] = std::clamp<std::size_t>(static_cast<std::size_t>(std::floor(
                minFrequency * std::pow(maxFrequency / minFrequency, i / 256.0)
                    * fftSize / sampleRate)), 1, fftSize / 2);
        for (std::size_t i = 0; i < thresholds_.size(); ++i) {
            const double db = minDb + ((static_cast<double>(i) + 0.5) / 255.0) * -minDb;
            const double mag = fftSize * std::pow(10.0, db / 20.0);
            thresholds_[i] = static_cast<float>(mag * mag);
        }
    }
    ~Analyzer() override {
        host_.setSpectrumCaptureEnabled(false);
        signalThreadShouldExit(); notify(); stopThread(-1);
    }
    void setCaptureEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_release);
        host_.setSpectrumCaptureEnabled(enabled);
        if (enabled && !isThreadRunning()) startThread();
        notify();
    }
    void setChannelsEnabled(bool enabled) { host_.setSpectrumChannelsEnabled(enabled); }
    void setSourceMask(std::uint8_t mask) { host_.setSpectrumSourceMask(mask); notify(); }
    bool pollFrame(Frame& f) noexcept { return queue_->tryPop(f); }
    std::uint64_t context() const noexcept { return host_.spectrumContext(); }
    std::uint64_t droppedInput() const noexcept { return host_.spectrumDroppedBlocks(); }
    std::uint64_t droppedOutput() const noexcept { return dropped_output_.load(); }
    std::uint64_t channelFftCount() const noexcept { return channel_fft_count_.load(); }
    double maximumAnalysisMs() const noexcept { return max_analysis_ms_.load(); }
    // Offline entry point for deterministic signal verification. Do not call while running.
    void processBlock(const engine::SpectrumCaptureBlock& b) noexcept {
            if (b.context != context_ || b.pcm_epoch != epoch_ || b.first_sample != next_sample_) reset();
            context_ = b.context; epoch_ = b.pcm_epoch; remote_ = b.simulated_remote;
            const auto mask = static_cast<std::uint8_t>(context_ & 7);
            for (std::size_t i = 0; i < engine::kSpectrumBlockSize; ++i) {
                for (std::size_t source = 0; source < 3; ++source)
                    append(source, (mask & (1U << source)) ? filters_[source].process(b.sources[source][i]) : 0);
                if (b.channels_valid[i]) {
                    if (!channels_active_) {
                        for (std::size_t ch = 0; ch < channels; ++ch) {
                            rings_[3 + ch] = {}; filters_[3 + ch].reset();
                        }
                        channel_samples_ = 0;
                    }
                    std::array<float, channels> logical{};
                    for (std::size_t ch = 0; ch < channels; ++ch)
                        if (b.channel_map[ch] < channels && (mask & (1U << engine::spectrumSource(ch))))
                            logical[b.channel_map[ch]] += b.channels[ch][i];
                    for (std::size_t ch = 0; ch < channels; ++ch)
                        append(3 + ch, filters_[3 + ch].process(logical[ch]) * b.output_gain[i]);
                    channel_samples_ = std::min(channel_samples_ + 1, fftSize);
                } else channel_samples_ = 0;
                channels_active_ = b.channels_valid[i];
                append(3 + channels, b.mixed[i]);
                notes_[write_] = b.notes[i];
                write_ = (write_ + 1) % fftSize;
                const auto sample = b.first_sample + i;
                if (filled_ < fftSize) {
                    if (++filled_ == fftSize) { publish(sample); since_fft_ = 0; }
                } else if (++since_fft_ == hop) { publish(sample); since_fft_ = 0; }
            }
            next_sample_ = b.first_sample + engine::kSpectrumBlockSize;
    }
private:
    struct Filter : engine::DcBlocker { Filter() : DcBlocker(48000.0F, 3.4F) {} };
    struct Ring {
        std::array<float, fftSize> samples{};
        std::array<bool, fftSize> active{};
        std::size_t count{};
    };
    void reset() noexcept {
        for (auto& r : rings_) r = {};
        for (auto& f : filters_) f.reset();
        write_ = filled_ = since_fft_ = channel_samples_ = 0;
        channels_active_ = false;
    }
    void run() override {
        const juce::ScopedNoDenormals no_denormals;
        while (!threadShouldExit()) {
            if (!host_.pollSpectrumCapture(*input_)) { wait(8); continue; }
            const auto& b = *input_;
            if (!enabled_.load(std::memory_order_acquire) || b.context != host_.spectrumContext()) continue;
            processBlock(b);
        }
    }
    void append(std::size_t channel, float sample) noexcept {
        auto& r = rings_[channel];
        if (r.active[write_]) --r.count;
        r.samples[write_] = sample;
        r.active[write_] = std::isfinite(sample) && std::abs(sample) >= 1.0e-5F;
        if (r.active[write_]) ++r.count;
    }
    bool transform(std::size_t channel) noexcept {
        const auto& ring = rings_[channel];
        if (!ring.count) { power_.fill(0); return false; }
        const auto tail = fftSize - write_;
        std::copy_n(ring.samples.begin() + static_cast<std::ptrdiff_t>(write_), tail, fft_data_.begin());
        std::copy_n(ring.samples.begin(), write_, fft_data_.begin() + static_cast<std::ptrdiff_t>(tail));
        if (channel >= 3 && channel < 3 + channels) channel_fft_count_.fetch_add(1, std::memory_order_relaxed);
        window_.multiplyWithWindowingTable(fft_data_.data(), fftSize);
        fft_.performRealOnlyForwardTransform(fft_data_.data(), true);
        for (std::size_t i = 0; i < bins; ++i) {
            const float re = fft_data_[2 * i], im = fft_data_[2 * i + 1];
            power_[i] = re * re + im * im;
        }
        return true;
    }
    void analyzeTrace(std::size_t channel, Trace& trace) noexcept {
        if (!transform(channel)) { trace = {}; return; }
        // JUCE's normalized Hann has sum(w) = N. DC/Nyquist are not doubled.
        constexpr float scale = 4.0F / static_cast<float>(fftSize * fftSize);
        for (std::size_t i = 0; i < bins; ++i)
            trace.power[i] = power_[i] * ((i == 0 || i == fftSize / 2) ? scale * 0.25F : scale);
        findPeaks(trace);
    }
    void legacy(std::size_t source, std::array<std::uint8_t, 256>& levels,
                spectrogram::HarmonicPeakList& harmonics, const engine::SpectrumGuideNote& note) noexcept {
        if (!transform(source)) { levels = {}; harmonics = {}; return; }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            float maximum = 0;
            const auto end = std::min(std::max(edges_[i] + 1, edges_[i + 1]), fftSize / 2);
            for (auto bin = edges_[i]; bin <= end; ++bin) maximum = std::max(maximum, power_[bin]);
            levels[i] = static_cast<std::uint8_t>(std::upper_bound(thresholds_.begin(), thresholds_.end(), maximum)
                                                - thresholds_.begin());
        }
        harmonics = spectrogram::findHarmonicPeaks(power_, note.active ? noteFrequency(note.note, source) : 0,
                                                  sampleRate, fftSize, thresholds_.front(), minDb);
    }
    void publish(std::uint64_t last_sample) noexcept {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        auto& f = *frame_;
        const auto guide = notes_[(write_ + fftSize / 2) % fftSize];
        f.info = {++sequence_, last_sample + 1 - fftSize / 2, context_, epoch_, guide, remote_};
        legacy(0, f.column.psg, f.column.psg_harmonics, guide);
        legacy(1, f.column.scc, f.column.scc_harmonics, guide);
        legacy(2, f.column.opll, f.column.opll_harmonics, guide);
        f.column.sequence = f.info.id;
        f.column.sample_position = f.info.sample;
        f.column.context = context_;
        f.column.guide_note = guide.note; f.column.guide_track = guide.track; f.column.note_active = guide.active;
        f.channels_available = channel_samples_ == fftSize;
        if (f.channels_available)
            for (std::size_t ch = 0; ch < channels; ++ch) analyzeTrace(3 + ch, f.channel[ch]);
        analyzeTrace(3 + channels, f.mixed);
        if (!queue_->tryPush(f)) dropped_output_.fetch_add(1, std::memory_order_relaxed);
        const auto elapsed = juce::Time::getMillisecondCounterHiRes() - started;
        if (elapsed > max_analysis_ms_.load(std::memory_order_relaxed))
            max_analysis_ms_.store(elapsed, std::memory_order_relaxed);
    }
    engine::RealtimeEngineHost& host_;
    std::unique_ptr<engine::SpscQueue<Frame, queueCapacity>> queue_;
    std::unique_ptr<engine::SpectrumCaptureBlock> input_;
    std::unique_ptr<Frame> frame_;
    juce::dsp::FFT fft_{11};
    juce::dsp::WindowingFunction<float> window_;
    std::array<Ring, 3 + channels + 1> rings_{};
    std::array<Filter, 3 + channels> filters_{};
    std::array<engine::SpectrumGuideNote, fftSize> notes_{};
    std::array<float, fftSize * 2> fft_data_{};
    std::array<float, bins> power_{};
    std::array<std::size_t, 257> edges_{};
    std::array<float, 255> thresholds_{};
    std::size_t write_{}, filled_{}, since_fft_{};
    std::uint64_t context_{}, epoch_{}, next_sample_{}, sequence_{};
    bool remote_{}, channels_active_{};
    std::size_t channel_samples_{};
    std::atomic<bool> enabled_{};
    std::atomic<std::uint64_t> dropped_output_{}, channel_fft_count_{};
    std::atomic<double> max_analysis_ms_{};
};
} // namespace mgstc::app::spectrum
