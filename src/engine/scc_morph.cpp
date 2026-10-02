// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/scc_morph.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace mgstc::engine {
namespace {

// All numerical/quality policy lives here. Samples are signed 8-bit SCC RAM
// normalized by 128; epsilon is below one quantization step, not an added tone.
struct Config {
    static constexpr double epsilon = 1e-7;
    static constexpr double silence = 1e-6;
    static constexpr double tolerance = 1e-12;
    static constexpr double value_weight = 1.0;
    static constexpr double slope_weight = 0.25;
    static constexpr double spectrum_weight = 0.4;
    static constexpr double centroid_weight = 3.0;
    static constexpr double spread_weight = 2.0;
    static constexpr double spectral_slope_weight = 0.6;
    static constexpr double odd_even_weight = 1.0;
    static constexpr double irregularity_weight = 0.25;
    static constexpr double flatness_weight = 0.15;
    static constexpr double rms_weight = 3.0;
    static constexpr double dc_weight = 3.0;
    static constexpr double shape_weight = 0.08;
    static constexpr double trajectory_weight = 2.0;
    // Bounded dimensionless distance: sample/128 MSE / 4, RMS squared,
    // Parseval-weighted harmonic amplitude squared, normalized power-shape
    // L2 / 2, and descriptor squared divided by its total weight (13).
    // Spectrum/level carry 80% of the connection metric; phase-invariant
    // sample shape carries 20%. The quartic rate term discourages outliers
    // without imposing a hard limit or equalizing every neighboring distance.
    static constexpr double connection_wave_weight = 0.2;
    static constexpr double connection_rms_weight = 0.3;
    static constexpr double connection_harmonic_weight = 0.3;
    static constexpr double connection_shape_weight = 0.1;
    static constexpr double connection_descriptor_weight = 0.1;
    // Finite coherent blends preserve the spectral objective while allowing
    // smooth connections; this balance was compared against three-state DP.
    static constexpr double motion_weight = 0.3;
    // Discourage meaningful mixture changes at a fixed interpolation position.
    // Nearly identical original spectra make this term vanish automatically.
    static constexpr double mixture_weight = 0.01;
    static constexpr double descriptor_weight_sum = centroid_weight + spread_weight
        + spectral_slope_weight + odd_even_weight + irregularity_weight
        + flatness_weight + rms_weight + dc_weight;
    static constexpr double harmonic_search_step = 0.18;
    static constexpr int quantization_passes = 2;
};

using FloatWave = std::array<double, 32>;
using Spectrum = std::array<std::complex<double>, 17>;
using Harmonics = std::array<double, 17>;

const auto& dftBasis() {
    static const auto table = [] {
        std::array<std::array<std::complex<double>, 32>, 17> result{};
        for (std::size_t k = 0; k < result.size(); ++k)
            for (std::size_t n = 0; n < 32; ++n)
                result[k][n] = std::polar(1.0, -2.0 * std::numbers::pi
                    * static_cast<double>(k * n) / 32.0);
        return result;
    }();
    return table;
}

FloatWave floating(const SccWaveform& wave) {
    FloatWave result{};
    for (std::size_t n = 0; n < 32; ++n) result[n] = wave[n] / 128.0;
    return result;
}

Spectrum transform(const FloatWave& wave) {
    Spectrum result{};
    const auto& basis = dftBasis();
    for (std::size_t k = 0; k < 17; ++k) {
        for (std::size_t n = 0; n < 32; ++n)
            result[k] += wave[n] * basis[k][n] / 32.0;
    }
    // Real-signal endpoints must not retain roundoff imaginary parts.
    result[0] = {result[0].real(), 0};
    result[16] = {result[16].real(), 0};
    return result;
}

SccMorphAnalysis analyze(const FloatWave& wave) {
    SccMorphAnalysis result;
    const auto spectrum = transform(wave);
    auto& d = result.descriptors;
    double energy{}, ac_energy{}, weighted{}, odd{};
    for (std::size_t n = 0; n < 32; ++n) {
        d.dc += wave[n] / 32.0;
        energy += wave[n] * wave[n] / 32.0;
        d.peak = std::max(d.peak, std::abs(wave[n]));
    }
    d.rms = std::sqrt(energy);
    Harmonics powers{};
    for (std::size_t k = 0; k <= 16; ++k) {
        result.magnitude[k] = std::abs(spectrum[k]);
        result.phase[k] = std::arg(spectrum[k]);
        if (k == 0) continue;
        // Parseval weights conjugate pairs twice; Nyquist only once.
        powers[k] = std::norm(spectrum[k]) * (k == 16 ? 1.0 : 2.0);
        ac_energy += powers[k];
        weighted += powers[k] * static_cast<double>(k) / 16.0;
        if (k & 1U) odd += powers[k];
    }
    if (ac_energy < Config::silence * Config::silence) return result;
    d.centroid = weighted / ac_energy;
    d.odd_even = odd / ac_energy;
    double variance{}, irregular{}, logarithm{};
    double regression{}, x_variance{};
    for (std::size_t k = 1; k <= 16; ++k) {
        const double frequency = static_cast<double>(k) / 16.0;
        variance += powers[k] * std::pow(frequency - d.centroid, 2);
        logarithm += std::log(powers[k] + Config::epsilon * Config::epsilon);
        const double x = frequency - 17.0 / 32.0;
        regression += x * std::log(result.magnitude[k] + Config::epsilon);
        x_variance += x * x;
        if (k < 16) irregular += std::pow(
            result.magnitude[k + 1] - result.magnitude[k], 2);
    }
    d.spread = std::sqrt(variance / ac_energy);
    d.slope = regression / x_variance / -std::log(Config::epsilon);
    d.irregularity = std::sqrt(irregular / ac_energy);
    d.flatness = std::clamp(std::exp(logarithm / 16.0)
        / (ac_energy / 16.0 + Config::epsilon * Config::epsilon), 0.0, 1.0);
    return result;
}

SccWaveform shifted(const SccWaveform& wave, std::size_t shift) {
    SccWaveform result{};
    for (std::size_t n = 0; n < 32; ++n) result[n] = wave[(n + shift) % 32];
    return result;
}

std::size_t align(const SccWaveform& first, const SccWaveform& second) {
    std::size_t selected{};
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t shift = 0; shift < 32; ++shift) {
        const double score = sccMorphTransitionCost(first, shifted(second, shift));
        if (score + Config::tolerance < best) { best = score; selected = shift; }
    }
    return selected;
}

SccMorphDescriptors interpolate(
    const SccMorphDescriptors& a, const SccMorphDescriptors& b, double u) {
    const auto lerp = [u](double first, double second) {
        return (1.0 - u) * first + u * second;
    };
    return {lerp(a.centroid,b.centroid), lerp(a.spread,b.spread),
        lerp(a.slope,b.slope), lerp(a.odd_even,b.odd_even),
        lerp(a.irregularity,b.irregularity), lerp(a.flatness,b.flatness),
        lerp(a.rms,b.rms), lerp(a.dc,b.dc), lerp(a.peak,b.peak)};
}

