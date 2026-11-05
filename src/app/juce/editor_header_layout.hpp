// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <array>
#include <algorithm>
#include <juce_gui_basics/juce_gui_basics.h>
#include "ui_fonts.hpp"
#include "ui_layout.hpp"

namespace mgstc::app {

[[nodiscard]] inline int editorGraphButtonWidth() {
    return juce::jmax(UiLayout::libraryButtonMinW,
        juce::GlyphArrangement::getStringWidthInt(UiFonts::body(),
            juce::String::fromUTF8("\xE3\x82\xB0\xE3\x83\xA9\xE3\x83\x95"))
            + UiLayout::sm * 2);
}

// Standalone resizing must leave each indivisible header group reachable.
// The host-controlled plugin editor keeps its existing size negotiation.
[[nodiscard]] inline int minimumEditorHeaderWidth() {
    using namespace UiLayout;
    const int graph_output_width = editorGraphButtonWidth() + controlGap + outputSwitchW;
    const int audio_width = iconButton * 2 + masterVolumeSize + controlGap * 2;
    const std::array<juce::String, 3> navigation_labels{
        juce::String::fromUTF8("\xE8\xA4\x87\xE5\x90\x88\xE9\x9F\xB3\xE8\x89\xB2"),
        juce::String::fromUTF8("OPLL\xE9\x9F\xB3\xE8\x89\xB2"),
        juce::String::fromUTF8("SCC\xE9\x9F\xB3\xE8\x89\xB2")};
    int navigation_width = 0;
    for (const auto& label : navigation_labels) {
        navigation_width = juce::jmax(navigation_width,
            juce::GlyphArrangement::getStringWidthInt(UiFonts::body(), label)
                + textButtonH + sm + controlGap * 3);
    }
    return pageMargin * 2 + juce::jmax(graph_output_width,
        juce::jmax(audio_width, navigation_width));
}

struct EditorHeaderLayout {
    juce::Rectangle<int> title, description;
    std::array<juce::Rectangle<int>, 2> navigation;
    juce::Rectangle<int> graph, output, immediate, volume, volume_label, settings;
    int bottom{};
};

// All dimensions use the current UiLayout scale. A graph and its output switch
// stay together; navigation and audio chrome can move onto further rows.
[[nodiscard]] inline EditorHeaderLayout calculateEditorHeaderLayout(
    int host_width, int text_width, int navigation_a_width,
    int navigation_b_width, int graph_width) {
    using namespace UiLayout;
    EditorHeaderLayout result;
    const int left = pageMargin;
    const int right = juce::jmax(left, host_width - pageMargin);
    const int width = right - left;
    const int label_height = juce::roundToInt(UiFonts::body().getHeight());
    const int icon_offset = juce::jmax(0, (masterVolumeSize - iconButton) / 2);
    const int row_height = juce::jmax(masterVolumeSize + label_height,
        icon_offset + textButtonH + label_height);
    const int text_height = titleH + descriptionH;
    const int graph_output_width = graph_width + controlGap + outputSwitchW;
    const int audio_width = iconButton * 2 + masterVolumeSize + controlGap * 2;
    struct Group { int id, width, gap; };
    std::array<Group, 4> groups{};
    int count = 0;
    if (navigation_a_width > 0) groups[count++] = {0, navigation_a_width, 0};
    if (navigation_b_width > 0) groups[count++] = {1, navigation_b_width, controlGap};
    const int graph_gap = count > 0 ? lg : 0;
    groups[count++] = {2, graph_output_width, graph_gap};
    groups[count++] = {3, audio_width, controlGap};
    int action_width = 0;
    for (int i = 0; i < count; ++i)
        action_width += groups[i].width + (i == 0 ? 0 : groups[i].gap);
    const bool shared_row = text_width + panelGap + action_width <= width;
    const int title_width = shared_row ? width - action_width - panelGap : width;
    result.title = {left, pageMargin, title_width, titleH};
    result.description = {left, pageMargin + titleH, title_width, descriptionH};
    int row_top = shared_row ? pageMargin : pageMargin + text_height + sm;
    auto place_group = [&](const Group& group, int x) {
        const int y = row_top + icon_offset;
        if (group.id < 2) {
            result.navigation[static_cast<std::size_t>(group.id)] =
                {x, y, group.width, textButtonH};
        } else if (group.id == 2) {
            result.graph = {x, y, graph_width, textButtonH};
            result.output = {x + graph_width + controlGap, y,
                outputSwitchW, textButtonH + label_height};
        } else {
            result.immediate = {x, y, iconButton, iconButton};
            x += iconButton + controlGap;
            result.volume = {x, row_top, masterVolumeSize, masterVolumeSize};
            result.volume_label = {x, row_top + masterVolumeSize,
                masterVolumeSize, label_height};
            x += masterVolumeSize + controlGap;
            result.settings = {x, y, iconButton, iconButton};
        }
    };
    int first = 0;
    while (first < count) {
        int last = first;
        int used = groups[first].width;
        while (last + 1 < count
            && used + groups[last + 1].gap + groups[last + 1].width <= width) {
            ++last;
            used += groups[last].gap + groups[last].width;
        }
        int x = right - used;
        for (int i = first; i <= last; ++i) {
            if (i != first) x += groups[i].gap;
            place_group(groups[i], x);
            x += groups[i].width;
        }
        result.bottom = juce::jmax(pageMargin + text_height, row_top + row_height);
        first = last + 1;
        row_top += row_height + sm;
    }
    return result;
}

} // namespace mgstc::app
