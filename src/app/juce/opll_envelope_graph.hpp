// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "opll_envelope_edit.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

class OpllEnvelopeGraph final : public juce::Component,
                                public juce::SettableTooltipClient {
public:
    OpllEnvelopeGraph(juce::Colour colour, bool modulator)
        : waveform_colour_(colour), modulator_(modulator) {
        setWantsKeyboardFocus(true);
    }
    void setTrace(const std::array<float, mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
                  bool valid) {
        levels_ = levels;
        valid_ = valid;
        repaint();
    }
    std::function<void(const OpllEnvelopeValues&, bool)> onEdit;
    // History/final status only. Do not audition the already-previewed value again.
    std::function<void()> onCommit;
    std::function<OpllEnvelopeValues()> getValues;
    std::function<bool()> getSustainedTone;

    void cancelDrag() {
        if (!drag_.active()) return;
        const bool changed = drag_.cancel();
        if (changed && onEdit) onEdit(drag_.values(), false);
        hover_.reset();
        expanded_ = false;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto frame = getLocalBounds().toFloat().reduced(0.5F);
        const auto plot = plotBounds();
        const auto v = values();
        const auto guide = currentGuide();
        const bool off = v.attack_rate == 0;
        const auto colour = off ? muted() : guideColour();
        g.setColour(background());
        g.fillRoundedRectangle(frame, float(UiLayout::controlGap));
        g.setColour(border());
        g.drawRoundedRectangle(frame, float(UiLayout::controlGap), 1.0F);
        g.setColour(border().withAlpha(0.5F));
        g.drawHorizontalLine(juce::roundToInt(plot.getCentreY()), plot.getX(), plot.getRight());
        dashed(g, {{guide.key_off.x, plot.getY()}, {guide.key_off.x, plot.getBottom()}}, keyColour());

        if (valid_) {
            juce::Path trace;
            for (std::size_t i = 0; i < levels_.size(); ++i) {
                const float x = plot.getX() + plot.getWidth() * float(i) / float(levels_.size() - 1);
                const float y = plot.getBottom() - plot.getHeight() * std::clamp(levels_[i], 0.0F, 1.0F);
                if (i == 0) trace.startNewSubPath(x, y);
                else trace.lineTo(x, y);
            }
            auto fill = trace;
            fill.lineTo(plot.getRight(), plot.getBottom());
            fill.lineTo(plot.getX(), plot.getBottom());
            fill.closeSubPath();
            g.setColour(waveform_colour_.withAlpha(0.24F));
            g.fillPath(fill);
            g.setColour(waveform_colour_);
            g.strokePath(trace, juce::PathStrokeType(1.7F));
        }

        // The guide is a parameter diagram. It does not overwrite the real EG,
        // including AM, the DR=0/SL=0 transition, or MOD's frozen post-key-off EG.
        juce::Path line;
        line.startNewSubPath(plot.getX(), plot.getBottom());
        line.lineTo(point(guide.handles[0]));
        line.lineTo(point(guide.handles[1]));
        g.setColour(colour.withAlpha(0.85F));
        g.strokePath(line, juce::PathStrokeType(1.5F));
        dashed(g, {point(guide.handles[1]), point(guide.key_off)}, colour.withAlpha(0.65F));
        dashed(g, {point(guide.key_off), point(guide.end)}, colour.withAlpha(0.65F));
        if (guide.end.x < plot.getRight())
            dashed(g, {point(guide.end), {plot.getRight(), guide.end.y}}, colour.withAlpha(0.65F));

        const bool editing_sl = (drag_.active() && drag_.handle() == OpllEnvelopeHandle::DecaySustain)
            || hover_ == OpllEnvelopeHandle::DecaySustain;
        const float mark = float(UiLayout::xl);
        dashed(g, {{guide.sl.x - mark * 0.5F, guide.sl.y},
                   {guide.sl.x + mark * 0.5F, guide.sl.y}}, colour.withAlpha(editing_sl ? 0.9F : 0.4F));
        g.setColour(colour.withAlpha(editing_sl ? 0.9F : 0.4F));
        g.drawRect(juce::Rectangle<float>(float(UiLayout::xs), float(UiLayout::xs))
                       .withCentre(point(guide.sl)), 1.0F);
        g.setFont(UiFonts::dense());
        if (editing_sl)
            g.drawText("SL " + juce::String(v.sustain_level),
                       juce::Rectangle<float>(guide.sl.x + mark * 0.6F,
                                              guide.sl.y - UiLayout::md,
                                              mark * 3, float(UiLayout::xl)),
                       juce::Justification::centredLeft);

        auto positions = visibleHandles(guide);
        if (drag_.active()) {
            // Other points follow parameters; the grabbed proxy stays under the
            // pointer (fine drag moves more slowly), with a visible return target.
            if (drag_.handle() != OpllEnvelopeHandle::DecaySustain)
                positions[std::size_t(drag_.handle())] = guide.handles[std::size_t(drag_.handle())];
            else if (drag_.moved())
                positions[1] = drag_.visual();
            else positions[1] = drag_handles_[1];
        }
        for (std::size_t i = 0; i < positions.size(); ++i) {
            auto p = point(positions[i]);
            // Keep the proxy visible even while capturing a drag outside the component.
            const auto safe = frame.reduced(handleRadius() + 1.0F);
            p.x = juce::jlimit(safe.getX(), safe.getRight(), p.x);
            p.y = juce::jlimit(safe.getY(), safe.getBottom(), p.y);
            const auto origin = point(guide.handles[i]);
            if (origin.getDistanceFrom(p) > 1.0F) {
                g.setColour(colour.withAlpha(0.55F));
                g.drawLine({origin, p}, 1.0F);
                g.drawEllipse(juce::Rectangle<float>(handleRadius() * 2, handleRadius() * 2)
                                  .withCentre(origin), 1.0F);
            }
            const bool selected = drag_.active()
                ? std::size_t(drag_.handle()) == i : hover_ && std::size_t(*hover_) == i;
            drawHandle(g, p, selected);
            if (expanded_ && i < 2) {
                g.setColour(textColour());
                g.drawText(i == 0 ? "AR" : "DR/SL",
                    juce::Rectangle<float>(p.x - mark, p.y + handleRadius(), mark * 2, mark),
                    juce::Justification::centred);
            }
        }
        g.setFont(UiFonts::dense());
        g.setColour(textColour());
        const auto rr = point(guide.handles[2]);
        g.drawText("RR " + juce::String(v.release_rate),
            juce::Rectangle<float>(rr.x - UiLayout::xl * 3, rr.y - UiLayout::md,
                                   float(UiLayout::xl * 2), float(UiLayout::xl)),
            juce::Justification::centredRight);

        const auto text_h = std::max(UiLayout::lg, int(std::ceil(UiFonts::dense().getHeight())) + UiLayout::xs);
        auto header = getLocalBounds().reduced(UiLayout::panelPad).removeFromTop(text_h);
        if (off) {
            g.setColour(muted());
            g.drawText("OFF", header.removeFromLeft(UiLayout::xl * 2), juce::Justification::centredLeft);
        }
        g.setColour(textColour());
        g.drawText(statusText(), header, juce::Justification::centredLeft, true);
        auto footer = getLocalBounds().reduced(UiLayout::panelPad).removeFromBottom(text_h);
        g.drawText("0s", footer, juce::Justification::centredLeft);
        g.drawText("4s", footer, juce::Justification::centredRight);
        g.setColour(keyColour());
        g.drawText("KO 2s", footer.withX(juce::roundToInt(guide.key_off.x) + UiLayout::xs)
                                   .withWidth(UiLayout::xl * 3), juce::Justification::centredLeft);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (!e.mods.isLeftButtonDown()) return;
        updateHover(e.position);
        if (!hover_) return;
        if (isShowing()) grabKeyboardFocus();
        drag_handles_ = visibleHandles(currentGuide());
        drag_.begin(*hover_, values(), modelPoint(e.position), drag_handles_[std::size_t(*hover_)],
                    modelBounds(), float(UiLayout::xs));
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override { moveDrag(e); }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!drag_.active()) return;
        moveDrag(e);
        const bool changed = drag_.finish();
        if (changed && onCommit) onCommit();
        updateHover(e.position);
        repaint();
    }
    bool keyPressed(const juce::KeyPress& key) override {
        if (key == juce::KeyPress::escapeKey && drag_.active()) {
            cancelDrag();
            return true;
        }
        return false;
    }
    void focusLost(FocusChangeType) override { cancelDrag(); }
    void resized() override { cancelDrag(); expanded_ = false; }
    void mouseMove(const juce::MouseEvent& e) override { updateHover(e.position); }
    void mouseExit(const juce::MouseEvent&) override {
        if (!drag_.active()) {
            hover_.reset();
            expanded_ = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
        }
    }
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}