double descriptorDistance(const SccMorphDescriptors& a,
                          const SccMorphDescriptors& b) {
    const auto sq = [](double x) { return x * x; };
    return Config::centroid_weight * sq(a.centroid - b.centroid)
        + Config::spread_weight * sq(a.spread - b.spread)
        + Config::spectral_slope_weight * sq(a.slope - b.slope)
        + Config::odd_even_weight * sq(a.odd_even - b.odd_even)
        + Config::irregularity_weight * sq(a.irregularity - b.irregularity)
        + Config::flatness_weight * sq(a.flatness - b.flatness)
        + Config::rms_weight * sq(a.rms - b.rms)
        + Config::dc_weight * sq(a.dc - b.dc);
}

double spectralDistance(const Harmonics& actual, const Harmonics& target) {
    double sum{}, weight{};
    for (std::size_t k = 1; k <= 16; ++k) {
        // Silence bins must not outweigh the harmonics that define the tone.
        const double salience = std::sqrt(target[k]) + 0.01;
        const double error = std::log(actual[k] + Config::epsilon)
            - std::log(target[k] + Config::epsilon);
        sum += salience * error * error;
        weight += salience;
    }
    return sum / std::max(weight, Config::epsilon);
}

struct Target {
    SccMorphDescriptors descriptors;
    Harmonics magnitude;
    FloatWave shape;
};

double objective(const FloatWave& wave, const Target& target,
                 const SccMorphAnalysis& actual) {
    double shape{};
    for (std::size_t n = 0; n < 32; ++n)
        shape += std::pow(wave[n] - target.shape[n], 2) / 32.0;
    // Log-spectrum deviation is the required spectral regularization, while
    // the weaker time-domain term discourages exotic phase-only shapes.
    return Config::spectrum_weight * spectralDistance(actual.magnitude, target.magnitude)
        + descriptorDistance(actual.descriptors, target.descriptors)
        + Config::shape_weight * shape;
}

FloatWave correct(FloatWave wave, const SccMorphDescriptors& target) {
    double mean{}, energy{};
    for (const double sample : wave) mean += sample / 32.0;
    for (double& sample : wave) { sample -= mean; energy += sample*sample / 32.0; }
    const double desired = std::sqrt(std::max(0.0,
        target.rms * target.rms - target.dc * target.dc));
    double gain = energy > Config::silence * Config::silence
        ? desired / std::sqrt(energy) : 0.0;
    // Fit the AC excursion around the interpolated DC before quantization.
    // This preserves DC and avoids clipping as a primary peak-management tool.
    for (const double sample : wave) {
        if (sample > Config::epsilon)
            gain = std::min(gain, (127.0/128.0 - target.dc) / sample);
        else if (sample < -Config::epsilon)
            gain = std::min(gain, (-1.0 - target.dc) / sample);
    }
    gain = std::max(0.0, gain);
    for (double& sample : wave) sample = target.dc + gain * sample;
    return wave;
}

SccWaveform quantize(const FloatWave& wave) {
    SccWaveform result{};
    for (std::size_t n = 0; n < 32; ++n)
        result[n] = static_cast<std::int8_t>(std::clamp(
            std::llround(wave[n] * 128.0), -128LL, 127LL));
    return result;
}

FloatWave reconstruct(const Harmonics& magnitude, const Spectrum& guide,
                      const SccMorphDescriptors& target,
                      FloatWave* uncorrected = nullptr) {
    FloatWave result{};
    const auto& basis = dftBasis();
    for (std::size_t n = 0; n < 32; ++n) {
        result[n] = target.dc;
        for (std::size_t k = 1; k < 16; ++k)
            result[n] += 2.0 * std::real(
                magnitude[k] * guide[k] * std::conj(basis[k][n]));
        // A real Nyquist coefficient is its own conjugate. Its phase can
        // only encode sign, never an arbitrary unit complex angle.
        result[n] += magnitude[16] * (guide[16].real() >= 0.0 ? 1.0 : -1.0)
            * (n & 1U ? -1.0 : 1.0);
    }
    if (uncorrected) *uncorrected = result;
    return correct(result, target);
}

Harmonics smoothEnvelope(const Harmonics& magnitude) {
    Harmonics result{};
    for (int k = 1; k <= 16; ++k) {
        double weight{};
        for (int offset = -2; offset <= 2; ++offset) {
            const int index = std::clamp(k + offset, 1, 16);
            const double w = static_cast<double>(3 - std::abs(offset));
            result[static_cast<std::size_t>(k)] += w
                * std::log(magnitude[static_cast<std::size_t>(index)] + Config::epsilon);
            weight += w;
        }
        result[static_cast<std::size_t>(k)] /= weight;
    }
    return result;
}

struct Candidate {
    SccWaveform wave;
    SccMorphAnalysis analysis;
    SccMorphCandidate method{};
    double pre_error{}, error{};
    FloatWave fitted{};
    std::array<double,3> weights{};
};

Candidate spectralCandidate(Harmonics magnitude, const Spectrum& guide,
                            const Target& target, SccMorphCandidate method,
                            SccMorphCandidateTrace* trace = nullptr) {
    auto wave = reconstruct(magnitude, guide, target.descriptors,
                            trace ? &trace->reconstructed : nullptr);
    if (trace) { trace->method = method; trace->corrected = wave; }
    double best = objective(wave, target, analyze(wave));
    // Deterministic spectral coordinate descent before quantization. This
    // reconciles linear descriptor targets with nonlinear log-magnitude paths.
    for (std::size_t k = 1; k <= 16; ++k) {
        if (magnitude[k] < Config::silence) continue;
        const double original = magnitude[k];
        double selected = original;
        for (const double direction : {-1.0, 1.0}) {
            magnitude[k] = original * std::exp(direction * Config::harmonic_search_step);
            const auto attempt = reconstruct(magnitude, guide, target.descriptors);
            const double score = objective(attempt, target, analyze(attempt));
            if (score + Config::tolerance < best) {
                best = score; wave = attempt; selected = magnitude[k];
            }
        }
        magnitude[k] = selected;
    }
    Candidate candidate;
    candidate.method = method;
    if (method != SccMorphCandidate::Blend)
        candidate.weights[method == SccMorphCandidate::LogHarmonic ? 0 : 2] = 1.0;
    candidate.pre_error = best;
    candidate.wave = quantize(wave);
    candidate.analysis = analyze(floating(candidate.wave));
    candidate.error = objective(floating(candidate.wave), target, candidate.analysis);
    candidate.fitted = wave;
    if (trace) {
        trace->fitted = wave;
        trace->quantized = candidate.wave;
        trace->pre_quantization_error = candidate.pre_error;
        trace->post_quantization_error = candidate.error;
        trace->weights = candidate.weights;
    }
    return candidate;
}

