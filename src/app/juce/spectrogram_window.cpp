// SPDX-License-Identifier: AGPL-3.0-only

#include "spectrogram_window.hpp"
#include "spectrogram_emphasis.hpp"
#include "spectrum_display.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <charconv>
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

SpectrogramWindow* g_spectrogram_window = nullptr;
bool g_in_pin_zorder = false;
HHOOK g_pin_zorder_ret_hook = nullptr;

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

using SpectrogramColumn = spectrum::Column;
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

using SpectrumAnalyzerThread = spectrum::Analyzer;

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
    explicit SpectrogramDisplay(spectrum::GuideState& guide)
        : common_guide_(guide), history_(kHistoryCapacity) {
        setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        setOpaque(true);
    }

    void appendColumn(const SpectrogramColumn& column) {
        if (source_context_ != column.context) clearHistory();
        const auto advance = source_sequence_ == 0 ? std::uint64_t{1}
            : std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(
                static_cast<double>(column.sample_position - source_sample_) / kFftHop)));
        source_sequence_ = column.sequence;
        source_sample_ = column.sample_position;
        source_context_ = column.context;
        auto displayed = column;
        displayed.sequence = latest_sequence_ + advance;
        history_[history_write_] = displayed;
        history_write_ = (history_write_ + 1) % history_.size();
        history_count_ = std::min(history_count_ + 1, history_.size());
        latest_sequence_ = displayed.sequence;
        guide_note_ = column.guide_note;
        guide_track_ = column.guide_track;
        note_active_ = column.note_active;
        if (!drawing_active_) image_dirty_ = true;
        else if (image_.isValid()) {
            if (advance > 1) clearMissingColumns(displayed.sequence - advance, displayed.sequence);
            drawColumn(displayed);
        }
    }

    void setDrawingActive(bool active) {
        drawing_active_ = active;
        cursor_position_ = getMouseXYRelative();
        cursor_inside_ = plotBounds().contains(cursor_position_);
        if (active && image_dirty_) { image_dirty_ = false; rebuildImage(); }
        if (active) requestFullRepaint();
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey && common_guide_.fixed()) {
            common_guide_.release(); requestFullRepaint(); return true;
        }
        return false;
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isLeftButtonDown() && plotBounds().contains(e.getPosition())) {
            common_guide_.click(frequencyAtPlotY(e.y));
            grabKeyboardFocus(); requestFullRepaint();
        }
    }
    void clearHistory() {
        history_count_ = 0;
        history_write_ = 0;
        latest_sequence_ = 0;
        source_sequence_ = 0;
        source_context_ = source_sample_ = 0;
        if (!drawing_active_) { image_dirty_ = true; return; }
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
        if (!drawing_active_) return;
        const auto plot = plotBounds();
        const int write_x = writePositionImageX();
        const GuideState guide_state{guide_note_, guide_track_, note_active_};
        const bool guide_changed = !common_guide_.fixed() && !cursor_inside_
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
        if (common_guide_.fixed()) {
            graphics.setFont(UiFonts::dense());
            graphics.setColour(juce::Colours::white);
            graphics.drawText(juce::String::fromUTF8("基準固定：")
                + juce::String(common_guide_.fixed_frequency, 1) + " Hz",
                plot.getX(), plot.getBottom(), plot.getWidth() / 2, UiLayout::fieldH,
                juce::Justification::centredLeft, false);
        }
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
        if (!drawing_active_) { image_dirty_ = true; return; }
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

    void clearMissingColumns(std::uint64_t previous, std::uint64_t next) {
        const auto first = static_cast<std::uint64_t>(std::ceil(previous * time_scale_));
        const auto end = static_cast<std::uint64_t>(std::floor((next - 1) * time_scale_));
        if (end <= first) return;
        const auto width = static_cast<std::uint64_t>(image_.getWidth());
        const auto height = static_cast<std::size_t>(image_.getHeight());
        // At most one image width even after a long acquisition interruption.
        const auto begin = std::max(first, end > width ? end - width : 0);
        juce::Image::BitmapData bitmap(image_, juce::Image::BitmapData::readWrite);
        for (auto absolute = begin; absolute < end; ++absolute) {
            const auto x = static_cast<std::size_t>(absolute % width);
            const auto offset = static_cast<std::ptrdiff_t>(x * height);
            std::fill_n(source_intensities_.begin() + offset, height, SourceIntensity{});
            std::fill_n(harmonic_influences_.begin() + offset, height, HarmonicInfluence{});
            pixel_harmonics_[x] = {};
            pixel_cycles_[x] = absolute / width;
            for (int y = 0; y < image_.getHeight(); ++y)
                bitmap.setPixelColour(static_cast<int>(x), y, juce::Colours::black);
            dirty_first_x_ = std::min(dirty_first_x_, static_cast<int>(x));
            dirty_last_x_ = std::max(dirty_last_x_, static_cast<int>(x));
        }
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
        if (!drawing_active_) { image_dirty_ = true; return; }
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
            const auto& column = history_[(oldest + index) % history_.size()];
            if (latest_sequence_ - column.sequence <= wanted) drawColumn(column);
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
        const double fundamental = common_guide_.fixed() ? common_guide_.fixed_frequency : cursor_inside_
            ? frequencyAtPlotY(cursor_position_.y)
            : noteFrequencyForTrack(guide_note_, guide_track_);
        const float alpha = common_guide_.fixed() || cursor_inside_ ? 0.72F
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

    spectrum::GuideState& common_guide_;
    bool drawing_active_{true}, image_dirty_{};
    std::uint64_t source_context_{}, source_sample_{};
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

[[nodiscard]] HWND spectrogramNativeHandle() {
    if (g_spectrogram_window == nullptr) {
        return nullptr;
    }
    auto* peer = g_spectrogram_window->getPeer();
    if (peer == nullptr) {
        return nullptr;
    }
    return static_cast<HWND>(peer->getNativeHandle());
}

[[nodiscard]] bool anotherProcessIsForeground() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id != GetCurrentProcessId();
}

[[nodiscard]] bool shouldKeepPinnedAboveSiblings() {
    return g_spectrogram_window != nullptr
        && g_spectrogram_window->isVisible()
        && !g_spectrogram_window->isMinimised()
        && !anotherProcessIsForeground()
        && juce::ModalComponentManager::getInstance()
                ->getNumModalComponents()
            == 0;
}

void raiseSpectrogramWithoutActivating(HWND spectrogram) {
    if (spectrogram == nullptr || g_in_pin_zorder) {
        return;
    }
    if (GetWindow(spectrogram, GW_HWNDPREV) == nullptr) {
        return;
    }
    g_in_pin_zorder = true;
    constexpr UINT kFlags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // TOPMOST then NOTOPMOST places this window above the foreground
    // sibling without leaving WS_EX_TOPMOST (other apps can still cover it).
    SetWindowPos(spectrogram, HWND_TOPMOST, 0, 0, 0, 0, kFlags);
    SetWindowPos(spectrogram, HWND_NOTOPMOST, 0, 0, 0, 0, kFlags);
    g_in_pin_zorder = false;
}

LRESULT CALLBACK pinZOrderCallWndRetProc(
    int code,
    WPARAM w_param,
    LPARAM l_param) {
    if (code == HC_ACTION && l_param != 0) {
        const auto* info = reinterpret_cast<const CWPRETSTRUCT*>(l_param);
        if (info->message == WM_WINDOWPOSCHANGED) {
            SpectrogramWindow::raisePinnedAfterSibling(info->hwnd);
        }
    }
    return CallNextHookEx(g_pin_zorder_ret_hook, code, w_param, l_param);
}

void installPinZOrderRetHook() {
    if (g_pin_zorder_ret_hook != nullptr) {
        return;
    }
    g_pin_zorder_ret_hook = SetWindowsHookExW(
        WH_CALLWNDPROCRET,
        &pinZOrderCallWndRetProc,
        nullptr,
        GetCurrentThreadId());
}

void removePinZOrderRetHook() {
    if (g_pin_zorder_ret_hook == nullptr) {
        return;
    }
    UnhookWindowsHookEx(g_pin_zorder_ret_hook);
    g_pin_zorder_ret_hook = nullptr;
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
          display_(common_guide_),
          spectrum_display_(spectrum_model_, common_guide_, {kPsgColour, kSccColour, kOpllColour}),
          incoming_frame_(std::make_unique<spectrum::Frame>()),
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
        addChildComponent(spectrum_display_);
        mode_gram_.setButtonText(juce::String::fromUTF8("スペクトログラム"));
        mode_spectrum_.setButtonText(juce::String::fromUTF8("スペアナ"));
        for (auto* b : {&mode_gram_, &mode_spectrum_}) {
            b->setClickingTogglesState(true); b->setRadioGroupId(264);
            addAndMakeVisible(*b);
        }
        mode_gram_.setToggleState(true, juce::dontSendNotification);
        mode_gram_.onClick = [this] { setSpectrumMode(false); };
        mode_spectrum_.onClick = [this] { setSpectrumMode(true); };
        configureZoomSlider(history_seconds_, 0, 5, 1);
        history_seconds_.setRange(0, 5, 0.1);
        history_seconds_.setDoubleClickReturnValue(true, 1.0);
        history_seconds_.setTextValueSuffix(" s");
        history_seconds_.onValueChange = [this] {
            spectrum_display_.history_seconds = history_seconds_.getValue(); spectrum_display_.refresh();
        };
        history_label_.setText(juce::String::fromUTF8("履歴時間"), juce::dontSendNotification);
        history_label_.setJustificationType(juce::Justification::centredRight);
        position_label_.setText(juce::String::fromUTF8("履歴位置"), juce::dontSendNotification);
        position_label_.setJustificationType(juce::Justification::centredRight);
        configureZoomSlider(history_position_, 0, 5, 0);
        history_position_.setRange(0, 5, 0.001);
        history_position_.setTextValueSuffix(" s");
        history_position_.onValueChange = [this] {
            spectrum_model_.selected_seconds = history_position_.getValue();
            spectrum_display_.refresh(); updateSpectrumControls();
        };
        channels_button_.setButtonText(juce::String::fromUTF8("チャンネル"));
        channels_button_.onClick = [this] { showChannelMenu(); };
        hold_button_.setButtonText(juce::String::fromUTF8("比較用に保持"));
        hold_button_.onClick = [this] {
            spectrum_model_.hold(); spectrum_display_.refresh(); updateSpectrumControls();
        };
        reference_button_.setButtonText(juce::String::fromUTF8("比較表示"));
        reference_button_.setClickingTogglesState(true);
        reference_button_.onClick = [this] {
            spectrum_model_.reference_visible = reference_button_.getToggleState();
            spectrum_display_.refresh();
        };
        clear_button_.setButtonText(juce::String::fromUTF8("比較クリア"));
        clear_button_.onClick = [this] {
            spectrum_model_.reference.reset(); spectrum_display_.refresh(); updateSpectrumControls();
        };
        pause_button_.onClick = [this] {
            spectrum_model_.setPaused(!spectrum_model_.paused);
            updateSpectrumControls(); resized(); spectrum_display_.refresh();
        };
        for (auto* c : std::array<juce::Component*, 9>{&history_label_, &history_seconds_,
                &position_label_, &history_position_, &channels_button_, &hold_button_,
                &reference_button_, &clear_button_, &pause_button_}) addChildComponent(*c);
        setSpectrumMode(false);
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
        for (auto* slider : {&history_seconds_, &history_position_})
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, UiScale::sx(64), UiScale::sx(22));
        sendLookAndFeelChange();
        resized();
        repaint();
    }

    void setActive(bool active) {
        if (active && !active_) {
            spectrum_model_.clearAcquisition();
            if (spectrum_mode_) spectrum_display_.refresh();
            display_.clearHistory();
            updateSpectrumControls();
            resized();
        }
        active_ = active;
        analyzer_.setCaptureEnabled(active);
        if (active) {
            startTimerHz(30);
        } else {
            stopTimer();
        }
    }

    void setAnalysisMode(SpectrogramAnalysisMode mode) {
        const auto mask = sourceMaskForMode(mode);
        if ((analyzer_.context() & 7) != mask) {
            spectrum_model_.clearAcquisition();
            if (spectrum_mode_) spectrum_display_.refresh();
            display_.clearHistory();
            updateSpectrumControls();
            resized();
        }
        analyzer_.setSourceMask(mask);
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
        const auto seconds_text = readIniString(L"SpectrumHistorySeconds", L"1.0").trim().toStdString();
        double seconds = 1.0;
        const auto parsed = std::from_chars(seconds_text.data(), seconds_text.data() + seconds_text.size(), seconds);
        if (parsed.ec != std::errc{} || parsed.ptr != seconds_text.data() + seconds_text.size() || !std::isfinite(seconds))
            seconds = 1.0;
        history_seconds_.setValue(juce::jlimit(0.0, 5.0, seconds),
                                   juce::dontSendNotification);
        spectrum_display_.history_seconds = history_seconds_.getValue();
        setSpectrumMode(readIniString(L"DisplayMode", L"0") == "1");
        updateScrollBar();
    }

    void setInternalPin(bool pinned) {
        pin_.setToggleState(pinned, juce::dontSendNotification);
    }

    [[nodiscard]] bool internalPin() const noexcept {
        return pin_.getToggleState();
    }

    void saveControls() const {
        writeIniString(L"DisplayMode", spectrum_mode_ ? "1" : "0");
        writeIniString(L"SpectrumHistorySeconds", juce::String(history_seconds_.getValue(), 1));
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
        auto modes = area.removeFromTop(UiLayout::fieldH);
        pin_.setBounds(modes.removeFromRight(UiLayout::fieldH));
        mode_gram_.setBounds(modes.removeFromLeft(UiLayout::spectrumModeW));
        modes.removeFromLeft(UiLayout::controlGap);
        mode_spectrum_.setBounds(modes.removeFromLeft(UiLayout::spectrumActionW));
        area.removeFromTop(UiLayout::controlGap);
        auto row = area.removeFromTop(UiLayout::fieldH);
        auto place = [&](juce::Component& c, int width) {
            if (row.getWidth() < width) {
                area.removeFromTop(UiLayout::controlGap);
                row = area.removeFromTop(UiLayout::fieldH);
            }
            c.setBounds(row.removeFromLeft(width));
            row.removeFromLeft(UiLayout::controlGap);
        };
        place(psg_, UiLayout::spectrogramLabelW);
        place(scc_, UiLayout::spectrogramLabelW);
        place(opll_, UiLayout::spectrogramLabelW);
        if (spectrum_mode_) {
            place(channels_button_, UiLayout::spectrumActionW);
            place(history_label_, UiLayout::setupRateEditW);
            place(history_seconds_, UiLayout::spectrumHistoryW);
            area.removeFromTop(UiLayout::controlGap);
            row = area.removeFromTop(UiLayout::fieldH);
            place(hold_button_, UiLayout::spectrumActionW);
            place(reference_button_, UiLayout::spectrumActionW);
            place(clear_button_, UiLayout::spectrumActionW);
            place(pause_button_, UiLayout::spectrumActionW);
            if (spectrum_model_.paused) {
                area.removeFromTop(UiLayout::controlGap);
                row = area.removeFromTop(UiLayout::fieldH);
                position_label_.setBounds(row.removeFromLeft(UiLayout::setupRateEditW));
                row.removeFromLeft(UiLayout::controlGap);
                history_position_.setBounds(row);
            }
        } else {
            place(time_label_, UiLayout::spectrogramLabelW);
            place(time_zoom_, UiLayout::spectrogramSliderW);
            place(frequency_label_, UiLayout::spectrogramLabelW);
            place(frequency_zoom_, UiLayout::spectrogramSliderW);
            place(emphasis_label_, UiLayout::spectrogramLabelW);
            place(emphasis_, UiLayout::spectrogramSliderW);
        }
        area.removeFromTop(UiLayout::controlGap);
        spectrum_display_.setBounds(area);
        if (!spectrum_mode_) {
            display_.setBounds(area);
            updateScrollBar();
            if (frequency_scroll_.isVisible()) {
                frequency_scroll_.setBounds(area.removeFromRight(UiLayout::spectrumScrollW));
                area.removeFromRight(UiLayout::xs);
                display_.setBounds(area);
                updateScrollBar();
            }
        } else frequency_scroll_.setVisible(false);
    }

