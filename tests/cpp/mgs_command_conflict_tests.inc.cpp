// SPDX-License-Identifier: AGPL-3.0-only

namespace {
mgstc::engine::CompositeLayer commandConflictLayer() {
    using namespace mgstc::engine;
    auto layer = defaultCompositeTimbre().layers.front();
    layer.source = TimbreSource::Psg;
    layer.channel = 0;
    layer.envelope_number = 0;
    layer.envelope_timeline = {.length_counts = 8};
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.volume_envelope.events = {{.kind = EnvelopeEventKind::Volume, .value = 15}};
    layer.pitch_envelope.events.clear();
    layer.timbre_automation.clear();
    return layer;
}
}

void testLegacyImportedTimingStateClassification() {
    using namespace mgstc::engine;
    auto timbre = defaultCompositeTimbre();
    timbre.layers = {commandConflictLayer()};
    timbre.layers.front().volume_envelope.events.push_back({
        .kind = EnvelopeEventKind::Volume, .value = 0, .secondary = 2,
        .count = 1, .automatic = true});
    const auto before = serializeCompositeSoundPayload(timbre);
    REQUIRE_EQ(mgsCompositeCommandConflict(timbre).has_value(), false);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(timbre), false);
    REQUIRE_EQ(mgsCompositeHasOnlyRepairableAuthoringIssues(timbre), true);
    const auto issue = mgsCompositeAuthoringIssue(timbre);
    REQUIRE_EQ(issue.has_value(), true);
    REQUIRE_EQ(issue->find("ct 1") != std::string::npos, true);
    REQUIRE_EQ(serializeCompositeSoundPayload(timbre), before);
    auto invalid = timbre;
    invalid.layers.front().pitch_envelope.events.push_back({
        .kind = EnvelopeEventKind::Pitch, .value = 128, .count = 2});
    REQUIRE_EQ(mgsCompositeHasOnlyRepairableAuthoringIssues(invalid), false);
    auto mixed = timbre;
    mixed.layers.front().timbre_automation = {
        {.kind = EnvelopeEventKind::Noise, .value = 2, .count = 2},
        {.kind = EnvelopeEventKind::Noise, .value = 3, .count = 2}};
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(mixed), false);
    REQUIRE_EQ(mgsCompositeHasOnlyRepairableAuthoringIssues(mixed), true);
}

void testMgsRestrictedCommandConflictsKeepCountTypeAndZone() {
    using namespace mgstc::engine;
    for (const auto kind : {EnvelopeEventKind::Noise,
            EnvelopeEventKind::ToneNoiseMode, EnvelopeEventKind::Timbre}) {
        for (const bool equal_values : {false, true}) {
            auto layer = commandConflictLayer();
            layer.timbre_automation = {
                {.kind = kind, .value = 1, .count = 2, .command_order = 2},
                {.kind = kind, .value = equal_values ? 1 : 2, .count = 2}};
            const auto formatted = formatMgsCompositeEnvelope(layer, 0);
            REQUIRE_EQ(formatted.valid(), false);
            REQUIRE_EQ(formatted.hasIssue(MgsEnvelopeIssue::DuplicateZeroTimeCommand), true);
            REQUIRE_EQ(formatted.command_conflicts.size(), std::size_t{1});
            const auto& conflict = formatted.command_conflicts.front();
            REQUIRE_EQ(conflict.count, std::uint32_t{2});
            REQUIRE_EQ(conflict.type, kind == EnvelopeEventKind::Noise
                ? MgsEnvelopeCommandType::Noise : kind == EnvelopeEventKind::ToneNoiseMode
                ? MgsEnvelopeCommandType::ToneNoiseMode : MgsEnvelopeCommandType::Timbre);
            REQUIRE_EQ(conflict.zone, MgsEnvelopeCommandZone::Count);
            REQUIRE_EQ(describeMgsEnvelopeCommandConflict(conflict).find("ct 2") != std::string::npos, true);
            REQUIRE_EQ(formatted.body.empty() && formatted.definition.empty()
                && formatted.bytecode.empty() && !formatted.output_cutoff_count, true);

            // Authored restrictions are checked even if modulation expansion
            // could replace its input. This must not become a valid prefix.
            layer.psg_noise_modulation.enabled = true;
            const auto modulated = formatMgsCompositeEnvelope(layer, 0, 4);
            REQUIRE_EQ(modulated.hasIssue(MgsEnvelopeIssue::DuplicateZeroTimeCommand), true);
            REQUIRE_EQ(modulated.bytecode.empty(), true);
        }
    }

    // These kinds have no one-command restriction: do not turn the new rule
    // into a generic same-count uniqueness requirement.
    auto unrestricted = commandConflictLayer();
    unrestricted.pitch_envelope.events = {
        {.kind = EnvelopeEventKind::Pitch, .value = 2, .count = 2},
        {.kind = EnvelopeEventKind::Pitch, .value = -1, .count = 2}};
    unrestricted.timbre_automation = {
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 7, .secondary = 1, .count = 2},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 7, .secondary = 2, .count = 2}};
    REQUIRE_EQ(formatMgsCompositeEnvelope(unrestricted, 0).valid(), true);

    auto aliases = commandConflictLayer();
    std::vector<std::string> parse_issues;
    REQUIRE_EQ(parseMgsSequenceBody(",,/1.*2.f:4", aliases, parse_issues), true);
    const auto alias_conflict = formatMgsCompositeEnvelope(aliases, 0);
    REQUIRE_EQ(alias_conflict.command_conflicts.size(), std::size_t{1});
    REQUIRE_EQ(alias_conflict.command_conflicts.front().type,
        MgsEnvelopeCommandType::ToneNoiseMode);
}

