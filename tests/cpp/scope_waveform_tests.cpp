// SPDX-License-Identifier: AGPL-3.0-only

#include "../../src/app/juce/scope_waveform.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <numeric>
#include <memory>
#include <utility>
#include <vector>

namespace {
using namespace mgstc::app;
using namespace mgstc::engine;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

double alignedWindowCorrelation(const std::vector<float>& a, ScopeWindow aw,
    const std::vector<float>& b, ScopeWindow bw) {
    constexpr std::size_t points = 128;
    std::array<double, points> x{}, y{};
    for (std::size_t i = 0; i < points; ++i) {
        const double phase = static_cast<double>(i) / static_cast<double>(points - 1);
        x[i] = scopeSampleLinear(a.size(), [&](std::size_t j) { return a[j]; }, aw.start + aw.span * phase);
        y[i] = scopeSampleLinear(b.size(), [&](std::size_t j) { return b[j]; }, bw.start + bw.span * phase);
    }
    const double mean_x = std::accumulate(x.begin(), x.end(), 0.0) / points;
    const double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / points;
    double dot = 0.0, energy_x = 0.0, energy_y = 0.0;
    for (std::size_t i = 0; i < points; ++i) {
        const double dx = x[i] - mean_x, dy = y[i] - mean_y;
        dot += dx * dy;
        energy_x += dx * dx;
        energy_y += dy * dy;
    }
    const double scale = std::sqrt(energy_x * energy_y);
    return scale > 1.0e-12 ? dot / scale : -1.0;
}

template<class SampleAt>
double recentLagCorrelation(std::size_t count, SampleAt sample_at, double lag) {
    constexpr std::size_t points = 128;
    const double span = std::min(lag, static_cast<double>(count - 1) - lag);
    if (span < std::max(16.0, lag * 0.25)) return -1.0;
    std::array<double, points> a{}, b{};
    for (std::size_t i = 0; i < points; ++i) {
        const double position = static_cast<double>(count - 1) - span
            + span * static_cast<double>(i) / static_cast<double>(points - 1);
        a[i] = scopeSampleLinear(count, sample_at, position);
        b[i] = scopeSampleLinear(count, sample_at, position - lag);
    }
    const double mean_a = std::accumulate(a.begin(), a.end(), 0.0) / points;
    const double mean_b = std::accumulate(b.begin(), b.end(), 0.0) / points;
    double dot = 0.0, ea = 0.0, eb = 0.0;
    for (std::size_t i = 0; i < points; ++i) {
        const double x = a[i] - mean_a, y = b[i] - mean_b;
        dot += x * y; ea += x * x; eb += y * y;
    }
    const double scale = std::sqrt(ea * eb);
    return scale > 1.0e-12 ? dot / scale : -1.0;
}

std::vector<float> periodic(std::size_t n, double cycles, int shape) {
    std::vector<float> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double phase = std::fmod(static_cast<double>(i) * cycles / n, 1.0);
        double value{};
        if (shape == 0) value = std::sin(2.0 * std::numbers::pi * phase);
        else if (shape == 1) value = phase < 0.5 ? 1.0 : -1.0;
        else if (shape == 2) value = 2.0 * phase - 1.0;
        else value = std::sin(2.0 * std::numbers::pi * phase)
                + 0.25 * std::sin(14.0 * std::numbers::pi * phase);
        result[i] = static_cast<float>(value);
    }
    return result;
}

template<class F>
void eachSignal(F&& test) {
    constexpr std::size_t count = 4096;
    for (int shape = 0; shape < 4; ++shape) {
        auto samples = periodic(count, 7.0, shape);
        test(samples, shape);
    }
    std::vector<float> pulse(count, 0.0F);
    pulse[count / 2] = 1.0F;
    test(pulse, 4);
    std::vector<float> silence(count, 0.0F);
    test(silence, 5);
    std::vector<float> tiny(count);
    for (std::size_t i = 0; i < count; ++i)
        tiny[i] = 1.0e-8F * static_cast<float>(std::sin(2.0 * std::numbers::pi * i / 31.0));
    test(tiny, 6);
    std::vector<float> step(count, 0.0F);
    std::fill(step.begin() + count / 2, step.end(), 0.8F);
    test(step, 7);
}

void testWindowSelectionBoundariesAndFiniteOutput() {
    eachSignal([](const std::vector<float>& pcm, int) {
        ScopeSynchronizer synchronizer;
        std::size_t reads = 0;
        auto sample = [&](std::size_t index) {
            ++reads;
            return pcm[index];
        };
        const auto window = synchronizer.select(pcm.size(), sample, 47.25);
        require(std::isfinite(window.start) && std::isfinite(window.span), "non-finite scope window");
        require(window.start >= 0.0 && window.span > 0.0
                && window.start + window.span <= static_cast<double>(pcm.size() - 1) + 1.0e-6,
            "scope window outside sample history");
        constexpr std::size_t kWorstCaseSampleReadBudget = 64 * 1024;
        require(reads <= kWorstCaseSampleReadBudget,
            "synchronizer exceeded its bounded sample budget");
        const auto recent = synchronizer.select(pcm.size(), sample, 47.25, false);
        require(!recent.synchronized, "disabled search must use recent PCM");
        require(recent.start + recent.span >= static_cast<double>(pcm.size() - 1) - 1.0e-6,
            "fallback did not show the newest PCM window");
    });
}

