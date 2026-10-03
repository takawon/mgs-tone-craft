// Opt-in regression diagnostics for the user-provided MGSC SCC morph output.
// Included by scc_morph_tests.cpp inside its anonymous namespace.

std::string sccMorphDiagnosticCandidateName(SccMorphCandidate candidate) {
    switch (candidate) {
    case SccMorphCandidate::Source: return "source";
    case SccMorphCandidate::LogHarmonic: return "log_harmonic";
    case SccMorphCandidate::AlignedTime: return "aligned_time";
    case SccMorphCandidate::SpectralEnvelope: return "spectral_envelope";
    case SccMorphCandidate::Blend: return "blend";
    }
    return "unknown";
}

std::string sccMorphDiagnosticCandidateLabel(
    const SccMorphCandidateTrace& candidate) {
    auto label = sccMorphDiagnosticCandidateName(candidate.method);
    if (candidate.method == SccMorphCandidate::Blend) {
        label += "_";
        for (std::size_t i = 0; i < candidate.weights.size(); ++i) {
            if (i) label += "_";
            label += std::to_string(static_cast<int>(std::lround(
                candidate.weights[i] * 100.0)));
        }
    }
    return label;
}

std::optional<std::filesystem::path> sccMorphDiagnosticFixturePath() {
    const std::filesystem::path relative{
        "tests/fixtures/scc_morph_diagnostics/user-output.mml"};
    std::vector<std::filesystem::path> roots;
    auto current = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        roots.push_back(current);
        if (!current.has_parent_path() || current.parent_path() == current) break;
        current = current.parent_path();
    }
    const std::filesystem::path source_file{__FILE__};
    if (source_file.has_parent_path()) {
        auto root = source_file.parent_path().parent_path().parent_path();
        roots.push_back(std::move(root));
    }
    for (const auto& root : roots) {
        const auto candidate = root / relative;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> sccMorphDiagnosticBaselinePath() {
    const std::filesystem::path relative{
        "analysis/scc-morph-phase-b/baseline/scc-morph-trace.csv"};
    auto current = std::filesystem::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = current / relative;
        if (std::filesystem::exists(candidate)) return candidate;
        if (!current.has_parent_path() || current.parent_path() == current) break;
        current = current.parent_path();
    }
    const std::filesystem::path source_file{__FILE__};
    if (source_file.has_parent_path()) {
        const auto root = source_file.parent_path().parent_path().parent_path();
        const auto candidate = root / relative;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return std::nullopt;
}

std::vector<std::string> sccMorphDiagnosticCsvFields(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::vector<std::string> result;
    std::size_t begin{};
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            result.emplace_back(line.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    return result;
}

struct SccMorphSequenceQuality {
    double max_adjacent_sample_rms{};
    double max_level_rms_step{};
    double total_sample_variation{};
    double max_spectral_distance{};
    double total_spectral_variation{};
    double endpoint_spectral_distance{};
    std::size_t distinct_intermediates{};
    std::vector<std::uint32_t> counts;
    std::vector<SccWaveform> waves;
};

double sccMorphDiagnosticSampleRmsDistance(
    const SccWaveform& first, const SccWaveform& second) {
    double sum{};
    for (std::size_t i = 0; i < first.size(); ++i) {
        const double difference = double(first[i]) - double(second[i]);
        sum += difference * difference;
    }
    return std::sqrt(sum / double(first.size()));
}

double sccMorphDiagnosticSpectralDistance(
    const SccWaveform& first, const SccWaveform& second) {
    const auto a = analyzeSccMorphWaveform(first);
    const auto b = analyzeSccMorphWaveform(second);
    double sum{};
    for (std::size_t k = 0; k < a.magnitude.size(); ++k) {
        const double difference = a.magnitude[k] - b.magnitude[k];
        sum += difference * difference;
    }
    return std::sqrt(sum);
}

SccMorphSequenceQuality sccMorphDiagnosticSequenceQuality(
    std::vector<std::uint32_t> counts, std::vector<SccWaveform> waves) {
    SccMorphSequenceQuality result;
    result.counts = std::move(counts);
    result.waves = std::move(waves);
    std::vector<SccWaveform> intermediates;
    if (result.waves.size() > 2) {
        intermediates.assign(result.waves.begin() + 1, result.waves.end() - 1);
    }
    std::sort(intermediates.begin(), intermediates.end());
    result.distinct_intermediates = static_cast<std::size_t>(
        std::distance(intermediates.begin(),
            std::unique(intermediates.begin(), intermediates.end())));
    for (std::size_t i = 1; i < result.waves.size(); ++i) {
        const double sample = sccMorphDiagnosticSampleRmsDistance(
            result.waves[i - 1], result.waves[i]);
        const double spectral = sccMorphDiagnosticSpectralDistance(
            result.waves[i - 1], result.waves[i]);
        result.max_adjacent_sample_rms = std::max(
            result.max_adjacent_sample_rms, sample);
        const auto prior_analysis = analyzeSccMorphWaveform(result.waves[i - 1]);
        const auto current_analysis = analyzeSccMorphWaveform(result.waves[i]);
        result.max_level_rms_step = std::max(result.max_level_rms_step,
            std::abs(prior_analysis.descriptors.rms
                - current_analysis.descriptors.rms));
        result.total_sample_variation += sample;
        result.max_spectral_distance = std::max(
            result.max_spectral_distance, spectral);
        result.total_spectral_variation += spectral;
    }
    if (result.waves.size() >= 2) {
        result.endpoint_spectral_distance = sccMorphDiagnosticSpectralDistance(
            result.waves.front(), result.waves.back());
    }
    return result;
}

std::optional<SccWaveform> sccMorphDiagnosticEventWave(
    const CompositeTimbre& model, const EnvelopeEvent& event);

std::optional<SccMorphSequenceQuality> readSccMorphDiagnosticBaseline() {
    const auto path = sccMorphDiagnosticBaselinePath();
    if (!path) return std::nullopt;
    std::ifstream input(*path, std::ios::binary);
    std::string line;
    if (!std::getline(input, line)) return std::nullopt;
    const auto header = sccMorphDiagnosticCsvFields(line);
    const auto column = [&](std::string_view name) -> std::optional<std::size_t> {
        const auto found = std::find(header.begin(), header.end(), name);
        if (found == header.end()) return std::nullopt;
        return static_cast<std::size_t>(std::distance(header.begin(), found));
    };
    const auto case_column = column("case");
    const auto stage_column = column("stage");
    const auto count_column = column("count");
    const auto number_column = column("output_number");
    if (!case_column || !stage_column || !count_column || !number_column)
        return std::nullopt;
    std::array<std::size_t, 32> sample_columns{};
    for (std::size_t i = 0; i < sample_columns.size(); ++i) {
        const auto sample_column = column("sample" + std::to_string(i));
        if (!sample_column) return std::nullopt;
        sample_columns[i] = *sample_column;
    }
    std::vector<std::uint32_t> counts;
    std::vector<SccWaveform> waves;
    while (std::getline(input, line)) {
        const auto fields = sccMorphDiagnosticCsvFields(line);
        const auto required = *std::max_element(sample_columns.begin(), sample_columns.end());
        if (fields.size() <= required || fields.size() <= *number_column
            || fields[*case_column] != "fixture_reconstruction"
            || fields[*stage_column] != "compiled_global") continue;
        std::uint32_t count{};
        try {
            count = static_cast<std::uint32_t>(std::stoul(fields[*count_column]));
            (void)std::stoul(fields[*number_column]);
        } catch (...) {
            continue;
        }
        SccWaveform wave{};
        bool valid = true;
        for (std::size_t i = 0; i < sample_columns.size(); ++i) {
            try {
                const int sample = std::stoi(fields[sample_columns[i]]);
                if (sample < -128 || sample > 127) { valid = false; break; }
                wave[i] = static_cast<std::int8_t>(sample);
            } catch (...) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        // The baseline file contains the source base only if it was emitted as
        // a compiled event. Keep actual morph events in file order; their count
        // values are the source of truth, including the count-zero start.
        counts.push_back(count);
        waves.push_back(wave);
    }
    if (waves.size() != 29) return std::nullopt;
    return sccMorphDiagnosticSequenceQuality(
        std::move(counts), std::move(waves));
}

SccMorphSequenceQuality sccMorphDiagnosticCompiledQuality(
    const SccMorphCompileResult& compiled) {
    std::vector<std::uint32_t> counts;
    std::vector<SccWaveform> waves;
    if (compiled.timbre.layers.empty()) return {};
    for (const auto& event : compiled.timbre.layers.front().timbre_automation) {
        if (event.kind != EnvelopeEventKind::Timbre) continue;
        const auto wave = sccMorphDiagnosticEventWave(compiled.timbre, event);
        if (!wave) continue;
        counts.push_back(event.count);
        waves.push_back(*wave);
    }
    return sccMorphDiagnosticSequenceQuality(
        std::move(counts), std::move(waves));
}

std::map<unsigned, SccWaveform> sccMorphDiagnosticDefinitions(
    std::string_view source) {
    std::map<unsigned, SccWaveform> result;
    for (const auto& definition : extractMgsSourceDefinitions(source, 's')) {
        const auto parsed = parseMgsSccDefinition(
            "@s" + std::to_string(definition.number)
            + " = {" + definition.body + "}");
        if (parsed) result.emplace(definition.number, parsed->waveform);
    }
    return result;
}

double sccMorphDiagnosticDistance(const SccWaveform& first,
                                  const SccWaveform& second) {
    double distance{};
    for (std::size_t i = 0; i < first.size(); ++i) {
        const double delta = double(first[i]) - double(second[i]);
        distance += delta * delta;
    }
    return distance;
}

std::optional<SccWaveform> sccMorphDiagnosticEventWave(
    const CompositeTimbre& model, const EnvelopeEvent& event) {
    if (!event.target_library_id) return std::nullopt;
    const auto* reference = findEmbeddedTimbreSnapshot(
        model, event.target_library_id);
    if (!reference || reference->source != TimbreSource::Scc) return std::nullopt;
    SccWaveform wave{};
    for (std::size_t i = 0; i < wave.size(); ++i) {
        const auto sample = reference->scc_waveform[i];
        wave[i] = static_cast<std::int8_t>(sample < 128 ? sample : int(sample) - 256);
    }
    return wave;
}

void sccMorphDiagnosticCsvRow(
    std::ostream& out, std::string_view case_name, std::size_t index,
    double t, double u, std::string_view stage, std::string_view candidate,
    bool selected, double score, double cumulative, std::size_t predecessor,
    std::size_t shift, std::uint32_t count, std::uint8_t output_number,
    const std::int8_t* samples, std::size_t sample_count) {
    out << case_name << ',' << index << ',' << t << ',' << u << ',' << stage
        << ',' << candidate << ',' << (selected ? 1 : 0) << ',' << score
        << ',' << cumulative << ',' << predecessor << ',' << shift << ','
        << count << ',' << static_cast<unsigned>(output_number);
    for (std::size_t i = 0; i < 32; ++i) {
        out << ',';
        if (i < sample_count) out << static_cast<int>(samples[i]);
    }
    out << '\n';
}

void sccMorphDiagnosticCsvHeader(std::ostream& out) {
    out << "case,index,t,u,stage,candidate,selected,score,cumulative,predecessor,"
           "shift,count,output_number,sample0,sample1,sample2,sample3,sample4,"
           "sample5,sample6,sample7,sample8,sample9,sample10,sample11,sample12,"
           "sample13,sample14,sample15,sample16,sample17,sample18,sample19,"
           "sample20,sample21,sample22,sample23,sample24,sample25,sample26,"
           "sample27,sample28,sample29,sample30,sample31\n";
}

template <std::size_t N>
void sccMorphDiagnosticCsvRow(
    std::ostream& out, std::string_view case_name, std::size_t index,
    double t, double u, std::string_view stage, std::string_view candidate,
    bool selected, double score, double cumulative, std::size_t predecessor,
    std::size_t shift, const std::array<double, N>& samples) {
    out << case_name << ',' << index << ',' << t << ',' << u << ',' << stage
        << ',' << candidate << ',' << (selected ? 1 : 0) << ',' << score
        << ',' << cumulative << ',' << predecessor << ',' << shift << ",,";
    for (std::size_t i = 0; i < 32; ++i) {
        out << ',';
        if (i < N) out << samples[i];
    }
    out << '\n';
}

void writeSccMorphPairTraceCsv(
    std::ostream& out, std::string_view case_name,
    const SccWaveform& first, const SccWaveform& second,
    const SccMorphPairTrace& trace, const SccMorphPairResult& ordinary) {
    const std::array<std::int8_t, 32> signed_first = first;
    sccMorphDiagnosticCsvRow(out, case_name, 0, 0.0, 0.0, "endpoint",
        "source", false, 0.0, 0.0, 0, 0, 0, 0, signed_first.data(), 32);
    for (std::size_t i = 0; i < trace.steps.size(); ++i) {
        const auto& step = trace.steps[i];
        const auto& diagnostic = ordinary.diagnostics[i];
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "target_magnitude", "target", false, 0.0, 0.0, 0, 0,
            step.target_magnitude);
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "advanced_magnitude", "spectral_envelope", false, 0.0, 0.0,
            0, 0, step.advanced_magnitude);
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "raw_log_magnitude", "target", false, 0.0, 0.0, 0, 0,
            step.raw_log_magnitude);
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "guide_phase", "phase_guide", false, 0.0, 0.0, 0, 0,
            step.guide_phase);
        for (std::size_t c = 0; c < step.candidates.size(); ++c) {
            const auto& candidate = step.candidates[c];
            const auto name = sccMorphDiagnosticCandidateLabel(candidate);
            const bool selected = c == step.selected_candidate;
            const double score = candidate.pre_quantization_error;
            for (const auto& [stage, samples] : std::array{
                     std::pair{"reconstructed", &candidate.reconstructed},
                     std::pair{"corrected", &candidate.corrected},
                     std::pair{"fitted", &candidate.fitted}}) {
                sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t,
                    step.u, stage, name, selected, score,
                    candidate.cumulative_cost, candidate.predecessor, 0,
                    *samples);
            }
            const auto quantized = [&] {
                std::array<std::int8_t, 32> values{};
                for (std::size_t n = 0; n < values.size(); ++n)
                    values[n] = candidate.quantized[n];
                return values;
            }();
            sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
                "quantized", name, selected, candidate.post_quantization_error,
                candidate.cumulative_cost, candidate.predecessor, 0,
                0, 0, quantized.data(), quantized.size());
            if (candidate.method == SccMorphCandidate::Blend) {
                sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t,
                    step.u, "blend_weights", name, selected,
                    candidate.pre_quantization_error,
                    candidate.cumulative_cost, candidate.predecessor, 0,
                    candidate.weights);
            }
        }
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "selected_pre_local", "selected", true,
            step.selected_pre_local_error, 0.0, 0, 0, 0, 0,
            step.selected_pre_local.data(), step.selected_pre_local.size());
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "selected_post_local", "selected", true,
            step.selected_post_local_error, 0.0, 0, 0, 0, 0,
            step.selected_post_local.data(), step.selected_post_local.size());
        const std::array<double, 9> target_descriptors{
            diagnostic.target.centroid, diagnostic.target.spread,
            diagnostic.target.slope, diagnostic.target.odd_even,
            diagnostic.target.irregularity, diagnostic.target.flatness,
            diagnostic.target.rms, diagnostic.target.dc, diagnostic.target.peak};
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "target_descriptors", "target", false,
            diagnostic.pre_quantization_error, 0.0, 0, 0, target_descriptors);
        const std::array<double, 9> result_descriptors{
            diagnostic.result.centroid, diagnostic.result.spread,
            diagnostic.result.slope, diagnostic.result.odd_even,
            diagnostic.result.irregularity, diagnostic.result.flatness,
            diagnostic.result.rms, diagnostic.result.dc, diagnostic.result.peak};
        sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
            "result_descriptors", "selected", true,
            diagnostic.post_quantization_error, 0.0, 0, 0, result_descriptors);
        for (std::size_t c = 0; c < step.candidates.size(); ++c) {
            const auto& candidate = step.candidates[c];
            const auto name = sccMorphDiagnosticCandidateLabel(candidate);
            for (std::size_t previous = 0; previous < step.candidates.size(); ++previous) {
                const auto& edge = step.candidates[c].incoming[previous];
                const std::array<double, 10> components{
                    edge.phase_wave, edge.rms, edge.harmonic, edge.spectral_shape,
                    edge.descriptor, edge.distance, edge.dt, edge.du,
                    edge.reverse, edge.cost};
                sccMorphDiagnosticCsvRow(out, case_name, i + 1, step.t, step.u,
                    "connection", name, previous == candidate.predecessor,
                    edge.cost, candidate.cumulative_cost, previous, 0, components);
            }
        }
    }
    if (!trace.steps.empty()) {
        const auto& last = trace.steps.back();
        for (std::size_t previous = 0; previous < trace.terminal.size(); ++previous) {
            const auto& edge = trace.terminal[previous];
            const std::array<double, 10> components{
                edge.phase_wave, edge.rms, edge.harmonic, edge.spectral_shape,
                edge.descriptor, edge.distance, edge.dt, edge.du,
                edge.reverse, edge.cost};
            sccMorphDiagnosticCsvRow(out, case_name, trace.steps.size() + 1,
                1.0, 1.0, "terminal_connection", "endpoint",
                previous == last.selected_candidate, edge.cost,
                trace.trajectory_cost, previous, 0, components);
        }
    }
    const auto aligned_second = trace.aligned_second;
    sccMorphDiagnosticCsvRow(out, case_name, trace.steps.size() + 1,
        1.0, 1.0, "endpoint", "source", false, 0.0, 0.0, 0,
        trace.alignment_shift, 60, 0, aligned_second.data(), aligned_second.size());
    std::vector<SccWaveform> global{first};
    global.insert(global.end(), ordinary.intermediate.begin(), ordinary.intermediate.end());
    global.push_back(second);
    const auto shifts = optimizeSccMorphBlockPhases(global, false, false);
    for (std::size_t i = 0; i < global.size(); ++i) {
        const double t = double(i) / double(global.size() - 1);
        const double u = morphPosition(t, 1.0);
        sccMorphDiagnosticCsvRow(out, case_name, i, t, u, "final_global_phase",
            "selected", true, 0.0, 0.0, 0, shifts[i],
            static_cast<std::uint32_t>(i), 0, global[i].data(), global[i].size());
    }
}

