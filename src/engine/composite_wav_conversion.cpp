// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_wav_conversion.hpp"

#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/wave_import.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <complex>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <set>
#include <thread>

namespace mgstc::engine {
namespace {
constexpr double pi = std::numbers::pi;
constexpr std::size_t rate = CompositeWavRenderOptions::kSampleRate;
constexpr double epsilon = 1e-9;

bool cancelled(const std::atomic_bool* flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

double rms(std::span<const float> samples) {
    double energy{};
    for (float x : samples) energy += static_cast<double>(x) * x;
    return std::sqrt(energy / std::max<std::size_t>(1, samples.size()));
}

void fft(std::vector<std::complex<double>>& data) {
    const auto n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= n; length *= 2) {
        const auto root = std::polar(1.0, -2 * pi / static_cast<double>(length));
        for (std::size_t first = 0; first < n; first += length) {
            std::complex<double> factor{1, 0};
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto a = data[first + j];
                const auto b = data[first + j + length / 2] * factor;
                data[first + j] = a + b;
                data[first + j + length / 2] = a - b;
                factor *= root;
            }
        }
    }
}

std::vector<double> spectrum(std::span<const float> pcm, std::size_t centre,
                             std::size_t window, double normalization) {
    std::vector<std::complex<double>> values(window);
    for (std::size_t i = 0; i < window; ++i) {
        const auto position = static_cast<std::int64_t>(centre)
            + static_cast<std::int64_t>(i) - static_cast<std::int64_t>(window / 2);
        if (position >= 0 && static_cast<std::size_t>(position) < pcm.size()) {
            const double hann = 0.5 - 0.5 * std::cos(2 * pi * i / (window - 1));
            values[i] = pcm[static_cast<std::size_t>(position)] * hann * normalization;
        }
    }
    fft(values);
    std::vector<double> magnitudes(window / 2 + 1);
    for (std::size_t i = 0; i < magnitudes.size(); ++i)
        magnitudes[i] = std::abs(values[i]) * 4.0 / window;
    return magnitudes;
}

double envelopeLoss(std::span<const float> x, std::span<const float> y,
                    std::size_t end, std::size_t window) {
    double error{}, energy{};
    end = std::min({end, x.size(), y.size()});
    for (std::size_t i = 0; i < end; i += window) {
        const auto count = std::min(window, end - i);
        const double a = rms(x.subspan(i, count));
        const double b = rms(y.subspan(i, count));
        error += (a - b) * (a - b);
        energy += a * a;
    }
    return std::sqrt(error / (energy + epsilon));
}

std::uint32_t countAt(const SourceAnalysis& a, std::size_t sample) {
    const double seconds = static_cast<double>(sample - a.selection.begin)
        / a.source->sample_rate;
    return static_cast<std::uint32_t>(std::max(0LL, std::llround(seconds * 60)));
}

std::size_t sampleAt(const SourceAnalysis& a, std::uint32_t count) {
    return std::min(a.selection.end - 1, a.selection.begin
        + static_cast<std::size_t>(std::llround(count * a.source->sample_rate / 60.0)));
}

template <typename Frame>
const Frame& nearest(const std::vector<Frame>& frames, std::size_t sample) {
    auto it = std::lower_bound(frames.begin(), frames.end(), sample,
        [](const Frame& f, std::size_t s) { return f.sample_position < s; });
    if (it == frames.end()) return frames.back();
    if (it != frames.begin() && sample - (it - 1)->sample_position
        < it->sample_position - sample) --it;
    return *it;
}

bool validAnalysis(const SourceAnalysis& a) {
    if (!a.source || !a.source->sample_rate || a.selection.begin >= a.selection.end
        || a.selection.end > a.source->mono_samples.size()
        || !std::isfinite(a.reference_pitch_hz) || a.reference_pitch_hz < 10 || a.reference_pitch_hz > 20000
        || a.harmonic_trajectory.empty() || a.amplitude_envelope.empty()) return false;
    if ((a.selection.end - a.selection.begin) / static_cast<double>(a.source->sample_rate) > 120) return false;
    const auto regionValid = [&](SampleSelection region) {
        return region.begin >= a.selection.begin && region.begin <= region.end && region.end <= a.selection.end;
    };
    if (!regionValid(a.attack_region.selection) || (a.sustain_region && !regionValid(a.sustain_region->selection))) return false;
    const auto framesValid = [&](const auto& frames) {
        std::size_t preceding = a.selection.begin;
        for (const auto& frame : frames) {
            if (frame.sample_position < preceding || frame.sample_position >= a.selection.end) return false;
            preceding = frame.sample_position;
        }
        return true;
    };
    if (!framesValid(a.harmonic_trajectory) || !framesValid(a.amplitude_envelope) || !framesValid(a.pitch_trajectory)) return false;
    for (const auto& frame : a.amplitude_envelope)
        if (!std::isfinite(frame.rms) || !std::isfinite(frame.peak) || frame.rms < 0 || frame.peak < 0) return false;
    for (const auto& frame : a.pitch_trajectory)
        if (!std::isfinite(frame.frequency_hz) || !std::isfinite(frame.confidence)
            || frame.frequency_hz < 0 || frame.frequency_hz > 20000) return false;
    for (const auto& frame : a.harmonic_trajectory) for (const auto& h : frame.harmonics)
        if (!std::isfinite(h.amplitude) || !std::isfinite(h.frequency_hz)
            || !std::isfinite(h.phase_radians) || h.amplitude < 0 || h.harmonic > 128) return false;
    const auto pcm = a.analysisPcm();
    if (pcm.size() < a.selection.end) return false;
    return std::all_of(pcm.begin() + a.selection.begin,
        pcm.begin() + a.selection.end, [](float value) { return std::isfinite(value); });
}

double harmonicWeight(CompositeWavStrategy strategy, std::size_t part,
                      std::size_t order, std::uint32_t tick, std::uint32_t attack) {
    switch (strategy) {
    case CompositeWavStrategy::FundamentalResidual:
        return (part == 0) == (order == 1) ? 1.0 : 0.0;
    case CompositeWavStrategy::LowHigh:
        return (part == 0) == (order <= 4) ? 1.0 : 0.0;
    case CompositeWavStrategy::AttackSustain: {
        const double sustained = std::clamp(static_cast<double>(tick)
            / std::max<std::uint32_t>(1, attack), 0.0, 1.0);
        return part == 0 ? 1.0 - sustained : sustained;
    }
    default: return 0.5;
    }
}

struct WaveShape { SccWaveform wave{}; double rms{}; };

WaveShape waveAt(const SourceAnalysis& analysis, std::uint32_t tick,
                 CompositeWavStrategy strategy, std::optional<std::size_t> part) {
    const auto& frame = nearest(analysis.harmonic_trajectory, sampleAt(analysis, tick));
    const auto attack = countAt(analysis, analysis.attack_region.selection.end);
    std::array<double, 32> values{};
    // Remove the time-varying oscillator phase using the fundamental guide;
    // retain relative harmonic phase rather than converting pitch to timbre.
    double phase0{};
    if (!frame.harmonics.empty()) phase0 = frame.harmonics.front().phase_radians;
    for (const auto& h : frame.harmonics) {
        if (h.harmonic == 0 || h.harmonic > 15) continue;
        const double weight = part ? harmonicWeight(strategy, *part, h.harmonic, tick, attack) : 1.0;
        const double phase = h.phase_radians - h.harmonic * phase0;
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] += weight * h.amplitude
                * std::cos(2 * pi * h.harmonic * i / values.size() + phase);
    }
    double peak{}, energy{};
    for (double v : values) { peak = std::max(peak, std::abs(v)); energy += v * v; }
    WaveShape shape;
    shape.rms = std::sqrt(energy / values.size());
    if (peak > 1e-6) for (std::size_t i = 0; i < values.size(); ++i)
        shape.wave[i] = static_cast<std::int8_t>(std::clamp(std::lround(values[i] * 127 / peak), -127L, 127L));
    return shape;
}

