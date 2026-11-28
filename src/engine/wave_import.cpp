#include "mgstc/engine/wave_import.hpp"
#include "mgstc/engine/detail/opll_perceptual_test_access.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <set>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <map>
#include <stdexcept>

#include "emu2413.h"
#include "mgstc/engine/note_pitch.hpp"

namespace mgstc::engine {

struct OpllContinuousSearchState {
    using Clock = std::chrono::steady_clock;
    const OpllContinuousOptions& options;
    OpllContinuousResult result;
    Clock::time_point started;
    Clock::time_point deadline;
    std::chrono::milliseconds limit;
    OpllApproximationPhase phase{OpllApproximationPhase::Idle};
    bool timed_out{};
    std::uint32_t seed{0x47415043U};
    std::uint64_t duplicate_cache_hits{}, fallback_candidates{}, generations{};
    bool ga_used{};
    float fundamental_hz{};
    std::size_t key_off_sample{}, sample_count{};
    std::uint64_t next_id{};

    explicit OpllContinuousSearchState(const OpllContinuousOptions& value)
        : options(value), started(now()),
          limit(options.test_hooks ? std::max(std::chrono::milliseconds{0},
                    options.test_hooks->time_limit) : std::chrono::milliseconds{300'000}) {
        deadline = started + limit;
    }
    Clock::time_point now() const {
        return options.test_hooks && options.test_hooks->now
            ? options.test_hooks->now() : Clock::now();
    }
    bool stopped() {
        if (options.control && options.control->cancelRequested()) return true;
        timed_out = timed_out || now() >= deadline;
        return timed_out;
    }
    std::uint64_t elapsed() const {
        const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(now() - started);
        return static_cast<std::uint64_t>(std::clamp(value.count(),
            std::int64_t{0}, static_cast<std::int64_t>(limit.count())));
    }
    void rank(const OpllPatchParameters& patch, const OpllFullScoreBreakdown& breakdown) {
        const auto score = breakdown.total;
        if (!std::isfinite(score) || (options.control && options.control->cancelRequested())) return;
        const auto registers = encodeOpllPatch(patch);
        const auto less = [](const auto& left, const auto& right) {
            return left->full_score != right->full_score
                ? left->full_score < right->full_score : left->registers < right->registers;
        };
        if (std::any_of(result.ranked.begin(), result.ranked.end(),
                [&](const auto& entry) { return entry->registers == registers; })) return;
        if (result.ranked.size() == 6) {
            const auto& last = result.ranked.back();
            if (score > last->full_score || (score == last->full_score && registers >= last->registers)) return;
        }
        auto candidate = std::make_shared<const OpllContinuousCandidate>(OpllContinuousCandidate{
            ++next_id, patch, registers, score, fundamental_hz,
            options.fixed_midi_note, key_off_sample, sample_count,
            result.full_evaluations, breakdown});
        result.ranked.push_back(candidate);
        std::sort(result.ranked.begin(), result.ranked.end(), less);
        if (result.ranked.size() > 6) result.ranked.resize(6);
    }

    void publishFront() {
        if (result.ranked.empty() || (options.control && options.control->cancelRequested())) return;
        const auto& candidate = result.ranked.front();
        const auto& previous = result.published_best;
        auto decision = OpllPublicationDecision::Published;
        if (previous) {
            if (candidate->full_score > previous->full_score
                || (candidate->full_score == previous->full_score
                    && candidate->registers >= previous->registers)) {
                decision = OpllPublicationDecision::NoImprovement;
            } else {
                decision = publicationDecision(previous->breakdown, candidate->breakdown);
            }
        }
        if (decision == OpllPublicationDecision::Published) {
            result.published_best = candidate;
            if (options.candidate_ready) options.candidate_ready(candidate);
        }
        if (options.test_hooks && options.test_hooks->publication_decided)
            options.test_hooks->publication_decided(*candidate, decision);
    }

    static OpllPublicationDecision publicationDecision(
        const OpllFullScoreBreakdown&, const OpllFullScoreBreakdown&);
};

struct OpllApproximationCoordinator {
    explicit OpllApproximationCoordinator(
        const OpllApproximationOptions& options,
        std::uint64_t total,
        OpllContinuousSearchState* continuous_state = nullptr)
        : control(options.control
              ? options.control
              : std::make_shared<OpllApproximationControl>()), continuous(continuous_state) {
        control->state_->completed.store(0, std::memory_order_relaxed);
        control->state_->total.store(total, std::memory_order_release);
        control->state_->phase.store(
            OpllApproximationPhase::Idle,
            std::memory_order_release);
        control->state_->elapsed_milliseconds.store(0, std::memory_order_relaxed);
        control->state_->time_limit_milliseconds.store(
            continuous ? static_cast<std::uint64_t>(continuous->limit.count()) : 0,
            std::memory_order_relaxed);
        control->state_->initial_completed.store(false, std::memory_order_relaxed);
        control->state_->continuous_started_ns.store(continuous
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() : 0,
            std::memory_order_relaxed);
        control->state_->continuous_running.store(continuous != nullptr, std::memory_order_release);
    }

    [[nodiscard]] bool cancelled() const {
        return control->state_->cancel_requested.load(std::memory_order_acquire)
            || (continuous && continuous->stopped());
    }

    void setPhase(OpllApproximationPhase phase) noexcept {
        if (continuous) continuous->phase = phase;
        control->state_->phase.store(phase, std::memory_order_release);
    }

    void advance(std::uint64_t work) noexcept {
        const auto total =
            control->state_->total.load(std::memory_order_relaxed);
        const auto previous =
            control->state_->completed.load(std::memory_order_relaxed);
        control->state_->completed.store(
            std::min(total, previous + work),
            std::memory_order_release);
    }

    void updateTime() {
        if (!continuous) return;
        control->state_->elapsed_milliseconds.store(continuous->elapsed(), std::memory_order_release);
        control->state_->initial_completed.store(continuous->result.initial_completed, std::memory_order_release);
    }
    bool synchronousFallback() const {
        return continuous && continuous->options.test_hooks
            && continuous->options.test_hooks->force_synchronous_fallback;
    }
    void setTotal(std::uint64_t total) noexcept {
        control->state_->total.store(total, std::memory_order_release);
    }
    void finishContinuous(bool cancelled) {
        updateTime();
        control->state_->continuous_running.store(false, std::memory_order_release);
        control->state_->phase.store(cancelled ? OpllApproximationPhase::Cancelled
            : OpllApproximationPhase::Completed, std::memory_order_release);
    }

    void finish(bool was_cancelled) noexcept {
        if (was_cancelled) {
            control->state_->phase.store(
                OpllApproximationPhase::Cancelled,
                std::memory_order_release);
            return;
        }
        const auto total =
            control->state_->total.load(std::memory_order_relaxed);
        control->state_->completed.store(total, std::memory_order_release);
        control->state_->phase.store(
            OpllApproximationPhase::Completed,
            std::memory_order_release);
    }

    std::shared_ptr<OpllApproximationControl> control;
    OpllContinuousSearchState* continuous{};
};

std::vector<OpllPatchParameters> searchWaveCycleWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    std::size_t evaluation_budget,
    OpllApproximationPhase phase);

namespace {

constexpr std::size_t kSteadyBatchSize = 256;
constexpr std::size_t kFullWaveBatchSize = 32;
constexpr std::size_t kStandardSteadyBatchSize = 5'000;
constexpr std::size_t kStandardFullWaveBatchSize = 360;

struct SearchBudgets {
    std::size_t representative{};
    std::size_t short_timbre{};
    std::size_t full_envelope{};
};

SearchBudgets searchBudgets(const OpllApproximationOptions& options) {
    if (options.profile == OpllApproximationProfile::Compact) {
        return SearchBudgets{384, 192, 16};
    }
    return options.effort == OpllApproximationEffort::Thorough
        ? SearchBudgets{48'000, 48'000, 6'000}
        : SearchBudgets{5'000, 2'100, 360};
}

std::uint64_t fullWaveWeight(std::size_t samples) {
    return 1U + static_cast<std::uint64_t>((samples + 4'095U) / 4'096U);
}

std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        bytes[offset]
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(
        bytes[offset]
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24));
}

float normalizedSample(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::uint16_t format,
    std::uint16_t bits) {
    if (format == 3 && bits == 32) {
        float value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
    }
    switch (bits) {
    case 8:
        return (static_cast<float>(bytes[offset]) - 128.0F) / 128.0F;
    case 16: {
        const auto value = static_cast<std::int16_t>(u16(bytes, offset));
        return static_cast<float>(value) / 32768.0F;
    }
    case 24: {
        std::int32_t value = static_cast<std::int32_t>(
            bytes[offset]
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16));
        if (value & 0x00800000) {
            value |= static_cast<std::int32_t>(0xFF000000);
        }
        return static_cast<float>(value) / 8388608.0F;
    }
    case 32: {
        const auto value = static_cast<std::int32_t>(u32(bytes, offset));
        return static_cast<float>(value) / 2147483648.0F;
    }
    default:
        return 0.0F;
    }
}

std::vector<float> normalizedCycle(std::span<const float> input) {
    if (input.empty()) {
        return {};
    }
    std::vector<float> result(input.begin(), input.end());
    float mean{};
    for (const float sample : result) {
        mean += sample;
    }
    mean /= static_cast<float>(result.size());
    float peak{};
    for (float& sample : result) {
        sample -= mean;
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 0.000001F) {
        for (float& sample : result) {
            sample /= peak;
        }
    }
    return result;
}

float periodicDifference(
    std::span<const float> samples,
    std::size_t lag) {
    double error{};
    double energy{};
    const std::size_t count = samples.size() - lag;
    for (std::size_t index = 0; index < count; ++index) {
        const double a = samples[index];
        const double b = samples[index + lag];
        const double difference = a - b;
        error += difference * difference;
        energy += a * a + b * b;
    }
    return energy > 1.0e-12
        ? static_cast<float>(error / energy)
        : std::numeric_limits<float>::max();
}

constexpr std::uint32_t kChipClock = 3'579'545;
constexpr std::uint32_t kAuditionSampleRate = 48'000;
constexpr std::uint8_t kAuditionMidiNote = 60;
constexpr std::size_t kAuditionWarmup = 1'024;
constexpr std::size_t kAuditionSamples = 4'096;

void removeDcAndNormalize(std::vector<float>& samples);

float sccFrequencyHz(std::uint16_t period) noexcept {
    return static_cast<float>(
        static_cast<double>(kChipClock)
        / ((static_cast<double>(period) + 1.0) * 32.0));
}

std::array<float, 16> spectrum(std::span<const float> cycle) {
    std::array<float, 16> result{};
    if (cycle.empty()) {
        return result;
    }
    for (std::size_t harmonic = 1; harmonic < result.size(); ++harmonic) {
        double real{};
        double imaginary{};
        for (std::size_t index = 0; index < cycle.size(); ++index) {
            const double angle =
                2.0 * std::numbers::pi
                * static_cast<double>(harmonic * index)
                / static_cast<double>(cycle.size());
            real += static_cast<double>(cycle[index]) * std::cos(angle);
            imaginary -= static_cast<double>(cycle[index]) * std::sin(angle);
        }
        result[harmonic] = static_cast<float>(
            std::sqrt(real * real + imaginary * imaginary));
    }
    float total{};
    for (const float value : result) {
        total += value;
    }
    if (total > 0.000001F) {
        for (float& value : result) {
            value /= total;
        }
    }
    return result;
}

std::array<float, 32> resampleCycle32(
    std::span<const float> cycle) {
    std::array<float, 32> result{};
    const auto normalized = normalizedCycle(cycle);
    if (normalized.empty()) {
        return result;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * normalized.size() / result.size();
        const auto first =
            static_cast<std::size_t>(position) % normalized.size();
        const auto second = (first + 1) % normalized.size();
        const float fraction =
            static_cast<float>(position - std::floor(position));
        result[index] =
            normalized[first] * (1.0F - fraction)
            + normalized[second] * fraction;
    }
    return result;
}

std::array<float, 32> resampleWindow32(
    std::span<const float> samples,
    std::size_t begin,
    std::size_t length) {
    std::array<float, 32> result{};
    if (length < 2 || begin + length > samples.size()) {
        return result;
    }
    double mean{};
    for (std::size_t index = 0; index < length; ++index) {
        mean += samples[begin + index];
    }
    mean /= static_cast<double>(length);
    float peak{};
    for (std::size_t index = 0; index < length; ++index) {
        peak = std::max(
            peak,
            std::abs(samples[begin + index] - static_cast<float>(mean)));
    }
    const float scale = peak > 1.0e-6F ? 1.0F / peak : 1.0F;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * length / result.size();
        const auto first = static_cast<std::size_t>(position) % length;
        const auto second = (first + 1) % length;
        const float fraction =
            static_cast<float>(position - std::floor(position));
        const float a =
            (samples[begin + first] - static_cast<float>(mean)) * scale;
        const float b =
            (samples[begin + second] - static_cast<float>(mean)) * scale;
        result[index] = a * (1.0F - fraction) + b * fraction;
    }
    return result;
}

