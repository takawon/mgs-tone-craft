// SPDX-License-Identifier: AGPL-3.0-only

#include "spectrogram_window.hpp"
#include "spectrogram_emphasis.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <juce_dsp/juce_dsp.h>

#include "ui_paths.hpp"
#include "ui_layout.hpp"
#include "ui_scale.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_fonts.hpp"
#include "mgstc/engine/dc_blocker.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/spsc_queue.hpp"

namespace mgstc::app {
namespace {

constexpr double kSampleRate = 48'000.0;
constexpr int kFftOrder = 11;
constexpr std::size_t kFftSize = 1U << kFftOrder;
constexpr std::size_t kFftHop = 512;
constexpr std::size_t kLogBinCount = 256;
constexpr float kMinimumFrequency = 30.0F;
constexpr float kMaximumFrequency = 24'000.0F;
constexpr float kMinimumDb = -100.0F;
constexpr float kSilenceAmplitude = 1.0e-5F;
constexpr float kDcBlockerCutoffHz = 3.4F;
constexpr std::size_t kColumnQueueCapacity = 128;
// Covers the 2400 px maximum window at 125% UI scale and the minimum
// 0.25 px/column time scale, with room for plot chrome and wrap redraw.
constexpr std::size_t kHistoryCapacity = 16384;
constexpr int kAnalyzerIdleWaitMs = 8;
constexpr double kColumnsPerSecond = kSampleRate
    / static_cast<double>(kFftHop);

constexpr std::uint8_t kAnalyzePsg = 1U << 0U;
constexpr std::uint8_t kAnalyzeScc = 1U << 1U;
constexpr std::uint8_t kAnalyzeOpll = 1U << 2U;
constexpr std::uint8_t kAnalyzeAll =
    kAnalyzePsg | kAnalyzeScc | kAnalyzeOpll;

[[nodiscard]] double noteFrequencyForChip(
    std::uint8_t midi_note,
    bool opll) noexcept {
    constexpr double kMasterClock = 3'579'545.0;
    engine::NotePitch pitch{};
    if (!engine::notePitch(midi_note, pitch)) {
        return 0.0;
    }
    if (opll) {
        return static_cast<double>(pitch.opll.f_number) * kMasterClock
            / (72.0
               * static_cast<double>(1U << (19 - pitch.opll.block)));
    }
    return kMasterClock
        / ((static_cast<double>(pitch.psg_scc_period) + 1.0) * 32.0);
}

[[nodiscard]] double noteFrequencyForTrack(
    std::uint8_t midi_note,
    std::uint8_t track) noexcept {
    return noteFrequencyForChip(midi_note, track >= 8);
}

[[nodiscard]] double noteFrequencyForSource(
    std::uint8_t midi_note,
    std::size_t source) noexcept {
    return noteFrequencyForChip(midi_note, source == 2);
}

const auto kPsgColour = juce::Colour(0xFFB990FF);
const auto kSccColour = juce::Colour(0xFF53E3A6);
const auto kOpllColour = juce::Colour(0xFFFFA75E);

struct SpectrogramColumn {
    std::array<std::uint8_t, kLogBinCount> psg{};
    std::array<std::uint8_t, kLogBinCount> scc{};
    std::array<std::uint8_t, kLogBinCount> opll{};
    spectrogram::HarmonicPeakList psg_harmonics{};
    spectrogram::HarmonicPeakList scc_harmonics{};
    spectrogram::HarmonicPeakList opll_harmonics{};
    std::uint64_t sequence{};
    std::uint8_t guide_note{60};
    std::uint8_t guide_track{};
    bool note_active{};
};

static_assert(std::is_trivially_copyable_v<SpectrogramColumn>);

[[nodiscard]] constexpr std::uint8_t sourceMaskForMode(
    SpectrogramAnalysisMode mode) noexcept {
    return static_cast<std::uint8_t>(mode) & kAnalyzeAll;
}

static_assert(sourceMaskForMode(SpectrogramAnalysisMode::None) == 0);
static_assert(
    sourceMaskForMode(SpectrogramAnalysisMode::PsgOnly) == kAnalyzePsg);
static_assert(
    sourceMaskForMode(SpectrogramAnalysisMode::SccOnly) == kAnalyzeScc);
static_assert(
    sourceMaskForMode(SpectrogramAnalysisMode::OpllOnly) == kAnalyzeOpll);
static_assert(
    sourceMaskForMode(SpectrogramAnalysisMode::AllSources) == kAnalyzeAll);

class SpectrumAnalyzerThread final : private juce::Thread {
public:
    explicit SpectrumAnalyzerThread(engine::RealtimeEngineHost& engine)
        : Thread("MGSTC spectrum analyzer"),
          engine_(engine),
          window_(
              kFftSize,
              juce::dsp::WindowingFunction<float>::hann,
              true) {
        for (std::size_t index = 0; index < log_bin_edges_.size(); ++index) {
            const double ratio = static_cast<double>(index)
                / static_cast<double>(kLogBinCount);
            const double frequency = static_cast<double>(kMinimumFrequency)
                * std::pow(
                    static_cast<double>(kMaximumFrequency)
                        / static_cast<double>(kMinimumFrequency),
                    ratio);
            log_bin_edges_[index] = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::floor(
                    frequency * static_cast<double>(kFftSize) / kSampleRate)),
                1,
                kFftSize / 2);
        }
        for (std::size_t log_bin = 0; log_bin < kLogBinCount; ++log_bin) {
            const LogBandRange range{
                log_bin_edges_[log_bin],
                std::min(
                    std::max(
                        log_bin_edges_[log_bin] + 1,
                        log_bin_edges_[log_bin + 1]),
                    kFftSize / 2),
            };
            std::size_t range_index = 0;
            for (; range_index < unique_log_range_count_; ++range_index) {
                if (unique_log_ranges_[range_index] == range) {
                    break;
                }
            }
            if (range_index == unique_log_range_count_) {
                unique_log_ranges_[unique_log_range_count_++] = range;
            }
            log_bin_range_indices_[log_bin] = range_index;
        }
        for (std::size_t level = 1;
             level <= power_thresholds_.size();
             ++level) {
            const double normalized_level =
                (static_cast<double>(level) - 0.5) / 255.0;
            const double decibels = static_cast<double>(kMinimumDb)
                + normalized_level * -static_cast<double>(kMinimumDb);
            const double magnitude = static_cast<double>(kFftSize)
                * std::pow(10.0, decibels / 20.0);
            power_thresholds_[level - 1] = static_cast<float>(
                magnitude * magnitude);
        }
    }

    ~SpectrumAnalyzerThread() override {
        engine_.setSpectrogramCaptureEnabled(false);
        signalThreadShouldExit();
        notify();
        static_cast<void>(stopThread(1500));
    }

    void setCaptureEnabled(bool enabled) noexcept {
        const bool previous = capture_enabled_.exchange(
            enabled,
            std::memory_order_acq_rel);
        if (previous == enabled) {
            return;
        }
        if (!enabled) {
            engine_.setSpectrogramCaptureEnabled(false);
        }
        reset_requested_.store(true, std::memory_order_release);
        if (enabled && !isThreadRunning()) {
            startThread();
        }
        if (enabled) {
            engine_.setSpectrogramCaptureEnabled(true);
        }
        notify();
    }

    [[nodiscard]] bool pollColumn(SpectrogramColumn& column) noexcept {
        return columns_.tryPop(column);
    }

    void discardPendingColumns() noexcept {
        reset_requested_.store(true, std::memory_order_release);
        SpectrogramColumn column{};
        while (columns_.tryPop(column)) {
        }
        notify();
    }

    void setSourceMask(std::uint8_t source_mask) noexcept {
        source_mask &= kAnalyzeAll;
        const auto previous = requested_source_mask_.exchange(
            source_mask,
            std::memory_order_acq_rel);
        if (previous == source_mask) {
            return;
        }
        source_mask_reset_requested_.store(true, std::memory_order_release);
        notify();
    }

private:
    struct LogBandRange {
        std::size_t first{};
        std::size_t last{};

        [[nodiscard]] bool operator==(
            const LogBandRange&) const noexcept = default;
    };

    void run() override {
        const juce::ScopedNoDenormals no_denormals;
        while (!threadShouldExit()) {
            const bool full_reset = reset_requested_.exchange(
                false,
                std::memory_order_acq_rel);
            const bool source_mask_reset =
                source_mask_reset_requested_.exchange(
                    false,
                    std::memory_order_acq_rel);
            if (source_mask_reset) {
                active_source_mask_ = requested_source_mask_.load(
                    std::memory_order_acquire);
            }
            if (full_reset) {
                resetAnalysisState();
            } else if (source_mask_reset) {
                // Do not join PCM from different editor analysis contexts.
                // Keep column_sequence_ continuous so existing history stays.
                resetPcmState(true);
            }
            if (full_reset || source_mask_reset) {
                engine::OpllScopeFrame stale{};
                while (engine_.pollSpectrogramScope(stale)) {
                }
            }
            engine::OpllScopeFrame frame{};
            if (!engine_.pollSpectrogramScope(frame)) {
                wait(kAnalyzerIdleWaitMs);
                continue;
            }
            // Program replacement restarts EngineCore's sequence, while a
            // forward gap means queued audio was lost.  In either case, do
            // not join unrelated PCM in one FFT window.  Keep the published
            // column sequence continuous so one-second preview history and
            // the UI time axis are preserved.
            if (has_frame_sequence_
                && frame.sequence != last_frame_sequence_ + 1) {
                resetPcmState(true);
            }
            last_frame_sequence_ = frame.sequence;
            has_frame_sequence_ = true;
            appendFrame(frame);
        }
    }