public:
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey && common_guide_.fixed()) {
            common_guide_.release();
            if (spectrum_mode_) spectrum_display_.refresh();
            else display_.repaint();
            return true;
        }
        return false;
    }
    void setSpectrumMode(bool spectrum_mode) {
        spectrum_mode_ = spectrum_mode;
        analyzer_.setChannelsEnabled(spectrum_mode);
        mode_gram_.setToggleState(!spectrum_mode, juce::dontSendNotification);
        mode_spectrum_.setToggleState(spectrum_mode, juce::dontSendNotification);
        display_.setDrawingActive(!spectrum_mode);
        display_.setVisible(!spectrum_mode);
        spectrum_display_.setVisible(spectrum_mode);
        for (auto* c : std::array<juce::Component*, 6>{&time_label_, &time_zoom_,
                &frequency_label_, &frequency_zoom_, &emphasis_label_, &emphasis_}) c->setVisible(!spectrum_mode);
        for (auto* c : std::array<juce::Component*, 7>{&history_label_, &history_seconds_,
                &channels_button_, &hold_button_, &reference_button_, &clear_button_, &pause_button_}) c->setVisible(spectrum_mode);
        updateSpectrumControls();
        resized();
        if (spectrum_mode) { spectrum_display_.reevaluatePointer(); spectrum_display_.refresh(); }
        else display_.setDrawingActive(true);
    }