struct OpllDeleter {
    void operator()(OPLL* chip) const noexcept {
        OPLL_delete(chip);
    }
};

std::vector<float> synthesizePeriodicAudition(
    std::span<const float> cycle,
    float frequency_hz,
    std::size_t sample_count) {
    std::vector<float> result(sample_count, 0.0F);
    if (cycle.size() < 2 || !(frequency_hz > 0.0F)) {
        return result;
    }
    double phase = 0.0;
    const double step =
        static_cast<double>(frequency_hz) * static_cast<double>(cycle.size())
        / static_cast<double>(kAuditionSampleRate);
    for (float& sample : result) {
        const auto first =
            static_cast<std::size_t>(phase) % cycle.size();
        const auto second = (first + 1) % cycle.size();
        const float fraction = static_cast<float>(
            phase - std::floor(phase));
        sample = cycle[first] * (1.0F - fraction)
            + cycle[second] * fraction;
        phase += step;
        while (phase >= static_cast<double>(cycle.size())) {
            phase -= static_cast<double>(cycle.size());
        }
    }
    removeDcAndNormalize(result);
    return result;
}

float cyclePairDistance(
    const std::array<float, 32>& target_cycle,
    const std::array<float, 32>& generated_cycle) {
    const auto target_spectrum = spectrum(target_cycle);
    const auto generated_spectrum = spectrum(generated_cycle);
    float distance{};
    float cumulative{};
    float wasserstein{};
    for (std::size_t index = 1; index < target_spectrum.size(); ++index) {
        const float weight = 1.0F / std::sqrt(static_cast<float>(index));
        const float difference =
            std::log1p(target_spectrum[index] * 40.0F)
            - std::log1p(generated_spectrum[index] * 40.0F);
        distance += weight * difference * difference;
        cumulative += target_spectrum[index] - generated_spectrum[index];
        wasserstein += std::abs(cumulative);
    }
    distance += wasserstein * 1.6F;
    float best_shape = std::numeric_limits<float>::max();
    for (std::size_t shift = 0; shift < target_cycle.size(); ++shift) {
        float normal_error{};
        float inverted_error{};
        for (std::size_t index = 0; index < target_cycle.size(); ++index) {
            const float generated =
                generated_cycle[(index + shift) % generated_cycle.size()];
            const float difference = target_cycle[index] - generated;
            const float inverted = target_cycle[index] + generated;
            normal_error += difference * difference;
            inverted_error += inverted * inverted;
        }
        best_shape = std::min(
            best_shape,
            std::min(normal_error, inverted_error)
                / static_cast<float>(target_cycle.size()));
    }
    distance += best_shape * 0.45F;
    return distance;
}

// Compare target vs generated using one SCC-period-length window. An octave
// error packs 2 (or 1/2) cycles into that window and scores poorly.
float periodWindowDistance(
    std::span<const float> target,
    std::span<const float> generated,
    std::size_t period_samples) {
    if (target.size() < period_samples * 2
        || generated.size() < period_samples * 2
        || period_samples < 8) {
        return 0.0F;
    }
    const std::size_t target_begin =
        (target.size() - period_samples) / 2;
    const auto target_cycle =
        resampleWindow32(target, target_begin, period_samples);
    const std::size_t generated_begin =
        (generated.size() - period_samples) / 2;
    return cyclePairDistance(
        target_cycle,
        resampleWindow32(generated, generated_begin, period_samples));
}

constexpr std::uint32_t kOpllClock = kChipClock;
constexpr std::uint32_t kOpllFitSampleRate = kAuditionSampleRate;
constexpr std::size_t kEnvelopeBins = 32;
constexpr std::size_t kSpectralBins = 80;
constexpr std::size_t kSpectralFrames = 3;
constexpr std::array<std::size_t, 3> kFftSizes{256, 1024, 4096};

using SpectralDistribution = std::array<float, kSpectralBins>;

struct TimedAudioFeatures {
    std::array<float, kEnvelopeBins> envelope{};
    std::array<
        SpectralDistribution,
        kFftSizes.size() * kSpectralFrames> spectra{};
};

struct TimedFeatureWorkspace {
    std::vector<std::complex<float>> fft_values;
};

constexpr std::size_t kDirectHarmonics = 20;

struct DirectHarmonics {
    std::array<float, kDirectHarmonics> amplitude{};
    std::size_t count{};
    std::size_t fft_size{};
    float radius{};
    bool estimate_confidence{};
    float spectrum_power{};
    std::size_t harmonic_bins{};
    std::size_t spectrum_bins{};
};

struct OpllFitTarget {
    std::optional<OpllPitch> fixed_pitch;
    std::vector<float> samples;
    TimedAudioFeatures features;
    float fundamental_hz{261.625565F};
    std::size_t key_off_sample{std::numeric_limits<std::size_t>::max()};
    std::size_t render_offset_sample{};
};

OpllPitch closestOpllPitch(float frequency_hz) noexcept {
    OpllPitch best{1, 0};
    double best_error = std::numeric_limits<double>::max();
    const double target = std::max(1.0, static_cast<double>(frequency_hz));
    for (std::uint8_t block = 0; block < 8; ++block) {
        const double scale =
            static_cast<double>(kOpllClock)
            / (72.0 * static_cast<double>(1U << (19 - block)));
        const auto f_number = static_cast<std::uint16_t>(std::clamp(
            static_cast<int>(std::lround(target / scale)),
            1,
            511));
        const double rendered = static_cast<double>(f_number) * scale;
        const double error = std::abs(std::log2(rendered / target));
        if (error < best_error) {
            best_error = error;
            best = OpllPitch{f_number, block};
        }
    }
    return best;
}

std::vector<float> resampleAudio(
    std::span<const float> input,
    std::uint32_t source_rate,
    std::uint32_t destination_rate) {
    if (input.empty() || source_rate == 0 || destination_rate == 0) {
        return {};
    }
    const std::size_t size = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::llround(
                static_cast<double>(input.size()) * destination_rate
                / source_rate)));
    std::vector<float> result(size);
    for (std::size_t index = 0; index < size; ++index) {
        const double position =
            static_cast<double>(index) * source_rate / destination_rate;
        const auto first = std::min(
            static_cast<std::size_t>(position),
            input.size() - 1);
        const auto second = std::min(first + 1, input.size() - 1);
        const float fraction =
            static_cast<float>(position - static_cast<double>(first));
        result[index] =
            input[first] * (1.0F - fraction) + input[second] * fraction;
    }
    return result;
}

void removeDcAndNormalize(std::vector<float>& samples) {
    if (samples.empty()) {
        return;
    }
    double mean{};
    for (const float sample : samples) {
        mean += sample;
    }
    mean /= static_cast<double>(samples.size());
    float peak{};
    for (float& sample : samples) {
        sample -= static_cast<float>(mean);
        peak = std::max(peak, std::abs(sample));
    }
    if (peak > 1.0e-6F) {
        for (float& sample : samples) {
            sample /= peak;
        }
    }
}

std::array<float, kEnvelopeBins> amplitudeEnvelope(
    std::span<const float> samples) {
    std::array<float, kEnvelopeBins> result{};
    if (samples.empty()) {
        return result;
    }
    for (std::size_t bin = 0; bin < result.size(); ++bin) {
        const std::size_t begin = bin * samples.size() / result.size();
        const std::size_t end = std::max(
            begin + 1,
            (bin + 1) * samples.size() / result.size());
        double squared{};
        for (std::size_t index = begin;
             index < std::min(end, samples.size());
             ++index) {
            squared += static_cast<double>(samples[index]) * samples[index];
        }
        result[bin] = static_cast<float>(
            std::sqrt(squared / static_cast<double>(end - begin)));
    }
    const float maximum =
        *std::max_element(result.begin(), result.end());
    if (maximum > 1.0e-6F) {
        for (float& value : result) {
            value /= maximum;
        }
    }
    return result;
}

