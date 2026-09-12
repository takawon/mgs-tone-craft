std::uint32_t crc32Of(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (0xEDB88320U ^ (crc >> 1U)) : (crc >> 1U);
        }
    }
    return ~crc;
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    appendU16(bytes, static_cast<std::uint16_t>(value));
    appendU16(bytes, static_cast<std::uint16_t>(value >> 16U));
}

std::vector<std::uint8_t> gzipStored(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes{
        0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const auto length = static_cast<std::uint16_t>(payload.size());
    appendU16(bytes, length);
    appendU16(bytes, static_cast<std::uint16_t>(length ^ 0xFFFFU));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    appendU32(bytes, crc32Of(payload));
    appendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    return bytes;
}

std::vector<std::uint8_t> makeVgm(const std::vector<std::uint8_t>& commands) {
    std::vector<std::uint8_t> bytes(0x40, 0);
    bytes[0] = 'V';
    bytes[1] = 'g';
    bytes[2] = 'm';
    bytes[3] = ' ';
    bytes[8] = 0x00;
    bytes[9] = 0x01;
    bytes.insert(bytes.end(), commands.begin(), commands.end());
    return bytes;
}

std::vector<std::uint8_t> opllWrites(
    const std::array<std::uint8_t, 8>& registers) {
    std::vector<std::uint8_t> commands;
    for (std::uint8_t reg = 0; reg < 8; ++reg) {
        commands.push_back(0x51);
        commands.push_back(reg);
        commands.push_back(registers[reg]);
    }
    commands.push_back(0x66);
    return commands;
}

void testGzipStoredRoundTrip() {
    const std::vector<std::uint8_t> payload{'V', 'g', 'm', ' ', 1, 2, 3, 4};
    const auto gz = gzipStored(payload);
    std::string error;
    const auto inflated = mgstc::engine::inflateGzip(gz, &error);
    REQUIRE_EQ(inflated.has_value(), true);
    REQUIRE_EQ(*inflated, payload);
}

void appendMgsMusicTrack(
    std::vector<std::uint8_t>& bytes,
    std::size_t header,
    int track,
    const std::vector<std::uint8_t>& commands) {
    const auto music_relative =
        static_cast<std::uint16_t>(bytes.size() - header);
    const auto offset = header + 4 + static_cast<std::size_t>(track) * 2;
    bytes[offset] = static_cast<std::uint8_t>(music_relative);
    bytes[offset + 1] = static_cast<std::uint8_t>(music_relative >> 8U);
    bytes.insert(bytes.end(), commands.begin(), commands.end());
}

const mgstc::engine::CompositeTimbre* firstCompositeTimbre(
    const mgstc::engine::ToneImportResult& result) {
    for (const auto& candidate : result.candidates) {
        if (const auto* timbre =
                std::get_if<mgstc::engine::CompositeTimbre>(&candidate.data)) {
            return timbre;
        }
    }
    return nullptr;
}

int countImportedType(
    const mgstc::engine::ToneImportResult& result,
    mgstc::engine::ImportedToneType type) {
    int count = 0;
    for (const auto& candidate : result.candidates) {
        if (candidate.type == type) {
            ++count;
        }
    }
    return count;
}

void testToneImportRejectsEmptyAndUnknown() {
    const auto empty = mgstc::engine::importTones({});
    REQUIRE_EQ(empty.valid(), false);
    REQUIRE_EQ(empty.errors.empty(), false);

    const std::vector<std::uint8_t> random{0x00, 0x01, 0x02, 0x03};
    const auto unknown = mgstc::engine::importTones(random, "bin");
    REQUIRE_EQ(unknown.format, mgstc::engine::ToneImportFormat::Unknown);
}

void testToneImportRejectsCompressedMgs() {
    const std::vector<std::uint8_t> compressed{'M', 'G', 'S', 'A', 0, 0};
    const auto result = mgstc::engine::importTones(compressed, "mgs");
    REQUIRE_EQ(result.valid(), false);
    REQUIRE_EQ(
        result.errors.front().find("compressed") != std::string::npos, true);
}

void testToneImportRejectsCompressedMgsAfterEofMarker() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A, 0x01};
    bytes.resize(0x30, 0);
    const auto packed = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(packed.valid(), false);
    REQUIRE_EQ(
        packed.errors.front().find("compressed") != std::string::npos, true);

    std::vector<std::uint8_t> flagged{'M', 'G', 'S', '3', '\r', '\n', 0x1A, 0x00, 0x80};
    flagged.resize(0x30, 0);
    const auto flagged_result = mgstc::engine::importTones(flagged, "mgs");
    REQUIRE_EQ(flagged_result.valid(), false);
    REQUIRE_EQ(
        flagged_result.errors.front().find("compressed") != std::string::npos,
        true);
}

