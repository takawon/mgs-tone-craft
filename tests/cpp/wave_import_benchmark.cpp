#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/wave_import.hpp"
#include "mgstc/engine/detail/opll_perceptual_test_access.hpp"

namespace {

using Clock = std::chrono::steady_clock;

enum class BenchmarkCase {
    Scc,
    WavShort,
    WavTwoSeconds,
    All,
};

struct CommandLineOptions {
    mgstc::engine::OpllApproximationEffort effort{
        mgstc::engine::OpllApproximationEffort::Standard};
    std::size_t workers{8};
    std::size_t runs{3};
    BenchmarkCase benchmark_case{BenchmarkCase::All};
};

struct RunSummary {
    double milliseconds{};
    mgstc::engine::OpllApproximationCompletion completion{
        mgstc::engine::OpllApproximationCompletion::Cancelled};
    std::size_t candidate_count{};
    mgstc::engine::OpllApproximationProgress progress{};
    std::uint64_t candidate_hash{};
};

void printUsage(std::ostream& output) {
    output
        << "Usage: wave_import_benchmark"
        << " [--effort standard|thorough] [--workers N] [--runs N]"
        << " [--case scc|wav-short|wav-2s|all]\n";
}

bool parsePositiveSize(std::string_view text, std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t parsed{};
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

bool parseCommandLine(
    int argc,
    char* argv[],
    CommandLineOptions& options,
    std::string& error) {
    bool runs_was_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            error = "missing value for " + std::string{argument};
            return false;
        }
        const std::string_view value{argv[++index]};
        if (argument == "--effort") {
            if (value == "standard") {
                options.effort =
                    mgstc::engine::OpllApproximationEffort::Standard;
            } else if (value == "thorough") {
                options.effort =
                    mgstc::engine::OpllApproximationEffort::Thorough;
            } else {
                error = "invalid --effort value: " + std::string{value};
                return false;
            }
        } else if (argument == "--workers") {
            if (!parsePositiveSize(value, options.workers)
                || options.workers > 8) {
                error = "--workers must be an integer from 1 to 8";
                return false;
            }
        } else if (argument == "--runs") {
            if (!parsePositiveSize(value, options.runs)) {
                error = "--runs must be a positive integer";
                return false;
            }
            runs_was_set = true;
        } else if (argument == "--case") {
            if (value == "scc") {
                options.benchmark_case = BenchmarkCase::Scc;
            } else if (value == "wav-short") {
                options.benchmark_case = BenchmarkCase::WavShort;
            } else if (value == "wav-2s") {
                options.benchmark_case = BenchmarkCase::WavTwoSeconds;
            } else if (value == "all") {
                options.benchmark_case = BenchmarkCase::All;
            } else {
                error = "invalid --case value: " + std::string{value};
                return false;
            }
        } else {
            error = "unknown argument: " + std::string{argument};
            return false;
        }
    }
    if (!runs_was_set
        && options.effort
            == mgstc::engine::OpllApproximationEffort::Thorough) {
        options.runs = 1;
    }
    return true;
}

std::uint64_t hashCandidates(
    const std::vector<mgstc::engine::OpllPatchParameters>& candidates) {
    constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = offset_basis;
    for (const auto& candidate : candidates) {
        for (const std::uint8_t value :
             mgstc::engine::encodeOpllPatch(candidate)) {
            hash ^= value;
            hash *= prime;
        }
    }
    return hash;
}

double medianMilliseconds(std::vector<double> elapsed) {
    std::sort(elapsed.begin(), elapsed.end());
    const std::size_t middle = elapsed.size() / 2;
    if (elapsed.size() % 2 == 0) {
        return (elapsed[middle - 1] + elapsed[middle]) / 2.0;
    }
    return elapsed[middle];
}

const char* completionName(
    mgstc::engine::OpllApproximationCompletion completion) {
    return completion == mgstc::engine::OpllApproximationCompletion::Completed
        ? "completed"
        : "cancelled";
}

