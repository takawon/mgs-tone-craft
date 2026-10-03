// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/tone_library_store.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mgstc::engine;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, double tolerance,
                 const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

SccWaveform waveFrom(const std::array<int, 32>& samples) {
    SccWaveform wave{};
    for (std::size_t i = 0; i < wave.size(); ++i) {
        wave[i] = static_cast<std::int8_t>(samples[i]);
    }
    return wave;
}

SccWaveform sawWave(int multiplier = 1, int bias = 0) {
    SccWaveform wave{};
    for (std::size_t i = 0; i < wave.size(); ++i) {
        const int phase = static_cast<int>((i * multiplier) % 32);
        wave[i] = static_cast<std::int8_t>(bias + (phase - 16) * 6);
    }
    return wave;
}

SavedTimbreReference sccTone(std::uint64_t id, std::uint8_t number,
                             const SccWaveform& wave) {
    SavedTimbreReference tone;
    tone.library_id = id;
    tone.name = "Morph fixture";
    tone.source = TimbreSource::Scc;
    tone.number_mode = TimbreNumberMode::Manual;
    tone.manual_number = number;
    for (std::size_t i = 0; i < wave.size(); ++i) {
        tone.scc_waveform[i] = static_cast<std::uint8_t>(wave[i]);
    }
    return tone;
}

EnvelopeEvent toneEvent(std::uint64_t id, std::uint32_t count,
                        SccMorphTransition incoming = {}) {
    EnvelopeEvent event;
    event.kind = EnvelopeEventKind::Timbre;
    event.count = count;
    event.target_library_id = id;
    event.scc_morph = incoming;
    return event;
}

CompositeTimbre morphProgram(const SccWaveform& first,
                             const SccWaveform& last,
                             std::uint32_t first_count = 0,
                             std::uint32_t last_count = 20,
                             std::uint8_t intermediate_count = 2,
                             std::uint8_t bank_base = 0,
                             double curve = 1.0) {
    CompositeTimbre timbre;
    timbre.name = "SCC morph test";
    timbre.scc_morph_bank_base = bank_base;
    timbre.scc_morph_algorithm_version = kSccMorphAlgorithmVersion;

    auto first_tone = sccTone(1001, 15, first);
    auto last_tone = sccTone(1002, 16, last);
    CompositeLayer layer;
    layer.name = "SCC";
    layer.source = TimbreSource::Scc;
    layer.channel = 0;
    layer.base_timbre = first_tone;
    layer.envelope_timeline.length_counts = std::max<std::uint32_t>(
        30, last_count + 1);
    layer.timbre_automation = {
        toneEvent(first_tone.library_id, first_count),
        toneEvent(last_tone.library_id, last_count,
                  SccMorphTransition{
                      .enabled = true,
                      .intermediate_count = intermediate_count,
                      .curve = curve,
                  }),
    };
    timbre.layers.push_back(std::move(layer));
    timbre.embedded_timbres.push_back(std::move(last_tone));
    return timbre;
}

CompositeTimbre blockProgram(const SccWaveform& x, const SccWaveform& a,
                             const SccWaveform& b, const SccWaveform& c,
                             const SccWaveform& y) {
    CompositeTimbre timbre;
    timbre.name = "SCC morph block test";
    CompositeLayer layer;
    layer.name = "SCC";
    layer.source = TimbreSource::Scc;
    layer.channel = 0;
    const std::array<SavedTimbreReference, 5> tones{
        sccTone(2001, 15, x), sccTone(2002, 16, a), sccTone(2003, 17, b),
        sccTone(2004, 18, c), sccTone(2005, 19, y),
    };
    layer.base_timbre = tones[0];
    layer.envelope_timeline.length_counts = 30;
    layer.timbre_automation = {
        toneEvent(tones[0].library_id, 0),
        toneEvent(tones[1].library_id, 2),
        toneEvent(tones[2].library_id, 8,
                  SccMorphTransition{.enabled = true,
                                     .intermediate_count = 0,
                                     .curve = 1.0}),
        toneEvent(tones[3].library_id, 14,
                  SccMorphTransition{.enabled = true,
                                     .intermediate_count = 0,
                                     .curve = 1.0}),
        toneEvent(tones[4].library_id, 20),
    };
    timbre.layers.push_back(std::move(layer));
    timbre.embedded_timbres.assign(tones.begin() + 1, tones.end());
    return timbre;
}

const SccMorphWaveDiagnostic& diagnosticAt(
    const SccMorphCompileResult& result, std::uint32_t count) {
    const auto found = std::find_if(result.diagnostics.begin(),
        result.diagnostics.end(), [count](const auto& diagnostic) {
            return diagnostic.count == count;
        });
    if (found == result.diagnostics.end()) {
        throw std::runtime_error("missing SCC morph event diagnostic");
    }
    return *found;
}

SccWaveform materializedWaveFor(
    const CompositeTimbre& timbre,
    const SccMorphWaveDiagnostic& diagnostic) {
    const auto& layer = timbre.layers.at(diagnostic.layer_index);
    const auto event = std::find_if(layer.timbre_automation.begin(),
        layer.timbre_automation.end(), [&](const auto& candidate) {
            return candidate.kind == EnvelopeEventKind::Timbre
                && candidate.count == diagnostic.count
                && candidate.value == diagnostic.output_number;
        });
    require(event != layer.timbre_automation.end(),
            "diagnostic points to a materialized tone event");
    const auto* tone = findEmbeddedTimbreSnapshot(timbre, event->target_library_id);
    require(tone != nullptr && tone->source == TimbreSource::Scc,
            "materialized tone event resolves to an SCC snapshot");
    SccWaveform wave{};
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = std::bit_cast<std::int8_t>(tone->scc_waveform[i]);
    return wave;
}

bool allFinite(const SccMorphDescriptors& d) {
    return std::isfinite(d.centroid) && std::isfinite(d.spread)
        && std::isfinite(d.slope) && std::isfinite(d.odd_even)
        && std::isfinite(d.irregularity) && std::isfinite(d.flatness)
        && std::isfinite(d.rms) && std::isfinite(d.dc)
        && std::isfinite(d.peak);
}

void testCurveAndQuantizedGeneration() {
    for (const double gamma : {kSccMorphGammaMin, 1.0, kSccMorphGammaMax}) {
        requireNear(morphPosition(0.0, gamma), 0.0, 1e-12,
                    "morph curve starts at source");
        requireNear(morphPosition(1.0, gamma), 1.0, 1e-12,
                    "morph curve ends at target");
        double previous = 0.0;
        for (int i = 1; i <= 100; ++i) {
            const double t = static_cast<double>(i) / 100.0;
            const double u = morphPosition(t, gamma);
            require(std::isfinite(u) && u >= previous && u <= 1.0,
                    "morph curve is finite and monotonic");
            previous = u;
        }
    }
    for (int i = 0; i <= 10; ++i) {
        const double t = static_cast<double>(i) / 10.0;
        requireNear(morphPosition(t, 1.0), t, 1e-12,
                    "uniform curve is identity");
    }
    require(morphPosition(0.25, kSccMorphGammaMax) > 0.25,
            "front-loaded curve advances faster near the start");
    require(morphPosition(0.75, kSccMorphGammaMin) < 0.75,
            "back-loaded curve advances slower before its endpoint");

    const auto first = sawWave();
    const auto last = sawWave(3, 8);
    const auto generated = generateSccMorph(first, last, 3, 2.5);
    require(generated.intermediate.size() == 3,
            "intermediate count excludes both endpoints");
    require(generated.diagnostics.size() == generated.intermediate.size(),
            "each quantized intermediate has a diagnostic");
    const auto repeated = generateSccMorph(first, last, 3, 2.5);
    require(repeated.intermediate == generated.intermediate,
            "same input produces byte-identical intermediates");
    for (std::size_t i = 0; i < generated.intermediate.size(); ++i) {
        const auto& wave = generated.intermediate[i];
        const auto& diagnostic = generated.diagnostics[i];
        for (const auto sample : wave) {
            require(sample >= -128 && sample <= 127,
                    "SCC quantized samples remain in signed byte range");
        }
        require(std::isfinite(diagnostic.pre_quantization_error)
                    && std::isfinite(diagnostic.post_quantization_error)
                    && allFinite(diagnostic.target)
                    && allFinite(diagnostic.result),
                "pre/post-quantization diagnostics are finite");
        const auto quantized_analysis = analyzeSccMorphWaveform(wave);
        requireNear(diagnostic.result.rms,
                    quantized_analysis.descriptors.rms, 1e-9,
                    "reported result RMS is measured from the quantized wave");
        requireNear(diagnostic.result.dc,
                    quantized_analysis.descriptors.dc, 1e-9,
                    "reported result DC is measured from the quantized wave");
    }
    require(generateSccMorph(first, last, 0).intermediate.empty(),
            "zero intermediate count produces no generated wave");
    require(generateSccMorph(first, last, 1).intermediate.size() == 1,
            "one intermediate count produces one generated wave");
}