void testToneImportVgmCompleteOpllAndDuplicates() {
    const std::array<std::uint8_t, 8> registers{
        0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26};
    auto commands = opllWrites(registers);
    commands.pop_back();
    const auto second = opllWrites(registers);
    commands.insert(commands.end(), second.begin(), second.end());
    const auto vgm = makeVgm(commands);
    const auto result = mgstc::engine::importTones(vgm, "vgm", "tone.vgm");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::Vgm);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(result.candidates.front().type, mgstc::engine::ImportedToneType::Opll);
    REQUIRE_EQ(result.candidates.front().name.empty(), true);
    const auto* patch = std::get_if<mgstc::engine::OpllPatchParameters>(
        &result.candidates.front().data);
    REQUIRE_EQ(patch != nullptr, true);
    REQUIRE_EQ(mgstc::engine::encodeOpllPatch(*patch), registers);
}

void testToneImportVgmRejectsPartialAndLongWait() {
    const std::array<std::uint8_t, 8> registers{
        0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26};
    std::vector<std::uint8_t> partial{0x51, 0x00, registers[0], 0x51, 0x02, registers[2], 0x66};
    const auto partial_result = mgstc::engine::importTones(makeVgm(partial), "vgm");
    REQUIRE_EQ(partial_result.valid(), false);

    std::vector<std::uint8_t> delayed{0x51, 0x00, registers[0], 0x61, 0x7D, 0x0B};
    for (std::uint8_t reg = 1; reg < 8; ++reg) {
        delayed.push_back(0x51);
        delayed.push_back(reg);
        delayed.push_back(registers[reg]);
    }
    delayed.push_back(0x66);
    const auto delayed_result = mgstc::engine::importTones(makeVgm(delayed), "vgm");
    REQUIRE_EQ(delayed_result.valid(), false);
}

void testToneImportVgmSccCompleteReorderAndPartial() {
    std::vector<std::uint8_t> commands;
    for (int offset = 31; offset >= 0; --offset) {
        commands.push_back(0xD2);
        commands.push_back(0x00);
        commands.push_back(static_cast<std::uint8_t>(offset));
        commands.push_back(static_cast<std::uint8_t>(offset + 1));
    }
    commands.push_back(0x66);
    const auto result = mgstc::engine::importTones(makeVgm(commands), "vgm");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(result.candidates.front().type, mgstc::engine::ImportedToneType::Scc);
    const auto* wave = std::get_if<mgstc::engine::SccWaveform>(
        &result.candidates.front().data);
    REQUIRE_EQ(wave != nullptr, true);
    REQUIRE_EQ((*wave)[0], static_cast<std::int8_t>(1));
    REQUIRE_EQ((*wave)[31], static_cast<std::int8_t>(32));

    std::vector<std::uint8_t> partial{0xD2, 0x00, 0x00, 0x10, 0xD2, 0x00, 0x01, 0x11, 0x66};
    const auto partial_result = mgstc::engine::importTones(makeVgm(partial), "vgm");
    REQUIRE_EQ(partial_result.valid(), false);
}

void testToneImportVgzMatchesVgm() {
    const std::array<std::uint8_t, 8> registers{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const auto vgm = makeVgm(opllWrites(registers));
    const auto vgz = gzipStored(vgm);
    const auto result = mgstc::engine::importTones(vgz, "vgz", "tone.vgz");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::Vgz);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
}

