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

enum class OpllEnvelopeParameter {
    AttackRate,
    DecayRate,
    SustainLevel,
    ReleaseRate,
};

class OpllEnvelopeGraph final
    : public juce::Component,
      public juce::SettableTooltipClient {
public:
    explicit OpllEnvelopeGraph(juce::Colour waveform_colour)
        : waveform_colour_(waveform_colour) {}

    void setTrace(
        const std::array<
            float,
            mgstc::engine::OpllEnvelopeTrace::kPointCount>& levels,
        bool valid) {
        levels_ = levels;
        valid_ = valid;
        repaint();
    }

    std::function<void(
        OpllEnvelopeParameter,
        std::uint8_t,
        bool)> onEdit;
    std::function<std::uint8_t(OpllEnvelopeParameter)> getValue;

    void paint(juce::Graphics& graphics) override {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(juce::Colour(0xFF111920));
        graphics.fillRoundedRectangle(graph, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(graph, 5.0F, 1.0F);

        graphics.setColour(juce::Colour(0xFF34434E));
        for (int section = 1; section < 4; ++section) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(section) / 4.0F;
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                graph.getY() + 1.0F,
                graph.getBottom() - 1.0F);
        }
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
            graphics.setColour(waveform_colour_);
            graphics.strokePath(
                envelope,
                juce::PathStrokeType(
                    1.7F,
                    juce::PathStrokeType::curved,
                    juce::PathStrokeType::rounded));
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
            "KO 1s",
            juce::Rectangle<int>(
                juce::roundToInt(key_off_x) + UiScale::sx(4),
                getHeight() - UiScale::sx(21),
                UiScale::sx(42),
                UiScale::sx(16)),
            juce::Justification::centredLeft);

        constexpr std::array<const char*, 4> section_names{
            "A / AR", "D / DR", "S / SL", "R / RR"};
        const float section_width = graph.getWidth() / 4.0F;
        graphics.setFont(UiFonts::dense(true));
        for (std::size_t index = 0;
             index < section_names.size();
             ++index) {
            const auto section = juce::Rectangle<float>(
                graph.getX()
                    + section_width * static_cast<float>(index),
                graph.getY(),
                section_width,
                static_cast<float>(UiScale::sx(18)));
            if (drag_parameter_
                && parameterIndex(*drag_parameter_) == index) {
                graphics.setColour(
                    waveform_colour_.withAlpha(0.18F));
                graphics.fillRect(
                    juce::Rectangle<float>(
                        section.getX(),
                        graph.getY() + 1.0F,
                        section.getWidth(),
                        graph.getHeight() - 2.0F));
            }
            graphics.setColour(juce::Colour(0xFFC6D0D8));
            graphics.drawText(
                section_names[index],
                section.toNearestInt(),
                juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        drag_parameter_ = parameterAt(event.position);
        drag_changed_ = false;
        updateFromMouse(event.position);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        updateFromMouse(event.position);
        if (drag_parameter_ && drag_changed_ && onEdit) {
            onEdit(*drag_parameter_, drag_value_, true);
        }
        drag_parameter_.reset();
        drag_changed_ = false;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& event) override {
        setMouseCursor(
            parameterAt(event.position)
                    == OpllEnvelopeParameter::SustainLevel
                ? juce::MouseCursor::UpDownResizeCursor
                : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (!drag_parameter_) {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override {
        if (drag_parameter_) {
            return;
        }
        const auto parameter = parameterAt(event.position);
        if (!getValue || !onEdit) {
            return;
        }
        const float primary =
            std::abs(wheel.deltaY) >= std::abs(wheel.deltaX)
                ? wheel.deltaY
                : wheel.deltaX;
        if (std::abs(primary) < 1.0e-4F) {
            return;
        }
        const int step = primary > 0.0F ? 1 : -1;
        const auto current = getValue(parameter);
        const auto next = static_cast<std::uint8_t>(juce::jlimit(
            0,
            15,
            static_cast<int>(current) + step));
        if (next == current) {
            return;
        }
        onEdit(parameter, next, true);
        repaint();
    }

private:
    [[nodiscard]] static std::size_t parameterIndex(
        OpllEnvelopeParameter parameter) {
        return static_cast<std::size_t>(parameter);
    }

    [[nodiscard]] OpllEnvelopeParameter parameterAt(
        juce::Point<float> position) const {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        const float normalized = juce::jlimit(
            0.0F,
            0.9999F,
            (position.x - graph.getX())
                / juce::jmax(1.0F, graph.getWidth()));
        return static_cast<OpllEnvelopeParameter>(
            juce::jlimit(
                0,
                3,
                static_cast<int>(normalized * 4.0F)));
    }

    [[nodiscard]] std::uint8_t valueAt(
        OpllEnvelopeParameter parameter,
        juce::Point<float> position) const {
        const auto graph =
            getLocalBounds().toFloat().reduced(0.5F);
        if (parameter == OpllEnvelopeParameter::SustainLevel) {
            const float normalized = juce::jlimit(
                0.0F,
                1.0F,
                (position.y - graph.getY())
                    / juce::jmax(1.0F, graph.getHeight()));
            return static_cast<std::uint8_t>(
                juce::roundToInt(normalized * 15.0F));
        }

        const auto index =
            static_cast<float>(parameterIndex(parameter));
        const float section_width = graph.getWidth() / 4.0F;
        const float section_x =
            graph.getX() + section_width * index;
        float normalized = juce::jlimit(
            0.0F,
            1.0F,
            (position.x - section_x)
                / juce::jmax(1.0F, section_width));
        if (parameter != OpllEnvelopeParameter::ReleaseRate) {
            normalized = 1.0F - normalized;
        }
        return static_cast<std::uint8_t>(
            juce::roundToInt(normalized * 15.0F));
    }

    void updateFromMouse(juce::Point<float> position) {
        if (!drag_parameter_) {
            return;
        }
        const auto value = valueAt(*drag_parameter_, position);
        if (drag_changed_ && value == drag_value_) {
            return;
        }
        drag_value_ = value;
        drag_changed_ = true;
        if (onEdit) {
            onEdit(*drag_parameter_, value, false);
        }
    }

    std::array<
        float,
        mgstc::engine::OpllEnvelopeTrace::kPointCount>
        levels_{};
    juce::Colour waveform_colour_;
    std::optional<OpllEnvelopeParameter> drag_parameter_;
    std::uint8_t drag_value_{};
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
                  : juce::Colour(0xFFFFA75E)) {
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
            flags_[index].onClick = [this] {
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
                "AR / DR / SL / RR の区間をドラッグまたは"
                "マウスホイールで編集します。"
                "AR・DRは右ほど遅く、RRは右ほど速く、"
                "SLは下ほど減衰量が大きくなります。"
                "赤い線は1秒地点のキーオフです"));
        envelope_graph_.onEdit =
            [this](
                OpllEnvelopeParameter parameter,
                std::uint8_t value,
                bool commit) {
                sliderFor(parameter).setValue(
                    value, juce::dontSendNotification);
                if (onEnvelopeEdit) {
                    onEnvelopeEdit(parameter, value, commit);
                }
            };
        envelope_graph_.getValue =
            [this](OpllEnvelopeParameter parameter) {
                return static_cast<std::uint8_t>(juce::jlimit(
                    0.0,
                    15.0,
                    sliderFor(parameter).getValue()));
            };
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

        auto graph_area = area.removeFromBottom(UiScale::sx(112));
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
    std::function<void(
        OpllEnvelopeParameter,
        std::uint8_t,
        bool)> onEnvelopeEdit;

private:
    [[nodiscard]] juce::Slider& sliderFor(
        OpllEnvelopeParameter parameter) {
        switch (parameter) {
        case OpllEnvelopeParameter::AttackRate:
            return attack_rate_;
        case OpllEnvelopeParameter::DecayRate:
            return decay_rate_;
        case OpllEnvelopeParameter::SustainLevel:
            return sustain_level_;
        case OpllEnvelopeParameter::ReleaseRate:
            return release_rate_;
        }
        jassertfalse;
        return attack_rate_;
    }

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
        modulator_.onEnvelopeEdit =
            [this](OpllEnvelopeParameter, std::uint8_t, bool commit) {
                notifyChanged(commit);
            };
        carrier_.onEnvelopeEdit =
            [this](OpllEnvelopeParameter, std::uint8_t, bool commit) {
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
        refreshEnvelopeTrace();
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
        refreshEnvelopeTrace();
    }

    void refreshEnvelopeTrace() {
        const auto patch = parameters();
        const auto trace =
            mgstc::engine::traceOpllEnvelope(patch, audition_note_);
        modulator_.setEnvelopeTrace(trace.modulator, trace.valid);
        carrier_.setEnvelopeTrace(trace.carrier, trace.valid);
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
        refreshEnvelopeTrace();
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
};

