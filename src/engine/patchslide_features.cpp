// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/patchslide_features.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <memory>
#include <utility>
#include <vector>

#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/register_write.hpp"

extern "C" {
#include "emu2413.h"
}

namespace mgstc::engine {
namespace {
constexpr std::size_t kEnvelopeBins = 50;
constexpr std::size_t kFrameCount = 8;
constexpr std::size_t kHarmonicCount = 32;
constexpr std::size_t kBandCount = 16;
constexpr std::size_t kFrameWidth = kHarmonicCount + kBandCount;
constexpr std::size_t kSpectralOffset = 52;
constexpr std::array<double, kFrameCount> kFrameTimes{
    0.005, 0.0167, 0.0333, 0.0667, 0.12, 0.2, 0.35, 0.475};
constexpr double kQuantizationEpsilon = 4.0 / 65535.0;
constexpr double kShapeScale = 4.0;
constexpr double kMeanWeight = 0.8;
constexpr double kStdWeight = 0.2;

void fft(std::span<std::complex<float>> values) noexcept {
    const auto count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const float angle = -2.0F * std::numbers::pi_v<float>
            / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t begin = 0; begin < count; begin += length) {
            std::complex<float> twiddle(1.0F, 0.0F);
            const auto half = length >> 1;
            for (std::size_t i = 0; i < half; ++i) {
                const auto even = values[begin + i];
                const auto odd = values[begin + i + half] * twiddle;
                values[begin + i] = even + odd;
                values[begin + i + half] = even - odd;
                twiddle *= step;
            }
        }
    }
}

std::uint16_t encodeQ16(double value, double maximum) noexcept {
    const auto encoded = std::floor(value * (65535.0 / maximum) + 0.5);
    return static_cast<std::uint16_t>(std::clamp(encoded, 0.0, 65535.0));
}

double decodeQ16(std::uint16_t value, double maximum) noexcept {
    return static_cast<double>(value) * (maximum / 65535.0);
}

} // namespace

PatchSlideFeatureError extractPatchSlideFingerprint(
    std::span<const std::int16_t> pcm,
    double fundamental_hz,
    PatchSlideFingerprint& output,
    const std::function<bool()>& cancelled) {
    if (pcm.empty()) return PatchSlideFeatureError::InvalidInput;
    if (!(fundamental_hz > 0.0) || !std::isfinite(fundamental_hz))
        return PatchSlideFeatureError::InvalidPitch;

    double sum = 0.0;
    double peak = 0.0;
    for (const auto sample : pcm) {
        const double value = static_cast<double>(sample) / 32768.0;
        sum += value * value;
        peak = std::max(peak, std::abs(value));
    }
    output[0] = static_cast<float>(std::sqrt(sum / pcm.size()));
    output[1] = static_cast<float>(peak);

    for (std::size_t bin = 0; bin < kEnvelopeBins; ++bin) {
        const auto begin = bin * pcm.size() / kEnvelopeBins;
        const auto end = (bin + 1) * pcm.size() / kEnvelopeBins;
        double power = 0.0;
        for (auto index = begin; index < end; ++index) {
            const double value = static_cast<double>(pcm[index]) / 32768.0;
            power += value * value;
        }
        const auto count = std::max<std::size_t>(1, end - begin);
        output[2 + bin] = static_cast<float>(std::sqrt(power / count));
    }

    const auto rate = static_cast<double>(kPatchSlideFingerprintSampleRate);
    for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
        if (cancelled && cancelled()) return PatchSlideFeatureError::InvalidInput;
        const std::size_t size = frame < 4 ? 4096 : 16384;
        std::vector<std::complex<float>> values(size);
        const auto center = static_cast<std::int64_t>(kFrameTimes[frame] * rate);
        for (std::size_t index = 0; index < size; ++index) {
            const auto source = center - static_cast<std::int64_t>(size / 2)
                + static_cast<std::int64_t>(index);
            const float window = 0.5F - 0.5F * std::cos(
                2.0F * std::numbers::pi_v<float> * static_cast<float>(index)
                / static_cast<float>(size - 1));
            values[index] = source >= 0 && source < static_cast<std::int64_t>(pcm.size())
                ? static_cast<float>(pcm[static_cast<std::size_t>(source)]) / 32768.0F * window
                : 0.0F;
        }
        fft(values);
        auto* target = output.data() + kSpectralOffset + frame * kFrameWidth;
        for (std::size_t harmonic = 1; harmonic <= kHarmonicCount; ++harmonic) {
            const auto bin = static_cast<int>(std::llround(
                static_cast<double>(harmonic) * fundamental_hz * size / rate));
            double energy = 0.0;
            for (int index = std::max(1, bin - 1);
                 index <= std::min(static_cast<int>(size / 2 - 1), bin + 1); ++index)
                energy += std::norm(values[static_cast<std::size_t>(index)]);
            target[harmonic - 1] = static_cast<float>(std::sqrt(energy) / size);
        }
        for (std::size_t band = 0; band < kBandCount; ++band) {
            const double low = 50.0 * std::pow(320.0, static_cast<double>(band) / kBandCount);
            const double high = 50.0 * std::pow(320.0, static_cast<double>(band + 1) / kBandCount);
            const int first = std::max(1, static_cast<int>(std::ceil(low * size / rate)));
            const int last = std::min(static_cast<int>(size / 2),
                static_cast<int>(std::ceil(high * size / rate)));
            double energy = 0.0;
            for (int index = first; index < last; ++index)
                energy += std::norm(values[static_cast<std::size_t>(index)]);
            target[kHarmonicCount + band] = static_cast<float>(std::sqrt(energy) / size);
        }
    }
    return PatchSlideFeatureError::None;
}

