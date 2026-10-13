// SPDX-License-Identifier: AGPL-3.0-only

#include "composite_timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <juce_gui_extra/juce_gui_extra.h>

#include "mgstc/engine/composite_envelope_compile.hpp"
#include "mgstc/engine/composite_step_edit.hpp"
#include "mgstc/engine/composite_loop_edit.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "juce_chip_tracks.hpp"
#include "juce_utf8.hpp"
#include "last_audition_note.hpp"
#include "library_browser_chrome.hpp"
#include "opll_patch_ui.hpp"
#include "pitch_command_labels.hpp"
#include "rate_envelope_trace.hpp"
#include "editor_session.hpp"
#include "switch_look_and_feel.hpp"
#include "tag_ui.hpp"
#include "ui_chrome_constants.hpp"
#include "ui_focus.hpp"
#include "ui_fonts.hpp"
#include "ui_layout.hpp"
#include "ui_modal_dialog.hpp"
#include "ui_paint.hpp"
#include "ui_scale.hpp"
#include "mgstc_editor_view.hpp"

#include "mgstc/engine/chip_volume_curve.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_modulation.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "mgstc/engine/software_lfo.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/timbre_tags.hpp"
#include "mgstc/engine/volume.hpp"

using mgstc::app::EnvelopeTimbreCatalogItem;
using mgstc::app::EnvelopeTimbreChoice;
using mgstc::app::LayerBaseTimbreAssign;
using mgstc::app::envelopeTimbreCatalogLabel;

bool mgstc::app::invalidateEditedSccMorphPlans(
    const engine::CompositeTimbre& before, engine::CompositeTimbre& after) {
    using namespace engine;
    struct Endpoint {
        std::uint64_t id{};
        TimbrePick pick{};
        std::int32_t raw_number{};
        std::uint32_t count{};
        bool after_loop{};
        int loop_phase{}; // first pass, repeated sustain, or unreachable after ]
        std::optional<std::array<std::uint8_t, 32>> wave;
        bool operator==(const Endpoint&) const = default;
    };
    struct Dependency {
        Endpoint source, destination, block_start, block_end;
        bool fixed_start{}, fixed_end{}, enabled{};
        std::uint8_t intermediates{};
        double curve{};
        SccMorphDistributionMode mode{};
        std::optional<std::uint32_t> loop_start, loop_end;
        bool operator==(const Dependency&) const = default;
    };
    const auto dependencies = [](const CompositeTimbre& sound, const CompositeLayer& layer) {
        std::vector<std::size_t> tones;
        for (std::size_t i = 0; i < layer.timbre_automation.size(); ++i)
            if (layer.timbre_automation[i].kind == EnvelopeEventKind::Timbre) tones.push_back(i);
        std::stable_sort(tones.begin(), tones.end(), [&](auto a, auto b) {
            const auto& left = layer.timbre_automation[a];
            const auto& right = layer.timbre_automation[b];
            return left.count != right.count ? left.count < right.count
                : left.after_loop_start < right.after_loop_start;
        });
        const auto numbers = resolveTimbreNumbers(sound);
        const auto endpoint = [&](std::size_t position) {
            const auto& event = layer.timbre_automation[tones[position]];
            Endpoint result{event.target_library_id, event.timbre_pick,
                event.target_library_id ? 0 : event.value, event.count, event.after_loop_start};
            const auto& timeline = layer.envelope_timeline;
            if (timeline.loop_end_count && (event.count > *timeline.loop_end_count
                    || (event.count == *timeline.loop_end_count && !event.after_loop_start)))
                result.loop_phase = 2;
            else if (timeline.loop_start_count && (event.count > *timeline.loop_start_count
                    || (event.count == *timeline.loop_start_count && event.after_loop_start)))
                result.loop_phase = 1;
            const SavedTimbreReference* reference = event.target_library_id
                ? findEmbeddedTimbreSnapshot(sound, event.target_library_id) : nullptr;
            if (!reference && !event.target_library_id && event.timbre_pick == TimbrePick::Library)
                for (const auto& assignment : numbers.assignments) {
                    const auto* candidate = findEmbeddedTimbreSnapshot(sound, assignment.library_id);
                    if (candidate && candidate->source == TimbreSource::Scc
                        && (assignment.number == event.value || candidate->manual_number == event.value)) {
                        reference = candidate;
                        break;
                    }
                }
            if (reference && reference->source == TimbreSource::Scc) result.wave = reference->scc_waveform;
            return result;
        };
        Endpoint base;
        base.pick = TimbrePick::Library;
        if (layer.base_timbre) {
            base.id = layer.base_timbre->library_id; base.wave = layer.base_timbre->scc_waveform;
        } else {
            std::array<std::uint8_t, 32> wave{};
            const auto sine = generateSccPreset(SccWavePreset::Sine, SccHarmonic::One);
            for (std::size_t i = 0; i < wave.size(); ++i) wave[i] = static_cast<std::uint8_t>(sine[i]);
            base.wave = wave;
        }
        std::vector<std::pair<std::size_t, Dependency>> result;
        for (std::size_t i = 0; i < tones.size(); ++i) {
            const auto& settings = layer.timbre_automation[tones[i]].scc_morph;
            if (i == 0 && !settings.enabled) continue;
            std::size_t start = i > 0 ? i - 1 : 0, end = i;
            while (start > 0 && layer.timbre_automation[tones[start]].scc_morph.enabled) --start;
            while (end + 1 < tones.size() && layer.timbre_automation[tones[end + 1]].scc_morph.enabled) ++end;
            const bool starts_at_base = start == 0
                && layer.timbre_automation[tones[0]].scc_morph.enabled;
            Dependency dependency{i > 0 ? endpoint(i - 1) : base, endpoint(i),
                starts_at_base ? base : endpoint(start), endpoint(end),
                start != 0, end + 1 != tones.size(), settings.enabled, settings.intermediate_count,
                settings.curve, settings.distribution_mode};
            const auto boundaryWithin = [&](const std::optional<std::uint32_t>& boundary) {
                return boundary && *boundary >= dependency.source.count && *boundary <= dependency.destination.count
                    ? boundary : std::optional<std::uint32_t>{};
            };
            dependency.loop_start = boundaryWithin(layer.envelope_timeline.loop_start_count);
            dependency.loop_end = boundaryWithin(layer.envelope_timeline.loop_end_count);
            result.push_back({tones[i], std::move(dependency)});
        }
        return result;
    };
    bool changed = false;
    for (std::size_t index = 0; index < after.layers.size(); ++index) {
        auto& layer = after.layers[index];
        if (layer.source != TimbreSource::Scc) continue;
        if (std::none_of(layer.timbre_automation.begin(), layer.timbre_automation.end(),
                [](const auto& event) { return event.scc_morph.explicit_plan.has_value(); })) continue;
        const auto* previous = index < before.layers.size() ? &before.layers[index] : nullptr;
        const bool same_context = previous && previous->source == layer.source
            && previous->volume_envelope.kind == layer.volume_envelope.kind
            && before.scc_morph_algorithm_version == after.scc_morph_algorithm_version;
        const auto old = previous ? dependencies(before, *previous)
            : std::vector<std::pair<std::size_t, Dependency>>{};
        const auto current = dependencies(after, layer);
        for (std::size_t event_index = 0; event_index < layer.timbre_automation.size(); ++event_index) {
            auto& settings = layer.timbre_automation[event_index].scc_morph;
            if (!settings.explicit_plan) continue;
            const auto entry = std::find_if(current.begin(), current.end(),
                [event_index](const auto& candidate) { return candidate.first == event_index; });
            if (same_context && entry != current.end()
                && std::any_of(old.begin(), old.end(), [&](const auto& candidate) {
                    return candidate.second == entry->second;
                })) continue;
            settings.explicit_plan.reset();
            changed = true;
        }
    }
    return changed;
}

namespace {

void insertCurrentAssignmentCatalogItem(
    std::vector<EnvelopeTimbreCatalogItem>& catalog,
    const EnvelopeTimbreChoice& current,
    const mgstc::engine::CompositeTimbre& timbre,
    const juce::String& fallback_name) {
    if (current.pick != mgstc::engine::TimbrePick::Library
        || current.library_id == 0) {
        return;
    }
    for (auto& item : catalog) {
        if (item.library_id == current.library_id) {
            item.current_assignment = true;
            return;
        }
    }
    EnvelopeTimbreCatalogItem item;
    item.library_id = current.library_id;
    item.current_assignment = true;
    if (const auto* snap =
            mgstc::engine::findEmbeddedTimbreSnapshot(
                timbre, current.library_id)) {
        item.name = juce::String::fromUTF8(snap->name.c_str());
        item.revision = snap->revision;
    }
    if (item.name.isEmpty()) {
        item.name = fallback_name.isNotEmpty()
            ? fallback_name
            : juce::String::fromUTF8("オリジナル");
    }
    const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre);
    item.assigned_number = mgstc::engine::assignedNumberForLibraryId(
        numbers, current.library_id);
    catalog.insert(catalog.begin(), std::move(item));
}

[[nodiscard]] juce::String currentAssignmentCaption(
    mgstc::engine::TimbreSource source,
    const EnvelopeTimbreChoice& current,
    const std::vector<EnvelopeTimbreCatalogItem>& catalog) {
    if (source == mgstc::engine::TimbreSource::Scc && current.scc_preset) {
        constexpr std::array<const char*, 6> names{
            "Sine", "Square", "Triangle", "Saw", "Pulse 25%", "Pulse 12.5%"};
        return juce::String::fromUTF8("現在: ")
            + juce::String(names[static_cast<std::size_t>(*current.scc_preset)]);
    }
    if (source == mgstc::engine::TimbreSource::Opll
        && current.pick == mgstc::engine::TimbrePick::OpllRom) {
        return juce::String::fromUTF8("現在: ")
            + opllRomPatchLabel(current.rom_number);
    }
    if (current.pick == mgstc::engine::TimbrePick::Library
        && current.library_id != 0) {
        for (const auto& item : catalog) {
            if (item.library_id == current.library_id) {
                auto name = item.name;
                if (name.isEmpty()) {
                    name = juce::String::fromUTF8("オリジナル");
                }
                return juce::String::fromUTF8("現在: ") + name;
            }
        }
        return juce::String::fromUTF8("現在: オリジナル");
    }
    return juce::String::fromUTF8("現在: （なし）");
}


class EnvelopeNumericEntryContent final : public juce::Component {
public:
    EnvelopeNumericEntryContent(
        const juce::String& prompt,
        const juce::String& initial,
        std::function<void(int)> on_ok,
        bool show_automatic = false,
        bool initial_automatic = false,
        std::function<void(int, bool, bool)> on_volume_ok = {},
        bool initial_precise = false)
        : on_ok_(std::move(on_ok)),
          on_volume_ok_(std::move(on_volume_ok)),
          show_automatic_(show_automatic),
          initial_automatic_(initial_automatic),
          initial_precise_(initial_precise) {
        hint_.setText(prompt, juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        addAndMakeVisible(hint_);
        UiFonts::styleBodyField(editor_);
        editor_.setInputRestrictions(8, "-0123456789");
        editor_.setText(initial, false);
        editor_.setJustification(juce::Justification::centred);
        editor_.onReturnKey = [this] { accept(); };
        addAndMakeVisible(editor_);
        if (show_automatic_) {
            automatic_.setButtonText(
                juce::String::fromUTF8("自動変化"));
            automatic_.setLookAndFeel(&switch_look_and_feel_);
            automatic_.setToggleState(
                initial_automatic_, juce::dontSendNotification);
            automatic_.setEnabled(true);
            automatic_.onClick = [this] { syncPreciseEnabled(); };
            addAndMakeVisible(automatic_);
            precise_.setButtonText(juce::String::fromUTF8("詳細"));
            precise_.setLookAndFeel(&switch_look_and_feel_);
            precise_.setToggleState(
                initial_precise_ && initial_automatic_,
                juce::dontSendNotification);
            precise_.setTooltip(juce::String::fromUTF8(
                "途中のコマンドがあっても、無いときと同じ音量段で変化します。"
                "OFFは従来どおり補間で=を分割します。ONは@eが長くなりやすいです"));
            addAndMakeVisible(precise_);
            syncPreciseEnabled();
        }
        ok_.setButtonText(juce::String::fromUTF8("設定"));
        ok_.onClick = [this] { accept(); };
        addAndMakeVisible(ok_);
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] { closeHost(0); };
        addAndMakeVisible(cancel_);
        setSize(
            UiLayout::libraryWidth,
            UiLayout::panelPad * 2
                + UiLayout::fieldH * 2
                + UiLayout::sm
                + (show_automatic_ ? UiLayout::fieldH + UiLayout::sm : 0)
                + UiLayout::textButtonH
                + UiLayout::sm);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        editor_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        if (show_automatic_) {
            auto row = area.removeFromTop(fieldH);
            const int switch_w = juce::jmax(
                1, (row.getWidth() - controlGap) / 2);
            automatic_.setBounds(row.removeFromLeft(switch_w));
            row.removeFromLeft(controlGap);
            precise_.setBounds(row);
            area.removeFromTop(sm);
        }
        auto buttons = area.removeFromTop(textButtonH);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons);
    }

    void visibilityChanged() override {
        if (isShowing()) {
            editor_.grabKeyboardFocus();
        }
    }

private:
    void syncPreciseEnabled() {
        const bool on = automatic_.getToggleState();
        precise_.setEnabled(on);
        if (!on) {
            precise_.setToggleState(false, juce::dontSendNotification);
        }
    }

    void accept() {
        if (on_volume_ok_) {
            const bool automatic_on = automatic_.getToggleState();
            on_volume_ok_(
                editor_.getText().getIntValue(),
                automatic_on,
                automatic_on && precise_.getToggleState());
        } else if (on_ok_) {
            on_ok_(editor_.getText().getIntValue());
        }
        closeHost(1);
    }

    void closeHost(int result) {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(result);
        }
    }

    std::function<void(int)> on_ok_;
    std::function<void(int, bool, bool)> on_volume_ok_;
    bool show_automatic_{};
    bool initial_automatic_{};
    bool initial_precise_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label hint_;
    juce::TextEditor editor_;
    juce::ToggleButton automatic_;
    juce::ToggleButton precise_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

// Only tall morph-enabled pickers use this container. Keep keyboard focus
// visible while retaining the existing picker controls and modal ownership.
class ScrollableTimbrePickerContent final : public juce::Component,
                                           private juce::FocusChangeListener {
public:
    ScrollableTimbrePickerContent(juce::Component* content, int maximum_height) {
        viewport_.setViewedComponent(content, true);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        setSize(content->getWidth() + viewport_.getScrollBarThickness(),
                juce::jmin(content->getHeight(), maximum_height));
        juce::Desktop::getInstance().addFocusChangeListener(this);
    }

    ~ScrollableTimbrePickerContent() override {
        juce::Desktop::getInstance().removeFocusChangeListener(this);
    }

    void resized() override { viewport_.setBounds(getLocalBounds()); }

    [[nodiscard]] bool verifyGeometry() {
        auto* content = viewport_.getViewedComponent();
        if (content == nullptr || content->getHeight() <= viewport_.getMaximumVisibleHeight()
            || content->getWidth() > viewport_.getMaximumVisibleWidth()) return false;
        viewport_.setViewPosition(0, content->getHeight());
        return viewport_.getViewPositionY() > 0
            && viewport_.getViewArea().getBottom() == content->getHeight();
    }

private:
    void globalFocusChanged(juce::Component* focused) override {
        auto* content = viewport_.getViewedComponent();
        if (focused == nullptr || content == nullptr || !content->isParentOf(focused)) return;
        const auto target = content->getLocalArea(focused, focused->getLocalBounds());
        const auto visible = viewport_.getViewArea();
        if (target.getY() < visible.getY()) viewport_.setViewPosition(0, target.getY());
        else if (target.getBottom() > visible.getBottom())
            viewport_.setViewPosition(0, target.getBottom() - visible.getHeight());
    }

    juce::Viewport viewport_;
};

// Radio semantics retain JUCE's keyboard/accessibility handling. The circle
// uses the same font, scaled geometry, hover and enabled-state tokens as chrome.
class MorphDistributionButton final : public juce::ToggleButton {
    void paintButton(juce::Graphics& graphics, bool highlighted, bool) override {
        auto area = getLocalBounds().reduced(UiLayout::controlGap, 0);
        const int diameter = UiLayout::switchTrackH;
        const auto circle = juce::Rectangle<float>(static_cast<float>(area.getX()),
            static_cast<float>(area.getCentreY()) - static_cast<float>(diameter) * 0.5F,
            static_cast<float>(diameter), static_cast<float>(diameter));
        const auto text = findColour(juce::ToggleButton::textColourId);
        graphics.setColour((highlighted || hasKeyboardFocus(true) ? uiHoverAccent() : text)
            .withMultipliedAlpha(isEnabled() ? 1.0F : 0.5F));
        graphics.drawEllipse(circle, 1.5F);
        if (getToggleState()) graphics.fillEllipse(circle.reduced(static_cast<float>(UiLayout::xs)));
        area.removeFromLeft(diameter + UiLayout::controlGap);
        graphics.setFont(UiFonts::body());
        graphics.drawFittedText(getButtonText(), area, juce::Justification::centredLeft, 1);
        if (hasKeyboardFocus(true)) {
            graphics.setColour(uiHoverAccent());
            graphics.drawRect(getLocalBounds().toFloat().reduced(1.0F), 1.5F);
        }
    }
};

class EnvelopeTimbrePickContent final : public juce::Component {
public:
    using MorphSettings = decltype(mgstc::engine::EnvelopeEvent{}.scc_morph);
    struct MorphChangeResult {
        bool accepted{true};
        juce::String error;
    };

    void configureMorph(
        MorphSettings settings, const juce::String& start,
        const juce::String& finish, int maximum_intermediates,
        std::function<MorphChangeResult(MorphSettings, bool)> on_change,
        bool focus_morph) {
        morph_settings_ = settings;
        morph_change_ = std::move(on_change);
        morph_available_ = maximum_intermediates >= 0;
        morph_enabled_.setButtonText(juce::String::fromUTF8("前の音色からモーフィング"));
        morph_enabled_.setLookAndFeel(&switch_look_and_feel_);
        morph_enabled_.setToggleState(settings.enabled, juce::dontSendNotification);
        morph_enabled_.setEnabled(morph_available_ || settings.enabled);
        morph_enabled_.setExplicitFocusOrder(220);
        morph_enabled_.onClick = [this] { changeMorph(true); };
        addAndMakeVisible(morph_enabled_);
        morph_endpoints_.setText(
            juce::String::fromUTF8("開始音色: ") + start
                + juce::String::fromUTF8("   終了音色: ") + finish,
            juce::dontSendNotification);
        morph_endpoints_.setFont(UiFonts::body());
        morph_endpoints_.setTooltip(morph_endpoints_.getText());
        addAndMakeVisible(morph_endpoints_);
        morph_distribution_label_.setText(juce::String::fromUTF8("変化方式"), juce::dontSendNotification);
        morph_distribution_label_.setFont(UiFonts::body());
        addAndMakeVisible(morph_distribution_label_);
        const std::array<juce::String, 3> distribution_names{
            juce::String::fromUTF8("適応型（標準）"),
            juce::String::fromUTF8("時間配分型"),
            juce::String::fromUTF8("音色配分型")};
        for (std::size_t index = 0; index < morph_distribution_.size(); ++index) {
            auto& button = morph_distribution_[index];
            button.setButtonText(distribution_names[index]);
            button.setRadioGroupId(1);
            button.setClickingTogglesState(true);
            button.setWantsKeyboardFocus(true);
            button.setExplicitFocusOrder(221 + static_cast<int>(index));
            button.setTooltip(juce::String::fromUTF8(
                "方式・カーブ・音色数や区間の端点を編集すると、WAV確定配置を解除して通常のモーフ配置を生成します"));
            button.onClick = [this] { changeMorph(true); };
            addAndMakeVisible(button);
        }
        syncMorphDistribution();
        morph_count_label_.setText(juce::String::fromUTF8("中間音色数"), juce::dontSendNotification);
        morph_count_label_.setFont(UiFonts::body());
        addAndMakeVisible(morph_count_label_);
        morph_count_.setSliderStyle(juce::Slider::LinearBar);
        // Keep an impossible loaded N visible until the user explicitly edits
        // it; changing mode must never silently shorten a morph's tone count.
        morph_count_max_ = juce::jmax(juce::jmax(0, maximum_intermediates),
                                    static_cast<int>(settings.intermediate_count));
        morph_count_.setRange(0, juce::jmax(1, morph_count_max_), 1);
        morph_count_.setNumDecimalPlacesToDisplay(0);
        morph_count_.setValue(settings.intermediate_count, juce::dontSendNotification);
        morph_count_.setExplicitFocusOrder(224);
        morph_count_.setWantsKeyboardFocus(true);
        morph_count_.onDragStart = [this] { morph_dragging_ = true; morph_drag_rejected_ = false; };
        morph_count_.onValueChange = [this] { changeMorph(!morph_dragging_); };
        morph_count_.onDragEnd = [this] { finishMorphDrag(); };
        addAndMakeVisible(morph_count_);
        morph_curve_label_.setText(juce::String::fromUTF8("変化速度"), juce::dontSendNotification);
        morph_curve_label_.setFont(UiFonts::body());
        addAndMakeVisible(morph_curve_label_);
        morph_curve_.setSliderStyle(juce::Slider::LinearHorizontal);
        morph_curve_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        morph_curve_.setRange(-1, 1, 0.001);
        morph_curve_.setValue(-std::log(settings.curve) / std::log(mgstc::engine::kSccMorphGammaMax), juce::dontSendNotification);
        morph_curve_.setTooltip(juce::String::fromUTF8("左: 前半速い / 中央: 均一 / 右: 後半速い"));
        morph_curve_.setExplicitFocusOrder(225);
        morph_curve_.setWantsKeyboardFocus(true);
        morph_curve_.onDragStart = [this] { morph_dragging_ = true; morph_drag_rejected_ = false; };
        morph_curve_.onValueChange = [this] { changeMorph(!morph_dragging_); };
        morph_curve_.onDragEnd = [this] { finishMorphDrag(); };
        addAndMakeVisible(morph_curve_);
        morph_front_.setText(juce::String::fromUTF8("前半速い"), juce::dontSendNotification);
        morph_uniform_.setText(juce::String::fromUTF8("均一"), juce::dontSendNotification);
        morph_back_.setText(juce::String::fromUTF8("後半速い"), juce::dontSendNotification);
        for (auto* label : {&morph_front_, &morph_uniform_, &morph_back_, &morph_error_}) {
            label->setFont(UiFonts::body());
            addAndMakeVisible(*label);
        }
        morph_uniform_.setJustificationType(juce::Justification::centredRight);
        if (!morph_available_) morph_error_.setText(
            juce::String::fromUTF8("開始音色より後のカウントを選んでください。同じカウントではモーフィングできません。"),
            juce::dontSendNotification);
        syncMorphEnabled();
        setSize(getWidth(), getHeight() + (UiLayout::fieldH + UiLayout::sm) * 10);
        if (focus_morph) juce::MessageManager::callAsync(
            [safe = juce::Component::SafePointer<EnvelopeTimbrePickContent>(this)] {
                if (safe != nullptr && safe->morph_enabled_.isEnabled())
                    safe->morph_enabled_.grabKeyboardFocus();
            });
    }

    [[nodiscard]] juce::String verifyMorphControls() {
        if (morph_count_.getExplicitFocusOrder() != 224 || morph_curve_.getExplicitFocusOrder() != 225)
            return "unexpected morph control Tab order";
        if (morph_settings_.distribution_mode != mgstc::engine::SccMorphDistributionMode::AdaptiveDistribution
            || !morph_distribution_[0].getToggleState())
            return "new morph interval does not default to Adaptive";
        for (std::size_t index = 0; index < morph_distribution_.size(); ++index) {
            const auto& button = morph_distribution_[index];
            if (button.getExplicitFocusOrder() != 221 + static_cast<int>(index))
                return "unexpected distribution mode Tab order";
            if (button.isEnabled()) return "Morph OFF leaves distribution mode enabled";
            if (button.getBounds().isEmpty() || !getLocalBounds().contains(button.getBounds()))
                return "distribution mode lies outside picker bounds";
        }
        if (morph_count_.isEnabled()) return "Morph OFF leaves intermediate count enabled";
        if (morph_curve_.isEnabled()) return "Morph OFF leaves curve enabled";
        if (morph_curve_.getWidth() < UiLayout::libraryButtonMinW * 2) return "curve track is too narrow";
        for (const auto* control : std::initializer_list<const juce::Component*>{
                 &morph_enabled_, &morph_endpoints_, &morph_distribution_label_,
                 &morph_count_, &morph_curve_, &morph_error_})
            if (control->getBounds().isEmpty() || !getLocalBounds().contains(control->getBounds()))
                return "morph control lies outside picker bounds";
        morph_enabled_.setToggleState(true, juce::dontSendNotification);
        morph_enabled_.onClick();
        if (!morph_count_.isEnabled()) return "Morph ON leaves intermediate count disabled";
        if (!morph_curve_.isEnabled()) return "Morph ON leaves curve disabled";
        for (std::size_t index = 0; index < morph_distribution_.size(); ++index) {
            auto& button = morph_distribution_[index];
            if (!button.isEnabled() || !button.getWantsKeyboardFocus())
                return "enabled distribution mode cannot receive keyboard focus";
            button.setToggleState(true, juce::dontSendNotification);
            button.onClick();
            if (morph_settings_.distribution_mode != distributionMode(index))
                return "distribution mode button did not apply interval state";
            for (std::size_t other = 0; other < morph_distribution_.size(); ++other)
                if (morph_distribution_[other].getToggleState() != (other == index))
                    return "distribution mode radio selection is not exclusive";
        }
        // JUCE getWantsKeyboardFocus() returns false for disabled components;
        // test focusability only once Morph ON has enabled both sliders.
        if (!morph_count_.getWantsKeyboardFocus()) return "enabled intermediate count cannot receive keyboard focus";
        if (!morph_curve_.getWantsKeyboardFocus()) return "enabled curve cannot receive keyboard focus";
        morph_count_.setValue(0, juce::sendNotificationSync);
        if (!morph_count_.keyPressed(juce::KeyPress(juce::KeyPress::rightKey))) return "intermediate count ignores right arrow";
        if (morph_settings_.intermediate_count != 1) return "right arrow did not apply one intermediate tone";
        morph_curve_.setValue(-1, juce::sendNotificationSync);
        if (morph_settings_.curve != mgstc::engine::kSccMorphGammaMax) return "left curve endpoint is not gamma maximum";
        morph_curve_.setValue(0, juce::sendNotificationSync);
        if (morph_settings_.curve != 1) return "centre curve is not uniform";
        morph_curve_.setValue(1, juce::sendNotificationSync);
        if (morph_settings_.curve != mgstc::engine::kSccMorphGammaMin) return "right curve endpoint is not back-loaded";
        morph_enabled_.setToggleState(false, juce::dontSendNotification);
        morph_enabled_.onClick();
        if (morph_count_.isEnabled() || morph_curve_.isEnabled()) return "Morph OFF did not disable settings after editing";
        for (const auto& button : morph_distribution_)
            if (button.isEnabled()) return "Morph OFF did not disable distribution mode after editing";
        return {};
    }

    void completeMorphChange(MorphSettings settings, const MorphChangeResult& result) {
        morph_settings_ = std::move(settings);
        morph_enabled_.setToggleState(morph_settings_.enabled, juce::dontSendNotification);
        morph_count_.setValue(morph_settings_.intermediate_count, juce::dontSendNotification);
        morph_curve_.setValue(-std::log(morph_settings_.curve) / std::log(mgstc::engine::kSccMorphGammaMax), juce::dontSendNotification);
        syncMorphDistribution();
        morph_error_.setText(result.error, juce::dontSendNotification);
        morph_error_.setTooltip(result.error);
        syncMorphEnabled();
    }

    [[nodiscard]] bool verifyUnavailableMorphCount() {
        if (morph_count_.getValue() != morph_settings_.intermediate_count) return false;
        const auto count = morph_settings_.intermediate_count;
        morph_distribution_[1].setToggleState(true, juce::dontSendNotification);
        morph_distribution_[1].onClick();
        return morph_settings_.intermediate_count == count;
    }

    EnvelopeTimbrePickContent(
        mgstc::engine::TimbreSource source,
        const std::vector<EnvelopeTimbreCatalogItem>& catalog,
        const EnvelopeTimbreChoice& current,
        std::function<void(EnvelopeTimbreChoice)> on_choose,
        std::function<void(std::uint64_t)> on_edit,
        std::function<void()> on_remove,
        std::function<void()> on_manage = {},
        std::function<bool(std::uint64_t, juce::String)> on_rename = {},
        std::function<bool(std::uint64_t, std::vector<std::string>)>
            on_set_tags = {},
        std::function<bool(std::uint64_t, juce::String)> on_set_memo = {},
        bool layer_base = false,
        mgstc::app::EditorSession* performance_session = nullptr,
        std::function<void(std::uint8_t)> on_note_on = {},
        std::function<void(std::uint8_t)> on_note_off = {},
        std::function<void()> on_audition = {},
        std::function<std::optional<std::uint64_t>()> on_blank = {},
        std::function<void(EnvelopeTimbreChoice, std::uint8_t)> on_perform = {},
        std::function<void()> on_performance_end = {},
        std::function<void(EnvelopeTimbreChoice)> on_selection_preview = {})
        : source_(source),
          catalog_(catalog),
          current_(current),
          on_choose_(std::move(on_choose)),
          on_edit_(std::move(on_edit)),
          on_remove_(std::move(on_remove)),
          on_manage_(std::move(on_manage)),
          on_rename_(std::move(on_rename)),
          on_set_tags_(std::move(on_set_tags)),
          on_set_memo_(std::move(on_set_memo)),
          on_note_on_(std::move(on_note_on)),
          on_note_off_(std::move(on_note_off)),
          on_audition_(std::move(on_audition)),
          on_blank_(std::move(on_blank)),
          on_performance_end_(std::move(on_performance_end)),
          on_selection_preview_(std::move(on_selection_preview)),
          layer_base_(layer_base) {
        current_label_.setText(
            currentAssignmentCaption(source_, current_, catalog_),
            juce::dontSendNotification);
        current_label_.setFont(UiFonts::body());
        current_label_.setTooltip(juce::String::fromUTF8(
            "このステップ（またはチャンネル）にいま割り当てられている音色"));
        addAndMakeVisible(current_label_);
        preset_label_.setText(
            juce::String::fromUTF8("プリセット音色"),
            juce::dontSendNotification);
        preset_label_.setFont(UiFonts::body());
        addAndMakeVisible(preset_label_);
        preset_.setTextWhenNothingSelected(
            juce::String::fromUTF8("プリセットを選択"));
        if (source_ == mgstc::engine::TimbreSource::Opll) {
            for (int rom = 0; rom < 15; ++rom) {
                preset_.addItem(opllRomPatchLabel(rom), rom + 1);
            }
            if (current_.pick == mgstc::engine::TimbrePick::OpllRom) {
                preset_.setSelectedId(
                    current_.rom_number + 1, juce::dontSendNotification);
            }
        } else if (source_ == mgstc::engine::TimbreSource::Scc) {
            constexpr std::array<const char*, 6> names{
                "Sine", "Square", "Triangle", "Saw",
                "Pulse 25%", "Pulse 12.5%"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                preset_.addItem(
                    juce::String(names[index]),
                    static_cast<int>(index) + 1);
            }
        }
        preset_.setTooltip(
            juce::String::fromUTF8(
                source_ == mgstc::engine::TimbreSource::Opll
                    ? (layer_base_
                           ? "OPLL ROM（@0～@14）。読込でこのチャンネルの基本音色に設定します"
                           : "OPLL ROM（@0～@14）。読込でこのカウントの@に設定します")
                    : "SCC波形プリセット。読込でこのカウントの所有音色として設定します"));
        preset_.onChange = [this] {
            if (preset_.getSelectedId() > 0) {
                list_.setSelectedId(0, juce::dontSendNotification);
            }
            syncDetailFromSelection();
            liveApplySelection();
            previewSelection();
        };
        addAndMakeVisible(preset_);

        edit_.setButtonText(juce::String::fromUTF8("音色を編集"));
        edit_.setTooltip(juce::String::fromUTF8(
            "現在の割り当て、または一覧で選んだライブラリ／オリジナル音色を"
            "総合音色編集ウィンドウで開きます。ROMは開けません"));
        edit_.onClick = [this] {
            std::optional<std::uint64_t> id = selectedLibraryId();
            if (!id
                && current_.pick == mgstc::engine::TimbrePick::Library
                && current_.library_id != 0) {
                id = current_.library_id;
            }
            if (!id || !on_edit_) {
                return;
            }
            auto edit = on_edit_;
            const auto library_id = *id;
            closeHost();
            juce::MessageManager::callAsync(
                [edit, library_id] { edit(library_id); });
        };
        addAndMakeVisible(edit_);
        if (on_blank_) {
            blank_.setButtonText(
                juce::String::fromUTF8("オリジナルを追加"));
            blank_.setTooltip(juce::String::fromUTF8(
                layer_base_ ? "ライブラリに無い素のオリジナルを、このチャンネルの基本音色にします"
                    : "ライブラリに保存せず、このステップに素のオリジナル音色を設定します"));
            blank_.onClick = [this] { addBlankOriginal(); };
            addAndMakeVisible(blank_);
        }

        filter_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("名前・タグ・メモを検索"),
            juce::Colour(0xFF7F8993));
        filter_.onTextChange = [this] { refreshList(); };
        UiFonts::styleBodyField(filter_);
        addAndMakeVisible(filter_);
        tag_filter_.setButtonText(
            juce::String::fromUTF8("タグで絞り込み"));
        tag_filter_.onClick = [this] { showTagFilter(); };
        addAndMakeVisible(tag_filter_);
        tag_manage_.setButtonText(
            juce::String::fromUTF8("ライブラリ管理"));
        tag_manage_.setTooltip(juce::String::fromUTF8(
            "音色ライブラリの一覧・複製・削除・タグ管理を行います"));
        tag_manage_.onClick = [this] {
            if (!on_manage_) {
                return;
            }
            if (layer_base_) {
                auto manage = on_manage_;
                closeHost();
                juce::MessageManager::callAsync(
                    [manage] { manage(); });
                return;
            }
            on_manage_();
        };
        tag_manage_.setEnabled(on_manage_ != nullptr);
        addAndMakeVisible(tag_manage_);
        favorite_only_.setButtonText(juce::String::fromUTF8("★のみ"));
        favorite_only_.setLookAndFeel(&switch_look_and_feel_);
        favorite_only_.onClick = [this] { refreshList(); };
        addAndMakeVisible(favorite_only_);
        ab_.setButtonText("A/B");
        ab_.setTooltip(juce::String::fromUTF8(
            "現在の@割当(A)と一覧で選んだ保存音色(B)を切り替えます"));
        ab_.onClick = [this] { toggleAb(); };
        addAndMakeVisible(ab_);
        sort_.addItem(juce::String::fromUTF8("★優先"), 1);
        sort_.addItem(juce::String::fromUTF8("最近使った順"), 2);
        sort_.addItem(juce::String::fromUTF8("更新日時順"), 3);
        sort_.addItem(juce::String::fromUTF8("名前順"), 4);
        sort_.setSelectedId(1, juce::dontSendNotification);
        sort_.onChange = [this] { refreshList(); };
        addAndMakeVisible(sort_);

        list_.setTextWhenNothingSelected(
            juce::String::fromUTF8("保存済み音色を選択"));
        list_.onChange = [this] {
            if (list_.getSelectedId() > 0) {
                preset_.setSelectedId(0, juce::dontSendNotification);
            }
            syncDetailFromSelection();
            liveApplySelection();
            previewSelection();
        };
        addAndMakeVisible(list_);
        load_.setButtonText(juce::String::fromUTF8("読込"));
        load_.setTooltip(juce::String::fromUTF8(
            layer_base_
                ? "選択したプリセットまたはライブラリ音色をこのチャンネルの基本音色に設定します"
                : "選択したプリセットまたはライブラリ音色をこのカウントの@に設定します"));
        load_.onClick = [this] {
            if (auto choice = selectedChoice()) {
                on_choose_(*choice);
                closeHost();
            }
        };
        addAndMakeVisible(load_);
        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.setTooltip(juce::String::fromUTF8(
            "このカウントの@イベントを削除します"));
        remove_.onClick = [this] {
            if (on_remove_) {
                on_remove_();
            }
            closeHost();
        };
        addAndMakeVisible(remove_);
        remove_.setVisible(!layer_base_);

        name_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("音色名"),
            juce::Colour(0xFF7F8993));
        UiFonts::styleBodyField(name_);
        addAndMakeVisible(name_);
        rename_.setButtonText(juce::String::fromUTF8("名前変更"));
        rename_.onClick = [this] { renameSelected(); };
        addAndMakeVisible(rename_);
        tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        tags_.onClick = [this] { editSelectedTags(); };
        addAndMakeVisible(tags_);
        memo_.setTextToShowWhenEmpty(
            juce::String::fromUTF8("メモ"),
            juce::Colour(0xFF7F8993));
        memo_.setMultiLine(true);
        memo_.setReturnKeyStartsNewLine(true);
        UiFonts::styleBodyField(memo_);
        memo_.onFocusLost = [this] { commitMemo(); };
        addAndMakeVisible(memo_);

        if (performance_session != nullptr) {
            keyboard_ = std::make_unique<mgstc::app::PerformanceKeyboard>(
                *performance_session);
            keyboard_->setHeadlessMode(true);
            keyboard_->setCallbacks(
                [this, perform = std::move(on_perform)](std::uint8_t note) {
                    if (perform) {
                        if (auto choice = selectedChoice()) perform(*choice, note);
                        else perform(current_, note);
                    } else if (on_note_on_) on_note_on_(note);
                }, on_note_off_);
            keyboard_->setSuppressPcInputCallback(
                [this] { return textEntryHasFocusWithin(*this); });
            addChildComponent(*keyboard_);
            setWantsKeyboardFocus(true);
        }
        if (layer_base_) {
            preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
            preview_.onClick = [this] {
                if (on_audition_) on_audition_();
            };
            addAndMakeVisible(preview_);
        }

        refreshList();
        syncDetailFromSelection();
        live_apply_armed_ = true;
        const int extra = blank_.isVisible() ? UiLayout::textButtonH + UiLayout::sm : 0;
        setSize(
            UiLayout::compositeTimbrePickW,
            UiLayout::compositeTimbrePickH + extra);
    }

    bool verifyPresetSelection() {
        for (int id = 1; id <= 6; ++id) {
            preset_.setSelectedId(id, juce::dontSendNotification);
            preset_.onChange();
            const auto choice = selectedChoice();
            if (!choice || !choice->scc_preset || !load_.isEnabled()
                || static_cast<int>(*choice->scc_preset) != id - 1
                || list_.getSelectedId() != 0) return false;
        }
        load_.onClick();
        if (layer_base_ && preview_.onClick) preview_.onClick();
        return true;
    }

    ~EnvelopeTimbrePickContent() override {
        // Release held PC/MIDI notes before restoring the authored program.
        if (keyboard_) keyboard_->allNotesOff();
        keyboard_.reset();
        if (on_performance_end_) on_performance_end_();
        favorite_only_.setLookAndFeel(nullptr);
        morph_enabled_.setLookAndFeel(nullptr);
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (keyboard_ != nullptr
            && keyboard_->shouldConsumeKeyPress(key)) {
            keyboard_->pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        if (keyboard_ != nullptr) {
            keyboard_->pollPerformanceInput();
        }
        return false;
    }

    void visibilityChanged() override {
        if (keyboard_ != nullptr && isShowing()) {
            grabKeyboardFocus();
        }
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        current_label_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        if (morph_change_) {
            morph_enabled_.setBounds(area.removeFromTop(fieldH));
            area.removeFromTop(sm);
            morph_endpoints_.setBounds(area.removeFromTop(fieldH));
            area.removeFromTop(sm);
            morph_distribution_label_.setBounds(area.removeFromTop(fieldH));
            area.removeFromTop(sm);
            for (auto& button : morph_distribution_) {
                button.setBounds(area.removeFromTop(fieldH));
                area.removeFromTop(sm);
            }
            auto count_row = area.removeFromTop(fieldH);
            morph_count_label_.setBounds(count_row.removeFromLeft(compositeInspectorLabelW));
            morph_count_.setBounds(count_row);
            area.removeFromTop(sm);
            auto curve_row = area.removeFromTop(fieldH);
            morph_curve_label_.setBounds(curve_row.removeFromLeft(compositeInspectorLabelW));
            morph_front_.setBounds(curve_row.removeFromLeft(libraryButtonMinW));
            morph_back_.setBounds(curve_row.removeFromRight(libraryButtonMinW));
            morph_uniform_.setBounds(curve_row);
            area.removeFromTop(sm);
            morph_curve_.setBounds(area.removeFromTop(fieldH));
            area.removeFromTop(sm);
            morph_error_.setBounds(area.removeFromTop(fieldH));
            area.removeFromTop(sm);
        }
        auto preset_row = area.removeFromTop(fieldH);
        preset_label_.setBounds(preset_row.removeFromLeft(UiScale::sx(108)));
        preset_row.removeFromLeft(controlGap);
        preset_.setBounds(preset_row);
        area.removeFromTop(sm);
        edit_.setBounds(area.removeFromTop(textButtonH));
        if (blank_.isVisible()) {
            area.removeFromTop(sm);
            blank_.setBounds(area.removeFromTop(textButtonH));
        }
        area.removeFromTop(sm);
        if (layer_base_) {
            preview_.setBounds(area.removeFromBottom(textButtonH));
            area.removeFromBottom(sm);
        }
        layoutLibraryBrowserChrome(
            area,
            LibraryBrowserChromeWidgets{
                filter_,
                tag_filter_,
                tag_manage_,
                favorite_only_,
                ab_,
                sort_,
                list_,
                load_,
                remove_,
                name_,
                rename_,
                tags_,
                &memo_});
    }

private:
    [[nodiscard]] static mgstc::engine::SccMorphDistributionMode distributionMode(std::size_t index) {
        constexpr std::array modes{
            mgstc::engine::SccMorphDistributionMode::AdaptiveDistribution,
            mgstc::engine::SccMorphDistributionMode::TimeDistribution,
            mgstc::engine::SccMorphDistributionMode::ToneDistribution};
        return modes[index];
    }

    void syncMorphDistribution() {
        for (std::size_t index = 0; index < morph_distribution_.size(); ++index)
            morph_distribution_[index].setToggleState(
                morph_settings_.distribution_mode == distributionMode(index), juce::dontSendNotification);
    }

    void finishMorphDrag() {
        const bool rejected = morph_drag_rejected_;
        const auto error = morph_error_.getText();
        morph_dragging_ = false;
        changeMorph(true);
        if (rejected) {
            morph_error_.setText(error, juce::dontSendNotification);
            morph_error_.setTooltip(error);
        }
    }

    void syncMorphEnabled() {
        morph_enabled_.setEnabled(morph_available_ || morph_enabled_.getToggleState());
        morph_count_.setEnabled(morph_available_ && morph_enabled_.getToggleState() && morph_count_max_ > 0);
        morph_curve_.setEnabled(morph_available_ && morph_enabled_.getToggleState());
        for (auto& button : morph_distribution_)
            button.setEnabled(morph_available_ && morph_enabled_.getToggleState());
    }

    void changeMorph(bool commit) {
        auto settings = morph_settings_;
        settings.enabled = morph_enabled_.getToggleState();
        settings.intermediate_count = static_cast<std::uint8_t>(juce::jlimit(
            0, morph_count_max_, juce::roundToInt(morph_count_.getValue())));
        settings.curve = std::pow(mgstc::engine::kSccMorphGammaMax, -morph_curve_.getValue());
        for (std::size_t index = 0; index < morph_distribution_.size(); ++index)
            if (morph_distribution_[index].getToggleState()) settings.distribution_mode = distributionMode(index);
        if (settings.distribution_mode != morph_settings_.distribution_mode
            || settings.intermediate_count != morph_settings_.intermediate_count
            || settings.curve != morph_settings_.curve)
            settings.explicit_plan.reset();
        const auto result = morph_change_ ? morph_change_(settings, commit) : MorphChangeResult{};
        const auto& error = result.error;
        if (morph_dragging_) morph_drag_rejected_ = !result.accepted;
        morph_error_.setText(error, juce::dontSendNotification);
        morph_error_.setTooltip(error);
        if (result.accepted) morph_settings_ = settings;
        else {
            morph_enabled_.setToggleState(morph_settings_.enabled, juce::dontSendNotification);
            morph_count_.setValue(morph_settings_.intermediate_count, juce::dontSendNotification);
            morph_curve_.setValue(-std::log(morph_settings_.curve) / std::log(mgstc::engine::kSccMorphGammaMax), juce::dontSendNotification);
            syncMorphDistribution();
        }
        syncMorphEnabled();
    }

    void closeHost() {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(1);
        }
    }

    [[nodiscard]] std::vector<const EnvelopeTimbreCatalogItem*>
    filteredCatalog() const {
        std::vector<const EnvelopeTimbreCatalogItem*> pinned;
        std::vector<const EnvelopeTimbreCatalogItem*> ordered;
        for (const auto& item : catalog_) {
            if (item.current_assignment) {
                pinned.push_back(&item);
                continue;
            }
            if (!favorite_only_.getToggleState() || item.favorite) {
                ordered.push_back(&item);
            }
        }
        const int sort_mode = sort_.getSelectedId();
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [sort_mode](const auto* left, const auto* right) {
                if (left->favorite != right->favorite) {
                    return left->favorite;
                }
                if (sort_mode == 2
                    && left->last_used_unix_seconds
                        != right->last_used_unix_seconds) {
                    return left->last_used_unix_seconds
                        > right->last_used_unix_seconds;
                }
                if (sort_mode == 3
                    && left->updated_unix_seconds
                        != right->updated_unix_seconds) {
                    return left->updated_unix_seconds
                        > right->updated_unix_seconds;
                }
                if (sort_mode == 4) {
                    return left->name < right->name;
                }
                return false;
            });
        const auto filter = filter_.getText().trim();
        std::vector<const EnvelopeTimbreCatalogItem*> visible = pinned;
        for (const auto* item : ordered) {
            if (filter.isNotEmpty()) {
                const auto tags = juce::String::fromUTF8(
                    mgstc::engine::serializeTimbreTags(item->tags).c_str());
                if (!item->name.containsIgnoreCase(filter)
                    && !tags.containsIgnoreCase(filter)
                    && !item->memo.containsIgnoreCase(filter)) {
                    continue;
                }
            }
            if (!mgstc::engine::containsAllTimbreTags(
                    item->tags, filter_tags_)) {
                continue;
            }
            visible.push_back(item);
        }
        return visible;
    }

    void refreshList() {
        const auto previous = selectedLibraryId();
        list_.clear(juce::dontSendNotification);
        visible_ids_.clear();
        int item_id = 1;
        int selected = 0;
        for (const auto* item : filteredCatalog()) {
            visible_ids_.push_back(item->library_id);
            list_.addItem(envelopeTimbreCatalogLabel(*item), item_id);
            if ((previous && *previous == item->library_id)
                || (!previous
                    && current_.library_id == item->library_id
                    && current_.pick
                        == mgstc::engine::TimbrePick::Library)) {
                selected = item_id;
            }
            ++item_id;
        }
        list_.setSelectedId(selected, juce::dontSendNotification);
        ab_.setEnabled(
            current_.library_id != 0 || selectedLibraryId().has_value());
        syncDetailFromSelection();
    }

    void syncDetailFromSelection() {
        syncing_ = true;
        const auto* item = selectedCatalogItem();
        if (item != nullptr) {
            name_.setText(item->name, juce::dontSendNotification);
            memo_.setText(item->memo, juce::dontSendNotification);
            tags_.setButtonText(
                tagSelectionSummary(
                    item->tags,
                    juce::String::fromUTF8("タグを選択")));
        } else {
            name_.clear();
            memo_.clear();
            tags_.setButtonText(juce::String::fromUTF8("タグを選択"));
        }
        const bool library_selected = item != nullptr;
        const bool standalone_selected =
            library_selected
            && mgstc::engine::isStandaloneLibraryTimbreId(item->library_id);
        edit_.setEnabled(canEditAssigned());
        rename_.setEnabled(standalone_selected && on_rename_ != nullptr);
        tags_.setEnabled(standalone_selected && on_set_tags_ != nullptr);
        memo_.setReadOnly(!standalone_selected || on_set_memo_ == nullptr);
        load_.setEnabled(selectedChoice().has_value());
        syncing_ = false;
    }

    [[nodiscard]] const EnvelopeTimbreCatalogItem* selectedCatalogItem() const {
        const auto id = list_.getSelectedId();
        if (id <= 0) {
            return nullptr;
        }
        const auto index = static_cast<std::size_t>(id - 1);
        if (index >= visible_ids_.size()) {
            return nullptr;
        }
        const auto library_id = visible_ids_[index];
        for (const auto& item : catalog_) {
            if (item.library_id == library_id) {
                return &item;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::uint64_t> selectedLibraryId() const {
        if (const auto* item = selectedCatalogItem()) {
            return item->library_id;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool canEditAssigned() const {
        if (source_ == mgstc::engine::TimbreSource::Opll
            && preset_.getSelectedId() > 0) {
            return false;
        }
        if (selectedLibraryId()) {
            return true;
        }
        return current_.pick == mgstc::engine::TimbrePick::Library
            && current_.library_id != 0;
    }

    void previewSelection() {
        if (live_apply_armed_ && !syncing_ && on_selection_preview_)
            if (const auto choice = selectedChoice()) on_selection_preview_(*choice);
    }

    [[nodiscard]] std::optional<EnvelopeTimbreChoice> selectedChoice() const {
        if (source_ == mgstc::engine::TimbreSource::Scc
            && preset_.getSelectedId() > 0) {
            EnvelopeTimbreChoice choice;
            choice.scc_preset = static_cast<mgstc::engine::SccWavePreset>(
                juce::jlimit(0, 5, preset_.getSelectedId() - 1));
            return choice;
        }
        if (source_ == mgstc::engine::TimbreSource::Opll
            && preset_.getSelectedId() > 0) {
            EnvelopeTimbreChoice choice;
            choice.pick = mgstc::engine::TimbrePick::OpllRom;
            choice.rom_number = static_cast<std::uint8_t>(
                juce::jlimit(0, 14, preset_.getSelectedId() - 1));
            return choice;
        }
        if (const auto library_id = selectedLibraryId()) {
            EnvelopeTimbreChoice choice;
            choice.pick = mgstc::engine::TimbrePick::Library;
            choice.library_id = *library_id;
            return choice;
        }
        return std::nullopt;
    }

    void toggleAb() {
        if (ab_showing_b_) {
            selectLibraryId(current_.library_id);
            if (current_.pick == mgstc::engine::TimbrePick::OpllRom) {
                preset_.setSelectedId(
                    current_.rom_number + 1, juce::dontSendNotification);
            }
            ab_showing_b_ = false;
            return;
        }
        ab_b_id_ = selectedLibraryId().value_or(ab_b_id_);
        if (ab_b_id_ != 0) {
            selectLibraryId(ab_b_id_);
            preset_.setSelectedId(0, juce::dontSendNotification);
            ab_showing_b_ = true;
        }
    }

    void selectLibraryId(std::uint64_t library_id) {
        for (int index = 0; index < static_cast<int>(visible_ids_.size());
             ++index) {
            if (visible_ids_[static_cast<std::size_t>(index)] == library_id) {
                list_.setSelectedId(index + 1, juce::dontSendNotification);
                syncDetailFromSelection();
                return;
            }
        }
        list_.setSelectedId(0, juce::dontSendNotification);
        syncDetailFromSelection();
    }

    void showTagFilter() {
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& item : catalog_) {
            tag_sets.push_back(item.tags);
        }
        juce::Component::SafePointer<EnvelopeTimbrePickContent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("音色ライブラリのタグ検索"),
            filterTagChoices(tag_sets, filter_tags_),
            filter_tags_,
            false,
            [safe](std::vector<std::string> selected) {
                if (safe != nullptr) {
                    safe->filter_tags_ = std::move(selected);
                    safe->refreshList();
                }
            });
    }

    void renameSelected() {
        const auto id = selectedLibraryId();
        if (!id || on_rename_ == nullptr) {
            return;
        }
        const auto requested = name_.getText().trim();
        if (requested.isEmpty()) {
            return;
        }
        if (on_rename_(*id, requested)) {
            for (auto& item : catalog_) {
                if (item.library_id == *id) {
                    item.name = requested;
                }
            }
            refreshList();
            selectLibraryId(*id);
        }
    }

    void editSelectedTags() {
        const auto* item = selectedCatalogItem();
        if (item == nullptr || on_set_tags_ == nullptr) {
            return;
        }
        const auto id = item->library_id;
        std::vector<std::vector<std::string>> tag_sets;
        for (const auto& entry : catalog_) {
            tag_sets.push_back(entry.tags);
        }
        juce::Component::SafePointer<EnvelopeTimbrePickContent> safe(this);
        showTagSelectionDialog(
            this,
            juce::String::fromUTF8("タグを選択"),
            filterTagChoices(tag_sets, item->tags),
            item->tags,
            true,
            [safe, id](std::vector<std::string> selected) {
                if (safe == nullptr || !safe->on_set_tags_(id, selected)) {
                    return;
                }
                for (auto& entry : safe->catalog_) {
                    if (entry.library_id == id) {
                        entry.tags = std::move(selected);
                    }
                }
                safe->refreshList();
                safe->selectLibraryId(id);
            });
    }

    void commitMemo() {
        if (syncing_ || on_set_memo_ == nullptr) {
            return;
        }
        const auto id = selectedLibraryId();
        if (!id) {
            return;
        }
        const auto text = memo_.getText();
        if (on_set_memo_(*id, text)) {
            for (auto& item : catalog_) {
                if (item.library_id == *id) {
                    item.memo = text;
                }
            }
        }
    }

    void liveApplySelection() {
        if (!layer_base_ || !live_apply_armed_ || syncing_ || !on_choose_) {
            return;
        }
        const auto choice = selectedChoice();
        if (!choice) {
            return;
        }
        on_choose_(*choice);
        current_ = *choice;
        current_label_.setText(
            currentAssignmentCaption(source_, current_, catalog_),
            juce::dontSendNotification);
        if (on_audition_) {
            on_audition_();
        }
    }

    void addBlankOriginal() {
        if (!on_blank_) {
            return;
        }
        const auto id = on_blank_();
        if (!id) {
            return;
        }
        EnvelopeTimbreChoice choice;
        choice.pick = mgstc::engine::TimbrePick::Library;
        choice.library_id = *id;
        current_ = choice;
        insertCurrentAssignmentCatalogItem(
            catalog_,
            current_,
            mgstc::engine::CompositeTimbre{},
            juce::String::fromUTF8("オリジナル"));
        if (source_ == mgstc::engine::TimbreSource::Opll) {
            preset_.setSelectedId(0, juce::dontSendNotification);
        }
        refreshList();
        selectLibraryId(*id);
        current_label_.setText(
            currentAssignmentCaption(source_, current_, catalog_),
            juce::dontSendNotification);
    }

    mgstc::engine::TimbreSource source_;
    std::vector<EnvelopeTimbreCatalogItem> catalog_;
    EnvelopeTimbreChoice current_;
    std::vector<std::uint64_t> visible_ids_;
    std::vector<std::string> filter_tags_;
    std::uint64_t ab_b_id_{};
    bool ab_showing_b_{};
    bool syncing_{};
    std::function<void(EnvelopeTimbreChoice)> on_choose_;
    std::function<void(std::uint64_t)> on_edit_;
    std::function<void()> on_remove_;
    std::function<void()> on_manage_;
    std::function<bool(std::uint64_t, juce::String)> on_rename_;
    std::function<bool(std::uint64_t, std::vector<std::string>)> on_set_tags_;
    std::function<bool(std::uint64_t, juce::String)> on_set_memo_;
    std::function<void(std::uint8_t)> on_note_on_;
    std::function<void(std::uint8_t)> on_note_off_;
    std::function<void()> on_audition_;
    std::function<void()> on_performance_end_;
    std::function<void(EnvelopeTimbreChoice)> on_selection_preview_;
    std::function<std::optional<std::uint64_t>()> on_blank_;
    bool layer_base_{};
    bool live_apply_armed_{};
    MorphSettings morph_settings_{};
    std::function<MorphChangeResult(MorphSettings, bool)> morph_change_;
    bool morph_available_{};
    bool morph_dragging_{};
    bool morph_drag_rejected_{};
    int morph_count_max_{};
    juce::ToggleButton morph_enabled_;
    juce::Label morph_distribution_label_;
    std::array<MorphDistributionButton, 3> morph_distribution_;
    juce::Label morph_endpoints_, morph_count_label_, morph_curve_label_;
    juce::Label morph_front_, morph_uniform_, morph_back_, morph_error_;
    juce::Slider morph_count_, morph_curve_;
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label current_label_;
    juce::Label preset_label_;
    juce::ComboBox preset_;
    juce::TextButton edit_;
    juce::TextButton blank_;
    juce::TextButton preview_;
    std::unique_ptr<mgstc::app::PerformanceKeyboard> keyboard_;
    juce::TextEditor filter_;
    juce::TextButton tag_filter_;
    juce::TextButton tag_manage_;
    juce::ToggleButton favorite_only_;
    juce::TextButton ab_;
    juce::ComboBox sort_;
    juce::ComboBox list_;
    juce::TextButton load_;
    juce::TextButton remove_;
    juce::TextEditor name_;
    juce::TextButton rename_;
    juce::TextButton tags_;
    juce::TextEditor memo_;
};
class EnvelopeOpllYParamContent final : public juce::Component {
public:
    using PreviewCallback = std::function<void(
        mgstc::engine::OpllPatchParameters, std::uint8_t, bool)>;
    EnvelopeOpllYParamContent(
        const mgstc::engine::OpllPatchParameters& baseline,
        const mgstc::engine::OpllPatchParameters& initial,
        bool editing_existing,
        std::function<void(mgstc::engine::OpllPatchParameters)> on_apply,
        mgstc::app::EditorSession* session,
        PreviewCallback on_preview,
        std::function<void(std::uint8_t)> on_note_off,
        std::function<void()> on_end)
        : session_(session),
          on_apply_(std::move(on_apply)),
          on_preview_(std::move(on_preview)),
          on_note_off_(std::move(on_note_off)),
          on_end_(std::move(on_end)),
          panel_(switch_look_and_feel_) {
        title_.setText(
            juce::String::fromUTF8(
                editing_existing
                    ? "Y-Register y（FMパラメータ）— 編集"
                    : "Y-Register y（FMパラメータ）— 新規"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        hint_.setText(
            juce::String::fromUTF8(
                editing_existing
                    ? "琥珀色＝このステップのY-Register y 群で変わったフィールド。"
                      "表示はTL/FB変調適用後。値変更で1秒試聴。"
                      "決定時は変更レジスタごとに最大1本の y"
                    : "OPLLエディタと同じレイアウト（表示はTL/FB変調込み）。"
                      "値変更で1秒試聴。変更レジスタごとに最大1本の yreg,data"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        addAndMakeVisible(hint_);

        panel_.setDiffBaseline(baseline);
        panel_.setAuditionNote(
            auditionNote());
        panel_.setParameters(initial);
        panel_.onChange = [this](bool commit) {
            juce::ignoreUnused(commit);
            auditionPreview();
        };
        addAndMakeVisible(panel_);
        if (session != nullptr) {
            keyboard_ = std::make_unique<mgstc::app::PerformanceKeyboard>(*session);
            keyboard_->setHeadlessMode(true);
            keyboard_->setCallbacks(
                [this](std::uint8_t note) {
                    if (on_preview_) on_preview_(panel_.parameters(), note, false);
                },
                [this](std::uint8_t note) {
                    if (on_note_off_) on_note_off_(note);
                });
            keyboard_->setSuppressPcInputCallback(
                [this] { return textEntryHasFocusWithin(*this); });
            addChildComponent(*keyboard_);
            setWantsKeyboardFocus(true);
        }

        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            stopPreviewAndRestore();
            if (on_apply_) {
                on_apply_(panel_.parameters());
            }
            closeHost(1);
        };
        addAndMakeVisible(ok_);
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            stopPreviewAndRestore();
            closeHost(0);
        };
        addAndMakeVisible(cancel_);

        setSize(UiLayout::compositeOpllYParamW, UiLayout::compositeOpllYParamH);
    }

    ~EnvelopeOpllYParamContent() override {
        stopPreviewAndRestore();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (keyboard_ != nullptr && keyboard_->shouldConsumeKeyPress(key)) {
            keyboard_->pollPerformanceInput();
            return true;
        }
        return false;
    }

    bool keyStateChanged(bool) override {
        if (keyboard_ != nullptr) keyboard_->pollPerformanceInput();
        return false;
    }

    void visibilityChanged() override {
        if (isShowing()) grabKeyboardFocus();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);

        auto buttons = area.removeFromBottom(textButtonH);
        area.removeFromBottom(sm);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons.removeFromLeft(button_w));

        panel_.setBounds(area);
    }

private:
    void closeHost(int result) {
        if (auto* window =
                findParentComponentOfClass<juce::DialogWindow>()) {
            window->exitModalState(result);
        }
    }

    std::uint8_t auditionNote() const {
        if (session_ != nullptr
            && !standaloneHostPreferences(session_->snapshot().capabilities))
            return session_->auditionPreferences().last_note;
        return LastAuditionNoteStore::instance().get();
    }

    void auditionPreview() {
        const auto note = auditionNote();
        panel_.setAuditionNote(note);
        if (on_preview_) on_preview_(panel_.parameters(), note, true);
    }

    void stopPreviewAndRestore() {
        if (keyboard_ != nullptr) keyboard_->allNotesOff();
        if (on_end_) {
            auto end = std::move(on_end_);
            end();
        }
    }

    mgstc::app::EditorSession* session_{};
    std::function<void(mgstc::engine::OpllPatchParameters)> on_apply_;
    PreviewCallback on_preview_;
    std::function<void(std::uint8_t)> on_note_off_;
    std::function<void()> on_end_;
    std::unique_ptr<mgstc::app::PerformanceKeyboard> keyboard_;
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label title_;
    juce::Label hint_;
    OpllPatchParameterPanel panel_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};


class EnvelopePsgYParamContent final : public juce::Component {
public:
    EnvelopePsgYParamContent(
        std::uint8_t reg,
        std::uint8_t data,
        std::function<void(std::uint8_t, std::uint8_t)> on_apply)
        : on_apply_(std::move(on_apply)) {
        hint_.setText(
            juce::String::fromUTF8("PSG Y-Register y（yreg,data）"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        addAndMakeVisible(hint_);
        reg_label_.setText("reg", juce::dontSendNotification);
        data_label_.setText("data", juce::dontSendNotification);
        for (auto* label : {&reg_label_, &data_label_}) {
            label->setFont(UiFonts::body());
            label->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(label);
        }
        UiFonts::styleBodyField(reg_);
        UiFonts::styleBodyField(data_);
        reg_.setInputRestrictions(2, "0123456789");
        data_.setInputRestrictions(3, "0123456789");
        reg_.setText(juce::String(static_cast<int>(reg)), false);
        data_.setText(juce::String(static_cast<int>(data)), false);
        addAndMakeVisible(reg_);
        addAndMakeVisible(data_);
        ok_.setButtonText(juce::String::fromUTF8("決定"));
        ok_.onClick = [this] {
            const auto r = static_cast<std::uint8_t>(juce::jlimit(
                0, 15, reg_.getText().getIntValue()));
            const auto d = static_cast<std::uint8_t>(juce::jlimit(
                0, 255, data_.getText().getIntValue()));
            if (on_apply_) {
                on_apply_(r, d);
            }
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(1);
            }
        };
        cancel_.setButtonText(juce::String::fromUTF8("キャンセル"));
        cancel_.onClick = [this] {
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);
        setSize(UiLayout::compositeTimbrePickW, UiLayout::compositeTimbrePickH);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        hint_.setBounds(area.removeFromTop(fieldH));
        area.removeFromTop(sm);
        auto row = area.removeFromTop(fieldH);
        reg_label_.setBounds(row.removeFromLeft(UiScale::sx(48)));
        reg_.setBounds(row.removeFromLeft(UiScale::sx(72)));
        row.removeFromLeft(sm);
        data_label_.setBounds(row.removeFromLeft(UiScale::sx(48)));
        data_.setBounds(row.removeFromLeft(UiScale::sx(80)));
        area.removeFromTop(sm);
        auto buttons = area.removeFromTop(textButtonH);
        const int button_w = juce::jmax(
            libraryButtonMinW,
            (buttons.getWidth() - controlGap) / 2);
        ok_.setBounds(buttons.removeFromLeft(button_w));
        buttons.removeFromLeft(controlGap);
        cancel_.setBounds(buttons.removeFromLeft(button_w));
    }

private:
    std::function<void(std::uint8_t, std::uint8_t)> on_apply_;
    juce::Label hint_;
    juce::Label reg_label_;
    juce::Label data_label_;
    juce::TextEditor reg_;
    juce::TextEditor data_;
    juce::TextButton ok_;
    juce::TextButton cancel_;
};

class SoftwareLfoWavePreview final : public juce::Component {
public:
    void setSettings(
        mgstc::engine::SoftwareLfoSettings settings,
        bool apply_extra_roughness,
        mgstc::engine::PsgSccModulationBase pitch_base = {},
        std::int32_t detune_pitch_up = 0) {
        settings_ = settings;
        apply_extra_roughness_ = apply_extra_roughness;
        pitch_base_ = pitch_base;
        detune_pitch_up_ = detune_pitch_up;
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0F);
        graphics.setColour(juce::Colour(UiLayout::panelFill));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);

        auto graph = bounds.reduced(
            static_cast<float>(UiLayout::xs),
            static_cast<float>(UiLayout::xs));
        auto scale_area = graph;
        scale_area.setWidth(static_cast<float>(UiLayout::fieldH));
        graph.removeFromLeft(static_cast<float>(UiLayout::fieldH));
        constexpr int kSeconds = 4;
        constexpr int kDisplayExtent =
            mgstc::engine::kSoftwareLfoPreviewExtent;
        const int interrupt_hz = juce::roundToInt(
            mgstc::engine::kMgscInterruptHz);
        const int tick_count = interrupt_hz * kSeconds;
        graphics.setColour(juce::Colour(0xFF586675));
        graphics.drawHorizontalLine(
            juce::roundToInt(graph.getCentreY()),
            graph.getX(),
            graph.getRight());
        graphics.setFont(UiFonts::dense());
        for (int second = 1; second < kSeconds; ++second) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(second * interrupt_hz)
                    / static_cast<float>(tick_count);
            graphics.drawVerticalLine(
                juce::roundToInt(x),
                graph.getY(),
                graph.getBottom());
            graphics.setColour(juce::Colour(0xFF8896A3));
            graphics.drawText(
                juce::String(second) + "s",
                juce::roundToInt(x) + 3,
                juce::roundToInt(graph.getY()) + 1,
                28,
                14,
                juce::Justification::centredLeft,
                false);
            graphics.setColour(juce::Colour(0xFF586675));
        }

        graphics.setColour(juce::Colour(0xFF8896A3));
        const float scale_text_h =
            UiFonts::dense().getHeight() + static_cast<float>(UiLayout::xs);
        graphics.drawText(
            "+" + juce::String(kDisplayExtent),
            scale_area.withHeight(scale_text_h).toNearestInt(),
            juce::Justification::centredRight,
            false);
        graphics.drawText(
            "0",
            scale_area.withSizeKeepingCentre(
                scale_area.getWidth(), scale_text_h).toNearestInt(),
            juce::Justification::centredRight,
            false);
        graphics.drawText(
            "-" + juce::String(kDisplayExtent),
            scale_area.withTop(
                scale_area.getBottom() - scale_text_h).toNearestInt(),
            juce::Justification::centredRight,
            false);

        const auto map_y = [&](std::int32_t offset) {
            const auto shown = std::clamp(
                offset,
                static_cast<std::int32_t>(-kDisplayExtent),
                static_cast<std::int32_t>(kDisplayExtent));
            return juce::jmap(
                static_cast<float>(shown),
                static_cast<float>(-kDisplayExtent),
                static_cast<float>(kDisplayExtent),
                graph.getBottom(),
                graph.getY());
        };
        juce::Path path;
        std::int32_t minimum_offset = 0;
        std::int32_t maximum_offset = 0;
        float previous_y = graph.getCentreY();
        for (int tick = 0; tick <= tick_count; ++tick) {
            const float x = graph.getX()
                + graph.getWidth()
                    * static_cast<float>(tick)
                    / static_cast<float>(tick_count);
            const auto offset = mgstc::engine::softwareLfoOffsetAtTick(
                settings_,
                static_cast<std::uint32_t>(tick),
                apply_extra_roughness_,
                pitch_base_,
                detune_pitch_up_);
            minimum_offset = std::min(minimum_offset, offset);
            maximum_offset = std::max(maximum_offset, offset);
            const float y = map_y(offset);
            if (tick == 0) {
                path.startNewSubPath(x, y);
            } else {
                // The register value is held between 60 Hz interrupts, then
                // changes immediately at the update tick.
                path.lineTo(x, previous_y);
                path.lineTo(x, y);
            }
            previous_y = y;
        }
        graphics.saveState();
        graphics.reduceClipRegion(graph.toNearestInt());
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.strokePath(path, juce::PathStrokeType(1.5F));
        graphics.restoreState();

        graphics.setColour(juce::Colour(kUiHoverAccent));
        if (maximum_offset > kDisplayExtent) {
            graphics.drawText(
                juce::String::fromUTF8("最大 ") + juce::String(maximum_offset),
                graph.withHeight(scale_text_h).toNearestInt(),
                juce::Justification::centredRight,
                false);
        }
        if (minimum_offset < -kDisplayExtent) {
            graphics.drawText(
                juce::String::fromUTF8("最小 ") + juce::String(minimum_offset),
                graph.withTop(
                    graph.getBottom() - scale_text_h).toNearestInt(),
                juce::Justification::centredRight,
                false);
        }
    }

private:
    mgstc::engine::SoftwareLfoSettings settings_{};
    mgstc::engine::PsgSccModulationBase pitch_base_{};
    std::int32_t detune_pitch_up_{};
    bool apply_extra_roughness_{true};
};

class SoftwareLfoDialogContent final
    : public juce::Component,
      private juce::Timer {
public:
    using ChangeCallback =
        std::function<void(const mgstc::engine::SoftwareLfoSettings&)>;
    using PreviewCallback = std::function<void()>;
    using PollKeysCallback = std::function<void()>;

    SoftwareLfoDialogContent(
        mgstc::engine::TimbreSource source,
        mgstc::engine::SoftwareLfoSettings initial,
        mgstc::engine::PsgSccModulationBase pitch_base,
        std::int32_t detune_pitch_up,
        ChangeCallback on_change,
        PreviewCallback on_preview,
        PollKeysCallback on_poll_keys)
        : source_(source),
          pitch_base_(pitch_base),
          detune_pitch_up_(detune_pitch_up),
          on_change_(std::move(on_change)),
          on_preview_(std::move(on_preview)),
          on_poll_keys_(std::move(on_poll_keys)) {
        title_.setText(
            juce::String::fromUTF8("MGSDRV ソフトウェア LFO（h）"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        enabled_.setButtonText(juce::String::fromUTF8("有効"));
        enabled_.setTooltip(juce::String::fromUTF8(
            "発音前トラック MML の h を出す。オフでは LFO なし"));
        enabled_.setLookAndFeel(&switch_look_and_feel_);
        enabled_.onClick = [this] { commitFromEditors(); };
        addAndMakeVisible(enabled_);

        configureParam(
            delay_,
            delay_label_,
            juce::String::fromUTF8("ディレイ"),
            0.0,
            255.0,
            juce::String::fromUTF8(
                "n1。最初の音程更新までに加える約1/60秒の待ち"
                "（0～255。実際の開始位置には速度n3も含む）"));
        configureParam(
            depth_,
            depth_label_,
            juce::String::fromUTF8("深さ"),
            0.0,
            127.0,
            juce::String::fromUTF8(
                "n2。三角波が折り返すまでの更新回数（0～127）。"
                "振幅は粗さとの積で決まる"));
        configureParam(
            speed_,
            speed_label_,
            juce::String::fromUTF8("速度"),
            0.0,
            255.0,
            juce::String::fromUTF8(
                "n3。音程を1ステップ更新する間隔（0～255）。"
                "値が大きいほど遅く、255は256カウント"));
        configureParam(
            roughness_,
            roughness_label_,
            juce::String::fromUTF8("粗さ"),
            -127.0,
            127.0,
            juce::String::fromUTF8(
                "n4。三角波の1ステップ（−127～127）。"
                "正で先に上がり、負で先に下がる"));
        configureParam(
            extra_roughness_,
            extra_roughness_label_,
            juce::String::fromUTF8("@p"),
            -32768.0,
            32767.0,
            juce::String::fromUTF8(
                "LFO の16bit粗さ（PSG／SCCのみ。−32768～32767）。"
                "0以外のとき n4 の代わりに、オクターブシフト前の周期へ足す。"
                "正で周期が増え、音程は下がる"));
        extra_roughness_.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 64, UiLayout::fieldH - 10);

        hint_.setText(
            juce::String::fromUTF8(
                "MGSDRV 3.20互換の約60Hz階段波。縦軸は音程オフセット"
                "±127固定、縦線は1秒。MIDI／PCキーでも演奏できます。"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        addAndMakeVisible(hint_);
        addAndMakeVisible(wave_);

        preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
        preview_.onClick = [this] {
            commitFromEditors();
            if (on_preview_) {
                on_preview_();
            }
        };
        addAndMakeVisible(preview_);

        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] {
            if (auto* window =
                    findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(close_);

        assignFromSettings(initial);
        extra_roughness_.setEnabled(
            source_ != mgstc::engine::TimbreSource::Opll);
        extra_roughness_label_.setEnabled(extra_roughness_.isEnabled());
        setWantsKeyboardFocus(true);
        setSize(
            UiLayout::compositeLfoDialogW,
            UiLayout::compositeLfoDialogH);
        startTimerHz(15);
    }

    ~SoftwareLfoDialogContent() override {
        enabled_.setLookAndFeel(nullptr);
        stopTimer();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        enabled_.setBounds(
            area.removeFromTop(textButtonH).removeFromLeft(
                switchControlWidth(
                    juce::String::fromUTF8("有効"), textButtonH)));
        area.removeFromTop(sm);
        layoutParam(area.removeFromTop(fieldH), delay_label_, delay_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), depth_label_, depth_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), speed_label_, speed_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), roughness_label_, roughness_);
        area.removeFromTop(controlGap);
        if (source_ != mgstc::engine::TimbreSource::Opll) {
            layoutParam(
                area.removeFromTop(fieldH), extra_roughness_label_, extra_roughness_);
            area.removeFromTop(controlGap);
        } else {
            extra_roughness_.setVisible(false);
            extra_roughness_label_.setVisible(false);
        }
        wave_.setBounds(area.removeFromTop(compositeLfoWaveH));
        area.removeFromTop(sm);
        hint_.setBounds(area.removeFromTop(fieldH + sm));
        area.removeFromTop(sm);
        auto buttons = area.removeFromTop(textButtonH);
        preview_.setBounds(buttons.removeFromLeft(libraryButtonMinW + 24));
        buttons.removeFromLeft(controlGap);
        close_.setBounds(buttons.removeFromLeft(libraryButtonMinW));
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        juce::ignoreUnused(key);
        if (on_poll_keys_) {
            on_poll_keys_();
        }
        return true;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
        return false;
    }

private:
    void timerCallback() override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
    }

    void configureParam(
        juce::Slider& slider,
        juce::Label& label,
        const juce::String& name,
        double minimum,
        double maximum,
        const juce::String& tooltip) {
        label.setText(name, juce::dontSendNotification);
        label.setFont(UiFonts::body());
        label.setJustificationType(juce::Justification::centredLeft);
        label.setTooltip(tooltip);
        addAndMakeVisible(label);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 48, UiLayout::fieldH - 10);
        slider.setTooltip(tooltip);
        slider.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors();
            }
        };
        addAndMakeVisible(slider);
    }

    static void layoutParam(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider) {
        label.setBounds(area.removeFromLeft(UiLayout::setupValueLabelW));
        area.removeFromLeft(UiLayout::xs);
        slider.setBounds(area);
    }

    void assignFromSettings(mgstc::engine::SoftwareLfoSettings settings) {
        assigning_ = true;
        const bool extra = source_ != mgstc::engine::TimbreSource::Opll;
        settings = mgstc::engine::clampSoftwareLfo(settings, extra);
        enabled_.setToggleState(settings.enabled, juce::dontSendNotification);
        delay_.setValue(settings.delay, juce::dontSendNotification);
        depth_.setValue(settings.depth, juce::dontSendNotification);
        speed_.setValue(settings.speed, juce::dontSendNotification);
        roughness_.setValue(settings.roughness, juce::dontSendNotification);
        extra_roughness_.setValue(
            settings.extra_roughness, juce::dontSendNotification);
        wave_.setSettings(settings, extra, pitch_base_, detune_pitch_up_);
        assigning_ = false;
    }

    void commitFromEditors() {
        const bool extra = source_ != mgstc::engine::TimbreSource::Opll;
        mgstc::engine::SoftwareLfoSettings settings;
        settings.enabled = enabled_.getToggleState();
        settings.delay = static_cast<std::uint8_t>(delay_.getValue());
        settings.depth = static_cast<std::uint8_t>(depth_.getValue());
        settings.speed = static_cast<std::uint8_t>(speed_.getValue());
        settings.roughness = static_cast<std::int8_t>(roughness_.getValue());
        settings.extra_roughness = extra
            ? static_cast<std::int32_t>(extra_roughness_.getValue())
            : 0;
        settings = mgstc::engine::clampSoftwareLfo(settings, extra);
        wave_.setSettings(settings, extra, pitch_base_, detune_pitch_up_);
        if (on_change_) {
            on_change_(settings);
        }
    }

    mgstc::engine::TimbreSource source_{};
    mgstc::engine::PsgSccModulationBase pitch_base_{};
    std::int32_t detune_pitch_up_{};
    ChangeCallback on_change_;
    PreviewCallback on_preview_;
    PollKeysCallback on_poll_keys_;
    bool assigning_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label title_;
    juce::ToggleButton enabled_;
    juce::Label delay_label_;
    juce::Label depth_label_;
    juce::Label speed_label_;
    juce::Label roughness_label_;
    juce::Label extra_roughness_label_;
    juce::Slider delay_;
    juce::Slider depth_;
    juce::Slider speed_;
    juce::Slider roughness_;
    juce::Slider extra_roughness_;
    SoftwareLfoWavePreview wave_;
    juce::Label hint_;
    juce::TextButton preview_;
    juce::TextButton close_;
};

enum class TimelineModulationTarget : std::uint8_t {
    Pitch,
    Tremolo,
    TotalLevel,
    Feedback,
};

[[nodiscard]] juce::String timelineModulationName(
    TimelineModulationTarget target) {
    switch (target) {
    case TimelineModulationTarget::Pitch: return "Pitch LFO";
    case TimelineModulationTarget::Tremolo: return "Tremolo";
    case TimelineModulationTarget::TotalLevel: return "TL LFO";
    case TimelineModulationTarget::Feedback: return "FB LFO";
    }
    return {};
}

// Evaluated on the message thread from the same authoring values and shared
// modulation evaluator used by the timeline, audition and MGSC expansion.
class TimelineModulationWavePreview final : public juce::Component {
public:
    static constexpr int kPreviewCounts = 512;
    struct Data {
        TimelineModulationTarget target{};
        std::array<std::optional<int>, kPreviewCounts + 1> base{}, free_curve{};
    };

    explicit TimelineModulationWavePreview(Data data) : data_(std::move(data)) {}

    void setSettings(mgstc::engine::ModulationParameters settings, bool free_curve) {
        settings_ = settings;
        free_curve_ = free_curve;
        repaint();
    }

    [[nodiscard]] std::optional<int> valueAt(int count) const {
        if (count < 0 || count > kPreviewCounts) return std::nullopt;
        const auto base = data_.base[static_cast<std::size_t>(count)];
        if (!base) return std::nullopt;
        if (free_curve_) {
            return settings_.enabled
                ? data_.free_curve[static_cast<std::size_t>(count)].value_or(*base)
                : *base;
        }
        const auto [minimum, maximum] = valueRange();
        return mgstc::engine::modulationEffectiveValueAtCount(
            settings_, static_cast<std::uint32_t>(count), *base, minimum, maximum);
    }

    void paint(juce::Graphics& graphics) override {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0F);
        graphics.setColour(juce::Colour(UiLayout::panelFill));
        graphics.fillRoundedRectangle(bounds, 5.0F);
        graphics.setColour(juce::Colour(0xFF435160));
        graphics.drawRoundedRectangle(bounds, 5.0F, 1.0F);
        auto graph = bounds.reduced(static_cast<float>(UiLayout::xs));
        auto scale = graph.removeFromLeft(static_cast<float>(UiLayout::fieldH));
        const int counts = free_curve_ ? kPreviewCounts : juce::jmin(kPreviewCounts,
            juce::jmax(240, 240 + static_cast<int>(mgstc::engine::softwareLfoFirstUpdateTick(
                settings_.delay, settings_.speed))));
        auto [minimum, maximum] = valueRange();
        if (data_.target == TimelineModulationTarget::Pitch) {
            int extent = mgstc::engine::kSoftwareLfoPreviewExtent;
            for (int count = 0; count <= counts; ++count) {
                if (const auto value = valueAt(count)) extent = juce::jmax(extent, std::abs(*value));
                if (const auto base = data_.base[static_cast<std::size_t>(count)])
                    extent = juce::jmax(extent, std::abs(*base));
            }
            minimum = -extent;
            maximum = extent;
        }
        const auto map_y = [&](int value) {
            return juce::jmap(static_cast<float>(value), static_cast<float>(minimum),
                static_cast<float>(maximum), graph.getBottom(), graph.getY());
        };
        graphics.setFont(UiFonts::dense());
        graphics.setColour(juce::Colour(0xFF8896A3));
        const float text_h = UiFonts::dense().getHeight();
        graphics.drawText(juce::String(maximum), scale.withHeight(text_h).toNearestInt(),
            juce::Justification::centredRight, false);
        graphics.drawText(juce::String(minimum), scale.withTop(scale.getBottom() - text_h).toNearestInt(),
            juce::Justification::centredRight, false);
        graphics.setColour(juce::Colour(0xFF586675));
        if (minimum < 0) graphics.drawHorizontalLine(juce::roundToInt(map_y(0)), graph.getX(), graph.getRight());
        for (int count = 60; count < counts; count += 60) {
            const float x = graph.getX() + graph.getWidth() * static_cast<float>(count) / counts;
            graphics.drawVerticalLine(juce::roundToInt(x), graph.getY(), graph.getBottom());
            graphics.drawText(juce::String(count / 60) + "s", juce::roundToInt(x) + UiLayout::xs,
                juce::roundToInt(graph.getY()), UiLayout::fieldH, juce::roundToInt(text_h),
                juce::Justification::centredLeft, false);
        }
        const auto draw = [&](bool effective, juce::Colour colour) {
            juce::Path path;
            std::optional<float> previous_y;
            for (int count = 0; count <= counts; ++count) {
                const auto value = effective ? valueAt(count) : data_.base[static_cast<std::size_t>(count)];
                if (!value) { previous_y.reset(); continue; }
                const float x = graph.getX() + graph.getWidth() * static_cast<float>(count) / counts;
                const float y = map_y(*value);
                if (previous_y) { path.lineTo(x, *previous_y); path.lineTo(x, y); }
                else path.startNewSubPath(x, y);
                previous_y = y;
            }
            graphics.setColour(colour);
            graphics.strokePath(path, juce::PathStrokeType(effective ? 1.5F : 1.0F));
        };
        graphics.saveState();
        graphics.reduceClipRegion(graph.toNearestInt());
        draw(false, juce::Colour(0xFF8896A3).withAlpha(0.45F));
        draw(true, juce::Colour(kUiHoverAccent));
        graphics.restoreState();
    }

private:
    [[nodiscard]] std::pair<int, int> valueRange() const {
        switch (data_.target) {
        case TimelineModulationTarget::Pitch: return {-32768, 32767};
        case TimelineModulationTarget::Tremolo: return {0, 15};
        case TimelineModulationTarget::TotalLevel: return {0, 63};
        case TimelineModulationTarget::Feedback: return {0, 7};
        }
        return {0, 15};
    }
    Data data_;
    mgstc::engine::ModulationParameters settings_{};
    bool free_curve_{};
};

class TimelineModulationDialogContent final : public juce::Component {
public:
    using ChangeCallback = std::function<void(
        const mgstc::engine::ModulationParameters&, bool)>;

    TimelineModulationDialogContent(
        TimelineModulationTarget target,
        mgstc::engine::ModulationParameters initial,
        bool free_curve,
        TimelineModulationWavePreview::Data preview_data,
        ChangeCallback on_change)
        : on_change_(std::move(on_change)), wave_(std::move(preview_data)) {
        title_.setText(timelineModulationName(target), juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);

        enabled_.setButtonText(juce::String::fromUTF8("有効"));
        enabled_.setLookAndFeel(&switch_look_and_feel_);
        enabled_.onClick = [this] { commit(); };
        addAndMakeVisible(enabled_);

        mode_label_.setText(juce::String::fromUTF8("モード"), juce::dontSendNotification);
        mode_label_.setFont(UiFonts::body());
        addAndMakeVisible(mode_label_);
        mode_.addItem(juce::String::fromUTF8("往復"), 1);
        mode_.addItem(juce::String::fromUTF8("上昇"), 2);
        mode_.addItem(juce::String::fromUTF8("下降"), 3);
        if (target == TimelineModulationTarget::TotalLevel
            || target == TimelineModulationTarget::Feedback) {
            mode_.addItem(juce::String::fromUTF8("自由曲線"), 4);
        }
        mode_.onChange = [this] { updateParamAvailability(); commit(); };
        addAndMakeVisible(mode_);

        configureParam(delay_, delay_label_, juce::String::fromUTF8("ディレイ"),
            0, 255, juce::String::fromUTF8("開始までのカウント (0～255)"));
        configureParam(depth_, depth_label_, juce::String::fromUTF8("深さ"),
            0, 127, juce::String::fromUTF8("往復モードの折り返しステップ数 (0～127)"));
        configureParam(speed_, speed_label_, juce::String::fromUTF8("速度"),
            0, 255, juce::String::fromUTF8("1ステップの更新間隔 (0～255)"));
        configureParam(roughness_, roughness_label_, juce::String::fromUTF8("粗さ"),
            -127, 127, juce::String::fromUTF8(
                "1ステップの変化量 (−127～127)。上昇／下降では絶対値を使用"));

        addAndMakeVisible(wave_);

        hint_.setText(juce::String::fromUTF8(
            "上昇／下降では深さを使わず、粗さの絶対値で変化します。"),
            juce::dontSendNotification);
        hint_.setFont(UiFonts::body());
        hint_.setColour(juce::Label::textColourId, juce::Colour(0xFFB9C6D2));
        addAndMakeVisible(hint_);

        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] {
            if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) {
                window->exitModalState(0);
            }
        };
        addAndMakeVisible(close_);

        assigning_ = true;
        enabled_.setToggleState(initial.enabled || free_curve,
            juce::dontSendNotification);
        mode_.setSelectedId(free_curve ? 4 : static_cast<int>(initial.mode) + 1,
            juce::dontSendNotification);
        delay_.setValue(initial.delay, juce::dontSendNotification);
        depth_.setValue(initial.depth, juce::dontSendNotification);
        speed_.setValue(initial.speed, juce::dontSendNotification);
        roughness_.setValue(initial.roughness, juce::dontSendNotification);
        assigning_ = false;
        updateParamAvailability();
        updateWave();
        setSize(UiLayout::compositeLfoDialogW,
            UiLayout::panelPad * 2 + UiLayout::titleH + UiLayout::xs
                + UiLayout::textButtonH + UiLayout::sm
                + (UiLayout::fieldH + UiLayout::controlGap) * 5
                + UiLayout::compositeLfoWaveH + UiLayout::sm
                + UiLayout::fieldH * 2 + UiLayout::sm
                + UiLayout::textButtonH);
    }

    ~TimelineModulationDialogContent() override {
        enabled_.setLookAndFeel(nullptr);
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(xs);
        enabled_.setBounds(area.removeFromTop(textButtonH).removeFromLeft(
            switchControlWidth(juce::String::fromUTF8("有効"), textButtonH)));
        area.removeFromTop(sm);
        layoutParam(area.removeFromTop(fieldH), mode_label_, mode_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), delay_label_, delay_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), depth_label_, depth_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), speed_label_, speed_);
        area.removeFromTop(controlGap);
        layoutParam(area.removeFromTop(fieldH), roughness_label_, roughness_);
        area.removeFromTop(sm);
        wave_.setBounds(area.removeFromTop(compositeLfoWaveH));
        area.removeFromTop(sm);
        hint_.setBounds(area.removeFromTop(fieldH * 2));
        close_.setBounds(area.removeFromBottom(textButtonH)
            .removeFromLeft(libraryButtonMinW));
    }

    [[nodiscard]] bool verifyPreviewLayoutAndDelay(int delay) const {
        return static_cast<int>(delay_.getValue()) == delay
            && wave_.getY() >= roughness_.getBottom() + UiLayout::sm
            && wave_.getHeight() == UiLayout::compositeLfoWaveH
            && wave_.getBottom() <= hint_.getY()
            && hint_.getBottom() <= close_.getY();
    }

private:
    void configureParam(juce::Slider& slider, juce::Label& label,
        const juce::String& name, int minimum, int maximum,
        const juce::String& tooltip) {
        label.setText(name, juce::dontSendNotification);
        label.setFont(UiFonts::body());
        label.setTooltip(tooltip);
        addAndMakeVisible(label);
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxLeft, false,
            UiLayout::setupValueLabelW / 2, UiLayout::fieldH - UiLayout::panelPad);
        slider.setTooltip(tooltip);
        slider.onValueChange = [this] { commit(); };
        addAndMakeVisible(slider);
    }

    static void layoutParam(juce::Rectangle<int> area,
        juce::Label& label, juce::Component& field) {
        label.setBounds(area.removeFromLeft(UiLayout::setupValueLabelW));
        area.removeFromLeft(UiLayout::xs);
        field.setBounds(area);
    }

    void updateParamAvailability() {
        const bool free_curve = mode_.getSelectedId() == 4;
        for (auto* control : {&delay_, &speed_, &roughness_}) {
            control->setEnabled(!free_curve);
        }
        depth_.setEnabled(mode_.getSelectedId() == 1);
        hint_.setText(free_curve
            ? juce::String::fromUTF8("自由曲線は TL/FB 自動レーンで描画します。")
            : juce::String::fromUTF8(
                "上昇／下降では深さを使わず、粗さの絶対値で変化します。"),
            juce::dontSendNotification);
    }

    void updateWave() {
        wave_.setSettings(settingsFromControls(), mode_.getSelectedId() == 4);
    }

    [[nodiscard]] mgstc::engine::ModulationParameters settingsFromControls() const {
        mgstc::engine::ModulationParameters settings;
        settings.enabled = enabled_.getToggleState();
        settings.mode = static_cast<mgstc::engine::ModulationMode>(
            juce::jlimit(0, 2, mode_.getSelectedId() - 1));
        settings.delay = static_cast<std::uint8_t>(delay_.getValue());
        settings.depth = static_cast<std::uint8_t>(depth_.getValue());
        settings.speed = static_cast<std::uint8_t>(speed_.getValue());
        settings.roughness = static_cast<std::int8_t>(roughness_.getValue());
        return settings;
    }

    void commit() {
        if (assigning_) return;
        updateWave();
        if (on_change_) on_change_(settingsFromControls(), mode_.getSelectedId() == 4);
    }

    ChangeCallback on_change_;
    bool assigning_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label title_, mode_label_, delay_label_, depth_label_, speed_label_,
        roughness_label_, hint_;
    juce::ToggleButton enabled_;
    juce::ComboBox mode_;
    juce::Slider delay_, depth_, speed_, roughness_;
    TimelineModulationWavePreview wave_;
    juce::TextButton close_;
};

[[nodiscard]] juce::Rectangle<float> rateEnvelopePlotRect(
    juce::Rectangle<int> bounds) {
    return bounds.reduced(UiLayout::rateEnvelopeHandleR)
        .withTrimmedBottom(UiLayout::fieldH / 2).toFloat();
}

[[nodiscard]] float rateEnvelopeXFromSeconds(
    juce::Rectangle<float> graph,
    float seconds) {
    const float duration = mgstc::engine::RateEnvelopeTrace::kDurationSeconds;
    return juce::jmap(
        juce::jlimit(0.0F, duration, seconds),
        0.0F,
        duration,
        graph.getX() + 2.0F,
        graph.getRight() - 2.0F);
}

[[nodiscard]] float rateEnvelopeYFromLevel(
    juce::Rectangle<float> graph,
    std::uint8_t level) {
    return juce::jmap(
        static_cast<float>(level),
        0.0F,
        255.0F,
        graph.getBottom() - 3.0F,
        graph.getY() + 3.0F);
}

[[nodiscard]] float rateEnvelopeSecondsFromX(
    juce::Rectangle<float> graph,
    float x) {
    return juce::jmap(
        juce::jlimit(graph.getX() + 2.0F, graph.getRight() - 2.0F, x),
        graph.getX() + 2.0F,
        graph.getRight() - 2.0F,
        0.0F,
        mgstc::engine::RateEnvelopeTrace::kDurationSeconds);
}

[[nodiscard]] std::uint8_t rateEnvelopeLevelFromY(
    juce::Rectangle<float> graph,
    float y) {
    return static_cast<std::uint8_t>(juce::jlimit(
        0,
        255,
        juce::roundToInt(
            juce::jmap(
                y,
                graph.getBottom() - 3.0F,
                graph.getY() + 3.0F,
                0.0F,
                255.0F))));
}

void paintRateEnvelopeHandles(
    juce::Graphics& graphics,
    juce::Rectangle<float> graph,
    const mgstc::engine::RateEnvelopeHandleLayout& handles,
    int hover_index,
    int active_index) {
    const float radius = static_cast<float>(UiLayout::rateEnvelopeHandleR);
    for (int index = 0;
         index < static_cast<int>(handles.handles.size());
         ++index) {
        const auto& handle = handles.handles[static_cast<std::size_t>(index)];
        if (!handle.available) {
            continue;
        }
        const auto centre = juce::Point<float>(
            rateEnvelopeXFromSeconds(graph, handle.seconds),
            rateEnvelopeYFromLevel(graph, handle.level));
        const bool hot = index == hover_index || index == active_index;
        graphics.setColour(
            hot
                ? juce::Colour(kUiHoverAccent)
                : juce::Colour(0xFFD66B6B));
        graphics.fillEllipse(
            centre.x - radius,
            centre.y - radius,
            radius * 2.0F,
            radius * 2.0F);
        graphics.setColour(juce::Colour(0xFF111920).withAlpha(0.85F));
        graphics.drawEllipse(
            centre.x - radius,
            centre.y - radius,
            radius * 2.0F,
            radius * 2.0F,
            1.2F);
    }
}

[[nodiscard]] juce::Path rateEnvelopeLevelPolyline(
    juce::Rectangle<float> graph,
    const mgstc::engine::RateEnvelopeHandleLayout& layout) {
    struct Point {
        float seconds{};
        std::uint8_t level{};
    };
    std::vector<Point> points;
    for (const auto& handle : layout.handles) {
        if (!handle.available) {
            continue;
        }
        points.push_back({handle.seconds, handle.level});
    }
    std::stable_sort(
        points.begin(),
        points.end(),
        [](const Point& left, const Point& right) {
            return left.seconds < right.seconds;
        });
    std::vector<Point> unique;
    for (const auto& point : points) {
        if (!unique.empty()
            && std::abs(unique.back().seconds - point.seconds) < 1.0e-4F
            && unique.back().level == point.level) {
            continue;
        }
        unique.push_back(point);
    }
    if (unique.empty()) {
        return {};
    }
    const float duration = mgstc::engine::RateEnvelopeTrace::kDurationSeconds;
    if (unique.front().seconds > 0.0F) {
        unique.insert(
            unique.begin(),
            {0.0F, unique.front().level});
    }
    if (unique.back().seconds < duration) {
        unique.push_back({duration, unique.back().level});
    }
    juce::Path path;
    path.startNewSubPath(
        rateEnvelopeXFromSeconds(graph, unique.front().seconds),
        rateEnvelopeYFromLevel(graph, unique.front().level));
    for (std::size_t index = 1; index < unique.size(); ++index) {
        path.lineTo(
            rateEnvelopeXFromSeconds(graph, unique[index].seconds),
            rateEnvelopeYFromLevel(graph, unique[index].level));
    }
    return path;
}

[[nodiscard]] mgstc::engine::RateEnvelopeHandleLayout
rateEnvelopeHandlesForLayer(const mgstc::engine::CompositeLayer& layer) {
    return mgstc::engine::rateEnvelopeHandleLayout(
        mgstc::engine::rateEnvelopeDefinitionFrom(
            mgstc::engine::clampRateEnvelope(layer.volume_envelope.rate)),
        layer.source != mgstc::engine::TimbreSource::Opll);
}

void paintRateEnvelopeGraph(
    juce::Graphics& graphics,
    juce::Rectangle<int> bounds,
    const mgstc::engine::RateEnvelopeTrace& trace,
    juce::Colour colour,
    mgstc::engine::TimbreSource source,
    bool draw_labels,
    const mgstc::engine::RateEnvelopeHandleLayout* handles = nullptr,
    int hover_index = -1,
    int active_index = -1,
    bool draw_handles = true,
    bool volume_lane = false) {
    // Main @r preview uses the full volume amplitude height; the separate
    // handle editor retains its handle and time-label margins.
    const auto graph = volume_lane
        ? bounds.toFloat().expanded(0.0F, 3.0F)
        : rateEnvelopePlotRect(bounds);
    graphics.setColour(juce::Colour(0xFF111920));
    const auto background = volume_lane ? bounds.toFloat() : graph;
    graphics.fillRoundedRectangle(background, 5.0F);
    graphics.setColour(juce::Colour(0xFF435160));
    graphics.drawRoundedRectangle(background, 5.0F, 1.0F);
    const float key_off_x = rateEnvelopeXFromSeconds(
        graph, mgstc::engine::RateEnvelopeTrace::kKeyOffSeconds);
    const float dash_pattern[]{4.0F, 3.0F};
    graphics.setColour(juce::Colour(0xFFD66B6B));
    graphics.drawDashedLine(
        {key_off_x, graph.getY() + 1.0F, key_off_x, graph.getBottom() - 1.0F},
        dash_pattern,
        2,
        1.0F);
    if (trace.valid) {
        juce::Path quantized;
        const int lane_h = juce::jmax(
            1, juce::roundToInt(graph.getHeight() - 6.0F));
        const float graph_bottom = graph.getBottom() - 3.0F;
        for (std::size_t index = 0; index < trace.level.size(); ++index) {
            const float x = juce::jmap(
                static_cast<float>(index),
                0.0F,
                static_cast<float>(trace.level.size() - 1),
                graph.getX() + 2.0F,
                graph.getRight() - 2.0F);
            const int nibble = juce::jlimit(
                0,
                15,
                juce::roundToInt(trace.quantized[index] * 15.0F));
            const float qy = graph_bottom
                - static_cast<float>(
                      mgstc::engine::chipVolumeDisplayPixelsFromBottom(
                          source, nibble, lane_h));
            if (index == 0) {
                quantized.startNewSubPath(x, qy);
            } else {
                quantized.lineTo(x, qy);
            }
        }
        juce::Path level;
        if (handles != nullptr) {
            level = rateEnvelopeLevelPolyline(graph, *handles);
        }
        if (!level.isEmpty()) {
            juce::Path filled = level;
            filled.lineTo(graph.getRight() - 2.0F, graph.getBottom() - 3.0F);
            filled.lineTo(graph.getX() + 2.0F, graph.getBottom() - 3.0F);
            filled.closeSubPath();
            graphics.setColour(colour.withAlpha(0.55F));
            graphics.fillPath(filled);
        }
        juce::Path chip_fill = quantized;
        chip_fill.lineTo(graph.getRight() - 2.0F, graph.getBottom() - 3.0F);
        chip_fill.lineTo(graph.getX() + 2.0F, graph.getBottom() - 3.0F);
        chip_fill.closeSubPath();
        graphics.setColour(colour.withAlpha(0.32F));
        graphics.fillPath(chip_fill);
        graphics.setColour(colour.withAlpha(0.55F));
        graphics.strokePath(
            quantized,
            juce::PathStrokeType(
                2.2F,
                juce::PathStrokeType::mitered,
                juce::PathStrokeType::butt));
        if (!level.isEmpty()) {
            graphics.setColour(colour);
            graphics.strokePath(
                level,
                juce::PathStrokeType(
                    1.7F,
                    juce::PathStrokeType::mitered,
                    juce::PathStrokeType::butt));
        }
    }
    if (handles != nullptr && draw_handles) {
        paintRateEnvelopeHandles(
            graphics, graph, *handles, hover_index, active_index);
    }
    if (!draw_labels) {
        return;
    }
    graphics.setFont(UiFonts::dense());
    graphics.setColour(juce::Colour(0xFF9BA8B2));
    graphics.drawText(
        "0s",
        bounds.reduced(UiScale::sx(5)).removeFromBottom(UiScale::sx(15)),
        juce::Justification::bottomLeft);
    graphics.drawText(
        juce::String(
            juce::roundToInt(
                mgstc::engine::RateEnvelopeTrace::kDurationSeconds))
            + "s",
        bounds.reduced(UiScale::sx(5)).removeFromBottom(UiScale::sx(15)),
        juce::Justification::bottomRight);
    graphics.setColour(juce::Colour(0xFFD98A8A));
    graphics.drawText(
        juce::String::fromUTF8("試聴キーオフ 1.0秒"),
        juce::Rectangle<int>(
            juce::roundToInt(key_off_x) + UiScale::sx(4),
            bounds.getBottom() - UiScale::sx(21),
            UiLayout::compositeSelectorW + UiLayout::sm,
            UiScale::sx(16)),
        juce::Justification::centredLeft);
}

class RateEnvelopeGraphView final : public juce::Component {
public:
    using DragCallback = std::function<void(
        mgstc::engine::RateEnvelopeHandleKind,
        float,
        std::uint8_t,
        bool)>;

    void setTrace(
        mgstc::engine::RateEnvelopeTrace trace,
        juce::Colour colour,
        mgstc::engine::TimbreSource source,
        mgstc::engine::RateEnvelopeHandleLayout handles,
        bool interactive) {
        trace_ = trace;
        colour_ = colour;
        source_ = source;
        handles_ = handles;
        interactive_ = interactive;
        if (!interactive_) {
            hover_index_ = -1;
            drag_index_ = -1;
        }
        repaint();
    }

    void setDragCallback(DragCallback callback) {
        on_drag_ = std::move(callback);
    }

    void paint(juce::Graphics& graphics) override {
        paintRateEnvelopeGraph(
            graphics,
            getLocalBounds(),
            trace_,
            colour_,
            source_,
            true,
            &handles_,
            hover_index_,
            drag_index_);
    }

    void mouseMove(const juce::MouseEvent& event) override {
        if (!interactive_) {
            return;
        }
        hover_index_ = hitHandle(event.position);
        setMouseCursor(
            hover_index_ >= 0
                ? juce::MouseCursor::DraggingHandCursor
                : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override {
        if (drag_index_ < 0) {
            hover_index_ = -1;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!interactive_) {
            return;
        }
        drag_index_ = hitHandle(event.position);
        hover_index_ = drag_index_;
        if (drag_index_ >= 0) {
            applyDrag(event.position, false);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (drag_index_ < 0) {
            return;
        }
        applyDrag(event.position, false);
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (drag_index_ < 0) {
            return;
        }
        applyDrag(event.position, true);
        drag_index_ = -1;
        hover_index_ = hitHandle(event.position);
        setMouseCursor(
            hover_index_ >= 0
                ? juce::MouseCursor::DraggingHandCursor
                : juce::MouseCursor::NormalCursor);
        repaint();
    }

private:
    [[nodiscard]] int hitHandle(juce::Point<float> position) const {
        const auto graph = rateEnvelopePlotRect(getLocalBounds());
        const float radius =
            static_cast<float>(UiLayout::rateEnvelopeHandleR) + 4.0F;
        int best = -1;
        float best_distance = radius * radius;
        for (int index = 0;
             index < static_cast<int>(handles_.handles.size());
             ++index) {
            const auto& handle =
                handles_.handles[static_cast<std::size_t>(index)];
            if (!handle.available) {
                continue;
            }
            const auto centre = juce::Point<float>(
                rateEnvelopeXFromSeconds(graph, handle.seconds),
                rateEnvelopeYFromLevel(graph, handle.level));
            const float distance = centre.getDistanceSquaredFrom(position);
            if (distance <= best_distance) {
                best = index;
                best_distance = distance;
            }
        }
        return best;
    }

    void applyDrag(juce::Point<float> position, bool commit) {
        if (drag_index_ < 0 || !on_drag_) {
            return;
        }
        const auto graph = rateEnvelopePlotRect(getLocalBounds());
        const auto kind =
            handles_.handles[static_cast<std::size_t>(drag_index_)].kind;
        on_drag_(
            kind,
            rateEnvelopeSecondsFromX(graph, position.x),
            rateEnvelopeLevelFromY(graph, position.y),
            commit);
    }

    mgstc::engine::RateEnvelopeTrace trace_{};
    juce::Colour colour_{0xFF53E3A6};
    mgstc::engine::TimbreSource source_{mgstc::engine::TimbreSource::Scc};
    mgstc::engine::RateEnvelopeHandleLayout handles_{};
    DragCallback on_drag_;
    bool interactive_{};
    int hover_index_{-1};
    int drag_index_{-1};
};

[[nodiscard]] juce::Colour colourForTimbreSource(
    mgstc::engine::TimbreSource source) {
    if (source == mgstc::engine::TimbreSource::Psg) {
        return juce::Colour(0xFFB990FF);
    }
    if (source == mgstc::engine::TimbreSource::Scc) {
        return juce::Colour(0xFF53E3A6);
    }
    return juce::Colour(0xFFFFA75E);
}

class RateEnvelopeEditorContent final
    : public juce::Component,
      private juce::Timer {
public:
    using ChangeCallback =
        std::function<void(const mgstc::engine::RateEnvelope&, bool)>;
    using PreviewCallback = std::function<void()>;
    using PollKeysCallback = std::function<void()>;

    RateEnvelopeEditorContent(
        const mgstc::engine::CompositeLayer& layer,
        ChangeCallback on_change,
        PreviewCallback on_preview,
        PollKeysCallback on_poll_keys)
        : source_(layer.source),
          on_change_(std::move(on_change)),
          on_preview_(std::move(on_preview)),
          on_poll_keys_(std::move(on_poll_keys)) {
        title_.setText(
            juce::String::fromUTF8("レートエンベロープ（@r）"),
            juce::dontSendNotification);
        title_.setFont(UiFonts::heading());
        addAndMakeVisible(title_);
        hint_.setText(juce::String::fromUTF8(
            "主線：8bit内部レベル　半透明：チップ実音量\n"
            "赤い点をドラッグ／数値を入力。確定時に1秒試聴。"),
            juce::dontSendNotification);
        hint_.setTooltip(juce::String::fromUTF8(
            "ALとSLはレベル、AR／DR／SR／RRは1割り込みあたりの増減量。"
            "1秒キーオフは試聴用で、定義には保存しません。"));
        hint_.setFont(UiFonts::body());
        hint_.setMinimumHorizontalScale(1.0F);
        addAndMakeVisible(hint_);
        configureBar(attack_level_, attack_level_label_, "AL");
        configureBar(attack_rate_, attack_rate_label_, "AR");
        configureBar(decay_rate_, decay_rate_label_, "DR");
        configureBar(sustain_level_, sustain_level_label_, "SL");
        configureBar(sustain_rate_, sustain_rate_label_, "SR");
        configureBar(release_rate_, release_rate_label_, "RR");
        mode_.addItem(juce::String::fromUTF8("0 変化なし"), 1);
        mode_.addItem(juce::String::fromUTF8("1 トーン"), 2);
        mode_.addItem(juce::String::fromUTF8("2 ノイズ"), 3);
        mode_.addItem(juce::String::fromUTF8("3 トーン+ノイズ"), 4);
        mode_.setTooltip(juce::String::fromUTF8(
            "PSG Tone/Noise モード（@r = { Mode, Noise, AL... } の Mode。0～3）"));
        mode_.onChange = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(mode_);
        mode_label_.setText(
            juce::String::fromUTF8("Mode"), juce::dontSendNotification);
        mode_label_.setFont(UiFonts::body());
        addAndMakeVisible(mode_label_);
        noise_label_.setText(
            juce::String::fromUTF8("Noise"), juce::dontSendNotification);
        noise_label_.setFont(UiFonts::body());
        addAndMakeVisible(noise_label_);
        noise_.setRange(0.0, 31.0, 1.0);
        noise_.setSliderStyle(juce::Slider::LinearHorizontal);
        noise_.setTextBoxStyle(
            juce::Slider::TextBoxLeft, false, 48, UiLayout::fieldH - 10);
        noise_.setTooltip(juce::String::fromUTF8(
            "PSG ノイズ周期（@r = { Mode, Noise, AL... } の Noise。0～31）"));
        noise_.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors(false);
            }
        };
        noise_.onDragEnd = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(noise_);
        addAndMakeVisible(graph_);
        graph_.setDragCallback(
            [this](
                mgstc::engine::RateEnvelopeHandleKind kind,
                float seconds,
                std::uint8_t level,
                bool commit) {
                applyHandleDrag(kind, seconds, level, commit);
            });
        preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
        preview_.setTooltip(juce::String::fromUTF8(
            "最後の試聴音程で総合ミックスを1秒キーオンします。"
            "キーオフ後は各音源の既存動作に従います"));
        preview_.onClick = [this] {
            commitFromEditors(true);
        };
        addAndMakeVisible(preview_);
        input_hint_.setText(juce::String::fromUTF8("PCキー／MIDI：総合ミックスを演奏"), juce::dontSendNotification);
        input_hint_.setFont(UiFonts::body());
        addAndMakeVisible(input_hint_);
        close_.setButtonText(juce::String::fromUTF8("閉じる"));
        close_.onClick = [this] {
            if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                window->closeButtonPressed();
        };
        addAndMakeVisible(close_);
        assignFromLayer(layer);
        setWantsKeyboardFocus(true);
        fitToContent();
        startTimerHz(15);
    }

    void assignFromLayer(const mgstc::engine::CompositeLayer& layer) {
        assigning_ = true;
        source_ = layer.source;
        title_.setText(juce::String(layer.source == mgstc::engine::TimbreSource::Psg ? "PSG" :
                layer.source == mgstc::engine::TimbreSource::Scc ? "SCC" : "OPLL")
            + " Ch." + juce::String(static_cast<int>(layer.channel) + 1)
            + "  /  " + juce::String::fromUTF8(layer.name.c_str()) + "  /  @r " + juce::String(layer.envelope_number),
            juce::dontSendNotification);
        title_.setTooltip(title_.getText());
        title_.setFont(UiFonts::heading());
        hint_.setFont(UiFonts::body());
        input_hint_.setFont(UiFonts::body());
        hint_.setText(juce::String::fromUTF8(
            source_ == mgstc::engine::TimbreSource::Opll
                ? "主線：8bit内部レベル　半透明：チップ実音量\nキーオフはFM側。ソフトウェアRRへは移りません。"
                : "主線：8bit内部レベル　半透明：チップ実音量\n赤い点をドラッグ／数値を入力。確定時に1秒試聴。"),
            juce::dontSendNotification);
        for (auto* label : {&attack_level_label_, &attack_rate_label_, &decay_rate_label_,
                &sustain_level_label_, &sustain_rate_label_, &release_rate_label_,
                &mode_label_, &noise_label_}) label->setFont(UiFonts::body());

        const auto rate = mgstc::engine::clampRateEnvelope(
            layer.volume_envelope.rate);
        attack_level_.setValue(rate.attack_level, juce::dontSendNotification);
        attack_rate_.setValue(rate.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(rate.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(rate.sustain_level, juce::dontSendNotification);
        sustain_rate_.setValue(rate.sustain_rate, juce::dontSendNotification);
        release_rate_.setValue(rate.release_rate, juce::dontSendNotification);
        mode_.setSelectedId(
            static_cast<int>(rate.tone_mode) + 1, juce::dontSendNotification);
        noise_.setValue(rate.noise, juce::dontSendNotification);
        const bool psg = source_ == mgstc::engine::TimbreSource::Psg;
        mode_.setVisible(psg);
        mode_label_.setVisible(psg);
        noise_.setVisible(psg);
        noise_label_.setVisible(psg);
        volume_ = layer.volume;
        refreshGraph();
        assigning_ = false;
        fitToContent();
        if (auto* window =
                findParentComponentOfClass<juce::DocumentWindow>()) {
            window->centreWithSize(getWidth(), getHeight() + 32);
        }
        resized();
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        title_.setBounds(area.removeFromTop(titleH));
        area.removeFromTop(sm);
        auto footer = area.removeFromBottom(textButtonH);
        preview_.setBounds(footer.removeFromLeft(compositeAuditionW + sm));
        footer.removeFromLeft(controlGap);
        close_.setBounds(footer.removeFromRight(libraryButtonMinW));
        input_hint_.setBounds(footer);
        area.removeFromBottom(sm);
        auto bars = area.removeFromRight(rateEnvelopeBarW * 6 + controlGap * 5);
        area.removeFromRight(panelGap);
        auto place = [&bars](juce::Label& label, juce::Slider& slider) {
            auto column = bars.removeFromLeft(rateEnvelopeBarW);
            bars.removeFromLeft(controlGap);
            label.setBounds(column.removeFromTop(fieldH));
            slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false,
                rateEnvelopeBarW - xs, rateEnvelopeBarTextH);
            slider.setBounds(column);
        };
        place(attack_level_label_, attack_level_);
        place(attack_rate_label_, attack_rate_);
        place(decay_rate_label_, decay_rate_);
        place(sustain_level_label_, sustain_level_);
        place(sustain_rate_label_, sustain_rate_);
        place(release_rate_label_, release_rate_);
        hint_.setBounds(area.removeFromBottom(fieldH * 2));
        area.removeFromBottom(sm);
        if (source_ == mgstc::engine::TimbreSource::Psg) {
            auto row = area.removeFromTop(fieldH);
            mode_label_.setBounds(row.removeFromLeft(rateEnvelopeBarW));
            mode_.setBounds(row.removeFromLeft(compositeSelectorW));
            row.removeFromLeft(controlGap);
            noise_label_.setBounds(row.removeFromLeft(rateEnvelopeBarW));
            noise_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, setupSliderTextW, fieldH - sm);
            noise_.setBounds(row);
            area.removeFromTop(sm);
        }
        graph_.setBounds(area);
    }

    void paint(juce::Graphics& g) override {
        paintPageBackground(g, getLocalBounds());
    }


    juce::String verifyEditing() {
        juce::String failures;
        auto check = [&](bool ok, const char* message) { if (!ok) failures += juce::String(message) + "\n"; };
        check(!graph_.getBounds().intersects(attack_level_.getBounds()), "rate graph overlaps bars");
        check(getLocalBounds().contains(close_.getBounds()), "rate close button clipped");
        check(attack_level_.getHeight() - UiLayout::rateEnvelopeBarTextH
            >= UiLayout::rateEnvelopeBarTrackH, "rate track below 256 levels");
        int changes = 0, previews = 0;
        bool committed = false;
        mgstc::engine::RateEnvelope result;
        on_change_ = [&](const auto& rate, bool commit) { ++changes; result = rate; committed = commit; };
        on_preview_ = [&] { ++previews; };
        auto expected = readRate();
        const auto original = expected;
        std::array<juce::Slider*, 6> bars{&attack_level_, &attack_rate_, &decay_rate_,
            &sustain_level_, &sustain_rate_, &release_rate_};
        std::array<std::uint8_t mgstc::engine::RateEnvelope::*, 6> members{
            &mgstc::engine::RateEnvelope::attack_level, &mgstc::engine::RateEnvelope::attack_rate,
            &mgstc::engine::RateEnvelope::decay_rate, &mgstc::engine::RateEnvelope::sustain_level,
            &mgstc::engine::RateEnvelope::sustain_rate, &mgstc::engine::RateEnvelope::release_rate};
        for (std::size_t i = 0; i < bars.size(); ++i) {
            const int value = expected.*members[i] == 255 ? 0 : 255;
            expected.*members[i] = static_cast<std::uint8_t>(value);
            changes = previews = 0;
            bars[i]->setValue(value, juce::sendNotificationSync);
            check(changes == 1 && !committed && previews == 0 && result == expected,
                "rate slider changed unrelated fields or previewed during drag");
            bars[i]->onDragEnd();
            check(changes == 2 && committed && previews == 1 && result == expected,
                "rate slider commit did not preserve preview behavior");
        }
        assigning_ = true; writeRateToEditors(original); assigning_ = false; refreshGraph();
        changes = previews = 0;
        preview_.onClick();
        check(changes == 1 && committed && previews == 1 && result == original,
            "rate explicit preview changed parameters");
        on_change_ = {}; on_preview_ = {};
        return failures;
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (textEntryHasFocusWithin(*this)) {
            return false;
        }
        juce::ignoreUnused(key);
        if (on_poll_keys_) {
            on_poll_keys_();
        }
        return true;
    }

    bool keyStateChanged(bool) override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
        return false;
    }

private:
    void timerCallback() override {
        if (!textEntryHasFocusWithin(*this) && on_poll_keys_) {
            on_poll_keys_();
        }
    }

    void configureBar(
        juce::Slider& slider,
        juce::Label& label,
        const char* name) {
        label.setText(name, juce::dontSendNotification);
        label.setFont(UiFonts::body());
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
        const auto explanation = juce::String::fromUTF8(
            juce::String(name) == "AL" ? "開始レベル AL（0～255）" :
            juce::String(name) == "AR" ? "上昇速度 AR（0～255）" :
            juce::String(name) == "DR" ? "減衰速度 DR（0～255）" :
            juce::String(name) == "SL" ? "持続レベル SL（0～255）" :
            juce::String(name) == "SR" ? "持続中の減衰 SR（0～255）" :
                                       "キーオフ後の減衰 RR（PSG／SCC、0～255）");
        label.setTooltip(explanation);
        slider.setTooltip(explanation);
        slider.setName(explanation);
        slider.setExplicitFocusOrder(getNumChildComponents());
        slider.setRange(0.0, 255.0, 1.0);
        slider.setSliderStyle(juce::Slider::LinearVertical);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxBelow,
            false,
            UiLayout::rateEnvelopeBarW - UiLayout::xs,
            UiLayout::rateEnvelopeBarTextH);
        slider.onValueChange = [this] {
            if (!assigning_) {
                commitFromEditors(false);
            }
        };
        slider.onDragEnd = [this] {
            if (!assigning_) {
                commitFromEditors(true);
            }
        };
        addAndMakeVisible(slider);
    }

    [[nodiscard]] mgstc::engine::RateEnvelope readRate() const {
        mgstc::engine::RateEnvelope rate;
        rate.attack_level = static_cast<std::uint8_t>(attack_level_.getValue());
        rate.attack_rate = static_cast<std::uint8_t>(attack_rate_.getValue());
        rate.decay_rate = static_cast<std::uint8_t>(decay_rate_.getValue());
        rate.sustain_level = static_cast<std::uint8_t>(sustain_level_.getValue());
        rate.sustain_rate = static_cast<std::uint8_t>(sustain_rate_.getValue());
        rate.release_rate = static_cast<std::uint8_t>(release_rate_.getValue());
        if (source_ == mgstc::engine::TimbreSource::Psg) {
            rate.tone_mode = static_cast<std::uint8_t>(
                juce::jlimit(0, 3, mode_.getSelectedId() - 1));
            rate.noise = static_cast<std::uint8_t>(
                juce::jlimit(0, 31, juce::roundToInt(noise_.getValue())));
        }
        return mgstc::engine::clampRateEnvelope(rate);
    }

    [[nodiscard]] bool softwareRelease() const noexcept {
        return source_ != mgstc::engine::TimbreSource::Opll;
    }

    void fitToContent() {
        setSize(
            UiLayout::rateEnvelopeDialogW,
            UiLayout::rateEnvelopeEditorHeight(
                source_ == mgstc::engine::TimbreSource::Psg));
    }

    void writeRateToEditors(const mgstc::engine::RateEnvelope& rate) {
        attack_level_.setValue(rate.attack_level, juce::dontSendNotification);
        attack_rate_.setValue(rate.attack_rate, juce::dontSendNotification);
        decay_rate_.setValue(rate.decay_rate, juce::dontSendNotification);
        sustain_level_.setValue(rate.sustain_level, juce::dontSendNotification);
        sustain_rate_.setValue(rate.sustain_rate, juce::dontSendNotification);
        release_rate_.setValue(rate.release_rate, juce::dontSendNotification);
    }

    void applyHandleDrag(
        mgstc::engine::RateEnvelopeHandleKind kind,
        float seconds,
        std::uint8_t level,
        bool commit) {
        if (assigning_) {
            return;
        }
        auto rate = readRate();
        const auto next = mgstc::engine::applyRateEnvelopeHandleDrag(
            mgstc::engine::rateEnvelopeDefinitionFrom(rate),
            kind,
            seconds,
            level,
            softwareRelease());
        rate.attack_level = next.attack_level;
        rate.attack_rate = next.attack_rate;
        rate.decay_rate = next.decay_rate;
        rate.sustain_level = next.sustain_level;
        rate.sustain_rate = next.sustain_rate;
        rate.release_rate = next.release_rate;
        assigning_ = true;
        writeRateToEditors(rate);
        assigning_ = false;
        commitFromEditors(commit);
    }

    void refreshGraph() {
        const auto rate = readRate();
        graph_.setTrace(
            mgstc::app::makeRateTrace(rate, volume_, source_),
            colourForTimbreSource(source_),
            source_,
            mgstc::engine::rateEnvelopeHandleLayout(
                mgstc::engine::rateEnvelopeDefinitionFrom(rate),
                softwareRelease()),
            true);
    }

    void commitFromEditors(bool commit) {
        const auto rate = readRate();
        refreshGraph();
        if (on_change_) {
            on_change_(rate, commit);
        }
        if (commit && on_preview_) {
            on_preview_();
        }
    }

    mgstc::engine::TimbreSource source_{};
    std::uint8_t volume_{15};
    ChangeCallback on_change_;
    PreviewCallback on_preview_;
    PollKeysCallback on_poll_keys_;
    bool assigning_{};
    juce::Label title_;
    juce::Label hint_;
    juce::Label input_hint_;
    juce::TextButton close_;
    RateEnvelopeGraphView graph_;
    juce::Label attack_level_label_;
    juce::Label attack_rate_label_;
    juce::Label decay_rate_label_;
    juce::Label sustain_level_label_;
    juce::Label sustain_rate_label_;
    juce::Label release_rate_label_;
    juce::Slider attack_level_;
    juce::Slider attack_rate_;
    juce::Slider decay_rate_;
    juce::Slider sustain_level_;
    juce::Slider sustain_rate_;
    juce::Slider release_rate_;
    juce::Label mode_label_;
    juce::ComboBox mode_;
    juce::Label noise_label_;
    juce::Slider noise_;
    juce::TextButton preview_;
};

class RateEnvelopeEditorWindow final : public juce::DocumentWindow {
public:
    RateEnvelopeEditorWindow(
        RateEnvelopeEditorContent* content,
        std::function<void()> on_close)
        : DocumentWindow(
              juce::String::fromUTF8("レートエンベロープ"),
              juce::Colour(0xFF1B222C),
              DocumentWindow::closeButton),
          on_close_(std::move(on_close)) {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        setContentOwned(content, true);
        setWantsKeyboardFocus(true);
        centreWithSize(content->getWidth(), content->getHeight() + 32);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override {
        if (on_close_) {
            on_close_();
        }
    }

    RateEnvelopeEditorContent* editor() const {
        return dynamic_cast<RateEnvelopeEditorContent*>(getContentComponent());
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (auto* content = getContentComponent()) {
            if (content->keyPressed(key)) {
                return true;
            }
        }
        return DocumentWindow::keyPressed(key);
    }

    bool keyStateChanged(bool is_key_down) override {
        if (auto* content = getContentComponent()) {
            static_cast<void>(content->keyStateChanged(is_key_down));
        }
        return DocumentWindow::keyStateChanged(is_key_down);
    }

private:
    std::function<void()> on_close_;
};

class CompositeLayerSetupView final : public juce::Component {
public:
    using ChangedCallback = std::function<void(bool)>;
    using EditCallback = std::function<void()>;
    using PickCallback = std::function<void()>;
    using LfoCallback = std::function<void()>;
    using RateEditCallback = std::function<void()>;
    using DuplicateCallback = std::function<void()>;
    using RemoveCallback = std::function<void()>;
    using AuditionCallback = std::function<void()>;

    CompositeLayerSetupView() {
        source_.setFont(UiFonts::heading());
        addAndMakeVisible(source_);
        mute_.setButtonText(juce::String::fromUTF8("ミュート"));
        solo_.setButtonText(juce::String::fromUTF8("ソロ"));
        for (auto* toggle : {&mute_, &solo_}) {
            toggle->setLookAndFeel(&switch_look_and_feel_);
            toggle->onClick = [this] {
                if (changed_) {
                    changed_(true);
                }
            };
            addAndMakeVisible(*toggle);
        }
        UiFonts::styleBodyField(name_);
        name_.onTextChange = [this] {
            if (changed_) {
                changed_(false);
            }
        };
        name_.onReturnKey = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        name_.onFocusLost = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(name_);
        envelope_number_.setRange(0.0, 31.0, 1.0);
        envelope_number_.setSliderStyle(juce::Slider::LinearBar);
        envelope_number_.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        envelope_number_.setNumDecimalPlacesToDisplay(0);
        envelope_number_.textFromValueFunction = [](double value) {
            return juce::String::formatted(
                "%02d", juce::jlimit(0, 31, juce::roundToInt(value)));
        };
        envelope_number_.valueFromTextFunction = [](const juce::String& text) {
            return static_cast<double>(
                juce::jlimit(0, 31, text.getIntValue()));
        };
        envelope_number_.setTooltip(juce::String::fromUTF8(
            "@e／@r 仮番号（0～31）。新規は00から連番。"
            "プレビュー定義とトラック @e／@r の両方に反映します。"
            "同一総合音色内で重複すると警告します"));
        envelope_number_.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        envelope_number_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(envelope_number_);
        rate_kind_.setButtonText(juce::String::fromUTF8("@r"));
        rate_kind_.setLookAndFeel(&switch_look_and_feel_);
        rate_kind_.setTooltip(juce::String::fromUTF8(
            "オフ＝シーケンス @e、オン＝レート @r。"
            "@e の命令列は残し、@r のときは使いません"));
        rate_kind_.onClick = [this] {
            rate_edit_.setEnabled(rate_kind_.getToggleState());
            const bool psg_sequence = !rate_kind_.getToggleState()
                && source_.getText() == "PSG";
            mixer_mode_.setVisible(psg_sequence);
            mixer_mode_label_.setVisible(psg_sequence);
            mixer_noise_.setVisible(psg_sequence);
            mixer_noise_label_.setVisible(psg_sequence);
            resized();
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(rate_kind_);
        rate_edit_.setButtonText(juce::String::fromUTF8("レート編集"));
        rate_edit_.setTooltip(juce::String::fromUTF8(
            "@r の AL〜RR（PSG は Mode/Noise）を別窓で編集します。"
            "試聴（1秒）と PCキー／MIDI 演奏が使えます"));
        rate_edit_.onClick = [this] {
            if (rate_edit_callback_) {
                rate_edit_callback_();
            }
        };
        addAndMakeVisible(rate_edit_);
        timbre_settings_.setButtonText(juce::String::fromUTF8("音色設定"));
        timbre_settings_.setTooltip(juce::String::fromUTF8(
            "基本音色を別ウィンドウで選びます（エンベロープ@と同じ選択画面）"));
        timbre_settings_.onClick = [this] {
            if (pick_) {
                pick_();
            }
        };
        addAndMakeVisible(timbre_settings_);
        preview_.setButtonText(juce::String::fromUTF8("試聴（1秒）"));
        preview_.setTooltip(juce::String::fromUTF8(
            "現在の初期設定を含む総合音色を1秒試聴します"));
        preview_.onClick = [this] {
            if (audition_callback_) audition_callback_();
        };
        addAndMakeVisible(preview_);
        channel_.onChange = [this] {
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(channel_);
        duplicate_.setButtonText(juce::String::fromUTF8("複製"));
        duplicate_.setTooltip(juce::String::fromUTF8(
            "このチャンネルを同じ音源の空き番号へ複製します。"
            "SCCからOPLLなど、異なる音源へは複製できません"));
        duplicate_.onClick = [this] {
            if (duplicate_callback_) {
                duplicate_callback_();
            }
        };
        addAndMakeVisible(duplicate_);
        remove_.setButtonText(juce::String::fromUTF8("削除"));
        remove_.setTooltip(juce::String::fromUTF8(
            "このチャンネルとその独立エンベロープを削除します"));
        remove_.onClick = [this] {
            if (remove_callback_) remove_callback_();
        };
        addAndMakeVisible(remove_);
        number_mode_.addItem(juce::String::fromUTF8("自動"), 1);
        number_mode_.addItem(juce::String::fromUTF8("手動"), 2);
        number_mode_.setSelectedId(1, juce::dontSendNotification);
        number_mode_.setTooltip(juce::String::fromUTF8(
            "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
        number_mode_.onChange = [this] {
            if (!assigning_) scc_number_edited_ = true;
            syncTimbreNumberEnabled();
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(number_mode_);
        timbre_number_label_.setText(
            juce::String::fromUTF8("音色番号"),
            juce::dontSendNotification);
        timbre_number_label_.setFont(UiFonts::dense());
        timbre_number_label_.setJustificationType(
            juce::Justification::centredLeft);
        timbre_number_label_.setTooltip(juce::String::fromUTF8(
            "手動時のMGSC @番号（15～31）。自動時は無効"));
        addAndMakeVisible(timbre_number_label_);
        timbre_number_.setRange(15.0, 31.0, 1.0);
        timbre_number_.setSliderStyle(juce::Slider::LinearHorizontal);
        timbre_number_.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        timbre_number_.onValueChange = [this] {
            if (!assigning_) scc_number_edited_ = true;
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        timbre_number_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(timbre_number_);
        edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
        edit_.onClick = [this] {
            if (edit_callback_) {
                edit_callback_();
            }
        };
        addAndMakeVisible(edit_);
        h_.setButtonText(juce::String::fromUTF8("LFO h"));
        h_.setClickingTogglesState(true);
        h_.setLookAndFeel(&switch_look_and_feel_);
        h_.setTooltip(juce::String::fromUTF8(
            "MGSDRV ソフトウェア LFO（発音前 h）。"
            "クリックで有効／無効を切り替えます"));
        h_.onClick = [this] {
            if (h_.getToggleState()) p_.setToggleState(false, juce::dontSendNotification);
            syncPitchCommandVisuals();
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(h_);
        h_edit_.setButtonText(juce::String::fromUTF8("編集"));
        h_edit_.setTooltip(juce::String::fromUTF8("LFO h の値を編集します"));
        h_edit_.onClick = [this] { if (lfo_) lfo_(); };
        addAndMakeVisible(h_edit_);
        p_.setButtonText("p");
        p_.setClickingTogglesState(true);
        p_.setTooltip(juce::String::fromUTF8(
            "発音前 p（PSG／SCC）。音程の自動下げ。"
            "下位7bitが量、128以上は出力後の加算。"
            "0と128は変化なし。LFO（h）とは同時に使えない"));
        p_.onClick = [this] {
            if (assigning_) {
                return;
            }
            if (p_.getToggleState()) {
                h_.setToggleState(false, juce::dontSendNotification);
            }
            syncPitchCommandVisuals();
            if (changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(p_);
        configureValueSlider(p_value_, 0.0, 255.0, "");
        p_value_.setTooltip(p_.getTooltip());
        p_value_.onValueChange = [this] {
            if (assigning_) {
                return;
            }
            if (!p_.getToggleState()) {
                p_.setToggleState(true, juce::dontSendNotification);
                h_.setToggleState(false, juce::dontSendNotification);
                syncPitchCommandVisuals();
            }
            if (changed_) {
                changed_(false);
            }
        };
        p_value_.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(p_value_);
        p_value_.setValue(128.0, juce::dontSendNotification);
        configureValueSlider(pitch_, -24.0, 24.0, " st");
        configureValueSlider(detune_, -127.0, 127.0, "");
        configureValueSlider(micro_detune_, -32768.0, 32767.0, "");
        configureValueSlider(delay_, 0.0, 256.0, "");
        configureFineDrag(pitch_, -24.0, 24.0);
        configureFineDrag(detune_, -127.0, 127.0);
        configureFineDrag(micro_detune_, -32768.0, 32767.0);
        configureFineDrag(delay_, 0.0, 256.0);
        configureValueSlider(volume_, 0.0, 15.0, "");
        configureValueSlider(key_off_hang_, 0.0, 255.0, "");
        configureValueSlider(mixer_noise_, 0.0, 31.0, "");
        delay_form_.addItem("r", 1);
        delay_form_.addItem("r%", 2);
        delay_form_.setSelectedId(2, juce::dontSendNotification);
        delay_form_.onChange = [this] {
            updateDelayValueRange();
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        delay_form_.setTooltip(juce::String::fromUTF8(
            "発音前休符の記法。r=音符長（テンポ依存）、"
            "r%=48ステップで4分音符（テンポ依存）。@e内の待ちではない"));
        addAndMakeVisible(delay_form_);
        mixer_mode_.addItem(juce::String::fromUTF8("0 変化なし"), 1);
        mixer_mode_.addItem(juce::String::fromUTF8("1 トーン"), 2);
        mixer_mode_.addItem(juce::String::fromUTF8("2 ノイズ"), 3);
        mixer_mode_.addItem(juce::String::fromUTF8("3 トーン+ノイズ"), 4);
        mixer_mode_.setSelectedId(2, juce::dontSendNotification);
        mixer_mode_.setTooltip(juce::String::fromUTF8(
            "MGSC @e = { Mode, Noise, data } の先頭 Mode。"
            "キーオン時のトーン／ノイズ（0～3。省略時は1）"));
        mixer_mode_.onChange = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(mixer_mode_);
        mixer_noise_.setTooltip(juce::String::fromUTF8(
            "MGSC @e = { Mode, Noise, data } の先頭 Noise（0～31。省略時は0）。"
            "Mode 2以上で有効"));
        pitch_label_.setText(
            juce::String::fromUTF8("相対音程"),
            juce::dontSendNotification);
        detune_label_.setText(juce::String::fromUTF8("デチューン \\"), juce::dontSendNotification);
        micro_detune_label_.setText(
            juce::String::fromUTF8("微デチューン @\\"), juce::dontSendNotification);
        delay_label_.setText(juce::String::fromUTF8("休符"), juce::dontSendNotification);
        volume_label_.setText(juce::String::fromUTF8("音量 v"), juce::dontSendNotification);
        key_off_hang_label_.setText(
            juce::String::fromUTF8("k"), juce::dontSendNotification);
        mixer_mode_label_.setText(
            juce::String::fromUTF8("Mode"), juce::dontSendNotification);
        mixer_noise_label_.setText(
            juce::String::fromUTF8("Noise"), juce::dontSendNotification);
        pitch_.setTooltip(juce::String::fromUTF8(
            "相対音程（半音）。鍵盤／PCキー／MIDI試聴の音程オフセット（§6.5.3）"));
        detune_.setTooltip(juce::String::fromUTF8(
            "トラック・デチューン（MML \\）。@e内の\\とは別。"
            "キーオン時に先に適用し、エンベロープ\\はこれに加算"));
        micro_detune_.setTooltip(juce::String::fromUTF8(
            "トラック微デチューン（MML @\\）。\\と同時利用で効果が重なる。"
            "PSG/SCC: -32768～32767（音程表へ加算してからオクターブ変換）。"
            "FM: 0～255（255で次の半音）"));
        delay_.setTooltip(juce::String::fromUTF8(
            "キーオン前の休符ディレイ（トラックMML。エンベロープ内カウントではない）。"
            "r<n>は音符長（例 r4＝4分、実時間は複合音色のテンポ依存）。"
            "r%<n>はMGSCステップ（48ステップ＝4分音符、テンポ依存）。"
            "グリッド左側の薄表示は発音前待ちの目安"));
        volume_.setTooltip(juce::String::fromUTF8(
            "トラック音量 v（発音前MML）。@eのfはこの値を最大として引き算"));
        key_off_hang_.setTooltip(juce::String::fromUTF8(
            "発音前 k（PSG／SCC）。キーオフ後に合成音量を1下げる間隔 0～255。"
            "0＝即時消音。大きいほど減衰が遅い。@r およびハードウェアEG使用時は無効（値は残る）"));
        key_off_hang_label_.setTooltip(key_off_hang_.getTooltip());
        sustain_.addItem("sf", 1);
        sustain_.addItem("so", 2);
        sustain_.setSelectedId(1, juce::dontSendNotification);
        sustain_.setTooltip(juce::String::fromUTF8(
            "FMサスティン。未指定は sf（既定）。so を選んだときだけ出力します"));
        sustain_.onChange = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(sustain_);
        delay_label_.setTooltip(delay_.getTooltip());
        for (auto* label : {
                 &pitch_label_,
                 &detune_label_,
                 &micro_detune_label_,
                 &delay_label_,
                 &volume_label_,
                 &key_off_hang_label_,
                 &mixer_mode_label_,
                 &mixer_noise_label_}) {
            label->setFont(UiFonts::body());
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        updateDelayValueRange();
        syncTimbreNumberEnabled();
        syncPitchCommandVisuals();
    }

    ~CompositeLayerSetupView() override {
        for (auto* toggle : {&mute_, &solo_, &rate_kind_, &h_}) {
            toggle->setLookAndFeel(nullptr);
        }
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour(UiLayout::panelFill));
        const std::array<juce::String, 3> headings{
            juce::String::fromUTF8("音色・エンベロープ"),
            juce::String::fromUTF8("音量・音程"),
            juce::String::fromUTF8("タイミング・変調")};
        graphics.setFont(UiFonts::body(true));
        graphics.setColour(juce::Colour(0xFF9FAFBD));
        for (std::size_t i = 0; i < headings.size(); ++i) {
            graphics.drawText(headings[i], section_bounds_[i], juce::Justification::centredLeft);
        }

    }

    void setCallbacks(
        ChangedCallback changed,
        EditCallback edit,
        PickCallback pick,
        LfoCallback lfo,
        RateEditCallback rate_edit,
        DuplicateCallback duplicate,
        RemoveCallback remove,
        AuditionCallback audition) {
        changed_ = std::move(changed);
        edit_callback_ = std::move(edit);
        pick_ = std::move(pick);
        lfo_ = std::move(lfo);
        rate_edit_callback_ = std::move(rate_edit);
        duplicate_callback_ = std::move(duplicate);
        remove_callback_ = std::move(remove);
        audition_callback_ = std::move(audition);
    }

    void setDuplicateEnabled(bool enabled) {
        duplicate_.setEnabled(enabled);
        duplicate_.setTooltip(
            enabled
                ? juce::String::fromUTF8(
                      "このチャンネルを同じ音源の空き番号へ複製します。"
                      "SCCからOPLLなど、異なる音源へは複製できません")
                : juce::String::fromUTF8(
                      "この音源のチャンネルはすべて使用中のため複製できません"));
    }

    void syncFromLayer(const mgstc::engine::CompositeLayer& layer) {
        assigning_ = true;
        scc_number_edited_ = false;
        const char* source_name =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? "PSG"
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? "SCC" : "OPLL";
        source_.setText(source_name, juce::dontSendNotification);
        source_.setColour(
            juce::Label::textColourId,
            layer.source == mgstc::engine::TimbreSource::Psg
                ? juce::Colour(0xFFB990FF)
                : layer.source == mgstc::engine::TimbreSource::Scc
                    ? juce::Colour(0xFF53E3A6)
                    : juce::Colour(0xFFFFA75E));
        mute_.setToggleState(layer.muted || !layer.enabled, juce::dontSendNotification);
        solo_.setToggleState(layer.solo, juce::dontSendNotification);
        name_.setText(juce::String::fromUTF8(layer.name.c_str()), false);
        envelope_number_.setValue(
            layer.envelope_number, juce::dontSendNotification);
        const bool rate =
            layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate;
        envelope_number_.setEnabled(
            rate || mgstc::engine::layerUsesSequenceEnvelope(layer));
        rate_kind_.setToggleState(rate, juce::dontSendNotification);
        rate_edit_.setEnabled(rate);
        const int channel_count =
            layer.source == mgstc::engine::TimbreSource::Psg
                ? 3
                : layer.source == mgstc::engine::TimbreSource::Scc ? 5 : 9;
        channel_.clear(juce::dontSendNotification);
        for (int channel = 0; channel < channel_count; ++channel) {
            channel_.addItem(
                juce::String::fromUTF8("Ch.") + juce::String(channel + 1),
                channel + 1);
        }
        channel_.setSelectedId(
            static_cast<int>(layer.channel) + 1,
            juce::dontSendNotification);
        const bool has_timbre_choice =
            layer.source != mgstc::engine::TimbreSource::Psg;
        const bool rom_base = mgstc::engine::layerUsesOpllRomBase(layer);
        const bool library_base = layer.base_timbre.has_value();
        timbre_settings_.setEnabled(has_timbre_choice);
        const bool triangle_preset = library_base
            && layer.base_timbre->library_id
                == mgstc::engine::kSccTrianglePresetLibraryId;
        edit_.setEnabled(library_base && !rom_base);
        number_mode_.setEnabled(library_base && !rom_base);
        if (!has_timbre_choice) {
            timbre_settings_.setTooltip(juce::String::fromUTF8("PSGは@なし"));
        } else if (rom_base) {
            timbre_settings_.setTooltip(
                juce::String::fromUTF8("現在: ")
                + opllRomPatchLabel(static_cast<int>(*layer.base_opll_rom))
                + juce::String::fromUTF8(
                    "\nクリックで基本音色を別ウィンドウから選びます"));
        } else if (library_base) {
            timbre_settings_.setTooltip(
                juce::String::fromUTF8("現在: ")
                + juce::String::fromUTF8(layer.base_timbre->name.c_str())
                + juce::String::fromUTF8(
                    "\nクリックで基本音色を別ウィンドウから選びます"));
        } else {
            timbre_settings_.setTooltip(juce::String::fromUTF8(
                "基本音色を別ウィンドウで選びます（エンベロープ@と同じ選択画面）"));
        }
        if (rom_base) {
            timbre_number_.setRange(0.0, 14.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            timbre_number_.setValue(
                static_cast<double>(*layer.base_opll_rom),
                juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "ROM音色は個別エディタで開けません"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ROM音色は音色番号 0〜14 固定のため変更できません"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        } else if (library_base) {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            const bool manual =
                layer.base_timbre->number_mode
                == mgstc::engine::TimbreNumberMode::Manual;
            number_mode_.setSelectedId(
                manual ? 2 : 1, juce::dontSendNotification);
            timbre_number_.setValue(
                layer.base_timbre->manual_number.value_or(15),
                juce::dontSendNotification);
            edit_.setButtonText(
                juce::String::fromUTF8("編集: ")
                    + juce::String::fromUTF8(layer.base_timbre->name.c_str()));
            edit_.setTooltip(
                triangle_preset
                    ? juce::String::fromUTF8(
                        "この総合音色にコピーしたTriangleを編集します。"
                        "単音色ライブラリとは独立です")
                    : juce::String::fromUTF8(
                        "この総合音色にコピーしたオリジナルを編集します。"
                        "単音色ライブラリとは独立です"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "MGSC音色番号の割り付け。自動＝空き番号、手動＝右の音色番号を使用"));
            timbre_number_label_.setTooltip(juce::String::fromUTF8(
                "手動時のMGSC @番号（15～31）。自動時は無効"));
            timbre_number_.setTooltip(timbre_number_label_.getTooltip());
        } else {
            timbre_number_.setRange(15.0, 31.0, 1.0);
            number_mode_.setSelectedId(1, juce::dontSendNotification);
            edit_.setButtonText(juce::String::fromUTF8("単音色編集"));
            edit_.setTooltip(juce::String::fromUTF8(
                "先に音色を割り当ててください"));
            number_mode_.setTooltip(juce::String::fromUTF8(
                "ライブラリ音色を割り当てると音色番号を設定できます"));
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
            timbre_number_.setTooltip(number_mode_.getTooltip());
        }
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            number_mode_.setEnabled(true);
            timbre_number_.setRange(0.0, 31.0, 1.0);
            const bool contiguous = layer.scc_output_allocation
                == mgstc::engine::SccOutputAllocationMode::Contiguous;
            const auto start = contiguous ? layer.scc_output_start
                : layer.base_timbre ? layer.base_timbre->manual_number : std::nullopt;
            number_mode_.setSelectedId(start ? 2 : 1, juce::dontSendNotification);
            timbre_number_.setValue(start.value_or(0), juce::dontSendNotification);
            timbre_number_label_.setText(juce::String::fromUTF8("開始 @"), juce::dontSendNotification);
            number_mode_.setTooltip(juce::String::fromUTF8(
                "SCCチャンネルの連番音色領域。自動＝空き連続領域、手動＝開始@0～@31。競合時は出力できません"));
            timbre_number_.setTooltip(number_mode_.getTooltip());
            timbre_number_label_.setTooltip(number_mode_.getTooltip());
        } else {
            timbre_number_label_.setText(juce::String::fromUTF8("音色番号"), juce::dontSendNotification);
        }
        syncTimbreNumberEnabled();
        pitch_.setValue(layer.relative_semitones, juce::dontSendNotification);
        h_.setToggleState(
            layer.software_lfo.enabled, juce::dontSendNotification);
        h_edit_.setEnabled(layer.software_lfo.enabled);
        const bool psg_scc =
            layer.source != mgstc::engine::TimbreSource::Opll;
        p_.setVisible(psg_scc);
        p_value_.setVisible(psg_scc);
        p_.setToggleState(
            psg_scc && layer.pitch_sweep.enabled,
            juce::dontSendNotification);
        p_value_.setValue(
            layer.pitch_sweep.value, juce::dontSendNotification);
        key_off_hang_.setVisible(psg_scc);
        key_off_hang_label_.setVisible(psg_scc);
        sustain_.setVisible(!psg_scc);
        key_off_hang_.setValue(
            layer.key_off_hang, juce::dontSendNotification);
        key_off_hang_.setEnabled(psg_scc && !rate);
        key_off_hang_.setAlpha(psg_scc && !rate ? 1.0F : 0.45F);
        key_off_hang_label_.setAlpha(psg_scc && !rate ? 1.0F : 0.45F);
        sustain_.setSelectedId(
            layer.opll_sustain ? 2 : 1, juce::dontSendNotification);
        syncPitchCommandVisuals();
        detune_.setValue(layer.detune, juce::dontSendNotification);
        updateMicroDetuneRange(layer.source);
        micro_detune_.setValue(
            layer.micro_detune, juce::dontSendNotification);
        delay_form_.setSelectedId(
            layer.start_delay_form
                    == mgstc::engine::StartDelayForm::NoteLength
                ? 1
                : 2,
            juce::dontSendNotification);
        updateDelayValueRange();
        delay_.setValue(
            static_cast<double>(layer.start_delay_value),
            juce::dontSendNotification);
        volume_.setValue(layer.volume, juce::dontSendNotification);
        const bool psg_sequence =
            layer.source == mgstc::engine::TimbreSource::Psg && !rate;
        mixer_mode_.setVisible(psg_sequence);
        mixer_mode_label_.setVisible(psg_sequence);
        mixer_noise_.setVisible(psg_sequence);
        mixer_noise_label_.setVisible(psg_sequence);
        const auto mixer =
            mgstc::engine::sequenceEnvelopeMixer(layer.volume_envelope.rate);
        mixer_mode_.setSelectedId(
            static_cast<int>(mixer.tone_mode) + 1,
            juce::dontSendNotification);
        mixer_noise_.setValue(mixer.noise, juce::dontSendNotification);
        assigning_ = false;
    }

    void applyToLayer(mgstc::engine::CompositeLayer& layer) const {
        layer.enabled = true;
        layer.muted = mute_.getToggleState();
        layer.solo = solo_.getToggleState();
        layer.name = name_.getText().toStdString();
        layer.envelope_number = static_cast<std::uint8_t>(
            juce::jlimit(0, 31, juce::roundToInt(envelope_number_.getValue())));
        if (rate_kind_.getToggleState()) {
            if (layer.volume_envelope.kind
                != mgstc::engine::EnvelopeKind::Rate) {
                mgstc::engine::seedDefaultRateEnvelope(
                    layer.volume_envelope.rate, layer.source);
            }
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Rate;
        } else {
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        }
        layer.channel = static_cast<std::uint8_t>(
            juce::jmax(1, channel_.getSelectedId()) - 1);
        layer.relative_semitones = static_cast<std::int8_t>(pitch_.getValue());
        layer.software_lfo.enabled = h_.getToggleState();
        layer.detune = static_cast<std::int16_t>(detune_.getValue());
        {
            const int micro = static_cast<int>(micro_detune_.getValue());
            layer.micro_detune = layer.source == mgstc::engine::TimbreSource::Opll
                ? juce::jlimit(0, 255, micro)
                : juce::jlimit(-32768, 32767, micro);
        }
        layer.start_delay_form =
            delay_form_.getSelectedId() == 1
                ? mgstc::engine::StartDelayForm::NoteLength
                : mgstc::engine::StartDelayForm::AbsoluteTicks;
        layer.start_delay_value =
            static_cast<std::uint32_t>(delay_.getValue());
        layer.volume = static_cast<std::uint8_t>(volume_.getValue());
        if (layer.source == mgstc::engine::TimbreSource::Psg
            && layer.volume_envelope.kind
                == mgstc::engine::EnvelopeKind::Sequence) {
            const auto mode = static_cast<std::uint8_t>(
                juce::jmax(0, mixer_mode_.getSelectedId() - 1));
            const auto noise = static_cast<std::uint8_t>(mixer_noise_.getValue());
            if (mode == mgstc::engine::kMgscSequenceToneModeDefault
                && noise == mgstc::engine::kMgscSequenceNoiseDefault) {
                layer.volume_envelope.rate.tone_mode = 0;
                layer.volume_envelope.rate.noise = 0;
            } else {
                layer.volume_envelope.rate.tone_mode = mode;
                layer.volume_envelope.rate.noise = noise;
            }
        }
        layer.key_off_hang = static_cast<std::uint8_t>(
            key_off_hang_.getValue());
        layer.opll_sustain = layer.source == mgstc::engine::TimbreSource::Opll
            && sustain_.getSelectedId() == 2;
        if (layer.source == mgstc::engine::TimbreSource::Opll) {
            layer.pitch_sweep.enabled = false;
            layer.key_off_hang = 0;
        } else {
            layer.pitch_sweep.enabled = p_.getToggleState();
            layer.pitch_sweep.value = static_cast<std::uint8_t>(
                p_value_.getValue());
            layer.opll_sustain = false;
            if (layer.pitch_sweep.enabled) {
                layer.software_lfo.enabled = false;
            }
        }
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            if (scc_number_edited_ || layer.scc_output_allocation
                    == mgstc::engine::SccOutputAllocationMode::Contiguous) {
                layer.scc_output_allocation = mgstc::engine::SccOutputAllocationMode::Contiguous;
                layer.scc_output_start = number_mode_.getSelectedId() == 2
                    ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(timbre_number_.getValue())}
                    : std::nullopt;
            }
            return;
        }
        if (!layer.base_timbre) {
            return;
        }
        const bool manual = number_mode_.getSelectedId() == 2;
        layer.base_timbre->number_mode = manual
            ? mgstc::engine::TimbreNumberMode::Manual
            : mgstc::engine::TimbreNumberMode::Automatic;
        layer.base_timbre->manual_number = manual
            ? std::optional<std::uint8_t>{
                  static_cast<std::uint8_t>(timbre_number_.getValue())}
            : std::nullopt;
    }

    void refreshFonts() {
        source_.setFont(UiFonts::heading());
        UiFonts::styleBodyField(name_);
        for (auto* label : {&pitch_label_, &detune_label_, &micro_detune_label_,
                 &delay_label_, &volume_label_, &key_off_hang_label_,
                 &mixer_mode_label_, &mixer_noise_label_, &timbre_number_label_}) {
            label->setFont(UiFonts::body());
        }
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto row = [&area] { auto r = area.removeFromTop(fieldH); area.removeFromTop(sm); return r; };
        auto section = [&area](juce::Rectangle<int>& bounds) {
            bounds = area.removeFromTop(libraryTitleH);
            area.removeFromTop(sm);
        };
        auto title = row();
        source_.setBounds(title.removeFromLeft(setupSourceLabelW));
        channel_.setBounds(title);
        auto switches = row();
        for (auto* toggle : {&mute_, &solo_}) {
            toggle->setBounds(switches.removeFromLeft(switchControlWidth(toggle->getButtonText(), fieldH)));
            switches.removeFromLeft(controlGap);
        }
        name_.setBounds(row());
        section(section_bounds_[0]);
        auto identity = row();
        envelope_number_.setBounds(identity.removeFromLeft(setupEnvelopeNumberW));
        identity.removeFromLeft(controlGap);
        rate_kind_.setBounds(identity.removeFromLeft(switchControlWidth("@r", fieldH)));
        identity.removeFromLeft(controlGap);
        rate_edit_.setBounds(identity);
        auto timbre = row();
        edit_.setBounds(timbre.removeFromRight(libraryManageButtonW));
        timbre.removeFromRight(controlGap);
        timbre_settings_.setBounds(timbre);
        auto number = row();
        number_mode_.setBounds(number.removeFromLeft(setupNumberModeW));
        number.removeFromLeft(controlGap);
        timbre_number_label_.setBounds(number.removeFromLeft(setupValueLabelW));
        timbre_number_.setBounds(number);
        section(section_bounds_[1]);
        layoutValue(row(), volume_label_, volume_, UiLayout::compositeInspectorLabelW);
        layoutValue(row(), pitch_label_, pitch_, UiLayout::compositeInspectorLabelW);
        layoutValue(row(), detune_label_, detune_, UiLayout::compositeInspectorLabelW);
        layoutValue(row(), micro_detune_label_, micro_detune_, UiLayout::compositeInspectorLabelW);
        section(section_bounds_[2]);
        auto delay = row();
        delay_label_.setBounds(delay.removeFromLeft(
            UiLayout::compositeInspectorLabelW - setupSustainComboW));
        delay_form_.setBounds(delay.removeFromLeft(setupSustainComboW));
        delay.removeFromLeft(controlGap);
        delay_.setBounds(delay);
        auto gate = row();
        if (key_off_hang_.isVisible()) {
            layoutValue(gate, key_off_hang_label_, key_off_hang_, UiLayout::compositeInspectorLabelW);
            sustain_.setBounds({});
        } else {
            key_off_hang_label_.setBounds({}); key_off_hang_.setBounds({});
            sustain_.setBounds(gate);
        }
        auto modulation = row();
        h_.setBounds(modulation.removeFromLeft(UiLayout::compositeInspectorLabelW));
        modulation.removeFromLeft(controlGap);
        h_edit_.setBounds(modulation);
        auto bend = row();
        p_.setBounds(bend.removeFromLeft(fieldH));
        bend.removeFromLeft(controlGap);
        p_value_.setBounds(bend);
        if (mixer_mode_.isVisible()) {
            auto mixer = row();
            mixer_mode_label_.setBounds(mixer.removeFromLeft(UiLayout::compositeInspectorLabelW));
            mixer_mode_.setBounds(mixer);
            layoutValue(row(), mixer_noise_label_, mixer_noise_, UiLayout::compositeInspectorLabelW);
        } else {
            mixer_mode_.setBounds({}); mixer_mode_label_.setBounds({});
            mixer_noise_.setBounds({}); mixer_noise_label_.setBounds({});
        }
        preview_.setBounds(row());
        auto actions = row();
        duplicate_.setBounds(actions.removeFromLeft((actions.getWidth() - controlGap) / 2));
        actions.removeFromLeft(controlGap);
        remove_.setBounds(actions);
    }

    void mouseDown(const juce::MouseEvent&) override {
        // Inspector whitespace never commits a model edit.
    }

private:
    void updateDelayValueRange() {
        const bool note_length = delay_form_.getSelectedId() == 1;
        delay_.setRange(
            0.0,
            note_length ? 192.0 : 256.0,
            1.0);
        configureFineDrag(delay_, 0.0, note_length ? 192.0 : 256.0);
        delay_.setTextValueSuffix(note_length ? "" : "");
    }

    void updateMicroDetuneRange(mgstc::engine::TimbreSource source) {
        if (source == mgstc::engine::TimbreSource::Opll) {
            micro_detune_.setRange(0.0, 255.0, 1.0);
            configureFineDrag(micro_detune_, 0.0, 255.0);
        } else {
            micro_detune_.setRange(-32768.0, 32767.0, 1.0);
            configureFineDrag(micro_detune_, -32768.0, 32767.0);
        }
    }

    void syncTimbreNumberEnabled() {
        const bool can_assign = number_mode_.isEnabled();
        const bool manual = number_mode_.getSelectedId() == 2;
        timbre_number_.setEnabled(can_assign && manual);
        timbre_number_label_.setEnabled(can_assign && manual);
    }

    void syncPitchCommandVisuals() {
        const bool p_on = p_.isVisible() && p_.getToggleState();
        const bool h_on = h_.getToggleState();
        h_edit_.setEnabled(h_on);
        p_.setAlpha(h_on ? 0.45F : 1.0F);
        h_.setAlpha(p_on ? 0.45F : 1.0F);
        p_value_.setEnabled(p_on && !h_on);
        p_value_.setAlpha(p_on && !h_on ? 1.0F : 0.45F);
    }

    void configureValueSlider(
        juce::Slider& slider,
        double minimum,
        double maximum,
        const juce::String& suffix) {
        slider.setRange(minimum, maximum, 1.0);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(
            juce::Slider::TextBoxLeft,
            false,
            UiLayout::setupSliderTextW,
            UiLayout::fieldH - UiLayout::panelPad);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = [this] {
            if (!assigning_ && changed_) {
                changed_(false);
            }
        };
        slider.onDragEnd = [this] {
            if (!assigning_ && changed_) {
                changed_(true);
            }
        };
        addAndMakeVisible(slider);
    }

    static void configureFineDrag(
        juce::Slider& slider, double minimum, double maximum) {
        slider.setVelocityBasedMode(true);
        slider.setVelocityModeParameters(
            5.0 / (maximum - minimum), 0, 0.5, false);
    }

    static void layoutValue(
        juce::Rectangle<int> area,
        juce::Label& label,
        juce::Slider& slider,
        int label_w = UiLayout::editSubLaneLabelW) {
        label.setBounds(area.removeFromLeft(label_w));
        area.removeFromLeft(UiLayout::controlGap);
        slider.setBounds(area);
    }

    ChangedCallback changed_;
    EditCallback edit_callback_;
    PickCallback pick_;
    LfoCallback lfo_;
    RateEditCallback rate_edit_callback_;
    DuplicateCallback duplicate_callback_;
    RemoveCallback remove_callback_;
    AuditionCallback audition_callback_;
    bool assigning_{};
    bool scc_number_edited_{};
    SwitchLookAndFeel switch_look_and_feel_;
    juce::Label source_;
    juce::ToggleButton mute_;
    juce::ToggleButton solo_;
    juce::TextEditor name_;
    juce::Slider envelope_number_;
    juce::ToggleButton rate_kind_;
    juce::TextButton rate_edit_;
    juce::TextButton timbre_settings_;
    juce::TextButton preview_;
    juce::ComboBox channel_;
    juce::TextButton duplicate_;
    juce::TextButton remove_;
    juce::ComboBox number_mode_;
    juce::Label timbre_number_label_;
    juce::Slider timbre_number_;
    juce::TextButton edit_;
    juce::ToggleButton h_;
    juce::TextButton h_edit_;
    juce::TextButton p_;
    juce::Slider p_value_;
    std::array<juce::Rectangle<int>, 3> section_bounds_;
    juce::Label pitch_label_;
    juce::Label detune_label_;
    juce::Label micro_detune_label_;
    juce::Label delay_label_;
    juce::Label volume_label_;
    juce::Label key_off_hang_label_;
    juce::Label mixer_mode_label_;
    juce::Label mixer_noise_label_;
    juce::ComboBox delay_form_;
    juce::ComboBox sustain_;
    juce::ComboBox mixer_mode_;
    juce::Slider pitch_;
    juce::Slider detune_;
    juce::Slider micro_detune_;
    juce::Slider delay_;
    juce::Slider volume_;
    juce::Slider key_off_hang_;
    juce::Slider mixer_noise_;
};

class CountCommandStackView final : public juce::Component {
public:
    using DeleteCallback = std::function<void(int)>;
    using MoveCallback = std::function<void(int, bool)>;
    using InsertZoneCallback = std::function<void(bool)>;
    void setDeleteCallback(DeleteCallback cb) { delete_callback_ = std::move(cb); }
    void setMoveCallback(MoveCallback cb) { move_callback_ = std::move(cb); }
    void setInsertZoneCallback(InsertZoneCallback cb) { insert_zone_callback_ = std::move(cb); }
    void clearStack() { setStack(-1, false, false, false, {}, {}, {}); }
    void cancelPendingActions() noexcept { ++action_generation_; }
    void setStack(int count, bool loop_start, bool loop_end, bool insert_after,
        std::vector<juce::String> before, std::vector<juce::String> after,
        std::vector<juce::String> after_end, std::vector<int> fixed_zone = {}) {
        // Rebuild only when content changes: keep keyboard focus during repaint/sync.
        if (count_ == count && has_loop_start_ == loop_start && has_loop_end_ == loop_end
            && insert_after_ == (insert_after && loop_start)
            && before_ == before && after_ == after && after_end_ == after_end && fixed_zone_ == fixed_zone) return;
        count_ = count; has_loop_start_ = loop_start; has_loop_end_ = loop_end;
        insert_after_ = insert_after && loop_start;
        before_ = std::move(before); after_ = std::move(after); after_end_ = std::move(after_end);
        fixed_zone_ = std::move(fixed_zone);
        rebuild();
    }
    int preferredHeight() const {
        return UiLayout::panelPad * 2
            + (UiLayout::fieldH + UiLayout::xs) * static_cast<int>(rows_.size());
    }
    void resized() override {
        auto area = getLocalBounds().reduced(UiLayout::panelPad);
        for (auto& row : rows_) {
            if (auto* label = dynamic_cast<juce::Label*>(row.get())) label->setFont(UiFonts::body());
            row->setBounds(area.removeFromTop(UiLayout::fieldH));
            area.removeFromTop(UiLayout::xs);
        }
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xFF26313B));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), static_cast<float>(UiLayout::xs));
    }
private:
    class CommandButton final : public juce::TextButton {
    public:
        explicit CommandButton(const juce::String& text) : juce::TextButton(text) {}
        void mouseDown(const juce::MouseEvent& e) override {
            if (e.mods.isPopupMenu()) { if (onClick) onClick(); }
            else juce::TextButton::mouseDown(e);
        }
    };
    void label(const juce::String& text, bool accent = false) {
        auto row = std::make_unique<juce::Label>();
        row->setText(text, juce::dontSendNotification);
        row->setFont(UiFonts::body());
        row->setColour(juce::Label::textColourId,
            juce::Colour(accent ? kUiHoverAccent : 0xFF9BA8B2));
        row->setTooltip(text);
        addAndMakeVisible(*row); rows_.push_back(std::move(row));
    }
    void zone(const juce::String& text, bool after) {
        auto row = std::make_unique<juce::TextButton>(text);
        row->setToggleState(insert_after_ == after, juce::dontSendNotification);
        row->setTooltip(juce::String::fromUTF8("新しい命令の追加先を選択。既存の命令は移動しません"));
        row->onClick = [safe = juce::Component::SafePointer<CountCommandStackView>(this), after] {
            // The callback synchronously refreshes this list; keep a copy alive.
            if (safe && safe->insert_zone_callback_) {
                auto callback = safe->insert_zone_callback_;
                callback(after);
            }
        };
        addAndMakeVisible(*row); rows_.push_back(std::move(row));
    }
    void commands(const std::vector<juce::String>& commands, int offset, bool readonly = false) {
        if (commands.empty()) { label(juce::String::fromUTF8("（命令なし）")); return; }
        for (int i = 0; i < static_cast<int>(commands.size()); ++i) {
            if (readonly) { label(commands[i]); continue; }
            const int index = offset + i;
            auto row = std::make_unique<CommandButton>(commands[i] + "   ...");
            row->setTooltip(commands[i] + juce::String::fromUTF8("：クリックで移動／削除"));
            row->onClick = [safe = juce::Component::SafePointer<CountCommandStackView>(this), index] {
                if (safe) safe->showCommandMenu(index);
            };
            addAndMakeVisible(*row); rows_.push_back(std::move(row));
        }
    }
    void rebuild() {
        rows_.clear();
        if (count_ < 0) { resized(); repaint(); return; }
        label(juce::String::fromUTF8("カウント ") + juce::String(count_) + juce::String::fromUTF8(" の命令"), true);
        if (has_loop_start_) zone(juce::String::fromUTF8("追加先：初回のみ（ [ より前）"), false);
        commands(before_, 0);
        if (has_loop_start_) {
            label(juce::String::fromUTF8("[  ループ開始"), true);
            zone(juce::String::fromUTF8("追加先：毎周実行（ [ より後）"), true);
            commands(after_, static_cast<int>(before_.size()));
        } else if (!after_.empty()) {
            label(juce::String::fromUTF8("ループ後の保持命令"));
            commands(after_, static_cast<int>(before_.size()), has_loop_end_);
        }
        if (has_loop_end_) label(juce::String::fromUTF8("]  ループ終了"), true);
        if (!after_end_.empty()) {
            label(juce::String::fromUTF8("] 以降（読取専用）"));
            commands(after_end_, 0, true);
        }
        resized(); repaint();
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (!e.mods.isPopupMenu()) return;
        auto* child = getComponentAt(e.getPosition());
        if (auto* button = dynamic_cast<juce::Button*>(child)) button->triggerClick();
    }
    void showCommandMenu(int index) {
        const bool after = index >= static_cast<int>(before_.size());
        juce::PopupMenu menu;
        menu.addItem(1, juce::String::fromUTF8("削除"));
        if (has_loop_start_ && std::find(fixed_zone_.begin(), fixed_zone_.end(), index) == fixed_zone_.end())
            menu.addItem(after ? 2 : 3, juce::String::fromUTF8(
            after ? "ループ前へ移動" : "ループ後へ移動"));
        // Reject a stale popup after switching count/channel or changing the list.
        const auto before = before_, after_commands = after_;
        const int count = count_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [safe = juce::Component::SafePointer<CountCommandStackView>(this),
                index, count, before, after_commands, generation = action_generation_](int result) {
                if (!safe || safe->action_generation_ != generation
                    || safe->count_ != count || safe->before_ != before
                    || safe->after_ != after_commands) return;
                if (result == 1 && safe->delete_callback_) {
                    auto cb = safe->delete_callback_; cb(index);
                } else if ((result == 2 || result == 3) && safe->move_callback_) {
                    auto cb = safe->move_callback_; cb(index, result == 3);
                }
            });
    }
    DeleteCallback delete_callback_;
    MoveCallback move_callback_;
    InsertZoneCallback insert_zone_callback_;
    std::uint64_t action_generation_{};
    int count_{-1};
    bool has_loop_start_{}, has_loop_end_{}, insert_after_{};
    std::vector<juce::String> before_, after_, after_end_;
    std::vector<int> fixed_zone_;
    std::vector<std::unique_ptr<juce::Component>> rows_;
};


}  // namespace

namespace {
class CompositeOverviewCanvas final : public juce::Component {
public:
    std::function<void(juce::Graphics&)> draw;
    std::function<void(juce::Point<int>)> select;
    void mouseDown(const juce::MouseEvent& e) override { if (select && !e.mods.isPopupMenu()) select(e.getPosition()); }
    void paint(juce::Graphics& g) override { if (draw) draw(g); }
};
}

class mgstc::app::CompositeTimeline::Impl final
    : public juce::Component,
      public juce::SettableTooltipClient,
      private juce::ScrollBar::Listener,
      private juce::Timer {
public:
    using EditCallback = std::function<void(
        const mgstc::engine::CompositeTimbre&, bool, bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using CatalogCallback = std::function<
        std::vector<EnvelopeTimbreCatalogItem>(mgstc::engine::TimbreSource)>;
    using OpenTimbreCallback = std::function<void(
        const juce::String&, std::optional<std::uint64_t>)>;
    using AssignTimbreCallback = std::function<void(
        std::size_t, LayerBaseTimbreAssign)>;
    using TimbreNameCallback = std::function<juce::String(std::uint64_t)>;
    using TimbreLibraryCallback =
        std::function<const mgstc::engine::TimbreLibrary*()>;
    using ManageTagsCallback = std::function<void()>;
    using MutateTimbreNameCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using MutateTimbreTagsCallback =
        std::function<bool(std::uint64_t, std::vector<std::string>)>;
    using MutateTimbreMemoCallback =
        std::function<bool(std::uint64_t, juce::String)>;
    using LfoSessionCallback =
        std::function<void(std::optional<std::size_t>, bool, bool)>;
    using LfoPollKeysCallback = std::function<void()>;

    Impl()
        : horizontal_scroll_(false), vertical_scroll_(true) {
        applySessionAllocationStarts(timbre_);
        parameter_.addItem(juce::String::fromUTF8("音量"), 1);
        parameter_.addItem(juce::String::fromUTF8("音程"), 2);
        parameter_.addItem(juce::String::fromUTF8("@音色"), 3);
        parameter_.addItem(juce::String::fromUTF8("Y-Register y"), 6);
        parameter_.addItem(juce::String::fromUTF8("TL変調"), 4);
        parameter_.addItem(juce::String::fromUTF8("FB変調"), 5);
        parameter_.setSelectedId(1, juce::dontSendNotification);
        parameter_.setTooltip(
            juce::String::fromUTF8(
                "時間軸へ描画するパラメーター。"
                "Y-Register yはOPLLオリジナル音色のレジスタ編集／PSGのyreg,data。"
                "TL/FB変調はOPLL専用（Y-Register yとは別）"));
        parameter_.onChange = [this] {
            syncPointEditors();
            syncRegisterAutoEditors();
            syncInspector();
            repaint();
        };
        addAndMakeVisible(parameter_);

        auto_mode_.addItem(juce::String::fromUTF8("OFF"), 1);
        auto_mode_.addItem(juce::String::fromUTF8("上昇"), 2);
        auto_mode_.addItem(juce::String::fromUTF8("下降"), 3);
        auto_mode_.addItem("LFO", 4);
        auto_mode_.addItem(juce::String::fromUTF8("自由曲線"), 5);
        auto_mode_.setSelectedId(1, juce::dontSendNotification);
        auto_mode_.setTooltip(juce::String::fromUTF8(
            "TL/FB変調の時間変化モード。出力は展開したyレジスタ書き込み"));
        auto_mode_.onChange = [this] {
            applyRegisterAutoFromEditors(false);
        };
        addAndMakeVisible(auto_mode_);

        configureAutoParamEditor(
            auto_depth_, juce::String::fromUTF8("深さ（開始値またはLFO振幅）"));
        configureAutoParamEditor(
            auto_speed_, juce::String::fromUTF8("変化量／変化スピード"));
        configureAutoParamEditor(
            auto_coarseness_,
            juce::String::fromUTF8("粗さ（何カウントごとに書き込むか）"));
        configureAutoParamEditor(
            auto_stop_, juce::String::fromUTF8("停止位置（到達値／LFO中心）"));
        auto_depth_label_.setText(
            juce::String::fromUTF8("深さ"), juce::dontSendNotification);
        auto_speed_label_.setText(
            juce::String::fromUTF8("変化"), juce::dontSendNotification);
        auto_coarseness_label_.setText(
            juce::String::fromUTF8("粗さ"), juce::dontSendNotification);
        auto_stop_label_.setText(
            juce::String::fromUTF8("停止"), juce::dontSendNotification);
        for (auto* label : {
                 &auto_depth_label_, &auto_speed_label_,
                 &auto_coarseness_label_, &auto_stop_label_}) {
            label->setJustificationType(juce::Justification::centredRight);
            addAndMakeVisible(*label);
        }

        psg_add_.setButtonText(juce::String::fromUTF8("PSG追加"));
        scc_add_.setButtonText(juce::String::fromUTF8("SCC追加"));
        opll_add_.setButtonText(juce::String::fromUTF8("OPLL追加"));
        psg_add_.setTooltip(juce::String::fromUTF8(
            "未使用のPSGチャンネルを追加します（最大3ch）"));
        scc_add_.setTooltip(juce::String::fromUTF8(
            "未使用のSCCチャンネルを追加します（最大5ch）"));
        opll_add_.setTooltip(juce::String::fromUTF8(
            "未使用のOPLLチャンネルを追加します（最大9ch）"));
        psg_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Psg);
        };
        scc_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Scc);
        };
        opll_add_.onClick = [this] {
            addLayer(mgstc::engine::TimbreSource::Opll);
        };
        addAndMakeVisible(psg_add_);
        addAndMakeVisible(scc_add_);
        addAndMakeVisible(opll_add_);
        add_channel_.setButtonText(juce::String::fromUTF8("+ チャンネル"));
        add_channel_.setTooltip(juce::String::fromUTF8("音源ごとの使用数を確認してチャンネルを追加"));
        add_channel_.onClick = [this] {
            makeAddChannelMenu().showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&add_channel_),
                [safe = juce::Component::SafePointer<Impl>(this), generation = dialog_generation_](int result) {
                    if (safe != nullptr && safe->dialog_generation_ == generation) safe->addChannelFromMenu(result);
                });
        };
        addAndMakeVisible(add_channel_);
        compact_overview_.setButtonText(juce::String::fromUTF8("概要を縮小"));
        compact_overview_.setTooltip(juce::String::fromUTF8("概要の行をコンパクト表示に切り替え（音色は変更しません）"));
        compact_overview_.setClickingTogglesState(true);
        compact_overview_.onClick = [this] {
            const int old_height = compact_overview_.getToggleState()
                ? UiLayout::fieldH * 2 + UiLayout::sm : UiLayout::fieldH + UiLayout::xs * 2;
            const double top_row = static_cast<double>(overview_view_.getViewPositionY()) / old_height;
            resized();
            overview_view_.setViewPosition(0, juce::roundToInt(top_row * overviewRowHeight()));
            repaint();
        };
        addAndMakeVisible(compact_overview_);

        step_edit_mode_.setButtonText(juce::String::fromUTF8("削除/挿入"));
        step_edit_mode_.setClickingTogglesState(true);
        step_edit_mode_.setTooltip(juce::String::fromUTF8(
            "左ドラッグで左側のステップを削除／挿入。命令行は全行、その他はその行のみ。"
            "OFFでもCtrl＋左ドラッグで操作できます。Escで取消"));
        step_edit_mode_.onClick = [this] { cancelStepDrag(); repaint(); };
        addAndMakeVisible(step_edit_mode_);
        setWantsKeyboardFocus(true);

        tempo_label_.setText(
            juce::String::fromUTF8("テンポ"),
            juce::dontSendNotification);
        tempo_label_.setFont(UiFonts::body());
        tempo_label_.setJustificationType(juce::Justification::centredRight);
        tempo_label_.setTooltip(juce::String::fromUTF8(
            "この複合音色の演奏テンポ（MGSC #tempo 57～2047）。ライブラリ保存対象"));
        addAndMakeVisible(tempo_label_);
        UiFonts::styleBodyField(tempo_);
        tempo_.setInputRestrictions(4, "0123456789");
        tempo_.setJustification(juce::Justification::centred);
        tempo_.setText(
            juce::String(playback_tempo_),
            juce::dontSendNotification);
        tempo_.setTooltip(tempo_label_.getTooltip());
        tempo_.onReturnKey = [this] { commitPlaybackTempoFromEditor(); };
        tempo_.onFocusLost = [this] { commitPlaybackTempoFromEditor(); };
        addAndMakeVisible(tempo_);

        edit_mode_.addItem(juce::String::fromUTF8("描画"), 1);
        edit_mode_.addItem(juce::String::fromUTF8("終端"), 2);
        edit_mode_.addItem(juce::String::fromUTF8("L開始"), 3);
        edit_mode_.addItem(juce::String::fromUTF8("L終了"), 4);
        edit_mode_.setSelectedId(1, juce::dontSendNotification);
        edit_mode_.setTooltip(juce::String::fromUTF8(
            "編集モード: 描画／終端／ループ開始(L開始)／ループ終了(L終了)"));
        addAndMakeVisible(edit_mode_);

        value_.setInputRestrictions(5, "-0123456789");
        value_.setText("15", false);
        value_.setTooltip(
            juce::String::fromUTF8("選択パラメーターの値"));
        UiFonts::styleBodyField(value_);
        addAndMakeVisible(value_);
        position_.setText("ct 0", juce::dontSendNotification);
        position_.setJustificationType(juce::Justification::centredRight);
        position_.setTooltip(juce::String::fromUTF8(
            "ホバーまたはクリック中のカウント（数値入力では変更しません）"));
        value_label_.setText(
            juce::String::fromUTF8("値"), juce::dontSendNotification);
        value_label_.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(position_);
        addAndMakeVisible(value_label_);
        apply_.setButtonText(juce::String::fromUTF8("設定"));
        apply_.setTooltip(
            juce::String::fromUTF8(
                "表示中のカウントへ入力値を設定します。"
                "TL/FB変調では開始ステップとパラメーターを確定します"));
        apply_.onClick = [this] {
            if (selectedEditMode() != EditMode::Draw) {
                return;
            }
            if (isRegisterAutoParameter(selectedParameter())) {
                if (selectedRegisterAutoLane() != nullptr
                    && selectedRegisterAutoLane()->mode
                        == mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
                    applyRegisterAutoFromEditors(true);
                } else {
                    openTimelineModulationDialog(
                        selectedParameter() == Parameter::OpllTlAuto
                            ? TimelineModulationTarget::TotalLevel
                            : TimelineModulationTarget::Feedback);
                }
                return;
            }
            if (selectedParameter() == Parameter::Timbre) {
                pickTimbreAt(selected_count_, false);
                return;
            }
            if (selectedParameter() == Parameter::RegisterWrite) {
                pickRegisterAt(selected_count_);
                return;
            }
            if (selectedParameter() == Parameter::Pitch) {
                addPitchCommand(
                    selected_count_, value_.getText().getIntValue(), true);
                return;
            }
            editPoint(selected_count_, value_.getText().getIntValue(), true);
        };
        addAndMakeVisible(apply_);

        inspector_selection_.setJustificationType(
            juce::Justification::centredLeft);
        inspector_selection_.setTooltip(juce::String::fromUTF8(
            "選択中のチャンネルとエンベロープ種類"));
        addAndMakeVisible(inspector_selection_);

        configureInspectorEditor(
            length_editor_, juce::String::fromUTF8("終端カウント"));
        configureInspectorEditor(
            loop_start_editor_, juce::String::fromUTF8("ループ開始カウント"));
        configureInspectorEditor(
            loop_end_editor_, juce::String::fromUTF8("ループ終了カウント"));
        configureInspectorLabel(length_label_, "END");
        configureInspectorLabel(loop_start_label_, juce::String::fromUTF8("開始"));
        configureInspectorLabel(loop_end_label_, juce::String::fromUTF8("終了"));
        loop_start_editor_.onReturnKey = [this] { applyInspector(); };
        loop_end_editor_.onReturnKey = [this] { applyInspector(); };

        inspector_apply_.setButtonText(juce::String::fromUTF8("反映"));
        inspector_apply_.setTooltip(juce::String::fromUTF8(
            "表示中の終端とループ位置を選択エンベロープへ反映します"));
        inspector_apply_.onClick = [this] { applyInspector(); };
        addAndMakeVisible(inspector_apply_);

        clear_loop_.setButtonText(juce::String::fromUTF8("ループ解除"));
        clear_loop_.setTooltip(juce::String::fromUTF8(
            "選択エンベロープのループ開始と終了を両方解除します"));
        clear_loop_.onClick = [this] { clearSelectedLoop(); };
        addAndMakeVisible(clear_loop_);

        command_stack_.setInterceptsMouseClicks(true, false);
        command_stack_.setDeleteCallback([this](int chip_index) {
            deleteStackChipAt(chip_index);
        });
        command_stack_.setMoveCallback(
            [this](int chip_index, bool to_after) {
                moveStackChipAt(chip_index, to_after);
            });
        command_stack_.setInsertZoneCallback([this](bool insert_after) {
            setLoopInsertZone(insert_after);
        });
        addAndMakeVisible(command_stack_);

        envelope_mml_preview_.setMultiLine(true);
        envelope_mml_preview_.setReadOnly(true);
        envelope_mml_preview_.setWantsKeyboardFocus(false);
        envelope_mml_preview_.setScrollbarsShown(true);
        envelope_mml_preview_.setFont(UiFonts::mono());
        envelope_mml_preview_.setColour(
            juce::TextEditor::backgroundColourId,
            juce::Colour(0xFF151A20));
        envelope_mml_preview_.setColour(
            juce::TextEditor::textColourId,
            juce::Colour(0xFF9BE9C3));
        envelope_mml_preview_.setColour(
            juce::TextEditor::outlineColourId,
            juce::Colour(0xFF40505E));
        envelope_mml_preview_.setColour(
            juce::TextEditor::focusedOutlineColourId,
            juce::Colour(kUiHoverAccent));
        envelope_mml_preview_.setTooltip(juce::String::fromUTF8(
            "音色定義=@s/@v、エンベロープ=@e/@r、その下=発音前トラックMML。"
            "マウスで範囲選択して Ctrl+C でコピーできます"));
        addAndMakeVisible(envelope_mml_preview_);

        mml_toggle_.setButtonText(juce::String::fromUTF8("MGSCプレビュー"));
        mml_toggle_.setClickingTogglesState(true);
        mml_toggle_.setTooltip(juce::String::fromUTF8("選択チャンネルの定義と発音前MML。読取専用・範囲コピー可能"));
        mml_toggle_.onClick = [this] { resized(); repaint(); };
        addAndMakeVisible(mml_toggle_);
        zoom_in_.setButtonText("+"); zoom_out_.setButtonText("-");
        zoom_in_.setTooltip(juce::String::fromUTF8("ステップ間隔を拡大"));
        zoom_out_.setTooltip(juce::String::fromUTF8("ステップ間隔を縮小"));
        zoom_in_.setExplicitFocusOrder(120); zoom_out_.setExplicitFocusOrder(121);
        zoom_in_.onClick = [this] { zoomCountColumns(graphArea().getCentreX(), 0.8); };
        zoom_out_.onClick = [this] { zoomCountColumns(graphArea().getCentreX(), 1.25); };
        addAndMakeVisible(zoom_in_); addAndMakeVisible(zoom_out_);
        pitch_lane_toggle_.setButtonText(juce::String::fromUTF8("音程"));
        pitch_lane_toggle_.setLookAndFeel(&modulation_switch_look_and_feel_);
        pitch_lane_toggle_.setTooltip(juce::String::fromUTF8(
            "音程編集を展開します。Pitch LFOまたはLFO hが有効な間は自動で展開します"));
        pitch_lane_toggle_.onClick = [this] {
            if (selected_layer_ < 0
                || selected_layer_ >= static_cast<int>(pitch_lane_expanded_.size()))
                return;
            const auto index = static_cast<std::size_t>(selected_layer_);
            pitch_lane_expanded_[index] = pitch_lane_toggle_.getToggleState();
            resized();
            repaint();
        };
        addAndMakeVisible(pitch_lane_toggle_);
        morph_bank_label_.setText("SCC", juce::dontSendNotification);
        morph_bank_label_.setFont(UiFonts::body());
        addAndMakeVisible(morph_bank_label_);
        for (int number = 0; number < 32; ++number)
            morph_bank_.addItem("@" + juce::String(number), number + 1);
        morph_bank_.setSelectedId(1, juce::dontSendNotification);
        morph_bank_.setExplicitFocusOrder(223);
        morph_bank_.setTooltip(juce::String::fromUTF8("この編集セッションのSCCオリジナル音色の自動割当開始位置（音色へ保存しません）"));
        morph_bank_.onChange = [this] { changeOriginalAllocationStarts(); };
        addAndMakeVisible(morph_bank_);
        morph_bank_usage_.setFont(UiFonts::body());
        addAndMakeVisible(morph_bank_usage_);
        opll_bank_label_.setText("OPLL", juce::dontSendNotification);
        opll_bank_label_.setFont(UiFonts::body());
        addAndMakeVisible(opll_bank_label_);
        for (int number = 16; number < 32; ++number)
            opll_bank_.addItem("@" + juce::String(number), number + 1);
        opll_bank_.setSelectedId(17, juce::dontSendNotification);
        opll_bank_.setExplicitFocusOrder(224);
        opll_bank_.setTooltip(juce::String::fromUTF8("この編集セッションのOPLLオリジナル音色の自動割当開始位置（音色へ保存しません）"));
        opll_bank_.onChange = [this] { changeOriginalAllocationStarts(); };
        addAndMakeVisible(opll_bank_);
        opll_bank_usage_.setFont(UiFonts::body());
        addAndMakeVisible(opll_bank_usage_);
        allocation_error_label_.setFont(UiFonts::body());
        allocation_error_label_.setColour(juce::Label::textColourId, juce::Colours::red);
        allocation_error_label_.setMinimumHorizontalScale(1.0F);
        addChildComponent(allocation_error_label_);
        for (auto* c : std::initializer_list<juce::Component*>{
                 &edit_mode_, &parameter_, &position_, &value_label_,
                 &value_, &apply_, &inspector_selection_,
                 &length_label_, &length_editor_, &loop_start_label_,
                 &loop_start_editor_, &loop_end_label_, &loop_end_editor_,
                 &inspector_apply_, &clear_loop_, &command_stack_,
                 &envelope_mml_preview_,
                 &auto_mode_, &auto_depth_, &auto_speed_,
                 &auto_coarseness_, &auto_stop_, &auto_depth_label_,
                 &auto_speed_label_, &auto_coarseness_label_,
                 &auto_stop_label_}) {
            inspector_content_.addAndMakeVisible(*c);
        }

        modulation_heading_.setText(
            "LFO / Modulation", juce::dontSendNotification);
        modulation_heading_.setFont(UiFonts::heading());
        inspector_content_.addAndMakeVisible(modulation_heading_);
        modulation_issue_.setFont(UiFonts::body());
        modulation_issue_.setColour(
            juce::Label::textColourId, juce::Colour(0xFFE7A083));
        inspector_content_.addAndMakeVisible(modulation_issue_);
        constexpr std::array<const char*, 4> modulation_labels{
            "Pitch", "Tremolo", "TL", "FB"};
        for (std::size_t index = 0; index < modulation_labels.size(); ++index) {
            auto& label = modulation_labels_[index];
            label.setText(modulation_labels[index], juce::dontSendNotification);
            label.setFont(UiFonts::body());
            inspector_content_.addAndMakeVisible(label);
            auto& toggle = modulation_toggles_[index];
            toggle.setButtonText(modulation_labels[index]);
            toggle.setLookAndFeel(&modulation_switch_look_and_feel_);
            toggle.onClick = [this, index] {
                setTimelineModulationEnabled(
                    static_cast<TimelineModulationTarget>(index),
                    toggleStateForModulation(index));
            };
            inspector_content_.addAndMakeVisible(toggle);
            auto& setup = modulation_settings_[index];
            setup.setButtonText(juce::String::fromUTF8("設定"));
            setup.onClick = [this, index] {
                openTimelineModulationDialog(
                    static_cast<TimelineModulationTarget>(index));
            };
            inspector_content_.addAndMakeVisible(setup);
        }

        command_view_.setViewedComponent(&command_stack_, false);
        command_view_.setScrollBarsShown(true, false);
        inspector_content_.addAndMakeVisible(command_view_);
        rate_open_.setButtonText(juce::String::fromUTF8("@r レート編集を開く"));
        rate_open_.onClick = [this] {
            if (selected_layer_ >= 0) openRateEditor(static_cast<std::size_t>(selected_layer_));
        };
        inspector_content_.addAndMakeVisible(rate_open_);
        addChildComponent(envelope_mml_preview_);
        addAndMakeVisible(edit_mode_);
        addAndMakeVisible(parameter_);
        inspector_view_.setViewedComponent(&inspector_content_, false);
        inspector_view_.setScrollBarsShown(true, false);
        addAndMakeVisible(inspector_view_);
        overview_view_.setViewedComponent(&overview_content_, false);
        overview_view_.setScrollBarsShown(true, false);
        addAndMakeVisible(overview_view_);
        overview_content_.draw = [this](juce::Graphics& g) { paintOverview(g); };
        overview_content_.select = [this](juce::Point<int> point) {
            const int i = point.y / overviewRowHeight();
            if (i >= 0 && i < static_cast<int>(timbre_.layers.size())) selectOverviewLayer(i);
        };
        setup_tab_.setButtonText(juce::String::fromUTF8("初期設定"));
        count_tab_.setButtonText(juce::String::fromUTF8("カウント編集"));
        focus_.setButtonText(juce::String::fromUTF8("編集に集中"));
        for (auto* button : {&setup_tab_, &count_tab_, &focus_}) {
            button->setClickingTogglesState(true);
            addAndMakeVisible(button);
        }
        setup_tab_.onClick = [this] { setCountInspector(false); };
        count_tab_.onClick = [this] { setCountInspector(true); };
        focus_.onClick = [this] { resized(); repaint(); };
        setup_tab_.setToggleState(true, juce::dontSendNotification);

        horizontal_scroll_.setRangeLimits(
            0.0, static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
        horizontal_scroll_.setCurrentRange(
            0.0, kDefaultVisibleCounts, juce::dontSendNotification);
        horizontal_scroll_.setSingleStepSize(1.0);
        horizontal_scroll_.setAutoHide(false);
        horizontal_scroll_.addListener(this);
        vertical_scroll_.setSingleStepSize(
            static_cast<double>(UiLayout::sm));
        vertical_scroll_.setAutoHide(false);
        vertical_scroll_.addListener(this);
        addAndMakeVisible(horizontal_scroll_);
        addAndMakeVisible(vertical_scroll_);
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }

    ~Impl() override {
        cancelPendingDialogs();
        stopTimer();
        cancelMorphGeneration();
        if (morph_worker_.joinable()) {
            morph_worker_.request_stop();
            morph_worker_wakeup_.notify_all();
            morph_worker_.join();
        }
        closeRateEditor(false);
        for (auto& toggle : modulation_toggles_) {
            toggle.setLookAndFeel(nullptr);
        }
        pitch_lane_toggle_.setLookAndFeel(nullptr);
        horizontal_scroll_.removeListener(this);
        vertical_scroll_.removeListener(this);
    }

    void setInspectorVisible(bool visible) {
        if (inspector_visible_ == visible) return;
        inspector_visible_ = visible;
        resized(); repaint();
    }

    juce::String verifyWorkspace(const juce::File& directory) {
        juce::String failures;
        auto check = [&](bool ok, const char* message) {
            if (!ok) failures += juce::String(message) + "\n";
        };
        int callbacks = 0;
        setEditCallback([&](const auto&, bool, bool) { ++callbacks; });
        auto model = mgstc::engine::defaultCompositeTimbre();
        applySessionAllocationStarts(model);
        check(model.layers.size() == 3, "fixture must contain PSG/SCC/OPLL");
        auto ramp_fixture = model.layers.front();
        ramp_fixture.volume_envelope.events = {
            {.kind = mgstc::engine::EnvelopeEventKind::Volume,
             .value = 15, .count = 0},
            {.kind = mgstc::engine::EnvelopeEventKind::Volume,
             .value = 8, .count = 10, .automatic = true},
        };
        check(!isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 0)
                && isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 1)
                && isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 5)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 10)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 11),
            "automatic ramp origin/target or display-only mouse edit range is wrong");
        ramp_fixture.volume_envelope.events.insert(
            ramp_fixture.volume_envelope.events.begin() + 1,
            {.kind = mgstc::engine::EnvelopeEventKind::Volume,
             .value = 12, .count = 5});
        check(!isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 1)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 4)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 5)
                && isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 6)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 10)
                && ramp_fixture.volume_envelope.events.size() == 3,
            "manual ramp origin or preceding hold was incorrectly mouse-protected");
        ramp_fixture.volume_envelope.events.push_back(
            {.kind = mgstc::engine::EnvelopeEventKind::Volume,
             .value = 6, .count = 15});
        ramp_fixture.volume_envelope.events.push_back(
            {.kind = mgstc::engine::EnvelopeEventKind::Volume,
             .value = 2, .count = 20, .automatic = true});
        check(!isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 12)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 15)
                && isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 16)
                && !isDisplayOnlyAutomaticVolumeCount(ramp_fixture, 20),
            "automatic ramp protection did not restart at intervening manual origin");
        {
            auto layer = mgstc::engine::defaultCompositeTimbre().layers[0];
            layer.envelope_timeline.length_counts = 16;
            layer.volume_envelope.events = {
                {.kind = mgstc::engine::EnvelopeEventKind::Volume, .value = 15, .count = 0},
                {.kind = mgstc::engine::EnvelopeEventKind::Volume, .value = 0, .count = 10, .automatic = true},
                {.kind = mgstc::engine::EnvelopeEventKind::Volume, .value = 8, .count = 12}};
            const auto author = authorVolumeSamples(layer, 16);
            const auto actual = mgstc::engine::sampleCompositeVolumeLane(layer, 16);
            check(author[10] == 0 && author[12] == 8 && actual[12] == 0 && actual[13] == 8,
                "post-ramp author editing position or real tick overlay shifted");
        }
        for (auto& layer : model.layers) {
            layer.envelope_timeline.length_counts = 32;
            layer.volume_envelope.events.clear();
            for (const auto point : std::array<std::pair<int, int>, 4>{{{0, 15}, {6, 9}, {16, 4}, {24, 2}}}) {
                mgstc::engine::EnvelopeEvent event;
                event.kind = mgstc::engine::EnvelopeEventKind::Volume;
                event.count = static_cast<std::uint32_t>(point.first);
                event.value = point.second;
                event.automatic = point.first != 0;
                layer.volume_envelope.events.push_back(event);
            }
        }
        model.layers[0].envelope_timeline.loop_start_count = 6;
        model.layers[0].envelope_timeline.loop_end_count = 24;
        mgstc::engine::EnvelopeEvent pitch;
        pitch.kind = mgstc::engine::EnvelopeEventKind::Pitch;
        pitch.count = 6;
        pitch.value = 3;
        model.layers[0].pitch_envelope.events.push_back(pitch);
        pitch.after_loop_start = true;
        pitch.value = -2;
        model.layers[0].pitch_envelope.events.push_back(pitch);

        setTimbre(model, true);
        setVisible(true);
        const auto save = [&](const juce::String& name) {
            if (directory == juce::File{}) return;
            auto stream = directory.getChildFile(name + ".png").createOutputStream();
            check(stream != nullptr, "snapshot output could not be opened");
            if (stream) {
                check(stream->setPosition(0) && stream->truncate().wasOk(), "snapshot truncate failed");
                juce::PNGImageFormat png;
                check(png.writeImageToStream(createComponentSnapshot(getLocalBounds()), *stream),
                    "snapshot encoding failed");
            }
        };
        const int original_scale = UiScale::active_percent;
        for (const int percent : {75, 100, 125}) {
            UiScale::setActivePercent(percent);
            setSize(UiScale::sx(1528), UiScale::sx(750));
            refreshUiScaleFonts();
            const auto before = timbre_;
            const auto caption_bounds = editHeaderCaptionBounds();
            check(!caption_bounds.intersects(morph_bank_label_.getBounds())
                    && !caption_bounds.intersects(morph_bank_.getBounds())
                    && !caption_bounds.intersects(morph_bank_usage_.getBounds())
                    && !caption_bounds.intersects(opll_bank_label_.getBounds())
                    && !caption_bounds.intersects(opll_bank_.getBounds())
                    && !caption_bounds.intersects(opll_bank_usage_.getBounds())
                    && !caption_bounds.intersects(edit_mode_.getBounds())
                    && !caption_bounds.intersects(parameter_.getBounds()),
                "envelope header controls overlap the selected-channel caption");
            check(editHeaderArea().contains(morph_bank_label_.getBounds())
                    && editHeaderArea().contains(morph_bank_.getBounds())
                    && editHeaderArea().contains(morph_bank_usage_.getBounds())
                    && editHeaderArea().contains(opll_bank_label_.getBounds())
                    && editHeaderArea().contains(opll_bank_.getBounds())
                    && editHeaderArea().contains(opll_bank_usage_.getBounds())
                    && morph_bank_label_.getRight() <= morph_bank_.getX()
                    && morph_bank_.getRight() <= morph_bank_usage_.getX()
                    && morph_bank_usage_.getRight() <= opll_bank_label_.getX()
                    && opll_bank_label_.getRight() <= opll_bank_.getX()
                    && opll_bank_.getRight() <= opll_bank_usage_.getX()
                    && editHeaderArea().getBottom() < graphArea().getY(),
                "envelope allocation header controls overlap or escape their own row");
            check(caption_bounds.getY() == morph_bank_.getY() && morph_bank_.getY() == opll_bank_.getY()
                    && opll_bank_.getY() == edit_mode_.getY() && edit_mode_.getY() == parameter_.getY()
                    && opll_bank_usage_.getRight() <= edit_mode_.getX()
                    && edit_mode_.getRight() <= parameter_.getX(),
                "envelope header is not one row in caption/SCC/OPLL/tools order");
            const int natural_usage_width = juce::jmax(
                juce::GlyphArrangement::getStringWidthInt(morph_bank_usage_.getFont(), morph_bank_usage_.getText()),
                juce::GlyphArrangement::getStringWidthInt(opll_bank_usage_.getFont(), opll_bank_usage_.getText())) + UiLayout::xs * 2;
            check(morph_bank_usage_.getWidth() <= natural_usage_width
                    && opll_bank_label_.getX() == morph_bank_usage_.getRight() + UiLayout::controlGap,
                "SCC/OPLL settings are separated by expanded empty usage space");
            check(opll_bank_.getWidth() >= juce::GlyphArrangement::getStringWidthInt(UiFonts::body(), "@31")
                    + UiLayout::fieldH + UiLayout::xs * 2,
                "allocation selector width cannot show a two-digit number and arrow");
            check(morph_bank_.isVisible() && morph_bank_.getNumItems() == 32
                    && morph_bank_.getItemText(0) == "@0" && morph_bank_.getItemText(31) == "@31"
                    && opll_bank_.isVisible() && opll_bank_.getNumItems() == 16
                    && opll_bank_.getItemText(0) == "@16" && opll_bank_.getItemText(15) == "@31",
                "original allocation selectors have wrong source ranges or visibility");
            save("envelope-allocation-header-" + juce::String(percent));
            auto overflow = mgstc::engine::defaultCompositeTimbre();
            overflow.layers.clear();
            for (int index = 0; index < 2; ++index) {
                mgstc::engine::CompositeLayer channel;
                channel.source = mgstc::engine::TimbreSource::Scc;
                channel.channel = static_cast<std::uint8_t>(index);
                mgstc::engine::SavedTimbreReference wave;
                wave.source = channel.source;
                wave.library_id = static_cast<std::uint64_t>(100 + index);
                wave.number_mode = mgstc::engine::TimbreNumberMode::Automatic;
                wave.scc_waveform[0] = static_cast<std::uint8_t>(index);
                channel.base_timbre = wave;
                overflow.layers.push_back(channel);
            }
            setTimbre(overflow, true);
            const auto before_shortage = timbre_;
            morph_bank_.setSelectedId(32, juce::sendNotificationSync);
            check(timbre_ == before_shortage && morph_bank_.getSelectedId() == 1
                    && allocation_error_label_.isVisible()
                    && allocation_error_label_.getText().contains(juce::String::fromUTF8("必要2個／割当可能1個"))
                    && allocation_error_label_.getText().contains("SCC @31")
                    && allocation_error_label_.findColour(juce::Label::textColourId) == juce::Colours::red,
                "number shortage did not preserve the prior setting and show Japanese red inline counts");
            check(editHeaderArea().contains(allocation_error_label_.getBounds())
                    && allocation_error_label_.getY() > morph_bank_.getBottom()
                    && allocation_error_label_.getBottom() < graphArea().getY(),
                "red allocation error overlaps the selectors or envelope graph");
            save("envelope-allocation-shortage-" + juce::String(percent));
            morph_bank_.setSelectedId(31, juce::sendNotificationSync);
            check(!allocation_error_label_.isVisible() && allocation_starts_.scc == 30,
                "successful allocation did not clear the red inline error");
            morph_bank_.setSelectedId(1, juce::sendNotificationSync);
            overflow.layers[1].base_timbre->scc_waveform = overflow.layers[0].base_timbre->scc_waveform;
            setTimbre(overflow, true);
            morph_bank_.setSelectedId(32, juce::sendNotificationSync);
            const auto shared_numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
            check(shared_numbers.valid() && shared_numbers.assignments.size() == 2
                    && shared_numbers.assignments[0].number == 31 && shared_numbers.assignments[1].number == 31
                    && shared_numbers.automatic_allocations[0].required == 1
                    && !allocation_error_label_.isVisible(),
                "identical SCC waves were counted as two timbres or rejected at the last available number");
            morph_bank_.setSelectedId(1, juce::sendNotificationSync);
            setTimbre(model, true);
            // Verify actual painted ramp outlines, independently of the runtime
            // stair-step overlay and the author-count mouse boundary checks.
            const auto saved_visible = visibleCountRange();
            horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
            for (std::size_t layer_index = 0; layer_index < model.layers.size(); ++layer_index) {
                const juce::Rectangle<int> plot(12, 12, UiScale::sx(700), UiScale::sx(180));
                juce::Image image(juce::Image::ARGB,
                    plot.getRight() + 12, plot.getBottom() + 12, true);
                {
                    juce::Graphics graphics(image);
                    drawVolumeBars(graphics, plot, layer_index, juce::Colours::white);
                }
                const auto is_outline = [&](int x, int y) {
                    const auto pixel = image.getPixelAt(x, y);
                    return static_cast<int>(pixel.getGreen()) - pixel.getRed() > 35
                        && static_cast<int>(pixel.getGreen()) - pixel.getBlue() > 15;
                };
                const int x0 = xForCount(plot, 0), x1 = xForCount(plot, 6);
                const float y0 = volumeYFromValue(model.layers[layer_index].source, plot, 15);
                const float y1 = volumeYFromValue(model.layers[layer_index].source, plot, 9);
                for (const float fraction : {0.25F, 0.75F}) {
                    const int x = juce::roundToInt(x0 + (x1 - x0) * fraction);
                    const int y = juce::roundToInt(y0 + (y1 - y0)
                        * static_cast<float>(x - x0) / (x1 - x0));
                    check(is_outline(x, y) || is_outline(x, y - 1) || is_outline(x, y + 1),
                        "automatic volume outline does not connect author endpoints");
                }
                const int hold_x = (xForCount(plot, 26) + xForCount(plot, 27)) / 2;
                const int hold_y = juce::roundToInt(volumeYFromValue(
                    model.layers[layer_index].source, plot, 2));
                check(!is_outline(hold_x, hold_y),
                    "manual volume hold was incorrectly drawn as an automatic outline");
                check(timbre_ == before, "volume outline painting changed author data");

                // A nonzero secondary stores an imported ramp's start + 1.
                // The prefix must remain flat, then connect to the same target.
                auto& target = timbre_.layers[layer_index].volume_envelope.events[1];
                const int saved_secondary = target.secondary;
                target.secondary = 4;
                const auto delayed_before = timbre_;
                image.clear(image.getBounds());
                {
                    juce::Graphics graphics(image);
                    drawVolumeBars(graphics, plot, layer_index, juce::Colours::white);
                }
                const int prefix_x = (xForCount(plot, 1) + xForCount(plot, 2)) / 2;
                const int prefix_y = juce::roundToInt(y0);
                check(is_outline(prefix_x, prefix_y)
                        || is_outline(prefix_x, prefix_y - 1)
                        || is_outline(prefix_x, prefix_y + 1),
                    "imported automatic volume lost its flat prefix");
                const int ramp_x0 = xForCount(plot, 3);
                const int middle_x = (ramp_x0 + x1) / 2;
                const int middle_y = juce::roundToInt(y0 + (y1 - y0)
                    * static_cast<float>(middle_x - ramp_x0) / (x1 - ramp_x0));
                check(is_outline(middle_x, middle_y)
                        || is_outline(middle_x, middle_y - 1)
                        || is_outline(middle_x, middle_y + 1),
                    "imported automatic volume does not connect from its saved start");
                check(timbre_ == delayed_before,
                    "imported volume outline painting changed author metadata");
                target.secondary = saved_secondary;
            }
            horizontal_scroll_.setCurrentRange(saved_visible.getStart(),
                saved_visible.getLength(), juce::dontSendNotification);
            callbacks = 0;
            overview_select_[1]->onClick();
            check(selected_layer_ == 1, "overview selection failed");
            const auto editor = graphArea();
            const int normal_height = overviewRowHeight();
            compact_overview_.setToggleState(true, juce::dontSendNotification);
            compact_overview_.onClick();
            check(overviewRowHeight() < normal_height, "compact overview did not shrink rows");
            for (int i = 0; i < overview_select_.size(); ++i) {
                check(!overview_select_[i]->getBounds().intersects(overview_mute_[i]->getBounds()),
                    "compact channel name overlaps mute");
                check(overview_mute_[i]->getHeight() >= UiLayout::fieldH,
                    "compact controls lost their minimum height");
            }
            const auto compact_editor = graphArea();
            overview_view_.setViewPosition(0, overviewRowHeight());
            check(graphArea() == compact_editor && timbre_ == before && callbacks == 0,
                "compact navigation changed data or moved editor");
            save("workspace-compact-" + juce::String(percent));
            compact_overview_.setToggleState(false, juce::dontSendNotification);
            compact_overview_.onClick();
            check(graphArea() == editor, "normal overview did not restore layout");
            const auto frames = lane_frames_;
            auto overview_plot = juce::Rectangle<int>(0, 0, overview_content_.getWidth(), overviewRowHeight())
                .withTrimmedLeft(UiLayout::compositeOverviewLabelW).reduced(UiLayout::sm, UiLayout::xs);
            overview_plot.translate(overview_view_.getX(), overview_view_.getY());
            const auto detail_plot = timelinePlotBounds(graph_bounds_[1]);
            check(overview_plot.getX() == detail_plot.getX() && overview_plot.getWidth() == detail_plot.getWidth(),
                "overview and detail count columns are not aligned");
            overview_view_.setViewPosition(0, overviewRowHeight());
            check(graphArea() == editor && lane_frames_ == frames, "overview scroll moved editor");
            count_tab_.onClick();
            check(count_inspector_ && !layer_setups_[1]->isVisible(), "count tab did not hide setup");
            setup_tab_.onClick();
            focus_.setToggleState(true, juce::dontSendNotification);
            focus_.onClick();
            check(graphArea().getHeight() > editor.getHeight(), "focus mode did not enlarge editor");
            focus_.setToggleState(false, juce::dontSendNotification);
            focus_.onClick();
            setInspectorVisible(false);
            check(graphArea().getWidth() > editor.getWidth(), "library replacement did not reclaim inspector");
            setInspectorVisible(true);
            check(callbacks == 0 && timbre_ == before, "navigation changed model or issued edit callback");
            check(graphArea().getWidth() > UiScale::sx(700), "detail graph too narrow");
            check(inspector_view_.getBounds().getIntersection(graphArea()).isEmpty(), "inspector overlaps graph");
            mml_toggle_.setToggleState(true, juce::dontSendNotification);
            mml_toggle_.onClick();
            check(envelope_mml_preview_.isVisible() && !envelope_mml_preview_.getText().isEmpty(),
                "MGSC drawer is empty");
            const auto preview_bounds = envelope_mml_preview_.getBounds();
            const auto pinned_summary = makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source).summary;
            const auto pinned_toggle = step_edit_mode_.getBounds();
            const auto scrolling_volume = makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source).volume;
            vertical_scroll_.setCurrentRangeStart(100.0, juce::sendNotificationSync);
            check(envelope_mml_preview_.getBounds() == preview_bounds, "MGSC drawer moved with graph");
            check(makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source).summary == pinned_summary
                    && step_edit_mode_.getBounds() == pinned_toggle && step_edit_mode_.isVisible()
                    && layerAt(pinned_summary.getCentre()) == 1,
                "command row/toggle or its hit test moved with vertical scroll");
            if (vertical_scroll_.getCurrentRangeStart() > 0.0)
                check(makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source).volume.getY() < scrolling_volume.getY(),
                    "pinning command row prevented envelope body scrolling");
            check(callbacks == 0 && timbre_ == before, "MGSC navigation changed model");
            save("workspace-mml-" + juce::String(percent));
            mml_toggle_.setToggleState(false, juce::dontSendNotification);
            mml_toggle_.onClick();
            vertical_scroll_.setCurrentRangeStart(0.0, juce::sendNotificationSync);
            for (const auto& fixture : model.layers) {
                int changes = 0, previews = 0;
                auto rate_fixture = fixture;
                auto& rate = rate_fixture.volume_envelope.rate;
                rate.tone_mode = fixture.source == mgstc::engine::TimbreSource::Psg ? 1 : 0;
                rate.attack_level = 32; rate.attack_rate = 12; rate.decay_rate = 6;
                rate.sustain_level = 160; rate.sustain_rate = 1; rate.release_rate = 5;
                RateEnvelopeEditorContent rate_editor(rate_fixture,
                    [&](const auto&, bool) { ++changes; }, [&] { ++previews; }, {});
                rate_editor.setVisible(true);
                check(changes == 0 && previews == 0, "opening rate editor changed or played sound");
                const auto name = "rate-" + juce::String(static_cast<int>(fixture.source))
                    + "-" + juce::String(percent) + ".png";
                auto stream = directory.getChildFile(name).createOutputStream();
                check(stream != nullptr, "rate snapshot unavailable");
                if (stream) {
                    stream->setPosition(0); stream->truncate();
                    juce::PNGImageFormat format;
                    check(format.writeImageToStream(rate_editor.createComponentSnapshot(rate_editor.getLocalBounds()), *stream),
                        "rate snapshot failed");
                }
                failures += rate_editor.verifyEditing();
            }
            save("workspace-" + juce::String(percent));
        }
        UiScale::setActivePercent(100);
        setSize(1528, 750);
        selectOverviewLayer(0);
        selected_count_ = 6;
        syncPointEditors();
        syncInspector();
        count_tab_.onClick();
        const auto before_zone = timbre_;
        callbacks = 0;
        // Exercise the visible insertion-zone button through its existing callback.
        bool zone_clicked = false;
        for (int i = 0; i < command_stack_.getNumChildComponents(); ++i) {
            auto* button = dynamic_cast<juce::Button*>(command_stack_.getChildComponent(i));
            if (button && button->getButtonText().contains(juce::String::fromUTF8("毎周実行"))) {
                auto click = button->onClick;
                click(); zone_clicked = true; break;
            }
        }
        check(zone_clicked && loop_insert_after_, "vertical stack insertion button unavailable");
        check(timbre_ == before_zone && callbacks == 0, "insert zone changed existing commands");
        save("workspace-loop-commands");
        moveStackChipAt(0, true);
        check(timbre_.layers[0].pitch_envelope.events.size() == 2
            && timbre_.layers[0].pitch_envelope.events[0].after_loop_start
            && timbre_.layers[0].pitch_envelope.events[0].value == 3,
            "vertical stack move changed command value/order");
        setTimbre(model, true);
        selectOverviewLayer(0);
        const auto before_mute = timbre_;
        callbacks = 0;
        overview_mute_[0]->setToggleState(!timbre_.layers[0].muted, juce::dontSendNotification);
        overview_mute_[0]->onClick();
        auto expected = before_mute;
        expected.layers[0].muted = !expected.layers[0].muted;
        check(timbre_ == expected && callbacks == 1, "mute changed unrelated state or commit count");
        setTimbre(model, true);
        selectOverviewLayer(0);
        // Exercise real hit testing and the actual mouse edit path after relayout.
        const auto slots = makeEditLaneSlots(graph_bounds_[0], timbre_.layers[0].source);
        const auto volume_plot = graphStrip(slots.volume);
        for (int count = 0; count < 24; ++count) {
            const int left = xForCount(volume_plot, count);
            const int right = xForCount(volume_plot, count + 1);
            if (right > left)
                check(countAtX(volume_plot, left) == count
                    && countAtX(volume_plot, (left + right - 1) / 2) == count,
                    "count hit test differs from drawn cell boundary");
        }
        const auto point = juce::Point<float>(
            static_cast<float>(xForCount(volume_plot, 0)),
            static_cast<float>(volume_plot.getCentreY()));
        const auto now = juce::Time::getCurrentTime();
        const auto generated_point = juce::Point<float>(
            static_cast<float>(xForCount(volume_plot, 3)),
            static_cast<float>(volume_plot.getCentreY()));
        const juce::MouseEvent generated_event(
            juce::Desktop::getInstance().getMainMouseSource(), generated_point,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, this, this, now, generated_point, now, 1, false);
        callbacks = 0;
        mouseDown(generated_event); mouseUp(generated_event);
        check(callbacks == 0 && timbre_ == model,
            "mouse click changed a display-only automatic volume cell");
        const juce::MouseEvent event(
            juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, this, this, now, point, now, 1, false);
        callbacks = 0;
        mouseDown(event); mouseUp(event);
        check(callbacks > 0 && timbre_.layers[0] != model.layers[0], "volume mouse editing stopped working");
        check(timbre_.layers[1] == model.layers[1] && timbre_.layers[2] == model.layers[2],
            "mouse edit changed another channel");
        failures += verifyStepDrag();
        failures += verifyLoopInteraction(directory);
        failures += verifyOriginalAllocationStarts();
        setTimbre(model, true);
        for (int i = 0; i < 3; ++i) addLayer(mgstc::engine::TimbreSource::Psg);
        for (int i = 0; i < 5; ++i) addLayer(mgstc::engine::TimbreSource::Scc);
        for (int i = 0; i < 9; ++i) addLayer(mgstc::engine::TimbreSource::Opll);
        check(timbre_.layers.size() == 17, "17-channel fixture failed");
        const auto full = timbre_;
        callbacks = 0;
        auto full_menu = makeAddChannelMenu();
        for (juce::PopupMenu::MenuItemIterator it(full_menu); it.next();)
            check(!it.getItem().isEnabled, "full source remains enabled in add menu");
        for (int id = 0; id <= 3; ++id) addChannelFromMenu(id);
        check(timbre_ == full && callbacks == 0, "full/cancelled add changed data");
        compact_overview_.setToggleState(true, juce::dontSendNotification);
        compact_overview_.onClick();
        overview_select_[16]->onClick();
        save("workspace-17-compact");
        compact_overview_.setToggleState(false, juce::dontSendNotification);
        compact_overview_.onClick();
        overview_view_.setViewPosition(0, overview_content_.getHeight());
        const auto full_frames = lane_frames_;
        overview_view_.setViewPosition(0, 0);
        check(lane_frames_ == full_frames && callbacks == 0 && timbre_ == full,
            "17-channel overview scroll changed selected edit or data");
        count_tab_.onClick();
        parameter_.setSelectedId(static_cast<int>(Parameter::OpllTlAuto), juce::sendNotificationSync);
        check(modulation_toggles_[2].isVisible(),
            "TL selection did not show modulation controls");
        parameter_.setSelectedId(static_cast<int>(Parameter::Volume), juce::sendNotificationSync);
        check(modulation_toggles_[1].isVisible()
                && timbre_ == full && callbacks == 0,
            "modulation control navigation changed data or visibility");
        {
            for (const auto target : {TimelineModulationTarget::Pitch,
                    TimelineModulationTarget::Tremolo, TimelineModulationTarget::TotalLevel,
                    TimelineModulationTarget::Feedback}) {
                TimelineModulationWavePreview::Data data;
                data.target = target;
                for (auto& base : data.base) base = 5;
                for (auto& value : data.free_curve) value = 3;
                data.base[1].reset(); // ROM/unavailable intervals must remain gaps.
                TimelineModulationWavePreview wave(data);
                mgstc::engine::ModulationParameters settings;
                settings.enabled = true;
                settings.mode = mgstc::engine::ModulationMode::StepUp;
                settings.delay = 17;
                settings.speed = 1;
                settings.roughness = 8;
                wave.setSettings(settings, false);
                const int first = static_cast<int>(mgstc::engine::softwareLfoFirstUpdateTick(
                    settings.delay, settings.speed));
                const int maximum = target == TimelineModulationTarget::Pitch ? 32767
                    : target == TimelineModulationTarget::Tremolo ? 15
                    : target == TimelineModulationTarget::TotalLevel ? 63 : 7;
                check(wave.valueAt(first - 1) == 5
                        && wave.valueAt(first) == juce::jmin(maximum, 13)
                        && !wave.valueAt(1),
                    "modulation preview differs from effective step or fills unavailable interval");
                settings.enabled = false;
                wave.setSettings(settings, false);
                check(wave.valueAt(first) == 5, "disabled modulation preview changes base value");
                settings.enabled = true;
                wave.setSettings(settings, true);
                check(wave.valueAt(first) == 3, "free curve preview does not show actual lane value");
                int changes = 0;
                TimelineModulationDialogContent dialog(target, settings, false, std::move(data),
                    [&changes](const auto&, bool) { ++changes; });
                dialog.resized();
                check(dialog.verifyPreviewLayoutAndDelay(17) && changes == 0,
                    "opening modulation settings changed authoring data or hid delay/wave preview");
                if (directory != juce::File{}) {
                    auto stream = directory.getChildFile("modulation-"
                        + juce::String(static_cast<int>(target)) + ".png").createOutputStream();
                    if (stream) juce::PNGImageFormat{}.writeImageToStream(
                        dialog.createComponentSnapshot(dialog.getLocalBounds()), *stream);
                }
            }
        }
        save("workspace-17-count");
        focus_.setToggleState(true, juce::dontSendNotification); focus_.onClick();
        save("workspace-focus");
        {
            auto morph_model = mgstc::engine::defaultCompositeTimbre();
            applySessionAllocationStarts(morph_model);
            auto& scc = morph_model.layers[1];
            scc.timbre_automation.clear();
            mgstc::engine::SavedTimbreReference first_wave;
            first_wave.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(morph_model);
            first_wave.source = mgstc::engine::TimbreSource::Scc;
            first_wave.name = "Sine";
            for (std::size_t n = 0; n < first_wave.scc_waveform.size(); ++n)
                first_wave.scc_waveform[n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(
                    juce::roundToInt(96.0 * std::sin(juce::MathConstants<double>::twoPi * static_cast<double>(n) / 32.0))));
            scc.base_timbre = first_wave;
            auto second_wave = first_wave;
            second_wave.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(morph_model);
            second_wave.name = "Square";
            for (std::size_t n = 0; n < second_wave.scc_waveform.size(); ++n)
                second_wave.scc_waveform[n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(n < 16 ? 96 : -96));
            morph_model.embedded_timbres.push_back(second_wave);
            mgstc::engine::EnvelopeEvent first;
            first.kind = mgstc::engine::EnvelopeEventKind::Timbre;
            first.count = 2;
            first.value = 15;
            first.target_library_id = first_wave.library_id;
            auto arrival = first;
            arrival.count = 12;
            arrival.value = 16;
            arrival.target_library_id = second_wave.library_id;
            arrival.scc_morph.enabled = true;
            auto normal = first;
            normal.count = 22;
            scc.timbre_automation = {arrival, first, normal};
            setTimbre(morph_model, true);
            selectOverviewLayer(1);
            check(morph_preview_.valid && morph_preview_.used > 0,
                "morph capture fixture must have valid owned source waves and a generated bank");
            const juce::Rectangle<int> lane(0, 0, getWidth(), UiLayout::fieldH);
            const juce::Point<int> middle(
                (xForCount(lane, 2) + xForCount(lane, 12)) / 2, timbreMarkerY(lane, scc.source));
            check(morphLineArrivalAt(timbre_.layers[1], lane, middle) == 12,
                "morph line does not open arrival event");
            const juce::Point<int> first_label(loopStartX(lane, 2),
                lane.getY() + juce::roundToInt(UiFonts::dense(true).getHeight()) / 2);
            check(timbreMarkerCountAt(timbre_.layers[1], lane, first_label) == 2,
                "outgoing morph line steals source marker label");
            timbre_.layers[1].timbre_automation[0].scc_morph.enabled = false;
            check(!morphLineArrivalAt(timbre_.layers[1], lane, middle),
                "normal tone interval has morph hit target");
            check(morph_bank_usage_.isVisible(),
                "Morph Bank usage is not always visible");
            {
                auto base_model = morph_model;
                base_model.layers[1].base_timbre = first_wave;
                base_model.layers[1].timbre_automation = {arrival};
                setTimbre(base_model, true); selectOverviewLayer(1);
                const auto source = morphStartEvent(timbre_.layers[1],
                    timbre_.layers[1].timbre_automation.front());
                check(source && source->count == 0 && source->target_library_id == first_wave.library_id
                    && morph_preview_.valid && timbre_.layers[1].timbre_automation.size() == 1,
                    "first morph did not use initial tone without author @ insertion");
                const juce::Point<int> base_middle(
                    (xForCount(lane, 0) + xForCount(lane, 12)) / 2,
                    timbreMarkerY(lane, scc.source));
                check(morphLineArrivalAt(timbre_.layers[1], lane, base_middle) == 12,
                    "base-to-first-tone morph line lacks arrival hit target");
                save("workspace-base-scc-morph");
                auto confirmed_base = timbre_;
                confirmed_base.layers[1].timbre_automation.front().scc_morph.explicit_plan = morph_preview_.plans.front().result.plan;
                auto edited_base = confirmed_base;
                edited_base.layers[1].base_timbre->scc_waveform[0] ^= 1;
                check(mgstc::app::invalidateEditedSccMorphPlans(confirmed_base, edited_base)
                    && !edited_base.layers[1].timbre_automation.front().scc_morph.explicit_plan,
                    "initial waveform edit retained first morph's obsolete confirmed plan");
                setTimbre(morph_model, true); selectOverviewLayer(1);
            }
            const auto picker_scale = UiScale::active_percent;
            for (const int percent : {75, 100, 125}) {
                UiScale::setActivePercent(percent);
                EnvelopeTimbrePickContent picker(mgstc::engine::TimbreSource::Scc,
                    {}, {}, [](auto) {}, {}, {});
                EnvelopeTimbrePickContent::MorphSettings picker_settings;
                picker_settings.intermediate_count = 2;
                picker.configureMorph(picker_settings, "A", "B", 2,
                    [](auto settings, bool) {
                        return EnvelopeTimbrePickContent::MorphChangeResult{
                            settings.intermediate_count <= 2,
                            settings.intermediate_count <= 2 ? juce::String{} : juce::String("time slot overflow")};
                    }, false);
                const auto picker_failure = picker.verifyMorphControls();
                if (picker_failure.isNotEmpty()) failures += "morph picker " + juce::String(percent)
                    + "%: " + picker_failure + "\n";
                if (directory != juce::File{}) {
                    auto stream = directory.getChildFile("scc-morph-picker-" + juce::String(percent) + ".png").createOutputStream();
                    check(stream != nullptr, "morph picker snapshot output could not be opened");
                    if (stream) {
                        check(stream->setPosition(0) && stream->truncate().wasOk(), "morph picker snapshot truncate failed");
                        juce::PNGImageFormat png;
                        check(png.writeImageToStream(picker.createComponentSnapshot(picker.getLocalBounds()), *stream),
                            "morph picker snapshot encoding failed");
                    }
                }
                auto* tall_picker = new EnvelopeTimbrePickContent(mgstc::engine::TimbreSource::Scc,
                    std::vector<EnvelopeTimbreCatalogItem>{}, EnvelopeTimbreChoice{}, [](auto) {}, {}, {});
                tall_picker->configureMorph({}, "A", "B", 2,
                    [](auto, bool) { return EnvelopeTimbrePickContent::MorphChangeResult{}; }, false);
                ScrollableTimbrePickerContent scroll(tall_picker, UiLayout::fieldH * 10);
                check(scroll.verifyGeometry(), "tall morph picker does not scroll to all controls");
                EnvelopeTimbrePickContent impossible_picker(mgstc::engine::TimbreSource::Scc,
                    {}, {}, [](auto) {}, {}, {});
                auto impossible_settings = picker_settings;
                impossible_settings.enabled = true;
                impossible_settings.intermediate_count = 6;
                impossible_picker.configureMorph(impossible_settings, "A", "B", 2,
                    [](auto, bool) { return EnvelopeTimbrePickContent::MorphChangeResult{}; }, false);
                check(impossible_picker.verifyUnavailableMorphCount(),
                    "changing distribution mode silently reduced an impossible authored count");
            }
            UiScale::setActivePercent(picker_scale);
            auto apply_verified_morph = [&](std::size_t layer_index, int count, auto settings) {
                std::optional<EnvelopeTimbrePickContent::MorphChangeResult> completed;
                auto result = applyIncomingMorph(layer_index, count, settings, true,
                    [&](auto, auto outcome) { completed = std::move(outcome); });
                const auto deadline = juce::Time::getMillisecondCounterHiRes() + 120000.0;
                while (!completed && result.accepted && juce::Time::getMillisecondCounterHiRes() < deadline) {
                    timerCallback();
                    juce::Thread::sleep(1);
                }
                if (completed) return *completed;
                return EnvelopeTimbrePickContent::MorphChangeResult{false, "morph worker did not complete"};
            };
            const auto saved_edit_callback = edit_callback_;
            std::vector<mgstc::engine::CompositeTimbre> mode_history{timbre_};
            edit_callback_ = [&](const auto& edited, bool commit, bool) {
                if (commit) mode_history.push_back(edited);
            };
            auto mode_settings = arrival.scc_morph;
            mode_settings.intermediate_count = 2;
            mode_settings.distribution_mode = mgstc::engine::SccMorphDistributionMode::TimeDistribution;
            check(apply_verified_morph(1, 12, mode_settings).accepted,
                "Time Distribution change was not committed from shared timeline");
            mode_settings.distribution_mode = mgstc::engine::SccMorphDistributionMode::ToneDistribution;
            check(apply_verified_morph(1, 12, mode_settings).accepted,
                "Tone Distribution change was not committed from shared timeline");
            check(mode_history.size() == 3, "mode changes did not create independent Undo transactions");
            if (mode_history.size() == 3) {
                setTimbre(mode_history[1]);
                check(timbreEventAt(timbre_.layers[1], 12)->scc_morph.distribution_mode
                        == mgstc::engine::SccMorphDistributionMode::TimeDistribution,
                    "Undo snapshot did not restore per-interval distribution mode");
                setTimbre(mode_history[2]);
                check(timbreEventAt(timbre_.layers[1], 12)->scc_morph.distribution_mode
                        == mgstc::engine::SccMorphDistributionMode::ToneDistribution,
                    "Redo snapshot did not restore per-interval distribution mode");
            }
            std::size_t latest_commits{};
            edit_callback_ = [&](const auto&, bool commit, bool) { if (commit) ++latest_commits; };
            mode_settings.distribution_mode = mgstc::engine::SccMorphDistributionMode::TimeDistribution;
            static_cast<void>(applyIncomingMorph(1, 12, mode_settings, true));
            mode_settings.distribution_mode = mgstc::engine::SccMorphDistributionMode::AdaptiveDistribution;
            check(apply_verified_morph(1, 12, mode_settings).accepted && latest_commits == 1,
                "superseded morph generation published an old setting or extra Undo transaction");
            const auto restored_model = timbre_;
            static_cast<void>(applyIncomingMorph(1, 12, mode_settings, true));
            setTimbre(restored_model);
            timerCallback();
            check(timbre_ == restored_model && latest_commits == 1 && !morph_pending_edit_,
                "model reload did not cancel pending morph generation");
            edit_callback_ = saved_edit_callback;
            // Confirmed WAV plans are author data until their actual inputs
            // change. Exercise the same timeline commands used by both editions.
            auto confirmed = morph_model;
            auto& confirmed_events = confirmed.layers[1].timbre_automation;
            for (auto& event : confirmed_events) if (event.count == 12) {
                event.scc_morph.intermediate_count = 1;
                event.scc_morph.explicit_plan = mgstc::engine::SccMorphPlan{{{0.0, 0}, {0.3, 4}, {1.0, 10}}};
            }
            auto later = *timbreEventAt(confirmed.layers[1], 12);
            later.count = 32;
            confirmed_events.push_back(later);
            const auto has_plan = [&](int count) {
                const auto* event = timbreEventAt(timbre_.layers[1], static_cast<std::uint32_t>(count));
                return event && event->scc_morph.explicit_plan.has_value();
            };
            setTimbre(confirmed, true); selectOverviewLayer(1);
            check(has_plan(12) && has_plan(32), "loading confirmed WAV plans discarded unchanged intervals");
            applyTimbreChoice(12, {mgstc::engine::TimbrePick::Library, second_wave.library_id});
            check(has_plan(12) && has_plan(32), "no-op source selection invalidated a confirmed WAV plan");
            applyTimbreChoice(12, {mgstc::engine::TimbrePick::Library, first_wave.library_id});
            check(!has_plan(12) && has_plan(32), "endpoint edit failed to invalidate only its affected WAV interval");
            setTimbre(confirmed, true); selectOverviewLayer(1);
            applyTimbreChoice(7, {mgstc::engine::TimbrePick::Library, first_wave.library_id});
            check(!has_plan(12) && has_plan(32), "new keyframe retained the next interval's stale WAV plan");
            setTimbre(confirmed, true); selectOverviewLayer(1);
            removeTimbreAt(2);
            check(!has_plan(12) && has_plan(32), "deleting source keyframe retained its destination WAV plan");
            auto loop_confirmed = confirmed;
            loop_confirmed.layers[1].envelope_timeline.loop_start_count = 12;
            loop_confirmed.layers[1].envelope_timeline.loop_end_count = 40;
            setTimbre(loop_confirmed, true); selectOverviewLayer(1);
            selected_count_ = 12;
            moveStackChipAt(0, true);
            check(!has_plan(12) && has_plan(32), "loop phase edit invalidated the wrong WAV intervals");
            setTimbre(loop_confirmed, true); selectOverviewLayer(1);
            edit_mode_.setSelectedId(static_cast<int>(EditMode::LoopStart), juce::dontSendNotification);
            setTimelineMarker(14, true);
            check(!has_plan(12) && has_plan(32), "moving loop boundary discarded an unrelated confirmed plan");
            auto unchanged = confirmed;
            unchanged.name = "Metadata only";
            unchanged.favorite = !unchanged.favorite;
            unchanged.layers[1].volume = 10;
            check(!mgstc::app::invalidateEditedSccMorphPlans(confirmed, unchanged)
                    && unchanged.layers[1].timbre_automation == confirmed_events,
                "metadata/volume-only author edit discarded confirmed WAV placement");
            setTimbre(confirmed, true); selectOverviewLayer(1);
            auto changed_curve = timbreEventAt(timbre_.layers[1], 12)->scc_morph;
            changed_curve.curve = 0.5;
            check(apply_verified_morph(1, 12, changed_curve).accepted && !has_plan(12) && has_plan(32),
                "curve edit failed to regenerate only its confirmed WAV interval");
            auto recovery_model = morph_model;
            for (auto& event : recovery_model.layers[1].timbre_automation) {
                event.scc_morph.enabled = event.count != 22;
                if (event.count == 12) event.scc_morph.intermediate_count = 255;
            }
            setTimbre(recovery_model, true);
            selectOverviewLayer(1);
            EnvelopeTimbrePickContent::MorphSettings reduced;
            reduced.enabled = true;
            reduced.intermediate_count = 1;
            const auto reduced_result = apply_verified_morph(1, 2, reduced);
            check(reduced_result.accepted && reduced_result.error.isNotEmpty(),
                "invalid morph state cannot reduce one interval while another error remains");
            auto increased = reduced;
            increased.intermediate_count = 2;
            const auto before_increase = timbre_;
            const auto increase_result = apply_verified_morph(1, 2, increased);
            check(!increase_result.accepted && timbre_ == before_increase,
                "invalid morph recovery accepted an increased intermediate count");
            reduced.enabled = false;
            const auto first_disabled = apply_verified_morph(1, 2, reduced);
            check(first_disabled.accepted && first_disabled.error.isNotEmpty() && !morph_preview_.valid,
                "disabling one invalid transition lost error or failed to retain the edit");
            const auto second_disabled = apply_verified_morph(1, 12, reduced);
            check(second_disabled.accepted && second_disabled.error.isEmpty() && morph_preview_.valid,
                "disabling the remaining invalid transition did not repair the morph state");
            reduced.enabled = true;
            reduced.intermediate_count = 2; // count0-to-2 cannot hold two intermediate states.
            const auto repaired = timbre_;
            const auto invalid_new = apply_verified_morph(1, 2, reduced);
            check(!invalid_new.accepted && timbre_ == repaired,
                "valid model accepted a new invalid morph proposal");
            setTimbre(morph_model, true);
            selectOverviewLayer(1);
            parameter_.setSelectedId(static_cast<int>(Parameter::Timbre), juce::sendNotificationSync);
            const auto capture_slots = makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source);
            const auto scroll_to_tone = vertical_scroll_.getCurrentRangeStart()
                + capture_slots.timbre.getBottom() - graphArea().getBottom() + UiLayout::sm;
            vertical_scroll_.setCurrentRangeStart(juce::jmax(0.0, scroll_to_tone), juce::dontSendNotification);
            scrollBarMoved(&vertical_scroll_, vertical_scroll_.getCurrentRangeStart());
            const auto visible_tone_lane = graphStrip(makeEditLaneSlots(graph_bounds_[1], timbre_.layers[1].source).timbre);
            check(graphArea().contains(visible_tone_lane.getCentre()) && morph_preview_.valid,
                "morph capture must show the enabled interval in a valid tone lane");
            const juce::Point<int> visible_morph_line(
                (xForCount(visible_tone_lane, 2) + xForCount(visible_tone_lane, 12)) / 2,
                timbreMarkerY(visible_tone_lane, timbre_.layers[1].source));
            const juce::Point<int> visible_normal_gap(
                (xForCount(visible_tone_lane, 12) + xForCount(visible_tone_lane, 22)) / 2,
                timbreMarkerY(visible_tone_lane, timbre_.layers[1].source));
            check(morphLineArrivalAt(timbre_.layers[1], visible_tone_lane, visible_morph_line) == 12
                    && !morphLineArrivalAt(timbre_.layers[1], visible_tone_lane, visible_normal_gap),
                "rendered morph interval and normal gap do not match hit testing");
            save("workspace-scc-morph");
        }
        {
            auto audition_model = mgstc::engine::defaultCompositeTimbre();
            applySessionAllocationStarts(audition_model);
            mgstc::engine::SavedTimbreReference target;
            target.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(audition_model);
            target.source = mgstc::engine::TimbreSource::Scc;
            target.name = "Picker target";
            target.scc_waveform.fill(42);
            audition_model.embedded_timbres.push_back(target);
            mgstc::engine::EnvelopeEvent at;
            at.kind = mgstc::engine::EnvelopeEventKind::Timbre;
            at.count = 8;
            at.target_library_id = target.library_id;
            audition_model.layers[1].timbre_automation.push_back(at);
            setTimbre(audition_model, true);
            const auto before = timbre_;
            std::optional<mgstc::engine::CompositeTimbre> played;
            const auto saved_preview = manual_y_preview_;
            manual_y_preview_ = [&](auto candidate, std::uint8_t note, bool one_second) {
                check(note == 60 && !one_second, "picker performance changed note or gate mode");
                played = std::move(candidate);
            };
            EnvelopeTimbreChoice choice;
            choice.library_id = target.library_id;
            previewTimbreChoice(1, choice, 60);
            check(played && played->layers[1].base_timbre
                    && played->layers[1].base_timbre->scc_waveform == target.scc_waveform
                    && played->layers[1].timbre_automation.empty(),
                "picker did not audition selected embedded tone from key-on");
            check(timbre_ == before && played && played->layers[0] == before.layers[0]
                    && played->layers[2] == before.layers[2]
                    && played->layers[1].volume_envelope == before.layers[1].volume_envelope,
                "picker performance altered authoring state, other channels or ENV");
            choice.pick = mgstc::engine::TimbrePick::OpllRom;
            choice.rom_number = 4;
            played.reset();
            previewTimbreChoice(2, choice, 60);
            check(played && !played->layers[2].base_timbre
                    && played->layers[2].base_opll_rom == 4 && timbre_ == before,
                "picker ROM audition changed authored state or retained original base");
            manual_y_preview_ = saved_preview;
        }
        setTimbre(model, true); selectOverviewLayer(0);
        {
            const auto before = timbre_;
            callbacks = 0;
            const auto original_length = visibleCountRange().getLength();
            zoom_in_.onClick();
            const auto enlarged_length = visibleCountRange().getLength();
            zoom_out_.onClick();
            check(enlarged_length <= original_length
                && visibleCountRange().getLength() >= enlarged_length
                && timbre_ == before && callbacks == 0,
                "step zoom buttons changed authoring state or zoom direction");
            check(zoom_in_.getX() >= mml_toggle_.getRight()
                && zoom_out_.getX() >= zoom_in_.getRight(),
                "step zoom buttons are not placed after MGSC preview");
        }
        const auto normal_volume_height = makeEditLaneSlots(
            graph_bounds_[0], model.layers[0].source).volume.getHeight();
        {
            int selected_previews = 0, loaded = 0;
            EnvelopeTimbreChoice preset_choice;
            EnvelopeTimbrePickContent picker(mgstc::engine::TimbreSource::Scc,
                {}, {}, [&](auto choice) { ++loaded; preset_choice = choice; }, {}, {},
                {}, {}, {}, {}, false, nullptr, {}, {}, {}, {}, {}, {},
                [&](auto choice) { if (choice.scc_preset) ++selected_previews; });
            check(picker.verifyPresetSelection() && selected_previews == 6 && loaded == 1,
                "SCC preset controls did not select, preview and load all six presets");
            setTimbre(model, true); selectOverviewLayer(1);
            const auto before = timbre_;
            applyTimbreChoice(12, preset_choice);
            const auto* event = timbreEventAt(timbre_.layers[1], 12);
            const auto* reference = event ? mgstc::engine::findEmbeddedTimbreSnapshot(
                timbre_, event->target_library_id) : nullptr;
            check(reference && reference->source == mgstc::engine::TimbreSource::Scc
                && reference->scc_waveform == presetReference(before,
                    mgstc::engine::SccWavePreset::Pulse12_5).scc_waveform,
                "SCC preset load did not persist an owned waveform");
            std::optional<mgstc::engine::CompositeTimbre> played;
            const auto saved_preview = manual_y_preview_;
            manual_y_preview_ = [&](auto candidate, auto, bool one_second) {
                check(one_second, "selection audition did not use one-second preview");
                played = std::move(candidate);
            };
            const auto authored = timbre_;
            previewTimbreChoice(1, preset_choice, 60, true);
            check(played && played->layers.size() == 1
                && played->layers[0].source == mgstc::engine::TimbreSource::Scc
                && played->layers[0].base_timbre && timbre_ == authored,
                "preset selection preview was not a temporary single tone");
            manual_y_preview_ = saved_preview;
            setTimbre(before, true); selectOverviewLayer(1);
            timbre_.layers[1].base_timbre = presetReference(timbre_, mgstc::engine::SccWavePreset::Triangle);
            timbre_.layers[1].base_timbre->number_mode = mgstc::engine::TimbreNumberMode::Manual;
            timbre_.layers[1].base_timbre->manual_number = std::uint8_t{23};
            const auto old_base = *timbre_.layers[1].base_timbre;
            mgstc::engine::EnvelopeEvent old_at;
            old_at.kind = mgstc::engine::EnvelopeEventKind::Timbre;
            old_at.count = 10; old_at.value = 23; old_at.target_library_id = 0;
            timbre_.layers[1].timbre_automation.push_back(old_at);
            int base_previews = 0;
            EnvelopeTimbrePickContent base_picker(mgstc::engine::TimbreSource::Scc,
                {}, {}, [&](auto choice) { applyLayerBaseChoice(1, choice); }, {}, {},
                {}, {}, {}, {}, true, nullptr, {}, {}, [&] { ++base_previews; });
            check(base_picker.verifyPresetSelection() && base_previews == 7
                && timbre_.layers[1].base_timbre
                && timbre_.layers[1].base_timbre->scc_waveform == presetReference(before,
                    mgstc::engine::SccWavePreset::Pulse12_5).scc_waveform,
                "base SCC preset assignment or existing one-second audition not connected");
            const auto* retained = mgstc::engine::findEmbeddedTimbreSnapshot(timbre_, old_base.library_id);
            check(retained && retained->scc_waveform == old_base.scc_waveform,
                "base preset replacement lost a referenced prior @ waveform");
            check(mgstc::engine::resolveTimbreNumbers(timbre_).warnings.empty()
                && timbre_.layers[1].timbre_automation.back().target_library_id == old_base.library_id
                && timbre_.layers[1].base_timbre->number_mode == mgstc::engine::TimbreNumberMode::Automatic,
                "base preset replacement collided with retained manual tone or rebound raw @ incorrectly");
            mgstc::engine::EngineCore preset_engine;
            check(mgstc::engine::formatMgsComposite(timbre_).valid()
                && mgstc::engine::compileCompositeProgram(preset_engine, timbre_),
                "manual base preset replacement broke export or audition preparation");
            auto shared_base = before;
            shared_base.layers[1].base_timbre = old_base;
            auto other = shared_base.layers[1]; other.channel = 1; other.envelope_number = 3;
            other.timbre_automation.clear(); shared_base.layers.push_back(other);
            setTimbre(shared_base, true); selectOverviewLayer(1);
            applyLayerBaseChoice(1, preset_choice);
            check(timbre_.layers.back().base_timbre == std::optional{old_base}
                && mgstc::engine::resolveTimbreNumbers(timbre_).warnings.empty()
                && mgstc::engine::formatMgsComposite(timbre_).valid()
                && mgstc::engine::compileCompositeProgram(preset_engine, timbre_),
                "base preset replacement damaged another channel's shared manual tone");
            setTimbre(before, true); selectOverviewLayer(0);
        }
        model.layers[0].volume_envelope.kind = mgstc::engine::EnvelopeKind::Rate;
        setTimbre(model, true); selectOverviewLayer(0); setup_tab_.onClick();
        const auto rate_slots = makeEditLaneSlots(graph_bounds_[0], model.layers[0].source);
        check(rate_slots.volume.getHeight() == normal_volume_height,
            "rate preview height differs from volume editing");
        check(rate_slots.pitch.isEmpty() && rate_slots.timbre.isEmpty()
                && rate_slots.volume.getBottom() == graph_bounds_[0].getBottom(),
            "rate preview retains unused lane space");
        check(graphArea().getHeight() == laneHeightForIndex(0)
                && horizontal_scroll_.getY() == graphArea().getBottom(),
            "rate preview outer whitespace was not compacted");
        save("workspace-rate");
        {
            setTimbre(model, true); selectOverviewLayer(1);
            const auto volume = graphStrip(makeEditLaneSlots(
                graph_bounds_[1], timbre_.layers[1].source).volume);
            parameter_.setSelectedId(static_cast<int>(Parameter::Volume), juce::dontSendNotification);
            hover_mouse_pos_ = volume.getCentre(); hover_preview_value_ = 7;
            const auto popup = hoverPopupBounds();
            if (popup) {
                juce::Image image(juce::Image::ARGB, getWidth(), getHeight(), true);
                const auto measure = [&](bool restore) {
                    const auto start = juce::Time::getHighResolutionTicks();
                    for (int i = 0; i < 200; ++i) {
                        juce::Graphics graphics(image);
                        graphics.reduceClipRegion(popup->expanded(6));
                        if (restore) {
                            hover_preview_value_.reset(); paint(graphics);
                            hover_preview_value_ = 7; drawHoverValuePopup(graphics);
                        } else drawHoverValuePopup(graphics);
                    }
                    return juce::Time::highResolutionTicksToSeconds(
                        juce::Time::getHighResolutionTicks() - start) * 1000.0 / 200.0;
                };
                {
                    juce::Graphics graphics(image); paint(graphics);
                }
                auto clean = image.createCopy();
                for (int repeat = 0; repeat < 3; ++repeat) {
                    juce::Graphics graphics(image);
                    graphics.reduceClipRegion(popup->expanded(6)); paint(graphics);
                }
                bool popup_restored = true;
                for (int y = popup->getY(); y < popup->getBottom(); ++y)
                    for (int x = popup->getX(); x < popup->getRight(); ++x)
                        popup_restored = popup_restored
                            && image.getPixelAt(x, y) == clean.getPixelAt(x, y);
                check(popup_restored, "partial popup repaint accumulated text or background pixels");
                measure(false); measure(true);
                const double current_ms = measure(false), restored_ms = measure(true);
                directory.getChildFile("hover-repaint-benchmark.txt").replaceWithText(
                    "CPU offscreen JUCE image; same popup clip; 200 paints after warmup\ncurrent_ms="
                    + juce::String(current_ms, 6) + "\nrestored_ms=" + juce::String(restored_ms, 6)
                    + "\nratio=" + juce::String(restored_ms / juce::jmax(0.000001, current_ms), 3) + "\n");
            }
            hover_preview_value_.reset(); hover_mouse_pos_.reset();
        }

        model.layers.clear(); setTimbre(model, true);
        check(selected_layer_ == -1 && !envelope_mml_preview_.isVisible(), "empty model retained editable controls");
        check(psg_add_.isVisible() && scc_add_.isVisible() && opll_add_.isVisible()
            && !add_channel_.isVisible(), "empty workspace lost explicit add buttons");
        save("workspace-empty");
        callbacks = 0;
        addChannelFromMenu(0);
        check(timbre_.layers.empty() && callbacks == 0, "cancelled add changed empty sound");
        for (int id = 1; id <= 3; ++id) addChannelFromMenu(id);
        check(timbre_.layers.size() == 3 && callbacks == 3
            && timbre_.layers[0].source == mgstc::engine::TimbreSource::Psg
            && timbre_.layers[1].source == mgstc::engine::TimbreSource::Scc
            && timbre_.layers[2].source == mgstc::engine::TimbreSource::Opll,
            "add menu changed source mapping or edit count");
        check(add_channel_.isVisible() && !psg_add_.isVisible(), "nonempty add menu not visible");
        UiScale::setActivePercent(original_scale);
        setEditCallback({});
        return failures;
    }

    void setEditCallback(EditCallback callback) {
        last_published_timbre_ = timbre_;
        edit_callback_ = std::move(callback);
    }

    void publishEdit(bool commit, bool preview) {
        const auto plan_count = [](const auto& sound) {
            std::size_t count{};
            for (const auto& layer : sound.layers)
                for (const auto& event : layer.timbre_automation)
                    count += event.scc_morph.explicit_plan.has_value() ? 1 : 0;
            return count;
        };
        const bool invalidated = mgstc::app::invalidateEditedSccMorphPlans(last_published_timbre_, timbre_)
            || pending_plan_invalidation_
            || plan_count(timbre_) < plan_count(last_published_timbre_);
        pending_plan_invalidation_ = false;
        last_published_timbre_ = timbre_;
        if (edit_callback_) edit_callback_(timbre_, commit, preview);
        if (invalidated) reportStatus(juce::String::fromUTF8(
            "編集した区間はWAV確定配置を解除し、共通モーフ処理で再生成します"));
    }

    void setStatusCallback(StatusCallback callback) {
        status_callback_ = std::move(callback);
    }

    void setTimbreCatalogCallback(CatalogCallback callback) {
        catalog_callback_ = std::move(callback);
    }

    void setOpenTimbreCallback(OpenTimbreCallback callback) {
        open_timbre_callback_ = std::move(callback);
    }

    void setAssignTimbreCallback(AssignTimbreCallback callback) {
        assign_timbre_callback_ = std::move(callback);
    }

    void setTimbreNameCallback(TimbreNameCallback callback) {
        timbre_name_callback_ = std::move(callback);
    }

    void setTimbreLibraryCallback(TimbreLibraryCallback callback) {
        timbre_library_callback_ = std::move(callback);
        refreshCountCeilingCache();
        updateEnvelopeMmlPreview();
        repaint();
    }

    void setManageTagsCallback(ManageTagsCallback callback) {
        manage_tags_callback_ = std::move(callback);
    }

    void setLibraryReplaceCallback(LibraryReplaceCallback callback) {
        library_replace_callback_ = std::move(callback);
    }

    void setBaseTimbrePerformance(
        std::function<void(std::uint8_t)> note_on,
        std::function<void(std::uint8_t)> note_off,
        std::function<void()> audition) {
        base_note_on_ = std::move(note_on);
        base_note_off_ = std::move(note_off);
        base_audition_ = std::move(audition);
    }

    void setManualYPerformance(
        ManualYPreviewCallback preview,
        std::function<void(std::uint8_t)> note_off,
        std::function<void()> end) {
        manual_y_preview_ = std::move(preview);
        manual_y_note_off_ = std::move(note_off);
        manual_y_end_ = std::move(end);
    }

    [[nodiscard]] std::optional<std::uint64_t> addBlankOriginal(
        std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return std::nullopt;
        }
        auto& layer = timbre_.layers[layer_index];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return std::nullopt;
        }
        mgstc::engine::SavedTimbreReference reference;
        reference.library_id =
            mgstc::engine::allocateCompositeOwnedTimbreId(timbre_);
        reference.revision = 1;
        reference.name = "オリジナル";
        reference.source = layer.source;
        reference.number_mode = mgstc::engine::TimbreNumberMode::Automatic;
        if (layer.source == mgstc::engine::TimbreSource::Opll) {
            reference.opll_registers = mgstc::engine::encodeOpllPatch(
                mgstc::engine::defaultOpllPatch());
            layer.base_opll_rom.reset();
        }
        const auto id = reference.library_id;
        layer.base_timbre = std::move(reference);
        if (static_cast<int>(layer_index) < layer_setups_.size()) {
            layer_setups_[static_cast<int>(layer_index)]->syncFromLayer(
                layer);
        }
        if (edit_callback_) {
            publishEdit(true, false);
        }
        if (base_audition_) {
            base_audition_();
        }
        return id;
    }

    [[nodiscard]] std::optional<std::uint64_t> addBlankOriginalAtStep(std::size_t layer_index, int count) {
        if (layer_index >= timbre_.layers.size()) return std::nullopt;
        const auto source = timbre_.layers[layer_index].source;
        if (source == mgstc::engine::TimbreSource::Psg) return std::nullopt;
        mgstc::engine::SavedTimbreReference reference;
        reference.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(timbre_);
        reference.revision = 1;
        reference.name = "オリジナル";
        reference.source = source;
        reference.number_mode = mgstc::engine::TimbreNumberMode::Automatic;
        if (source == mgstc::engine::TimbreSource::Opll)
            reference.opll_registers = mgstc::engine::encodeOpllPatch(mgstc::engine::defaultOpllPatch());
        const auto id = reference.library_id;
        timbre_.embedded_timbres.push_back(std::move(reference));
        selected_layer_ = static_cast<int>(layer_index);
        applyTimbreChoice(count, {.pick = mgstc::engine::TimbrePick::Library, .library_id = id});
        return id;
    }

    void setMutateTimbreNameCallback(MutateTimbreNameCallback callback) {
        mutate_name_callback_ = std::move(callback);
    }

    void setMutateTimbreTagsCallback(MutateTimbreTagsCallback callback) {
        mutate_tags_callback_ = std::move(callback);
    }

    void setMutateTimbreMemoCallback(MutateTimbreMemoCallback callback) {
        mutate_memo_callback_ = std::move(callback);
    }

    void setLfoSessionCallback(LfoSessionCallback callback) {
        lfo_session_callback_ = std::move(callback);
    }

    void setLfoPollKeysCallback(LfoPollKeysCallback callback) {
        lfo_poll_keys_callback_ = std::move(callback);
    }

    void closeRateEditor(bool notify_session) {
        rate_editor_layer_.reset();
        rate_window_.reset();
        if (notify_session && lfo_session_callback_) {
            lfo_session_callback_(std::nullopt, false, false);
        }
    }

    void openRateEditor(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Rate) {
            mgstc::engine::seedDefaultRateEnvelope(
                layer.volume_envelope.rate, layer.source);
            layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Rate;
            if (static_cast<int>(layer_index) < layer_setups_.size()) {
                layer_setups_[static_cast<int>(layer_index)]
                    ->syncFromLayer(layer);
            }
            if (edit_callback_) {
                publishEdit(true, false);
            }
        }
        selected_layer_ = static_cast<int>(layer_index);
        UiScale::forceGlobalForNonEditorUi();
        if (rate_window_ != nullptr
            && rate_editor_layer_ == layer_index) {
            rate_window_->toFront(true);
            if (auto* editor = rate_window_->editor()) {
                editor->assignFromLayer(layer);
            }
            if (lfo_session_callback_) {
                lfo_session_callback_(layer_index, false, false);
            }
            return;
        }
        closeRateEditor(false);
        rate_editor_layer_ = layer_index;
        auto* content = new RateEnvelopeEditorContent(
            layer,
            [this, layer_index](
                const mgstc::engine::RateEnvelope& rate, bool commit) {
                if (layer_index >= timbre_.layers.size()) {
                    return;
                }
                timbre_.layers[layer_index].volume_envelope.rate = rate;
                timbre_.layers[layer_index].volume_envelope.kind =
                    mgstc::engine::EnvelopeKind::Rate;
                if (static_cast<int>(layer_index) < layer_setups_.size()) {
                    layer_setups_[static_cast<int>(layer_index)]
                        ->syncFromLayer(timbre_.layers[layer_index]);
                }
                updateEnvelopeMmlPreview();
                repaint();
                if (edit_callback_) {
                    publishEdit(commit, false);
                }
            },
            [this, layer_index] {
                if (lfo_session_callback_) {
                    lfo_session_callback_(layer_index, true, false);
                }
            },
            [this] {
                if (lfo_poll_keys_callback_) {
                    lfo_poll_keys_callback_();
                }
            });
        rate_window_ = std::make_unique<RateEnvelopeEditorWindow>(
            content,
            [this] {
                closeRateEditor(true);
            });
        if (lfo_session_callback_) {
            lfo_session_callback_(layer_index, false, false);
        }
    }

    void syncRateEditorFromModel() {
        if (!rate_editor_layer_
            || *rate_editor_layer_ >= timbre_.layers.size()
            || rate_window_ == nullptr) {
            closeRateEditor(false);
            return;
        }
        const auto& layer = timbre_.layers[*rate_editor_layer_];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Rate) {
            closeRateEditor(true);
            return;
        }
        if (auto* editor = rate_window_->editor()) {
            editor->assignFromLayer(layer);
        }
    }

    [[nodiscard]] mgstc::engine::PsgSccModulationBase modulationBaseForLayer(
        const mgstc::engine::CompositeLayer& layer) const {
        const int midi = juce::jlimit(
            24,
            119,
            static_cast<int>(audition_note_) + layer.relative_semitones);
        mgstc::engine::PsgSccModulationBase base{};
        static_cast<void>(mgstc::engine::psgSccModulationBase(
            static_cast<std::uint8_t>(midi),
            layer.micro_detune,
            base));
        return base;
    }

    void openSoftwareLfoDialog(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        selected_layer_ = static_cast<int>(layer_index);
        if (lfo_session_callback_) {
            lfo_session_callback_(layer_index, true, true);
        }
        UiScale::forceGlobalForNonEditorUi();
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("ソフトウェア LFO"),
            juce::Colour(0xFF1B222C));
        auto* content = new SoftwareLfoDialogContent(
            layer.source,
            layer.software_lfo,
            modulationBaseForLayer(layer),
            layer.detune,
            [this, layer_index](
                const mgstc::engine::SoftwareLfoSettings& settings) {
                if (layer_index >= timbre_.layers.size()) {
                    return;
                }
                timbre_.layers[layer_index].software_lfo = settings;
                if (settings.enabled) {
                    timbre_.layers[layer_index].pitch_sweep.enabled = false;
                }
                if (static_cast<int>(layer_index) < layer_setups_.size()) {
                    layer_setups_[static_cast<int>(layer_index)]
                        ->syncFromLayer(timbre_.layers[layer_index]);
                }
                repaint();
                if (edit_callback_) {
                    publishEdit(true, false);
                }
            },
            [this, layer_index] {
                if (lfo_session_callback_) {
                    lfo_session_callback_(layer_index, true, true);
                }
            },
            [this] {
                if (lfo_poll_keys_callback_) {
                    lfo_poll_keys_callback_();
                }
            });
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(
            true,
            juce::ModalCallbackFunction::create(
                [this](int) {
                    if (lfo_session_callback_) {
                        lfo_session_callback_(std::nullopt, false, false);
                    }
                }),
            true);
    }

    void setEditorSession(mgstc::app::EditorSession* session) noexcept {
        session_ = session;
    }

    void setPlaybackTempo(int tempo_bpm) {
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_bpm);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        repaint();
    }

    void cancelPendingDialogs() {
        cancelLoopDrag();
        cancelStepDrag();
        pending_auto_settings_.reset();
        ++dialog_generation_;
        command_stack_.cancelPendingActions();
        closeRateEditor(false);
        auto dialogs = std::move(pending_dialogs_);
        pending_dialogs_.clear();
        for (auto& dialog : dialogs)
            if (dialog) delete dialog.getComponent();
    }

    void applySessionAllocationStarts(mgstc::engine::CompositeTimbre& model) const {
        model.original_timbre_allocation_starts = allocation_starts_;
    }

    void setAllocationError(const juce::String& error) {
        allocation_error_label_.setText(error, juce::dontSendNotification);
        allocation_error_label_.setTooltip(error);
        const bool visibility_changed = allocation_error_label_.isVisible() != error.isNotEmpty();
        allocation_error_label_.setVisible(error.isNotEmpty());
        if (visibility_changed) resized();
        repaint();
    }

    void changeOriginalAllocationStarts() {
        const mgstc::engine::OriginalTimbreAllocationStarts proposed{
            static_cast<std::uint8_t>(juce::jlimit(0, 31, morph_bank_.getSelectedId() - 1)),
            static_cast<std::uint8_t>(juce::jlimit(16, 31, opll_bank_.getSelectedId() - 1))};
        if (proposed == allocation_starts_) return;
        auto candidate = timbre_;
        candidate.original_timbre_allocation_starts = proposed;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(candidate);
        const auto morph = *mgstc::engine::compileSccMorphCached(candidate);
        if (!numbers.valid() || !morph.valid) {
            const bool scc = proposed.scc != allocation_starts_.scc;
            const auto source = scc ? mgstc::engine::TimbreSource::Scc : mgstc::engine::TimbreSource::Opll;
            const int attempted = scc ? proposed.scc : proposed.opll;
            const int previous = scc ? allocation_starts_.scc : allocation_starts_.opll;
            auto error = juce::String(scc ? "SCC @" : "OPLL @") + juce::String(attempted);
            const auto shortage = std::find_if(numbers.automatic_allocations.begin(), numbers.automatic_allocations.end(),
                [source](const auto& capacity) { return capacity.source == source && capacity.required > capacity.available; });
            if (shortage != numbers.automatic_allocations.end()) {
                error += juce::String::fromUTF8("：番号不足 必要") + juce::String(static_cast<int>(shortage->required))
                    + juce::String::fromUTF8("個／割当可能") + juce::String(static_cast<int>(shortage->available)) + juce::String::fromUTF8("個");
            } else if (scc && morph.required > morph.capacity) {
                error += juce::String::fromUTF8("：番号不足 必要") + juce::String(static_cast<int>(morph.required))
                    + juce::String::fromUTF8("個／割当可能") + juce::String(static_cast<int>(morph.capacity)) + juce::String::fromUTF8("個");
            } else {
                error += juce::String::fromUTF8("：音色番号の割当に競合があります");
            }
            error += juce::String::fromUTF8("（開始@") + juce::String(previous) + juce::String::fromUTF8("を維持）");
            if (proposed.scc != allocation_starts_.scc) morph_bank_error_ = error;
            else opll_bank_error_ = error;
            morph_bank_.setSelectedId(allocation_starts_.scc + 1, juce::dontSendNotification);
            opll_bank_.setSelectedId(allocation_starts_.opll + 1, juce::dontSendNotification);
            syncMorphBank();
            setAllocationError(error);
            reportStatus(error);
            return;
        }
        allocation_starts_ = proposed;
        timbre_ = std::move(candidate);
        morph_bank_error_.clear(); opll_bank_error_.clear();
        setAllocationError({});
        updateEnvelopeMmlPreview();
        if (edit_callback_) publishEdit(true, true);
    }

    void setTimbre(
        const mgstc::engine::CompositeTimbre& timbre,
        bool reset_scroll_extent = false) {
        // A host restore / Undo supersedes a provisional mouse gesture.
        if (step_drag_) stopTimer();
        step_drag_.reset();
        loop_drag_.reset();
        const auto previous_pitch_expanded = pitch_lane_expanded_;
        pending_auto_settings_.reset();
        timbre_ = timbre;
        applySessionAllocationStarts(timbre_);
        last_published_timbre_ = timbre_;
        pending_plan_invalidation_ = false;
        cancelMorphGeneration(true);
        envelope_number_active_.resize(timbre_.layers.size());
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = timbre_.layers[index];
            envelope_number_active_[index] =
                layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate
                || mgstc::engine::layerUsesSequenceEnvelope(layer);
        }
        pitch_lane_expanded_.resize(timbre_.layers.size(), false);
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            pitch_lane_expanded_[index] = index < previous_pitch_expanded.size()
                ? previous_pitch_expanded[index]
                : !timbre_.layers[index].pitch_envelope.events.empty();
            if (timbre_.layers[index].software_lfo.enabled
                || hasModulation(timbre_.layers[index], mgstc::engine::CompositeModulationTarget::Pitch))
                pitch_lane_expanded_[index] = true;
        }
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        playback_tempo_ = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            timbre_.playback_tempo);
        timbre_.playback_tempo = playback_tempo_;
        tempo_.setText(
            juce::String(playback_tempo_), juce::dontSendNotification);
        clampAllLayersToTimelineRange();
        selected_layer_ = timbre_.layers.empty()
            ? -1
            : juce::jlimit(
                  0,
                  static_cast<int>(timbre_.layers.size()) - 1,
                  selected_layer_);
        refreshCountCeilingCache();
        if (reset_scroll_extent || !scroll_extent_initialized_) {
            resetScrollExtentToContent();
        } else {
            ensureScrollExtentIncludesContent();
        }
        updateScrollRanges();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        syncRateEditorFromModel();
        resized();
        repaint();
    }

    void refreshUiScaleFonts() {
        tempo_label_.setFont(UiFonts::body());
        UiFonts::styleBodyField(tempo_);
        UiFonts::styleBodyField(value_);
        UiFonts::refreshMgscPreviewFont(envelope_mml_preview_);
        morph_bank_label_.setFont(UiFonts::body());
        morph_bank_usage_.setFont(UiFonts::body());
        opll_bank_label_.setFont(UiFonts::body());
        opll_bank_usage_.setFont(UiFonts::body());
        allocation_error_label_.setFont(UiFonts::body());
        for (auto* setup : layer_setups_) setup->refreshFonts();
        syncRateEditorFromModel();
        resized();
        repaint();
    }

    [[nodiscard]] bool appendScopeFrame(
        const mgstc::engine::OpllScopeFrame& frame,
        std::uint8_t midi_note) {
        constexpr float kVisibleAmplitude = 1.0e-5F;
        const bool was_visible = scope_visible_sample_count_ != 0;
        audition_note_ = midi_note;
        for (std::size_t index = 0;
             index < mgstc::engine::OpllScopeFrame::kSampleCount;
             ++index) {
            const std::array<float, 4> samples{
                frame.psg_samples[index],
                frame.scc_samples[index],
                frame.samples[index],
                frame.mixed_samples[index],
            };
            for (std::size_t channel = 0; channel < samples.size(); ++channel) {
                auto& cached = scope_history_[channel][scope_write_position_];
                if (std::abs(cached) >= kVisibleAmplitude) {
                    --scope_visible_sample_count_;
                }
                cached = samples[channel];
                if (std::abs(cached) >= kVisibleAmplitude) {
                    ++scope_visible_sample_count_;
                }
            }
            scope_write_position_ =
                (scope_write_position_ + 1) % kScopeHistorySize;
            scope_size_ = juce::jmin(kScopeHistorySize, scope_size_ + 1);
        }
        if (was_visible || scope_visible_sample_count_ != 0) overview_content_.repaint();
        return was_visible || scope_visible_sample_count_ != 0;
    }

    void resized() override {
        using namespace UiLayout;
        auto area = getLocalBounds().reduced(panelPad);
        auto header = area.removeFromTop(textButtonH);
        tempo_.setBounds(header.removeFromRight(compositeTempoFieldW));
        tempo_label_.setBounds(header.removeFromRight(compositeTempoLabelW));
        header.removeFromRight(sm);
        const bool empty = timbre_.layers.empty();
        for (auto* button : {&psg_add_, &scc_add_, &opll_add_}) button->setVisible(empty);
        add_channel_.setVisible(!empty);
        if (empty) {
            opll_add_.setBounds(header.removeFromRight(compositeAddOpllW));
            header.removeFromRight(controlGap);
            scc_add_.setBounds(header.removeFromRight(compositeAddSccW));
            header.removeFromRight(controlGap);
            psg_add_.setBounds(header.removeFromRight(compositeAddPsgW));
        } else {
            add_channel_.setBounds(header.removeFromRight(compositeSelectorW));
        }
        focus_.setBounds(header.removeFromLeft(compositeFocusW));
        header.removeFromLeft(controlGap);
        compact_overview_.setBounds(header.removeFromLeft(compositeFocusW));
        compact_overview_.setEnabled(!empty && !focus_.getToggleState());
        header.removeFromLeft(controlGap);
        mml_toggle_.setBounds(header.removeFromLeft(compositeSelectorW));
        header.removeFromLeft(controlGap);
        zoom_in_.setBounds(header.removeFromLeft(textButtonH));
        header.removeFromLeft(controlGap);
        zoom_out_.setBounds(header.removeFromLeft(textButtonH));
        auto side = inspectorArea();
        setup_tab_.setVisible(inspector_visible_);
        count_tab_.setVisible(inspector_visible_);
        inspector_view_.setVisible(inspector_visible_);
        auto tabs = side.removeFromTop(textButtonH);
        setup_tab_.setBounds(tabs.removeFromLeft(tabs.getWidth() / 2));
        count_tab_.setBounds(tabs);
        side.removeFromTop(sm);
        inspector_view_.setBounds(side);
        inspector_content_.setSize(juce::jmax(1, side.getWidth() - kScrollBarSize),
            count_inspector_ ? UiLayout::compositeInspectorContentH
                + (UiLayout::fieldH + UiLayout::sm) * 5
                : panelPad * 2 + (fieldH + sm) * 18 + (libraryTitleH + sm) * 3);
        overview_view_.setVisible(!focus_.getToggleState());
        overview_view_.setBounds(overviewArea());
        layoutOverview();
        auto graph = graphArea();
        auto edit_header_area = editHeaderArea();
        auto edit_header = edit_header_area.removeFromTop(fieldH);
        // One shared row: channel caption, original-number starts, edit tools.
        edit_header.removeFromLeft(editHeaderCaptionBounds().getWidth() + controlGap);
        const int selector_width = libraryButtonMinW + xs * 2;
        const int tools_width = compositeSelectorW - sm;
        parameter_.setBounds(edit_header.removeFromRight(tools_width));
        edit_header.removeFromRight(controlGap);
        edit_mode_.setBounds(edit_header.removeFromRight(tools_width));
        edit_header.removeFromRight(controlGap);
        const auto layout_allocation = [&](juce::Rectangle<int>& row, juce::Label& label,
            juce::ComboBox& selector, juce::Label& usage, int usage_width) {
            const int label_width = juce::GlyphArrangement::getStringWidthInt(UiFonts::body(), label.getText()) + UiLayout::xs * 2;
            label.setBounds(row.removeFromLeft(label_width));
            selector.setBounds(row.removeFromLeft(selector_width));
            row.removeFromLeft(controlGap);
            usage.setBounds(row.removeFromLeft(usage_width));
            row.removeFromLeft(controlGap);
        };
        const int fixed_width = selector_width * 2 + controlGap * 4
            + juce::GlyphArrangement::getStringWidthInt(UiFonts::body(), "SCC")
            + juce::GlyphArrangement::getStringWidthInt(UiFonts::body(), "OPLL") + xs * 4;
        const int available_usage_width = juce::jmax(0, (edit_header.getWidth() - fixed_width) / 2);
        const int usage_width = juce::jmin(available_usage_width,
            juce::jmax(juce::GlyphArrangement::getStringWidthInt(morph_bank_usage_.getFont(), morph_bank_usage_.getText()),
                juce::GlyphArrangement::getStringWidthInt(opll_bank_usage_.getFont(), opll_bank_usage_.getText())) + xs * 2);
        layout_allocation(edit_header, morph_bank_label_, morph_bank_, morph_bank_usage_, usage_width);
        layout_allocation(edit_header, opll_bank_label_, opll_bank_, opll_bank_usage_, usage_width);
        if (allocation_error_label_.isVisible()) {
            edit_header_area.removeFromTop(sm);
            allocation_error_label_.setBounds(edit_header_area.removeFromTop(fieldH));
        }
        auto preview_area = workspaceArea();
        preview_area.setTop(graph.getBottom() + kScrollBarSize + sm);
        preview_area.setHeight(juce::jmin(preview_area.getHeight(), envelopePreviewH));
        envelope_mml_preview_.setBounds(preview_area);
        vertical_scroll_.setBounds(graph.getRight(), graph.getY(), kScrollBarSize, graph.getHeight());
        horizontal_scroll_.setBounds(graph.getX(), graph.getBottom(), graph.getWidth(), kScrollBarSize);
        updateScrollRanges();
        lane_layout_applied_ = false;
        lane_layout_dirty_ = true;
        ensureLaneLayout();
    }

    juce::String verifyLoopInteraction(const juce::File& directory) {
        juce::String failures;
        const auto check = [&](bool ok, const char* message) {
            if (!ok) failures += juce::String(message) + "\n";
        };
        const auto saved_callback = edit_callback_;
        const auto saved_scale = UiScale::active_percent;
        auto model = mgstc::engine::defaultCompositeTimbre();
        applySessionAllocationStarts(model);
        auto& authored = model.layers[0];
        authored.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        authored.envelope_timeline.length_counts = 24;
        authored.envelope_timeline.loop_start_count.reset();
        authored.envelope_timeline.loop_end_count.reset();
        authored.pitch_envelope.events = {
            {.kind = mgstc::engine::EnvelopeEventKind::Pitch, .value = 2, .count = 6},
            {.kind = mgstc::engine::EnvelopeEventKind::Pitch, .value = 3, .count = 6, .after_loop_start = true},
            {.kind = mgstc::engine::EnvelopeEventKind::Pitch, .value = -1, .count = 10}};
        const auto mouse = [&](juce::Point<int> point, juce::Point<int> down, bool dragged) {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, this, this, now, down.toFloat(), now, 1, dragged);
        };
        for (const int scale : {75, 100, 125}) {
            UiScale::setActivePercent(scale);
            setTimbre(model, true);
            selectOverviewLayer(0);
            refreshUiScaleFonts();
            step_edit_mode_.setToggleState(false, juce::dontSendNotification);
            int commits{};
            setEditCallback([&](const auto&, bool commit, bool) { if (commit) ++commits; });
            check(setLoopStart(0, 6), "loop start rejected valid step");
            check(timbre_.layers[0].envelope_timeline.loop_end_count == 24,
                "first loop start did not automatically use END");
            auto& layer = timbre_.layers[0];
            layer.pitch_envelope.events[0].after_loop_start = false;
            selected_count_ = 6;
            loop_insert_after_ = true;
            const auto slots = makeEditLaneSlots(graph_bounds_[0], layer.source);
            const auto plot = graphStrip(slots.pitch);
            const auto before = commandLabelBounds(plot, layer, 6, false);
            const auto after = commandLabelBounds(plot, layer, 6, true);
            check(before.getRight() <= after.getX() && before.getBottom() <= after.getY(),
                "initial and repeating command labels overlap");
            check(before.getRight() == loopStartX(plot, 6), "loop line is not at step centre");
            const auto full_text = juce::String("MOD: AR 1 / DR 1 / SL 8 / RR 6");
            const auto full_before = expandedCommandLabelBounds(plot, layer, 6, false, false,
                full_text, UiFonts::dense(true), false);
            const auto full_after = expandedCommandLabelBounds(plot, layer, 6, true, false,
                full_text, UiFonts::dense(true), false);
            check(full_before.getWidth() > before.getWidth() && full_after.getWidth() > after.getWidth()
                    && full_before.getBottom() <= full_after.getY(),
                "empty adjacent steps did not expand both loop command rows independently");
            check(expandedCommandLabelBounds(plot, layer, 6, false, false,
                    full_text, UiFonts::dense(true), true) == before,
                "occupied adjacent step no longer keeps the original abbreviated label bounds");
            const auto volume_plot = graphStrip(slots.volume);
            {
                // Relative Pitch drifts on later passes. The drawing exposes
                // only the first pass: a solid prefix and dashed loop body.
                const auto saved_range = visibleCountRange();
                horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
                const juce::Rectangle<int> sample_plot(0, 0, 320, 24);
                juce::Image image(juce::Image::ARGB, 320, 24, true);
                juce::Path path;
                path.startNewSubPath(0.0F, 12.0F);
                path.lineTo(320.0F, 12.0F);
                {
                    juce::Graphics graphics(image);
                    graphics.setColour(juce::Colours::white);
                    strokeEffectiveWaveform(graphics, path, sample_plot, layer,
                        mgstc::engine::CompositeModulationTarget::Pitch, 2.0F);
                }
                bool prefix_solid = true, loop_ink = false, loop_gap = false, suffix_empty = true;
                const int loop_x = xForCount(sample_plot, 6), end_x = xForCount(sample_plot, 24);
                for (int x = 1; x < 319; ++x) {
                    const auto alpha = image.getPixelAt(x, 12).getAlpha();
                    if (x < loop_x - 1) prefix_solid = prefix_solid && alpha > 200;
                    else if (x > loop_x && x < end_x - 1) {
                        loop_ink = loop_ink || alpha > 200;
                        loop_gap = loop_gap || alpha == 0;
                    } else if (x > end_x) suffix_empty = suffix_empty && alpha == 0;
                }
                check(prefix_solid && loop_ink && loop_gap && suffix_empty,
                    "first-loop effective curve is not solid prefix/dashed body/hidden unreachable suffix");
                horizontal_scroll_.setCurrentRange(saved_range.getStart(), saved_range.getLength(), juce::dontSendNotification);
            }
            check(commandLabelBounds(volume_plot, layer, 6, false, true)
                    == commandLabelBounds(volume_plot, layer, 6, true, true),
                "volume label was split into initial and repeating halves");
            juce::Image marker_image(juce::Image::ARGB, getWidth(), getHeight(), true);
            {
                juce::Graphics marker_graphics(marker_image);
                drawTimelineMarkers(marker_graphics, plot.getUnion(volume_plot), 0,
                    juce::Colours::cyan, volume_plot);
            }
            const int volume_y = volume_plot.getCentreY();
            check(marker_image.getPixelAt(xForCount(volume_plot, 6), volume_y).getAlpha() != 0,
                "volume loop line is not at step head");
            check(marker_image.getPixelAt(loopStartX(volume_plot, 6), volume_y).getAlpha() == 0,
                "volume loop line remained at step centre");
            loop_insert_after_ = false;
            static_cast<void>(editableModulationFor(layer, TimelineModulationTarget::Tremolo));
            check(std::all_of(layer.modulation_profiles.begin(), layer.modulation_profiles.end(),
                [](const auto& profile) {
                    return profile.target != mgstc::engine::CompositeModulationTarget::Volume
                        || profile.after_loop_start;
                }), "tremolo was placed before loop start");
            loop_insert_after_ = true;
            const auto unchanged = timbre_;
            static_cast<void>(modulationFor(layer, TimelineModulationTarget::Pitch));
            check(timbre_ == unchanged, "reading modulation created a profile");
            editPitchCumulative(6, 9, true);
            check(layer.pitch_envelope.events[0].value == 2 && !layer.pitch_envelope.events[0].after_loop_start,
                "editing repeating pitch replaced initial pitch");
            check(cumulativePitchAt(layer, 6) == 9, "repeating pitch ignored initial contribution");
            parameter_.setSelectedId(static_cast<int>(Parameter::Pitch), juce::dontSendNotification);
            const auto before_zone_switch = timbre_;
            const int before_zone_commits = commits;
            setLoopInsertZone(false);
            check(value_.getText().getIntValue() == 2, "initial zone kept repeating pitch editor value");
            setLoopInsertZone(true);
            check(value_.getText().getIntValue() == 7, "repeating zone kept initial pitch editor value");
            check(timbre_ == before_zone_switch && commits == before_zone_commits,
                "switching loop zone edited author data");
            const auto original = timbre_;
            const int saved_commits = commits;
            const auto ruler = graphStrip(slots.summary);
            const juce::Point<int> start(loopStartX(ruler, 6), ruler.getBottom() - 2);
            const juce::Point<int> destination(loopStartX(ruler, 10), start.y);
            check(beginLoopDrag(mouse(start, start, false)), "loop handle is not reachable");
            updateLoopDrag(mouse(destination, start, true));
            check(timbre_.layers[0].envelope_timeline.loop_start_count == 10, "loop handle did not move start");
            keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
            check(timbre_ == original && commits == saved_commits, "Escape committed loop preview");
            mouseDown(mouse(start, start, false));
            mouseDrag(mouse(destination, start, true));
            mouseUp(mouse(destination, start, true));
            check(commits == saved_commits + 1, "loop drag is not one Undo transaction");
            check(std::all_of(timbre_.layers[0].pitch_envelope.events.begin(),
                timbre_.layers[0].pitch_envelope.events.end(), [](const auto& command) {
                    return command.count != 10 || command.after_loop_start;
                }), "destination commands were not placed inside loop");
            timbre_.layers[0].pitch_envelope.events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch, .value = 3, .count = 18});
            // Include END's complete column; the normal viewport may end
            // before count 24 at the smaller verification window sizes.
            horizontal_scroll_.setRangeLimits(0.0, 64.0, juce::dontSendNotification);
            horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
            const juce::Point<int> end_handle(loopStartX(ruler, 24), start.y);
            const juce::Point<int> end_destination(loopStartX(ruler, 18), start.y);
            const int before_end_commits = commits;
            mouseDown(mouse(end_handle, end_handle, false));
            mouseDrag(mouse(end_destination, end_handle, true));
            check(commits == before_end_commits, "end preview published author data");
            mouseUp(mouse(end_destination, end_handle, true));
            check(commits == before_end_commits + 1, "end drag is not one Undo transaction");
            auto& moved = timbre_.layers[0];
            check(moved.envelope_timeline.loop_end_count == 18
                && moved.pitch_envelope.events.back().after_loop_start,
                "end destination pitch was left outside the loop");
            const auto point_at = [&](int count) {
                return juce::Point<int>(loopStartX(ruler, count), ruler.getY() + UiLayout::xs);
            };
            const auto reset = point_at(8);
            const int before_reset = commits;
            mouseDown(mouse(reset, reset, false));
            mouseUp(mouse(reset, reset, false));
            check(timbre_.layers[0].envelope_timeline.loop_start_count == 8
                && timbre_.layers[0].envelope_timeline.loop_end_count == 24 && commits == before_reset + 1,
                "command row click did not reset start and END loop end");
            const auto reset_start = point_at(8), beyond_end = point_at(28);
            mouseDown(mouse(reset_start, reset_start, false));
            mouseDrag(mouse(beyond_end, reset_start, true));
            mouseUp(mouse(beyond_end, reset_start, true));
            check(timbre_.layers[0].envelope_timeline.loop_start_count == 23
                    && timbre_.layers[0].envelope_timeline.loop_end_count == 24,
                "loop start passed its end");
            static_cast<void>(mgstc::engine::setCompositeLoopRange(timbre_.layers[0], 8, 24));
            // Finishing the preceding gesture enforces the normal minimum
            // column width. Restore this fixture's wide view before testing
            // an END-column mouse-down, particularly at 125%.
            horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
            const auto overlap = point_at(24), separate = point_at(20);
            mouseDown(mouse(overlap, overlap, false));
            mouseDrag(mouse(separate, overlap, true));
            mouseUp(mouse(separate, overlap, true));
            check(timbre_.layers[0].envelope_timeline.loop_end_count == 20
                    && timbre_.layers[0].envelope_timeline.length_counts == 24,
                "END won dragging priority over overlapping loop end");
            const auto author_counts = timbre_.layers[0].pitch_envelope.events;
            horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
            const auto extend_end = point_at(28);
            const int before_length = commits;
            mouseDown(mouse(overlap, overlap, false));
            mouseDrag(mouse(extend_end, overlap, true));
            mouseUp(mouse(extend_end, overlap, true));
            check(timbre_.layers[0].envelope_timeline.length_counts == 28
                    && timbre_.layers[0].envelope_timeline.loop_end_count == 20
                    && timbre_.layers[0].pitch_envelope.events == author_counts
                    && commits == before_length + 1,
                "END drag changed loop end or event counts or transaction count");
            const auto unchanged_click = timbre_;
            const int before_blocked_click = commits;
            step_edit_mode_.setToggleState(true, juce::dontSendNotification);
            mouseDown(mouse(reset, reset, false));
            mouseUp(mouse(reset, reset, false));
            check(timbre_ == unchanged_click && commits == before_blocked_click,
                "step-edit ON allowed a command row click to change loop");
            step_edit_mode_.setToggleState(false, juce::dontSendNotification);
            const auto loop_to_delete = timbre_;
            for (const bool start_marker : {true, false}) {
                setTimbre(loop_to_delete, true);
                auto expected = timbre_;
                mgstc::engine::clearCompositeLoop(expected.layers[0]);
                const auto& timeline = timbre_.layers[0].envelope_timeline;
                const auto summary = graphStrip(makeEditLaneSlots(graph_bounds_[0], timbre_.layers[0].source).summary);
                const auto marker = loopMarkerBounds(summary,
                    start_marker ? *timeline.loop_start_count : *timeline.loop_end_count,
                    start_marker ? "[ LOOP" : "LOOP ]");
                const auto right_point = marker.getCentre().toFloat();
                const auto now = juce::Time::getCurrentTime();
                const juce::MouseEvent right_event(juce::Desktop::getInstance().getMainMouseSource(), right_point,
                    juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier),
                    1.0F, 0.0F, 0.0F, 0.0F, 0.0F, this, this, now, right_point, now, 1, false);
                const int before_delete = commits;
                mouseDown(right_event);
                check(timbre_ == expected && commits == before_delete + 1,
                    "right-click loop bracket failed paired removal or changed existing flatten behavior");
            }
            setTimbre(loop_to_delete, true);
            // Restore END for the following command-stack deletion fixture.
            auto& restored = timbre_.layers[0];
            restored.envelope_timeline.length_counts = 24;
            static_cast<void>(mgstc::engine::setCompositeLoopEnd(restored, 24));
            selected_count_ = 24;
            restored.timbre_automation = {
                {.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 10, .count = 24},
                {.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 20, .count = 24, .after_loop_start = true}};
            changeStackCommand(0, std::nullopt);
            check(timbre_.layers[0].timbre_automation.size() == 1
                && timbre_.layers[0].timbre_automation[0].secondary == 10,
                "loop END command deletion chose outside command instead of visible inside command");
            auto auto_model = mgstc::engine::defaultCompositeTimbre();
            applySessionAllocationStarts(auto_model);
            auto& opll = auto_model.layers[2];
            opll.envelope_timeline = {.length_counts = 24, .loop_start_count = 6, .loop_end_count = 24};
            mgstc::engine::SavedTimbreReference patch;
            patch.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(auto_model);
            patch.source = mgstc::engine::TimbreSource::Opll;
            patch.opll_registers = mgstc::engine::encodeOpllPatch(mgstc::engine::defaultOpllPatch());
            opll.base_timbre = patch;
            opll.base_opll_rom.reset();
            mgstc::engine::OpllRegisterAutoLane initial;
            initial.mode = mgstc::engine::OpllRegisterAutoMode::FreeCurve;
            initial.depth = 3;
            initial.free_curve = {3};
            auto repeating = initial;
            repeating.depth = 8;
            repeating.free_curve = {8};
            opll.register_auto_profiles = {
                {mgstc::engine::OpllRegisterAutoTarget::TotalLevel, 6, false, initial},
                {mgstc::engine::OpllRegisterAutoTarget::TotalLevel, 6, true, repeating}};
            setTimbre(auto_model, true);
            selectOverviewLayer(2);
            selected_count_ = 6;
            parameter_.setSelectedId(static_cast<int>(Parameter::OpllTlAuto), juce::dontSendNotification);
            const auto unchanged_auto = timbre_;
            setLoopInsertZone(false);
            check(auto_depth_.getText().getIntValue() == 3, "initial zone kept repeating TL editor value");
            setLoopInsertZone(true);
            check(auto_depth_.getText().getIntValue() == 8, "repeating zone kept initial TL editor value");
            check(timbre_ == unchanged_auto, "switching TL zone edited author data");
            applyRegisterAutoFromEditors(false);
            check(timbre_.layers[2].register_auto_profiles[0].lane.depth == 3
                && timbre_.layers[2].register_auto_profiles[1].lane.depth == 8,
                "applying repeating TL copied initial settings into repeating profile");
            auto label_patch = mgstc::engine::defaultOpllPatch();
            label_patch.modulator.attack_rate = 0;
            label_patch.modulator.decay_rate = 0;
            auto_model.layers[2].base_timbre->opll_registers = mgstc::engine::encodeOpllPatch(label_patch);
            auto_model.layers[2].timbre_automation = {
                {.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite, .value = 4, .secondary = 0x11, .count = 4},
                {.kind = mgstc::engine::EnvelopeEventKind::Timbre, .value = 3, .count = 6,
                 .timbre_pick = mgstc::engine::TimbrePick::OpllRom, .after_loop_start = true},
                {.kind = mgstc::engine::EnvelopeEventKind::Timbre, .value = 15, .count = 16,
                 .target_library_id = patch.library_id, .after_loop_start = true}};
            setTimbre(auto_model, true);
            selectOverviewLayer(2);
            selected_count_ = 4;
            parameter_.setSelectedId(static_cast<int>(Parameter::RegisterWrite), juce::dontSendNotification);
            syncPointEditors(); syncInspector();
            const auto& label_layer = timbre_.layers[2];
            const auto manual_label = registerInspectLabel(label_layer, 4);
            check(manual_label.contains("AR 1") && manual_label.contains("DR 1") && !manual_label.contains("y4,17")
                    && value_.getText() == manual_label,
                "manual Y label or inspector regressed to raw y command");
            const auto manual_commands = laneCommands(label_layer, Parameter::RegisterWrite);
            check(manual_commands.size() == 1 && manual_commands.front().text == manual_label,
                "manual Y stack and summary did not use decoded parameter label");
            auto reload_layer = label_layer;
            reload_layer.timbre_automation = {
                {.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite, .value = 4, .secondary = 0x11, .count = 6},
                {.kind = mgstc::engine::EnvelopeEventKind::Timbre, .value = 15, .count = 6,
                 .target_library_id = patch.library_id, .after_loop_start = true},
                {.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite, .value = 4, .secondary = 0x01, .count = 6,
                 .after_loop_start = true}};
            const auto reload_label = formatRegisterGroupLabel(reload_layer, 6, {&reload_layer.timbre_automation[2]});
            check(reload_label.contains("DR 1") && !reload_label.contains("AR 0"),
                "manual Y label reapplied prefix writes after a same-count original timbre reload");
            horizontal_scroll_.setRangeLimits(0.0, 64.0, juce::dontSendNotification);
            horizontal_scroll_.setCurrentRange(0.0, 32.0, juce::dontSendNotification);
            const auto label_slots = makeEditLaneSlots(graph_bounds_[2], label_layer.source);
            juce::Image shade_image(juce::Image::ARGB, getWidth(), getHeight(), true);
            {
                juce::Graphics shade_graphics(shade_image);
                for (const auto row : {label_slots.register_write, label_slots.tl_auto, label_slots.fb_auto})
                    paintUnavailableOpllSteps(shade_graphics, graphStrip(row), label_layer);
            }
            // Native image drawing is flushed when Graphics is destroyed.
            for (const auto row : {label_slots.register_write, label_slots.tl_auto, label_slots.fb_auto}) {
                const auto plot = graphStrip(row);
                check(shade_image.getPixelAt(loopStartX(plot, 9), plot.getCentreY()).getAlpha() != 0
                    && shade_image.getPixelAt(loopStartX(plot, 2), plot.getCentreY()).getAlpha() == 0,
                    "ROM step shading absent or original step incorrectly disabled");
                check(shade_image.getPixelAt(xForCount(plot, 6) + UiLayout::xs, plot.getCentreY()).getAlpha() == 0
                    && shade_image.getPixelAt(loopStartX(plot, 6) + UiLayout::xs, plot.getCentreY()).getAlpha() != 0,
                    "ROM step shading ignored bracket-zone timbre change");
            }
            check(loopCommandTooltip({loopStartX(graphStrip(label_slots.register_write), 4), label_slots.register_write.getCentreY()})
                    .contains("y4,17"), "manual Y tooltip lost raw MGSC command");
            if (directory != juce::File{}) {
                auto stream = directory.getChildFile("envelope-loop-rom-y-" + juce::String(scale) + ".png").createOutputStream();
                check(stream != nullptr, "loop/ROM/Y snapshot could not be opened");
                if (stream) {
                    check(stream->setPosition(0) && stream->truncate().wasOk(), "loop/ROM/Y snapshot truncate failed");
                    check(juce::PNGImageFormat{}.writeImageToStream(createComponentSnapshot(getLocalBounds()), *stream),
                        "loop/ROM/Y snapshot encoding failed");
                }
            }
        }
        setEditCallback(saved_callback);
        UiScale::setActivePercent(saved_scale);
        refreshUiScaleFonts();
        return failures;
    }

    juce::String verifyOriginalAllocationStarts() {
        juce::String failures;
        const auto check = [&](bool ok, const char* message) {
            if (!ok) failures += juce::String(message) + "\n";
        };
        const auto saved_callback = edit_callback_;
        const auto saved_timbre = timbre_;
        const auto saved_starts = allocation_starts_;
        auto model = mgstc::engine::defaultCompositeTimbre();
        mgstc::engine::SavedTimbreReference scc;
        scc.source = mgstc::engine::TimbreSource::Scc;
        scc.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(model);
        scc.scc_waveform.fill(8);
        model.layers[1].base_timbre = scc;
        mgstc::engine::SavedTimbreReference opll;
        opll.source = mgstc::engine::TimbreSource::Opll;
        opll.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(model);
        opll.opll_registers = mgstc::engine::encodeOpllPatch(mgstc::engine::defaultOpllPatch());
        model.layers[2].base_timbre = opll;
        model.layers[2].base_opll_rom.reset();
        auto second = opll;
        second.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(model);
        model.embedded_timbres.push_back(second);
        model.layers[2].timbre_automation = {
            {.kind = mgstc::engine::EnvelopeEventKind::Timbre, .value = 17, .count = 4,
             .target_library_id = second.library_id}};
        setTimbre(model, true);
        int commits{};
        bool published_valid = true;
        mgstc::engine::EngineCore engine;
        setEditCallback([&](const auto& edited, bool commit, bool preview) {
            if (commit) {
                ++commits;
                published_valid &= preview && mgstc::engine::formatMgsComposite(edited).valid()
                    && mgstc::engine::compileCompositeProgram(engine, edited);
            }
        });
        const auto stored_bank = timbre_.scc_morph_bank_base;
        morph_bank_.setSelectedId(9, juce::sendNotificationSync);
        opll_bank_.setSelectedId(31, juce::sendNotificationSync);
        const auto numbers = mgstc::engine::resolveTimbreNumbers(timbre_);
        const auto number_for = [&](std::uint64_t id) {
            for (const auto& assignment : numbers.assignments)
                if (assignment.library_id == id) return static_cast<int>(assignment.number);
            return -1;
        };
        check(commits == 2 && published_valid && number_for(scc.library_id) == 8
                && number_for(opll.library_id) == 30 && number_for(second.library_id) == 31
                && timbre_.scc_morph_bank_base == stored_bank,
            "allocation selector did not publish chosen SCC/OPLL output starts or changed stored Morph Bank");
        const auto output = mgstc::engine::formatMgsComposite(timbre_);
        const auto definitions = mgstc::engine::extractMgsSourceDefinitions(output.source, 'v');
        check(definitions.size() == 2 && definitions[0].number == 30 && definitions[1].number == 31,
            "OPLL allocation selector did not change MGSC definition numbers");
        const auto accepted = timbre_;
        opll_bank_.setSelectedId(32, juce::sendNotificationSync);
        check(timbre_ == accepted && opll_bank_.getSelectedId() == 31 && commits == 2
                && opll_bank_usage_.getTooltip().isNotEmpty(),
            "exhausted OPLL allocation was not rejected without changing sound or setting");
        auto loaded = model;
        loaded.original_timbre_allocation_starts = mgstc::engine::OriginalTimbreAllocationStarts{0, 16};
        setTimbre(loaded, true);
        check(timbre_.original_timbre_allocation_starts == std::optional{mgstc::engine::OriginalTimbreAllocationStarts{8, 30}}
                && morph_bank_.getSelectedId() == 9 && opll_bank_.getSelectedId() == 31,
            "tone load/Undo replaced current editor-session allocation starts");
        allocation_starts_ = saved_starts;
        setTimbre(saved_timbre, true);
        setEditCallback(saved_callback);
        return failures;
    }

    juce::String verifyStepDrag() {
        juce::String failures;
        const auto check = [&](bool ok, const char* message) {
            if (!ok) failures += juce::String(message) + "\n";
        };
        const auto saved_callback = edit_callback_;
        const auto saved_scale = UiScale::active_percent;
        auto model = mgstc::engine::defaultCompositeTimbre();
        applySessionAllocationStarts(model);
        auto& layer = model.layers[0];
        layer.envelope_timeline.length_counts = 24;
        layer.envelope_timeline.loop_start_count.reset();
        layer.envelope_timeline.loop_end_count.reset();
        layer.volume_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        layer.volume_envelope.events = {
            {mgstc::engine::EnvelopeEventKind::Volume, 15, 0, 0},
            {mgstc::engine::EnvelopeEventKind::Volume, 8, 0, 6},
            {mgstc::engine::EnvelopeEventKind::Volume, 4, 0, 12}};
        const auto event_at = [&](juce::Point<int> p, juce::Point<int> down, bool dragged, bool ctrl) {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier
                    | (ctrl ? juce::ModifierKeys::ctrlModifier : 0)),
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, this, this, now, down.toFloat(), now, 1, dragged);
        };
        for (int scale : {75, 100, 125}) {
            UiScale::setActivePercent(scale);
            selected_layer_ = 0;
            setTimbre(model, true);
            refreshUiScaleFonts();
            int commits{};
            setEditCallback([&](const auto&, bool commit, bool) { if (commit) ++commits; });
            check(step_edit_mode_.isVisible(), "step edit toggle is not visible");
            const auto graph = graph_bounds_[0];
            const auto slots = makeEditLaneSlots(graph, layer.source);
            const auto plot = timelinePlotBounds(graph);
            const auto point_at = [&](int count, juce::Rectangle<int> row) {
                return juce::Point<int>((xForCount(plot, count) + xForCount(plot, count + 1)) / 2,
                    row.getCentreY());
            };
            const auto from = point_at(6, slots.summary);
            const auto to = point_at(8, slots.summary);
            step_edit_mode_.setToggleState(true, juce::dontSendNotification);
            step_edit_mode_.onClick();
            check(timbre_ == model && commits == 0, "step edit toggle mutates sound");
            mouseDown(event_at(from, from, false, false));
            mouseUp(event_at(from, from, false, false));
            check(timbre_ == model && commits == 0, "step edit click mutates sound");
            mouseDown(event_at(from, from, false, false));
            mouseDrag(event_at(to, from, true, false));
            check(step_drag_ && step_drag_->delta == 2 && commits == 0,
                "step drag fails to preview without committing");
            check(timbre_.layers[0].envelope_timeline.length_counts == 26
                && timbre_.layers[1] == model.layers[1], "all-lane slide moved other channel or missed END");
            mouseDrag(event_at(from, from, true, false));
            check(timbre_ == model, "returning step drag fails to restore exact original");
            mouseDrag(event_at(to, from, true, false));
            mouseUp(event_at(to, from, true, false));
            check(commits == 1 && !step_drag_, "step drag must commit exactly once");
            setTimbre(model, true);
            commits = 0;
            step_edit_mode_.setToggleState(false, juce::dontSendNotification);
            const auto volume_from = point_at(6, slots.volume);
            const auto volume_to = point_at(8, slots.volume);
            mouseDown(event_at(volume_from, volume_from, false, true));
            mouseDrag(event_at(volume_to, volume_from, true, true));
            check(step_drag_ && step_drag_->delta == 2
                && timbre_.layers[0].envelope_timeline == model.layers[0].envelope_timeline,
                "Ctrl individual slide failed or moved timeline");
            check(keyPressed(juce::KeyPress(juce::KeyPress::escapeKey))
                && timbre_ == model && commits == 0, "Esc must discard local preview");
            mouseUp(event_at(volume_to, volume_from, true, true));
            check(commits == 0, "mouseUp committed an escaped slide");
            auto long_model = model;
            long_model.layers[0].envelope_timeline.length_counts =
                mgstc::engine::kMgscEnvelopeUiLengthCap;
            setTimbre(long_model, true);
            const auto edge = juce::Point<int>(plot.getRight() - 1, slots.summary.getCentreY());
            const auto before_scroll = visibleCountRange();
            mouseDown(event_at(edge, edge, false, true));
            check(step_drag_.has_value(), "edge click must exercise a valid pending gesture");
            for (int tick = 0; tick < 4; ++tick) autoScrollStepDrag();
            mouseUp(event_at(edge, edge, false, true));
            check(timbre_ == long_model && commits == 0 && visibleCountRange() == before_scroll,
                "holding a click at the edge must not start auto-scroll or insert steps");
            setTimbre(model, true);
            mouseDown(event_at(from, from, false, true));
            mouseDrag(event_at(to, from, true, true));
            focusLost(juce::Component::focusChangedDirectly);
            check(!step_drag_ && timbre_ == model && commits == 0,
                "focus transfer must discard an uncommitted slide");
            mouseDown(event_at(from, from, false, true));
            mouseDrag(event_at(to, from, true, true));
            selectOverviewLayer(1);
            check(!step_drag_ && timbre_ == model && commits == 0,
                "channel selection must discard an uncommitted slide");
            selectOverviewLayer(0);
            mouseDown(event_at(from, from, false, true));
            mouseDrag(event_at(to, from, true, true));
            setTimbre(model, true);
            mouseUp(event_at(to, from, true, true));
            check(commits == 0 && timbre_ == model, "host replacement retained a stale gesture");
        }
        UiScale::setActivePercent(saved_scale);
        setEditCallback(saved_callback);
        step_edit_mode_.setToggleState(false, juce::dontSendNotification);
        setTimbre(model, true);
        return failures;
    }

    void layoutStepEditToggle() {
        step_edit_mode_.setVisible(false);
        if (selected_layer_ < 0 || selected_layer_ >= static_cast<int>(graph_bounds_.size())
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) return;
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence) return;
        auto label = makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(selected_layer_)],
            layer.source).summary.removeFromLeft(UiLayout::compositeOverviewLabelW);
        // The longer row name occupies the first text line; the existing
        // toggle stays in the label column on the following line.
        label.removeFromTop(juce::roundToInt(UiFonts::dense(true).getHeight()));
        auto bounds = label.reduced(UiLayout::xs, 0);
        step_edit_mode_.setBounds(bounds);
        step_edit_mode_.setVisible(graphArea().contains(bounds));
        step_edit_mode_.toFront(false);
    }

    void beginStepDrag(const juce::MouseEvent& event, int layer_index,
        juce::Rectangle<int> graph) {
        using Lane = mgstc::engine::CompositeStepEditLane;
        drawing_ = false;
        edit_draw_bounds_.reset();
        const auto& layer = timbre_.layers[static_cast<std::size_t>(layer_index)];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence) {
            reportStatus(juce::String::fromUTF8("@rには削除／挿入するステップ列がありません"));
            return;
        }
        const auto slots = makeEditLaneSlots(graph, layer.source);
        std::optional<Lane> lane;
        const auto point = event.getPosition();
        if (graphStrip(slots.summary).contains(point)) lane = Lane::All;
        else if (graphStrip(slots.volume).contains(point)) lane = Lane::Volume;
        else if (graphStrip(slots.pitch).contains(point)) lane = Lane::Pitch;
        else if (graphStrip(slots.timbre).contains(point)) lane = Lane::Timbre;
        else if (graphStrip(slots.register_write).contains(point)) lane = Lane::RegisterWrite;
        else if (graphStrip(slots.tl_auto).contains(point)) lane = Lane::Tl;
        else if (graphStrip(slots.fb_auto).contains(point)) lane = Lane::Fb;
        if (!lane) return;
        const int start = countAtX(timelinePlotBounds(graph), point.x);
        if (start > static_cast<int>(layer.envelope_timeline.length_counts)) return;
        cancelMorphGeneration(true);
        step_drag_ = StepDrag{timbre_, *lane, layer_index, start, 0,
            std::numeric_limits<int>::min(), selected_count_, point};
        selected_count_ = start;
        if (isShowing()) grabKeyboardFocus();
        startTimerHz(30);
        repaint();
    }

    void updateStepDrag(const juce::MouseEvent& event) {
        if (!step_drag_) return;
        if (!step_drag_->active && !event.mouseWasDraggedSinceMouseDown()) return;
        step_drag_->active = true;
        step_drag_->pointer = event.getPosition();
        updateStepDragAtPointer();
    }

    void updateStepDragAtPointer() {
        if (!step_drag_) return;
        auto& drag = *step_drag_;
        const auto plot = timelinePlotBounds(graph_bounds_[static_cast<std::size_t>(drag.layer)]);
        const int requested = countAtX(plot, drag.pointer.x) - drag.start;
        if (requested == drag.requested_delta) return;
        drag.requested_delta = requested;
        auto delta = requested;
        auto result = mgstc::engine::editCompositeSteps(
            drag.original.layers[static_cast<std::size_t>(drag.layer)], drag.lane,
            static_cast<std::uint32_t>(drag.start), delta);
        const auto error = result.error;
        // Stop at the nearest usable count, never cut off the retained suffix.
        while (!result.accepted && delta != 0) {
            delta += delta > 0 ? -1 : 1;
            result = mgstc::engine::editCompositeSteps(
                drag.original.layers[static_cast<std::size_t>(drag.layer)], drag.lane,
                static_cast<std::uint32_t>(drag.start), delta);
        }
        if (!result.accepted) return;
        drag.delta = result.applied_delta;
        timbre_ = drag.original;
        timbre_.layers[static_cast<std::size_t>(drag.layer)] = std::move(result.layer);
        selected_count_ = drag.start + drag.delta;
        position_.setText("ct " + juce::String(selected_count_), juce::dontSendNotification);
        reportStatus(juce::String::fromUTF8(drag.delta < 0 ? "削除 " : "挿入 ")
            + juce::String(std::abs(drag.delta)) + juce::String::fromUTF8(" ステップ（離すと確定、Escで取消）")
            + (delta != requested ? " / " + juce::String::fromUTF8(error.c_str()) : juce::String{}));
        overview_content_.repaint();
        repaint();
    }

    void autoScrollStepDrag() {
        if (!step_drag_ || !step_drag_->active) return;
        const auto plot = timelinePlotBounds(graph_bounds_[static_cast<std::size_t>(step_drag_->layer)]);
        const int x = step_drag_->pointer.x;
        int direction = x < plot.getX() + UiLayout::sm ? -1
            : x >= plot.getRight() - UiLayout::sm ? 1 : 0;
        if (direction == 0) return;
        const auto range = visibleCountRange();
        if (direction > 0) maybeExpandScrollExtent(static_cast<int>(range.getEnd()));
        horizontal_scroll_.setCurrentRangeStart(juce::jmax(0.0,
            range.getStart() + direction), juce::dontSendNotification);
        updateStepDragAtPointer();
        overview_content_.repaint();
        repaint();
    }

    void cancelStepDrag() {
        if (!step_drag_) return;
        timbre_ = std::move(step_drag_->original);
        selected_count_ = step_drag_->original_selection;
        step_drag_.reset();
        stopTimer();
        position_.setText("ct " + juce::String(selected_count_), juce::dontSendNotification);
        reportStatus(juce::String::fromUTF8("ステップの削除／挿入を取り消しました"));
        overview_content_.repaint();
        repaint();
    }

    void finishStepDrag() {
        if (!step_drag_) return;
        const auto changed = timbre_ != step_drag_->original;
        step_drag_.reset();
        stopTimer();
        if (!changed) return;
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        updateEnvelopeMmlPreview();
        publishEdit(true, true);
        overview_content_.repaint();
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (step_drag_ && key.getKeyCode() == juce::KeyPress::escapeKey) {
            cancelStepDrag();
            return true;
        }
        // Commands such as Undo must not capture a provisional local candidate.
        if (step_drag_ && (key.getModifiers().isCtrlDown()
            || key.getKeyCode() == juce::KeyPress::tabKey)) cancelStepDrag();
        if (loop_drag_ && key.getKeyCode() == juce::KeyPress::escapeKey) {
            cancelLoopDrag();
            return true;
        }
        if (loop_drag_ && (key.getModifiers().isCtrlDown()
            || key.getKeyCode() == juce::KeyPress::tabKey)) cancelLoopDrag();
        if (!step_drag_ && !loop_drag_ && selected_layer_ >= 0 && hasKeyboardFocus(false)) {
            auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
            if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey) {
                const bool right = key.getKeyCode() == juce::KeyPress::rightKey;
                if (isLoopStartCount(layer, selected_count_) && right != loop_insert_after_)
                    loop_insert_after_ = right;
                else {
                    selected_count_ = juce::jlimit(0, maximumCount(), selected_count_ + (right ? 1 : -1));
                    loop_insert_after_ = !right && isLoopStartCount(layer, selected_count_);
                }
                syncPointEditors(); syncRegisterAutoEditors(); syncInspector(); repaint(); return true;
            }
            if (key.getKeyCode() == juce::KeyPress::F10Key && key.getModifiers().isShiftDown()
                && isLoopStartCount(layer, selected_count_)) {
                const auto plot = timelinePlotBounds(graph_bounds_[static_cast<std::size_t>(selected_layer_)]);
                const auto slots = makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(selected_layer_)], layer.source);
                return showLoopCommandMenu({loopStartX(plot, selected_count_) + (loop_insert_after_ ? UiLayout::xs : -UiLayout::xs),
                    slots.summary.getY() + UiLayout::xs});
            }
        }
        return false;
    }

    void focusLost(FocusChangeType) override {
        cancelLoopDrag();
        cancelStepDrag();
    }

    bool setLoopStart(std::size_t layer_index, int count) {
        cancelLoopDrag();
        cancelStepDrag();
        if (layer_index >= timbre_.layers.size() || count < 0) return false;
        if (!mgstc::engine::setCompositeLoopStart(timbre_.layers[layer_index],
                static_cast<std::uint32_t>(count))) return false;
        selected_layer_ = static_cast<int>(layer_index);
        selected_count_ = count;
        finishLoopEdit();
        return true;
    }

    void clearLoop(std::size_t layer_index) {
        cancelLoopDrag();
        cancelStepDrag();
        if (layer_index >= timbre_.layers.size()) return;
        selected_layer_ = static_cast<int>(layer_index);
        mgstc::engine::clearCompositeLoop(timbre_.layers[layer_index]);
        finishLoopEdit();
    }

    void finishLoopEdit() {
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        updateEnvelopeMmlPreview();
        publishEdit(true, true);
        overview_content_.repaint();
        repaint();
    }

    struct LoopDrag {
        mgstc::engine::CompositeTimbre original;
        std::size_t layer{};
        int endpoint{}; // 0=start, 1=loop end, 2=END, 3=new start
        int anchor{};
        int original_selection{};
    };

    void cancelLoopDrag() {
        if (!loop_drag_) return;
        timbre_ = std::move(loop_drag_->original);
        selected_count_ = loop_drag_->original_selection;
        loop_drag_.reset();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        overview_content_.repaint();
        repaint();
    }

    int loopStartX(juce::Rectangle<int> plot, int count) const {
        return (xForCount(plot, count) + xForCount(plot, count + 1)) / 2;
    }

    bool beginLoopDrag(const juce::MouseEvent& event) {
        if (!event.mods.isLeftButtonDown() || event.mods.isCtrlDown()
            || step_edit_mode_.getToggleState()) return false;
        const int layer_index = layerAt(event.getPosition());
        if (layer_index < 0 || layer_index != selected_layer_
            || layer_index >= static_cast<int>(graph_bounds_.size())) return false;
        const auto& layer = timbre_.layers[static_cast<std::size_t>(layer_index)];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence) return false;
        const auto slots = makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(layer_index)], layer.source);
        const auto ruler = graphStrip(slots.summary);
        if (!ruler.contains(event.getPosition())) return false;
        const auto& timeline = layer.envelope_timeline;
        const int count = countAtX(ruler, event.x);
        // Markers belong to their complete step column. LOOP ] wins when
        // it shares END's column; an ordinary step always resets the start.
        int endpoint = 3;
        if (timeline.loop_end_count && count == static_cast<int>(*timeline.loop_end_count)) endpoint = 1;
        else if (timeline.loop_start_count && count == static_cast<int>(*timeline.loop_start_count)) endpoint = 0;
        else if (count == static_cast<int>(timeline.length_counts)) endpoint = 2;
        loop_drag_ = LoopDrag{timbre_, static_cast<std::size_t>(layer_index), endpoint, count, selected_count_};
        selected_count_ = count;
        grabKeyboardFocus();
        if (endpoint == 3 || endpoint == 0) {
            auto& selected = timbre_.layers[loop_drag_->layer];
            if (!mgstc::engine::setCompositeLoopRange(selected, static_cast<std::uint32_t>(count),
                    selected.envelope_timeline.length_counts)) {
                loop_drag_.reset();
                return true;
            }
            // Preview only: a mouse-up commits this entire gesture once.
            syncInspector(); repaint();
        }
        return true;
    }

    void updateLoopDrag(const juce::MouseEvent& event) {
        if (!loop_drag_) return;
        const auto drag = *loop_drag_;
        auto candidate = drag.original;
        auto& layer = candidate.layers[drag.layer];
        const auto plot = timelinePlotBounds(graph_bounds_[drag.layer]);
        const int count = countAtX(plot, event.x);
        auto& timeline = layer.envelope_timeline;
        if (timeline.loop_start_count && !timeline.loop_end_count) timeline.loop_end_count = timeline.length_counts;
        if (drag.endpoint == 3 || (drag.endpoint != 2 && !event.mouseWasDraggedSinceMouseDown())) {
            const int position = event.mouseWasDraggedSinceMouseDown() ? count : drag.anchor;
            if (!mgstc::engine::setCompositeLoopRange(layer,
                    static_cast<std::uint32_t>(juce::jlimit(0, static_cast<int>(timeline.length_counts) - 1, position)),
                    timeline.length_counts)) return;
            selected_count_ = static_cast<int>(*timeline.loop_start_count);
        } else if (drag.endpoint == 0) {
            static_cast<void>(mgstc::engine::setCompositeLoopStart(layer,
                static_cast<std::uint32_t>(juce::jlimit(0, static_cast<int>(*timeline.loop_end_count) - 1, count))));
            selected_count_ = static_cast<int>(*timeline.loop_start_count);
        } else if (drag.endpoint == 1) {
            static_cast<void>(mgstc::engine::setCompositeLoopEnd(layer,
                static_cast<std::uint32_t>(juce::jlimit(
                    static_cast<int>(*timeline.loop_start_count) + 1, static_cast<int>(timeline.length_counts), count))));
            selected_count_ = static_cast<int>(*timeline.loop_end_count);
        } else if (drag.endpoint == 2 && event.mouseWasDraggedSinceMouseDown()) {
            // Keep the existing valid loop and every authored event count.
            const int minimum = static_cast<int>(timeline.loop_end_count.value_or(1));
            timeline.length_counts = static_cast<std::uint32_t>(juce::jlimit(minimum, maximumCount(), count));
            selected_count_ = static_cast<int>(timeline.length_counts);
        }
        timbre_ = std::move(candidate);
        syncInspector();
        overview_content_.repaint(); repaint();
    }

    void mouseDown(const juce::MouseEvent& event) override {
        cancelLoopDrag();
        cancelStepDrag();
        pending_auto_settings_.reset();
        if (event.mods.isPopupMenu()) {
            if (!step_edit_mode_.getToggleState() && deleteLoopMarkerAt(event.getPosition())) return;
            if (showLoopCommandMenu(event.getPosition())) return;
            deleteCommandAt(event.getPosition());
            return;
        }
        if (!event.mods.isCtrlDown() && !step_edit_mode_.getToggleState()
            && beginLoopDrag(event)) return;
        const int layer = layerAt(event.getPosition());
        if (layer < 0) {
            return;
        }
        const bool selection_changed = selected_layer_ != layer;
        selected_layer_ = layer;
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const bool in_graph = graph.contains(event.getPosition());
        if (in_graph && event.mods.isLeftButtonDown()
            && (step_edit_mode_.getToggleState() || event.mods.isCtrlDown())) {
            // Reserve the gesture before Draw mode can alter values or open a picker.
            beginStepDrag(event, layer, graph);
            return;
        }
        if (in_graph) {
            auto count_bounds = timelinePlotBounds(graph);
            selected_count_ = countAtX(count_bounds, event.getPosition().x);
            selectLoopZoneAt(count_bounds, event.getPosition());
            position_.setText(
                "ct " + juce::String(selected_count_),
                juce::dontSendNotification);
        }
        bool edit_tier = false;
        if (!selection_changed && in_graph) {
            edit_tier = hitEditLane(
                timbre_.layers[static_cast<std::size_t>(layer)],
                graph,
                event.getPosition());
        } else {
            edit_draw_bounds_.reset();
        }
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        if (selection_changed) {
            refreshCountCeilingCache();
            updateScrollRanges();
            ensureSelectedLaneVisible();
            drawing_ = false;
            resized();
            repaint();
            return;
        }
        if (!selection_changed && in_graph
            && selectedEditMode() == EditMode::Draw) {
            const auto& selected_layer =
                timbre_.layers[static_cast<std::size_t>(layer)];
            const auto slots = makeEditLaneSlots(graph, selected_layer.source);
            if (!slots.timbre.isEmpty()
                && slots.timbre.contains(event.getPosition())
                && selected_layer.source != mgstc::engine::TimbreSource::Psg) {
                parameter_.setSelectedId(
                    static_cast<int>(Parameter::Timbre),
                    juce::dontSendNotification);
                const auto tone_plot = graphStrip(slots.timbre);
                const auto marked = timbreMarkerCountAt(selected_layer, tone_plot, event.getPosition());
                const auto morph_arrival = marked ? std::nullopt
                    : morphLineArrivalAt(selected_layer, tone_plot, event.getPosition());
                if (marked) {
                    selected_count_ = *marked;
                    position_.setText(
                        "ct " + juce::String(selected_count_),
                        juce::dontSendNotification);
                } else if (morph_arrival) {
                    selected_count_ = *morph_arrival;
                    position_.setText("ct " + juce::String(selected_count_), juce::dontSendNotification);
                }
                pickTimbreAt(selected_count_, morph_arrival.has_value());
                drawing_ = false;
                repaint();
                return;
            }
            if (!slots.register_write.isEmpty()
                && slots.register_write.contains(event.getPosition())) {
                parameter_.setSelectedId(
                    static_cast<int>(Parameter::RegisterWrite),
                    juce::dontSendNotification);
                pickRegisterAt(selected_count_);
                drawing_ = false;
                repaint();
                return;
            }
            if (slots.tl_auto.contains(event.getPosition())
                || slots.fb_auto.contains(event.getPosition())) {
                const bool tl = slots.tl_auto.contains(event.getPosition());
                const auto target = tl ? TimelineModulationTarget::TotalLevel
                    : TimelineModulationTarget::Feedback;
                if (!mgstc::engine::opllRegisterAutoAvailableAt(selected_layer,
                        static_cast<std::uint32_t>(selected_count_),
                        !isLoopStartCount(selected_layer, selected_count_) || loop_insert_after_)) {
                    reportStatus(juce::String::fromUTF8("ROM音色区間ではTL/FB変調を設定できません"));
                } else {
                    // A click opens settings; only an actual drag writes free
                    // curve points. Opening settings never edits the model.
                    pending_auto_settings_ = target;
                }
                drawing_ = false;
                repaint();
                return;
            }
        }
        if (selectedEditMode() == EditMode::Draw) {
            if (!in_graph) {
                drawing_ = false;
            } else if (edit_tier) {
                drawing_ = edit_draw_bounds_.has_value();
                if (drawing_) {
                    editFromMouse(event, false);
                    updateDrawHoverPopup(event.getPosition());
                }
            } else {
                // Selected edit-tier uses sublane bounds only; never map
                // Y against the full lane (wrong volume/pitch values).
                drawing_ = false;
            }
        } else if (in_graph) {
            setTimelineMarker(selected_count_, true);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (loop_drag_) { updateLoopDrag(event); return; }
        if (step_drag_) {
            updateStepDrag(event);
            return;
        }
        if (pending_auto_settings_ && event.mouseWasDraggedSinceMouseDown()) {
            const auto* curve = freeCurveFor(timbre_.layers[static_cast<std::size_t>(selected_layer_)],
                *pending_auto_settings_);
            if (curve && curve->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
                pending_auto_settings_.reset();
                drawing_ = edit_draw_bounds_.has_value();
            }
        }
        if (!drawing_) {
            return;
        }
        editFromMouse(event, false);
        // inspectFromMouse skips while drawing_; keep the idle-hover popup
        // at the pointer (SCC waveform mouseDrag does the same).
        updateDrawHoverPopup(event.getPosition());
        repaint();
    }

    void mouseUp(const juce::MouseEvent& event) override {
        if (loop_drag_) {
            updateLoopDrag(event);
            const bool changed = timbre_ != loop_drag_->original;
            loop_drag_.reset();
            if (changed) finishLoopEdit();
            return;
        }
        if (step_drag_) {
            updateStepDrag(event);
            finishStepDrag();
            return;
        }
        if (pending_auto_settings_) {
            const auto target = *pending_auto_settings_;
            pending_auto_settings_.reset();
            if (!event.mouseWasDraggedSinceMouseDown()) {
                openTimelineModulationDialog(target, selected_count_);
            }
            return;
        }
        if (!drawing_) {
            return;
        }
        editFromMouse(event, true);
        drawing_ = false;
        hover_mouse_pos_ = event.getPosition();
        inspectFromMouse(event.getPosition());
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override {
        if (step_edit_mode_.getToggleState() || event.mods.isCtrlDown()) return;
        const int layer = layerAt(event.getPosition());
        if (layer < 0
            || layer != selected_layer_
            || layer >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(layer)];
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer)];
        const auto slots = makeEditLaneSlots(graph, selected.source);
        if (!slots.volume.isEmpty()
            && slots.volume.contains(event.getPosition())) {
            drawing_ = false;
            promptVolumeNumeric(event.getPosition());
            return;
        }
        if (pitchLaneExpanded() && !slots.pitch.isEmpty()
            && slots.pitch.contains(event.getPosition())) {
            drawing_ = false;
            promptPitchNumeric(event.getPosition());
        }
    }

    void mouseMove(const juce::MouseEvent& event) override {
        setTooltip(loopCommandTooltip(event.getPosition()));
        const auto previous_popup = hoverPopupBounds();
        const auto previous_value = hover_preview_value_;
        const auto previous_count = selected_count_;
        const auto previous_hover_parameter = hover_edit_parameter_;
        hover_mouse_pos_ = event.getPosition();
        inspectFromMouse(event.getPosition());
        const auto next_popup = hoverPopupBounds();
        const bool selection_overlay_changed =
            previous_count != selected_count_
            || previous_hover_parameter != hover_edit_parameter_;
        if (selection_overlay_changed) {
            juce::Rectangle<int> dirty;
            if (selected_layer_ >= 0
                && selected_layer_ < static_cast<int>(graph_bounds_.size())) {
                dirty = graph_bounds_[static_cast<std::size_t>(selected_layer_)];
            }
            if (previous_popup) {
                dirty = dirty.getUnion(*previous_popup);
            }
            if (next_popup) {
                dirty = dirty.getUnion(*next_popup);
            }
            if (!dirty.isEmpty()) {
                repaint(dirty.expanded(6));
            }
            return;
        }
        if (previous_popup != next_popup
            || previous_value != hover_preview_value_) {
            if (previous_popup) {
                repaint(previous_popup->expanded(6));
            }
            if (next_popup) {
                repaint(next_popup->expanded(6));
            }
        }
    }

    void mouseExit(const juce::MouseEvent&) override {
        setTooltip({});
        if (drawing_) {
            return;
        }
        hover_preview_value_.reset();
        hover_mouse_pos_.reset();
        hover_edit_parameter_.reset();
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        syncPointEditors();
        repaint();
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override {
        if (step_drag_ || loop_drag_) return; // Keep gesture coordinates stable.
        if (event.mods.isCtrlDown()) {
            const double factor = wheel.deltaY > 0.0 ? 0.85 : 1.18;
            zoomCountColumns(event.getPosition().x, factor);
            return;
        }
        const double delta = wheel.deltaY != 0.0
            ? static_cast<double>(wheel.deltaY)
            : static_cast<double>(wheel.deltaX);
        if (event.mods.isShiftDown()
            || (wheel.deltaY == 0.0 && wheel.deltaX != 0.0)) {
            const double requested_start =
                horizontal_scroll_.getCurrentRangeStart()
                - delta * 4.0;
            maybeExpandScrollExtent(static_cast<int>(std::ceil(
                requested_start
                + horizontal_scroll_.getCurrentRangeSize())));
            horizontal_scroll_.setCurrentRangeStart(
                requested_start,
                juce::dontSendNotification);
        } else {
            vertical_scroll_.setCurrentRangeStart(
                vertical_scroll_.getCurrentRangeStart()
                    - delta * static_cast<double>(UiLayout::xl * 2),
                juce::dontSendNotification);
        }
        lane_layout_dirty_ = true;
        ensureLaneLayout();
        repaint();
    }

    void paint(juce::Graphics& graphics) override {
        const auto clip = graphics.getClipBounds().getSmallestIntegerContainer();
        // Always restore the clipped underlying scene before drawing a popup.
        // Alpha text/rounded corners must never accumulate over previous paints.
        graphics.fillAll(juce::Colour(0xFF161B22));
        graphics.setColour(juce::Colour(0xFF20262E));
        graphics.fillRect(inspectorArea());
        auto area = workspaceArea();
        if (!focus_.getToggleState()) {
            area.removeFromTop(overviewHeight());
            auto mix = area.removeFromTop(UiLayout::fieldH);
            graphics.setColour(juce::Colour(0xFF9FAFBD));
            graphics.setFont(UiFonts::body(true));
            graphics.drawText("MIX", mix.removeFromLeft(UiLayout::compositeOverviewLabelW), juce::Justification::centredLeft);
            mix.removeFromRight(kScrollBarSize);
            mix.reduce(UiLayout::sm, 0);
            drawScope(graphics, mix, timbre_.layers.size(), juce::Colour(0xFFE6EDF3));
            area.removeFromTop(UiLayout::sm);
        }
        auto title = editHeaderCaptionBounds();
        graphics.setFont(UiFonts::body(true));
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        if (selected_layer_ < 0 || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            graphics.drawText(juce::String::fromUTF8("＋ PSG / SCC / OPLL からチャンネルを追加"),
                title, juce::Justification::centredLeft);
            return;
        }
        const auto index = static_cast<std::size_t>(selected_layer_);
        const auto& layer = timbre_.layers[index];
        const auto colour = sourceColour(layer.source);
        graphics.drawText(juce::String::fromUTF8(layer.name.c_str())
            + juce::String::fromUTF8(" / エンベロープ"), title, juce::Justification::centredLeft);
        if (!clip.intersects(graphArea())) return;
        graphics.reduceClipRegion(graphArea());
        if (index >= graph_bounds_.size()) return;
        const auto lane = graph_bounds_[index];
        graphics.setColour(juce::Colour(0xFF20262E));
        graphics.fillRect(lane_frames_[index]);
        const auto plot = timelinePlotBounds(lane);
        drawCountGrid(graphics, plot);
        drawSelectedCountBand(graphics, plot);
        paintStartDelay(graphics, plot, index);
        {
            juce::Graphics::ScopedSaveState body_state(graphics);
            const auto summary = makeEditLaneSlots(lane, layer.source).summary;
            graphics.reduceClipRegion(graphArea().withTop(summary.getBottom()));
            paintEditTier(graphics, lane, index, colour);
        }
        if (step_drag_) {
            graphics.setColour(uiHoverAccent());
            const int start_x = xForCount(plot, step_drag_->start);
            const int end_x = xForCount(plot, step_drag_->start + step_drag_->delta);
            graphics.drawVerticalLine(end_x, static_cast<float>(lane.getY()),
                static_cast<float>(lane.getBottom()));
            graphics.setColour(uiHoverAccent().withAlpha(0.12f));
            graphics.fillRect(juce::Rectangle<int>(juce::jmin(start_x, end_x), lane.getY(),
                std::abs(end_x - start_x), lane.getHeight()));
        }
        paintPinnedCommandRow(graphics, lane, layer, colour);
        drawHoverValuePopup(graphics);
    }

private:
    void setCountInspector(bool count) {
        count_inspector_ = count;
        setup_tab_.setToggleState(!count, juce::dontSendNotification);
        count_tab_.setToggleState(count, juce::dontSendNotification);
        inspector_view_.setViewPosition(0, 0);
        resized(); repaint();
    }

    [[nodiscard]] bool toggleStateForModulation(std::size_t index) const {
        return modulation_toggles_[index].getToggleState();
    }

    [[nodiscard]] static bool hasModulation(const mgstc::engine::CompositeLayer& layer,
        mgstc::engine::CompositeModulationTarget target) {
        return mgstc::engine::compositeBaseModulation(layer, target).enabled
            || std::any_of(layer.modulation_profiles.begin(), layer.modulation_profiles.end(),
                [target](const auto& profile) { return profile.target == target && profile.parameters.enabled; });
    }

    void strokeEffectiveWaveform(juce::Graphics& graphics, const juce::Path& path,
        juce::Rectangle<int> plot, const mgstc::engine::CompositeLayer& layer,
        mgstc::engine::CompositeModulationTarget target, float width) const {
        const auto& timeline = layer.envelope_timeline;
        if (!timeline.loop_start_count || !timeline.loop_end_count) {
            graphics.strokePath(path, juce::PathStrokeType(width));
            return;
        }
        const int start_x = xForCount(plot, static_cast<int>(*timeline.loop_start_count));
        const int end_x = xForCount(plot, static_cast<int>(*timeline.loop_end_count));
        const bool first_pass_only = mgstc::engine::compositeLoopEffectiveWaveformFirstPassOnly(layer, target, timbreLibrary());
        {
            juce::Graphics::ScopedSaveState prefix_state(graphics);
            graphics.reduceClipRegion(plot.withRight(juce::jlimit(plot.getX(), plot.getRight(), first_pass_only ? start_x : end_x)));
            graphics.strokePath(path, juce::PathStrokeType(width));
        }
        if (first_pass_only) {
            juce::Graphics::ScopedSaveState loop_state(graphics);
            graphics.reduceClipRegion(plot.withLeft(juce::jlimit(plot.getX(), plot.getRight(), start_x))
                .withRight(juce::jlimit(plot.getX(), plot.getRight(), end_x)));
            juce::Path dashed;
            const float lengths[]{static_cast<float>(UiLayout::sm), static_cast<float>(UiLayout::xs)};
            juce::PathStrokeType(width).createDashedStroke(dashed, path, lengths, 2);
            graphics.fillPath(dashed);
        }
    }

    static constexpr mgstc::engine::CompositeModulationTarget kEffectiveWaveformTargets[]{
        mgstc::engine::CompositeModulationTarget::Pitch, mgstc::engine::CompositeModulationTarget::Volume,
        mgstc::engine::CompositeModulationTarget::TotalLevel, mgstc::engine::CompositeModulationTarget::Feedback};

    [[nodiscard]] static mgstc::engine::CompositeModulationTarget coreModulationTarget(TimelineModulationTarget target) {
        return static_cast<mgstc::engine::CompositeModulationTarget>(target);
    }

    [[nodiscard]] const mgstc::engine::ModulationParameters& modulationFor(
        mgstc::engine::CompositeLayer& layer,
        TimelineModulationTarget target) {
        return mgstc::engine::compositeModulationAt(layer, coreModulationTarget(target),
            static_cast<std::uint32_t>(juce::jmax(0, selected_count_)),
            target == TimelineModulationTarget::Tremolo || loop_insert_after_ || !isLoopStartCount(layer, selected_count_));
    }

    [[nodiscard]] mgstc::engine::ModulationParameters& editableModulationFor(
        mgstc::engine::CompositeLayer& layer, TimelineModulationTarget target) {
        if (isLoopStartCount(layer, selected_count_) || !layer.modulation_profiles.empty())
            return mgstc::engine::editCompositeModulationAt(layer, coreModulationTarget(target),
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)),
                target == TimelineModulationTarget::Tremolo || loop_insert_after_);
        switch (target) {
        case TimelineModulationTarget::Pitch: return layer.pitch_modulation;
        case TimelineModulationTarget::Tremolo: return layer.volume_modulation;
        case TimelineModulationTarget::TotalLevel: return layer.opll_tl_modulation;
        case TimelineModulationTarget::Feedback: return layer.opll_fb_modulation;
        }
        return layer.pitch_modulation;
    }

    [[nodiscard]] const mgstc::engine::OpllRegisterAutoLane* freeCurveFor(
        mgstc::engine::CompositeLayer& layer,
        TimelineModulationTarget target) {
        if (target == TimelineModulationTarget::TotalLevel || target == TimelineModulationTarget::Feedback)
            return &mgstc::engine::compositeRegisterAutoAt(layer, target == TimelineModulationTarget::TotalLevel
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel : mgstc::engine::OpllRegisterAutoTarget::Feedback,
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)), loop_insert_after_ || !isLoopStartCount(layer, selected_count_));
        return nullptr;
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoLane* editableFreeCurveFor(
        mgstc::engine::CompositeLayer& layer, TimelineModulationTarget target) {
        if (target != TimelineModulationTarget::TotalLevel && target != TimelineModulationTarget::Feedback) return nullptr;
        const auto field = target == TimelineModulationTarget::TotalLevel
            ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel : mgstc::engine::OpllRegisterAutoTarget::Feedback;
        if (isLoopStartCount(layer, selected_count_) || !layer.register_auto_profiles.empty())
            return &mgstc::engine::editCompositeRegisterAutoAt(layer, field,
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)), loop_insert_after_);
        return target == TimelineModulationTarget::TotalLevel ? &layer.opll_tl_auto : &layer.opll_fb_auto;
    }

    void syncTimelineModulationControls() {
        const bool selected = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        for (std::size_t index = 0; index < modulation_toggles_.size(); ++index) {
            const auto target = static_cast<TimelineModulationTarget>(index);
            const bool supported = selected
                && timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                    .volume_envelope.kind == mgstc::engine::EnvelopeKind::Sequence
                && (index < 2 || timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                    .source == mgstc::engine::TimbreSource::Opll);
            bool active = false;
            if (supported) {
                auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
                active = modulationFor(layer, target).enabled;
                if (auto* curve = freeCurveFor(layer, target)) {
                    active = active || curve->mode
                        == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
                }
            }
            modulation_toggles_[index].setToggleState(
                active, juce::dontSendNotification);
            modulation_toggles_[index].setEnabled(supported);
            modulation_settings_[index].setEnabled(supported);
            modulation_labels_[index].setEnabled(supported);
        }
    }

    void applyTimelineModulation(
        std::size_t layer_index,
        TimelineModulationTarget target,
        mgstc::engine::ModulationParameters settings,
        bool free_curve) {
        if (layer_index >= timbre_.layers.size()) return;
        auto& layer = timbre_.layers[layer_index];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence)
            return;
        if ((target == TimelineModulationTarget::TotalLevel
                || target == TimelineModulationTarget::Feedback)
            && layer.source != mgstc::engine::TimbreSource::Opll) return;
        if (settings.enabled && (target == TimelineModulationTarget::TotalLevel
                || target == TimelineModulationTarget::Feedback)) {
            const auto register_target = target == TimelineModulationTarget::TotalLevel
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                : mgstc::engine::OpllRegisterAutoTarget::Feedback;
            if (!mgstc::engine::opllRegisterAutoOwnedByLayer(
                    timbre_, layer_index, register_target)) {
                reportStatus(juce::String::fromUTF8(
                    "TL/FB変調はそれぞれOPLL全体で1チャンネルのみ設定できます"));
                syncTimelineModulationControls();
                return;
            }
        }
        settings = mgstc::engine::clampModulation(settings);
        editableModulationFor(layer, target) = settings;
        if (auto* curve = editableFreeCurveFor(layer, target)) {
            if (free_curve) {
                editableModulationFor(layer, target).enabled = false;
                curve->mode = settings.enabled
                    ? mgstc::engine::OpllRegisterAutoMode::FreeCurve
                    : mgstc::engine::OpllRegisterAutoMode::Off;
                if (curve->free_curve.empty() && settings.enabled) {
                    curve->free_curve.push_back(curve->depth);
                }
            } else {
                curve->mode = mgstc::engine::OpllRegisterAutoMode::Off;
            }
        }
        static_cast<void>(mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        syncTimelineModulationControls();
        syncRegisterAutoEditors();
        updateEnvelopeMmlPreview();
        if (target == TimelineModulationTarget::Pitch) {
            lane_layout_dirty_ = true;
            resized();
        }
        repaint();
        if (edit_callback_) publishEdit(true, true);
    }

    void setTimelineModulationEnabled(
        TimelineModulationTarget target, bool enabled) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) return;
        const auto layer_index = static_cast<std::size_t>(selected_layer_);
        auto& layer = timbre_.layers[layer_index];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence)
            return;
        const auto* curve = freeCurveFor(layer, target);
        const bool free_curve = curve != nullptr
            && curve->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
        auto settings = modulationFor(layer, target);
        settings.enabled = enabled;
        applyTimelineModulation(layer_index, target, settings, free_curve);
    }

    void openTimelineModulationDialog(TimelineModulationTarget target,
        std::optional<int> clicked_count = std::nullopt) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) return;
        const auto layer_index = static_cast<std::size_t>(selected_layer_);
        auto& layer = timbre_.layers[layer_index];
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence)
            return;
        const auto* curve = freeCurveFor(layer, target);
        const bool free_curve = curve != nullptr
            && curve->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
        auto initial = modulationFor(layer, target);
        if (clicked_count) initial.delay = static_cast<std::uint8_t>(
            juce::jlimit(0, 255, *clicked_count));
        TimelineModulationWavePreview::Data preview_data;
        preview_data.target = target;
        const auto* library = timbreLibrary();
        const auto* volumes = target == TimelineModulationTarget::Tremolo
            ? &cachedVolumeSamples(layer_index, TimelineModulationWavePreview::kPreviewCounts + 1)
            : nullptr;
        auto preview_curve = curve ? *curve : mgstc::engine::OpllRegisterAutoLane{};
        preview_curve.mode = mgstc::engine::OpllRegisterAutoMode::FreeCurve;
        if (preview_curve.free_curve.empty()) preview_curve.free_curve.push_back(preview_curve.depth);
        const auto register_target = target == TimelineModulationTarget::TotalLevel
            ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
            : mgstc::engine::OpllRegisterAutoTarget::Feedback;
        for (int count = 0; count <= TimelineModulationWavePreview::kPreviewCounts; ++count) {
            auto& base = preview_data.base[static_cast<std::size_t>(count)];
            if (target == TimelineModulationTarget::Pitch) {
                base = cumulativePitchAt(layer, count);
            } else if (volumes) {
                base = static_cast<int>((*volumes)[static_cast<std::size_t>(count)]);
            } else if (const auto regs = mgstc::engine::opllOriginalRegisterImageAt(
                    layer, static_cast<std::uint32_t>(count), library, true, false)) {
                base = target == TimelineModulationTarget::TotalLevel
                    ? static_cast<int>((*regs)[2] & 0x3F)
                    : static_cast<int>((*regs)[3] & 0x07);
                preview_data.free_curve[static_cast<std::size_t>(count)] =
                    mgstc::engine::opllRegisterAutoValueAt(preview_curve, register_target,
                        static_cast<std::uint32_t>(count), layer.envelope_timeline.length_counts);
            }
        }
        UiScale::forceGlobalForNonEditorUi();
        auto* dialog = new ModalDialogWindow(
            timelineModulationName(target), juce::Colour(0xFF1B222C));
        auto* content = new TimelineModulationDialogContent(
            target, initial, free_curve, std::move(preview_data),
            [safe = juce::Component::SafePointer<Impl>(this), layer_index, target](
                const mgstc::engine::ModulationParameters& settings,
                bool use_free_curve) {
                if (safe != nullptr) {
                    safe->applyTimelineModulation(
                        layer_index, target, settings, use_free_curve);
                }
            });
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + UiLayout::fieldH);
        dialog->enterModalState(true, nullptr, true);
    }

    juce::Rectangle<int> inspectorArea() const {
        auto a = getLocalBounds().reduced(UiLayout::panelPad);
        a.removeFromTop(timelineChromeHeight());
        return inspector_visible_ ? a.removeFromRight(UiLayout::compositeInspectorColumnW) : juce::Rectangle<int>{};
    }
    juce::Rectangle<int> workspaceArea() const {
        auto a = getLocalBounds().reduced(UiLayout::panelPad);
        a.removeFromTop(timelineChromeHeight());
        if (inspector_visible_) a.removeFromRight(UiLayout::compositeInspectorColumnW + UiLayout::panelGap);
        return a;
    }
    int overviewRowHeight() const {
        return compact_overview_.getToggleState()
            ? UiLayout::fieldH + UiLayout::xs * 2 : UiLayout::fieldH * 2 + UiLayout::sm;
    }
    int overviewHeight() const {
        return juce::jmin(overviewRowHeight() * juce::jlimit(1, compact_overview_.getToggleState() ? 6 : 3, static_cast<int>(timbre_.layers.size())),
            juce::jmax(overviewRowHeight(), workspaceArea().getHeight() / 3));
    }
    juce::Rectangle<int> overviewArea() const {
        return workspaceArea().withHeight(overviewHeight());
    }
    void selectOverviewLayer(int index) {
        cancelLoopDrag();
        cancelStepDrag();
        if (index == selected_layer_) return;
        selected_layer_ = index;
        drawing_ = false; edit_draw_bounds_.reset();
        hover_preview_value_.reset(); hover_edit_parameter_.reset();
        vertical_scroll_.setCurrentRangeStart(0.0, juce::dontSendNotification);
        syncPointEditors(); syncRegisterAutoEditors(); syncInspector();
        refreshCountCeilingCache();
        resized(); repaint();
    }
    void layoutOverview() {
        const int count = static_cast<int>(timbre_.layers.size());
        while (overview_select_.size() > count) {
            overview_select_.removeLast();
            overview_mute_.removeLast(); overview_solo_.removeLast();
        }
        while (overview_select_.size() < count) {
            const int i = overview_select_.size();
            auto* select = overview_select_.add(new juce::TextButton());
            select->onClick = [this, i] { selectOverviewLayer(i); };
            overview_content_.addAndMakeVisible(select);
            for (auto* buttons : {&overview_mute_, &overview_solo_}) {
                auto* button = buttons->add(new juce::TextButton());
                button->setClickingTogglesState(true);
                button->onClick = [this, i, buttons] {
                    cancelStepDrag();
                    auto& layer = timbre_.layers[static_cast<std::size_t>(i)];
                    const bool on = (*buttons)[i]->getToggleState();
                    if (buttons == &overview_mute_) {
                        layer.enabled = true;
                        layer.muted = on;
                    }
                    else layer.solo = on;
                    layer_setups_[i]->syncFromLayer(layer);
                    repaint();
                    if (edit_callback_) publishEdit(true, false);
                };
                overview_content_.addAndMakeVisible(button);
            }
        }
        const int width = juce::jmax(1, overview_view_.getWidth() - kScrollBarSize);
        overview_content_.setSize(width, juce::jmax(overview_view_.getHeight(), count * overviewRowHeight()));
        for (int i = 0; i < count; ++i) {
            const auto& layer = timbre_.layers[static_cast<std::size_t>(i)];
            auto label = juce::Rectangle<int>(0, i * overviewRowHeight(), UiLayout::compositeOverviewLabelW, overviewRowHeight()).reduced(UiLayout::xs);
            const bool compact = compact_overview_.getToggleState();
            overview_select_[i]->setBounds(compact
                ? label.removeFromLeft(label.getWidth() - UiLayout::fieldH * 2 - UiLayout::sm * 2)
                : label.removeFromTop(UiLayout::fieldH));
            overview_select_[i]->setButtonText(juce::String::fromUTF8(layer.name.c_str()));
            overview_select_[i]->setTooltip(juce::String::fromUTF8(layer.name.c_str())
                + juce::String::fromUTF8(" — チャンネルを選択（音色・命令は変更しません）"));
            overview_select_[i]->setToggleState(i == selected_layer_, juce::dontSendNotification);
            overview_select_[i]->setColour(juce::TextButton::buttonOnColourId, sourceColour(layer.source).withAlpha(0.3F));
            const int button_w = label.getWidth() / 2;
            int n = 0;
            for (auto* button : {overview_mute_[i], overview_solo_[i]}) {
                button->setBounds(label.removeFromLeft(compact
                    ? UiLayout::fieldH + (n == 0 ? UiLayout::sm * 2 : 0) : button_w));
                button->setButtonText(n == 0 ? "M" : "S");
                button->setTooltip(juce::String::fromUTF8(n == 0 ? "ミュート" : "ソロ"));
                button->setToggleState(n == 0 ? layer.muted || !layer.enabled : layer.solo, juce::dontSendNotification);
                ++n;
            }
        }
        overview_content_.repaint();
    }
    void paintStartDelay(juce::Graphics& g, juce::Rectangle<int> plot, std::size_t index) {
        const auto& layer = timbre_.layers[index];
        const int delay = static_cast<int>(mgstc::engine::startDelayGridCounts(
            layer.start_delay_form, layer.start_delay_value, playback_tempo_));
        if (delay <= 0) return;
        const int width = juce::jlimit(0, plot.getWidth(), xForCount(plot, delay) - plot.getX());
        g.setColour(juce::Colours::black.withAlpha(
            mgstc::engine::layerIsAudible(timbre_, index) ? 0.28F : 0.14F));
        g.fillRect(plot.withWidth(width));
    }

    void paintOverview(juce::Graphics& g) {
        g.fillAll(juce::Colour(0xFF161B22));
        for (std::size_t i = 0; i < timbre_.layers.size(); ++i) {
            auto row = juce::Rectangle<int>(0, static_cast<int>(i) * overviewRowHeight(), overview_content_.getWidth(), overviewRowHeight());
            if (!g.getClipBounds().intersects(row)) continue;
            auto plot = row.withTrimmedLeft(UiLayout::compositeOverviewLabelW).reduced(UiLayout::sm, UiLayout::xs);
            const auto& layer = timbre_.layers[i];
            const auto colour = sourceColour(layer.source);
            g.setColour(juce::Colour(i == static_cast<std::size_t>(selected_layer_) ? 0xFF25343A : 0xFF20262E));
            g.fillRect(plot);
            drawCountGrid(g, plot);
            paintStartDelay(g, plot, i);
            if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
                const auto handles = rateEnvelopeHandlesForLayer(layer);
                paintRateEnvelopeGraph(g, plot, cachedLayerRateTrace(i), colour,
                    layer.source, false, &handles, -1, -1, false);
            } else {
                drawVolumeBars(g, plot, i, colour, 0.12F);
                drawAutomation(g, plot, i, colour.withAlpha(0.7F), Parameter::Pitch);
            }
            drawScope(g, plot, i, colour.withAlpha(0.6F));
            drawTimelineMarkers(g, plot, i, colour);
            drawOutputCutoff(g, plot, i);
        }
    }

    enum class Parameter : int {
        Volume = 1,
        Pitch = 2,
        Timbre = 3,
        OpllTlAuto = 4,
        OpllFbAuto = 5,
        RegisterWrite = 6,
    };

    enum class EditMode : int {
        Draw = 1,
        End = 2,
        LoopStart = 3,
        LoopEnd = 4,
    };

    static constexpr int kDefaultVisibleCounts = 32;
    static constexpr int kScrollBarSize = 15;
    static constexpr int kPitchCommandMin = -127;
    static constexpr int kPitchCommandMax = 127;

    [[nodiscard]] static bool isRegisterAutoParameter(
        Parameter parameter) noexcept {
        return parameter == Parameter::OpllTlAuto
            || parameter == Parameter::OpllFbAuto;
    }

    [[nodiscard]] bool selectedLayerShowsRegisterAuto() const noexcept {
        return selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)].source
                == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoLane* selectedRegisterAutoLane() {
        if (!selectedLayerShowsRegisterAuto()
            || !isRegisterAutoParameter(selectedParameter())) {
            return nullptr;
        }
        auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        return const_cast<mgstc::engine::OpllRegisterAutoLane*>(&mgstc::engine::compositeRegisterAutoAt(
            layer, selectedRegisterAutoTarget(), static_cast<std::uint32_t>(juce::jmax(0, selected_count_)),
            loop_insert_after_ || !isLoopStartCount(layer, selected_count_)));
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoLane* editableSelectedRegisterAutoLane() {
        if (!selectedLayerShowsRegisterAuto() || !isRegisterAutoParameter(selectedParameter())) return nullptr;
        return editableFreeCurveFor(timbre_.layers[static_cast<std::size_t>(selected_layer_)],
            selectedParameter() == Parameter::OpllTlAuto ? TimelineModulationTarget::TotalLevel : TimelineModulationTarget::Feedback);
    }

    [[nodiscard]] const mgstc::engine::OpllRegisterAutoLane*
    selectedRegisterAutoLane() const {
        return const_cast<Impl*>(this)
            ->selectedRegisterAutoLane();
    }

    [[nodiscard]] mgstc::engine::OpllRegisterAutoTarget
    selectedRegisterAutoTarget() const noexcept {
        return selectedParameter() == Parameter::OpllFbAuto
            ? mgstc::engine::OpllRegisterAutoTarget::Feedback
            : mgstc::engine::OpllRegisterAutoTarget::TotalLevel;
    }

    void configureAutoParamEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
        editor.setInputRestrictions(3, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] {
            applyRegisterAutoFromEditors(true);
        };
        addAndMakeVisible(editor);
    }

    [[nodiscard]] const mgstc::engine::TimbreLibrary* timbreLibrary() const {
        return timbre_library_callback_ != nullptr
            ? timbre_library_callback_()
            : nullptr;
    }

    [[nodiscard]] bool registerAutoAvailableAtSelectedCount() const {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return false;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source != mgstc::engine::TimbreSource::Opll) {
            return false;
        }
        return mgstc::engine::opllRegisterAutoAvailableAt(
            layer, static_cast<std::uint32_t>(juce::jmax(0, selected_count_)),
            !isLoopStartCount(layer, selected_count_) || loop_insert_after_);
    }

    void syncRegisterAutoEditors() {
        const auto* lane = selectedRegisterAutoLane();
        const bool selected =
            selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        const bool enabled = lane != nullptr;
        const auto target = selectedRegisterAutoTarget();
        const bool owned = enabled
            && selected
            && mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_,
                   static_cast<std::size_t>(selected_layer_),
                   target);
        const bool count_ok =
            enabled && owned && registerAutoAvailableAtSelectedCount();
        // Other OPLL layers may view Off, but cannot take ownership.
        // The common dialog owns mode changes; this row only edits FreeCurve.
        auto_mode_.setEnabled(false);
        auto_depth_.setEnabled(enabled && owned);
        auto_speed_.setEnabled(enabled && owned);
        auto_coarseness_.setEnabled(enabled && owned);
        auto_stop_.setEnabled(enabled && owned);
        if (!enabled) {
            return;
        }
        auto_mode_.setSelectedId(
            static_cast<int>(lane->mode) + 1,
            juce::dontSendNotification);
        auto_depth_.setText(juce::String(static_cast<int>(lane->depth)), false);
        auto_speed_.setText(
            juce::String(static_cast<int>(lane->change_speed)), false);
        auto_coarseness_.setText(
            juce::String(static_cast<int>(lane->coarseness)), false);
        auto_stop_.setText(
            juce::String(static_cast<int>(lane->stop_position)), false);
        if (!owned) {
            const auto owner = mgstc::engine::opllRegisterAutoOwnerLayer(
                timbre_, target);
            juce::String owner_label = juce::String::fromUTF8("別のOPLL ch");
            if (owner.has_value()
                && *owner < timbre_.layers.size()) {
                owner_label = juce::String::fromUTF8("OPLL Ch.")
                    + juce::String(
                          static_cast<int>(
                              timbre_.layers[*owner].channel) + 1);
            }
            const auto kind = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? juce::String::fromUTF8("TL変調")
                : juce::String::fromUTF8("FB変調");
            auto_mode_.setTooltip(
                kind + juce::String::fromUTF8("はチップ全体で1本のみ。")
                + owner_label
                + juce::String::fromUTF8("が使用中のため、このchでは設定できません"
                                         "（TLとFBを別chへ分けるのは可）"));
        } else if (!count_ok && isRegisterAutoParameter(selectedParameter())) {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "選択カウントはOPLL ROM音色区間のため、TL/FB変調のyは出力されません"));
        } else {
            auto_mode_.setTooltip(juce::String::fromUTF8(
                "TL/FB変調の時間変化モード。出力は展開したyレジスタ書き込み。"
                "TL・FBはそれぞれOPLL全体で1本（別ch分担可）"));
        }
    }

    void applyRegisterAutoFromEditors(bool set_start_count) {
        cancelStepDrag();
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto target = selectedRegisterAutoTarget();
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto next_mode =
            static_cast<mgstc::engine::OpllRegisterAutoMode>(
                juce::jlimit(0, 4, auto_mode_.getSelectedId() - 1));
        if (next_mode != mgstc::engine::OpllRegisterAutoMode::Off
            && !mgstc::engine::opllRegisterAutoOwnedByLayer(
                   timbre_, layer_index, target)) {
            reportStatus(juce::String::fromUTF8(
                "TL/FB変調はそれぞれOPLL全体で1チャンネルのみ設定できます"
                "（例: 1ch=FB・2ch=TL の分担は可）"));
            syncRegisterAutoEditors();
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        if (set_start_count && !registerAutoAvailableAtSelectedCount()) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB変調の開始ステップを置けません"));
                syncRegisterAutoEditors();
                return;
        }
        lane = editableSelectedRegisterAutoLane();
        lane->mode = next_mode;
        if (set_start_count) {
            lane->start_count = static_cast<std::uint32_t>(
                juce::jmax(0, selected_count_));
        }
        lane->depth = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_depth_.getText().getIntValue()));
        lane->change_speed = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_speed_.getText().getIntValue()));
        lane->coarseness = static_cast<std::uint8_t>(juce::jlimit(
            1, 255, auto_coarseness_.getText().getIntValue()));
        lane->stop_position = static_cast<std::uint8_t>(juce::jlimit(
            0, maximum, auto_stop_.getText().getIntValue()));
        if (lane->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve
            && lane->free_curve.empty()
            && set_start_count) {
            lane->free_curve.push_back(lane->depth);
        }
        if (lane->mode == mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
            auto& layer = timbre_.layers[layer_index];
            editableModulationFor(layer,
                target == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                    ? TimelineModulationTarget::TotalLevel
                    : TimelineModulationTarget::Feedback).enabled = false;
        }
        static_cast<void>(
            mgstc::engine::enforceOpllRegisterAutoExclusivity(timbre_));
        syncRegisterAutoEditors();
        syncTimelineModulationControls();
        updateEnvelopeMmlPreview();
        repaint();
        if (edit_callback_) {
            publishEdit(true, true);
        }
    }

    void editRegisterAutoFreeCurve(
        int timeline_count,
        int parameter_value,
        bool commit) {
        auto* lane = selectedRegisterAutoLane();
        if (lane == nullptr
            || lane->mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
            return;
        }
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto layer_index =
            static_cast<std::size_t>(selected_layer_);
        const auto target = selectedRegisterAutoTarget();
        if (!mgstc::engine::opllRegisterAutoOwnedByLayer(
                timbre_, layer_index, target)) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "このTL/FB変調は他のOPLLチャンネルが使用中です"));
            }
            return;
        }
        const auto& layer = timbre_.layers[layer_index];
        if (!mgstc::engine::opllRegisterAutoAvailableAt(
                layer,
                static_cast<std::uint32_t>(juce::jmax(0, timeline_count)),
                !isLoopStartCount(layer, timeline_count) || loop_insert_after_)) {
            if (commit) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間ではTL/FB変調を描けません"));
            }
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        parameter_value = juce::jlimit(0, maximum, parameter_value);
        timeline_count = juce::jmax(0, timeline_count);
        lane = editableSelectedRegisterAutoLane();
        const auto step = juce::jmax<std::uint8_t>(1, lane->coarseness);
        if (lane->free_curve.empty()) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
        }
        if (static_cast<std::uint32_t>(timeline_count) < lane->start_count) {
            lane->start_count = static_cast<std::uint32_t>(timeline_count);
            lane->free_curve.clear();
        }
        const auto offset =
            (static_cast<std::uint32_t>(timeline_count) - lane->start_count)
            / step;
        if (lane->free_curve.size() <= offset) {
            const auto fill = lane->free_curve.empty()
                ? static_cast<std::uint8_t>(parameter_value)
                : lane->free_curve.back();
            lane->free_curve.resize(offset + 1, fill);
        }
        lane->free_curve[offset] =
            static_cast<std::uint8_t>(parameter_value);
        syncPointEditors();
        if (commit && edit_callback_) {
            publishEdit(true, true);
        }
        repaint();
    }

    [[nodiscard]] bool pitchLaneExpanded() const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size()))
            return false;
        const auto index = static_cast<std::size_t>(selected_layer_);
        const auto& layer = timbre_.layers[index];
        return layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate
            || layer.software_lfo.enabled || hasModulation(layer, mgstc::engine::CompositeModulationTarget::Pitch)
            || (index < pitch_lane_expanded_.size()
                && pitch_lane_expanded_[index]);
    }

    [[nodiscard]] int commandSummaryHeight() const noexcept {
        return juce::jmax(UiLayout::commandSummaryH,
            juce::roundToInt(UiFonts::dense(true).getHeight() + UiFonts::body().getHeight()) + UiLayout::xs);
    }

    [[nodiscard]] int laneHeightForIndex(int lane_index) const noexcept {
        if (lane_index == static_cast<int>(timbre_.layers.size())) {
            return UiLayout::compositeMixLaneH;
        }
        if (lane_index == selected_layer_) {
            if (lane_index >= 0
                && lane_index < static_cast<int>(timbre_.layers.size())
                && timbre_.layers[static_cast<std::size_t>(lane_index)]
                    .volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
                return UiLayout::editLaneFramePadV * 2
                    + UiLayout::editLaneInnerPadV * 2
                    + commandSummaryHeight() + UiLayout::editVolumeLaneH;
            }
            if (lane_index >= 0
                && lane_index < static_cast<int>(timbre_.layers.size())
                && timbre_.layers[static_cast<std::size_t>(lane_index)].source
                    == mgstc::engine::TimbreSource::Opll) {
                return UiLayout::compositeEditLaneH - UiLayout::envelopePreviewH
                    + commandSummaryHeight() - UiLayout::commandSummaryH
                    + UiLayout::compositeEditLaneOpllExtraH
                    - (pitchLaneExpanded() ? 0
                        : UiLayout::editPitchLaneMinH - UiLayout::fieldH);
            }
            return UiLayout::compositeEditLaneH - UiLayout::envelopePreviewH
                + commandSummaryHeight() - UiLayout::commandSummaryH
                - (pitchLaneExpanded() ? 0
                    : UiLayout::editPitchLaneMinH - UiLayout::fieldH);
        }
        return UiLayout::compositeChannelLaneH;
    }

    [[nodiscard]] int totalLaneStackHeight() const noexcept {
        return selected_layer_ >= 0 ? laneHeightForIndex(selected_layer_) : 1;
    }

    [[nodiscard]] int lanePixelOffset(int lane_index) const noexcept {
        juce::ignoreUnused(lane_index);
        return 0;
    }

    [[nodiscard]] int maximumCount() const noexcept {
        return cached_count_ceiling_;
    }

    [[nodiscard]] int maximumCountForSelectedLayer() const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return cached_count_ceiling_;
        }
        return cached_count_ceiling_;
    }

    [[nodiscard]] int contentHorizonCount() const noexcept {
        std::uint64_t horizon = 1;
        const auto include_events =
            [&horizon](const std::vector<mgstc::engine::EnvelopeEvent>& events) {
                for (const auto& event : events) {
                    horizon = std::max<std::uint64_t>(
                        horizon, event.count);
                }
            };
        const auto include_auto =
            [&horizon](const mgstc::engine::OpllRegisterAutoLane& lane) {
                if (!lane.active()) {
                    return;
                }
                std::uint64_t last = lane.start_count;
                if (!lane.free_curve.empty()) {
                    last += static_cast<std::uint64_t>(
                                lane.free_curve.size() - 1)
                        * juce::jmax<std::uint8_t>(1, lane.coarseness);
                }
                horizon = std::max(horizon, last);
            };
        for (const auto& layer : timbre_.layers) {
            const auto& timeline = layer.envelope_timeline;
            horizon = std::max<std::uint64_t>(
                horizon, timeline.length_counts);
            if (timeline.loop_start_count) {
                horizon = std::max<std::uint64_t>(
                    horizon, *timeline.loop_start_count);
            }
            if (timeline.loop_end_count) {
                horizon = std::max<std::uint64_t>(
                    horizon, *timeline.loop_end_count);
            }
            include_events(layer.volume_envelope.events);
            include_events(layer.pitch_envelope.events);
            include_events(layer.timbre_automation);
            include_auto(layer.opll_tl_auto);
            include_auto(layer.opll_fb_auto);
        }
        return static_cast<int>(std::min<std::uint64_t>(
            horizon, static_cast<std::uint64_t>(maximumCount())));
    }

    [[nodiscard]] int scrollExpansionCounts() const noexcept {
        return juce::jmax(
            kDefaultVisibleCounts,
            static_cast<int>(std::ceil(
                horizontal_scroll_.getCurrentRangeSize())));
    }

    [[nodiscard]] int initialScrollExtentForContent() const noexcept {
        return juce::jlimit(
            1,
            maximumCount(),
            contentHorizonCount() + scrollExpansionCounts());
    }

    void resetScrollExtentToContent() {
        scroll_extent_counts_ = initialScrollExtentForContent();
        scroll_extent_initialized_ = true;
        horizontal_scroll_.setCurrentRangeStart(
            0.0, juce::dontSendNotification);
    }

    void ensureScrollExtentIncludesContent() {
        scroll_extent_counts_ = juce::jlimit(
            1,
            maximumCount(),
            juce::jmax(
                scroll_extent_counts_,
                initialScrollExtentForContent()));
        scroll_extent_initialized_ = true;
    }

    void maybeExpandScrollExtent(int target_count) {
        if (scroll_extent_counts_ >= maximumCount()
            || target_count < scroll_extent_counts_ - 1) {
            return;
        }
        const int expanded = juce::jlimit(
            1,
            maximumCount(),
            juce::jmax(
                target_count,
                scroll_extent_counts_ + scrollExpansionCounts()));
        if (expanded == scroll_extent_counts_) {
            return;
        }
        scroll_extent_counts_ = expanded;
        horizontal_scroll_.setRangeLimits(
            0.0,
            static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
    }

    void refreshCountCeilingCache() {
        syncMorphBank();
        cached_count_ceiling_ = static_cast<int>(
            mgstc::engine::kMgscEnvelopeUiLengthCap);
        output_cutoff_counts_.assign(timbre_.layers.size(), std::nullopt);
        const auto& preview = morph_preview_.valid ? morph_preview_.timbre : timbre_;
        const auto numbers = mgstc::engine::resolveTimbreNumbers(preview);
        const auto* library = timbreLibrary();
        for (std::size_t index = 0; index < timbre_.layers.size(); ++index) {
            const auto& layer = preview.layers[index];
            if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence) {
                continue;
            }
            const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
                layer,
                layer.envelope_number,
                mgstc::engine::kMgscEnvelopeCompiledByteLimit,
                &numbers,
                library);
            output_cutoff_counts_[index] = formatted.output_cutoff_count;
        }
    }

    void clampLayerToTimelineRange(mgstc::engine::CompositeLayer& layer) {
        auto& timeline = layer.envelope_timeline;
        if (timeline.loop_start_count
            && *timeline.loop_start_count > timeline.length_counts) {
            timeline.loop_start_count = timeline.length_counts;
        }
        if (timeline.loop_end_count
            && *timeline.loop_end_count > timeline.length_counts) {
            timeline.loop_end_count = timeline.length_counts;
        }
    }

    void clampAllLayersToTimelineRange() {
        for (auto& layer : timbre_.layers) {
            clampLayerToTimelineRange(layer);
        }
        refreshCountCeilingCache();
    }

    [[nodiscard]] EditMode selectedEditMode() const noexcept {
        return static_cast<EditMode>(edit_mode_.getSelectedId());
    }

    [[nodiscard]] Parameter selectedParameter() const noexcept {
        return static_cast<Parameter>(parameter_.getSelectedId());
    }

    [[nodiscard]] juce::String selectedParameterName() const {
        switch (selectedParameter()) {
        case Parameter::Volume:
            return juce::String::fromUTF8("音量");
        case Parameter::Pitch:
            return juce::String::fromUTF8("音程");
        case Parameter::Timbre:
            return juce::String::fromUTF8("音色番号");
        case Parameter::RegisterWrite:
            return juce::String::fromUTF8("Y-Register y");
        case Parameter::OpllTlAuto:
            return juce::String::fromUTF8("TL変調");
        case Parameter::OpllFbAuto:
            return juce::String::fromUTF8("FB変調");
        }
        return {};
    }

    void configureInspectorEditor(
        juce::TextEditor& editor,
        const juce::String& tooltip) {
        UiFonts::styleBodyField(editor);
        editor.setInputRestrictions(5, "0123456789");
        editor.setJustification(juce::Justification::centred);
        editor.setTextToShowWhenEmpty(
            juce::String::fromUTF8("なし"), juce::Colour(0xFF8896A3));
        editor.setTooltip(tooltip);
        editor.onReturnKey = [this] { applyInspector(); };
        addAndMakeVisible(editor);
    }

    void configureInspectorLabel(
        juce::Label& label,
        const juce::String& text) {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
    }

    [[nodiscard]] std::optional<std::uint32_t> inspectorCount(
        const juce::TextEditor& editor,
        std::uint32_t length) const {
        const auto text = editor.getText().trim();
        if (text.isEmpty()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(juce::jlimit(
            0, static_cast<int>(length), text.getIntValue()));
    }

    void syncInspector() {
        syncMorphBank();
        syncTimelineModulationControls();
        const bool has_selection = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size());
        length_editor_.setEnabled(has_selection);
        loop_start_editor_.setEnabled(has_selection);
        loop_end_editor_.setEnabled(has_selection);
        inspector_apply_.setEnabled(has_selection);
        clear_loop_.setEnabled(has_selection);
        if (!has_selection) {
            inspector_selection_.setText(
                juce::String::fromUTF8("チャンネル未選択"),
                juce::dontSendNotification);
            length_editor_.setText({}, false);
            loop_start_editor_.setText({}, false);
            loop_end_editor_.setText({}, false);
            command_stack_.clearStack();
            return;
        }

        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto& timeline = layer.envelope_timeline;
        inspector_selection_.setText(
            juce::String::fromUTF8(layer.name.c_str())
                + juce::String::fromUTF8(" / ソフトウェアEG"),
            juce::dontSendNotification);
        length_editor_.setText(
            juce::String(static_cast<int>(timeline.length_counts)), false);
        loop_start_editor_.setText(
            timeline.loop_start_count
                ? juce::String(static_cast<int>(*timeline.loop_start_count))
                : juce::String{},
            false);
        loop_end_editor_.setText(
            timeline.loop_end_count
                ? juce::String(static_cast<int>(*timeline.loop_end_count))
                : juce::String{},
            false);
        refreshCommandStack(layer);
        layoutEditToolsDock();
    }

    void applyInspector() {
        cancelLoopDrag();
        cancelStepDrag();
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto length = static_cast<std::uint32_t>(juce::jlimit(
            1,
            maximumCountForSelectedLayer(),
            length_editor_.getText().getIntValue()));
        auto loop_start = inspectorCount(
            loop_start_editor_, length);
        auto loop_end = inspectorCount(
            loop_end_editor_, length);
        auto candidate = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        candidate.envelope_timeline.length_counts = length;
        if (!loop_start) {
            mgstc::engine::clearCompositeLoop(candidate);
        } else {
            if (!mgstc::engine::setCompositeLoopRange(candidate, *loop_start, loop_end.value_or(length))) {
                reportStatus(juce::String::fromUTF8("開始は終了より前、終了はEND以内に設定してください"));
                syncInspector(); return;
            }
        }
        timbre_.layers[static_cast<std::size_t>(selected_layer_)] = std::move(candidate);
        refreshCountCeilingCache();
        maybeExpandScrollExtent(static_cast<int>(length));
        syncInspector();
        updateScrollRanges();
        repaint();
        if (edit_callback_) {
            publishEdit(true, true);
        }
    }

    void clearSelectedLoop() {
        if (selected_layer_ >= 0) clearLoop(static_cast<std::size_t>(selected_layer_));
    }

    [[nodiscard]] std::pair<int, int> valueRange() const noexcept {
        return valueRangeFor(selectedParameter());
    }

    [[nodiscard]] static std::pair<int, int> valueRangeFor(
        Parameter parameter) noexcept {
        switch (parameter) {
        case Parameter::Volume:
            return {0, 15};
        case Parameter::Pitch:
            return {kPitchCommandMin, kPitchCommandMax};
        case Parameter::Timbre:
            return {0, 31};
        case Parameter::RegisterWrite:
            return {0, 255};
        case Parameter::OpllTlAuto:
            return {0, 63};
        case Parameter::OpllFbAuto:
            return {0, 7};
        }
        return {0, 15};
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    selectedEvents(const mgstc::engine::CompositeLayer& layer) const {
        return eventsFor(layer, selectedParameter());
    }

    [[nodiscard]] const std::vector<mgstc::engine::EnvelopeEvent>&
    eventsFor(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter) const {
        if (parameter == Parameter::Volume) {
            return layer.volume_envelope.events;
        }
        if (parameter == Parameter::Pitch) {
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    [[nodiscard]] mgstc::engine::EnvelopeEventKind selectedEventKind() const {
        return eventKindFor(selectedParameter());
    }

    [[nodiscard]] static mgstc::engine::EnvelopeEventKind eventKindFor(
        Parameter parameter) noexcept {
        if (parameter == Parameter::Volume) {
            return mgstc::engine::EnvelopeEventKind::Volume;
        }
        if (parameter == Parameter::Pitch) {
            return mgstc::engine::EnvelopeEventKind::Pitch;
        }
        if (parameter == Parameter::RegisterWrite) {
            return mgstc::engine::EnvelopeEventKind::RegisterWrite;
        }
        return mgstc::engine::EnvelopeEventKind::Timbre;
    }

    [[nodiscard]] const mgstc::engine::EnvelopeTimeline& selectedTimeline(
        const mgstc::engine::CompositeLayer& layer) const {
        return layer.envelope_timeline;
    }

    [[nodiscard]] mgstc::engine::EnvelopeTimeline& selectedTimeline(
        mgstc::engine::CompositeLayer& layer) {
        return const_cast<mgstc::engine::EnvelopeTimeline&>(
            std::as_const(*this).selectedTimeline(layer));
    }

    juce::PopupMenu makeAddChannelMenu() const {
        juce::PopupMenu menu;
        int id = 1;
        for (const auto source : {mgstc::engine::TimbreSource::Psg,
                 mgstc::engine::TimbreSource::Scc, mgstc::engine::TimbreSource::Opll}) {
            const int used = static_cast<int>(std::count_if(timbre_.layers.begin(), timbre_.layers.end(),
                [source](const auto& layer) { return layer.source == source; }));
            const char* name = id == 1 ? "PSG" : id == 2 ? "SCC" : "OPLL";
            const int maximum = id == 1 ? 3 : id == 2 ? 5 : 9;
            menu.addItem(id++, juce::String(name) + juce::String::fromUTF8(" 追加  (")
                + juce::String(used) + "/" + juce::String(maximum) + ")",
                mgstc::engine::firstAvailableChannel(timbre_, source).has_value());
        }
        return menu;
    }
    void addChannelFromMenu(int id) {
        if (id == 1) addLayer(mgstc::engine::TimbreSource::Psg);
        else if (id == 2) addLayer(mgstc::engine::TimbreSource::Scc);
        else if (id == 3) addLayer(mgstc::engine::TimbreSource::Opll);
    }

    void updateAddButtons() {
        psg_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Psg).has_value());
        scc_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Scc).has_value());
        opll_add_.setEnabled(
            mgstc::engine::firstAvailableChannel(
                timbre_, mgstc::engine::TimbreSource::Opll).has_value());
        for (int index = 0; index < layer_setups_.size(); ++index) {
            if (index >= static_cast<int>(timbre_.layers.size())) {
                break;
            }
            layer_setups_[index]->setDuplicateEnabled(
                mgstc::engine::firstAvailableChannel(
                    timbre_, timbre_.layers[static_cast<std::size_t>(index)]
                                 .source)
                    .has_value());
        }
    }

    void addLayer(mgstc::engine::TimbreSource source) {
        cancelStepDrag();
        const auto free_channel = mgstc::engine::firstAvailableChannel(
            timbre_, source);
        if (!free_channel) {
            return;
        }
        const auto defaults = mgstc::engine::defaultCompositeTimbre();
        const auto template_layer = std::find_if(
            defaults.layers.begin(), defaults.layers.end(),
            [source](const auto& layer) {
                return layer.source == source;
            });
        if (template_layer == defaults.layers.end()) {
            return;
        }
        auto layer = *template_layer;
        layer.channel = *free_channel;
        const char* source_name = source == mgstc::engine::TimbreSource::Psg
            ? "PSG"
            : source == mgstc::engine::TimbreSource::Scc ? "SCC" : "OPLL";
        layer.name = std::string(source_name) + " Ch."
            + std::to_string(static_cast<int>(layer.channel) + 1);
        layer.envelope_number =
            mgstc::engine::nextFreeEnvelopeNumber(timbre_);
        mgstc::engine::seedDefaultLayerTimbre(timbre_, layer);
        timbre_.layers.push_back(std::move(layer));
        selected_layer_ = static_cast<int>(timbre_.layers.size()) - 1;
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
        repaint();
        if (edit_callback_) {
            publishEdit(true, false);
        }
    }

    void requestRemoveLayer(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        const auto layer_name = juce::String::fromUTF8(
            timbre_.layers[layer_index].name.c_str());
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle(juce::String::fromUTF8("チャンネル削除"))
                .withMessage(
                    layer_name
                    + juce::String::fromUTF8(
                        " とそのエンベロープを削除しますか？"))
                .withButton(juce::String::fromUTF8("削除"))
                .withButton(juce::String::fromUTF8("キャンセル")),
            [safe = juce::Component::SafePointer<Impl>(this), layer_index, generation = dialog_generation_](
                int result) {
                if (safe != nullptr && result == 1 && safe->dialog_generation_ == generation) {
                    safe->removeLayerAt(layer_index);
                }
            });
    }

    void removeLayerAt(std::size_t layer_index) {
        cancelStepDrag();
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        const int removed = static_cast<int>(layer_index);
        if (!mgstc::engine::removeCompositeLayer(timbre_, layer_index)) {
            return;
        }
        if (layer_index < envelope_number_active_.size())
            envelope_number_active_.erase(
                envelope_number_active_.begin()
                    + static_cast<std::ptrdiff_t>(layer_index));
        if (timbre_.layers.empty()) {
            selected_layer_ = -1;
        } else if (selected_layer_ == removed) {
            selected_layer_ = juce::jmin(
                selected_layer_,
                static_cast<int>(timbre_.layers.size()) - 1);
        } else if (selected_layer_ > removed) {
            --selected_layer_;
        }
        selected_count_ = 0;
        position_.setText("ct 0", juce::dontSendNotification);
        refreshCountCeilingCache();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        closeRateEditor(true);
        resized();
        repaint();
        if (edit_callback_) {
            publishEdit(true, false);
        }
    }

    void duplicateLayerAt(std::size_t layer_index) {
        cancelStepDrag();
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        if (layer_index < static_cast<std::size_t>(layer_setups_.size())) {
            layer_setups_[static_cast<int>(layer_index)]->applyToLayer(
                timbre_.layers[layer_index]);
        }
        const auto copied = mgstc::engine::duplicateCompositeLayer(
            timbre_, layer_index);
        if (!copied) {
            updateAddButtons();
            return;
        }
        selected_layer_ = static_cast<int>(*copied);
        refreshCountCeilingCache();
        ensureScrollExtentIncludesContent();
        updateScrollRanges();
        ensureSelectedLaneVisible();
        updateAddButtons();
        rebuildLayerSetups();
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        resized();
        repaint();
        if (edit_callback_) {
            publishEdit(true, false);
        }
    }

    std::vector<mgstc::engine::EnvelopeEvent>& selectedEvents(
        mgstc::engine::CompositeLayer& layer) {
        if (selectedParameter() == Parameter::Volume) {
            if (layer.volume_envelope.kind
                != mgstc::engine::EnvelopeKind::Rate) {
                layer.volume_envelope.kind =
                    mgstc::engine::EnvelopeKind::Sequence;
            }
            return layer.volume_envelope.events;
        }
        if (selectedParameter() == Parameter::Pitch) {
            layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
            return layer.pitch_envelope.events;
        }
        return layer.timbre_automation;
    }

    [[nodiscard]] static bool isDisplayOnlyAutomaticVolumeCount(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count) {
        const mgstc::engine::EnvelopeEvent* next = nullptr;
        std::uint32_t origin{};
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Volume) continue;
            // An authored point always remains editable. The serializer stops
            // at every manual point before searching for the next ramp target.
            if (event.count == count) return false;
            if (event.count < count) origin = std::max(origin, event.count);
            else if (!next || event.count < next->count) next = &event;
        }
        return next && next->automatic && count > origin;
    }
    void editFromMouse(const juce::MouseEvent& event, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto point = event.getPosition();
        const auto bounds = edit_draw_bounds_.value_or(
            timelinePlotBounds(
                graph_bounds_[static_cast<std::size_t>(selected_layer_)]));
        if (bounds.isEmpty()) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter == Parameter::Timbre
            || parameter == Parameter::RegisterWrite) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            return;
        }
        const int count = countAtX(bounds, point.x);
        if (parameter == Parameter::Volume
            && isDisplayOnlyAutomaticVolumeCount(
                layer, static_cast<std::uint32_t>(count))) {
            return;
        }
        if (isRegisterAutoParameter(parameter)) {
            const auto [minimum, maximum] = valueRangeFor(parameter);
            editRegisterAutoFreeCurve(
                count,
                valueFromY(bounds, point.y, minimum, maximum),
                commit);
            return;
        }
        if (parameter == Parameter::Pitch) {
            const auto range = pitchDisplayRange(layer);
            editPitchCumulative(
                count,
                pitchValueFromY(
                    bounds, point.y, range.second),
                commit,
                event.mods.isShiftDown());
            return;
        }
        editPoint(
            count,
            volumeValueFromY(layer.source, bounds, point.y),
            commit);
    }

    void updateDrawHoverPopup(juce::Point<int> point) {
        hover_mouse_pos_ = point;
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            hover_preview_value_.reset();
            return;
        }
        if (!edit_draw_bounds_ || edit_draw_bounds_->isEmpty()
            || selected_layer_ < 0
            || selected_layer_
                >= static_cast<int>(timbre_.layers.size())) {
            hover_preview_value_.reset();
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (parameter == Parameter::Pitch) {
            const auto range = pitchDisplayRange(layer);
            hover_preview_value_ = pitchValueFromY(
                *edit_draw_bounds_,
                point.y,
                range.second);
        } else if (parameter == Parameter::Volume) {
            hover_preview_value_ = volumeValueFromY(
                layer.source, *edit_draw_bounds_, point.y);
        } else {
            hover_preview_value_ = valueFromY(
                *edit_draw_bounds_, point.y, parameter);
        }
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        int minimum,
        int maximum) noexcept {
        if (bounds.isEmpty()) {
            return minimum;
        }
        return juce::jlimit(
            minimum, maximum,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - y)
                * (maximum - minimum) / juce::jmax(1, bounds.getHeight())
                + minimum));
    }

    [[nodiscard]] static int valueFromY(
        juce::Rectangle<int> bounds,
        int y,
        Parameter parameter) noexcept {
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return valueFromY(bounds, y, minimum, maximum);
    }

    [[nodiscard]] static int volumeValueFromY(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int y) noexcept {
        juce::ignoreUnused(source);
        if (bounds.isEmpty()) {
            return 0;
        }
        const int height = juce::jmax(1, bounds.getHeight());
        const int y_clamped = juce::jlimit(
            bounds.getY(), bounds.getBottom() - 1, y);
        return juce::jlimit(
            0,
            15,
            juce::roundToInt(
                static_cast<double>(bounds.getBottom() - y_clamped)
                * 15.0
                / static_cast<double>(height)));
    }

    [[nodiscard]] static float volumeYFromValue(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int volume) noexcept {
        juce::ignoreUnused(source);
        const int height = juce::jmax(1, bounds.getHeight());
        return static_cast<float>(
            bounds.getBottom()
            - juce::roundToInt(
                  static_cast<double>(juce::jlimit(0, 15, volume))
                  * static_cast<double>(height) / 15.0));
    }

    [[nodiscard]] static float chipTransferYFromValue(
        mgstc::engine::TimbreSource source,
        juce::Rectangle<int> bounds,
        int envelope_volume,
        int track_volume) noexcept {
        const int height = juce::jmax(1, bounds.getHeight());
        const auto output_volume =
            mgstc::engine::sequenceOutputVolume(
                static_cast<std::uint8_t>(juce::jlimit(
                    0, 15, envelope_volume)),
                static_cast<std::uint8_t>(juce::jlimit(
                    0, 15, track_volume)));
        const int pixels =
            mgstc::engine::chipVolumeDisplayPixelsFromBottom(
                source, output_volume, height);
        return static_cast<float>(bounds.getBottom() - pixels);
    }

    // First editable frequency unit is 1 screen pixel from the 0 line.
    // Remaining |value| maps onto the rest of the half-lane (existing extent scale).
    [[nodiscard]] static int pitchValueFromY(
        juce::Rectangle<int> bounds,
        int y,
        int extent) noexcept {
        if (bounds.isEmpty()) {
            return 0;
        }
        const float centre = static_cast<float>(bounds.getCentreY());
        const float half = static_cast<float>(bounds.getHeight()) * 0.5F;
        const float dy = centre - static_cast<float>(y);
        const float first_px = 1.0F;
        if (std::abs(dy) <= first_px) {
            return juce::roundToInt(dy);
        }
        const int mag_extent = juce::jmax(1, extent);
        const float remaining = juce::jmax(0.0F, half - first_px);
        if (remaining <= 0.0F) {
            return dy >= 0.0F ? mag_extent : -mag_extent;
        }
        const float extra = std::abs(dy) - first_px;
        const float t = juce::jlimit(0.0F, 1.0F, extra / remaining);
        const int mag = 1
            + juce::roundToInt(t * static_cast<float>(mag_extent - 1));
        return dy >= 0.0F ? mag : -mag;
    }

    [[nodiscard]] static float pitchYFromValue(
        juce::Rectangle<int> bounds,
        int value,
        int extent) noexcept {
        const float centre = static_cast<float>(bounds.getCentreY());
        const float half = static_cast<float>(bounds.getHeight()) * 0.5F;
        const float first_px = 1.0F;
        if (std::abs(value) <= 1) {
            return centre - static_cast<float>(value) * first_px;
        }
        const int mag_extent = juce::jmax(1, extent);
        const float remaining = juce::jmax(0.0F, half - first_px);
        const float t = mag_extent <= 1
            ? 1.0F
            : static_cast<float>(std::abs(value) - 1)
                / static_cast<float>(mag_extent - 1);
        const float pixels = first_px + juce::jlimit(0.0F, 1.0F, t) * remaining;
        return centre - std::copysign(pixels, static_cast<float>(value));
    }

    [[nodiscard]] int storedValueAt(
        const mgstc::engine::CompositeLayer& layer,
        Parameter parameter,
        int timeline_count) const {
        if (isRegisterAutoParameter(parameter)) {
            const auto target = parameter == Parameter::OpllTlAuto
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                : mgstc::engine::OpllRegisterAutoTarget::Feedback;
            const auto count = static_cast<std::uint32_t>(
                juce::jmax(0, timeline_count));
            // Prefer effective field after @ / manual y / TL·FB auto (§7.5).
            if (const auto regs = mgstc::engine::opllOriginalRegisterImageAt(
                    layer, count, timbreLibrary(), true, true,
                    !isLoopStartCount(layer, timeline_count) || loop_insert_after_)) {
                return target
                        == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                    ? static_cast<int>((*regs)[2] & 0x3F)
                    : static_cast<int>((*regs)[3] & 0x07);
            }
            const auto& auto_lane = parameter == Parameter::OpllTlAuto
                ? layer.opll_tl_auto
                : layer.opll_fb_auto;
            if (const auto value = mgstc::engine::opllRegisterAutoValueAt(
                    auto_lane,
                    target,
                    count,
                    layer.envelope_timeline.length_counts)) {
                return *value;
            }
            return 0;
        }
        if (parameter == Parameter::Pitch) {
            return cumulativePitchAt(layer, timeline_count);
        }
        if (parameter == Parameter::RegisterWrite
            || parameter == Parameter::Timbre) {
            return 0;
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        int current = parameter == Parameter::Volume ? layer.volume : 0;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count <= static_cast<std::uint32_t>(timeline_count)) {
                current = event.value;
            }
        }
        const auto [minimum, maximum] = valueRangeFor(parameter);
        return juce::jlimit(minimum, maximum, current);
    }

    [[nodiscard]] int lastPitchCommandAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        int value = 0;
        bool found = false;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && (!isLoopStartCount(layer, timeline_count) || event.after_loop_start == loop_insert_after_)) {
                value = event.value;
                found = true;
            }
        }
        juce::ignoreUnused(found);
        return value;
    }

    [[nodiscard]] int layerAt(juce::Point<int> point) const noexcept {
        if (!graphArea().contains(point)) {
            return -1;
        }
        if (selected_layer_ >= 0 && selected_layer_ < static_cast<int>(graph_bounds_.size())
            && makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(selected_layer_)],
                timbre_.layers[static_cast<std::size_t>(selected_layer_)].source).summary.contains(point))
            return selected_layer_;
        for (std::size_t index = 0; index < lane_frames_.size(); ++index) {
            if (lane_frames_[index].contains(point)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void applyMouseCursor(juce::MouseCursor::StandardCursorType type) {
        if (last_mouse_cursor_ == type) {
            return;
        }
        last_mouse_cursor_ = type;
        setMouseCursor(juce::MouseCursor(type));
    }

    void setLabelTextIfChanged(
        juce::Label& label, const juce::String& text) {
        if (label.getText() != text) {
            label.setText(text, juce::dontSendNotification);
        }
    }

    void setEditorTextIfChanged(
        juce::TextEditor& editor, const juce::String& text) {
        if (editor.getText() != text) {
            editor.setText(text, false);
        }
    }

    void inspectFromMouse(juce::Point<int> point) {
        if (drawing_) {
            return;
        }
        const int layer = layerAt(point);
        hover_edit_parameter_.reset();
        if (layer < 0) {
            hover_preview_value_.reset();
            applyMouseCursor(juce::MouseCursor::CrosshairCursor);
            return;
        }
        if (layer != selected_layer_) {
            hover_preview_value_.reset();
            applyMouseCursor(juce::MouseCursor::PointingHandCursor);
            return;
        }
        applyMouseCursor(juce::MouseCursor::CrosshairCursor);
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto& selected =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const bool in_graph = graph.contains(point);
        const int previous_count = selected_count_;
        bool parameter_changed = false;
        std::optional<int> mapped_value;
        auto count_bounds = timelinePlotBounds(graph);
        if (in_graph) {
            const auto slots = makeEditLaneSlots(graph, selected.source);
            if (slots.summary.contains(point)) {
                hover_preview_value_.reset();
                selected_count_ = countAtX(count_bounds, point.x);
                return;
            }
            const struct LaneHit {
                Parameter parameter;
                juce::Rectangle<int> slot;
                bool drawable;
            } hits[] = {
                {Parameter::Volume, slots.volume, true},
                {Parameter::Pitch, slots.pitch, pitchLaneExpanded()},
                {Parameter::Timbre, slots.timbre, false},
                {Parameter::RegisterWrite, slots.register_write, false},
                {Parameter::OpllTlAuto, slots.tl_auto, true},
                {Parameter::OpllFbAuto, slots.fb_auto, true},
            };
            for (const auto& hit : hits) {
                if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                    continue;
                }
                hover_edit_parameter_ = hit.parameter;
                if (selectedParameter() != hit.parameter) {
                    parameter_.setSelectedId(
                        static_cast<int>(hit.parameter),
                        juce::dontSendNotification);
                    syncRegisterAutoEditors();
                    parameter_changed = true;
                }
                if (hit.drawable) {
                    if (hit.parameter == Parameter::Pitch) {
                        const auto range = pitchDisplayRange(selected);
                        mapped_value = pitchValueFromY(
                            graphStrip(hit.slot),
                            point.y,
                            range.second);
                    } else if (hit.parameter == Parameter::Volume) {
                        mapped_value = volumeValueFromY(
                            selected.source,
                            graphStrip(hit.slot),
                            point.y);
                    } else {
                        mapped_value = valueFromY(
                            graphStrip(hit.slot), point.y, hit.parameter);
                    }
                }
                break;
            }
            selected_count_ = countAtX(count_bounds, point.x);
        }
        const int shown = mapped_value.value_or(
            storedValueAt(selected, selectedParameter(), selected_count_));
        const bool count_changed = selected_count_ != previous_count;
        hover_preview_value_ = mapped_value;
        setLabelTextIfChanged(
            position_, "ct " + juce::String(selected_count_));
        if (selectedParameter() == Parameter::Timbre && !mapped_value) {
            setEditorTextIfChanged(
                value_, timbreInspectLabel(selected, selected_count_));
        } else if (
            selectedParameter() == Parameter::RegisterWrite
            && !mapped_value) {
            setEditorTextIfChanged(
                value_, registerInspectLabel(selected, selected_count_));
        } else {
            setEditorTextIfChanged(value_, juce::String(shown));
        }
        if (selectedParameter() == Parameter::RegisterWrite) {
            const auto tip = registerInspectTooltip(selected, selected_count_);
            value_.setTooltip(tip);
        } else if (selectedParameter() == Parameter::Pitch) {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の累積オフセット（未確定）。"
                          "「設定」は1個の\\命令を追加します")
                    : juce::String::fromUTF8(
                          "元音域からの累積オフセット。"
                          "「設定」は入力値を1個の\\として追加。"
                          "Shift＋ドラッグでも同一カウントへ追加"));
        } else if (isRegisterAutoParameter(selectedParameter())) {
            const bool available = mgstc::engine::opllRegisterAutoAvailableAt(
                selected,
                static_cast<std::uint32_t>(juce::jmax(0, selected_count_)),
                !isLoopStartCount(selected, selected_count_) || loop_insert_after_);
            value_.setTooltip(
                available
                    ? juce::String::fromUTF8(
                          "TL/FB変調。展開時はアクティブなオリジナル音色の"
                          "レジスタ2/3をベースにフィールドだけ書き換えます")
                    : juce::String::fromUTF8(
                          "ROM音色区間ではTL/FB変調は使えません（yを出力しません）"));
            if (!available && (count_changed || parameter_changed)) {
                reportStatus(juce::String::fromUTF8(
                    "ROM音色区間 — TL/FB変調は無効"));
            }
        } else {
            value_.setTooltip(
                mapped_value
                    ? juce::String::fromUTF8(
                          "ホバー位置の値（未確定）。クリックまたは「設定」で反映")
                    : juce::String::fromUTF8("選択パラメーターの値"));
        }
        if (count_changed || parameter_changed) {
            refreshCommandStack(selected);
            if (parameter_changed) {
                syncInspector();
            }
        }
    }

    void editPoint(
        int timeline_count,
        int parameter_value,
        bool commit,
        std::optional<bool> automatic_override = std::nullopt,
        std::optional<bool> precise_override = std::nullopt) {
        cancelStepDrag();
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        if (selectedParameter() == Parameter::Pitch) {
            addPitchCommand(timeline_count, parameter_value, commit);
            return;
        }
        if (selectedParameter() == Parameter::Timbre) {
            return;
        }
        if (selectedParameter() == Parameter::RegisterWrite) {
            return;
        }
        const auto [minimum, maximum] = valueRange();
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        parameter_value = juce::jlimit(minimum, maximum, parameter_value);
        auto& events = selectedEvents(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto kind = selectedEventKind();
        bool automatic = false;
        bool precise = false;
        std::int32_t ramp_start_metadata = 0;
        for (const auto& event : events) {
            if (event.kind == kind
                && event.count
                    == static_cast<std::uint32_t>(timeline_count)) {
                automatic = event.automatic;
                precise = event.precise;
                ramp_start_metadata = event.secondary;
                break;
            }
        }
        if (automatic_override.has_value()
            && kind == mgstc::engine::EnvelopeEventKind::Volume) {
            automatic = *automatic_override;
        }
        if (precise_override.has_value()
            && kind == mgstc::engine::EnvelopeEventKind::Volume) {
            precise = *precise_override;
        }
        if (!automatic) {
            precise = false;
            ramp_start_metadata = 0;
        }
        std::erase_if(events, [kind, timeline_count](const auto& event) {
            return event.kind == kind
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        events.push_back({
            .kind = kind,
            .value = parameter_value,
            .secondary = ramp_start_metadata,
            .count = static_cast<std::uint32_t>(timeline_count),
            .automatic = automatic,
            .precise = precise,
        });
        std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            return left.count < right.count;
        });
        notifyLayerEdit(timeline_count, parameter_value, commit);
    }

    void syncPointEditors() {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        position_.setText(
            "ct " + juce::String(selected_count_),
            juce::dontSendNotification);
        if (selectedParameter() == Parameter::Pitch) {
            value_.setText(
                juce::String(lastPitchCommandAt(layer, selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "次に追加する\\のオペランド（-127～127）。"
                "グラフのYは累積オフセットです"));
        } else if (selectedParameter() == Parameter::Timbre) {
            value_.setText(
                timbreInspectLabel(layer, selected_count_),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "このカウントの@切替先。ライブラリから選びます"));
        } else if (selectedParameter() == Parameter::RegisterWrite) {
            value_.setText(
                registerInspectLabel(layer, selected_count_),
                false);
            value_.setTooltip(juce::String::fromUTF8(
                "Y-Register y。OPLLはオリジナル音色全フィールド（regs 0～7）を"
                "ウィンドウで編集し、変更レジスタだけ y を出します。"
                "同ステップの y 群はまとめて編集。ROM区間は不可"));
        } else {
            value_.setText(
                juce::String(
                    storedValueAt(layer, selectedParameter(), selected_count_)),
                false);
            value_.setTooltip(juce::String::fromUTF8("選択パラメーターの値"));
        }
        // Drawing path restores the popup after edit; do not clear mid-drag.
        if (!drawing_) {
            hover_preview_value_.reset();
        }
    }

    void setTimelineMarker(int timeline_count, bool commit) {
        cancelStepDrag();
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& timeline = selectedTimeline(
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        const auto count = static_cast<std::uint32_t>(
            juce::jlimit(0, maximumCount(), timeline_count));
        switch (selectedEditMode()) {
        case EditMode::Draw:
            return;
        case EditMode::End: {
            timeline.length_counts = juce::jmax<std::uint32_t>(1, count);
            if (timeline.loop_start_count
                && *timeline.loop_start_count > timeline.length_counts) {
                timeline.loop_start_count = timeline.length_counts;
            }
            if (timeline.loop_end_count
                && *timeline.loop_end_count > timeline.length_counts) {
                timeline.loop_end_count = timeline.length_counts;
            }
            break;
        }
        case EditMode::LoopStart:
            if (!mgstc::engine::setCompositeLoopStart(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)], count)) return;
            break;
        case EditMode::LoopEnd:
            if (!mgstc::engine::setCompositeLoopEnd(
                    timbre_.layers[static_cast<std::size_t>(selected_layer_)],
                    juce::jmin(count, timeline.length_counts))) return;
            break;
        }
        refreshCountCeilingCache();
        maybeExpandScrollExtent(static_cast<int>(count));
        syncInspector();
        updateScrollRanges();
        repaint();
        if (edit_callback_) {
            publishEdit(commit, commit);
        }
    }

    [[nodiscard]] int timelineChromeHeight() const noexcept {
        using namespace UiLayout;
        // Add/help/tempo row only; envelope edit tools live in the edit-tier dock.
        return textButtonH + sm;
    }

    [[nodiscard]] juce::Rectangle<int> editHeaderArea() const {
        auto area = workspaceArea();
        if (!focus_.getToggleState()) area.removeFromTop(overviewHeight() + UiLayout::fieldH + UiLayout::sm);
        area.removeFromRight(kScrollBarSize);
        return area.withHeight(UiLayout::fieldH
            + (allocation_error_label_.isVisible() ? UiLayout::fieldH + UiLayout::sm : 0));
    }

    [[nodiscard]] juce::Rectangle<int> editHeaderCaptionBounds() const {
        auto area = editHeaderArea().withHeight(UiLayout::fieldH);
        const auto caption = selected_layer_ >= 0 && selected_layer_ < static_cast<int>(timbre_.layers.size())
            ? juce::String::fromUTF8(timbre_.layers[static_cast<std::size_t>(selected_layer_)].name.c_str())
                + juce::String::fromUTF8(" / エンベロープ") : juce::String::fromUTF8("エンベロープ");
        return area.withWidth(juce::GlyphArrangement::getStringWidthInt(UiFonts::body(true), caption) + UiLayout::xs * 2);
    }

    [[nodiscard]] juce::Rectangle<int> graphArea() const {
        auto area = workspaceArea();
        area.setTop(editHeaderArea().getBottom() + UiLayout::sm);
        area.removeFromRight(kScrollBarSize);
        area.removeFromBottom(kScrollBarSize + UiLayout::sm
            + (mml_toggle_.getToggleState() ? UiLayout::envelopePreviewH + UiLayout::sm : 0));
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            area.setHeight(juce::jmin(area.getHeight(),
                laneHeightForIndex(selected_layer_)));
        }
        return area;
    }

    [[nodiscard]] juce::Range<double> visibleCountRange() const {
        return horizontal_scroll_.getCurrentRange();
    }

    [[nodiscard]] int countAtX(
        juce::Rectangle<int> bounds, int x) const {
        int first = 0, last = maximumCount();
        while (first < last) {
            const int middle = first + (last - first + 1) / 2;
            if (xForCount(bounds, middle) <= x) first = middle;
            else last = middle - 1;
        }
        return first;
    }

    [[nodiscard]] int xForCount(
        juce::Rectangle<int> bounds, int count) const {
        const auto visible = visibleCountRange();
        return bounds.getX() + juce::roundToInt(
            (static_cast<double>(count) - visible.getStart())
            * bounds.getWidth() / juce::jmax(1.0, visible.getLength()));
    }

    void updateScrollRanges() {
        const auto graph = graphArea();
        const double total = juce::jmax(
            1.0, static_cast<double>(totalLaneStackHeight()));
        const double view = juce::jmax(
            1.0, static_cast<double>(graph.getHeight()));
        const double visible = juce::jmin(view, total);
        const double max_start = juce::jmax(0.0, total - visible);
        const double start = juce::jlimit(
            0.0, max_start, vertical_scroll_.getCurrentRangeStart());
        vertical_scroll_.setRangeLimits(
            0.0, total, juce::dontSendNotification);
        vertical_scroll_.setCurrentRange(
            start, visible, juce::dontSendNotification);
        scroll_extent_counts_ = juce::jlimit(
            1, maximumCount(), scroll_extent_counts_);
        horizontal_scroll_.setRangeLimits(
            0.0,
            static_cast<double>(scroll_extent_counts_) + 1.0,
            juce::dontSendNotification);
        applyCountColumnWidth();
    }

    [[nodiscard]] int timelineGraphWidth() const {
        return juce::jmax(
            1,
            graphArea().getWidth()
                - UiLayout::panelGap
                - UiLayout::compositeOverviewLabelW);
    }

    [[nodiscard]] double countColumnWidthPx() const {
        return static_cast<double>(timelineGraphWidth())
            / juce::jmax(1.0, visibleCountRange().getLength());
    }

    void applyCountColumnWidth() {
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double current = horizontal_scroll_.getCurrentRangeSize();
        const double visible = juce::jlimit(min_visible, max_visible, current);
        horizontal_scroll_.setCurrentRange(
            horizontal_scroll_.getCurrentRangeStart(),
            visible,
            juce::dontSendNotification);
    }

    void zoomCountColumns(int anchor_x, double length_factor) {
        const auto graph = graphArea();
        auto bounds = graph;
        bounds.reduce(UiLayout::sm, 0);
        bounds.removeFromLeft(UiLayout::compositeOverviewLabelW);
        const int count = countAtX(bounds, anchor_x);
        const double min_visible = juce::jmax(
            8.0,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMaxW));
        const double max_visible = juce::jmax(
            min_visible,
            static_cast<double>(timelineGraphWidth())
                / static_cast<double>(UiLayout::countColumnMinW));
        const double visible = juce::jlimit(
            min_visible,
            max_visible,
            visibleCountRange().getLength() * length_factor);
        const double start = juce::jmax(
            0.0,
            static_cast<double>(count)
                - visible * juce::jlimit(
                    0.0, 1.0,
                    static_cast<double>(anchor_x - bounds.getX())
                        / juce::jmax(1.0, static_cast<double>(bounds.getWidth()))));
        maybeExpandScrollExtent(
            static_cast<int>(std::ceil(start + visible)));
        horizontal_scroll_.setCurrentRange(
            start, visible, juce::dontSendNotification);
        repaint();
    }

    void drawCountGrid(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        const double column_w = countColumnWidthPx();
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x = xForCount(bounds, timeline_count);
            const bool major = timeline_count % 10 == 0;
            graphics.setColour(
                major
                    ? juce::Colour(0xFF52616E)
                    : juce::Colour(0xFF35404A).withAlpha(
                        column_w >= static_cast<double>(UiLayout::countColumnMinW)
                            ? 0.95F : 0.45F));
            graphics.drawVerticalLine(
                x, static_cast<float>(bounds.getY()),
                static_cast<float>(bounds.getBottom()));
        }
    }

    void scrollBarMoved(juce::ScrollBar* scroll_bar, double) override {
        if (scroll_bar == &horizontal_scroll_) {
            maybeExpandScrollExtent(static_cast<int>(std::ceil(
                horizontal_scroll_.getCurrentRange().getEnd())));
        }
        lane_layout_dirty_ = true;
        ensureLaneLayout();
        repaint();
    }

    void drawTimelineMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour,
        juce::Rectangle<int> volume_lane = {}) const {
        const auto& timeline = selectedTimeline(timbre_.layers[layer_index]);
        const auto draw_marker = [&](std::uint32_t count,
                                     juce::Colour marker_colour,
                                     const juce::String& label) {
            const auto visible = visibleCountRange();
            if (count < visible.getStart() || count > visible.getEnd()) {
                return;
            }
            const bool loop_marker = label == "[ LOOP" || label == "LOOP ]";
            const int x = loop_marker
                ? loopStartX(bounds, static_cast<int>(count))
                : xForCount(bounds, static_cast<int>(count));
            graphics.setColour(marker_colour);
            if (loop_marker && !volume_lane.isEmpty()) {
                const int volume_x = xForCount(bounds, static_cast<int>(count));
                graphics.drawVerticalLine(x, static_cast<float>(bounds.getY()),
                    static_cast<float>(volume_lane.getY()));
                graphics.drawVerticalLine(volume_x, static_cast<float>(volume_lane.getY()),
                    static_cast<float>(volume_lane.getBottom()));
                graphics.drawVerticalLine(x, static_cast<float>(volume_lane.getBottom()),
                    static_cast<float>(bounds.getBottom()));
                graphics.drawHorizontalLine(volume_lane.getY(), static_cast<float>(volume_x), static_cast<float>(x));
                graphics.drawHorizontalLine(volume_lane.getBottom() - 1, static_cast<float>(volume_x), static_cast<float>(x));
            } else {
                graphics.drawVerticalLine(x, static_cast<float>(bounds.getY()),
                    static_cast<float>(bounds.getBottom()));
            }
            graphics.setFont(UiFonts::dense(true));
            const int line_height = juce::jmax(16, juce::roundToInt(graphics.getCurrentFont().getHeight()));
            const int label_y = bounds.getY() + (label == "END" && timeline.loop_end_count == count ? line_height : 0);
            graphics.drawText(
                label, x + 2, label_y, juce::GlyphArrangement::getStringWidthInt(graphics.getCurrentFont(), label) + UiLayout::xs,
                line_height,
                juce::Justification::centredLeft, false);
        };
        draw_marker(timeline.length_counts,
                    juce::Colour(0xFFFFD166), "END");
        if (timeline.loop_start_count) {
            draw_marker(*timeline.loop_start_count,
                        colour.brighter(0.35F), "[ LOOP");
        }
        if (timeline.loop_end_count) {
            draw_marker(*timeline.loop_end_count,
                        colour.brighter(0.35F), "LOOP ]");
        }
    }

    void drawOutputCutoff(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index) const {
        if (layer_index >= output_cutoff_counts_.size()
            || !output_cutoff_counts_[layer_index]) {
            return;
        }
        const auto count = *output_cutoff_counts_[layer_index];
        const auto visible = visibleCountRange();
        if (count < visible.getStart() || count > visible.getEnd()) {
            return;
        }
        const int x = xForCount(bounds, static_cast<int>(count));
        graphics.setColour(juce::Colours::red);
        graphics.drawVerticalLine(
            x, static_cast<float>(bounds.getY()),
            static_cast<float>(bounds.getBottom()));
        graphics.setFont(UiFonts::dense(true));
        const auto label = juce::String::fromUTF8("@e出力上限");
        const int label_width = juce::GlyphArrangement::getStringWidthInt(
            graphics.getCurrentFont(), label)
            + UiLayout::xs * 2;
        const int label_x = juce::jlimit(
            bounds.getX(), juce::jmax(bounds.getX(), bounds.getRight() - label_width),
            x + UiLayout::xs);
        graphics.drawText(
            label, label_x, bounds.getY(), label_width,
            static_cast<int>(std::ceil(graphics.getCurrentFont().getHeight()))
                + UiLayout::xs,
            juce::Justification::centredLeft, false);
    }

    void drawAutomation(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour,
        std::optional<Parameter> forced_parameter = std::nullopt) const {
        const auto parameter = forced_parameter.value_or(selectedParameter());
        const auto& layer = timbre_.layers[layer_index];
        if (parameter == Parameter::Pitch) {
            drawPitchLane(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::Timbre) {
            drawTimbreMarkers(graphics, bounds, layer, colour);
            return;
        }
        if (parameter == Parameter::RegisterWrite) {
            return;
        }
        if (parameter == Parameter::Volume) {
            drawVolumeBars(graphics, bounds, layer_index, colour);
            return;
        }
        const auto& events = eventsFor(layer, parameter);
        const auto kind = eventKindFor(parameter);
        const auto [minimum, maximum] = valueRangeFor(parameter);
        std::vector<std::pair<int, int>> points;
        for (const auto& event : events) {
            if (event.kind == kind) {
                points.emplace_back(
                    static_cast<int>(event.count),
                    juce::jlimit(minimum, maximum, event.value));
            }
        }
        if (points.empty()) {
            points.emplace_back(0, 0);
        }
        std::stable_sort(points.begin(), points.end());
        drawSteppedValuePath(graphics, bounds, points, minimum, maximum, colour);
    }

    void drawSteppedValuePath(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const std::vector<std::pair<int, int>>& points,
        int minimum,
        int maximum,
        juce::Colour colour) const {
        if (points.empty() || bounds.isEmpty()) {
            return;
        }
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                static_cast<float>(minimum),
                static_cast<float>(maximum),
                static_cast<float>(bounds.getBottom()),
                static_cast<float>(bounds.getY()));
        };
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().first));
        float y = map_y(points.front().second);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].first));
            const float next_y = map_y(points[index].second);
            // Hold horizontally until the next step, then jump vertically.
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(2.0F));
        for (const auto& [timeline_count, parameter_value] : points) {
            const float px = static_cast<float>(
                xForCount(bounds, timeline_count));
            const float py = map_y(parameter_value);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
    }

    void drawVolumeBars(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t layer_index,
        juce::Colour colour,
        float fill_opacity = 0.55F) const {
        const auto& layer = timbre_.layers[layer_index];
        const bool show_effective = hasModulation(layer, mgstc::engine::CompositeModulationTarget::Volume)
            || mgstc::engine::compositeLoopEffectiveWaveformFirstPassOnly(layer,
                mgstc::engine::CompositeModulationTarget::Volume, timbreLibrary());
        if (show_effective) {
            fill_opacity *= 0.45F;
        }
        struct Point {
            int count{};
            int volume{};
            bool automatic{};
            int secondary{};
        };
        std::vector<Point> points;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
                points.push_back({
                    static_cast<int>(event.count),
                    juce::jlimit(0, 15, event.value),
                    event.automatic,
                    event.secondary});
            }
        }
        std::stable_sort(
            points.begin(),
            points.end(),
            [](const Point& left, const Point& right) {
                return left.count < right.count;
            });
        if (points.empty() || points.front().count > 0) {
            points.insert(
                points.begin(),
                {
                    0,
                    juce::jlimit(0, 15, static_cast<int>(layer.volume)),
                    false});
        }
        const int end_count = static_cast<int>(
            layer.envelope_timeline.length_counts);
        const auto& sampled = cachedVolumeSamples(layer_index, end_count);
        const auto draw_output_span = [&](int start, int stop) {
            const int last = juce::jmin(stop, static_cast<int>(layer.envelope_timeline.loop_end_count.value_or(
                static_cast<std::uint32_t>(end_count))));
            for (int count = start; count < last; ++count) {
                const int x0 = xForCount(bounds, count);
                const int x1 = xForCount(bounds, count + 1);
                const auto actual_top = chipTransferYFromValue(
                    layer.source,
                    bounds,
                    sampled[static_cast<std::size_t>(count)],
                    layer.volume);
                graphics.setColour(colour.withAlpha(fill_opacity * (0.32F / 0.55F)));
                graphics.fillRect(
                    x0,
                    juce::roundToInt(actual_top),
                    juce::jmax(1, x1 - x0),
                    juce::jmax(
                        1,
                        bounds.getBottom() - juce::roundToInt(actual_top)));
            }
        };
        for (std::size_t index = 0; index < points.size(); ++index) {
            const int start = points[index].count;
            const int stop = index + 1 < points.size()
                ? points[index + 1].count
                : juce::jmax(start + 1, end_count);
            const int x0 = xForCount(bounds, start);
            const int x1 = xForCount(bounds, juce::jmax(start + 1, stop));
            const int width = juce::jmax(2, x1 - x0);
            const bool ramp_to_next = index + 1 < points.size()
                && points[index + 1].automatic
                && points[index + 1].count > start;
            if (ramp_to_next) {
                // Imported ramps may start after a hold; retain that author
                // boundary while drawing a straight outline to the target.
                const int ramp_start = points[index + 1].secondary > 0
                    ? juce::jlimit(start, stop, points[index + 1].secondary - 1)
                    : start;
                const float ramp_x = static_cast<float>(xForCount(bounds, ramp_start));
                const float y0 = volumeYFromValue(
                    layer.source, bounds, points[index].volume);
                const float y1 = volumeYFromValue(
                    layer.source, bounds, points[index + 1].volume);
                juce::Path bar;
                bar.startNewSubPath(
                    static_cast<float>(x0),
                    static_cast<float>(bounds.getBottom()));
                bar.lineTo(static_cast<float>(x0), y0);
                bar.lineTo(ramp_x, y0);
                bar.lineTo(static_cast<float>(x1), y1);
                bar.lineTo(
                    static_cast<float>(x1),
                    static_cast<float>(bounds.getBottom()));
                bar.closeSubPath();
                graphics.setColour(colour.withAlpha(fill_opacity));
                graphics.fillPath(bar);
                graphics.setColour(juce::Colour(kUiHoverAccent));
                if (ramp_start > start) {
                    graphics.drawLine(
                        static_cast<float>(x0), y0, ramp_x, y0, 2.0F);
                }
                graphics.drawLine(
                    ramp_x, y0, static_cast<float>(x1), y1, 2.0F);
            } else {
                const float top = volumeYFromValue(
                    layer.source, bounds, points[index].volume);
                graphics.setColour(colour.withAlpha(fill_opacity));
                graphics.fillRect(
                    x0,
                    juce::roundToInt(top),
                    width,
                    juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
                graphics.setColour(colour);
                graphics.drawRect(
                    x0,
                    juce::roundToInt(top),
                    width,
                    juce::jmax(1, bounds.getBottom() - juce::roundToInt(top)));
            }
            draw_output_span(start, juce::jmax(start + 1, stop));
        }
        if (show_effective) {
            const auto visible = visibleCountRange();
            const int first = juce::jmax(0, static_cast<int>(
                std::floor(visible.getStart())));
            const int last = juce::jmin(end_count - 1, static_cast<int>(
                std::ceil(visible.getEnd())));
            juce::Path base_path, effective_path;
            const auto expanded = mgstc::engine::expandCompositeLayerModulations(layer, timbreLibrary(), false);
            const auto effective_samples = mgstc::engine::sampleCompositeVolumeLane(expanded, end_count);
            for (int count = first; count <= last; ++count) {
                const int base = sampled[static_cast<std::size_t>(count)];
                const int effective = effective_samples[static_cast<std::size_t>(count)];
                const float x0 = static_cast<float>(xForCount(bounds, count));
                const float x1 = static_cast<float>(xForCount(bounds, count + 1));
                const float base_y = volumeYFromValue(layer.source, bounds, base);
                const float effective_y = volumeYFromValue(
                    layer.source, bounds, effective);
                base_path.startNewSubPath(x0, base_y);
                base_path.lineTo(x1, base_y);
                effective_path.startNewSubPath(x0, effective_y);
                effective_path.lineTo(x1, effective_y);
            }
            graphics.setColour(colour.withAlpha(0.45F));
            graphics.strokePath(base_path, juce::PathStrokeType(1.0F));
            graphics.setColour(colour.brighter(0.2F));
            strokeEffectiveWaveform(graphics, effective_path, bounds, layer,
                mgstc::engine::CompositeModulationTarget::Volume, 2.0F);
        }
    }

    void drawScope(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        std::size_t channel,
        juce::Colour colour) const {
        const float centre_y = static_cast<float>(bounds.getCentreY());
        graphics.setColour(juce::Colour(0xFF52616E).withAlpha(0.65F));
        graphics.drawHorizontalLine(
            bounds.getCentreY(),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        if (scope_size_ < 2 || bounds.getWidth() < 2) {
            return;
        }
        constexpr double sample_rate = 48000.0;
        const double frequency = 440.0 * std::pow(
            2.0,
            (static_cast<double>(audition_note_) - 69.0) / 12.0);
        const int sample_count = juce::jlimit(
            2,
            scope_size_,
            juce::roundToInt(2.0 * sample_rate / frequency));
        const int oldest =
            (scope_write_position_ + kScopeHistorySize - scope_size_)
            % kScopeHistorySize;
        const int logical_start = scope_size_ - sample_count;
        const std::size_t scope_channel = channel >= timbre_.layers.size()
            ? 3
            : static_cast<std::size_t>(timbre_.layers[channel].source);
        const auto sample_at = [&](int logical_index) {
            return scope_history_[scope_channel][static_cast<std::size_t>(
                (oldest + logical_start + logical_index)
                % kScopeHistorySize)];
        };
        float peak = 0.0001F;
        for (int index = 0; index < sample_count; ++index) {
            peak = juce::jmax(peak, std::abs(sample_at(index)));
        }
        const float scale =
            static_cast<float>(bounds.getHeight()) * 0.43F / peak;
        juce::Path path;
        for (int x = 0; x < bounds.getWidth(); ++x) {
            const int sample_index = juce::jlimit(
                0,
                sample_count - 1,
                juce::roundToInt(
                    static_cast<double>(x)
                    * static_cast<double>(sample_count - 1)
                    / static_cast<double>(bounds.getWidth() - 1)));
            const float point_x =
                static_cast<float>(bounds.getX() + x);
            const float point_y = centre_y - sample_at(sample_index) * scale;
            if (x == 0) {
                path.startNewSubPath(point_x, point_y);
            } else {
                path.lineTo(point_x, point_y);
            }
        }
        graphics.setColour(colour);
        graphics.strokePath(path, juce::PathStrokeType(1.4F));
    }

    struct EditLaneSlots {
        juce::Rectangle<int> summary;
        juce::Rectangle<int> volume;
        juce::Rectangle<int> pitch;
        juce::Rectangle<int> timbre;
        juce::Rectangle<int> register_write;
        juce::Rectangle<int> tl_auto;
        juce::Rectangle<int> fb_auto;
        juce::Rectangle<int> preview;
    };

    [[nodiscard]] bool showsRegisterLane(
        mgstc::engine::TimbreSource source) const noexcept {
        return source != mgstc::engine::TimbreSource::Scc;
    }

    [[nodiscard]] bool showsRegisterAutoLanes(
        mgstc::engine::TimbreSource source) const noexcept {
        return source == mgstc::engine::TimbreSource::Opll;
    }

    [[nodiscard]] EditLaneSlots makeEditLaneSlots(
        juce::Rectangle<int> content,
        mgstc::engine::TimbreSource source) const {
        EditLaneSlots slots;
        slots.preview = {}; // MGSC preview is pinned below the scrolling graph.
        slots.summary = content.removeFromTop(commandSummaryHeight());
        slots.summary.setY(graphArea().getY() + UiLayout::editLaneFramePadV + UiLayout::editLaneInnerPadV);
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            slots.volume = content.removeFromTop(UiLayout::editVolumeLaneH);
            return slots;
        }
        const bool has_register = showsRegisterLane(source);
        const int marker_h = UiLayout::editMarkerLaneH;
        if (has_register) {
            slots.timbre = content.removeFromTop(marker_h);
            slots.register_write = content.removeFromTop(marker_h);
        } else {
            slots.timbre = content.removeFromTop(marker_h);
            slots.register_write = {};
        }
        if (showsRegisterAutoLanes(source)) {
            slots.tl_auto = content.removeFromTop(UiLayout::registerAutoLaneH);
            slots.fb_auto = content.removeFromTop(UiLayout::registerAutoLaneH);
        }
        // Shared height (UiLayout::editVolumeLaneH). Quiet chip steps that
        // would be sub-pixel are rounded so each 0–15 unit still has ≥1px.
        const int volume_h = juce::jmin(
            content.getHeight(),
            juce::jmax(UiLayout::editMarkerLaneH, UiLayout::editVolumeLaneH));
        slots.volume = content.removeFromTop(volume_h);
        slots.pitch = pitchLaneExpanded()
            ? content : content.removeFromTop(UiLayout::fieldH);
        return slots;
    }

    [[nodiscard]] juce::Rectangle<int> timelinePlotBounds(
        juce::Rectangle<int> lane_content) const {
        if (lane_content.isEmpty()) {
            return {};
        }
        lane_content.removeFromLeft(UiLayout::compositeOverviewLabelW);
        return lane_content;
    }

    [[nodiscard]] juce::Rectangle<int> graphStrip(juce::Rectangle<int> slot) const {
        if (slot.isEmpty()) {
            return {};
        }
        slot.removeFromLeft(UiLayout::compositeOverviewLabelW);
        return slot;
    }

    bool isLoopStartCount(const mgstc::engine::CompositeLayer& layer, int count) const {
        return layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == static_cast<std::uint32_t>(count);
    }

    void setLoopInsertZone(bool after) {
        loop_insert_after_ = after;
        syncPointEditors();
        syncRegisterAutoEditors();
        syncInspector();
        repaint();
    }

    void selectLoopZoneAt(juce::Rectangle<int> plot, juce::Point<int> point) {
        if (selected_layer_ < 0) return;
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        loop_insert_after_ = isLoopStartCount(layer, selected_count_)
            && point.x >= loopStartX(plot, selected_count_);
    }

    struct LaneCommand {
        Parameter parameter{};
        std::size_t index{};
        std::uint32_t count{};
        bool after{};
        juce::String text;
        int source{}; // 0=ordinary event, 1=modulation profile, 2=auto profile
        std::uint32_t order{};
    };

    std::vector<LaneCommand> laneCommands(const mgstc::engine::CompositeLayer& layer,
        std::optional<Parameter> parameter = std::nullopt) const {
        std::vector<LaneCommand> commands;
        // Match the serializer's order within each bracket zone.
        for (std::size_t index = 0; index < layer.timbre_automation.size(); ++index) {
            const auto& e = layer.timbre_automation[index];
            if (e.kind == mgstc::engine::EnvelopeEventKind::Timbre)
                commands.push_back({Parameter::Timbre, index, e.count, e.after_loop_start, timbreCommandLabel(e)});
            else if (e.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite)
                commands.push_back({Parameter::RegisterWrite, index, e.count, e.after_loop_start,
                    formatRegisterGroupLabel(layer, e.count, {&e})});
        }
        for (std::size_t index = 0; index < layer.pitch_envelope.events.size(); ++index) {
            const auto& e = layer.pitch_envelope.events[index];
            if (e.kind == mgstc::engine::EnvelopeEventKind::Pitch)
                commands.push_back({Parameter::Pitch, index, e.count, e.after_loop_start, pitchCommandLabel(e.value)});
        }
        for (std::size_t index = 0; index < layer.volume_envelope.events.size(); ++index) {
            const auto& e = layer.volume_envelope.events[index];
            if (e.kind == mgstc::engine::EnvelopeEventKind::Volume)
                commands.push_back({Parameter::Volume, index, e.count,
                    isLoopStartCount(layer, static_cast<int>(e.count)) || e.after_loop_start,
                    juce::String(e.value) + (e.automatic ? " =" : "")});
        }
        for (std::size_t index = 0; index < layer.modulation_profiles.size(); ++index) {
            const auto& profile = layer.modulation_profiles[index];
            const auto& value = profile.parameters;
            const auto parameter_kind = profile.target == mgstc::engine::CompositeModulationTarget::Pitch ? Parameter::Pitch
                : profile.target == mgstc::engine::CompositeModulationTarget::Volume ? Parameter::Volume
                : profile.target == mgstc::engine::CompositeModulationTarget::TotalLevel ? Parameter::OpllTlAuto : Parameter::OpllFbAuto;
            const auto name = profile.target == mgstc::engine::CompositeModulationTarget::Pitch ? "Pitch"
                : profile.target == mgstc::engine::CompositeModulationTarget::Volume ? "Tremolo"
                : profile.target == mgstc::engine::CompositeModulationTarget::TotalLevel ? "TL" : "FB";
            commands.push_back({parameter_kind, index, profile.count,
                parameter_kind == Parameter::Volume && isLoopStartCount(layer, static_cast<int>(profile.count))
                    ? true : profile.after_loop_start,
                juce::String(name) + (value.enabled ? " " : " OFF ")
                    + juce::String(static_cast<int>(value.mode)) + " / " + juce::String(static_cast<int>(value.delay))
                    + " / " + juce::String(static_cast<int>(value.depth)) + " / " + juce::String(static_cast<int>(value.speed))
                    + " / " + juce::String(static_cast<int>(value.roughness)), 1});
        }
        for (std::size_t index = 0; index < layer.register_auto_profiles.size(); ++index) {
            const auto& profile = layer.register_auto_profiles[index];
            const auto& value = profile.lane;
            commands.push_back({profile.target == mgstc::engine::OpllRegisterAutoTarget::TotalLevel ? Parameter::OpllTlAuto : Parameter::OpllFbAuto,
                index, profile.count, profile.after_loop_start,
                juce::String(profile.target == mgstc::engine::OpllRegisterAutoTarget::TotalLevel ? "TL " : "FB ")
                    + juce::String(static_cast<int>(value.mode)) + " / " + juce::String(static_cast<int>(value.depth))
                    + " / " + juce::String(static_cast<int>(value.change_speed)) + " / " + juce::String(static_cast<int>(value.coarseness))
                    + " / " + juce::String(static_cast<int>(value.stop_position)), 2});
        }
        std::erase_if(commands, [parameter](const auto& command) {
            return parameter && command.parameter != *parameter;
        });
        for (auto& command : commands) {
            if (command.source == 1) {
                command.order = layer.modulation_profiles[command.index].command_order;
                continue;
            }
            if (command.source == 2) {
                command.order = layer.register_auto_profiles[command.index].command_order;
                continue;
            }
            const auto& events = command.parameter == Parameter::Pitch ? layer.pitch_envelope.events
                : command.parameter == Parameter::Volume ? layer.volume_envelope.events : layer.timbre_automation;
            command.order = events[command.index].command_order;
        }
        std::stable_sort(commands.begin(), commands.end(), [&layer](const auto& a, const auto& b) {
            if (a.count != b.count) return a.count < b.count;
            if (layer.envelope_timeline.loop_start_count && a.count == *layer.envelope_timeline.loop_start_count)
                if (a.after != b.after) return a.after < b.after;
            if (a.order || b.order) return (a.order ? a.order : ~std::uint32_t{})
                < (b.order ? b.order : ~std::uint32_t{});
            return false;
        });
        return commands;
    }

    juce::Rectangle<int> commandLabelBounds(juce::Rectangle<int> plot,
        const mgstc::engine::CompositeLayer& layer, int count, bool after, bool volume = false) const {
        const int left = xForCount(plot, count);
        const int right = xForCount(plot, count + 1);
        auto area = juce::Rectangle<int>(left, plot.getY(), juce::jmax(1, right - left), plot.getHeight());
        if (!volume && isLoopStartCount(layer, count)) {
            const int half = area.getWidth() / 2;
            const int row = juce::jmax(1, area.getHeight() / 2);
            if (after) area = {left + half, plot.getY() + row, juce::jmax(1, right - left - half), plot.getHeight() - row};
            else area = {left, plot.getY(), juce::jmax(1, half), row};
        }
        return area.getIntersection(plot);
    }

    juce::Rectangle<int> expandedCommandLabelBounds(juce::Rectangle<int> plot,
        const mgstc::engine::CompositeLayer& layer, int count, bool after, bool volume,
        const juce::String& text, const juce::Font& font, bool adjacent_parameter) const {
        auto area = commandLabelBounds(plot, layer, count, after, volume);
        if (!adjacent_parameter && !area.isEmpty()) {
            const int full_width = juce::GlyphArrangement::getStringWidthInt(font, text) + UiLayout::xs * 2;
            area = area.withWidth(juce::jmax(area.getWidth(), full_width)).getIntersection(plot);
        }
        return area;
    }

    void drawLaneCommandLabels(juce::Graphics& graphics, juce::Rectangle<int> plot,
        const mgstc::engine::CompositeLayer& layer, Parameter parameter, juce::Colour colour) const {
        std::map<std::pair<std::uint32_t, bool>, juce::StringArray> labels;
        if (parameter == Parameter::RegisterWrite) {
            std::map<std::pair<std::uint32_t, bool>, std::vector<const mgstc::engine::EnvelopeEvent*>> groups;
            for (const auto& event : layer.timbre_automation) {
                if (event.kind != mgstc::engine::EnvelopeEventKind::RegisterWrite) continue;
                const bool split = isLoopStartCount(layer, static_cast<int>(event.count));
                groups[{event.count, split && event.after_loop_start}].push_back(&event);
            }
            for (const auto& [key, events] : groups)
                labels[key].add(formatRegisterGroupLabel(layer, key.first, events));
        } else {
            for (const auto& command : laneCommands(layer, parameter)) {
                const bool split = isLoopStartCount(layer, static_cast<int>(command.count));
                labels[{command.count, split && command.after}].add(command.text);
            }
        }
        graphics.setFont(UiFonts::dense(true));
        graphics.setColour(colour);
        for (const auto& [key, text] : labels) {
            const auto adjacent = labels.lower_bound({key.first + 1, false});
            const bool adjacent_parameter = adjacent != labels.end() && adjacent->first.first == key.first + 1;
            const auto display_text = text.joinIntoString(" ");
            const auto area = expandedCommandLabelBounds(plot, layer, static_cast<int>(key.first), key.second,
                parameter == Parameter::Volume, display_text, graphics.getCurrentFont(), adjacent_parameter);
            if (area.isEmpty()) continue;
            graphics.drawText(display_text, area.reduced(UiLayout::xs, 0),
                parameter == Parameter::Volume ? juce::Justification::topLeft
                    : adjacent_parameter ? juce::Justification::centred : juce::Justification::centredLeft,
                adjacent_parameter);
        }
    }

    std::optional<Parameter> commandParameterAt(const EditLaneSlots& slots, juce::Point<int> point) const {
        if (slots.summary.contains(point)) return std::nullopt;
        if (slots.volume.contains(point)) return Parameter::Volume;
        if (slots.pitch.contains(point)) return Parameter::Pitch;
        if (slots.timbre.contains(point)) return Parameter::Timbre;
        if (slots.register_write.contains(point)) return Parameter::RegisterWrite;
        if (slots.tl_auto.contains(point)) return Parameter::OpllTlAuto;
        if (slots.fb_auto.contains(point)) return Parameter::OpllFbAuto;
        return std::nullopt;
    }

    juce::String loopCommandTooltip(juce::Point<int> point) const {
        const int index = layerAt(point);
        if (index < 0 || index != selected_layer_ || index >= static_cast<int>(graph_bounds_.size())) return {};
        const auto& layer = timbre_.layers[static_cast<std::size_t>(index)];
        const auto slots = makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(index)], layer.source);
        const int count = countAtX(timelinePlotBounds(graph_bounds_[static_cast<std::size_t>(index)]), point.x);
        const auto parameter = commandParameterAt(slots, point);
        if (!parameter && !slots.summary.contains(point)) return {};
        const bool start = isLoopStartCount(layer, count);
        const bool after = parameter == Parameter::Volume
            || point.x >= loopStartX(timelinePlotBounds(graph_bounds_[static_cast<std::size_t>(index)]), count);
        juce::StringArray lines;
        for (const auto& command : laneCommands(layer, parameter)) {
            if (command.count != static_cast<std::uint32_t>(count) || (start && command.after != after)) continue;
            lines.add(command.parameter == Parameter::RegisterWrite && command.source == 0
                ? formatRegisterWriteLabel(layer.timbre_automation[command.index]) : command.text);
        }
        if (lines.isEmpty() && !start) return {};
        lines.insert(0, "ct " + juce::String(count) + (start ? juce::String::fromUTF8(after
            ? "：毎周実行（ [ より後）" : "：初回だけ（ [ より前）") : juce::String{}));
        return lines.joinIntoString("\n");
    }

    [[nodiscard]] juce::Rectangle<int> loopMarkerBounds(juce::Rectangle<int> summary,
        std::uint32_t count, const char* name) const {
        const int ruler_h = juce::jmax(12, juce::roundToInt(UiFonts::dense().getHeight()));
        const int width = juce::GlyphArrangement::getStringWidthInt(UiFonts::dense(true), name) + UiLayout::xs * 2;
        return {loopStartX(summary, static_cast<int>(count)) - ruler_h / 2,
            summary.getBottom() - ruler_h, width, ruler_h};
    }

    bool deleteLoopMarkerAt(juce::Point<int> point) {
        const int index = layerAt(point);
        if (index < 0 || index != selected_layer_) return false;
        const auto& layer = timbre_.layers[static_cast<std::size_t>(index)];
        const auto& timeline = layer.envelope_timeline;
        if (!timeline.loop_start_count || !timeline.loop_end_count) return false;
        const auto summary = graphStrip(makeEditLaneSlots(graph_bounds_[static_cast<std::size_t>(index)], layer.source).summary);
        if (!summary.contains(point)) return false;
        if (!loopMarkerBounds(summary, *timeline.loop_start_count, "[ LOOP").contains(point)
            && !loopMarkerBounds(summary, *timeline.loop_end_count, "LOOP ]").contains(point)) return false;
        clearLoop(static_cast<std::size_t>(index));
        return true;
    }

    bool showLoopCommandMenu(juce::Point<int> point) {
        const int index = layerAt(point);
        if (index < 0 || index != selected_layer_ || index >= static_cast<int>(graph_bounds_.size())) return false;
        const auto layer_index = static_cast<std::size_t>(index);
        const auto& layer = timbre_.layers[layer_index];
        const auto plot = timelinePlotBounds(graph_bounds_[layer_index]);
        const int count = countAtX(plot, point.x);
        if (!isLoopStartCount(layer, count)) return false;
        const auto slots = makeEditLaneSlots(graph_bounds_[layer_index], layer.source);
        const auto parameter = commandParameterAt(slots, point);
        if (!parameter && !slots.summary.contains(point)) return false;
        selected_count_ = count;
        selectLoopZoneAt(plot, point);
        if (parameter == Parameter::Volume) loop_insert_after_ = true;
        if (parameter) parameter_.setSelectedId(static_cast<int>(*parameter), juce::dontSendNotification);
        auto commands = laneCommands(layer, parameter);
        std::erase_if(commands, [count](const auto& command) { return command.count != static_cast<std::uint32_t>(count); });
        juce::PopupMenu menu;
        if (parameter != Parameter::Volume) menu.addItem(1, juce::String::fromUTF8("初回側へ追加"), true, !loop_insert_after_);
        menu.addItem(2, juce::String::fromUTF8("毎周側へ追加"), true, loop_insert_after_);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            juce::PopupMenu sub;
            if (commands[i].parameter != Parameter::Volume)
                sub.addItem(static_cast<int>(100 + i * 2), juce::String::fromUTF8(commands[i].after ? "初回側へ移動" : "毎周側へ移動"));
            sub.addItem(static_cast<int>(101 + i * 2), juce::String::fromUTF8("削除"));
            menu.addSubMenu(commands[i].text, sub);
        }
        const auto original = timbre_;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
            [safe = juce::Component::SafePointer<Impl>(this), original, commands, layer_index](int result) {
                if (safe == nullptr || result == 0 || safe->timbre_ != original) return;
                if (result == 1 || result == 2) {
                    safe->setLoopInsertZone(result == 2);
                    return;
                }
                const auto ordinal = static_cast<std::size_t>((result - 100) / 2);
                if (result < 100 || ordinal >= commands.size()) return;
                auto& layer = safe->timbre_.layers[layer_index];
                const auto command = commands[ordinal];
                if (command.source == 1) {
                    if ((result - 100) % 2 == 0) layer.modulation_profiles[command.index].after_loop_start = !command.after;
                    else layer.modulation_profiles.erase(layer.modulation_profiles.begin() + static_cast<std::ptrdiff_t>(command.index));
                    safe->finishLoopEdit(); return;
                }
                if (command.source == 2) {
                    if ((result - 100) % 2 == 0) layer.register_auto_profiles[command.index].after_loop_start = !command.after;
                    else layer.register_auto_profiles.erase(layer.register_auto_profiles.begin() + static_cast<std::ptrdiff_t>(command.index));
                    safe->finishLoopEdit(); return;
                }
                auto& events = command.parameter == Parameter::Pitch ? layer.pitch_envelope.events
                    : command.parameter == Parameter::Volume ? layer.volume_envelope.events : layer.timbre_automation;
                if ((result - 100) % 2 == 0) events[command.index].after_loop_start = !command.after;
                else events.erase(events.begin() + static_cast<std::ptrdiff_t>(command.index));
                safe->finishLoopEdit();
            });
        syncPointEditors(); syncRegisterAutoEditors(); syncInspector(); repaint();
        return true;
    }

    bool hitEditLane(
        const mgstc::engine::CompositeLayer& layer,
        juce::Rectangle<int> content,
        juce::Point<int> point) {
        edit_draw_bounds_.reset();
        const auto slots = makeEditLaneSlots(content, layer.source);
        if (slots.summary.contains(point) || slots.preview.contains(point)) {
            return true;
        }
        const struct LaneHit {
            Parameter parameter;
            juce::Rectangle<int> slot;
            bool drawable;
        } hits[] = {
            {Parameter::Volume, slots.volume, true},
            {Parameter::Pitch, slots.pitch, pitchLaneExpanded()},
            {Parameter::Timbre, slots.timbre, false},
            {Parameter::RegisterWrite, slots.register_write, false},
            {Parameter::OpllTlAuto, slots.tl_auto, true},
            {Parameter::OpllFbAuto, slots.fb_auto, true},
        };
        for (const auto& hit : hits) {
            if (hit.slot.isEmpty() || !hit.slot.contains(point)) {
                continue;
            }
            parameter_.setSelectedId(
                static_cast<int>(hit.parameter),
                juce::dontSendNotification);
            syncRegisterAutoEditors();
            if (hit.drawable) {
                const bool free_curve = isRegisterAutoParameter(hit.parameter)
                    && selectedRegisterAutoLane() != nullptr
                    && selectedRegisterAutoLane()->mode
                        == mgstc::engine::OpllRegisterAutoMode::FreeCurve;
                const bool rise_fall_params =
                    isRegisterAutoParameter(hit.parameter);
                // Free-curve is mouse-traced; rise/fall/LFO are param-driven
                // lines (still selectable). Only free-curve starts a draw.
                if (!isRegisterAutoParameter(hit.parameter) || free_curve) {
                    edit_draw_bounds_ = graphStrip(hit.slot);
                } else {
                    edit_draw_bounds_.reset();
                    juce::ignoreUnused(rise_fall_params);
                }
            }
            return true;
        }
        return true;
    }

    void ensureSelectedLaneVisible() {
        if (selected_layer_ < 0) {
            return;
        }
        const int y0 = lanePixelOffset(selected_layer_);
        const int y1 = y0 + laneHeightForIndex(selected_layer_);
        const int view0 = juce::roundToInt(
            vertical_scroll_.getCurrentRangeStart());
        const int view_h = juce::jmax(
            1, juce::roundToInt(vertical_scroll_.getCurrentRangeSize()));
        const int view1 = view0 + view_h;
        if (y1 > view0 && y0 < view1) {
            return;
        }
        vertical_scroll_.setCurrentRangeStart(
            static_cast<double>(y0), juce::dontSendNotification);
    }

    void refreshCommandStack(const mgstc::engine::CompositeLayer& layer) {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));
        std::vector<juce::String> before;
        std::vector<juce::String> after;
        std::vector<juce::String> after_end;
        std::vector<int> fixed_zone;
        const bool loop_start = layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
        const bool loop_end = layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count == count;
        if (!loop_start) {
            loop_insert_after_ = false;
        }
        const auto push_label =
            [&](const juce::String& label, bool after_loop) {
                if (loop_end) {
                    if (after_loop) {
                        after.push_back(label);
                    } else {
                        after_end.push_back(label);
                    }
                } else if (loop_start && after_loop) {
                    after.push_back(label);
                } else {
                    before.push_back(label);
                }
            };
        auto commands = laneCommands(layer);
        std::erase_if(commands, [count](const auto& command) { return command.count != count; });
        if (loop_end) std::stable_sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return a.after > b.after; });
        for (std::size_t index = 0; index < commands.size(); ++index) {
            const auto& command = commands[index];
            push_label(command.text, command.after);
            if (command.parameter == Parameter::Volume) fixed_zone.push_back(static_cast<int>(index));
        }
        command_stack_.setStack(
            selected_count_,
            loop_start,
            loop_end,
            loop_insert_after_,
            std::move(before),
            std::move(after),
            std::move(after_end), std::move(fixed_zone));
        command_stack_.setSize(juce::jmax(1, command_view_.getWidth() - kScrollBarSize),
            command_stack_.preferredHeight());
    }

    [[nodiscard]] bool newCommandAfterLoopStart(
        std::uint32_t count) const noexcept {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return false;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        return loop_insert_after_
            && layer.envelope_timeline.loop_start_count
            && *layer.envelope_timeline.loop_start_count == count;
    }

    void drawSelectedCountBand(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds) const {
        const auto visible = visibleCountRange();
        if (selected_count_ < visible.getStart()
            || selected_count_ > visible.getEnd()) {
            return;
        }
        const int x0 = xForCount(bounds, selected_count_);
        const int x1 = xForCount(bounds, selected_count_ + 1);
        const int width = juce::jmax(3, x1 - x0);
        graphics.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.12F));
        graphics.fillRect(x0, bounds.getY(), width, bounds.getHeight());
    }

    void paintEditTier(
        juce::Graphics& graphics,
        juce::Rectangle<int> content,
        std::size_t layer_index,
        juce::Colour colour) {
        const auto& layer = timbre_.layers[layer_index];
        const auto slots = makeEditLaneSlots(content, layer.source);
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            const auto rate = mgstc::engine::clampRateEnvelope(
                layer.volume_envelope.rate);
            auto summary = slots.summary;
            auto label = summary.removeFromLeft(UiLayout::compositeOverviewLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                juce::String::fromUTF8("@r"),
                label,
                juce::Justification::centredLeft,
                1);
            graphics.drawFittedText(
                juce::String::formatted(
                    "AL %d  AR %d  DR %d  SL %d  SR %d  RR %d",
                    static_cast<int>(rate.attack_level),
                    static_cast<int>(rate.attack_rate),
                    static_cast<int>(rate.decay_rate),
                    static_cast<int>(rate.sustain_level),
                    static_cast<int>(rate.sustain_rate),
                    static_cast<int>(rate.release_rate)),
                summary,
                juce::Justification::centredLeft,
                1);
            auto graph_slot = slots.volume;
            auto graph_label = graph_slot.removeFromLeft(
                UiLayout::compositeOverviewLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.drawFittedText(
                juce::String::fromUTF8("曲線"),
                graph_label,
                juce::Justification::centredLeft,
                1);
            const auto layout = rateEnvelopeHandlesForLayer(layer);
            paintRateEnvelopeGraph(
                graphics,
                graph_slot,
                cachedLayerRateTrace(layer_index),
                colour,
                layer.source,
                true,
                &layout,
                -1,
                -1,
                false,
                true);
            {
                auto preview = slots.preview;
                auto preview_label = preview.removeFromLeft(
                    UiLayout::compositeOverviewLabelW);
                graphics.setColour(juce::Colour(0xFFC9D1D9));
                graphics.setFont(UiFonts::dense(true));
                graphics.drawFittedText(
                    "MML", preview_label, juce::Justification::centredLeft, 1);
            }
            return;
        }
        drawScope(
            graphics,
            timelinePlotBounds(content),
            layer_index,
            colour.withAlpha(0.32F));
        paintEditSubLane(
            graphics, slots.volume, layer_index, colour,
            Parameter::Volume, juce::String::fromUTF8("音量"));
        paintEditSubLane(
            graphics, slots.pitch, layer_index, colour,
            Parameter::Pitch, juce::String::fromUTF8("音程"));
        paintEditSubLane(
            graphics, slots.timbre, layer_index, colour,
            Parameter::Timbre, juce::String::fromUTF8("音色 @"));
        if (!slots.register_write.isEmpty()) {
            paintRegisterLane(graphics, slots.register_write, layer, colour);
        }
        if (!slots.tl_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.tl_auto, layer, colour,
                Parameter::OpllTlAuto, juce::String::fromUTF8("TL変調"));
        }
        if (!slots.fb_auto.isEmpty()) {
            paintRegisterAutoLane(
                graphics, slots.fb_auto, layer, colour,
                Parameter::OpllFbAuto, juce::String::fromUTF8("FB変調"));
        }
        auto output_bounds = graphStrip(slots.volume).getUnion(graphStrip(slots.pitch));
        for (const auto slot : {slots.timbre, slots.register_write,
                               slots.tl_auto, slots.fb_auto}) {
            if (!slot.isEmpty()) {
                output_bounds = output_bounds.getUnion(graphStrip(slot));
            }
        }
        drawTimelineMarkers(graphics, output_bounds, layer_index, colour, graphStrip(slots.volume));
        drawOutputCutoff(graphics, output_bounds, layer_index);
        {
            auto preview = slots.preview;
            auto label = preview.removeFromLeft(UiLayout::compositeOverviewLabelW);
            graphics.setColour(juce::Colour(0xFFC9D1D9));
            graphics.setFont(UiFonts::dense(true));
            graphics.drawFittedText(
                "MML", label, juce::Justification::centredLeft, 1);
        }
    }

    void paintPinnedCommandRow(juce::Graphics& graphics, juce::Rectangle<int> content,
        const mgstc::engine::CompositeLayer& layer, juce::Colour colour) {
        if (layer.volume_envelope.kind != mgstc::engine::EnvelopeKind::Sequence) return;
        auto summary = makeEditLaneSlots(content, layer.source).summary;
        graphics.setColour(juce::Colour(0xFF20262E));
        graphics.fillRect(summary);
        auto label = summary.removeFromLeft(UiLayout::compositeOverviewLabelW);
        graphics.setColour(juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::dense(true));
        graphics.drawFittedText(juce::String::fromUTF8("命令/LOOP/END"),
            label.withHeight(juce::roundToInt(UiFonts::dense(true).getHeight())),
            juce::Justification::centredLeft, 1);
        drawCommandSummary(graphics, summary, layer, colour);
    }

    void syncMorphBank() {
        if (!morph_preview_input_ || *morph_preview_input_ != timbre_) {
            morph_bank_error_.clear();
            opll_bank_error_.clear();
            setAllocationError({});
            morph_preview_ = *mgstc::engine::compileSccMorphCached(timbre_);
            morph_preview_input_ = timbre_;
        }
        scc_timbre_usage_.resize(timbre_.layers.size());
        for (std::size_t i = 0; i < timbre_.layers.size(); ++i)
            scc_timbre_usage_[i] = timbre_.layers[i].source == mgstc::engine::TimbreSource::Scc
                ? sccTimbreUsage(i) : juce::String{};
        morph_bank_.setSelectedId(allocation_starts_.scc + 1, juce::dontSendNotification);
        opll_bank_.setSelectedId(allocation_starts_.opll + 1, juce::dontSendNotification);
        morph_bank_.setVisible(true);
        const auto numbers = mgstc::engine::resolveTimbreNumbers(morph_preview_.timbre);
        for (const auto& capacity : numbers.automatic_allocations) {
            if (capacity.required <= capacity.available) continue;
            const auto error = juce::String(capacity.source == mgstc::engine::TimbreSource::Scc ? "SCC @" : "OPLL @")
                + juce::String(static_cast<int>(capacity.start)) + juce::String::fromUTF8("：番号不足 必要")
                + juce::String(static_cast<int>(capacity.required)) + juce::String::fromUTF8("個／割当可能")
                + juce::String(static_cast<int>(capacity.available)) + juce::String::fromUTF8("個");
            setAllocationError(error);
            break;
        }
        if (!morph_preview_.valid && morph_preview_.required > morph_preview_.capacity) {
            setAllocationError("SCC @" + juce::String(static_cast<int>(allocation_starts_.scc))
                + juce::String::fromUTF8("：番号不足 必要") + juce::String(static_cast<int>(morph_preview_.required))
                + juce::String::fromUTF8("個／割当可能") + juce::String(static_cast<int>(morph_preview_.capacity))
                + juce::String::fromUTF8("個"));
        }
        const auto usage_for = [&](mgstc::engine::TimbreSource source, std::uint8_t start) {
            std::array<bool, 32> occupied{};
            for (const auto& assignment : numbers.assignments)
                if (assignment.layer_index < morph_preview_.timbre.layers.size()
                    && morph_preview_.timbre.layers[assignment.layer_index].source == source
                    && assignment.number >= start && assignment.number < 32)
                    occupied[assignment.number] = true;
            const int used = static_cast<int>(std::count(occupied.begin(), occupied.end(), true));
            return juce::String::fromUTF8("使用") + juce::String(used) + "/" + juce::String(32 - start)
                + juce::String::fromUTF8(" 空き") + juce::String(32 - start - used);
        };
        const auto usage = usage_for(mgstc::engine::TimbreSource::Scc, allocation_starts_.scc);
        const auto text = morph_bank_error_.isNotEmpty() ? morph_bank_error_
            : morph_preview_.valid ? usage : usage + "  "
            + juce::String::fromUTF8(morph_preview_.error.c_str())
            + juce::String::fromUTF8(" 必要: ") + juce::String(static_cast<int>(morph_preview_.required));
        morph_bank_usage_.setText(text, juce::dontSendNotification);
        morph_bank_usage_.setTooltip(text);
        const auto opll_usage = opll_bank_error_.isNotEmpty() ? opll_bank_error_
            : usage_for(mgstc::engine::TimbreSource::Opll, allocation_starts_.opll);
        opll_bank_usage_.setText(opll_usage, juce::dontSendNotification);
        opll_bank_usage_.setTooltip(opll_usage);
    }

    void updateEnvelopeMmlPreview() {
        pending_plan_invalidation_ = mgstc::app::invalidateEditedSccMorphPlans(last_published_timbre_, timbre_)
            || pending_plan_invalidation_;
        envelope_number_active_.resize(timbre_.layers.size(), false);
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            const auto index = static_cast<std::size_t>(selected_layer_);
            auto& edited = timbre_.layers[index];
            const bool active = edited.volume_envelope.kind
                    == mgstc::engine::EnvelopeKind::Rate
                || mgstc::engine::layerUsesSequenceEnvelope(edited);
            if (active && !envelope_number_active_[index]) {
                const bool collision = std::any_of(
                    timbre_.layers.begin(), timbre_.layers.end(),
                    [this, index, &edited](const auto& other) {
                        if (&other == &timbre_.layers[index]) return false;
                        return other.envelope_number == edited.envelope_number
                            && (other.volume_envelope.kind
                                    == mgstc::engine::EnvelopeKind::Rate
                                || mgstc::engine::layerUsesSequenceEnvelope(other));
                    });
                if (collision) {
                    edited.envelope_number =
                        mgstc::engine::nextFreeEnvelopeNumber(timbre_);
                    if (selected_layer_ < layer_setups_.size())
                        layer_setups_[selected_layer_]->syncFromLayer(edited);
                    morph_preview_input_.reset();
                }
            }
            envelope_number_active_[index] = active;
        }
        syncMorphBank();
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            if (last_envelope_mml_preview_.isNotEmpty()) {
                last_envelope_mml_preview_.clear();
                envelope_mml_preview_.clear();
            }
            envelope_mml_preview_.setVisible(false);
            return;
        }
        const auto& preview = morph_preview_.valid ? morph_preview_.timbre : timbre_;
        const auto& layer = preview.layers[static_cast<std::size_t>(selected_layer_)];
        const auto numbers = mgstc::engine::resolveTimbreNumbers(preview);
        juce::String definition;
        juce::String validation_error;
        juce::String output_notice;
        output_cutoff_counts_.resize(timbre_.layers.size());
        output_cutoff_counts_[static_cast<std::size_t>(selected_layer_)].reset();
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            definition = juce::String::fromUTF8(
                mgstc::engine::formatMgsRateDefinition(
                    layer, layer.envelope_number)
                    .c_str())
                .trimEnd();
        } else if (mgstc::engine::layerUsesSequenceEnvelope(layer)) {
            const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
                layer,
                layer.envelope_number,
                mgstc::engine::kMgscEnvelopeCompiledByteLimit,
                &numbers,
                timbreLibrary(),
                timbre_.name);
            definition = juce::String::fromUTF8(formatted.definition.c_str())
                              .trimEnd();
            output_cutoff_counts_[static_cast<std::size_t>(selected_layer_)] =
                formatted.output_cutoff_count;
            if (formatted.valid() && formatted.output_cutoff_count) {
                output_notice = juce::String::fromUTF8("ct ")
                    + juce::String(static_cast<int>(*formatted.output_cutoff_count))
                    + juce::String::fromUTF8(
                        " 以降は@e出力上限のため出力不可。試聴・鍵盤再生は生成した@eに従います。");
            }
            if (formatted.hasIssue(
                    mgstc::engine::MgsEnvelopeIssue::DefinitionLengthExceeded)) {
                validation_error = juce::String::fromUTF8(
                    "エンベロープ設定がMGSCの上限を超えています (")
                    + juce::String(static_cast<int>(formatted.compiled_bytes))
                    + "/"
                    + juce::String(static_cast<int>(
                        mgstc::engine::kMgscEnvelopeCompiledByteLimit))
                    + juce::String::fromUTF8("バイト)。MML出力と試聴を停止します。");
            } else if (!formatted.valid()) {
                validation_error = juce::String::fromUTF8(
                    "エンベロープ設定を@eに変換できません。MML出力と試聴を停止します。");
            }
            if (definition.isEmpty()) {
                definition = validation_error.isNotEmpty()
                    ? validation_error
                    : "@e" + juce::String(static_cast<int>(layer.envelope_number))
                        + juce::String::fromUTF8(" = { }");
            }
            if (formatted.valid() && std::any_of(
                    std::begin(kEffectiveWaveformTargets), std::end(kEffectiveWaveformTargets),
                    [&](const auto target) { return mgstc::engine::compositeLoopEffectiveWaveformFirstPassOnly(layer, target, timbreLibrary()); })) {
                if (output_notice.isNotEmpty()) output_notice += " ";
                output_notice += juce::String::fromUTF8("破線は1ループ目の実効波形です。繰り返しでは最終ステップの状態を引き継ぎます。");
            }
        }
        if (!morph_preview_.valid) validation_error = juce::String::fromUTF8(morph_preview_.error.c_str());
        const auto tones = mgstc::engine::formatMgsCompositeLayerTimbres(
            preview, static_cast<std::size_t>(selected_layer_), timbreLibrary());
        const auto tone_definitions = juce::String::fromUTF8(tones.source.c_str()).trimEnd();
        if (!tones.valid() && validation_error.isEmpty())
            validation_error = juce::String::fromUTF8(tones.issues.front().c_str());
        const auto envelope_notice = validation_error.isNotEmpty()
            ? validation_error : output_notice;
        const bool new_notice = envelope_notice.isNotEmpty()
            && envelope_notice != modulation_issue_.getText();
        modulation_issue_.setText(envelope_notice, juce::dontSendNotification);
        modulation_issue_.setTooltip(envelope_notice);
        modulation_issue_.setVisible(
            count_inspector_ && envelope_notice.isNotEmpty());
        if (new_notice) reportStatus(envelope_notice);
        const auto track = juce::String::fromUTF8(
            mgstc::engine::formatMgsCompositeTrackPreview(layer, &numbers)
                .c_str());
        const auto preview_text = validation_error.isNotEmpty()
            ? validation_error
            : (tone_definitions.isNotEmpty() ? tone_definitions + "\n\n" : juce::String{})
                + (definition.isNotEmpty() ? definition + "\n" : juce::String{})
                + track
                + (output_notice.isNotEmpty() ? "\n; " + output_notice : juce::String{});
        if (preview_text == last_envelope_mml_preview_) {
            return;
        }
        last_envelope_mml_preview_ = preview_text;
        UiFonts::setMgscPreviewText(
            envelope_mml_preview_, preview_text);
    }

    [[nodiscard]] bool isEditSubLaneHovered(Parameter parameter) const noexcept {
        return hover_edit_parameter_.has_value()
            && *hover_edit_parameter_ == parameter;
    }

    void paintEditSubLaneChrome(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        Parameter parameter,
        const juce::String& title) const {
        const bool selected = parameter == selectedParameter();
        const bool hovered = isEditSubLaneHovered(parameter);
        if (hovered) {
            graphics.setColour(uiHoverAccentStrong().withAlpha(0.28F));
            graphics.fillRect(slot);
        } else if (selected) {
            graphics.setColour(uiHoverAccent().withAlpha(0.12F));
            graphics.fillRect(slot);
        }
        auto label = slot.removeFromLeft(UiLayout::compositeOverviewLabelW);
        graphics.setColour(
            hovered
                ? uiHoverAccentStrong()
                : selected ? uiHoverAccent() : juce::Colour(0xFFC9D1D9));
        graphics.setFont(UiFonts::body());
        graphics.drawFittedText(
            title, label, juce::Justification::centredLeft, 1);
        graphics.setColour(juce::Colour(0xFF40505E));
        graphics.drawHorizontalLine(
            slot.getBottom() - 1,
            static_cast<float>(slot.getX()),
            static_cast<float>(slot.getRight()));
    }

    void paintUnavailableOpllSteps(juce::Graphics& graphics, juce::Rectangle<int> plot,
        const mgstc::engine::CompositeLayer& layer) const {
        if (layer.source != mgstc::engine::TimbreSource::Opll) return;
        const auto visible = visibleCountRange();
        const int first = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int last = juce::jmin(maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        for (int count = first; count <= last; ++count) {
            const int left = xForCount(plot, count), right = xForCount(plot, count + 1);
            const int centre = loopStartX(plot, count);
            const bool split = isLoopStartCount(layer, count);
            for (bool after : {false, true}) {
                if (!split && !after) continue;
                if (mgstc::engine::opllRegisterAutoAvailableAt(layer, static_cast<std::uint32_t>(count), after)) continue;
                const auto area = juce::Rectangle<int>(split && after ? centre : left, plot.getY(),
                    juce::jmax(1, (split && !after ? centre : right) - (split && after ? centre : left)),
                    plot.getHeight()).getIntersection(plot);
                graphics.setColour(juce::Colour(0xFF52616E).withAlpha(0.55F));
                graphics.fillRect(area);
                graphics.setColour(juce::Colour(0xFF8896A3).withAlpha(0.4F));
                graphics.drawRect(area);
            }
        }
    }

    void paintRegisterAutoLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        paintEditSubLaneChrome(graphics, slot, parameter, title);
        slot.removeFromLeft(UiLayout::compositeOverviewLabelW);
        const auto& auto_lane = parameter == Parameter::OpllTlAuto
            ? layer.opll_tl_auto
            : layer.opll_fb_auto;
        const auto target = parameter == Parameter::OpllTlAuto
            ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
            : mgstc::engine::OpllRegisterAutoTarget::Feedback;
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(
            0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        paintUnavailableOpllSteps(graphics, slot, layer);
        const bool common_active = hasModulation(layer, parameter == Parameter::OpllTlAuto
            ? mgstc::engine::CompositeModulationTarget::TotalLevel : mgstc::engine::CompositeModulationTarget::Feedback);
        const bool profiles_active = std::any_of(layer.register_auto_profiles.begin(), layer.register_auto_profiles.end(),
            [target](const auto& profile) { return profile.target == target && profile.lane.active(); });
        if (auto_lane.mode == mgstc::engine::OpllRegisterAutoMode::Off
            && !common_active && !profiles_active) {
            graphics.setColour(juce::Colour(0xFF6B7785));
            graphics.setFont(UiFonts::dense());
            graphics.drawText(
                juce::String::fromUTF8("OFF"),
                slot.reduced(UiLayout::xs, 0),
                juce::Justification::centredLeft,
                false);
            drawLaneCommandLabels(graphics, slot, layer, parameter, colour);
            return;
        }
        const int maximum = static_cast<int>(
            mgstc::engine::opllRegisterAutoValueMax(target));
        const auto* library = timbreLibrary();
        const auto packed_events =
            mgstc::engine::expandOpllLayerRegisterAutos(layer, library);
        const auto map_y = [&](int value) {
            return juce::jmap(
                static_cast<float>(value),
                0.0F,
                static_cast<float>(maximum),
                static_cast<float>(slot.getBottom() - 1),
                static_cast<float>(slot.getY() + 1));
        };
        // Polyline = effective TL/FB at each count (base/@/manual y + auto).
        juce::Path path;
        bool started = false;
        std::optional<float> last_x;
        std::optional<float> last_y;
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const auto count = static_cast<std::uint32_t>(timeline_count);
            if (!mgstc::engine::opllRegisterAutoAvailableAt(layer, count)) {
                started = false;
                last_x.reset();
                last_y.reset();
                continue;
            }
            const auto regs = mgstc::engine::opllOriginalRegisterImageAt(
                layer, count, library, true, true, true, &packed_events);
            if (!regs) {
                started = false;
                last_x.reset();
                last_y.reset();
                continue;
            }
            const int field = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? static_cast<int>((*regs)[2] & 0x3F)
                : static_cast<int>((*regs)[3] & 0x07);
            const float x = static_cast<float>(xForCount(slot, timeline_count));
            const float y = map_y(field);
            if (!started) {
                path.startNewSubPath(x, y);
                started = true;
            } else if (last_x && last_y) {
                path.lineTo(x, *last_y);
                path.lineTo(x, y);
            }
            last_x = x;
            last_y = y;
        }
        graphics.setColour(colour);
        strokeEffectiveWaveform(graphics, path, slot, layer,
            parameter == Parameter::OpllTlAuto ? mgstc::engine::CompositeModulationTarget::TotalLevel
                : mgstc::engine::CompositeModulationTarget::Feedback, 1.6F);
        graphics.setFont(UiFonts::dense());
        const auto reg_number =
            mgstc::engine::opllRegisterAutoRegisterNumber(target);
        const auto visible_counts = visibleCountRange();
        for (const auto& event : packed_events) {
            if (event.value != reg_number
                || event.count < visible_counts.getStart()
                || event.count > visible_counts.getEnd()) {
                continue;
            }
            const int value = target
                    == mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                ? (event.secondary & 0x3F)
                : (event.secondary & 0x07);
            const int x = xForCount(slot, static_cast<int>(event.count));
            graphics.fillEllipse(
                static_cast<float>(x - 2),
                map_y(value) - 2.0F,
                4.0F,
                4.0F);
        }
        const auto labels = slot.withHeight(juce::jmin(slot.getHeight(),
            juce::roundToInt(UiFonts::dense(true).getHeight()) * 2 + UiLayout::xs));
        drawLaneCommandLabels(graphics, labels, layer, parameter, colour);
    }

    [[nodiscard]] juce::String sccTimbreUsage(std::size_t layer_index) const {
        if (layer_index >= morph_preview_.timbre.layers.size()) return "0 (0)";
        std::array<bool, 32> used{}, generated{};
        const auto& layer = morph_preview_.timbre.layers[layer_index];
        const auto numbers = mgstc::engine::resolveTimbreNumbers(morph_preview_.timbre);
        const auto mark = [&](std::uint64_t id, int fallback) {
            int number = fallback;
            for (const auto& assignment : numbers.assignments)
                if (assignment.library_id == id) { number = assignment.number; break; }
            if (number >= 0 && number < 32) used[static_cast<std::size_t>(number)] = true;
        };
        if (layer.base_timbre) mark(layer.base_timbre->library_id, -1);
        for (const auto& event : layer.timbre_automation)
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre)
                mark(event.target_library_id, event.value);
        for (const auto& diagnostic : morph_preview_.diagnostics)
            if (diagnostic.layer_index == layer_index && diagnostic.output_number < 32) {
                used[diagnostic.output_number] = true;
                if (diagnostic.generated) generated[diagnostic.output_number] = true;
            }
        return juce::String(static_cast<int>(std::count(used.begin(), used.end(), true)))
            + " (" + juce::String(static_cast<int>(std::count(generated.begin(), generated.end(), true))) + ")";
    }

    void paintEditSubLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        std::size_t layer_index,
        juce::Colour colour,
        Parameter parameter,
        const juce::String& title) {
        auto caption = parameter == Parameter::Pitch ? juce::String{} : title;
        if (parameter == Parameter::Timbre
            && timbre_.layers[layer_index].source == mgstc::engine::TimbreSource::Scc)
            caption += " / " + (layer_index < scc_timbre_usage_.size()
                ? scc_timbre_usage_[layer_index] : juce::String("0 (0)"))
                + juce::String::fromUTF8(" 種");
        paintEditSubLaneChrome(graphics, slot, parameter, caption);
        if (parameter == Parameter::Pitch && !pitchLaneExpanded())
            return;
        slot.removeFromLeft(UiLayout::compositeOverviewLabelW);
        drawAutomation(graphics, slot, layer_index, colour, parameter);
        if (parameter == Parameter::Volume && !timbre_.layers[layer_index].modulation_profiles.empty()) {
            const auto labels = slot.withHeight(juce::jmin(slot.getHeight(),
                juce::roundToInt(UiFonts::dense(true).getHeight()) * 2 + UiLayout::xs));
            drawLaneCommandLabels(graphics, labels, timbre_.layers[layer_index], Parameter::Volume, colour);
        }
    }

    [[nodiscard]] static juce::String formatRegisterWriteLabel(
        const mgstc::engine::EnvelopeEvent& event) {
        return "y" + juce::String(event.value) + ","
            + juce::String(event.secondary);
    }

    [[nodiscard]] juce::String formatRegisterGroupLabel(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count,
        const std::vector<const mgstc::engine::EnvelopeEvent*>& events) const {
        if (events.empty()) {
            return {};
        }
        if (layer.source == mgstc::engine::TimbreSource::Opll) {
            const bool after_zone = !isLoopStartCount(layer, static_cast<int>(count))
                || events.front()->after_loop_start;
            auto prefix = layer;
            mgstc::engine::composite_loop_detail::preserveBoundaryCommandOrder(prefix, count, true);
            std::erase_if(prefix.timbre_automation, [&](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite && event.count == count
                    && (!isLoopStartCount(layer, static_cast<int>(count)) || event.after_loop_start == after_zone);
            });
            auto before = mgstc::engine::opllOriginalRegisterImageAt(
                prefix, count, timbreLibrary(), true, false, after_zone);
            auto after = before;
            if (after) {
                // Decode only this authored group. Other bracket-zone writes
                // and automatic TL/FB changes must not appear in its label.
                for (const auto* event : events)
                    if (event && event->value >= 0 && event->value <= 7)
                        (*after)[static_cast<std::size_t>(event->value)] = static_cast<std::uint8_t>(event->secondary);
            }
            if (before && after) {
                const auto old_patch = mgstc::engine::decodeOpllPatch(*before);
                const auto new_patch = mgstc::engine::decodeOpllPatch(*after);
                juce::StringArray parts;
                const auto add_value = [&parts](const char* name, int value) {
                    parts.add(juce::String(name) + " " + juce::String(value));
                };
                const auto add_operator = [&parts](
                    const char* prefix,
                    const mgstc::engine::OpllOperatorParameters& old_op,
                    const mgstc::engine::OpllOperatorParameters& new_op) {
                    juce::StringArray fields;
                    const auto flag = [&fields](const char* name, bool old_value, bool new_value) {
                        if (old_value != new_value)
                            fields.add(juce::String(name) + (new_value ? "" : " OFF"));
                    };
                    const auto value = [&fields](const char* name, int old_value, int new_value) {
                        if (old_value != new_value)
                            fields.add(juce::String(name) + " " + juce::String(new_value));
                    };
                    flag("AM", old_op.amplitude_modulation, new_op.amplitude_modulation);
                    flag("PM", old_op.pitch_modulation, new_op.pitch_modulation);
                    flag("EG", old_op.sustained_tone, new_op.sustained_tone);
                    flag("KR", old_op.key_rate_scaling, new_op.key_rate_scaling);
                    flag("WS", old_op.waveform, new_op.waveform);
                    value("MULT", old_op.multiplier, new_op.multiplier);
                    value("KSL", old_op.key_scale_level, new_op.key_scale_level);
                    value("AR", old_op.attack_rate, new_op.attack_rate);
                    value("DR", old_op.decay_rate, new_op.decay_rate);
                    value("SL", old_op.sustain_level, new_op.sustain_level);
                    value("RR", old_op.release_rate, new_op.release_rate);
                    if (!fields.isEmpty())
                        parts.add(juce::String(prefix) + ": " + fields.joinIntoString(" / "));
                };
                if (old_patch.modulator.total_level != new_patch.modulator.total_level)
                    add_value("TL", new_patch.modulator.total_level);
                if (old_patch.feedback != new_patch.feedback)
                    add_value("FB", new_patch.feedback);
                add_operator("MOD", old_patch.modulator, new_patch.modulator);
                add_operator("CAR", old_patch.carrier, new_patch.carrier);
                if (!parts.isEmpty()) return parts.joinIntoString(" / ");
            }
            return before ? juce::String::fromUTF8("変更なし")
                          : juce::String::fromUTF8("ROM音色区間");
        }
        juce::StringArray parts;
        parts.ensureStorageAllocated(static_cast<int>(events.size()));
        for (const auto* event : events) {
            if (event != nullptr) {
                parts.add(formatRegisterWriteLabel(*event));
            }
        }
        // Side-by-side tokens (no y×N collapse) so each yreg,data stays readable.
        return parts.joinIntoString(" ");
    }

    [[nodiscard]] static juce::String formatRegisterGroupTooltip(
        const std::vector<const mgstc::engine::EnvelopeEvent*>& events) {
        juce::StringArray lines;
        for (const auto* event : events) {
            if (event != nullptr) {
                lines.add(formatRegisterWriteLabel(*event));
            }
        }
        return lines.joinIntoString("\n");
    }

    [[nodiscard]] static std::map<
        std::uint32_t,
        std::vector<const mgstc::engine::EnvelopeEvent*>>
    registerWritesByCount(const mgstc::engine::CompositeLayer& layer) {
        std::map<
            std::uint32_t,
            std::vector<const mgstc::engine::EnvelopeEvent*>>
            grouped;
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite) {
                grouped[event.count].push_back(&event);
            }
        }
        return grouped;
    }

    void paintRegisterLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> slot,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        paintEditSubLaneChrome(
            graphics, slot, Parameter::RegisterWrite, juce::String::fromUTF8("Y-Register y"));
        slot.removeFromLeft(UiLayout::compositeOverviewLabelW);
        paintUnavailableOpllSteps(graphics, slot, layer);
        drawLaneCommandLabels(graphics, slot, layer, Parameter::RegisterWrite, colour);
    }

    void drawCommandSummary(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        graphics.setColour(juce::Colour(0xFF1B222A));
        graphics.fillRoundedRectangle(bounds.toFloat(), 3.0F);
        const auto visible = visibleCountRange();
        const int begin = juce::jmax(0, static_cast<int>(std::floor(visible.getStart())));
        const int end = juce::jmin(
            maximumCount(), static_cast<int>(std::ceil(visible.getEnd())));
        graphics.setFont(UiFonts::dense());
        for (int timeline_count = begin; timeline_count <= end; ++timeline_count) {
            const int x0 = xForCount(bounds, timeline_count);
            const int x1 = xForCount(bounds, timeline_count + 1);
            const int width = juce::jmax(1, x1 - x0);
            graphics.setColour(
                timeline_count == selected_count_
                    ? juce::Colour(kUiHoverAccent)
                    : juce::Colour(0xFF6B7785));
            graphics.drawText(
                juce::String(timeline_count),
                x0,
                bounds.getBottom() - 13,
                width,
                12,
                juce::Justification::centred,
                false);
        }
        auto labels = bounds;
        const int ruler_h = juce::jmax(12, juce::roundToInt(UiFonts::dense().getHeight()));
        labels.removeFromBottom(ruler_h);
        std::map<std::pair<std::uint32_t, bool>, juce::StringArray> grouped;
        for (const auto& command : laneCommands(layer)) {
            if (command.parameter == Parameter::Volume) continue;
            grouped[{command.count, isLoopStartCount(layer, static_cast<int>(command.count)) && command.after}].add(command.text);
        }
        graphics.setFont(UiFonts::dense(true));
        graphics.setColour(colour);
        for (const auto& [key, text] : grouped) {
            const auto adjacent = grouped.lower_bound({key.first + 1, false});
            const bool adjacent_parameter = adjacent != grouped.end() && adjacent->first.first == key.first + 1;
            const auto display_text = text.joinIntoString(" ");
            const auto area = expandedCommandLabelBounds(labels, layer, static_cast<int>(key.first), key.second,
                false, display_text, graphics.getCurrentFont(), adjacent_parameter);
            graphics.drawText(display_text, area.reduced(UiLayout::xs, 0), juce::Justification::centredLeft, adjacent_parameter);
        }
        const auto& timeline = layer.envelope_timeline;
        if (timeline.loop_start_count && timeline.loop_end_count) {
            auto ruler = bounds.withTop(bounds.getBottom() - ruler_h);
            const int left = loopStartX(bounds, static_cast<int>(*timeline.loop_start_count));
            const int right = loopStartX(bounds, static_cast<int>(*timeline.loop_end_count));
            graphics.setColour(juce::Colour(kUiHoverAccent).withAlpha(0.14F));
            graphics.fillRect(juce::Rectangle<int>(left, ruler.getY(), juce::jmax(1, right - left), ruler_h).getIntersection(ruler));
            graphics.setColour(juce::Colour(kUiHoverAccent));
            graphics.drawHorizontalLine(ruler.getY(), static_cast<float>(juce::jmax(left, ruler.getX())),
                static_cast<float>(juce::jmin(right, ruler.getRight())));
            for (const auto& boundary : {std::pair{*timeline.loop_start_count, "[ LOOP"}, std::pair{*timeline.loop_end_count, "LOOP ]"}}) {
                auto handle = loopMarkerBounds(bounds, boundary.first, boundary.second);
                if (!handle.intersects(ruler)) continue;
                graphics.setColour(juce::Colour(0xFF1F8A5C));
                graphics.fillRoundedRectangle(handle.toFloat(), static_cast<float>(UiLayout::xs));
                graphics.setColour(juce::Colours::white);
                graphics.drawText(boundary.second, handle, juce::Justification::centred);
            }
        }
    }

    [[nodiscard]] static juce::Colour sourceColour(
        mgstc::engine::TimbreSource source) {
        switch (source) {
        case mgstc::engine::TimbreSource::Psg:
            return juce::Colour(0xFFB990FF);
        case mgstc::engine::TimbreSource::Scc:
            return juce::Colour(0xFF53E3A6);
        case mgstc::engine::TimbreSource::Opll:
            return juce::Colour(0xFFFFA75E);
        }
        return juce::Colours::white;
    }

    [[nodiscard]] static int clampPitchCommand(int value) noexcept {
        return juce::jlimit(kPitchCommandMin, kPitchCommandMax, value);
    }

    [[nodiscard]] static int cumulativePitchAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count <= static_cast<std::uint32_t>(
                       juce::jmax(0, timeline_count))) {
                acc += event.value;
            }
        }
        return acc;
    }

    [[nodiscard]] static int pitchCommandCountAt(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) noexcept {
        int count = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] static std::vector<int> splitPitchDelta(int delta) {
        std::vector<int> parts;
        while (delta > kPitchCommandMax) {
            parts.push_back(kPitchCommandMax);
            delta -= kPitchCommandMax;
        }
        while (delta < kPitchCommandMin) {
            parts.push_back(kPitchCommandMin);
            delta -= kPitchCommandMin;
        }
        if (delta != 0) {
            parts.push_back(delta);
        }
        return parts;
    }

    [[nodiscard]] std::pair<int, int> pitchDisplayRange(
        const mgstc::engine::CompositeLayer& layer) const {
        int extent = kPitchCommandMax;
        int acc = 0;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            extent = juce::jmax(extent, std::abs(acc));
        }
        if (hover_preview_value_) {
            extent = juce::jmax(extent, std::abs(*hover_preview_value_));
        }
        if (hasModulation(layer, mgstc::engine::CompositeModulationTarget::Pitch)) {
            const int end = juce::jmax(1, maximumCount());
            for (int count = 0; count <= end; ++count) {
                const int effective = mgstc::engine::modulationEffectiveValueAtCount(
                    mgstc::engine::compositeModulationAt(layer, mgstc::engine::CompositeModulationTarget::Pitch, static_cast<std::uint32_t>(count)), static_cast<std::uint32_t>(count),
                    cumulativePitchAt(layer, count), -32768, 32767);
                extent = juce::jmax(extent, std::abs(effective));
            }
        }
        const bool extra =
            layer.source != mgstc::engine::TimbreSource::Opll;
        if (layer.pitch_sweep.enabled
            && layer.source != mgstc::engine::TimbreSource::Opll) {
            const auto sweep_base = modulationBaseForLayer(layer);
            extent = juce::jmax(
                extent,
                mgstc::engine::pitchSweepDisplayExtent(
                    layer.pitch_sweep,
                    static_cast<std::uint32_t>(
                        juce::jmax(1, maximumCount())),
                    sweep_base,
                    layer.detune));
        } else {
            extent = juce::jmax(
                extent,
                mgstc::engine::softwareLfoDisplayExtent(
                    layer.software_lfo,
                    extra,
                    modulationBaseForLayer(layer),
                    layer.detune));
        }
        return {-extent, extent};
    }

    void notifyLayerEdit(int timeline_count, int shown_value, bool commit) {
        cancelMorphGeneration(true);
        maybeExpandScrollExtent(timeline_count);
        if (commit
            && selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            clampLayerToTimelineRange(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
            refreshCountCeilingCache();
            updateScrollRanges();
        }
        selected_count_ = timeline_count;
        position_.setText(
            "ct " + juce::String(timeline_count),
            juce::dontSendNotification);
        value_.setText(juce::String(shown_value), false);
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
            if (selectedParameter() == Parameter::RegisterWrite)
                syncPointEditors();
        }
        updateEnvelopeMmlPreview();
        repaint();
        if (edit_callback_) {
            publishEdit(commit, commit);
        }
    }

    void addPitchCommand(int timeline_count, int operand, bool commit) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        operand = clampPitchCommand(operand);
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .pitch_envelope.events;
        timbre_.layers[static_cast<std::size_t>(selected_layer_)]
            .pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        events.push_back({
            .kind = mgstc::engine::EnvelopeEventKind::Pitch,
            .value = operand,
            .count = static_cast<std::uint32_t>(timeline_count),
            .after_loop_start = newCommandAfterLoopStart(
                static_cast<std::uint32_t>(timeline_count)),
        });
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, operand, commit);
    }

    void editPitchCumulative(
        int timeline_count,
        int desired_cumulative,
        bool commit,
        bool append = false) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto& events = layer.pitch_envelope.events;
        layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        if (append) {
            const int previous = cumulativePitchAt(layer, timeline_count);
            const int desired_delta = desired_cumulative - previous;
            const bool after_loop = newCommandAfterLoopStart(
                static_cast<std::uint32_t>(timeline_count));
            for (const int part : splitPitchDelta(desired_delta)) {
                events.push_back({
                    .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                    .value = part,
                    .count = static_cast<std::uint32_t>(timeline_count),
                    .after_loop_start = after_loop,
                });
            }
            std::stable_sort(
                events.begin(), events.end(),
                [](const auto& left, const auto& right) {
                    return left.count < right.count;
                });
            notifyLayerEdit(timeline_count, desired_cumulative, commit);
            return;
        }
        int previous = timeline_count <= 0
            ? 0
            : cumulativePitchAt(layer, timeline_count - 1);
        // Preserve zone of erased siblings when redrawing the count.
        bool after_loop = newCommandAfterLoopStart(
            static_cast<std::uint32_t>(timeline_count));
        const bool split = isLoopStartCount(layer, timeline_count);
        if (split && after_loop) {
            for (const auto& event : events) {
                if (event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                    && event.count == static_cast<std::uint32_t>(timeline_count)
                    && !event.after_loop_start) previous += event.value;
            }
        }
        const int desired_delta = desired_cumulative - previous;
        std::erase_if(events, [timeline_count, split, after_loop](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && (!split || event.after_loop_start == after_loop);
        });
        for (const int part : splitPitchDelta(desired_delta)) {
            events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                .value = part,
                .count = static_cast<std::uint32_t>(timeline_count),
                .after_loop_start = after_loop,
            });
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, desired_cumulative, commit);
    }

    void drawPitchLane(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        const auto [minimum, maximum] = pitchDisplayRange(layer);
        juce::ignoreUnused(minimum);
        const int extent = juce::jmax(1, maximum);
        const auto map_y = [&](int value) {
            return pitchYFromValue(bounds, value, extent);
        };
        graphics.setColour(juce::Colour(0xFFE6EDF3).withAlpha(0.55F));
        graphics.drawHorizontalLine(
            juce::roundToInt(map_y(0)),
            static_cast<float>(bounds.getX()),
            static_cast<float>(bounds.getRight()));
        graphics.setFont(UiFonts::dense());
        graphics.drawText(
            "0",
            bounds.getX() + 2,
            juce::roundToInt(map_y(0)) - 12,
            24,
            12,
            juce::Justification::centredLeft,
            false);

        struct Point {
            int count{};
            int cumulative{};
            int sibling{};
        };
        std::vector<Point> points;
        points.push_back({0, 0, 0});
        int acc = 0;
        std::map<int, int> siblings;
        for (const auto& event : layer.pitch_envelope.events) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Pitch) {
                continue;
            }
            acc += event.value;
            const int count = static_cast<int>(event.count);
            points.push_back({count, acc, siblings[count]++});
        }
        juce::Path path;
        float x = static_cast<float>(xForCount(bounds, points.front().count));
        float y = map_y(points.front().cumulative);
        path.startNewSubPath(x, y);
        for (std::size_t index = 1; index < points.size(); ++index) {
            const float next_x = static_cast<float>(
                xForCount(bounds, points[index].count));
            const float next_y = map_y(points[index].cumulative);
            path.lineTo(next_x, y);
            path.lineTo(next_x, next_y);
            x = next_x;
            y = next_y;
        }
        path.lineTo(static_cast<float>(bounds.getRight()), y);
        const bool has_effective_pitch = hasModulation(layer, mgstc::engine::CompositeModulationTarget::Pitch)
            || layer.software_lfo.enabled || layer.pitch_sweep.enabled;
        graphics.setColour(has_effective_pitch
            ? colour.withAlpha(0.45F) : colour);
        if (has_effective_pitch)
            graphics.strokePath(path, juce::PathStrokeType(1.0F));
        else strokeEffectiveWaveform(graphics, path, bounds, layer,
            mgstc::engine::CompositeModulationTarget::Pitch, 2.0F);
        const auto expanded = mgstc::engine::expandCompositeLayerModulations(layer, timbreLibrary(), false);
        if (hasModulation(layer, mgstc::engine::CompositeModulationTarget::Pitch)) {
            const auto visible = visibleCountRange();
            const int first = juce::jmax(0, static_cast<int>(
                std::floor(visible.getStart())));
            const int last = juce::jmin(maximumCount(), static_cast<int>(
                std::ceil(visible.getEnd())));
            juce::Path effective_path;
            bool started = false;
            float previous_y = 0.0F;
            for (int count = first; count <= last; ++count) {
                const int value = cumulativePitchAt(expanded, count);
                const float next_x = static_cast<float>(xForCount(bounds, count));
                const float next_y = map_y(value);
                if (!started) {
                    effective_path.startNewSubPath(next_x, next_y);
                    started = true;
                } else {
                    effective_path.lineTo(next_x, previous_y);
                    effective_path.lineTo(next_x, next_y);
                }
                previous_y = next_y;
            }
            if (started) {
                effective_path.lineTo(static_cast<float>(bounds.getRight()),
                    previous_y);
                graphics.setColour(colour.brighter(0.2F));
                strokeEffectiveWaveform(graphics, effective_path, bounds, layer,
                    mgstc::engine::CompositeModulationTarget::Pitch, 2.0F);
            }
        }
        if (layer.pitch_sweep.enabled
            && layer.source != mgstc::engine::TimbreSource::Opll) {
            const auto visible = visibleCountRange();
            const int start = juce::jmax(
                0, juce::roundToInt(std::floor(visible.getStart())));
            const int end = juce::jmin(
                maximumCount(),
                juce::roundToInt(std::ceil(visible.getEnd())));
            const auto sweep_base = modulationBaseForLayer(layer);
            juce::Path sweep_path;
            bool started = false;
            float previous_y = 0.0F;
            for (int count = start; count <= end; ++count) {
                const int value = cumulativePitchAt(expanded, count)
                    + static_cast<int>(
                        mgstc::engine::pitchSweepOffsetAtTick(
                            layer.pitch_sweep,
                            static_cast<std::uint32_t>(count),
                            sweep_base,
                            layer.detune));
                const float sx = static_cast<float>(xForCount(bounds, count));
                const float sy = map_y(value);
                if (!started) {
                    sweep_path.startNewSubPath(sx, sy);
                    started = true;
                } else {
                    sweep_path.lineTo(sx, previous_y);
                    sweep_path.lineTo(sx, sy);
                }
                previous_y = sy;
            }
            if (started) {
                graphics.setColour(
                    juce::Colour(kUiHoverAccent).withAlpha(0.85F));
                strokeEffectiveWaveform(graphics, sweep_path, bounds, layer,
                    mgstc::engine::CompositeModulationTarget::Pitch, 1.0F);
            }
        } else if (layer.software_lfo.enabled) {
            const bool extra =
                layer.source != mgstc::engine::TimbreSource::Opll;
            const auto lfo_base = modulationBaseForLayer(layer);
            const auto visible = visibleCountRange();
            const int start = juce::jmax(
                0, juce::roundToInt(std::floor(visible.getStart())));
            const int end = juce::jmin(
                maximumCount(),
                juce::roundToInt(std::ceil(visible.getEnd())));
            juce::Path lfo_path;
            bool started = false;
            float previous_y = 0.0F;
            for (int count = start; count <= end; ++count) {
                const int value = cumulativePitchAt(expanded, count)
                    + static_cast<int>(
                        mgstc::engine::softwareLfoOffsetAtTick(
                            layer.software_lfo,
                            static_cast<std::uint32_t>(count),
                            extra,
                            lfo_base,
                            layer.detune));
                const float lx = static_cast<float>(xForCount(bounds, count));
                const float ly = map_y(value);
                if (!started) {
                    lfo_path.startNewSubPath(lx, ly);
                    started = true;
                } else {
                    lfo_path.lineTo(lx, previous_y);
                    lfo_path.lineTo(lx, ly);
                }
                previous_y = ly;
            }
            if (started) {
                graphics.setColour(
                    juce::Colour(kUiHoverAccent).withAlpha(0.85F));
                strokeEffectiveWaveform(graphics, lfo_path, bounds, layer,
                    mgstc::engine::CompositeModulationTarget::Pitch, 1.0F);
            }
        }
        for (const auto& point : points) {
            if (point.count == 0 && point.cumulative == 0 && point.sibling == 0
                && layer.pitch_envelope.events.empty()) {
                continue;
            }
            const float column = static_cast<float>(
                xForCount(bounds, point.count));
            const float next_column = static_cast<float>(
                xForCount(bounds, point.count + 1));
            const float span = juce::jmax(6.0F, next_column - column);
            const float px = column
                + span * (0.2F + 0.15F * static_cast<float>(point.sibling));
            const float py = map_y(point.cumulative);
            graphics.setColour(colour);
            graphics.fillEllipse(px - 3.0F, py - 3.0F, 6.0F, 6.0F);
        }
        auto labels = bounds.withHeight(juce::jmin(bounds.getHeight(),
            juce::roundToInt(UiFonts::dense(true).getHeight()) * 2 + UiLayout::xs));
        drawLaneCommandLabels(graphics, labels, layer, Parameter::Pitch, colour);
    }

    [[nodiscard]] static int timbreMarkerY(juce::Rectangle<int> bounds,
        mgstc::engine::TimbreSource source) {
        return source == mgstc::engine::TimbreSource::Scc
            ? bounds.getBottom() - UiLayout::xs - 4 : bounds.getCentreY();
    }

    void drawTimbreMarkers(
        juce::Graphics& graphics,
        juce::Rectangle<int> bounds,
        const mgstc::engine::CompositeLayer& layer,
        juce::Colour colour) const {
        const int marker_y = timbreMarkerY(bounds, layer.source);
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            for (const auto* event : orderedTimbreEvents(layer)) {
                const auto previous = morphStartEvent(layer, *event);
                if (!event->scc_morph.enabled || !previous || previous->count >= event->count) continue;
                graphics.setColour(colour.withAlpha(0.8F));
                graphics.drawLine(
                    static_cast<float>(xForCount(bounds, static_cast<int>(previous->count))),
                    static_cast<float>(marker_y),
                    static_cast<float>(xForCount(bounds, static_cast<int>(event->count))),
                    static_cast<float>(marker_y), static_cast<float>(UiLayout::xs));
            }
        }
        drawLaneCommandLabels(graphics, bounds, layer, Parameter::Timbre, colour);
    }

    [[nodiscard]] juce::String timbreCommandLabel(
        const mgstc::engine::EnvelopeEvent& event) const {
        if (event.timbre_pick == mgstc::engine::TimbrePick::OpllRom) {
            return opllRomPatchLabel(event.value);
        }
        if (event.target_library_id != 0 && timbre_name_callback_) {
            auto name = timbre_name_callback_(event.target_library_id);
            if (name.isNotEmpty()) {
                return name;
            }
        }
        return "@" + juce::String(event.value);
    }

    [[nodiscard]] juce::String timbreInspectLabel(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        if (const auto* event = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count),
                isLoopStartCount(layer, timeline_count) ? std::optional<bool>{loop_insert_after_} : std::nullopt)) {
            return timbreCommandLabel(*event);
        }
        return juce::String::fromUTF8("（なし）");
    }

    [[nodiscard]] static const mgstc::engine::EnvelopeEvent* timbreEventAt(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count, std::optional<bool> after = std::nullopt) noexcept {
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == count && (!after || event.after_loop_start == *after)) {
                return &event;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static std::vector<const mgstc::engine::EnvelopeEvent*>
    orderedTimbreEvents(const mgstc::engine::CompositeLayer& layer) {
        std::vector<const mgstc::engine::EnvelopeEvent*> events;
        for (const auto& event : layer.timbre_automation)
            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre) events.push_back(&event);
        std::stable_sort(events.begin(), events.end(), [](auto* a, auto* b) {
            if (a->count != b->count) return a->count < b->count;
            return a->after_loop_start < b->after_loop_start;
        });
        return events;
    }

    [[nodiscard]] static const mgstc::engine::EnvelopeEvent* previousTimbreEvent(
        const mgstc::engine::CompositeLayer& layer,
        const mgstc::engine::EnvelopeEvent& arrival) {
        const auto ordered = orderedTimbreEvents(layer);
        const auto found = std::find(ordered.begin(), ordered.end(), &arrival);
        return found != ordered.end() && found != ordered.begin() ? *(found - 1) : nullptr;
    }

    [[nodiscard]] static std::optional<mgstc::engine::EnvelopeEvent> morphStartEvent(
        const mgstc::engine::CompositeLayer& layer,
        const mgstc::engine::EnvelopeEvent& arrival) {
        if (const auto* previous = previousTimbreEvent(layer, arrival)) return *previous;
        if (layer.source != mgstc::engine::TimbreSource::Scc) return std::nullopt;
        mgstc::engine::EnvelopeEvent base;
        base.kind = mgstc::engine::EnvelopeEventKind::Timbre;
        base.timbre_pick = mgstc::engine::TimbrePick::Library;
        base.target_library_id = layer.base_timbre ? layer.base_timbre->library_id : 0;
        return base;
    }

    [[nodiscard]] std::optional<int> morphLineArrivalAt(
        const mgstc::engine::CompositeLayer& layer, juce::Rectangle<int> bounds,
        juce::Point<int> point) const {
        if (layer.source != mgstc::engine::TimbreSource::Scc || !bounds.contains(point)
            || std::abs(point.y - timbreMarkerY(bounds, layer.source)) > UiLayout::controlGap) return std::nullopt;
        for (const auto* event : orderedTimbreEvents(layer)) {
            const auto previous = morphStartEvent(layer, *event);
            if (event->scc_morph.enabled && previous && previous->count < event->count
                && point.x > xForCount(bounds, static_cast<int>(previous->count)) + UiLayout::xs
                && point.x < xForCount(bounds, static_cast<int>(event->count)) - UiLayout::xs)
                return static_cast<int>(event->count);
        }
        return std::nullopt;
    }

    using MorphCompletion = std::function<void(
        EnvelopeTimbrePickContent::MorphSettings,
        EnvelopeTimbrePickContent::MorphChangeResult)>;

    struct MorphWorkerRequest {
        std::uint64_t generation{};
        mgstc::engine::CompositeTimbre candidate;
        std::shared_ptr<std::stop_source> cancellation;
    };

    struct MorphPendingEdit {
        std::uint64_t generation{};
        std::size_t layer_index{};
        int count{};
        mgstc::engine::CompositeTimbre source, candidate;
        EnvelopeTimbrePickContent::MorphSettings previous, settings;
        bool reduces_invalid_transition{};
        MorphCompletion completion;
    };

    void cancelMorphGeneration(bool restore_picker = false) {
        ++morph_generation_;
        auto pending = std::move(morph_pending_edit_);
        morph_pending_edit_.reset();
        {
            std::lock_guard lock(morph_worker_mutex_);
            if (morph_active_cancellation_) morph_active_cancellation_->request_stop();
            if (morph_worker_request_) morph_worker_request_->cancellation->request_stop();
            morph_worker_request_.reset();
            morph_worker_result_.reset();
        }
        if (restore_picker && pending && pending->completion) {
            auto settings = pending->previous;
            if (pending->layer_index < timbre_.layers.size()) {
                if (const auto* event = timbreEventAt(timbre_.layers[pending->layer_index],
                    static_cast<std::uint32_t>(pending->count))) settings = event->scc_morph;
            }
            pending->completion(std::move(settings), {false, juce::String::fromUTF8("設定変更により生成を中止しました")});
        }
    }

    void ensureMorphWorker() {
        if (morph_worker_.joinable()) return;
        morph_worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::optional<MorphWorkerRequest> request;
                {
                    std::unique_lock lock(morph_worker_mutex_);
                    if (!morph_worker_wakeup_.wait(lock, stop, [this] { return morph_worker_request_.has_value(); }))
                        break;
                    request = std::move(morph_worker_request_);
                    morph_worker_request_.reset();
                    morph_active_cancellation_ = request->cancellation;
                }
                const auto token = request->cancellation->get_token();
                auto result = std::make_shared<const mgstc::engine::SccMorphCompileResult>(
                    mgstc::engine::compileSccMorph(request->candidate, token));
                {
                    std::lock_guard lock(morph_worker_mutex_);
                    if (!token.stop_requested() && !stop.stop_requested()) {
                        morph_worker_result_ = std::move(result);
                        morph_result_generation_ = request->generation;
                    }
                    if (morph_active_cancellation_ == request->cancellation)
                        morph_active_cancellation_.reset();
                }
            }
        });
    }

    void timerCallback() override {
        if (step_drag_) {
            autoScrollStepDrag();
            return;
        }
        std::shared_ptr<const mgstc::engine::SccMorphCompileResult> result;
        std::uint64_t generation{};
        {
            std::lock_guard lock(morph_worker_mutex_);
            result = std::move(morph_worker_result_);
            morph_worker_result_.reset();
            generation = morph_result_generation_;
        }
        if (!morph_pending_edit_) { stopTimer(); return; }
        if (morph_pending_edit_->source != timbre_) {
            cancelMorphGeneration(true);
            stopTimer();
            return;
        }
        if (!result || generation != morph_generation_ || generation != morph_pending_edit_->generation) return;
        auto pending = std::move(*morph_pending_edit_);
        morph_pending_edit_.reset();
        stopTimer();
        const auto error = result->valid ? juce::String{}
            : juce::String::fromUTF8(result->error.c_str())
                + juce::String::fromUTF8(" 必要: ") + juce::String(static_cast<int>(result->required))
                + juce::String::fromUTF8(" / 使用可能: ") + juce::String(static_cast<int>(result->capacity));
        const bool accepted = result->valid || pending.reduces_invalid_transition || pending.candidate == timbre_;
        if (accepted) {
            if (!mgstc::engine::seedSccMorphCache(pending.candidate, result)) {
                const auto failure = juce::String::fromUTF8("モーフィング生成結果が設定と一致しません");
                if (pending.completion) pending.completion(pending.previous, {false, failure});
                reportStatus(failure);
                return;
            }
            timbre_ = std::move(pending.candidate);
            morph_preview_ = *result;
            morph_preview_input_ = timbre_;
            notifyLayerEdit(pending.count, 0, true);
        }
        if (pending.completion) pending.completion(accepted ? pending.settings : pending.previous, {accepted, error});
        if (error.isNotEmpty()) reportStatus(error);
    }

    EnvelopeTimbrePickContent::MorphChangeResult applyIncomingMorph(
        std::size_t layer_index, int count,
        EnvelopeTimbrePickContent::MorphSettings settings, bool commit,
        MorphCompletion completion = {}) {
        cancelMorphGeneration();
        auto candidate = timbre_;
        if (layer_index >= candidate.layers.size()) return {false, juce::String::fromUTF8("チャンネルがありません")};
        auto& events = candidate.layers[layer_index].timbre_automation;
        const auto event = std::find_if(events.begin(), events.end(), [count](const auto& e) {
            return e.kind == mgstc::engine::EnvelopeEventKind::Timbre && e.count == static_cast<std::uint32_t>(count);
        });
        if (event == events.end()) return {false, juce::String::fromUTF8("音色イベントがありません")};
        const auto previous = event->scc_morph;
        if (settings.distribution_mode != previous.distribution_mode
            || settings.intermediate_count != previous.intermediate_count
            || settings.curve != previous.curve)
            settings.explicit_plan.reset();
        event->scc_morph = settings;
        static_cast<void>(mgstc::app::invalidateEditedSccMorphPlans(timbre_, candidate));
        // A slider drag only changes picker state. Its final commit schedules
        // one immutable preparation job and one existing Undo transaction.
        if (!commit) return {};
        const bool prior_valid = morph_preview_input_ && *morph_preview_input_ == timbre_ && morph_preview_.valid;
        const bool reduces_invalid_transition = !prior_valid
            && ((previous.enabled && !settings.enabled)
                || (settings.enabled == previous.enabled && settings.curve == previous.curve
                    && settings.distribution_mode == previous.distribution_mode
                    && settings.intermediate_count < previous.intermediate_count));
        morph_pending_edit_ = MorphPendingEdit{morph_generation_, layer_index, count, timbre_, candidate,
            previous, settings, reduces_invalid_transition, std::move(completion)};
        ensureMorphWorker();
        {
            std::lock_guard lock(morph_worker_mutex_);
            morph_worker_request_ = MorphWorkerRequest{morph_generation_, std::move(candidate),
                std::make_shared<std::stop_source>()};
        }
        morph_worker_wakeup_.notify_one();
        startTimerHz(30);
        return {true, juce::String::fromUTF8("モーフィングを生成中…")};
    }

    [[nodiscard]] std::optional<int> timbreMarkerCountAt(
        const mgstc::engine::CompositeLayer& layer,
        juce::Rectangle<int> bounds,
        juce::Point<int> point) const {
        if (bounds.isEmpty() || !bounds.contains(point)) {
            return std::nullopt;
        }
        std::optional<int> hit;
        int best_distance = std::numeric_limits<int>::max();
        for (const auto& event : layer.timbre_automation) {
            if (event.kind != mgstc::engine::EnvelopeEventKind::Timbre) {
                continue;
            }
            const int x = xForCount(bounds, static_cast<int>(event.count));
            const auto area = commandLabelBounds(bounds, layer,
                static_cast<int>(event.count), event.after_loop_start);
            if (!area.contains(point)) continue;
            const int distance = point.x >= x ? point.x - x : x - point.x;
            if (distance < best_distance) {
                best_distance = distance;
                hit = static_cast<int>(event.count);
            }
        }
        return hit;
    }

    [[nodiscard]] static mgstc::engine::SavedTimbreReference presetReference(
        const mgstc::engine::CompositeTimbre& sound,
        mgstc::engine::SccWavePreset preset) {
        constexpr std::array<const char*, 6> names{
            "Sine", "Square", "Triangle", "Saw", "Pulse 25%", "Pulse 12.5%"};
        mgstc::engine::SavedTimbreReference result;
        result.library_id = mgstc::engine::allocateCompositeOwnedTimbreId(sound);
        result.source = mgstc::engine::TimbreSource::Scc;
        result.name = names[static_cast<std::size_t>(preset)];
        const auto wave = mgstc::engine::generateSccPreset(
            preset, mgstc::engine::SccHarmonic::One);
        for (std::size_t i = 0; i < wave.size(); ++i)
            result.scc_waveform[i] = static_cast<std::uint8_t>(wave[i]);
        return result;
    }

    void previewTimbreChoice(std::size_t layer_index,
        const EnvelopeTimbreChoice& choice, std::uint8_t note, bool one_second = false) {
        if (!manual_y_preview_ || layer_index >= timbre_.layers.size()) return;
        auto preview = timbre_;
        auto& layer = preview.layers[layer_index];
        if (choice.scc_preset && layer.source == mgstc::engine::TimbreSource::Scc) {
            layer.base_timbre = presetReference(preview, *choice.scc_preset);
            layer.base_opll_rom.reset();
        } else if (choice.pick == mgstc::engine::TimbrePick::OpllRom) {
            layer.base_timbre.reset();
            layer.base_opll_rom = choice.rom_number;
        } else if (choice.pick == mgstc::engine::TimbrePick::Library) {
            std::optional<mgstc::engine::SavedTimbreReference> snapshot;
            if (const auto* embedded = mgstc::engine::findEmbeddedTimbreSnapshot(
                    preview, choice.library_id)) snapshot = *embedded;
            else if (const auto* library = timbreLibrary()) {
                if (const auto* entry = library->find(choice.library_id))
                    snapshot = mgstc::engine::makeSavedTimbreReference(*entry);
            }
            if (!snapshot || snapshot->source != layer.source) return;
            layer.base_timbre = std::move(snapshot);
            layer.base_opll_rom.reset();
        } else return;
        // Audition the selected source from key-on through the existing ENV,
        // pitch and modulation paths, without earlier @/manual-y replacing it.
        std::erase_if(layer.timbre_automation, [](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                || (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && event.value >= 0 && event.value <= 7);
        });
        if (one_second) {
            // Selection audition is a single neutral tone, independent of the ENV.
            const auto defaults = mgstc::engine::defaultCompositeTimbre();
            auto plain = *std::find_if(defaults.layers.begin(), defaults.layers.end(),
                [&](const auto& candidate) { return candidate.source == layer.source; });
            plain.base_timbre = layer.base_timbre;
            plain.base_opll_rom = layer.base_opll_rom;
            preview.layers = {std::move(plain)};
        }
        manual_y_preview_(std::move(preview), note, one_second);
    }

    void pickTimbreAt(int timeline_count, bool focus_morph) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        selected_count_ = timeline_count;
        std::vector<EnvelopeTimbreCatalogItem> catalog;
        if (catalog_callback_) {
            catalog = catalog_callback_(layer.source);
        }
        EnvelopeTimbreChoice current;
        juce::String current_name;
        if (const auto* existing = timbreEventAt(
                layer, static_cast<std::uint32_t>(timeline_count),
                isLoopStartCount(layer, timeline_count) ? std::optional<bool>{loop_insert_after_} : std::nullopt)) {
            current.pick = existing->timbre_pick;
            current.library_id = existing->target_library_id;
            current.rom_number = static_cast<std::uint8_t>(
                juce::jlimit(0, 14, existing->value));
            current_name = timbreCommandLabel(*existing);
        }
        insertCurrentAssignmentCatalogItem(
            catalog, current, timbre_, current_name);
        const auto count = timeline_count;
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音色（@）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeTimbrePickContent(
            layer.source,
            catalog,
            current,
            [this, count](EnvelopeTimbreChoice choice) {
                applyTimbreChoice(count, choice);
            },
            [this, count, source = layer.source](std::uint64_t library_id) {
                if (mgstc::engine::isStandaloneLibraryTimbreId(library_id)) {
                    EnvelopeTimbreChoice choice;
                    choice.pick = mgstc::engine::TimbrePick::Library;
                    choice.library_id = library_id;
                    applyTimbreChoice(count, choice);
                    if (selected_layer_ >= 0
                        && selected_layer_
                            < static_cast<int>(timbre_.layers.size())) {
                        if (const auto* event = timbreEventAt(
                                timbre_.layers[static_cast<std::size_t>(
                                    selected_layer_)],
                                static_cast<std::uint32_t>(count))) {
                            library_id = event->target_library_id;
                        }
                    }
                }
                if (open_timbre_callback_) {
                    open_timbre_callback_(
                        source == mgstc::engine::TimbreSource::Scc
                            ? "scc-envelope" : "opll-envelope",
                        library_id);
                }
            },
            [this, count] {
                removeTimbreAt(count);
            },
            [this] {
                if (manage_tags_callback_) {
                    manage_tags_callback_();
                }
            },
            [this](std::uint64_t id, juce::String name) {
                return mutate_name_callback_
                    ? mutate_name_callback_(id, std::move(name))
                    : false;
            },
            [this](std::uint64_t id, std::vector<std::string> tags) {
                return mutate_tags_callback_
                    ? mutate_tags_callback_(id, std::move(tags))
                    : false;
            },
            [this](std::uint64_t id, juce::String memo) {
                return mutate_memo_callback_
                    ? mutate_memo_callback_(id, std::move(memo))
                    : false;
            },
            false, session_, {},
            [this](std::uint8_t note) {
                if (manual_y_note_off_) manual_y_note_off_(note);
            }, {},
            [this, count, layer_index = static_cast<std::size_t>(selected_layer_)] {
                return addBlankOriginalAtStep(layer_index, count);
            },
            [this, layer_index = static_cast<std::size_t>(selected_layer_)](
                EnvelopeTimbreChoice choice, std::uint8_t note) {
                previewTimbreChoice(layer_index, choice, note);
            },
            [this] { if (manual_y_end_) manual_y_end_(); },
            [this, layer_index = static_cast<std::size_t>(selected_layer_)](
                EnvelopeTimbreChoice choice) {
                if (timbre_.layers[layer_index].source != mgstc::engine::TimbreSource::Scc) return;
                const auto note = session_ != nullptr
                    && !standaloneHostPreferences(session_->snapshot().capabilities)
                    ? session_->auditionPreferences().last_note
                    : LastAuditionNoteStore::instance().get();
                previewTimbreChoice(layer_index, choice, note, true);
            });
        bool has_morph_settings = false;
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            const auto* arrival = timbreEventAt(layer, static_cast<std::uint32_t>(timeline_count));
            const auto previous = arrival ? morphStartEvent(layer, *arrival) : std::nullopt;
            if (arrival) {
                has_morph_settings = true;
                const auto maximum = previous && previous->count < arrival->count
                    ? static_cast<int>(std::min<std::uint32_t>(255, arrival->count - previous->count - 1)) : -1;
                const auto layer_index = static_cast<std::size_t>(selected_layer_);
                content->configureMorph(arrival->scc_morph,
                    previous ? (previousTimbreEvent(layer, *arrival)
                        ? timbreCommandLabel(*previous)
                        : juce::String::fromUTF8("初期音色")) : juce::String::fromUTF8("（なし）"),
                    timbreCommandLabel(*arrival), maximum,
                    [this, count, layer_index,
                     safe = juce::Component::SafePointer<EnvelopeTimbrePickContent>(content)](
                        EnvelopeTimbrePickContent::MorphSettings settings, bool commit) {
                        return applyIncomingMorph(layer_index, count, settings, commit,
                            [safe](auto confirmed, auto result) {
                                if (safe != nullptr) safe->completeMorphChange(std::move(confirmed), result);
                            });
                    }, focus_morph);
            }
        }
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        juce::Component* dialog_content = content;
        if (has_morph_settings) {
            const auto& displays = juce::Desktop::getInstance().getDisplays();
            const auto* display = displays.getDisplayForRect(getScreenBounds());
            if (display == nullptr) display = displays.getPrimaryDisplay();
            if (display != nullptr) {
                const auto height_limit = juce::jmax(UiLayout::fieldH * 4,
                    display->userBounds.toNearestInt().getHeight() - UiLayout::fieldH * 2 - UiLayout::panelPad * 2);
                if (content->getHeight() > height_limit)
                    dialog_content = new ScrollableTimbrePickerContent(content, height_limit);
            }
        }
        dialog->setContentOwned(dialog_content, true);
        dialog->centreAroundComponent(
            this, dialog_content->getWidth(), dialog_content->getHeight()
                + (has_morph_settings ? UiLayout::fieldH : 32));
        dialog->enterModalState(true, nullptr, true);
    }

    void pickBaseTimbre(std::size_t layer_index) {
        if (layer_index >= timbre_.layers.size()) {
            return;
        }
        auto& layer = timbre_.layers[layer_index];
        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            return;
        }
        selected_layer_ = static_cast<int>(layer_index);
        std::vector<EnvelopeTimbreCatalogItem> catalog;
        if (catalog_callback_) {
            catalog = catalog_callback_(layer.source);
        }
        EnvelopeTimbreChoice current;
        juce::String current_name;
        if (mgstc::engine::layerUsesOpllRomBase(layer) && layer.base_opll_rom) {
            current.pick = mgstc::engine::TimbrePick::OpllRom;
            current.rom_number = *layer.base_opll_rom;
            current_name = opllRomPatchLabel(
                static_cast<int>(*layer.base_opll_rom));
        } else if (layer.base_timbre) {
            current.pick = mgstc::engine::TimbrePick::Library;
            current.library_id = layer.base_timbre->library_id;
            if (timbre_name_callback_) {
                current_name = timbre_name_callback_(current.library_id);
            }
            if (current_name.isEmpty()) {
                current_name = juce::String::fromUTF8(
                    layer.base_timbre->name.c_str());
            }
        }
        insertCurrentAssignmentCatalogItem(
            catalog, current, timbre_, current_name);
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音色設定"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeTimbrePickContent(
            layer.source,
            catalog,
            current,
            [this, layer_index](EnvelopeTimbreChoice choice) {
                applyLayerBaseChoice(layer_index, choice);
            },
            [this, layer_index, source = layer.source](
                std::uint64_t library_id) {
                if (mgstc::engine::isStandaloneLibraryTimbreId(library_id)) {
                    EnvelopeTimbreChoice choice;
                    choice.pick = mgstc::engine::TimbrePick::Library;
                    choice.library_id = library_id;
                    applyLayerBaseChoice(layer_index, choice);
                    if (layer_index < timbre_.layers.size()
                        && timbre_.layers[layer_index].base_timbre) {
                        library_id =
                            timbre_.layers[layer_index]
                                .base_timbre->library_id;
                    }
                }
                if (open_timbre_callback_) {
                    open_timbre_callback_(
                        source == mgstc::engine::TimbreSource::Scc
                            ? "scc-envelope" : "opll-envelope",
                        library_id);
                }
            },
            {},
            [this, layer_index, source = layer.source] {
                if (library_replace_callback_) {
                    library_replace_callback_(source, layer_index);
                }
            },
            [this](std::uint64_t id, juce::String name) {
                return mutate_name_callback_
                    ? mutate_name_callback_(id, std::move(name))
                    : false;
            },
            [this](std::uint64_t id, std::vector<std::string> tags) {
                return mutate_tags_callback_
                    ? mutate_tags_callback_(id, std::move(tags))
                    : false;
            },
            [this](std::uint64_t id, juce::String memo) {
                return mutate_memo_callback_
                    ? mutate_memo_callback_(id, std::move(memo))
                    : false;
            },
            true,
            session_,
            base_note_on_,
            base_note_off_,
            base_audition_,
            [this, layer_index]() -> std::optional<std::uint64_t> {
                return addBlankOriginal(layer_index);
            });
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyLayerBaseChoice(
        std::size_t layer_index, const EnvelopeTimbreChoice& choice) {
        if (layer_index < timbre_.layers.size() && choice.scc_preset
            && timbre_.layers[layer_index].source == mgstc::engine::TimbreSource::Scc) {
            selected_layer_ = static_cast<int>(layer_index);
            auto snapshot = presetReference(timbre_, *choice.scc_preset);
            if (const auto& previous = timbre_.layers[layer_index].base_timbre) {
                snapshot.number_mode = previous->number_mode;
                snapshot.manual_number = previous->manual_number;
                const auto id = previous->library_id;
                const bool another_owner = std::any_of(timbre_.embedded_timbres.begin(),
                    timbre_.embedded_timbres.end(), [id](const auto& entry) { return entry.library_id == id; })
                    || std::any_of(timbre_.layers.begin(), timbre_.layers.end(),
                        [&](const auto& layer) { return &layer != &timbre_.layers[layer_index]
                            && layer.base_timbre && layer.base_timbre->library_id == id; });
                const auto assigned = mgstc::engine::assignedNumberForLibraryId(
                    mgstc::engine::resolveTimbreNumbers(timbre_), id);
                bool referenced = false;
                for (const auto& layer : timbre_.layers)
                    for (const auto& event : layer.timbre_automation)
                        referenced = referenced || (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                            && event.timbre_pick == mgstc::engine::TimbrePick::Library
                            && (event.target_library_id == id || (!event.target_library_id
                                && layer.source == previous->source
                                && ((assigned && event.value == *assigned)
                                    || (previous->manual_number && event.value == *previous->manual_number)))));
                if (referenced && !another_owner) timbre_.embedded_timbres.push_back(*previous);
                if (referenced || another_owner) {
                    // Keep the former tone's output assignment for existing users.
                    // The replacement is a distinct waveform and needs its own number.
                    snapshot.number_mode = mgstc::engine::TimbreNumberMode::Automatic;
                    snapshot.manual_number.reset();
                    for (auto& layer : timbre_.layers)
                        for (auto& event : layer.timbre_automation)
                            if (event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                                && event.timbre_pick == mgstc::engine::TimbrePick::Library
                                && !event.target_library_id && layer.source == previous->source
                                && ((assigned && event.value == *assigned)
                                    || (previous->manual_number && event.value == *previous->manual_number)))
                                event.target_library_id = id;
                }
            }
            timbre_.layers[layer_index].base_timbre = std::move(snapshot);
            timbre_.layers[layer_index].base_opll_rom.reset();
            notifyLayerEdit(0, 0, true);
            return;
        }
        if (!assign_timbre_callback_ || layer_index >= timbre_.layers.size()) {
            return;
        }
        selected_layer_ = static_cast<int>(layer_index);
        LayerBaseTimbreAssign assign;
        if (choice.pick == mgstc::engine::TimbrePick::OpllRom) {
            assign.opll_rom = choice.rom_number;
        } else {
            assign.library_id = choice.library_id;
        }
        assign_timbre_callback_(layer_index, assign);
    }

    void promptVolumeNumeric(juce::Point<int> point) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto bounds = graphStrip(slots.volume);
        if (!bounds.isEmpty()) {
            selected_count_ = countAtX(bounds, point.x);
        }
        parameter_.setSelectedId(
            static_cast<int>(Parameter::Volume), juce::dontSendNotification);
        const int current = storedValueAt(
            layer, Parameter::Volume, selected_count_);
        bool automatic = false;
        bool precise = false;
        for (const auto& event : layer.volume_envelope.events) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume
                && event.count
                    == static_cast<std::uint32_t>(selected_count_)) {
                automatic = event.automatic;
                precise = event.precise;
                break;
            }
        }
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音量"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeNumericEntryContent(
            juce::String::fromUTF8(
                "音量（0～15）を入力。範囲外はクランプします"),
            juce::String(current),
            [this, automatic, precise](int typed) {
                editPoint(
                    selected_count_,
                    juce::jlimit(0, 15, typed),
                    true,
                    automatic,
                    precise);
            },
            true,
            automatic,
            [this](int typed, bool automatic_mode, bool precise_mode) {
                editPoint(
                    selected_count_,
                    juce::jlimit(0, 15, typed),
                    true,
                    automatic_mode,
                    precise_mode);
            },
            precise);
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void promptPitchNumeric(juce::Point<int> point) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        const auto& layer =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const auto graph =
            graph_bounds_[static_cast<std::size_t>(selected_layer_)];
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto bounds = graphStrip(slots.pitch);
        if (!bounds.isEmpty()) {
            selected_count_ = countAtX(bounds, point.x);
        }
        parameter_.setSelectedId(
            static_cast<int>(Parameter::Pitch), juce::dontSendNotification);
        const int current = lastPitchCommandAt(layer, selected_count_);
        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("音程（\\）"),
            juce::Colour(0xFF1B222C));
        auto* content = new EnvelopeNumericEntryContent(
            juce::String::fromUTF8(
                "周波数オフセットを入力。-200 は -127 -73 に展開します"),
            juce::String(current),
            [this](int typed) {
                applyPitchTypedDelta(selected_count_, typed);
            });
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyPitchTypedDelta(int timeline_count, int typed_delta) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto& events = layer.pitch_envelope.events;
        layer.pitch_envelope.kind = mgstc::engine::EnvelopeKind::Sequence;
        bool after_loop = newCommandAfterLoopStart(
            static_cast<std::uint32_t>(timeline_count));
        const bool split = isLoopStartCount(layer, timeline_count);
        std::erase_if(events, [timeline_count, split, after_loop](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Pitch
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && (!split || event.after_loop_start == after_loop);
        });
        auto parts = splitPitchDelta(typed_delta);
        if (parts.empty() && typed_delta == 0) {
            parts.push_back(0);
        }
        for (const int part : parts) {
            events.push_back({
                .kind = mgstc::engine::EnvelopeEventKind::Pitch,
                .value = part,
                .count = static_cast<std::uint32_t>(timeline_count),
                .after_loop_start = after_loop,
            });
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(
            timeline_count,
            cumulativePitchAt(layer, timeline_count),
            true);
    }

    [[nodiscard]] bool countHasManualOriginalY(
        const mgstc::engine::CompositeLayer& layer,
        std::uint32_t count) const noexcept {
        for (const auto& event : layer.timbre_automation) {
            if (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                && event.count == count
                && (!isLoopStartCount(layer, static_cast<int>(count))
                    || event.after_loop_start == loop_insert_after_)
                && event.value >= 0
                && event.value <= 7) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] juce::String registerInspectLabel(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, timeline_count));
        const auto grouped = registerWritesByCount(layer);
        const auto it = grouped.find(count);
        if (it == grouped.end() || it->second.empty()) {
            return juce::String::fromUTF8("（なし）");
        }
        auto events = it->second;
        if (isLoopStartCount(layer, timeline_count))
            std::erase_if(events, [&](const auto* event) { return event->after_loop_start != loop_insert_after_; });
        return events.empty() ? juce::String::fromUTF8("（なし）")
                              : formatRegisterGroupLabel(layer, count, events);
    }

    [[nodiscard]] juce::String registerInspectTooltip(
        const mgstc::engine::CompositeLayer& layer,
        int timeline_count) const {
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, timeline_count));
        const auto grouped = registerWritesByCount(layer);
        const auto it = grouped.find(count);
        if (it == grouped.end() || it->second.empty()) {
            return juce::String::fromUTF8(
                "このカウントにY-Register y はありません");
        }
        return formatRegisterGroupTooltip(it->second);
    }

    void pickRegisterAt(int timeline_count) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.source == mgstc::engine::TimbreSource::Scc) {
            return;
        }
        timeline_count = juce::jlimit(0, maximumCount(), timeline_count);
        selected_count_ = timeline_count;
        const auto count = static_cast<std::uint32_t>(timeline_count);
        const auto* library = timbreLibrary();

        if (layer.source == mgstc::engine::TimbreSource::Psg) {
            std::uint8_t reg = 0;
            std::uint8_t data = 0;
            for (const auto& event : layer.timbre_automation) {
                if (event.kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && event.count == count) {
                    reg = static_cast<std::uint8_t>(
                        juce::jlimit(0, 15, event.value));
                    data = static_cast<std::uint8_t>(
                        juce::jlimit(0, 255, event.secondary));
                }
            }
            auto* dialog = new ModalDialogWindow(
                juce::String::fromUTF8("Y-Register y（PSG）"),
                juce::Colour(0xFF1B222C));
            auto* content = new EnvelopePsgYParamContent(
                reg,
                data,
                [this, count](std::uint8_t r, std::uint8_t d) {
                    applyPsgManualY(count, r, d);
                });
            pending_dialogs_.emplace_back(dialog);
            dialog->setUsingNativeTitleBar(true);
            dialog->setResizable(false, false);
            dialog->setContentOwned(content, true);
            dialog->centreAroundComponent(
                this, content->getWidth(), content->getHeight() + 32);
            dialog->enterModalState(true, nullptr, true);
            return;
        }

        // OPLL: original-tone regs 0–7 only; ROM intervals unavailable.
        if (mgstc::engine::opllActiveTimbreAt(layer, count,
                !isLoopStartCount(layer, timeline_count) || loop_insert_after_).kind
            == mgstc::engine::OpllActiveTimbreKind::Rom) {
            reportStatus(juce::String::fromUTF8(
                "ROM音色区間ではオリジナル音色のY-Register yは使えません"));
            return;
        }
        // Display / diff include TL/FB auto at this count (§7.5 after manual y).
        // Apply baseline uses the same auto-inclusive image so unchanged auto
        // fields are not re-emitted as manual y.
        const auto baseline_regs = mgstc::engine::opllOriginalRegisterImageAt(
            layer, count, library, false, true,
            !isLoopStartCount(layer, timeline_count) || loop_insert_after_);
        if (!baseline_regs) {
            reportStatus(juce::String::fromUTF8(
                "このステップにオリジナル音色画像がありません"
                "（ベース／@ライブラリを設定してください）"));
            return;
        }
        const bool editing = countHasManualOriginalY(layer, count);
        const auto current_regs = editing
            ? mgstc::engine::opllOriginalRegisterImageAt(
                  layer, count, library, true, true,
                  !isLoopStartCount(layer, timeline_count) || loop_insert_after_)
                  .value_or(*baseline_regs)
            : *baseline_regs;
        const auto baseline_patch =
            mgstc::engine::decodeOpllPatch(*baseline_regs);
        const auto initial_patch =
            mgstc::engine::decodeOpllPatch(current_regs);

        auto* dialog = new ModalDialogWindow(
            juce::String::fromUTF8("Y-Register y（FMパラメータ）"),
            juce::Colour(0xFF1B222C));
        const auto layer_index = static_cast<std::size_t>(selected_layer_);
        auto* content = new EnvelopeOpllYParamContent(
            baseline_patch,
            initial_patch,
            editing,
            [this, count, baseline = *baseline_regs](
                mgstc::engine::OpllPatchParameters patch) {
                applyOpllManualYPatch(count, baseline, patch);
            },
            session_,
            [this, layer_index](mgstc::engine::OpllPatchParameters patch,
                std::uint8_t note, bool one_second) {
                if (!manual_y_preview_ || layer_index >= timbre_.layers.size())
                    return;
                auto preview = timbre_;
                auto& preview_layer = preview.layers[layer_index];
                if (!preview_layer.base_timbre) {
                    mgstc::engine::SavedTimbreReference base;
                    base.source = mgstc::engine::TimbreSource::Opll;
                    base.name = "Y-Register y試聴";
                    preview_layer.base_timbre = std::move(base);
                }
                preview_layer.base_opll_rom.reset();
                preview_layer.base_timbre->library_id =
                    mgstc::engine::allocateCompositeOwnedTimbreId(preview);
                preview_layer.base_timbre->number_mode =
                    mgstc::engine::TimbreNumberMode::Automatic;
                preview_layer.base_timbre->manual_number.reset();
                preview_layer.base_timbre->opll_registers =
                    mgstc::engine::encodeOpllPatch(patch);
                std::erase_if(preview_layer.timbre_automation,
                    [](const auto& event) {
                        return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                            || (event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                                && event.value >= 0 && event.value <= 7);
                    });
                preview_layer.opll_tl_auto.mode = mgstc::engine::OpllRegisterAutoMode::Off;
                preview_layer.opll_fb_auto.mode = mgstc::engine::OpllRegisterAutoMode::Off;
                preview_layer.register_auto_profiles.clear();
                manual_y_preview_(std::move(preview), note, one_second);
            },
            [this](std::uint8_t note) {
                if (manual_y_note_off_) manual_y_note_off_(note);
            },
            [this] {
                if (manual_y_end_) manual_y_end_();
            });
        pending_dialogs_.emplace_back(dialog);
        dialog->setUsingNativeTitleBar(true);
        dialog->setResizable(false, false);
        dialog->setContentOwned(content, true);
        dialog->centreAroundComponent(
            this, content->getWidth(), content->getHeight() + 32);
        dialog->enterModalState(true, nullptr, true);
    }

    void applyOpllManualYPatch(
        std::uint32_t count,
        const std::array<std::uint8_t, 8>& baseline_regs,
        const mgstc::engine::OpllPatchParameters& patch) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        const bool after_loop = newCommandAfterLoopStart(count);
        const bool split = isLoopStartCount(timbre_.layers[static_cast<std::size_t>(selected_layer_)], static_cast<int>(count));
        std::erase_if(events, [count, split, after_loop](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::RegisterWrite
                && event.count == count && event.value >= 0 && event.value <= 7
                && (!split || event.after_loop_start == after_loop);
        });
        const auto next = mgstc::engine::encodeOpllPatch(patch);
        int emitted = 0;
        for (int reg = 0; reg < 8; ++reg) {
            if (next[static_cast<std::size_t>(reg)]
                == baseline_regs[static_cast<std::size_t>(reg)]) {
                continue;
            }
            mgstc::engine::EnvelopeEvent event;
            event.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite;
            event.count = count;
            event.value = reg;
            event.secondary = next[static_cast<std::size_t>(reg)];
            event.after_loop_start = after_loop;
            events.push_back(event);
            ++emitted;
        }
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                if (left.count != right.count) {
                    return left.count < right.count;
                }
                return false;
            });
        notifyLayerEdit(static_cast<int>(count), emitted, true);
        reportStatus(
            emitted == 0
                ? juce::String::fromUTF8(
                      "変更なし（このステップのY-Register y群をクリア）")
                : juce::String::fromUTF8("Y-Register yを ")
                      + juce::String(emitted)
                      + juce::String::fromUTF8(" 本設定しました"));
    }

    void applyPsgManualY(
        std::uint32_t count, std::uint8_t reg, std::uint8_t data) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        mgstc::engine::EnvelopeEvent event;
        event.kind = mgstc::engine::EnvelopeEventKind::RegisterWrite;
        event.count = count;
        event.value = reg;
        event.secondary = data;
        event.after_loop_start = newCommandAfterLoopStart(count);
        events.push_back(event);
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(static_cast<int>(count), reg, true);
        reportStatus(
            "y" + juce::String(static_cast<int>(reg)) + ","
            + juce::String(static_cast<int>(data))
            + juce::String::fromUTF8(" を追加しました"));
    }

    void applyTimbreChoice(int timeline_count, const EnvelopeTimbreChoice& choice) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        EnvelopeTimbrePickContent::MorphSettings morph_settings;
        if (const auto* existing = timbreEventAt(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)],
                static_cast<std::uint32_t>(timeline_count),
                isLoopStartCount(timbre_.layers[static_cast<std::size_t>(selected_layer_)], timeline_count)
                    ? std::optional<bool>{loop_insert_after_} : std::nullopt))
            morph_settings = existing->scc_morph;
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        const bool split = isLoopStartCount(layer, timeline_count);
        const bool after = newCommandAfterLoopStart(static_cast<std::uint32_t>(timeline_count));
        std::erase_if(events, [timeline_count, split, after](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count)
                && (!split || event.after_loop_start == after);
        });
        mgstc::engine::EnvelopeEvent event;
        event.scc_morph = morph_settings;
        event.kind = mgstc::engine::EnvelopeEventKind::Timbre;
        event.count = static_cast<std::uint32_t>(timeline_count);
        event.timbre_pick = choice.pick;
        event.target_library_id = choice.library_id;
        if (choice.scc_preset
            && timbre_.layers[static_cast<std::size_t>(selected_layer_)].source
                == mgstc::engine::TimbreSource::Scc) {
            auto snapshot = presetReference(timbre_, *choice.scc_preset);
            event.target_library_id = snapshot.library_id;
            timbre_.embedded_timbres.push_back(std::move(snapshot));
        }
        if (choice.pick == mgstc::engine::TimbrePick::Library
            && mgstc::engine::isStandaloneLibraryTimbreId(choice.library_id)
            && timbre_library_callback_) {
            if (const auto* library = timbre_library_callback_()) {
                if (const auto* entry = library->find(choice.library_id)) {
                    auto adopted =
                        mgstc::engine::adoptLibraryTimbre(timbre_, *entry);
                    timbre_.embedded_timbres.push_back(adopted);
                    event.target_library_id = adopted.library_id;
                }
            }
        }
        event.value = choice.pick == mgstc::engine::TimbrePick::OpllRom
            ? static_cast<std::int32_t>(choice.rom_number)
            : 0;
        event.after_loop_start = newCommandAfterLoopStart(event.count);
        events.push_back(event);
        std::stable_sort(
            events.begin(), events.end(),
            [](const auto& left, const auto& right) {
                return left.count < right.count;
            });
        notifyLayerEdit(timeline_count, event.value, true);
    }

    void removeTimbreAt(int timeline_count) {
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())) {
            return;
        }
        auto& events =
            timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                .timbre_automation;
        std::erase_if(events, [timeline_count](const auto& event) {
            return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                && event.count == static_cast<std::uint32_t>(timeline_count);
        });
        notifyLayerEdit(timeline_count, 0, true);
    }

    void reportStatus(const juce::String& text) {
        if (status_callback_) {
            status_callback_(text);
        }
    }

    void notifyDelete(const juce::String& what) {
        syncPointEditors();
        syncRegisterAutoEditors();
        if (selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(timbre_.layers.size())) {
            refreshCommandStack(
                timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
        }
        repaint();
        if (edit_callback_) {
            publishEdit(true, true);
        }
        reportStatus(what);
    }

    void deleteCommandAt(juce::Point<int> point) {
        const int layer_index = layerAt(point);
        if (layer_index < 0
            || layer_index >= static_cast<int>(timbre_.layers.size())
            || layer_index >= static_cast<int>(graph_bounds_.size())) {
            return;
        }
        selected_layer_ = layer_index;
        auto& layer = timbre_.layers[static_cast<std::size_t>(layer_index)];
        const auto graph = graph_bounds_[static_cast<std::size_t>(layer_index)];
        if (!graph.contains(point)) {
            return;
        }
        const auto slots = makeEditLaneSlots(graph, layer.source);
        auto count_bounds = graph;
        count_bounds.removeFromLeft(UiLayout::compositeOverviewLabelW);
        selected_count_ = countAtX(count_bounds, point.x);
        const auto count = static_cast<std::uint32_t>(
            juce::jmax(0, selected_count_));

        if (slots.volume.contains(point)) {
            auto& events = layer.volume_envelope.events;
            const auto before = events.size();
            std::erase_if(events, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Volume
                    && event.count == count;
            });
            if (events.size() != before) {
                notifyDelete(juce::String::fromUTF8("音量イベントを削除しました"));
            }
            return;
        }
        if (pitchLaneExpanded() && slots.pitch.contains(point)) {
            auto& events = layer.pitch_envelope.events;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::Pitch
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("\\命令を削除しました"));
            }
            return;
        }
        if (slots.timbre.contains(point)) {
            const auto before = layer.timbre_automation.size();
            std::erase_if(layer.timbre_automation, [count](const auto& event) {
                return event.kind == mgstc::engine::EnvelopeEventKind::Timbre
                    && event.count == count;
            });
            if (layer.timbre_automation.size() != before) {
                notifyDelete(juce::String::fromUTF8("@切替を削除しました"));
            }
            return;
        }
        if (slots.register_write.contains(point)) {
            auto& events = layer.timbre_automation;
            int last = -1;
            for (int i = 0; i < static_cast<int>(events.size()); ++i) {
                if (events[static_cast<std::size_t>(i)].kind
                        == mgstc::engine::EnvelopeEventKind::RegisterWrite
                    && events[static_cast<std::size_t>(i)].count == count) {
                    last = i;
                }
            }
            if (last >= 0) {
                events.erase(events.begin() + last);
                notifyDelete(juce::String::fromUTF8("y命令を削除しました"));
            }
            return;
        }
        if (slots.tl_auto.contains(point) || slots.fb_auto.contains(point)) {
            const auto target = slots.tl_auto.contains(point)
                ? mgstc::engine::OpllRegisterAutoTarget::TotalLevel
                : mgstc::engine::OpllRegisterAutoTarget::Feedback;
            const auto& selected_auto = mgstc::engine::compositeRegisterAutoAt(layer, target, count,
                !isLoopStartCount(layer, static_cast<int>(count)) || loop_insert_after_);
            // Right-click clear applies only to free-curve drawings.
            if (selected_auto.mode
                != mgstc::engine::OpllRegisterAutoMode::FreeCurve) {
                reportStatus(juce::String::fromUTF8(
                    "TL/FB変調の右クリック削除は自由曲線のみです"
                    "（上昇／下降／LFOはモードやパラメーターで変更）"));
                parameter_.setSelectedId(
                    slots.tl_auto.contains(point)
                        ? static_cast<int>(Parameter::OpllTlAuto)
                        : static_cast<int>(Parameter::OpllFbAuto),
                    juce::dontSendNotification);
                syncRegisterAutoEditors();
                return;
            }
            if (selected_auto.free_curve.empty()) {
                return;
            }
            auto& auto_lane = mgstc::engine::editCompositeRegisterAutoAt(layer, target, count,
                !isLoopStartCount(layer, static_cast<int>(count)) || loop_insert_after_);
            const auto step = juce::jmax<std::uint8_t>(1, auto_lane.coarseness);
            if (count < auto_lane.start_count) {
                return;
            }
            const auto offset =
                (count - auto_lane.start_count) / step;
            if (offset >= auto_lane.free_curve.size()) {
                // Clear entire free curve when clicking past end.
                auto_lane.free_curve.clear();
                notifyDelete(juce::String::fromUTF8(
                    "自由曲線をクリアしました"));
                return;
            }
            auto_lane.free_curve.erase(
                auto_lane.free_curve.begin()
                + static_cast<std::ptrdiff_t>(offset));
            if (auto_lane.free_curve.empty()) {
                auto_lane.start_count = 0;
            }
            notifyDelete(juce::String::fromUTF8(
                "自由曲線の点を削除しました"));
            return;
        }
    }

    void changeStackCommand(int chip_index, std::optional<bool> to_after) {
        if (selected_layer_ < 0 || chip_index < 0) return;
        auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        auto commands = laneCommands(layer);
        std::erase_if(commands, [this](const auto& command) { return command.count != static_cast<std::uint32_t>(selected_count_); });
        if (layer.envelope_timeline.loop_end_count
            && *layer.envelope_timeline.loop_end_count == static_cast<std::uint32_t>(selected_count_)) {
            std::stable_sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) {
                return a.after > b.after;
            });
        }
        if (chip_index >= static_cast<int>(commands.size())) return;
        const auto command = commands[static_cast<std::size_t>(chip_index)];
        if (command.source == 1) {
            if (to_after) layer.modulation_profiles[command.index].after_loop_start = *to_after;
            else layer.modulation_profiles.erase(layer.modulation_profiles.begin() + static_cast<std::ptrdiff_t>(command.index));
        } else if (command.source == 2) {
            if (to_after) layer.register_auto_profiles[command.index].after_loop_start = *to_after;
            else layer.register_auto_profiles.erase(layer.register_auto_profiles.begin() + static_cast<std::ptrdiff_t>(command.index));
        } else {
            auto& events = command.parameter == Parameter::Pitch ? layer.pitch_envelope.events
                : command.parameter == Parameter::Volume ? layer.volume_envelope.events : layer.timbre_automation;
            if (to_after) events[command.index].after_loop_start = *to_after;
            else events.erase(events.begin() + static_cast<std::ptrdiff_t>(command.index));
        }
        finishLoopEdit();
    }

    void deleteStackChipAt(int chip_index) { changeStackCommand(chip_index, std::nullopt); }

    void moveStackChipAt(int chip_index, bool to_after) {
        if (selected_layer_ < 0 || !isLoopStartCount(timbre_.layers[static_cast<std::size_t>(selected_layer_)], selected_count_)) return;
        if (!to_after) {
            auto commands = laneCommands(timbre_.layers[static_cast<std::size_t>(selected_layer_)]);
            std::erase_if(commands, [this](const auto& command) { return command.count != static_cast<std::uint32_t>(selected_count_); });
            if (chip_index >= 0 && chip_index < static_cast<int>(commands.size())
                && commands[static_cast<std::size_t>(chip_index)].parameter == Parameter::Volume) return;
        }
        changeStackCommand(chip_index, to_after);
    }

    void rebuildLayerSetups() {
        pitch_lane_expanded_.resize(timbre_.layers.size(), false);
        while (layer_setups_.size()
               > static_cast<int>(timbre_.layers.size())) {
            layer_setups_.removeLast();
        }
        while (layer_setups_.size()
               < static_cast<int>(timbre_.layers.size())) {
            auto* setup = layer_setups_.add(new CompositeLayerSetupView());
            inspector_content_.addAndMakeVisible(setup);
        }
        for (int index = 0; index < layer_setups_.size(); ++index) {
            auto* setup = layer_setups_[index];
            const auto layer_index = static_cast<std::size_t>(index);
            setup->setCallbacks(
                [this, layer_index](bool commit) {
                    selected_layer_ = static_cast<int>(layer_index);
                    if (layer_index < timbre_.layers.size()
                        && static_cast<int>(layer_index) < layer_setups_.size()) {
                        layer_setups_[static_cast<int>(layer_index)]
                            ->applyToLayer(timbre_.layers[layer_index]);
                        if (commit
                            && mgstc::engine::reconcileLayerTimbreIdentity(
                                timbre_, layer_index)) {
                            for (int setup_index = 0;
                                 setup_index < layer_setups_.size();
                                 ++setup_index) {
                                const auto refreshed =
                                    static_cast<std::size_t>(setup_index);
                                if (refreshed < timbre_.layers.size()) {
                                    layer_setups_[setup_index]->syncFromLayer(
                                        timbre_.layers[refreshed]);
                                }
                            }
                        }
                    }
                    syncPointEditors();
                    syncRegisterAutoEditors();
                    syncInspector();
                    updateScrollRanges();
                    resized();
                    repaint();
                    if (edit_callback_) {
                        publishEdit(commit, false);
                    }
                    syncRateEditorFromModel();
                },
                [this, layer_index] {
                    if (layer_index >= timbre_.layers.size()
                        || !open_timbre_callback_) {
                        return;
                    }
                    const auto& layer = timbre_.layers[layer_index];
                    if (!layer.base_timbre
                        || mgstc::engine::layerUsesOpllRomBase(layer)) {
                        return;
                    }
                    if (layer.base_timbre->library_id
                        == mgstc::engine::kSccTrianglePresetLibraryId) {
                        timbre_.layers[layer_index].base_timbre->library_id =
                            mgstc::engine::allocateCompositeOwnedTimbreId(
                                timbre_);
                        if (edit_callback_) {
                            publishEdit(true, false);
                        }
                    }
                    open_timbre_callback_(
                        layer.source == mgstc::engine::TimbreSource::Scc
                            ? "scc-envelope" : "opll-envelope",
                        timbre_.layers[layer_index].base_timbre->library_id);
                },
                [this, layer_index] {
                    pickBaseTimbre(layer_index);
                },
                [this, layer_index] {
                    openSoftwareLfoDialog(layer_index);
                },
                [this, layer_index] {
                    openRateEditor(layer_index);
                },
                [this, layer_index] {
                    duplicateLayerAt(layer_index);
                },
                [this, layer_index] {
                    juce::MessageManager::callAsync(
                        [safe = juce::Component::SafePointer<Impl>(this),
                         layer_index] {
                            if (safe != nullptr) {
                                safe->requestRemoveLayer(layer_index);
                            }
                        });
                },
                [this] {
                    if (base_audition_) base_audition_();
                });
            setup->syncFromLayer(timbre_.layers[layer_index]);
        }
        updateAddButtons();
        layoutLayerSetups();
    }

    void rebuildLaneFrames() {
        const auto area = graphArea();
        graph_bounds_.assign(timbre_.layers.size(), {});
        lane_frames_.assign(timbre_.layers.size(), {});
        if (selected_layer_ < 0 || selected_layer_ >= static_cast<int>(timbre_.layers.size())) return;
        auto frame = area.withY(area.getY() - juce::roundToInt(vertical_scroll_.getCurrentRangeStart()))
            .withHeight(laneHeightForIndex(selected_layer_)).reduced(0, UiLayout::editLaneFramePadV);
        lane_frames_[static_cast<std::size_t>(selected_layer_)] = frame;
        graph_bounds_[static_cast<std::size_t>(selected_layer_)] =
            frame.reduced(UiLayout::sm, UiLayout::editLaneInnerPadV);
    }

    [[nodiscard]] static std::uint64_t volumeLaneFingerprint(
        const mgstc::engine::CompositeLayer& layer) noexcept {
        std::uint64_t hash = 14695981039346656037ull;
        const auto mix = [&](std::uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        mix(layer.volume);
        mix(static_cast<std::uint64_t>(layer.volume_envelope.kind));
        mix(layer.envelope_timeline.length_counts);
        mix(layer.envelope_timeline.loop_start_count.value_or(~0u));
        mix(layer.envelope_timeline.loop_end_count.value_or(~0u));
        const auto& rate = layer.volume_envelope.rate;
        mix(rate.tone_mode);
        mix(rate.noise);
        mix(rate.attack_level);
        mix(rate.attack_rate);
        mix(rate.decay_rate);
        mix(rate.sustain_level);
        mix(rate.sustain_rate);
        mix(rate.release_rate);
        for (const auto& event : layer.volume_envelope.events) {
            mix(static_cast<std::uint64_t>(event.kind));
            mix(static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(event.value)));
            mix(event.count);
            mix(event.automatic ? 1u : 0u);
            mix(event.precise ? 1u : 0u);
            mix(static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(event.secondary)));
            mix(event.after_loop_start ? 1u : 0u);
            mix(event.command_order);
        }
        for (const auto& profile : layer.modulation_profiles) {
            mix(static_cast<std::uint64_t>(profile.target)); mix(profile.count); mix(profile.after_loop_start);
            mix(profile.command_order);
            mix(profile.parameters.enabled); mix(static_cast<std::uint64_t>(profile.parameters.mode));
            mix(profile.parameters.delay); mix(profile.parameters.depth); mix(profile.parameters.speed);
            mix(static_cast<std::uint8_t>(profile.parameters.roughness));
        }
        for (const auto& placement : layer.derived_command_placements) {
            mix(placement.count); mix(static_cast<std::uint64_t>(placement.target));
            mix(placement.after_loop_start); mix(placement.command_order);
        }
        return hash;
    }

    [[nodiscard]] const mgstc::engine::RateEnvelopeTrace& cachedLayerRateTrace(
        std::size_t layer_index) const {
        if (layer_index >= rate_trace_cache_.size()) {
            rate_trace_cache_.resize(timbre_.layers.size());
        }
        const auto& layer = timbre_.layers[layer_index];
        auto& cache = rate_trace_cache_[layer_index];
        const auto rate = mgstc::engine::clampRateEnvelope(
            layer.volume_envelope.rate);
        if (cache.valid
            && cache.volume == layer.volume
            && cache.source == layer.source
            && cache.rate == rate) {
            return cache.trace;
        }
        cache.volume = layer.volume;
        cache.source = layer.source;
        cache.rate = rate;
        cache.trace = mgstc::app::makeLayerRateTrace(layer);
        cache.valid = true;
        return cache.trace;
    }

    [[nodiscard]] static std::vector<std::uint8_t> authorVolumeSamples(
        const mgstc::engine::CompositeLayer& layer, int ticks) {
        std::vector<mgstc::engine::EnvelopeEvent> points;
        for (const auto& event : layer.volume_envelope.events)
            if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) points.push_back(event);
        std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
            return a.count < b.count;
        });
        if (points.empty() || points.front().count > 0) points.insert(points.begin(),
            {.kind = mgstc::engine::EnvelopeEventKind::Volume, .value = layer.volume});
        std::vector<std::uint8_t> result(static_cast<std::size_t>(juce::jmax(0, ticks)), layer.volume);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto start = static_cast<int>(points[i].count);
            const int stop = i + 1 < points.size() ? static_cast<int>(points[i + 1].count) : ticks;
            const auto origin = static_cast<std::uint8_t>(juce::jlimit(0, 15, points[i].value));
            const bool ramp = i + 1 < points.size() && points[i + 1].automatic;
            const int ramp_start = ramp && points[i + 1].secondary > 0
                ? juce::jlimit(start, stop, points[i + 1].secondary - 1) : start;
            const auto samples = ramp && stop > ramp_start
                ? mgstc::engine::sampleAutomaticRampVolumes(origin,
                    static_cast<std::uint8_t>(juce::jlimit(0, 15, points[i + 1].value)),
                    static_cast<std::uint32_t>(stop - ramp_start), false)
                : std::vector<std::uint8_t>{};
            for (int count = juce::jmax(0, start); count < juce::jmin(stop, ticks); ++count)
                result[static_cast<std::size_t>(count)] = !samples.empty() && count >= ramp_start
                    ? samples[std::min(static_cast<std::size_t>(count - ramp_start), samples.size() - 1)]
                    : origin;
        }
        return result;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& cachedVolumeSamples(
        std::size_t layer_index,
        int ticks) const {
        if (layer_index >= volume_sample_cache_.size()) {
            volume_sample_cache_.resize(timbre_.layers.size());
        }
        const auto& layer = timbre_.layers[layer_index];
        auto& cache = volume_sample_cache_[layer_index];
        const auto fingerprint = volumeLaneFingerprint(layer);
        if (cache.ticks == ticks
            && cache.fingerprint == fingerprint) {
            return cache.samples;
        }
        cache.ticks = ticks;
        cache.fingerprint = fingerprint;
        cache.samples = mgstc::engine::sampleCompositeVolumeLane(layer, ticks);
        cache.author_samples = authorVolumeSamples(layer, ticks);
        return cache.samples;
    }

    void ensureLaneLayout() {
        if (!lane_layout_dirty_) {
            return;
        }
        auto previous_frames = lane_frames_;
        rebuildLaneFrames();
        layoutPitchLaneToggle();
        const auto envelope_kind =
            selected_layer_ >= 0
                && selected_layer_
                    < static_cast<int>(timbre_.layers.size())
            ? timbre_.layers[static_cast<std::size_t>(selected_layer_)]
                  .volume_envelope.kind
            : mgstc::engine::EnvelopeKind::Sequence;
        const bool register_auto = selectedLayerShowsRegisterAuto();
        if (lane_layout_applied_
            && previous_frames == lane_frames_
            && laid_out_selected_layer_ == selected_layer_
            && laid_out_envelope_kind_ == envelope_kind
            && laid_out_register_auto_ == register_auto) {
            lane_layout_dirty_ = false;
            return;
        }
        layoutLayerSetups();
        layoutEditToolsDock();
        laid_out_selected_layer_ = selected_layer_;
        laid_out_envelope_kind_ = envelope_kind;
        laid_out_register_auto_ = register_auto;
        lane_layout_applied_ = true;
        lane_layout_dirty_ = false;
    }

    void layoutPitchLaneToggle() {
        layoutStepEditToggle();
        if (selected_layer_ < 0
            || selected_layer_ >= static_cast<int>(timbre_.layers.size())
            || selected_layer_ >= static_cast<int>(graph_bounds_.size())) {
            pitch_lane_toggle_.setVisible(false);
            return;
        }
        const auto& layer = timbre_.layers[static_cast<std::size_t>(selected_layer_)];
        if (layer.volume_envelope.kind == mgstc::engine::EnvelopeKind::Rate) {
            pitch_lane_toggle_.setVisible(false);
            return;
        }
        auto label = makeEditLaneSlots(
            graph_bounds_[static_cast<std::size_t>(selected_layer_)],
            layer.source).pitch.removeFromLeft(UiLayout::compositeOverviewLabelW);
        const auto bounds = label.withHeight(UiLayout::fieldH)
            .reduced(UiLayout::xs, 0);
        pitch_lane_toggle_.setBounds(bounds);
        pitch_lane_toggle_.setToggleState(
            pitchLaneExpanded(), juce::dontSendNotification);
        pitch_lane_toggle_.setEnabled(
            !layer.software_lfo.enabled && !hasModulation(layer, mgstc::engine::CompositeModulationTarget::Pitch));
        pitch_lane_toggle_.setVisible(graphArea().contains(bounds));
        pitch_lane_toggle_.toFront(false);
    }

    void layoutLayerSetups() {
        for (int i = 0; i < layer_setups_.size(); ++i) {
            layer_setups_[i]->setVisible(!count_inspector_ && i == selected_layer_);
            layer_setups_[i]->setBounds(inspector_content_.getLocalBounds());
        }
        layoutOverview();
    }

    void layoutEditToolsDock() {
        using namespace UiLayout;
        const bool show = selected_layer_ >= 0
            && selected_layer_ < static_cast<int>(lane_frames_.size())
            && !lane_frames_[static_cast<std::size_t>(selected_layer_)]
                    .isEmpty();
        const bool show_rate = show
            && timbre_.layers[
                static_cast<std::size_t>(selected_layer_)]
                    .volume_envelope.kind
                == mgstc::engine::EnvelopeKind::Rate;
        const bool show_auto = show && !show_rate
            && selectedLayerShowsRegisterAuto()
            && (selectedParameter() == Parameter::OpllTlAuto
                || selectedParameter() == Parameter::OpllFbAuto)
            && selectedRegisterAutoLane() != nullptr
            && selectedRegisterAutoLane()->mode
                == mgstc::engine::OpllRegisterAutoMode::FreeCurve;

        auto set_tools_visible = [this, show, show_auto, show_rate](
            bool visible) {
            const bool sequence_tools = visible && !show_rate && count_inspector_;
            for (auto* c : std::initializer_list<juce::Component*>{
                     &position_, &value_label_,
                     &value_, &apply_, &inspector_selection_,
                     &length_label_, &length_editor_, &loop_start_label_,
                     &loop_start_editor_, &loop_end_label_, &loop_end_editor_,
                     &inspector_apply_, &clear_loop_, &command_stack_}) {
                c->setVisible(sequence_tools);
            }
            command_view_.setVisible(sequence_tools);
            rate_open_.setVisible(visible && show_rate && count_inspector_);
            modulation_heading_.setVisible(visible && !show_rate && count_inspector_);
            modulation_issue_.setVisible(visible && !show_rate && count_inspector_
                && modulation_issue_.getText().isNotEmpty());
            for (std::size_t i = 0; i < modulation_toggles_.size(); ++i) {
                modulation_labels_[i].setVisible(false);
                modulation_toggles_[i].setVisible(visible && !show_rate && count_inspector_);
                modulation_settings_[i].setVisible(visible && !show_rate && count_inspector_);
            }
            edit_mode_.setVisible(visible && !show_rate);
            parameter_.setVisible(visible && !show_rate);
            mml_toggle_.setEnabled(visible);
            zoom_in_.setEnabled(visible); zoom_out_.setEnabled(visible);
            envelope_mml_preview_.setVisible(visible && mml_toggle_.getToggleState());
            auto_mode_.setVisible(visible && show_auto && count_inspector_);
            auto_depth_.setVisible(visible && show_auto && count_inspector_);
            auto_speed_.setVisible(visible && show_auto && count_inspector_);
            auto_coarseness_.setVisible(visible && show_auto && count_inspector_);
            auto_stop_.setVisible(visible && show_auto && count_inspector_);
            auto_depth_label_.setVisible(visible && show_auto && count_inspector_);
            auto_speed_label_.setVisible(visible && show_auto && count_inspector_);
            auto_coarseness_label_.setVisible(visible && show_auto && count_inspector_);
            auto_stop_label_.setVisible(visible && show_auto && count_inspector_);
        };

        if (!show) {
            set_tools_visible(false);
            return;
        }
        set_tools_visible(true);
        updateEnvelopeMmlPreview();

        auto dock = inspector_content_.getLocalBounds().reduced(panelPad);
        auto row = [&dock] { auto r = dock.removeFromTop(fieldH); dock.removeFromTop(sm); return r; };
        auto split = [&](juce::Component& label, juce::Component& field) {
            auto r = row(); label.setBounds(r.removeFromLeft(UiLayout::compositeInspectorLabelW)); field.setBounds(r);
        };
        rate_open_.setBounds(dock.withHeight(fieldH));
        if (!show_rate) {
            auto point = row();
            position_.setBounds(point.removeFromLeft(compositeDockCountW));
            value_label_.setBounds(point.removeFromLeft(compositeDockLabelW));
            apply_.setBounds(point.removeFromRight(compositeDockApplyW));
            point.removeFromRight(controlGap); value_.setBounds(point);
        } else {
            rate_open_.setBounds(row());
        }
        modulation_heading_.setBounds(row());
        for (std::size_t i = 0; i < modulation_toggles_.size(); ++i) {
            auto modulation_row = row();
            modulation_labels_[i].setBounds({});
            modulation_settings_[i].setBounds(
                modulation_row.removeFromRight(UiLayout::libraryButtonMinW));
            modulation_row.removeFromRight(controlGap);
            modulation_toggles_[i].setBounds(modulation_row);
        }
        modulation_issue_.setBounds(row());
        if (!show_rate) {
            if (show_auto) {
                auto_mode_.setBounds(row());
                split(auto_depth_label_, auto_depth_);
                split(auto_speed_label_, auto_speed_);
                split(auto_coarseness_label_, auto_coarseness_);
                split(auto_stop_label_, auto_stop_);
            }
            inspector_selection_.setBounds(row());
            split(length_label_, length_editor_);
            split(loop_start_label_, loop_start_editor_);
            split(loop_end_label_, loop_end_editor_);
            auto actions = row();
            inspector_apply_.setBounds(actions.removeFromLeft(actions.getWidth() / 2));
            actions.removeFromLeft(controlGap); clear_loop_.setBounds(actions);
            command_view_.setBounds(dock.removeFromTop(UiLayout::fieldH * 8));
            command_stack_.setSize(juce::jmax(1, command_view_.getWidth() - kScrollBarSize),
                command_stack_.preferredHeight());
        }

    }

    void commitPlaybackTempoFromEditor() {
        cancelStepDrag();
        const int next = juce::jlimit(
            mgstc::engine::kMgscTempoMin,
            mgstc::engine::kMgscTempoMax,
            tempo_.getText().getIntValue() == 0
                ? mgstc::engine::kMgscDefaultTempo
                : tempo_.getText().getIntValue());
        tempo_.setText(juce::String(next), juce::dontSendNotification);
        if (next == playback_tempo_ && next == timbre_.playback_tempo) {
            return;
        }
        playback_tempo_ = next;
        timbre_.playback_tempo = next;
        repaint();
        if (edit_callback_) {
            publishEdit(true, false);
        }
    }

    juce::Component inspector_content_;
    juce::Viewport inspector_view_;
    CompositeOverviewCanvas overview_content_;
    juce::Viewport overview_view_;
    juce::OwnedArray<juce::TextButton> overview_select_, overview_mute_, overview_solo_;
    juce::TextButton setup_tab_, count_tab_, focus_;
    bool count_inspector_{};
    bool inspector_visible_{true};
    mgstc::engine::OriginalTimbreAllocationStarts allocation_starts_;
    mgstc::engine::CompositeTimbre timbre_;
    mgstc::engine::CompositeTimbre last_published_timbre_;
    bool pending_plan_invalidation_{};
    juce::ComboBox parameter_;
    juce::ComboBox edit_mode_;
    std::optional<LoopDrag> loop_drag_;
    juce::ComboBox auto_mode_;
    juce::TextButton add_channel_;
    juce::TextButton compact_overview_;
    juce::TextButton step_edit_mode_;
    juce::TextButton psg_add_;
    juce::TextButton scc_add_;
    juce::TextButton opll_add_;
    juce::TextEditor value_;
    juce::Label position_;
    juce::Label value_label_;
    juce::TextButton apply_;
    SwitchLookAndFeel modulation_switch_look_and_feel_;
    juce::Label modulation_heading_;
    juce::Label modulation_issue_;
    std::array<juce::Label, 4> modulation_labels_;
    std::array<juce::ToggleButton, 4> modulation_toggles_;
    std::array<juce::TextButton, 4> modulation_settings_;
    juce::Label auto_depth_label_;
    juce::Label auto_speed_label_;
    juce::Label auto_coarseness_label_;
    juce::Label auto_stop_label_;
    juce::TextEditor auto_depth_;
    juce::TextEditor auto_speed_;
    juce::TextEditor auto_coarseness_;
    juce::TextEditor auto_stop_;
    juce::Label inspector_selection_;
    juce::Label length_label_;
    juce::Label loop_start_label_;
    juce::Label loop_end_label_;
    juce::TextEditor length_editor_;
    juce::TextEditor loop_start_editor_;
    juce::TextEditor loop_end_editor_;
    juce::TextButton inspector_apply_;
    juce::TextButton clear_loop_;
    juce::Viewport command_view_;
    CountCommandStackView command_stack_;
    juce::TextButton rate_open_;
    juce::TextEditor envelope_mml_preview_;
    juce::String last_envelope_mml_preview_;
    juce::Label morph_bank_label_, morph_bank_usage_, opll_bank_label_, opll_bank_usage_;
    juce::Label allocation_error_label_;
    std::vector<juce::String> scc_timbre_usage_;
    juce::String morph_bank_error_, opll_bank_error_;
    juce::ComboBox morph_bank_, opll_bank_;
    std::optional<mgstc::engine::OriginalTimbreAllocationStarts> rejected_allocation_starts_;
    std::optional<mgstc::engine::CompositeTimbre> morph_preview_input_;
    mgstc::engine::SccMorphCompileResult morph_preview_;
    std::uint64_t morph_generation_{};
    std::optional<MorphPendingEdit> morph_pending_edit_;
    std::mutex morph_worker_mutex_;
    std::condition_variable_any morph_worker_wakeup_;
    std::optional<MorphWorkerRequest> morph_worker_request_;
    std::shared_ptr<std::stop_source> morph_active_cancellation_;
    std::shared_ptr<const mgstc::engine::SccMorphCompileResult> morph_worker_result_;
    std::uint64_t morph_result_generation_{};
    std::jthread morph_worker_;
    juce::ScrollBar horizontal_scroll_;
    juce::ScrollBar vertical_scroll_;
    EditCallback edit_callback_;
    StatusCallback status_callback_;
    CatalogCallback catalog_callback_;
    OpenTimbreCallback open_timbre_callback_;
    AssignTimbreCallback assign_timbre_callback_;
    TimbreNameCallback timbre_name_callback_;
    TimbreLibraryCallback timbre_library_callback_;
    ManageTagsCallback manage_tags_callback_;
    LibraryReplaceCallback library_replace_callback_;
    std::function<void(std::uint8_t)> base_note_on_;
    std::function<void(std::uint8_t)> base_note_off_;
    std::function<void()> base_audition_;
    ManualYPreviewCallback manual_y_preview_;
    std::function<void(std::uint8_t)> manual_y_note_off_;
    std::function<void()> manual_y_end_;
    MutateTimbreNameCallback mutate_name_callback_;
    MutateTimbreTagsCallback mutate_tags_callback_;
    MutateTimbreMemoCallback mutate_memo_callback_;
    LfoSessionCallback lfo_session_callback_;
    LfoPollKeysCallback lfo_poll_keys_callback_;
    std::uint64_t dialog_generation_{};
    std::vector<juce::Component::SafePointer<juce::DialogWindow>> pending_dialogs_;
    mgstc::app::EditorSession* session_{};
    std::unique_ptr<RateEnvelopeEditorWindow> rate_window_;
    std::optional<std::size_t> rate_editor_layer_;
    juce::OwnedArray<CompositeLayerSetupView> layer_setups_;
    juce::TextButton mml_toggle_;
    juce::TextButton zoom_in_, zoom_out_;
    juce::ToggleButton pitch_lane_toggle_;
    std::vector<bool> pitch_lane_expanded_;
    std::vector<bool> envelope_number_active_;
    juce::Label tempo_label_;
    juce::TextEditor tempo_;
    std::vector<juce::Rectangle<int>> graph_bounds_;
    std::vector<juce::Rectangle<int>> lane_frames_;
    bool lane_layout_dirty_{true};
    bool lane_layout_applied_{};
    int laid_out_selected_layer_{-2};
    mgstc::engine::EnvelopeKind laid_out_envelope_kind_{
        mgstc::engine::EnvelopeKind::Sequence};
    bool laid_out_register_auto_{};
    struct RateTraceCache {
        bool valid{};
        std::uint8_t volume{};
        mgstc::engine::TimbreSource source{};
        mgstc::engine::RateEnvelope rate{};
        mgstc::engine::RateEnvelopeTrace trace{};
    };
    struct VolumeSampleCache {
        int ticks{-1};
        std::uint64_t fingerprint{};
        std::vector<std::uint8_t> samples;
        std::vector<std::uint8_t> author_samples;
    };
    mutable std::vector<RateTraceCache> rate_trace_cache_;
    mutable std::vector<VolumeSampleCache> volume_sample_cache_;
    juce::MouseCursor::StandardCursorType last_mouse_cursor_{
        juce::MouseCursor::NormalCursor};
    std::optional<juce::Rectangle<int>> edit_draw_bounds_;
    std::optional<TimelineModulationTarget> pending_auto_settings_;
    std::optional<int> hover_preview_value_;
    std::optional<juce::Point<int>> hover_mouse_pos_;
    std::optional<Parameter> hover_edit_parameter_;
    int selected_layer_{};
    int selected_count_{};
    bool loop_insert_after_{};
    int playback_tempo_{mgstc::engine::kMgscDefaultTempo};
    int cached_count_ceiling_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    std::vector<std::optional<std::uint32_t>> output_cutoff_counts_;
    int scroll_extent_counts_{
        static_cast<int>(mgstc::engine::EnvelopeTimeline::kDefaultLengthCounts)};
    bool scroll_extent_initialized_{};
    bool drawing_{};
    struct StepDrag {
        mgstc::engine::CompositeTimbre original;
        mgstc::engine::CompositeStepEditLane lane;
        int layer{};
        int start{};
        int delta{};
        int requested_delta{std::numeric_limits<int>::min()};
        int original_selection{};
        juce::Point<int> pointer;
        bool active{};
    };
    std::optional<StepDrag> step_drag_;
    static constexpr int kScopeHistorySize = 4096;
    std::array<std::array<float, kScopeHistorySize>, 4>
        scope_history_{};
    int scope_write_position_{};
    int scope_size_{};
    std::size_t scope_visible_sample_count_{};
    std::uint8_t audition_note_{kPreviewNote};

    [[nodiscard]] std::optional<juce::Rectangle<int>> hoverPopupBounds()
        const {
        if (!hover_preview_value_ || !hover_mouse_pos_) {
            return std::nullopt;
        }
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            return std::nullopt;
        }
        const auto text = juce::String(*hover_preview_value_);
        const int text_w = juce::jmax(24, text.length() * 8 + 12);
        const int text_h = 18;
        auto box = juce::Rectangle<int>(
            hover_mouse_pos_->x + 12,
            hover_mouse_pos_->y - text_h - 8,
            text_w,
            text_h);
        if (auto area = graphArea(); !area.isEmpty()) {
            box.setX(juce::jlimit(
                area.getX(), area.getRight() - text_w, box.getX()));
            box.setY(juce::jlimit(
                area.getY(), area.getBottom() - text_h, box.getY()));
        }
        return box;
    }

    void drawHoverValuePopup(juce::Graphics& graphics) const {
        if (!hover_preview_value_ || !hover_mouse_pos_) {
            return;
        }
        const auto parameter = selectedParameter();
        if (parameter != Parameter::Volume
            && parameter != Parameter::Pitch
            && parameter != Parameter::OpllTlAuto
            && parameter != Parameter::OpllFbAuto) {
            return;
        }
        const auto text = juce::String(*hover_preview_value_);
        graphics.setFont(UiFonts::dense(true));
        const int text_w = juce::jmax(
            24, text.length() * 8 + 12);
        const int text_h = 18;
        auto box = juce::Rectangle<int>(
            hover_mouse_pos_->x + 12,
            hover_mouse_pos_->y - text_h - 8,
            text_w,
            text_h);
        if (auto area = graphArea(); !area.isEmpty()) {
            box.setX(juce::jlimit(
                area.getX(), area.getRight() - text_w, box.getX()));
            box.setY(juce::jlimit(
                area.getY(), area.getBottom() - text_h, box.getY()));
        }
        graphics.setColour(juce::Colour(0xEE151A20));
        graphics.fillRoundedRectangle(box.toFloat(), 3.0F);
        graphics.setColour(juce::Colour(kUiHoverAccent));
        graphics.drawRoundedRectangle(box.toFloat(), 3.0F, 1.0F);
        graphics.setColour(juce::Colour(0xFFE6EDF3));
        graphics.drawText(
            text, box, juce::Justification::centred, false);
    }
};


namespace mgstc::app {

CompositeTimeline::CompositeTimeline() {
    impl_ = std::make_unique<Impl>();
    addAndMakeVisible(*impl_);
}

CompositeTimeline::~CompositeTimeline() = default;

void CompositeTimeline::resized() {
    if (impl_ != nullptr) {
        impl_->setBounds(getLocalBounds());
    }
}

void CompositeTimeline::setEditCallback(EditCallback callback) {
    impl_->setEditCallback(std::move(callback));
}

void CompositeTimeline::setStatusCallback(StatusCallback callback) {
    impl_->setStatusCallback(std::move(callback));
}

void CompositeTimeline::setTimbreCatalogCallback(CatalogCallback callback) {
    impl_->setTimbreCatalogCallback(std::move(callback));
}

void CompositeTimeline::setOpenTimbreCallback(OpenTimbreCallback callback) {
    impl_->setOpenTimbreCallback(std::move(callback));
}

void CompositeTimeline::setAssignTimbreCallback(AssignTimbreCallback callback) {
    impl_->setAssignTimbreCallback(std::move(callback));
}

void CompositeTimeline::setTimbreNameCallback(TimbreNameCallback callback) {
    impl_->setTimbreNameCallback(std::move(callback));
}

void CompositeTimeline::setTimbreLibraryCallback(TimbreLibraryCallback callback) {
    impl_->setTimbreLibraryCallback(std::move(callback));
}

void CompositeTimeline::setManageTagsCallback(ManageTagsCallback callback) {
    impl_->setManageTagsCallback(std::move(callback));
}

void CompositeTimeline::setLibraryReplaceCallback(
    LibraryReplaceCallback callback) {
    impl_->setLibraryReplaceCallback(std::move(callback));
}

void CompositeTimeline::setBaseTimbrePerformance(
    std::function<void(std::uint8_t)> note_on,
    std::function<void(std::uint8_t)> note_off,
    std::function<void()> audition) {
    impl_->setBaseTimbrePerformance(
        std::move(note_on), std::move(note_off), std::move(audition));
}

void CompositeTimeline::setManualYPerformance(
    ManualYPreviewCallback preview,
    std::function<void(std::uint8_t)> note_off,
    std::function<void()> end) {
    impl_->setManualYPerformance(
        std::move(preview), std::move(note_off), std::move(end));
}

void CompositeTimeline::setMutateTimbreNameCallback(
    MutateTimbreNameCallback callback) {
    impl_->setMutateTimbreNameCallback(std::move(callback));
}

void CompositeTimeline::setMutateTimbreTagsCallback(
    MutateTimbreTagsCallback callback) {
    impl_->setMutateTimbreTagsCallback(std::move(callback));
}

void CompositeTimeline::setMutateTimbreMemoCallback(
    MutateTimbreMemoCallback callback) {
    impl_->setMutateTimbreMemoCallback(std::move(callback));
}

void CompositeTimeline::setLfoSessionCallback(LfoSessionCallback callback) {
    impl_->setLfoSessionCallback(std::move(callback));
}

void CompositeTimeline::setLfoPollKeysCallback(LfoPollKeysCallback callback) {
    impl_->setLfoPollKeysCallback(std::move(callback));
}

void CompositeTimeline::setEditorSession(
    EditorSession* session) noexcept {
    impl_->setEditorSession(session);
}

void CompositeTimeline::setPlaybackTempo(int tempo_bpm) {
    impl_->setPlaybackTempo(tempo_bpm);
}

void CompositeTimeline::cancelPendingDialogs() {
    impl_->cancelPendingDialogs();
}

void CompositeTimeline::applySessionAllocationStarts(engine::CompositeTimbre& timbre) const {
    impl_->applySessionAllocationStarts(timbre);
}

void CompositeTimeline::setTimbre(
    const engine::CompositeTimbre& timbre,
    bool reset_scroll_extent) {
    impl_->setTimbre(timbre, reset_scroll_extent);
}

bool CompositeTimeline::setLoopStart(std::size_t layer, int count) {
    return impl_->setLoopStart(layer, count);
}

void CompositeTimeline::clearLoop(std::size_t layer) {
    impl_->clearLoop(layer);
}

void CompositeTimeline::refreshUiScaleFonts() {
    impl_->refreshUiScaleFonts();
}

bool CompositeTimeline::appendScopeFrame(
    const engine::OpllScopeFrame& frame,
    std::uint8_t midi_note) {
    return impl_->appendScopeFrame(frame, midi_note);
}

}  // namespace mgstc::app

void mgstc::app::CompositeTimeline::setInspectorVisible(bool visible) {
    impl_->setInspectorVisible(visible);
}

juce::String mgstc::app::CompositeTimeline::verifyWorkspace(const juce::File& directory) {
    CompositeTimeline timeline;
    return timeline.impl_->verifyWorkspace(directory);
}
