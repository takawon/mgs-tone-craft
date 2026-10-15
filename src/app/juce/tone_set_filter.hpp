// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "mgstc/engine/mgstc_tone_set.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "tone_library_session.hpp"

namespace mgstc::app {

// Sets shown by the editor combo. listed_set_ids matches combo item ids
// from 3 upward, in the same order as the last fill.
struct EditorSetFilterState {
    std::vector<engine::ToneSetRecord> sets;
    std::vector<engine::ToneSetMemberRecord> members;
    std::vector<std::uint64_t> listed_set_ids;
};

inline void configureEditorSetFilter(
    juce::ComboBox& combo,
    std::function<void()> on_change) {
    combo.setTooltip(juce::String::fromUTF8(
        "一覧をセットで絞ります。セット名を選んで新規保存または別名保存すると、"
        "そのセットの末尾へ所属を追加します。上書き保存は所属を変えません。"));
    combo.addItem(juce::String::fromUTF8("すべて"), 1);
    combo.addItem(juce::String::fromUTF8("未所属"), 2);
    combo.setSelectedId(1, juce::dontSendNotification);
    combo.onChange = std::move(on_change);
}

inline juce::String setAssignmentFailedMessage() {
    return juce::String::fromUTF8(
        "音色は保存しましたが、選んだセットへ所属を追加できませんでした");
}

// Rebuild items from state.sets. Keeps すべて, 未所属, or the same set id.
inline void fillEditorSetFilter(
    juce::ComboBox& combo,
    EditorSetFilterState& state) {
    std::uint64_t keep_set = 0;
    const int previous = combo.getSelectedId();
    if (previous >= 3) {
        const auto index = static_cast<std::size_t>(previous - 3);
        if (index < state.listed_set_ids.size()) {
            keep_set = state.listed_set_ids[index];
        }
    }
    auto ordered = state.sets;
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const engine::ToneSetRecord& left,
           const engine::ToneSetRecord& right) {
            return left.name < right.name;
        });
    state.listed_set_ids.clear();
    combo.clear(juce::dontSendNotification);
    combo.addItem(juce::String::fromUTF8("すべて"), 1);
    combo.addItem(juce::String::fromUTF8("未所属"), 2);
    int item_id = 3;
    int restore = previous == 2 ? 2 : 1;
    for (const auto& set : ordered) {
        state.listed_set_ids.push_back(set.id);
        combo.addItem(juce::String::fromUTF8(set.name.c_str()), item_id);
        if (set.id == keep_set) {
            restore = item_id;
        }
        ++item_id;
    }
    combo.setSelectedId(restore, juce::dontSendNotification);
}

[[nodiscard]] inline engine::ToneSetListFilter editorSetListFilter(
    const juce::ComboBox& combo) {
    if (combo.getSelectedId() == 2) {
        return engine::ToneSetListFilter::Unassigned;
    }
    if (combo.getSelectedId() >= 3) {
        return engine::ToneSetListFilter::NamedSet;
    }
    return engine::ToneSetListFilter::All;
}

[[nodiscard]] inline std::uint64_t selectedEditorSetId(
    const juce::ComboBox& combo,
    const EditorSetFilterState& state) {
    const int selected = combo.getSelectedId();
    if (selected < 3) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(selected - 3);
    return index < state.listed_set_ids.size()
        ? state.listed_set_ids[index]
        : 0;
}

// True when no named set is selected, or the new id was appended.
// Reloads sets inside the caller's library lock. A failed append does not
// roll back a timbre that was already written.
inline bool assignNewToneToSelectedSet(
    juce::ComboBox& combo,
    EditorSetFilterState& state,
    const std::uint64_t timbre_id,
    const std::uint64_t composite_id) {
    const auto set_id = selectedEditorSetId(combo, state);
    if (set_id == 0) {
        return true;
    }
    auto live = loadToneLibraries(nullptr);
    if (!live) {
        return false;
    }
    const bool exists = std::any_of(
        live->sets.begin(),
        live->sets.end(),
        [&](const engine::ToneSetRecord& set) {
            return set.id == set_id;
        });
    if (!exists) {
        state.sets = std::move(live->sets);
        state.members = std::move(live->members);
        fillEditorSetFilter(combo, state);
        return false;
    }
    const auto appended = engine::appendToneSetMember(
        live->members, set_id, timbre_id, composite_id);
    if (appended == engine::ToneSetMembershipAppend::Rejected) {
        return false;
    }
    if (appended == engine::ToneSetMembershipAppend::Added
        && !persistToneSets(live->sets, live->members)) {
        return false;
    }
    state.sets = std::move(live->sets);
    state.members = std::move(live->members);
    fillEditorSetFilter(combo, state);
    return true;
}

inline bool takeTimbreLibraryAndSets(
    engine::TimbreLibrary& library,
    EditorSetFilterState& state,
    juce::ComboBox& combo,
    std::string* error) {
    auto loaded = loadToneLibraries(error);
    if (!loaded) {
        return false;
    }
    library = std::move(loaded->timbres);
    state.sets = std::move(loaded->sets);
    state.members = std::move(loaded->members);
    fillEditorSetFilter(combo, state);
    return true;
}

}  // namespace mgstc::app