void testUnipolarPeriodicAndConstantDcSignals() {
    constexpr std::size_t count = 4096;
    for (const int shape : {0, 1}) {
        std::vector<float> pcm(count);
        for (std::size_t i = 0; i < count; ++i) {
            const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / 64.0;
            pcm[i] = shape == 0
                ? static_cast<float>(0.35 + 0.2 * std::sin(phase))
                : (i % 64 < 32 ? 0.8F : 0.0F);
        }
        ScopeSynchronizer sync;
        const auto window = sync.select(count, [&](std::size_t i) { return pcm[i]; }, 64.0);
        require(window.synchronized, "positive-offset/unipolar periodic PCM did not synchronize");
    }
    std::vector<float> dc(count, 0.2F);
    ScopeSynchronizer dc_sync;
    const auto dc_window = dc_sync.select(count, [&](std::size_t i) { return dc[i]; }, 64.0);
    require(!dc_window.synchronized, "constant DC was classified as periodic waveform");
    require(dc_window.start + dc_window.span >= static_cast<double>(count - 1) - 1.0e-6,
        "constant DC fallback did not use newest PCM");
}

void testRollingColoredNoiseStaysRecentAndReadBounded() {
    constexpr std::size_t count = 4096;
    constexpr std::size_t append = 800;
    constexpr double hinted_period = 734.0;
    constexpr std::size_t kWorstCaseSampleReadBudget = 64 * 1024;
    std::vector<float> pcm(count);
    std::uint32_t state = 0x2149a11U;
    auto white = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<std::int32_t>(state) / 2147483648.0);
    };
    float colored = 0.0F;
    for (auto& value : pcm) {
        colored = 0.98F * colored + 0.2F * white();
        value = colored;
    }
    ScopeSynchronizer sync;
    double previous_absolute_end = -1.0;
    for (std::uint64_t frame = 1; frame <= 12; ++frame) {
        if (frame > 1) {
            std::move(pcm.begin() + append, pcm.end(), pcm.begin());
            for (std::size_t i = count - append; i < count; ++i) {
                colored = 0.98F * colored + 0.2F * white();
                pcm[i] = colored;
            }
        }
        std::size_t reads = 0;
        const auto window = sync.select(count, [&](std::size_t i) {
            ++reads;
            return pcm[i];
        }, hinted_period, true, frame * append);
        require(reads <= kWorstCaseSampleReadBudget,
            "warmed noisy synchronizer exceeded 64K sample reads");
        require(!window.synchronized, "rolling colored noise locked to an old display phase");
        const double absolute_end = static_cast<double>(frame * append)
            - static_cast<double>(count) + window.start + window.span;
        require(absolute_end + 1.0e-6 >= previous_absolute_end,
            "rolling-noise display window moved backward in absolute sample time");
        require(absolute_end <= static_cast<double>(frame * append) - 1.0 + 1.0e-6,
            "rolling-noise fallback displayed PCM beyond the newest sample");
        previous_absolute_end = absolute_end;
    }

    // Saturate the crossing candidate cap after warming previous-window state.
    std::vector<float> many_crossings(count);
    for (std::size_t i = 0; i < count; ++i)
        many_crossings[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * i / 4.0));
    sync.reset();
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        std::size_t reads = 0;
        const auto result = sync.select(count, [&](std::size_t i) {
            ++reads;
            return many_crossings[i];
        }, hinted_period, true, frame * append);
        require(reads <= kWorstCaseSampleReadBudget,
            "warmed maximum-candidate synchronizer exceeded 64K sample reads");
        require(std::isfinite(result.start) && std::isfinite(result.span),
            "maximum-candidate search produced invalid window");
    }
}

void testPeriodicPhaseStabilityAndNonPeriodicFallback() {
    constexpr std::size_t count = 4096;
    ScopeSynchronizer sync;
    std::vector<float> pcm(count);
    double prior = -1.0;
    std::vector<float> previous_pcm;
    ScopeWindow previous_window{};
    for (int frame = 0; frame < 8; ++frame) {
        const double shift = frame * 13.0;
        for (std::size_t i = 0; i < count; ++i) {
            const double phase = (static_cast<double>(i) + shift) * 2.0 * std::numbers::pi / 53.0;
            pcm[i] = static_cast<float>(std::sin(phase) + 0.16 * std::sin(3.0 * phase));
        }
        const auto result = sync.select(count, [&](std::size_t i) { return pcm[i]; }, 53.0,
            true, static_cast<std::uint64_t>(frame + 1) * 800);
        require(result.synchronized, "stable periodic signal did not synchronize");
        if (prior >= 0.0) {
            double delta = std::fmod(std::abs(result.start - prior), 53.0);
            delta = std::min(delta, 53.0 - delta);
            require(delta < 15.0, "periodic display phase jumped between adjacent frames");
            require(alignedWindowCorrelation(previous_pcm, previous_window, pcm, result) > 0.9,
                "synchronized periodic window shape drifted between adjacent frames");
        }
        prior = result.start;
        previous_pcm = pcm;
        previous_window = result;
    }

    sync.reset();
    std::uint32_t state = 0x7f4a7c15U;
    for (auto& value : pcm) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        value = static_cast<float>(static_cast<std::int32_t>(state) / 2147483648.0);
    }
    const auto noise = sync.select(count, [&](std::size_t i) { return pcm[i]; }, 53.0);
    require(!noise.synchronized, "noise was presented as a stable periodic signal");
    require(noise.start > static_cast<double>(count) * 0.5,
        "low confidence synchronization did not follow recent PCM");
}