double shapeDistance(const SccWaveform& a, const SccWaveform& b) {
    const auto x = analyzeSccMorphWaveform(a), y = analyzeSccMorphWaveform(b);
    double difference{};
    for (std::size_t i = 1; i < 16; ++i) {
        const double d = x.magnitude[i] - y.magnitude[i];
        difference += d * d;
    }
    return difference;
}

SavedTimbreReference referenceFor(CompositeTimbre& tone, const SccWaveform& wave) {
    const auto matches = [&](const SavedTimbreReference& ref) {
        if (ref.source != TimbreSource::Scc) return false;
        for (std::size_t i = 0; i < 32; ++i)
            if (ref.scc_waveform[i] != static_cast<std::uint8_t>(wave[i])) return false;
        return true;
    };
    for (const auto& layer : tone.layers)
        if (layer.base_timbre && matches(*layer.base_timbre)) return *layer.base_timbre;
    for (const auto& ref : tone.embedded_timbres) if (matches(ref)) return ref;
    SavedTimbreReference ref;
    ref.library_id = allocateCompositeOwnedTimbreId(tone);
    ref.source = TimbreSource::Scc;
    ref.name = "WAV SCC";
    ref.number_mode = TimbreNumberMode::Manual;
    for (std::size_t i = 0; i < 32; ++i) ref.scc_waveform[i] = static_cast<std::uint8_t>(wave[i]);
    return ref;
}

void assignSourceNumbers(CompositeTimbre& tone, std::uint8_t start) {
    std::map<std::uint64_t, std::uint8_t> numbers;
    const auto assign = [&](SavedTimbreReference& ref) {
        if (ref.source != TimbreSource::Scc) return;
        if (!numbers.contains(ref.library_id))
            numbers[ref.library_id] = static_cast<std::uint8_t>(start + numbers.size());
        ref.manual_number = numbers[ref.library_id];
        ref.number_mode = TimbreNumberMode::Manual;
    };
    for (auto& layer : tone.layers) if (layer.base_timbre) assign(*layer.base_timbre);
    for (auto& ref : tone.embedded_timbres) assign(ref);
    for (auto& layer : tone.layers) for (auto& event : layer.timbre_automation)
        if (event.kind == EnvelopeEventKind::Timbre && numbers.contains(event.target_library_id))
            event.value = numbers[event.target_library_id];
}

// Rebuilt SCC keyframes can consume IDs previously held by the retained OPLL
// snapshot. Keep those namespaces distinct before any resolver sees the tone.
void preserveOpllSnapshotIds(CompositeTimbre& tone) {
    for (auto& layer : tone.layers) {
        if (layer.source != TimbreSource::Opll || !layer.base_timbre) continue;
        const auto old = layer.base_timbre->library_id;
        const bool collision = std::any_of(tone.embedded_timbres.begin(), tone.embedded_timbres.end(),
            [old](const auto& ref) { return ref.library_id == old; })
            || std::any_of(tone.layers.begin(), tone.layers.end(), [old](const auto& other) {
                return other.source == TimbreSource::Scc && other.base_timbre
                    && other.base_timbre->library_id == old;
            });
        if (collision) layer.base_timbre->library_id = allocateCompositeOwnedTimbreId(tone);
    }
}

CompositeWavResourcePlan resourcePlan(const CompositeTimbre& tone) {
    CompositeWavResourcePlan plan;
    plan.first_scc_number = tone.scc_morph_bank_base;
    const auto numbers = resolveTimbreNumbers(tone);
    std::set<std::uint8_t> used;
    for (const auto& item : numbers.assignments) {
        const auto* ref = findEmbeddedTimbreSnapshot(tone, item.library_id);
        if (ref && ref->source == TimbreSource::Scc) used.insert(item.number);
    }
    plan.scc_numbers.assign(used.begin(), used.end());
    plan.scc_waveforms = used.size();
    return plan;
}

void addVolume(CompositeLayer& layer, std::uint32_t count, int volume) {
    volume = std::clamp(volume, 0, 15);
    if (!layer.volume_envelope.events.empty()
        && layer.volume_envelope.events.back().value == volume) return;
    EnvelopeEvent event;
    event.kind = EnvelopeEventKind::Volume;
    event.count = count;
    event.value = volume;
    layer.volume_envelope.events.push_back(event);
}

void simplifyVolume(CompositeLayer& layer, std::uint32_t keyoff) {
    if (layer.volume_envelope.events.size() < 3) return;
    std::vector<int> values(keyoff);
    std::size_t current{};
    for (std::uint32_t tick = 0; tick < keyoff; ++tick) {
        while (current + 1 < layer.volume_envelope.events.size()
            && layer.volume_envelope.events[current + 1].count <= tick) ++current;
        values[tick] = layer.volume_envelope.events[current].value;
    }
    std::set<std::uint32_t> selected{0, keyoff - 1};
    while (selected.size() < 40) {
        double worst{0.7};
        std::optional<std::uint32_t> position;
        for (auto first = selected.begin(), second = std::next(first);
             second != selected.end(); ++first, ++second) {
            for (auto tick = *first + 1; tick < *second; ++tick) {
                const double fitted = std::lerp(static_cast<double>(values[*first]),
                    static_cast<double>(values[*second]), static_cast<double>(tick - *first) / (*second - *first));
                const auto error = std::abs(values[tick] - fitted);
                if (error > worst) { worst = error; position = tick; }
            }
        }
        if (!position) break;
        selected.insert(*position);
    }
    layer.volume_envelope.events.clear();
    std::uint32_t preceding{};
    for (const auto tick : selected) {
        EnvelopeEvent event;
        event.kind = EnvelopeEventKind::Volume;
        event.count = tick;
        event.value = values[tick];
        const auto interval = tick - preceding;
        event.automatic = interval > 1 && interval <= 239;
        layer.volume_envelope.events.push_back(event);
        preceding = tick;
    }
}