void fft(std::vector<std::complex<float>>& values) {
    const std::size_t size = values.size();
    for (std::size_t index = 1, reversed = 0; index < size; ++index) {
        std::size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(values[index], values[reversed]);
        }
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const float angle =
            -2.0F * std::numbers::pi_v<float>
            / static_cast<float>(length);
        const std::complex<float> root{
            std::cos(angle),
            std::sin(angle)};
        for (std::size_t offset = 0; offset < size; offset += length) {
            std::complex<float> factor{1.0F, 0.0F};
            for (std::size_t index = 0; index < length / 2; ++index) {
                const auto even = values[offset + index];
                const auto odd =
                    values[offset + index + length / 2] * factor;
                values[offset + index] = even + odd;
                values[offset + index + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
}

const std::vector<float>& hannWindow(std::size_t fft_size) {
    static const auto windows = [] {
        std::array<std::vector<float>, kFftSizes.size()> result;
        for (std::size_t size_index = 0;
             size_index < kFftSizes.size();
             ++size_index) {
            const std::size_t size = kFftSizes[size_index];
            result[size_index].resize(size);
            for (std::size_t index = 0; index < size; ++index) {
                result[size_index][index] =
                    0.5F
                    - 0.5F * std::cos(
                        2.0F * std::numbers::pi_v<float>
                        * static_cast<float>(index)
                        / static_cast<float>(size - 1));
            }
        }
        return result;
    }();
    for (std::size_t index = 0; index < kFftSizes.size(); ++index) {
        if (kFftSizes[index] == fft_size) {
            return windows[index];
        }
    }
    return windows.back();
}

SpectralDistribution harmonicDistribution(
    std::span<const float> samples,
    std::size_t center,
    std::size_t fft_size,
    float fundamental_hz,
    TimedFeatureWorkspace& workspace,
    DirectHarmonics* direct = nullptr) {
    workspace.fft_values.assign(
        fft_size,
        std::complex<float>{});
    auto& values = workspace.fft_values;
    const auto& window_values = hannWindow(fft_size);
    const std::size_t half = fft_size / 2;
    const std::size_t begin = center > half ? center - half : 0;
    for (std::size_t index = 0; index < fft_size; ++index) {
        const std::size_t source = begin + index;
        const float sample = source < samples.size() ? samples[source] : 0.0F;
        values[index] = sample * window_values[index];
    }
    fft(values);

    if (direct && fft_size >= 1024 && samples.size() >= fft_size
        && begin <= samples.size() - fft_size
        && fft_size >= direct->fft_size) {
        const float spacing = fundamental_hz * static_cast<float>(fft_size)
            / static_cast<float>(kOpllFitSampleRate);
        if (std::isfinite(spacing) && spacing >= 5.0F) {
            const float radius = std::min(2.25F, spacing * 0.42F);
            if (fft_size > direct->fft_size) {
                const bool estimate_confidence = direct->estimate_confidence;
                *direct = {};
                direct->estimate_confidence = estimate_confidence;
                direct->fft_size = fft_size;
                direct->radius = radius;
            }
            if (direct->estimate_confidence) {
                direct->spectrum_bins += half - 1;
                for (std::size_t bin = 1; bin < half; ++bin)
                    direct->spectrum_power += std::norm(values[bin]);
            }
            for (std::size_t index = 0; index < kDirectHarmonics; ++index) {
                const auto position = spacing * static_cast<float>(index + 1);
                if (position + radius >= static_cast<float>(half)) break;
                const auto first = static_cast<std::size_t>(std::ceil(position - radius));
                const auto last = static_cast<std::size_t>(std::floor(position + radius));
                float power = 0.0F;
                for (auto bin = first; bin <= last; ++bin) power += std::norm(values[bin]);
                direct->amplitude[index] += power;
                if (direct->estimate_confidence) direct->harmonic_bins += last - first + 1;
                direct->count = index + 1;
            }
        }
    }

    SpectralDistribution result{};
    constexpr float harmonic_step = 0.25F;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const float frequency =
            fundamental_hz * harmonic_step * static_cast<float>(index + 1);
        const float fft_position =
            frequency * static_cast<float>(fft_size)
            / static_cast<float>(kOpllFitSampleRate);
        if (fft_position >= static_cast<float>(fft_size / 2 - 1)) {
            break;
        }
        const auto first = static_cast<std::size_t>(fft_position);
        const float fraction =
            fft_position - static_cast<float>(first);
        result[index] =
            std::abs(values[first]) * (1.0F - fraction)
            + std::abs(values[first + 1]) * fraction;
    }
    float total{};
    for (const float value : result) {
        total += value;
    }
    if (total > 1.0e-6F) {
        for (float& value : result) {
            value /= total;
        }
    }
    return result;
}

TimedAudioFeatures timedFeatures(
    std::span<const float> samples,
    float fundamental_hz,
    TimedFeatureWorkspace& workspace,
    DirectHarmonics* direct = nullptr) {
    TimedAudioFeatures result{};
    result.envelope = amplitudeEnvelope(samples);
    std::size_t output{};
    for (const std::size_t fft_size : kFftSizes) {
        for (std::size_t frame = 0; frame < kSpectralFrames; ++frame) {
            const std::size_t center =
                samples.empty()
                    ? 0
                    : (frame + 1) * samples.size()
                        / (kSpectralFrames + 1);
            result.spectra[output++] = harmonicDistribution(
                samples,
                center,
                fft_size,
                fundamental_hz,
                workspace, direct);
        }
    }
    return result;
}

TimedAudioFeatures timedFeatures(
    std::span<const float> samples,
    float fundamental_hz) {
    TimedFeatureWorkspace workspace;
    return timedFeatures(samples, fundamental_hz, workspace);
}

float wassersteinDistance(
    const SpectralDistribution& target,
    const SpectralDistribution& generated) {
    float cumulative{};
    float distance{};
    for (std::size_t index = 0; index < target.size(); ++index) {
        cumulative += target[index] - generated[index];
        distance += std::abs(cumulative) * 0.25F;
    }
    return distance;
}

float featureDistance(
    const TimedAudioFeatures& target,
    const TimedAudioFeatures& generated,
    float* spectrum = nullptr) {
    float spectral{};
    for (std::size_t frame = 0; frame < target.spectra.size(); ++frame) {
        spectral +=
            wassersteinDistance(
                target.spectra[frame],
                generated.spectra[frame])
            * 2.4F;
        for (std::size_t bin = 0; bin < kSpectralBins; ++bin) {
            const float difference =
                std::log1p(target.spectra[frame][bin] * 80.0F)
                - std::log1p(generated.spectra[frame][bin] * 80.0F);
            spectral += std::abs(difference) * 0.025F;
        }
    }
    spectral /= static_cast<float>(target.spectra.size());
    if (spectrum) *spectrum = spectral;

    float envelope{};
    for (std::size_t index = 0; index < target.envelope.size(); ++index) {
        const float difference =
            std::log1p(target.envelope[index] * 20.0F)
            - std::log1p(generated.envelope[index] * 20.0F);
        envelope += difference * difference;
    }
    envelope /= static_cast<float>(target.envelope.size());
    return spectral + envelope * 0.7F;
}

class SteadyOpllRenderer {
public:
    SteadyOpllRenderer()
        : chip_(OPLL_new(kOpllClock, kOpllFitSampleRate)) {}

    const std::vector<float>& render(
        const OpllPatchParameters& patch,
        OpllPitch pitch,
        std::size_t sample_count) {
        result_.assign(sample_count, 0.0F);
        if (!chip_) {
            return result_;
        }
        OPLL_reset(chip_.get());
        OPLL_setChipType(chip_.get(), 0);
        OPLL_resetPatch(chip_.get(), OPLL_2413_TONE);
        const auto registers = encodeOpllPatch(patch);
        for (std::size_t index = 0; index < registers.size(); ++index) {
            OPLL_writeReg(
                chip_.get(),
                static_cast<std::uint32_t>(index),
                registers[index]);
        }
        OPLL_writeReg(
            chip_.get(),
            0x10,
            static_cast<std::uint8_t>(pitch.f_number & 0xFF));
        OPLL_writeReg(chip_.get(), 0x30, 0x00);
        OPLL_writeReg(
            chip_.get(),
            0x20,
            static_cast<std::uint8_t>(
                ((pitch.f_number >> 8) & 1)
                | ((pitch.block & 7) << 1)
                | 0x10));
        for (std::size_t index = 0;
             index < kAuditionWarmup + sample_count;
             ++index) {
            const float sample = static_cast<float>(OPLL_calc(chip_.get()));
            if (index >= kAuditionWarmup) {
                result_[index - kAuditionWarmup] = sample;
            }
        }
        removeDcAndNormalize(result_);
        return result_;
    }

private:
    std::unique_ptr<OPLL, OpllDeleter> chip_;
    std::vector<float> result_;
};

// Score OPLL patches by rendering at the same MIDI note as the SCC/cycle
// reference and comparing both broadband features and one SCC-period window.
class SameMidiScoreContext {
public:
    SameMidiScoreContext(
        std::span<const float> cycle,
        float fundamental_hz,
        std::uint8_t midi_note)
        : fundamental_hz_(fundamental_hz),
          period_samples_(std::clamp<std::size_t>(
              static_cast<std::size_t>(std::llround(
                  static_cast<double>(kAuditionSampleRate)
                  / std::max(static_cast<double>(fundamental_hz), 1.0))),
              16,
              kAuditionSamples / 2)),
          target_(synthesizePeriodicAudition(
              cycle,
              fundamental_hz,
              kAuditionSamples)),
          target_features_(timedFeatures(target_, fundamental_hz_)) {
        NotePitch note_pitch{};
        if (notePitch(midi_note, note_pitch)) {
            opll_pitch_ = note_pitch.opll;
        }
    }

    float score(const OpllPatchParameters& patch) {
        const auto& generated =
            renderer_.render(patch, opll_pitch_, kAuditionSamples);
        float distance = featureDistance(
            target_features_,
            timedFeatures(generated, fundamental_hz_, feature_workspace_));
        distance += periodWindowDistance(
                        target_,
                        generated,
                        period_samples_)
            * 1.25F;
        return distance;
    }

private:
    float fundamental_hz_;
    std::size_t period_samples_;
    std::vector<float> target_;
    TimedAudioFeatures target_features_;
    OpllPitch opll_pitch_{};
    SteadyOpllRenderer renderer_;
    TimedFeatureWorkspace feature_workspace_;
};

class OpllTimedRenderer {
public:
    OpllTimedRenderer()
        : chip_(OPLL_new(kOpllClock, kOpllFitSampleRate)) {}

    [[nodiscard]] bool valid() const noexcept { return chip_ != nullptr; }

    const std::vector<float>& render(
        const OpllPatchParameters& patch,
        float frequency_hz,
        std::size_t sample_count,
        std::size_t key_off_sample,
        std::size_t render_offset_sample,
        const std::optional<OpllPitch>& fixed_pitch = {}) {
        result_.assign(sample_count, 0.0F);
        if (!chip_) {
            return result_;
        }
        OPLL_reset(chip_.get());
        OPLL_setChipType(chip_.get(), 0);
        OPLL_resetPatch(chip_.get(), OPLL_2413_TONE);
        const auto registers = encodeOpllPatch(patch);
        for (std::size_t index = 0; index < registers.size(); ++index) {
            OPLL_writeReg(
                chip_.get(),
                static_cast<std::uint32_t>(index),
                registers[index]);
        }
        const auto pitch = fixed_pitch ? *fixed_pitch : closestOpllPitch(frequency_hz);
        OPLL_writeReg(
            chip_.get(),
            0x10,
            static_cast<std::uint8_t>(pitch.f_number & 0xFF));
        OPLL_writeReg(chip_.get(), 0x30, 0x00);
        const std::uint8_t pitch_without_key = static_cast<std::uint8_t>(
            ((pitch.f_number >> 8) & 1)
            | ((pitch.block & 7) << 1));
        OPLL_writeReg(
            chip_.get(),
            0x20,
            static_cast<std::uint8_t>(pitch_without_key | 0x10));
        for (std::size_t index = 0;
             index < render_offset_sample + sample_count;
             ++index) {
            if (index == key_off_sample) {
                OPLL_writeReg(chip_.get(), 0x20, pitch_without_key);
            }
            const float sample = static_cast<float>(OPLL_calc(chip_.get()));
            if (index >= render_offset_sample) {
                result_[index - render_offset_sample] = sample;
            }
        }
        removeDcAndNormalize(result_);
        return result_;
    }

private:
    std::unique_ptr<OPLL, OpllDeleter> chip_;
    std::vector<float> result_;
};

#include "wave_import_perceptual.inc"

OpllFullScoreBreakdown scoreTimedPcm(std::span<const float> reference,
    std::span<const float> rendered, const TimedAudioFeatures& reference_features,
    float fundamental_hz, TimedFeatureWorkspace& workspace, const FullPerceptualTarget* perceptual) {
    DirectHarmonics direct;
    OpllFullScoreBreakdown breakdown;
    const auto generated = timedFeatures(rendered, fundamental_hz, workspace, perceptual ? &direct : nullptr);
    float distance = featureDistance(reference_features, generated, &breakdown.spectrum);
    if (!perceptual) {
        const auto period_samples = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(static_cast<double>(kOpllFitSampleRate)
                / std::max(1.0, static_cast<double>(fundamental_hz)))),
            16, std::max<std::size_t>(16, reference.size() / 2));
        distance += periodWindowDistance(reference, rendered, period_samples) * 0.35F;
    }
    breakdown.total = perceptual ? perceptual->addDistances(
        rendered, direct, workspace, breakdown, distance) : distance;
    return breakdown;
}

class TimedOpllScoreContext {
public:
    explicit TimedOpllScoreContext(const OpllFitTarget& target, bool require_renderer = false,
        const FullPerceptualTarget* perceptual = nullptr)
        : target_(target),
          fundamental_hz_(target.fundamental_hz),
          perceptual_(perceptual) {
        if (require_renderer && !renderer_.valid())
            throw std::runtime_error("Could not initialize the OPLL renderer.");
    }

    OpllFullScoreBreakdown scoreWithBreakdown(const OpllPatchParameters& patch) {
        const auto& rendered = renderer_.render(
            patch,
            target_.fundamental_hz,
            target_.samples.size(),
            target_.key_off_sample,
            target_.render_offset_sample,
            target_.fixed_pitch);
        return scoreTimedPcm(target_.samples, rendered, target_.features,
            fundamental_hz_, feature_workspace_, perceptual_);
    }

private:
    const OpllFitTarget& target_;
    float fundamental_hz_;
    const FullPerceptualTarget* perceptual_;
    OpllTimedRenderer renderer_;
    TimedFeatureWorkspace feature_workspace_;
};