void testSamplingAndPeakPreservingPath() {
    std::vector<float> dense(2048, 0.0F);
    dense[1013] = 0.95F;
    juce::Path path;
    const juce::Rectangle<float> bounds(0.0F, 0.0F, 64.0F, 100.0F);
    buildScopePath(path, bounds, dense.size(), [&](std::size_t i) { return dense[i]; }, 1.0F, true);
    require(!path.isEmpty(), "dense scope path missing");
    const auto b = path.getBounds();
    require(b.getY() < 5.0F && b.getHeight() > 40.0F,
        "narrow dense peak disappeared during pixel reduction");

    const std::array<float, 4> sparse{0.0F, 1.0F, -1.0F, 0.0F};
    require(std::abs(scopeSampleLinear(sparse.size(), [&](std::size_t i) { return sparse[i]; }, 1.5) - 0.0F) < 1.0e-6F,
        "linear interpolation did not preserve midpoint");
    buildScopePath(path, bounds, sparse.size(), [&](std::size_t i) { return sparse[i]; }, 1.0F);
    require(!path.isEmpty(), "sparse interpolated scope path missing");

    const std::array<float, 3> invalid{0.0F, std::numeric_limits<float>::quiet_NaN(), 0.0F};
    buildScopePath(path, bounds, invalid.size(), [&](std::size_t i) { return invalid[i]; }, 1.0F, true);
    const auto finite_bounds = path.getBounds();
    require(std::isfinite(finite_bounds.getX()) && std::isfinite(finite_bounds.getY())
            && std::isfinite(finite_bounds.getWidth()) && std::isfinite(finite_bounds.getHeight()),
        "NaN PCM produced invalid draw geometry");
}

void testCompositeWindowPathAndSmallHistories() {
    std::array<float, 32> pcm{};
    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<float>(std::sin(i * 2.0 * std::numbers::pi / 8.0));
    ScopeSynchronizer sync;
    const auto window = sync.select(pcm.size(), [&](std::size_t i) { return pcm[i]; }, 8.0);
    require(std::isfinite(window.start + window.span), "short-history fallback invalid");
    juce::Path path;
    buildScopeWindowPath(path, {0, 0, 16, 30}, pcm.size(),
        [&](std::size_t i) { return pcm[i]; }, window, 0.8F, true);
    require(!path.isEmpty(), "shared period-window path missing");
}

// Reference implementations copied from the current checkout's pre-change
// OPLL phase search and composite nearest-neighbour draw loop. These are used
// only by --benchmark, never by product code or pass/fail timing limits.
struct LegacyOpllScope {
    static constexpr std::size_t display_count = OpllScopeFrame::kSampleCount;
    std::array<float, 4096> history{};
    std::array<float, display_count> displayed{};

    float sample(double position) const {
        position = std::clamp(position, 0.0, static_cast<double>(history.size() - 1));
        const auto left = static_cast<std::size_t>(position);
        const auto right = std::min(left + 1, history.size() - 1);
        return history[left] + (history[right] - history[left])
            * static_cast<float>(position - static_cast<double>(left));
    }
    static double correlation(const auto& a, const auto& b) {
        double ma = std::accumulate(a.begin(), a.end(), 0.0) / a.size();
        double mb = std::accumulate(b.begin(), b.end(), 0.0) / b.size();
        double dot = 0.0, ea = 0.0, eb = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double x = a[i] - ma, y = b[i] - mb;
            dot += x * y; ea += x * x; eb += y * y;
        }
        const double scale = std::sqrt(ea * eb);
        return scale > 1.0e-12 ? dot / scale : -1.0;
    }
    void rebuild(double period) {
        const double span = period * 2.0;
        const double maximum_start = static_cast<double>(history.size() - 1) - span;
        const auto first = static_cast<std::size_t>(std::max(1.0, maximum_start - period * 2.5));
        const auto last = static_cast<std::size_t>(std::floor(maximum_start));
        std::vector<double> candidates;
        for (std::size_t i = first; i <= last; ++i) {
            const float previous = sample(static_cast<double>(i - 1));
            const float current = sample(static_cast<double>(i));
            if (previous <= 0.0F && current > 0.0F) {
                const double fraction = -static_cast<double>(previous)
                    / static_cast<double>(current - previous);
                candidates.push_back(static_cast<double>(i - 1) + fraction);
            }
        }
        if (candidates.empty()) candidates.push_back(maximum_start);
        const auto evaluation_count = std::min<std::size_t>(candidates.size(), 64);
        double best_start = candidates.back();
        double best_score = -std::numeric_limits<double>::infinity();
        std::array<float, display_count> candidate{};
        for (std::size_t evaluation = 0; evaluation < evaluation_count; ++evaluation) {
            const auto ci = evaluation_count > 1
                ? evaluation * (candidates.size() - 1) / (evaluation_count - 1)
                : candidates.size() - 1;
            const double start = candidates[ci];
            for (std::size_t i = 0; i < candidate.size(); ++i)
                candidate[i] = sample(start + span * static_cast<double>(i)
                    / static_cast<double>(candidate.size() - 1));
            const double score = correlation(candidate, displayed)
                + (maximum_start > 0.0 ? start / maximum_start : 0.0) * 1.0e-4;
            if (score > best_score) { best_score = score; best_start = start; }
        }
        for (std::size_t i = 0; i < displayed.size(); ++i)
            displayed[i] = sample(best_start + span * static_cast<double>(i)
                / static_cast<double>(displayed.size() - 1));
    }
};

void legacyCompositePath(juce::Path& path, juce::Rectangle<float> bounds,
    const std::array<float, 4096>& history, std::size_t size,
    std::size_t channel, std::size_t channels, double period) {
    const std::size_t count = std::min(size, static_cast<std::size_t>(std::max(2.0, std::round(period * 2.0))));
    const std::size_t oldest = 4096 - size;
    const std::size_t start = size - count;
    (void)(channel >= channels ? 17 : channel); // MIX occupies the final history slot.
    const auto at = [&](std::size_t i) { return history[(oldest + start + i) % 4096]; };
    float peak = 0.0001F;
    for (std::size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(at(i)));
    path.clear();
    const auto points = static_cast<std::size_t>(bounds.getWidth());
    if (points < 2) return;
    path.preallocateSpace(static_cast<int>(points * 3 + 4));
    const float path_width = bounds.getWidth() - 1.0F;
    const float step = path_width / static_cast<float>(points - 1);
    for (std::size_t x = 0; x < points; ++x) {
        const auto index = static_cast<std::size_t>(std::round(static_cast<double>(x)
            * static_cast<double>(count - 1) / static_cast<double>(points - 1)));
        const float value = at(std::min(index, count - 1)) * (0.86F / peak);
        const float px = bounds.getX() + static_cast<float>(x) * step;
        const float py = bounds.getCentreY() - value * bounds.getHeight() * 0.5F;
        if (x == 0) path.startNewSubPath(px, py);
        else path.lineTo(px, py);
    }
}

