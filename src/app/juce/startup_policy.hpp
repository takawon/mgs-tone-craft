// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <juce_core/juce_core.h>

namespace mgstc::app {

// Diagnostic windows exercise the real UI, with all device effects disabled.
// Application preferences and the tone library use a private temporary folder.
struct StartupPolicy {
    bool isolated = false;
    bool verify_native_window = false;

    [[nodiscard]] static StartupPolicy fromArguments(const juce::ArgumentList& arguments) {
        const bool verify = arguments.containsOption("--verify-startup");
        return {verify || arguments.containsOption("--verify-composite-workspace")
            || arguments.containsOption("--verify-window-routing")
            || arguments.getValueForOption("--capture-ui").isNotEmpty(), verify};
    }
};

} // namespace mgstc::app