struct ScoredOpllPatch {
    float distance{};
    std::array<std::uint8_t, 8> registers{};
    OpllPatchParameters patch{};
    OpllFullScoreBreakdown breakdown;
};

template <typename ContextFactory>
class OpllScoreExecutor {
public:
    using Context = decltype(std::declval<ContextFactory&>()());

    OpllScoreExecutor(ContextFactory make_context, std::size_t max_workers,
        bool synchronous_fallback = false) {
        const unsigned hardware = std::thread::hardware_concurrency();
        worker_capacity_ = std::min<std::size_t>(
            std::clamp<std::size_t>(max_workers, 1, 8),
            hardware == 0 ? 4U : hardware);
        contexts_.reserve(worker_capacity_);
        for (std::size_t worker = 0; worker < worker_capacity_; ++worker) {
            contexts_.push_back(
                std::make_unique<Context>(make_context()));
        }
        if (worker_capacity_ == 1) {
            return;
        }
        if (synchronous_fallback) return;
        workers_.reserve(worker_capacity_);
        try {
            for (std::size_t worker = 0; worker < worker_capacity_; ++worker) {
                workers_.emplace_back([this, worker] {
                    workerLoop(worker);
                });
            }
        } catch (const std::system_error&) {
            // The already-created workers remain usable. Missing deterministic
            // worker chunks are scored synchronously by score().
        }
    }

    OpllScoreExecutor(const OpllScoreExecutor&) = delete;
    OpllScoreExecutor& operator=(const OpllScoreExecutor&) = delete;

    ~OpllScoreExecutor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_ready_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }

    void score(std::span<ScoredOpllPatch> batch) {
        if (batch.empty()) {
            return;
        }
        const std::size_t active_workers =
            std::min(worker_capacity_, batch.size());
        if (active_workers == 1) {
            scoreChunk(batch, 0, active_workers);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            batch_data_ = batch.data();
            batch_size_ = batch.size();
            active_workers_ = active_workers;
            completed_workers_ = 0;
            ++generation_;
        }
        work_ready_.notify_all();

        // A failed thread launch leaves a suffix of deterministic worker
        // indices. Run exactly those chunks on the submitting thread.
        for (std::size_t worker = workers_.size();
             worker < active_workers;
             ++worker) {
            scoreChunk(batch, worker, active_workers);
        }

        std::unique_lock lock(mutex_);
        work_done_.wait(lock, [&] {
            return completed_workers_ == workers_.size();
        });
    }

private:
    void scoreChunk(
        std::span<ScoredOpllPatch> batch,
        std::size_t worker,
        std::size_t worker_count) {
        const std::size_t chunk_size =
            (batch.size() + worker_count - 1) / worker_count;
        const std::size_t begin = worker * chunk_size;
        const std::size_t end = std::min(begin + chunk_size, batch.size());
        auto& context = *contexts_[worker];
        for (std::size_t index = begin; index < end; ++index) {
            auto& candidate = batch[index];
            if constexpr (requires { context.scoreWithBreakdown(candidate.patch); }) {
                candidate.breakdown = context.scoreWithBreakdown(candidate.patch);
                candidate.distance = candidate.breakdown.total;
            } else {
                candidate.distance = context.score(candidate.patch);
            }
        }
    }

    void workerLoop(std::size_t worker) {
        std::uint64_t observed_generation{};
        for (;;) {
            std::span<ScoredOpllPatch> batch;
            std::size_t active_workers{};
            {
                std::unique_lock lock(mutex_);
                work_ready_.wait(lock, [&] {
                    return stopping_ || generation_ != observed_generation;
                });
                if (stopping_) {
                    return;
                }
                observed_generation = generation_;
                batch = {batch_data_, batch_size_};
                active_workers = active_workers_;
            }
            if (worker < active_workers) {
                scoreChunk(batch, worker, active_workers);
            }
            {
                std::lock_guard lock(mutex_);
                ++completed_workers_;
            }
            work_done_.notify_one();
        }
    }

    std::size_t worker_capacity_{1};
    std::vector<std::unique_ptr<Context>> contexts_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable work_done_;
    ScoredOpllPatch* batch_data_{};
    std::size_t batch_size_{};
    std::size_t active_workers_{};
    std::size_t completed_workers_{};
    std::uint64_t generation_{};
    bool stopping_{};
};

template <typename Executor>
bool scoreOpllSubBatches(
    std::vector<ScoredOpllPatch>& candidates,
    Executor& executor,
    std::size_t batch_size,
    std::uint64_t weight,
    OpllApproximationCoordinator* coordinator) {
    for (std::size_t begin = 0; begin < candidates.size();
         begin += batch_size) {
        if (coordinator && coordinator->cancelled()) {
            return false;
        }
        std::size_t count = std::min(batch_size, candidates.size() - begin);
        if (coordinator && coordinator->continuous) {
            const auto& state = *coordinator->continuous;
            const auto& hooks = state.options.test_hooks;
            if (hooks && hooks->cancel_after_evaluations && state.phase == hooks->cancel_phase) {
                const auto completed = state.phase == OpllApproximationPhase::ShortTimbre
                    ? state.result.short_evaluations : state.phase == OpllApproximationPhase::FullEnvelope
                        ? state.result.full_evaluations : state.result.representative_evaluations;
                if (completed < hooks->cancel_after_evaluations)
                    count = std::min<std::uint64_t>(count, hooks->cancel_after_evaluations - completed);
            }
        }
        executor.score(
            std::span<ScoredOpllPatch>(candidates).subspan(begin, count));
        // A sub-batch is published only after every worker has completed.
        if (coordinator) {
            coordinator->advance(
                static_cast<std::uint64_t>(count) * weight);
        }
        if (coordinator && coordinator->continuous) {
            auto& state = *coordinator->continuous;
            switch (state.phase) {
            case OpllApproximationPhase::RepresentativeCycle:
            case OpllApproximationPhase::SteadyTimbre:
                state.result.representative_evaluations += count; break;
            case OpllApproximationPhase::ShortTimbre:
                state.result.short_evaluations += count; break;
            case OpllApproximationPhase::FullEnvelope: {
                const auto previous = state.result.ranked.empty()
                    ? std::shared_ptr<const OpllContinuousCandidate>{} : state.result.ranked.front();
                state.result.full_evaluations += count;
                for (const auto& entry : std::span<ScoredOpllPatch>(candidates).subspan(begin, count))
                    state.rank(entry.patch, entry.breakdown);
                if (!state.result.ranked.empty() && state.result.ranked.front() != previous
                    && !coordinator->control->cancelRequested())
                    state.publishFront();
                break;
            }
            default: break;
            }
            coordinator->updateTime();
            if (state.options.test_hooks && state.options.test_hooks->batch_completed)
                state.options.test_hooks->batch_completed(state.phase,
                    state.result.representative_evaluations, state.result.short_evaluations,
                    state.result.full_evaluations);
            if (state.options.test_hooks) {
                const auto& hooks = *state.options.test_hooks;
                const auto completed = state.phase == OpllApproximationPhase::ShortTimbre
                    ? state.result.short_evaluations : state.phase == OpllApproximationPhase::FullEnvelope
                        ? state.result.full_evaluations : state.result.representative_evaluations;
                if (hooks.cancel_after_evaluations && state.phase == hooks.cancel_phase
                    && completed >= hooks.cancel_after_evaluations)
                    coordinator->control->requestCancel();
            }
            if (coordinator->cancelled()) return false;
        }
    }
    return true;
}

#include "wave_import_ga.inc"