private:
    using Point = OpllEnvelopeEditing::Point;
    using Guide = OpllEnvelopeEditing::Guide;
    static juce::Point<float> point(Point p) { return {p.x, p.y}; }
    static Point modelPoint(juce::Point<float> p) { return {p.x, p.y}; }
    static juce::Colour background() { return juce::Colour(0xFF111920); }
    static juce::Colour border() { return juce::Colour(0xFF435160); }
    static juce::Colour muted() { return juce::Colour(0xFF7B858D); }
    static juce::Colour guideColour() { return juce::Colour(0xFFFFF1A8); }
    static juce::Colour keyColour() { return juce::Colour(0xFFD66B6B); }
    static juce::Colour textColour() { return juce::Colour(0xFFB9C6D2); }
    static float handleRadius() { return float(UiLayout::rateEnvelopeHandleR); }
    static float hitRadius() { return float(UiLayout::lg); }
    static float separation() { return float(UiLayout::xl + UiLayout::md); }
    static void dashed(juce::Graphics& g, juce::Line<float> line, juce::Colour colour) {
        const float pattern[]{float(UiLayout::xs), float(UiLayout::xs)};
        g.setColour(colour);
        g.drawDashedLine(line, pattern, 2);
    }
    void drawHandle(juce::Graphics& g, juce::Point<float> p, bool selected) const {
        const auto circle = juce::Rectangle<float>(handleRadius() * 2, handleRadius() * 2).withCentre(p);
        g.setColour(background());
        g.fillEllipse(circle.expanded(float(UiLayout::xs) * 0.5F));
        g.setColour(juce::Colour(0xFFFF514D));
        g.fillEllipse(circle);
        g.setColour(selected ? juce::Colour(0xFF53E3A6) : juce::Colour(0xFFFFC1BB));
        g.drawEllipse(circle, selected ? 2.0F : 1.0F);
    }
    juce::Rectangle<float> plotBounds() const {
        auto b = getLocalBounds().toFloat().reduced(float(UiLayout::panelPad + UiLayout::lg));
        b.removeFromTop(float(UiLayout::lg));
        b.removeFromBottom(float(UiLayout::sm));
        return b.withWidth(std::max(1.0F, b.getWidth())).withHeight(std::max(1.0F, b.getHeight()));
    }
    OpllEnvelopeEditing::Bounds modelBounds() const {
        const auto b = plotBounds();
        return {b.getX(), b.getY(), b.getWidth(), b.getHeight()};
    }
    OpllEnvelopeValues values() const {
        return drag_.active() ? drag_.values() : getValues ? getValues() : OpllEnvelopeValues{};
    }
    bool sustainedTone() const { return getSustainedTone ? getSustainedTone() : true; }
    Guide currentGuide() const {
        return OpllEnvelopeEditing::layout(modelBounds(), values(), modulator_, sustainedTone());
    }
    std::array<Point, 3> visibleHandles(const Guide& guide) const {
        return OpllEnvelopeEditing::handlePositions(guide, expanded_, separation());
    }
    void updateHover(juce::Point<float> pointer) {
        if (drag_.active()) return;
        const auto guide = currentGuide();
        const auto p = modelPoint(pointer);
        const auto expanded_positions = OpllEnvelopeEditing::handlePositions(guide, true, separation());
        expanded_ = OpllEnvelopeEditing::distance(guide.handles[0], guide.handles[1]) < separation()
            && (OpllEnvelopeEditing::distance(p, guide.handles[0]) < hitRadius() * 2
                || OpllEnvelopeEditing::distance(p, guide.handles[1]) < hitRadius() * 2
                || (expanded_ && OpllEnvelopeEditing::distance(p, expanded_positions[1]) < hitRadius() * 2));
        hover_ = OpllEnvelopeEditing::hitTest(p, visibleHandles(guide), hitRadius());
        setMouseCursor(hover_ ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }
    void moveDrag(const juce::MouseEvent& e) {
        if (!drag_.active()) return;
        if (drag_.move(modelPoint(e.position), e.mods.isShiftDown()) && onEdit)
            onEdit(drag_.values(), false);
        repaint();
    }
    juce::String statusText() const {
        const auto v = values();
        const auto handle = drag_.active() ? std::optional{drag_.handle()} : hover_;
        if (handle == OpllEnvelopeHandle::Attack)
            return juce::String::fromUTF8("AR ") + juce::String(v.attack_rate) + juce::String::fromUTF8("  ←速い  Shift:微調整");
        if (handle == OpllEnvelopeHandle::DecaySustain)
            return "DR " + juce::String(v.decay_rate) + " / SL " + juce::String(v.sustain_level)
                + juce::String::fromUTF8("  ←DR増  ↓SL増  Shift:微調整");
        juce::String rr;
        if (v.attack_rate == 0) rr = juce::String::fromUTF8("RR: AR=0で作用なし");
        else if (modulator_ && sustainedTone()) rr = juce::String::fromUTF8("RR: EG1では作用なし");
        else if (sustainedTone()) rr = juce::String::fromUTF8("RR: キーオフ後");
        else if (v.decay_rate == 0 && v.sustain_level != 0) rr = juce::String::fromUTF8("RR: DR=0でSL未到達");
        else rr = juce::String::fromUTF8("RR: SL到達後〜キーオフ");
        return handle == OpllEnvelopeHandle::Release
            ? rr + juce::String::fromUTF8("  ↓増  Shift:微調整")
            : juce::String::fromUTF8("補助線:模式表示  ") + rr;
    }
    std::array<float, mgstc::engine::OpllEnvelopeTrace::kPointCount> levels_{};
    juce::Colour waveform_colour_;
    bool modulator_{}, valid_{}, expanded_{};
    std::optional<OpllEnvelopeHandle> hover_;
    std::array<Point, 3> drag_handles_{};
    OpllEnvelopeEditing::Drag drag_;
};
