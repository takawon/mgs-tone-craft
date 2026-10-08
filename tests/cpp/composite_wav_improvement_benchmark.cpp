// SPDX-License-Identifier: AGPL-3.0-only
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/composite_wav_analysis.hpp"
#include "mgstc/engine/composite_wav_conversion.hpp"
#include "mgstc/engine/mgs_composite_io.hpp"
#include "mgstc/engine/scc_morph.hpp"
#include "mgstc/engine/composite_wav_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <optional>
#include <string_view>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
using namespace mgstc::engine;

constexpr std::uint32_t kSampleRate = 48'000;
constexpr double kReferencePitchHz = 261.625565;
constexpr std::size_t kEvaluationBudget = 96;
constexpr std::array<double, 3> kDurations{0.25, 1.0, 2.0};

const char* configurationName(CompositeWavConfiguration value) {
    switch (value) {
    case CompositeWavConfiguration::Scc: return "scc";
    case CompositeWavConfiguration::SccScc: return "scc_scc";
    case CompositeWavConfiguration::SccOpllRom: return "scc_opll_rom";
    case CompositeWavConfiguration::SccOpllOriginal: return "scc_opll_original";
    }
    return "unknown";
}

CompositeWavConfiguration parseConfiguration(const std::string& value) {
    if (value == "scc") return CompositeWavConfiguration::Scc;
    if (value == "scc_scc") return CompositeWavConfiguration::SccScc;
    if (value == "scc_opll_rom") return CompositeWavConfiguration::SccOpllRom;
    if (value == "scc_opll_original") return CompositeWavConfiguration::SccOpllOriginal;
    throw std::runtime_error("unknown --config value: " + value);
}

std::shared_ptr<const SourcePcm> makeFixture(double durationSeconds) {
    auto pcm = std::make_shared<SourcePcm>();
    pcm->sample_rate = kSampleRate;
    pcm->channels = 1;
    pcm->bit_depth = 32;
    pcm->sample_format = SourceSampleFormat::FloatPcm;
    const auto frames = static_cast<std::size_t>(std::llround(durationSeconds * kSampleRate));
    pcm->mono_samples.reserve(frames);
    pcm->interleaved_samples.reserve(frames);
    const auto keyOff = static_cast<std::size_t>(std::llround(frames * 0.72));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double t = static_cast<double>(frame) / kSampleRate;
        const double attack = std::min(1.0, t / 0.012);
        const double release = frame < keyOff
            ? 1.0
            : std::exp(-static_cast<double>(frame - keyOff) / (kSampleRate * 0.055));
        const double phase = 2.0 * std::numbers::pi * kReferencePitchHz * t;
        const double value = 0.34 * attack * release
            * (0.72 * std::sin(phase) + 0.21 * std::sin(2.0 * phase + 0.17)
                + 0.07 * std::sin(3.0 * phase + 0.41));
        const auto sample = static_cast<float>(value);
        pcm->mono_samples.push_back(sample);
        pcm->interleaved_samples.push_back(sample);
    }
    return pcm;
}

std::string csvQuote(const std::string& value) {
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '"') result += "\"\"";
        else result += ch;
    }
    result += '"';
    return result;
}

std::string joinNumbers(const std::vector<std::size_t>& values) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ';';
        out << values[i];
    }
    return out.str();
}

struct WaveCounts {
    std::size_t snapshots{};
    std::set<std::array<std::uint8_t, 32>> unique;
    std::size_t morphTotal{}, morphUnique{}, compiledUnique{};
    std::vector<std::size_t> selectedNs;
    double maxAdjacentSigned{}, maxAdjacentRms{}, worstCircularMinimumRms{}, maxMagnitudeRms{};
    double maxAdjacentHarmonic{}, maxAdjacentLogSpectrum{};
};

SccWaveform signedWave(const SavedTimbreReference& reference) {
    SccWaveform wave{};
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = std::bit_cast<std::int8_t>(reference.scc_waveform[i]);
    return wave;
}

const SavedTimbreReference* findSnapshot(const CompositeTimbre& tone, std::uint64_t id) {
    if (id == 0) return nullptr;
    for (const auto& reference : tone.embedded_timbres)
        if (reference.library_id == id && reference.source == TimbreSource::Scc) return &reference;
    for (const auto& layer : tone.layers)
        if (layer.base_timbre && layer.base_timbre->library_id == id
            && layer.base_timbre->source == TimbreSource::Scc) return &*layer.base_timbre;
    return nullptr;
}

