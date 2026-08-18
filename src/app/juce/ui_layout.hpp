// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_fonts.hpp"
#include "ui_scale.hpp"

namespace UiLayout {
// Unscaled design tokens (100%). Runtime values below are multiplied by UiScale.
namespace Base {
constexpr int xs = 4;
constexpr int sm = 8;
constexpr int md = 12;
constexpr int lg = 16;
constexpr int xl = 24;

constexpr int pageMargin = 24;
constexpr int pageMarginPaint = 20;
constexpr int panelGap = 16;
constexpr int panelRadius = 9;
constexpr int panelPad = 10;
constexpr int rowGap = 8;
constexpr int controlGap = 6;
constexpr int iconButton = 40;
constexpr int masterVolumeSize = 48;
constexpr int switchTrackW = 34;
constexpr int switchTrackH = 18;
constexpr int switchThumb = 14;
constexpr int switchLabelPad = 8; // track right → label
constexpr int textButtonH = 36;
constexpr int fieldH = 34;
constexpr int titleH = 36;
constexpr int descriptionH = 30;
constexpr int keyboardH = 94;
constexpr int keyboardGap = 10;
constexpr int statusH = 40;
constexpr int toolbarH = 44;

constexpr int editorWindowW = 1600;
constexpr int editorWindowH = 1050;
constexpr int libraryWidth = 360;
constexpr int libraryTitleH = 30;
constexpr int libraryMemoH = 80;
constexpr int libraryButtonH = 36;
constexpr int libraryButtonMinW = 80;
constexpr int libraryManageButtonW = 108;
constexpr int compositeToolRowH = 34; // match fieldH; header uses textButtonH
constexpr int commandStackH = 108;
constexpr int commandSummaryH = 36;
constexpr int envelopePreviewH = 48; // 2-line selectable MGSC preview
constexpr int editSubLaneLabelW = 44;
constexpr int setupValueLabelW = 64; // relative pitch etc.
constexpr int setupEnvelopeNumberW = 56; // @e 00–31
constexpr int setupPitchSweepW = 80; // p 0–255
constexpr int setupSustainComboW = 72; // so / sf
constexpr int setupSourceLabelW = 64; // PSG / SCC / OPLL (heading)
constexpr int setupChannelComboW = 72; // Ch.n
constexpr int setupNumberModeW = 72; // 自動 / 手動
constexpr int setupSliderTextW = 40;
constexpr int editMarkerLaneH = 16; // @ / y: no vertical value extent
constexpr int registerAutoLaneH = 44;
constexpr int compositeMixLaneH = 104;
constexpr int compositeEditLaneH = 520;
constexpr int registerAutoLanePairExtra = 44 * 2;
constexpr int compositeSetupColumnW = 420;
constexpr int compositeAddPsgW = 70;
constexpr int compositeAddSccW = 70;
constexpr int compositeAddOpllW = 76;
constexpr int compositeRemoveLayerW = 92;
constexpr int compositeTempoLabelW = 44;
constexpr int compositeTempoFieldW = 56;
constexpr int compositeOpenSccW = 88;
constexpr int compositeOpenOpllW = 92;
constexpr int compositeStopW = 80;
constexpr int compositeDockApplyW = 48;
constexpr int compositeDockFieldW = 48;
constexpr int compositeDockLabelW = 24;
constexpr int compositeDockCountW = 56;
constexpr int compositeDockParamW = 108;
constexpr int compositeDockAutoValueW = 40;
constexpr int compositeDockAutoLabelW = 32;
constexpr int compositeDockClearLoopW = 84;
constexpr int compositeDockLengthLabelW = 32;
constexpr int countColumnMinW = 28;
constexpr int countColumnMaxW = 72;
constexpr int compositeTimbrePickW = 360;
constexpr int compositeTimbrePickH = 560;
constexpr int editVolumeLaneShare = 1;
constexpr int editPitchLaneShare = 3;
constexpr int editVolumeLaneH = 265; // 100%; quiet steps round to ≥1px
constexpr int compositeOpllYParamW = 900;
constexpr int compositeOpllYParamH = 680;
constexpr int compositeLfoDialogW = 500;
constexpr int compositeLfoDialogH = 680;
constexpr int compositeLfoWaveH = 140;
constexpr int settingsDialogW = 500;
constexpr int settingsDialogH = 720;
constexpr int libraryManagerW = 1100;
constexpr int libraryManagerH = 700;
constexpr int editLaneFramePadV = 8; // sm: gap between channel frames
constexpr int editLaneInnerPadV = 8; // sm: plot inset inside a frame
} // namespace Base

inline int xs = Base::xs;
inline int sm = Base::sm;
inline int md = Base::md;
inline int lg = Base::lg;
inline int xl = Base::xl;

inline int pageMargin = Base::pageMargin;
inline int pageMarginPaint = Base::pageMarginPaint;
inline int panelGap = Base::panelGap;
inline int panelRadius = Base::panelRadius;
inline int panelPad = Base::panelPad;
inline int rowGap = Base::rowGap;
inline int controlGap = Base::controlGap;
inline int iconButton = Base::iconButton;
inline int masterVolumeSize = Base::masterVolumeSize;
inline int switchTrackW = Base::switchTrackW;
inline int switchTrackH = Base::switchTrackH;
inline int switchThumb = Base::switchThumb;
inline int switchLabelPad = Base::switchLabelPad;
inline int textButtonH = Base::textButtonH;
inline int fieldH = Base::fieldH;
inline int titleH = Base::titleH;
inline int descriptionH = Base::descriptionH;
inline int keyboardH = Base::keyboardH;
inline int keyboardGap = Base::keyboardGap;
inline int statusH = Base::statusH;
inline int toolbarH = Base::toolbarH;

inline int editorWindowW = Base::editorWindowW;
inline int editorWindowH = Base::editorWindowH;
inline int libraryWidth = Base::libraryWidth;
inline int libraryTitleH = Base::libraryTitleH;
inline int libraryMemoH = Base::libraryMemoH;
inline int libraryButtonH = Base::libraryButtonH;
inline int libraryButtonMinW = Base::libraryButtonMinW;
inline int libraryManageButtonW = Base::libraryManageButtonW;
inline int compositeToolRowH = Base::compositeToolRowH;
inline int commandStackH = Base::commandStackH;
inline int commandSummaryH = Base::commandSummaryH;
inline int envelopePreviewH = Base::envelopePreviewH;
inline int editSubLaneLabelW = Base::editSubLaneLabelW;
inline int setupValueLabelW = Base::setupValueLabelW;
inline int setupEnvelopeNumberW = Base::setupEnvelopeNumberW;
inline int setupPitchSweepW = Base::setupPitchSweepW;
inline int setupSustainComboW = Base::setupSustainComboW;
inline int setupSourceLabelW = Base::setupSourceLabelW;
inline int setupChannelComboW = Base::setupChannelComboW;
inline int setupNumberModeW = Base::setupNumberModeW;
inline int setupSliderTextW = Base::setupSliderTextW;
inline int editMarkerLaneH = Base::editMarkerLaneH;
inline int registerAutoLaneH = Base::registerAutoLaneH;
inline int compositeSetupContentH = 0;
inline int compositeChannelLaneH = 0;
inline int compositeHeaderControlsW = 0;
inline int compositeMixLaneH = Base::compositeMixLaneH;
inline int compositeEditLaneH = Base::compositeEditLaneH;
inline int compositeEditLaneOpllExtraH = Base::registerAutoLanePairExtra;
inline int compositeSetupColumnW = Base::compositeSetupColumnW;
inline int compositeLaneLabelW = Base::compositeSetupColumnW;
inline int compositeAddPsgW = Base::compositeAddPsgW;
inline int compositeAddSccW = Base::compositeAddSccW;
inline int compositeAddOpllW = Base::compositeAddOpllW;
inline int compositeRemoveLayerW = Base::compositeRemoveLayerW;
inline int compositeTempoLabelW = Base::compositeTempoLabelW;
inline int compositeTempoFieldW = Base::compositeTempoFieldW;
inline int compositeOpenSccW = Base::compositeOpenSccW;
inline int compositeOpenOpllW = Base::compositeOpenOpllW;
inline int compositeStopW = Base::compositeStopW;
inline int compositeDockApplyW = Base::compositeDockApplyW;
inline int compositeDockFieldW = Base::compositeDockFieldW;
inline int compositeDockLabelW = Base::compositeDockLabelW;
inline int compositeDockCountW = Base::compositeDockCountW;
inline int compositeDockParamW = Base::compositeDockParamW;
inline int compositeDockAutoValueW = Base::compositeDockAutoValueW;
inline int compositeDockAutoLabelW = Base::compositeDockAutoLabelW;
inline int compositeDockClearLoopW = Base::compositeDockClearLoopW;
inline int compositeDockLengthLabelW = Base::compositeDockLengthLabelW;
inline int countColumnMinW = Base::countColumnMinW;
inline int countColumnMaxW = Base::countColumnMaxW;
inline int compositeTimbrePickW = Base::compositeTimbrePickW;
inline int compositeTimbrePickH = Base::compositeTimbrePickH;
inline int editVolumeLaneShare = Base::editVolumeLaneShare;
inline int editPitchLaneShare = Base::editPitchLaneShare;
inline int editVolumeLaneH = Base::editVolumeLaneH; // same for PSG/SCC/OPLL
inline int editPitchLaneMinH = 0;
inline int editLaneFramePadV = Base::editLaneFramePadV;
inline int editLaneInnerPadV = Base::editLaneInnerPadV;
inline int compositeOpllYParamW = Base::compositeOpllYParamW;
inline int compositeOpllYParamH = Base::compositeOpllYParamH;
inline int compositeLfoDialogW = Base::compositeLfoDialogW;
inline int compositeLfoDialogH = Base::compositeLfoDialogH;
inline int compositeLfoWaveH = Base::compositeLfoWaveH;
inline int compositeLayerLibraryH = 0;
inline int settingsDialogW = Base::settingsDialogW;
inline int settingsDialogH = Base::settingsDialogH;
inline int libraryManagerW = Base::libraryManagerW;
inline int libraryManagerH = Base::libraryManagerH;

constexpr juce::uint32 panelFill = 0xFF29323C;
constexpr juce::uint32 panelStroke = 0xFF435160;
constexpr juce::uint32 pageFill = 0xFF20262E;
constexpr juce::uint32 pageFillBottom = 0xFF161B22;

inline void recomputeDerived() {
    // Setup column matches layoutLibraryBrowserChrome: panelPad inset,
    // fieldH rows, sm between rows (title + 6 control rows).
    compositeSetupContentH =
        panelPad * 2
        + fieldH * 7
        + sm * 6;
    compositeChannelLaneH =
        compositeSetupContentH + editLaneFramePadV * 2;
    compositeHeaderControlsW =
        compositeTempoFieldW
        + compositeTempoLabelW
        + sm
        + compositeAddOpllW
        + controlGap
        + compositeAddSccW
        + controlGap
        + compositeAddPsgW
        + sm
        + compositeRemoveLayerW;
    compositeLaneLabelW = compositeSetupColumnW;
    compositeEditLaneOpllExtraH = registerAutoLaneH * 2;
    compositeLayerLibraryH =
        panelPad * 2 + libraryTitleH + sm + fieldH * 2 + sm + fieldH;
    // Volume is the scaled 100% token `editVolumeLaneH` (265px). Pitch keeps
    // the 0.231 remainder (share 3/4 of that pre-growth value area).
    // `compositeEditLaneH` still holds the scaled 0.231 fallback here.
    const int legacy_value_h = juce::jmax(
        1,
        compositeEditLaneH
            - editLaneFramePadV * 2
            - editLaneInnerPadV * 2
            - envelopePreviewH
            - commandSummaryH
            - editMarkerLaneH * 2);
    editPitchLaneMinH = juce::jmax(
        1,
        (legacy_value_h * editPitchLaneShare)
            / juce::jmax(1, editVolumeLaneShare + editPitchLaneShare));
    compositeEditLaneH =
        editLaneFramePadV * 2
        + editLaneInnerPadV * 2
        + envelopePreviewH
        + commandSummaryH
        + editMarkerLaneH * 2
        + editVolumeLaneH
        + editPitchLaneMinH;
}

inline void applyScale(float factor) {
    auto s = [factor](int value) {
        return juce::jmax(
            1, juce::roundToInt(static_cast<float>(value) * factor));
    };
    xs = s(Base::xs);
    sm = s(Base::sm);
    md = s(Base::md);
    lg = s(Base::lg);
    xl = s(Base::xl);
    pageMargin = s(Base::pageMargin);
    pageMarginPaint = s(Base::pageMarginPaint);
    panelGap = s(Base::panelGap);
    panelRadius = s(Base::panelRadius);
    panelPad = s(Base::panelPad);
    rowGap = s(Base::rowGap);
    controlGap = s(Base::controlGap);
    iconButton = s(Base::iconButton);
    masterVolumeSize = s(Base::masterVolumeSize);
    switchTrackW = s(Base::switchTrackW);
    switchTrackH = s(Base::switchTrackH);
    switchThumb = s(Base::switchThumb);
    switchLabelPad = s(Base::switchLabelPad);
    textButtonH = s(Base::textButtonH);
    fieldH = s(Base::fieldH);
    titleH = s(Base::titleH);
    descriptionH = s(Base::descriptionH);
    keyboardH = s(Base::keyboardH);
    keyboardGap = s(Base::keyboardGap);
    statusH = s(Base::statusH);
    toolbarH = s(Base::toolbarH);
    editorWindowW = s(Base::editorWindowW);
    editorWindowH = s(Base::editorWindowH);
    libraryWidth = s(Base::libraryWidth);
    libraryTitleH = s(Base::libraryTitleH);
    libraryMemoH = s(Base::libraryMemoH);
    libraryButtonH = s(Base::libraryButtonH);
    libraryButtonMinW = s(Base::libraryButtonMinW);
    libraryManageButtonW = s(Base::libraryManageButtonW);
    compositeToolRowH = s(Base::compositeToolRowH);
    commandStackH = s(Base::commandStackH);
    commandSummaryH = s(Base::commandSummaryH);
    envelopePreviewH = s(Base::envelopePreviewH);
    editSubLaneLabelW = s(Base::editSubLaneLabelW);
    setupValueLabelW = s(Base::setupValueLabelW);
    setupEnvelopeNumberW = s(Base::setupEnvelopeNumberW);
    setupPitchSweepW = s(Base::setupPitchSweepW);
    setupSustainComboW = s(Base::setupSustainComboW);
    setupSourceLabelW = s(Base::setupSourceLabelW);
    setupChannelComboW = s(Base::setupChannelComboW);
    setupNumberModeW = s(Base::setupNumberModeW);
    setupSliderTextW = s(Base::setupSliderTextW);
    editMarkerLaneH = s(Base::editMarkerLaneH);
    registerAutoLaneH = s(Base::registerAutoLaneH);
    compositeMixLaneH = s(Base::compositeMixLaneH);
    compositeEditLaneH = s(Base::compositeEditLaneH);
    editLaneFramePadV = s(Base::editLaneFramePadV);
    editLaneInnerPadV = s(Base::editLaneInnerPadV);
    compositeSetupColumnW = s(Base::compositeSetupColumnW);
    compositeAddPsgW = s(Base::compositeAddPsgW);
    compositeAddSccW = s(Base::compositeAddSccW);
    compositeAddOpllW = s(Base::compositeAddOpllW);
    compositeRemoveLayerW = s(Base::compositeRemoveLayerW);
    compositeTempoLabelW = s(Base::compositeTempoLabelW);
    compositeTempoFieldW = s(Base::compositeTempoFieldW);
    compositeOpenSccW = s(Base::compositeOpenSccW);
    compositeOpenOpllW = s(Base::compositeOpenOpllW);
    compositeStopW = s(Base::compositeStopW);
    compositeDockApplyW = s(Base::compositeDockApplyW);
    compositeDockFieldW = s(Base::compositeDockFieldW);
    compositeDockLabelW = s(Base::compositeDockLabelW);
    compositeDockCountW = s(Base::compositeDockCountW);
    compositeDockParamW = s(Base::compositeDockParamW);
    compositeDockAutoValueW = s(Base::compositeDockAutoValueW);
    compositeDockAutoLabelW = s(Base::compositeDockAutoLabelW);
    compositeDockClearLoopW = s(Base::compositeDockClearLoopW);
    compositeDockLengthLabelW = s(Base::compositeDockLengthLabelW);
    countColumnMinW = s(Base::countColumnMinW);
    countColumnMaxW = s(Base::countColumnMaxW);
    compositeTimbrePickW = s(Base::compositeTimbrePickW);
    compositeTimbrePickH = s(Base::compositeTimbrePickH);
    editVolumeLaneH = s(Base::editVolumeLaneH);
    compositeOpllYParamW = s(Base::compositeOpllYParamW);
    compositeOpllYParamH = s(Base::compositeOpllYParamH);
    compositeLfoDialogW = s(Base::compositeLfoDialogW);
    compositeLfoDialogH = s(Base::compositeLfoDialogH);
    compositeLfoWaveH = s(Base::compositeLfoWaveH);
    settingsDialogW = s(Base::settingsDialogW);
    settingsDialogH = s(Base::settingsDialogH);
    libraryManagerW = s(Base::libraryManagerW);
    libraryManagerH = s(Base::libraryManagerH);
    recomputeDerived();
}

// Preferred width for a SwitchLookAndFeel toggle: track + pad + label + trailing xs.
[[nodiscard]] inline int switchControlWidth(
    const juce::String& label,
    int control_height = -1) {
    if (control_height < 0) {
        control_height = textButtonH;
    }
    const auto font = UiFonts::make(
        UiFonts::controlTextHeight(
            static_cast<float>(control_height)));
    const int text_w =
        juce::GlyphArrangement::getStringWidthInt(font, label);
    return switchTrackW + switchLabelPad + text_w + xs;
}

} // namespace UiLayout