std::size_t detectKeyOff(
    const std::array<float, kEnvelopeBins>& envelope,
    std::size_t sample_count) {
    if (sample_count == 0) {
        return std::numeric_limits<std::size_t>::max();
    }
    const auto peak = static_cast<std::size_t>(
        std::distance(
            envelope.begin(),
            std::max_element(envelope.begin(), envelope.end())));
    for (std::size_t bin = std::max<std::size_t>(
             peak + 2,
             envelope.size() * 2 / 3);
         bin + 3 < envelope.size();
         ++bin) {
        if (envelope[bin] > 0.35F
            && envelope[bin + 3] < envelope[bin] * 0.45F
            && envelope.back() < 0.18F) {
            return bin * sample_count / envelope.size();
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

OpllFitTarget makeFitTarget(
    const WavePcm& pcm,
    const WaveCycleAnalysis& cycle_analysis,
    std::optional<std::uint8_t> fixed_midi_note = {}) {
    OpllFitTarget target{};
    auto resampled = resampleAudio(
        pcm.mono_samples,
        pcm.sample_rate,
        kOpllFitSampleRate);
    removeDcAndNormalize(resampled);
    float peak{};
    for (const float sample : resampled) {
        peak = std::max(peak, std::abs(sample));
    }
    std::size_t first_audible{};
    const float threshold = peak * 0.0125F;
    while (!fixed_midi_note && first_audible < resampled.size()
           && std::abs(resampled[first_audible]) < threshold) {
        ++first_audible;
    }
    constexpr std::size_t maximum_fit_samples =
        kOpllFitSampleRate * 2;
    const std::size_t end = std::min(
        resampled.size(),
        first_audible + maximum_fit_samples);
    if (first_audible < end) {
        target.samples.assign(
            resampled.begin() + static_cast<std::ptrdiff_t>(first_audible),
            resampled.begin() + static_cast<std::ptrdiff_t>(end));
    } else {
        target.samples = std::move(resampled);
    }
    removeDcAndNormalize(target.samples);
    target.fundamental_hz =
        cycle_analysis.estimated_frequency_hz > 0.0F
            ? cycle_analysis.estimated_frequency_hz
            : 261.625565F;
    if (fixed_midi_note) {
        NotePitch pitch{};
        if (!notePitch(*fixed_midi_note, pitch))
            throw std::invalid_argument("Unsupported conversion MIDI note");
        target.fixed_pitch = pitch.opll;
        target.fundamental_hz = sccFrequencyHz(pitch.psg_scc_period);
    }
    target.features = timedFeatures(
        target.samples,
        target.fundamental_hz);
    // Fixed-note input is a complete single-note audition render. Its
    // release follows the captured interval at the source audition boundary.
    target.key_off_sample = fixed_midi_note ? target.samples.size()
        : detectKeyOff(target.features.envelope, target.samples.size());
    return target;
}

OpllFitTarget makeTimbreTarget(const OpllFitTarget& full_target) {
    OpllFitTarget target{};
    target.fundamental_hz = full_target.fundamental_hz;
    target.fixed_pitch = full_target.fixed_pitch;
    target.key_off_sample = std::numeric_limits<std::size_t>::max();
    if (full_target.samples.empty()) {
        return target;
    }
    constexpr std::size_t maximum_samples = 4'096;
    const std::size_t length =
        std::min(maximum_samples, full_target.samples.size());
    // Skip only a bounded onset interval. Rendering applies the same offset,
    // keeping target and candidate envelope time aligned without making the
    // cheap phase scale with a long source WAV.
    const std::size_t best_begin = std::min<std::size_t>(
        1'024,
        full_target.samples.size() - length);
    target.render_offset_sample = best_begin;
    target.samples.assign(
        full_target.samples.begin() + static_cast<std::ptrdiff_t>(best_begin),
        full_target.samples.begin()
            + static_cast<std::ptrdiff_t>(best_begin + length));
    removeDcAndNormalize(target.samples);
    target.features = timedFeatures(target.samples, target.fundamental_hz);
    return target;
}

std::vector<OpllPatchParameters> searchTimedOpll(
    const OpllFitTarget& target,
    std::span<const float> representative_cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    const SearchBudgets& budgets) {
    // Cheap timbre phase: 92 seeds + 8 * (81 + 72 + 32 + 4*4 + 16)
    // = 1,828 probes before register deduplication.
    const std::size_t evaluation_budget = budgets.short_timbre;
    const std::size_t steady_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardSteadyBatchSize
        : kSteadyBatchSize;
    const std::size_t full_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardFullWaveBatchSize
        : kFullWaveBatchSize;
    constexpr std::size_t beam_width = 8;
    constexpr std::size_t result_capacity = 6;
    const auto timbre_target = makeTimbreTarget(target);
    std::size_t evaluations{};
    std::set<std::array<std::uint8_t, 8>> seen;
    std::vector<ScoredOpllPatch> short_scored;
    // OPLL_new performs emu2413's one-time table initialization. Complete it
    // on this thread before any worker constructs its private context.
    const bool require_renderer = coordinator && coordinator->continuous;
    TimedOpllScoreContext priming_context(timbre_target, require_renderer);
    static_cast<void>(priming_context);
    const auto make_context = [&timbre_target, require_renderer] {
        return TimedOpllScoreContext(timbre_target, require_renderer);
    };
    const auto less = [](
                          const ScoredOpllPatch& left,
                          const ScoredOpllPatch& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.registers < right.registers;
    };
    const auto admit = [&](
                           const OpllPatchParameters& patch,
                           std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (evaluations >= evaluation_budget || !seen.insert(registers).second) {
            return;
        }
        ++evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > beam_width) {
            candidates.resize(beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> seed_batch;
    const auto seed = [&](const OpllPatchParameters& patch) {
        admit(patch, seed_batch);
    };

    for (const auto& patch : searchWaveCycleWithOpll(
             representative_cycle,
             options,
             coordinator,
             budgets.representative,
             OpllApproximationPhase::RepresentativeCycle)) {
        seed(patch);
    }
    if (coordinator && coordinator->cancelled()) {
        return {};
    }
    seed(defaultOpllPatch());
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            seed(*rom);
        }
    }
    std::uint32_t random = 0x5749464DU;
    const auto next_random = [&]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    for (int index = 0; index < 64; ++index) {
        auto candidate = defaultOpllPatch();
        std::uint32_t bits = next_random();
        candidate.modulator.multiplier = bits & 15;
        candidate.carrier.multiplier = (bits >> 4) & 15;
        candidate.feedback = (bits >> 8) & 7;
        candidate.modulator.waveform = ((bits >> 11) & 1) != 0;
        candidate.carrier.waveform = ((bits >> 12) & 1) != 0;
        candidate.modulator.pitch_modulation = ((bits >> 13) & 1) != 0;
        candidate.carrier.pitch_modulation = ((bits >> 14) & 1) != 0;
        candidate.modulator.amplitude_modulation = ((bits >> 15) & 1) != 0;
        candidate.carrier.amplitude_modulation = ((bits >> 16) & 1) != 0;
        candidate.modulator.sustained_tone = ((bits >> 17) & 1) != 0;
        candidate.carrier.sustained_tone = ((bits >> 18) & 1) != 0;
        candidate.modulator.key_rate_scaling = ((bits >> 19) & 1) != 0;
        candidate.carrier.key_rate_scaling = ((bits >> 20) & 1) != 0;
        candidate.modulator.key_scale_level = (bits >> 21) & 3;
        candidate.carrier.key_scale_level = (bits >> 23) & 3;
        bits = next_random();
        candidate.modulator.total_level = bits & 63;
        candidate.modulator.attack_rate = (bits >> 6) & 15;
        candidate.modulator.decay_rate = (bits >> 10) & 15;
        candidate.modulator.sustain_level = (bits >> 14) & 15;
        candidate.modulator.release_rate = (bits >> 18) & 15;
        candidate.carrier.attack_rate = (bits >> 22) & 15;
        candidate.carrier.decay_rate = (bits >> 26) & 15;
        bits = next_random();
        candidate.carrier.sustain_level = bits & 15;
        candidate.carrier.release_rate = (bits >> 4) & 15;
        seed(candidate);
    }
    if (coordinator) {
        coordinator->setPhase(OpllApproximationPhase::ShortTimbre);
    }
    // Do not keep the short-target pool alive during the nested
    // representative-cycle search; each homogeneous phase owns one pool.
    OpllScoreExecutor short_executor(make_context, options.max_workers,
        coordinator && coordinator->synchronousFallback());
    if (!scoreOpllSubBatches(
            seed_batch,
            short_executor,
            steady_batch_size,
            1,
            coordinator)) {
        return {};
    }
    short_scored.insert(
        short_scored.end(), seed_batch.begin(), seed_batch.end());
    const auto ga_origin_seeds = coordinator && coordinator->continuous
        ? seed_batch : std::vector<ScoredOpllPatch>{};
    std::vector<ScoredOpllPatch> beam = retain(std::move(seed_batch));
    const auto run_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                short_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return false;
        }
        short_scored.insert(
            short_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = beam;
        next.insert(next.end(), batch.begin(), batch.end());
        beam = retain(std::move(next));
        return true;
    };

    constexpr std::array<std::uint8_t, 9> multipliers{
        0, 1, 2, 3, 4, 6, 8, 12, 15};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto modulator : multipliers) {
            for (const auto carrier : multipliers) {
                auto candidate = base;
                candidate.modulator.multiplier = modulator;
                candidate.carrier.multiplier = carrier;
                emit(candidate);
            }
        }
    })) return {};
    constexpr std::array<std::uint8_t, 9> levels{
        0, 8, 16, 24, 32, 40, 48, 56, 63};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto level : levels) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.total_level = level;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int waveform = 0; waveform < 4; ++waveform) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.waveform = (waveform & 1) != 0;
                candidate.carrier.waveform = (waveform & 2) != 0;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.amplitude_modulation = (value & 1) != 0;
            candidate.carrier.amplitude_modulation = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.pitch_modulation = (value & 1) != 0;
            candidate.carrier.pitch_modulation = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.sustained_tone = (value & 1) != 0;
            candidate.carrier.sustained_tone = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int value = 0; value < 4; ++value) {
            auto candidate = base;
            candidate.modulator.key_rate_scaling = (value & 1) != 0;
            candidate.carrier.key_rate_scaling = (value & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (std::uint8_t modulator = 0; modulator < 4; ++modulator) {
            for (std::uint8_t carrier = 0; carrier < 4; ++carrier) {
                auto candidate = base;
                candidate.modulator.key_scale_level = modulator;
                candidate.carrier.key_scale_level = carrier;
                emit(candidate);
            }
        }
    })) return {};

    auto staged_short_beam = beam;
    std::vector<ScoredOpllPatch> short_ga_promotions;
    // Thorough uses deterministic full-register restarts after the complete
    // Standard path. Decoding arbitrary register bytes spans the legal patch
    // space and prevents later stages from starving on duplicate mutations.
    if (options.effort == OpllApproximationEffort::Thorough) {
        std::sort(short_scored.begin(), short_scored.end(), less);
        std::vector<ScoredOpllPatch> broad_beam = short_scored;
        if (broad_beam.size() > 24) {
            broad_beam.resize(24);
        }
        std::vector<ScoredOpllPatch> pair_local_batch;
        for (const auto& parent : broad_beam) {
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = parent.patch;
                candidate.modulator.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.multiplier)
                            + delta,
                        0,
                        15));
                candidate.carrier.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(parent.patch.carrier.multiplier)
                            - delta,
                        0,
                        15));
                candidate.modulator.total_level =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.total_level)
                            + delta * 2,
                        0,
                        63));
                admit(candidate, pair_local_batch);
            }
        }
        if (!scoreOpllSubBatches(
                pair_local_batch,
                short_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return {};
        }
        short_scored.insert(
            short_scored.end(),
            pair_local_batch.begin(),
            pair_local_batch.end());
        if (coordinator && coordinator->continuous) {
            auto staged = short_scored;
            std::sort(staged.begin(), staged.end(), less);
            staged_short_beam = retain(staged);
            if (!gaResidual(false, evaluation_budget, evaluations,
                    short_scored, staged, ga_origin_seeds,
                    short_executor, steady_batch_size, 1, coordinator,
                    admit, less)) return {};
            short_ga_promotions = retain(short_scored);
            beam = staged_short_beam;
        }
        while ((!coordinator || !coordinator->continuous) && evaluations < evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(steady_batch_size);
            while (batch.size() < steady_batch_size
                   && evaluations < evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit(decodeOpllPatch(registers), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    short_executor,
                    steady_batch_size,
                    1,
                    coordinator)) {
                return {};
            }
            short_scored.insert(
                short_scored.end(), batch.begin(), batch.end());
            std::vector<ScoredOpllPatch> next = beam;
            next.insert(next.end(), batch.begin(), batch.end());
            beam = retain(std::move(next));
        }
    }

    // Full-length phase: 8 promoted timbres + 3 * (4 * 16 + 8 * 5)
    // = 320 probes before register deduplication.
    const std::size_t full_evaluation_budget = budgets.full_envelope;
    constexpr std::size_t full_beam_width = 3;
    std::size_t full_evaluations{};
    std::set<std::array<std::uint8_t, 8>> full_seen;
    std::vector<ScoredOpllPatch> full_scored;
    TimedOpllScoreContext full_priming_context(target, require_renderer);
    static_cast<void>(full_priming_context);
    std::optional<FullPerceptualTarget> perceptual;
    if (coordinator && coordinator->continuous)
        perceptual.emplace(target.samples, target.fundamental_hz, target.key_off_sample);
    const auto make_full_context = [&target, require_renderer, &perceptual] {
        return TimedOpllScoreContext(target, require_renderer, perceptual ? &*perceptual : nullptr);
    };
    OpllScoreExecutor full_executor(make_full_context, options.max_workers,
        coordinator && coordinator->synchronousFallback());
    const auto admit_full = [&](
                                const OpllPatchParameters& patch,
                                std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (full_evaluations >= full_evaluation_budget
            || !full_seen.insert(registers).second) {
            return;
        }
        ++full_evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain_full = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > full_beam_width) {
            candidates.resize(full_beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> promoted_batch;
    for (const auto& entry : beam) {
        admit_full(entry.patch, promoted_batch);
    }
    if (coordinator) {
        coordinator->setPhase(OpllApproximationPhase::FullEnvelope);
    }
    const auto full_weight = fullWaveWeight(target.samples.size());
    if (!scoreOpllSubBatches(
            promoted_batch,
            full_executor,
            full_batch_size,
            full_weight,
            coordinator)) {
        return {};
    }
    full_scored.insert(
        full_scored.end(),
        promoted_batch.begin(),
        promoted_batch.end());
    std::vector<ScoredOpllPatch> full_beam =
        retain_full(std::move(promoted_batch));
    const auto run_full_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : full_beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit_full(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                full_executor,
                full_batch_size,
                full_weight,
                coordinator)) {
            return false;
        }
        full_scored.insert(full_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = full_beam;
        next.insert(next.end(), batch.begin(), batch.end());
        full_beam = retain_full(std::move(next));
        return true;
    };

    constexpr std::array<std::uint8_t, 4> eg{0, 5, 10, 15};
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto attack : eg) {
            for (const auto decay : eg) {
                auto candidate = base;
                candidate.modulator.attack_rate = attack;
                candidate.modulator.decay_rate = decay;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto sustain : eg) {
            for (const auto release : eg) {
                auto candidate = base;
                candidate.modulator.sustain_level = sustain;
                candidate.modulator.release_rate = release;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto attack : eg) {
            for (const auto decay : eg) {
                auto candidate = base;
                candidate.carrier.attack_rate = attack;
                candidate.carrier.decay_rate = decay;
                emit(candidate);
            }
        }
    });
    run_full_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto sustain : eg) {
            for (const auto release : eg) {
                auto candidate = base;
                candidate.carrier.sustain_level = sustain;
                candidate.carrier.release_rate = release;
                emit(candidate);
            }
        }
    });

    // Quantization-aware ±2 refinement runs only after all four joint EG
    // stages; the full-phase upper bound reserves all 120 probes.
    const auto refine_local = [&](auto member) {
        run_full_stage([&](const OpllPatchParameters& base, auto emit) {
            const int center = member(base);
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = base;
                member(candidate) = static_cast<std::uint8_t>(
                    std::clamp(center + delta, 0, 15));
                emit(candidate);
            }
        });
    };
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.attack_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.decay_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.sustain_level;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.modulator.release_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.attack_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.decay_rate;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.sustain_level;
    });
    refine_local([](auto& patch) -> auto& {
        return patch.carrier.release_rate;
    });

    if (coordinator && coordinator->cancelled()) {
        return {};
    }
    if (options.effort == OpllApproximationEffort::Thorough) {
        if (coordinator && coordinator->continuous) {
            auto staged = full_scored;
            std::sort(staged.begin(), staged.end(), less);
            std::vector<ScoredOpllPatch> promoted_ga;
            for (const auto& candidate : short_ga_promotions)
                admit_full(candidate.patch, promoted_ga);
            if (!scoreOpllSubBatches(promoted_ga, full_executor, full_batch_size,
                    full_weight, coordinator)) return {};
            full_scored.insert(full_scored.end(), promoted_ga.begin(), promoted_ga.end());
            if (!gaResidual(true, full_evaluation_budget, full_evaluations,
                    full_scored, staged, ga_origin_seeds,
                    full_executor, full_batch_size, full_weight, coordinator,
                    admit_full, less)) return {};
        }
        while ((!coordinator || !coordinator->continuous) && full_evaluations < full_evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(full_batch_size);
            while (batch.size() < full_batch_size
                   && full_evaluations < full_evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit_full(decodeOpllPatch(registers), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    full_executor,
                    full_batch_size,
                    full_weight,
                    coordinator)) {
                return {};
            }
            full_scored.insert(
                full_scored.end(), batch.begin(), batch.end());
        }
    }

    if (coordinator && coordinator->continuous) {
        auto& state = *coordinator->continuous;
        state.result.initial_completed = true;
        if (!state.result.ranked.empty()) state.result.initial_best = state.result.ranked.front();
        coordinator->updateTime();
        if (!(state.options.test_hooks && state.options.test_hooks->initial_only)) {
            std::uint32_t epoch{};
            const auto compact_cache = [&](auto& scored, auto& keys) {
                if (scored.size() <= 65'536) return;
                std::sort(scored.begin(), scored.end(), less);
                scored.resize(64);
                keys.clear();
                for (const auto& entry : scored) keys.insert(entry.registers);
            };
            while (!coordinator->cancelled()) {
                state.seed = 0x47415043U ^ (++epoch * 0x9E3779B9U);
                compact_cache(short_scored, seen);
                compact_cache(full_scored, full_seen);
                auto staged_short = short_scored;
                std::sort(staged_short.begin(), staged_short.end(), less);
                if (staged_short.size() > 64) staged_short.resize(64);
                std::size_t additional_short{};
                const auto admit_short_extra = [&](const auto& patch, auto& batch) {
                    const auto registers = encodeOpllPatch(patch);
                    if (additional_short < 1'024 && seen.insert(registers).second) {
                        ++additional_short;
                        batch.push_back(ScoredOpllPatch{0, registers, patch});
                    }
                };
                coordinator->setPhase(OpllApproximationPhase::ShortTimbre);
                if (!gaResidual(false, 1'024, additional_short, short_scored,
                        staged_short, ga_origin_seeds, short_executor,
                        steady_batch_size, 1, coordinator, admit_short_extra, less)) break;
                std::size_t additional_full{};
                const auto admit_full_extra = [&](const auto& patch, auto& batch) {
                    const auto registers = encodeOpllPatch(patch);
                    if (additional_full < 1'024 && full_seen.insert(registers).second) {
                        ++additional_full;
                        batch.push_back(ScoredOpllPatch{0, registers, patch});
                    }
                };
                coordinator->setPhase(OpllApproximationPhase::FullEnvelope);
                std::vector<ScoredOpllPatch> promoted;
                for (const auto& candidate : retain(short_scored))
                    admit_full_extra(candidate.patch, promoted);
                if (!scoreOpllSubBatches(promoted, full_executor, full_batch_size,
                        full_weight, coordinator)) break;
                full_scored.insert(full_scored.end(), promoted.begin(), promoted.end());
                auto staged_full = full_scored;
                std::sort(staged_full.begin(), staged_full.end(), less);
                if (staged_full.size() > 64) staged_full.resize(64);
                if (!gaResidual(true, 1'024, additional_full, full_scored,
                        staged_full, ga_origin_seeds, full_executor,
                        full_batch_size, full_weight, coordinator, admit_full_extra, less)) break;
            }
        }
    }
    std::sort(full_scored.begin(), full_scored.end(), less);
    std::vector<OpllPatchParameters> result;
    result.reserve(result_capacity);
    for (const auto& entry : full_scored) {
        result.push_back(entry.patch);
        if (result.size() == result_capacity) {
            break;
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

}  // namespace

OpllPublicationDecision OpllContinuousSearchState::publicationDecision(
    const OpllFullScoreBreakdown& previous, const OpllFullScoreBreakdown& candidate) {
    constexpr float absolute_harmonic_margin = 0.14F;
    constexpr float absolute_stage_margin = 0.10F;
    constexpr float relative_margin = 0.35F;
    constexpr float relative_floor = 0.10F;
    const auto regressed = [=](float before, float after, float absolute_margin) {
        return after - before > std::max(absolute_margin,
            relative_margin * std::max(relative_floor, before));
    };
    if (previous.harmonic_count && candidate.harmonic_count
        && regressed(previous.harmonics, candidate.harmonics, absolute_harmonic_margin))
        return OpllPublicationDecision::HarmonicsRegressed;
    for (std::size_t stage = 0; stage < previous.stage_envelope.size(); ++stage) {
        if ((previous.stage_mask & candidate.stage_mask & (1U << stage)) != 0
            && (regressed(previous.stage_envelope[stage], candidate.stage_envelope[stage], absolute_stage_margin)
                || regressed(previous.stage_timbre[stage], candidate.stage_timbre[stage], absolute_stage_margin)))
            return OpllPublicationDecision::StageRegressed;
    }
    return OpllPublicationDecision::Published;
}

OpllFullScoreBreakdown detail::compareOpllFullPcmForRegression(
    std::span<const float> reference, std::span<const float> candidate,
    float fundamental_hz, std::size_t key_off) {
    if (reference.size() != candidate.size() || !std::isfinite(fundamental_hz)
        || fundamental_hz <= 0.0F) throw std::invalid_argument("Invalid PCM comparison");
    const FullPerceptualTarget target(reference, fundamental_hz, key_off);
    TimedFeatureWorkspace workspace;
    const auto source_features = timedFeatures(reference, fundamental_hz, workspace);
    return scoreTimedPcm(reference, candidate, source_features, fundamental_hz, workspace, &target);
}

OpllPublicationDecision detail::checkOpllPerceptualGateForRegression(
    const OpllFullScoreBreakdown& previous, const OpllFullScoreBreakdown& candidate) {
    return OpllContinuousSearchState::publicationDecision(previous, candidate);
}

bool detail::checkOpllPublicationHistoryForRegression() {
    OpllContinuousOptions options;
    options.control = std::make_shared<OpllApproximationControl>();
    unsigned notifications = 0;
    options.candidate_ready = [&](const auto&) { ++notifications; };
    OpllContinuousSearchState state(options);
    const auto submit = [&](std::uint8_t level, float score, float harmonic_error) {
        auto patch = defaultOpllPatch();
        patch.modulator.total_level = level;
        const auto before = state.result.ranked.empty() ? nullptr : state.result.ranked.front();
        state.result.full_evaluations += 32;
        state.rank(patch, {.total = score, .harmonics = harmonic_error, .harmonic_count = 20});
        if (!state.result.ranked.empty() && state.result.ranked.front() != before) state.publishFront();
    };
    submit(0, 1.0F, 0.2F);
    const auto first = state.result.published_best;
    if (!first || notifications != 1) return false;
    for (std::uint8_t i = 1; i < 8; ++i) submit(i, 1.0F - i * 0.05F, 0.6F);
    if (state.result.ranked.size() != 6 || state.result.published_best != first || notifications != 1
        || std::find(state.result.ranked.begin(), state.result.ranked.end(), first) != state.result.ranked.end())
        return false;
    state.result.initial_best = state.result.ranked.front();
    submit(9, 0.4F, 0.21F);
    const auto improved = state.result.published_best;
    if (notifications != 2 || improved != state.result.ranked.front()
        || state.result.initial_best == improved) return false;
    submit(9, 0.4F, 0.21F);
    submit(8, 0.4F, 0.21F);
    if (notifications != 3 || state.result.published_best->registers >= improved->registers) return false;
    options.control->requestCancel();
    submit(10, 0.1F, 0.0F);
    return notifications == 3 && state.result.published_best->full_score == 0.4F;
}

std::vector<float> detail::renderOpllFullPcmForRegression(
    const OpllPatchParameters& patch, float fundamental_hz, std::size_t frames, std::size_t key_off) {
    OpllTimedRenderer renderer;
    if (!renderer.valid()) throw std::runtime_error("Could not initialize the OPLL renderer.");
    return renderer.render(patch, fundamental_hz, frames, key_off, 0);
}

bool parseWavePcm(
    std::span<const std::uint8_t> bytes,
    WavePcm& output,
    std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (bytes.size() < 12
        || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return fail("Not a RIFF/WAVE file.");
    }
    std::uint16_t format{};
    std::uint16_t channels{};
    std::uint16_t bits{};
    std::uint32_t sample_rate{};
    std::uint16_t block_align{};
    std::span<const std::uint8_t> data;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::uint32_t size = u32(bytes, offset + 4);
        const std::size_t begin = offset + 8;
        if (begin + size > bytes.size()) {
            return fail("Truncated WAVE chunk.");
        }
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0
            && size >= 16) {
            format = u16(bytes, begin);
            channels = u16(bytes, begin + 2);
            sample_rate = u32(bytes, begin + 4);
            block_align = u16(bytes, begin + 12);
            bits = u16(bytes, begin + 14);
            if (format == 0xFFFE && size >= 40) {
                // WAVE_FORMAT_EXTENSIBLE stores PCM/IEEE-float as the
                // leading word of its SubFormat GUID.
                format = u16(bytes, begin + 24);
            }
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            data = bytes.subspan(begin, size);
        }
        offset = begin + size + (size & 1U);
    }
    const bool supported =
        format == 1 || (format == 3 && bits == 32);
    if (!supported || channels == 0 || channels > 32
        || sample_rate == 0 || block_align == 0 || data.empty()
        || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        return fail("Unsupported or incomplete PCM WAVE format.");
    }
    const std::size_t bytes_per_sample = bits / 8;
    if (block_align < channels * bytes_per_sample) {
        return fail("Invalid WAVE block alignment.");
    }
    const std::size_t frames = data.size() / block_align;
    if (frames < 2 || frames > sample_rate * 60ULL) {
        return fail("WAVE must contain between two samples and 60 seconds.");
    }
    output.sample_rate = sample_rate;
    output.mono_samples.assign(frames, 0.0F);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum{};
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sum += normalizedSample(
                data,
                frame * block_align + channel * bytes_per_sample,
                format,
                bits);
        }
        output.mono_samples[frame] =
            sum / static_cast<float>(channels);
    }
    return true;
}