WaveCounts countSccWaves(const CompositeTimbre& tone) {
    WaveCounts counts;
    const auto add = [&](const SavedTimbreReference& reference) {
        if (reference.source != TimbreSource::Scc) return;
        ++counts.snapshots;
        counts.unique.insert(reference.scc_waveform);
    };
    for (const auto& layer : tone.layers)
        if (layer.base_timbre) add(*layer.base_timbre);
    for (const auto& reference : tone.embedded_timbres) add(reference);
    const auto compiled = compileSccMorph(tone);
    if (!compiled.valid) return counts;
    counts.compiledUnique = compiled.used;
    std::set<SccWaveform> uniqueIntermediates;
    for (const auto& plan : compiled.plans) {
        const auto& waves = plan.result.generated.intermediate;
        counts.selectedNs.push_back(waves.size());
        counts.morphTotal += waves.size();
        uniqueIntermediates.insert(waves.begin(), waves.end());
        if (plan.layer_index >= tone.layers.size()) continue;
        const auto& layer = tone.layers[plan.layer_index];
        if (plan.destination_event_index >= layer.timbre_automation.size()) continue;
        const auto& destination = layer.timbre_automation[plan.destination_event_index];
        const auto* to = findSnapshot(tone, destination.target_library_id);
        const SavedTimbreReference* from = layer.base_timbre
            && layer.base_timbre->source == TimbreSource::Scc ? &*layer.base_timbre : nullptr;
        for (std::size_t i = 0; i < plan.destination_event_index; ++i) {
            const auto& event = layer.timbre_automation[i];
            if (event.kind == EnvelopeEventKind::Timbre)
                if (const auto* candidate = findSnapshot(tone, event.target_library_id)) from = candidate;
        }
        if (!from || !to) continue;
        std::vector<SccWaveform> chain{signedWave(*from)};
        chain.insert(chain.end(), waves.begin(), waves.end());
        chain.push_back(signedWave(*to));
        for (std::size_t n = 1; n < chain.size(); ++n) {
            const auto& a = chain[n - 1];
            const auto& b = chain[n];
            double energy{};
            double maximum{};
            for (std::size_t i = 0; i < a.size(); ++i) {
                const double difference = static_cast<double>(a[i]) - b[i];
                energy += difference * difference;
                maximum = std::max(maximum, std::abs(difference));
            }
            counts.maxAdjacentSigned = std::max(counts.maxAdjacentSigned, maximum);
            counts.maxAdjacentRms = std::max(counts.maxAdjacentRms, std::sqrt(energy / a.size()));
            double minimumCircularRms = std::numeric_limits<double>::infinity();
            for (std::size_t shift = 0; shift < b.size(); ++shift) {
                double shiftedEnergy{};
                for (std::size_t i = 0; i < a.size(); ++i) {
                    const double difference = static_cast<double>(a[i]) - b[(i + shift) % b.size()];
                    shiftedEnergy += difference * difference;
                }
                minimumCircularRms = std::min(minimumCircularRms, std::sqrt(shiftedEnergy / a.size()));
            }
            counts.worstCircularMinimumRms = std::max(counts.worstCircularMinimumRms, minimumCircularRms);
            const auto aa = analyzeSccMorphWaveform(a);
            const auto ab = analyzeSccMorphWaveform(b);
            double magnitudeEnergy{};
            for (std::size_t i = 0; i < aa.magnitude.size(); ++i) {
                const double difference = aa.magnitude[i] - ab.magnitude[i];
                magnitudeEnergy += difference * difference;
            }
            counts.maxMagnitudeRms = std::max(counts.maxMagnitudeRms,
                std::sqrt(magnitudeEnergy / aa.magnitude.size()));
            double harmonicEnergy{}, totalHarmonicEnergy{}, logDifference{}, significance{};
            for (std::size_t h = 1; h < 16; ++h) {
                const double difference = aa.magnitude[h] - ab.magnitude[h];
                harmonicEnergy += difference * difference;
                totalHarmonicEnergy += aa.magnitude[h] * aa.magnitude[h] + ab.magnitude[h] * ab.magnitude[h];
                const double weight = std::max(aa.magnitude[h], ab.magnitude[h]);
                logDifference += weight * std::abs(std::log(aa.magnitude[h] + 1e-4)
                    - std::log(ab.magnitude[h] + 1e-4));
                significance += weight;
            }
            counts.maxAdjacentHarmonic = std::max(counts.maxAdjacentHarmonic,
                std::sqrt(harmonicEnergy / (totalHarmonicEnergy + 1e-12)));
            counts.maxAdjacentLogSpectrum = std::max(counts.maxAdjacentLogSpectrum,
                logDifference / (significance + 1e-12));
        }
    }
    std::sort(counts.selectedNs.begin(), counts.selectedNs.end());
    counts.selectedNs.erase(std::unique(counts.selectedNs.begin(), counts.selectedNs.end()), counts.selectedNs.end());
    counts.morphUnique = uniqueIntermediates.size();
    return counts;
}