struct Timing { double median_us{}, p95_us{}; };
template<class Work>
Timing timeWork(Work&& work, int iterations = 140) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations + 20; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        work(i);
        const auto end = std::chrono::steady_clock::now();
        if (i >= 20) values.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
    }
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2], values[values.size() * 95 / 100]};
}

void displayTimingDiagnostic() {
    constexpr std::size_t channels = 18;
    constexpr std::size_t history_size = 4096;
    constexpr double base_period = 61.7;
    std::array<float, history_size> one{};
    for (std::size_t i = 0; i < history_size; ++i)
        one[i] = static_cast<float>(0.6 * std::sin(i * 2.0 * std::numbers::pi / base_period)
            + 0.12 * std::sin(i * 6.0 * std::numbers::pi / base_period));
    LegacyOpllScope legacy_opll;
    legacy_opll.history = one;
    ScopeSynchronizer common_opll;
    const auto old_sync = timeWork([&](int) { legacy_opll.rebuild(base_period); });
    const auto new_sync = timeWork([&](int) {
        (void)common_opll.select(one.size(), [&](std::size_t i) { return one[i]; }, base_period);
    });

    std::array<std::array<float, history_size>, channels> rows{};
    std::array<ScopeSynchronizer, channels> synchronizers{};
    std::array<juce::Path, channels> paths{};
    for (std::size_t c = 0; c < channels; ++c) {
        const double period = base_period * (1.0 + static_cast<double>(c) * 0.006);
        for (std::size_t i = 0; i < history_size; ++i) {
            double value = 0.45 * std::sin(i * 2.0 * std::numbers::pi / period + c * 0.13)
                + 0.08 * std::sin(i * 6.0 * std::numbers::pi / period);
            if (c == channels - 1) value += 0.05 * std::sin(i * 2.399);
            rows[c][i] = static_cast<float>(value);
        }
    }
    const auto bounds = juce::Rectangle<float>(0.0F, 0.0F, 320.0F, 28.0F);
    const auto old_composite = timeWork([&](int) {
        for (std::size_t c = 0; c < channels; ++c)
            legacyCompositePath(paths[c], bounds, rows[c], history_size, c, channels, base_period);
    });
    const auto new_composite = timeWork([&](int) {
        for (std::size_t c = 0; c < channels; ++c) {
            const double period = base_period * (1.0 + static_cast<double>(c) * 0.006);
            const auto window = synchronizers[c].select(history_size,
                [&](std::size_t i) { return rows[c][i]; }, period);
            buildScopeWindowPath(paths[c], bounds, history_size,
                [&](std::size_t i) { return rows[c][i]; }, window, 0.86F / 0.55F, true);
        }
    });
    std::printf("scope display diagnostic (140 ticks, %zu channels): OPLL sync legacy median=%.2fus p95=%.2fus shared median=%.2fus p95=%.2fus; composite 18-channel legacy draw-data median=%.2fus p95=%.2fus shared sync+draw-data median=%.2fus p95=%.2fus\n",
        channels, old_sync.median_us, old_sync.p95_us, new_sync.median_us, new_sync.p95_us,
        old_composite.median_us, old_composite.p95_us,
        new_composite.median_us, new_composite.p95_us);
}

struct RenderMeasurement { double median_us{}; double p95_us{}; double checksum{}; };

RenderMeasurement measureRender(bool capture_scope, int iterations) {
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "engine edit unavailable for render timing");
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(*edit.engine, defaultCompositeTimbre(), {.polyphonic = true}, &plan),
        "default program compile failed for render timing");
    require(host.submitProgram(edit), "program submit failed for render timing");
    host.servicePendingControlCommands();
    if (!plan.audible_layers.empty())
        require(host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
            "note on failed for render timing");
    host.setOpllScopeEnabled(capture_scope);
    std::array<float, 1024> output{};
    std::vector<double> elapsed;
    elapsed.reserve(static_cast<std::size_t>(iterations));
    double checksum = 0.0;
    for (int n = 0; n < iterations + 20; ++n) {
        const auto begin = std::chrono::steady_clock::now();
        const auto result = host.renderAudio(output);
        const auto end = std::chrono::steady_clock::now();
        require(result.ok(), "render failed during timing run");
        if (n >= 20) {
            elapsed.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
            checksum += output[static_cast<std::size_t>(n) % output.size()];
        }
        OpllScopeFrame frame;
        while (host.pollLatestOpllScope(frame)) {}
    }
    std::sort(elapsed.begin(), elapsed.end());
    return {elapsed[elapsed.size() / 2], elapsed[(elapsed.size() * 95) / 100], checksum};
}