void testSilenceDcAndPhaseOnlyInputs() {
    const SccWaveform silence{};
    const auto zero = generateSccMorph(silence, silence, 2);
    require(zero.intermediate.size() == 2,
            "silent sources still produce the requested states");
    for (const auto& wave : zero.intermediate) {
        require(wave == silence, "silence does not grow a noise floor");
        require(allFinite(analyzeSccMorphWaveform(wave).descriptors),
                "silence analysis avoids invalid divisions");
    }

    SccWaveform dc_a{};
    SccWaveform dc_b{};
    dc_a.fill(24);
    dc_b.fill(-40);
    const auto dc_morph = generateSccMorph(dc_a, dc_b, 1);
    require(dc_morph.intermediate.size() == 1,
            "DC-only sources are accepted");
    require(allFinite(analyzeSccMorphWaveform(dc_morph.intermediate.front()).descriptors),
            "DC-only analysis remains finite");
    requireNear(analyzeSccMorphWaveform(dc_morph.intermediate.front()).descriptors.dc,
                -8.0 / 128.0, 2.0 / 128.0,
                "DC target is interpolated without unconditional removal");

    SccWaveform single_harmonic{};
    for (std::size_t i = 0; i < single_harmonic.size(); ++i) {
        single_harmonic[i] = static_cast<std::int8_t>(std::lround(
            88.0 * std::sin(2.0 * std::numbers::pi
                * static_cast<double>(i) / 32.0)));
    }
    const auto fade = generateSccMorph(silence, single_harmonic, 4);
    require(fade.intermediate.size() == 4,
            "silence-to-harmonic fade creates every requested state");
    double previous_rms{};
    for (const auto& wave : fade.intermediate) {
        const auto descriptors = analyzeSccMorphWaveform(wave).descriptors;
        require(allFinite(descriptors), "silence fade descriptors remain finite");
        require(descriptors.rms > previous_rms,
                "silence fade raises level without a silent noise-floor plateau");
        previous_rms = descriptors.rms;
    }

    const auto phase_source = sawWave();
    const auto phase_target = rotateSccWaveform(phase_source, 9);
    const auto source_analysis = analyzeSccMorphWaveform(phase_source);
    const auto target_analysis = analyzeSccMorphWaveform(phase_target);
    for (std::size_t k = 0; k < source_analysis.magnitude.size(); ++k) {
        requireNear(source_analysis.magnitude[k], target_analysis.magnitude[k],
                    1e-8, "circularly shifted source preserves DFT magnitude");
    }
    const auto phase_morph = generateSccMorph(phase_source, phase_target, 3);
    for (const auto& wave : phase_morph.intermediate) {
        require(wave == phase_source,
                "phase-only sources produce no unnecessary timbre change");
        const auto analysis = analyzeSccMorphWaveform(wave);
        require(allFinite(analysis.descriptors),
                "phase-only morph remains finite");
        requireNear(analysis.descriptors.rms,
                    source_analysis.descriptors.rms, 1.0 / 128.0,
                    "phase-only difference preserves RMS within quantization error");
        for (std::size_t k = 0; k < analysis.magnitude.size(); ++k) {
            requireNear(analysis.magnitude[k], source_analysis.magnitude[k],
                        2.0 / 128.0,
                        "phase-only morph preserves harmonic magnitudes");
        }
    }
}

void testSccMorphPhaseBMatrixAndTiming() {
    const auto pulse = generateSccPreset(SccWavePreset::Pulse25, SccHarmonic::One);
    const auto square = generateSccPreset(SccWavePreset::Square, SccHarmonic::One);
    const auto triangle = generateSccPreset(SccWavePreset::Triangle, SccHarmonic::One);
    const auto sine = generateSccPreset(SccWavePreset::Sine, SccHarmonic::One);
    const auto silence = SccWaveform{};
    const auto phase_only = rotateSccWaveform(sine, 9);
    SccWaveform dc_positive{}, dc_negative{}, sparse_a{}, sparse_b{};
    for (std::size_t i = 0; i < 32; ++i) {
        dc_positive[i] = static_cast<std::int8_t>(48 + (i % 2 ? 2 : -2));
        dc_negative[i] = static_cast<std::int8_t>(-48 + (i % 2 ? 3 : -3));
        const double phase = 2.0 * std::numbers::pi * double(i) / 32.0;
        sparse_a[i] = static_cast<std::int8_t>(std::lround(
            80.0 * std::sin(2.0 * phase) + 28.0 * std::sin(7.0 * phase)));
        sparse_b[i] = static_cast<std::int8_t>(std::lround(
            56.0 * std::sin(3.0 * phase) + 20.0 * std::sin(11.0 * phase)));
    }
    struct Pair { const char* name; SccWaveform first, second; };
    const std::array<Pair, 12> pairs{{
        {"Pulse25 -> Sine", pulse, sine}, {"Sine -> Pulse25", sine, pulse},
        {"Square -> Sine", square, sine}, {"Sine -> Square", sine, square},
        {"Triangle -> Sine", triangle, sine}, {"same", sine, sine},
        {"phase only", sine, phase_only}, {"silence -> Sine", silence, sine},
        {"Sine -> silence", sine, silence}, {"large bipolar DC", dc_positive, dc_negative},
        {"large RMS difference", silence, pulse}, {"sparse harmonics", sparse_a, sparse_b},
    }};
    for (const auto& pair : pairs) {
        const auto first = generateSccMorph(pair.first, pair.second, 11, 1.0);
        const auto repeated = generateSccMorph(pair.first, pair.second, 11, 1.0);
        require(first.intermediate.size() == 11
                    && first.diagnostics.size() == first.intermediate.size(),
                "Phase B pair matrix keeps the requested intermediate count");
        require(first.intermediate == repeated.intermediate,
                "Phase B pair matrix remains byte deterministic");
        for (std::size_t i = 0; i < first.intermediate.size(); ++i) {
            for (const auto sample : first.intermediate[i])
                require(sample >= -128 && sample <= 127,
                        "Phase B pair matrix remains in signed SCC sample range");
            require(allFinite(first.diagnostics[i].target)
                        && allFinite(first.diagnostics[i].result)
                        && std::isfinite(first.diagnostics[i].post_quantization_error),
                    "Phase B pair matrix diagnostics remain finite");
        }
        std::cout << "SCC morph Phase B matrix verified: " << pair.name << '\n';
    }

    const auto extreme_started = std::chrono::steady_clock::now();
    const auto extreme = generateSccMorph(pulse, sine, 255, kSccMorphGammaMax);
    const auto extreme_elapsed = std::chrono::steady_clock::now() - extreme_started;
    SccMorphPairTrace extreme_trace;
    const auto extreme_trace_started = std::chrono::steady_clock::now();
    const auto extreme_repeat = generateSccMorphTraced(
        pulse, sine, 255, kSccMorphGammaMax, extreme_trace);
    const auto extreme_trace_elapsed = std::chrono::steady_clock::now()
        - extreme_trace_started;
    require(extreme.intermediate.size() == 255
                && extreme.intermediate == extreme_repeat.intermediate,
            "maximum intermediate count and gamma remain finite and deterministic");
    for (const auto& diagnostic : extreme.diagnostics)
        require(std::isfinite(diagnostic.pre_quantization_error)
                    && std::isfinite(diagnostic.post_quantization_error)
                    && allFinite(diagnostic.target) && allFinite(diagnostic.result),
                "gamma-8 high-density morph diagnostics remain finite");
    double endpoint_sum{};
    for (std::size_t i = 0; i < extreme_trace.aligned_second.size(); ++i) {
        const double difference = double(extreme_trace.aligned_second[i])
            - double(extreme_repeat.intermediate.back()[i]);
        endpoint_sum += difference * difference;
    }
    const auto max_trace_storage = sizeof(SccMorphPairTrace)
        + extreme_trace.steps.capacity() * sizeof(SccMorphStepTrace);
    std::cout << "SCC morph N=255 gamma=8 ordinary_us="
              << std::chrono::duration<double, std::micro>(extreme_elapsed).count()
              << " traced_us="
              << std::chrono::duration<double, std::micro>(extreme_trace_elapsed).count()
              << " max_trace_capacity_bytes=" << max_trace_storage
              << " final_to_aligned_endpoint_sample_rms="
              << std::sqrt(endpoint_sum / 32.0) << '\n';

    const auto short_program = morphProgram(pulse, sine, 0, 5, 3, 0, 1.0);
    const auto short_compiled = compileSccMorph(short_program);
    require(short_compiled.valid, "short rounded-time morph compiles");
    std::vector<std::uint32_t> playback_counts{0};
    for (const auto& diagnostic : short_compiled.diagnostics)
        if (diagnostic.generated && diagnostic.count > 0 && diagnostic.count < 5)
            playback_counts.push_back(diagnostic.count);
    playback_counts.push_back(5);
    require(playback_counts == std::vector<std::uint32_t>{0, 1, 3, 4, 5},
            "three intermediate states retain compiler-rounded event counts");
    SccMorphPairTrace playback_trace;
    const auto playback_pair = generateSccMorphTraced(
        pulse, sine, 3, 1.0, playback_trace, playback_counts);
    require(playback_pair.intermediate.size() == 3
                && playback_trace.steps.size() == 3,
            "actual-time scoring keeps all three requested waves");
    for (std::size_t i = 0; i < playback_trace.steps.size(); ++i) {
        const auto& step = playback_trace.steps[i];
        const auto& selected = step.candidates[step.selected_candidate];
        const auto& edge = selected.incoming[selected.predecessor];
        const double expected_dt = i == 1 ? 0.4 : 0.2;
        requireNear(edge.dt, expected_dt, 1e-12,
                    "transition scoring uses rounded playback interval duration");
        requireNear(step.u, morphPosition(double(i + 1) / 4.0, 1.0), 1e-12,
                    "uneven playback counts do not reposition authored interpolation t/u");
    }
    SccMorphPairTrace repeat_trace;
    const auto repeated_playback = generateSccMorphTraced(
        pulse, sine, 3, 1.0, repeat_trace, playback_counts);
    require(playback_pair.intermediate == repeated_playback.intermediate,
            "rounded-time transition optimization remains deterministic");
}