mgstc::engine::SccWaveform benchmarkSccWaveform() {
    mgstc::engine::SccWaveform waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        const double phase =
            2.0 * std::numbers::pi * static_cast<double>(index)
            / static_cast<double>(waveform.size());
        waveform[index] = static_cast<std::int8_t>(std::lround(
            std::clamp(
                std::sin(phase)
                    + 0.42 * std::sin(phase * 2.0)
                    + 0.23 * std::sin(phase * 5.0),
                -1.0,
                1.0)
            * 127.0));
    }
    return waveform;
}

mgstc::engine::WavePcm benchmarkWavePcm(double seconds) {
    mgstc::engine::WavePcm pcm;
    pcm.sample_rate = 48'000;
    pcm.mono_samples.resize(static_cast<std::size_t>(
        std::llround(static_cast<double>(pcm.sample_rate) * seconds)));
    constexpr double frequency = 261.625565;
    for (std::size_t index = 0; index < pcm.mono_samples.size(); ++index) {
        const double time = static_cast<double>(index) / pcm.sample_rate;
        const double attack = std::min(1.0, time / 0.012);
        const double decay = std::exp(-time * 5.5);
        pcm.mono_samples[index] = static_cast<float>(
            attack * decay
            * (std::sin(2.0 * std::numbers::pi * frequency * time)
               + 0.3
                   * std::sin(
                       2.0 * std::numbers::pi * frequency * 3.0 * time)));
    }
    return pcm;
}

template <typename Function>
bool runBenchmarkCase(
    std::string_view name,
    const CommandLineOptions& command_line,
    Function&& function) {
    std::vector<double> elapsed;
    elapsed.reserve(command_line.runs);
    RunSummary reported{};
    std::uint64_t expected_hash{};
    bool successful = true;

    for (std::size_t run = 0; run < command_line.runs; ++run) {
        mgstc::engine::OpllApproximationOptions options;
        options.max_workers = command_line.workers;
        options.effort = command_line.effort;
        options.control =
            std::make_shared<mgstc::engine::OpllApproximationControl>();

        const auto begin = Clock::now();
        const mgstc::engine::OpllApproximationResult result =
            function(options);
        const auto end = Clock::now();

        RunSummary summary;
        summary.milliseconds =
            std::chrono::duration<double, std::milli>(end - begin).count();
        summary.completion = result.completion;
        summary.candidate_count = result.candidates.size();
        summary.progress = options.control->progress();
        summary.candidate_hash = hashCandidates(result.candidates);
        elapsed.push_back(summary.milliseconds);

        if (run == 0) {
            expected_hash = summary.candidate_hash;
        } else if (
            summary.candidate_hash != expected_hash
            || summary.candidate_count != reported.candidate_count) {
            std::cerr << name
                      << ": candidate output changed between runs\n";
            successful = false;
        }
        if (summary.completion
                != mgstc::engine::OpllApproximationCompletion::Completed
            || summary.candidate_count == 0) {
            successful = false;
        }
        reported = summary;
    }

    const double milliseconds = command_line.runs > 1
        ? medianMilliseconds(elapsed)
        : elapsed.front();
    std::cout << std::fixed << std::setprecision(1)
              << name
              << " completion=" << completionName(reported.completion)
              << ' ' << (command_line.runs > 1 ? "median_ms=" : "elapsed_ms=")
              << milliseconds
              << " candidates=" << reported.candidate_count
              << " progress=" << reported.progress.completed << '/'
              << reported.progress.total
              << " hash=0x" << std::hex << std::setw(16)
              << std::setfill('0') << reported.candidate_hash << std::dec
              << std::setfill(' ') << '\n';
    return successful;
}

bool loadWaveFixture(const char* file, mgstc::engine::WavePcm& pcm) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open WAV fixture\n";
        return false;
    }
    const std::vector<std::uint8_t> bytes(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    std::string error;
    if (!mgstc::engine::parseWavePcm(bytes, pcm, &error)) {
        std::cerr << error << '\n';
        return false;
    }
    return true;
}