void testScopeCaptureDoesNotChangeRenderedAudio() {
    RealtimeEngineHost plain;
    RealtimeEngineHost monitored;
    auto prepare = [](RealtimeEngineHost& host) {
        auto edit = host.beginProgramEdit();
        require(edit.valid(), "engine edit unavailable for PCM parity");
        CompositePlaybackPlan plan;
        require(compileCompositeProgram(*edit.engine, defaultCompositeTimbre(), {.polyphonic = true}, &plan),
            "program compile failed for PCM parity");
        require(host.submitProgram(edit), "program submit failed for PCM parity");
        host.servicePendingControlCommands();
        require(!plan.audible_layers.empty(), "default plan has no layer for PCM parity");
        require(host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
            "note on failed for PCM parity");
    };
    prepare(plain);
    prepare(monitored);
    monitored.setOpllScopeEnabled(true);
    std::array<float, 512> a{}, b{};
    bool heard_pcm = false;
    for (int block = 0; block < 24; ++block) {
        require(plain.renderAudio(a).ok() && monitored.renderAudio(b).ok(), "PCM parity render failed");
        require(a == b, "enabling display capture changed rendered audio samples");
        heard_pcm = heard_pcm || std::any_of(a.begin(), a.end(), [](float sample) { return std::abs(sample) > 1.0e-8F; });
        OpllScopeFrame frame;
        while (monitored.pollLatestOpllScope(frame)) {}
    }
    require(heard_pcm, "PCM parity probe did not render an audible note");
}

struct CapturedScenario {
    const char* name{};
    CompositeTimbre timbre;
    std::vector<std::uint8_t> notes;
};

CapturedScenario singleSourceScenario(TimbreSource source, const char* name) {
    auto timbre = defaultCompositeTimbre();
    std::erase_if(timbre.layers, [source](const CompositeLayer& layer) { return layer.source != source; });
    for (auto& layer : timbre.layers) seedDefaultLayerTimbre(timbre, layer);
    return {name, std::move(timbre), {60}};
}