void testGlobalPhaseOptimizationAndTransitionCost() {
    const auto source = sawWave();
    const auto shifted = rotateSccWaveform(source, 7);
    const auto before = sccMorphTransitionCost(source, shifted);
    require(before > 0.0, "shifted waveforms have nonzero aligned transition cost");

    auto fixed = std::vector<SccWaveform>{source, shifted};
    const auto fixed_shifts = optimizeSccMorphBlockPhases(fixed, true, true);
    require(fixed_shifts.size() == 2, "phase result covers both block endpoints");
    require(fixed_shifts[0] == 0 && fixed_shifts[1] == 0,
            "independently fixed block boundaries keep phase zero");
    require(fixed == std::vector<SccWaveform>{source, shifted},
            "fixed block boundaries preserve authored endpoint waves");

    auto one_fixed = std::vector<SccWaveform>{source, shifted};
    const auto one_fixed_shifts = optimizeSccMorphBlockPhases(one_fixed, true, false);
    require(one_fixed_shifts[0] == 0,
            "fixed start remains unchanged when the other end is free");
    require(one_fixed[0] == source,
            "fixed start bytes are not modified");
    require(sccMorphTransitionCost(one_fixed[0], one_fixed[1]) <= before + 1e-9,
            "free end can improve the all-phase transition cost");

    auto other_fixed = std::vector<SccWaveform>{shifted, source};
    const auto other_fixed_shifts = optimizeSccMorphBlockPhases(
        other_fixed, false, true);
    require(other_fixed_shifts[1] == 0 && other_fixed[1] == source,
            "fixed end remains unchanged when the start is free");
    require(sccMorphTransitionCost(other_fixed[0], other_fixed[1]) <= before + 1e-9,
            "free start can improve the all-phase transition cost");

    auto free = std::vector<SccWaveform>{source, shifted};
    (void)optimizeSccMorphBlockPhases(free, false, false);
    require(sccMorphTransitionCost(free[0], free[1]) <= before + 1e-9,
            "free block endpoints do not worsen transition cost");

    auto internal = std::vector<SccWaveform>{source, shifted, source};
    const auto original_internal = internal;
    const double internal_before = sccMorphTransitionCost(internal[0], internal[1])
        + sccMorphTransitionCost(internal[1], internal[2]);
    const auto shifts = optimizeSccMorphBlockPhases(internal, true, true);
    require(shifts[0] == 0 && shifts[2] == 0,
            "fixed block endpoints retain phase zero around an internal keyframe");
    require(internal[0] == original_internal[0]
                && internal[2] == original_internal[2],
            "fixed endpoints retain their authored source samples");
    require(sccMorphTransitionCost(internal[0], internal[1])
                + sccMorphTransitionCost(internal[1], internal[2])
                <= internal_before + 1e-9,
            "global block optimization does not worsen adjacent transition cost");
    double brute_force_best = std::numeric_limits<double>::infinity();
    for (int candidate_shift = 0; candidate_shift < 32; ++candidate_shift) {
        const auto candidate = rotateSccWaveform(
            original_internal[1], candidate_shift);
        brute_force_best = std::min(brute_force_best,
            sccMorphTransitionCost(source, candidate)
                + sccMorphTransitionCost(candidate, source));
    }
    const double optimized_cost = sccMorphTransitionCost(internal[0], internal[1])
        + sccMorphTransitionCost(internal[1], internal[2]);
    requireNear(optimized_cost, brute_force_best, 1e-9,
                "three-node dynamic program reaches the global phase optimum");
    require(shifts[1] != 0 && internal[1] != original_internal[1],
            "internal keyframe may use a phase-shifted derived waveform");
}