PatchSlideFeatureError makePatchSlideSearchVector(
    const std::array<PatchSlideFingerprint, 4>& fingerprints,
    PatchSlideSearchVector& output) {
    std::array<std::array<double, kFrameCount * kFrameWidth>, 4> shapes{};
    for (std::size_t note = 0; note < fingerprints.size(); ++note) {
        for (std::size_t value = 0; value < kPatchSlideFingerprintDimension; ++value)
            if (!std::isfinite(fingerprints[note][value]) || fingerprints[note][value] < 0.0F)
                return PatchSlideFeatureError::InvalidInput;
        for (std::size_t frame = 0; frame < kFrameCount; ++frame) {
            const auto offset = kSpectralOffset + frame * kFrameWidth;
            double harmonic_norm = 0.0;
            double band_norm = 0.0;
            for (std::size_t index = 0; index < kHarmonicCount; ++index) {
                if (fingerprints[note][offset + index] > 0.625F)
                    return PatchSlideFeatureError::InvalidInput;
                const auto q = encodeQ16(fingerprints[note][offset + index], 0.625);
                const auto value = decodeQ16(q, 0.625);
                shapes[note][frame * kFrameWidth + index] = value;
                harmonic_norm += value * value;
            }
            for (std::size_t index = 0; index < kBandCount; ++index) {
                if (fingerprints[note][offset + kHarmonicCount + index] > 0.625F)
                    return PatchSlideFeatureError::InvalidInput;
                const auto q = encodeQ16(fingerprints[note][offset + kHarmonicCount + index], 0.625);
                const auto value = decodeQ16(q, 0.625);
                shapes[note][frame * kFrameWidth + kHarmonicCount + index] = value;
                band_norm += value * value;
            }
            harmonic_norm = std::max(std::sqrt(harmonic_norm), kQuantizationEpsilon);
            band_norm = std::max(std::sqrt(band_norm), kQuantizationEpsilon);
            for (std::size_t index = 0; index < kHarmonicCount; ++index)
                shapes[note][frame * kFrameWidth + index] /= harmonic_norm;
            for (std::size_t index = 0; index < kBandCount; ++index)
                shapes[note][frame * kFrameWidth + kHarmonicCount + index] /= band_norm;
            for (std::size_t index = 0; index < kFrameWidth; ++index)
                shapes[note][frame * kFrameWidth + index] /= kShapeScale;
        }
    }

    constexpr double mean_weight = 0.89442719099991587856; // sqrt(0.8)
    constexpr double std_weight = 0.44721359549995793928; // sqrt(0.2)
    for (std::size_t coordinate = 0; coordinate < shapes[0].size(); ++coordinate) {
        double mean = 0.0;
        for (std::size_t note = 0; note < shapes.size(); ++note)
            mean += shapes[note][coordinate];
        mean /= static_cast<double>(shapes.size());
        double variance = 0.0;
        for (std::size_t note = 0; note < shapes.size(); ++note) {
            const double delta = shapes[note][coordinate] - mean;
            variance += delta * delta;
        }
        variance /= static_cast<double>(shapes.size());
        output[coordinate] = mean_weight * mean;
        output[shapes[0].size() + coordinate] = std_weight * std::sqrt(variance);
    }
    return PatchSlideFeatureError::None;
}

