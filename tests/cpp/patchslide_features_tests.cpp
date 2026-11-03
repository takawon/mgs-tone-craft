// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_features.hpp"
#include "mgstc/engine/opll_patch.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace mgstc::engine;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void close(double actual, double expected, double tolerance, const char* message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}

void testFeatureLayoutAndDeterminism() {
    std::vector<std::int16_t> pcm(kPatchSlideFingerprintSampleRate / 2);
    constexpr double f0 = 1'000.0;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * f0 * i
            / kPatchSlideFingerprintSampleRate;
        pcm[i] = static_cast<std::int16_t>(std::lround(12'000.0 * std::sin(phase)));
    }
    PatchSlideFingerprint first{}, second{};
    require(extractPatchSlideFingerprint(pcm, f0, first) == PatchSlideFeatureError::None,
            "extracting a valid 192 kHz key-on region succeeds");
    require(extractPatchSlideFingerprint(pcm, f0, second) == PatchSlideFeatureError::None,
            "repeating the same extraction succeeds");
    require(first == second, "feature extraction is deterministic");
    close(first[0], 12'000.0 / (32768.0 * std::sqrt(2.0)), 0.0001,
          "whole-region RMS uses signed PCM16 full scale");
    close(first[1], 12'000.0 / 32768.0, 0.00004,
          "peak retains absolute full-scale amplitude");
    for (std::size_t bin = 0; bin < 50; ++bin)
        close(first[2 + bin], first[0], 0.0003, "50 envelope bins retain amplitude");
    double spectral_energy = 0.0;
    for (std::size_t frame = 0; frame < 8; ++frame)
        for (std::size_t feature = 0; feature < 48; ++feature)
            spectral_energy += first[52 + frame * 48 + feature];
    require(spectral_energy > 0.0, "eight spectral frames contain harmonic/band energy");

    int checks = 0;
    const auto cancelled = extractPatchSlideFingerprint(pcm, f0, second, [&] { return ++checks >= 3; });
    require(cancelled == PatchSlideFeatureError::InvalidInput && checks == 3,
            "analysis checks cancellation between spectral frames");
    require(extractPatchSlideFingerprint({}, f0, second) == PatchSlideFeatureError::InvalidInput,
            "empty PCM is rejected");
    require(extractPatchSlideFingerprint(pcm, 0.0, second) == PatchSlideFeatureError::InvalidPitch,
            "nonpositive fundamental is rejected");
}

void testFrozenQ16ShapeAggregationGolden() {
    std::array<PatchSlideFingerprint, 4> notes{};
    for (std::size_t note = 0; note < notes.size(); ++note) {
        for (std::size_t frame = 0; frame < 8; ++frame) {
            const auto offset = 52 + frame * 48;
            notes[note][offset + note] = static_cast<float>(0.2 + note * 0.03);
            notes[note][offset + 32] = static_cast<float>(0.1 + note * 0.01);
        }
    }
    PatchSlideSearchVector vector{};
    require(makePatchSlideSearchVector(notes, vector) == PatchSlideFeatureError::None,
            "four-note fingerprints produce a 768-dimensional search vector");
    constexpr double expected_mean = (1.0 / 16.0) * 0.89442719099991587856;
    constexpr double expected_band_mean = 0.25 * 0.89442719099991587856;
    const double expected_std = (std::sqrt(3.0 / 16.0) / 4.0)
        * 0.44721359549995793928;
    for (std::size_t frame = 0; frame < 8; ++frame) {
        const auto offset = frame * 48;
        close(vector[offset], expected_mean, 1e-14,
              "mean segment is sqrt(0.8) times mean normalized shape divided by four");
        close(vector[offset + 1], expected_mean, 1e-14,
              "four-note mean preserves component layout");
        close(vector[offset + 32], expected_band_mean, 1e-14,
              "log-band values follow harmonic values within every frame");
        close(vector[384 + offset], expected_std, 1e-14,
              "history segment is sqrt(0.2) times population standard deviation divided by four");
        close(vector[384 + offset + 32], 0.0, 1e-14,
              "harmonic and log-band groups use separate normalization");
    }
    for (std::size_t i = 0; i < vector.size(); ++i)
        require(std::isfinite(vector[i]), "all 768 golden coordinates are finite");

    notes[2][52] = -0.1F;
    require(makePatchSlideSearchVector(notes, vector) == PatchSlideFeatureError::InvalidInput,
            "negative spectral magnitudes are rejected");
}

void testCompositeRendererBoundariesAndCancellation() {
    CompositeTimbre isolated;
    std::string error;
    const auto empty = makePatchSlideSearchVector(isolated, &error);
    require(!empty && !error.empty(), "renderer rejects a model without its isolated OPLL layer");
    const auto stopped = makePatchSlideSearchVector(isolated, &error, [] { return true; });
    require(!stopped && error.find("cancelled") != std::string::npos,
            "renderer returns promptly when its worker generation is cancelled");

    CompositeLayer layer;
    layer.source = TimbreSource::Opll;
    layer.channel = 0;
    layer.base_opll_rom = 0;
    isolated.layers.push_back(layer);
    const auto rendered = makePatchSlideSearchVector(isolated, &error);
    require(rendered.has_value(), "isolated ROM OPLL layer renders a four-note 192 kHz query");
    require(error.empty(), "successful query clears the error string");
    for (const auto value : *rendered)
        require(std::isfinite(value), "rendered 768-dimensional query is finite");
    SavedTimbreReference snapshot;
    snapshot.library_id = 4242;
    snapshot.source = TimbreSource::Opll;
    snapshot.opll_registers = encodeOpllPatch(defaultOpllPatch());
    isolated.layers[0].base_opll_rom.reset();
    isolated.layers[0].base_timbre = snapshot;
    isolated.embedded_timbres.push_back(snapshot);
    isolated.layers[0].relative_semitones = 7;
    isolated.layers[0].detune = 3;
    isolated.layers[0].micro_detune = 23;
    isolated.layers[0].envelope_timeline.length_counts = 10;
    isolated.layers[0].pitch_envelope.events = {{.kind=EnvelopeEventKind::Pitch,.value=1,.count=2}};
    isolated.layers[0].timbre_automation = {{.kind=EnvelopeEventKind::Timbre,.value=5,.count=4,.timbre_pick=TimbrePick::OpllRom}};
    const auto original = isolated;
    const auto custom = makePatchSlideSearchVector(isolated, &error);
    require(custom.has_value() && *custom != *rendered && isolated == original,
        "DB-unregistered original/ROM sequence with pitch parameters produces an immutable acoustic query");
    isolated.layers[0].relative_semitones = -100;
    require(!makePatchSlideSearchVector(isolated, &error) && !error.empty(),
        "out-of-range reference-note transposition is rejected without clamping");
}

std::uint32_t readU32(std::istream& input) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    require(bool(input), "truncated feature golden");
    return bytes[0] | (std::uint32_t(bytes[1]) << 8)
        | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

double readF64(std::istream& input) {
    const auto low = readU32(input);
    const auto high = readU32(input);
    return std::bit_cast<double>(std::uint64_t(low) | (std::uint64_t(high) << 32));
}

void testProductionPcmGolden() {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/fixtures/patchslide/features/db_pcm_golden_v1.bin";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cout << "Production PCM/436/768 golden skipped (local fixture absent).\n";
        return;
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), magic.size());
    require(std::string_view(magic.data(), magic.size()) == "MGSTFP01", "feature golden magic");
    const auto cases = readU32(input);
    require(cases == 6, "production golden must cover six independent DB candidates");
    double max_fingerprint_error = 0.0, max_vector_error = 0.0;
    for (std::uint32_t case_index = 0; case_index < cases; ++case_index) {
        std::array<char, 64> id{};
        input.read(id.data(), id.size());
        std::array<PatchSlideFingerprint, 4> fingerprints{};
        for (std::size_t note = 0; note < fingerprints.size(); ++note) {
            const auto count = readU32(input);
            const auto f0 = readF64(input);
            require(count > 86'400 && count < 105'600, "golden key-on region length");
            PatchSlideFingerprint expected{};
            for (auto& value : expected) value = std::bit_cast<float>(readU32(input));
            std::vector<std::int16_t> pcm(count);
            for (auto& value : pcm) {
                const auto lo = input.get(), hi = input.get();
                require(lo >= 0 && hi >= 0, "truncated golden PCM");
                value = std::bit_cast<std::int16_t>(std::uint16_t(lo | (hi << 8)));
            }
            require(extractPatchSlideFingerprint(pcm, f0, fingerprints[note]) == PatchSlideFeatureError::None,
                "production PCM extraction");
            for (std::size_t coordinate = 0; coordinate < expected.size(); ++coordinate) {
                const double error = std::abs(double(fingerprints[note][coordinate]) - expected[coordinate]);
                max_fingerprint_error = std::max(max_fingerprint_error, error);
                close(fingerprints[note][coordinate], expected[coordinate],
                    2e-7 + 1e-5 * expected[coordinate], "production 436-coordinate reference mismatch");
            }
        }
        PatchSlideSearchVector expected{}, actual{};
        for (auto& value : expected) value = readF64(input);
        require(makePatchSlideSearchVector(fingerprints, actual) == PatchSlideFeatureError::None,
            "production four-note vector extraction");
        for (std::size_t coordinate = 0; coordinate < expected.size(); ++coordinate) {
            max_vector_error = std::max(max_vector_error, std::abs(actual[coordinate] - expected[coordinate]));
            if (std::abs(actual[coordinate] - expected[coordinate]) > 1e-12) {
                std::cerr << id.data() << " vector coordinate " << coordinate << ": actual="
                    << actual[coordinate] << " expected=" << expected[coordinate] << '\n';
            }
            close(actual[coordinate], expected[coordinate], 1e-12, "production stored 768-coordinate reference mismatch");
        }
    }
    require(input.peek() == std::char_traits<char>::eof(), "unexpected feature golden suffix");
    std::cout << "Production PCM/436/768 golden passed (6 candidates, 24 notes); max436="
        << max_fingerprint_error << ", max768=" << max_vector_error << '\n';
}

} // namespace

int main() {
    testFeatureLayoutAndDeterminism();
    testFrozenQ16ShapeAggregationGolden();
    testCompositeRendererBoundariesAndCancellation();
    testProductionPcmGolden();
    return 0;
}