    void resetAnalysisState() noexcept {
        resetPcmState(false);
        column_sequence_ = 0;
        last_frame_sequence_ = 0;
        has_frame_sequence_ = false;
    }

    void resetPcmState(bool prime_with_silence) noexcept {
        psg_ring_.fill(0.0F);
        scc_ring_.fill(0.0F);
        opll_ring_.fill(0.0F);
        for (auto& active : active_samples_) {
            active.fill(false);
        }
        psg_dc_blocker_.reset();
        scc_dc_blocker_.reset();
        opll_dc_blocker_.reset();
        ring_write_ = 0;
        buffered_samples_ = prime_with_silence ? kFftSize : 0;
        samples_since_fft_ = 0;
        active_sample_counts_.fill(0);
    }

    void appendFrame(const engine::OpllScopeFrame& frame) noexcept {
        guide_note_ = frame.guide_note;
        guide_track_ = frame.guide_track;
        note_active_ = frame.note_active;
        for (std::size_t index = 0; index < frame.samples.size(); ++index) {
            if ((active_source_mask_ & kAnalyzePsg) != 0) {
                appendSourceSample(
                    0,
                    psg_dc_blocker_.process(frame.psg_samples[index]),
                    psg_ring_);
            }
            if ((active_source_mask_ & kAnalyzeScc) != 0) {
                appendSourceSample(
                    1,
                    scc_dc_blocker_.process(frame.scc_samples[index]),
                    scc_ring_);
            }
            if ((active_source_mask_ & kAnalyzeOpll) != 0) {
                appendSourceSample(
                    2,
                    opll_dc_blocker_.process(frame.samples[index]),
                    opll_ring_);
            }
            ring_write_ = (ring_write_ + 1) % kFftSize;

            if (buffered_samples_ < kFftSize) {
                ++buffered_samples_;
                if (buffered_samples_ == kFftSize) {
                    publishColumn();
                    samples_since_fft_ = 0;
                }
                continue;
            }
            if (++samples_since_fft_ >= kFftHop) {
                samples_since_fft_ = 0;
                publishColumn();
            }
        }
    }

    void appendSourceSample(
        std::size_t source,
        float sample,
        std::array<float, kFftSize>& ring) noexcept {
        if (active_samples_[source][ring_write_]) {
            --active_sample_counts_[source];
        }
        ring[ring_write_] = sample;
        const bool active = std::abs(sample) >= kSilenceAmplitude;
        active_samples_[source][ring_write_] = active;
        if (active) {
            ++active_sample_counts_[source];
        }
    }

    void publishColumn() noexcept {
        SpectrogramColumn column{};
        const double psg_scc_fundamental = note_active_
            ? noteFrequencyForSource(guide_note_, 0)
            : 0.0;
        const double opll_fundamental = note_active_
            ? noteFrequencyForSource(guide_note_, 2)
            : 0.0;
        // Keep the time axis moving during silence, but leave each inactive
        // source black and skip its FFT.  The cheap sample activity check also
        // preserves release tails until the whole FFT window is below the
        // display floor.
        if ((active_source_mask_ & kAnalyzePsg) != 0
            && active_sample_counts_[0] != 0) {
            analyze(
                psg_ring_,
                column.psg,
                column.psg_harmonics,
                psg_scc_fundamental);
        }
        if ((active_source_mask_ & kAnalyzeScc) != 0
            && active_sample_counts_[1] != 0) {
            analyze(
                scc_ring_,
                column.scc,
                column.scc_harmonics,
                psg_scc_fundamental);
        }
        if ((active_source_mask_ & kAnalyzeOpll) != 0
            && active_sample_counts_[2] != 0) {
            analyze(
                opll_ring_,
                column.opll,
                column.opll_harmonics,
                opll_fundamental);
        }
        column.sequence = ++column_sequence_;
        column.guide_note = guide_note_;
        column.guide_track = guide_track_;
        column.note_active = note_active_;
        static_cast<void>(columns_.tryPush(column));
    }

    void analyze(
        const std::array<float, kFftSize>& ring,
        std::array<std::uint8_t, kLogBinCount>& output,
        spectrogram::HarmonicPeakList& harmonic_peaks,
        double fundamental_frequency) noexcept {
        const auto tail_size = kFftSize - ring_write_;
        std::copy_n(
            ring.begin() + static_cast<std::ptrdiff_t>(ring_write_),
            tail_size,
            fft_data_.begin());
        std::copy_n(
            ring.begin(),
            ring_write_,
            fft_data_.begin() + static_cast<std::ptrdiff_t>(tail_size));
        window_.multiplyWithWindowingTable(fft_data_.data(), kFftSize);
        fft_.performRealOnlyForwardTransform(fft_data_.data(), true);

        const auto* spectrum = reinterpret_cast<
            const juce::dsp::Complex<float>*>(fft_data_.data());
        for (std::size_t fft_bin = 0;
             fft_bin < power_spectrum_.size();
             ++fft_bin) {
            const auto real = spectrum[fft_bin].real();
            const auto imaginary = spectrum[fft_bin].imag();
            power_spectrum_[fft_bin] = real * real + imaginary * imaginary;
        }
        harmonic_peaks = spectrogram::findHarmonicPeaks(
            power_spectrum_,
            fundamental_frequency,
            kSampleRate,
            kFftSize,
            power_thresholds_.front(),
            kMinimumDb);

        std::array<std::uint8_t, kLogBinCount> range_levels{};
        for (std::size_t range_index = 0;
             range_index < unique_log_range_count_;
             ++range_index) {
            const auto range = unique_log_ranges_[range_index];
            float maximum_power = 0.0F;
            for (std::size_t fft_bin = range.first;
                 fft_bin <= range.last;
                 ++fft_bin) {
                maximum_power = std::max(
                    maximum_power,
                    power_spectrum_[fft_bin]);
            }
            range_levels[range_index] = static_cast<std::uint8_t>(
                std::upper_bound(
                    power_thresholds_.begin(),
                    power_thresholds_.end(),
                    maximum_power)
                - power_thresholds_.begin());
        }
        for (std::size_t log_bin = 0; log_bin < kLogBinCount; ++log_bin) {
            output[log_bin] = range_levels[
                log_bin_range_indices_[log_bin]];
        }
    }

    engine::RealtimeEngineHost& engine_;
    juce::dsp::FFT fft_{kFftOrder};
    juce::dsp::WindowingFunction<float> window_;
    engine::SpscQueue<SpectrogramColumn, kColumnQueueCapacity> columns_{};
    std::array<float, kFftSize> psg_ring_{};
    std::array<float, kFftSize> scc_ring_{};
    std::array<float, kFftSize> opll_ring_{};
    std::array<std::array<bool, kFftSize>, 3> active_samples_{};
    std::array<float, kFftSize * 2> fft_data_{};
    std::array<float, kFftSize / 2 + 1> power_spectrum_{};
    std::array<std::size_t, kLogBinCount + 1> log_bin_edges_{};
    std::array<LogBandRange, kLogBinCount> unique_log_ranges_{};
    std::array<std::size_t, kLogBinCount> log_bin_range_indices_{};
    std::array<float, 255> power_thresholds_{};
    engine::DcBlocker psg_dc_blocker_{
        static_cast<float>(kSampleRate), kDcBlockerCutoffHz};
    engine::DcBlocker scc_dc_blocker_{
        static_cast<float>(kSampleRate), kDcBlockerCutoffHz};
    engine::DcBlocker opll_dc_blocker_{
        static_cast<float>(kSampleRate), kDcBlockerCutoffHz};
    std::size_t ring_write_{};
    std::size_t buffered_samples_{};
    std::size_t samples_since_fft_{};
    std::array<std::size_t, 3> active_sample_counts_{};
    std::size_t unique_log_range_count_{};
    std::uint64_t column_sequence_{};
    std::uint64_t last_frame_sequence_{};
    std::uint8_t guide_note_{60};
    std::uint8_t guide_track_{};
    std::atomic<bool> capture_enabled_{false};
    std::atomic<bool> reset_requested_{true};
    std::atomic<std::uint8_t> requested_source_mask_{kAnalyzeAll};
    std::atomic<bool> source_mask_reset_requested_{false};
    std::uint8_t active_source_mask_{kAnalyzeAll};
    bool has_frame_sequence_{};
    bool note_active_{};
};