struct LayerSummary {
    bool looping{};
    std::size_t opllRegisterWrites{};
    int detune{};
    int microDetune{};
};

std::vector<LayerSummary> summarizeLayers(const CompositeTimbre& tone) {
    std::vector<LayerSummary> result;
    result.reserve(tone.layers.size());
    for (const auto& layer : tone.layers) {
        LayerSummary summary;
        summary.detune = layer.detune;
        summary.microDetune = layer.micro_detune;
        summary.looping = layer.envelope_timeline.loop_start_count.has_value()
            && layer.envelope_timeline.loop_end_count.has_value();
        for (const auto& event : layer.volume_envelope.events)
            summary.looping = summary.looping || event.kind == EnvelopeEventKind::LoopStart
                || event.kind == EnvelopeEventKind::LoopEnd;
        if (layer.source == TimbreSource::Opll)
            summary.opllRegisterWrites = static_cast<std::size_t>(std::count_if(
                layer.timbre_automation.begin(), layer.timbre_automation.end(),
                [](const EnvelopeEvent& event) { return event.kind == EnvelopeEventKind::RegisterWrite; }));
        result.push_back(summary);
    }
    return result;
}

std::string joinLayerValues(const std::vector<LayerSummary>& layers, int field) {
    std::ostringstream out;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i) out << ';';
        if (field == 0) out << (layers[i].looping ? 1 : 0);
        else if (field == 1) out << layers[i].opllRegisterWrites;
        else if (field == 2) out << layers[i].detune;
        else out << layers[i].microDetune;
    }
    return out.str();
}

std::optional<CompositeWavQualityMetrics> scoreWithCurrentEngine(
    const CompositeTimbre& tone, const SourceAnalysis& analysis,
    const SourcePcm& source, std::size_t keyOff) {
    CompositeWavRenderOptions options;
    options.frame_count = source.mono_samples.size();
    options.key_off_frame = keyOff;
    options.midi_note = 60;
    const auto rendered = renderCompositeWav(tone, options);
    if (!rendered.ok() || rendered.mono_pcm.size() != source.mono_samples.size())
        return std::nullopt;
    const auto bank = compileSccMorph(tone);
    if (!bank.valid) return std::nullopt;
    return evaluateCompositeWavePcm(source.mono_samples, rendered.mono_pcm,
        analysis.reference_pitch_hz, analysis.attack_region.selection.end,
        bank.used, {}, nullptr, &analysis);
}
std::optional<CompositeTimbre> loadBaseline(const std::filesystem::path& directory,
    double duration, CompositeWavConfiguration configuration) {
    const auto durationText = duration < 1.0 ? "0.25" : duration < 2.0 ? "1.0" : "2.0";
    const auto path = directory / (std::string("duration-") + durationText
        + "-" + configurationName(configuration) + ".mgstc");
    if (!std::filesystem::exists(path)) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) throw std::runtime_error("cannot read baseline output: " + path.string());
    std::string error;
    auto tone = CompositeTimbreLibrary::deserializeTimbreFile(contents, &error);
    if (!tone) throw std::runtime_error("cannot deserialize baseline output " + path.string() + ": " + error);
    return tone;
}
struct MgsCounts {
    std::size_t sccDefinitions{}, envelopeDefinitions{}, envelopeTrackReferences{};
};

