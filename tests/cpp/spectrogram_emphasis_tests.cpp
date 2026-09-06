// SPDX-License-Identifier: AGPL-3.0-only

#include "../../src/app/juce/spectrogram_emphasis.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void zeroPercentIsIdentity() {
    for (int level = 0; level <= 255; ++level) {
        for (int average = 0; average <= 255; ++average) {
            require(
                mgstc::app::spectrogram::emphasizeLevel(
                    static_cast<std::uint8_t>(level),
                    static_cast<std::uint8_t>(average),
                    0.0)
                    == level,
                "zero percent must preserve every input level");
        }
    }
}

void localPeaksRemainVisible() {
    using mgstc::app::spectrogram::emphasizeLevel;
    for (int average = 0; average <= 255; ++average) {
        for (int level = average; level <= 255; ++level) {
            require(
                emphasizeLevel(
                    static_cast<std::uint8_t>(level),
                    static_cast<std::uint8_t>(average),
                    1.0)
                    >= level,
                "maximum emphasis must not darken a local peak");
        }
    }
    require(emphasizeLevel(128, 64, 1.0) > 128,
            "a spectral ridge must become brighter");
    require(emphasizeLevel(96, 96, 1.0) == 96,
            "a flat spectral region must remain unchanged");
}

void valleysAreReducedWithoutMovingThePeak() {
    using mgstc::app::spectrogram::emphasizeLevel;
    const auto valley = emphasizeLevel(96, 160, 1.0);
    require(valley < 96 && valley > 0,
            "a non-black valley should be reduced without a hard cutoff");
    require(emphasizeLevel(0, 0, 1.0) == 0,
            "black must remain black");
    require(emphasizeLevel(255, 32, 1.0) == 255,
            "full-scale peaks must remain full scale");
}

}  // namespace

int main() {
    try {
        zeroPercentIsIdentity();
        localPeaksRemainVisible();
        valleysAreReducedWithoutMovingThePeak();
        std::cout << "spectrogram emphasis tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "spectrogram emphasis test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