void testToneImportMmlTonesAndSelfContainedEnvelope() {
    const auto opll_a = mgstc::engine::formatMgsOpllDefinition(
        mgstc::engine::decodeOpllPatch(
            std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}),
        12,
        {});
    const auto opll_b = mgstc::engine::formatMgsOpllDefinition(
        mgstc::engine::decodeOpllPatch(
            std::array<std::uint8_t, 8>{8, 7, 6, 5, 4, 3, 2, 1}),
        20,
        {});
    std::string source = opll_a + opll_b + "@e0 = { @12.f @20.0 }\n9 @e0 c\n";
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::MgsMml);
    int opll_count = 0;
    int composite_count = 0;
    const mgstc::engine::ImportedToneCandidate* composite = nullptr;
    for (const auto& candidate : result.candidates) {
        if (candidate.type == mgstc::engine::ImportedToneType::Opll) {
            ++opll_count;
        }
        if (candidate.type == mgstc::engine::ImportedToneType::Composite) {
            ++composite_count;
            composite = &candidate;
        }
    }
    REQUIRE_EQ(opll_count, 2);
    REQUIRE_EQ(composite_count, 1);
    REQUIRE_EQ(composite != nullptr, true);
    const auto* timbre = std::get_if<mgstc::engine::CompositeTimbre>(
        &composite->data);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(mgstc::engine::validateCompositeTimbre(*timbre).valid(), true);
    REQUIRE_EQ(timbre->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Opll);
    REQUIRE_EQ(timbre->embedded_timbres.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(
        mgstc::engine::findEmbeddedTimbreSnapshot(
            *timbre, timbre->embedded_timbres.front().library_id)
            != nullptr,
        true);
    std::uint64_t first_id = 0;
    for (const auto& event : timbre->layers.front().timbre_automation) {
        if (event.kind != mgstc::engine::EnvelopeEventKind::Timbre) {
            continue;
        }
        REQUIRE_EQ(event.timbre_pick, mgstc::engine::TimbrePick::Library);
        REQUIRE_EQ(event.target_library_id != 0, true);
        REQUIRE_EQ(event.value, 0);
        if (first_id == 0) {
            first_id = event.target_library_id;
        } else {
            REQUIRE_EQ(event.target_library_id != first_id, true);
        }
    }
}

void testToneImportMgsBinaryOpllSccAndEnvelope() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    for (int index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(index + 1));
    }
    bytes.push_back(0x03);
    bytes.push_back(0x00);
    for (int index = 0; index < 32; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(index));
    }
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    const std::vector<std::uint8_t> env{0x0F};
    bytes.push_back(static_cast<std::uint8_t>(env.size()));
    bytes.insert(bytes.end(), env.begin(), env.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x00, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::Mgs);
    REQUIRE_EQ(result.candidates.size() >= 3, true);
}

void testToneImportMgsVoiceTrackDoesNotWalkIntoMusic() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    const auto voice_relative = 0x28;
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    for (int index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(index + 1));
    }
    const auto music_relative = static_cast<std::uint16_t>(
        bytes.size() - header);
    bytes[header + 6] = static_cast<std::uint8_t>(music_relative);
    bytes[header + 7] = static_cast<std::uint8_t>(music_relative >> 8U);
    bytes.push_back(0x30);
    bytes.push_back(0x31);
    bytes.push_back(0xFF);
    REQUIRE_EQ(voice_relative, 0x28);
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::Mgs);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(result.candidates.front().type, mgstc::engine::ImportedToneType::Opll);
}

void testToneImportMgsEnvelopeHoldWaitIsLiteral() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    const std::vector<std::uint8_t> env{0xEF, 0x00, 0x00};
    bytes.push_back(static_cast<std::uint8_t>(env.size()));
    bytes.insert(bytes.end(), env.begin(), env.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x00, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.empty(), false);
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.length_counts,
        static_cast<std::uint32_t>(1));
}