[[nodiscard]] juce::String nearestNoteName(double frequency) {
    if (!(frequency > 0.0)) {
        return {};
    }
    constexpr std::array<const char*, 12> names{
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"};
    const int midi = juce::jlimit(
        0,
        127,
        juce::roundToInt(69.0 + 12.0 * std::log2(frequency / 440.0)));
    return juce::String(names[static_cast<std::size_t>(midi % 12)])
        + juce::String(midi / 12 - 1);
}

class SpectrogramToggleLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool should_draw_highlight,
        bool should_draw_down) override {
        auto bounds = button.getLocalBounds().toFloat().reduced(
            static_cast<float>(UiScale::sx(2)),
            static_cast<float>(UiScale::sx(3)));
        const bool enabled = button.isEnabled();
        const bool active = button.getToggleState();
        const float corner = bounds.getHeight() * 0.5F;
        constexpr auto kPanelFill = juce::uint32{0xFF2A3540};
        constexpr auto kOutline = juce::uint32{0xFF607080};

        auto fill = juce::Colour(active ? kUiToggleOnFill : kPanelFill)
            .withMultipliedAlpha(enabled ? 1.0F : 0.5F);
        if (should_draw_down || should_draw_highlight) {
            fill = fill.contrasting(should_draw_down ? 0.2F : 0.05F);
        }
        graphics.setColour(fill);
        graphics.fillRoundedRectangle(bounds, corner);
        graphics.setColour(
            button.hasKeyboardFocus(true)
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(kOutline));
        graphics.drawRoundedRectangle(
            bounds,
            corner,
            button.hasKeyboardFocus(true) ? 1.5F : 1.0F);

        graphics.setFont(UiFonts::make(UiFonts::controlTextHeight(
            static_cast<float>(button.getHeight()))));
        const auto source_colour = button.findColour(
            juce::ToggleButton::textColourId);
        const auto text_colour = should_draw_highlight && enabled
            ? juce::Colours::white
            : source_colour;
        graphics.setColour(
            text_colour.withMultipliedAlpha(enabled ? 1.0F : 0.5F));
        graphics.drawFittedText(
            button.getButtonText(),
            bounds.toNearestInt(),
            juce::Justification::centred,
            1);
    }
};

[[nodiscard]] std::unique_ptr<juce::Drawable> makePinIcon(
    juce::Colour colour) {
    juce::Path path;
    // JUCE has no stock pin/down-triangle glyph, so use the requested
    // fallback up arrow in the same button style as one-second preview.
    path.startNewSubPath(12.0F, 21.0F);
    path.lineTo(12.0F, 3.0F);
    path.startNewSubPath(12.0F, 3.0F);
    path.lineTo(6.0F, 9.0F);
    path.startNewSubPath(12.0F, 3.0F);
    path.lineTo(18.0F, 9.0F);
    auto drawable = std::make_unique<juce::DrawablePath>();
    drawable->setPath(std::move(path));
    drawable->setFill(juce::Colours::transparentBlack);
    drawable->setStrokeFill(colour);
    drawable->setStrokeType(
        juce::PathStrokeType(
            1.7F,
            juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded));
    return drawable;
}

void configurePinButton(juce::DrawableButton& button) {
    const auto normal = makePinIcon(juce::Colour(0xFF9AA8B5));
    const auto over = makePinIcon(juce::Colour(0xFFE6EDF3));
    const auto normal_on = makePinIcon(juce::Colour(0xFFFFFFFF));
    const auto over_on = makePinIcon(juce::Colour(0xFFF2F4F5));
    button.setImages(
        normal.get(),
        over.get(),
        over.get(),
        nullptr,
        normal_on.get(),
        over_on.get(),
        over_on.get(),
        nullptr);
    button.setClickingTogglesState(true);
    button.setTooltip(
        juce::String::fromUTF8(
            "MGSTC内でスペクトログラムを前面固定（他アプリの前面は奪いません）"));
}

class SpectrogramDisplay final : public juce::Component {
private:
    static constexpr std::size_t kMaximumPixelHarmonics =
        spectrogram::kMaximumHarmonicPeaks * 4;

    struct SourceIntensity {
        std::uint8_t psg{};
        std::uint8_t scc{};
        std::uint8_t opll{};
    };

    struct PixelHarmonicPeakList {
        std::array<spectrogram::HarmonicPeak, kMaximumPixelHarmonics> peaks{};
        std::uint8_t count{};
    };

    struct PixelHarmonics {
        PixelHarmonicPeakList psg{};
        PixelHarmonicPeakList scc{};
        PixelHarmonicPeakList opll{};
    };

    static constexpr std::uint8_t kNoHarmonic = 0xFF;

    struct HarmonicInfluence {
        std::uint8_t psg{kNoHarmonic};
        std::uint8_t scc{kNoHarmonic};
        std::uint8_t opll{kNoHarmonic};
    };

    struct GuideState {
        std::uint8_t note{60};
        std::uint8_t track{};
        bool active{};

        friend bool operator==(
            const GuideState&,
            const GuideState&) = default;
    };

public:
    SpectrogramDisplay()
        : history_(kHistoryCapacity) {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        setOpaque(true);
    }

    void appendColumn(const SpectrogramColumn& column) {
        if (source_sequence_ != 0
            && column.sequence != source_sequence_ + 1) {
            clearHistory();
        }
        source_sequence_ = column.sequence;
        auto displayed = column;
        displayed.sequence = latest_sequence_ + 1;
        history_[history_write_] = displayed;
        history_write_ = (history_write_ + 1) % history_.size();
        history_count_ = std::min(history_count_ + 1, history_.size());
        latest_sequence_ = displayed.sequence;
        guide_note_ = column.guide_note;
        guide_track_ = column.guide_track;
        note_active_ = column.note_active;
        if (image_.isValid()) {
            drawColumn(displayed);
        }
    }

    void clearHistory() {
        history_count_ = 0;
        history_write_ = 0;
        latest_sequence_ = 0;
        source_sequence_ = 0;
        const auto plot = plotBounds();
        if (image_.isValid()
            && image_.getWidth() == plot.getWidth()
            && image_.getHeight() == plot.getHeight()) {
            image_.clear(image_.getBounds(), juce::Colours::black);
            std::fill(
                source_intensities_.begin(),
                source_intensities_.end(),
                SourceIntensity{});
            std::fill(
                pixel_cycles_.begin(),
                pixel_cycles_.end(),
                std::numeric_limits<std::uint64_t>::max());
            std::fill(
                pixel_harmonics_.begin(),
                pixel_harmonics_.end(),
                PixelHarmonics{});
            std::fill(
                harmonic_influences_.begin(),
                harmonic_influences_.end(),
                HarmonicInfluence{});
            dirty_first_x_ = std::numeric_limits<int>::max();
            dirty_last_x_ = -1;
            requestFullRepaint();
        } else {
            rebuildImage();
        }
    }

    void setSourceVisibility(bool psg, bool scc, bool opll) {
        UiScale::forceGlobalForNonEditorUi();
        if (show_psg_ == psg && show_scc_ == scc && show_opll_ == opll) {
            return;
        }
        show_psg_ = psg;
        show_scc_ = scc;
        show_opll_ = opll;
        recomposeImage();
    }

    void setEmphasis(double percent) {
        const auto next = juce::jlimit(0.0, 100.0, percent);
        if (std::abs(next - emphasis_percent_) < 0.0001) {
            return;
        }
        emphasis_percent_ = next;
        recomposeImage();
    }

    void setTimeScale(double pixels_per_column) {
        UiScale::forceGlobalForNonEditorUi();
        const auto next = juce::jlimit(0.25, 4.0, pixels_per_column);
        if (std::abs(next - time_scale_) < 0.0001) {
            return;
        }
        time_scale_ = next;
        clearHistory();
    }

    void setFrequencyZoom(double zoom) {
        UiScale::forceGlobalForNonEditorUi();
        const auto next = juce::jlimit(0.5, 4.0, zoom);
        if (std::abs(next - frequency_zoom_) < 0.0001) {
            return;
        }
        frequency_zoom_ = next;
        clampFrequencyOffset();
        rebuildImage();
    }

    void setFrequencyOffset(double octaves) {
        UiScale::forceGlobalForNonEditorUi();
        const auto next = juce::jlimit(
            0.0,
            maximumFrequencyOffset(),
            octaves);
        if (std::abs(next - frequency_offset_) < 0.0001) {
            return;
        }
        frequency_offset_ = next;
        rebuildImage();
    }

    [[nodiscard]] double timeScale() const noexcept { return time_scale_; }
    [[nodiscard]] double frequencyZoom() const noexcept {
        return frequency_zoom_;
    }
    [[nodiscard]] double frequencyOffset() const noexcept {
        return frequency_offset_;
    }

    [[nodiscard]] double emphasis() const noexcept {
        return emphasis_percent_;
    }

