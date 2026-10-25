// SPDX-License-Identifier: AGPL-3.0-only
#include "../../src/app/scc_numeric_wave.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace mgstc::app;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

SccNumericWave parse(std::string_view text, int resolution = 256,
    bool allow_shared = false) {
    auto result = parseSccNumericWave(text, resolution, allow_shared);
    require(result.value.has_value() && result.error == SccNumericError::None,
        "valid numeric wave rejected");
    return *result.value;
}

void testExpansion() {
    const auto low = parse("0", 16).waveform();
    const auto high = parse("15", 16).waveform();
    for (std::size_t i = 0; i < low.size(); ++i) {
        require(low[i] == -128, "zero must be bottom at all 32 samples");
        require(high[i] == 127, "maximum must be top at all 32 samples");
    }
    const auto half = parse("0 15", 16).waveform();
    const auto quarter = parse("0 15 15 15", 16).waveform();
    const auto alternating = parse("0 15 0 15", 16).waveform();
    for (std::size_t i = 0; i < half.size(); ++i) {
        require(half[i] == (i < 16 ? -128 : 127), "half hold distribution");
        require(quarter[i] == (i < 8 ? -128 : 127), "quarter hold distribution");
        require(alternating[i] == ((i / 8) % 2 == 0 ? -128 : 127),
            "alternating hold distribution");
    }
    for (const auto count : {3, 5, 17, 32}) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            if (!text.empty()) text += ' ';
            text += std::to_string(i % 16);
        }
        const auto waveform = parse(text, 16).waveform();
        for (std::size_t i = 0; i < waveform.size(); ++i) {
            const auto input_index = static_cast<int>(i) * count / 32;
            require(waveform[i] == 17 * (input_index % 16) - 128,
                "uneven count must use floor(i * N / 32), with no interpolation");
        }
    }
    std::string text;
    for (int i = 0; i < 32; ++i) {
        if (!text.empty()) text += ' ';
        text += std::to_string(i * 8 + 7);
    }
    const auto identity = parse(text).waveform();
    for (std::size_t i = 0; i < identity.size(); ++i)
        require(identity[i] == static_cast<int>(i) * 8 + 7 - 128,
            "256-resolution 32-value mapping must be exact");
}

void testResolutionAndRounding() {
    const auto source = parse("0 8 15", 16);
    const auto expanded = source.requantized(256);
    require(expanded.text() == "0 136 255" && expanded.resolution == 256,
        "16 to 256 scaling");
    require(expanded.waveform() == source.waveform(), "exact expansion changed wave");
    require(expanded.requantized(16) == source, "16 to 256 round trip");
    require(parse("0 128 255").requantized(16).text() == "0 8 15",
        "256 to 16 quantization");

    for (const auto resolution : kSccNumericResolutions) {
        for (int value = 0; value < resolution; ++value) {
            const auto input = parse(std::to_string(value), resolution);
            const auto sample = static_cast<int>(std::round(
                255.0 * value / (resolution - 1))) - 128;
            for (const auto actual : input.waveform())
                require(actual == sample, "numeric conversion rounding");
            for (const auto target : kSccNumericResolutions) {
                const auto converted = input.requantized(target);
                const auto expected = static_cast<int>(std::round(
                    static_cast<double>(value) * (target - 1) / (resolution - 1)));
                require(converted.values.size() == 1 && converted.values[0] == expected,
                    "resolution conversion rounding or input count");
            }
        }
    }
    require(source.requantized(17) == source, "invalid target resolution changed data");
}

void testSharingAndExternalSync() {
    for (const auto resolution : kSccNumericResolutions) {
        const auto original = parse("0 " + std::to_string(resolution - 1), resolution);
        const auto shared = original.sharedText();
        require(shared == "SCCNUM/1 L=" + std::to_string(resolution)
            + " 0 " + std::to_string(resolution - 1), "share text format");
        const auto pasted = parseSccNumericWave(shared, 256, true);
        require(pasted.shared && pasted.value && *pasted.value == original,
            "share paste did not restore original resolution and two input values");
        require(pasted.value->waveform() == original.waveform(),
            "share round trip changed waveform");
    }
    require(parse("  000  015  ", 16).text() == "0 15", "spaces/leading zeros");
    require(parse("0 15", 16, true).resolution == 16,
        "plain paste must use selected resolution");
    require(parse("SCCNUM/1   L=16   0 15 ", 256, true).values.size() == 2,
        "sharing whitespace rejected");
    for (int first = -128; first <= 96; first += 32) {
        mgstc::engine::SccWaveform waveform;
        for (std::size_t i = 0; i < waveform.size(); ++i)
            waveform[i] = static_cast<std::int8_t>(first + static_cast<int>(i));
        const auto synced = SccNumericWave::fromWaveform(waveform);
        require(synced.resolution == 256 && synced.values.size() == 32,
            "external waveform must sync as 256-resolution 32-value text");
        require(synced.waveform() == waveform && parse(synced.text()).waveform() == waveform,
            "external waveform signed-to-unsigned conversion lost a sample");
    }
    const auto compressed = parse("0 15", 16);
    const auto external = SccNumericWave::fromWaveform(compressed.waveform());
    require(compressed != external && compressed.waveform() == external.waveform(),
        "equal waveform must allow distinct editable resolution and value count");
}

void testRejections() {
    const auto reject = [](std::string_view text, int resolution,
        SccNumericError error, bool allow_shared = false) {
        const auto parsed = parseSccNumericWave(text, resolution, allow_shared);
        require(!parsed.value && parsed.error == error, "incorrect invalid-input result");
    };
    reject("", 256, SccNumericError::Empty);
    reject("   ", 16, SccNumericError::Empty);
    reject("0", 17, SccNumericError::InvalidResolution);
    for (const auto text : {"-1", "+1", "1.5", "0,1", "1\t2", "1\n2",
             "0x10", "one", "1a", "\xEF\xBC\x91"})
        reject(text, 256, SccNumericError::InvalidCharacter);
    reject("16", 16, SccNumericError::OutOfRange);
    reject("256", 256, SccNumericError::OutOfRange);
    reject("99999999999999999999999999999999999", 256, SccNumericError::OutOfRange);
    std::string too_many = "0";
    for (int i = 1; i < 33; ++i) too_many += " 0";
    reject(too_many, 256, SccNumericError::TooManyValues);
    reject("SCCNUM/2 L=16 0 15", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCNUM/1", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCNUM/1X L=16 0", 256, SccNumericError::UnsupportedFormat, true);
    reject("SCCNUM/1 L=17 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCNUM/1 l=16 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCNUM/1 L=-16 0", 256, SccNumericError::InvalidResolution, true);
    reject("SCCNUM/1 L=99999999999999999999 0", 256,
        SccNumericError::InvalidResolution, true);
    reject("SCCNUM/1 L=16", 256, SccNumericError::Empty, true);
    reject("SCCNUM/1 L=16 16", 256, SccNumericError::OutOfRange, true);
    reject("SCCNUM/1 L=16 0", 256, SccNumericError::InvalidCharacter);
}
} // namespace

int main() {
    try {
        testExpansion();
        testResolutionAndRounding();
        testSharingAndExternalSync();
        testRejections();
        std::cout << "SCC numeric waveform tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