void printRegisters(const std::array<std::uint8_t, 8>& registers);

void writeCandidateJson(const mgstc::engine::OpllContinuousCandidate& candidate) {
    const auto& s = candidate.breakdown;
    std::cout << "{\"registers\":\"";
    printRegisters(candidate.registers);
    std::cout << "\",\"score\":" << std::setprecision(17) << candidate.full_score
              << ",\"full_index\":" << candidate.full_evaluation_index
              << ",\"f0\":" << candidate.fundamental_hz
              << ",\"key_off\":" << candidate.key_off_sample << ",\"samples\":" << candidate.sample_count
              << ",\"spectrum\":" << s.spectrum << ",\"harmonics\":" << s.harmonics
              << ",\"periodic\":" << s.periodic_shape
              << ",\"harmonic_count\":" << static_cast<int>(s.harmonic_count)
              << ",\"fft\":" << s.harmonic_fft_size << ",\"radius\":" << s.harmonic_radius_bins
              << ",\"periodic_windows\":" << static_cast<int>(s.periodic_windows)
              << ",\"stage_mask\":" << static_cast<int>(s.stage_mask)
              << ",\"phase_shifts\":32,\"period_status\":[";
    for (std::size_t i = 0; i < s.period_status.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << static_cast<int>(s.period_status[i]);
    }
    std::cout << ']'
              << ",\"stage_envelope\":[";
    for (std::size_t i = 0; i < s.stage_envelope.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << s.stage_envelope[i];
    }
    std::cout << "],\"stage_timbre\":[";
    for (std::size_t i = 0; i < s.stage_timbre.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << s.stage_timbre[i];
    }
    std::cout << "]}";
}

