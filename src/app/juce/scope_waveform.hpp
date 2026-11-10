// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <juce_graphics/juce_graphics.h>

namespace mgstc::app {

[[nodiscard]] inline float finiteScopeSample(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0F;
}

[[nodiscard]] inline std::uint8_t scopeDisplayNote(std::uint64_t context,
    std::uint8_t selected_note, std::uint8_t ui_note) noexcept {
    return (context & (std::uint64_t{1} << 63)) != 0 ? selected_note : ui_note;
}

[[nodiscard]] inline double scopePeriodSamples(unsigned midi_note, double sample_rate = 48000.0) noexcept {
    return sample_rate / (440.0 * std::pow(2.0, (static_cast<double>(midi_note) - 69.0) / 12.0));
}

template <typename SampleAt>
[[nodiscard]] float scopePeak(std::size_t sample_count, SampleAt sample_at, float floor = 0.0001F) {
    float peak = floor;
    for (std::size_t index = 0; index < sample_count; ++index)
        peak = std::max(peak, std::abs(finiteScopeSample(sample_at(index))));
    return peak;
}

struct ScopeWindow {
    double start{};
    double span{};
    bool synchronized{};
};

// Message-thread waveform cadence; the host timer and performance control stay
// at their existing rate. Call once per tick, including ticks with no new PCM.
class ScopeDisplayCadence final {
public:
    void reset() noexcept { interval_ = 1; remaining_ = healthy_ = 0; }
    [[nodiscard]] bool shouldRefresh(bool urgent = false) noexcept {
        if (urgent) { remaining_ = 0; return true; }
        if (remaining_ == 0) return true;
        --remaining_;
        return false;
    }
    void recordRefresh(double elapsed_ms) noexcept {
        if (!std::isfinite(elapsed_ms) || elapsed_ms > 3.0) {
            interval_ = std::min(interval_ * 2U, 4U);
            healthy_ = 0;
        } else if (elapsed_ms < 1.5) {
            if (++healthy_ >= 8) {
                interval_ = std::max(1U, interval_ / 2U);
                healthy_ = 0;
            }
        } else healthy_ = 0;
        remaining_ = interval_ - 1;
    }
private:
    unsigned interval_{1}, remaining_{}, healthy_{};
};

template <typename SampleAt>
[[nodiscard]] float scopeSampleLinear(std::size_t count, SampleAt sample_at, double position) {
    if (count == 0 || !std::isfinite(position)) return 0.0F;
    position = std::clamp(position, 0.0, static_cast<double>(count - 1));
    const auto left = static_cast<std::size_t>(position);
    const auto right = std::min(left + 1, count - 1);
    const auto a = finiteScopeSample(sample_at(left));
    const auto b = finiteScopeSample(sample_at(right));
    return static_cast<float>(static_cast<double>(a)
        + (static_cast<double>(b) - static_cast<double>(a)) * (position - static_cast<double>(left)));
}

// Message-thread display work only. Storage, candidate count, history scan and
// correlation length are bounded independently of rendering width and voices.
class ScopeSynchronizer final {
public:
    static constexpr std::size_t kMaxCandidates = 32;
    static constexpr std::size_t kCorrelationSamples = 128;
    static constexpr std::size_t kMaxHistorySamples = 4096;

    void reset() noexcept {
        previous_available_ = false;
        previous_peak_ = 0.0F;
        previous_display_end_ = -1.0;
    }