void testToneImportMmlStripsPsgModeNoise() {
    const std::string source = "@e0 = { n7./3.f }\n1 @e0 c\n";
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        result.candidates.front().type,
        mgstc::engine::ImportedToneType::Composite);
    REQUIRE_EQ(result.candidates.front().warnings.empty(), false);
    const auto* timbre = std::get_if<mgstc::engine::CompositeTimbre>(
        &result.candidates.front().data);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.empty(), false);
    int volumes = 0;
    for (const auto& event : timbre->layers.front().volume_envelope.events) {
        if (event.kind == mgstc::engine::EnvelopeEventKind::Volume) {
            ++volumes;
            REQUIRE_EQ(event.value, 15);
        }
    }
    REQUIRE_EQ(volumes, 1);
}

void testToneImportMmlSccTrackEmbedsWaveWithoutEnvelopePatch() {
    mgstc::engine::SccWaveform wave{};
    for (int index = 0; index < 32; ++index) {
        wave[static_cast<std::size_t>(index)] =
            static_cast<std::int8_t>(index + 4);
    }
    const auto source =
        mgstc::engine::formatMgsSccDefinition(wave, 3, {})
        + "@e0 = { f }\n4 @3 @e0 c\n";
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    const mgstc::engine::CompositeTimbre* timbre = nullptr;
    for (const auto& candidate : result.candidates) {
        timbre = std::get_if<mgstc::engine::CompositeTimbre>(&candidate.data);
        if (timbre != nullptr) {
            break;
        }
    }
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(timbre->embedded_timbres.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        timbre->embedded_timbres.front().scc_waveform,
        mgstc::engine::sccWaveformToBytes(wave));
}

void testToneImportMmlSccTrackEmbedsEveryTrackPatch() {
    mgstc::engine::SccWaveform wave_a{};
    mgstc::engine::SccWaveform wave_b{};
    for (int index = 0; index < 32; ++index) {
        wave_a[static_cast<std::size_t>(index)] =
            static_cast<std::int8_t>(index);
        wave_b[static_cast<std::size_t>(index)] =
            static_cast<std::int8_t>(31 - index);
    }
    const auto source =
        mgstc::engine::formatMgsSccDefinition(wave_a, 3, {})
        + mgstc::engine::formatMgsSccDefinition(wave_b, 7, {})
        + "@e0 = { f }\n4 @3 @7 @e0 c\n";
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    const mgstc::engine::CompositeTimbre* timbre = nullptr;
    for (const auto& candidate : result.candidates) {
        timbre = std::get_if<mgstc::engine::CompositeTimbre>(&candidate.data);
        if (timbre != nullptr) {
            break;
        }
    }
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(timbre->embedded_timbres.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        timbre->embedded_timbres.front().scc_waveform,
        mgstc::engine::sccWaveformToBytes(wave_b));
}

void testToneImportDropsZeroTimeEnvelopeLoop() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    const std::vector<std::uint8_t> env{0x40, 0x60, 0x0F};
    bytes.push_back(static_cast<std::uint8_t>(env.size()));
    bytes.insert(bytes.end(), env.begin(), env.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x00, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.empty(), false);
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.loop_start_count.has_value(),
        false);
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.loop_end_count.has_value(),
        false);
}

void testToneImportMgsBinarySccTrackUsesSccLayer() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x03);
    bytes.push_back(0x03);
    for (int index = 0; index < 32; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(index + 5));
    }
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.push_back(0x0F);
    bytes.push_back(0xFF);
    const auto music_relative = static_cast<std::uint16_t>(
        bytes.size() - header);
    bytes[header + 12] = static_cast<std::uint8_t>(music_relative);
    bytes[header + 13] = static_cast<std::uint8_t>(music_relative >> 8U);
    bytes.push_back(0x83);
    bytes.push_back(0x49);
    bytes.push_back(0x00);
    bytes.push_back(0xFF);
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    const mgstc::engine::CompositeTimbre* timbre = nullptr;
    for (const auto& candidate : result.candidates) {
        timbre = std::get_if<mgstc::engine::CompositeTimbre>(&candidate.data);
        if (timbre != nullptr) {
            break;
        }
    }
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(timbre->embedded_timbres.empty(), false);
}