void testRenderedPcmScopeScenarios() {
    std::vector<CapturedScenario> scenarios;
    scenarios.push_back(singleSourceScenario(TimbreSource::Opll, "OPLL ROM"));
    auto original_opll = singleSourceScenario(TimbreSource::Opll, "OPLL original");
    {
        auto patch = defaultOpllPatch();
        patch.carrier.release_rate = 1;
        SavedTimbreReference reference;
        reference.library_id = 100;
        reference.revision = 1;
        reference.source = TimbreSource::Opll;
        reference.opll_registers = encodeOpllPatch(patch);
        original_opll.timbre.layers.front().base_timbre = reference;
        original_opll.timbre.layers.front().base_opll_rom.reset();
    }
    scenarios.push_back(std::move(original_opll));
    scenarios.push_back(singleSourceScenario(TimbreSource::Scc, "SCC single"));
    scenarios.push_back(singleSourceScenario(TimbreSource::Psg, "PSG single"));

    auto patchslide = singleSourceScenario(TimbreSource::Opll, "OPLL PatchSlide");
    patchslide.timbre.layers.front().volume_envelope.kind = EnvelopeKind::Sequence;
    patchslide.timbre.layers.front().volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 15, .count = 0}};
    patchslide.timbre.layers.front().envelope_timeline.length_counts = 32;
    patchslide.timbre.layers.front().envelope_timeline.loop_start_count = 0;
    patchslide.timbre.layers.front().envelope_timeline.loop_end_count = 32;
    patchslide.timbre.layers.front().timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .value = 1, .count = 8, .timbre_pick = TimbrePick::OpllRom},
        {.kind = EnvelopeEventKind::Timbre, .value = 4, .count = 16, .timbre_pick = TimbrePick::OpllRom}};
    require(layerEnvelopeHasPatchSlide(patchslide.timbre.layers.front()), "PatchSlide fixture is invalid");
    scenarios.push_back(std::move(patchslide));

    auto combined = defaultCompositeTimbre();
    for (auto& layer : combined.layers) seedDefaultLayerTimbre(combined, layer);
    auto pair = combined;
    std::erase_if(pair.layers, [](const CompositeLayer& layer) { return layer.source == TimbreSource::Psg; });
    scenarios.push_back({"OPLL + SCC", std::move(pair), {60, 60}});
    auto different = combined;
    different.layers[1].relative_semitones = 7;
    different.layers[2].relative_semitones = 12;
    scenarios.push_back({"Different-frequency MIX", std::move(different), {60, 67, 72}});
    scenarios.push_back({"OPLL + SCC + PSG", std::move(combined), {60, 60, 60}});

    auto detuned = defaultCompositeTimbre();
    auto scc = detuned.layers[1];
    detuned.layers.clear();
    scc.channel = 0;
    seedDefaultLayerTimbre(detuned, scc);
    detuned.layers.push_back(scc);
    auto second = scc;
    second.channel = 1;
    second.name += " detuned";
    second.detune = 12;
    seedDefaultLayerTimbre(detuned, second);
    detuned.layers.push_back(second);
    scenarios.push_back({"SCC + SCC detuned", std::move(detuned), {60, 60}});

    auto noise = singleSourceScenario(TimbreSource::Psg, "PSG noise");
    noise.timbre.layers.front().psg_tone_mode = 2;
    noise.timbre.layers.front().psg_noise = 12;
    scenarios.push_back(std::move(noise));

    auto lfo = singleSourceScenario(TimbreSource::Scc, "SCC software LFO");
    lfo.timbre.layers.front().software_lfo = {
        .enabled = true, .delay = 0, .depth = 6, .speed = 0, .roughness = 3};
    scenarios.push_back(std::move(lfo));

    auto envelope = singleSourceScenario(TimbreSource::Psg, "PSG sequence ENV");
    envelope.timbre.layers.front().volume_envelope.kind = EnvelopeKind::Sequence;
    envelope.timbre.layers.front().volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 15, .count = 0},
        {.kind = EnvelopeEventKind::Volume, .value = 7, .count = 2},
        {.kind = EnvelopeEventKind::Volume, .value = 15, .count = 4},
    };
    envelope.timbre.layers.front().envelope_timeline.length_counts = 8;
    scenarios.push_back(std::move(envelope));

    for (auto& scenario : scenarios) {
        EngineCore core;
        CompositePlaybackPlan plan;
        require(compileCompositeProgram(core, scenario.timbre, {.polyphonic = true}, &plan),
            "program compile failed for rendered PCM scenario");
        std::uint32_t mask = 0;
        std::vector<std::uint8_t> tracks;
        for (const auto& binding : plan.audible_layers) {
            const auto track = plan.physicalTrack(binding, 0);
            tracks.push_back(track);
            mask |= 1U << track;
        }
        require(!tracks.empty(), "scenario had no audible physical tracks");
        core.setScopeSelection(mask, 1);
        core.setOpllScopeEnabled(true);
        for (std::size_t i = 0; i < tracks.size(); ++i)
            require(core.session().queueNoteOn(tracks[i], scenario.notes[std::min(i, scenario.notes.size() - 1)]),
                "note-on enqueue failed for rendered PCM scenario");
        std::array<float, OpllScopeFrame::kSampleCount * 2> output{};
        OpllScopeFrame frame;
        std::array<ScopeSynchronizer, RuntimeSession::kTrackCount> synchronizers{};
        std::array<bool, RuntimeSession::kTrackCount> saw_pcm{};
        std::array<bool, RuntimeSession::kTrackCount> saw_window{};
        std::array<bool, RuntimeSession::kTrackCount> saw_sync{};
        const std::string scenario_name(scenario.name);
        const bool expect_periodic_opll = scenario_name == "OPLL ROM"
            || scenario_name == "OPLL original";
        const bool expect_periodic_physical = scenario_name == "PSG single"
            || scenario_name == "SCC single";
        ScopeSynchronizer opll_synchronizer;
        ScopeSynchronizer mix_synchronizer;
        bool saw_opll_sync = false;
        double opll_last_mean = 0.0;
        double opll_last_peak = 0.0;
        double opll_last_min = 0.0;
        double opll_corr_half = -1.0, opll_corr_double = -1.0, opll_corr_quadruple = -1.0;
        for (int tick = 0; tick < 24; ++tick) {
            require(core.render(output).ok(), "render failed for PCM scenario");
            require(core.takeOpllScopeFrame(frame), "scope frame missing for PCM scenario");
            const auto mix_window = mix_synchronizer.select(frame.mixed_samples.size(),
                [&](std::size_t i) { return frame.mixed_samples[i]; }, scopePeriodSamples(60), true, frame.sample_end);
            require(std::isfinite(mix_window.start) && std::isfinite(mix_window.span)
                    && mix_window.start >= 0.0
                    && mix_window.start + mix_window.span <= static_cast<double>(frame.kSampleCount - 1) + 1.0e-6,
                "rendered multi-source MIX produced an invalid scope window");
            for (std::size_t track = 0; track < RuntimeSession::kTrackCount; ++track) {
                if ((frame.captured_track_mask & (1U << track)) == 0) continue;
                const auto& pcm = frame.physical_samples[track];
                require(std::all_of(pcm.begin(), pcm.end(), [](float v) { return std::isfinite(v); }),
                    "non-finite physical rendered PCM");
                const auto peak = *std::max_element(pcm.begin(), pcm.end(),
                    [](float a, float b) { return std::abs(a) < std::abs(b); });
                saw_pcm[track] = saw_pcm[track] || std::abs(peak) > 1.0e-8F;
                const auto window = synchronizers[track].select(pcm.size(),
                    [&](std::size_t i) { return pcm[i]; }, scopePeriodSamples(60), true, frame.sample_end);
                require(std::isfinite(window.start) && std::isfinite(window.span)
                        && window.start >= 0.0
                        && window.start + window.span <= static_cast<double>(pcm.size() - 1) + 1.0e-6,
                    "rendered PCM produced invalid scope window");
                saw_window[track] = true;
                saw_sync[track] = saw_sync[track] || window.synchronized;
            }
            for (const auto* values : {&frame.psg_samples, &frame.scc_samples,
                    &frame.samples, &frame.mixed_samples})
                require(std::all_of(values->begin(), values->end(),
                    [](float v) { return std::isfinite(v); }), "non-finite rendered source or MIX PCM");
            if (expect_periodic_opll) {
                const auto window = opll_synchronizer.select(frame.samples.size(),
                    [&](std::size_t i) { return frame.samples[i]; }, scopePeriodSamples(60), true, frame.sample_end);
                saw_opll_sync = saw_opll_sync || window.synchronized;
                opll_last_mean = std::accumulate(frame.samples.begin(), frame.samples.end(), 0.0)
                    / static_cast<double>(frame.samples.size());
                opll_last_peak = 0.0;
                opll_last_min = std::numeric_limits<double>::infinity();
                for (const auto value : frame.samples)
                {
                    opll_last_peak = std::max(opll_last_peak, static_cast<double>(std::abs(value)));
                    opll_last_min = std::min(opll_last_min, static_cast<double>(value));
                }
                const auto at = [&](std::size_t i) { return frame.samples[i]; };
                const double hint = scopePeriodSamples(60);
                opll_corr_half = recentLagCorrelation(frame.samples.size(), at, hint * 0.5);
                opll_corr_double = recentLagCorrelation(frame.samples.size(), at, hint * 2.0);
                opll_corr_quadruple = recentLagCorrelation(frame.samples.size(), at, hint * 4.0);
            }
        }
        for (const auto track : tracks) {
            require(saw_pcm[track], "active rendered physical channel stayed silent");
            require(saw_window[track], "rendered physical channel did not reach display processing");
        }
        if (expect_periodic_opll)
            require(saw_opll_sync, ("rendered " + scenario_name + " PCM never synchronized; mean="
                + std::to_string(opll_last_mean) + " peak=" + std::to_string(opll_last_peak)
                + " min=" + std::to_string(opll_last_min)
                + " hint_period=" + std::to_string(scopePeriodSamples(60))
                + " lag_corr=" + std::to_string(opll_corr_half) + ","
                + std::to_string(opll_corr_double) + ","
                + std::to_string(opll_corr_quadruple)).c_str());
        if (expect_periodic_physical) {
            for (const auto track : tracks)
                require(saw_sync[track], ("rendered " + scenario_name + " track "
                    + std::to_string(track) + " PCM never synchronized").c_str());
        }
    }
}