private:
    void updateSpectrumControls() {
        pause_button_.setButtonText(juce::String::fromUTF8(spectrum_model_.paused ? "再開" : "表示停止"));
        pause_button_.setEnabled(spectrum_model_.history().size() > 0);
        hold_button_.setEnabled(spectrum_model_.front() != nullptr);
        clear_button_.setEnabled(spectrum_model_.reference.has_value());
        reference_button_.setEnabled(spectrum_model_.reference.has_value());
        reference_button_.setToggleState(spectrum_model_.reference_visible, juce::dontSendNotification);
        position_label_.setVisible(spectrum_mode_ && spectrum_model_.paused);
        history_position_.setVisible(spectrum_mode_ && spectrum_model_.paused);
        if (spectrum_model_.paused) {
            const auto seconds = spectrum_model_.availableSeconds();
            history_position_.setRange(0, std::max(0.001, seconds), 0.001);
            history_position_.setEnabled(seconds > 0);
            history_position_.setValue(spectrum_model_.selected_seconds, juce::dontSendNotification);
        }
    }
    void showChannelMenu() {
        juce::PopupMenu menu;
        menu.addItem(1001, juce::String::fromUTF8("すべて表示"));
        menu.addItem(1002, juce::String::fromUTF8("すべて非表示"));
        menu.addSeparator();
        for (std::size_t ch = 0; ch < spectrum::channels; ++ch)
            if ((analyzer_.context() & (std::uint64_t{1} << engine::spectrumSource(ch))) != 0)
                menu.addItem(static_cast<int>(ch + 1), spectrum::channelName(ch), true, spectrum_display_.channel_visible[ch]);
        juce::Component::SafePointer<Content> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&channels_button_), [safe](int id) {
            if (!safe || id == 0) return;
            if (id == 1001 || id == 1002) safe->spectrum_display_.channel_visible.fill(id == 1001);
            else if (id > 0 && id <= static_cast<int>(spectrum::channels)) {
                auto& visible = safe->spectrum_display_.channel_visible[static_cast<std::size_t>(id - 1)];
                visible = !visible;
            }
            safe->spectrum_display_.refresh();
        });
    }

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
        bool changed = false;
        const bool was_paused = spectrum_model_.paused;
        for (std::size_t count = 0; count < SpectrumAnalyzerThread::queueCapacity
             && analyzer_.pollFrame(*incoming_frame_); ++count) {
            if (incoming_frame_->info.context != analyzer_.context()) continue;
            spectrum_model_.ingest(*incoming_frame_);
            display_.appendColumn(incoming_frame_->column);
            changed = true;
        }
        if (changed) {
            if (spectrum_mode_ && (!spectrum_model_.paused || !was_paused)) spectrum_display_.refresh();
            else if (!spectrum_mode_) display_.flushPendingRepaint();
            updateSpectrumControls();
            if (was_paused != spectrum_model_.paused) resized();
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
        spectrum_display_.source_visible = {psg_.getToggleState(), scc_.getToggleState(), opll_.getToggleState()};
        if (spectrum_mode_) spectrum_display_.refresh();
    }

    void updateScrollBar() {
        if (spectrum_mode_) { frequency_scroll_.setVisible(false); return; }
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

    spectrum::GuideState common_guide_;
    spectrum::Model spectrum_model_;
    SpectrumAnalyzerThread analyzer_;
    SpectrogramDisplay display_;
    spectrum::Display spectrum_display_;
    std::unique_ptr<spectrum::Frame> incoming_frame_;
    juce::TextButton mode_gram_, mode_spectrum_, channels_button_, hold_button_,
        reference_button_, clear_button_, pause_button_;
    juce::Label history_label_, position_label_;
    juce::Slider history_seconds_, history_position_;
    bool spectrum_mode_{}, active_{};
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
    g_spectrogram_window = this;
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
    if (g_spectrogram_window == this) {
        g_spectrogram_window = nullptr;
    }
    removePinZOrderRetHook();
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
    updatePinZOrderHook();
}