WaveCycleAnalysis analyzeWaveCycle(const WavePcm& pcm) {
    WaveCycleAnalysis result{};
    if (pcm.mono_samples.size() < 2 || pcm.sample_rate == 0) {
        return result;
    }
    const auto samples = normalizedCycle(pcm.mono_samples);
    const std::size_t minimum =
        std::max<std::size_t>(8, pcm.sample_rate / 2000);
    const std::size_t maximum = std::min<std::size_t>(
        pcm.sample_rate / 40,
        samples.size() / 2);
    std::size_t period = samples.size();
    if (samples.size() > pcm.sample_rate / 20 && maximum > minimum) {
        const std::size_t probe_size = std::min<std::size_t>(
            samples.size(),
            std::max<std::size_t>(
                pcm.sample_rate / 2,
                maximum * 2));
        const std::size_t probe_begin =
            (samples.size() - probe_size) / 2;
        const std::span<const float> probe(
            samples.data() + probe_begin,
            probe_size);
        float best = std::numeric_limits<float>::max();
        for (std::size_t lag = minimum; lag <= maximum; ++lag) {
            const float difference = periodicDifference(probe, lag);
            if (difference < best) {
                best = difference;
                period = lag;
            }
        }
        // Non-integer periods often score 2T slightly better than T (sine).
        // Collapse octave-up locks while the half-period remains competitive.
        while (best < std::numeric_limits<float>::max() && std::isfinite(best)
            && period / 2 >= minimum) {
            const std::size_t half = period / 2;
            const float half_difference = periodicDifference(probe, half);
            if (half_difference
                <= std::max(best * 1.08F, best + 0.002F)) {
                period = half;
                best = half_difference;
            } else {
                break;
            }
        }
    }
    period = std::clamp<std::size_t>(period, 2, samples.size());
    result.cycle.assign(period, 0.0F);
    const std::size_t cycle_count =
        std::max<std::size_t>(1, samples.size() / period);
    const std::size_t used_cycles = std::min<std::size_t>(cycle_count, 16);
    const std::size_t start_cycle =
        cycle_count > used_cycles ? (cycle_count - used_cycles) / 2 : 0;
    for (std::size_t cycle = 0; cycle < used_cycles; ++cycle) {
        for (std::size_t index = 0; index < period; ++index) {
            result.cycle[index] +=
                samples[(start_cycle + cycle) * period + index];
        }
    }
    for (float& sample : result.cycle) {
        sample /= static_cast<float>(used_cycles);
    }
    result.cycle = normalizedCycle(result.cycle);
    result.estimated_frequency_hz =
        static_cast<float>(pcm.sample_rate)
        / static_cast<float>(period);
    return result;
}

