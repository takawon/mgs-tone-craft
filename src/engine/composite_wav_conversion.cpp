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

// Offline diagnostic scope only. RAII includes bounded no-op/cancel exits and
// never participates in search limits, ordering, scores or persistent data.
struct ScopedWavSearchSeconds {
    double& seconds;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    ~ScopedWavSearchSeconds() {
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }
};

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
    if (!std::isfinite(a.periodic_rms) || a.periodic_rms < 0
        || !std::isfinite(a.periodic_confidence) || a.periodic_confidence < 0 || a.periodic_confidence > 1) return false;
    for (const auto& frame : a.harmonic_trajectory) {
        if (!std::isfinite(frame.residual_rms) || frame.residual_rms < 0
            || !std::isfinite(frame.periodic_rms) || frame.periodic_rms < 0
            || !std::isfinite(frame.periodic_confidence) || frame.periodic_confidence < 0 || frame.periodic_confidence > 1) return false;
        for (const auto& h : frame.harmonics)
            if (!std::isfinite(h.amplitude) || !std::isfinite(h.frequency_hz)
                || !std::isfinite(h.phase_radians) || h.amplitude < 0 || h.frequency_hz < 0 || h.harmonic > 128) return false;
    }
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
    default:
        // Complementary, overlapping seeds have distinct shapes even after
        // peak normalization. Both channels remain free to change every
        // harmonic in the subsequent independent coordinate search.
        return part == 0 ? .25 + .5 / (1 + .2 * order) : .75 - .5 / (1 + .2 * order);
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
    double sourcePitch = analysis.reference_pitch_hz;
    if (!analysis.pitch_trajectory.empty()) {
        const auto& pitch = nearest(analysis.pitch_trajectory, frame.sample_position);
        if (pitch.frequency_hz > 0 && pitch.confidence >= .2) sourcePitch = pitch.frequency_hz;
    }
    for (const auto& h : frame.harmonics) {
        if (h.harmonic == 0 || h.harmonic > 15) continue;
        // Detuned partials belong to the measured residual; do not force them
        // into a different, integer harmonic of the 32-sample SCC waveform.
        if (std::abs(h.frequency_hz - h.harmonic * sourcePitch) > .06 * sourcePitch) continue;
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
            event.scc_morph.distribution_mode = SccMorphDistributionMode::AdaptiveDistribution;
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

void normalizePitchAxis(std::vector<double>& x, const SourceAnalysis* analysis,
                        std::size_t centre, double frequency) {
    if (!analysis || !analysis->source || !analysis->source->sample_rate
        || analysis->selection.begin >= analysis->selection.end
        || !std::isfinite(analysis->reference_pitch_hz) || analysis->reference_pitch_hz <= 0) return;
    double sourceFrequency = analysis->reference_pitch_hz;
    if (!analysis->pitch_trajectory.empty()) {
        const auto sample = std::min(analysis->selection.end - 1, analysis->selection.begin
            + static_cast<std::size_t>(centre * static_cast<double>(analysis->source->sample_rate) / rate));
        const auto& pitch = nearest(analysis->pitch_trajectory, sample);
        if (pitch.frequency_hz > 0 && pitch.confidence >= .2) sourceFrequency = pitch.frequency_hz;
    }
    const auto original = x;
    for (std::size_t k = 1; k < x.size(); ++k) {
        const double sourceBin = k * sourceFrequency / frequency;
        if (!std::isfinite(sourceBin) || sourceBin >= original.size() - 1) {
            x[k] = 0;
            continue;
        }
        const auto low = static_cast<std::size_t>(sourceBin);
        x[k] = low + 1 < original.size()
            ? std::lerp(original[low], original[low + 1], sourceBin - low) : 0;
    }
}

double representableWeight(const SourceAnalysis* analysis, std::size_t centre,
                           std::size_t bin, std::size_t window, double frequency,
                           double sourceMagnitude, double outputMagnitude,
                           std::size_t maximumSupportedHarmonic = 15) {
    if (!analysis || !analysis->source || !analysis->source->sample_rate
        || analysis->selection.begin >= analysis->selection.end || analysis->harmonic_trajectory.empty()) return 1;
    const auto sample = std::min(analysis->selection.end - 1, analysis->selection.begin
        + static_cast<std::size_t>(centre * static_cast<double>(analysis->source->sample_rate) / rate));
    const auto& frame = nearest(analysis->harmonic_trajectory, sample);
    const double hz = static_cast<double>(bin * rate) / window;
    const double harmonicPosition = hz / frequency;
    const auto harmonic = std::isfinite(harmonicPosition) && harmonicPosition <= 128
        ? std::lround(harmonicPosition) : 0L;
    // A 32-sample SCC waveform can represent orders 1..15. Keep a bounded
    // residual weight (including attacks), rather than fitting stochastic or
    // above-capacity source bins as if they were missing periodic harmonics.
    const double floor = .08 + .17 * (1 - std::clamp(frame.periodic_confidence, 0.0, 1.0));
    double sourcePitch = analysis->reference_pitch_hz;
    if (!analysis->pitch_trajectory.empty()) {
        const auto& pitch = nearest(analysis->pitch_trajectory, sample);
        if (pitch.frequency_hz > 0 && pitch.confidence >= .2) sourcePitch = pitch.frequency_hz;
    }
    const bool coherent = std::any_of(frame.harmonics.begin(), frame.harmonics.end(), [&](const auto& h) {
        return h.harmonic == harmonic && h.amplitude > .01 * frame.periodic_rms
            && std::abs(h.frequency_hz - harmonic * sourcePitch) <= .06 * sourcePitch;
    });
    const bool supported = harmonic >= 1 && harmonic <= static_cast<long>(maximumSupportedHarmonic) && coherent
        && std::abs(hz - harmonic * frequency) <= 2.0 * rate / window;
    const double targetWeight = supported ? 1.0 : floor;
    // Unexpected output energy is still penalized; the floor only limits the
    // pressure to invent source noise, it never hides synthesized noise.
    return std::max(targetWeight, outputMagnitude / (sourceMagnitude + outputMagnitude + epsilon));
}

void discardUnusedSnapshots(CompositeTimbre& tone) {
    std::set<std::uint64_t> used;
    for (const auto& layer : tone.layers) {
        if (layer.base_timbre) used.insert(layer.base_timbre->library_id);
        for (const auto& event : layer.timbre_automation) used.insert(event.target_library_id);
    }
    std::erase_if(tone.embedded_timbres, [&](const auto& ref) { return !used.contains(ref.library_id); });
    assignSourceNumbers(tone, tone.scc_morph_bank_base);
}

bool replaceEventWave(CompositeTimbre& tone, std::size_t layerIndex,
                      std::size_t eventIndex, const SccWaveform& waveform) {
    auto& layer = tone.layers[layerIndex];
    auto& event = layer.timbre_automation[eventIndex];
    const auto* old = findEmbeddedTimbreSnapshot(tone, event.target_library_id);
    if (!old || old->source != TimbreSource::Scc) return false;
    auto ref = *old;
    const auto previous = ref;
    ref.library_id = allocateCompositeOwnedTimbreId(tone);
    for (std::size_t i = 0; i < waveform.size(); ++i) ref.scc_waveform[i] = static_cast<std::uint8_t>(waveform[i]);
    event.target_library_id = ref.library_id;
    if (eventIndex == 0) {
        layer.base_timbre = ref;
        if (!findEmbeddedTimbreSnapshot(tone, previous.library_id))
            tone.embedded_timbres.push_back(previous);
    }
    else tone.embedded_timbres.push_back(ref);
    discardUnusedSnapshots(tone);
    return true;
}

void updateLoopSides(CompositeLayer& layer) {
    for (auto& event : layer.timbre_automation)
        event.after_loop_start = layer.envelope_timeline.loop_start_count
            && (event.count == *layer.envelope_timeline.loop_start_count
                || event.count == layer.envelope_timeline.loop_end_count.value_or(std::numeric_limits<std::uint32_t>::max()));
}

// Only inherited confirmed plans are invalidated. A newly proposed explicit
// plan belongs to the new settings and is subsequently validated by the common
// planner. Compare source bytes rather than library IDs (dedup can change IDs).
void invalidateChangedWavPlans(CompositeTimbre& candidate, const CompositeTimbre& previous) {
    for (std::size_t l = 0; l < candidate.layers.size(); ++l) {
        auto& events = candidate.layers[l].timbre_automation;
        for (std::size_t e = 1; e < events.size(); ++e) {
            auto& morph = events[e].scc_morph;
            if (!morph.explicit_plan) continue;
            const auto sameWave = [&](const CompositeTimbre& x, const EnvelopeEvent& a,
                                      const CompositeTimbre& y, const EnvelopeEvent& b) {
                const auto* first = findEmbeddedTimbreSnapshot(x, a.target_library_id);
                const auto* second = findEmbeddedTimbreSnapshot(y, b.target_library_id);
                return first && second && first->source == second->source
                    && first->scc_waveform == second->scc_waveform;
            };
            if (l >= previous.layers.size() || e >= previous.layers[l].timbre_automation.size()) {
                morph.explicit_plan.reset();
                continue;
            }
            const auto& old = previous.layers[l].timbre_automation;
            if (morph.explicit_plan != old[e].scc_morph.explicit_plan) continue;
            if (morph.enabled != old[e].scc_morph.enabled
                || morph.intermediate_count != old[e].scc_morph.intermediate_count
                || morph.curve != old[e].scc_morph.curve
                || morph.distribution_mode != old[e].scc_morph.distribution_mode
                || events[e].count != old[e].count || events[e - 1].count != old[e - 1].count
                || !sameWave(candidate, events[e], previous, old[e])
                || !sameWave(candidate, events[e - 1], previous, old[e - 1]))
                morph.explicit_plan.reset();
        }
    }
}

std::size_t wavToneStorage(const CompositeTimbre& tone) {
    std::size_t bytes = sizeof(tone) + tone.name.capacity() + tone.memo.capacity()
        + tone.layers.capacity() * sizeof(CompositeLayer)
        + tone.embedded_timbres.capacity() * sizeof(SavedTimbreReference);
    for (const auto& layer : tone.layers) {
        bytes += layer.timbre_automation.capacity() * sizeof(EnvelopeEvent)
            + layer.volume_envelope.events.capacity() * sizeof(EnvelopeEvent)
            + layer.pitch_envelope.events.capacity() * sizeof(EnvelopeEvent)
            + layer.name.capacity() + layer.opll_tl_auto.free_curve.capacity()
            + layer.opll_fb_auto.free_curve.capacity();
        for (const auto& event : layer.timbre_automation)
            if (event.scc_morph.explicit_plan)
                bytes += event.scc_morph.explicit_plan->points.capacity() * sizeof(SccMorphPoint);
    }
    return bytes;
}

CompositeTimbre wavRenderCacheKey(const SccMorphCompileResult& compiled) {
    auto key = compiled.timbre;
    // All distributions have already become quantized runtime events. Their
    // authoring labels cannot affect PCM and must not prevent exact reuse.
    for (auto& layer : key.layers)
        for (auto& event : layer.timbre_automation) event.scc_morph = {};
    return key;
}

bool sameWavModeComparison(const CompositeTimbre& first, const CompositeTimbre& second) {
    auto a = first, b = second;
    for (auto* tone : {&a, &b}) for (auto& layer : tone->layers)
        for (auto& event : layer.timbre_automation) {
            event.scc_morph.distribution_mode = SccMorphDistributionMode::AdaptiveDistribution;
            event.scc_morph.explicit_plan.reset();
        }
    return a == b;
}

double loopJoinLoss(const CompositeTimbre& tone, std::span<const float> reference,
                    std::span<const float> output, double frequency, const SourceAnalysis& analysis,
                    std::size_t keyOffFrame, const std::atomic_bool* cancel, std::size_t maximumSupportedHarmonic) {
    double error{};
    std::size_t joins{};
    const double referenceRms = rms(reference), outputRms = rms(output);
    for (const auto& layer : tone.layers) {
        const auto& timeline = layer.envelope_timeline;
        if (!timeline.loop_start_count || !timeline.loop_end_count
            || *timeline.loop_end_count <= *timeline.loop_start_count) continue;
        const auto end = static_cast<std::size_t>(*timeline.loop_end_count) * 800;
        const auto period = static_cast<std::size_t>(*timeline.loop_end_count - *timeline.loop_start_count) * 800;
        const auto limit = std::min(keyOffFrame, output.size());
        const auto sourceStart = analysis.sustain_region
            ? static_cast<std::size_t>((analysis.sustain_region->selection.begin - analysis.selection.begin)
                * static_cast<double>(rate) / analysis.source->sample_rate) : end > period ? end - period : 0;
        const auto sourceEnd = analysis.sustain_region
            ? static_cast<std::size_t>((analysis.sustain_region->selection.end - analysis.selection.begin)
                * static_cast<double>(rate) / analysis.source->sample_rate) : end;
        const auto baselineStart = std::min(reference.size() - 1, sourceStart);
        const auto baselineEnd = std::min(reference.size() - 1, sourceEnd > 400 ? sourceEnd - 400 : sourceEnd);
        auto xs = spectrum(reference, baselineStart, 1024, 1), xe = spectrum(reference, baselineEnd, 1024, 1);
        normalizePitchAxis(xs, &analysis, baselineStart, frequency);
        normalizePitchAxis(xe, &analysis, baselineEnd, frequency);
        double sourceChange{}, sourceEnergy{};
        for (std::size_t k = 1; k < xs.size(); ++k) {
            const double weight = representableWeight(&analysis, baselineEnd, k, 1024,
                frequency, xe[k], 0, maximumSupportedHarmonic);
            sourceChange += weight * std::pow(xs[k] - xe[k], 2);
            sourceEnergy += weight * (xs[k] * xs[k] + xe[k] * xe[k]);
        }
        const auto sourceChunk = std::min<std::size_t>({400, reference.size() - baselineStart, reference.size() - baselineEnd});
        const double sourceDelta = std::abs(rms(reference.subspan(baselineStart, sourceChunk))
            - rms(reference.subspan(baselineEnd, sourceChunk))) / (referenceRms + epsilon);
        // Inspect PCM on both sides of repeated runtime loop traversals, rather
        // than comparing authoring endpoints. Cap work even for one-tick loops.
        std::size_t traversals{};
        for (auto seam = end; seam + 400 < limit && traversals < 3; seam += period, ++traversals) {
            if (cancelled(cancel)) return std::numeric_limits<double>::infinity();
            const auto before = seam >= 400 ? seam - 400 : 0;
            const auto after = seam + 400;
            const auto ys = spectrum(output, before, 1024, 1), ye = spectrum(output, after, 1024, 1);
            double outputChange{}, energy{};
            for (std::size_t k = 1; k < ys.size(); ++k) {
                const double weight = representableWeight(&analysis, seam, k, 1024, frequency, xe[k], ye[k], maximumSupportedHarmonic);
                outputChange += weight * std::pow(ys[k] - ye[k], 2);
                energy += weight * (ys[k] * ys[k] + ye[k] * ye[k]);
            }
            error += std::max(0.0, std::sqrt(outputChange / (energy + epsilon))
                - std::sqrt(sourceChange / (sourceEnergy + epsilon)));
            const double outputDelta = std::abs(rms(output.subspan(seam - 400, 400)) - rms(output.subspan(seam, 400)));
            error += std::max(0.0, outputDelta / (outputRms + epsilon)
                - sourceDelta);
            ++joins;
        }
    }
    return error / std::max<std::size_t>(1, joins);
}
} // namespace