void testMgsRestrictedCommandZonesAndCapacityProbe() {
    using namespace mgstc::engine;
    for (const auto kind : {EnvelopeEventKind::Noise,
            EnvelopeEventKind::ToneNoiseMode, EnvelopeEventKind::Timbre}) {
        auto layer = commandConflictLayer();
        layer.envelope_timeline.loop_start_count = 2;
        layer.envelope_timeline.loop_end_count = 8;
        layer.timbre_automation = {
            {.kind = kind, .value = 1, .count = 2},
            {.kind = kind, .value = 2, .count = 2, .after_loop_start = true}};
        REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).valid(), true);
        const auto capacity = maxEnvelopeLengthFittingBodyLimit(layer);
        REQUIRE_EQ(capacity > layer.envelope_timeline.length_counts, true);
        const auto snapshot = layer;
        layer.timbre_automation.push_back(
            {.kind = kind, .value = 1, .count = 2, .after_loop_start = true});
        const auto conflict = formatMgsCompositeEnvelope(layer, 0);
        REQUIRE_EQ(conflict.command_conflicts.size(), std::size_t{1});
        REQUIRE_EQ(conflict.command_conflicts.front().zone, MgsEnvelopeCommandZone::AfterLoopStart);
        layer = snapshot;
        layer.timbre_automation.push_back({.kind = kind, .value = 1, .count = 2});
        REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).command_conflicts.front().zone,
            MgsEnvelopeCommandZone::BeforeLoopStart);

        layer = snapshot;
        layer.timbre_automation = {
            {.kind = kind, .value = 1, .count = 8},
            {.kind = kind, .value = 2, .count = 8, .after_loop_start = true}};
        REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).valid(), true);
        layer.timbre_automation.push_back({.kind = kind, .value = 1, .count = 8});
        REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).command_conflicts.front().zone,
            MgsEnvelopeCommandZone::AfterLoopEnd);
    }
}