    void flushPendingRepaint() {
        const auto plot = plotBounds();
        const int write_x = writePositionImageX();
        const GuideState guide_state{guide_note_, guide_track_, note_active_};
        const bool guide_changed = !cursor_inside_
            && (!painted_guide_valid_
                || painted_guide_state_ != guide_state);
        if (dirty_first_x_ > dirty_last_x_
            && painted_write_x_ == write_x
            && !guide_changed) {
            return;
        }
        if (dirty_first_x_ <= dirty_last_x_) {
            repaint(
                plot.getX() + dirty_first_x_,
                plot.getY(),
                dirty_last_x_ - dirty_first_x_ + 1,
                plot.getHeight());
        }
        repaintWritePositionStrip(plot, painted_write_x_);
        if (write_x != painted_write_x_) {
            repaintWritePositionStrip(plot, write_x);
        }
        if (guide_changed) {
            repaintChangedGuideRows(
                plot,
                painted_guide_state_,
                painted_guide_valid_,
                guide_state);
            painted_guide_state_ = guide_state;
            painted_guide_valid_ = true;
        } else if (cursor_inside_) {
            painted_guide_valid_ = false;
        }
        painted_write_x_ = write_x;
        dirty_first_x_ = std::numeric_limits<int>::max();
        dirty_last_x_ = -1;
    }

    [[nodiscard]] double visibleOctaves() const noexcept {
        return static_cast<double>(std::max(1, plotBounds().getHeight()))
            / pixelsPerOctave();
    }

    [[nodiscard]] double totalOctaves() const noexcept {
        return std::log2(
            static_cast<double>(kMaximumFrequency)
            / static_cast<double>(kMinimumFrequency));
    }

    [[nodiscard]] double maximumFrequencyOffset() const noexcept {
        return std::max(0.0, totalOctaves() - visibleOctaves());
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        graphics.fillAll(juce::Colour(0xFF111820));
        const auto plot = plotBounds();
        if (image_.isValid()) {
            graphics.drawImageAt(image_, plot.getX(), plot.getY());
        }
        graphics.setColour(juce::Colour(0xFF34404C));
        graphics.drawRect(plot);
        drawFrequencyAxis(graphics, plot);
        drawHarmonicGuides(graphics, plot);
        drawCursor(graphics, plot);
        drawWritePosition(graphics, plot);
    }

    void resized() override {
        clampFrequencyOffset();
        rebuildImage();
    }

    void mouseEnter(const juce::MouseEvent& event) override {
        UiScale::forceGlobalForNonEditorUi();
        cursor_inside_ = plotBounds().contains(event.getPosition());
        cursor_position_ = event.getPosition();
        requestFullRepaint();
    }

    void mouseMove(const juce::MouseEvent& event) override {
        UiScale::forceGlobalForNonEditorUi();
        cursor_position_ = event.getPosition();
        cursor_inside_ = plotBounds().contains(cursor_position_);
        requestFullRepaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        cursor_inside_ = false;
        requestFullRepaint();
    }

    void mouseWheelMove(
        const juce::MouseEvent&,
        const juce::MouseWheelDetails& wheel) override {
        UiScale::forceGlobalForNonEditorUi();
        const double requested = frequency_offset_
            + static_cast<double>(wheel.deltaY) * 0.8;
        if (frequency_scroll_) {
            frequency_scroll_(requested);
        } else {
            setFrequencyOffset(requested);
        }
    }

    void setFrequencyScrollCallback(
        std::function<void(double)> callback) {
        frequency_scroll_ = std::move(callback);
    }

private:
    [[nodiscard]] juce::Rectangle<int> plotBounds() const noexcept {
        auto bounds = getLocalBounds();
        bounds.removeFromLeft(UiScale::sx(58));
        bounds.removeFromBottom(UiScale::sx(24));
        return bounds.reduced(UiScale::sx(1));
    }

    [[nodiscard]] double pixelsPerOctave() const noexcept {
        return 60.0 * frequency_zoom_;
    }

    void clampFrequencyOffset() noexcept {
        frequency_offset_ = juce::jlimit(
            0.0,
            maximumFrequencyOffset(),
            frequency_offset_);
    }

    [[nodiscard]] int writePositionImageX() const noexcept {
        const auto plot = plotBounds();
        if (latest_sequence_ == 0 || plot.getWidth() <= 0) {
            return -1;
        }
        return static_cast<int>(std::fmod(
            static_cast<double>(latest_sequence_) * time_scale_,
            static_cast<double>(plot.getWidth())));
    }

    void syncOverlayStateForFullRepaint() noexcept {
        painted_write_x_ = writePositionImageX();
        if (cursor_inside_) {
            painted_guide_valid_ = false;
            return;
        }
        painted_guide_state_ = {guide_note_, guide_track_, note_active_};
        painted_guide_valid_ = true;
    }

    void requestFullRepaint() {
        syncOverlayStateForFullRepaint();
        repaint();
    }

    void repaintWritePositionStrip(
        juce::Rectangle<int> plot,
        int image_x) {
        if (image_x < 0 || image_x >= plot.getWidth()) {
            return;
        }
        constexpr int kLinePadding = 1;
        const int left = std::max(
            plot.getX(),
            plot.getX() + image_x - kLinePadding);
        const int right = std::min(
            plot.getRight(),
            plot.getX() + image_x + kLinePadding + 1);
        repaint(left, plot.getY(), right - left, plot.getHeight());
    }

    void addGuideDirtyRows(
        juce::Rectangle<int> plot,
        GuideState state,
        std::array<juce::Rectangle<int>, 32>& regions,
        std::size_t& count) const {
        const double fundamental = noteFrequencyForTrack(state.note, state.track);
        for (int harmonic = 1; harmonic <= 16; ++harmonic) {
            const int y = yForFrequency(fundamental * harmonic);
            if (y == std::numeric_limits<int>::min()
                || count >= regions.size()) {
                continue;
            }
            const int top = std::max(plot.getY(), y - UiScale::sx(12));
            const int bottom = std::min(
                plot.getBottom(),
                y + UiScale::sx(2));
            regions[count++] = {
                plot.getX(),
                top,
                plot.getWidth(),
                std::max(1, bottom - top),
            };
        }
    }

    void repaintChangedGuideRows(
        juce::Rectangle<int> plot,
        GuideState old_state,
        bool old_state_valid,
        GuideState new_state) {
        std::array<juce::Rectangle<int>, 32> regions{};
        std::size_t count = 0;
        if (old_state_valid) {
            addGuideDirtyRows(plot, old_state, regions, count);
        }
        addGuideDirtyRows(plot, new_state, regions, count);
        if (count == 0) {
            return;
        }
        std::sort(
            regions.begin(),
            regions.begin() + static_cast<std::ptrdiff_t>(count),
            [](const auto& left, const auto& right) {
                return left.getY() < right.getY();
            });
        auto merged = regions[0];
        for (std::size_t index = 1; index < count; ++index) {
            if (regions[index].getY() <= merged.getBottom()) {
                merged = merged.getUnion(regions[index]);
                continue;
            }
            repaint(merged);
            merged = regions[index];
        }
        repaint(merged);
    }

    [[nodiscard]] double frequencyAtPlotY(int y) const noexcept {
        const auto plot = plotBounds();
        const double pixels_from_bottom = static_cast<double>(
            plot.getBottom() - 1 - y);
        const double octave = frequency_offset_
            + pixels_from_bottom / pixelsPerOctave();
        return static_cast<double>(kMinimumFrequency) * std::pow(2.0, octave);
    }

    [[nodiscard]] int yForFrequency(double frequency) const noexcept {
        const auto plot = plotBounds();
        if (frequency < kMinimumFrequency || frequency > kMaximumFrequency) {
            return std::numeric_limits<int>::min();
        }
        const double octave = std::log2(
            frequency / static_cast<double>(kMinimumFrequency));
        const double pixels = (octave - frequency_offset_)
            * pixelsPerOctave();
        const int y = plot.getBottom() - 1 - juce::roundToInt(pixels);
        return plot.contains(plot.getX(), y)
            ? y
            : std::numeric_limits<int>::min();
    }

    [[nodiscard]] std::size_t logBinForY(int image_y) const noexcept {
        const auto plot = plotBounds();
        const int component_y = plot.getY() + image_y;
        const double frequency = frequencyAtPlotY(component_y);
        if (frequency < static_cast<double>(kMinimumFrequency)
            || frequency > static_cast<double>(kMaximumFrequency)) {
            return kLogBinCount;
        }
        const double normalized = std::log(
            frequency / static_cast<double>(kMinimumFrequency))
            / std::log(
                static_cast<double>(kMaximumFrequency)
                / static_cast<double>(kMinimumFrequency));
        return static_cast<std::size_t>(juce::jlimit(
            0,
            static_cast<int>(kLogBinCount - 1),
            juce::roundToInt(
                normalized * static_cast<double>(kLogBinCount - 1))));
    }

