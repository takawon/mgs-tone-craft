// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "spectrum_analyzer.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

namespace mgstc::app::spectrum {
inline juce::String channelName(std::size_t channel) {
    if (channel < 3) return "PSG " + juce::String(static_cast<int>(channel + 1));
    if (channel < 8) return "SCC " + juce::String(static_cast<int>(channel - 2));
    if (channel < 17) return "OPLL " + juce::String(static_cast<int>(channel - 7));
    constexpr std::array<const char*, 5> drums{"OPLL 7 BD", "OPLL 8 HH", "OPLL 8 SD", "OPLL 9 TOM", "OPLL 9 CYM"};
    return drums[channel - 17];
}
inline juce::String noteName(double f) {
    if (!(f > 0) || !std::isfinite(f)) return juce::String::fromUTF8("—");
    constexpr std::array<const char*, 12> names{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const auto midi = juce::jlimit(0, 127, static_cast<int>(std::lround(69 + 12 * std::log2(f / 440.0))));
    return juce::String(names[static_cast<std::size_t>(midi % 12)]) + juce::String(midi / 12 - 1);
}
inline juce::String sourceNames(std::uint64_t context) {
    juce::StringArray names;
    if (context & 1) names.add("PSG");
    if (context & 2) names.add("SCC");
    if (context & 4) names.add("OPLL");
    return names.isEmpty() ? juce::String::fromUTF8("対象なし") : names.joinIntoString("+");
}
class Display final : public juce::Component {
public:
    Display(Model& model, GuideState& guide, std::array<juce::Colour, 3> colours)
        : model_(model), guide_(guide), colours_(colours) {
        channel_visible.fill(true);
        setOpaque(true); setWantsKeyboardFocus(true);
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }
    std::array<bool, channels> channel_visible{};
    std::array<bool, 3> source_visible{true, true, true};
    double history_seconds{1.0};
    std::function<void()> guide_changed;
    std::uint64_t pathBuilds() const noexcept { return path_builds_; }

    void refresh() {
        if (!isVisible()) return;
        history_paths_.fill(nullptr);
        front_path_ = nullptr;
        if (const auto* front = model_.front()) {
            auto& caches = model_.paused ? stopped_cache_ : live_cache_;
            front_path_ = &cached(caches[front->info.id % historyCapacity], *front);
            if (history_seconds > 0)
                for (std::size_t i = 0; i < historyLines; ++i) {
                    const double at = static_cast<double>(front->info.sample)
                        - history_seconds * sampleRate * (i + 1) / historyLines;
                    if (const auto* f = model_.history().nearest(at))
                        history_paths_[i] = &cached(caches[f->info.id % historyCapacity], *f);
                }
        }
        if (const auto* f = model_.channelFrame())
            for (std::size_t ch = 0; ch < channels; ++ch)
                if (channel_cache_[ch].id != f->info.id || channel_cache_[ch].context != f->info.context) {
                    buildPath(channel_cache_[ch].path, f->channel[ch]);
                    channel_cache_[ch].id = f->info.id;
                    channel_cache_[ch].context = f->info.context;
                }
        if (model_.reference) {
            cached(reference_cache_, *model_.reference);
            if (reference_stroke_id_ != model_.reference->info.id
                || reference_stroke_context_ != model_.reference->info.context
                || reference_stroke_bounds_ != getLocalBounds()
                || reference_stroke_scale_ != UiScale::active_factor) {
                const float dash[]{static_cast<float>(UiScale::sx(5)), static_cast<float>(UiScale::sx(4))};
                juce::PathStrokeType(1.2F).createDashedStroke(reference_stroke_, reference_cache_.path,
                                                            dash, 2, transformFor(0));
                reference_stroke_id_ = model_.reference->info.id;
                reference_stroke_context_ = model_.reference->info.context;
                reference_stroke_bounds_ = getLocalBounds();
                reference_stroke_scale_ = UiScale::active_factor;
            }
        }
        updateSelection();
        repaint();
    }
    void reevaluatePointer() {
        cursor_ = getMouseXYRelative().toFloat();
        cursor_inside_ = frontBounds().contains(cursor_);
        updateSelection();
    }
    void lookAndFeelChanged() override {
        dense_font_ = UiFonts::dense(); body_font_ = UiFonts::body();
        font_scale_ = UiScale::active_factor;
    }
    void resized() override {
        if (font_scale_ != UiScale::active_factor) lookAndFeelChanged();
        reevaluatePointer(); if (isVisible()) refresh();
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey && guide_.fixed()) {
            guide_.release(); changedGuide(); return true;
        }
        return false;
    }
    void mouseMove(const juce::MouseEvent& e) override {
        cursor_ = e.position; cursor_inside_ = frontBounds().contains(cursor_);
        updateSelection(); repaint();
    }
    void mouseEnter(const juce::MouseEvent& e) override { mouseMove(e); }
    void mouseDrag(const juce::MouseEvent& e) override { mouseMove(e); }
    void mouseExit(const juce::MouseEvent&) override { cursor_inside_ = false; selected_.reset(); repaint(); }
    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isLeftButtonDown() && frontBounds().contains(e.position)) {
            cursor_ = e.position; cursor_inside_ = true;
            guide_.click(frequencyAt((cursor_.x - frontBounds().getX()) / frontBounds().getWidth()));
            grabKeyboardFocus(); changedGuide();
        }
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colours::black);
        const auto front = frontBounds();
        if (front.getWidth() <= 0 || front.getHeight() <= 0) return;
        drawAxes(g, front);
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(graphBounds().toNearestInt());
            for (std::size_t n = historyLines; n > 0; --n) {
                const auto* path = history_paths_[n - 1];
                if (!path) continue;
                const auto depth = static_cast<float>(n) / historyLines;
                g.setColour(juce::Colours::lightgrey.withAlpha(0.27F * (1.0F - depth * 0.78F)));
                g.strokePath(*path, juce::PathStrokeType(1.0F), transformFor(depth));
            }
            if (model_.reference && model_.reference_visible) {
                g.setColour(juce::Colours::lightgrey.withAlpha(0.65F)); g.fillPath(reference_stroke_);
            }
            // A single channel often coincides exactly with the mix. Keep a
            // white outline beneath the coloured centre so both remain visible.
            if (front_path_) {
                g.setColour(juce::Colours::white);
                g.strokePath(*front_path_, juce::PathStrokeType(2.6F), transformFor(0));
            }
            if (model_.channelFrame())
                for (std::size_t ch = 0; ch < channels; ++ch)
                    if (visible(ch)) {
                        g.setColour(colours_[engine::spectrumSource(ch)].withAlpha(0.85F));
                        g.strokePath(channel_cache_[ch].path, juce::PathStrokeType(1.3F), transformFor(0));
                    }
            drawGuides(g, front);
            if (selected_) {
                const auto point = pointFor(selected_->peak, front);
                g.setColour(juce::Colours::white);
                const auto r = static_cast<float>(UiScale::sx(4));
                g.drawEllipse(point.x - r, point.y - r, r * 2, r * 2, 1.2F);
            }
        }
        g.setFont(dense_font_);
        g.setColour(juce::Colours::lightgrey);
        if (history_seconds > 0)
            g.drawText(juce::String::fromUTF8("−") + juce::String(history_seconds, 1) + " s",
                       graphBounds().toNearestInt().removeFromTop(UiLayout::fieldH),
                       juce::Justification::topRight);
        drawInformation(g);
    }