void SpectrogramWindow::setSpectrumMode(bool enabled) {
    if (content_ != nullptr) content_->setSpectrumMode(enabled);
}

void SpectrogramWindow::setAnalysisMode(SpectrogramAnalysisMode mode) {
    if (content_ != nullptr) {
        content_->setAnalysisMode(mode);
    }
}

juce::Component* SpectrogramWindow::snapshotContent() const noexcept {
    return content_;
}

void SpectrogramWindow::setInternalPin(bool pinned) {
    internal_pin_ = pinned;
    updatePinZOrderHook();
    if (internal_pin_ && isVisible()) {
        toFront(false);
    }
}

void SpectrogramWindow::updatePinZOrderHook() {
    if (internal_pin_ && isVisible()) {
        installPinZOrderRetHook();
        return;
    }
    removePinZOrderRetHook();
}

void SpectrogramWindow::constrainSiblingZOrder(
    void* window_pos,
    void* caller_native_handle) {
    auto* position = static_cast<WINDOWPOS*>(window_pos);
    const auto caller = static_cast<HWND>(caller_native_handle);
    if (position == nullptr
        || caller == nullptr
        || g_in_pin_zorder
        || (position->flags & SWP_NOZORDER) != 0
        || (position->flags & SWP_HIDEWINDOW) != 0
        || g_spectrogram_window == nullptr
        || !g_spectrogram_window->internal_pin_
        || !shouldKeepPinnedAboveSiblings()) {
        return;
    }
    const HWND spectrogram = spectrogramNativeHandle();
    if (spectrogram == nullptr
        || spectrogram == caller
        || !IsWindowVisible(spectrogram)) {
        return;
    }
    raiseSpectrogramWithoutActivating(spectrogram);
    position->hwndInsertAfter = spectrogram;
}

void SpectrogramWindow::raisePinnedAfterSibling(void* caller_native_handle) {
    const auto caller = static_cast<HWND>(caller_native_handle);
    if (caller == nullptr
        || g_in_pin_zorder
        || g_spectrogram_window == nullptr
        || !g_spectrogram_window->internal_pin_
        || !shouldKeepPinnedAboveSiblings()) {
        return;
    }
    const HWND spectrogram = spectrogramNativeHandle();
    if (spectrogram == nullptr || spectrogram == caller) {
        return;
    }
    raiseSpectrogramWithoutActivating(spectrogram);
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
    if (content_ != nullptr) {
        content_->setActive(false);
    }
    saveState();
    setVisible(false);
    updatePinZOrderHook();
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