    [[nodiscard]] juce::Colour colourFor(
        const SourceIntensity& intensity) const noexcept {
        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        const auto add = [&](std::uint8_t level, juce::Colour colour) {
            const float amount = static_cast<float>(level) / 255.0F;
            red += colour.getFloatRed() * amount;
            green += colour.getFloatGreen() * amount;
            blue += colour.getFloatBlue() * amount;
        };
        if (show_psg_) {
            add(intensity.psg, kPsgColour);
        }
        if (show_scc_) {
            add(intensity.scc, kSccColour);
        }
        if (show_opll_) {
            add(intensity.opll, kOpllColour);
        }
        return juce::Colour::fromFloatRGBA(
            std::clamp(red, 0.0F, 1.0F),
            std::clamp(green, 0.0F, 1.0F),
            std::clamp(blue, 0.0F, 1.0F),
            1.0F);
    }

    [[nodiscard]] double imageYForFrequency(double frequency) const noexcept {
        const double octave = std::log2(
            frequency / static_cast<double>(kMinimumFrequency));
        return static_cast<double>(image_.getHeight() - 1)
            - (octave - frequency_offset_) * pixelsPerOctave();
    }

    [[nodiscard]] double harmonicLobeRadiusPixels(
        double frequency) const noexcept {
        constexpr double kFftBinWidth = kSampleRate
            / static_cast<double>(kFftSize);
        constexpr double kHannMainLobeHalfWidthBins = 2.0;
        const double centre_y = imageYForFrequency(frequency);
        const double lower = std::max(
            static_cast<double>(kMinimumFrequency),
            frequency - kHannMainLobeHalfWidthBins * kFftBinWidth);
        const double upper = std::min(
            static_cast<double>(kMaximumFrequency),
            frequency + kHannMainLobeHalfWidthBins * kFftBinWidth);
        return std::clamp(
            std::max(
                std::abs(imageYForFrequency(lower) - centre_y),
                std::abs(imageYForFrequency(upper) - centre_y)),
            1.5,
            12.0);
    }

    [[nodiscard]] std::uint8_t emphasizedSourceLevel(
        std::uint8_t original,
        const PixelHarmonicPeakList& harmonics,
        std::uint8_t harmonic_index,
        int image_y) const noexcept {
        if (emphasis_percent_ <= 0.0
            || harmonic_index == kNoHarmonic
            || harmonic_index >= harmonics.count) {
            return original;
        }
        const auto& peak = harmonics.peaks[harmonic_index];
        const double radius = harmonicLobeRadiusPixels(peak.frequency);
        const double distance = std::abs(
            static_cast<double>(image_y)
            - imageYForFrequency(peak.frequency));
        return spectrogram::composeHarmonicRidgeLevel(
            original,
            peak.level,
            distance,
            radius,
            emphasis_percent_ / 100.0);
    }

    [[nodiscard]] SourceIntensity emphasizedIntensity(
        const SourceIntensity& original,
        const PixelHarmonics& harmonics,
        const HarmonicInfluence& influence,
        int image_y) const noexcept {
        return {
            emphasizedSourceLevel(
                original.psg, harmonics.psg, influence.psg, image_y),
            emphasizedSourceLevel(
                original.scc, harmonics.scc, influence.scc, image_y),
            emphasizedSourceLevel(
                original.opll, harmonics.opll, influence.opll, image_y),
        };
    }

    static void mergeHarmonics(
        PixelHarmonicPeakList& destination,
        const spectrogram::HarmonicPeakList& source) noexcept {
        constexpr float kSamePeakToleranceHz = static_cast<float>(
            kSampleRate / static_cast<double>(kFftSize));
        for (std::size_t source_index = 0;
             source_index < source.count;
             ++source_index) {
            const auto candidate = source.peaks[source_index];
            bool merged = false;
            for (std::size_t index = 0; index < destination.count; ++index) {
                auto& existing = destination.peaks[index];
                if (std::abs(existing.frequency - candidate.frequency)
                    <= kSamePeakToleranceHz) {
                    if (candidate.level > existing.level) {
                        existing = candidate;
                    }
                    merged = true;
                    break;
                }
            }
            if (!merged && destination.count < destination.peaks.size()) {
                destination.peaks[destination.count++] = candidate;
            }
        }
    }

    void rebuildHarmonicInfluencesForSource(
        const PixelHarmonicPeakList& harmonics,
        std::size_t column_offset,
        std::size_t height,
        std::uint8_t HarmonicInfluence::* member) noexcept {
        for (std::size_t y = 0; y < height; ++y) {
            harmonic_influences_[column_offset + y].*member = kNoHarmonic;
        }
        for (std::size_t peak_index = 0;
             peak_index < harmonics.count;
             ++peak_index) {
            const auto& peak = harmonics.peaks[peak_index];
            const double centre_y = imageYForFrequency(peak.frequency);
            const double radius = harmonicLobeRadiusPixels(peak.frequency);
            const int first_y = std::max(
                0, static_cast<int>(std::floor(centre_y - radius)));
            const int last_y = std::min(
                static_cast<int>(height) - 1,
                static_cast<int>(std::ceil(centre_y + radius)));
            for (int y = first_y; y <= last_y; ++y) {
                const double candidate_distance = std::abs(
                    static_cast<double>(y) - centre_y) / radius;
                if (candidate_distance > 1.0) {
                    continue;
                }
                auto& nearest = harmonic_influences_[
                    column_offset + static_cast<std::size_t>(y)].*member;
                if (nearest == kNoHarmonic) {
                    nearest = static_cast<std::uint8_t>(peak_index);
                    continue;
                }
                const auto& current = harmonics.peaks[nearest];
                const double current_distance = std::abs(
                    static_cast<double>(y)
                    - imageYForFrequency(current.frequency))
                    / harmonicLobeRadiusPixels(current.frequency);
                if (candidate_distance < current_distance) {
                    nearest = static_cast<std::uint8_t>(peak_index);
                }
            }
        }
    }

    void rebuildHarmonicInfluencesForColumn(
        std::size_t x,
        std::size_t height) noexcept {
        const auto column_offset = x * height;
        const auto& harmonics = pixel_harmonics_[x];
        rebuildHarmonicInfluencesForSource(
            harmonics.psg,
            column_offset,
            height,
            &HarmonicInfluence::psg);
        rebuildHarmonicInfluencesForSource(
            harmonics.scc,
            column_offset,
            height,
            &HarmonicInfluence::scc);
        rebuildHarmonicInfluencesForSource(
            harmonics.opll,
            column_offset,
            height,
            &HarmonicInfluence::opll);
    }

    [[nodiscard]] static bool columnIsSilent(
        const SpectrogramColumn& column) noexcept {
        return std::none_of(
                   column.psg.begin(), column.psg.end(),
                   [](std::uint8_t level) { return level != 0; })
            && std::none_of(
                   column.scc.begin(), column.scc.end(),
                   [](std::uint8_t level) { return level != 0; })
            && std::none_of(
                   column.opll.begin(), column.opll.end(),
                   [](std::uint8_t level) { return level != 0; });
    }

    void rebuildFrequencyBinCache() {
        if (!image_.isValid()) {
            frequency_bins_.clear();
            return;
        }
        frequency_bins_.resize(static_cast<std::size_t>(image_.getHeight()));
        for (int y = 0; y < image_.getHeight(); ++y) {
            frequency_bins_[static_cast<std::size_t>(y)] = logBinForY(y);
        }
    }

    void recomposeImage() {
        if (!image_.isValid()
            || source_intensities_.size()
                != static_cast<std::size_t>(
                    image_.getWidth() * image_.getHeight())
            || pixel_harmonics_.size()
                != static_cast<std::size_t>(image_.getWidth())
            || harmonic_influences_.size() != source_intensities_.size()) {
            rebuildImage();
            return;
        }
        const auto height = static_cast<std::size_t>(image_.getHeight());
        juce::Image::BitmapData bitmap(
            image_, juce::Image::BitmapData::readWrite);
        for (int x = 0; x < image_.getWidth(); ++x) {
            const auto column_offset = static_cast<std::size_t>(x) * height;
            for (int y = 0; y < image_.getHeight(); ++y) {
                bitmap.setPixelColour(
                    x,
                    y,
                    colourFor(
                        emphasizedIntensity(
                            source_intensities_[
                                column_offset + static_cast<std::size_t>(y)],
                            pixel_harmonics_[static_cast<std::size_t>(x)],
                            harmonic_influences_[
                                column_offset + static_cast<std::size_t>(y)],
                            y)));
            }
        }
        dirty_first_x_ = std::numeric_limits<int>::max();
        dirty_last_x_ = -1;
        requestFullRepaint();
    }