void testContiguousChannelAllocation() {
    auto tone = morphProgram(sawWave(), sawWave(3), 0, 20, 0);
    tone.layers.front().scc_output_allocation = SccOutputAllocationMode::Contiguous;
    tone.layers.front().scc_output_start.reset();
    auto second = tone.layers.front();
    second.channel = 1;
    second.scc_output_start = 0;
    tone.layers.push_back(second);
    const auto allocated = compileSccMorph(tone);
    require(allocated.valid && allocated.channel_allocations.size() == 2,
            "two SCC channels receive distinct contiguous ranges");
    const auto a = allocated.channel_allocations[0];
    const auto b = allocated.channel_allocations[1];
    require(b.start == 0 && b.manual && a.start >= b.count,
            "manual range reserved before automatic regardless of layer order");
    require(allocated.used == a.count + b.count && allocated.capacity == 32,
            "global capacity includes both complete channel ranges");
    const auto saved = CompositeTimbreLibrary::serializeTimbreFile(tone, 0);
    require(CompositeTimbreLibrary::deserializeTimbreFile(saved) == tone,
            "manual and automatic starts survive portable save");
    tone.layers.front().scc_output_start = 0;
    const auto conflict = compileSccMorph(tone);
    require(!conflict.valid && conflict.timbre == tone, "overlapping manual ranges fail atomically");
    tone.layers.resize(1);
    tone.layers.front().scc_output_start = 31;
    require(!compileSccMorph(tone).valid, "multi-wave range cannot extend beyond @31");
    tone.layers.front().timbre_automation.clear();
    const auto lastSlot = compileSccMorph(tone);
    require(lastSlot.valid && lastSlot.channel_allocations.front().start == 31
            && lastSlot.channel_allocations.front().count == 1,
            "single static waveform may occupy final slot");
    require(formatMgsComposite(tone).valid(), "ordinary MGSC export shares final-slot allocation");
    tone.layers.front().scc_output_start = 0;
    tone.layers.front().envelope_timeline.length_counts = 64;
    for (std::size_t i = 0; i < 32; ++i) {
        SccWaveform wave{};
        wave.fill(static_cast<std::int8_t>(i));
        auto ref = sccTone(2000 + i, static_cast<std::uint8_t>(i), wave);
        tone.embedded_timbres.push_back(ref);
        tone.layers.front().timbre_automation.push_back(toneEvent(ref.library_id, static_cast<std::uint32_t>(i + 1)));
        if (i == 30) {
            const auto full = compileSccMorph(tone);
            require(full.valid && full.used == 32, "exact global 32-wave capacity accepted");
        }
    }
    require(!compileSccMorph(tone).valid, "33 distinct output waves rejected");
    tone.layers.front().timbre_automation.clear();
    auto legacy = tone.layers.front();
    legacy.channel = 1;
    legacy.scc_output_allocation = SccOutputAllocationMode::LegacyBank;
    legacy.scc_output_start.reset();
    legacy.base_timbre = sccTone(3000, 0, sawWave(5));
    tone.layers.push_back(legacy);
    require(!compileSccMorph(tone).valid, "contiguous range cannot overwrite ordinary channel waveform");
}
void testCompositeCompilationBoundariesAndAllocation() {
    const auto first = sawWave();
    const auto last = sawWave(3, 8);
    const auto arbitrary_bank = morphProgram(first, last, 0, 20, 2, 7, kSccMorphGammaMin);
    const auto arbitrary_compiled = compileSccMorph(arbitrary_bank);
    require(arbitrary_compiled.valid, "arbitrary SCC start and back-loaded curve compile");
    for (const auto& diagnostic : arbitrary_compiled.diagnostics)
        if (diagnostic.generated)
            require(diagnostic.output_number >= 7 && diagnostic.output_number <= 31,
                    "arbitrary bank respects requested available region");
    for (const auto& round_trip : {
             CompositeTimbreLibrary::deserializeTimbreFile(
                 CompositeTimbreLibrary::serializeTimbreFile(arbitrary_bank, 1)),
             deserializeCompositeSoundPayload(serializeCompositeSoundPayload(arbitrary_bank),
                                               kCompositeSoundPayloadVersion)}) {
        require(round_trip.has_value() && serializeCompositeSoundPayload(*round_trip) == serializeCompositeSoundPayload(arbitrary_bank),
                "arbitrary bank and back-loaded curve survive ordinary saving");
    }
    auto input = morphProgram(first, last, 0, 20, 2, 16, 6.25);
    const auto output = compileSccMorph(input);
    require(output.valid, "valid SCC transition compiles");
    require(output.timbre.scc_morph_materialized,
            "compiled result is marked transient materialized data");
    require(!input.scc_morph_materialized,
            "compilation does not mark or mutate the source model");
    require(input.layers[0].timbre_automation[1].scc_morph.enabled,
            "source transition remains enabled after compilation");
    require(output.diagnostics.size() >= 3,
            "block diagnostics include source and intermediate states");
    require(output.capacity == 15,
            "@16 capacity excludes the occupied source slot");
    for (const auto& diagnostic : output.diagnostics) {
        if (diagnostic.generated) {
            require(diagnostic.output_number >= 17 && diagnostic.output_number <= 31,
                    "@16 allocation leaves the existing @16 source untouched");
        }
    }
    requireNear(output.diagnostics[1].u,
                morphPosition(1.0 / 3.0, 6.25),
                1e-8, "transition curve is applied once to generated position");

    auto morph_off = input;
    morph_off.layers[0].timbre_automation[1].scc_morph.enabled = false;
    const auto morph_off_result = compileSccMorph(morph_off);
    require(morph_off_result.valid && morph_off_result.timbre == morph_off,
            "Morph OFF preserves the established tone-event model exactly");
    require(morph_off_result.diagnostics.empty(),
            "Morph OFF does not leave stale derived diagnostics");

    auto no_previous = morphProgram(first, last);
    no_previous.layers[0].timbre_automation.erase(
        no_previous.layers[0].timbre_automation.begin());
    const auto first_event_id = no_previous.embedded_timbres.front().library_id;
    no_previous.layers[0].timbre_automation[0] =
        toneEvent(first_event_id, 0, SccMorphTransition{true, 1, 1.0});
    const auto no_previous_result = compileSccMorph(no_previous);
    require(!no_previous_result.valid,
            "enabled incoming morph without a previous tone is rejected");

    auto no_slots = morphProgram(first, last, 4, 5, 1);
    const auto no_slots_result = compileSccMorph(no_slots);
    require(!no_slots_result.valid,
            "intermediate wave cannot share an endpoint count slot");

    auto equal_counts = morphProgram(first, last, 4, 4, 1);
    require(!compileSccMorph(equal_counts).valid,
            "equal tone-event counts cannot contain an intermediate state");

    auto source_equal = morphProgram(first, first, 0, 10, 4);
    const auto equal_result = compileSccMorph(source_equal);
    require(equal_result.valid, "identical source endpoints are valid");
    require(equal_result.used == 0 && equal_result.required == 0,
            "byte-identical intermediates reuse the existing source slot");
    auto maximum = morphProgram(first, first, 0, 20, 19);
    const auto maximum_result = compileSccMorph(maximum);
    require(maximum_result.valid && maximum_result.used == 0,
            "maximum intermediate count fills every available time slot");
    require(maximum_result.timbre.layers[0].timbre_automation.size() == 21,
            "maximum count retains both endpoints plus every intermediate");

    const auto byte_maximum = generateSccMorph(first, first, 255);
    require(byte_maximum.intermediate.size() == 255,
            "uint8 maximum intermediate count does not wrap its generation loop");
    require(std::all_of(byte_maximum.intermediate.begin(),
                        byte_maximum.intermediate.end(),
                        [&](const auto& wave) { return wave == first; }),
            "all byte-maximum identical-source intermediates reuse exact bytes");
    auto long_timeline = morphProgram(first, first, 0, 65'534, 255);
    const auto long_result = compileSccMorph(long_timeline);
    require(long_result.valid && long_result.used == 0,
            "255 identical intermediates fit the maximum envelope timeline");
    const auto& long_events = long_result.timbre.layers[0].timbre_automation;
    require(long_events.size() == 257,
            "long timeline contains both endpoints and all 255 intermediates");
    require(long_events.front().count == 0
                && long_events.front().target_library_id
                    == long_timeline.layers[0].timbre_automation.front().target_library_id
                && long_events.back().count == 65'534
                && long_events.back().target_library_id
                    == long_timeline.layers[0].timbre_automation.back().target_library_id,
            "byte-maximum expansion keeps both authored endpoint identities and counts");
    for (std::size_t i = 1; i < long_events.size(); ++i) {
        require(long_events[i - 1].count < long_events[i].count,
                "maximum-count insertion remains strictly increasing");
    }

    auto bank_zero = morphProgram(first, last, 0, 20, 2, 0);
    const auto bank_zero_result = compileSccMorph(bank_zero);
    require(bank_zero_result.valid && bank_zero_result.capacity == 30,
            "@0 bank capacity accounts for two reserved source numbers");
    for (const auto& diagnostic : bank_zero_result.diagnostics) {
        if (diagnostic.generated) {
            require(diagnostic.output_number <= 31
                        && diagnostic.output_number != 15
                        && diagnostic.output_number != 16,
                    "@0 allocation leaves existing source slots untouched");
        }
    }
    std::vector<const SccMorphWaveDiagnostic*> derived;
    for (const auto& diagnostic : bank_zero_result.diagnostics)
        if (diagnostic.generated) derived.push_back(&diagnostic);
    for (std::size_t i = 0; i < derived.size(); ++i) {
        const auto wave_i = materializedWaveFor(bank_zero_result.timbre, *derived[i]);
        for (std::size_t j = i + 1; j < derived.size(); ++j) {
            const auto wave_j = materializedWaveFor(bank_zero_result.timbre, *derived[j]);
            require((wave_i == wave_j)
                        == (derived[i]->output_number == derived[j]->output_number),
                    "only exact byte-identical derived waves share a bank number");
        }
    }

    SccWaveform overflow_first{};
    SccWaveform overflow_last{};
    for (std::size_t i = 0; i < 32; ++i) {
        overflow_last[i] = static_cast<std::int8_t>(i % 2 == 0 ? -120 : 120);
    }
    auto overflow = morphProgram(overflow_first, overflow_last, 0, 20, 17, 16);
    const auto overflow_result = compileSccMorph(overflow);
    require(!overflow_result.valid,
            "bank overflow is rejected during morph compilation");
    require(overflow_result.required > overflow_result.capacity,
            "overflow result reports required and available slot counts");
    require(overflow_result.timbre == overflow,
            "invalid compilation returns the unmaterialized source model");
}

void testMorphBlockBoundaryAnchors() {
    const auto a = sawWave();
    const auto shifted = rotateSccWaveform(a, 7);
    const auto x = sawWave(3, 5);
    const auto y = sawWave(5, -4);
    const auto fixed_result = compileSccMorph(
        blockProgram(x, a, shifted, a, y));
    require(fixed_result.valid, "bounded multi-transition Morph Block compiles");
    require(diagnosticAt(fixed_result, 2).phase_shift == 0,
            "block start stays fixed when a normal event precedes it");
    require(diagnosticAt(fixed_result, 14).phase_shift == 0,
            "block end stays fixed when a normal event follows it");
    require(diagnosticAt(fixed_result, 8).phase_shift != 0,
            "internal keyframe can move to improve both adjacent transitions");

    auto single_free = morphProgram(a, shifted, 0, 12, 0);
    single_free.layers[0].timbre_automation[0].count = 0;
    const auto free_result = compileSccMorph(single_free);
    require(free_result.valid, "unbounded curve-start and curve-end block compiles");
    require(diagnosticAt(free_result, 0).phase_shift != 0
                || diagnosticAt(free_result, 12).phase_shift != 0,
            "free block endpoints permit relative phase alignment");
}

void testMorphAcrossEnvelopeLoopBrackets() {
    auto source = blockProgram(sawWave(3, 2), sawWave(), rotateSccWaveform(sawWave(), 5),
                               sawWave(3, 2), sawWave(5, -3));
    auto& layer = source.layers[0];
    layer.envelope_timeline.loop_start_count = 8;
    layer.envelope_timeline.loop_end_count = 18;
    auto& events = layer.timbre_automation;
    events[1].count = 4;
    events[1].after_loop_start = false;
    events[2].count = 12;
    events[2].after_loop_start = true;
    events[2].scc_morph.intermediate_count = 3;
    events[3].count = 20;
    events[3].after_loop_start = false;
    events[3].scc_morph.intermediate_count = 3;
    events[4].count = 24;
    events[4].after_loop_start = false;

    const auto compiled = compileSccMorph(source);
    require(compiled.valid, "Morph Block may cross both existing ENV brackets");
    const auto& materialized = compiled.timbre.layers[0].timbre_automation;
    const auto find_event = [&](std::uint32_t count) -> const EnvelopeEvent& {
        const auto found = std::find_if(materialized.begin(), materialized.end(),
            [count](const auto& event) {
                return event.kind == EnvelopeEventKind::Timbre
                    && event.count == count;
            });
        if (found == materialized.end())
            throw std::runtime_error("missing materialized loop-crossing tone event");
        return *found;
    };
    require(!find_event(4).after_loop_start
                && find_event(12).after_loop_start
                && !find_event(20).after_loop_start,
            "authored endpoint loop-zone flags remain unchanged");
    for (const auto count : {6U, 8U, 10U, 14U, 16U, 18U}) {
        (void)find_event(count);
    }
    require(find_event(8).after_loop_start,
            "generated @ at loop start is ordered after the opening bracket");
    require(find_event(18).after_loop_start,
            "generated @ at loop end is ordered before the closing bracket");

    const auto formatted = formatMgsComposite(source);
    require(formatted.valid(), "loop-crossing Morph Block formats as MGSC");
    const auto parsed = parseMgsComposite(formatted.source);
    require(parsed.valid(), "loop-crossing MGSC output parses");
    const auto& parsed_events = parsed.timbre.layers[0].timbre_automation;
    const auto parsed_find = [&](std::uint32_t count) -> const EnvelopeEvent& {
        const auto found = std::find_if(parsed_events.begin(), parsed_events.end(),
            [count](const auto& event) {
                return event.kind == EnvelopeEventKind::Timbre
                    && event.count == count;
            });
        if (found == parsed_events.end()) {
            std::string actual;
            for (const auto& event : parsed_events)
                if (event.kind == EnvelopeEventKind::Timbre)
                    actual += " " + std::to_string(event.count);
            throw std::runtime_error("MGSC parse lost a loop-crossing tone event at "
                + std::to_string(count) + "; actual counts:" + actual
                + "\n" + formatted.source);
        }
        return *found;
    };
    require(parsed_find(8).after_loop_start && parsed_find(18).after_loop_start,
            "MGSC keeps generated tone changes on the ENV bracket boundaries");
    require(!parsed_find(4).after_loop_start
                && parsed_find(12).after_loop_start,
            "MGSC round-trip preserves reachable authored endpoint loop zones");
    require(parsed.timbre.layers[0].envelope_timeline.loop_end_count == 18
                && std::none_of(parsed_events.begin(), parsed_events.end(),
                    [](const auto& event) { return event.count > 18; }),
            "existing repeating ENV excludes unreachable events after loop end");
}

void testSeparateMorphBlocksKeepNormalTransition() {
    CompositeTimbre input;
    CompositeLayer layer;
    layer.name = "SCC";
    layer.source = TimbreSource::Scc;
    layer.channel = 0;
    const std::array<SavedTimbreReference, 5> tones{
        sccTone(3001, 15, sawWave(3, 2)),
        sccTone(3002, 16, sawWave()),
        sccTone(3003, 17, rotateSccWaveform(sawWave(), 5)),
        sccTone(3004, 18, sawWave(2, -2)),
        sccTone(3005, 19, sawWave(5, 4)),
    };
    layer.base_timbre = tones[0];
    layer.envelope_timeline.length_counts = 30;
    layer.timbre_automation = {
        toneEvent(tones[0].library_id, 0),
        toneEvent(tones[1].library_id, 2),
        toneEvent(tones[2].library_id, 8,
                  SccMorphTransition{.enabled = true,
                                     .intermediate_count = 1,
                                     .curve = 1.0}),
        toneEvent(tones[3].library_id, 14),
        toneEvent(tones[4].library_id, 22,
                  SccMorphTransition{.enabled = true,
                                     .intermediate_count = 1,
                                     .curve = 1.0}),
    };
    input.layers.push_back(std::move(layer));
    input.embedded_timbres.assign(tones.begin() + 1, tones.end());

    const auto compiled = compileSccMorph(input);
    require(compiled.valid, "two isolated morph transitions compile");
    const auto& events = compiled.timbre.layers[0].timbre_automation;
    require(events.size() == 7,
            "only the two enabled transitions receive intermediate events");
    require(std::none_of(events.begin(), events.end(), [](const auto& event) {
                return event.kind == EnvelopeEventKind::Timbre
                    && event.count > 8 && event.count < 14;
            }), "normal @ transition between morph blocks remains untouched");
    require(diagnosticAt(compiled, 2).phase_shift == 0
                && diagnosticAt(compiled, 8).phase_shift == 0
                && diagnosticAt(compiled, 14).phase_shift == 0,
            "normal tone events fix the adjacent independent block boundaries");
}

void testDormantRateMorph() {
    auto dormant = morphProgram(sawWave(), sawWave(3, 8));
    dormant.layers[0].volume_envelope.kind = EnvelopeKind::Rate;
    dormant.layers[0].timbre_automation.erase(
        dormant.layers[0].timbre_automation.begin());
    // The retained arrival now lacks a predecessor and references a missing
    // snapshot. Neither condition belongs to the active Rate program.
    dormant.embedded_timbres.clear();
    dormant.layers[0].timbre_automation.push_back(toneEvent(0, 1));
    dormant.layers[0].timbre_automation.back().value = 0;
    const auto authored = dormant;
    const auto result = compileSccMorph(dormant);
    require(result.valid && result.used == 0 && result.capacity == 31,
            "dormant Rate tone events neither fail nor reserve Bank slots");
    require(result.timbre == authored && dormant == authored,
            "Rate compilation preserves authored dormant lane");
    EngineCore engine;
    require(compileCompositeProgram(engine, dormant),
            "Rate playback ignores unresolved dormant arrival");
    require(formatMgsComposite(dormant).valid(),
            "Rate export ignores invalid dormant morph");
    dormant.layers[0].volume_envelope.kind = EnvelopeKind::Sequence;
    require(!compileSccMorph(dormant).valid,
            "switching back to Sequence revalidates retained morph");

    auto active = morphProgram(sawWave(), sawWave(3, 8), 0, 40, 6, 16);
    auto mixed = active;
    auto retained = morphProgram(sawWave(), sawWave(5, 4), 0, 300, 255, 16);
    retained.layers[0].channel = 1;
    retained.layers[0].envelope_number = 1;
    retained.layers[0].volume_envelope.kind = EnvelopeKind::Rate;
    retained.layers[0].timbre_automation.back().target_library_id = 999999;
    mixed.layers.push_back(retained.layers[0]);
    const auto only_active = compileSccMorph(active);
    const auto both = compileSccMorph(mixed);
    require(only_active.valid && both.valid,
            "large dormant morph cannot overflow another layer's Bank");
    require(both.used == only_active.used && both.capacity == only_active.capacity
                && both.timbre.layers[0] == only_active.timbre.layers[0],
            "dormant morph does not change active waves, timing or allocation");
    require(both.timbre.layers[1].timbre_automation
                == retained.layers[0].timbre_automation,
            "materializing another layer retains dormant events unchanged");
    require(compileCompositeProgram(engine, mixed) && formatMgsComposite(mixed).valid(),
            "mixed Rate and Sequence program plays and exports");
}

void testCacheInvalidationAndSaveRoundTrips() {
    const auto first = sawWave();
    const auto last = sawWave(3, 8);
    auto input = morphProgram(first, last, 0, 20, 2, 0, 1.5);
    auto legacy_input = input;
    legacy_input.scc_morph_algorithm_version = 1;
    auto future_input = input;
    future_input.scc_morph_algorithm_version = 3;
    const auto legacy_compiled = compileSccMorph(legacy_input);
    const auto current_compiled = compileSccMorph(input);
    require(legacy_compiled.valid && current_compiled.valid,
            "stored SCC morph algorithms 1 and 2 remain supported");
    require(legacy_input.scc_morph_algorithm_version == 1
                && input.scc_morph_algorithm_version == kSccMorphAlgorithmVersion,
            "compilation does not rewrite authored algorithm-version metadata");
    require(legacy_compiled.timbre.scc_morph_algorithm_version
                == kSccMorphAlgorithmVersion
                && current_compiled.timbre.scc_morph_algorithm_version
                    == kSccMorphAlgorithmVersion,
            "derived SCC morph result is stamped with the current generator version");
    const auto generatedSequence = [](const SccMorphCompileResult& result) {
        std::vector<std::pair<std::uint32_t, SccWaveform>> sequence;
        for (const auto& diagnostic : result.diagnostics) {
            if (!diagnostic.generated) continue;
            sequence.emplace_back(diagnostic.count,
                materializedWaveFor(result.timbre, diagnostic));
        }
        return sequence;
    };
    require(generatedSequence(legacy_compiled) == generatedSequence(current_compiled),
            "algorithm 1 and current metadata use the same deterministic current generator");
    const auto unsupported = compileSccMorph(future_input);
    require(!unsupported.valid
                && unsupported.timbre == future_input
                && future_input.scc_morph_algorithm_version == 3,
            "unknown future morph algorithm version is rejected without mutating input");

    for (const auto* authored : {&legacy_input, &input}) {
        std::string compatibility_error;
        const auto old_format = CompositeTimbreLibrary::serializeTimbreFile(
            *authored, 1);
        const auto old_round_trip = CompositeTimbreLibrary::deserializeTimbreFile(
            old_format, &compatibility_error);
        require(old_round_trip.has_value()
                    && old_round_trip->scc_morph_algorithm_version
                        == authored->scc_morph_algorithm_version,
                "portable project round-trip preserves legacy and current morph versions");
        const auto payload_round_trip = deserializeCompositeSoundPayload(
            serializeCompositeSoundPayload(*authored),
            kCompositeSoundPayloadVersion, &compatibility_error);
        require(payload_round_trip.has_value()
                    && payload_round_trip->scc_morph_algorithm_version
                        == authored->scc_morph_algorithm_version,
                "persistent payload round-trip preserves legacy and current morph versions");
    }
    auto saved = input;
    saved.scc_morph_materialized = true;

    const auto portable = CompositeTimbreLibrary::serializeTimbreFile(saved, 1);
    std::string error;
    const auto portable_round_trip =
        CompositeTimbreLibrary::deserializeTimbreFile(portable, &error);
    require(portable_round_trip.has_value(), "portable morph file loads");
    require(portable_round_trip->scc_morph_bank_base == saved.scc_morph_bank_base
                && portable_round_trip->scc_morph_algorithm_version
                    == saved.scc_morph_algorithm_version,
            "portable file retains Morph Bank and algorithm version");
    require(portable_round_trip->layers[0].timbre_automation[1].scc_morph
                == saved.layers[0].timbre_automation[1].scc_morph,
            "portable file retains the incoming transition settings");
    require(!portable_round_trip->scc_morph_materialized,
            "transient compiled marker is not saved in a portable file");

    const auto blob = serializeCompositeSoundPayload(saved);
    const auto payload_round_trip = deserializeCompositeSoundPayload(
        blob, kCompositeSoundPayloadVersion, &error);
    require(payload_round_trip.has_value(), "SQLite sound payload loads");
    require(payload_round_trip->scc_morph_bank_base == saved.scc_morph_bank_base
                && payload_round_trip->scc_morph_algorithm_version
                    == saved.scc_morph_algorithm_version
                && payload_round_trip->layers[0].timbre_automation[1].scc_morph
                    == saved.layers[0].timbre_automation[1].scc_morph,
            "SQLite payload retains morph settings");
    require(!payload_round_trip->scc_morph_materialized,
            "transient compiled marker is not saved in SQLite");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / ("mgstc_scc_morph_" + std::to_string(unique) + ".sqlite");
    CompositeTimbreLibrary composites;
    const auto id = composites.add(saved, 10);
    TimbreLibrary timbres;
    {
        ToneLibraryDatabase database;
        require(database.open(path.string(), &error), "SQLite morph database opens");
        require(database.replaceAll(timbres, composites, &error),
                "SQLite morph composite saves");
    }
    CompositeTimbreLibrary loaded;
    {
        ToneLibraryDatabase database;
        require(database.open(path.string(), &error), "SQLite morph database reopens");
        require(database.load(timbres, loaded, &error),
                "SQLite morph composite loads");
    }
    const auto* entry = loaded.find(id);
    require(entry != nullptr, "SQLite morph entry exists after reload");
    require(entry->timbre.layers[0].timbre_automation[1].scc_morph
                == saved.layers[0].timbre_automation[1].scc_morph
                && entry->timbre.scc_morph_bank_base == saved.scc_morph_bank_base,
            "SQLite database round-trip preserves morph settings");
    require(!entry->timbre.scc_morph_materialized,
            "SQLite database omits transient generated state");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto cached = compileSccMorphCached(input);
    auto same = compileSccMorphCached(input);
    require(cached == same, "unchanged source reuses a prepared morph cache");
    auto changed = input;
    changed.layers[0].timbre_automation[1].count++;
    auto changed_cache = compileSccMorphCached(changed);
    require(changed_cache != cached,
            "moving a tone event invalidates the prepared morph cache");
    changed = input;
    changed.layers[0].timbre_automation[1].scc_morph.enabled = false;
    auto off_cache = compileSccMorphCached(changed);
    require(off_cache != cached,
            "morph on/off changes invalidate the prepared morph cache");
    changed = input;
    changed.layers[0].timbre_automation[1].scc_morph.intermediate_count++;
    auto count_cache = compileSccMorphCached(changed);
    require(count_cache != cached,
            "intermediate count changes invalidate the prepared morph cache");
    changed = input;
    changed.layers[0].timbre_automation[1].scc_morph.curve = 4.0;
    auto curve_cache = compileSccMorphCached(changed);
    require(curve_cache != cached,
            "curve changes invalidate the prepared morph cache");
    changed = input;
    changed.scc_morph_bank_base = 16;
    auto bank_cache = compileSccMorphCached(changed);
    require(bank_cache != cached,
            "Morph Bank changes invalidate the prepared morph cache");
    changed = input;
    changed.embedded_timbres[0].scc_waveform[0]++;
    auto waveform_cache = compileSccMorphCached(changed);
    require(waveform_cache != cached,
            "source waveform edits invalidate the prepared morph cache");
    changed = input;
    changed.layers[0].timbre_automation.pop_back();
    auto deleted_cache = compileSccMorphCached(changed);
    require(deleted_cache != cached,
            "deleting the destination tone event invalidates the cache");
    changed = input;
    changed.layers[0].timbre_automation.insert(
        changed.layers[0].timbre_automation.begin() + 1,
        toneEvent(changed.layers[0].timbre_automation[0].target_library_id, 5));
    auto added_cache = compileSccMorphCached(changed);
    require(added_cache != cached,
            "adding a tone event invalidates the prepared morph cache");
    changed = input;
    std::reverse(changed.layers[0].timbre_automation.begin(),
                 changed.layers[0].timbre_automation.end());
    auto reordered_cache = compileSccMorphCached(changed);
    require(reordered_cache != cached,
            "reordering tone events invalidates the cache");
}

void testMaterializedMgscOutput() {
    for (int fixture = 0; fixture < 3; ++fixture) {
    const auto bank_base = static_cast<std::uint8_t>(fixture == 1 ? 16 : 0);
    auto source = morphProgram(sawWave(), sawWave(3, 8), 0, 20, 2, bank_base);
    if (fixture == 2) {
        source.layers[0].base_timbre->manual_number = 0;
        source.embedded_timbres[0].manual_number = 2;
    }
    const auto compiled = compileSccMorph(source);
    require(compiled.valid, "MGSC fixture morph compiles");
    const auto formatted = formatMgsComposite(source);
    require(formatted.valid(), "MGSC formatting compiles the morph");
    const auto parsed = parseMgsComposite(formatted.source);
    require(parsed.valid(), "materialized MGSC output parses");
    const auto& expected_events = compiled.timbre.layers[0].timbre_automation;
    const auto& parsed_events = parsed.timbre.layers[0].timbre_automation;
    require(parsed_events.size() == expected_events.size(),
            "MGSC retains every generated and authored SCC tone event");
    for (std::size_t i = 0; i < expected_events.size(); ++i) {
        require(parsed_events[i].count == expected_events[i].count
                    && parsed_events[i].value == expected_events[i].value,
                "MGSC preserves materialized SCC event numbers and counts");
    }
    const auto definitions = extractMgsSourceDefinitions(formatted.source, 's');
    for (const auto& diagnostic : compiled.diagnostics) {
        if (!diagnostic.generated) {
            continue;
        }
        const auto found = std::find_if(definitions.begin(), definitions.end(),
            [&](const auto& definition) {
                return definition.number == diagnostic.output_number;
            });
        require(found != definitions.end(),
                "every generated output number has an MGSC SCC definition");
    }
    const auto recompiled = compileSccMorph(parsed.timbre);
    require(recompiled.valid, "MGSC-parsed program retains materializable morph state");
    require(!parsed.timbre.scc_morph_materialized,
            "normal MGSC import does not require a transient execution marker");
    for (const auto& event : parsed_events) {
        if (event.kind != EnvelopeEventKind::Timbre) continue;
        const auto* snapshot = findEmbeddedTimbreSnapshot(
            parsed.timbre, event.target_library_id);
        require(snapshot && snapshot->manual_number == event.value,
                "normal import owns every event waveform at its declared SCC number");
    }

    std::string error;
    const auto saved = CompositeTimbreLibrary::deserializeTimbreFile(
        CompositeTimbreLibrary::serializeTimbreFile(parsed.timbre, 1), &error);
    require(saved.has_value() && !saved->scc_morph_materialized,
            "imported execution bank survives portable save/load without a marker");
    const auto restored = deserializeCompositeSoundPayload(
        serializeCompositeSoundPayload(*saved), kCompositeSoundPayloadVersion, &error);
    require(restored.has_value() && !restored->scc_morph_materialized,
            "imported execution bank survives persistent sound-payload save/load");
    if (fixture == 2) {
        const auto restricted = resolveTimbreNumbers(*restored, 16, 31);
        require(!restricted.valid(),
                "an explicit restricted range rejects native SCC slots outside that range");
        require(std::all_of(restricted.assignments.begin(), restricted.assignments.end(),
            [](const auto& assignment) { return assignment.number >= 16; }),
                "custom allocation ranges are not silently widened for imported SCC numbers");
    }
    const auto reformatted = formatMgsComposite(*restored);
    require(reformatted.valid(), "normal imported MGSC re-exports without external snapshots");
    const auto redefinitions = extractMgsSourceDefinitions(reformatted.source, 's');
    require(redefinitions.size() == definitions.size(),
            "normal imported MGSC preserves its complete SCC definition bank");
    for (const auto& definition : definitions) {
        const auto found = std::find_if(redefinitions.begin(), redefinitions.end(),
            [&](const auto& item) { return item.number == definition.number; });
        require(found != redefinitions.end(), "MGSC re-export preserves declared SCC numbers");
        const auto decode = [](const auto& item) {
            return parseMgsSccDefinition("@s" + std::to_string(item.number)
                + " = {" + item.body + "}");
        };
        const auto original = decode(definition);
        const auto rewritten = decode(*found);
        require(original && rewritten && original->waveform == rewritten->waveform,
                "MGSC re-export preserves every declared waveform byte");
    }

    EngineCore preview_engine;
    EngineCore output_engine;
    require(compileCompositeProgram(preview_engine, source),
            "application preview compiles morph source");
    require(compileCompositeProgram(output_engine, *restored),
            "normal parsed and persisted MGSC model compiles for playback");
    require(preview_engine.session().queueNoteOn(3, 60)
                && output_engine.session().queueNoteOn(3, 60),
            "both preview and output models accept the SCC note");
    for (int tick = 0; tick < 22; ++tick) {
        require(preview_engine.session().processTick().ok()
                    && output_engine.session().processTick().ok(),
                "preview and materialized output advance without runtime faults");
        const auto preview_writes = preview_engine.session().writes();
        const auto output_writes = output_engine.session().writes();
        require(std::equal(preview_writes.begin(), preview_writes.end(),
                           output_writes.begin(), output_writes.end()),
                "preview and materialized MGSC model issue identical chip writes");
    }
    }

    // OPLL event-only definitions share the importer. This asymmetric order
    // used to assign @17 to @15, then overwrite it with the later @15 alias.
    CompositeTimbre opll;
    CompositeLayer layer;
    layer.source = TimbreSource::Opll;
    layer.channel = 0;
    layer.envelope_timeline.length_counts = 16;
    SavedTimbreReference base;
    base.library_id = 4100;
    base.source = TimbreSource::Opll;
    base.number_mode = TimbreNumberMode::Manual;
    base.manual_number = 16;
    base.opll_registers = {0x01, 0x11, 0x04, 0x07, 0xF2, 0xF3, 0x06, 0x17};
    auto first = base;
    first.library_id = 4101;
    first.manual_number = 17;
    first.opll_registers[2] = 0x12;
    auto last = base;
    last.library_id = 4102;
    last.manual_number = 15;
    last.opll_registers[2] = 0x27;
    layer.base_timbre = base;
    layer.timbre_automation = {
        toneEvent(first.library_id, 4), toneEvent(last.library_id, 10)};
    opll.layers.push_back(layer);
    opll.embedded_timbres = {first, last};
    const auto formatted = formatMgsComposite(opll);
    require(formatted.valid(), "asymmetric OPLL definition order formats");
    const auto parsed = parseMgsComposite(formatted.source);
    require(parsed.valid(), "event-only OPLL definitions import normally");
    const auto persisted = CompositeTimbreLibrary::deserializeTimbreFile(
        CompositeTimbreLibrary::serializeTimbreFile(parsed.timbre, 1));
    require(persisted.has_value(), "event-only OPLL definitions survive save/load");
    const auto reformatted = formatMgsComposite(*persisted);
    require(reformatted.valid(), "event-only OPLL definitions re-export normally");
    const auto original_definitions = extractMgsSourceDefinitions(formatted.source, 'v');
    const auto restored_definitions = extractMgsSourceDefinitions(reformatted.source, 'v');
    require(original_definitions.size() == 3
                && restored_definitions.size() == original_definitions.size(),
            "OPLL re-export retains base and both event-only definitions");
    for (const auto& definition : original_definitions) {
        const auto found = std::find_if(restored_definitions.begin(), restored_definitions.end(),
            [&](const auto& item) { return item.number == definition.number; });
        require(found != restored_definitions.end(), "OPLL re-export preserves declared numbers");
        const auto decode = [](const auto& item) {
            return parseMgsOpllDefinition("@v" + std::to_string(item.number)
                + " = {" + item.body + "}");
        };
        const auto original = decode(definition);
        const auto restored = decode(*found);
        require(original && restored && original->patch == restored->patch,
                "OPLL re-export preserves every declared patch byte");
    }
    EngineCore expected_engine;
    EngineCore imported_engine;
    require(compileCompositeProgram(expected_engine, opll)
                && compileCompositeProgram(imported_engine, *persisted),
            "original and normally imported OPLL programs compile");
    require(expected_engine.session().queueNoteOn(8, 60)
                && imported_engine.session().queueNoteOn(8, 60),
            "both OPLL programs accept the same note");
    for (int tick = 0; tick < 12; ++tick) {
        require(expected_engine.session().processTick().ok()
                    && imported_engine.session().processTick().ok(),
                "both OPLL programs advance without faults");
        const auto expected = expected_engine.session().writes();
        const auto actual = imported_engine.session().writes();
        require(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()),
                "normal OPLL import preserves ordered runtime writes");
        if (tick == 4 || tick == 10) {
            const auto value = tick == 4 ? 0x12 : 0x27;
            require(std::any_of(actual.begin(), actual.end(), [&](const auto& write) {
                return write.chip == ChipId::Opll && write.address == 2 && write.value == value;
            }), "OPLL event selects its declared waveform instead of an overwritten alias");
        }
    }
}