void testScopeGainMetadataAcrossPartialRenderCalls() {
    EngineCore core;
    auto timbre = singleSourceScenario(TimbreSource::Opll, "OPLL gain metadata");
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(core, timbre.timbre, {.polyphonic = true}, &plan),
        "OPLL gain metadata program compile failed");
    require(!plan.audible_layers.empty(), "OPLL gain metadata plan is empty");
    const auto track = plan.physicalTrack(plan.audible_layers.front(), 0);
    core.setScopeSelection(1U << track, 11);
    core.setOpllScopeEnabled(true);
    require(core.session().queueNoteOn(track, 60), "OPLL gain metadata note-on failed");
    constexpr std::array<MixerGains, 4> gains{{
        {.master = 1.0F, .psg = 0.2F, .scc = 0.3F, .opll = 0.4F},
        {.master = 0.8F, .psg = 0.4F, .scc = 0.6F, .opll = 0.8F},
        {.master = 0.6F, .psg = 0.6F, .scc = 0.8F, .opll = 1.2F},
        {.master = 0.4F, .psg = 0.8F, .scc = 1.0F, .opll = 1.6F},
    }};
    std::array<float, 400> output{};
    for (const auto& gain : gains) {
        require(core.setGains(gain), "mixer gain change rejected during capture");
        require(core.render(output).ok(), "partial render failed while capturing gain metadata");
    }
    OpllScopeFrame frame;
    require(core.takeOpllScopeFrame(frame), "partial render calls did not complete the scope frame");
    for (std::size_t i = 0; i < frame.mixer_gains.size(); ++i) {
        const auto& expected = gains[i / 200];
        const auto& actual = frame.mixer_gains[i];
        require(actual.master == expected.master && actual.psg == expected.psg
                && actual.scc == expected.scc && actual.opll == expected.opll,
            "scope gain metadata did not follow the gain active for each PCM sample");
    }
}

void renderTimingDiagnostic() {
    constexpr int iterations = 300;
    const auto no_capture = measureRender(false, iterations);
    const auto with_capture = measureRender(true, iterations);
    std::printf("scope render diagnostic (%d blocks): capture_off median=%.2fus p95=%.2fus; capture_on median=%.2fus p95=%.2fus; checksum_delta=%.9g\n",
        iterations, no_capture.median_us, no_capture.p95_us,
        with_capture.median_us, with_capture.p95_us,
        with_capture.checksum - no_capture.checksum);
}

CompositeTimbre maximumLayerScopeFixture() {
    auto timbre = defaultCompositeTimbre();
    const auto prototypes = timbre.layers;
    timbre.layers.clear();
    const std::array<std::pair<TimbreSource, std::uint8_t>, 3> groups{{
        {TimbreSource::Psg, 3}, {TimbreSource::Scc, 5}, {TimbreSource::Opll, 9}}};
    for (const auto [source, count] : groups) {
        const auto found = std::find_if(prototypes.begin(), prototypes.end(),
            [source](const CompositeLayer& layer) { return layer.source == source; });
        require(found != prototypes.end(), "default source prototype missing for 17-layer fixture");
        for (std::uint8_t channel = 0; channel < count; ++channel) {
            auto layer = *found;
            layer.channel = channel;
            layer.name += " " + std::to_string(channel + 1);
            layer.envelope_number = static_cast<std::uint8_t>(timbre.layers.size());
            seedDefaultLayerTimbre(timbre, layer);
            timbre.layers.push_back(std::move(layer));
        }
    }
    require(timbre.layers.size() == 17, "maximum layer fixture did not create all 17 channels");
    return timbre;
}