double trajectoryPosition(const SccMorphDescriptors& value,
                          const SccMorphDescriptors& first,
                          const SccMorphDescriptors& second) {
    const std::array a{first.centroid,first.spread,first.slope,first.odd_even,
        first.irregularity,first.flatness,first.rms,first.dc};
    const std::array b{second.centroid,second.spread,second.slope,second.odd_even,
        second.irregularity,second.flatness,second.rms,second.dc};
    const std::array x{value.centroid,value.spread,value.slope,value.odd_even,
        value.irregularity,value.flatness,value.rms,value.dc};
    double numerator{}, denominator{};
    for (std::size_t i = 0; i < a.size(); ++i) {
        numerator += (x[i]-a[i])*(b[i]-a[i]);
        denominator += (b[i]-a[i])*(b[i]-a[i]);
    }
    return denominator > Config::epsilon ? numerator / denominator : 0.0;
}

double connection(const Candidate& a, const Candidate& b, double dt, double du,
                  const SccMorphDescriptors& first,
                  const SccMorphDescriptors& second,
                  SccMorphConnectionTrace* trace = nullptr) {
    SccMorphConnectionTrace edge;
    edge.dt = dt; edge.du = du;
    // Circular shift is exclusively a distance measurement here. Candidate
    // bytes and the block's endpoint/phase policy remain unchanged.
    std::uint32_t wave = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t shift = 0; shift < 32; ++shift) {
        std::uint32_t error{};
        for (std::size_t n = 0; n < 32; ++n) {
            const int difference = int(a.wave[n])-int(b.wave[(n+shift)%32]);
            error += static_cast<std::uint32_t>(difference*difference);
        }
        wave = std::min(wave,error);
    }
    // At most 32*255^2, so exact integer accumulation fits uint32_t and avoids
    // floating normalization inside all 32 alignment trials.
    edge.phase_wave = double(wave)/(32.0*128.0*128.0*4.0);
    edge.rms = std::pow(a.analysis.descriptors.rms-b.analysis.descriptors.rms,2);
    double energy_a{}, energy_b{};
    for (std::size_t k = 1; k <= 16; ++k) {
        const double weight = k == 16 ? 1.0 : 2.0;
        const double ma = a.analysis.magnitude[k], mb = b.analysis.magnitude[k];
        edge.harmonic += weight*(ma-mb)*(ma-mb);
        energy_a += weight*ma*ma; energy_b += weight*mb*mb;
    }
    for (std::size_t k = 1; k <= 16; ++k) {
        const double weight = k == 16 ? 1.0 : 2.0;
        const double ma = a.analysis.magnitude[k], mb = b.analysis.magnitude[k];
        const double power_a = weight*ma*ma / std::max(energy_a,Config::tolerance);
        const double power_b = weight*mb*mb / std::max(energy_b,Config::tolerance);
        edge.spectral_shape += (power_a-power_b)*(power_a-power_b)/2.0;
    }
    // A virtually silent tone has no salient normalized spectral shape.
    edge.spectral_shape *= std::min(1.0,std::sqrt(std::max(energy_a,energy_b)));
    edge.descriptor = descriptorDistance(a.analysis.descriptors,b.analysis.descriptors)
        / Config::descriptor_weight_sum;
    edge.distance = Config::connection_wave_weight*edge.phase_wave
        + Config::connection_rms_weight*edge.rms
        + Config::connection_harmonic_weight*edge.harmonic
        + Config::connection_shape_weight*edge.spectral_shape
        + Config::connection_descriptor_weight*edge.descriptor;
    const double reverse = std::max(0.0,
        trajectoryPosition(a.analysis.descriptors, first, second)
        - trajectoryPosition(b.analysis.descriptors, first, second));
    edge.reverse = reverse;
    // Integrate squared velocity over normalized time. The fourth-power
    // term measures an unusually large change relative to the authored du;
    // it follows fast curves rather than imposing a uniform perceptual rate.
    const double interval = std::max(dt,Config::tolerance);
    // Below one byte of progress per ideal interval, rate estimates describe
    // quantization noise rather than controllable motion. This also bounds
    // gamma=8 tails where adjacent u values can round to exactly 1.0.
    const double position = std::max(du,interval/128.0);
    edge.cost = Config::motion_weight*(edge.distance/interval
        + edge.distance*edge.distance/(interval*position*position))
        + interval*Config::trajectory_weight*reverse*reverse;
    if (trace) *trace = edge;
    return edge.cost;
}

SccWaveform sourceWave(const SavedTimbreReference& reference) {
    SccWaveform result{};
    for (std::size_t n = 0; n < 32; ++n) {
        const auto value = reference.scc_waveform[n];
        result[n] = static_cast<std::int8_t>(value < 128 ? value : int(value)-256);
    }
    return result;
}

} // namespace

double morphPosition(double time, double gamma) noexcept {
    if (!std::isfinite(time)) time = 0.0;
    if (!std::isfinite(gamma)) gamma = 1.0;
    time = std::clamp(time, 0.0, 1.0);
    gamma = std::clamp(gamma, 1.0, kSccMorphGammaMax);
    return 1.0 - std::pow(1.0 - time, gamma);
}

SccMorphAnalysis analyzeSccMorphWaveform(const SccWaveform& wave) {
    return analyze(floating(wave));
}

double sccMorphTransitionCost(const SccWaveform& first,
                             const SccWaveform& second) noexcept {
    double value{}, slope{};
    for (std::size_t n = 0; n < 32; ++n) {
        const double difference = (double(first[n]) - double(second[n])) / 128.0;
        const auto next = (n + 1) % 32;
        const double derivative = (double(first[next]) - double(first[n])
            - double(second[next]) + double(second[n])) / 128.0;
        value += difference*difference / 32.0;
        slope += derivative*derivative / 32.0;
    }
    return Config::value_weight * value + Config::slope_weight * slope;
}

