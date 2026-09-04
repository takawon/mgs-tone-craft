// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "midi_note_name.hpp"
#include "switch_look_and_feel.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"

// Compact FM original-tone editor for manual `y` authoring on the composite
// timeline. Highlights fields that differ from the pre-step baseline
// (base + prior @ + prior y).

struct OpllEnvelopeValues {
    std::uint8_t attack_rate{};
    std::uint8_t decay_rate{};
    std::uint8_t sustain_level{};
    std::uint8_t release_rate{};
};

enum class OpllEnvelopeHandle {
    Attack,
    DecaySustain,
    SustainRelease,
    Release,
};

class OpllEnvelopeGraph final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    OpllEnvelopeGraph(
        juce::Colour waveform_colour,
        bool modulator)
        : waveform_colour_(waveform_colour),
          modulator_(modulator) {}

    void setTrace(
        const std::array<
            float,
            mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
        bool valid) {
        levels_ = levels;
        valid_ = valid;
        repaint();
    }

    std::function<void(const OpllEnvelopeValues&, bool)> onEdit;
    std::function<OpllEnvelopeValues()> getValues;
    std::function<bool()> getSustainedTone;

    void paint(juce::Graphics& graphics) override {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF111920));
        graphics.fillRoundedRectangle(graph, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(graph, 5.0F, 1.0F);

        graphics.setColour(juce::Colour(0xFF34434E));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX() + 1.0F,
            graph.getRight() - 1.0F);

        const float key_off_x = graph.getX()
            + graph.getWidth()
                * mgstc::engine::OpllEnvelopeTrace::
                      kKeyOffSeconds
                / mgstc::engine::OpllEnvelopeTrace::
                      kDurationSeconds;
        const float dash_pattern[]{4.0F, 3.0F};
        graphics.setColour(juce::Colour(0xFFD66B6B));
        graphics.drawDashedLine(
            {key_off_x,
             graph.getY() + 1.0F,
             key_off_x,
             graph.getBottom() - 1.0F},
            dash_pattern,
            2,
            1.0F);

        if (valid_) {
            juce::Path envelope;
            for (std::size_t index = 0;
                 index < levels_.size();
                 ++index) {
                const float x = juce::jmap(
                    static_cast<float>(index),
                    0.0F,
                    static_cast<float>(levels_.size() - 1),
                    graph.getX() + 2.0F,
                    graph.getRight() - 2.0F);
                const float level = juce::jlimit(
                    0.0F, 1.0F, levels_[index]);
                const float y = juce::jmap(
                    level,
                    0.0F,
                    1.0F,
                    graph.getBottom() - 3.0F,
                    graph.getY() + 3.0F);
                if (index == 0) {
                    envelope.startNewSubPath(x, y);
                } else {
                    envelope.lineTo(x, y);
                }
            }
            juce::Path envelope_fill = envelope;
            envelope_fill.lineTo(
                graph.getRight() - 2.0F, graph.getBottom() - 3.0F);
            envelope_fill.lineTo(
                graph.getX() + 2.0F, graph.getBottom() - 3.0F);
            envelope_fill.closeSubPath();
            graphics.setColour(waveform_colour_.withAlpha(0.24F));
            graphics.fillPath(envelope_fill);
            graphics.setColour(waveform_colour_.withAlpha(0.98F));
            graphics.strokePath(
                envelope,
                juce::PathStrokeType(
                    1.7F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
        }

        const auto guide = adsrGuide(
            graph, values(), sustainedTone());
        juce::Path adsr;
        adsr.startNewSubPath(graph.getX() + 2.0F, graph.getBottom() - 3.0F);
        adsr.lineTo(guide.attack);
        adsr.lineTo(guide.decay_sustain);
        adsr.lineTo(guide.sustain_release);
        adsr.lineTo(guide.release);
        graphics.setColour(juce::Colour(0xFFFFF1A8));
        graphics.strokePath(
            adsr,
            juce::PathStrokeType(
                2.0F,
                juce::PathStrokeType::curved,
                juce::PathStrokeType::rounded));
        const auto drawHandle = [&graphics](juce::Point<float> handle) {
            graphics.setColour(juce::Colour(0xFF141A20));
            graphics.fillEllipse(handle.x - 10.0F, handle.y - 10.0F,
                                 20.0F, 20.0F);
            graphics.setColour(juce::Colour(0xFFFF514D));
            graphics.fillEllipse(handle.x - 7.0F, handle.y - 7.0F,
                                 14.0F, 14.0F);
            graphics.setColour(juce::Colour(0xFFFFC1BB));
            graphics.drawEllipse(handle.x - 7.0F, handle.y - 7.0F,
                                 14.0F, 14.0F, 1.0F);
        };
        drawHandle(guide.attack);
        drawHandle(guide.decay_sustain);
        drawHandle(guide.key_off_handle);
        if (guide.release_editable) {
            drawHandle(guide.release);
        }

        graphics.setFont(UiFonts::dense());
        graphics.setColour(juce::Colour(0xFF9BA8B2));
        graphics.drawText(
            "0s",
            getLocalBounds().reduced(UiScale::sx(5)).removeFromBottom(
                UiScale::sx(15)),
            juce::Justification::bottomLeft);
        graphics.drawText(
            "4s",
            getLocalBounds().reduced(UiScale::sx(5)).removeFromBottom(
                UiScale::sx(15)),
            juce::Justification::bottomRight);
        graphics.setColour(juce::Colour(0xFFD98A8A));
        graphics.drawText(
            "KO 2s",
            juce::Rectangle<int>(
                juce::roundToInt(key_off_x) + UiScale::sx(4),
                getHeight() - UiScale::sx(21),
                UiScale::sx(42),
                UiScale::sx(16)),
            juce::Justification::centredLeft);

    }

    void mouseDown(const juce::MouseEvent& event) override {
        const auto graph = getLocalBounds().toFloat().reduced(0.5F);
        drag_start_position_ = event.position;
        drag_start_values_ = values();
        drag_values_ = drag_start_values_;
        drag_start_sustained_tone_ = sustainedTone();
        drag_handle_ = handleAt(
            event.position,
            adsrGuide(
                graph,
                drag_start_values_,
                drag_start_sustained_tone_));
        drag_changed_ = false;
        if (drag_handle_) {
            updateFromMouse(event.position);
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
        if (drag_handle_ && drag_changed_ && onEdit) {
            onEdit(drag_values_, true);
        }
        drag_handle_.reset();
        drag_changed_ = false;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& event) override {
        const auto graph = getLocalBounds().toFloat().reduced(0.5F);
        const auto handle = handleAt(
            event.position, adsrGuide(graph, values(), sustainedTone()));
        if (handle) {
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        } else {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (!drag_handle_) {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }

    void mouseWheelMove(
        const juce::MouseEvent&,
        const juce::MouseWheelDetails&) override {}

private:
    struct AdsrGuide {
        juce::Point<float> attack;
        juce::Point<float> decay_sustain;
        juce::Point<float> sustain_release;
        juce::Point<float> key_off_handle;
        juce::Point<float> release;
        bool release_editable{};
    };

    [[nodiscard]] OpllEnvelopeValues values() const {
        return getValues ? getValues() : OpllEnvelopeValues{};
    }

    [[nodiscard]] bool sustainedTone() const {
        return getSustainedTone ? getSustainedTone() : true;
    }

    [[nodiscard]] static float levelY(
        const juce::Rectangle<float>& graph,
        std::uint8_t sustain_level) {
        return juce::jmap(
            static_cast<float>(sustain_level), 0.0F, 15.0F,
            graph.getY() + 3.0F, graph.getBottom() - 3.0F);
    }

    [[nodiscard]] static float inverseRateX(
        float left,
        float width,
        std::uint8_t rate) {
        return left + width * (1.0F - static_cast<float>(rate) / 15.0F);
    }

    [[nodiscard]] static float rateFromInverseX(
        float position,
        float left,
        float width) {
        const float fraction = juce::jlimit(
            0.0F, 1.0F, (position - left) / juce::jmax(1.0F, width));
        return (1.0F - fraction) * 15.0F;
    }

    [[nodiscard]] AdsrGuide adsrGuide(
        const juce::Rectangle<float>& graph,
        const OpllEnvelopeValues& values,
        bool sustained_tone) const {
        const float section = graph.getWidth() / 4.0F;
        const float top = graph.getY() + 3.0F;
        const float bottom = graph.getBottom() - 3.0F;
        const float key_off_x = graph.getX() + graph.getWidth() * 0.5F;
        const float attack_x = inverseRateX(
            graph.getX() + 4.0F, section - 8.0F, values.attack_rate);
        const float decay_x = values.decay_rate == 0
            ? attack_x
            : inverseRateX(
                  graph.getX() + section + 4.0F,
                  section - 8.0F,
                  values.decay_rate);
        const float sustain_y = levelY(graph, values.sustain_level);
        constexpr std::uint8_t kNonSustainedReleaseRate = 7;
        const auto release_rate = sustained_tone
            ? values.release_rate
            : kNonSustainedReleaseRate;
        const float release_x = inverseRateX(
            graph.getX() + section * 3.0F + 4.0F, section - 8.0F,
            release_rate);
        float key_off_y = sustain_y;
        if (!sustained_tone) {
            // EG=0 continues the post-DR decay with RR until key-off. The
            // emu2413 trace remains the authoritative curve; this is only a
            // readable parameter guide over the trace.
            const float rr_progress = juce::jmap(
                static_cast<float>(values.release_rate),
                0.0F,
                15.0F,
                0.08F,
                0.94F);
            const float time_to_key_off = juce::jlimit(
                0.0F,
                1.0F,
                (key_off_x - decay_x)
                    / juce::jmax(1.0F, graph.getWidth() * 0.5F));
            key_off_y = juce::jmap(
                juce::jlimit(
                    0.0F,
                    1.0F,
                    rr_progress * time_to_key_off * 1.25F),
                0.0F,
                1.0F,
                sustain_y,
                bottom);
        }
        const float rr_handle_y = modulator_ && sustained_tone
            ? juce::jmap(
                  static_cast<float>(values.release_rate),
                  0.0F,
                  15.0F,
                  sustain_y,
                  bottom)
            : key_off_y;
        const auto release = modulator_
            ? juce::Point<float>(key_off_x, key_off_y)
            : juce::Point<float>(release_x, bottom);
        return {{attack_x, top},
                {decay_x, sustain_y},
                {key_off_x, key_off_y},
                {key_off_x, rr_handle_y},
                release,
                sustained_tone && !modulator_};
    }

    [[nodiscard]] static std::optional<OpllEnvelopeHandle> handleAt(
        juce::Point<float> position,
        const AdsrGuide& guide) {
        constexpr float hit_radius = 18.0F;
        const auto hit = [position](juce::Point<float> point) {
            return position.getDistanceFrom(point) <= hit_radius;
        };
        if (hit(guide.attack)) {
            return OpllEnvelopeHandle::Attack;
        }
        if (hit(guide.decay_sustain)) {
            return OpllEnvelopeHandle::DecaySustain;
        }
        if (hit(guide.key_off_handle)) {
            return OpllEnvelopeHandle::SustainRelease;
        }
        if (guide.release_editable && hit(guide.release)) {
            return OpllEnvelopeHandle::Release;
        }
        return std::nullopt;
    }

    void updateFromMouse(juce::Point<float> position) {
        if (!drag_handle_) {
            return;
        }
        auto next = values();
        const auto graph = getLocalBounds().toFloat().reduced(0.5F);
        if (drag_handle_) {
            const float section = graph.getWidth() / 4.0F;
            const auto start_guide = adsrGuide(
                graph,
                drag_start_values_,
                drag_start_sustained_tone_);
            const float delta_x =
                position.x - drag_start_position_.x;
            switch (*drag_handle_) {
            case OpllEnvelopeHandle::Attack:
                next.attack_rate = static_cast<std::uint8_t>(juce::roundToInt(
                    rateFromInverseX(start_guide.attack.x + delta_x,
                                     graph.getX() + 4.0F,
                                     section - 8.0F)));
                next.decay_rate = drag_start_values_.decay_rate == 0
                    ? 0
                    : static_cast<std::uint8_t>(juce::roundToInt(
                          rateFromInverseX(
                              start_guide.decay_sustain.x + delta_x,
                              graph.getX() + section + 4.0F,
                              section - 8.0F)));
                break;
            case OpllEnvelopeHandle::DecaySustain:
                next.decay_rate = static_cast<std::uint8_t>(juce::roundToInt(
                    rateFromInverseX(position.x, graph.getX() + section + 4.0F,
                                     section - 8.0F)));
                next.sustain_level = static_cast<std::uint8_t>(juce::roundToInt(
                    juce::jmap(position.y, graph.getY() + 3.0F,
                               graph.getBottom() - 3.0F, 0.0F, 15.0F)));
                break;
            case OpllEnvelopeHandle::SustainRelease: {
                if (drag_start_sustained_tone_ && !modulator_) {
                    next.sustain_level = static_cast<std::uint8_t>(
                        juce::roundToInt(
                            juce::jmap(
                                position.y,
                                graph.getY() + 3.0F,
                                graph.getBottom() - 3.0F,
                                0.0F,
                                15.0F)));
                }
                // EG=0 has no sustain phase: its key-off handle edits RR
                // only, while SL remains assigned to the second handle. Its
                // vertical position is the RR cue: up decreases RR and down
                // increases RR. Only a sustained CAR uses horizontal RR.
                const bool rr_is_vertical = !drag_start_sustained_tone_
                    || modulator_;
                const float rr_delta = rr_is_vertical
                    ? position.y - drag_start_position_.y
                    : position.x - drag_start_position_.x;
                const float rr_direction = rr_is_vertical ? 1.0F : -1.0F;
                next.release_rate = static_cast<std::uint8_t>(juce::roundToInt(
                    juce::jlimit(
                        0.0F,
                        15.0F,
                        static_cast<float>(drag_start_values_.release_rate)
                            + rr_direction * rr_delta * 15.0F
                                / juce::jmax(
                                    1.0F,
                                    rr_is_vertical
                                        ? graph.getHeight()
                                        : graph.getWidth()))));
                break;
            }
            case OpllEnvelopeHandle::Release:
                next.release_rate = static_cast<std::uint8_t>(juce::roundToInt(
                    rateFromInverseX(position.x,
                                     graph.getX() + section * 3.0F + 4.0F,
                                     section - 8.0F)));
                break;
            }
        } else {
            return;
        }
        next.attack_rate = juce::jlimit<std::uint8_t>(0, 15, next.attack_rate);
        next.decay_rate = juce::jlimit<std::uint8_t>(0, 15, next.decay_rate);
        next.sustain_level = juce::jlimit<std::uint8_t>(0, 15, next.sustain_level);
        next.release_rate = juce::jlimit<std::uint8_t>(0, 15, next.release_rate);
        if (drag_changed_
            && next.attack_rate == drag_values_.attack_rate
            && next.decay_rate == drag_values_.decay_rate
            && next.sustain_level == drag_values_.sustain_level
            && next.release_rate == drag_values_.release_rate) {
            return;
        }
        drag_values_ = next;
        drag_changed_ = true;
        if (onEdit) {
            onEdit(drag_values_, false);
        }
    }

    std::array<
        float,
        mgstc::engine::OpllEnvelopeTrace::kPointCount>
        levels_{};
    juce::Colour waveform_colour_;
    bool modulator_{};
    juce::Point<float> drag_start_position_;
    OpllEnvelopeValues drag_start_values_{};
    bool drag_start_sustained_tone_{};
    std::optional<OpllEnvelopeHandle> drag_handle_;
    OpllEnvelopeValues drag_values_{};
    bool drag_changed_{};
    bool valid_{};
};

class OpllOperatorPanel final : public juce::Component {
public:
    OpllOperatorPanel(
        const juce::String& title,
        bool modulator,
        SwitchLookAndFeel& switch_look_and_feel)
        : modulator_(modulator),
          switch_look_and_feel_(switch_look_and_feel),
          envelope_graph_(
              modulator
                  ? juce::Colour(0xFF64A7FF)
                  : juce::Colour(0xFFFFA75E),
              modulator) {
        title_.setText(title, juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        constexpr std::array<const char*, 5> flag_names{
            "AM", "PM", "EG", "KR", "WS"};
        for (std::size_t index = 0; index < flags_.size(); ++index) {
            flags_[index].setButtonText(flag_names[index]);
            flags_[index].setLookAndFeel(&switch_look_and_feel_);
            flags_[index].setColour(
                juce::ToggleButton::textColourId,
                juce::Colour(0xFFC9D1D9));
            flags_[index].onClick = [this, index] {
                if (index == 2) {
                    envelope_graph_.repaint();
                }
                notifyChanged(true);
            };
            addAndMakeVisible(flags_[index]);
        }

        configureSlider(multiplier_, "MULT", 0.0, 15.0);
        configureSlider(key_scale_level_, "KSL", 0.0, 3.0);
        configureSlider(attack_rate_, "AR (A)", 0.0, 15.0);
        configureSlider(decay_rate_, "DR (D)", 0.0, 15.0);
        configureSlider(sustain_level_, "SL (S)", 0.0, 15.0);
        configureSlider(release_rate_, "RR (R)", 0.0, 15.0);
        envelope_title_.setText(
            modulator_
                ? juce::String::fromUTF8(
                      "MOD 実効EG  C4／4秒")
                : juce::String::fromUTF8(
                      "CAR 実効EG  C4／4秒"),
            juce::dontSendNotification);
        envelope_title_.setFont(UiFonts::body(true));
        addAndMakeVisible(envelope_title_);
        envelope_graph_.setTooltip(
            juce::String::fromUTF8(
                "淡いADSRライン上の赤い●をドラッグして編集します。"
                "Attack点はAR、Decay/Sustain点はDRとSL、"
                "EG=1のキーオフ点はSLとRR、EG=0のキーオフ点はRR、"
                "Release終端点はEG=1のRRに対応します。"
                "EG=0とMODのRRは上で減少、下で増加します。"
                "AR・DR・RRは右ほど遅く、SLは下ほど減衰量が"
                "大きくなります。赤い線は2秒地点のキーオフです"));
        envelope_graph_.onEdit =
            [this](
                const OpllEnvelopeValues& values,
                bool commit) {
                attack_rate_.setValue(
                    values.attack_rate, juce::dontSendNotification);
                decay_rate_.setValue(
                    values.decay_rate, juce::dontSendNotification);
                sustain_level_.setValue(
                    values.sustain_level, juce::dontSendNotification);
                release_rate_.setValue(
                    values.release_rate, juce::dontSendNotification);
                if (onEnvelopeEdit) {
                    onEnvelopeEdit(commit);
                }
            };
        envelope_graph_.getValues =
            [this] {
                return OpllEnvelopeValues{
                    static_cast<std::uint8_t>(attack_rate_.getValue()),
                    static_cast<std::uint8_t>(decay_rate_.getValue()),
                    static_cast<std::uint8_t>(sustain_level_.getValue()),
                    static_cast<std::uint8_t>(release_rate_.getValue())};
            };
        envelope_graph_.getSustainedTone =
            [this] { return flags_[2].getToggleState(); };
        addAndMakeVisible(envelope_graph_);
    }

    ~OpllOperatorPanel() override {
        for (auto& flag : flags_) {
            flag.setLookAndFeel(nullptr);
        }
    }

    void setParameters(
        const mgstc::engine::OpllOperatorParameters& parameters) {
        syncing_ = true;
        flags_[0].setToggleState(
            parameters.amplitude_modulation,
            juce::dontSendNotification);
        flags_[1].setToggleState(
            parameters.pitch_modulation,
            juce::dontSendNotification);
        flags_[2].setToggleState(
            parameters.sustained_tone,
            juce::dontSendNotification);
        flags_[3].setToggleState(
            parameters.key_rate_scaling,
            juce::dontSendNotification);
        flags_[4].setToggleState(
            parameters.waveform,
            juce::dontSendNotification);
        multiplier_.setValue(
            parameters.multiplier, juce::dontSendNotification);
        key_scale_level_.setValue(
            parameters.key_scale_level, juce::dontSendNotification);
        attack_rate_.setValue(
            parameters.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(
            parameters.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(
            parameters.sustain_level, juce::dontSendNotification);
        release_rate_.setValue(
            parameters.release_rate, juce::dontSendNotification);
        syncing_ = false;
    }

    void setEnvelopeTrace(
        const std::array<
            float,
            mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
        bool valid) {
        envelope_graph_.setTrace(levels, valid);
    }

    void setAuditionNote(std::uint8_t midi_note) {
        envelope_title_.setText(
            (modulator_ ? "MOD" : "CAR")
                + juce::String::fromUTF8(" 実効EG  ")
                + midiNoteName(midi_note)
                + juce::String::fromUTF8("／4秒"),
            juce::dontSendNotification);
    }

    void applyDiffHighlight(
        const mgstc::engine::OpllOperatorParameters& current,
        const mgstc::engine::OpllOperatorParameters& baseline) {
        constexpr juce::uint32 kDiffAccent = 0xFFE8B84A;
        constexpr juce::uint32 kNormal = 0xFFC9D1D9;
        const auto colour_for = [](bool dirty) {
            return juce::Colour(dirty ? kDiffAccent : kNormal);
        };
        flags_[0].setColour(
            juce::ToggleButton::textColourId,
            colour_for(
                current.amplitude_modulation
                != baseline.amplitude_modulation));
        flags_[1].setColour(
            juce::ToggleButton::textColourId,
            colour_for(
                current.pitch_modulation != baseline.pitch_modulation));
        flags_[2].setColour(
            juce::ToggleButton::textColourId,
            colour_for(current.sustained_tone != baseline.sustained_tone));
        flags_[3].setColour(
            juce::ToggleButton::textColourId,
            colour_for(
                current.key_rate_scaling != baseline.key_rate_scaling));
        flags_[4].setColour(
            juce::ToggleButton::textColourId,
            colour_for(current.waveform != baseline.waveform));
        for (auto& flag : flags_) {
            flag.repaint();
        }
        multiplier_label_.setColour(
            juce::Label::textColourId,
            colour_for(current.multiplier != baseline.multiplier));
        key_scale_level_label_.setColour(
            juce::Label::textColourId,
            colour_for(
                current.key_scale_level != baseline.key_scale_level));
        attack_rate_label_.setColour(
            juce::Label::textColourId,
            colour_for(current.attack_rate != baseline.attack_rate));
        decay_rate_label_.setColour(
            juce::Label::textColourId,
            colour_for(current.decay_rate != baseline.decay_rate));
        sustain_level_label_.setColour(
            juce::Label::textColourId,
            colour_for(current.sustain_level != baseline.sustain_level));
        release_rate_label_.setColour(
            juce::Label::textColourId,
            colour_for(current.release_rate != baseline.release_rate));
    }

    void clearDiffHighlight() {
        const auto normal = juce::Colour(0xFFC9D1D9);
        for (auto& flag : flags_) {
            flag.setColour(juce::ToggleButton::textColourId, normal);
            flag.repaint();
        }
        for (auto* label :
             {&multiplier_label_,
              &key_scale_level_label_,
              &attack_rate_label_,
              &decay_rate_label_,
              &sustain_level_label_,
              &release_rate_label_}) {
            label->setColour(juce::Label::textColourId, normal);
        }
    }

    [[nodiscard]] mgstc::engine::OpllOperatorParameters
    parameters() const {
        mgstc::engine::OpllOperatorParameters result;
        result.amplitude_modulation = flags_[0].getToggleState();
        result.pitch_modulation = flags_[1].getToggleState();
        result.sustained_tone = flags_[2].getToggleState();
        result.key_rate_scaling = flags_[3].getToggleState();
        result.waveform = flags_[4].getToggleState();
        result.multiplier =
            static_cast<std::uint8_t>(multiplier_.getValue());
        result.key_scale_level =
            static_cast<std::uint8_t>(key_scale_level_.getValue());
        result.total_level = 0;
        result.attack_rate =
            static_cast<std::uint8_t>(attack_rate_.getValue());
        result.decay_rate =
            static_cast<std::uint8_t>(decay_rate_.getValue());
        result.sustain_level =
            static_cast<std::uint8_t>(sustain_level_.getValue());
        result.release_rate =
            static_cast<std::uint8_t>(release_rate_.getValue());
        return result;
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds =
            getLocalBounds().toFloat().reduced(0.5F);
        const auto radius =
            static_cast<float>(UiScale::sx(9));
        graphics.setColour(juce::Colour(0xFF29323C));
        graphics.fillRoundedRectangle(bounds, radius);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, radius, 1.0F);
    }

    void resized() override {
        title_.setFont(UiFonts::heading());
        envelope_title_.setFont(UiFonts::body(true));
        for (auto* label :
             {&multiplier_label_,
              &key_scale_level_label_,
              &attack_rate_label_,
              &decay_rate_label_,
              &sustain_level_label_,
              &release_rate_label_}) {
            label->setFont(UiFonts::body());
        }
        auto area = getLocalBounds().reduced(UiScale::sx(14));
        title_.setBounds(area.removeFromTop(UiScale::sx(28)));
        area.removeFromTop(UiScale::sx(6));
        auto flag_area = area.removeFromTop(UiScale::sx(30));
        const auto flag_width = juce::jmax(
            UiScale::sx(54),
            (flag_area.getWidth() - UiLayout::controlGap * 4) / 5);
        for (std::size_t index = 0; index < flags_.size(); ++index) {
            flags_[index].setBounds(
                flag_area.removeFromLeft(flag_width));
            if (index + 1 < flags_.size()) {
                flag_area.removeFromLeft(UiLayout::controlGap);
            }
        }
        area.removeFromTop(UiScale::sx(10));
        auto basic_row = area.removeFromTop(UiScale::sx(36));
        const auto basic_gap = UiScale::sx(8);
        const auto basic_width =
            (basic_row.getWidth() - basic_gap) / 2;
        auto multiplier_area = basic_row.removeFromLeft(basic_width);
        multiplier_label_.setBounds(
            multiplier_area.removeFromLeft(UiScale::sx(48)));
        multiplier_.setBounds(multiplier_area);
        basic_row.removeFromLeft(basic_gap);
        key_scale_level_label_.setBounds(
            basic_row.removeFromLeft(UiScale::sx(42)));
        key_scale_level_.setBounds(basic_row);
        area.removeFromTop(UiScale::sx(10));

        auto graph_area = area.removeFromBottom(UiScale::sx(224));
        area.removeFromBottom(UiScale::sx(8));
        auto envelope = area;
        const int gap = UiScale::sx(8);
        const auto column_width =
            (envelope.getWidth() - gap * 3) / 4;
        const int label_h = UiScale::sx(24);
        const int text_box_w = UiScale::sx(46);
        const int text_box_h = UiScale::sx(24);
        std::array<juce::Label*, 4> labels{
            &attack_rate_label_,
            &decay_rate_label_,
            &sustain_level_label_,
            &release_rate_label_};
        std::array<juce::Slider*, 4> sliders{
            &attack_rate_,
            &decay_rate_,
            &sustain_level_,
            &release_rate_};
        for (std::size_t index = 0; index < sliders.size(); ++index) {
            auto column = envelope.removeFromLeft(column_width);
            labels[index]->setBounds(column.removeFromTop(label_h));
            sliders[index]->setTextBoxStyle(
                juce::Slider::TextBoxBelow,
                false,
                text_box_w,
                text_box_h);
            sliders[index]->setBounds(column);
            if (index + 1 < sliders.size()) {
                envelope.removeFromLeft(gap);
            }
        }
        envelope_title_.setBounds(
            graph_area.removeFromTop(UiScale::sx(20)));
        graph_area.removeFromTop(UiScale::sx(3));
        envelope_graph_.setBounds(graph_area);

        multiplier_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            text_box_w,
            text_box_h);
        key_scale_level_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            text_box_w,
            text_box_h);
    }

    std::function<void(bool commit)> onChange;
    std::function<void(bool)> onEnvelopeEdit;

private:
    void configureSlider(
        juce::Slider& slider,
        const juce::String& label_text,
        double minimum,
        double maximum) {
        auto* label = labelFor(slider);
        label->setText(label_text, juce::dontSendNotification);
        label->setFont(UiFonts::body());
        label->setJustificationType(
            juce::Justification::centred);
        addAndMakeVisible(*label);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(
            &slider == &attack_rate_
                || &slider == &decay_rate_
                || &slider == &sustain_level_
                || &slider == &release_rate_
                ? juce::Slider::LinearVertical
                : juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            slider.getSliderStyle() == juce::Slider::LinearVertical
                ? juce::Slider::TextBoxBelow
                : juce::Slider::TextBoxRight,
            false,
            UiScale::sx(46),
            UiScale::sx(24));
        // Text box Label is created lazily; apply after first layout via LAF.
        slider.setColour(
            juce::Slider::textBoxTextColourId,
            juce::Colour(0xFFF2F4F5));
        slider.setScrollWheelEnabled(true);
        slider.onValueChange = [this, &slider] {
            const bool dragging =
                slider.isMouseButtonDown()
                || slider.getThumbBeingDragged() >= 0;
            notifyChanged(!dragging);
        };
        slider.onDragEnd = [this] {
            notifyChanged(true);
        };
        addAndMakeVisible(slider);
    }

    [[nodiscard]] juce::Label* labelFor(juce::Slider& slider) {
        if (&slider == &multiplier_) {
            return &multiplier_label_;
        }
        if (&slider == &key_scale_level_) {
            return &key_scale_level_label_;
        }
        if (&slider == &attack_rate_) {
            return &attack_rate_label_;
        }
        if (&slider == &decay_rate_) {
            return &decay_rate_label_;
        }
        if (&slider == &sustain_level_) {
            return &sustain_level_label_;
        }
        return &release_rate_label_;
    }

    void notifyChanged(bool commit = true) {
        if (!syncing_ && onChange) {
            onChange(commit);
        }
    }

    bool modulator_{};
    bool syncing_{};
    SwitchLookAndFeel& switch_look_and_feel_;
    juce::Label title_;
    std::array<juce::ToggleButton, 5> flags_;
    juce::Slider multiplier_;
    juce::Slider key_scale_level_;
    juce::Slider attack_rate_;
    juce::Slider decay_rate_;
    juce::Slider sustain_level_;
    juce::Slider release_rate_;
    juce::Label multiplier_label_;
    juce::Label key_scale_level_label_;
    juce::Label attack_rate_label_;
    juce::Label decay_rate_label_;
    juce::Label sustain_level_label_;
    juce::Label release_rate_label_;
    juce::Label envelope_title_;
    OpllEnvelopeGraph envelope_graph_;
};


// Shared FM parameter view: TL/Feedback header + MOD/CAR operator panels
// (OPLL editor and manual-y dialog).
class OpllPatchParameterPanel final : public juce::Component {
public:
    explicit OpllPatchParameterPanel(SwitchLookAndFeel& switch_look_and_feel)
        : modulator_("MODULATOR", true, switch_look_and_feel),
          carrier_("CARRIER", false, switch_look_and_feel) {
        common_title_.setText(
            juce::String::fromUTF8("モジュレーター制御"),
            juce::dontSendNotification);
        common_title_.setFont(UiFonts::heading());
        addAndMakeVisible(common_title_);

        const auto configure_common = [this](
            juce::Label& label,
            juce::Slider& slider,
            const juce::String& text,
            double maximum) {
            label.setText(text, juce::dontSendNotification);
            label.setFont(UiFonts::body());
            label.setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(label);
            slider.setRange(0.0, maximum, 1.0);
            slider.setSliderStyle(juce::Slider::LinearHorizontal);
            slider.setTextBoxStyle(
                juce::Slider::TextBoxRight,
                false,
                UiScale::sx(46),
                UiScale::sx(24));
            slider.setScrollWheelEnabled(true);
            slider.setColour(
                juce::Slider::textBoxTextColourId, juce::Colour(0xFFF2F4F5));
            slider.onValueChange = [this, &slider] {
                const bool dragging =
                    slider.isMouseButtonDown()
                    || slider.getThumbBeingDragged() >= 0;
                notifyChanged(!dragging);
            };
            slider.onDragEnd = [this] { notifyChanged(true); };
            addAndMakeVisible(slider);
        };
        configure_common(tl_label_, tl_, "TL", 63.0);
        tl_.setTooltip(juce::String::fromUTF8(
            "モジュレーターの出力レベル 0～63"));
        configure_common(feedback_label_, feedback_, "Feedback", 7.0);
        feedback_.setTooltip(juce::String::fromUTF8(
            "モジュレーターの自己帰還量 0～7"));

        addAndMakeVisible(modulator_);
        addAndMakeVisible(carrier_);
        modulator_.onChange = [this](bool commit) { notifyChanged(commit); };
        carrier_.onChange = [this](bool commit) { notifyChanged(commit); };
        modulator_.onEnvelopeEdit = [this](bool commit) {
            notifyChanged(commit);
        };
        carrier_.onEnvelopeEdit = [this](bool commit) {
            notifyChanged(commit);
        };
    }

    void setParameters(const mgstc::engine::OpllPatchParameters& patch) {
        syncing_ = true;
        modulator_.setParameters(patch.modulator);
        carrier_.setParameters(patch.carrier);
        tl_.setValue(patch.modulator.total_level, juce::dontSendNotification);
        feedback_.setValue(patch.feedback, juce::dontSendNotification);
        syncing_ = false;
        refreshDiffHighlight();
    }

    [[nodiscard]] mgstc::engine::OpllPatchParameters parameters() const {
        auto patch = mgstc::engine::OpllPatchParameters{
            .modulator = modulator_.parameters(),
            .carrier = carrier_.parameters(),
            .feedback = static_cast<std::uint8_t>(feedback_.getValue()),
        };
        patch.modulator.total_level =
            static_cast<std::uint8_t>(tl_.getValue());
        return patch;
    }

    void setDiffBaseline(
        const std::optional<mgstc::engine::OpllPatchParameters>& baseline) {
        baseline_ = baseline;
        refreshDiffHighlight();
    }

    void setAuditionNote(std::uint8_t midi_note) {
        audition_note_ = midi_note;
        modulator_.setAuditionNote(midi_note);
        carrier_.setAuditionNote(midi_note);
    }

    void refreshEnvelopeTrace() {
        const auto patch = parameters();
        if (trace_cached_
            && traced_note_ == audition_note_
            && traced_patch_ == patch) {
            return;
        }
        const auto trace =
            mgstc::engine::traceOpllEnvelope(patch, audition_note_);
        modulator_.setEnvelopeTrace(trace.modulator, trace.valid);
        carrier_.setEnvelopeTrace(trace.carrier, trace.valid);
        traced_patch_ = patch;
        traced_note_ = audition_note_;
        trace_cached_ = true;
    }

    void paint(juce::Graphics& graphics) override {
        paintRoundedPanelFrame(graphics, common_bounds_);
    }

    void resized() override {
        using namespace UiLayout;
        common_title_.setFont(UiFonts::heading());
        tl_label_.setFont(UiFonts::body());
        feedback_label_.setFont(UiFonts::body());
        auto area = getLocalBounds();
        common_bounds_ = area.removeFromTop(UiScale::sx(62));
        auto common = common_bounds_.reduced(panelPad, xs + 1);
        common_title_.setBounds(common.removeFromTop(UiScale::sx(20)));
        auto common_row = common;
        const auto common_width = (common_row.getWidth() - panelGap) / 2;
        auto tl_area = common_row.removeFromLeft(common_width);
        tl_label_.setBounds(tl_area.removeFromLeft(UiScale::sx(34)));
        tl_.setBounds(tl_area);
        common_row.removeFromLeft(panelGap);
        feedback_label_.setBounds(common_row.removeFromLeft(UiScale::sx(72)));
        feedback_.setBounds(common_row);
        const int text_box_w = UiScale::sx(46);
        const int text_box_h = UiScale::sx(24);
        tl_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, text_box_w, text_box_h);
        feedback_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, text_box_w, text_box_h);
        area.removeFromTop(sm);
        const auto panel_width = (area.getWidth() - panelGap + xs) / 2;
        modulator_.setBounds(area.removeFromLeft(panel_width));
        area.removeFromLeft(panelGap - xs);
        carrier_.setBounds(area);
    }

    std::function<void(bool commit)> onChange;

private:
    void notifyChanged(bool commit) {
        if (syncing_) {
            return;
        }
        refreshDiffHighlight();
        if (onChange) {
            onChange(commit);
        }
    }

    void refreshDiffHighlight() {
        if (!baseline_) {
            modulator_.clearDiffHighlight();
            carrier_.clearDiffHighlight();
            tl_label_.setColour(
                juce::Label::textColourId, juce::Colour(0xFFC9D1D9));
            feedback_label_.setColour(
                juce::Label::textColourId, juce::Colour(0xFFC9D1D9));
            return;
        }
        const auto cur = parameters();
        modulator_.applyDiffHighlight(cur.modulator, baseline_->modulator);
        carrier_.applyDiffHighlight(cur.carrier, baseline_->carrier);
        constexpr juce::uint32 kDiffAccent = 0xFFE8B84A;
        constexpr juce::uint32 kNormal = 0xFFC9D1D9;
        tl_label_.setColour(
            juce::Label::textColourId,
            juce::Colour(
                cur.modulator.total_level != baseline_->modulator.total_level
                    ? kDiffAccent
                    : kNormal));
        feedback_label_.setColour(
            juce::Label::textColourId,
            juce::Colour(
                cur.feedback != baseline_->feedback ? kDiffAccent : kNormal));
    }

    bool syncing_{};
    std::uint8_t audition_note_{kPreviewNote};
    std::optional<mgstc::engine::OpllPatchParameters> baseline_;
    juce::Label common_title_;
    juce::Label tl_label_;
    juce::Slider tl_;
    juce::Label feedback_label_;
    juce::Slider feedback_;
    juce::Rectangle<int> common_bounds_;
    OpllOperatorPanel modulator_;
    OpllOperatorPanel carrier_;
    std::optional<mgstc::engine::OpllPatchParameters> traced_patch_;
    std::uint8_t traced_note_{};
    bool trace_cached_{};
};

