// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <juce_core/juce_core.h>

[[nodiscard]] inline juce::File mgstcApplicationDataDirectory() {
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