void testMgsImplicitBaseCommandConflictStopsOutput() {
    using namespace mgstc::engine;
    auto layer = commandConflictLayer();
    layer.source = TimbreSource::Opll;
    layer.base_timbre = SavedTimbreReference{
        .library_id = 42, .source = TimbreSource::Opll,
        .number_mode = TimbreNumberMode::Manual, .manual_number = 16};
    layer.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre, .value = 3, .count = 0,
            .timbre_pick = TimbrePick::OpllRom},
        {.kind = EnvelopeEventKind::RegisterWrite, .value = 2, .secondary = 19, .count = 1}};
    const auto conflict = formatMgsCompositeEnvelope(layer, 0);
    REQUIRE_EQ(conflict.valid(), false);
    REQUIRE_EQ(conflict.command_conflicts.size(), std::size_t{1});
    REQUIRE_EQ(conflict.command_conflicts.front().includes_automatic_base, true);
    REQUIRE_EQ(conflict.bytecode.empty(), true);

    // An already leading identical authored base is the existing safe case:
    // no extra restore is inserted, so exactly one @ remains in this zone.
    layer.timbre_automation.front().value = 16;
    layer.timbre_automation.front().timbre_pick = TimbrePick::Library;
    layer.timbre_automation.front().target_library_id = 42;
    const auto one_base = formatMgsCompositeEnvelope(layer, 0);
    REQUIRE_EQ(one_base.valid(), true);
    const auto at = one_base.body.find("@16");
    REQUIRE_EQ(at != std::string::npos, true);
    REQUIRE_EQ(one_base.body.find("@16", at + 1) == std::string::npos, true);

    // Loop-prefix base and loop-body @ have separate execution zones.
    layer.envelope_timeline.loop_start_count = 0;
    layer.envelope_timeline.loop_end_count = 8;
    layer.timbre_automation.front() = {.kind = EnvelopeEventKind::Timbre,
        .value = 3, .count = 0, .timbre_pick = TimbrePick::OpllRom,
        .after_loop_start = true};
    REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).valid(), true);
}

