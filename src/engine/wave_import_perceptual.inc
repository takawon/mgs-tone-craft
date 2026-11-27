// SPDX-License-Identifier: AGPL-3.0-only

constexpr float kPerceptualNoiseFloor = 1.0e-5F;
constexpr float kHarmonicWeight = 1.5F;
constexpr float kStageWeight = 0.9F;
constexpr float kPeriodicWeight = 0.5F;

float windowRms(std::span<const float> samples) {
    if (samples.empty()) return 0.0F;
    double power = 0.0;
    for (const auto sample : samples) power += static_cast<double>(sample) * sample;
    return static_cast<float>(std::sqrt(power / static_cast<double>(samples.size())));
}

std::array<float, kDirectHarmonics> harmonicShares(const DirectHarmonics& harmonics) {
    std::array<float, kDirectHarmonics> result{};
    const auto first = result.begin();
    const auto last = first + static_cast<std::ptrdiff_t>(harmonics.count);
    std::transform(harmonics.amplitude.begin(),
        harmonics.amplitude.begin() + static_cast<std::ptrdiff_t>(harmonics.count),
        first, [](float power) { return std::sqrt(power); });
    const float total = std::accumulate(first, last, 0.0F);
    if (total > kPerceptualNoiseFloor) {
        for (auto& amplitude : result) amplitude /= total;
    }
    return result;
}

struct SampleRange {
    std::size_t first{};
    std::size_t end{};
    [[nodiscard]] std::size_t size() const { return end - first; }
};

struct StageFeatures {
    std::array<float, 4> levels{};
    SpectralDistribution spectrum{};
    float attack_position{};
};

StageFeatures measureStage(std::span<const float> samples,
    float fundamental_hz, TimedFeatureWorkspace& workspace) {
    StageFeatures features{};
    for (std::size_t i = 0; i < features.levels.size(); ++i) {
        const auto first = i * samples.size() / features.levels.size();
        const auto end = (i + 1) * samples.size() / features.levels.size();
        features.levels[i] = windowRms(samples.subspan(first, end - first));
    }
    const float half_level = *std::max_element(features.levels.begin(), features.levels.end()) * 0.5F;
    if (half_level > kPerceptualNoiseFloor) {
        constexpr std::size_t bins = 12;
        for (std::size_t i = 0; i < bins; ++i) {
            const auto first = i * samples.size() / bins;
            const auto end = (i + 1) * samples.size() / bins;
            if (windowRms(samples.subspan(first, end - first)) >= half_level) {
                features.attack_position = static_cast<float>(i) / static_cast<float>(bins);
                break;
            }
        }
    }
    if (samples.size() >= 256) {
        const auto size = samples.size() >= 1024 ? 1024U : 256U;
        features.spectrum = harmonicDistribution(samples, samples.size() / 2,
            size, fundamental_hz, workspace);
    }
    return features;
}

std::array<SampleRange, 4> findStages(
    std::span<const float> pcm, float fundamental_hz, std::size_t key_off) {
    constexpr std::size_t minimum_bin = kOpllFitSampleRate / 200; // 5 ms.
    const double period = static_cast<double>(kOpllFitSampleRate)
        / std::clamp(fundamental_hz, 20.0F, 6000.0F);
    const auto bin_size = static_cast<std::size_t>(std::llround(
        std::max(1.0, std::round(static_cast<double>(minimum_bin) / period)) * period));
    const auto held = std::min(key_off, pcm.size());
    std::array<SampleRange, 4> ranges{};
    // A measurable release needs one audible held window, independently of
    // whether the held interval supports attack/decay/sustain detection.
    if (held >= bin_size && pcm.size() - held >= 2 * bin_size) {
        const auto onset = windowRms(pcm.subspan(held, bin_size));
        if (windowRms(pcm.subspan(held - bin_size, bin_size)) > kPerceptualNoiseFloor
            && onset > kPerceptualNoiseFloor
            && windowRms(pcm.last(bin_size)) < onset * 0.82F)
            ranges[3] = {held, pcm.size()};
    }
    if (held < 2 * bin_size) return ranges;
    const auto window_count = held / bin_size;
    std::vector<float> levels(window_count);
    for (std::size_t bin = 0; bin < window_count; ++bin) {
        const auto start = bin * bin_size;
        levels[bin] = windowRms(pcm.subspan(start, bin_size));
    }
    const auto early_end = levels.begin() + static_cast<std::ptrdiff_t>(
        std::min(levels.size(), std::max<std::size_t>(2, 5760 / bin_size)));
    const auto peak = *std::max_element(levels.begin(), early_end);
    if (peak <= kPerceptualNoiseFloor) return ranges;
    const auto peak_at = std::find_if(levels.begin(), early_end,
        [&](float value) { return value >= peak * 0.95F; });
    const auto peak_index = static_cast<std::size_t>(peak_at - levels.begin());
    const auto peak_end = std::min((peak_index + 1) * bin_size, held);
    if (peak_index != 0 && levels.front() < peak * 0.82F) ranges[0] = {0, peak_end};

    // Identify the terminal plateau against one fixed level. A short plateau
    // at the early peak must not absorb a later real decay into sustain.
    auto audible_end = levels.size();
    while (audible_end > peak_index + 1 && levels[audible_end - 1] < peak * 0.08F)
        --audible_end;
    const float terminal_level = levels[audible_end - 1];
    auto plateau_first = audible_end - 1;
    const auto earliest_sustain = ranges[0].size() == 0 ? peak_index : peak_index + 1;
    const float plateau_band = std::min(terminal_level * 0.12F, peak * 0.06F);
    while (plateau_first > earliest_sustain
            && std::abs(levels[plateau_first - 1] - terminal_level) <= plateau_band)
        --plateau_first;
    const bool sustained = audible_end - plateau_first >= 4;
    if (sustained) ranges[2] = {plateau_first * bin_size, audible_end * bin_size};

    const auto decay_end = sustained ? plateau_first : audible_end;
    auto decay_first = peak_index + 1;
    while (decay_first < decay_end && levels[decay_first] >= peak * 0.92F)
        ++decay_first;
    if (decay_first + 1 < decay_end && terminal_level < peak * 0.82F)
        ranges[1] = {decay_first * bin_size, decay_end * bin_size};

    return ranges;
}