void testToneImportMusicaVcdNamesAndSccEnvelope() {
    std::vector<std::uint8_t> bytes(0x10B8, 0);
    const char name[] = "LEAD";
    std::copy(name, name + 4, bytes.begin());
    bytes[0x05A0] = 0x21;
    bytes[0x05A1] = 0x24;
    bytes[0x05A2] = 0x0A;
    bytes[0x05A3] = 0x02;
    bytes[0x05A4] = 0xF3;
    bytes[0x05A5] = 0xAF;
    bytes[0x05A6] = 0x60;
    bytes[0x05A7] = 0x26;
    const char scc_name[] = "WAVE";
    std::copy(scc_name, scc_name + 4, bytes.begin() + 0x0410);
    bytes[0x09B0] = 0x11;
    bytes[0x09B1] = 0x11;
    bytes[0x09B2] = 0x08;
    bytes[0x09B3] = 0x11;
    for (int index = 0; index < 32; ++index) {
        bytes[0x09B4 + index] = static_cast<std::uint8_t>(index + 3);
    }
    const auto result = mgstc::engine::importTones(bytes, "vcd");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::MusicaVcd);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(result.candidates[0].name, std::string("LEAD"));
    REQUIRE_EQ(result.candidates[1].name, std::string("WAVE"));
    REQUIRE_EQ(result.candidates[1].type, mgstc::engine::ImportedToneType::Scc);
    REQUIRE_EQ(
        result.candidates[1].default_register_as,
        mgstc::engine::ImportRegisterAs::Composite);
    REQUIRE_EQ(result.candidates[1].composite_alternative.has_value(), true);
}

void testToneImportSngWaveAndName() {
    std::vector<std::uint8_t> bytes(40, 0);
    const char name[] = "SINEWAVE";
    std::copy(name, name + 8, bytes.begin());
    for (int index = 0; index < 32; ++index) {
        bytes[8 + index] = static_cast<std::uint8_t>(index);
    }
    const auto result = mgstc::engine::importTones(bytes, "sng");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::SccMusixxSng);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(result.candidates.front().name, std::string("SINEWAVE"));
}

std::vector<std::uint8_t> makeMgsEnvelopeOnlyVoice() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x01);
    bytes.push_back(0x0F);
    bytes.push_back(0xFF);
    return bytes;
}

void requireNoImportedTones(const mgstc::engine::ToneImportResult& result) {
    REQUIRE_EQ(result.valid(), false);
    REQUIRE_EQ(result.candidates.empty(), true);
    REQUIRE_EQ(result.errors.empty(), false);
    REQUIRE_EQ(
        result.errors.front().find("no complete tones") != std::string::npos,
        true);
}

void testToneImportSkipsUnusedEnvelope() {
    requireNoImportedTones(
        mgstc::engine::importTones(makeMgsEnvelopeOnlyVoice(), "mgs"));

    const std::string source = "@e0 = { f }\n";
    const std::vector<std::uint8_t> mml(source.begin(), source.end());
    requireNoImportedTones(mgstc::engine::importTones(mml, "mus"));
}