void testMgsCommandConflictsReachExportCompileAndSaveWithoutChangingRawState() {
    using namespace mgstc::engine;
    CompositeTimbre tone;
    tone.name = "Conflicting command test";
    tone.layers = {commandConflictLayer()};
    tone.layers.front().timbre_automation = {
        {.kind = EnvelopeEventKind::ToneNoiseMode, .value = 1, .count = 2},
        {.kind = EnvelopeEventKind::ToneNoiseMode, .value = 2, .count = 2}};
    const auto saved_input = serializeCompositeSoundPayload(tone);
    const auto source = formatMgsComposite(tone);
    REQUIRE_EQ(source.valid(), false);
    REQUIRE_EQ(source.source.empty(), true);
    REQUIRE_EQ(source.issues.front().find("ct 2") != std::string::npos, true);
    REQUIRE_EQ(mgsCompositeCommandConflict(tone).has_value(), true);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(tone), true);
    auto no_conflict = tone;
    no_conflict.layers.front().timbre_automation.resize(1);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(no_conflict), false);
    auto bad_loop = tone;
    bad_loop.layers.front().envelope_timeline.loop_start_count = 2;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_loop), false);
    auto bad_volume = tone;
    bad_volume.layers.front().volume_envelope.events.front().value = 16;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_volume), false);
    auto bad_pitch = tone;
    bad_pitch.layers.front().pitch_envelope.events = {
        {.kind = EnvelopeEventKind::Pitch, .value = 128, .count = 1}};
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_pitch), false);
    auto bad_register = tone;
    bad_register.layers.front().timbre_automation.push_back({
        .kind = EnvelopeEventKind::RegisterWrite, .value = 16, .secondary = 1, .count = 1});
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_register), false);
    auto generated_over_bad_source = tone;
    generated_over_bad_source.layers.front().psg_noise_modulation.enabled = true;
    generated_over_bad_source.layers.front().timbre_automation.push_back({
        .kind = EnvelopeEventKind::Noise, .value = 32, .count = 1});
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(generated_over_bad_source), false);
    auto bad_definition_number = tone;
    bad_definition_number.layers.front().envelope_number = 32;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_definition_number), false);
    auto bad_morph = tone;
    bad_morph.scc_morph_algorithm_version = 0;
    auto morph_channel = defaultCompositeTimbre().layers[1];
    morph_channel.scc_output_allocation = SccOutputAllocationMode::Contiguous;
    morph_channel.envelope_number = 1;
    bad_morph.layers.push_back(morph_channel);
    REQUIRE_EQ(compileSccMorphCached(bad_morph)->valid, false);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(bad_morph), false);
    auto inactive_invalid = tone;
    auto extra_layer = commandConflictLayer();
    extra_layer.enabled = false;
    extra_layer.envelope_number = 1;
    extra_layer.volume_envelope.events.front().value = 16;
    inactive_invalid.layers.push_back(extra_layer);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(inactive_invalid), false);
    auto engine = std::make_unique<EngineCore>();
    CompositePlaybackPlan plan;
    REQUIRE_EQ(compileCompositeProgram(*engine, tone, {}, &plan), false);
    REQUIRE_EQ(plan.voice_capacity, std::uint8_t{1}); // cleared plan's existing default

    CompositeTimbreLibrary composites;
    composites.add(tone, 100);
    MgstcToneSetDocument document;
    document.name = "Command validation";
    document.members.push_back(composites.entries().front());
    std::string error;
    REQUIRE_EQ(serializeMgstcToneSet(document, &error).has_value(), false);
    REQUIRE_EQ(error.find("ct 2") != std::string::npos, true);

    // Use the normal file-backed migration path. Seed an existing library rather than merely
    // proving rejection against an empty database: replaceAll writes the
    // timbre side before encountering the rejected composite.
    ToneLibraryDatabase database;
    const auto database_path = std::filesystem::temp_directory_path()
        / ("mgstc-command-conflict-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    if (!database.open(database_path.string(), &error)) throw std::runtime_error(error);
    TimbreLibrary baseline_singles;
    TimbreLibraryEntry retained_single;
    retained_single.name = "Retained OPLL";
    retained_single.memo = "single metadata must survive rejected save";
    retained_single.tags = {"shared", "single"};
    retained_single.favorite = true;
    retained_single.category = TimbreCategory::Opll;
    retained_single.data_version = 1;
    retained_single.opll_registers = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto single_id = baseline_singles.add(retained_single, 11);
    auto* single_metadata = baseline_singles.find(single_id);
    single_metadata->revision = 7;
    single_metadata->updated_unix_seconds = 13;
    single_metadata->last_used_unix_seconds = 17;
    single_metadata->use_count = 5;

    auto retained_tone = tone;
    retained_tone.name = "Retained composite";
    retained_tone.memo = "composite metadata must survive rejected save";
    retained_tone.tags = {"composite", "shared"};
    retained_tone.favorite = true;
    retained_tone.layers.front().timbre_automation.resize(1);
    CompositeTimbreLibrary baseline_composites;
    const auto composite_id = baseline_composites.add(retained_tone, 23);
    auto* composite_metadata = baseline_composites.find(composite_id);
    composite_metadata->revision = 9;
    composite_metadata->updated_unix_seconds = 29;
    composite_metadata->last_used_unix_seconds = 31;
    composite_metadata->use_count = 6;
    std::vector<ToneSetRecord> baseline_sets{{
        .id = 41, .name = "Retained set", .memo = "set metadata",
        .created_unix_seconds = 43, .updated_unix_seconds = 47, .revision = 11}};
    std::vector<ToneSetMemberRecord> baseline_members{
        {.set_id = 41, .timbre_id = single_id, .position = 0},
        {.set_id = 41, .composite_id = composite_id, .position = 1}};
    REQUIRE_EQ(database.replaceAllAndToneSets(baseline_singles, baseline_composites,
        baseline_sets, baseline_members, &error), true);

    TimbreLibrary expected_singles;
    CompositeTimbreLibrary expected_composites;
    REQUIRE_EQ(database.load(expected_singles, expected_composites, &error), true);
    const auto expected_single_text = expected_singles.serialize();
    const auto expected_composite_text = expected_composites.serialize();
    const auto assert_existing_library_unchanged = [&] {
        TimbreLibrary loaded_singles;
        CompositeTimbreLibrary loaded_composites;
        REQUIRE_EQ(database.load(loaded_singles, loaded_composites, &error), true);
        REQUIRE_EQ(loaded_singles.serialize(), expected_single_text);
        REQUIRE_EQ(loaded_composites.serialize(), expected_composite_text);
        REQUIRE_EQ(loaded_singles.find(single_id)->tags,
            std::vector<std::string>({"shared", "single"}));
        REQUIRE_EQ(loaded_composites.find(composite_id)->timbre.tags,
            std::vector<std::string>({"composite", "shared"}));
        std::vector<ToneSetRecord> loaded_sets;
        std::vector<ToneSetMemberRecord> loaded_members;
        REQUIRE_EQ(database.loadToneSets(loaded_sets, loaded_members, &error), true);
        REQUIRE_EQ(loaded_sets.size(), std::size_t{1});
        REQUIRE_EQ(loaded_sets.front().id, baseline_sets.front().id);
        REQUIRE_EQ(loaded_sets.front().name, baseline_sets.front().name);
        REQUIRE_EQ(loaded_sets.front().memo, baseline_sets.front().memo);
        REQUIRE_EQ(loaded_sets.front().created_unix_seconds, baseline_sets.front().created_unix_seconds);
        REQUIRE_EQ(loaded_sets.front().updated_unix_seconds, baseline_sets.front().updated_unix_seconds);
        REQUIRE_EQ(loaded_sets.front().revision, baseline_sets.front().revision);
        REQUIRE_EQ(loaded_members.size(), baseline_members.size());
        for (std::size_t at = 0; at < loaded_members.size(); ++at) {
            REQUIRE_EQ(loaded_members[at].set_id, baseline_members[at].set_id);
            REQUIRE_EQ(loaded_members[at].timbre_id, baseline_members[at].timbre_id);
            REQUIRE_EQ(loaded_members[at].composite_id, baseline_members[at].composite_id);
            REQUIRE_EQ(loaded_members[at].position, baseline_members[at].position);
        }
    };
    assert_existing_library_unchanged();

    auto rejected_composites = baseline_composites;
    auto rejected_tone = tone;
    rejected_tone.tags = {"rejected"};
    rejected_tone.memo = "must never replace stored metadata";
    REQUIRE_EQ(rejected_composites.update(composite_id, rejected_tone, 997), true);
    error.clear();
    REQUIRE_EQ(database.replaceComposites(rejected_composites, &error), false);
    REQUIRE_EQ(error.find("ct 2") != std::string::npos, true);
    assert_existing_library_unchanged();

    auto rejected_singles = baseline_singles;
    auto* rejected_single = rejected_singles.find(single_id);
    rejected_single->name = "Should roll back";
    rejected_single->memo = "Should roll back";
    rejected_single->tags = {"rejected"};
    rejected_single->opll_registers[0] ^= 0x7f;
    ++rejected_single->revision;
    error.clear();
    REQUIRE_EQ(database.replaceAll(rejected_singles, rejected_composites, &error), false);
    REQUIRE_EQ(error.find("ct 2") != std::string::npos, true);
    assert_existing_library_unchanged();

    auto rejected_sets = baseline_sets;
    rejected_sets.front().name = "Should roll back";
    rejected_sets.front().memo = "Should roll back";
    ++rejected_sets.front().revision;
    auto rejected_members = baseline_members;
    rejected_members.pop_back();
    error.clear();
    REQUIRE_EQ(database.replaceAllAndToneSets(rejected_singles, rejected_composites,
        rejected_sets, rejected_members, &error), false);
    REQUIRE_EQ(error.find("ct 2") != std::string::npos, true);
    assert_existing_library_unchanged();

    // Raw authoring serialization stays lossless. This alone does not prove
    // DAW State restoration: its caller must retain the author model before
    // rejecting compilation and warning until the conflict is resolved.
    REQUIRE_EQ(serializeCompositeSoundPayload(tone), saved_input);
    const auto restored = deserializeCompositeSoundPayload(saved_input,
        kCompositeSoundPayloadVersion, &error);
    REQUIRE_EQ(restored.has_value(), true);
    REQUIRE_EQ(restored->layers.front().timbre_automation.size(), std::size_t{2});
    REQUIRE_EQ(mgsCompositeCommandConflict(*restored).has_value(), true);
    database.close();
    std::filesystem::remove(database_path);
}

