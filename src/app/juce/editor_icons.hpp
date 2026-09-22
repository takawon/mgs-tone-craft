// SPDX-License-Identifier: AGPL-3.0-only
// Toolbar icons live in mgstc::app. library_manager.hpp includes this
// from global scope so its declarations are not pulled into that namespace.
#pragma once

#include <cmath>
#include <memory>

#include <juce_gui_extra/juce_gui_extra.h>

namespace mgstc::app {

enum class EditorIcon {
    Open,
    Save,
    Paste,
    Copy,
    Undo,
    Redo,
    Average,
    Normalize,
    Invert,
    Up,
    Left,
    Right,
    Down,
    Convert,
    Audition,
    Settings,
    AbAudition,
    CancelPreview,
};

inline std::unique_ptr<juce::Drawable> makeEditorIcon(
    EditorIcon icon,
    juce::Colour colour) {
    juce::Path path;
    switch (icon) {
    case EditorIcon::Open:
        path.startNewSubPath(2.0F, 7.0F);
        path.lineTo(9.0F, 7.0F);
        path.lineTo(11.0F, 10.0F);
        path.lineTo(22.0F, 10.0F);
        path.lineTo(19.0F, 20.0F);
        path.lineTo(3.0F, 20.0F);
        path.closeSubPath();
        path.startNewSubPath(3.0F, 7.0F);
        path.lineTo(3.0F, 4.0F);
        path.lineTo(10.0F, 4.0F);
        path.lineTo(12.0F, 7.0F);
        path.lineTo(20.0F, 7.0F);
        path.lineTo(20.0F, 10.0F);
        break;
    case EditorIcon::Save:
        path.addRectangle(4.0F, 3.0F, 16.0F, 18.0F);
        path.addRectangle(7.0F, 4.0F, 9.0F, 6.0F);
        path.addRectangle(7.0F, 14.0F, 10.0F, 7.0F);
        path.startNewSubPath(15.0F, 5.0F);
        path.lineTo(15.0F, 9.0F);
        break;
    case EditorIcon::Paste:
        path.addRoundedRectangle(
            5.0F, 6.0F, 14.0F, 16.0F, 1.5F);
        path.addRoundedRectangle(
            8.0F, 2.0F, 8.0F, 5.0F, 1.5F);
        path.startNewSubPath(9.0F, 11.0F);
        path.lineTo(15.0F, 11.0F);
        path.startNewSubPath(9.0F, 15.0F);
        path.lineTo(15.0F, 15.0F);
        break;
    case EditorIcon::Copy:
        path.addRoundedRectangle(
            7.0F, 7.0F, 13.0F, 14.0F, 1.5F);
        path.addRoundedRectangle(
            3.0F, 3.0F, 13.0F, 14.0F, 1.5F);
        break;
    case EditorIcon::Undo:
    case EditorIcon::Redo: {
        const auto direction =
            icon == EditorIcon::Undo ? 1.0F : -1.0F;
        const auto centre = 12.0F;
        path.startNewSubPath(
            centre - direction * 7.0F, 8.0F);
        path.lineTo(
            centre - direction * 2.0F, 4.0F);
        path.startNewSubPath(
            centre - direction * 7.0F, 8.0F);
        path.lineTo(
            centre - direction * 2.0F, 12.0F);
        path.startNewSubPath(
            centre - direction * 6.0F, 8.0F);
        path.cubicTo(
            centre + direction * 8.0F,
            5.0F,
            centre + direction * 8.0F,
            18.0F,
            centre,
            19.0F);
        break;
    }
    case EditorIcon::Average:
        path.startNewSubPath(2.0F, 15.0F);
        path.cubicTo(
            5.0F, 5.0F, 9.0F, 5.0F, 12.0F, 15.0F);
        path.cubicTo(
            15.0F, 22.0F, 19.0F, 22.0F, 22.0F, 11.0F);
        break;
    case EditorIcon::Normalize:
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(8.0F, 8.0F);
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(16.0F, 8.0F);
        path.startNewSubPath(12.0F, 3.0F);
        path.lineTo(12.0F, 21.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(8.0F, 16.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(16.0F, 16.0F);
        break;
    case EditorIcon::Invert:
        path.startNewSubPath(4.0F, 7.0F);
        path.lineTo(10.0F, 7.0F);
        path.startNewSubPath(7.0F, 4.0F);
        path.lineTo(7.0F, 10.0F);
        path.startNewSubPath(14.0F, 17.0F);
        path.lineTo(20.0F, 17.0F);
        path.startNewSubPath(12.0F, 21.0F);
        path.lineTo(22.0F, 3.0F);
        break;
    case EditorIcon::Convert:
        path.startNewSubPath(3.0F, 8.0F);
        path.lineTo(20.0F, 8.0F);
        path.lineTo(16.0F, 4.0F);
        path.startNewSubPath(21.0F, 16.0F);
        path.lineTo(4.0F, 16.0F);
        path.lineTo(8.0F, 20.0F);
        break;
    case EditorIcon::Audition:
        path.addTriangle(4.0F, 9.0F, 9.0F, 9.0F, 9.0F, 15.0F);
        path.startNewSubPath(9.0F, 9.0F);
        path.lineTo(14.0F, 5.0F);
        path.lineTo(14.0F, 19.0F);
        path.lineTo(9.0F, 15.0F);
        path.startNewSubPath(16.0F, 8.0F);
        path.cubicTo(20.0F, 10.0F, 20.0F, 14.0F, 16.0F, 16.0F);
        path.startNewSubPath(18.0F, 5.0F);
        path.cubicTo(24.0F, 9.0F, 24.0F, 15.0F, 18.0F, 19.0F);
        break;
    case EditorIcon::Settings:
        path.addEllipse(8.0F, 8.0F, 8.0F, 8.0F);
        path.addEllipse(4.0F, 4.0F, 16.0F, 16.0F);
        for (int index = 0; index < 8; ++index) {
            const auto angle = juce::MathConstants<float>::twoPi
                * static_cast<float>(index) / 8.0F;
            path.startNewSubPath(
                12.0F + std::cos(angle) * 8.0F,
                12.0F + std::sin(angle) * 8.0F);
            path.lineTo(
                12.0F + std::cos(angle) * 11.0F,
                12.0F + std::sin(angle) * 11.0F);
        }
        break;
    case EditorIcon::AbAudition:
        // A / B を左右パネルで対比（試聴切替）
        path.addRoundedRectangle(2.0F, 4.0F, 9.0F, 16.0F, 1.5F);
        path.addRoundedRectangle(13.0F, 4.0F, 9.0F, 16.0F, 1.5F);
        path.startNewSubPath(6.5F, 8.0F);
        path.lineTo(4.5F, 16.0F);
        path.startNewSubPath(6.5F, 8.0F);
        path.lineTo(8.5F, 16.0F);
        path.startNewSubPath(5.2F, 13.0F);
        path.lineTo(7.8F, 13.0F);
        path.startNewSubPath(15.5F, 8.0F);
        path.lineTo(15.5F, 16.0F);
        path.startNewSubPath(15.5F, 8.0F);
        path.lineTo(18.5F, 8.0F);
        path.cubicTo(20.2F, 8.0F, 20.2F, 11.0F, 18.5F, 11.0F);
        path.lineTo(15.5F, 11.0F);
        path.startNewSubPath(15.5F, 11.0F);
        path.lineTo(18.8F, 11.0F);
        path.cubicTo(20.5F, 11.0F, 20.5F, 16.0F, 18.5F, 16.0F);
        path.lineTo(15.5F, 16.0F);
        break;
    case EditorIcon::CancelPreview:
        path.startNewSubPath(5.0F, 5.0F);
        path.lineTo(19.0F, 19.0F);
        path.startNewSubPath(19.0F, 5.0F);
        path.lineTo(5.0F, 19.0F);
        path.addEllipse(3.0F, 3.0F, 18.0F, 18.0F);
        break;
    case EditorIcon::Up:
    case EditorIcon::Down:
    case EditorIcon::Left:
    case EditorIcon::Right: {
        juce::Point<float> start;
        juce::Point<float> end;
        juce::Point<float> wing_a;
        juce::Point<float> wing_b;
        if (icon == EditorIcon::Up) {
            start = {12.0F, 21.0F};
            end = {12.0F, 3.0F};
            wing_a = {6.0F, 9.0F};
            wing_b = {18.0F, 9.0F};
        } else if (icon == EditorIcon::Down) {
            start = {12.0F, 3.0F};
            end = {12.0F, 21.0F};
            wing_a = {6.0F, 15.0F};
            wing_b = {18.0F, 15.0F};
        } else if (icon == EditorIcon::Left) {
            start = {21.0F, 12.0F};
            end = {3.0F, 12.0F};
            wing_a = {9.0F, 6.0F};
            wing_b = {9.0F, 18.0F};
        } else {
            start = {3.0F, 12.0F};
            end = {21.0F, 12.0F};
            wing_a = {15.0F, 6.0F};
            wing_b = {15.0F, 18.0F};
        }
        path.startNewSubPath(start);
        path.lineTo(end);
        path.startNewSubPath(end);
        path.lineTo(wing_a);
        path.startNewSubPath(end);
        path.lineTo(wing_b);
        break;
    }
    }

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

}  // namespace mgstc::app