void testToneImportSkipsOpllRhythmChannelEnvelope() {
    auto bytes = makeMgsEnvelopeOnlyVoice();
    const auto marker = std::find(bytes.begin(), bytes.end(), 0x1A);
    const auto header_off =
        static_cast<std::size_t>(std::distance(bytes.begin(), marker) + 1);
    appendMgsMusicTrack(
        bytes,
        header_off,
        15,
        {0x5C, 0x0E, 0x20, 0x49, 0x00, 0xFF});
    requireNoImportedTones(mgstc::engine::importTones(bytes, "mgs"));

    auto restored = makeMgsEnvelopeOnlyVoice();
    appendMgsMusicTrack(
        restored,
        header_off,
        15,
        {0x5C, 0x0E, 0x20, 0x20, 0x10, 0x5C, 0x0E, 0x00, 0x49, 0x00, 0xFF});
    const auto kept = mgstc::engine::importTones(restored, "mgs");
    REQUIRE_EQ(kept.valid(), true);
    REQUIRE_EQ(
        countImportedType(kept, mgstc::engine::ImportedToneType::Composite),
        1);
    const auto* timbre = firstCompositeTimbre(kept);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Opll);

    const std::string nine_voice =
        "@e0 = { f }\n15 y14,32 @e0 c\n";
    const std::vector<std::uint8_t> nine_bytes(
        nine_voice.begin(), nine_voice.end());
    const auto nine = mgstc::engine::importTones(nine_bytes, "mus");
    REQUIRE_EQ(
        countImportedType(nine, mgstc::engine::ImportedToneType::Composite),
        0);

    const std::string key_bits =
        "@e0 = { f }\n15 y14,48 @e0 c\n";
    const std::vector<std::uint8_t> key_bytes(
        key_bits.begin(), key_bits.end());
    const auto keyed = mgstc::engine::importTones(key_bytes, "mus");
    REQUIRE_EQ(
        countImportedType(keyed, mgstc::engine::ImportedToneType::Composite),
        0);

    const std::string psg_y =
        "@e0 = { f }\n1 y14,32\n15 @e0 c\n";
    const std::vector<std::uint8_t> psg_bytes(psg_y.begin(), psg_y.end());
    const auto psg = mgstc::engine::importTones(psg_bytes, "mus");
    REQUIRE_EQ(
        countImportedType(psg, mgstc::engine::ImportedToneType::Composite),
        1);

    const std::string f_letter =
        "@e0 = { f }\nF y14,32 @e0 c\n";
    const std::vector<std::uint8_t> f_bytes(
        f_letter.begin(), f_letter.end());
    const auto f_track = mgstc::engine::importTones(f_bytes, "mus");
    REQUIRE_EQ(
        countImportedType(f_track, mgstc::engine::ImportedToneType::Composite),
        0);

    const std::string g_y_f_env =
        "@e0 = { f }\nF @e0 c\nG y14,32\n";
    const std::vector<std::uint8_t> g_y_bytes(
        g_y_f_env.begin(), g_y_f_env.end());
    REQUIRE_EQ(
        countImportedType(
            mgstc::engine::importTones(g_y_bytes, "mus"),
            mgstc::engine::ImportedToneType::Composite),
        0);

    const std::string h_y =
        "@e0 = { f }\n15 @e0 c\n17 y14,32\n";
    const std::vector<std::uint8_t> h_y_bytes(h_y.begin(), h_y.end());
    REQUIRE_EQ(
        countImportedType(
            mgstc::engine::importTones(h_y_bytes, "mus"),
            mgstc::engine::ImportedToneType::Composite),
        0);

    auto other_ch = makeMgsEnvelopeOnlyVoice();
    appendMgsMusicTrack(other_ch, header_off, 15, {0x49, 0x00, 0xFF});
    appendMgsMusicTrack(
        other_ch, header_off, 16, {0x5C, 0x0E, 0x20, 0xFF});
    requireNoImportedTones(mgstc::engine::importTones(other_ch, "mgs"));

    const std::string melody_y =
        "@e0 = { f }\n9 y14,32\n15 @e0 c\n";
    const std::vector<std::uint8_t> melody_bytes(
        melody_y.begin(), melody_y.end());
    const auto from_melody = mgstc::engine::importTones(melody_bytes, "mus");
    REQUIRE_EQ(
        countImportedType(
            from_melody, mgstc::engine::ImportedToneType::Composite),
        1);
}