MgsCounts countMgsDefinitions(const CompositeTimbre& tone) {
    const auto formatted = formatMgsComposite(tone);
    MgsCounts counts;
    std::istringstream lines(formatted.source);
    std::string line;
    while (std::getline(lines, line)) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto view = std::string_view(line).substr(first);
        if (view.starts_with("@s") && view.find(" = {") != std::string_view::npos)
            ++counts.sccDefinitions;
        if (view.starts_with("@e") && view.find(" = {") != std::string_view::npos)
            ++counts.envelopeDefinitions;
        if (view.find("track=") != std::string_view::npos) {
            std::size_t position{};
            while ((position = view.find("@e", position)) != std::string_view::npos) {
                ++counts.envelopeTrackReferences;
                position += 2;
            }
        }
    }
    return counts;
}
void writeCommonMetrics(std::ostream& out, const std::optional<CompositeWavQualityMetrics>& metrics) {
    if (!metrics) { for (int i = 0; i < 18; ++i) out << ','; return; }
    out << metrics->multi_resolution_stft << ',' << metrics->harmonic << ',' << metrics->erb << ','
        << metrics->attack << ',' << metrics->volume << ',' << metrics->transition << ','
        << metrics->complexity << ',' << metrics->max_wave_sample << ',' << metrics->max_wave_aligned << ','
        << metrics->max_wave_harmonic << ',' << metrics->max_wave_log_spectrum << ',' << metrics->max_wave_rms << ','
        << metrics->max_transition_per_count << ',' << metrics->max_pcm_discontinuity << ','
        << metrics->max_pcm_spectral_change << ',' << metrics->loop_entry << ',' << metrics->loop_boundary << ','
        << metrics->loop_steady << ',' << metrics->total;
}