namespace {

std::vector<double> playbackIntervals(const std::vector<std::uint32_t>& counts) {
    std::vector<double> intervals;
    intervals.reserve(counts.size()-1);
    const double duration = double(counts.back())-double(counts.front());
    for (std::size_t index = 1; index < counts.size(); ++index)
        intervals.push_back((double(counts[index])-double(counts[index-1]))/duration);
    return intervals;
}

SccMorphPairResult generateSccMorphImpl(const SccWaveform& first,
    const SccWaveform& second, std::uint8_t count, double gamma,
    SccMorphPairTrace* trace, const std::vector<double>* intervals = nullptr) {
    SccMorphPairResult result;
    if (trace) {
        *trace = {};
        trace->first = first; trace->second = second;
        trace->epsilon = Config::epsilon; trace->silence = Config::silence;
    }
    if (count == 0) return result;
    const auto alignment_shift = align(first, second);
    const auto aligned = shifted(second, alignment_shift);
    const auto a = analyzeSccMorphWaveform(first);
    const auto b = analyzeSccMorphWaveform(aligned);
    if (trace) {
        trace->alignment_shift = alignment_shift;
        trace->aligned_second = aligned;
        trace->first_analysis = a; trace->second_analysis = b;
        trace->steps.resize(count);
    }
    const auto fa = floating(first), fb = floating(aligned);
    const auto spectrum_a = transform(fa), spectrum_b = transform(fb);
    const auto envelope_a = smoothEnvelope(a.magnitude);
    const auto envelope_b = smoothEnvelope(b.magnitude);
    std::vector<Target> targets;
    std::vector<std::array<Candidate,kSccMorphCandidateCount>> choices;
    std::vector<std::array<Harmonics,3>> mixture_bases;
    targets.reserve(count); choices.reserve(count);
    mixture_bases.reserve(count);
    for (unsigned index = 1; index <= count; ++index) {
        const double u = morphPosition(double(index) / (double(count)+1.0), gamma);
        Target target{};
        target.descriptors = interpolate(a.descriptors,b.descriptors,u);
        for (std::size_t n = 0; n < 32; ++n)
            target.shape[n] = (1-u)*fa[n] + u*fb[n];
        auto* step = trace ? &trace->steps[index-1] : nullptr;
        if (step) {
            step->t = double(index) / (double(count)+1.0);
            step->u = u; step->target = target.descriptors;
            step->target_shape = target.shape;
        }
        // Phase-equivalent sources require no synthesized timbre changes.
        if (first == aligned) {
            Candidate candidate{first,a,SccMorphCandidate::Source,0,0};
            std::array<Candidate,kSccMorphCandidateCount> unchanged;
            unchanged.fill(candidate);
            choices.push_back(unchanged);
            mixture_bases.push_back({a.magnitude,a.magnitude,a.magnitude});
            target.magnitude = a.magnitude;
            if (step) {
                step->raw_log_magnitude = step->target_magnitude
                    = step->advanced_magnitude = a.magnitude;
                step->guide_phase = a.phase;
                for (auto& candidate_trace : step->candidates) {
                    candidate_trace.reconstructed = candidate_trace.corrected
                        = candidate_trace.fitted = fa;
                    candidate_trace.quantized = first;
                }
            }
            targets.push_back(target);
            continue;
        }
        Spectrum guide{};
        Harmonics advanced{};
        for (std::size_t k = 1; k <= 16; ++k) {
            if (a.magnitude[k] < Config::silence && b.magnitude[k] < Config::silence) {
                target.magnitude[k] = 0.0; advanced[k] = 0.0;
            } else {
                const double logarithm = (1-u)*std::log(a.magnitude[k]+Config::epsilon)
                    + u*std::log(b.magnitude[k]+Config::epsilon);
                target.magnitude[k] = std::max(0.0,std::exp(logarithm)-Config::epsilon);
                // Smooth envelope interpolation in linear amplitude space,
                // plus the original log-domain fine-structure residual.
                const double envelope = std::log((1-u)*std::exp(envelope_a[k])
                    + u*std::exp(envelope_b[k]));
                const double residual = (1-u)*(std::log(a.magnitude[k]+Config::epsilon)-envelope_a[k])
                    + u*(std::log(b.magnitude[k]+Config::epsilon)-envelope_b[k]);
                advanced[k] = std::max(0.0,std::exp(envelope+residual)-Config::epsilon);
            }
            const auto phase = (1-u)*spectrum_a[k] + u*spectrum_b[k];
            const auto fallback = a.magnitude[k]*(1-u) >= b.magnitude[k]*u
                ? spectrum_a[k] : spectrum_b[k];
            const auto chosen = std::abs(phase)>Config::epsilon ? phase : fallback;
            guide[k] = std::abs(chosen)>Config::epsilon ? chosen/std::abs(chosen)
                                                       : std::complex<double>{1,0};
        }
        if (step) {
            step->raw_log_magnitude = target.magnitude;
            step->advanced_magnitude = advanced;
            for (std::size_t k = 0; k <= 16; ++k)
                step->guide_phase[k] = std::arg(guide[k]);
        }
        // The log spectrum controls harmonic proportions, not overall gain.
        // Evaluate against the same interpolated RMS/DC used by reconstruction
        // so silence fades and endpoint level differences are not penalized
        // for following their level target.
        double target_ac_energy{};
        for (std::size_t k = 1; k <= 16; ++k)
            target_ac_energy += target.magnitude[k]*target.magnitude[k]
                * (k == 16 ? 1.0 : 2.0);
        const double desired_ac_energy = std::max(0.0,
            target.descriptors.rms*target.descriptors.rms
            - target.descriptors.dc*target.descriptors.dc);
        if (target_ac_energy > std::numeric_limits<double>::min()) {
            const double gain = std::sqrt(desired_ac_energy / target_ac_energy);
            for (std::size_t k = 1; k <= 16; ++k) target.magnitude[k] *= gain;
        }
        const auto time = correct(target.shape,target.descriptors);
        Candidate time_candidate;
        time_candidate.wave = quantize(time);
        time_candidate.analysis = analyzeSccMorphWaveform(time_candidate.wave);
        time_candidate.method = SccMorphCandidate::AlignedTime;
        time_candidate.fitted = time;
        time_candidate.weights[1] = 1.0;
        time_candidate.pre_error = objective(time,target,analyze(time));
        time_candidate.error = objective(floating(time_candidate.wave),target,time_candidate.analysis);
        if (step) {
            step->target_magnitude = target.magnitude;
            auto& candidate_trace = step->candidates[1];
            candidate_trace.method = SccMorphCandidate::AlignedTime;
            candidate_trace.reconstructed = target.shape;
            candidate_trace.corrected = candidate_trace.fitted = time;
            candidate_trace.quantized = time_candidate.wave;
            candidate_trace.pre_quantization_error = time_candidate.pre_error;
            candidate_trace.post_quantization_error = time_candidate.error;
            candidate_trace.weights = time_candidate.weights;
        }
        std::array<Candidate,kSccMorphCandidateCount> candidates;
        candidates[0] = spectralCandidate(target.magnitude,guide,target,
            SccMorphCandidate::LogHarmonic,step ? &step->candidates[0] : nullptr);
        candidates[1] = time_candidate;
        candidates[2] = spectralCandidate(advanced,guide,target,
            SccMorphCandidate::SpectralEnvelope,step ? &step->candidates[2] : nullptr);
        // All three original float candidates share the complex guide phases:
        // time interpolation produces that guide, and correction/coordinate
        // fitting only change real AC gain and harmonic magnitudes. Blend the
        // fitted amplitudes under that guide, so incompatible phases cannot
        // cancel. Reuse those fitted spectra, then apply the same bounded
        // correction, harmonic fit, and post-quantization objective.
        std::array<Harmonics,3> fitted_magnitude;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto spectrum = transform(candidates[c].fitted);
            for (std::size_t k = 0; k <= 16; ++k)
                fitted_magnitude[c][k] = std::abs(spectrum[k]);
        }
        mixture_bases.push_back(fitted_magnitude);
        std::size_t state = 3;
        for (const auto [left,right] : {std::pair{0U,1U},std::pair{1U,2U},std::pair{0U,2U}})
            for (const double mix : {0.25,0.5,0.75}) {
                Harmonics magnitude{};
                for (std::size_t k = 1; k <= 16; ++k)
                    magnitude[k] = (1-mix)*fitted_magnitude[left][k]
                        + mix*fitted_magnitude[right][k];
                auto* candidate_trace = step ? &step->candidates[state] : nullptr;
                candidates[state] = spectralCandidate(magnitude,guide,target,
                    SccMorphCandidate::Blend,candidate_trace);
                candidates[state].weights[left] = 1-mix;
                candidates[state].weights[right] = mix;
                if (candidate_trace) candidate_trace->weights = candidates[state].weights;
                ++state;
            }
        choices.push_back(candidates);
        targets.push_back(target);
    }
    // Choose a whole trajectory of post-quantized candidates. Independent
    // per-node minima can produce descriptor reversals or abrupt neighbors.
    const Candidate start{first,a,SccMorphCandidate::Source,0,0};
    const Candidate end{aligned,b,SccMorphCandidate::Source,0,0};
    const double ideal_dt = 1.0/(double(count)+1.0);
    const auto interval = [&](std::size_t arrival) {
        return intervals ? (*intervals)[arrival-1] : ideal_dt;
    };
    // Trapezoidal time integration gives each interior node the half-width
    // of its two neighboring intervals. Uniform timing reduces to ideal_dt.
    const auto node_width = [&](std::size_t index) {
        return (interval(index+1)+interval(index+2))/2.0;
    };
    const auto position = [&](std::size_t index) {
        return morphPosition(double(index)/(double(count)+1.0),gamma);
    };
    const auto connect = [&](const Candidate& left, const Candidate& right,
                             std::size_t arrival, SccMorphConnectionTrace* edge = nullptr) {
        double value = connection(left,right,interval(arrival),
            position(arrival)-position(arrival-1),a.descriptors,b.descriptors,edge);
        if (arrival > 1 && arrival <= count) {
            // Evaluate both coefficient vectors against the same arrival-node
            // fitted spectra under the common phase guide. Parseval weights
            // exclude DC: all candidates have the same authored DC target.
            // Source endpoints have no mixture coefficients and are excluded.
            const auto& bases = mixture_bases[arrival-1];
            double mixture{};
            for (std::size_t k = 1; k <= 16; ++k) {
                double difference{};
                for (std::size_t c = 0; c < 3; ++c)
                    difference += (right.weights[c]-left.weights[c])*bases[c][k];
                mixture += (k == 16 ? 1.0 : 2.0)*difference*difference;
            }
            value += Config::mixture_weight*mixture
                / std::max(interval(arrival),Config::tolerance);
            // distance retains the physical waveform/feature components;
            // cost includes this coefficient term and can be reconstructed
            // from the recorded original candidates and mixture weights.
            if (edge) edge->cost = value;
        }
        return value;
    };
    std::vector<std::array<double,kSccMorphCandidateCount>> cost(count);
    std::vector<std::array<std::size_t,kSccMorphCandidateCount>> previous(count);
    for (std::size_t c = 0; c < kSccMorphCandidateCount; ++c) {
        auto* incoming = trace ? &trace->steps[0].candidates[c].incoming : nullptr;
        cost[0][c] = choices[0][c].error*node_width(0)
            + connect(start,choices[0][c],1,incoming ? &(*incoming)[0] : nullptr);
        if (incoming) incoming->fill((*incoming)[0]);
    }
    for (std::size_t index = 1; index < count; ++index)
        for (std::size_t c = 0; c < kSccMorphCandidateCount; ++c) {
            cost[index][c] = std::numeric_limits<double>::infinity();
            for (std::size_t p = 0; p < kSccMorphCandidateCount; ++p) {
                auto* edge = trace ? &trace->steps[index].candidates[c].incoming[p] : nullptr;
                const double candidate = cost[index-1][p] + choices[index][c].error*node_width(index)
                    + connect(choices[index-1][p],choices[index][c],index+1,edge);
                if (candidate + Config::tolerance < cost[index][c]) {
                    cost[index][c] = candidate; previous[index][c] = p;
                }
            }
        }
    std::size_t selected{};
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t c = 0; c < kSccMorphCandidateCount; ++c) {
        const double candidate = cost[count-1][c]
            + connect(choices[count-1][c],end,count+1,trace ? &trace->terminal[c] : nullptr);
        if (candidate + Config::tolerance < best) { best=candidate; selected=c; }
    }
    std::vector<Candidate> trajectory(count);
    if (trace) {
        trace->trajectory_cost = best;
        for (std::size_t index = 0; index < count; ++index)
            for (std::size_t c = 0; c < kSccMorphCandidateCount; ++c) {
                auto& candidate_trace = trace->steps[index].candidates[c];
                candidate_trace.cumulative_cost = cost[index][c];
                candidate_trace.predecessor = previous[index][c];
            }
    }
    for (std::size_t index = count; index-- > 0;) {
        trajectory[index] = choices[index][selected];
        if (trace) {
            trace->steps[index].selected_candidate = selected;
            trace->steps[index].selected_pre_local = trajectory[index].wave;
            trace->steps[index].selected_pre_local_error = trajectory[index].error;
        }
        selected = previous[index][selected];
    }
    // Re-evaluate each +/- one-byte mutation, including its neighbors, after
    // quantization. Source endpoints and phase-only correspondences never move.
    if (first != aligned)
        for (int pass = 0; pass < Config::quantization_passes; ++pass)
            for (std::size_t index = 0; index < count; ++index) {
                auto& current = trajectory[index];
                const auto& left = index ? trajectory[index-1] : start;
                const auto& right = index+1 < count ? trajectory[index+1] : end;
                const auto quality = [&](const Candidate& candidate) {
                    return candidate.error*node_width(index)
                        + connect(left,candidate,index+1)
                        + connect(candidate,right,index+2);
                };
                double best_score = quality(current);
                for (std::size_t n = 0; n < 32; ++n) {
                    const int original = current.wave[n];
                    Candidate chosen = current;
                    for (const int direction : {-1,1}) {
                        if (original+direction < -128 || original+direction > 127) continue;
                        Candidate attempt = current;
                        attempt.wave[n] = static_cast<std::int8_t>(original+direction);
                        attempt.analysis = analyzeSccMorphWaveform(attempt.wave);
                        attempt.error = objective(floating(attempt.wave),targets[index],attempt.analysis);
                        const double score = quality(attempt);
                        if (score + Config::tolerance < best_score) {
                            best_score=score; chosen=attempt;
                        }
                    }
                    current = chosen;
                }
            }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& candidate = trajectory[index];
        if (trace) {
            trace->steps[index].selected_post_local = candidate.wave;
            trace->steps[index].selected_post_local_error = candidate.error;
        }
        result.intermediate.push_back(candidate.wave);
        SccMorphWaveDiagnostic diagnostic;
        diagnostic.u = morphPosition(double(index+1)/(double(count)+1.0),gamma);
        diagnostic.candidate = candidate.method;
        diagnostic.candidate_weights = candidate.weights;
        diagnostic.pre_quantization_error = candidate.pre_error;
        diagnostic.post_quantization_error = candidate.error;
        diagnostic.target = targets[index].descriptors;
        diagnostic.result = candidate.analysis.descriptors;
        diagnostic.generated = true;
        result.diagnostics.push_back(diagnostic);
    }
    return result;
}

} // namespace