int runContinuousInitial(const char* file, std::string_view schedule, bool timed = false) {
    namespace engine = mgstc::engine;
    if (schedule != "1" && schedule != "8" && schedule != "sync") return 2;
    engine::WavePcm pcm;
    if (!loadWaveFixture(file, pcm)) return 2;
    auto hooks = std::make_shared<engine::OpllContinuousTestHooks>();
    hooks->initial_only = !timed;
    if (!timed) hooks->time_limit = std::chrono::hours{2};
    hooks->force_synchronous_fallback = schedule == "sync";
    engine::OpllContinuousOptions options;
    std::vector<std::pair<engine::OpllContinuousCandidate, engine::OpllPublicationDecision>> decisions;
    hooks->publication_decided = [&](const auto& candidate, auto decision) {
        decisions.emplace_back(candidate, decision);
    };
    options.max_workers = schedule == "1" ? 1 : 8;
    options.test_hooks = std::move(hooks);
    const auto started = Clock::now();
    const auto result = engine::approximateWavePcmContinuouslyWithOpll(pcm, options);
    if (!timed && (result.completion != engine::OpllContinuousCompletion::InitialCompleted
        || !result.initial_completed || !result.initial_best || result.ranked.empty())) {
        std::cerr << "Initial exploration did not complete: " << result.error << '\n';
        return 1;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    const auto add = [&](std::uint8_t byte) { hash ^= byte; hash *= 1099511628211ULL; };
    add(1);
    add(static_cast<std::uint8_t>(engine::OpllApproximationCompletion::Completed));
    add(static_cast<std::uint8_t>(result.ranked.size()));
    for (const auto& candidate : result.ranked)
        for (const auto byte : candidate->registers) add(byte);
    std::cout << "{\"hash\":\"" << std::hex << std::setw(16) << std::setfill('0') << hash
              << std::dec << "\",\"representative\":" << result.representative_evaluations
              << ",\"short\":" << result.short_evaluations << ",\"full\":" << result.full_evaluations
              << ",\"version\":\"opll-full-perceptual-v1\""
              << ",\"completion\":" << static_cast<int>(result.completion)
              << ",\"seconds\":" << std::chrono::duration<double>(Clock::now() - started).count()
              << ",\"candidates\":[";
    for (std::size_t index = 0; index < result.ranked.size(); ++index) {
        if (index != 0) std::cout << ',';
        const auto& candidate = result.ranked[index];
        writeCandidateJson(*candidate);
    }
    std::cout << "],\"publication\":[";
    for (std::size_t i = 0; i < decisions.size(); ++i) {
        if (i != 0) std::cout << ',';
        std::cout << "{\"decision\":" << static_cast<int>(decisions[i].second) << ",\"candidate\":";
        writeCandidateJson(decisions[i].first);
        std::cout << '}';
    }
    std::cout << "]}\n";
    return !timed || result.completion == engine::OpllContinuousCompletion::TimedOut ? 0 : 1;
}

constexpr std::size_t kStationaryPeriod = 182;

mgstc::engine::WavePcm stationaryPcm(std::size_t frames, float amplitude) {
    mgstc::engine::WavePcm pcm{48'000, {}};
    pcm.mono_samples.reserve(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto position = (frame + 17) % kStationaryPeriod;
        const double phase = 2.0 * std::numbers::pi * position / kStationaryPeriod;
        pcm.mono_samples.push_back(amplitude
            * static_cast<float>(std::sin(phase) + 0.25 * std::sin(2.0 * phase)));
    }
    return pcm;
}

int runStationaryPcmChecks() {
    namespace engine = mgstc::engine;
    constexpr std::size_t frames = kStationaryPeriod * 14 + 23;
    const auto stationary = stationaryPcm(frames, 0.5F);
    const auto detected = engine::analyzeWaveCycle(stationary);
    const bool period_ok = detected.cycle.size() == kStationaryPeriod
        && std::abs(detected.estimated_frequency_hz - 48'000.0F / kStationaryPeriod) < 0.01F;
    std::cout << (period_ok ? "PASS" : "FAIL") << " ga-stationary-period-detected period="
              << detected.cycle.size() << " frequency_hz=" << detected.estimated_frequency_hz << '\n';

    const auto expected = engine::analyzeWaveCycle(stationaryPcm(kStationaryPeriod, 0.5F));
    const bool boundary_ok = std::equal(detected.cycle.begin(), detected.cycle.end(),
        expected.cycle.begin(), expected.cycle.end(),
        [](float left, float right) { return std::abs(left - right) < 1.0e-5F; });
    std::cout << (boundary_ok ? "PASS" : "FAIL") << " ga-cycle-boundary-partial-tail phase_offset=17"
              << " tail=23 frames=" << frames << '\n';

    const auto quiet = stationaryPcm(frames, 1.0e-9F);
    const auto fallback = engine::analyzeWaveCycle(quiet);
    const auto peak = std::max_element(fallback.cycle.begin(), fallback.cycle.end());
    const bool fallback_ok = fallback.cycle.size() == frames
        && peak != fallback.cycle.end() && *peak > 0.0F && *peak < 2.0e-9F;
    std::cout << (fallback_ok ? "PASS" : "FAIL") << " ga-period-detection-low-energy-fallback frames="
              << fallback.cycle.size() << " expected=" << frames
              << " peak=" << (peak == fallback.cycle.end() ? 0.0F : *peak) << '\n';
    if (!period_ok || !boundary_ok || !fallback_ok) return 1;

    auto hooks = std::make_shared<engine::OpllContinuousTestHooks>();
    hooks->initial_only = true;
    hooks->time_limit = std::chrono::hours{2};
    engine::OpllContinuousOptions options;
    options.fixed_midi_note = 60;
    options.test_hooks = std::move(hooks);
    const auto result = engine::approximateWavePcmContinuouslyWithOpll(quiet, options);
    const bool evaluations_ok = result.completion == engine::OpllContinuousCompletion::InitialCompleted
        && result.representative_evaluations == 48'000 && result.short_evaluations == 48'000
        && result.full_evaluations == 6'000 && !result.ranked.empty();
    const bool quiet_ok = evaluations_ok && std::all_of(result.ranked.begin(), result.ranked.end(),
        [&](const auto& candidate) {
            return std::isfinite(candidate->full_score) && candidate->fixed_midi_note == 60
                && candidate->sample_count == frames && candidate->key_off_sample == frames
                && candidate->registers == engine::encodeOpllPatch(candidate->patch);
        });
    std::cout << (quiet_ok ? "PASS" : "FAIL") << " ga-quiet-fixed-note-60 pcm_frames=" << frames
              << " candidates=" << result.ranked.size() << '\n';
    std::cout << (evaluations_ok ? "PASS" : "FAIL") << " ga-short-and-full-evaluation-complete"
              << " representative=" << result.representative_evaluations
              << " short=" << result.short_evaluations << " full=" << result.full_evaluations
              << " error=" << result.error << '\n';
    return quiet_ok ? 0 : 1;
}

void printRegisters(const std::array<std::uint8_t, 8>& registers) {
    for (const auto byte : registers)
        std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    std::cout << std::dec << std::setfill(' ');
}

int runPerceptualChecks() {
    using namespace mgstc::engine;
    constexpr std::size_t frames = 31'200, key_off = 21'600;
    constexpr float fundamental = 250.0F;
    const auto signal = [](float second, float third, double phase_shift,
                           bool wrong_attack, bool wrong_sustain) {
        std::vector<float> pcm(frames);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            const double t = static_cast<double>(i) / 48'000;
            const double phase = t * fundamental * 2.0 * std::numbers::pi + phase_shift;
            const float envelope = t < 0.05 ? static_cast<float>(t / 0.05)
                : t < 0.15 ? static_cast<float>(1.0 - 3.0 * (t - 0.05))
                : t < 0.45 ? 0.7F : static_cast<float>(0.7 * (1.0 - (t - 0.45) / 0.20));
            const float changed_level = wrong_attack && t < 0.05 ? 0.35F : 1.0F;
            const float changed_third = wrong_sustain && t >= 0.15 && t < 0.45 ? 0.75F : third;
            pcm[i] = envelope * changed_level * static_cast<float>(
                std::sin(phase) + second * std::sin(2.0 * phase) + changed_third * std::sin(3.0 * phase));
        }
        return pcm;
    };
    const auto reference = signal(0.35F, 0.0F, 0, false, false);
    const auto score = [&](const auto& pcm, std::size_t off = 21'600) {
        return detail::compareOpllFullPcmForRegression(reference, pcm, fundamental, off);
    };
    const auto identical = score(reference);
    const auto missing = score(signal(0, 0, 0, false, false));
    const auto extra = score(signal(0.35F, 0.7F, 0, false, false));
    const auto shifted = score(signal(0.35F, 0, std::numbers::pi, false, false));
    const auto attack = score(signal(0.35F, 0, 0, true, false));
    const auto sustain = score(signal(0.35F, 0, 0, false, true));
    const auto no_off = score(reference, frames);
    const std::vector<float> silence(frames, 1.0e-9F);
    const auto quiet = detail::compareOpllFullPcmForRegression(silence, silence, fundamental, frames);
    const auto short_note = detail::compareOpllFullPcmForRegression(
        std::span(reference).first(100), std::span(reference).first(100), fundamental, 100);
    std::vector<float> noise(3500);
    std::uint32_t noise_state = 1;
    for (auto& sample : noise) {
        noise_state = 1664525U * noise_state + 1013904223U;
        sample = static_cast<float>(noise_state >> 8U) / 8388608.0F - 1.0F;
    }
    const auto noisy = detail::compareOpllFullPcmForRegression(noise, noise, 300.0F, noise.size());
    std::vector<float> low(48'000), plateau(48'000);
    for (std::size_t i = 0; i < low.size(); ++i) {
        const auto t = static_cast<double>(i) / 48'000.0;
        low[i] = static_cast<float>(std::sin(t * 82.0 * std::numbers::pi));
        const double level = t < 0.025 ? t / 0.025 : t < 0.06 ? 1.0
            : t < 0.16 ? 1.0 - 7.0 * (t - 0.06) : 0.3;
        plateau[i] = static_cast<float>(level * std::sin(t * 440.0 * std::numbers::pi));
    }
    const auto low_steady = detail::compareOpllFullPcmForRegression(low, low, 41.0F, low.size());
    const auto delayed_decay = detail::compareOpllFullPcmForRegression(
        plateau, plateau, 220.0F, plateau.size());
    const auto patch = decodeOpllPatch(std::array<std::uint8_t, 8>{0x31,0x21,0x18,0x05,0xa4,0xc6,0x63,0x44});
    const auto known = detail::renderOpllFullPcmForRegression(patch, fundamental, frames, key_off);
    const auto known_equal = detail::compareOpllFullPcmForRegression(known, known, fundamental, key_off);
    constexpr std::size_t early_off = 1756;
    std::vector<float> short_held(early_off + 4800);
    for (std::size_t i = 0; i < short_held.size(); ++i) {
        const auto time = static_cast<double>(i) / 48'000.0;
        const auto release_time = static_cast<double>(i > early_off ? i - early_off : 0) / 48'000.0;
        short_held[i] = static_cast<float>(std::sin(time * 82.0 * std::numbers::pi)
            * std::exp(-40.0 * release_time));
    }
    const auto early_release = detail::compareOpllFullPcmForRegression(
        short_held, short_held, 41.0F, early_off);
    bool ok = true;
    const auto check = [&](bool pass, const char* name) {
        std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
        ok &= pass;
    };
    check(identical.total < 1.0e-5F && known_equal.total < 1.0e-5F, "full-PCM-and-OPLL-self");
    check(identical.harmonic_count == 20 && missing.harmonics > 0.15F
        && extra.harmonics > 0.15F, "full-missing-and-extra-harmonics");
    check(noisy.harmonic_count == 0 && noisy.periodic_windows == 0
        && std::isfinite(noisy.total), "full-short-broadband-noise-excludes-harmonics");
    check(identical.periodic_windows >= 2 && shifted.periodic_windows >= 2
        && shifted.periodic_shape < 0.01F, "full-half-period-shift");
    check(identical.stage_mask == 15 && attack.stage_envelope[0] > attack.stage_envelope[2]
        && sustain.stage_timbre[2] > sustain.stage_timbre[0] + 0.02F,
        "full-independent-ADSR");
    check((no_off.stage_mask & 8) == 0 && quiet.harmonic_count == 0
        && quiet.stage_mask == 0 && quiet.periodic_windows == 0 && std::isfinite(quiet.total)
        && short_note.periodic_windows == 0 && std::isfinite(short_note.total),
        "full-absent-release-quiet-short");
    check(low_steady.stage_mask == 4 && delayed_decay.stage_mask == 7,
        "full-low-pitch-and-early-plateau-stage-detection");
    check(early_release.stage_mask == 8 && early_release.total < 1.0e-5F,
        "full-release-after-short-held-note");
    check(detail::checkOpllPerceptualGateForRegression(identical, missing)
            == OpllPublicationDecision::HarmonicsRegressed
        && detail::checkOpllPerceptualGateForRegression(missing, identical)
            == OpllPublicationDecision::Published
        && detail::checkOpllPerceptualGateForRegression(identical, attack)
            == OpllPublicationDecision::StageRegressed, "full-publish-gate-reject-and-recover");
    check(detail::checkOpllPublicationHistoryForRegression(),
        "full-publication-history-independent-of-internal-ranking");
    std::cout << "stage_mask=" << static_cast<int>(identical.stage_mask)
              << " attack_error=" << attack.stage_envelope[0]
              << " sustain_timbre_error=" << sustain.stage_timbre[2]
              << " periodic=" << shifted.periodic_shape << '\n';
    return ok ? 0 : 1;
}

int renderPerceptualFixture(const char* hex, const char* frequency, const char* length,
    const char* key_off, const char* filename) {
    namespace engine = mgstc::engine;
    const std::string_view digits{hex};
    float fundamental{};
    std::size_t frames{}, off{};
    const auto parse = [](std::string_view text, auto& value) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    if (digits.size() != 16 || !parse(frequency, fundamental) || !std::isfinite(fundamental)
        || fundamental < 20 || fundamental > 6000 || !parse(length, frames)
        || frames == 0 || frames > 96000 || !parse(key_off, off) || off > frames) return 2;
    std::array<std::uint8_t, 8> registers{};
    for (std::size_t i = 0; i < registers.size(); ++i) {
        const auto* first = digits.data() + 2 * i;
        const auto result = std::from_chars(first, first + 2, registers[i], 16);
        if (result.ec != std::errc{} || result.ptr != first + 2) return 2;
    }
    const auto pcm = engine::detail::renderOpllFullPcmForRegression(
        engine::decodeOpllPatch(registers), fundamental, frames, off);
    std::ofstream file(filename, std::ios::binary);
    const auto number = [&](std::uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) file.put(static_cast<char>((value >> (8 * i)) & 0xffU));
    };
    file.write("RIFF", 4); number(static_cast<std::uint32_t>(36 + frames * 2), 4);
    file.write("WAVEfmt ", 8); number(16, 4);
    number(1, 2); number(1, 2); number(48'000, 4); number(96'000, 4); number(2, 2); number(16, 2);
    file.write("data", 4); number(static_cast<std::uint32_t>(frames * 2), 4);
    for (float sample : pcm) {
        const auto signed_sample = static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0F, 1.0F) * 32767.0F));
        number(static_cast<std::uint16_t>(signed_sample), 2);
    }
    return file ? 0 : 1;
}