    template <typename SampleAt>
    [[nodiscard]] ScopeWindow select(std::size_t count, SampleAt sample_at,
        double period, bool allow_search = true, std::uint64_t sample_end = 0) {
        if (count < 2 || !std::isfinite(period) || period < 2.0) {
            reset();
            return {0.0, count > 0 ? static_cast<double>(count - 1) : 0.0, false};
        }
        const auto used = std::min(count, kMaxHistorySamples);
        const auto offset = count - used;
        const auto sample = [&](double position) {
            return scopeSampleLinear(used,
                [&](std::size_t index) { return sample_at(offset + index); }, position);
        };
        const double span = std::min(period * 2.0, static_cast<double>(used - 1));
        const double newest = static_cast<double>(used - 1) - span;
        ScopeWindow result{static_cast<double>(offset) + newest, span, false};
        const auto absoluteEnd = [&](double start) {
            return static_cast<double>(sample_end) - static_cast<double>(used) + start + span;
        };
        const auto rawFallback = [&] {
            if (sample_end != 0) previous_display_end_ = absoluteEnd(newest);
            return result;
        };
        const auto finish = [&](double start, bool synchronized) {
            float peak = 0.0F;
            for (std::size_t i = 0; i < previous_.size(); ++i) {
                previous_[i] = sample(start + span * static_cast<double>(i)
                    / static_cast<double>(previous_.size() - 1));
                peak = std::max(peak, std::abs(previous_[i]));
            }
            previous_peak_ = peak;
            previous_available_ = true;
            if (synchronized && sample_end != 0) previous_display_end_ = absoluteEnd(start);
            return ScopeWindow{static_cast<double>(offset) + start, span, synchronized};
        };
        if (!allow_search || span < period * 1.99) {
            reset();
            return rawFallback();
        }
        // Inspect the recent PCM at full resolution: an old loud segment must
        // not keep a silent tail or a newly changing waveform on screen.
        float recent_peak = 0.0F;
        float recent_low = sample(newest), recent_high = recent_low;
        double recent_sum = 0.0;
        const auto recent_first = static_cast<std::size_t>(newest);
        for (std::size_t i = recent_first; i < used; ++i) {
            const float value = sample(static_cast<double>(i));
            recent_peak = std::max(recent_peak, std::abs(value));
            recent_low = std::min(recent_low, value);
            recent_high = std::max(recent_high, value);
            recent_sum += value;
        }
        if (recent_peak < 0.00001F || recent_high - recent_low < 0.00002F) {
            reset(); return rawFallback();
        }
        // Unipolar chip PCM and half-wave patches need a crossing relative to
        // their recent mean. Only analysis is centered; display PCM is intact.
        const float crossing_level = static_cast<float>(recent_sum / static_cast<double>(used - recent_first));

        std::array<double, kMaxCandidates> candidates{};
        std::size_t candidate_count = 0;
        const auto first = static_cast<std::size_t>(std::max(1.0, newest - period * 2.5));
        const auto last = static_cast<std::size_t>(std::floor(newest));
        // Scan newest first so exhaustion discards older candidates.
        for (std::size_t i = last; i >= first && i > 0; --i) {
            const float a = sample(static_cast<double>(i - 1)) - crossing_level;
            const float b = sample(static_cast<double>(i)) - crossing_level;
            if (a <= 0.0F && b > 0.0F) {
                candidates[candidate_count++] = static_cast<double>(i - 1)
                    - static_cast<double>(a) / static_cast<double>(b - a);
                if (candidate_count == candidates.size()) break;
            }
        }
        if (candidate_count == 0) return rawFallback();

        const bool compare_previous = previous_available_
            && recent_peak < previous_peak_ * 2.5F
            && recent_peak > previous_peak_ * 0.2F;
        double best_start = newest;
        double best_score = -2.0;
        bool found = false;
        std::array<float, kCorrelationSamples> candidate{};
        std::array<float, kCorrelationSamples> next_cycle{};
        const auto recentLagCorrelation = [&](double lag) {
            const double comparison_span = std::min(lag, static_cast<double>(used - 1) - lag);
            if (comparison_span < std::max(16.0, lag * 0.25)) return -1.0;
            std::array<float, kCorrelationSamples> a{}, b{};
            for (std::size_t i = 0; i < a.size(); ++i) {
                const double position = static_cast<double>(used - 1) - comparison_span
                    + comparison_span * static_cast<double>(i) / static_cast<double>(a.size() - 1);
                a[i] = sample(position);
                b[i] = sample(position - lag);
            }
            return correlation(a, b);
        };
        bool nominal_checked = false, nominal_periodic = false;
        const auto recentNominalPeriodicity = [&] {
            if (!nominal_checked) {
                nominal_periodic = recentLagCorrelation(period) >= 0.65;
                nominal_checked = true;
            }
            return nominal_periodic;
        };
        bool alternate_checked = false, alternate_periodic = false;
        const auto alternatePeriodicity = [&] {
            if (alternate_checked) return alternate_periodic;
            alternate_checked = true;
            // Validate an alternate lag on independent portions of the recent
            // PCM. An overlapping old noise window can match a phase reference
            // perfectly, so previous correlation alone must never enable sync.
            for (const double factor : {0.5, 2.0, 4.0}) {
                if (recentLagCorrelation(period * factor) >= 0.90) { alternate_periodic = true; break; }
            }
            return alternate_periodic;
        };
        for (std::size_t c = 0; c < candidate_count; ++c) {
            const double start = candidates[c];
            if (sample_end != 0 && absoluteEnd(start) + 1.0e-6 < previous_display_end_) continue;
            for (std::size_t i = 0; i < candidate.size(); ++i) {
                const double phase = static_cast<double>(i) / static_cast<double>(candidate.size() - 1);
                candidate[i] = sample(start + phase * period);
                next_cycle[i] = sample(start + period + phase * period);
            }
            // A periodicity test protects MIX, noise and real modulation from
            // being held at a visually similar but unrelated old phase.
            const double periodicity = correlation(candidate, next_cycle);
            double similarity = periodicity;
            if (compare_previous) {
                for (std::size_t i = 0; i < candidate.size(); ++i)
                    candidate[i] = sample(start + span * static_cast<double>(i)
                        / static_cast<double>(candidate.size() - 1));
                similarity = correlation(candidate, previous_);
                if (similarity < 0.55) continue;
            }
            // A pitch label is only a hint: chip tuning and patch multipliers
            // can differ. A strong phase match plus an independently checked
            // alternate lag handles these without holding overlapping noise.
            if ((periodicity < 0.65 || !recentNominalPeriodicity())
                && (!compare_previous || similarity < 0.90 || !alternatePeriodicity())) continue;
            const double age = (newest - start) / std::max(1.0, period);
            const double score = similarity - age * 0.002;
            if (!found || score > best_score) {
                found = true;
                best_score = score;
                best_start = start;
            }
        }
        if (found) return finish(best_start, true);
        // Seed the next bounded comparison without phase-shifting the raw
        // fallback shown now. This also handles a new, differently shaped note.
        static_cast<void>(finish(candidates[0], false));
        return rawFallback();
    }

private:
    [[nodiscard]] static double correlation(
        const std::array<float, kCorrelationSamples>& a,
        const std::array<float, kCorrelationSamples>& b) noexcept {
        double sum_a = 0.0, sum_b = 0.0, aa = 0.0, bb = 0.0, ab = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double x = a[i], y = b[i];
            sum_a += x; sum_b += y; aa += x * x; bb += y * y; ab += x * y;
        }
        const double n = static_cast<double>(a.size());
        const double energy = std::sqrt(std::max(0.0, aa - sum_a * sum_a / n)
            * std::max(0.0, bb - sum_b * sum_b / n));
        return energy > 1.0e-12 ? (ab - sum_a * sum_b / n) / energy : -1.0;
    }

