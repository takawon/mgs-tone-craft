// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_core/juce_core.h>

// Set once on the message thread, before preferences or library initialization.
// The directory remains stable until process exit, including static teardown.
[[nodiscard]] inline juce::File& mgstcDiagnosticDataDirectory() {
    static juce::File directory;
    return directory;
}

[[nodiscard]] inline bool mgstcUseIsolatedApplicationDataDirectory() {
    auto& directory = mgstcDiagnosticDataDirectory();
    if (directory == juce::File{}) {
        directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("MgsToneCraft-diagnostic-" + juce::Uuid().toString());
    }
    return directory.createDirectory().wasOk();
}

[[nodiscard]] inline juce::File mgstcApplicationDataDirectory() {
    if (mgstcDiagnosticDataDirectory() != juce::File{})
        return mgstcDiagnosticDataDirectory();
#if defined(MGSTC_TEST_ISOLATED_PREFERENCES)
    // UI integration tests must never migrate the user's library or modify
    // preferences. One private directory is shared by all test TUs/processors.
    static const auto directory = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(
            "MgsToneCraft-tests-" + juce::Uuid().toString());
    return directory;
#else
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("MgsToneCraft");
#endif
}
