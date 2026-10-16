// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <juce_core/juce_core.h>

#ifndef MGSTC_DOC_VERSION
#define MGSTC_DOC_VERSION "0.0"
#endif

namespace mgstc::app {

inline juce::String editorWindowTitle(juce::String title, bool include_version = false) {
    if (include_version) title += " " MGSTC_DOC_VERSION;
#if JUCE_DEBUG
    title += " (Debug Build)";
#endif
    return title;
}

} // namespace mgstc::app