CompositeTimbre generateCandidate(const SourceAnalysis& analysis,
    const CompositeWavConversionOptions& options, CompositeWavStrategy strategy,
    const std::vector<std::uint32_t>& keyframes, std::uint8_t intermediates,
    double curve, std::uint8_t rom, const OpllPatchParameters* patch,
    std::uint32_t keyoff, bool morph_enabled = false) {
    CompositeTimbre tone;
    tone.name = "WAV converted";
    tone.scc_morph_bank_base = options.scc_start_number.value_or(0);
    const bool dualScc = options.configuration == CompositeWavConfiguration::SccScc;
    const bool opll = options.configuration == CompositeWavConfiguration::SccOpllRom
        || options.configuration == CompositeWavConfiguration::SccOpllOriginal;
    const auto total = std::max<std::uint32_t>(1, countAt(analysis, analysis.selection.end));
    const auto attack = countAt(analysis, analysis.attack_region.selection.end);
    double amplitudePeak{};
    for (const auto& f : analysis.amplitude_envelope) amplitudePeak = std::max(amplitudePeak, f.rms);
    const auto sustain = analysis.sustain_region;
    const bool loop = options.loop_mode == CompositeWavLoopMode::Automatic && sustain
        && sustain->confidence >= 0.5 && countAt(analysis, sustain->selection.begin) + 1 < keyoff;
    for (std::size_t part = 0; part < (dualScc ? 2U : 1U); ++part) {
        const std::optional<std::size_t> wavePart = dualScc ? std::optional<std::size_t>(part)
            : opll && (strategy == CompositeWavStrategy::FundamentalResidual
                || strategy == CompositeWavStrategy::LowHigh) ? std::optional<std::size_t>(1) : std::nullopt;
        CompositeLayer layer;
        layer.source = TimbreSource::Scc;
        layer.scc_output_allocation = SccOutputAllocationMode::Contiguous;
        if (part == 0) layer.scc_output_start = options.scc_start_number;
        layer.channel = static_cast<std::uint8_t>(part); // Independent physical wave RAM.
        layer.envelope_number = static_cast<std::uint8_t>(part);
        layer.name = "WAV SCC " + std::to_string(part + 1);
        layer.envelope_timeline.length_counts = total;
        const auto first = waveAt(analysis, keyframes.front(), strategy,
            wavePart);
        layer.base_timbre = referenceFor(tone, first.wave);
        tone.layers.push_back(layer); // Reserve ID before event-only snapshots.
        auto& dest = tone.layers.back();
        EnvelopeEvent initial;
        initial.kind = EnvelopeEventKind::Timbre;
        initial.count = 0;
        initial.target_library_id = dest.base_timbre->library_id;
        dest.timbre_automation.push_back(initial);
        for (std::size_t i = 1; i < keyframes.size(); ++i) {
            const auto shape = waveAt(analysis, keyframes[i], strategy,
                wavePart);
            auto ref = referenceFor(tone, shape.wave);
            if (!findEmbeddedTimbreSnapshot(tone, ref.library_id)) tone.embedded_timbres.push_back(ref);
            EnvelopeEvent event;
            event.kind = EnvelopeEventKind::Timbre;
            event.count = keyframes[i];
            event.target_library_id = ref.library_id;
            event.scc_morph = {morph_enabled || intermediates > 0, intermediates, curve};
            event.scc_morph.distribution_mode = SccMorphDistributionMode::ToneDistribution;
            dest.timbre_automation.push_back(event);
        }
        for (std::uint32_t tick = 0; tick < keyoff; ++tick) {
            const auto& amp = nearest(analysis.amplitude_envelope, sampleAt(analysis, tick));
            double fraction = 1;
            if (dualScc) {
                const auto component = waveAt(analysis, tick, strategy, part);
                const auto combined = waveAt(analysis, tick, strategy, std::nullopt);
                fraction = combined.rms > 1e-6 ? component.rms / combined.rms : 0;
            } else if (opll) {
                if (strategy == CompositeWavStrategy::AttackSustain)
                    fraction = harmonicWeight(strategy, 0, 1, tick, attack);
                else if (wavePart) {
                    const auto component = waveAt(analysis, tick, strategy, wavePart);
                    const auto combined = waveAt(analysis, tick, strategy, std::nullopt);
                    fraction = combined.rms > 1e-6 ? component.rms / combined.rms : 0;
                } else fraction = 0.65;
            }
            addVolume(dest, tick, static_cast<int>(std::lround(15 * fraction * amp.rms / (amplitudePeak + epsilon))));
        }
        simplifyVolume(dest, keyoff);
        if (loop) {
            const auto loopStart = countAt(analysis, sustain->selection.begin);
            const auto loopEnd = std::min(keyoff - 1, countAt(analysis, sustain->selection.end));
            if (loopEnd > loopStart) {
                dest.envelope_timeline.loop_start_count = loopStart;
                dest.envelope_timeline.loop_end_count = loopEnd;
                for (auto& event : dest.timbre_automation)
                    event.after_loop_start = event.count == loopStart || event.count == loopEnd;
            }
        }
        const auto release = total > keyoff ? total - keyoff : 0;
        dest.key_off_hang = static_cast<std::uint8_t>(std::min<std::uint32_t>(255,
            release ? std::max<std::uint32_t>(1, release / 15) : 0));
    }
    if (opll) {
        CompositeLayer layer;
        layer.name = "WAV OPLL";
        layer.source = TimbreSource::Opll;
        layer.envelope_number = 2;
        layer.envelope_timeline.length_counts = total;
        if (patch) {
            SavedTimbreReference ref;
            ref.library_id = allocateCompositeOwnedTimbreId(tone);
            ref.name = "WAV OPLL original";
            ref.source = TimbreSource::Opll;
            ref.opll_registers = encodeOpllPatch(*patch);
            layer.base_timbre = ref;
        } else layer.base_opll_rom = rom;
        for (std::uint32_t tick = 0; tick < keyoff; ++tick) {
            const auto& amp = nearest(analysis.amplitude_envelope, sampleAt(analysis, tick));
            double fraction = strategy == CompositeWavStrategy::AttackSustain
                ? harmonicWeight(strategy, 1, 1, tick, attack) : 0.7;
            // YM2413 channel volume is attenuation in 3 dB steps.
            const double desired = fraction * amp.rms / (amplitudePeak + epsilon);
            const int volume = desired > 1e-4
                ? 15 + static_cast<int>(std::lround(20 * std::log10(desired) / 3)) : 0;
            addVolume(layer, tick, volume);
        }
        simplifyVolume(layer, keyoff);
        if (loop) {
            layer.envelope_timeline.loop_start_count = countAt(analysis, sustain->selection.begin);
            layer.envelope_timeline.loop_end_count = std::min(keyoff - 1, countAt(analysis, sustain->selection.end));
        }
        tone.layers.push_back(std::move(layer));
    }
    assignSourceNumbers(tone, tone.scc_morph_bank_base);
    return tone;
}