std::optional<PatchSlideSearchVector> makePatchSlideSearchVector(
    const CompositeTimbre& source,
    std::string* error,
    std::function<bool()> cancelled) {
    auto fail = [error](std::string message)
        -> std::optional<PatchSlideSearchVector> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    auto is_cancelled = [&cancelled] {
        return cancelled && cancelled();
    };
    if (is_cancelled()) return fail("PatchSlide query rendering was cancelled");

    const auto plan = buildCompositePlaybackPlan(source);
    if (plan.audible_layers.size() != 1
        || plan.audible_layers.front().source != TimbreSource::Opll
        || source.layers[plan.audible_layers.front().layer_index].source != TimbreSource::Opll) {
        return fail("PatchSlide query requires exactly one audible isolated OPLL layer");
    }

    EngineCore timeline;
    if (!timeline.valid()) return fail("Cannot initialize MGSTC query timeline");
    timeline.setOpllScopeEnabled(false);
    CompositePlaybackPlan verified_plan;
    if (!compileCompositeProgram(timeline, source, {}, &verified_plan)
        || verified_plan.audible_layers.size() != 1
        || verified_plan.audible_layers.front().source != TimbreSource::Opll) {
        return fail("Cannot compile the isolated OPLL layer for PatchSlide query");
    }
    timeline.session().gateUntilNoteOn();
    const auto query_note = layerMidiNote(source.layers[verified_plan.audible_layers.front().layer_index], 60);
    if (!query_note) return fail("The isolated OPLL layer is outside the supported pitch range at the reference note");

    constexpr std::uint64_t kTicksPerNote = 30; // C4 quarter at tempo 120.
    // Quarter plus sixteenth rest at tempo120 has a 37.5-tick period.
    constexpr std::array<std::uint64_t, 4> kNoteStarts{0, 38, 75, 113};
    constexpr std::uint64_t kLastKeyOffTick = kNoteStarts.back() + kTicksPerNote;
    constexpr std::uint64_t kFramesPerTick = kPatchSlideFingerprintSampleRate / 60;
    constexpr std::size_t kMaximumCapturedWrites = 65'536;
    struct TimedWrite { std::uint64_t sample; std::uint8_t address; std::uint8_t value; };
    std::vector<TimedWrite> writes;
    writes.reserve(2048);

    for (std::uint64_t tick = 0; tick <= kLastKeyOffTick; ++tick) {
        if (is_cancelled()) return fail("PatchSlide query rendering was cancelled");
        for (std::uint8_t note = 0; note < 4; ++note) {
            const auto start = kNoteStarts[note];
            if (tick == start
                && !timeline.session().queueNoteOn(
                    verified_plan.physicalTrack(verified_plan.audible_layers.front(), 0), *query_note))
                return fail("Cannot schedule PatchSlide query note-on");
            if (tick == start + kTicksPerNote
                && !timeline.session().queueKeyOff(
                    verified_plan.physicalTrack(verified_plan.audible_layers.front(), 0)))
                return fail("Cannot schedule PatchSlide query key-off");
        }
        const auto tick_result = timeline.session().processTick();
        if (!tick_result.ok()) return fail("PatchSlide query sequence processing failed");
        for (const auto& write : timeline.session().writes()) {
            if (write.chip != ChipId::Opll) continue;
            if (write.port != 0 || write.address > 0x38)
                return fail("PatchSlide query produced an unsupported OPLL register write");
            if (writes.size() >= kMaximumCapturedWrites)
                return fail("PatchSlide query exceeded the bounded OPLL write budget");
            writes.push_back({tick * kFramesPerTick, write.address, write.value});
        }
    }

    // Derive key-on regions and their final frequency from the actual ordered
    // register stream, including detune and pitch automation. The isolated
    // playback plan assigns its only OPLL voice to physical channel zero.
    struct Region { std::uint64_t begin, end; double f0; };
    std::vector<Region> regions;
    std::array<std::uint8_t, 64> registers{};
    std::optional<std::uint64_t> key_on;
    for (const auto& write : writes) {
        if (write.address == 0x20) {
            const bool old_on = (registers[0x20] & 0x10) != 0;
            const bool new_on = (write.value & 0x10) != 0;
            if (!old_on && new_on) key_on = write.sample;
            else if (old_on && !new_on) {
                if (!key_on) return fail("Invalid OPLL query key-off sequence");
                const auto fnum = registers[0x10] + ((registers[0x20] & 1) << 8);
                const auto block = (registers[0x20] >> 1) & 7;
                const double frequency = fnum * 3'579'545.0 / (72.0 * (1u << (19 - block)));
                if (!(frequency > 0.0)) return fail("The OPLL query has no usable fundamental");
                regions.push_back({*key_on, write.sample, frequency});
                key_on.reset();
            }
        }
        registers[write.address] = write.value;
    }
    if (key_on || regions.size() != 4)
        return fail("The OPLL query requires four complete key-on regions");
    for (const auto& region : regions) {
        const double seconds = double(region.end - region.begin) / kPatchSlideFingerprintSampleRate;
        if (!(seconds > 0.45 && seconds < 0.55))
            return fail("The OPLL query key-on duration is outside the analysis contract");
    }
    using OpllOwner = std::unique_ptr<OPLL, decltype(&OPLL_delete)>;
    OpllOwner chip(OPLL_new(3'579'545, kPatchSlideFingerprintSampleRate), &OPLL_delete);
    if (!chip) return fail("Cannot initialize 192 kHz YM2413 query renderer");
    OPLL_setChipType(chip.get(), 0);
    OPLL_reset(chip.get());
    OPLL_resetPatch(chip.get(), 0);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(regions.back().end));
    std::size_t next_write = 0;
    for (std::uint64_t sample = 0; sample < pcm.size(); ++sample) {
        if (sample % kFramesPerTick == 0 && is_cancelled())
            return fail("PatchSlide query rendering was cancelled");
        while (next_write < writes.size() && writes[next_write].sample <= sample) {
            OPLL_writeReg(chip.get(), writes[next_write].address, writes[next_write].value);
            ++next_write;
        }
        pcm[static_cast<std::size_t>(sample)] = OPLL_calc(chip.get());
    }
    std::array<PatchSlideFingerprint, 4> fingerprints{};
    for (std::size_t note = 0; note < fingerprints.size(); ++note) {
        if (is_cancelled()) return fail("PatchSlide query analysis was cancelled");
        const auto& region = regions[note];
        const auto samples = std::span<const std::int16_t>(pcm).subspan(
            static_cast<std::size_t>(region.begin), static_cast<std::size_t>(region.end - region.begin));
        const auto result = extractPatchSlideFingerprint(samples, region.f0, fingerprints[note], is_cancelled);
        if (result != PatchSlideFeatureError::None)
            return fail(is_cancelled()
                ? "PatchSlide query analysis was cancelled"
                : "PatchSlide query fingerprint extraction failed");
    }
    PatchSlideSearchVector vector{};
    if (makePatchSlideSearchVector(fingerprints, vector) != PatchSlideFeatureError::None)
        return fail("PatchSlide query vector construction failed");
    if (error != nullptr) error->clear();
    return vector;
}

} // namespace mgstc::engine