void maximumLayerTimingDiagnostic() {
    constexpr int measured_ticks = 60;
    constexpr int warmup_ticks = 16;
    const auto timbre = maximumLayerScopeFixture();
    auto prepare = [&](bool capture) {
        auto host = std::make_unique<RealtimeEngineHost>();
        host->setOpllScopeEnabled(capture);
        auto edit = host->beginProgramEdit();
        require(edit.valid(), "engine edit unavailable for maximum-layer diagnostic");
        CompositePlaybackPlan plan;
        require(compileCompositeProgram(*edit.engine, timbre, {.polyphonic = false}, &plan),
            "maximum-layer program compile failed");
        require(plan.audible_layers.size() == 17, "maximum-layer plan did not retain 17 audible layers");
        require(host->submitProgram(edit), "maximum-layer program submit failed");
        std::uint32_t mask = 0;
        for (const auto& binding : plan.audible_layers)
            mask |= 1U << plan.physicalTrack(binding, 0);
        require(host->setScopeSelection(mask, 31), "maximum-layer scope selection rejected");
        for (const auto& binding : plan.audible_layers) {
            const auto track = plan.physicalTrack(binding, 0);
            require(host->submit(EngineCommand::noteOn(track, 60)), "maximum-layer note-on rejected");
        }
        return std::pair{std::move(host), std::move(plan)};
    };
    auto [plain, plain_plan] = prepare(false);
    auto [captured, captured_plan] = prepare(true);
    require(plain_plan.audible_layers.size() == captured_plan.audible_layers.size(),
        "capture comparison plans diverged");
    std::array<float, 1600> plain_pcm{}, captured_pcm{};
    std::vector<double> plain_times, captured_times;
    bool heard_pcm = false;
    plain_times.reserve(measured_ticks);
    captured_times.reserve(measured_ticks);
    for (int tick = 0; tick < warmup_ticks + measured_ticks; ++tick) {
        auto begin = std::chrono::steady_clock::now();
        require(plain->render(plain_pcm).ok(), "maximum-layer capture-off render failed");
        auto end = std::chrono::steady_clock::now();
        if (tick >= warmup_ticks)
            plain_times.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        begin = std::chrono::steady_clock::now();
        require(captured->render(captured_pcm).ok(), "maximum-layer capture-on render failed");
        end = std::chrono::steady_clock::now();
        require(plain_pcm == captured_pcm, "scope capture changed maximum-layer output PCM");
        heard_pcm = heard_pcm || std::any_of(plain_pcm.begin(), plain_pcm.end(),
            [](float sample) { return std::abs(sample) > 1.0e-8F; });
        if (tick >= warmup_ticks)
            captured_times.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        OpllScopeFrame frame;
        while (captured->pollLatestOpllScope(frame)) {}
    }
    require(heard_pcm, "maximum-layer diagnostic did not render sounding PCM");
    const auto quantile = [](std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        return Timing{values[values.size() / 2], values[values.size() * 95 / 100]};
    };
    const auto off = quantile(plain_times);
    const auto on = quantile(captured_times);

    std::vector<double> note_times;
    note_times.reserve(40);
    std::size_t accepted_commands = 0;
    for (int tick = 0; tick < 40; ++tick) {
        for (const auto& binding : captured_plan.audible_layers) {
            const auto track = captured_plan.physicalTrack(binding, 0);
            if (captured->submit(EngineCommand::noteOn(track, static_cast<std::uint8_t>(60 + tick % 12))))
                ++accepted_commands;
            if (captured->submit(EngineCommand::noteOff(track))) ++accepted_commands;
        }
        const auto begin = std::chrono::steady_clock::now();
        require(captured->render(captured_pcm).ok(), "rapid max-layer NoteOn/Off render failed");
        const auto end = std::chrono::steady_clock::now();
        note_times.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        OpllScopeFrame frame;
        while (captured->pollLatestOpllScope(frame)) {}
    }
    const auto note = quantile(note_times);
    require(accepted_commands == 1360, "rapid maximum-layer NoteOn/Off commands were dropped");
    std::printf("17-layer audio diagnostic (%d measured 800-sample ticks): capture_off median=%.2fus p95=%.2fus; capture_on median=%.2fus p95=%.2fus; PCM identical=yes; rapid NoteOn/Off blocks median=%.2fus p95=%.2fus accepted=%zu/1360\n",
        measured_ticks, off.median_us, off.p95_us, on.median_us, on.p95_us,
        note.median_us, note.p95_us, accepted_commands);
}

std::size_t firstPcmAfterQueuedNoteOn(bool capture_scope) {
    RealtimeEngineHost host;
    host.setOpllScopeEnabled(capture_scope);
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "engine edit unavailable for NoteOn response probe");
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(*edit.engine, defaultCompositeTimbre(), {.polyphonic = true}, &plan),
        "program compile failed for NoteOn response probe");
    require(host.submitProgram(edit), "program submit failed for NoteOn response probe");
    require(!plan.audible_layers.empty(), "default plan has no layer for NoteOn response probe");
    require(host.submit(EngineCommand::noteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60)),
        "queued NoteOn rejected in response probe");
    std::array<float, 1024> output{};
    constexpr std::size_t maximum_samples = 8192;
    for (std::size_t offset = 0; offset < maximum_samples; offset += output.size() / 2) {
        require(host.render(output).ok(), "render failed in NoteOn response probe");
        for (std::size_t frame = 0; frame < output.size() / 2; ++frame) {
            if (std::abs(output[frame * 2]) > 1.0e-9F
                || std::abs(output[frame * 2 + 1]) > 1.0e-9F)
                return offset + frame;
        }
    }
    throw std::runtime_error("queued NoteOn did not produce PCM within the response probe window");
}
}

int main(int argc, char** argv) {
    try {
        testWindowSelectionBoundariesAndFiniteOutput();
        testUnipolarPeriodicAndConstantDcSignals();
        testRollingColoredNoiseStaysRecentAndReadBounded();
        testPeriodicPhaseStabilityAndNonPeriodicFallback();
        testSamplingAndPeakPreservingPath();
        testCompositeWindowPathAndSmallHistories();
        testScopeCaptureDoesNotChangeRenderedAudio();
        testRenderedPcmScopeScenarios();
        testScopeGainMetadataAcrossPartialRenderCalls();
        if (argc > 1 && std::string(argv[1]) == "--benchmark") {
            displayTimingDiagnostic();
            renderTimingDiagnostic();
            maximumLayerTimingDiagnostic();
            const auto onset_without_capture = firstPcmAfterQueuedNoteOn(false);
            const auto onset_with_capture = firstPcmAfterQueuedNoteOn(true);
            require(onset_without_capture == onset_with_capture,
                "scope capture changed the queued NoteOn-to-PCM sample offset");
            std::printf("queued NoteOn to first nonzero rendered PCM: capture_off=%zu samples capture_on=%zu samples (EngineHost mock render; not a device/DAW latency measurement)\n",
                onset_without_capture, onset_with_capture);
        }
        std::puts("scope waveform tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "scope waveform test failure: %s\n", error.what());
        return 1;
    }
}