private:
    struct PathCache { juce::Path path; std::uint64_t id{}, context{}; };
    struct Selection { Peak peak; int channel{-1}; };
    bool visible(std::size_t ch) const noexcept {
        return channel_visible[ch] && source_visible[engine::spectrumSource(ch)];
    }
    juce::Rectangle<float> graphBounds() const {
        auto b = getLocalBounds().toFloat().reduced(static_cast<float>(UiLayout::panelPad));
        b.removeFromBottom(static_cast<float>(UiLayout::fieldH * 4));
        b.removeFromLeft(static_cast<float>(UiLayout::spectrogramLabelW));
        b.removeFromBottom(static_cast<float>(UiLayout::fieldH));
        return b;
    }
    juce::Rectangle<float> frontBounds() const {
        auto b = graphBounds();
        b.removeFromTop(b.getHeight() * 0.16F);
        b.removeFromRight(b.getWidth() * 0.12F);
        return b;
    }
    juce::AffineTransform transformFor(float depth) const {
        const auto b = frontBounds();
        const auto all = graphBounds();
        return juce::AffineTransform::scale(b.getWidth(), b.getHeight())
            .translated(b.getX() + all.getWidth() * 0.12F * depth,
                        b.getY() - all.getHeight() * 0.16F * depth);
    }
    static juce::Point<float> pointFor(Peak peak, juce::Rectangle<float> b) {
        return {b.getX() + static_cast<float>(frequencyU(peak.frequency)) * b.getWidth(),
                b.getY() + (0.0F - std::clamp(peak.db, minDb, 0.0F)) / -minDb * b.getHeight()};
    }
    void buildPath(juce::Path& path, const Trace& trace) {
        path.clear(); ++path_builds_;
        if (!trace.audible) return;
        bool started = false, pending_valid = false;
        juce::Point<float> committed{}, pending{};
        auto point = [&](float x, float y) {
            const juce::Point<float> next{x, y};
            if (!started) { path.startNewSubPath(next); committed = next; started = true; return; }
            // Preserve every bend and peak; collapse only exactly horizontal runs
            // (especially the hundreds of bins clipped to the display floor).
            if (pending_valid && !(committed.y == pending.y && pending.y == next.y)) {
                path.lineTo(pending); committed = pending;
            }
            pending = next; pending_valid = true;
        };
        std::size_t peak = 0;
        double previous_frequency = 0;
        float previous_db = minDb;
        auto add = [&](double f, float db) {
            if (f < minFrequency) { previous_frequency = f; previous_db = db; return; }
            if (f > maxFrequency) return;
            if (!started && previous_frequency > 0 && f > minFrequency) {
                const auto weight = std::log(minFrequency / previous_frequency) / std::log(f / previous_frequency);
                const auto boundary_db = previous_db + static_cast<float>(weight) * (db - previous_db);
                point(0, -std::clamp(boundary_db, minDb, 0.0F) / -minDb);
            }
            const float u = static_cast<float>(frequencyU(f));
            const float v = -std::clamp(db, minDb, 0.0F) / -minDb;
            point(u, v);
        };
        for (std::size_t i = 1; i < bins; ++i) {
            const double f = i * sampleRate / fftSize;
            while (peak < trace.peak_count && trace.peaks[peak].frequency < f) {
                add(trace.peaks[peak].frequency, trace.peaks[peak].db); ++peak;
            }
            add(f, decibels(trace.power[i]));
        }
        if (pending_valid) path.lineTo(pending);
        simplifyPath(path, trace);
    }
    static void simplifyPath(juce::Path& path, const Trace& trace) {
        constexpr std::size_t capacity = bins + fftSize / 4 + 2;
        std::array<juce::Point<float>, capacity> points{};
        std::array<bool, capacity> keep{};
        std::array<std::pair<std::size_t, std::size_t>, capacity> stack{};
        std::size_t count = 0, peak = 0;
        juce::Path::Iterator it(path);
        while (it.next() && count < capacity) {
            points[count] = {it.x1, it.y1};
            while (peak < trace.peak_count
                && frequencyU(trace.peaks[peak].frequency) < it.x1 - 1.0e-6) ++peak;
            keep[count] = peak < trace.peak_count
                && std::abs(frequencyU(trace.peaks[peak].frequency) - it.x1) <= 1.0e-6;
            ++count;
        }
        if (count < 3) return;
        keep[0] = keep[count - 1] = true;
        // Bound display error to 0.1 px at the maximum 2000 px plot height.
        // Measured peaks are mandatory anchors; original float data is untouched.
        constexpr float tolerance = 0.00005F;
        for (std::size_t first = 0; first + 1 < count;) {
            auto last = first + 1;
            while (!keep[last]) ++last;
            std::size_t pending = 0;
            stack[pending++] = {first, last};
            while (pending) {
                const auto [a, b] = stack[--pending];
                const auto dx = points[b].x - points[a].x;
                if (b <= a + 1 || dx <= 0) continue;
                float maximum = tolerance;
                std::size_t split = a;
                for (auto i = a + 1; i < b; ++i) {
                    const auto expected = points[a].y + (points[b].y - points[a].y)
                        * (points[i].x - points[a].x) / dx;
                    const auto error = std::abs(points[i].y - expected);
                    if (error > maximum) { maximum = error; split = i; }
                }
                if (split != a) {
                    keep[split] = true;
                    stack[pending++] = {a, split}; stack[pending++] = {split, b};
                }
            }
            first = last;
        }
        path.clear(); path.startNewSubPath(points[0]);
        for (std::size_t i = 1; i < count; ++i) if (keep[i]) path.lineTo(points[i]);
    }

    const juce::Path& cached(PathCache& c, const HistoryFrame& f) {
        if (c.id != f.info.id || c.context != f.info.context) {
            buildPath(c.path, f.mixed); c.id = f.info.id; c.context = f.info.context;
        }
        return c.path;
    }
    double guideFrequency() const {
        if (guide_.fixed()) return guide_.fixed_frequency;
        const auto b = frontBounds();
        if (cursor_inside_ && b.getWidth() > 0) return frequencyAt((cursor_.x - b.getX()) / b.getWidth());
        const auto* f = model_.front();
        return f ? noteFrequency(f->info.guide.note, engine::spectrumSource(f->info.guide.track)) : 0;
    }
    void changedGuide() { updateSelection(); repaint(); if (guide_changed) guide_changed(); }
    void updateSelection() {
        selected_.reset();
        const auto* front = model_.front();
        const auto bounds = frontBounds();
        if (!cursor_inside_ || !front || bounds.getWidth() <= 0) return;
        const float radius = static_cast<float>(UiScale::sx(10));
        float best = radius * radius;
        const auto fmin = frequencyAt((cursor_.x - radius - bounds.getX()) / bounds.getWidth());
        const auto fmax = frequencyAt((cursor_.x + radius - bounds.getX()) / bounds.getWidth());
        auto consider = [&](const Trace& trace, int channel) {
            auto end = trace.peaks.begin() + trace.peak_count;
            auto p = std::lower_bound(trace.peaks.begin(), end, fmin,
                [](const Peak& peak, double f) { return peak.frequency < f; });
            for (; p != end && p->frequency <= fmax; ++p) {
                const auto point = pointFor(*p, bounds);
                const auto distance = point.getDistanceSquaredFrom(cursor_);
                if (distance <= best) { best = distance; selected_ = Selection{*p, channel}; }
            }
        };
        if (const auto* f = model_.channelFrame())
            for (std::size_t ch = 0; ch < channels; ++ch)
                if (visible(ch)) consider(f->channel[ch], static_cast<int>(ch));
        consider(front->mixed, -1);
    }
    void drawAxes(juce::Graphics& g, juce::Rectangle<float> b) {
        g.setFont(dense_font_);
        float last_x = -10000;
        constexpr double ticks[]{30, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
        for (double f : ticks) {
            const float x = b.getX() + static_cast<float>(frequencyU(f)) * b.getWidth();
            g.setColour(juce::Colours::white.withAlpha(0.09F));
            g.drawVerticalLine(static_cast<int>(x), b.getY(), b.getBottom());
            if (x - last_x < UiScale::sx(40)) continue;
            last_x = x;
            g.setColour(juce::Colours::lightgrey);
            g.drawText(f >= 1000 ? juce::String(f / 1000, 0) + "k" : juce::String(f, 0),
                juce::Rectangle<float>(x - UiLayout::spectrogramLabelW / 2.0F, b.getBottom(),
                    static_cast<float>(UiLayout::spectrogramLabelW), static_cast<float>(UiLayout::fieldH)),
                juce::Justification::centred);
        }
        for (int db = 0; db >= -100; db -= 20) {
            const float y = b.getY() + (-db / 100.0F) * b.getHeight();
            g.setColour(juce::Colours::white.withAlpha(0.09F));
            g.drawHorizontalLine(static_cast<int>(y), b.getX(), b.getRight());
            g.setColour(juce::Colours::lightgrey);
            g.drawText(juce::String(db), juce::Rectangle<float>(
                b.getX() - UiLayout::spectrogramLabelW, y - UiLayout::fieldH / 2.0F,
                static_cast<float>(UiLayout::spectrogramLabelW - UiLayout::controlGap),
                static_cast<float>(UiLayout::fieldH)), juce::Justification::centredRight);
        }
        g.drawText("dBFS", getLocalBounds().removeFromTop(UiLayout::fieldH).removeFromLeft(UiLayout::spectrogramLabelW),
                   juce::Justification::centred);
    }
    void drawGuides(juce::Graphics& g, juce::Rectangle<float> b) {
        const double base = guideFrequency();
        if (!(base > 0)) return;
        const auto* f = model_.front();
        const float alpha = guide_.fixed() || cursor_inside_ ? 0.52F : f && f->info.guide.active ? 0.36F : 0.18F;
        const float dash[]{static_cast<float>(UiScale::sx(4)), static_cast<float>(UiScale::sx(5))};
        g.setFont(dense_font_);
        float last_label = -10000;
        for (int n = 1; n <= 16; ++n) {
            const auto frequency = base * n;
            if (frequency < minFrequency || frequency > maxFrequency) continue;
            const float x = b.getX() + static_cast<float>(frequencyU(frequency)) * b.getWidth();
            g.setColour(juce::Colours::white.withAlpha(n == 1 ? alpha : alpha * 0.65F));
            g.drawDashedLine({x, b.getY(), x, b.getBottom()}, dash, 2, n == 1 ? 1.3F : 1.0F);
            if (x - last_label >= UiScale::sx(26)) {
                g.drawText(juce::String::fromUTF8("×") + juce::String(n),
                    juce::Rectangle<float>(x + UiLayout::xs, b.getY(), static_cast<float>(UiLayout::spectrogramLabelW),
                        static_cast<float>(UiLayout::fieldH)), juce::Justification::topLeft);
                last_label = x;
            }
        }
    }
    void drawInformation(juce::Graphics& g) {
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        area = area.removeFromBottom(UiLayout::fieldH * 4);
        g.setFont(body_font_); g.setColour(juce::Colours::lightgrey);
        auto legend = area.removeFromTop(UiLayout::fieldH);
        g.setFont(dense_font_);
        g.drawText(juce::String::fromUTF8("個別："), legend.removeFromLeft(UiLayout::spectrogramLabelW),
                   juce::Justification::centredLeft);
        if (const auto* f = model_.channelFrame()) {
            for (std::size_t ch = 0; ch < channels; ++ch) {
                if (!f->channel[ch].audible) continue;
                const auto label = channelName(ch);
                const int width = UiScale::sx(ch >= 17 ? 96 : 72);
                if (legend.getWidth() < width) {
                    g.setColour(juce::Colours::lightgrey);
                    g.drawText("...", legend, juce::Justification::centredLeft); break;
                }
                g.setColour(colours_[engine::spectrumSource(ch)].withAlpha(visible(ch) ? 1.0F : 0.3F));
                g.drawText(label, legend.removeFromLeft(width), juce::Justification::centredLeft);
            }
        }
        g.setFont(body_font_); g.setColour(juce::Colours::lightgrey);
        juce::String peak_text = juce::String::fromUTF8("ピーク：—");
        if (selected_) {
            peak_text = selected_->channel < 0 ? juce::String::fromUTF8("合成") : channelName(static_cast<std::size_t>(selected_->channel));
            peak_text += " | " + juce::String(selected_->peak.frequency, 0) + " Hz | " + noteName(selected_->peak.frequency);
            peak_text += " | " + juce::String(selected_->peak.db, 1) + " dBFS";
            if (const auto base = guideFrequency(); base > 0)
                peak_text += juce::String::fromUTF8(" | 基準比 ×") + juce::String(selected_->peak.frequency / base, 3);
        }
        g.drawFittedText(peak_text, area.removeFromTop(UiLayout::fieldH), juce::Justification::centredLeft, 1);
        juce::String state = guide_.fixed() ? juce::String::fromUTF8("基準固定：") : juce::String::fromUTF8("基準：");
        const auto base = guideFrequency();
        state += base > 0 ? juce::String(base, 1) + " Hz" : juce::String::fromUTF8("—");
        if (model_.paused)
            state += juce::String::fromUTF8(" | 表示停止  選択時点：−") + juce::String(model_.selectedAge(), 2) + " s";
        if (!model_.front()) state += juce::String::fromUTF8(" | データなし／取得欠落");
        g.drawFittedText(state, area.removeFromTop(UiLayout::fieldH), juce::Justification::centredLeft, 1);
        juce::String conditions = juce::String::fromUTF8("48 kHz 内部ミックス・音量反映");
        if (const auto* front = model_.front(); front && front->info.simulated_remote)
            conditions = juce::String::fromUTF8("内蔵シミュレーション（実機音声の測定ではありません）");
        if (model_.reference) {
            const auto& ref = *model_.reference;
            conditions = juce::String::fromUTF8("比較：") + sourceNames(ref.info.context) + " | "
                + noteName(noteFrequency(ref.info.guide.note, engine::spectrumSource(ref.info.guide.track)))
                + juce::String::fromUTF8(" | 取得 ") + juce::String(ref.info.sample / sampleRate, 2) + " s";
            if (model_.reference_was_paused)
                conditions += juce::String::fromUTF8("（停止履歴 −") + juce::String(model_.reference_selection_seconds, 2) + " s)";
            if (const auto* f = model_.front(); f && ((f->info.context & 7) != (ref.info.context & 7)
                || f->info.simulated_remote != ref.info.simulated_remote))
                conditions += juce::String::fromUTF8(" | 解析対象が異なります");
        }
        g.drawFittedText(conditions, area, juce::Justification::centredLeft, 1);
    }
    Model& model_;
    GuideState& guide_;
    std::array<juce::Colour, 3> colours_;
    std::array<PathCache, historyCapacity> live_cache_, stopped_cache_;
    std::array<PathCache, channels> channel_cache_;
    PathCache reference_cache_;
    juce::Path reference_stroke_;
    juce::Rectangle<int> reference_stroke_bounds_;
    std::uint64_t reference_stroke_id_{}, reference_stroke_context_{};
    float reference_stroke_scale_{};
    const juce::Path* front_path_{};
    std::array<const juce::Path*, historyLines> history_paths_{};
    juce::Point<float> cursor_{};
    bool cursor_inside_{};
    std::optional<Selection> selected_;
    std::uint64_t path_builds_{};
    juce::Font dense_font_{UiFonts::dense()}, body_font_{UiFonts::body()};
    float font_scale_{UiScale::active_factor};
};
} // namespace mgstc::app::spectrum