std::vector<float> referencePcm(const SourceAnalysis& a) {
    // Resample sample rate only. Pitch normalization belongs to the evaluator's
    // frequency axes and must never shift attacks, amplitude or key-off times.
    const auto count = static_cast<std::size_t>(std::llround(
        (a.selection.end - a.selection.begin) * static_cast<double>(rate) / a.source->sample_rate));
    std::vector<float> pcm(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double sourcePos = a.selection.begin + i * static_cast<double>(a.source->sample_rate) / rate;
        const auto first = static_cast<std::size_t>(sourcePos);
        if (first >= a.selection.end) break;
        const auto next = std::min(first + 1, a.selection.end - 1);
        pcm[i] = static_cast<float>(std::lerp(a.analysisPcm()[first],
            a.analysisPcm()[next], sourcePos - first));
    }
    return pcm;
}
} // namespace

CompositeWavQualityMetrics evaluateCompositeWavePcm(
    std::span<const float> reference, std::span<const float> rendered,
    double frequency, std::size_t attackEnd, std::size_t waves,
    const CompositeWavQualityWeights& weights, const std::atomic_bool* cancel,
    const SourceAnalysis* analysis) {
    CompositeWavQualityMetrics metrics;
    const auto count = std::min(reference.size(), rendered.size());
    if (!count || !std::isfinite(frequency) || frequency <= 0
        || std::any_of(reference.begin(), reference.end(), [](float x) { return !std::isfinite(x); })
        || std::any_of(rendered.begin(), rendered.end(), [](float x) { return !std::isfinite(x); })) {
        metrics.total = std::numeric_limits<double>::infinity();
        return metrics;
    }
    const double xnorm = 1 / (rms(reference) + epsilon);
    const double ynorm = 1 / (rms(rendered) + epsilon);
    for (const std::size_t window : {256U, 1024U, 4096U}) {
        double difference{}, energy{}, logDifference{}, logWeight{};
        const std::size_t hop = std::max(window / 4, (count + 511) / 512);
        for (std::size_t centre = 0; centre < count; centre += hop) {
            if (cancelled(cancel)) { metrics.total = std::numeric_limits<double>::infinity(); return metrics; }
            auto x = spectrum(reference, centre, window, xnorm);
            const auto y = spectrum(rendered, centre, window, ynorm);
            if (analysis) {
                double sourceFrequency = analysis->reference_pitch_hz;
                if (!analysis->pitch_trajectory.empty()) {
                    const auto sourceSample = analysis->selection.begin
                        + static_cast<std::size_t>(centre * static_cast<double>(analysis->source->sample_rate) / rate);
                    const auto& pitch = nearest(analysis->pitch_trajectory, sourceSample);
                    if (pitch.frequency_hz > 0 && pitch.confidence >= 0.2) sourceFrequency = pitch.frequency_hz;
                }
                const auto original = x;
                for (std::size_t k = 1; k < x.size(); ++k) {
                    const double sourceBin = k * sourceFrequency / frequency;
                    const auto low = static_cast<std::size_t>(sourceBin);
                    x[k] = low + 1 < original.size()
                        ? std::lerp(original[low], original[low + 1], sourceBin - low) : 0;
                }
            }
            std::array<double, 40> erbX{}, erbY{};
            const double erbMax = 21.4 * std::log10(1 + 0.00437 * rate / 2);
            for (std::size_t k = 1; k < x.size(); ++k) {
                const double d = x[k] - y[k];
                difference += d * d; energy += x[k] * x[k];
                const double significance = std::max(x[k], y[k]);
                logDifference += significance * std::abs(std::log(x[k] + 1e-5) - std::log(y[k] + 1e-5));
                logWeight += significance;
                const double hz = static_cast<double>(k * rate) / window;
                const auto band = std::min<std::size_t>(39, static_cast<std::size_t>(
                    40 * 21.4 * std::log10(1 + 0.00437 * hz) / erbMax));
                erbX[band] += x[k] * x[k]; erbY[band] += y[k] * y[k];
            }
            if (window == 4096) {
                double harmonicDifference{}, harmonicEnergy{}, erbDifference{}, erbEnergy{};
                for (std::size_t h = 1; h <= 16 && h * frequency < rate / 2; ++h) {
                    const auto bin = static_cast<std::size_t>(std::lround(h * frequency * window / rate));
                    double a{}, b{};
                    for (std::size_t k = bin > 1 ? bin - 1 : 0; k <= std::min(bin + 1, x.size() - 1); ++k) {
                        a += x[k] * x[k]; b += y[k] * y[k];
                    }
                    harmonicDifference += std::pow(std::sqrt(a) - std::sqrt(b), 2);
                    harmonicEnergy += a;
                }
                for (std::size_t band = 0; band < 40; ++band) {
                    erbDifference += std::pow(std::sqrt(erbX[band]) - std::sqrt(erbY[band]), 2);
                    erbEnergy += erbX[band];
                }
                metrics.harmonic += harmonicDifference;
                metrics.erb += erbDifference;
                // Separate denominators accumulated below using local scratch.
                metrics.transition += harmonicEnergy;
                metrics.complexity += erbEnergy;
            }
        }
        metrics.multi_resolution_stft += (std::sqrt(difference / (energy + epsilon))
            + 0.15 * logDifference / (logWeight + epsilon)) / 3;
    }
    metrics.harmonic = std::sqrt(metrics.harmonic / (metrics.transition + epsilon));
    metrics.erb = std::sqrt(metrics.erb / (metrics.complexity + epsilon));
    metrics.transition = 0;
    // Step discontinuity above source transients at the actual 60 Hz grid.
    double transitions{}, sourceTransitions{};
    for (std::size_t i = 800; i < count; i += 800) {
        const auto before = i > 32 ? i - 32 : 0;
        const auto after = std::min(count, i + 32);
        const double dx = rms(reference.subspan(i, after - i)) - rms(reference.subspan(before, i - before));
        const double dy = rms(rendered.subspan(i, after - i)) - rms(rendered.subspan(before, i - before));
        transitions += std::max(0.0, std::abs(dy * ynorm) - std::abs(dx * xnorm));
        sourceTransitions += 1;
    }
    metrics.transition = transitions / (sourceTransitions + epsilon);
    metrics.volume = envelopeLoss(reference, rendered, count, 800);
    const auto attackLength = std::min(count, std::max<std::size_t>(240, attackEnd));
    metrics.attack = envelopeLoss(reference, rendered, attackLength, 240);
    double attackDifference{}, attackEnergy{};
    for (std::size_t centre = 0; centre < attackLength; centre += 120) {
        if (cancelled(cancel)) { metrics.total = std::numeric_limits<double>::infinity(); return metrics; }
        const auto x = spectrum(reference, centre, 256, xnorm);
        const auto y = spectrum(rendered, centre, 256, ynorm);
        for (std::size_t k = 1; k < x.size(); ++k) {
            attackDifference += std::pow(x[k] - y[k], 2);
            attackEnergy += x[k] * x[k];
        }
    }
    metrics.attack += 0.5 * std::sqrt(attackDifference / (attackEnergy + epsilon));
    metrics.complexity = static_cast<double>(waves) / 32;
    metrics.total = weights.multi_resolution_stft * metrics.multi_resolution_stft
        + weights.harmonic * metrics.harmonic + weights.erb * metrics.erb
        + weights.attack * metrics.attack + weights.volume * metrics.volume
        + weights.transition * metrics.transition + weights.complexity * metrics.complexity;
    return metrics;
}