void writeBaseWaveMetrics(std::ostream& out, const std::optional<WaveCounts>& waves,
                          const std::vector<LayerSummary>& layers) {
    if (!waves) { for (int i = 0; i < 17; ++i) out << ','; return; }
    out << csvQuote(joinNumbers(waves->selectedNs)) << ',' << waves->compiledUnique << ','
        << waves->snapshots << ',' << waves->unique.size() << ','
        << (waves->snapshots - waves->unique.size()) << ','
        << waves->morphTotal << ',' << waves->morphUnique << ','
        << (waves->morphTotal - waves->morphUnique) << ','
        << waves->maxAdjacentSigned << ',' << waves->maxAdjacentRms << ','
        << waves->worstCircularMinimumRms << ',' << waves->maxMagnitudeRms << ','
        << waves->maxAdjacentHarmonic << ',' << waves->maxAdjacentLogSpectrum << ','
        << csvQuote(joinLayerValues(layers, 0)) << ','
        << csvQuote(joinLayerValues(layers, 1)) << ','
        << csvQuote(joinLayerValues(layers, 2)) << ','
        << csvQuote(joinLayerValues(layers, 3));
}
void run(const std::filesystem::path& outputDirectory,
         const std::vector<CompositeWavConfiguration>& configurations,
         const std::optional<std::filesystem::path>& baselineDirectory) {
    std::filesystem::create_directories(outputDirectory);
    std::ofstream csv(outputDirectory / "composite_wav_improvement.csv", std::ios::binary);
    if (!csv) throw std::runtime_error("cannot create benchmark CSV");
    csv << "duration_s,configuration,analysis_s,converter_wall_s,reported_elapsed_s,planning_s,evaluation_s,adaptive_candidate_s,adaptive_refinement_s,wave_generation_s,direct_extraction_s,rendering_s,scoring_s,evaluations,loop_probe_renders,morph_cache_hits,morph_cache_misses,render_cache_hits,render_cache_misses,stft,harmonic,erb,attack,volume,transition,complexity,total,selected_morph_ns,resource_scc_waveforms,scc_snapshot_count,scc_unique_waveforms,scc_reused_snapshots,layer_loop_flags,opll_register_write_counts,layer_detune,layer_micro_detune,search_family_trials,search_family_accepted,morph_mode_trials,morph_mode_accepted,complete_comparisons,incomplete_comparisons,global_combo_trials,global_combo_accepted,retained_segment_pools,candidate_pool_peak,working_set_bytes,scc_morph_intermediate_total,scc_morph_intermediate_unique,scc_morph_intermediate_reused,scc_compiled_unique_bank,max_adjacent_signed_sample,max_adjacent_signed_rms,worst_circular32_minimum_rms,max_adjacent_magnitude_rms,max_adjacent_harmonic,max_adjacent_log_spectrum,source_spectrum_cache_hits,source_spectrum_cache_misses,source_wave_cache_hits,source_wave_cache_misses,source_cache_bytes,same_wave_trials,detune_trials,near_wave_trials,mixed_role_trials,approximate_reuse_trials,approximate_reuse_accepted,dynamic_opll_trials,dynamic_opll_accepted,neighbour_pitch_probe_renders,neighbour_pitch_penalty,opll_contour_probe_renders,opll_counterfactual_probe_renders,source_feature_s,keyframe_s,reuse_s,loop_s,detune_s,opll_s,serialization_s,common_base_stft,common_base_harmonic,common_base_erb,common_base_attack,common_base_volume,common_base_transition,common_base_complexity,common_base_max_wave_sample,common_base_max_wave_aligned,common_base_max_wave_harmonic,common_base_max_wave_log_spectrum,common_base_max_wave_rms,common_base_max_transition,common_base_max_pcm_discontinuity,common_base_max_pcm_spectral_change,common_base_loop_entry,common_base_loop_boundary,common_base_loop_steady,common_base_total,base_scc_definitions,base_envelope_definitions,base_envelope_track_references,base_selected_morph_ns,base_resource_scc_waveforms,base_scc_snapshot_count,base_scc_unique_waveforms,base_scc_reused_snapshots,base_morph_intermediate_total,base_morph_intermediate_unique,base_morph_intermediate_reused,base_max_adjacent_signed_sample,base_max_adjacent_signed_rms,base_worst_circular32_minimum_rms,base_max_adjacent_magnitude_rms,base_max_adjacent_harmonic,base_max_adjacent_log_spectrum,base_layer_loop_flags,base_opll_register_write_counts,base_layer_detune,base_layer_micro_detune,current_scc_definitions,current_envelope_definitions,current_envelope_track_references,common_current_stft,common_current_harmonic,common_current_erb,common_current_attack,common_current_volume,common_current_transition,common_current_complexity,common_current_max_wave_sample,common_current_max_wave_aligned,common_current_max_wave_harmonic,common_current_max_wave_log_spectrum,common_current_max_wave_rms,common_current_max_transition,common_current_max_pcm_discontinuity,common_current_max_pcm_spectral_change,common_current_loop_entry,common_current_loop_boundary,common_current_loop_steady,common_current_total,completion,error\n";
    csv << std::setprecision(10);

    for (const auto duration : kDurations) {
        const auto source = makeFixture(duration);
        SourceAnalysisOptions analysisOptions;
        analysisOptions.reference_pitch_hz = kReferencePitchHz;
        const auto analysisStart = std::chrono::steady_clock::now();
        const auto analyzed = analyzeCompositeWaveSource(source, {0, source->mono_samples.size()}, analysisOptions);
        const auto analysisEnd = std::chrono::steady_clock::now();
        if (analyzed.completion != SourceAnalysisCompletion::Completed || !analyzed.analysis)
            throw std::runtime_error("synthetic analysis failed: " + analyzed.error);
        const double analysisSeconds = std::chrono::duration<double>(analysisEnd - analysisStart).count();
        const auto keyOff = static_cast<std::size_t>(std::llround(source->mono_samples.size() * 0.72));

        for (const auto configuration : configurations) {
            CompositeWavConversionOptions options;
            options.configuration = configuration;
            options.preference = CompositeWavPreference::Balanced;
            options.loop_mode = CompositeWavLoopMode::Automatic;
            options.max_scc_waveforms = 8;
            options.key_off_position = keyOff;
            options.max_evaluations = kEvaluationBudget;
            options.stagnation_limit = 0;
            const auto conversionStart = std::chrono::steady_clock::now();
            const auto result = convertCompositeWave(analyzed.analysis, options);
            const auto conversionEnd = std::chrono::steady_clock::now();
            const double wallSeconds = std::chrono::duration<double>(conversionEnd - conversionStart).count();

            std::vector<std::size_t> selectedNs;
            for (const auto& candidate : result.morph_candidates)
                if (candidate.selected) selectedNs.push_back(candidate.intermediate_count);
            std::sort(selectedNs.begin(), selectedNs.end());
            selectedNs.erase(std::unique(selectedNs.begin(), selectedNs.end()), selectedNs.end());

            WaveCounts waves;
            std::vector<LayerSummary> layers;
            if (result.composite_tone) {
                waves = countSccWaves(*result.composite_tone);
                layers = summarizeLayers(*result.composite_tone);
            }
            std::optional<CompositeWavQualityMetrics> baselineCommonScore;
            std::optional<CompositeWavQualityMetrics> currentCommonScore;
            std::optional<MgsCounts> baselineMgsCounts;
            std::optional<MgsCounts> currentMgsCounts;
            std::optional<WaveCounts> baselineWaves;
            std::vector<LayerSummary> baselineLayers;
            if (result.composite_tone) {
                currentMgsCounts = countMgsDefinitions(*result.composite_tone);
                currentCommonScore = scoreWithCurrentEngine(
                    *result.composite_tone, *analyzed.analysis, *source, keyOff);
            }
            if (baselineDirectory) {
                const auto baselineTone = loadBaseline(*baselineDirectory, duration, configuration);
                if (baselineTone) {
                    baselineMgsCounts = countMgsDefinitions(*baselineTone);
                    baselineWaves = countSccWaves(*baselineTone);
                    baselineLayers = summarizeLayers(*baselineTone);
                    baselineCommonScore = scoreWithCurrentEngine(
                        *baselineTone, *analyzed.analysis, *source, keyOff);
                }
            }
            csv << duration << ',' << configurationName(configuration) << ','
                << analysisSeconds << ',' << wallSeconds << ',' << result.elapsed_seconds << ','
                << result.morph_search.planning_seconds << ',' << result.morph_search.evaluation_seconds << ','
                << result.morph_search.adaptive_candidate_seconds << ','
                << result.morph_search.adaptive_refinement_seconds << ','
                << result.morph_search.wave_generation_seconds << ','
                << result.morph_search.direct_extraction_seconds << ','
                << result.morph_search.rendering_seconds << ','
                << result.morph_search.scoring_seconds << ','
                << result.evaluations << ',' << result.loop_probe_renders << ','
                << result.morph_search.morph_cache_hits << ',' << result.morph_search.morph_cache_misses << ','
                << result.morph_search.render_cache_hits << ',' << result.morph_search.render_cache_misses << ','
                << result.quality.multi_resolution_stft << ',' << result.quality.harmonic << ','
                << result.quality.erb << ',' << result.quality.attack << ',' << result.quality.volume << ','
                << result.quality.transition << ',' << result.quality.complexity << ',' << result.quality.total << ','
                << csvQuote(joinNumbers(selectedNs)) << ',' << result.resource_plan.scc_waveforms << ','
                << waves.snapshots << ',' << waves.unique.size() << ','
                << (waves.snapshots - waves.unique.size()) << ','
                << csvQuote(joinLayerValues(layers, 0)) << ','
                << csvQuote(joinLayerValues(layers, 1)) << ','
                << csvQuote(joinLayerValues(layers, 2)) << ','
                << csvQuote(joinLayerValues(layers, 3)) << ','
                << csvQuote(joinNumbers(std::vector<std::size_t>(result.search.trials.begin(), result.search.trials.end()))) << ','
                << csvQuote(joinNumbers(std::vector<std::size_t>(result.search.accepted.begin(), result.search.accepted.end()))) << ','
                << csvQuote(joinNumbers(std::vector<std::size_t>(result.morph_search.mode_trials.begin(), result.morph_search.mode_trials.end()))) << ','
                << csvQuote(joinNumbers(std::vector<std::size_t>(result.morph_search.mode_accepted.begin(), result.morph_search.mode_accepted.end()))) << ','
                << result.morph_search.complete_comparisons << ',' << result.morph_search.incomplete_comparisons << ','
                << result.morph_search.global_combination_trials << ',' << result.morph_search.global_combination_accepted << ','
                << result.morph_search.retained_segment_pools << ',' << result.morph_search.candidate_pool_peak << ','
                << result.morph_search.working_set_bytes << ','
                << waves.morphTotal << ',' << waves.morphUnique << ',' << (waves.morphTotal - waves.morphUnique) << ',' << waves.compiledUnique << ','
                << waves.maxAdjacentSigned << ',' << waves.maxAdjacentRms << ',' << waves.worstCircularMinimumRms << ',' << waves.maxMagnitudeRms << ',' << waves.maxAdjacentHarmonic << ',' << waves.maxAdjacentLogSpectrum << ','
                << result.morph_search.source_spectrum_cache_hits << ',' << result.morph_search.source_spectrum_cache_misses << ','
                << result.morph_search.source_wave_cache_hits << ',' << result.morph_search.source_wave_cache_misses << ','
                << result.morph_search.source_cache_bytes << ','
                << result.morph_search.same_wave_trials << ',' << result.morph_search.detune_trials << ','
                << result.morph_search.near_wave_trials << ',' << result.morph_search.mixed_role_trials << ','
                << result.morph_search.approximate_reuse_trials << ',' << result.morph_search.approximate_reuse_accepted << ','
                << result.morph_search.dynamic_opll_trials << ',' << result.morph_search.dynamic_opll_accepted << ','
                << result.morph_search.neighbour_pitch_probe_renders << ',' << result.quality.neighbour_pitch_penalty << ',' <<
                result.morph_search.opll_contour_probe_renders << ',' << result.morph_search.opll_counterfactual_probe_renders << ','
                << result.morph_search.source_feature_seconds << ',' << result.morph_search.keyframe_seconds << ','
                << result.morph_search.reuse_seconds << ',' << result.morph_search.loop_seconds << ','
                << result.morph_search.detune_seconds << ',' << result.morph_search.opll_seconds << ','
                << result.morph_search.serialization_seconds << ',';
            writeCommonMetrics(csv, baselineCommonScore);
            csv << ',';
            if (baselineMgsCounts)
                csv << baselineMgsCounts->sccDefinitions << ',' << baselineMgsCounts->envelopeDefinitions
                    << ',' << baselineMgsCounts->envelopeTrackReferences;
            else csv << ",,";
            csv << ',';
            writeBaseWaveMetrics(csv, baselineWaves, baselineLayers);
            csv << ',';
            if (currentMgsCounts)
                csv << currentMgsCounts->sccDefinitions << ',' << currentMgsCounts->envelopeDefinitions
                    << ',' << currentMgsCounts->envelopeTrackReferences;
            else csv << ",,";
            csv << ',';
            writeCommonMetrics(csv, currentCommonScore);
            csv << ',' << static_cast<unsigned>(result.completion) << ',' << csvQuote(result.error) << '\n';

            if (result.composite_tone) {
                const auto durationText = duration < 1.0 ? "0.25" : duration < 2.0 ? "1.0" : "2.0";
                const auto stem = std::string("duration-") + durationText + "-" + configurationName(configuration);
                const auto portable = CompositeTimbreLibrary::serializeTimbreFile(*result.composite_tone, 0);
                std::ofstream sound(outputDirectory / (stem + ".mgstc"), std::ios::binary);
                sound.write(portable.data(), static_cast<std::streamsize>(portable.size()));
                if (!sound) throw std::runtime_error("cannot write serialized benchmark output");
                const auto mgs = formatMgsComposite(*result.composite_tone);
                std::ofstream text(outputDirectory / (stem + ".mml"), std::ios::binary);
                text.write(mgs.source.data(), static_cast<std::streamsize>(mgs.source.size()));
                if (!text) throw std::runtime_error("cannot write MGSC benchmark output");
            }
            std::cout << duration << " s / " << configurationName(configuration) << ": "
                << wallSeconds << " s, " << result.evaluations << " evaluations, score "
                << result.quality.total << '\n';
        }
    }
}

struct Arguments {
    std::filesystem::path output;
    std::vector<CompositeWavConfiguration> configurations;
    std::optional<std::filesystem::path> baseline;
};
Arguments parseArgs(int argc, char** argv) {
    std::filesystem::path output = "composite-wav-improvement-output";
    std::vector<CompositeWavConfiguration> configurations{CompositeWavConfiguration::Scc};
    bool haveExplicitConfig{};
    std::optional<std::filesystem::path> baseline;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--baseline" && i + 1 < argc) {
            baseline = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            if (!haveExplicitConfig) {
                configurations.clear();
                haveExplicitConfig = true;
            }
            configurations.push_back(parseConfiguration(argv[++i]));
        } else {
            throw std::runtime_error("usage: benchmark [--output DIR] [--baseline DIR] [--config scc|scc_scc|scc_opll_rom|scc_opll_original ...]");
        }
    }
    return {output, configurations, baseline};
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parseArgs(argc, argv);
        run(args.output, args.configurations, args.baseline);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "composite WAV improvement benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