    void drawColumn(const SpectrogramColumn& column) {
        if (!image_.isValid() || image_.getWidth() <= 0) {
            return;
        }
        const double first_pixel = static_cast<double>(column.sequence - 1)
            * time_scale_;
        const double next_pixel = static_cast<double>(column.sequence)
            * time_scale_;
        const auto first = static_cast<std::uint64_t>(std::floor(first_pixel));
        const auto last = std::max(
            first,
            static_cast<std::uint64_t>(std::ceil(next_pixel)) - 1);
        const auto width = static_cast<std::uint64_t>(image_.getWidth());
        const auto height = static_cast<std::size_t>(image_.getHeight());
        const bool silent = columnIsSilent(column);
        juce::Image::BitmapData bitmap(
            image_, juce::Image::BitmapData::readWrite);
        for (auto absolute_pixel = first;
             absolute_pixel <= last;
             ++absolute_pixel) {
            const int x = static_cast<int>(absolute_pixel % width);
            const std::uint64_t cycle = absolute_pixel / width;
            const bool blend = pixel_cycles_[static_cast<std::size_t>(x)]
                == cycle;
            const auto column_offset = static_cast<std::size_t>(x) * height;
            auto& cached_harmonics =
                pixel_harmonics_[static_cast<std::size_t>(x)];
            if (silent) {
                if (!blend) {
                    std::fill_n(
                        source_intensities_.begin()
                            + static_cast<std::ptrdiff_t>(column_offset),
                        height,
                        SourceIntensity{});
                    cached_harmonics = {};
                    std::fill_n(
                        harmonic_influences_.begin()
                            + static_cast<std::ptrdiff_t>(column_offset),
                        height,
                        HarmonicInfluence{});
                    for (int y = 0; y < image_.getHeight(); ++y) {
                        bitmap.setPixelColour(x, y, juce::Colours::black);
                    }
                }
                pixel_cycles_[static_cast<std::size_t>(x)] = cycle;
                dirty_first_x_ = std::min(dirty_first_x_, x);
                dirty_last_x_ = std::max(dirty_last_x_, x);
                continue;
            }
            if (!blend) {
                cached_harmonics = {};
            }
            mergeHarmonics(cached_harmonics.psg, column.psg_harmonics);
            mergeHarmonics(cached_harmonics.scc, column.scc_harmonics);
            mergeHarmonics(cached_harmonics.opll, column.opll_harmonics);
            rebuildHarmonicInfluencesForColumn(
                static_cast<std::size_t>(x), height);
            for (int y = 0; y < image_.getHeight(); ++y) {
                const auto bin = frequency_bins_[static_cast<std::size_t>(y)];
                SourceIntensity next{};
                if (bin < kLogBinCount) {
                    next = {
                        column.psg[bin],
                        column.scc[bin],
                        column.opll[bin],
                    };
                }
                auto& cached = source_intensities_[
                    column_offset + static_cast<std::size_t>(y)];
                if (blend) {
                    cached.psg = std::max(cached.psg, next.psg);
                    cached.scc = std::max(cached.scc, next.scc);
                    cached.opll = std::max(cached.opll, next.opll);
                } else {
                    cached = next;
                }
            }
            for (int y = 0; y < image_.getHeight(); ++y) {
                const auto pixel_offset = column_offset
                    + static_cast<std::size_t>(y);
                bitmap.setPixelColour(
                    x,
                    y,
                    colourFor(
                        emphasizedIntensity(
                            source_intensities_[pixel_offset],
                            cached_harmonics,
                            harmonic_influences_[pixel_offset],
                            y)));
            }
            pixel_cycles_[static_cast<std::size_t>(x)] = cycle;
            dirty_first_x_ = std::min(dirty_first_x_, x);
            dirty_last_x_ = std::max(dirty_last_x_, x);
        }
    }

    void rebuildImage() {
        const auto plot = plotBounds();
        if (plot.getWidth() <= 0 || plot.getHeight() <= 0) {
            image_ = {};
            source_intensities_.clear();
            frequency_bins_.clear();
            pixel_cycles_.clear();
            pixel_harmonics_.clear();
            harmonic_influences_.clear();
            dirty_first_x_ = std::numeric_limits<int>::max();
            dirty_last_x_ = -1;
            requestFullRepaint();
            return;
        }
        image_ = juce::Image(
            juce::Image::RGB,
            plot.getWidth(),
            plot.getHeight(),
            true);
        source_intensities_.assign(
            static_cast<std::size_t>(plot.getWidth() * plot.getHeight()),
            SourceIntensity{});
        rebuildFrequencyBinCache();
        pixel_cycles_.assign(
            static_cast<std::size_t>(plot.getWidth()),
            std::numeric_limits<std::uint64_t>::max());
        pixel_harmonics_.assign(
            static_cast<std::size_t>(plot.getWidth()),
            PixelHarmonics{});
        harmonic_influences_.assign(
            source_intensities_.size(),
            HarmonicInfluence{});
        dirty_first_x_ = std::numeric_limits<int>::max();
        dirty_last_x_ = -1;
        const auto wanted = static_cast<std::size_t>(std::ceil(
            static_cast<double>(plot.getWidth()) / time_scale_)) + 2;
        const auto count = std::min(history_count_, wanted);
        const auto oldest = (history_write_ + history_.size() - count)
            % history_.size();
        for (std::size_t index = 0; index < count; ++index) {
            drawColumn(history_[(oldest + index) % history_.size()]);
        }
        dirty_first_x_ = std::numeric_limits<int>::max();
        dirty_last_x_ = -1;
        requestFullRepaint();
    }