void testToneImportRhythmHeaderAndMelodyTrack() {
    auto rhythm_header = makeMgsEnvelopeOnlyVoice();
    const auto marker = std::find(
        rhythm_header.begin(), rhythm_header.end(), 0x1A);
    const auto header_off =
        static_cast<std::size_t>(std::distance(rhythm_header.begin(), marker) + 1);
    rhythm_header[header_off + 1] = 0x01;
    appendMgsMusicTrack(
        rhythm_header, header_off, 15, {0x49, 0x00, 0xFF});
    const auto skipped = mgstc::engine::importTones(rhythm_header, "mgs");
    REQUIRE_EQ(
        countImportedType(skipped, mgstc::engine::ImportedToneType::Composite),
        0);

    auto melody = makeMgsEnvelopeOnlyVoice();
    melody[header_off + 1] = 0x01;
    appendMgsMusicTrack(melody, header_off, 9, {0x49, 0x00, 0xFF});
    appendMgsMusicTrack(
        melody, header_off, 15, {0x5C, 0x0E, 0x20, 0xFF});
    const auto kept = mgstc::engine::importTones(melody, "mgs");
    REQUIRE_EQ(
        countImportedType(kept, mgstc::engine::ImportedToneType::Composite),
        1);

    const std::string source =
        "#opll_mode 1\n@e0 = { f }\n@e1 = { 8 }\n"
        "15 y14,32 @e0 c y14,0 @e1 c\n";
    const std::vector<std::uint8_t> mml(source.begin(), source.end());
    const auto text = mgstc::engine::importTones(mml, "mus");
    REQUIRE_EQ(text.valid(), true);
    REQUIRE_EQ(
        countImportedType(text, mgstc::engine::ImportedToneType::Composite),
        1);
}

void testToneImportUsedEnvelopeWithoutPatchUsesDefault() {
    const std::string opll_source = "@e0 = { f }\n9 @e0 c\n";
    const std::vector<std::uint8_t> opll_bytes(
        opll_source.begin(), opll_source.end());
    const auto opll = mgstc::engine::importTones(opll_bytes, "mus");
    const auto* opll_timbre = firstCompositeTimbre(opll);
    REQUIRE_EQ(opll_timbre != nullptr, true);
    REQUIRE_EQ(opll_timbre->layers.front().source, mgstc::engine::TimbreSource::Opll);
    REQUIRE_EQ(opll_timbre->layers.front().base_opll_rom.has_value(), true);
    REQUIRE_EQ(*opll_timbre->layers.front().base_opll_rom, 0);
    REQUIRE_EQ(opll_timbre->embedded_timbres.empty(), true);

    const std::string rom_source = "@e0 = { f }\n9 @5 @e0 c\n";
    const std::vector<std::uint8_t> rom_bytes(
        rom_source.begin(), rom_source.end());
    const auto rom = mgstc::engine::importTones(rom_bytes, "mus");
    const auto* rom_timbre = firstCompositeTimbre(rom);
    REQUIRE_EQ(rom_timbre != nullptr, true);
    REQUIRE_EQ(rom_timbre->layers.front().base_opll_rom.has_value(), true);
    REQUIRE_EQ(*rom_timbre->layers.front().base_opll_rom, 5);

    const std::string scc_source = "@e0 = { f }\n4 @e0 c\n";
    const std::vector<std::uint8_t> scc_bytes(
        scc_source.begin(), scc_source.end());
    const auto scc = mgstc::engine::importTones(scc_bytes, "mus");
    const auto* scc_timbre = firstCompositeTimbre(scc);
    REQUIRE_EQ(scc_timbre != nullptr, true);
    REQUIRE_EQ(scc_timbre->layers.front().source, mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(scc_timbre->layers.front().base_timbre.has_value(), true);
    const auto triangle = mgstc::engine::generateSccPreset(
        mgstc::engine::SccWavePreset::Triangle,
        mgstc::engine::SccHarmonic::One);
    REQUIRE_EQ(
        scc_timbre->layers.front().base_timbre->scc_waveform,
        mgstc::engine::sccWaveformToBytes(triangle));
}

void testImportedLibraryEntryKeepsEmptyName() {
    mgstc::engine::ImportedToneCandidate candidate;
    candidate.type = mgstc::engine::ImportedToneType::Opll;
    candidate.data = mgstc::engine::decodeOpllPatch(
        std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    candidate.default_register_as = mgstc::engine::ImportRegisterAs::Opll;
    const auto entry = mgstc::engine::makeImportedLibraryEntry(
        candidate, mgstc::engine::ImportRegisterAs::Opll);
    REQUIRE_EQ(entry.name.empty(), true);
    REQUIRE_EQ(entry.category, mgstc::engine::TimbreCategory::Opll);
}
