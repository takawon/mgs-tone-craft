// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui_layout.hpp"
#include "ui_scale.hpp"

struct LibraryBrowserChromeWidgets {
    juce::TextEditor& filter;
    juce::TextButton& tag_filter;
    juce::TextButton& tag_manage;
    juce::ToggleButton& favorite_only;
    juce::TextButton& ab_audition;
    juce::ComboBox& sort;
    juce::ComboBox& list;
    juce::TextButton& library_load;
    juce::TextButton& library_delete;
    juce::TextEditor& name;
    juce::TextButton& library_rename;
    juce::TextButton& tags;
    juce::TextEditor* memo = nullptr;
};

inline void layoutLibraryBrowserChrome(
    juce::Rectangle<int>& area,
    LibraryBrowserChromeWidgets widgets) {
    using namespace UiLayout;
    widgets.filter.setBounds(area.removeFromTop(fieldH));
    area.removeFromTop(sm);
    auto tag_row = area.removeFromTop(fieldH);
    widgets.tag_manage.setBounds(
        tag_row.removeFromRight(libraryManageButtonW));
    tag_row.removeFromRight(controlGap);
    widgets.tag_filter.setBounds(tag_row);
    area.removeFromTop(sm);
    auto filter_options = area.removeFromTop(fieldH);
    widgets.favorite_only.setBounds(
        filter_options.removeFromLeft(UiScale::sx(100)));
    filter_options.removeFromLeft(controlGap);
    widgets.ab_audition.setBounds(
        filter_options.removeFromLeft(UiScale::sx(48)));
    filter_options.removeFromLeft(controlGap);
    widgets.sort.setBounds(filter_options);
    area.removeFromTop(sm);

    auto list_row = area.removeFromTop(fieldH + xs);
    if (widgets.library_delete.isVisible()) {
        widgets.library_delete.setBounds(
            list_row.removeFromRight(UiScale::sx(54)));
        list_row.removeFromRight(controlGap);
    }
    widgets.library_load.setBounds(
        list_row.removeFromRight(UiScale::sx(54)));
    list_row.removeFromRight(controlGap);
    widgets.list.setBounds(list_row);
    area.removeFromTop(sm);

    auto name_row = area.removeFromTop(fieldH);
    widgets.library_rename.setBounds(
        name_row.removeFromRight(UiScale::sx(88)));
    name_row.removeFromRight(controlGap);
    widgets.name.setBounds(name_row);
    area.removeFromTop(sm);
    widgets.tags.setBounds(area.removeFromTop(fieldH));
    if (widgets.memo != nullptr) {
        area.removeFromTop(sm);
        widgets.memo->setBounds(area.removeFromTop(libraryMemoH));
    }
}