SccMorphPairResult generateSccMorph(const SccWaveform& first,
    const SccWaveform& second, std::uint8_t count, double gamma) {
    return generateSccMorphImpl(first,second,count,gamma,nullptr);
}

SccMorphPairResult generateSccMorphTraced(const SccWaveform& first,
    const SccWaveform& second, std::uint8_t count, double gamma,
    SccMorphPairTrace& trace) {
    return generateSccMorphImpl(first,second,count,gamma,&trace);
}

SccMorphPairResult generateSccMorphTraced(const SccWaveform& first,
    const SccWaveform& second, std::uint8_t count, double gamma,
    SccMorphPairTrace& trace, const std::vector<std::uint32_t>& counts) {
    if (counts.size() != std::size_t(count)+2
        || std::adjacent_find(counts.begin(),counts.end(),
            [](auto left,auto right) { return left >= right; }) != counts.end())
        throw std::invalid_argument("SCC morph diagnostic counts must increase and include both endpoints");
    const auto intervals = playbackIntervals(counts);
    return generateSccMorphImpl(first,second,count,gamma,&trace,&intervals);
}

std::vector<std::size_t> optimizeSccMorphBlockPhases(
    std::vector<SccWaveform>& waves, bool fixed_start, bool fixed_end) {
    std::vector<std::size_t> shifts(waves.size());
    if (waves.empty()) return shifts;
    std::vector<std::array<std::size_t,32>> back(waves.size());
    std::array<double,32> previous{}, current{};
    for (std::size_t shift = 0; shift < 32; ++shift)
        previous[shift] = fixed_start && shift != 0
            ? std::numeric_limits<double>::infinity() : 0.0;
    for (std::size_t index = 1; index < waves.size(); ++index) {
        // Cost depends only on relative phase: precompute 32 correlations,
        // then solve all 32 x 32 DP edges without repeated sample work.
        std::array<double,32> relative{};
        for (std::size_t shift = 0; shift < 32; ++shift)
            relative[shift] = sccMorphTransitionCost(waves[index-1],shifted(waves[index],shift));
        current.fill(std::numeric_limits<double>::infinity());
        for (std::size_t shift = 0; shift < 32; ++shift) {
            if (fixed_end && index+1 == waves.size() && shift != 0) continue;
            for (std::size_t p = 0; p < 32; ++p) {
                const double cost = previous[p] + relative[(shift+32-p)%32];
                if (cost + Config::tolerance < current[shift]) {
                    current[shift] = cost; back[index][shift] = p;
                }
            }
        }
        previous = current;
    }
    shifts.back() = static_cast<std::size_t>(
        std::distance(previous.begin(),std::min_element(previous.begin(),previous.end())));
    for (std::size_t index = waves.size()-1; index > 0; --index)
        shifts[index-1] = back[index][shifts[index]];
    for (std::size_t index = 0; index < waves.size(); ++index)
        waves[index] = shifted(waves[index],shifts[index]);
    return shifts;
}