    void drawFrequencyAxis(
        juce::Graphics& graphics,
        juce::Rectangle<int> plot) const {
        constexpr std::array<double, 10> ticks{
            30.0, 50.0, 100.0, 200.0, 500.0,
            1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
        graphics.setFont(static_cast<float>(UiScale::sx(11)));
        for (const double frequency : ticks) {
            const int y = yForFrequency(frequency);
            if (y == std::numeric_limits<int>::min()) {
                continue;
            }
            graphics.setColour(juce::Colour(0x3034404C));
            graphics.drawHorizontalLine(
                y,
                static_cast<float>(plot.getX()),
                static_cast<float>(plot.getRight()));
            graphics.setColour(juce::Colour(0xFF9AA8B5));
            const juce::String text = frequency >= 1000.0
                ? juce::String(frequency / 1000.0, frequency < 10'000.0 ? 1 : 0)
                    + "k"
                : juce::String(juce::roundToInt(frequency));
            graphics.drawText(
                text,
                UiScale::sx(2),
                y - UiScale::sx(8),
                UiScale::sx(52),
                UiScale::sx(16),
                juce::Justification::centredRight,
                false);
        }
        graphics.setColour(juce::Colour(0xFF68737E));
        graphics.drawText(
            juce::String::fromUTF8("時間 →"),
            plot.getX(),
            plot.getBottom() + UiScale::sx(2),
            plot.getWidth(),
            UiScale::sx(20),
            juce::Justification::centredRight,
            false);
    }

    void drawHarmonicGuides(
        juce::Graphics& graphics,
        juce::Rectangle<int> plot) const {
        const double fundamental = cursor_inside_
            ? frequencyAtPlotY(cursor_position_.y)
            : noteFrequencyForTrack(guide_note_, guide_track_);
        const float alpha = cursor_inside_ ? 0.72F
            : note_active_ ? 0.48F : 0.23F;
        graphics.setFont(static_cast<float>(UiScale::sx(10)));
        for (int harmonic = 1; harmonic <= 16; ++harmonic) {
            const double frequency = fundamental * harmonic;
            const int y = yForFrequency(frequency);
            if (y == std::numeric_limits<int>::min()) {
                continue;
            }
            graphics.setColour(juce::Colours::white.withAlpha(alpha));
            graphics.drawHorizontalLine(
                y,
                static_cast<float>(plot.getX()),
                static_cast<float>(plot.getRight()));
            graphics.drawText(
                juce::String(harmonic) + "x",
                plot.getX() + UiScale::sx(3),
                y - UiScale::sx(12),
                UiScale::sx(28),
                UiScale::sx(12),
                juce::Justification::centredLeft,
                false);
        }
    }

    void drawCursor(
        juce::Graphics& graphics,
        juce::Rectangle<int> plot) const {
        if (!cursor_inside_) {
            return;
        }
        graphics.setColour(juce::Colours::white.withAlpha(0.35F));
        graphics.drawVerticalLine(
            cursor_position_.x,
            static_cast<float>(plot.getY()),
            static_cast<float>(plot.getBottom()));
        const double frequency = frequencyAtPlotY(cursor_position_.y);
        const int write_x = latest_sequence_ == 0
            ? 0
            : static_cast<int>(std::fmod(
                static_cast<double>(latest_sequence_) * time_scale_,
                static_cast<double>(std::max(1, plot.getWidth()))));
        const int cursor_x = cursor_position_.x - plot.getX();
        const int pixels_ago = (write_x - cursor_x + plot.getWidth())
            % plot.getWidth();
        const double milliseconds = static_cast<double>(pixels_ago)
            / (time_scale_ * kColumnsPerSecond) * 1000.0;
        juce::String status;
        status << "-" << juce::String(milliseconds, 0) << " ms / "
               << juce::String(frequency, 1) << " Hz / "
               << nearestNoteName(frequency);
        const int status_width = UiScale::sx(184);
        const int status_height = UiScale::sx(18);
        const auto box = juce::Rectangle<int>(
            juce::jlimit(
                plot.getX(),
                plot.getRight() - status_width - UiScale::sx(6),
                cursor_position_.x + UiScale::sx(8)),
            juce::jlimit(
                plot.getY(),
                plot.getBottom() - status_height - UiScale::sx(4),
                cursor_position_.y + UiScale::sx(8)),
            status_width,
            status_height);
        graphics.setColour(juce::Colour(0xD0182028));
        graphics.fillRoundedRectangle(box.toFloat(), 3.0F);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.setFont(static_cast<float>(UiScale::sx(11)));
        graphics.drawText(
            status,
            box.reduced(UiScale::sx(4), 0),
            juce::Justification::centredLeft);
    }

    void drawWritePosition(
        juce::Graphics& graphics,
        juce::Rectangle<int> plot) const {
        const int image_x = writePositionImageX();
        if (image_x < 0) {
            return;
        }
        const int x = plot.getX() + image_x;
        graphics.setColour(juce::Colour(0x9060E8FF));
        graphics.drawVerticalLine(
            x,
            static_cast<float>(plot.getY()),
            static_cast<float>(plot.getBottom()));
    }

    std::vector<SpectrogramColumn> history_;
    std::size_t history_write_{};
    std::size_t history_count_{};
    juce::Image image_;
    std::vector<SourceIntensity> source_intensities_;
    std::vector<std::size_t> frequency_bins_;
    std::vector<std::uint64_t> pixel_cycles_;
    std::vector<PixelHarmonics> pixel_harmonics_;
    std::vector<HarmonicInfluence> harmonic_influences_;
    std::function<void(double)> frequency_scroll_;
    juce::Point<int> cursor_position_{};
    std::uint64_t latest_sequence_{};
    std::uint64_t source_sequence_{};
    double time_scale_{1.0};
    double frequency_zoom_{1.0};
    double frequency_offset_{};
    double emphasis_percent_{};
    int painted_write_x_{-1};
    GuideState painted_guide_state_{};
    bool painted_guide_valid_{true};
    int dirty_first_x_{std::numeric_limits<int>::max()};
    int dirty_last_x_{-1};
    std::uint8_t guide_note_{60};
    std::uint8_t guide_track_{};
    bool note_active_{};
    bool cursor_inside_{};
    bool show_psg_{true};
    bool show_scc_{true};
    bool show_opll_{true};
};

[[nodiscard]] juce::String readIniString(
    const wchar_t* key,
    const wchar_t* fallback = L"") {
    const auto file = mgstcApplicationDataDirectory().getChildFile(
        "settings-v1.ini");
    std::array<wchar_t, 1024> buffer{};
    GetPrivateProfileStringW(
        L"Spectrogram",
        key,
        fallback,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        file.getFullPathName().toWideCharPointer());
    return juce::String(buffer.data());
}

void writeIniString(const wchar_t* key, const juce::String& value) {
    const auto file = mgstcApplicationDataDirectory().getChildFile(
        "settings-v1.ini");
    if (file.getParentDirectory().createDirectory().failed()) {
        return;
    }
    WritePrivateProfileStringW(
        L"Spectrogram",
        key,
        value.toWideCharPointer(),
        file.getFullPathName().toWideCharPointer());
}

[[nodiscard]] double readIniDouble(const wchar_t* key, double fallback) {
    const auto text = readIniString(key);
    return text.isEmpty() ? fallback : text.getDoubleValue();
}

[[nodiscard]] bool readIniBool(const wchar_t* key, bool fallback) {
    const auto text = readIniString(key);
    return text.isEmpty() ? fallback : text.getIntValue() != 0;
}

}  // namespace

class SpectrogramWindow::Content final
    : public juce::Component,
      private juce::Timer,
      private juce::ScrollBar::Listener {
public:
    explicit Content(
        engine::RealtimeEngineHost& engine,
        std::function<void(bool)> internal_pin_changed)
        : analyzer_(engine),
          internal_pin_changed_(std::move(internal_pin_changed)),
          frequency_scroll_(true) {
        psg_.setButtonText("PSG");
        scc_.setButtonText("SCC");
        opll_.setButtonText("OPLL");
        for (auto* button : std::array<juce::ToggleButton*, 3>{
                 &psg_, &scc_, &opll_}) {
            button->setToggleState(true, juce::dontSendNotification);
            button->setLookAndFeel(&source_toggle_look_and_feel_);
            button->onClick = [this] { updateSourceVisibility(); };
            addAndMakeVisible(*button);
        }
        psg_.setColour(juce::ToggleButton::textColourId, kPsgColour);
        scc_.setColour(juce::ToggleButton::textColourId, kSccColour);
        opll_.setColour(juce::ToggleButton::textColourId, kOpllColour);

        configurePinButton(pin_);
        pin_.onClick = [this] {
            if (internal_pin_changed_) {
                internal_pin_changed_(pin_.getToggleState());
            }
        };
        addAndMakeVisible(pin_);

        time_label_.setText(
            juce::String::fromUTF8("時間"),
            juce::dontSendNotification);
        frequency_label_.setText(
            juce::String::fromUTF8("周波数"),
            juce::dontSendNotification);
        emphasis_label_.setText(
            juce::String::fromUTF8("強調率"),
            juce::dontSendNotification);
        for (auto* label : std::array<juce::Label*, 3>{
                 &time_label_, &frequency_label_, &emphasis_label_}) {
            label->setJustificationType(juce::Justification::centredRight);
        }
        addAndMakeVisible(time_label_);
        addAndMakeVisible(frequency_label_);
        addAndMakeVisible(emphasis_label_);

        configureZoomSlider(time_zoom_, 0.25, 4.0, 1.0);
        configureZoomSlider(frequency_zoom_, 0.5, 4.0, 1.0);
        time_zoom_.onValueChange = [this] {
            analyzer_.discardPendingColumns();
            display_.setTimeScale(time_zoom_.getValue());
        };
        frequency_zoom_.onValueChange = [this] {
            display_.setFrequencyZoom(frequency_zoom_.getValue());
            resized();
        };
        configureEmphasisSlider(emphasis_);
        emphasis_.onValueChange = [this] {
            emphasis_update_pending_ = true;
        };
        emphasis_.onDragEnd = [this] { applyPendingEmphasis(); };
        addAndMakeVisible(time_zoom_);
        addAndMakeVisible(frequency_zoom_);
        addAndMakeVisible(emphasis_);

        frequency_scroll_.setAutoHide(false);
        frequency_scroll_.addListener(this);
        addAndMakeVisible(frequency_scroll_);
        display_.setFrequencyScrollCallback([this](double offset) {
            resetFrequencyView(offset);
            frequency_scroll_.setCurrentRangeStart(
                display_.maximumFrequencyOffset()
                    - display_.frequencyOffset(),
                juce::dontSendNotification);
        });
        addAndMakeVisible(display_);
        setSize(UiScale::sx(960), UiScale::sx(560));
    }

    ~Content() override {
        setActive(false);
        frequency_scroll_.removeListener(this);
        for (auto* button : std::array<juce::ToggleButton*, 3>{
                 &psg_, &scc_, &opll_}) {
            button->setLookAndFeel(nullptr);
        }
    }

    void applyGlobalUiScale() {
        UiScale::forceGlobalForNonEditorUi();
        time_zoom_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            UiScale::sx(54),
            UiScale::sx(22));
        frequency_zoom_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            UiScale::sx(54),
            UiScale::sx(22));
        emphasis_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            UiScale::sx(54),
            UiScale::sx(22));
        sendLookAndFeelChange();
        resized();
        repaint();
    }

    void setActive(bool active) {
        analyzer_.setCaptureEnabled(active);
        if (active) {
            startTimerHz(30);
        } else {
            stopTimer();
        }
    }

    void setAnalysisMode(SpectrogramAnalysisMode mode) {
        analyzer_.setSourceMask(sourceMaskForMode(mode));
    }

    void loadControls() {
        // Source visibility is intentionally session-only.  Every new app
        // launch/window instance starts with all three sources enabled.
        psg_.setToggleState(true, juce::dontSendNotification);
        scc_.setToggleState(true, juce::dontSendNotification);
        opll_.setToggleState(true, juce::dontSendNotification);
        time_zoom_.setValue(
            readIniDouble(L"TimeZoom", 1.0), juce::dontSendNotification);
        frequency_zoom_.setValue(
            readIniDouble(L"FrequencyZoom", 1.0),
            juce::dontSendNotification);
        emphasis_.setValue(
            readIniDouble(L"Emphasis", 0.0),
            juce::dontSendNotification);
        display_.setTimeScale(time_zoom_.getValue());
        display_.setFrequencyZoom(frequency_zoom_.getValue());
        display_.setEmphasis(emphasis_.getValue());
        display_.setFrequencyOffset(
            readIniDouble(L"FrequencyOffset", 0.0));
        updateSourceVisibility();
        updateScrollBar();
    }

    void setInternalPin(bool pinned) {
        pin_.setToggleState(pinned, juce::dontSendNotification);
    }

    [[nodiscard]] bool internalPin() const noexcept {
        return pin_.getToggleState();
    }

    void saveControls() const {
        writeIniString(L"TimeZoom", juce::String(time_zoom_.getValue(), 3));
        writeIniString(
            L"FrequencyZoom",
            juce::String(frequency_zoom_.getValue(), 3));
        writeIniString(
            L"FrequencyOffset",
            juce::String(display_.frequencyOffset(), 4));
        writeIniString(L"Emphasis", juce::String(emphasis_.getValue(), 1));
    }

    void paint(juce::Graphics& graphics) override {
        UiScale::forceGlobalForNonEditorUi();
        graphics.fillAll(juce::Colour(0xFF1B222C));
    }

    void resized() override {
        UiScale::forceGlobalForNonEditorUi();
        auto area = getLocalBounds().reduced(UiLayout::sm);
        auto controls = area.removeFromTop(UiLayout::fieldH);
        psg_.setBounds(controls.removeFromLeft(UiScale::sx(66)));
        controls.removeFromLeft(UiLayout::controlGap);
        scc_.setBounds(controls.removeFromLeft(UiScale::sx(66)));
        controls.removeFromLeft(UiLayout::controlGap);
        opll_.setBounds(controls.removeFromLeft(UiScale::sx(72)));
        controls.removeFromLeft(UiLayout::controlGap);
        const auto layoutSlider = [&](juce::Label& label, juce::Slider& slider) {
            label.setBounds(
                controls.removeFromLeft(UiLayout::spectrogramLabelW));
            controls.removeFromLeft(UiLayout::controlGap);
            slider.setBounds(
                controls.removeFromLeft(UiLayout::spectrogramSliderW));
        };
        layoutSlider(time_label_, time_zoom_);
        controls.removeFromLeft(UiLayout::controlGap);
        layoutSlider(frequency_label_, frequency_zoom_);
        controls.removeFromLeft(UiLayout::controlGap);
        layoutSlider(emphasis_label_, emphasis_);
        pin_.setBounds(
            controls.removeFromRight(UiScale::sx(28)));
        area.removeFromTop(UiLayout::controlGap);
        display_.setBounds(area);
        updateScrollBar();
        if (frequency_scroll_.isVisible()) {
            frequency_scroll_.setBounds(
                area.removeFromRight(UiScale::sx(18)));
            area.removeFromRight(UiScale::sx(4));
            display_.setBounds(area);
            updateScrollBar();
        } else {
            frequency_scroll_.setBounds({});
        }
    }