void testMgsCommandConflictClassificationRequiresResolvedSnapshots() {
    using namespace mgstc::engine;
    CompositeTimbre unresolved;
    unresolved.name = "Unresolved State snapshot";
    auto noise = commandConflictLayer();
    noise.timbre_automation = {
        {.kind = EnvelopeEventKind::Noise, .value = 2, .count = 2},
        {.kind = EnvelopeEventKind::Noise, .value = 3, .count = 2}};
    auto scc = commandConflictLayer();
    scc.source = TimbreSource::Scc;
    scc.envelope_number = 1;
    scc.scc_output_allocation = SccOutputAllocationMode::LegacyBank;
    scc.base_timbre.reset();
    scc.timbre_automation = {{.kind = EnvelopeEventKind::Timbre,
        .value = 0, .count = 1, .target_library_id = 9999,
        .timbre_pick = TimbrePick::Library}};
    unresolved.layers = {noise, scc};
    unresolved.embedded_timbres.clear();

    // Number resolution deliberately supplies a dummy slot. That alone is
    // insufficient for the existing program compiler to resolve a waveform.
    REQUIRE_EQ(resolveTimbreNumbers(unresolved).valid(), true);
    REQUIRE_EQ(findEmbeddedTimbreSnapshot(unresolved, 9999) == nullptr, true);
    REQUIRE_EQ(mgsCompositeCommandConflict(unresolved).has_value(), true);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(unresolved), false);
    TimbreLibrary empty_library;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(unresolved, &empty_library), false);

    auto without_duplicate = unresolved;
    without_duplicate.layers.front().timbre_automation.pop_back();
    auto engine = std::make_unique<EngineCore>();
    REQUIRE_EQ(compileCompositeProgram(*engine, without_duplicate), false);

    TimbreLibraryEntry live_wave;
    live_wave.id = 9999;
    live_wave.name = "Resolved SCC wave";
    live_wave.category = TimbreCategory::Scc;
    live_wave.data_version = 1;
    TimbreLibrary live_library;
    live_library.installEntries({live_wave});
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(unresolved, &live_library), true);
    REQUIRE_EQ(compileCompositeProgram(*engine, without_duplicate,
        {.library = &live_library}), true);

    auto embedded = unresolved;
    embedded.embedded_timbres.push_back(makeSavedTimbreReference(live_wave));
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(embedded), true);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(embedded, &empty_library), true);
    auto base_snapshot = unresolved;
    base_snapshot.layers.back().base_timbre = makeSavedTimbreReference(live_wave);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(base_snapshot), true);

    // Check every assignment, not only the first resolved snapshot.
    auto partially_resolved = unresolved;
    partially_resolved.layers.back().timbre_automation.push_back({
        .kind = EnvelopeEventKind::Timbre, .value = 1, .count = 3,
        .target_library_id = 9998, .timbre_pick = TimbrePick::Library});
    REQUIRE_EQ(resolveTimbreNumbers(partially_resolved).valid(), true);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(partially_resolved, &live_library), false);

    auto relocated = base_snapshot;
    auto& relocated_layer = relocated.layers.back();
    relocated_layer.scc_output_allocation = SccOutputAllocationMode::Contiguous;
    relocated_layer.base_timbre->number_mode = TimbreNumberMode::Manual;
    relocated_layer.base_timbre->manual_number = 0;
    auto old_source = *relocated_layer.base_timbre;
    old_source.library_id = 9998;
    old_source.scc_waveform[0] ^= 1;
    relocated.embedded_timbres.push_back(old_source);
    relocated_layer.timbre_automation.front().target_library_id = old_source.library_id;
    const auto raw_source = serializeCompositeSoundPayload(relocated);
    REQUIRE_EQ(resolveTimbreNumbers(relocated).valid(), false);
    REQUIRE_EQ(compileSccMorph(relocated).valid, true);
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(relocated), true);
    REQUIRE_EQ(serializeCompositeSoundPayload(relocated), raw_source);
    auto legacy_alias = relocated;
    legacy_alias.layers.back().scc_output_allocation = SccOutputAllocationMode::LegacyBank;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(legacy_alias), false);
    auto missing_alias = relocated;
    missing_alias.embedded_timbres.clear();
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(missing_alias), false);
    auto wrong_source = relocated;
    wrong_source.embedded_timbres.front().source = TimbreSource::Opll;
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(wrong_source), false);
    auto invalid_authored = relocated;
    invalid_authored.layers.back().timbre_automation.push_back({
        .kind = EnvelopeEventKind::RegisterWrite, .value = 256, .secondary = 1, .count = 3});
    REQUIRE_EQ(mgsCompositeHasOnlyCommandConflicts(invalid_authored), false);
}