// Characterize ROM pitch/envelope dependence before ordering combined seeds.
// This bounded preparation is separate from complete-candidate search; every
// accepted result is still judged only by its actual source-note Engine PCM.
std::vector<std::uint8_t> rankFixedRomCandidates(const SourceAnalysis& analysis,
    int reference_note, const std::atomic_bool* cancel) {
    std::vector<std::pair<double, std::uint8_t>> ranking;
    for (std::uint8_t rom = 0; rom < 15 && !cancelled(cancel); ++rom) {
        double score{};
        bool valid = true;
        for (const int offset : {-12, 0, 12}) {
            for (const std::size_t held : {std::size_t{2400}, std::size_t{9600}}) {
                if (cancelled(cancel)) return {};
                CompositeTimbre isolated;
                CompositeLayer layer;
                layer.source = TimbreSource::Opll;
                layer.base_opll_rom = rom;
                layer.volume = 15;
                isolated.layers.push_back(layer);
                CompositeWavRenderOptions options;
                const auto probeOffset = offset < 0 && reference_note < 36 ? 24
                    : offset > 0 && reference_note > 107 ? -24 : offset;
                const auto note = reference_note + probeOffset;
                options.midi_note = static_cast<std::uint8_t>(note);
                options.key_off_frame = held;
                options.frame_count = held + 4800;
                options.cancel = cancel;
                const auto rendered = renderCompositeWav(isolated, options);
                if (!rendered.ok()) { valid = false; break; }
                const auto centre = held - 800;
                const auto sourcePosition = std::min(analysis.selection.end - 1,
                    analysis.selection.begin + held * analysis.source->sample_rate / rate);
                const auto& source = nearest(analysis.harmonic_trajectory, sourcePosition);
                std::array<double, 15> target{}, actual{};
                for (const auto& harmonic : source.harmonics)
                    if (harmonic.harmonic >= 1 && harmonic.harmonic <= 15)
                        target[harmonic.harmonic - 1] = harmonic.amplitude;
                const double frequency = 440 * std::pow(2.0, (note - 69) / 12.0);
                for (std::size_t harmonic = 1; harmonic <= actual.size(); ++harmonic) {
                    std::complex<double> projection{};
                    double weight{};
                    const double omega = -2 * pi * frequency * harmonic / rate;
                    auto oscillator = std::polar(1.0, omega * -512);
                    const auto step = std::polar(1.0, omega);
                    for (std::size_t i = 0; i < 1024; ++i) {
                        const auto position = static_cast<std::int64_t>(centre) + static_cast<std::int64_t>(i) - 512;
                        const double hann = .5 - .5 * std::cos(2 * pi * i / 1023);
                        if (position >= 0 && static_cast<std::size_t>(position) < rendered.mono_pcm.size())
                            projection += rendered.mono_pcm[static_cast<std::size_t>(position)] * hann * oscillator;
                        weight += hann;
                        oscillator *= step;
                    }
                    actual[harmonic - 1] = 2 * std::abs(projection) / weight;
                }
                const double targetSum = std::accumulate(target.begin(), target.end(), epsilon);
                const double actualSum = std::accumulate(actual.begin(), actual.end(), epsilon);
                for (std::size_t i = 0; i < actual.size(); ++i)
                    score += std::pow(target[i] / targetSum - actual[i] / actualSum, 2);
                // Include the release characteristic without comparing pitches
                // directly in Hertz: dimensionless tail/held RMS ratios.
                const double heldRms = rms(std::span<const float>(rendered.mono_pcm).subspan(held - 800, 800));
                const double tailRms = rms(std::span<const float>(rendered.mono_pcm).subspan(held + 3200, 800));
                const auto& amplitude = nearest(analysis.amplitude_envelope, sourcePosition);
                const auto laterPosition = std::min(analysis.selection.end - 1,
                    sourcePosition + 3200 * analysis.source->sample_rate / rate);
                const auto& later = nearest(analysis.amplitude_envelope, laterPosition);
                score += .1 * std::pow(std::min(2.0, tailRms / (heldRms + epsilon))
                    - std::min(2.0, later.rms / (amplitude.rms + epsilon)), 2);
            }
            if (!valid) break;
        }
        if (valid) ranking.emplace_back(score, rom);
    }
    std::stable_sort(ranking.begin(), ranking.end());
    std::vector<std::uint8_t> roms;
    for (const auto& item : ranking) roms.push_back(item.second);
    return roms;
}
CompositeWavConversionResult convertCompositeWave(
    std::shared_ptr<const SourceAnalysis> analysis, const CompositeWavConversionOptions& options) {
    CompositeWavConversionResult result;
    result.analysis_reference = analysis;
    const auto started = std::chrono::steady_clock::now();
    const auto control = options.control;
    const auto* cancel = control ? &control->cancel_requested : nullptr;
    const auto stage = [&](CompositeWavStage value) {
        if (control) control->stage.store(value, std::memory_order_relaxed);
    };
    const auto finish = [&]() {
        result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (cancelled(cancel)) { result.completion = CompositeWavConversionCompletion::Cancelled; stage(CompositeWavStage::Cancelled); }
        else stage(CompositeWavStage::Completed);
        return std::move(result);
    };
    if (control) control->evaluations.store(0, std::memory_order_relaxed);
    if (!analysis || !validAnalysis(*analysis)
        || static_cast<unsigned>(options.configuration) > static_cast<unsigned>(CompositeWavConfiguration::SccOpllOriginal)
        || static_cast<unsigned>(options.strategy) > static_cast<unsigned>(CompositeWavStrategy::Independent)
        || static_cast<unsigned>(options.preference) > static_cast<unsigned>(CompositeWavPreference::Compact)
        || static_cast<unsigned>(options.loop_mode) > static_cast<unsigned>(CompositeWavLoopMode::None)
        || !options.max_scc_waveforms || options.max_scc_waveforms > 32
        || !options.max_evaluations || options.max_evaluations > 4096
        || (options.scc_start_number && *options.scc_start_number > 31)
        || (options.fixed_opll_tone && *options.fixed_opll_tone > 14)
        || !std::isfinite(options.minimum_improvement) || options.minimum_improvement < 0) {
        result.error = "Invalid source analysis or conversion settings";
        return finish();
    }
    if (cancelled(cancel)) return finish();
    const int note = static_cast<int>(std::lround(69 + 12 * std::log2(analysis->reference_pitch_hz / 440)));
    if (note < 24 || note > 119) { result.error = "Reference pitch lies outside the playable note range"; return finish(); }
    const double frequency = 440 * std::pow(2.0, (note - 69) / 12.0);
    auto reference = referencePcm(*analysis);
    if (reference.size() > CompositeWavRenderOptions::kMaximumFrameCount) {
        result.error = "Selected audio exceeds the 120 second conversion limit"; return finish();
    }
    const auto keyoffSample = options.key_off_position.value_or(analysis->estimated_key_off
        ? analysis->estimated_key_off->sample_position : analysis->selection.end);
    if (keyoffSample <= analysis->selection.begin || keyoffSample > analysis->selection.end) {
        result.error = "Key-off must be inside the selected audio or at its end"; return finish();
    }
    const auto keyoff = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(
        (keyoffSample - analysis->selection.begin) * 60.0 / analysis->source->sample_rate)));
    const auto attackFrame = static_cast<std::size_t>((analysis->attack_region.selection.end
        - analysis->selection.begin) * static_cast<double>(rate) / analysis->source->sample_rate);
    CompositeWavRenderOptions rendering;
    rendering.midi_note = static_cast<std::uint8_t>(note);
    rendering.frame_count = reference.size();
    rendering.key_off_frame = static_cast<std::size_t>(std::llround((keyoffSample - analysis->selection.begin)
        * static_cast<double>(rate) / analysis->source->sample_rate));
    rendering.cancel = cancel;
    std::size_t stalled{};
    std::size_t stageLimit = options.max_evaluations;
    CompositeWavStrategy currentStrategy = options.strategy;
    CompositeWavStrategy bestStrategy = options.strategy;
    CompositeWavQualityWeights qualityWeights;
    qualityWeights.complexity = options.preference == CompositeWavPreference::Compact ? 0.02
        : options.preference == CompositeWavPreference::Quality ? 0.001 : 0.003;
    const auto budget = [&]() {
        return !cancelled(cancel) && result.evaluations < stageLimit
            && (options.stagnation_limit == 0 || stalled < options.stagnation_limit);
    };
    const auto evaluate = [&](CompositeTimbre candidate) {
        if (!budget()) return false;
        stage(CompositeWavStage::Morph);
        const auto compiled = compileSccMorph(candidate);
        if (!compiled.valid) return false;
        const auto resources = resourcePlan(compiled.timbre);
        if (!resources.scc_waveforms || resources.scc_waveforms > options.max_scc_waveforms) return false;
        stage(CompositeWavStage::Evaluation);
        auto rendered = renderCompositeWav(candidate, rendering);
        ++result.evaluations;
        if (control) control->evaluations.store(result.evaluations, std::memory_order_relaxed);
        if (!rendered.ok()) { ++stalled; return false; }
        const auto quality = evaluateCompositeWavePcm(reference, rendered.mono_pcm,
            frequency, attackFrame, resources.scc_waveforms, qualityWeights, cancel, analysis.get());
        if (!std::isfinite(quality.total)) { ++stalled; return false; }
        if (!result.composite_tone) result.initial_loss = quality.total;
        if (!result.composite_tone || quality.total + options.minimum_improvement < result.best_loss) {
            result.best_loss = quality.total;
            bestStrategy = currentStrategy;
            result.quality = quality;
            result.composite_tone = std::move(candidate);
            result.resource_plan = resources;
            result.preview = std::move(rendered);
            stalled = 0;
            return true;
        }
        ++stalled;
        return false;
    };
    stage(CompositeWavStage::Candidates);
    std::vector<CompositeWavStrategy> strategies{options.strategy};
    if (options.configuration == CompositeWavConfiguration::Scc) strategies = {CompositeWavStrategy::Independent};
    else if (options.strategy == CompositeWavStrategy::Automatic)
        strategies = {CompositeWavStrategy::FundamentalResidual, CompositeWavStrategy::LowHigh,
            CompositeWavStrategy::AttackSustain, CompositeWavStrategy::Independent};
    const auto initialFrame = std::min(keyoff - 1, countAt(*analysis, analysis->attack_region.selection.end));
    std::vector<std::uint32_t> keyframes{0};
    if (initialFrame > 0 && options.max_scc_waveforms >= 2) keyframes.push_back(initialFrame);
    std::vector<OpllPatchParameters> patches;
    if (options.configuration == CompositeWavConfiguration::SccOpllOriginal) {
        const auto shape = waveAt(*analysis, initialFrame, CompositeWavStrategy::Independent, std::nullopt);
        OpllApproximationOptions approximation;
        approximation.profile = OpllApproximationProfile::Compact;
        approximation.max_workers = 1;
        approximation.control = std::make_shared<OpllApproximationControl>();
        // Existing approximation owns its cancellation token. Bridge it only
        // on this offline worker, never via audio/UI blocking work.
        std::jthread cancellationBridge([&, token = approximation.control](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancelled(cancel)) { token->requestCancel(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        patches = approximateSccWaveformWithOpllResult(shape.wave, approximation).candidates;
        cancellationBridge.request_stop();
        if (patches.empty() && !cancelled(cancel)) patches.push_back(defaultOpllPatch());
        if (patches.size() > 6) patches.resize(6);
    }
    std::vector<std::uint8_t> roms{0};
    if (options.configuration == CompositeWavConfiguration::SccOpllRom) {
        roms.clear();
        if (options.fixed_opll_tone) roms.push_back(*options.fixed_opll_tone);
        else roms = rankFixedRomCandidates(*analysis, note, cancel);
    }
    // Structural seed comparisons always cover every ROM and every requested
    // automatic role before stagnation termination is enabled.
    for (std::size_t strategyIndex = 0; strategyIndex < strategies.size(); ++strategyIndex) {
        const auto strategy = strategies[strategyIndex];
        currentStrategy = strategy;
        for (auto rom : roms) {
            // All ROM15 are evaluated once; later role seeds reuse the best ROM
            // so the candidate stage cannot consume the complete search budget.
            if (strategyIndex > 0 && result.composite_tone
                && options.configuration == CompositeWavConfiguration::SccOpllRom
                && rom != result.composite_tone->layers.back().base_opll_rom.value_or(0)) continue;
            if (cancelled(cancel) || result.evaluations >= options.max_evaluations) break;
            stalled = 0;
            if (!patches.empty()) for (const auto& patch : patches) {
                if (strategyIndex > 0 && result.composite_tone
                    && encodeOpllPatch(patch) != result.composite_tone->layers.back().base_timbre->opll_registers) continue;
                evaluate(generateCandidate(*analysis, options, strategy, keyframes, 0, 1, rom, &patch, keyoff));
            } else evaluate(generateCandidate(*analysis, options, strategy, keyframes, 0, 1, rom, nullptr, keyoff));
        }
    }
    stalled = 0;
    currentStrategy = bestStrategy;
    // Joint channel volume coordinates: actual combined Engine PCM selects the
    // result, including interference, chip envelope and mix-stage clipping.
    const auto volumeSearch = [&]() {
        if (!result.composite_tone) return;
        for (std::size_t layer = 0; layer < result.composite_tone->layers.size() && budget(); ++layer) {
            for (int volume : {12, 9, 6, 3, 15}) {
                if (!budget()) break;
                auto candidate = *result.composite_tone;
                candidate.layers[layer].volume = static_cast<std::uint8_t>(volume);
                evaluate(std::move(candidate));
            }
        }
    };
    stageLimit = std::min(options.max_evaluations, result.evaluations + std::max<std::size_t>(1, options.max_evaluations / 8));
    volumeSearch();
    // Build a complementary SCC seed from the selected OPLL's actual temporal
    // output. Magnitude subtraction is an initialization only; acceptance still
    // depends on the full two-source Engine render, including relative phase.
    stageLimit = std::min(options.max_evaluations, result.evaluations + 2);
    stalled = 0;
    std::optional<SourceAnalysis> residualAnalysis;
    if (result.composite_tone && budget()
        && (options.configuration == CompositeWavConfiguration::SccOpllRom
            || options.configuration == CompositeWavConfiguration::SccOpllOriginal)) {
        auto isolated = *result.composite_tone;
        isolated.layers.erase(isolated.layers.begin());
        isolated.embedded_timbres.clear();
        auto opllPcm = renderCompositeWav(isolated, rendering);
        ++result.evaluations;
        if (control) control->evaluations.store(result.evaluations, std::memory_order_relaxed);
        if (opllPcm.ok()) {
            residualAnalysis = *analysis;
            for (auto& frame : residualAnalysis->harmonic_trajectory) {
                if (cancelled(cancel)) break;
                const auto centre = static_cast<std::size_t>((frame.sample_position - analysis->selection.begin)
                    * static_cast<double>(rate) / analysis->source->sample_rate);
                for (auto& harmonic : frame.harmonics) {
                    if (harmonic.harmonic == 0 || harmonic.harmonic > 15) continue;
                    std::complex<double> projection{};
                    double hannSum{};
                    const auto omega = -2 * pi * frequency * harmonic.harmonic / rate;
                    auto oscillator = std::polar(1.0, omega * -512);
                    const auto step = std::polar(1.0, omega);
                    for (std::size_t i = 0; i < 1024; ++i) {
                        const auto position = static_cast<std::int64_t>(centre) + static_cast<std::int64_t>(i) - 512;
                        const double hann = 0.5 - 0.5 * std::cos(2 * pi * i / 1023);
                        if (position >= 0 && static_cast<std::size_t>(position) < opllPcm.mono_pcm.size())
                            projection += opllPcm.mono_pcm[static_cast<std::size_t>(position)] * hann * oscillator;
                        hannSum += hann;
                        oscillator *= step;
                    }
                    harmonic.amplitude = std::max(0.0, harmonic.amplitude - 2 * std::abs(projection) / hannSum);
                }
            }
            auto candidate = *result.composite_tone;
            auto residual = generateCandidate(*residualAnalysis, options, bestStrategy, keyframes, 0, 1, 0, nullptr, keyoff);
            candidate.layers.front().base_timbre = residual.layers.front().base_timbre;
            candidate.layers.front().timbre_automation = residual.layers.front().timbre_automation;
            candidate.embedded_timbres = std::move(residual.embedded_timbres);
            preserveOpllSnapshotIds(candidate);
            assignSourceNumbers(candidate, candidate.scc_morph_bank_base);
            if (!evaluate(std::move(candidate))) residualAnalysis.reset();
        }
    }
    stageLimit = std::min(options.max_evaluations, result.evaluations + std::max<std::size_t>(1, options.max_evaluations / 3));
    stalled = 0;
    // Add a keyframe at the largest unresolved time-local spectral/RMS error.
    // Rebuild the whole interval using the established morph generator, then
    // compare curve/intermediate alternatives rather than blindly filling RAM.
    const std::size_t structureIterations = options.preference == CompositeWavPreference::Compact ? 2
        : options.preference == CompositeWavPreference::Quality ? 7 : 5;
    for (std::size_t iteration = 0; iteration < structureIterations && budget() && result.composite_tone; ++iteration) {
        std::uint32_t worstTick{};
        double worstError{-1};
        for (std::uint32_t tick = 0; tick < keyoff; ++tick) {
            if (cancelled(cancel)) break;
            if (std::find(keyframes.begin(), keyframes.end(), tick) != keyframes.end()) continue;
            const auto frame = static_cast<std::size_t>(tick) * 800;
            if (frame >= reference.size()) break;
            const auto x = spectrum(reference, frame, 1024, 1);
            const auto y = spectrum(result.preview.mono_pcm, frame, 1024, 1);
            double error{};
            for (std::size_t k = 1; k < x.size(); ++k) error += std::pow(x[k] - y[k], 2);
            if (error > worstError) { worstError = error; worstTick = tick; }
        }
        if (worstError <= epsilon) break;
        keyframes.push_back(worstTick);
        std::sort(keyframes.begin(), keyframes.end());
        for (std::uint8_t intermediates : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{3}})
            for (double curve : {1.0, 0.5, 2.0}) {
                if (!budget()) break;
                auto candidate = *result.composite_tone;
                // Retain the selected OPLL patch and optimized volumes while
                // replacing only SCC source snapshots and tone automation.
                const auto strategy = bestStrategy;
                auto rebuilt = generateCandidate(residualAnalysis ? *residualAnalysis : *analysis, options, strategy, keyframes, intermediates,
                    curve, 0, nullptr, keyoff, true);
                candidate.embedded_timbres.clear();
                for (std::size_t layer = 0; layer < rebuilt.layers.size(); ++layer) {
                    if (rebuilt.layers[layer].source != TimbreSource::Scc) continue;
                    candidate.layers[layer].base_timbre = rebuilt.layers[layer].base_timbre;
                    candidate.layers[layer].timbre_automation = rebuilt.layers[layer].timbre_automation;
                }
                candidate.embedded_timbres = std::move(rebuilt.embedded_timbres);
                preserveOpllSnapshotIds(candidate);
                assignSourceNumbers(candidate, candidate.scc_morph_bank_base);
                evaluate(std::move(candidate));
            }
    }
    stageLimit = std::min(options.max_evaluations, result.evaluations + std::max<std::size_t>(1, options.max_evaluations / 3));
    stalled = 0;
    // Original-tone packed lanes preserve KSL/waveform bits through the shared
    // compiler. Search both static TL/FB and time-varying contours jointly with
    // SCC; ROM candidates never acquire unsupported original register writes.
    if (result.composite_tone && options.configuration == CompositeWavConfiguration::SccOpllOriginal) {
        const auto index = result.composite_tone->layers.size() - 1;
        for (const bool tl : {true, false}) for (int delta : {-8, -2, 2, 8}) {
            if (!budget()) break;
            auto candidate = *result.composite_tone;
            auto& layer = candidate.layers[index];
            auto patch = decodeOpllPatch(layer.base_timbre->opll_registers);
            if (tl) patch.modulator.total_level = static_cast<std::uint8_t>(std::clamp<int>(patch.modulator.total_level + delta, 0, 63));
            else patch.feedback = static_cast<std::uint8_t>(std::clamp<int>(patch.feedback + (delta > 0 ? 1 : -1), 0, 7));
            layer.base_timbre->opll_registers = encodeOpllPatch(patch);
            evaluate(std::move(candidate));
        }
        for (const bool tl : {true, false}) for (const bool reverse : {false, true}) {
            if (!budget()) break;
            auto candidate = *result.composite_tone;
            auto& layer = candidate.layers[index];
            auto& lane = tl ? layer.opll_tl_auto : layer.opll_fb_auto;
            lane.mode = OpllRegisterAutoMode::FreeCurve;
            lane.start_count = 0;
            lane.change_speed = 1;
            lane.coarseness = 1;
            const auto maximum = tl ? 63 : 7;
            const auto base = tl ? (layer.base_timbre->opll_registers[2] & 63)
                : (layer.base_timbre->opll_registers[3] & 7);
            lane.free_curve.clear();
            // Keep within compiled @e capacity even on long source selections.
            lane.coarseness = static_cast<std::uint8_t>(std::clamp<std::uint32_t>((keyoff + 23) / 24, 1, 255));
            for (std::uint32_t tick = 0; tick < keyoff && lane.free_curve.size() < 24; tick += lane.coarseness) {
                const auto& amp = nearest(analysis->amplitude_envelope, sampleAt(*analysis, tick));
                const double relative = amp.rms / (analysis->metadata.rms + epsilon) - 1;
                const int offset = static_cast<int>(std::lround(relative * (tl ? 8 : 2))) * (reverse ? -1 : 1);
                lane.free_curve.push_back(static_cast<std::uint8_t>(std::clamp(base + offset, 0, maximum)));
            }
            evaluate(std::move(candidate));
        }
        for (std::size_t parameter = 0; parameter < 12 && budget(); ++parameter) {
            for (const int direction : {-1, 1}) {
                if (!budget()) break;
                auto candidate = *result.composite_tone;
                auto& layer = candidate.layers[index];
                auto patch = decodeOpllPatch(layer.base_timbre->opll_registers);
                auto& op = parameter < 6 ? patch.modulator : patch.carrier;
                std::uint8_t* value = nullptr;
                switch (parameter % 6) {
                case 0: value = &op.multiplier; break;
                case 1: value = &op.attack_rate; break;
                case 2: value = &op.decay_rate; break;
                case 3: value = &op.sustain_level; break;
                case 4: value = &op.release_rate; break;
                default: value = &op.key_scale_level; break;
                }
                *value = static_cast<std::uint8_t>(std::clamp<int>(*value + direction, 0, parameter % 6 == 5 ? 3 : 15));
                layer.base_timbre->opll_registers = encodeOpllPatch(patch);
                evaluate(std::move(candidate));
            }
        }
    }
    stageLimit = options.max_evaluations > 6 ? options.max_evaluations - 6 : options.max_evaluations;
    stalled = 0;
    volumeSearch();
    stageLimit = options.max_evaluations;
    stalled = 0;
    // Complexity reduction accepts a removal only after evaluating audible
    // output. The sources not referenced after removing a key are discarded.
    if (result.composite_tone) for (std::size_t layer = 0; layer < result.composite_tone->layers.size(); ++layer) {
        for (std::size_t e = result.composite_tone->layers[layer].timbre_automation.size(); e > 0 && budget(); --e) {
            auto candidate = *result.composite_tone;
            if (e > candidate.layers[layer].timbre_automation.size()) continue;
            candidate.layers[layer].timbre_automation.erase(candidate.layers[layer].timbre_automation.begin() + e - 1);
            std::set<std::uint64_t> needed;
            for (const auto& l : candidate.layers) for (const auto& event : l.timbre_automation) needed.insert(event.target_library_id);
            std::erase_if(candidate.embedded_timbres, [&](const auto& ref) { return !needed.contains(ref.library_id); });
            assignSourceNumbers(candidate, candidate.scc_morph_bank_base);
            evaluate(std::move(candidate));
        }
    }
    if (result.composite_tone) {
        result.completion = CompositeWavConversionCompletion::Completed;
        result.warnings = analysis->warnings;
        if (options.configuration == CompositeWavConfiguration::SccOpllRom && !options.fixed_opll_tone)
            result.warnings.push_back("Fixed ROM preparation uses 90 fresh Engine renders at three pitches and two key-on durations; candidate evaluation count excludes this bounded preparation.");
        result.warnings.push_back("A bounded local search cannot reproduce all nonperiodic or high-harmonic source components; compare the rendered audio before applying.");
        if (result.evaluations >= options.max_evaluations)
            result.warnings.push_back("Evaluation budget exhausted; the best valid candidate was retained.");
    } else {
        result.completion = CompositeWavConversionCompletion::NoValidCandidate;
        result.error = "No candidate fits the waveform allocation and MGSC envelope limits";
    }
    return finish();
}
} // namespace mgstc::engine