SccWaveform waveCycleToScc(std::span<const float> cycle) noexcept {
    SccWaveform result{};
    const auto normalized = normalizedCycle(cycle);
    if (normalized.empty()) {
        return result;
    }
    for (std::size_t index = 0; index < result.size(); ++index) {
        const double position =
            static_cast<double>(index) * normalized.size() / result.size();
        const auto first =
            static_cast<std::size_t>(position) % normalized.size();
        const auto second = (first + 1) % normalized.size();
        const float fraction =
            static_cast<float>(position - std::floor(position));
        const float sample =
            normalized[first] * (1.0F - fraction)
            + normalized[second] * fraction;
        result[index] = static_cast<std::int8_t>(
            std::clamp(
                static_cast<int>(std::lround(sample * 127.0F)),
                -128,
                127));
    }
    return result;
}

OpllPatchParameters approximateWaveCycleWithOpll(
    std::span<const float> cycle) noexcept {
    const auto candidates = approximateWaveCycleCandidatesWithOpll(cycle);
    return candidates.empty() ? defaultOpllPatch() : candidates.front();
}

std::vector<OpllPatchParameters> approximateWaveCycleCandidatesWithOpll(
    std::span<const float> cycle) {
    return approximateWaveCycleCandidatesWithOpll(
        cycle,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters> searchWaveCycleWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options,
    OpllApproximationCoordinator* coordinator,
    std::size_t evaluation_budget,
    OpllApproximationPhase phase) {
    const std::size_t steady_batch_size =
        options.effort == OpllApproximationEffort::Standard
        ? kStandardSteadyBatchSize
        : kSteadyBatchSize;
    const auto normalized = normalizedCycle(cycle);
    if (normalized.size() < 2) {
        return {defaultOpllPatch()};
    }
    const auto midi_note = coordinator && coordinator->continuous
        ? coordinator->continuous->options.fixed_midi_note.value_or(kAuditionMidiNote)
        : kAuditionMidiNote;
    NotePitch pitch{};
    if (!notePitch(midi_note, pitch)) {
        return {defaultOpllPatch()};
    }
    const float fundamental_hz = sccFrequencyHz(pitch.psg_scc_period);
    SameMidiScoreContext priming_context(
        normalized,
        fundamental_hz,
        midi_note);
    static_cast<void>(priming_context);
    const auto make_context = [&normalized, fundamental_hz, midi_note] {
        return SameMidiScoreContext(
            normalized,
            fundamental_hz,
            midi_note);
    };
    OpllScoreExecutor score_executor(make_context, options.max_workers,
        coordinator && coordinator->synchronousFallback());
    const auto prepare = [](OpllPatchParameters patch) {
        patch.modulator.amplitude_modulation = false;
        patch.carrier.amplitude_modulation = false;
        patch.modulator.sustained_tone = true;
        patch.carrier.sustained_tone = true;
        patch.modulator.key_rate_scaling = false;
        patch.carrier.key_rate_scaling = false;
        patch.modulator.key_scale_level = 0;
        patch.carrier.key_scale_level = 0;
        patch.modulator.attack_rate = 15;
        patch.carrier.attack_rate = 15;
        patch.modulator.decay_rate = 0;
        patch.carrier.decay_rate = 0;
        patch.modulator.sustain_level = 0;
        patch.carrier.sustain_level = 0;
        patch.modulator.release_rate = 8;
        patch.carrier.release_rate = 8;
        return patch;
    };
    // Upper bound before register deduplication:
    // 402 seeds + 12 * (256 + 72 + 32 + 4 + 19) = 4,998.
    constexpr std::size_t beam_width = 12;
    std::size_t evaluations{};
    std::set<std::array<std::uint8_t, 8>> seen;
    std::vector<ScoredOpllPatch> all_scored;
    const auto less = [](
                          const ScoredOpllPatch& left,
                          const ScoredOpllPatch& right) {
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        return left.registers < right.registers;
    };
    const auto admit = [&](
                           const OpllPatchParameters& patch,
                           std::vector<ScoredOpllPatch>& batch) {
        const auto registers = encodeOpllPatch(patch);
        if (evaluations >= evaluation_budget || !seen.insert(registers).second) {
            return;
        }
        ++evaluations;
        batch.push_back(ScoredOpllPatch{0.0F, registers, patch});
    };
    const auto retain = [&](std::vector<ScoredOpllPatch> candidates) {
        std::sort(candidates.begin(), candidates.end(), less);
        if (candidates.size() > beam_width) {
            candidates.resize(beam_width);
        }
        return candidates;
    };
    std::vector<ScoredOpllPatch> seed_batch;
    const auto seed = [&](const OpllPatchParameters& patch) {
        admit(patch, seed_batch);
    };

    seed(prepare(defaultOpllPatch()));
    for (std::uint8_t instrument = 1; instrument <= 15; ++instrument) {
        if (const auto rom = ym2413RomPatch(instrument)) {
            seed(prepare(*rom));
        }
    }

    // Insurance: always evaluate MULTI 1 and 2 with quiet modulator.
    // These seeds come before the random seeds: the Compact budget (384)
    // is smaller than the full seed count (402), so seeds admitted last
    // would be dropped before evaluation.
    for (const std::uint8_t car_mul :
         std::array<std::uint8_t, 2>{1, 2}) {
        auto candidate = prepare(defaultOpllPatch());
        candidate.modulator.multiplier = 0;
        candidate.carrier.multiplier = car_mul;
        candidate.modulator.total_level = 63;
        candidate.feedback = 0;
        seed(candidate);
    }

    std::uint32_t random = 0x2413C0DEU;
    auto next_random = [&]() {
        random = random * 1664525U + 1013904223U;
        return random;
    };
    for (int index = 0; index < 384; ++index) {
        OpllPatchParameters candidate =
            prepare(defaultOpllPatch());
        std::uint32_t bits = next_random();
        candidate.modulator.multiplier =
            static_cast<std::uint8_t>(bits & 15);
        // Prefer MULTI that can match SCC pitch on the shared keyboard;
        // still allow the full legal set for timbre, scored by same-MIDI audio.
        candidate.carrier.multiplier =
            static_cast<std::uint8_t>(bits >> 4) & 15;
        candidate.feedback =
            static_cast<std::uint8_t>((bits >> 8) & 7);
        candidate.modulator.waveform = ((bits >> 11) & 1) != 0;
        candidate.carrier.waveform = ((bits >> 12) & 1) != 0;
        bits = next_random();
        candidate.modulator.total_level =
            static_cast<std::uint8_t>(bits & 63);
        candidate.modulator.pitch_modulation =
            ((bits >> 6) & 3) == 1;
        candidate.carrier.pitch_modulation =
            ((bits >> 8) & 3) == 1;
        seed(candidate);
    }

    if (coordinator) {
        coordinator->setPhase(phase);
    }
    if (!scoreOpllSubBatches(
            seed_batch,
            score_executor,
            steady_batch_size,
            1,
            coordinator)) {
        return {};
    }
    all_scored.insert(
        all_scored.end(),
        seed_batch.begin(),
        seed_batch.end());
    std::vector<ScoredOpllPatch> beam = retain(std::move(seed_batch));
    const auto run_stage = [&](auto generate) {
        std::vector<ScoredOpllPatch> batch;
        for (const auto& parent : beam) {
            generate(parent.patch, [&](const OpllPatchParameters& candidate) {
                admit(candidate, batch);
            });
        }
        if (!scoreOpllSubBatches(
                batch,
                score_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return false;
        }
        all_scored.insert(all_scored.end(), batch.begin(), batch.end());
        std::vector<ScoredOpllPatch> next = beam;
        next.insert(next.end(), batch.begin(), batch.end());
        beam = retain(std::move(next));
        return true;
    };

    // Joint operator-ratio search preserves alternatives that greedy
    // one-register-at-a-time refinement cannot reach.
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (std::uint8_t mod = 0; mod < 16; ++mod) {
            for (std::uint8_t car = 0; car < 16; ++car) {
                auto candidate = base;
                candidate.modulator.multiplier = mod;
                candidate.carrier.multiplier = car;
                emit(candidate);
            }
        }
    })) return {};
    constexpr std::array<std::uint8_t, 9> levels{
        0, 8, 16, 24, 32, 40, 48, 56, 63};
    if (!run_stage([&](const OpllPatchParameters& base, auto emit) {
        for (const auto level : levels) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.total_level = level;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int waveforms = 0; waveforms < 4; ++waveforms) {
            for (std::uint8_t feedback = 0; feedback < 8; ++feedback) {
                auto candidate = base;
                candidate.modulator.waveform = (waveforms & 1) != 0;
                candidate.carrier.waveform = (waveforms & 2) != 0;
                candidate.feedback = feedback;
                emit(candidate);
            }
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        for (int modulation = 0; modulation < 4; ++modulation) {
            auto candidate = base;
            candidate.modulator.pitch_modulation = (modulation & 1) != 0;
            candidate.carrier.pitch_modulation = (modulation & 2) != 0;
            emit(candidate);
        }
    })) return {};
    if (!run_stage([](const OpllPatchParameters& base, auto emit) {
        const int center = base.modulator.total_level;
        for (int delta = -4; delta <= 4; ++delta) {
            auto candidate = base;
            candidate.modulator.total_level = static_cast<std::uint8_t>(
                std::clamp(center + delta, 0, 63));
            emit(candidate);
        }
        for (int delta = -2; delta <= 2; ++delta) {
            auto mod = base;
            mod.modulator.multiplier = static_cast<std::uint8_t>(
                std::clamp(
                    static_cast<int>(base.modulator.multiplier) + delta,
                    0,
                    15));
            emit(mod);
            auto car = base;
            car.carrier.multiplier = static_cast<std::uint8_t>(
                std::clamp(
                    static_cast<int>(base.carrier.multiplier) + delta,
                    0,
                    15));
            emit(car);
        }
    })) return {};

    if (options.effort == OpllApproximationEffort::Thorough) {
        // Re-open the beam after the complete Standard path, then perform a
        // joint ratio/level neighborhood before fixed-seed restarts.
        std::sort(all_scored.begin(), all_scored.end(), less);
        std::vector<ScoredOpllPatch> broad_beam = all_scored;
        if (broad_beam.size() > 48) {
            broad_beam.resize(48);
        }
        std::vector<ScoredOpllPatch> pair_local_batch;
        for (const auto& parent : broad_beam) {
            for (int delta = -2; delta <= 2; ++delta) {
                auto candidate = parent.patch;
                candidate.modulator.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.multiplier)
                            + delta,
                        0,
                        15));
                candidate.carrier.multiplier =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(parent.patch.carrier.multiplier)
                            - delta,
                        0,
                        15));
                candidate.modulator.total_level =
                    static_cast<std::uint8_t>(std::clamp(
                        static_cast<int>(
                            parent.patch.modulator.total_level)
                            + delta * 2,
                        0,
                        63));
                admit(candidate, pair_local_batch);
            }
        }
        if (!scoreOpllSubBatches(
                pair_local_batch,
                score_executor,
                steady_batch_size,
                1,
                coordinator)) {
            return {};
        }
        all_scored.insert(
            all_scored.end(),
            pair_local_batch.begin(),
            pair_local_batch.end());
        while (evaluations < evaluation_budget) {
            std::vector<ScoredOpllPatch> batch;
            batch.reserve(steady_batch_size);
            while (batch.size() < steady_batch_size
                   && evaluations < evaluation_budget) {
                std::array<std::uint8_t, 8> registers{};
                for (auto& value : registers) {
                    value = static_cast<std::uint8_t>(next_random() >> 24);
                }
                admit(prepare(decodeOpllPatch(registers)), batch);
            }
            if (!scoreOpllSubBatches(
                    batch,
                    score_executor,
                    steady_batch_size,
                    1,
                    coordinator)) {
                return {};
            }
            all_scored.insert(
                all_scored.end(), batch.begin(), batch.end());
            std::vector<ScoredOpllPatch> next = beam;
            next.insert(next.end(), batch.begin(), batch.end());
            beam = retain(std::move(next));
        }
    }

    std::sort(all_scored.begin(), all_scored.end(), less);
    std::vector<OpllPatchParameters> result;
    result.reserve(beam_width);
    for (const auto& entry : all_scored) {
        result.push_back(entry.patch);
        if (result.size() == beam_width) {
            break;
        }
    }
    if (result.empty()) {
        result.push_back(defaultOpllPatch());
    }
    return result;
}