    std::array<float, kCorrelationSamples> previous_{};
    float previous_peak_{};
    double previous_display_end_{-1.0};
    bool previous_available_{};
};

// The path represents the piecewise-linear original PCM. Dense views retain
// both extrema in each pixel bucket, in source order. Sparse views connect the
// original samples linearly, including interpolated sub-sample window edges.
template <typename SampleAt>
void buildScopeWindowPath(juce::Path& path, juce::Rectangle<float> bounds,
    std::size_t count, SampleAt sample_at, ScopeWindow window,
    float gain, bool clamp = false) {
    path.clear();
    if (count < 2 || bounds.isEmpty() || !std::isfinite(window.start)
        || !std::isfinite(window.span) || window.span <= 0.0) return;
    window.start = std::clamp(window.start, 0.0, static_cast<double>(count - 1));
    window.span = std::min(window.span, static_cast<double>(count - 1) - window.start);
    if (window.span <= 0.0) return;
    gain = finiteScopeSample(gain);
    const auto sample = [&](double position) { return scopeSampleLinear(count, sample_at, position); };
    const auto columns = static_cast<std::size_t>(std::clamp(std::ceil(bounds.getWidth()), 1.0F, 16384.0F));
    const bool dense = window.span > static_cast<double>(columns) * 1.25;
    const auto vertices = dense ? columns * 4 + 2 : static_cast<std::size_t>(std::ceil(window.span)) + 2;
    path.preallocateSpace(static_cast<int>(vertices * 3 + 4));
    bool first = true;
    const auto emit = [&](double position, float source) {
        float value = finiteScopeSample(source * gain);
        if (clamp) value = std::clamp(value, -1.0F, 1.0F);
        const float x = bounds.getX() + static_cast<float>((position - window.start) / window.span) * bounds.getWidth();
        const float y = bounds.getCentreY() - value * bounds.getHeight() * 0.5F;
        if (first) { path.startNewSubPath(x, y); first = false; }
        else path.lineTo(x, y);
    };
    emit(window.start, sample(window.start));
    if (dense) {
        for (std::size_t column = 0; column < columns; ++column) {
            const double left = window.start + window.span * static_cast<double>(column) / static_cast<double>(columns);
            const double right = window.start + window.span * static_cast<double>(column + 1) / static_cast<double>(columns);
            float low = sample(left), high = low;
            double low_position = left, high_position = left;
            for (auto i = static_cast<std::size_t>(std::ceil(left));
                i <= static_cast<std::size_t>(std::floor(right)); ++i) {
                const float value = finiteScopeSample(sample_at(i));
                if (value < low) { low = value; low_position = static_cast<double>(i); }
                if (value > high) { high = value; high_position = static_cast<double>(i); }
            }
            const float end = sample(right);
            if (end < low) { low = end; low_position = right; }
            if (end > high) { high = end; high_position = right; }
            if (low_position <= high_position) { emit(low_position, low); emit(high_position, high); }
            else { emit(high_position, high); emit(low_position, low); }
            emit(right, end);
        }
    } else {
        for (auto i = static_cast<std::size_t>(std::floor(window.start)) + 1;
            static_cast<double>(i) < window.start + window.span; ++i)
            emit(static_cast<double>(i), finiteScopeSample(sample_at(i)));
        emit(window.start + window.span, sample(window.start + window.span));
    }
}

template <typename SampleAt>
void buildScopePath(juce::Path& path, juce::Rectangle<float> bounds,
    std::size_t points, SampleAt sample_at, float gain, bool clamp = false) {
    buildScopeWindowPath(path, bounds, points, sample_at,
        {0.0, points > 0 ? static_cast<double>(points - 1) : 0.0, false}, gain, clamp);
}

} // namespace mgstc::app
