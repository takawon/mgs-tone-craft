#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/wave_import.hpp"

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

}  // namespace

int main(int argc, char* argv[]) {
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