struct Fnv1a64 {
    std::uint64_t value{14'695'981'039'346'656'037ULL};

    void add(std::uint8_t byte) noexcept {
        value ^= byte;
        value *= 1'099'511'628'211ULL;
    }

    void addUnsigned(std::uint64_t number, std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < bytes; ++i)
            add(static_cast<std::uint8_t>(number >> (i * 8U)));
    }

    void addWave(const SccWaveform& wave) noexcept {
        for (const auto sample : wave)
            add(std::bit_cast<std::uint8_t>(sample));
    }

    void addGenerated(const SccMorphPairResult& generated) noexcept {
        addUnsigned(generated.intermediate.size(), 4);
        for (const auto& wave : generated.intermediate)
            addWave(wave);
    }
};

void printSccMorphDigest() {
    Fnv1a64 digest;
    const auto first = sawWave();
    const auto shifted = rotateSccWaveform(first, 9);
    const SccWaveform silence{};
    SccWaveform dc{};
    dc.fill(24);
    const auto other = sawWave(3, 8);

    digest.addGenerated(generateSccMorph(first, first, 7, 1.0));
    digest.addGenerated(generateSccMorph(first, shifted, 7, kSccMorphGammaMax));
    digest.addGenerated(generateSccMorph(silence, dc, 7, 1.0));
    digest.addGenerated(generateSccMorph(first, other, 7, 1.0));
    digest.addGenerated(generateSccMorph(first, other, 7, kSccMorphGammaMax));

    auto program = morphProgram(first, other, 0, 20, 7, 16,
                                kSccMorphGammaMax);
    const auto bank = compileSccMorph(program);
    require(bank.valid, "canonical diagnostic bank compiles");
    digest.addUnsigned(bank.timbre.scc_morph_bank_base, 1);
    digest.addUnsigned(bank.capacity, 4);
    digest.addUnsigned(bank.required, 4);
    digest.addUnsigned(bank.used, 4);
    digest.addUnsigned(bank.diagnostics.size(), 4);
    for (const auto& diagnostic : bank.diagnostics) {
        digest.addUnsigned(diagnostic.count, 4);
        digest.addUnsigned(static_cast<std::uint64_t>(
            std::llround(diagnostic.u * 1'000'000'000.0)), 8);
        digest.addUnsigned(diagnostic.output_number, 1);
        digest.addUnsigned(diagnostic.phase_shift, 1);
        digest.add(diagnostic.generated ? 1 : 0);
    }
    std::array<bool, 32> generated_numbers{};
    for (const auto& diagnostic : bank.diagnostics)
        if (diagnostic.generated && diagnostic.output_number < generated_numbers.size())
            generated_numbers[diagnostic.output_number] = true;
    std::size_t definition_count{};
    for (const bool generated : generated_numbers)
        if (generated) ++definition_count;
    digest.addUnsigned(definition_count, 4);
    for (std::size_t number = 0; number < generated_numbers.size(); ++number) {
        if (!generated_numbers[number]) continue;
        const auto found = std::find_if(bank.timbre.embedded_timbres.begin(),
            bank.timbre.embedded_timbres.end(), [number](const auto& tone) {
                return tone.source == TimbreSource::Scc
                    && tone.number_mode == TimbreNumberMode::Manual
                    && tone.manual_number == number;
            });
        require(found != bank.timbre.embedded_timbres.end(),
                "canonical generated bank number resolves to a definition");
        digest.addUnsigned(number, 1);
        SccWaveform wave{};
        for (std::size_t i = 0; i < wave.size(); ++i)
            wave[i] = std::bit_cast<std::int8_t>(found->scc_waveform[i]);
        digest.addWave(wave);
    }
    digest.addUnsigned(bank.timbre.layers[0].timbre_automation.size(), 4);
    for (const auto& event : bank.timbre.layers[0].timbre_automation) {
        if (event.kind != EnvelopeEventKind::Timbre) continue;
        digest.addUnsigned(event.count, 4);
        digest.addUnsigned(static_cast<std::uint32_t>(event.value), 4);
    }
    std::cout << "SCC morph digest: " << std::hex << std::setw(16)
              << std::setfill('0') << digest.value << std::dec << '\n';
}

// Opt-in Phase A/B waveform and playback-timing comparisons.
#include "scc_morph_diagnostics.inc"

} // namespace

int main() {
    try {
        testCurveAndQuantizedGeneration();
        testSilenceDcAndPhaseOnlyInputs();
        testSccMorphPhaseBMatrixAndTiming();
        testGlobalPhaseOptimizationAndTransitionCost();
        testContiguousChannelAllocation();
        testCompositeCompilationBoundariesAndAllocation();
        testMorphBlockBoundaryAnchors();
        testMorphAcrossEnvelopeLoopBrackets();
        testSeparateMorphBlocksKeepNormalTransition();
        testDormantRateMorph();
        testCacheInvalidationAndSaveRoundTrips();
        testMaterializedMgscOutput();
        testSccMorphDiagnosticFixture();
        printSccMorphDigest();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SCC morph regression failed: %s\n", error.what());
        return 1;
    }
}