SccMorphCompileResult compileSccMorph(const CompositeTimbre& input) {
    SccMorphCompileResult result;
    result.timbre = input;
    const bool active = std::any_of(input.layers.begin(),input.layers.end(),
        [](const CompositeLayer& layer) {
            return layer.volume_envelope.kind == EnvelopeKind::Sequence
                && std::any_of(layer.timbre_automation.begin(),layer.timbre_automation.end(),
                [](const EnvelopeEvent& event) { return event.scc_morph.enabled; });
        });
    if (!active || input.scc_morph_materialized) {
        const auto numbers = resolveTimbreNumbers(input);
        std::array<bool,32> reserved{};
        for (const auto& assignment : numbers.assignments) {
            const auto* reference = findEmbeddedTimbreSnapshot(input,assignment.library_id);
            if (reference && reference->source == TimbreSource::Scc && assignment.number < 32)
                reserved[assignment.number] = true;
        }
        for (const auto& layer : input.layers) {
            if (layer.source != TimbreSource::Scc) continue;
            if (!layer.base_timbre) reserved[0] = true;
            if (layer.volume_envelope.kind != EnvelopeKind::Sequence) continue;
            for (const auto& event : layer.timbre_automation)
                if (event.kind == EnvelopeEventKind::Timbre && !event.target_library_id
                    && event.value >= 0 && event.value <= 31)
                    reserved[static_cast<std::size_t>(event.value)] = true;
        }
        for (std::size_t number = input.scc_morph_bank_base == 16 ? 16 : 0;
             number < 32; ++number)
            if (!reserved[number]) ++result.capacity;
        return result;
    }
    const auto fail = [&](std::string error) {
        result.valid = false;
        result.error = std::move(error);
        result.timbre = input; // Never publish a partial program/cache.
        result.used = 0;
        return result;
    };
    if (input.scc_morph_bank_base != 0 && input.scc_morph_bank_base != 16)
        return fail("SCC Morph Bank start must be @0 or @16");
    if (!isSupportedSccMorphAlgorithmVersion(input.scc_morph_algorithm_version))
        return fail("Unsupported SCC morph algorithm version");
    const auto source_numbers = resolveTimbreNumbers(input);
    for (const auto& warning : source_numbers.warnings) {
        // Legacy import preserves @s0..14 then falls back to automatic15..31.
        // That expected warning is safely recovered by freezing the assignment.
        if (warning.find("uses an out-of-range timbre number") == std::string::npos)
            return fail("SCC morph source allocation: " + warning);
    }
    std::array<bool,32> occupied{};
    struct Materialized { std::uint64_t id{}; std::uint8_t number{}; bool derived{}; };
    std::map<SccWaveform,Materialized> materialized;
    // Preserve current automatic source allocation. Generated definitions may
    // fill lower free slots, but cannot move a normal existing @ switch.
    for (const auto& assignment : source_numbers.assignments) {
        const auto* reference = findEmbeddedTimbreSnapshot(input,assignment.library_id);
        if (!reference || reference->source != TimbreSource::Scc) continue;
        if (assignment.number > 31) return fail("SCC source number is out of range");
        occupied[assignment.number] = true;
        const auto wave = sourceWave(*reference);
        const auto existing = materialized.find(wave);
        if (existing == materialized.end() || assignment.number < existing->second.number)
            materialized[wave] = {reference->library_id,assignment.number,false};
    }
    for (const auto& layer : input.layers) {
        if (layer.source != TimbreSource::Scc) continue;
        if (!layer.base_timbre) occupied[0] = true;
        if (layer.volume_envelope.kind != EnvelopeKind::Sequence) continue;
        for (const auto& event : layer.timbre_automation)
            if (event.kind == EnvelopeEventKind::Timbre && !event.target_library_id
                && event.value >= 0 && event.value <= 31)
                occupied[static_cast<std::size_t>(event.value)] = true;
    }
    for (std::size_t number = input.scc_morph_bank_base; number < 32; ++number)
        if (!occupied[number]) ++result.capacity;
    const auto freeze = [&](SavedTimbreReference& reference) {
        if (reference.source != TimbreSource::Scc) return;
        if (const auto number = assignedNumberForLibraryId(source_numbers,reference.library_id)) {
            reference.number_mode = TimbreNumberMode::Manual;
            reference.manual_number = *number;
        }
    };
    for (auto& layer : result.timbre.layers)
        if (layer.base_timbre) freeze(*layer.base_timbre);
    for (auto& reference : result.timbre.embedded_timbres) freeze(reference);
    result.timbre.scc_morph_materialized = true;
    result.timbre.scc_morph_algorithm_version = kSccMorphAlgorithmVersion;

    const auto sourceForEvent = [&](const EnvelopeEvent& event)
        -> const SavedTimbreReference* {
        if (event.timbre_pick != TimbrePick::Library) return nullptr;
        if (event.target_library_id)
            return findEmbeddedTimbreSnapshot(input,event.target_library_id);
        // Imported/raw-number events can use an owned snapshot assigned to
        // that number, but must never depend on the live timbre library.
        for (const auto& assignment : source_numbers.assignments)
            if (assignment.number == event.value) {
                const auto* reference = findEmbeddedTimbreSnapshot(input,assignment.library_id);
                if (reference && reference->source == TimbreSource::Scc) return reference;
            }
        for (const auto& assignment : source_numbers.assignments) {
            const auto* reference = findEmbeddedTimbreSnapshot(input,assignment.library_id);
            if (reference && reference->source == TimbreSource::Scc
                && reference->manual_number && *reference->manual_number == event.value)
                return reference;
        }
        return nullptr;
    };
    // An imported raw @ may address the original manual alias that legacy
    // audition also defines. Pin its owned snapshot before freezing numbering;
    // otherwise the alias could disappear from the materialized program.
    for (std::size_t index = 0; index < input.layers.size(); ++index) {
        if (input.layers[index].source != TimbreSource::Scc) continue;
        if (input.layers[index].volume_envelope.kind != EnvelopeKind::Sequence) continue;
        for (std::size_t event_index = 0;
             event_index < input.layers[index].timbre_automation.size(); ++event_index) {
            const auto& original = input.layers[index].timbre_automation[event_index];
            if (original.kind != EnvelopeEventKind::Timbre || original.target_library_id
                || original.timbre_pick != TimbrePick::Library) continue;
            std::optional<SccWaveform> matching_wave;
            for (const auto& assignment : source_numbers.assignments) {
                const auto* candidate = findEmbeddedTimbreSnapshot(input,assignment.library_id);
                if (!candidate || candidate->source != TimbreSource::Scc) continue;
                if (assignment.number != original.value
                    && (!candidate->manual_number || *candidate->manual_number != original.value))
                    continue;
                const auto wave = sourceWave(*candidate);
                if (matching_wave && *matching_wave != wave)
                    return fail("SCC morph source allocation has an ambiguous raw tone number");
                matching_wave = wave;
            }
            const auto* reference = sourceForEvent(original);
            if (!reference) continue;
            auto& event = result.timbre.layers[index].timbre_automation[event_index];
            event.target_library_id = reference->library_id;
            event.value = *assignedNumberForLibraryId(source_numbers,reference->library_id);
        }
    }
    struct Node {
        SccWaveform wave;
        std::optional<std::size_t> event_index;
        SccMorphWaveDiagnostic diagnostic;
    };
    struct Pending { std::size_t layer{}; Node node; };
    std::vector<Pending> pending;
    // Integer event placement below is translation-invariant in start count:
    // rounded and clamp bounds are all start + an integer relative count.
    // Duration therefore uniquely determines the normalized interval vector.
    using PairKey = std::tuple<SccWaveform,SccWaveform,std::uint8_t,double,std::uint32_t>;
    std::map<PairKey,SccMorphPairResult> pair_cache;
    for (std::size_t layer_index = 0; layer_index < input.layers.size(); ++layer_index) {
        const auto& layer = input.layers[layer_index];
        // Rate mode retains the authored @e lane for switching back, but
        // none of its tone events participate in the executable program.
        if (layer.volume_envelope.kind != EnvelopeKind::Sequence) continue;
        std::vector<std::size_t> tones;
        for (std::size_t i = 0; i < layer.timbre_automation.size(); ++i) {
            const auto& event = layer.timbre_automation[i];
            if (event.scc_morph.enabled
                && (layer.source != TimbreSource::Scc
                    || event.kind != EnvelopeEventKind::Timbre))
                return fail("Morphing is available only on SCC tone events");
            if (event.kind == EnvelopeEventKind::Timbre) tones.push_back(i);
        }
        std::stable_sort(tones.begin(),tones.end(),[&](std::size_t a,std::size_t b) {
            const auto& first = layer.timbre_automation[a];
            const auto& second = layer.timbre_automation[b];
            if (first.count != second.count) return first.count < second.count;
            return first.after_loop_start < second.after_loop_start;
        });
        if (tones.empty()) continue;
        if (layer.timbre_automation[tones.front()].scc_morph.enabled)
            return fail("SCC morph destination has no preceding tone event");
        for (std::size_t arrival = 1; arrival < tones.size();) {
            if (!layer.timbre_automation[tones[arrival]].scc_morph.enabled) {
                ++arrival; continue;
            }
            const std::size_t block_start = arrival-1;
            std::size_t block_end = arrival;
            while (block_end+1 < tones.size()
                && layer.timbre_automation[tones[block_end+1]].scc_morph.enabled)
                ++block_end;
            std::vector<Node> nodes;
            const auto sourceNode = [&](std::size_t tone_position) -> std::optional<Node> {
                const auto event_index = tones[tone_position];
                const auto& event = layer.timbre_automation[event_index];
                const auto* source = sourceForEvent(event);
                if (!source || source->source != TimbreSource::Scc) return std::nullopt;
                if (!assignedNumberForLibraryId(source_numbers,source->library_id)) return std::nullopt;
                Node node;
                node.wave = sourceWave(*source);
                node.event_index = event_index;
                node.diagnostic.layer_index = layer_index;
                node.diagnostic.count = event.count;
                node.diagnostic.result = analyzeSccMorphWaveform(node.wave).descriptors;
                node.diagnostic.target = node.diagnostic.result;
                return node;
            };
            const auto start_node = sourceNode(block_start);
            if (!start_node) return fail("SCC morph source waveform snapshot is unavailable");
            nodes.push_back(*start_node);
            for (std::size_t position = block_start+1; position <= block_end; ++position) {
                const auto& start_event = layer.timbre_automation[tones[position-1]];
                const auto& end_event = layer.timbre_automation[tones[position]];
                const auto& settings = end_event.scc_morph;
                if (end_event.count <= start_event.count)
                    return fail("SCC morph tone events must have strictly increasing counts");
                if (end_event.count > layer.envelope_timeline.length_counts
                    || end_event.count > EnvelopeTimeline::kMaximumLengthCounts)
                    return fail("SCC morph tone event lies outside the envelope timeline");
                const auto duration = end_event.count-start_event.count;
                if (settings.intermediate_count > duration-1)
                    return fail("SCC morph intermediate count exceeds available time slots");
                if (!std::isfinite(settings.curve) || settings.curve < 1.0
                    || settings.curve > kSccMorphGammaMax)
                    return fail("SCC morph curve is out of range");
                const auto end_node = sourceNode(position);
                const auto original_start = sourceNode(position-1);
                if (!end_node || !original_start)
                    return fail("SCC morph source waveform snapshot is unavailable");
                // Preserve the existing rounded/clamped event positions;
                // use these exact intervals for connection velocity scoring.
                std::vector<std::uint32_t> counts{start_event.count};
                const auto divisions = std::uint64_t(settings.intermediate_count)+1;
                for (std::size_t i = 0; i < settings.intermediate_count; ++i) {
                    const auto numerator = std::uint64_t(duration)*(i+1);
                    const auto rounded = start_event.count + static_cast<std::uint32_t>(
                        (numerator+divisions/2)/divisions);
                    const auto latest = end_event.count
                        - static_cast<std::uint32_t>(settings.intermediate_count-i);
                    counts.push_back(std::clamp(rounded,counts.back()+1,latest));
                }
                counts.push_back(end_event.count);
                const PairKey key{original_start->wave,end_node->wave,
                    settings.intermediate_count,settings.curve,duration};
                auto cached = pair_cache.find(key);
                if (cached == pair_cache.end()) {
                    const auto intervals = playbackIntervals(counts);
                    cached = pair_cache.emplace(key,generateSccMorphImpl(original_start->wave,
                        end_node->wave,settings.intermediate_count,settings.curve,nullptr,&intervals)).first;
                }
                const auto& generated = cached->second;
                for (std::size_t i = 0; i < generated.intermediate.size(); ++i) {
                    Node node;
                    node.wave = generated.intermediate[i];
                    node.diagnostic = generated.diagnostics[i];
                    node.diagnostic.count = counts[i+1];
                    node.diagnostic.layer_index = layer_index;
                    nodes.push_back(node);
                }
                nodes.push_back(*end_node);
            }
            std::vector<SccWaveform> waves;
            waves.reserve(nodes.size());
            for (const auto& node : nodes) waves.push_back(node.wave);
            const auto shifts = optimizeSccMorphBlockPhases(waves,
                block_start != 0,block_end+1 != tones.size());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                nodes[i].wave = waves[i];
                nodes[i].diagnostic.phase_shift = shifts[i];
                nodes[i].diagnostic.result = analyzeSccMorphWaveform(waves[i]).descriptors;
                pending.push_back({layer_index,std::move(nodes[i])});
            }
            arrival = block_end+1;
        }
    }
    // Count all byte-distinct derived waves before returning overflow so the
    // UI can show required / available immediately, never partial success.
    std::map<SccWaveform,bool> new_waves;
    for (const auto& item : pending)
        if (!materialized.contains(item.node.wave)) new_waves[item.node.wave] = true;
    result.required = new_waves.size();
    if (result.required > result.capacity)
        return fail("SCC Morph Bank overflow: required " + std::to_string(result.required)
            + ", available " + std::to_string(result.capacity));
    for (auto& item : pending) {
        auto& node = item.node;
        auto allocation = materialized.find(node.wave);
        if (allocation == materialized.end()) {
            std::size_t number = input.scc_morph_bank_base;
            while (number < 32 && occupied[number]) ++number;
            occupied[number] = true;
            SavedTimbreReference reference;
            reference.library_id = allocateCompositeOwnedTimbreId(result.timbre);
            reference.name = "SCC Morph @" + std::to_string(number);
            reference.source = TimbreSource::Scc;
            reference.number_mode = TimbreNumberMode::Manual;
            reference.manual_number = static_cast<std::uint8_t>(number);
            for (std::size_t n = 0; n < 32; ++n)
                reference.scc_waveform[n] = static_cast<std::uint8_t>(node.wave[n]);
            result.timbre.embedded_timbres.push_back(reference);
            allocation = materialized.emplace(node.wave,Materialized{
                reference.library_id,static_cast<std::uint8_t>(number),true}).first;
            ++result.used;
        }
        auto& events = result.timbre.layers[item.layer].timbre_automation;
        const auto& assigned = allocation->second;
        if (node.event_index) {
            // Byte-identical source nodes retain their existing snapshot ID.
            // Only an actually phase-shifted source needs a derived reference.
            auto& event = events[*node.event_index];
            const auto* original = sourceForEvent(input.layers[item.layer].timbre_automation[*node.event_index]);
            if (original && sourceWave(*original) == node.wave) {
                event.target_library_id = original->library_id;
                event.value = *assignedNumberForLibraryId(source_numbers,original->library_id);
            } else {
                event.target_library_id = assigned.id;
                event.value = assigned.number;
            }
            event.timbre_pick = TimbrePick::Library;
            node.diagnostic.output_number = static_cast<std::uint8_t>(event.value);
        } else {
            EnvelopeEvent event;
            event.kind = EnvelopeEventKind::Timbre;
            event.count = node.diagnostic.count;
            event.value = assigned.number;
            event.target_library_id = assigned.id;
            event.timbre_pick = TimbrePick::Library;
            const auto& timeline = input.layers[item.layer].envelope_timeline;
            event.after_loop_start =
                (timeline.loop_start_count && event.count == *timeline.loop_start_count)
                || (timeline.loop_end_count && event.count == *timeline.loop_end_count);
            events.push_back(event);
            node.diagnostic.output_number = assigned.number;
        }
        node.diagnostic.generated = assigned.derived;
        result.diagnostics.push_back(node.diagnostic);
    }
    for (auto& layer : result.timbre.layers) {
        if (layer.volume_envelope.kind != EnvelopeKind::Sequence) continue;
        for (auto& event : layer.timbre_automation) event.scc_morph.enabled = false;
        // Stable order preserves original same-count commands; inserted tone
        // events occupy strictly interior slots of their transition.
        std::stable_sort(layer.timbre_automation.begin(),layer.timbre_automation.end(),
            [](const EnvelopeEvent& a,const EnvelopeEvent& b) { return a.count < b.count; });
    }
    const auto final_numbers = resolveTimbreNumbers(result.timbre);
    if (!final_numbers.valid())
        return fail("SCC morph output allocation: " + final_numbers.warnings.front());
    return result;
}

std::shared_ptr<const SccMorphCompileResult> compileSccMorphCached(
    const CompositeTimbre& input) {
    struct Cache {
        CompositeTimbre input;
        std::uint32_t algorithm_version{};
        std::shared_ptr<const SccMorphCompileResult> result;
    };
    thread_local Cache cache;
    if (!cache.result || cache.input != input
        || cache.algorithm_version != kSccMorphAlgorithmVersion) {
        auto result = std::make_shared<const SccMorphCompileResult>(compileSccMorph(input));
        cache.input = input;
        cache.algorithm_version = kSccMorphAlgorithmVersion;
        cache.result = std::move(result);
    }
    return cache.result;
}

} // namespace mgstc::engine