static CompositeWavQualityMetrics evaluateCompositeWavePcmImpl(
    std::span<const float> reference, std::span<const float> rendered,
    double frequency, std::size_t attackEnd, std::size_t waves,
    const CompositeWavQualityWeights& weights, const std::atomic_bool* cancel,
    const SourceAnalysis* analysis, std::size_t maximumSupportedHarmonic) {
    CompositeWavQualityMetrics metrics;
    const auto count = std::min(reference.size(), rendered.size());
    if (!count || !std::isfinite(frequency) || frequency <= 0
        || !maximumSupportedHarmonic || maximumSupportedHarmonic > 128
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
            normalizePitchAxis(x, analysis, centre, frequency);
            std::array<double, 40> erbX{}, erbY{};
            const double erbMax = 21.4 * std::log10(1 + 0.00437 * rate / 2);
            for (std::size_t k = 1; k < x.size(); ++k) {
                const double d = x[k] - y[k];
                const double weight = representableWeight(analysis, centre, k, window, frequency, x[k], y[k], maximumSupportedHarmonic);
                difference += weight * d * d; energy += weight * x[k] * x[k];
                const double significance = weight * std::max(x[k], y[k]);
                logDifference += significance * std::abs(std::log(x[k] + 1e-5) - std::log(y[k] + 1e-5));
                logWeight += significance;
                const double hz = static_cast<double>(k * rate) / window;
                const auto band = std::min<std::size_t>(39, static_cast<std::size_t>(
                    40 * 21.4 * std::log10(1 + 0.00437 * hz) / erbMax));
                erbX[band] += weight * x[k] * x[k]; erbY[band] += weight * y[k] * y[k];
            }
            if (window == 4096) {
                double harmonicDifference{}, harmonicEnergy{}, erbDifference{}, erbEnergy{};
                for (std::size_t h = 1; h <= 16 && h * frequency < rate / 2; ++h) {
                    const auto bin = static_cast<std::size_t>(std::lround(h * frequency * window / rate));
                    double a{}, b{};
                    for (std::size_t k = bin > 1 ? bin - 1 : 0; k <= std::min(bin + 1, x.size() - 1); ++k) {
                        a += x[k] * x[k]; b += y[k] * y[k];
                    }
                    const double weight = representableWeight(analysis, centre, bin, window, frequency,
                        std::sqrt(a), std::sqrt(b), maximumSupportedHarmonic);
                    harmonicDifference += weight * std::pow(std::sqrt(a) - std::sqrt(b), 2);
                    harmonicEnergy += weight * a;
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
        auto x = spectrum(reference, centre, 256, xnorm);
        const auto y = spectrum(rendered, centre, 256, ynorm);
        normalizePitchAxis(x, analysis, centre, frequency);
        for (std::size_t k = 1; k < x.size(); ++k) {
            const double weight = representableWeight(analysis, centre, k, 256, frequency, x[k], y[k], maximumSupportedHarmonic);
            attackDifference += weight * std::pow(x[k] - y[k], 2);
            attackEnergy += weight * x[k] * x[k];
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

CompositeWavQualityMetrics evaluateCompositeWavePcm(
    std::span<const float> reference, std::span<const float> rendered,
    double frequency, std::size_t attackEnd, std::size_t waves,
    const CompositeWavQualityWeights& weights, const std::atomic_bool* cancel,
    const SourceAnalysis* analysis) {
    return evaluateCompositeWavePcmImpl(reference, rendered, frequency, attackEnd,
        waves, weights, cancel, analysis, 15);
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

namespace {
struct OpllCandidate {
    std::uint8_t rom{};
    std::optional<OpllPatchParameters> original;
};

// Candidate acquisition owns source-specific preparation. The optimizer sees
// only playable snapshots and evaluates all sources through the same Engine.
// There is deliberately no database adapter until that product scope exists.
class OpllCandidateSource {
public:
    virtual ~OpllCandidateSource() = default;
    virtual std::vector<OpllCandidate> acquire(const SourceAnalysis&, int,
                                               const std::atomic_bool*) const = 0;
};
class FixedOpllCandidateSource final : public OpllCandidateSource {
public:
    explicit FixedOpllCandidateSource(std::optional<std::uint8_t> fixed) : fixed_(fixed) {}
    std::vector<OpllCandidate> acquire(const SourceAnalysis& analysis, int note,
                                       const std::atomic_bool* cancel) const override {
        const auto roms = fixed_ ? std::vector<std::uint8_t>{*fixed_}
            : rankFixedRomCandidates(analysis, note, cancel);
        std::vector<OpllCandidate> result;
        for (auto rom : roms) result.push_back({rom, std::nullopt});
        return result;
    }
private:
    std::optional<std::uint8_t> fixed_;
};
class OriginalOpllCandidateSource final : public OpllCandidateSource {
public:
    explicit OriginalOpllCandidateSource(std::uint32_t frame) : frame_(frame) {}
    std::vector<OpllCandidate> acquire(const SourceAnalysis& analysis, int,
                                       const std::atomic_bool* cancel) const override {
        const auto shape = waveAt(analysis, frame_, CompositeWavStrategy::Independent, std::nullopt);
        OpllApproximationOptions approximation;
        approximation.profile = OpllApproximationProfile::Compact;
        approximation.max_workers = 1;
        approximation.control = std::make_shared<OpllApproximationControl>();
        std::jthread bridge([cancel, token = approximation.control](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (cancelled(cancel)) { token->requestCancel(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        auto patches = approximateSccWaveformWithOpllResult(shape.wave, approximation).candidates;
        bridge.request_stop();
        if (patches.empty() && !cancelled(cancel)) patches.push_back(defaultOpllPatch());
        if (patches.size() > 6) patches.resize(6);
        std::vector<OpllCandidate> result;
        for (const auto& patch : patches) result.push_back({0, patch});
        return result;
    }
private:
    std::uint32_t frame_;
};
}
CompositeWavConversionResult convertCompositeWave(
    std::shared_ptr<const SourceAnalysis> analysis, const CompositeWavConversionOptions& options) {
    CompositeWavConversionResult result;
    std::uint64_t finalCandidateId{};
    bool insufficientModeBudget{}, diagnosticLimitReached{};
    result.analysis_reference = analysis;
    const auto started = std::chrono::steady_clock::now();
    const auto control = options.control;
    const auto* cancel = control ? &control->cancel_requested : nullptr;
    const auto stage = [&](CompositeWavStage value) {
        if (control) control->stage.store(value, std::memory_order_relaxed);
    };
    const auto finish = [&]() {
        for (auto& item : result.morph_candidates) {
            item.selected = item.valid && item.candidate_id == finalCandidateId;
            if (item.selected) item.reason = "Selected final feasible whole-composite Engine candidate";
        }
        if (insufficientModeBudget)
            result.warnings.push_back("The remaining evaluation budget cannot cover a matched Adaptive/Time/Tone triplet; no winner is inferred from a partial comparison.");
        if (diagnosticLimitReached)
            result.warnings.push_back("Morph candidate diagnostics reached their bounded 8192-record limit.");
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
    // SCC alone cannot render order16. OPLL composites can retain measured
    // higher coherent partials; source residuals still receive bounded weight.
    const bool includesOpll = options.configuration == CompositeWavConfiguration::SccOpllRom
        || options.configuration == CompositeWavConfiguration::SccOpllOriginal;
    std::size_t harmonicCapacity = 15;
    if (includesOpll) for (const auto& frame : analysis->harmonic_trajectory)
        for (const auto& h : frame.harmonics) harmonicCapacity = std::max<std::size_t>(harmonicCapacity, h.harmonic);
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
    std::stop_source morphCancellation;
    std::jthread morphCancellationBridge;
    if (cancel) morphCancellationBridge = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (cancelled(cancel)) { morphCancellation.request_stop(); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    struct MorphCacheEntry {
        CompositeTimbre key;
        std::shared_ptr<const SccMorphCompileResult> compiled;
    };
    struct RenderCacheEntry {
        CompositeTimbre key;
        std::shared_ptr<const CompositeWavRenderResult> pcm;
        CompositeWavQualityMetrics acoustic_quality;
    };
    std::vector<MorphCacheEntry> morphCache;
    std::vector<RenderCacheEntry> renderCache;
    constexpr std::size_t renderCacheByteLimit = 24 * 1024 * 1024;
    std::size_t plannerScratch{}, poolStorage{};
    using Coordinate = std::pair<std::size_t, std::size_t>;
    std::uint64_t candidateSerial{}, groupSerial{}, activeGroup{};
    std::optional<Coordinate> activeCoordinate;
    std::optional<std::size_t> activeDiagnostic;
    bool comparingModes{}, preferEqual{};
    std::optional<CompositeTimbre> lastEvaluatedTone;
    CompositeWavQualityMetrics lastEvaluatedQuality;
    CompositeWavResourcePlan lastEvaluatedResources;
    std::shared_ptr<const CompositeWavRenderResult> lastEvaluatedPcm;
    std::uint64_t lastEvaluatedId{};
    const auto pcmStorage = [](const CompositeWavRenderResult& pcm) {
        return (pcm.mono_pcm.capacity() + pcm.stereo_pcm.capacity()) * sizeof(float);
    };
    const auto recordStorage = [&](std::size_t extra = 0) {
        std::size_t bytes = reference.capacity() * sizeof(float) + pcmStorage(result.preview)
            + result.morph_candidates.capacity() * sizeof(CompositeWavMorphCandidateDiagnostic)
            + plannerScratch + poolStorage + extra;
        if (result.composite_tone) bytes += wavToneStorage(*result.composite_tone);
        for (const auto& entry : morphCache) {
            bytes += wavToneStorage(entry.key) + wavToneStorage(entry.compiled->timbre)
                + sizeof(SccMorphCompileResult)
                + entry.compiled->plans.capacity() * sizeof(SccMorphCompileResult::SegmentPlan)
                + entry.compiled->channel_allocations.capacity() * sizeof(SccMorphChannelAllocation);
            if (entry.compiled->authored_input) bytes += wavToneStorage(*entry.compiled->authored_input);
            bytes += entry.compiled->diagnostics.capacity() * sizeof(SccMorphWaveDiagnostic);
            for (const auto& plan : entry.compiled->plans) {
                bytes += plan.result.plan.points.capacity() * sizeof(SccMorphPoint)
                    + plan.result.generated.intermediate.capacity() * sizeof(SccWaveform)
                    + plan.result.generated.diagnostics.capacity() * sizeof(SccMorphWaveDiagnostic);
                for (const auto& item : plan.result.considered)
                    bytes += sizeof(item) + item.plan.points.capacity() * sizeof(SccMorphPoint);
            }
        }
        for (const auto& entry : renderCache) bytes += wavToneStorage(entry.key) + pcmStorage(*entry.pcm);
        for (const auto& item : result.morph_candidates)
            bytes += item.plan.points.capacity() * sizeof(SccMorphPoint) + item.reason.capacity();
        result.morph_search.working_set_bytes = std::max(result.morph_search.working_set_bytes, bytes);
    };
    const auto appendDiagnostic = [&](const CompositeTimbre& tone, Coordinate coordinate) -> std::optional<std::size_t> {
        if (result.morph_candidates.size() >= 8192) { diagnosticLimitReached = true; return std::nullopt; }
        const auto [l, e] = coordinate;
        const auto& events = tone.layers[l].timbre_automation;
        const auto& morph = events[e].scc_morph;
        CompositeWavMorphCandidateDiagnostic item;
        item.configuration = options.configuration;
        item.candidate_id = candidateSerial;
        item.comparison_group = activeGroup;
        item.layer_index = l;
        item.destination_event_index = e;
        item.start_count = e ? events[e - 1].count : 0;
        item.end_count = events[e].count;
        item.distribution = morph.distribution_mode;
        item.intermediate_count = morph.intermediate_count;
        item.curve = morph.curve;
        if (morph.explicit_plan) item.plan = *morph.explicit_plan;
        item.reason = "Not evaluated";
        result.morph_candidates.push_back(std::move(item));
        return result.morph_candidates.size() - 1;
    };
    const auto budget = [&]() {
        return !cancelled(cancel) && result.evaluations < stageLimit
            && (comparingModes || options.stagnation_limit == 0 || stalled < options.stagnation_limit);
    };
    const auto evaluate = [&](CompositeTimbre candidate) {
        lastEvaluatedTone.reset();
        lastEvaluatedPcm.reset();
        if (!budget()) return false;
        ++candidateSerial;
        if (activeCoordinate && !activeDiagnostic) activeDiagnostic = appendDiagnostic(candidate, *activeCoordinate);
        std::vector<std::size_t> diagnosticIndices;
        if (activeDiagnostic) {
            result.morph_candidates[*activeDiagnostic].candidate_id = candidateSerial;
            diagnosticIndices.push_back(*activeDiagnostic);
        }
        const auto reject = [&](const std::string& reason) {
            for (const auto index : diagnosticIndices) result.morph_candidates[index].reason = reason;
            return false;
        };
        if (!comparingModes && result.composite_tone)
            invalidateChangedWavPlans(candidate, *result.composite_tone);
        candidate.scc_morph_algorithm_version = kSccMorphAlgorithmVersion;
        stage(CompositeWavStage::Morph);
        const auto planningStarted = std::chrono::steady_clock::now();
        std::shared_ptr<const SccMorphCompileResult> compiled;
        const auto cachedMorph = std::find_if(morphCache.begin(), morphCache.end(),
            [&](const auto& entry) { return entry.key == candidate; });
        if (cachedMorph != morphCache.end()) {
            ++result.morph_search.morph_cache_hits;
            compiled = cachedMorph->compiled;
            if (compiled->valid && compiled->authored_input) candidate = *compiled->authored_input;
        } else {
            ++result.morph_search.morph_cache_misses;
            const auto key = candidate;
            auto preparation = compileSccMorph(candidate, morphCancellation.get_token());
            if (preparation.valid) {
                bool changed{};
                for (const auto& segment : preparation.plans) {
                    auto& settings = candidate.layers[segment.layer_index]
                        .timbre_automation[segment.destination_event_index].scc_morph;
                    changed = changed || settings.explicit_plan != segment.result.plan;
                    settings.explicit_plan = segment.result.plan;
                    plannerScratch = std::max(plannerScratch, segment.result.working_set_bytes);
                }
                // Seed the normal preparation cache with the *saved* authoring
                // model. The renderer, editor and serializer then all see the
                // exact same confirmed plan and block phase policy.
                if (changed) preparation = compileSccMorph(candidate, morphCancellation.get_token());
            }
            compiled = std::make_shared<const SccMorphCompileResult>(std::move(preparation));
            if (morphCache.size() == 8) morphCache.erase(morphCache.begin());
            morphCache.push_back({key, compiled});
        }
        result.morph_search.planning_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - planningStarted).count();
        if (!compiled->valid) return reject(compiled->error);
        if (cancelled(cancel)) return reject("Cancelled during common Morph planning");
        (void)seedSccMorphCache(candidate, compiled);
        const auto resources = resourcePlan(compiled->timbre);
        if (!resources.scc_waveforms || resources.scc_waveforms > options.max_scc_waveforms)
            return reject("Whole-composite SCC Bank requirement exceeds the requested waveform budget");
        if (!activeCoordinate) for (const auto& segment : compiled->plans) {
            const auto index = appendDiagnostic(candidate, {segment.layer_index, segment.destination_event_index});
            if (index) diagnosticIndices.push_back(*index);
        }
        for (const auto index : diagnosticIndices) {
            auto& item = result.morph_candidates[index];
            const auto& settings = candidate.layers[item.layer_index].timbre_automation[item.destination_event_index].scc_morph;
            if (settings.explicit_plan) item.plan = *settings.explicit_plan;
            item.bank_waveforms = resources.scc_waveforms;
        }
        stage(CompositeWavStage::Evaluation);
        const auto evaluationStarted = std::chrono::steady_clock::now();
        const auto renderKey = wavRenderCacheKey(*compiled);
        const auto cachedRender = std::find_if(renderCache.begin(), renderCache.end(),
            [&](const auto& entry) { return entry.key == renderKey; });
        std::shared_ptr<const CompositeWavRenderResult> rendered;
        CompositeWavQualityMetrics quality;
        const bool reusedRender = cachedRender != renderCache.end();
        if (reusedRender) {
            ++result.morph_search.render_cache_hits;
            rendered = cachedRender->pcm;
            quality = cachedRender->acoustic_quality;
        } else {
            ++result.morph_search.render_cache_misses;
            rendered = std::make_shared<const CompositeWavRenderResult>(renderCompositeWav(candidate, rendering));
        }
        ++result.evaluations;
        if (control) control->evaluations.store(result.evaluations, std::memory_order_relaxed);
        if (!rendered->ok()) { ++stalled; return reject("Actual Engine render failed or was cancelled"); }
        if (!reusedRender) quality = evaluateCompositeWavePcmImpl(reference, rendered->mono_pcm,
            frequency, attackFrame, resources.scc_waveforms, qualityWeights, cancel, analysis.get(), harmonicCapacity);
        std::size_t probeFrames{};
        if (!reusedRender) for (const auto& layer : candidate.layers) {
            const auto& t = layer.envelope_timeline;
            if (!t.loop_start_count || !t.loop_end_count || *t.loop_end_count <= *t.loop_start_count) continue;
            const auto end = static_cast<std::size_t>(*t.loop_end_count) * 800;
            const auto period = static_cast<std::size_t>(*t.loop_end_count - *t.loop_start_count) * 800;
            if (end + period + 400 >= rendered->effective_key_off_frame)
                probeFrames = std::max(probeFrames, std::min(CompositeWavRenderOptions::kMaximumFrameCount,
                    end + 2 * period + 801));
        }
        std::optional<CompositeWavRenderResult> loopProbe;
        if (probeFrames && !cancelled(cancel)) {
            auto probeOptions = rendering;
            probeOptions.frame_count = probeFrames;
            probeOptions.key_off_frame = probeFrames;
            loopProbe = renderCompositeWav(candidate, probeOptions);
            ++result.loop_probe_renders;
            if (!loopProbe->ok()) { ++stalled; return reject("Held-note loop Engine probe failed or was cancelled"); }
        }
        const auto& loopOutput = loopProbe ? *loopProbe : *rendered;
        const double loopLoss = reusedRender ? 0 : loopJoinLoss(candidate, reference, loopOutput.mono_pcm, frequency, *analysis,
            loopOutput.effective_key_off_frame, cancel, harmonicCapacity);
        quality.transition += loopLoss;
        quality.total += qualityWeights.transition * loopLoss;
        recordStorage(wavToneStorage(candidate) + wavToneStorage(renderKey)
            + (reusedRender ? 0 : pcmStorage(*rendered)) + (loopProbe ? pcmStorage(*loopProbe) : 0));
        if (!reusedRender && std::isfinite(quality.total) && pcmStorage(*rendered) <= renderCacheByteLimit) {
            std::size_t retained = pcmStorage(*rendered);
            for (const auto& entry : renderCache) retained += pcmStorage(*entry.pcm);
            while (!renderCache.empty() && (renderCache.size() >= 2 || retained > renderCacheByteLimit)) {
                retained -= pcmStorage(*renderCache.front().pcm);
                renderCache.erase(renderCache.begin());
            }
            renderCache.push_back({renderKey, rendered, quality});
        }
        std::size_t events{};
        for (const auto& layer : candidate.layers)
            events += layer.volume_envelope.events.size() + layer.timbre_automation.size();
        quality.complexity += static_cast<double>(events) / 253;
        quality.total += qualityWeights.complexity * static_cast<double>(events) / 253;
        result.morph_search.evaluation_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - evaluationStarted).count();
        if (!std::isfinite(quality.total)) { ++stalled; return reject("Nonfinite WAV objective or cancelled evaluation"); }
        lastEvaluatedTone = candidate;
        lastEvaluatedQuality = quality;
        lastEvaluatedResources = resources;
        lastEvaluatedPcm = rendered;
        lastEvaluatedId = candidateSerial;
        for (const auto index : diagnosticIndices) {
            auto& item = result.morph_candidates[index];
            item.valid = true;
            item.quality = quality;
            item.reason = "Valid full-composite Engine evaluation; not the final selected candidate";
        }
        if (!result.composite_tone) result.initial_loss = quality.total;
        const double tolerance = comparingModes ? std::min(options.minimum_improvement, 1e-9) : options.minimum_improvement;
        const bool equalDefault = preferEqual && result.composite_tone
            && std::abs(quality.total - result.best_loss) <= 1e-9
            && sameWavModeComparison(candidate, *result.composite_tone);
        if (!result.composite_tone || quality.total + tolerance < result.best_loss || equalDefault) {
            result.best_loss = quality.total;
            bestStrategy = currentStrategy;
            result.quality = quality;
            result.composite_tone = std::move(candidate);
            finalCandidateId = candidateSerial;
            for (const auto index : diagnosticIndices) result.morph_candidates[index].accepted = true;
            result.resource_plan = resources;
            result.preview = *rendered;
            stalled = 0;
            return true;
        }
        ++stalled;
        return false;
    };
    struct ModeCandidate {
        CompositeTimbre tone;
        double score{};
    };
    struct SegmentPoolKey {
        std::size_t layer{}, event{};
        std::uint32_t start{}, end{};
        std::array<std::uint8_t, 32> first{}, second{};
        bool preceding_morph{}, following_morph{}, first_after_loop{}, second_after_loop{};
        std::optional<std::uint32_t> loop_start, loop_end;
        bool operator==(const SegmentPoolKey&) const = default;
    };
    struct SegmentPool {
        SegmentPoolKey key;
        std::vector<SccMorphTransition> choices;
    };
    std::vector<SegmentPool> segmentPools;
    const auto segmentKey = [&](const CompositeTimbre& tone, Coordinate coordinate) -> std::optional<SegmentPoolKey> {
        const auto [l, e] = coordinate;
        if (l >= tone.layers.size()) return std::nullopt;
        const auto& layer = tone.layers[l];
        const auto& events = layer.timbre_automation;
        if (!e || e >= events.size() || !events[e].scc_morph.enabled) return std::nullopt;
        const auto* first = findEmbeddedTimbreSnapshot(tone, events[e - 1].target_library_id);
        const auto* second = findEmbeddedTimbreSnapshot(tone, events[e].target_library_id);
        if (!first || !second) return std::nullopt;
        return SegmentPoolKey{l, e, events[e - 1].count, events[e].count,
            first->scc_waveform, second->scc_waveform, events[e - 1].scc_morph.enabled,
            e + 1 < events.size() && events[e + 1].scc_morph.enabled,
            events[e - 1].after_loop_start, events[e].after_loop_start,
            layer.envelope_timeline.loop_start_count, layer.envelope_timeline.loop_end_count};
    };
    const auto combineRetainedSegments = [&](std::vector<ModeCandidate>* retained) {
        if (!result.composite_tone || cancelled(cancel)) return;
        const auto base = *result.composite_tone;
        std::erase_if(segmentPools, [&](const auto& pool) {
            return segmentKey(base, {pool.key.layer, pool.key.event}) != pool.key;
        });
        if (segmentPools.size() < 2) return;
        // Exactly two extra global proposals per completed group. Each alters
        // at least two intervals together, including an alternative N/curve
        // when retained, and is never selected by stale per-interval scores.
        for (std::size_t variant = 0; variant < 2 && budget(); ++variant) {
            auto candidate = base;
            std::size_t changed{};
            for (std::size_t offset = 0; offset < segmentPools.size() && changed < 2; ++offset) {
                const auto& pool = segmentPools[(offset + groupSerial) % segmentPools.size()];
                auto& settings = candidate.layers[pool.key.layer].timbre_automation[pool.key.event].scc_morph;
                for (std::size_t choice = 0; choice < pool.choices.size(); ++choice) {
                    const auto& alternative = pool.choices[(choice + variant + offset) % pool.choices.size()];
                    if (alternative == settings) continue;
                    if (variant == 1 && alternative.intermediate_count > settings.intermediate_count) continue;
                    settings = alternative;
                    ++changed;
                    break;
                }
            }
            if (changed < 2) continue;
            const auto before = result.evaluations;
            const bool accepted = evaluate(std::move(candidate));
            result.morph_search.global_combination_trials += result.evaluations - before;
            result.morph_search.global_combination_accepted += accepted ? 1 : 0;
            if (lastEvaluatedTone && retained) retained->push_back({*lastEvaluatedTone, lastEvaluatedQuality.total});
        }
    };
    const auto compareModes = [&](const CompositeTimbre& base, Coordinate coordinate,
                                  std::vector<ModeCandidate>* retained = nullptr) {
        if (cancelled(cancel)) return false;
        if (stageLimit - std::min(stageLimit, result.evaluations) < 3) {
            insufficientModeBudget = true;
            ++result.morph_search.incomplete_comparisons;
            return false;
        }
        const auto [l, e] = coordinate;
        const auto group = ++groupSerial;
        activeGroup = group;
        activeCoordinate = coordinate;
        comparingModes = true;
        constexpr std::array modes{SccMorphDistributionMode::AdaptiveDistribution,
            SccMorphDistributionMode::TimeDistribution, SccMorphDistributionMode::ToneDistribution};
        std::array<std::optional<std::size_t>, 3> records;
        for (std::size_t i = 0; i < modes.size(); ++i) {
            auto candidate = base;
            auto& settings = candidate.layers[l].timbre_automation[e].scc_morph;
            settings.distribution_mode = modes[i];
            settings.explicit_plan.reset();
            records[i] = appendDiagnostic(candidate, coordinate);
            if (records[i]) result.morph_candidates[*records[i]].reason = "Not evaluated: comparison cancelled";
        }
        bool accepted{};
        std::size_t attempted{};
        std::optional<std::size_t> winner;
        std::vector<SccMorphTransition> choices;
        struct EvaluatedMember {
            CompositeTimbre tone;
            CompositeWavQualityMetrics quality;
            CompositeWavResourcePlan resources;
            std::shared_ptr<const CompositeWavRenderResult> pcm;
            std::uint64_t id{};
        };
        std::array<std::optional<EvaluatedMember>, 3> members;
        for (std::size_t i = 0; i < modes.size() && !cancelled(cancel); ++i) {
            auto candidate = base;
            auto& settings = candidate.layers[l].timbre_automation[e].scc_morph;
            settings.distribution_mode = modes[i];
            settings.explicit_plan.reset();
            activeDiagnostic = records[i];
            preferEqual = i == 0;
            bool improved{};
            if (i == 0) {
                const ScopedWavSearchSeconds timer{result.morph_search.adaptive_candidate_seconds};
                improved = evaluate(std::move(candidate));
            } else improved = evaluate(std::move(candidate));
            ++attempted;
            accepted = accepted || improved;
            if (lastEvaluatedTone) {
                ++result.morph_search.mode_trials[i];
                if (improved) ++result.morph_search.mode_accepted[i];
                if (retained) retained->push_back({*lastEvaluatedTone, lastEvaluatedQuality.total});
                choices.push_back(lastEvaluatedTone->layers[l].timbre_automation[e].scc_morph);
                members[i] = EvaluatedMember{*lastEvaluatedTone, lastEvaluatedQuality,
                    lastEvaluatedResources, lastEvaluatedPcm, lastEvaluatedId};
                std::size_t retainedBytes{};
                std::set<const CompositeWavRenderResult*> counted;
                for (const auto& member : members) if (member) {
                    retainedBytes += wavToneStorage(member->tone);
                    const bool alreadyCached = std::any_of(renderCache.begin(), renderCache.end(),
                        [&](const auto& entry) { return entry.pcm == member->pcm; });
                    if (!alreadyCached && counted.insert(member->pcm.get()).second)
                        retainedBytes += pcmStorage(*member->pcm);
                }
                recordStorage(retainedBytes);
            }
        }
        const bool complete = attempted == modes.size() && !cancelled(cancel);
        if (complete) {
            double minimum = std::numeric_limits<double>::infinity();
            for (const auto& member : members)
                if (member) minimum = std::min(minimum, member->quality.total);
            for (std::size_t i = 0; i < members.size(); ++i)
                if (members[i] && members[i]->quality.total <= minimum + 1e-9) {
                    winner = i;
                    break;
                }
        }
        // Resolve ties relative to the absolute group minimum, not a streaming
        // sticky comparison (whose tolerance can otherwise chain). Adopt the
        // chosen member's *existing real PCM* and complete plan atomically;
        // no fourth render/evaluation or metadata-only mode substitution.
        if (complete && winner && result.composite_tone
            && std::any_of(members.begin(), members.end(), [&](const auto& member) {
                return member && member->id == finalCandidateId;
            }) && members[*winner]->quality.total <= result.best_loss + 1e-9) {
            const auto& chosen = *members[*winner];
            result.composite_tone = chosen.tone;
            result.quality = chosen.quality;
            result.best_loss = chosen.quality.total;
            result.resource_plan = chosen.resources;
            result.preview = *chosen.pcm;
            finalCandidateId = chosen.id;
            bestStrategy = currentStrategy;
            accepted = true;
            stalled = 0;
            if (records[*winner] && !result.morph_candidates[*records[*winner]].accepted) {
                result.morph_candidates[*records[*winner]].accepted = true;
                ++result.morph_search.mode_accepted[*winner];
            }
        }
        if (complete) ++result.morph_search.complete_comparisons;
        else ++result.morph_search.incomplete_comparisons;
        for (std::size_t i = 0; i < records.size(); ++i) if (records[i]) {
            auto& item = result.morph_candidates[*records[i]];
            item.comparison_complete = complete;
            item.group_winner = complete && winner == i;
            if (item.valid) item.reason = item.group_winner
                ? "Lowest matched three-mode whole-composite WAV objective (Adaptive-first ties)"
                : "Higher matched whole-composite WAV objective or deterministic tie";
        }
        for (auto& member : members) member.reset();
        activeDiagnostic.reset();
        activeCoordinate.reset();
        activeGroup = 0;
        preferEqual = false;
        comparingModes = false;
        if (complete && !choices.empty()) if (const auto key = segmentKey(base, coordinate)) {
            auto pool = std::find_if(segmentPools.begin(), segmentPools.end(),
                [&](const auto& item) { return item.key == *key; });
            if (pool == segmentPools.end()) {
                if (segmentPools.size() == 64) segmentPools.erase(segmentPools.begin());
                segmentPools.push_back({*key, {}});
                pool = std::prev(segmentPools.end());
            }
            for (const auto& old : pool->choices)
                if (std::find(choices.begin(), choices.end(), old) == choices.end()) choices.push_back(old);
            // Two N/curve sets, each with up to three modes, are sufficient for
            // the bounded cross-interval proposals. Source/phase/loop changes
            // invalidate the pool through its exact key before any reuse.
            if (choices.size() > 6) choices.resize(6);
            pool->choices = std::move(choices);
            result.morph_search.retained_segment_pools = std::max(result.morph_search.retained_segment_pools, segmentPools.size());
            std::size_t storage = segmentPools.capacity() * sizeof(SegmentPool);
            for (const auto& item : segmentPools) {
                storage += item.choices.capacity() * sizeof(SccMorphTransition);
                for (const auto& settings : item.choices)
                    if (settings.explicit_plan) storage += settings.explicit_plan->points.capacity() * sizeof(SccMorphPoint);
            }
            poolStorage = storage;
            recordStorage();
            combineRetainedSegments(retained);
        }
        return accepted;
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
    std::unique_ptr<OpllCandidateSource> candidateSource;
    if (options.configuration == CompositeWavConfiguration::SccOpllOriginal)
        candidateSource = std::make_unique<OriginalOpllCandidateSource>(initialFrame);
    else if (options.configuration == CompositeWavConfiguration::SccOpllRom)
        candidateSource = std::make_unique<FixedOpllCandidateSource>(options.fixed_opll_tone);
    const auto opllCandidates = candidateSource ? candidateSource->acquire(*analysis, note, cancel)
        : std::vector<OpllCandidate>{{0, std::nullopt}};
    // Structural seed comparisons always cover every ROM and every requested
    // automatic role before stagnation termination is enabled.
    for (std::size_t strategyIndex = 0; strategyIndex < strategies.size(); ++strategyIndex) {
        const auto strategy = strategies[strategyIndex];
        currentStrategy = strategy;
        for (const auto& seed : opllCandidates) {
            // Compare every acquired candidate in the first role. Subsequent
            // roles reuse the selected snapshot without depending on its source.
            if (strategyIndex > 0 && result.composite_tone && candidateSource) {
                const auto& selected = result.composite_tone->layers.back();
                if (seed.original ? (!selected.base_timbre
                    || encodeOpllPatch(*seed.original) != selected.base_timbre->opll_registers)
                    : seed.rom != selected.base_opll_rom.value_or(0)) continue;
            }
            if (cancelled(cancel) || result.evaluations >= options.max_evaluations) break;
            stalled = 0;
            auto candidate = generateCandidate(*analysis, options, strategy, keyframes, 0, 1,
                seed.rom, seed.original ? &*seed.original : nullptr, keyoff);
            // Structural seeds preserve the old endpoint/keyframe extraction;
            // only their incoming interpolation uses the common Adaptive mode.
            for (auto& layer : candidate.layers) if (layer.source == TimbreSource::Scc)
                for (std::size_t e = 1; e < layer.timbre_automation.size(); ++e) {
                    auto& morph = layer.timbre_automation[e].scc_morph;
                    morph.enabled = true;
                    const auto duration = layer.timbre_automation[e].count - layer.timbre_automation[e - 1].count;
                    morph.intermediate_count = duration > 1 && options.max_scc_waveforms > 2 ? 1 : 0;
                    const auto start = layer.timbre_automation[e - 1].count;
                    const auto a = waveAt(*analysis, start, strategy, std::nullopt).wave;
                    const auto b = waveAt(*analysis, start + duration, strategy, std::nullopt).wave;
                    const auto mid = waveAt(*analysis, start + duration / 2, strategy, std::nullopt).wave;
                    const auto early = std::sqrt(shapeDistance(a, mid)), late = std::sqrt(shapeDistance(mid, b));
                    morph.curve = early > 1.5 * late ? .5 : late > 1.5 * early ? 2 : 1;
                }
            {
                const ScopedWavSearchSeconds timer{result.morph_search.adaptive_candidate_seconds};
                evaluate(std::move(candidate));
            }
        }
    }
    stalled = 0;
    currentStrategy = bestStrategy;
    // A small feasible beam retains alternatives across intervals and SCC
    // channels. Every extension is compiled by the one common allocator and
    // evaluated as the whole sound; per-interval independent minima are never
    // assembled without a global Bank/phase/resource check. This is bounded
    // search, not a claim of a globally minimal solution.
    if (result.composite_tone) {
        const auto base = *result.composite_tone;
        std::vector<Coordinate> coordinates;
        for (std::size_t l = 0; l < base.layers.size(); ++l)
            if (base.layers[l].source == TimbreSource::Scc)
                for (std::size_t e = 1; e < base.layers[l].timbre_automation.size(); ++e)
                    if (base.layers[l].timbre_automation[e].count < keyoff) coordinates.emplace_back(l, e);
        std::vector<ModeCandidate> beam{{base, result.best_loss}};
        const auto modeBudget = std::max<std::size_t>(3, options.max_evaluations / 5);
        stageLimit = std::min(options.max_evaluations, result.evaluations + modeBudget);
        for (const auto coordinate : coordinates) {
            std::vector<ModeCandidate> pool;
            for (std::size_t parent = 0; parent < beam.size(); ++parent) {
                if (stageLimit - std::min(stageLimit, result.evaluations) < 3 || cancelled(cancel)) break;
                auto proposal = beam[parent].tone;
                const auto [l, e] = coordinate;
                auto& morph = proposal.layers[l].timbre_automation[e].scc_morph;
                const auto duration = proposal.layers[l].timbre_automation[e].count
                    - proposal.layers[l].timbre_automation[e - 1].count;
                // Retained parents also offer neighboring budget/curve choices.
                // Each such choice receives its own matched three-way group.
                if (parent == 1 && duration > morph.intermediate_count + 1U)
                    morph.intermediate_count = static_cast<std::uint8_t>(std::min<int>(30, morph.intermediate_count + 1));
                if (parent == 2) {
                    if (morph.intermediate_count) --morph.intermediate_count;
                    const auto a = waveAt(*analysis, proposal.layers[l].timbre_automation[e - 1].count,
                        bestStrategy, std::nullopt).wave;
                    const auto b = waveAt(*analysis, proposal.layers[l].timbre_automation[e].count,
                        bestStrategy, std::nullopt).wave;
                    const auto mid = waveAt(*analysis, proposal.layers[l].timbre_automation[e - 1].count + duration / 2,
                        bestStrategy, std::nullopt).wave;
                    const auto early = std::sqrt(shapeDistance(a, mid)), late = std::sqrt(shapeDistance(mid, b));
                    morph.curve = early > 1.5 * late ? .5 : late > 1.5 * early ? 2 : 1;
                }
                compareModes(proposal, coordinate, &pool);
            }
            if (pool.empty()) break;
            std::stable_sort(pool.begin(), pool.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
            beam.clear();
            for (auto& item : pool) {
                if (std::any_of(beam.begin(), beam.end(), [&](const auto& old) { return old.tone == item.tone; })) continue;
                beam.push_back(std::move(item));
                if (beam.size() == 3) break;
            }
            result.morph_search.candidate_pool_peak = std::max(result.morph_search.candidate_pool_peak, beam.size());
            std::size_t storage{};
            for (const auto& item : pool) storage += wavToneStorage(item.tone);
            for (const auto& item : beam) storage += wavToneStorage(item.tone);
            recordStorage(storage);
        }
        if (!coordinates.empty() && !result.morph_search.complete_comparisons) insufficientModeBudget = true;
    }
    stalled = 0;
    stageLimit = options.max_evaluations;
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
    // Reserve most of the budget for all six SCC/ENV/time search families.
    // OPLL preparation and original-patch refinement may not starve them.
    stageLimit = std::min(options.max_evaluations, result.evaluations + options.max_evaluations / 4);
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
    stageLimit = options.max_evaluations;
    stalled = 0;
    constexpr auto familyCount = static_cast<std::size_t>(CompositeWavSearchFamily::Count);
    std::array<std::size_t, familyCount> cursors{};
    // Each round visits every family before another coordinate in that family.
    // Invalid allocations consume an attempt, not a render evaluation, and are
    // separately bounded so small/full banks cannot create an endless search.
    const auto refine = [&](CompositeWavSearchFamily family, std::size_t cursor) {
        if (!result.composite_tone) return;
        auto candidate = *result.composite_tone;
        std::vector<Coordinate> toneCoordinates, intervals, envCoordinates;
        std::vector<std::size_t> sccLayers, loopLayers;
        for (std::size_t l = 0; l < candidate.layers.size(); ++l) {
            const auto& layer = candidate.layers[l];
            if (layer.source == TimbreSource::Scc) {
                sccLayers.push_back(l);
                for (std::size_t e = 0; e < layer.timbre_automation.size(); ++e) {
                    toneCoordinates.emplace_back(l, e);
                    if (e && layer.timbre_automation[e].count < keyoff) intervals.emplace_back(l, e);
                }
            }
            for (std::size_t e = 0; e < layer.volume_envelope.events.size(); ++e)
                if (layer.volume_envelope.events[e].kind == EnvelopeEventKind::Volume)
                    envCoordinates.emplace_back(l, e);
            if (layer.envelope_timeline.loop_start_count && layer.envelope_timeline.loop_end_count)
                loopLayers.push_back(l);
        }
        switch (family) {
        case CompositeWavSearchFamily::MorphInterval: {
            if (intervals.empty()) return;
            const auto [l, e] = intervals[(cursor / 8) % intervals.size()];
            auto& morph = candidate.layers[l].timbre_automation[e].scc_morph;
            auto before = morph;
            before.explicit_plan.reset();
            morph.enabled = true;
            if (cursor % 8 >= 6) {
                const ScopedWavSearchSeconds timer{result.morph_search.adaptive_refinement_seconds};
                // Refine a confirmed common plan using only WAV feature goals.
                // The common generator validates and quantizes this proposal;
                // final acceptance always measures the combined Engine PCM.
                const auto& events = candidate.layers[l].timbre_automation;
                const auto* aRef = findEmbeddedTimbreSnapshot(candidate, events[e - 1].target_library_id);
                const auto* bRef = findEmbeddedTimbreSnapshot(candidate, events[e].target_library_id);
                if (!aRef || !bRef || !morph.intermediate_count) return;
                SccWaveform a{}, b{};
                for (std::size_t i = 0; i < a.size(); ++i) {
                    a[i] = std::bit_cast<std::int8_t>(aRef->scc_waveform[i]);
                    b[i] = std::bit_cast<std::int8_t>(bRef->scc_waveform[i]);
                }
                const auto duration = events[e].count - events[e - 1].count;
                auto planned = planSccMorph(a, b, morph.intermediate_count, morph.curve, duration,
                    SccMorphDistributionMode::AdaptiveDistribution, morph.explicit_plan,
                    morphCancellation.get_token(), false);
                if (!planned.valid || cancelled(cancel)) return;
                plannerScratch = std::max(plannerScratch, planned.working_set_bytes);
                auto plan = planned.plan;
                const auto index = 1 + (cursor / (8 * intervals.size())) % morph.intermediate_count;
                auto& point = plan.points[index];
                const auto part = options.configuration == CompositeWavConfiguration::SccScc
                    ? std::optional<std::size_t>(l) : std::nullopt;
                const auto targetPosition = [&](std::uint32_t tick) {
                    const auto target = waveAt(residualAnalysis ? *residualAnalysis : *analysis,
                        events[e - 1].count + tick, bestStrategy, part).wave;
                    const auto from = std::sqrt(shapeDistance(a, target)), to = std::sqrt(shapeDistance(target, b));
                    return from + to > epsilon ? from / (from + to) : point.morph_position;
                };
                if (cursor % 8 == 6) {
                    const double lower = plan.points[index - 1].morph_position + 1e-6;
                    const double upper = plan.points[index + 1].morph_position - 1e-6;
                    if (lower >= upper) return;
                    const auto position = std::clamp((point.morph_position + targetPosition(point.event_count)) / 2,
                        lower, upper);
                    if (std::abs(position - point.morph_position) < 1e-8) return;
                    point.morph_position = position;
                } else {
                    std::uint32_t proposed = point.event_count;
                    double error = std::abs(targetPosition(proposed) - point.morph_position);
                    for (int direction : {-1, 1}) {
                        const auto count = static_cast<std::int64_t>(point.event_count) + direction;
                        if (count <= plan.points[index - 1].event_count || count >= plan.points[index + 1].event_count) continue;
                        const auto distance = std::abs(targetPosition(static_cast<std::uint32_t>(count)) - point.morph_position);
                        if (distance + 1e-9 < error) { error = distance; proposed = static_cast<std::uint32_t>(count); }
                    }
                    if (proposed == point.event_count) return;
                    point.event_count = proposed;
                }
                morph.distribution_mode = SccMorphDistributionMode::AdaptiveDistribution;
                morph.explicit_plan = std::move(plan);
                ++result.morph_search.adaptive_plan_trials;
                activeCoordinate = Coordinate{l, e};
                const auto evaluations = result.evaluations;
                const bool accepted = evaluate(std::move(candidate));
                activeDiagnostic.reset();
                activeCoordinate.reset();
                const auto familyIndex = static_cast<std::size_t>(family);
                result.search.trials[familyIndex] += result.evaluations - evaluations;
                result.search.accepted[familyIndex] += accepted ? 1 : 0;
                return;
            }
            switch (cursor % 8) {
            case 0: morph.curve = .5; morph.intermediate_count = std::max<std::uint8_t>(1, morph.intermediate_count); break;
            case 1: morph.curve = 2; morph.intermediate_count = std::max<std::uint8_t>(1, morph.intermediate_count); break;
            case 2: morph.curve = 1; morph.intermediate_count = static_cast<std::uint8_t>(std::min<int>(30, morph.intermediate_count + 2)); break;
            case 3: morph.enabled = false; morph.intermediate_count = 0; break;
            case 4: if (morph.intermediate_count) --morph.intermediate_count; break;
            default: morph.curve = 1; morph.intermediate_count = 0; break;
            }
            morph.explicit_plan.reset();
            if (morph == before) return;
            if (morph.enabled) {
                const auto evaluations = result.evaluations;
                const bool accepted = compareModes(candidate, {l, e});
                const auto familyIndex = static_cast<std::size_t>(family);
                result.search.trials[familyIndex] += result.evaluations - evaluations;
                result.search.accepted[familyIndex] += accepted ? 1 : 0;
                return;
            }
            break;
        }
        case CompositeWavSearchFamily::KeyTime: {
            if (sccLayers.empty()) return;
            if (cursor % 3 == 0 || intervals.empty()) {
                const auto l = sccLayers[(cursor / 3) % sccLayers.size()];
                auto& layer = candidate.layers[l];
                std::uint32_t worstTick{};
                double worstError{-1};
                const auto step = std::max<std::uint32_t>(1, (keyoff + 511) / 512);
                for (std::uint32_t tick = 1; tick < keyoff; tick += step) {
                    if (cancelled(cancel)) return;
                    if (std::any_of(layer.timbre_automation.begin(), layer.timbre_automation.end(),
                        [&](const auto& event) { return event.count == tick; })) continue;
                    const auto frame = static_cast<std::size_t>(tick) * 800;
                    if (frame >= reference.size()) break;
                    auto x = spectrum(reference, frame, 1024, 1);
                    normalizePitchAxis(x, analysis.get(), frame, frequency);
                    const auto y = spectrum(result.preview.mono_pcm, frame, 1024, 1);
                    double error{};
                    for (std::size_t k = 1; k < x.size(); ++k)
                        error += representableWeight(analysis.get(), frame, k, 1024, frequency, x[k], y[k])
                            * std::pow(x[k] - y[k], 2);
                    if (error > worstError) { worstError = error; worstTick = tick; }
                }
                if (worstError <= epsilon) return;
                const auto part = options.configuration == CompositeWavConfiguration::SccScc
                    ? std::optional<std::size_t>(l) : std::nullopt;
                const auto shape = waveAt(residualAnalysis ? *residualAnalysis : *analysis,
                    worstTick, bestStrategy, part);
                auto ref = referenceFor(candidate, shape.wave);
                if (!findEmbeddedTimbreSnapshot(candidate, ref.library_id)) candidate.embedded_timbres.push_back(ref);
                EnvelopeEvent event;
                event.kind = EnvelopeEventKind::Timbre;
                event.count = worstTick;
                event.target_library_id = ref.library_id;
                event.scc_morph = {true, 0, 1};
                event.scc_morph.distribution_mode = SccMorphDistributionMode::AdaptiveDistribution;
                layer.timbre_automation.push_back(event);
                std::sort(layer.timbre_automation.begin(), layer.timbre_automation.end(),
                    [](const auto& a, const auto& b) { return a.count < b.count; });
                updateLoopSides(layer);
            } else {
                const auto [l, e] = intervals[(cursor / 3) % intervals.size()];
                auto& events = candidate.layers[l].timbre_automation;
                const auto delta = cursor % 3 == 1 ? -1 : 1;
                const auto tick = static_cast<std::int64_t>(events[e].count) + delta;
                const auto upper = e + 1 < events.size() ? events[e + 1].count : keyoff;
                if (tick <= events[e - 1].count || tick >= upper) return;
                events[e].count = static_cast<std::uint32_t>(tick);
                updateLoopSides(candidate.layers[l]);
            }
            break;
        }
        case CompositeWavSearchFamily::WaveShape: {
            if (toneCoordinates.empty()) return;
            const auto [l, e] = toneCoordinates[(cursor / 2) % toneCoordinates.size()];
            const auto* ref = findEmbeddedTimbreSnapshot(candidate,
                candidate.layers[l].timbre_automation[e].target_library_id);
            if (!ref) return;
            const auto frame = static_cast<std::size_t>(candidate.layers[l].timbre_automation[e].count) * 800;
            auto targetSpectrum = spectrum(reference, frame, 4096, 1);
            normalizePitchAxis(targetSpectrum, analysis.get(), frame, frequency);
            const auto actualSpectrum = spectrum(result.preview.mono_pcm, frame, 4096, 1);
            std::array<std::pair<double, std::size_t>, 15> harmonicErrors{};
            const double targetNorm = std::accumulate(targetSpectrum.begin(), targetSpectrum.end(), epsilon);
            const double actualNorm = std::accumulate(actualSpectrum.begin(), actualSpectrum.end(), epsilon);
            for (std::size_t h = 1; h <= harmonicErrors.size(); ++h) {
                const auto bin = std::min<std::size_t>(targetSpectrum.size() - 1,
                    static_cast<std::size_t>(std::lround(h * frequency * 4096 / rate)));
                harmonicErrors[h - 1] = {std::abs(targetSpectrum[bin] / targetNorm - actualSpectrum[bin] / actualNorm), h};
            }
            std::stable_sort(harmonicErrors.begin(), harmonicErrors.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
            const auto harmonic = harmonicErrors[(cursor / (4 * toneCoordinates.size())) % 15].second;
            const double direction = cursor % 2 ? 1 : -1;
            double cosine{}, sine{};
            SccWaveform wave;
            for (std::size_t i = 0; i < wave.size(); ++i) {
                wave[i] = std::bit_cast<std::int8_t>(ref->scc_waveform[i]);
                const auto phase = 2 * pi * harmonic * i / wave.size();
                cosine += 2.0 * wave[i] * std::cos(phase) / wave.size();
                sine += 2.0 * wave[i] * std::sin(phase) / wave.size();
            }
            const bool phaseCoordinate = (cursor / (2 * toneCoordinates.size())) % 2;
            for (std::size_t i = 0; i < wave.size(); ++i) {
                const double phase = 2 * pi * harmonic * i / wave.size();
                const double delta = phaseCoordinate
                    ? direction * .15 * (cosine * std::sin(phase) - sine * std::cos(phase))
                    : direction * (.15 * (cosine * std::cos(phase) + sine * std::sin(phase))
                        + (std::hypot(cosine, sine) < 2 ? 4 * std::cos(phase) : 0));
                wave[i] = static_cast<std::int8_t>(std::clamp(std::lround(wave[i] + delta), -127L, 127L));
            }
            if (!replaceEventWave(candidate, l, e, wave)) return;
            break;
        }
        case CompositeWavSearchFamily::Envelope: {
            stage(CompositeWavStage::Envelope);
            if (cursor % 6 >= 4) {
                if (sccLayers.empty()) return;
                auto& layer = candidate.layers[sccLayers[(cursor / 6) % sccLayers.size()]];
                const int direction = cursor % 2 ? 1 : -1;
                const auto hang = static_cast<std::uint8_t>(std::clamp<int>(layer.key_off_hang + direction, 0, 255));
                if (hang == layer.key_off_hang) return;
                // Existing MGSDRV-compatible key-off attenuation; only its
                // authored parameter changes, never the runtime or key time.
                layer.key_off_hang = hang;
                break;
            }
            if (envCoordinates.empty()) return;
            if (cursor % 6 >= 2) {
                std::erase_if(envCoordinates, [](const auto& coordinate) { return coordinate.second == 0; });
                if (envCoordinates.empty()) return;
            }
            const auto [l, e] = envCoordinates[(cursor / 6) % envCoordinates.size()];
            auto& events = candidate.layers[l].volume_envelope.events;
            const int direction = cursor % 2 ? 1 : -1;
            if (cursor % 6 < 2) {
                const int value = std::clamp(events[e].value + direction, 0, 15);
                if (value == events[e].value) return;
                events[e].value = value;
            } else {
                if (!e) return;
                const auto tick = static_cast<std::int64_t>(events[e].count) + direction;
                const auto upper = e + 1 < events.size() ? events[e + 1].count : keyoff;
                if (tick <= events[e - 1].count || tick >= upper) return;
                events[e].count = static_cast<std::uint32_t>(tick);
                for (std::size_t i = 1; i < events.size(); ++i)
                    if (events[i].automatic) {
                        const auto interval = events[i].count - events[i - 1].count;
                        events[i].automatic = interval > 1 && interval <= 239;
                    }
            }
            break;
        }
        case CompositeWavSearchFamily::Loop: {
            stage(CompositeWavStage::Envelope);
            if (loopLayers.empty()) return;
            const auto l = loopLayers[(cursor / 4) % loopLayers.size()];
            auto& timeline = candidate.layers[l].envelope_timeline;
            const auto start = static_cast<std::int64_t>(*timeline.loop_start_count);
            const auto end = static_cast<std::int64_t>(*timeline.loop_end_count);
            const int direction = cursor % 2 ? 1 : -1;
            if (cursor % 4 < 2) {
                if (start + direction < 0 || start + direction >= end) return;
                timeline.loop_start_count = static_cast<std::uint32_t>(start + direction);
            } else {
                if (end + direction <= start || end + direction >= keyoff) return;
                timeline.loop_end_count = static_cast<std::uint32_t>(end + direction);
            }
            updateLoopSides(candidate.layers[l]);
            break;
        }
        case CompositeWavSearchFamily::Reduction: {
            if (cursor % 3 == 0 && !intervals.empty()) {
                const auto [l, e] = intervals[(cursor / 3) % intervals.size()];
                candidate.layers[l].timbre_automation.erase(candidate.layers[l].timbre_automation.begin() + e);
            } else if (cursor % 3 == 1 && envCoordinates.size() > candidate.layers.size()) {
                std::erase_if(envCoordinates, [](const auto& coordinate) { return coordinate.second == 0; });
                if (envCoordinates.empty()) return;
                const auto [l, e] = envCoordinates[(cursor / 3) % envCoordinates.size()];
                auto& events = candidate.layers[l].volume_envelope.events;
                events.erase(events.begin() + e);
                for (std::size_t i = 1; i < events.size(); ++i)
                    if (events[i].automatic && events[i].count - events[i - 1].count > 239)
                        events[i].automatic = false;
            } else if (cursor % 3 == 2 && !intervals.empty()) {
                const auto [l, e] = intervals[(cursor / 3) % intervals.size()];
                auto& morph = candidate.layers[l].timbre_automation[e].scc_morph;
                if (!morph.intermediate_count) return;
                --morph.intermediate_count;
            } else return;
            discardUnusedSnapshots(candidate);
            break;
        }
        default: return;
        }
        discardUnusedSnapshots(candidate);
        const auto before = result.evaluations;
        const bool accepted = evaluate(std::move(candidate));
        const auto index = static_cast<std::size_t>(family);
        result.search.trials[index] += result.evaluations - before;
        result.search.accepted[index] += accepted ? 1 : 0;
    };
    const auto attemptLimit = std::min<std::size_t>(32768, options.max_evaluations * 16);
    for (std::size_t attempt = 0; attempt < attemptLimit && budget() && result.composite_tone; ++attempt) {
        const auto family = attempt % familyCount;
        refine(static_cast<CompositeWavSearchFamily>(family), cursors[family]++);
    }
    if (result.composite_tone) {
        result.completion = CompositeWavConversionCompletion::Completed;
        result.warnings = analysis->warnings;
        if (result.morph_search.complete_comparisons)
            result.warnings.push_back("SCC distribution used " + std::to_string(result.morph_search.complete_comparisons)
                + " matched Adaptive/Time/Tone whole-composite comparisons. The feasible candidate beam is bounded to three alternatives; the result is not a global minimum guarantee.");
        else if (std::none_of(result.composite_tone->layers.begin(), result.composite_tone->layers.end(),
            [](const auto& layer) { return layer.source == TimbreSource::Scc && layer.timbre_automation.size() > 1; }))
            result.warnings.push_back("This selection has no SCC morph interval; three-way distribution comparison is not applicable.");
        if (result.loop_probe_renders)
            result.warnings.push_back("Loop continuity also used " + std::to_string(result.loop_probe_renders)
                + " bounded held-note Engine probes; these are separate from complete-candidate evaluations and do not change preview key-off or duration.");
        if (options.configuration == CompositeWavConfiguration::SccOpllRom && !options.fixed_opll_tone)
            result.warnings.push_back("Fixed ROM preparation uses 90 fresh Engine renders at three pitches and two key-on durations; candidate evaluation count excludes this bounded preparation.");
        result.warnings.push_back("A bounded local search cannot reproduce all nonperiodic or high-harmonic source components; compare the rendered audio before applying.");
        if (result.evaluations >= options.max_evaluations)
            result.warnings.push_back("Evaluation budget exhausted; the best valid candidate was retained.");
    } else {
        result.completion = CompositeWavConversionCompletion::NoValidCandidate;
        result.error = "No candidate fits the waveform allocation and MGSC envelope limits";
    }
    recordStorage();
    return finish();
}
} // namespace mgstc::engine