using PerceptualCycle = std::array<float, 32>;

PerceptualCycle fractionalCycle(std::span<const float> pcm, double first, double period) {
    PerceptualCycle points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const double at = first + period * static_cast<double>(i) / points.size();
        const auto index = static_cast<std::size_t>(at);
        const auto fraction = static_cast<float>(at - static_cast<double>(index));
        points[i] = pcm[index] + (pcm[index + 1] - pcm[index]) * fraction;
    }
    const float mean = std::accumulate(points.begin(), points.end(), 0.0F) / points.size();
    for (auto& point : points) point -= mean;
    return points;
}

float cycleCorrelation(const PerceptualCycle& a, const PerceptualCycle& b,
    float a_rms, float b_rms, std::size_t shift) {
    float product{};
    for (std::size_t i = 0; i < a.size(); ++i) product += a[i] * b[(i + shift) % b.size()];
    return std::clamp(product / (a_rms * b_rms * a.size()), -1.0F, 1.0F);
}

struct PeriodicWindow {
    double first{};
    PerceptualCycle points{};
    float rms{};
};

std::optional<PeriodicWindow> periodicWindow(
    std::span<const float> pcm, double first, double period,
    OpllPeriodicWindowStatus& status, bool reference) {
    using Status = OpllPeriodicWindowStatus;
    if (first < 0 || first + 2 * period + 1 >= static_cast<double>(pcm.size())) {
        status = reference ? Status::ReferenceTooShort : Status::CandidateTooShort;
        return {};
    }
    auto a = fractionalCycle(pcm, first, period);
    const auto b = fractionalCycle(pcm, first + period, period);
    const float a_rms = windowRms(a), b_rms = windowRms(b);
    if (std::min(a_rms, b_rms) < kPerceptualNoiseFloor) {
        status = reference ? Status::ReferenceQuiet : Status::CandidateQuiet;
        return {};
    }
    if (std::min(a_rms, b_rms) < 0.7F * std::max(a_rms, b_rms)
        || cycleCorrelation(a, b, a_rms, b_rms, 0) < 0.75F) {
        status = reference ? Status::ReferenceUnstable : Status::CandidateUnstable;
        return {};
    }
    status = Status::Compared;
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = (a[i] + b[i]) * 0.5F;
    return PeriodicWindow{first, a, windowRms(a)};
}

class FullPerceptualTarget {
public:
    FullPerceptualTarget(std::span<const float> pcm, float fundamental_hz, std::size_t key_off)
        : fundamental_hz_(fundamental_hz) {
        if (!std::isfinite(fundamental_hz) || fundamental_hz <= 0.0F) return;
        if (windowRms(pcm) <= kPerceptualNoiseFloor) {
            period_status_.fill(OpllPeriodicWindowStatus::ReferenceQuiet);
            return;
        }
        TimedFeatureWorkspace workspace;
        harmonics_.estimate_confidence = true;
        static_cast<void>(timedFeatures(pcm, fundamental_hz, workspace, &harmonics_));
        harmonic_shares_ = harmonicShares(harmonics_);
        stages_ = findStages(pcm, fundamental_hz, key_off);
        for (std::size_t i = 0; i < stages_.size(); ++i) {
            if (stages_[i].size() != 0) stage_features_[i] = measureStage(
                pcm.subspan(stages_[i].first, stages_[i].size()), fundamental_hz_, workspace);
        }
        if (std::isfinite(fundamental_hz) && fundamental_hz >= 20.0F) {
            period_ = static_cast<double>(kOpllFitSampleRate) / fundamental_hz;
            const auto held = std::min(key_off, pcm.size());
            const auto start = stages_[0].end;
            if (held > start && period_ >= 8.0 && period_ * 2 + 2 < static_cast<double>(held - start)) {
                const double travel = static_cast<double>(held - start) - 2 * period_ - 2;
                for (std::size_t i = 0; i < windows_.size(); ++i) {
                    const double first = static_cast<double>(start)
                        + travel * static_cast<double>(i + 1) / static_cast<double>(windows_.size() + 1);
                    windows_[i] = periodicWindow(pcm.first(held), first, period_, period_status_[i], true);
                }
            }
        }
    }