void writeSccMorphCompiledRowsCsv(
    std::ostream& out, const SccMorphCompileResult& compiled) {
    if (compiled.timbre.layers.empty()) return;
    const auto& events = compiled.timbre.layers.front().timbre_automation;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (event.kind != EnvelopeEventKind::Timbre) continue;
        const auto wave = sccMorphDiagnosticEventWave(compiled.timbre, event);
        if (!wave) continue;
        const auto found = std::find_if(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            [&](const auto& diagnostic) {
                return diagnostic.count == event.count
                    && diagnostic.output_number == event.value;
            });
        const auto* diagnostic = found == compiled.diagnostics.end()
            ? nullptr : &*found;
        sccMorphDiagnosticCsvRow(out, "fixture_reconstruction",
            i, double(event.count) / 60.0, diagnostic ? diagnostic->u : 0.0,
            "compiled_global", diagnostic
                ? (diagnostic->candidate == SccMorphCandidate::Blend
                    ? "blend" : sccMorphDiagnosticCandidateName(diagnostic->candidate))
                : "materialized",
            true, diagnostic ? diagnostic->post_quantization_error : 0.0,
            0.0, 0, diagnostic ? diagnostic->phase_shift : 0,
            event.count, static_cast<std::uint8_t>(event.value),
            wave->data(), wave->size());
        if (diagnostic && diagnostic->candidate == SccMorphCandidate::Blend) {
            const std::array<double, 3> weights = diagnostic->candidate_weights;
            sccMorphDiagnosticCsvRow(out, "fixture_reconstruction", i,
                double(event.count) / 60.0, diagnostic->u,
                "compiled_blend_weights", "blend", true,
                diagnostic->post_quantization_error, 0.0, 0,
                diagnostic->phase_shift, weights);
        }
    }
}

