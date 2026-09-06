// SPDX-License-Identifier: AGPL-3.0-only

#include "../../src/app/juce/spectrogram_emphasis.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr std::size_t kFftSize = 2048;
constexpr float kMinimumDb = -100.0F;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

using Spectrum = std::array<float, kFftSize / 2 + 1>;

void addPeak(Spectrum& spectrum, double bin, float power) {
    const auto centre = static_cast<std::size_t>(std::floor(bin));
    const double fraction = bin - static_cast<double>(centre);
    spectrum[centre - 1] = power * static_cast<float>(0.08 * (1.0 - fraction));
    spectrum[centre] = power * static_cast<float>(1.0 - 0.35 * fraction);
    spectrum[centre + 1] = power * static_cast<float>(0.35 + 0.65 * fraction);
    spectrum[centre + 2] = power * static_cast<float>(0.08 * fraction);
}

[[nodiscard]] double binForFrequency(double frequency) {
    return frequency * static_cast<double>(kFftSize) / kSampleRate;
}

[[nodiscard]] mgstc::app::spectrogram::HarmonicPeakList analyze(
    const Spectrum& spectrum,
    double fundamental) {
    return mgstc::app::spectrogram::findHarmonicPeaks(
        spectrum,
        fundamental,
        kSampleRate,
        kFftSize,
        1.0F,
        kMinimumDb);
}

void qifftFindsSubBinPeak() {
    Spectrum spectrum{};
    constexpr double wanted_bin = 20.25;
    for (int offset = -1; offset <= 1; ++offset) {
        const double distance = static_cast<double>(20 + offset) - wanted_bin;
        spectrum[static_cast<std::size_t>(20 + offset)] = static_cast<float>(
            1.0e8 * std::exp(-0.5 * distance * distance / 0.36));
    }
    const auto peak = mgstc::app::spectrogram::interpolatePeak(spectrum, 20);
    require(std::abs(peak.bin - wanted_bin) < 0.001,
            "QIFFT must recover a finite sub-bin peak position");
    require(peak.power > spectrum[20],
            "QIFFT must estimate the interpolated peak level");
}

void pureSineDoesNotCreateHarmonics() {
    Spectrum spectrum{};
    constexpr double fundamental = 440.0;
    addPeak(spectrum, binForFrequency(fundamental), 1.0e8F);
    const auto peaks = analyze(spectrum, fundamental);
    require(peaks.count == 1, "a sine must produce only its real fundamental");
    require(peaks.peaks[0].harmonic == 1,
            "the sine peak must be classified as the fundamental");
}

void squareWaveMatchesOnlyPresentOddHarmonics() {
    Spectrum spectrum{};
    constexpr double fundamental = 440.0;
    for (const int harmonic : {1, 3, 5, 7}) {
        addPeak(
            spectrum,
            binForFrequency(fundamental * harmonic),
            1.0e8F / static_cast<float>(harmonic * harmonic));
    }
    const auto peaks = analyze(spectrum, fundamental);
    require(peaks.count == 4,
            "only the four present square-wave peaks must be matched");
    for (std::size_t index = 0; index < peaks.count; ++index) {
        require((peaks.peaks[index].harmonic & 1U) != 0,
                "an absent even harmonic must not be generated");
    }
}

void inharmonicAndNoiseRemainUnclassified() {
    Spectrum spectrum{};
    spectrum.fill(2.0F);
    constexpr double fundamental = 440.0;
    addPeak(spectrum, binForFrequency(fundamental), 1.0e8F);
    addPeak(spectrum, binForFrequency(731.0), 8.0e7F);
    const auto peaks = analyze(spectrum, fundamental);
    require(peaks.count == 1,
            "an inharmonic sideband and broadband floor must stay unmatched");
    require(peaks.peaks[0].harmonic == 1,
            "only the real fundamental should be classified");
}

void invalidOrChangedNoteUsesOnlyThatColumnsF0() {
    Spectrum spectrum{};
    addPeak(spectrum, binForFrequency(880.0), 1.0e8F);
    require(analyze(spectrum, 0.0).count == 0,
            "an inactive or invalid note must disable matching");
    require(analyze(spectrum, 440.0).count == 1,
            "the same real peak may be harmonic two for the old column F0");
    const auto changed = analyze(spectrum, 880.0);
    require(changed.count == 1 && changed.peaks[0].harmonic == 1,
            "a new column must classify against its new F0 only");
}

void ridgeCompositionPreservesUnmatchedPixels() {
    using mgstc::app::spectrogram::composeHarmonicRidgeLevel;
    for (int level = 0; level <= 255; ++level) {
        require(composeHarmonicRidgeLevel(
                    static_cast<std::uint8_t>(level), 180, 0.0, 5.0, 0.0)
                    == level,
                "zero percent must preserve every original level");
    }
    require(composeHarmonicRidgeLevel(120, 180, 0.0, 5.0, 1.0) > 180,
            "maximum emphasis must brighten the detected peak centre");
    require(composeHarmonicRidgeLevel(120, 180, 3.0, 5.0, 1.0) < 120,
            "the detected harmonic lobe shoulder should be lightly reduced");
    require(composeHarmonicRidgeLevel(120, 180, 6.0, 5.0, 1.0) == 120,
            "unmatched pixels outside a detected harmonic lobe stay unchanged");
}

}  // namespace

int main() {
    try {
        qifftFindsSubBinPeak();
        pureSineDoesNotCreateHarmonics();
        squareWaveMatchesOnlyPresentOddHarmonics();
        inharmonicAndNoiseRemainUnclassified();
        invalidOrChangedNoteUsesOnlyThatColumnsF0();
        ridgeCompositionPreservesUnmatchedPixels();
        std::cout << "spectrogram harmonic emphasis tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "spectrogram harmonic emphasis test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