int runRealDeadline(const char* file, std::string_view scenario) {
    namespace engine = mgstc::engine;
    if (scenario != "slow" && scenario != "retain") return 2;
    engine::WavePcm pcm;
    if (!loadWaveFixture(file, pcm)) return 2;

    engine::OpllContinuousOptions options;
    options.max_workers = scenario == "slow" ? 1 : 8;
    options.control = std::make_shared<engine::OpllApproximationControl>();
    using Candidate = std::shared_ptr<const engine::OpllContinuousCandidate>;
    std::vector<Candidate> published;
    std::vector<std::uint64_t> completed_full_batches;
    const auto started = Clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - started).count(); };
    options.candidate_ready = [&](Candidate candidate) {
        published.push_back(candidate);
        std::cout << "candidate seconds=" << elapsed()
                  << " full_batch_end=" << candidate->full_evaluation_index
                  << " score=" << std::setprecision(17) << candidate->full_score << " registers=";
        printRegisters(candidate->registers);
        std::cout << std::endl;
    };
    auto hooks = std::make_shared<engine::OpllContinuousTestHooks>();
    hooks->batch_completed = [&](auto phase, auto, auto, auto full) {
        if (phase == engine::OpllApproximationPhase::FullEnvelope)
            completed_full_batches.push_back(full);
    };
    options.test_hooks = hooks; // Default 300 s limit and real steady clock.

    const auto result = engine::approximateWavePcmContinuouslyWithOpll(pcm, options);
    const auto seconds = elapsed();
    const auto complete = [&](const Candidate& candidate) {
        return candidate->full_evaluation_index > 0 && std::isfinite(candidate->full_score)
            && candidate->registers == engine::encodeOpllPatch(candidate->patch)
            && std::binary_search(completed_full_batches.begin(), completed_full_batches.end(),
                candidate->full_evaluation_index);
    };
    const auto better = [](const Candidate& left, const Candidate& right) {
        return left->full_score != right->full_score
            ? left->full_score < right->full_score : left->registers < right->registers;
    };
    const bool only_complete = std::all_of(published.begin(), published.end(), complete)
        && std::all_of(result.ranked.begin(), result.ranked.end(), complete)
        && (published.empty() ? !result.published_best
                              : result.published_best == published.back());
    const bool initial_ok = scenario == "slow"
        ? !result.initial_completed && !result.initial_best
        : result.initial_completed && result.initial_best && !result.ranked.empty()
            && complete(result.initial_best) && !better(result.initial_best, result.ranked.front());
    const bool timed_out = result.completion == engine::OpllContinuousCompletion::TimedOut;
    const bool passed = timed_out && initial_ok && only_complete && seconds >= 299.999;
    std::cout << (passed ? "PASS" : "FAIL") << " ga-real-deadline-" << scenario
              << " workers=" << options.max_workers << " limit_ms=" << hooks->time_limit.count()
              << " elapsed_seconds=" << seconds << " pcm_samples=" << pcm.mono_samples.size()
              << " timed_out=" << timed_out << " initial_complete=" << result.initial_completed
              << " representative=" << result.representative_evaluations
              << " short=" << result.short_evaluations << " full=" << result.full_evaluations
              << " published=" << published.size() << " ranked=" << result.ranked.size()
              << " complete_only=" << only_complete << '\n';
    if (result.initial_best) {
        std::cout << "initial_best registers=";
        printRegisters(result.initial_best->registers);
        std::cout << " score=" << std::setprecision(17) << result.initial_best->full_score
                  << " full_batch_end=" << result.initial_best->full_evaluation_index << '\n';
    }
    return passed ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 4 && std::string_view{argv[1]} == "--ga-initial")
        return runContinuousInitial(argv[2], argv[3]);
    if (argc == 4 && std::string_view{argv[1]} == "--ga-perceptual-deadline")
        return runContinuousInitial(argv[2], argv[3], true);
    if (argc == 2 && std::string_view{argv[1]} == "--ga-pcm-basics")
        return runStationaryPcmChecks();
    if (argc == 2 && std::string_view{argv[1]} == "--ga-perceptual-checks")
        return runPerceptualChecks();
    if (argc == 7 && std::string_view{argv[1]} == "--ga-render-opll")
        return renderPerceptualFixture(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (argc == 4 && std::string_view{argv[1]} == "--ga-real-deadline")
        return runRealDeadline(argv[2], argv[3]);
    CommandLineOptions command_line;
    std::string error;
    if (!parseCommandLine(argc, argv, command_line, error)) {
        std::cerr << "error: " << error << '\n';
        printUsage(std::cerr);
        return 2;
    }

    const auto scc = benchmarkSccWaveform();
    const auto short_wav = benchmarkWavePcm(0.25);
    const auto maximum_wav = benchmarkWavePcm(2.0);
    bool successful = true;

    if (command_line.benchmark_case == BenchmarkCase::Scc
        || command_line.benchmark_case == BenchmarkCase::All) {
        successful =
            runBenchmarkCase("SCC", command_line, [&](const auto& options) {
                return mgstc::engine::approximateSccWaveformWithOpllResult(
                    scc,
                    options);
            })
            && successful;
    }
    if (command_line.benchmark_case == BenchmarkCase::WavShort
        || command_line.benchmark_case == BenchmarkCase::All) {
        successful = runBenchmarkCase(
                         "WAV 0.25s",
                         command_line,
                         [&](const auto& options) {
                             return mgstc::engine::
                                 approximateWavePcmWithOpllResult(
                                     short_wav,
                                     options);
                         })
            && successful;
    }
    if (command_line.benchmark_case == BenchmarkCase::WavTwoSeconds
        || command_line.benchmark_case == BenchmarkCase::All) {
        successful = runBenchmarkCase(
                         "WAV 2.00s",
                         command_line,
                         [&](const auto& options) {
                             return mgstc::engine::
                                 approximateWavePcmWithOpllResult(
                                     maximum_wav,
                                     options);
                         })
            && successful;
    }
    return successful ? 0 : 1;
}
