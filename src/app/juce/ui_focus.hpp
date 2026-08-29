// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

[[nodiscard]] inline bool textEntryHasFocusWithin(
    const juce::Component& root) {
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if (!focused
        || (focused != &root && !root.isParentOf(focused))) {
        return false;
    }
    for (auto* component = focused;
         component != nullptr;
         component = component->getParentComponent()) {
        if (const auto* editor =
                dynamic_cast<const juce::TextEditor*>(component);
            editor != nullptr && !editor->isReadOnly()) {
            // 読み取り専用のMGSCプレビューは入力欄とみなさない。
            return true;
        }
        if (const auto* label =
                dynamic_cast<juce::Label*>(component);
            label && label->isBeingEdited()) {
            return true;
        }
        if (component == &root) {
            break;
        }
    }
    return false;
}