private:
    static void configureZoomSlider(
        juce::Slider& slider,
        double minimum,
        double maximum,
        double initial) {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(minimum, maximum, 0.05);
        slider.setValue(initial, juce::dontSendNotification);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            UiScale::sx(54),
            UiScale::sx(22));
        slider.setTextValueSuffix("x");
        slider.setSkewFactorFromMidPoint(1.0);
    }

    static void configureEmphasisSlider(juce::Slider& slider) {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(0.0, 100.0, 1.0);
        slider.setValue(0.0, juce::dontSendNotification);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            UiScale::sx(54),
            UiScale::sx(22));
        slider.setTextValueSuffix("%");
        slider.setDoubleClickReturnValue(true, 0.0);
    }

    void applyPendingEmphasis() {
        if (!emphasis_update_pending_) {
            return;
        }
        emphasis_update_pending_ = false;
        display_.setEmphasis(emphasis_.getValue());
    }

    void timerCallback() override {
        UiScale::forceGlobalForNonEditorUi();
        applyPendingEmphasis();
        SpectrogramColumn column{};
        bool changed = false;
        while (analyzer_.pollColumn(column)) {
            display_.appendColumn(column);
            changed = true;
        }
        if (changed) {
            display_.flushPendingRepaint();
        }
    }

    void scrollBarMoved(juce::ScrollBar*, double new_range_start) override {
        resetFrequencyView(
            display_.maximumFrequencyOffset() - new_range_start);
    }

    void resetFrequencyView(double offset) {
        display_.setFrequencyOffset(offset);
    }

    void updateSourceVisibility() {
        display_.setSourceVisibility(
            psg_.getToggleState(),
            scc_.getToggleState(),
            opll_.getToggleState());
    }

    void updateScrollBar() {
        const double total = display_.totalOctaves();
        const double visible = std::min(total, display_.visibleOctaves());
        const bool scrollable = visible + 0.0001 < total;
        frequency_scroll_.setVisible(scrollable);
        if (!scrollable) {
            display_.setFrequencyOffset(0.0);
        }
        frequency_scroll_.setRangeLimits(
            0.0, total, juce::dontSendNotification);
        frequency_scroll_.setCurrentRange(
            display_.maximumFrequencyOffset() - display_.frequencyOffset(),
            visible,
            juce::dontSendNotification);
        frequency_scroll_.setSingleStepSize(0.25);
    }

    SpectrumAnalyzerThread analyzer_;
    SpectrogramDisplay display_;
    std::function<void(bool)> internal_pin_changed_;
    SpectrogramToggleLookAndFeel source_toggle_look_and_feel_;
    juce::ToggleButton psg_;
    juce::ToggleButton scc_;
    juce::ToggleButton opll_;
    juce::Label time_label_;
    juce::Label frequency_label_;
    juce::Label emphasis_label_;
    juce::Slider time_zoom_;
    juce::Slider frequency_zoom_;
    juce::Slider emphasis_;
    bool emphasis_update_pending_{};
    juce::DrawableButton pin_{
        "spectrogram-internal-pin",
        juce::DrawableButton::ImageOnButtonBackground};
    juce::ScrollBar frequency_scroll_;
};

SpectrogramWindow::SpectrogramWindow(engine::RealtimeEngineHost& engine)
    : DocumentWindow(
          juce::String::fromUTF8("MGS Tone Craft - スペクトログラム"),
          juce::Colour(0xFF1B222C),
          DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    content_ = new Content(engine, [this](bool pinned) {
        setInternalPin(pinned);
    });
    setContentOwned(content_, true);
    setResizable(true, false);
    {
        juce::Component::SafePointer<SpectrogramWindow> safe(this);
        UiScale::addGlobalListener([safe] {
            if (safe != nullptr) {
                safe->applyGlobalUiScale();
            }
        });
    }
    applyGlobalUiScale();
    loadState();
    setVisible(false);
}

SpectrogramWindow::~SpectrogramWindow() {
    stopTimer();
    if (content_ != nullptr) {
        content_->setActive(false);
    }
    saveState();
}

void SpectrogramWindow::showWindow() {
    applyGlobalUiScale();
    if (content_ != nullptr) {
        content_->setActive(true);
    }
    setVisible(true);
    toFront(true);
    if (internal_pin_) {
        startTimerHz(15);
    }
}

void SpectrogramWindow::setAnalysisMode(SpectrogramAnalysisMode mode) {
    if (content_ != nullptr) {
        content_->setAnalysisMode(mode);
    }
}

void SpectrogramWindow::setInternalPin(bool pinned) {
    internal_pin_ = pinned;
    if (!internal_pin_ || !isVisible()) {
        stopTimer();
        return;
    }
    startTimerHz(15);
    toFront(false);
}

void SpectrogramWindow::timerCallback() {
    if (!internal_pin_ || !isVisible()) {
        stopTimer();
        return;
    }
    const auto* active = juce::TopLevelWindow::getActiveTopLevelWindow();
    if (active != nullptr && active != this) {
        toFront(false);
    }
}

void SpectrogramWindow::applyGlobalUiScale() {
    UiScale::forceGlobalForNonEditorUi();
    setResizeLimits(
        UiScale::sx(760),
        UiScale::sx(360),
        UiScale::sx(2400),
        UiScale::sx(1600));
    if (content_ != nullptr) {
        content_->applyGlobalUiScale();
    }
}

void SpectrogramWindow::closeButtonPressed() {
    hideWindow();
}

void SpectrogramWindow::hideWindow() {
    stopTimer();
    if (content_ != nullptr) {
        content_->setActive(false);
    }
    saveState();
    setVisible(false);
}

void SpectrogramWindow::loadState() {
    if (content_ != nullptr) {
        content_->loadControls();
        const auto pinned = readIniBool(L"InternalPin", false);
        content_->setInternalPin(pinned);
        internal_pin_ = pinned;
    }
    const auto state = readIniString(L"WindowState");
    if (state.isEmpty() || !restoreWindowStateFromString(state)) {
        centreWithSize(UiScale::sx(960), UiScale::sx(560));
    }
}

void SpectrogramWindow::saveState() {
    if (content_ != nullptr) {
        content_->saveControls();
        writeIniString(
            L"InternalPin",
            content_->internalPin() ? "1" : "0");
    }
    writeIniString(L"WindowState", getWindowStateAsString());
    const auto file = mgstcApplicationDataDirectory().getChildFile(
        "settings-v1.ini");
    WritePrivateProfileStringW(
        nullptr,
        nullptr,
        nullptr,
        file.getFullPathName().toWideCharPointer());
}

}  // namespace mgstc::app