OpllApproximationControl::OpllApproximationControl()
    : state_(std::make_shared<State>()) {}

void OpllApproximationControl::requestCancel() noexcept {
    state_->cancel_requested.store(true, std::memory_order_release);
}

bool OpllApproximationControl::cancelRequested() const noexcept {
    return state_->cancel_requested.load(std::memory_order_acquire);
}

OpllApproximationProgress OpllApproximationControl::progress() const noexcept {
    OpllApproximationProgress result;
    result.phase = state_->phase.load(std::memory_order_acquire);
    result.completed = state_->completed.load(std::memory_order_acquire);
    result.total = state_->total.load(std::memory_order_acquire);
    result.cancel_requested =
        state_->cancel_requested.load(std::memory_order_acquire);
    result.elapsed_milliseconds = state_->elapsed_milliseconds.load(std::memory_order_acquire);
    result.time_limit_milliseconds = state_->time_limit_milliseconds.load(std::memory_order_acquire);
    result.initial_completed = state_->initial_completed.load(std::memory_order_acquire);
    if (state_->continuous_running.load(std::memory_order_acquire)) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto start = state_->continuous_started_ns.load(std::memory_order_relaxed);
        const auto elapsed = std::max<std::int64_t>(0, (now - start) / 1'000'000);
        result.elapsed_milliseconds = std::min(result.time_limit_milliseconds,
            static_cast<std::uint64_t>(elapsed));
    }
    return result;
}

OpllApproximationResult approximateWaveCycleWithOpllResult(
    std::span<const float> cycle,
    const OpllApproximationOptions& options) {
    const std::size_t budget =
        options.profile == OpllApproximationProfile::Compact
        ? 384U
        : (options.effort == OpllApproximationEffort::Thorough
               ? 160'000U
               : 5'000U);
    OpllApproximationCoordinator coordinator(options, budget);
    if (coordinator.cancelled()) {
        coordinator.finish(true);
        return {OpllApproximationCompletion::Cancelled, {}};
    }
    auto candidates = searchWaveCycleWithOpll(
        cycle,
        options,
        &coordinator,
        budget,
        OpllApproximationPhase::SteadyTimbre);
    const bool cancelled = coordinator.cancelled();
    coordinator.finish(cancelled);
    return {
        cancelled
            ? OpllApproximationCompletion::Cancelled
            : OpllApproximationCompletion::Completed,
        cancelled ? std::vector<OpllPatchParameters>{}
                  : std::move(candidates)};
}

std::vector<OpllPatchParameters> approximateWaveCycleCandidatesWithOpll(
    std::span<const float> cycle,
    const OpllApproximationOptions& options) {
    return approximateWaveCycleWithOpllResult(cycle, options).candidates;
}

std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(const SccWaveform& waveform) {
    return approximateSccWaveformCandidatesWithOpll(
        waveform,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters>
approximateSccWaveformCandidatesWithOpll(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options) {
    return approximateSccWaveformWithOpllResult(waveform, options).candidates;
}

OpllApproximationResult approximateSccWaveformWithOpllResult(
    const SccWaveform& waveform,
    const OpllApproximationOptions& options) {
    std::array<float, 32> cycle{};
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        cycle[index] = static_cast<float>(waveform[index]) / 128.0F;
    }
    return approximateWaveCycleWithOpllResult(cycle, options);
}

std::vector<OpllPatchParameters> approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm) {
    return approximateWavePcmCandidatesWithOpll(
        pcm,
        OpllApproximationOptions{});
}

std::vector<OpllPatchParameters> approximateWavePcmCandidatesWithOpll(
    const WavePcm& pcm,
    const OpllApproximationOptions& options) {
    return approximateWavePcmWithOpllResult(pcm, options).candidates;
}

OpllApproximationResult approximateWavePcmWithOpllResult(
    const WavePcm& pcm,
    const OpllApproximationOptions& options) {
    const auto cycle_analysis = analyzeWaveCycle(pcm);
    if (cycle_analysis.cycle.size() < 2) {
        OpllApproximationCoordinator coordinator(options, 0);
        const bool cancelled = coordinator.cancelled();
        coordinator.finish(cancelled);
        return {
            cancelled
                ? OpllApproximationCompletion::Cancelled
                : OpllApproximationCompletion::Completed,
            cancelled ? std::vector<OpllPatchParameters>{}
                      : std::vector<OpllPatchParameters>{defaultOpllPatch()}};
    }
    const auto target = makeFitTarget(pcm, cycle_analysis);
    if (target.samples.size() < 2) {
        return approximateWaveCycleWithOpllResult(
            cycle_analysis.cycle, options);
    }
    const auto budgets = searchBudgets(options);
    const std::uint64_t total =
        budgets.representative + budgets.short_timbre
        + static_cast<std::uint64_t>(budgets.full_envelope)
            * fullWaveWeight(target.samples.size());
    OpllApproximationCoordinator coordinator(options, total);
    if (coordinator.cancelled()) {
        coordinator.finish(true);
        return {OpllApproximationCompletion::Cancelled, {}};
    }
    auto candidates = searchTimedOpll(
        target, cycle_analysis.cycle, options, &coordinator, budgets);
    const bool cancelled = coordinator.cancelled();
    coordinator.finish(cancelled);
    return {
        cancelled
            ? OpllApproximationCompletion::Cancelled
            : OpllApproximationCompletion::Completed,
        cancelled ? std::vector<OpllPatchParameters>{}
                  : std::move(candidates)};
}

OpllPatchParameters approximateWavePcmWithOpll(
    const WavePcm& pcm) {
    auto candidates = approximateWavePcmCandidatesWithOpll(pcm);
    return candidates.empty() ? defaultOpllPatch() : candidates.front();
}

OpllContinuousResult approximateWavePcmContinuouslyWithOpll(
    const WavePcm& pcm, const OpllContinuousOptions& requested_options) {
    auto options = requested_options;
    if (!options.control) options.control = std::make_shared<OpllApproximationControl>();
    OpllContinuousSearchState state(options);
    OpllApproximationOptions initial_options;
    initial_options.max_workers = options.max_workers;
    initial_options.effort = OpllApproximationEffort::Thorough;
    initial_options.profile = OpllApproximationProfile::Production;
    initial_options.control = options.control;
    OpllApproximationCoordinator coordinator(initial_options, 0, &state);
    try {
        if (!coordinator.cancelled()) {
            if (pcm.sample_rate == 0 || pcm.mono_samples.size() < 2
                || !std::all_of(pcm.mono_samples.begin(), pcm.mono_samples.end(),
                    [](float value) { return std::isfinite(value); }))
                throw std::invalid_argument("No valid PCM samples for conversion");
            const auto analysis = analyzeWaveCycle(pcm);
            if (!coordinator.cancelled()) {
                const auto target = makeFitTarget(pcm, analysis, options.fixed_midi_note);
                if (analysis.cycle.size() < 2 || target.samples.size() < 2)
                    throw std::invalid_argument("PCM is too short for full evaluation");
                state.fundamental_hz = target.fundamental_hz;
                state.key_off_sample = target.key_off_sample;
                state.sample_count = target.samples.size();
                const auto budgets = searchBudgets(initial_options);
                coordinator.setTotal(budgets.representative + budgets.short_timbre
                    + static_cast<std::uint64_t>(budgets.full_envelope)
                        * fullWaveWeight(target.samples.size()));
                if (!coordinator.cancelled())
                    static_cast<void>(searchTimedOpll(target, analysis.cycle,
                        initial_options, &coordinator, budgets));
            }
        }
        if (options.control->cancelRequested()) {
            state.result.completion = OpllContinuousCompletion::Cancelled;
        } else if (state.stopped()) {
            state.result.completion = OpllContinuousCompletion::TimedOut;
            if (state.result.ranked.empty())
                state.result.error = "Time limit reached before any full candidate evaluation completed";
        } else if (state.result.initial_completed && options.test_hooks
            && options.test_hooks->initial_only) {
            state.result.completion = OpllContinuousCompletion::InitialCompleted;
        } else {
            state.result.error = "Conversion stopped without a complete result";
        }
    } catch (const std::exception& error) {
        state.result.error = error.what();
        state.result.completion = options.control->cancelRequested()
            ? OpllContinuousCompletion::Cancelled : OpllContinuousCompletion::Failed;
    } catch (...) {
        state.result.error = "Unknown conversion failure";
    }
    if (state.result.completion == OpllContinuousCompletion::Cancelled) {
        state.result.ranked.clear();
        state.result.initial_best.reset();
        state.result.published_best.reset();
    }
    coordinator.finishContinuous(state.result.completion == OpllContinuousCompletion::Cancelled);
    return std::move(state.result);
}

}  // namespace mgstc::engine
