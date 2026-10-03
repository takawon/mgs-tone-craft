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
    gamma = std::clamp(gamma, kSccMorphGammaMin, kSccMorphGammaMax);
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

struct MorphCancelled {};
void checkCancellation(std::stop_token token) {
    if (token.stop_requested()) throw MorphCancelled{};
}

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
    SccMorphPairTrace* trace, const std::vector<double>* intervals = nullptr,
    const SccMorphPlan* plan = nullptr, std::stop_token cancellation = {}) {
    SccMorphPairResult result;
    checkCancellation(cancellation);
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
        checkCancellation(cancellation);
        const double u = plan ? plan->points[index].morph_position
            : morphPosition(double(index) / (double(count)+1.0), gamma);
        Target target{};
        target.descriptors = interpolate(a.descriptors,b.descriptors,u);
        for (std::size_t n = 0; n < 32; ++n)
            target.shape[n] = (1-u)*fa[n] + u*fb[n];
        auto* step = trace ? &trace->steps[index-1] : nullptr;
        if (step) {
            step->t = plan ? double(plan->points[index].event_count)/plan->points.back().event_count
                : double(index) / (double(count)+1.0);
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
        return plan ? plan->points[index].morph_position
            : morphPosition(double(index)/(double(count)+1.0),gamma);
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
            checkCancellation(cancellation);
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
                checkCancellation(cancellation);
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
        diagnostic.u = position(index+1);
        if (plan) diagnostic.count = plan->points[index+1].event_count;
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


// Integer-placement search is bounded independently of timeline duration. For
// short timelines it examines every feasible count; otherwise each node has
// at most 84 ordered candidates (target-neighborhood, uniform and boundaries).
constexpr std::size_t kPlanReferenceIntervals = 64;
constexpr std::size_t kPlanMaximumEvaluations = 6;
constexpr std::size_t kPlanHoldSamples = 256;

SccMorphPlan tonePlan(std::uint8_t count, double gamma, std::uint32_t duration) {
    SccMorphPlan plan;
    plan.points.push_back({0.0,0});
    const auto divisions = std::uint64_t(count)+1;
    for (std::size_t i = 0; i < count; ++i) {
        const auto rounded = static_cast<std::uint32_t>(
            (std::uint64_t(duration)*(i+1)+divisions/2)/divisions);
        const auto latest = duration-static_cast<std::uint32_t>(count-i);
        plan.points.push_back({morphPosition(double(i+1)/double(divisions),gamma),
            std::clamp(rounded,plan.points.back().event_count+1,latest)});
    }
    plan.points.push_back({1.0,duration});
    for (std::size_t i = plan.points.size()-1; i-- > 1;)
        if (plan.points[i].morph_position >= plan.points[i+1].morph_position)
            plan.points[i].morph_position = std::nextafter(plan.points[i+1].morph_position,0.0);
    return plan;
}

double inversePosition(double u, double gamma) {
    if (u <= 0.0) return 0.0;
    if (u >= 1.0) return 1.0;
    return -std::expm1(std::log1p(-u)/gamma);
}

// The public legacy wrapper preserves its original floating math. New plans
// stabilize unrepresentable gamma tails by minimal adjacent double steps.
bool validGeneratedPlan(const SccMorphPlan& plan) {
    if (plan.points.size() < 2 || plan.points.size() > 257
        || plan.points.front() != SccMorphPoint{0.0,0}
        || plan.points.back().morph_position != 1.0) return false;
    for (std::size_t i = 1; i < plan.points.size(); ++i)
        if (!std::isfinite(plan.points[i].morph_position)
            || plan.points[i].morph_position <= plan.points[i-1].morph_position
            || plan.points[i].morph_position > 1.0
            || plan.points[i].event_count <= plan.points[i-1].event_count) return false;
    return true;
}

std::vector<std::uint32_t> planCounts(const SccMorphPlan& plan) {
    std::vector<std::uint32_t> counts;
    counts.reserve(plan.points.size());
    for (const auto& point : plan.points) counts.push_back(point.event_count);
    return counts;
}

SccMorphPairResult generatePlan(const SccWaveform& first, const SccWaveform& second,
    const SccMorphPlan& plan, SccMorphPairTrace* trace, std::stop_token cancellation) {
    const auto counts = planCounts(plan);
    const auto intervals = playbackIntervals(counts);
    return generateSccMorphImpl(first,second,
        static_cast<std::uint8_t>(plan.points.size()-2),1.0,trace,&intervals,&plan,cancellation);
}

SccMorphPlan integerPlan(const std::vector<double>& positions, double gamma,
    std::uint32_t duration, bool centered, std::stop_token cancellation) {
    const std::size_t count = positions.size()-2;
    if (count == 0) return {{{0.0,0},{1.0,duration}}};
    const auto uniform = tonePlan(static_cast<std::uint8_t>(count),1.0,duration);
    std::vector<std::vector<std::uint32_t>> candidates(count);
    std::vector<std::vector<std::size_t>> back(count);
    std::vector<double> previous;
    for (std::size_t i = 0; i < count; ++i) {
        checkCancellation(cancellation);
        const std::uint32_t first = static_cast<std::uint32_t>(i+1);
        const std::uint32_t last = duration-static_cast<std::uint32_t>(count-i);
        const double target = centered ? (positions[i]+positions[i+1])/2.0 : positions[i+1];
        const double ideal = inversePosition(target,gamma)*duration;
        auto& choices = candidates[i];
        if (duration <= 512) {
            for (auto c = first; c <= last; ++c) choices.push_back(c);
        } else {
            const auto add = [&](std::int64_t c) {
                choices.push_back(static_cast<std::uint32_t>(std::clamp<std::int64_t>(c,first,last)));
            };
            const auto rounded = static_cast<std::int64_t>(std::floor(ideal+0.5));
            for (int offset = -16; offset <= 16; ++offset) add(rounded+offset);
            for (std::size_t grid = 0; grid <= 32; ++grid)
                add(first+std::uint64_t(last-first)*grid/32);
            add(uniform.points[i+1].event_count); add(first); add(last);
            std::sort(choices.begin(),choices.end());
            choices.erase(std::unique(choices.begin(),choices.end()),choices.end());
        }
        std::vector<double> current(choices.size(),std::numeric_limits<double>::infinity());
        back[i].resize(choices.size());
        std::size_t cursor{}, best_predecessor{};
        double best_prefix = std::numeric_limits<double>::infinity();
        for (std::size_t j = 0; j < choices.size(); ++j) {
            if (i) {
                const auto& before = candidates[i-1];
                while (cursor < before.size() && before[cursor] < choices[j]) {
                    // Later count wins exact ties, matching half-up legacy
                    // rounding for a uniform target when there is no collision.
                    if (previous[cursor] <= best_prefix) {
                        best_prefix = previous[cursor]; best_predecessor = cursor;
                    }
                    ++cursor;
                }
            } else best_prefix = 0.0;
            const double deviation = morphPosition(double(choices[j])/duration,gamma)-target;
            current[j] = best_prefix+deviation*deviation;
            back[i][j] = best_predecessor;
        }
        previous = std::move(current);
    }
    std::size_t selected{};
    for (std::size_t j = 1; j < previous.size(); ++j)
        if (previous[j] <= previous[selected]) selected = j;
    SccMorphPlan plan;
    plan.points.resize(count+2);
    plan.points.front() = {0.0,0}; plan.points.back() = {1.0,duration};
    for (std::size_t i = count; i-- > 0;) {
        plan.points[i+1] = {positions[i+1],candidates[i][selected]};
        selected = back[i][selected];
    }
    return plan;
}

double featureDistance(const SccMorphAnalysis& a, const SccMorphAnalysis& b) {
    double harmonic{};
    for (std::size_t k = 0; k <= 16; ++k) {
        const double d = a.magnitude[k]-b.magnitude[k];
        harmonic += (k == 0 || k == 16 ? 1.0 : 2.0)*d*d;
    }
    return 0.6*harmonic + 0.3*std::pow(a.descriptors.rms-b.descriptors.rms,2)
        + 0.1*descriptorDistance(a.descriptors,b.descriptors)/Config::descriptor_weight_sum;
}

struct PlanReference {
    std::vector<SccWaveform> waves;
    std::vector<SccMorphAnalysis> analyses;
    std::vector<double> arc;
    double scale{}, travel{};
};

PlanReference makeReference(const SccWaveform& first, const SccWaveform& second,
    std::uint8_t count, std::stop_token cancellation) {
    PlanReference reference;
    const auto divisions = std::min(kPlanReferenceIntervals,
        std::max<std::size_t>(16,2*(std::size_t(count)+1)));
    reference.waves.push_back(first);
    const auto generated = generateSccMorphImpl(first,second,
        static_cast<std::uint8_t>(divisions-1),1.0,nullptr,nullptr,nullptr,cancellation);
    reference.waves.insert(reference.waves.end(),generated.intermediate.begin(),generated.intermediate.end());
    reference.waves.push_back(shifted(second,align(first,second)));
    for (const auto& wave : reference.waves) reference.analyses.push_back(analyzeSccMorphWaveform(wave));
    reference.arc.push_back(0.0);
    for (std::size_t i = 1; i < reference.analyses.size(); ++i) {
        const Candidate a{reference.waves[i-1],reference.analyses[i-1],SccMorphCandidate::Source,0,0};
        const Candidate b{reference.waves[i],reference.analyses[i],SccMorphCandidate::Source,0,0};
        SccMorphConnectionTrace edge;
        (void)connection(a,b,1.0,1.0,reference.analyses.front().descriptors,
            reference.analyses.back().descriptors,&edge);
        reference.arc.push_back(reference.arc.back()+std::sqrt(edge.distance));
        double spectral{};
        for (std::size_t k = 1; k <= 16; ++k)
            spectral += std::pow(reference.analyses[i].magnitude[k]-reference.analyses[i-1].magnitude[k],2);
        reference.travel += std::sqrt(spectral);
        reference.scale = std::max(reference.scale,featureDistance(reference.analyses.front(),reference.analyses[i]));
    }
    reference.scale = std::max(reference.scale,1.0/(128.0*128.0));
    return reference;
}

SccMorphAnalysis referenceAt(const PlanReference& reference, double u) {
    const double index = std::clamp(u,0.0,1.0)*(reference.analyses.size()-1);
    const auto left = std::min(static_cast<std::size_t>(index),reference.analyses.size()-2);
    const double mix = index-left;
    SccMorphAnalysis result;
    result.descriptors = interpolate(reference.analyses[left].descriptors,
        reference.analyses[left+1].descriptors,mix);
    for (std::size_t k = 0; k <= 16; ++k)
        result.magnitude[k] = (1-mix)*reference.analyses[left].magnitude[k]
            + mix*reference.analyses[left+1].magnitude[k];
    return result;
}

SccMorphPlanEvaluation evaluatePlan(const SccMorphPlan& plan,
    const SccMorphPairResult& generated, const PlanReference& reference,
    double gamma, std::stop_token cancellation) {
    SccMorphPlanEvaluation score;
    std::vector<SccWaveform> waves{reference.waves.front()};
    waves.insert(waves.end(),generated.intermediate.begin(),generated.intermediate.end());
    waves.push_back(reference.waves.back());
    std::vector<SccMorphAnalysis> analyses;
    for (const auto& wave : waves) analyses.push_back(analyzeSccMorphWaveform(wave));
    const auto duration = plan.points.back().event_count;
    // Exact count integration for <=256 counts; larger durations use 256
    // equal-width cells subdivided at every actual event boundary. Therefore
    // even one-count holds receive their exact duration, with bounded samples.
    const auto samples = std::min<std::uint32_t>(duration,kPlanHoldSamples);
    for (std::size_t i = 0; i+1 < waves.size(); ++i) {
        checkCancellation(cancellation);
        const double begin = double(plan.points[i].event_count)/duration;
        const double end = double(plan.points[i+1].event_count)/duration;
        double t = begin;
        while (t < end) {
            const double next = std::min(end,(std::floor(t*samples+1e-9)+1)/samples);
            const double midpoint = (t+next)/2;
            score.curve += (next-t)*featureDistance(analyses[i],referenceAt(reference,morphPosition(midpoint,gamma)));
            t = next;
        }
        const double expected = inversePosition(plan.points[i+1].morph_position,gamma)
            - inversePosition(plan.points[i].morph_position,gamma);
        const double deviation = ((end-begin)-expected)/((end-begin)+expected+1.0/duration);
        score.concentration += (end-begin)*deviation*deviation;
        score.fidelity += (end-begin)*featureDistance(analyses[i],referenceAt(reference,plan.points[i].morph_position));
        const Candidate a{waves[i],analyses[i],SccMorphCandidate::Source,0,0};
        const Candidate b{waves[i+1],analyses[i+1],SccMorphCandidate::Source,0,0};
        SccMorphConnectionTrace edge;
        (void)connection(a,b,end-begin,
            plan.points[i+1].morph_position-plan.points[i].morph_position,
            reference.analyses.front().descriptors,reference.analyses.back().descriptors,&edge);
        score.transition += edge.distance;
        score.max_wave_gap = std::max(score.max_wave_gap,std::sqrt(edge.phase_wave*4.0)*128.0);
        double spectral{};
        for (std::size_t k = 1; k <= 16; ++k)
            spectral += std::pow(analyses[i+1].magnitude[k]-analyses[i].magnitude[k],2);
        score.max_spectral_gap = std::max(score.max_spectral_gap,std::sqrt(spectral));
        score.spectral_travel += std::sqrt(spectral);
    }
    score.curve /= reference.scale;
    score.transition /= reference.scale;
    score.fidelity /= reference.scale;
    // Preserve the whole path as well as endpoints. Fewer spectral changes
    // cannot improve the score merely by discarding the intended trajectory.
    if (reference.travel > 1.0/128.0)
        score.fidelity += std::pow(score.spectral_travel/reference.travel-1.0,2);
    // Fixture sweeps (N=8/27, gamma=1 and 8) found 0.15 could accept a
    // 122% larger worst jump for only 1.83% improvement in total score.
    // 0.30 favors the tested Time fallback there, improves gamma=1 gaps,
    // and retains N=8 selections. Fidelity/hold concentration stay bounded.
    score.total = score.curve+0.30*score.transition+0.25*score.fidelity+0.025*score.concentration;
    return score;
}

std::vector<double> strictPositions(std::vector<double> positions) {
    // Only newly searched adaptive points are stabilized. Legacy Tone math
    // and public wrapper outputs must not be changed by numerical cleanup.
    double last = 1.0;
    for (std::size_t i = positions.size()-1; i-- > 1;) {
        positions[i] = std::min(positions[i],std::nextafter(last,0.0));
        last = positions[i];
    }
    last = 0.0;
    for (std::size_t i = 1; i+1 < positions.size(); ++i) {
        positions[i] = std::max(positions[i],std::nextafter(last,1.0));
        last = positions[i];
    }
    return positions;
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


SccMorphPairResult generateSccMorphPlanned(const SccWaveform& first,
    const SccWaveform& second, const SccMorphPlan& plan, SccMorphPairTrace* trace,
    std::stop_token cancellation) {
    if (!validGeneratedPlan(plan))
        throw std::invalid_argument("SCC morph plan must include ordered positions/counts and both endpoints");
    try { return generatePlan(first,second,plan,trace,cancellation); }
    catch (const MorphCancelled&) { throw std::runtime_error("SCC morph generation cancelled"); }
}

SccMorphPlanResult planSccMorph(const SccWaveform& first, const SccWaveform& second,
    std::uint8_t count, double gamma, std::uint32_t duration, SccMorphDistributionMode mode,
    const std::optional<SccMorphPlan>& explicit_plan, std::stop_token cancellation, bool evaluate) {
    SccMorphPlanResult result;
    const auto fail = [&](const char* error) {
        result.valid = false; result.error = error; return result;
    };
    if (!isValidSccMorphDistributionMode(mode)) return fail("SCC morph distribution mode is invalid");
    if (duration < std::uint32_t(count)+1 || duration > EnvelopeTimeline::kMaximumLengthCounts)
        return fail("SCC morph intermediate count exceeds available time slots");
    if (!std::isfinite(gamma) || gamma < kSccMorphGammaMin || gamma > kSccMorphGammaMax)
        return fail("SCC morph curve is out of range");
    if (explicit_plan && (!isValidSccMorphPlan(*explicit_plan,count)
        || explicit_plan->points.back().event_count != duration))
        return fail("SCC morph explicit plan does not match segment duration/count");
    try {
        checkCancellation(cancellation);
        if (!evaluate && mode != SccMorphDistributionMode::AdaptiveDistribution) {
            result.plan = explicit_plan ? *explicit_plan : tonePlan(count,gamma,duration);
            if (!explicit_plan && mode == SccMorphDistributionMode::TimeDistribution && gamma != 1.0) {
                std::vector<double> positions(count+2);
                for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = double(i)/(count+1.0);
                result.plan = integerPlan(positions,gamma,duration,false,cancellation);
            }
            result.generated = generatePlan(first,second,result.plan,nullptr,cancellation);
            result.selected_distribution = mode;
            result.evaluated_plans = 1;
            result.selection_reason = explicit_plan ? "Confirmed external plan retained"
                : "Formal distribution plan; quality assessment not requested";
            result.working_set_bytes = std::size_t(count)*(sizeof(Target)+12*sizeof(Candidate)
                +3*sizeof(Harmonics)+12*(sizeof(double)+sizeof(std::size_t)));
            return result;
        }
        const auto reference = makeReference(first,second,count,cancellation);
        result.reference_points = reference.waves.size();
        result.evaluation_available = true;
        // Logical owned-data upper estimate (no diagnostic traces): reference
        // + one generator's 12 Candidate/Target rows + DP + bounded count rows.
        result.working_set_bytes = reference.waves.size()*(sizeof(SccWaveform)+sizeof(SccMorphAnalysis)+sizeof(double))
            + std::size_t(count)*(sizeof(Target)+12*sizeof(Candidate)+3*sizeof(Harmonics)+12*(sizeof(double)+sizeof(std::size_t)))
            + std::size_t(count)*512*(sizeof(std::uint32_t)+sizeof(std::size_t)+sizeof(double));
        const auto consider = [&](const SccMorphPlan& plan, SccMorphDistributionMode candidate_mode,
                                  const char* reason) {
            checkCancellation(cancellation);
            auto generated = generatePlan(first,second,plan,nullptr,cancellation);
            const auto score = evaluatePlan(plan,generated,reference,gamma,cancellation);
            ++result.evaluated_plans;
            result.considered.push_back({plan,score,candidate_mode,reason});
            if (result.evaluated_plans == 1 || score.total+Config::tolerance < result.evaluation.total) {
                result.plan = plan; result.generated = std::move(generated);
                result.evaluation = score; result.selected_distribution = candidate_mode;
                result.selection_reason = reason;
            }
        };
        if (explicit_plan) {
            consider(*explicit_plan,mode,"Confirmed external plan retained");
            return result;
        }
        const auto tone = tonePlan(count,gamma,duration);
        std::vector<double> uniform(count+2);
        for (std::size_t i = 0; i < uniform.size(); ++i) uniform[i] = double(i)/(count+1.0);
        const auto time = gamma == 1.0 ? tone : integerPlan(uniform,gamma,duration,false,cancellation);
        if (mode == SccMorphDistributionMode::ToneDistribution) {
            consider(tone,mode,"Legacy tone distribution retained"); return result;
        }
        if (mode == SccMorphDistributionMode::TimeDistribution) {
            consider(time,mode,"Uniform positions with bounded integer timing optimization"); return result;
        }
        consider(tone,SccMorphDistributionMode::ToneDistribution,"Tone baseline minimizes common score");
        if (time != tone)
            consider(time,SccMorphDistributionMode::TimeDistribution,"Time baseline minimizes common score");
        std::vector<double> acoustic(count+2);
        acoustic.back() = 1.0;
        for (std::size_t i = 1; i <= count; ++i) {
            const double target = reference.arc.back()*double(i)/(count+1.0);
            if (reference.arc.back() <= Config::tolerance) acoustic[i] = uniform[i];
            else {
                const auto found = std::lower_bound(reference.arc.begin(),reference.arc.end(),target);
                const auto right = std::clamp<std::size_t>(found-reference.arc.begin(),1,reference.arc.size()-1);
                const double mix = (target-reference.arc[right-1])
                    /std::max(reference.arc[right]-reference.arc[right-1],Config::tolerance);
                acoustic[i] = (double(right-1)+mix)/(reference.arc.size()-1);
            }
        }
        acoustic = strictPositions(std::move(acoustic));
        consider(integerPlan(acoustic,gamma,duration,true,cancellation),mode,
            "Acoustic position and hold-centered count search");
        std::vector<double> tone_positions;
        for (const auto& point : tone.points) tone_positions.push_back(point.morph_position);
        tone_positions = strictPositions(std::move(tone_positions));
        consider(integerPlan(tone_positions,gamma,duration,true,cancellation),mode,
            "Curve positions with hold-centered count search");
        for (std::size_t i = 1; i <= count; ++i) acoustic[i] = (acoustic[i]+tone_positions[i])/2.0;
        consider(integerPlan(strictPositions(std::move(acoustic)),gamma,duration,true,cancellation),mode,
            "Mixed acoustic/curve positions with hold-centered count search");
        if (count && result.evaluated_plans < kPlanMaximumEvaluations) {
            std::vector<double> refined{0.0};
            for (std::size_t i = 1; i <= count; ++i) {
                const double begin = double(result.plan.points[i].event_count)/duration;
                const double end = double(result.plan.points[i+1].event_count)/duration;
                refined.push_back((result.plan.points[i].morph_position+morphPosition((begin+end)/2,gamma))/2.0);
            }
            refined.push_back(1.0);
            consider(integerPlan(strictPositions(std::move(refined)),gamma,duration,true,cancellation),mode,
                "One bounded joint position/count refinement");
        }
        result.fallback = result.selected_distribution != mode;
        return result;
    } catch (const MorphCancelled&) { return fail("SCC morph generation cancelled"); }
}

namespace {
std::vector<std::size_t> optimizeBlockPhases(
    std::vector<SccWaveform>& waves, bool fixed_start, bool fixed_end,
    std::stop_token cancellation) {
    std::vector<std::size_t> shifts(waves.size());
    if (waves.empty()) return shifts;
    std::vector<std::array<std::size_t,32>> back(waves.size());
    std::array<double,32> previous{}, current{};
    for (std::size_t shift = 0; shift < 32; ++shift)
        previous[shift] = fixed_start && shift != 0
            ? std::numeric_limits<double>::infinity() : 0.0;
    for (std::size_t index = 1; index < waves.size(); ++index) {
        checkCancellation(cancellation);
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

} // namespace

std::vector<std::size_t> optimizeSccMorphBlockPhases(
    std::vector<SccWaveform>& waves, bool fixed_start, bool fixed_end) {
    return optimizeBlockPhases(waves,fixed_start,fixed_end,{});
}

SccMorphCompileResult compileSccMorph(const CompositeTimbre& input,
    std::stop_token cancellation) {
    SccMorphCompileResult result;
    result.authored_input = std::make_shared<const CompositeTimbre>(input);
    result.timbre = input;
    if (cancellation.stop_requested()) {
        result.valid = false; result.cancelled = true;
        result.error = "SCC morph generation cancelled"; return result;
    }
    const auto contiguous = [](const CompositeLayer& layer) {
        return layer.source == TimbreSource::Scc
            && layer.scc_output_allocation == SccOutputAllocationMode::Contiguous;
    };
    const bool channel_allocation = std::any_of(input.layers.begin(), input.layers.end(), contiguous);
    const bool active = std::any_of(input.layers.begin(),input.layers.end(),
        [](const CompositeLayer& layer) {
            return layer.volume_envelope.kind == EnvelopeKind::Sequence
                && std::any_of(layer.timbre_automation.begin(),layer.timbre_automation.end(),
                [](const EnvelopeEvent& event) { return event.scc_morph.enabled; });
        });
    if ((!active && !channel_allocation) || input.scc_morph_materialized) {
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
        for (std::size_t number = input.scc_morph_bank_base;
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
    if (input.scc_morph_bank_base > 31)
        return fail("SCC Morph Bank start must be @0 through @31");
    if (!isSupportedSccMorphAlgorithmVersion(input.scc_morph_algorithm_version))
        return fail("Unsupported SCC morph algorithm version");
    auto legacy_input = input;
    for (auto& layer : legacy_input.layers) {
        if (contiguous(layer)) {
            // Authored source @ numbers are not output reservations for an
            // explicitly relocated channel. Keep other channels unchanged.
            layer.base_timbre.reset();
            layer.timbre_automation.clear();
        }
    }
    const auto source_numbers = resolveTimbreNumbers(legacy_input);
    const auto original_numbers = channel_allocation ? resolveTimbreNumbers(input) : source_numbers;
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
        if (layer.source != TimbreSource::Scc || contiguous(layer)) continue;
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
        for (const auto& assignment : original_numbers.assignments)
            if (assignment.number == event.value) {
                const auto* reference = findEmbeddedTimbreSnapshot(input,assignment.library_id);
                if (reference && reference->source == TimbreSource::Scc) return reference;
            }
        for (const auto& assignment : original_numbers.assignments) {
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
            if (const auto number = assignedNumberForLibraryId(source_numbers,reference->library_id))
                event.value = *number;
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
    using PlanKey = std::vector<std::pair<double,std::uint32_t>>;
    using PairKey = std::tuple<SccWaveform,SccWaveform,std::uint8_t,double,std::uint32_t,
        SccMorphDistributionMode,PlanKey>;
    std::map<PairKey,SccMorphPlanResult> pair_cache;
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
            if (cancellation.stop_requested()) {
                result.cancelled = true; return fail("SCC morph generation cancelled");
            }
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
                if (!contiguous(layer)
                    && !assignedNumberForLibraryId(source_numbers,source->library_id)) return std::nullopt;
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
                if (!std::isfinite(settings.curve) || settings.curve < kSccMorphGammaMin
                    || settings.curve > kSccMorphGammaMax)
                    return fail("SCC morph curve is out of range");
                const auto end_node = sourceNode(position);
                const auto original_start = sourceNode(position-1);
                if (!end_node || !original_start)
                    return fail("SCC morph source waveform snapshot is unavailable");
                if (!isValidSccMorphDistributionMode(settings.distribution_mode))
                    return fail("SCC morph distribution mode is invalid");
                if (settings.explicit_plan
                    && (!isValidSccMorphPlan(*settings.explicit_plan,settings.intermediate_count)
                        || settings.explicit_plan->points.back().event_count != duration))
                    return fail("SCC morph explicit plan does not match segment duration/count");
                PlanKey explicit_key;
                if (settings.explicit_plan)
                    for (const auto& point : settings.explicit_plan->points)
                        explicit_key.emplace_back(point.morph_position,point.event_count);
                const PairKey key{original_start->wave,end_node->wave,
                    settings.intermediate_count,settings.curve,duration,
                    settings.distribution_mode,std::move(explicit_key)};
                auto cached = pair_cache.find(key);
                if (cached == pair_cache.end()) {
                    auto planned = planSccMorph(original_start->wave,end_node->wave,
                        settings.intermediate_count,settings.curve,duration,
                        settings.distribution_mode,settings.explicit_plan,cancellation,false);
                    if (!planned.valid) {
                        result.cancelled = cancellation.stop_requested();
                        return fail(planned.error);
                    }
                    cached = pair_cache.emplace(key,std::move(planned)).first;
                }
                const auto& planned = cached->second;
                result.plans.push_back({layer_index,tones[position],planned});
                const auto& generated = planned.generated;
                for (std::size_t i = 0; i < generated.intermediate.size(); ++i) {
                    Node node;
                    node.wave = generated.intermediate[i];
                    node.diagnostic = generated.diagnostics[i];
                    node.diagnostic.count = start_event.count+planned.plan.points[i+1].event_count;
                    node.diagnostic.layer_index = layer_index;
                    nodes.push_back(node);
                }
                nodes.push_back(*end_node);
            }
            std::vector<SccWaveform> waves;
            waves.reserve(nodes.size());
            for (const auto& node : nodes) waves.push_back(node.wave);
            std::vector<std::size_t> shifts;
            try {
                shifts = optimizeBlockPhases(waves,block_start != 0,
                    block_end+1 != tones.size(),cancellation);
            } catch (const MorphCancelled&) {
                result.cancelled = true; return fail("SCC morph generation cancelled");
            }
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
        if (!contiguous(input.layers[item.layer]) && !materialized.contains(item.node.wave))
            new_waves[item.node.wave] = true;
    result.required = new_waves.size();
    if (result.required > result.capacity)
        return fail("SCC Morph Bank overflow: required " + std::to_string(result.required)
            + ", available " + std::to_string(result.capacity));
    for (auto& item : pending) {
        if (contiguous(input.layers[item.layer])) continue;
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
    if (channel_allocation) {
        struct Group {
            std::size_t layer{};
            std::vector<SccWaveform> waves;
            std::map<SccWaveform, std::size_t> offsets;
            std::map<std::size_t, SccWaveform> event_waves;
            std::optional<std::uint8_t> start;
        };
        std::vector<Group> groups;
        for (std::size_t layer_index = 0; layer_index < input.layers.size(); ++layer_index) {
            const auto& layer = input.layers[layer_index];
            if (!contiguous(layer)) continue;
            if (layer.scc_output_start && *layer.scc_output_start > 31)
                return fail("SCC channel output start must be @0 through @31");
            Group group;
            group.layer = layer_index;
            const auto add_wave = [&](const SccWaveform& wave) {
                if (!group.offsets.contains(wave)) {
                    group.offsets.emplace(wave, group.waves.size());
                    group.waves.push_back(wave);
                }
            };
            if (layer.base_timbre && layer.base_timbre->source != TimbreSource::Scc)
                return fail("SCC channel base snapshot has a different sound source");
            add_wave(layer.base_timbre ? sourceWave(*layer.base_timbre)
                : generateSccPreset(SccWavePreset::Sine, SccHarmonic::One));
            // Final phase-adjusted endpoints replace their authored event
            // waveform. Do not reserve discarded endpoint phase variants.
            for (const auto& item : pending) {
                if (item.layer != layer_index) continue;
                if (item.node.event_index)
                    group.event_waves[*item.node.event_index] = item.node.wave;
            }
            if (layer.volume_envelope.kind == EnvelopeKind::Sequence) {
                for (std::size_t index = 0; index < layer.timbre_automation.size(); ++index) {
                    const auto& event = layer.timbre_automation[index];
                    if (event.kind != EnvelopeEventKind::Timbre) continue;
                    if (!group.event_waves.contains(index)) {
                        const auto* reference = sourceForEvent(event);
                        if (!reference || reference->source != TimbreSource::Scc)
                            return fail("SCC channel tone waveform snapshot is unavailable");
                        group.event_waves.emplace(index, sourceWave(*reference));
                    }
                    add_wave(group.event_waves.at(index));
                }
            }
            for (const auto& item : pending)
                if (item.layer == layer_index) add_wave(item.node.wave);
            if (group.waves.size() > 32)
                return fail("SCC channel needs more than 32 output waveforms");
            groups.push_back(std::move(group));
        }
        const auto legacy_used = static_cast<std::size_t>(
            std::count(occupied.begin(), occupied.end(), true));
        result.required = legacy_used;
        for (const auto& group : groups) result.required += group.waves.size();
        result.capacity = 32;
        if (result.required > 32)
            return fail("SCC global output capacity exceeded: required "
                + std::to_string(result.required) + ", available 32");
        const auto fits = [&](std::size_t start, std::size_t count) {
            if (start + count > 32) return false;
            for (std::size_t n = start; n < start + count; ++n)
                if (occupied[n]) return false;
            return true;
        };
        const auto reserve = [&](std::size_t start, std::size_t count) {
            for (std::size_t n = start; n < start + count; ++n) occupied[n] = true;
        };
        // A manual range never silently moves. Reserve every manual request
        // before first-fit automatic channels, irrespective of layer order.
        for (auto& group : groups) {
            const auto requested = input.layers[group.layer].scc_output_start;
            if (!requested) continue;
            if (!fits(*requested, group.waves.size()))
                return fail("SCC manual channel range conflicts or exceeds @31 (start @"
                    + std::to_string(*requested) + ")");
            group.start = requested;
            reserve(*requested, group.waves.size());
        }
        for (auto& group : groups) {
            if (group.start) continue;
            for (std::size_t start = 0; start < 32; ++start) {
                if (!fits(start, group.waves.size())) continue;
                group.start = static_cast<std::uint8_t>(start);
                reserve(start, group.waves.size());
                break;
            }
            if (!group.start)
                return fail("SCC automatic allocation has no contiguous range of "
                    + std::to_string(group.waves.size()) + " waveforms");
        }
        for (const auto& group : groups) {
            std::vector<SavedTimbreReference> references;
            for (std::size_t offset = 0; offset < group.waves.size(); ++offset) {
                SavedTimbreReference reference;
                reference.library_id = allocateCompositeOwnedTimbreId(result.timbre);
                reference.source = TimbreSource::Scc;
                reference.number_mode = TimbreNumberMode::Manual;
                reference.manual_number = static_cast<std::uint8_t>(*group.start + offset);
                reference.name = "SCC channel @" + std::to_string(*reference.manual_number);
                for (std::size_t n = 0; n < 32; ++n)
                    reference.scc_waveform[n] = static_cast<std::uint8_t>(group.waves[offset][n]);
                result.timbre.embedded_timbres.push_back(reference);
                references.push_back(std::move(reference));
            }
            auto& layer = result.timbre.layers[group.layer];
            layer.base_timbre = references.front();
            for (const auto& [index, wave] : group.event_waves) {
                const auto& reference = references[group.offsets.at(wave)];
                auto& event = layer.timbre_automation[index];
                event.target_library_id = reference.library_id;
                event.value = *reference.manual_number;
                event.timbre_pick = TimbrePick::Library;
            }
            for (auto& item : pending) {
                if (item.layer != group.layer) continue;
                const auto& reference = references[group.offsets.at(item.node.wave)];
                auto& diagnostic = item.node.diagnostic;
                if (!item.node.event_index) {
                    EnvelopeEvent event;
                    event.kind = EnvelopeEventKind::Timbre;
                    event.count = diagnostic.count;
                    event.target_library_id = reference.library_id;
                    event.value = *reference.manual_number;
                    const auto& timeline = input.layers[group.layer].envelope_timeline;
                    event.after_loop_start =
                        (timeline.loop_start_count && event.count == *timeline.loop_start_count)
                        || (timeline.loop_end_count && event.count == *timeline.loop_end_count);
                    layer.timbre_automation.push_back(event);
                }
                diagnostic.output_number = *reference.manual_number;
                diagnostic.generated = !item.node.event_index || diagnostic.phase_shift != 0;
                result.diagnostics.push_back(diagnostic);
            }
            result.channel_allocations.push_back({group.layer, *group.start,
                static_cast<std::uint8_t>(group.waves.size()),
                input.layers[group.layer].scc_output_start.has_value()});
        }
        result.used = result.required;
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

SccMorphCompileResult compileSccMorph(const CompositeTimbre& input) {
    return compileSccMorph(input,{});
}

namespace {
struct MorphCache {
    CompositeTimbre input;
    std::uint32_t algorithm_version{};
    std::shared_ptr<const SccMorphCompileResult> result;
};
MorphCache& morphCache() {
    thread_local MorphCache cache;
    return cache;
}
} // namespace

bool seedSccMorphCache(const CompositeTimbre& input,
    std::shared_ptr<const SccMorphCompileResult> result) {
    if (!result || result->cancelled || result->algorithm_version != kSccMorphAlgorithmVersion
        || !result->authored_input
        || *result->authored_input != input) return false;
    auto& cache = morphCache();
    cache.input = input;
    cache.algorithm_version = kSccMorphAlgorithmVersion;
    cache.result = std::move(result);
    return true;
}

std::shared_ptr<const SccMorphCompileResult> compileSccMorphCached(
    const CompositeTimbre& input) {
    auto& cache = morphCache();
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