void testSccMorphDiagnosticFixture() {
    const auto fixture_path = sccMorphDiagnosticFixturePath();
    if (!fixture_path) {
        std::cout << "SCC morph diagnostic fixture skipped (not present in this checkout)\n";
        return;
    }
    std::ifstream fixture_file(*fixture_path, std::ios::binary);
    std::ostringstream fixture_stream;
    fixture_stream << fixture_file.rdbuf();
    require(static_cast<bool>(fixture_file) || fixture_file.eof(),
            "diagnostic fixture can be read");
    std::string source = fixture_stream.str();
    source += "\n4 v15 @15 @e0\n";
    const auto parsed = parseMgsComposite(source);
    require(parsed.valid(), "user MGSC diagnostic output parses with one track");
    require(parsed.timbre.layers.size() == 1,
            "diagnostic MGSC fixture has one SCC layer");
    const auto actual_definitions = sccMorphDiagnosticDefinitions(source);
    require(actual_definitions.contains(15) && actual_definitions.contains(17)
                && actual_definitions.contains(0),
            "diagnostic fixture contains the expected endpoints and first generated wave");
    const auto& layer = parsed.timbre.layers.front();
    std::vector<const EnvelopeEvent*> actual_events;
    for (const auto& event : layer.timbre_automation)
        if (event.kind == EnvelopeEventKind::Timbre) actual_events.push_back(&event);
    std::cout << "SCC morph fixture: " << fixture_path->string()
              << " events=" << actual_events.size() << "\n";
    for (const auto* event : actual_events) {
        std::cout << "  @" << event->value << " count=" << event->count << '\n';
    }

    const auto triangle = actual_definitions.at(15);
    const auto sine = actual_definitions.at(17);
    const auto observed_first = actual_definitions.at(0);
    double best_distance = std::numeric_limits<double>::infinity();
    int best_phase = 0;
    bool exact = false;
    CompositeTimbre best_compiled;
    SccMorphCompileResult best_result;
    SccWaveform best_start = observed_first;
    std::map<unsigned, SccWaveform> best_definitions;
    for (int phase = 28; phase < 29; ++phase) {
        CompositeTimbre input;
        input.name = "SCC morph diagnostic reconstruction";
        input.scc_morph_bank_base = 0;
        input.scc_morph_algorithm_version = kSccMorphAlgorithmVersion;
        CompositeLayer source_layer;
        source_layer.name = "SCC diagnostics";
        source_layer.source = TimbreSource::Scc;
        source_layer.channel = 0;
        source_layer.base_timbre = sccTone(900, 15, triangle);
        source_layer.envelope_timeline.length_counts = 61;
        const auto start = rotateSccWaveform(observed_first, phase);
        input.embedded_timbres = {
            sccTone(1001, 16, start), sccTone(1002, 17, sine)};
        source_layer.timbre_automation = {
            toneEvent(1001, 0),
            toneEvent(1002, 60, SccMorphTransition{
                .enabled = true,
                .intermediate_count = 27,
                .curve = 1.0,
                .distribution_mode = SccMorphDistributionMode::ToneDistribution,
            })};
        input.layers.push_back(std::move(source_layer));
        const auto compiled = compileSccMorph(input);
        if (!compiled.valid) continue;
        const auto& events = compiled.timbre.layers.front().timbre_automation;
        double distance{};
        bool same = events.size() == (actual_events.empty() ? 0 : actual_events.size() - 1);
        const std::size_t expected_count = actual_events.empty()
            ? 0 : actual_events.size() - 1;
        distance += std::abs(double(events.size()) - double(expected_count))
            * 1'000'000'000.0;
        if (!actual_events.empty()) {
            const auto compare_count = std::min(events.size(), expected_count);
            for (std::size_t i = 0; i < compare_count; ++i) {
                const auto* expected = actual_events[i + 1];
                distance += std::abs(double(events[i].count) - double(expected->count))
                    * 1'000'000.0;
                distance += std::abs(double(events[i].value) - double(expected->value))
                    * 10'000.0;
                const auto expected_wave = sccMorphDiagnosticEventWave(
                    parsed.timbre, *expected);
                const auto generated_wave = sccMorphDiagnosticEventWave(
                    compiled.timbre, events[i]);
                if (!expected_wave || !generated_wave) {
                    same = false;
                    distance += 1'000'000.0;
                } else {
                    distance += sccMorphDiagnosticDistance(*expected_wave, *generated_wave);
                    same = same && *expected_wave == *generated_wave
                        && events[i].count == expected->count
                        && events[i].value == expected->value;
                }
            }
        }
        if (distance < best_distance) {
            best_distance = distance;
            best_phase = phase;
            best_start = start;
            best_result = compiled;
            best_compiled = compiled.timbre;
            best_definitions = sccMorphDiagnosticDefinitions(
                formatMgsComposite(compiled.timbre).source);
        }
        if (same) {
            const auto formatted = formatMgsComposite(compiled.timbre);
            require(formatted.valid(), "exactly reconstructed output formats");
            const auto reconstructed_definitions =
                sccMorphDiagnosticDefinitions(formatted.source);
            require(reconstructed_definitions == actual_definitions,
                    "exact reconstruction preserves every SCC definition byte and occupied slot");
            require(!reconstructed_definitions.contains(16)
                        && !reconstructed_definitions.contains(31),
                    "exact reconstruction leaves the unused SCC slots @16 and @31 absent");
            exact = true;
            best_compiled = compiled.timbre;
            best_result = compiled;
            best_start = start;
            best_definitions = reconstructed_definitions;
            best_phase = phase;
            break;
        }
    }
    std::cout << "SCC morph exact reconstruction=" << (exact ? "yes" : "no")
              << " phase=" << best_phase << " best_distance=" << best_distance
              << "\n";
    if (!exact) {
        std::cout << "Changed Phase B waves differ from the frozen output; nearest phase is diagnostic only.\n";
    }

    // The investigation established that the authored @16 source is the
    // observed @s0 rotated by 28 samples. Keep this source phase fixed for all
    // before/after measurements; do not let the improved output change which
    // reconstruction is judged closest to the old bytes.
    best_phase = 28;
    best_start = rotateSccWaveform(observed_first, best_phase);
    CompositeTimbre canonical_input;
    canonical_input.name = "SCC morph Phase B fixed reconstruction";
    canonical_input.scc_morph_bank_base = 0;
    canonical_input.scc_morph_algorithm_version = kSccMorphAlgorithmVersion;
    CompositeLayer canonical_layer;
    canonical_layer.name = "SCC diagnostics";
    canonical_layer.source = TimbreSource::Scc;
    canonical_layer.channel = 0;
    canonical_layer.base_timbre = sccTone(900, 15, triangle);
    canonical_layer.envelope_timeline.length_counts = 61;
    canonical_layer.timbre_automation = {
        toneEvent(1001, 0),
        toneEvent(1002, 60, SccMorphTransition{
            .enabled = true,
            .intermediate_count = 27,
            .curve = 1.0,
            .distribution_mode = SccMorphDistributionMode::ToneDistribution,
        })};
    canonical_input.embedded_timbres = {
        sccTone(1001, 16, best_start), sccTone(1002, 17, sine)};
    canonical_input.layers.push_back(std::move(canonical_layer));
    best_result = compileSccMorph(canonical_input);
    require(best_result.valid,
            "fixed-phase Pulse25-to-Sine reference still compiles after algorithm change");
    best_compiled = best_result.timbre;
    best_definitions = sccMorphDiagnosticDefinitions(
        formatMgsComposite(best_compiled).source);
    require(best_compiled.scc_morph_algorithm_version
                == kSccMorphAlgorithmVersion,
            "Phase B materialization stamps the current algorithm version");
    const auto preserved_start = std::find_if(best_compiled.embedded_timbres.begin(),
        best_compiled.embedded_timbres.end(), [](const auto& tone) {
            return tone.source == TimbreSource::Scc
                && tone.number_mode == TimbreNumberMode::Manual
                && tone.manual_number == 16;
        });
    const auto preserved_end = std::find_if(best_compiled.embedded_timbres.begin(),
        best_compiled.embedded_timbres.end(), [](const auto& tone) {
            return tone.source == TimbreSource::Scc
                && tone.number_mode == TimbreNumberMode::Manual
                && tone.manual_number == 17;
        });
    require(preserved_start != best_compiled.embedded_timbres.end()
                && preserved_end != best_compiled.embedded_timbres.end()
                && preserved_start->scc_waveform == sccTone(1001, 16, best_start).scc_waveform
                && preserved_end->scc_waveform == sccTone(1002, 17, sine).scc_waveform,
            "morph compilation preserves original source and endpoint definitions");
    const auto phase_b_quality = sccMorphDiagnosticCompiledQuality(best_result);
    require(phase_b_quality.waves.size() == 29
                && phase_b_quality.counts.front() == 0
                && phase_b_quality.counts.back() == 60,
            "fixed-phase quality sequence retains endpoints and 27 intermediate events");
    for (const auto& diagnostic : best_result.diagnostics) {
        if (!diagnostic.generated) continue;
        require(diagnostic.output_number != 15 && diagnostic.output_number != 16
                    && diagnostic.output_number != 17,
                "Phase B generated SCC definitions do not overwrite the authored source range");
    }
    require(phase_b_quality.distinct_intermediates > 13,
            "continuity improvement retains a substantial number of distinct intermediate waves");
    const auto phase_a_quality = readSccMorphDiagnosticBaseline();
    require(!sccMorphDiagnosticBaselinePath() || phase_a_quality.has_value(),
            "present frozen Phase A diagnostic baseline must parse successfully");
    if (phase_a_quality) {
        require(phase_a_quality->waves.size() == phase_b_quality.waves.size(),
                "frozen Phase A comparison has the same endpoint and intermediate count");
        require(phase_a_quality->counts == phase_b_quality.counts,
                "algorithm update leaves every compiled event count unchanged");
        require(phase_a_quality->max_adjacent_sample_rms > 40.0,
                "frozen Phase A fixture reproduces its large sample discontinuity");
        require(phase_b_quality.max_adjacent_sample_rms
                    < phase_a_quality->max_adjacent_sample_rms * 0.5,
                "Phase B reduces the largest whole-sequence sample transition");
        require(phase_b_quality.total_spectral_variation
                    >= std::max(phase_b_quality.endpoint_spectral_distance * 0.95,
                                phase_a_quality->total_spectral_variation * 0.35),
                "lower maximum gap does not collapse total spectral travel");
        std::cout << "SCC morph Phase A->B max_sample_gap="
                  << phase_a_quality->max_adjacent_sample_rms << "->"
                  << phase_b_quality.max_adjacent_sample_rms
                  << " max_rms_step=" << phase_a_quality->max_level_rms_step
                  << "->" << phase_b_quality.max_level_rms_step
                  << " max_spectral_gap=" << phase_a_quality->max_spectral_distance
                  << "->" << phase_b_quality.max_spectral_distance
                  << " spectral_TV=" << phase_a_quality->total_spectral_variation
                  << "->" << phase_b_quality.total_spectral_variation
                  << " distinct_intermediates="
                  << phase_a_quality->distinct_intermediates << "->"
                  << phase_b_quality.distinct_intermediates << '\n';
    } else {
        std::cout << "SCC morph frozen Phase A comparison skipped (local baseline absent)\n";
    }

    const auto pulse = generateSccPreset(SccWavePreset::Pulse25, SccHarmonic::One);
    const auto square = generateSccPreset(SccWavePreset::Square, SccHarmonic::One);
    const auto triangle_wave = generateSccPreset(
        SccWavePreset::Triangle, SccHarmonic::One);
    const auto phase_only = rotateSccWaveform(sine, 9);
    const SccWaveform silence{};
    SccWaveform high_dc_a{}, high_dc_b{};
    for (std::size_t i = 0; i < high_dc_a.size(); ++i) {
        high_dc_a[i] = static_cast<std::int8_t>(44 + (i % 2 ? 3 : -3));
        high_dc_b[i] = static_cast<std::int8_t>(-52 + (i % 2 ? 2 : -2));
    }
    SccWaveform sparse_a{}, sparse_b{};
    for (std::size_t i = 0; i < sparse_a.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * double(i) / 32.0;
        sparse_a[i] = static_cast<std::int8_t>(std::lround(
            82.0 * std::sin(2.0 * phase) + 31.0 * std::sin(7.0 * phase)));
        sparse_b[i] = static_cast<std::int8_t>(std::lround(
            52.0 * std::sin(3.0 * phase) + 24.0 * std::sin(11.0 * phase)));
    }
    struct MatrixCase {
        const char* name;
        SccWaveform first, second;
        std::uint8_t count{27};
        double gamma{1.0};
    };
    const std::array<MatrixCase, 12> cases{{
        {"pulse25_to_sine", pulse, sine},
        {"sine_to_pulse25", sine, pulse},
        {"square_to_sine", square, sine},
        {"sine_to_square", sine, square},
        {"triangle_to_sine", triangle_wave, sine},
        {"same", sine, sine},
        {"phase_only", sine, phase_only},
        {"silence_to_sine", silence, sine},
        {"sine_to_silence", sine, silence},
        {"high_dc_bipolar", high_dc_a, high_dc_b},
        {"rms_difference", silence, pulse},
        {"sparse_harmonics", sparse_a, sparse_b},
    }};
    std::filesystem::path output_directory =
        std::filesystem::current_path() / "scc-morph-diagnostics";
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory, directory_error);
    std::ofstream csv;
    if (!directory_error) {
        csv.open(output_directory / "scc-morph-trace.csv", std::ios::binary);
    }
    if (csv) {
        csv << std::setprecision(17);
        sccMorphDiagnosticCsvHeader(csv);
    }
    std::vector<std::uint32_t> fixture_counts;
    if (!best_compiled.layers.empty()) {
        for (const auto& event : best_compiled.layers.front().timbre_automation)
            if (event.kind == EnvelopeEventKind::Timbre)
                fixture_counts.push_back(event.count);
    }
    require(fixture_counts.size() == 29 && fixture_counts.front() == 0
                && fixture_counts.back() == 60,
            "fixture playback counts contain both endpoints and every rounded intermediate");
    SccMorphPairTrace fixture_trace;
    const auto fixture_trace_begin = std::chrono::steady_clock::now();
    const auto fixture_traced = generateSccMorphTraced(
        best_start, sine, 27, 1.0, fixture_trace, fixture_counts);
    const auto fixture_trace_elapsed = std::chrono::steady_clock::now()
        - fixture_trace_begin;
    require(fixture_traced.intermediate.size() == 27
                && fixture_trace.steps.size() == 27,
            "playback-timed trace retains the authored intermediate count");
    for (std::size_t i = 0; i < fixture_counts.size() - 2; ++i) {
        const auto found = std::find_if(best_result.diagnostics.begin(),
            best_result.diagnostics.end(), [&](const auto& diagnostic) {
                return diagnostic.count == fixture_counts[i + 1];
            });
        require(found != best_result.diagnostics.end(),
                "compiled playback has a diagnostic at each rounded morph time");
        const auto materialized = materializedWaveFor(best_compiled, *found);
        // The morph core shifts left; the public editor rotation shifts right.
        const auto unshifted = rotateSccWaveform(materialized,
            found->phase_shift % 32);
        require(unshifted == fixture_traced.intermediate[i],
                "playback-timed trace matches compiled wave before block phase rotation");
        requireNear(fixture_trace.steps[i].u,
                    morphPosition(double(i + 1) / 28.0, 1.0), 1e-12,
                    "rounded event timing does not move authored morph positions");
    }
    if (csv) {
        writeSccMorphPairTraceCsv(csv, "fixture_reconstruction", best_start,
            sine, fixture_trace, fixture_traced);
        writeSccMorphCompiledRowsCsv(csv, best_result);
    }
    std::size_t fixture_trace_bytes = sizeof(fixture_trace)
        + fixture_trace.steps.capacity() * sizeof(SccMorphStepTrace)
        + fixture_traced.intermediate.capacity() * sizeof(SccWaveform)
        + fixture_traced.diagnostics.capacity() * sizeof(SccMorphWaveDiagnostic);
    std::cout << "SCC morph Phase B pulse25_to_sine elapsed_us="
              << std::chrono::duration<double, std::micro>(fixture_trace_elapsed).count()
              << " playback_timed_trace_bytes=" << fixture_trace_bytes << '\n';
    for (const auto& entry : cases) {
        SccMorphPairTrace trace;
        const auto trace_started = std::chrono::steady_clock::now();
        const auto traced = generateSccMorphTraced(
            entry.first, entry.second, entry.count, entry.gamma, trace);
        const auto trace_elapsed = std::chrono::steady_clock::now() - trace_started;
        const auto ordinary_started = std::chrono::steady_clock::now();
        const auto ordinary = generateSccMorph(
            entry.first, entry.second, entry.count, entry.gamma);
        const auto ordinary_elapsed = std::chrono::steady_clock::now()
            - ordinary_started;
        require(traced.intermediate == ordinary.intermediate,
                "opt-in diagnostics leave ordinary generated waves unchanged");
        require(trace.steps.size() == ordinary.intermediate.size()
                    && traced.diagnostics.size() == ordinary.diagnostics.size(),
                "trace includes every generated point");
        require(trace.steps.size() == entry.count,
                "diagnostic tracing retains requested intermediate count");
        require(trace.first == entry.first && trace.second == entry.second,
                "diagnostic tracing retains authored endpoint bytes");
        for (std::size_t i = 0; i < trace.steps.size(); ++i) {
            require(trace.steps[i].selected_post_local == ordinary.intermediate[i],
                    "trace selected local waveform matches ordinary output");
            requireNear(trace.steps[i].u, ordinary.diagnostics[i].u, 1e-12,
                        "trace and ordinary diagnostics use the same morph position");
            require(trace.steps[i].selected_candidate < trace.steps[i].candidates.size(),
                    "trace records a valid selected candidate");
            const auto& selected = trace.steps[i].candidates[
                trace.steps[i].selected_candidate];
            require(selected.quantized == trace.steps[i].selected_pre_local,
                    "selected candidate quantization matches pre-local-search output");
            for (const auto& edge : selected.incoming) {
                require(std::isfinite(edge.phase_wave) && std::isfinite(edge.rms)
                            && std::isfinite(edge.harmonic)
                            && std::isfinite(edge.spectral_shape)
                            && std::isfinite(edge.descriptor)
                            && std::isfinite(edge.distance) && std::isfinite(edge.dt)
                            && std::isfinite(edge.du) && std::isfinite(edge.reverse)
                            && std::isfinite(edge.cost),
                        "candidate connection trace reports finite components");
                require(edge.dt >= 0.0 && edge.du >= 0.0 && edge.cost >= 0.0,
                        "candidate connection time and cost are nonnegative");
            }
        }
        require(trace.terminal.size() == trace.steps.back().candidates.size(),
                "terminal DP trace covers each candidate state");
        for (const auto& edge : trace.terminal)
            require(std::isfinite(edge.cost) && edge.cost >= 0.0,
                    "candidate-to-endpoint cost is finite and nonnegative");
        const auto first_analysis = analyzeSccMorphWaveform(entry.first);
        const auto second_analysis = analyzeSccMorphWaveform(entry.second);
        const double transition = sccMorphTransitionCost(entry.first, entry.second);
        std::cout << "SCC morph matrix " << entry.name
                  << " shift=" << trace.alignment_shift
                  << " transition_cost=" << transition
                  << " centroid=" << first_analysis.descriptors.centroid << "->"
                  << second_analysis.descriptors.centroid
                  << " RMS=" << first_analysis.descriptors.rms << "->"
                  << second_analysis.descriptors.rms
                  << " trace_us="
                  << std::chrono::duration<double, std::micro>(trace_elapsed).count()
                  << " ordinary_us="
                  << std::chrono::duration<double, std::micro>(ordinary_elapsed).count()
                  << " trace_bytes=" << (sizeof(trace)
                      + trace.steps.capacity() * sizeof(SccMorphStepTrace)) << '\n';
        if (csv) writeSccMorphPairTraceCsv(
            csv, entry.name, entry.first, entry.second, trace, ordinary);
    }
    if (csv) {
        csv.close();
        std::cout << "SCC morph trace CSV: "
                  << (output_directory / "scc-morph-trace.csv").string() << '\n';
    } else {
        std::cout << "SCC morph trace CSV could not be written in this working directory.\n";
    }

    if (exact) {
        const auto& events = best_compiled.layers.front().timbre_automation;
        std::cout << "Exact reconstruction event/wave rows: " << events.size() << '\n';
        for (const auto& event : events) {
            if (event.kind != EnvelopeEventKind::Timbre) continue;
            const auto wave = sccMorphDiagnosticEventWave(best_compiled, event);
            const auto descriptors = wave
                ? analyzeSccMorphWaveform(*wave).descriptors : SccMorphDescriptors{};
            std::cout << "  @" << event.value << " count=" << event.count
                      << " centroid=" << descriptors.centroid
                      << " odd_even=" << descriptors.odd_even
                      << " rms=" << descriptors.rms
                      << " peak=" << descriptors.peak << '\n';
        }
        (void)best_definitions;
    }
}