    float addDistances(std::span<const float> pcm, const DirectHarmonics& generated,
        TimedFeatureWorkspace& workspace, OpllFullScoreBreakdown& result, float legacy) const {
        float combined = legacy;
        float weight = 1.0F;
        const float resolved_power = std::accumulate(
            harmonics_.amplitude.begin(), harmonics_.amplitude.end(), 0.0F);
        // Compare to the fraction expected for a flat spectrum. On short FFTs
        // the harmonic bands can cover more than 15 percent of the bins.
        const float flat_spectrum_share = static_cast<float>(harmonics_.harmonic_bins)
            / static_cast<float>(std::max<std::size_t>(1, harmonics_.spectrum_bins));
        const float required_share = std::max(0.15F, 2.0F * flat_spectrum_share);
        if (harmonics_.count && resolved_power > 0.0F
            && resolved_power > harmonics_.spectrum_power * required_share) {
            const auto shares = harmonicShares(generated);
            for (std::size_t i = 0; i < harmonics_.count; ++i)
                result.harmonics += std::abs(shares[i] - harmonic_shares_[i]);
            result.harmonic_count = static_cast<std::uint8_t>(harmonics_.count);
            result.harmonic_fft_size = static_cast<std::uint16_t>(harmonics_.fft_size);
            result.harmonic_radius_bins = harmonics_.radius;
            combined += result.harmonics * kHarmonicWeight;
            weight += kHarmonicWeight;
        }
        float stages_error = 0.0F;
        unsigned stage_count = 0;
        for (std::size_t i = 0; i < stages_.size(); ++i) {
            const auto range = stages_[i];
            if (range.size() == 0) continue;
            const auto candidate = measureStage(pcm.subspan(range.first, range.size()), fundamental_hz_, workspace);
            const auto& reference = stage_features_[i];
            for (std::size_t bin = 0; bin < candidate.levels.size(); ++bin)
                result.stage_envelope[i] += std::abs(reference.levels[bin] - candidate.levels[bin])
                    / candidate.levels.size();
            if (i == 0) result.stage_envelope[i] +=
                0.25F * std::abs(reference.attack_position - candidate.attack_position);
            result.stage_timbre[i] = wassersteinDistance(reference.spectrum, candidate.spectrum) / 20.0F;
            result.stage_mask |= static_cast<std::uint8_t>(1U << i);
            stages_error += result.stage_envelope[i] + result.stage_timbre[i];
            ++stage_count;
        }
        if (stage_count) {
            combined += kStageWeight * stages_error / static_cast<float>(stage_count);
            weight += kStageWeight;
        }
        result.period_status = period_status_;
        for (std::size_t i = 0; i < windows_.size(); ++i) {
            const auto& reference = windows_[i];
            if (!reference) continue;
            const auto candidate = periodicWindow(pcm, reference->first, period_, result.period_status[i], false);
            if (!candidate) continue;
            float best = 0.0F;
            for (std::size_t shift = 0; shift < reference->points.size(); ++shift)
                best = std::max(best, std::abs(cycleCorrelation(reference->points,
                    candidate->points, reference->rms, candidate->rms, shift)));
            result.periodic_shape += 1.0F - best;
            ++result.periodic_windows;
        }
        if (result.periodic_windows) {
            result.periodic_shape /= result.periodic_windows;
            combined += kPeriodicWeight * result.periodic_shape;
            weight += kPeriodicWeight;
        }
        return combined / weight;
    }

private:
    float fundamental_hz_{};
    double period_{};
    DirectHarmonics harmonics_;
    std::array<float, kDirectHarmonics> harmonic_shares_{};
    std::array<SampleRange, 4> stages_{};
    std::array<StageFeatures, 4> stage_features_{};
    std::array<std::optional<PeriodicWindow>, 3> windows_;
    std::array<OpllPeriodicWindowStatus, 3> period_status_{};
};
