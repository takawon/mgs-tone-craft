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
    const auto inflated = mgstc::engine::inflateGzip(gz, payload.size(), &error);
    REQUIRE_EQ(inflated.has_value(), true);
    REQUIRE_EQ(*inflated, payload);
    REQUIRE_EQ(inflated->capacity() <= payload.size(), true);
}

void testGzipOutputLimitStoredAndMalformed() {
    const std::vector<std::uint8_t> payload(4097, 0x5A);
    const auto gz = gzipStored(payload);
    std::string error;
    const auto exact = mgstc::engine::inflateGzip(gz, payload.size(), &error);
    REQUIRE_EQ(exact.has_value(), true);
    REQUIRE_EQ(*exact, payload);
    REQUIRE_EQ(exact->capacity() <= payload.size(), true);
    const auto over = mgstc::engine::inflateGzip(gz, payload.size() - 1, &error);
    REQUIRE_EQ(over.has_value(), false);
    REQUIRE_EQ(error, std::string("gzip expanded data exceeds output byte limit"));

    const auto empty = mgstc::engine::inflateGzip(gzipStored({}), 0, &error);
    REQUIRE_EQ(empty.has_value(), true);
    REQUIRE_EQ(empty->capacity(), std::size_t{0});
    REQUIRE_EQ(mgstc::engine::inflateGzip(gz, 0, &error).has_value(), false);

    auto corrupt = gz;
    corrupt[corrupt.size() - 8] ^= 1;
    REQUIRE_EQ(mgstc::engine::inflateGzip(corrupt, payload.size(), &error).has_value(), false);
    REQUIRE_EQ(error, std::string("gzip crc mismatch"));
    corrupt = gz;
    corrupt.back() ^= 1;
    REQUIRE_EQ(mgstc::engine::inflateGzip(corrupt, payload.size(), &error).has_value(), false);
    REQUIRE_EQ(error, std::string("gzip size mismatch"));
    corrupt = gz;
    corrupt.erase(corrupt.end() - 9);
    REQUIRE_EQ(mgstc::engine::inflateGzip(corrupt, payload.size(), &error).has_value(), false);
    REQUIRE_EQ(error, std::string("truncated stored deflate payload"));
    corrupt = gz;
    corrupt[13] ^= 1;
    REQUIRE_EQ(mgstc::engine::inflateGzip(corrupt, payload.size(), &error).has_value(), false);
    REQUIRE_EQ(error, std::string("invalid stored deflate length"));
}

void testGzipOutputLimitHuffmanGrowth() {
    // Self-authored repeated-byte payloads encoded with fixed/dynamic DEFLATE.
    const std::vector<std::uint8_t> fixed{
        0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x0A,
        0x73, 0x72, 0x1A, 0x05, 0xA3, 0x60, 0x14, 0x8C, 0x82, 0x51,
        0x30, 0x0A, 0x46, 0xC1, 0x28, 0x18, 0x05, 0xA3, 0x60, 0x14,
        0x8C, 0x82, 0x51, 0x30, 0x0A, 0x46, 0xC1, 0xB0, 0x07, 0x00,
        0xE9, 0x8F, 0x43, 0x2F, 0x01, 0x10, 0x00, 0x00};
    std::vector<std::uint8_t> dynamic{
        0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A,
        0xED, 0xC1, 0x81, 0x00, 0x00, 0x00, 0x00, 0xC3, 0x20, 0xB6,
        0xF9, 0x4B, 0xFD, 0x20, 0x55, 0x01};
    dynamic.resize(88, 0);
    dynamic.insert(dynamic.end(), {
        0xC0, 0x0D, 0xAD, 0x86, 0xC1, 0x3E, 0x01, 0x00, 0x01, 0x00});

    const auto check = [](const auto& gz, std::size_t size, std::uint8_t value) {
        std::string error;
        const auto exact = mgstc::engine::inflateGzip(gz, size, &error);
        REQUIRE_EQ(exact.has_value(), true);
        REQUIRE_EQ(*exact, std::vector<std::uint8_t>(size, value));
        // Non-geometric limits force the final growth to clamp its capacity.
        REQUIRE_EQ(exact->capacity() <= size, true);
        REQUIRE_EQ(mgstc::engine::inflateGzip(gz, size - 1, &error).has_value(), false);
        REQUIRE_EQ(error, std::string("gzip expanded data exceeds output byte limit"));
        // The footer may claim any size, including a wrapped 32-bit value.
        // A small/forged ISIZE must never bypass the incremental limit.
        auto forged = gz;
        std::fill(forged.end() - 4, forged.end(), 0);
        REQUIRE_EQ(mgstc::engine::inflateGzip(forged, size - 1, &error).has_value(), false);
        REQUIRE_EQ(error, std::string("gzip expanded data exceeds output byte limit"));
    };
    check(fixed, 4097, 'B');
    check(dynamic, 65537, 'A');
    auto truncated = dynamic;
    truncated.erase(truncated.end() - 9);
    std::string error;
    REQUIRE_EQ(mgstc::engine::inflateGzip(truncated, 65537, &error).has_value(), false);
    REQUIRE_EQ(error.empty(), false);
}

void testGzipOutputLimitLiteral() {
    const std::vector<std::uint8_t> payload{'V', 'g', 'm', ' ', 1, 2, 3, 4};
    // Fixed Huffman literals, without a length/distance match.
    const std::vector<std::uint8_t> gz{
        0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 3,
        0x0B, 0x4B, 0xCF, 0x55, 0x60, 0x64, 0x62, 0x66, 0x01, 0x00,
        0x12, 0x40, 0xA9, 0x5A, 8, 0, 0, 0};
    std::string error;
    const auto exact = mgstc::engine::inflateGzip(gz, payload.size(), &error);
    REQUIRE_EQ(exact.has_value(), true);
    REQUIRE_EQ(*exact, payload);
    REQUIRE_EQ(exact->capacity() <= payload.size(), true);
    REQUIRE_EQ(mgstc::engine::inflateGzip(gz, payload.size() - 1, &error).has_value(), false);
    REQUIRE_EQ(error, std::string("gzip expanded data exceeds output byte limit"));
}

void testToneImportReportsGzipFailureWithoutExtension() {
    auto gz = gzipStored(makeVgm({0x66}));
    gz[gz.size() - 8] ^= 1;
    const auto result = mgstc::engine::importTones(gz);
    REQUIRE_EQ(result.valid(), false);
    REQUIRE_EQ(result.candidates.empty(), true);
    REQUIRE_EQ(result.errors.size(), std::size_t{1});
    REQUIRE_EQ(result.errors.front(), std::string("gzip crc mismatch"));
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
    REQUIRE_EQ(result.candidates.front().name, std::string("tone - 00"));
    const auto* patch = std::get_if<mgstc::engine::OpllPatchParameters>(
        &result.candidates.front().data);
    REQUIRE_EQ(patch != nullptr, true);
    REQUIRE_EQ(mgstc::engine::encodeOpllPatch(*patch), registers);
}

void testToneImportVgmDataBlockBounds() {
    using namespace mgstc::engine;
    const std::array<std::uint8_t, 8> first{
        0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26};
    auto second = first;
    second[0] ^= 1;
    auto prefix = opllWrites(first);
    prefix.pop_back();
    const auto suffix = opllWrites(second);
    const auto block = [](std::uint32_t size,
                          std::span<const std::uint8_t> payload,
                          std::uint8_t type = 0) {
        std::vector<std::uint8_t> bytes{0x67, 0x66, type};
        appendU32(bytes, size);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    };
    const auto expect_prefix = [&](const std::vector<std::uint8_t>& tail,
                                   std::size_t count) {
        auto commands = prefix;
        commands.insert(commands.end(), tail.begin(), tail.end());
        const auto result = importTones(makeVgm(commands), "vgm");
        REQUIRE_EQ(result.valid(), true);
        REQUIRE_EQ(result.candidates.size(), count);
        REQUIRE_EQ(encodeOpllPatch(std::get<OpllPatchParameters>(result.candidates[0].data)), first);
        if (count == 2) {
            REQUIRE_EQ(encodeOpllPatch(std::get<OpllPatchParameters>(result.candidates[1].data)), second);
        }
    };

    const std::array<std::uint8_t, 3> payload{0x66, 0x51, 0x00};
    expect_prefix(block(3, payload), 1); // Payload ends exactly at EOF.
    expect_prefix(block(4, payload), 1); // One byte short; keep earlier candidates.
    for (std::size_t header_bytes = 0; header_bytes < 6; ++header_bytes) {
        auto truncated = block(0, {});
        truncated.resize(1 + header_bytes);
        expect_prefix(truncated, 1);
    }
    for (const auto size : {0x7FFFFFFFU, 0xFFFFFFFBU, 0xFFFFFFFFU}) {
        auto oversized = block(size, {}, 0xE0);
        oversized.insert(oversized.end(), suffix.begin(), suffix.end());
        // A wrapped header advance must not admit later bytes as commands.
        expect_prefix(oversized, 1);
        const auto result = importTones(makeVgm(oversized), "vgm");
        REQUIRE_EQ(result.candidates.empty(), true);
        REQUIRE_EQ(result.valid(), false);
    }
    auto consecutive = block(0, {});
    const auto next = block(3, payload);
    consecutive.insert(consecutive.end(), next.begin(), next.end());
    consecutive.insert(consecutive.end(), suffix.begin(), suffix.end());
    expect_prefix(consecutive, 2);
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
    REQUIRE_EQ(result.candidates.front().name, std::string("00"));
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
    REQUIRE_EQ(result.candidates.front().name, std::string("tone - 00"));
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
    REQUIRE_EQ(
        composite->register_choices.size() >= static_cast<std::size_t>(2),
        true);
    bool has_opll_choice = false;
    for (const auto choice : composite->register_choices) {
        if (choice == mgstc::engine::ImportRegisterAs::Opll) {
            has_opll_choice = true;
        }
    }
    REQUIRE_EQ(has_opll_choice, true);
    const auto extracted = mgstc::engine::makeImportedLibraryEntry(
        *composite, mgstc::engine::ImportRegisterAs::Opll);
    REQUIRE_EQ(extracted.category, mgstc::engine::TimbreCategory::Opll);
    REQUIRE_EQ(extracted.opll_registers[0] != 0, true);
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
        REQUIRE_EQ(event.value == 12 || event.value == 20, true);
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

void testToneImportMgsLoopEndingInAutomaticRamp() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    const std::vector<std::uint8_t> env{0x0F, 0x40, 0x28, 0x03, 0x60};
    bytes.push_back(static_cast<std::uint8_t>(env.size()));
    bytes.insert(bytes.end(), env.begin(), env.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x00, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    const auto& layer = timbre->layers.front();
    const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(layer, 0);
    REQUIRE_EQ(formatted.valid(), true);
    const std::vector<std::uint8_t> ramp{0x28, 0x03};
    REQUIRE_EQ(std::search(formatted.bytecode.begin(),
                   formatted.bytecode.end(), ramp.begin(), ramp.end())
            != formatted.bytecode.end(), true);
    REQUIRE_EQ(std::find(formatted.bytecode.begin(),
                   formatted.bytecode.end(), 0x60)
            != formatted.bytecode.end(), true);
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
    const mgstc::engine::ImportedToneCandidate* composite = nullptr;
    for (const auto& candidate : result.candidates) {
        if (candidate.type == mgstc::engine::ImportedToneType::Composite) {
            composite = &candidate;
            break;
        }
    }
    REQUIRE_EQ(composite != nullptr, true);
    bool has_scc_choice = false;
    for (const auto choice : composite->register_choices) {
        if (choice == mgstc::engine::ImportRegisterAs::Scc) {
            has_scc_choice = true;
        }
    }
    REQUIRE_EQ(has_scc_choice, true);
    const auto extracted = mgstc::engine::makeImportedLibraryEntry(
        *composite, mgstc::engine::ImportRegisterAs::Scc);
    REQUIRE_EQ(extracted.category, mgstc::engine::TimbreCategory::Scc);
    REQUIRE_EQ(extracted.scc_waveform, mgstc::engine::sccWaveformToBytes(wave));
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
    const char psg_name[] = "BASS";
    std::copy(psg_name, psg_name + 4, bytes.begin() + 0x0320);
    bytes[0x08C0] = 0x11;
    bytes[0x08C1] = 0x11;
    bytes[0x08C2] = 0x08;
    bytes[0x08C3] = 0x11;
    bytes[0x08C4] = 0x0C;
    bytes[0x08C5] = 0x00;
    bytes[0x08C6] = 0x00;
    bytes[0x08C7] = 0x00;
    const char scc_name[] = "WAVE";
    std::copy(scc_name, scc_name + 4, bytes.begin() + 0x0410);
    bytes[0x09B0] = 0x11;
    bytes[0x09B1] = 0x11;
    bytes[0x09B2] = 0x08;
    bytes[0x09B3] = 0x11;
    for (int index = 0; index < 32; ++index) {
        bytes[0x09B4 + index] = static_cast<std::uint8_t>(index + 3);
    }
    const char raw_name[] = "RAW";
    std::copy(raw_name, raw_name + 3, bytes.begin() + 0x0418);
    bytes[0x09D4] = 0xF1;
    bytes[0x09D5] = 0xF1;
    bytes[0x09D6] = 0x00;
    bytes[0x09D7] = 0xF1;
    for (int index = 0; index < 32; ++index) {
        bytes[0x09D8 + index] = static_cast<std::uint8_t>(40 + index);
    }
    bytes[0x09F8] = 0xF1;
    bytes[0x09F9] = 0xF1;
    bytes[0x09FA] = 0x00;
    bytes[0x09FB] = 0xF1;
    const auto result = mgstc::engine::importTones(bytes, "vcd");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::MusicaVcd);
    REQUIRE_EQ(result.candidates.size(), static_cast<std::size_t>(4));
    REQUIRE_EQ(result.candidates[0].name, std::string("LEAD"));
    REQUIRE_EQ(result.candidates[0].type, mgstc::engine::ImportedToneType::Opll);
    REQUIRE_EQ(
        mgstc::engine::importedCandidateDisplaysAsComposite(
            result.candidates[0]),
        false);
    REQUIRE_EQ(result.candidates[1].name, std::string("BASS"));
    REQUIRE_EQ(
        result.candidates[1].type, mgstc::engine::ImportedToneType::Composite);
    REQUIRE_EQ(
        mgstc::engine::importedCandidateDisplaysAsComposite(
            result.candidates[1]),
        true);
    const auto* psg = std::get_if<mgstc::engine::CompositeTimbre>(
        &result.candidates[1].data);
    REQUIRE_EQ(psg != nullptr, true);
    REQUIRE_EQ(psg->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(psg->layers.front().source, mgstc::engine::TimbreSource::Psg);
    REQUIRE_EQ(psg->layers.front().key_off_hang, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(
        psg->layers.front().envelope_timeline.loop_start_count.has_value(),
        false);
    REQUIRE_EQ(
        psg->layers.front().envelope_timeline.loop_end_count.has_value(),
        false);
    REQUIRE_EQ(
        psg->layers.front().volume_envelope.rate.tone_mode,
        static_cast<std::uint8_t>(3));
    REQUIRE_EQ(
        psg->layers.front().volume_envelope.rate.noise,
        static_cast<std::uint8_t>(12));
    for (const auto& event : psg->layers.front().volume_envelope.events) {
        REQUIRE_EQ(
            event.kind == mgstc::engine::EnvelopeEventKind::Volume
                && event.count == 0
                && event.value == 0,
            false);
    }
    REQUIRE_EQ(result.candidates[2].name, std::string("WAVE"));
    REQUIRE_EQ(
        result.candidates[2].type, mgstc::engine::ImportedToneType::Composite);
    REQUIRE_EQ(
        result.candidates[2].default_register_as,
        mgstc::engine::ImportRegisterAs::Composite);
    REQUIRE_EQ(result.candidates[2].composite_alternative.has_value(), true);
    REQUIRE_EQ(
        result.candidates[2].register_choices.size()
            >= static_cast<std::size_t>(2),
        true);
    REQUIRE_EQ(
        mgstc::engine::importedCandidateDisplaysAsComposite(
            result.candidates[2]),
        true);
    const auto imported = mgstc::engine::makeImportedComposite(
        result.candidates[2], mgstc::engine::ImportRegisterAs::Composite);
    REQUIRE_EQ(imported.has_value(), true);
    REQUIRE_EQ(imported->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        imported->layers.front().source, mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(
        imported->layers.front().key_off_hang, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(
        imported->layers.front().envelope_timeline.loop_start_count.has_value(),
        false);
    REQUIRE_EQ(
        imported->layers.front().envelope_timeline.loop_end_count.has_value(),
        false);
    REQUIRE_EQ(imported->layers.front().base_timbre.has_value(), true);
    REQUIRE_EQ(
        imported->layers.front().base_timbre->scc_waveform[0],
        static_cast<std::uint8_t>(3));
    REQUIRE_EQ(
        imported->layers.front().base_timbre->scc_waveform[31],
        static_cast<std::uint8_t>(34));
    REQUIRE_EQ(result.candidates[3].name, std::string("RAW"));
    REQUIRE_EQ(result.candidates[3].type, mgstc::engine::ImportedToneType::Scc);
    REQUIRE_EQ(result.candidates[3].composite_alternative.has_value(), false);
    REQUIRE_EQ(
        mgstc::engine::importedCandidateDisplaysAsComposite(
            result.candidates[3]),
        false);
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

std::vector<std::uint8_t> makeMgsRawEnvelopeVoice(const std::vector<std::uint8_t>& body) {
    auto bytes = makeMgsEnvelopeOnlyVoice();
    const auto marker = std::find(bytes.begin(), bytes.end(), 0x1A);
    const auto header = static_cast<std::size_t>(std::distance(bytes.begin(), marker) + 1);
    bytes.resize(header + 0x28);
    REQUIRE_EQ(body.size() <= 255, true);
    bytes.insert(bytes.end(), {0x02, 0x00, 0x00, static_cast<std::uint8_t>(body.size())});
    bytes.insert(bytes.end(), body.begin(), body.end());
    bytes.push_back(0xFF);
    return bytes;
}

void testImportedZeroCountRampNormalizesOneTickVolume() {
    using namespace mgstc::engine;
    struct Case {
        std::vector<std::uint8_t> raw;
        std::vector<std::uint8_t> normalized;
        std::vector<std::uint8_t> expected_volumes;
    };
    const std::vector<Case> cases{
        {{0x28, 0}, {8}, {8, 8, 8}},
        {{0x0F, 0x20, 0, 0x12, 3, 8}, {15, 0, 0x12, 3, 8}, {15, 0, 8, 8}},
        {{0x28, 0, 0x23, 0, 0x12, 1, 15}, {8, 3, 0x12, 1, 15}, {8, 3, 15, 15}},
        {{0xEF, 3, 0x25, 0}, {0xEF, 3, 5}, {15, 15, 15, 5, 5}},
    };
    for (const auto& item : cases) {
        const auto imported = importTones(makeMgsRawEnvelopeVoice(item.raw), "mgs");
        REQUIRE_EQ(imported.valid(), true);
        const auto* timbre = firstCompositeTimbre(imported);
        REQUIRE_EQ(timbre != nullptr, true);
        const auto& layer = timbre->layers.front();
        REQUIRE_EQ(std::all_of(layer.volume_envelope.events.begin(), layer.volume_envelope.events.end(),
            [](const auto& event) { return !event.automatic && event.secondary == 0; }), true);
        const auto formatted = formatMgsCompositeEnvelope(layer, 0);
        REQUIRE_EQ(formatted.valid(), true);
        REQUIRE_EQ(formatted.bytecode, item.normalized);
        REQUIRE_EQ(sampleSequenceEnvelopeVolumes(formatted.bytecode, item.expected_volumes.size()), item.expected_volumes);
        const auto restored = deserializeCompositeSoundPayload(serializeCompositeSoundPayload(*timbre),
            kCompositeSoundPayloadVersion);
        REQUIRE_EQ(restored.has_value(), true);
        REQUIRE_EQ(restored->layers.front().volume_envelope, layer.volume_envelope);
        REQUIRE_EQ(restored->layers.front().pitch_envelope, layer.pitch_envelope);
        REQUIRE_EQ(formatMgsCompositeEnvelope(restored->layers.front(), 0).bytecode, item.normalized);
    }
    const auto imported = importTones(makeMgsRawEnvelopeVoice({15, 0x20, 0, 0x12, 3, 8}), "mgs");
    const auto& layer = firstCompositeTimbre(imported)->layers.front();
    REQUIRE_EQ(layer.volume_envelope.events[1].count, 1U);
    REQUIRE_EQ(layer.volume_envelope.events[2].count, 2U);
    REQUIRE_EQ(layer.pitch_envelope.events.front().count, 2U);
}

void testImportedZeroCountRampKeepsLoopAndBoundaries() {
    using namespace mgstc::engine;
    struct Case { std::vector<std::uint8_t> raw, normalized, volumes; std::uint32_t start, end; };
    const std::vector<Case> cases{
        {{0x40, 0x28, 0, 3, 0x60}, {0x40, 8, 3, 0x60}, {8, 3, 8, 3, 8}, 0, 2},
        {{0x2F, 0, 0x40, 0x28, 0, 0x60}, {15, 0x40, 8, 0x60}, {15, 8, 8, 8}, 1, 2},
        {{0x40, 0x28, 0, 0x60, 0x12, 1, 15}, {0x40, 8, 0x60}, {8, 8, 8}, 0, 1},
        {{0x40, 8, 0x60, 0x23, 0, 0x12, 1, 15}, {0x40, 8, 0x60}, {8, 8, 8}, 0, 1},
    };
    for (const auto& item : cases) {
        const auto imported = importTones(makeMgsRawEnvelopeVoice(item.raw), "mgs");
        REQUIRE_EQ(imported.valid(), true);
        const auto* timbre = firstCompositeTimbre(imported);
        REQUIRE_EQ(timbre != nullptr, true);
        const auto& layer = timbre->layers.front();
        REQUIRE_EQ(layer.envelope_timeline.loop_start_count, std::optional<std::uint32_t>{item.start});
        REQUIRE_EQ(layer.envelope_timeline.loop_end_count, std::optional<std::uint32_t>{item.end});
        const auto restored = deserializeCompositeSoundPayload(serializeCompositeSoundPayload(*timbre),
            kCompositeSoundPayloadVersion);
        REQUIRE_EQ(restored.has_value(), true);
        REQUIRE_EQ(restored->layers.front().volume_envelope, layer.volume_envelope);
        const auto formatted = formatMgsCompositeEnvelope(restored->layers.front(), 0);
        REQUIRE_EQ(formatted.valid(), true);
        REQUIRE_EQ(formatted.bytecode, item.normalized);
        REQUIRE_EQ(sampleSequenceEnvelopeVolumes(formatted.bytecode, item.volumes.size()), item.volumes);
    }
    const auto boundary = importTones(makeMgsRawEnvelopeVoice({0xEF, 255, 0xEF, 255, 1, 0x20, 0}), "mgs");
    REQUIRE_EQ(boundary.valid(), true);
    const auto* timbre = firstCompositeTimbre(boundary);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.front().envelope_timeline.length_counts, 512U);
    REQUIRE_EQ(timbre->layers.front().volume_envelope.events.back().count, 511U);
    const auto formatted = formatMgsCompositeEnvelope(timbre->layers.front(), 0);
    REQUIRE_EQ(formatted.valid(), true);
    REQUIRE_EQ(sampleSequenceEnvelopeVolumes(formatted.bytecode, 513)[511], std::uint8_t{0});
    const auto beyond = importTones(makeMgsRawEnvelopeVoice({0xEF, 255, 0xEF, 255, 0xEF, 2, 0x20, 0}), "mgs");
    // 512 is the editor's UI ceiling, not the imported model's hard limit.
    REQUIRE_EQ(beyond.valid(), true);
    const auto* extended = firstCompositeTimbre(beyond);
    REQUIRE_EQ(extended != nullptr, true);
    REQUIRE_EQ(extended->layers.front().envelope_timeline.length_counts, 513U);
    REQUIRE_EQ(extended->layers.front().volume_envelope.events.back().count, 512U);
    const auto extended_output = formatMgsCompositeEnvelope(extended->layers.front(), 0);
    REQUIRE_EQ(extended_output.valid(), true);
    REQUIRE_EQ(sampleSequenceEnvelopeVolumes(extended_output.bytecode, 514)[512], std::uint8_t{0});

    const auto trailing = importTones(makeMgsRawEnvelopeVoice({0x40, 8, 0x60, 0x23, 0, 0x12, 1, 15}), "mgs");
    REQUIRE_EQ(trailing.valid(), true);
    const auto* trailing_timbre = firstCompositeTimbre(trailing);
    REQUIRE_EQ(trailing_timbre != nullptr, true);
    const auto& trailing_layer = trailing_timbre->layers.front();
    const auto outside = std::find_if(trailing_layer.volume_envelope.events.begin(),
        trailing_layer.volume_envelope.events.end(), [](const EnvelopeEvent& event) {
            return event.count == 1 && event.value == 3 && !event.automatic;
        });
    REQUIRE_EQ(outside != trailing_layer.volume_envelope.events.end(), true);
    REQUIRE_EQ(outside->after_loop_start, false);
    REQUIRE_EQ(trailing_layer.pitch_envelope.events.front().count, 2U);
    REQUIRE_EQ(trailing_layer.pitch_envelope.events.front().after_loop_start, false);
}

void testLegacyImportedZeroSpanPreservesAuthoringAndStopsOutput() {
    using namespace mgstc::engine;
    auto timbre = defaultCompositeTimbre();
    auto& layer = timbre.layers.front();
    layer.volume_envelope.kind = EnvelopeKind::Sequence;
    layer.envelope_timeline.length_counts = 2;
    layer.volume_envelope.events = {
        {.kind = EnvelopeEventKind::Volume, .value = 15, .count = 0},
        {.kind = EnvelopeEventKind::Volume, .value = 0, .secondary = 2, .count = 1, .automatic = true},
        {.kind = EnvelopeEventKind::Volume, .value = 8, .count = 1},
    };
    const auto before = layer.volume_envelope;
    const auto restored = deserializeCompositeSoundPayload(serializeCompositeSoundPayload(timbre),
        kCompositeSoundPayloadVersion);
    REQUIRE_EQ(restored.has_value(), true);
    REQUIRE_EQ(restored->layers.front().volume_envelope, before);
    const auto rejected = formatMgsCompositeEnvelope(restored->layers.front(), 0);
    REQUIRE_EQ(rejected.valid(), false);
    REQUIRE_EQ(rejected.hasIssue(MgsEnvelopeIssue::AmbiguousImportedAutomaticVolume), true);
    REQUIRE_EQ(rejected.ambiguous_imported_automatic_counts, (std::vector<std::uint32_t>{1}));
    REQUIRE_EQ(rejected.bytecode.empty(), true);
    REQUIRE_EQ(rejected.definition.empty(), true);
    REQUIRE_EQ(layer.volume_envelope, before);
    // A same-count authored stack has no imported start marker and remains
    // governed by its established authoring convention, not this guard.
    layer.volume_envelope.events[1].secondary = 0;
    REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).valid(), true);
    // A one-count imported ramp marker is not a zero-span legacy marker.
    layer.volume_envelope.events.resize(2);
    layer.volume_envelope.events[1].secondary = 1;
    layer.volume_envelope.events[1].count = 1;
    REQUIRE_EQ(formatMgsCompositeEnvelope(layer, 0).hasIssue(MgsEnvelopeIssue::AmbiguousImportedAutomaticVolume), false);
    std::vector<std::string> issues;
    REQUIRE_EQ(parseMgsSequenceBody(",,f.0=0", layer, issues), false);
    REQUIRE_EQ(issues.empty(), false);
}

void testToneImportUnusedEnvelopeAsPsg() {
    const auto binary = mgstc::engine::importTones(
        makeMgsEnvelopeOnlyVoice(), "mgs");
    REQUIRE_EQ(binary.valid(), true);
    REQUIRE_EQ(binary.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        binary.candidates.front().type,
        mgstc::engine::ImportedToneType::Composite);
    const auto* binary_timbre = std::get_if<mgstc::engine::CompositeTimbre>(
        &binary.candidates.front().data);
    REQUIRE_EQ(binary_timbre != nullptr, true);
    REQUIRE_EQ(binary_timbre->layers.empty(), false);
    REQUIRE_EQ(
        binary_timbre->layers.front().source, mgstc::engine::TimbreSource::Psg);

    const std::string source = "@e0 = { f }\n";
    const std::vector<std::uint8_t> mml(source.begin(), source.end());
    const auto text = mgstc::engine::importTones(mml, "mus");
    REQUIRE_EQ(text.valid(), true);
    REQUIRE_EQ(text.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        text.candidates.front().type,
        mgstc::engine::ImportedToneType::Composite);
    const auto* text_timbre = std::get_if<mgstc::engine::CompositeTimbre>(
        &text.candidates.front().data);
    REQUIRE_EQ(text_timbre != nullptr, true);
    REQUIRE_EQ(text_timbre->layers.empty(), false);
    REQUIRE_EQ(
        text_timbre->layers.front().source, mgstc::engine::TimbreSource::Psg);

    const std::string rate = "@r0 = { 1, 0, 0, 0, 255, 0, 0, 255 }\n";
    const std::vector<std::uint8_t> rate_bytes(rate.begin(), rate.end());
    const auto rate_result = mgstc::engine::importTones(rate_bytes, "mus");
    REQUIRE_EQ(rate_result.valid(), true);
    REQUIRE_EQ(rate_result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        rate_result.candidates.front().type,
        mgstc::engine::ImportedToneType::Composite);
    const auto* rate_timbre = std::get_if<mgstc::engine::CompositeTimbre>(
        &rate_result.candidates.front().data);
    REQUIRE_EQ(rate_timbre != nullptr, true);
    REQUIRE_EQ(rate_timbre->layers.empty(), false);
    REQUIRE_EQ(
        rate_timbre->layers.front().source, mgstc::engine::TimbreSource::Psg);
    REQUIRE_EQ(
        rate_timbre->layers.front().volume_envelope.kind,
        mgstc::engine::EnvelopeKind::Rate);
}

std::vector<std::uint8_t> makeLooptestMgsBytes() {
    // looptest.mgs: @v15, @e00 = {,,ff[\-1f\1] }, unused @e01, track 9 uses @e0.
    std::vector<std::uint8_t> bytes{
        'M', 'G', 'S', '3', '1', '3', '\r', '\n',
        'N', 'o', ' ', 'T', 'i', 't', 'l', 'e', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 1] = 0x01;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x00);
    bytes.push_back(0x0F);
    bytes.insert(
        bytes.end(),
        {0x21, 0x41, 0x18, 0x07, 0x70, 0xA7, 0x00, 0x11});
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x20);
    const std::vector<std::uint8_t> env00{
        0x0F, 0x0F, 0x40, 0x12, 0xFF, 0x0F, 0x12, 0x01, 0x60};
    bytes.push_back(static_cast<std::uint8_t>(env00.size()));
    bytes.insert(bytes.end(), env00.begin(), env00.end());
    bytes.push_back(0x02);
    bytes.push_back(0x01);
    bytes.push_back(0x20);
    const std::vector<std::uint8_t> env01{
        0x0F, 0x0F, 0x12, 0xFF, 0x40, 0x0F, 0x12, 0x01, 0x60};
    bytes.push_back(static_cast<std::uint8_t>(env01.size()));
    bytes.insert(bytes.end(), env01.begin(), env01.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(
        bytes,
        header,
        9,
        {0xCF, 0x80, 0x49, 0x00, 0xD3, 0x20, 0xC0, 0xFF});
    return bytes;
}

std::vector<std::uint8_t> makeLooptest1MgsBytes() {
    // looptest1.mgs: @e00 = {,,ff \-1 [ f ] \1 fdae }
    std::vector<std::uint8_t> bytes{
        'M', 'G', 'S', '3', '1', '3', '\r', '\n',
        'N', 'o', ' ', 'T', 'i', 't', 'l', 'e', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 1] = 0x01;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x00);
    bytes.push_back(0x0F);
    bytes.insert(
        bytes.end(),
        {0x21, 0x41, 0x18, 0x07, 0x70, 0xA7, 0x00, 0x11});
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x20);
    const std::vector<std::uint8_t> env00{
        0x0F, 0x0F, 0x12, 0xFF, 0x40, 0x0F, 0x60, 0x12, 0x01, 0x0F};
    bytes.push_back(static_cast<std::uint8_t>(env00.size()));
    bytes.insert(bytes.end(), env00.begin(), env00.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(
        bytes,
        header,
        9,
        {0xCF, 0x80, 0x49, 0x00, 0xD3, 0x20, 0xC0, 0xFF});
    return bytes;
}

void testToneImportLooptest1MgsEnvelope() {
    const auto result =
        mgstc::engine::importTones(makeLooptest1MgsBytes(), "mgs");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        *timbre->layers.front().envelope_timeline.loop_start_count,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        *timbre->layers.front().envelope_timeline.loop_end_count,
        static_cast<std::uint32_t>(3));
    bool minus_before_bracket = false;
    bool plus_after_bracket = false;
    for (const auto& event : timbre->layers.front().pitch_envelope.events) {
        if (event.value == -1
            && event.count == 2
            && !event.after_loop_start) {
            minus_before_bracket = true;
        }
        if (event.value == 1
            && event.count == 3
            && !event.after_loop_start) {
            plus_after_bracket = true;
        }
    }
    REQUIRE_EQ(minus_before_bracket, true);
    REQUIRE_EQ(plus_after_bracket, true);
    const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
        timbre->layers.front(), 0);
    REQUIRE_EQ(formatted.valid(), true);
    const auto open = formatted.body.find('[');
    const auto close = formatted.body.find(']');
    const auto minus_one = formatted.body.find("\\-1");
    REQUIRE_EQ(open != std::string::npos, true);
    REQUIRE_EQ(close != std::string::npos, true);
    REQUIRE_EQ(minus_one != std::string::npos, true);
    REQUIRE_EQ(minus_one < open, true);
    REQUIRE_EQ(formatted.body.find("\\1") == std::string::npos, true);
}

void testToneImportLoopEndPitchBeforeBracket() {
    std::vector<std::uint8_t> bytes{
        'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x49);
    const std::vector<std::uint8_t> env{
        0x0F, 0x0F, 0x40, 0x0F, 0x12, 0x01, 0x60};
    bytes.push_back(static_cast<std::uint8_t>(env.size()));
    bytes.insert(bytes.end(), env.begin(), env.end());
    bytes.push_back(0xFF);
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x00, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    bool plus_before_close = false;
    for (const auto& event : timbre->layers.front().pitch_envelope.events) {
        if (event.value == 1
            && event.count == 3
            && event.after_loop_start) {
            plus_before_close = true;
        }
    }
    REQUIRE_EQ(plus_before_close, true);
    const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
        timbre->layers.front(), 0);
    REQUIRE_EQ(formatted.valid(), true);
    const auto open = formatted.body.find('[');
    const auto close = formatted.body.find(']');
    const auto plus_one = formatted.body.find("\\1");
    REQUIRE_EQ(open != std::string::npos, true);
    REQUIRE_EQ(close != std::string::npos, true);
    REQUIRE_EQ(plus_one != std::string::npos, true);
    REQUIRE_EQ(plus_one > open && plus_one < close, true);
}

void testToneImportLooptestMgsEnvelope() {
    const auto result = mgstc::engine::importTones(makeLooptestMgsBytes(), "mgs");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(
        countImportedType(result, mgstc::engine::ImportedToneType::Composite),
        2);
    REQUIRE_EQ(
        countImportedType(result, mgstc::engine::ImportedToneType::Opll),
        1);
    int unused_psg = 0;
    for (const auto& candidate : result.candidates) {
        const auto* composite = std::get_if<mgstc::engine::CompositeTimbre>(
            &candidate.data);
        if (composite == nullptr || composite->layers.empty()) {
            continue;
        }
        if (composite->layers.front().source
            == mgstc::engine::TimbreSource::Psg) {
            ++unused_psg;
        }
    }
    REQUIRE_EQ(unused_psg, 1);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Opll);
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.loop_start_count.has_value(),
        true);
    REQUIRE_EQ(
        *timbre->layers.front().envelope_timeline.loop_start_count,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.loop_end_count.has_value(),
        true);
    REQUIRE_EQ(
        *timbre->layers.front().envelope_timeline.loop_end_count,
        static_cast<std::uint32_t>(3));
    bool found_minus_one = false;
    bool found_plus_one = false;
    for (const auto& event : timbre->layers.front().pitch_envelope.events) {
        if (event.value == -1 && event.after_loop_start) {
            found_minus_one = true;
        }
        if (event.value == 1 && event.after_loop_start) {
            found_plus_one = true;
        }
    }
    REQUIRE_EQ(found_minus_one, true);
    REQUIRE_EQ(found_plus_one, true);
    const auto formatted = mgstc::engine::formatMgsCompositeEnvelope(
        timbre->layers.front(), 0);
    REQUIRE_EQ(formatted.valid(), true);
    const auto open = formatted.body.find('[');
    const auto close = formatted.body.find(']');
    REQUIRE_EQ(open != std::string::npos, true);
    REQUIRE_EQ(close != std::string::npos, true);
    const auto minus_one = formatted.body.find("\\-1");
    const auto plus_one = formatted.body.find("\\1");
    REQUIRE_EQ(minus_one != std::string::npos, true);
    REQUIRE_EQ(plus_one != std::string::npos, true);
    REQUIRE_EQ(minus_one > open && minus_one < close, true);
    REQUIRE_EQ(plus_one > open && plus_one < close, true);
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

void testToneImportDefaultCandidateNames() {
    const auto opll = mgstc::engine::formatMgsOpllDefinition(
        mgstc::engine::decodeOpllPatch(
            std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}),
        5,
        {});
    const std::string titled = "#title { Storm Night }\n" + opll;
    const std::vector<std::uint8_t> titled_bytes(titled.begin(), titled.end());
    const auto titled_result = mgstc::engine::importTones(
        titled_bytes, "mus", "ignored.mus");
    REQUIRE_EQ(titled_result.valid(), true);
    REQUIRE_EQ(titled_result.title, std::string("Storm Night"));
    REQUIRE_EQ(titled_result.candidates.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        titled_result.candidates.front().name, std::string("Storm Night - 05"));

    const std::string commented =
        "; #title { Nope }\n" + opll;
    const std::vector<std::uint8_t> commented_bytes(
        commented.begin(), commented.end());
    const auto file_result = mgstc::engine::importTones(
        commented_bytes, "mus", "tune.mus");
    REQUIRE_EQ(file_result.valid(), true);
    REQUIRE_EQ(file_result.title.empty(), true);
    REQUIRE_EQ(file_result.candidates.front().name, std::string("tune - 05"));

    std::vector<std::uint8_t> mgs{
        'M', 'G', 'S', '3', '\r', '\n', 'S', 't', 'o', 'r', 'm', '\r', '\n',
        0x1A};
    const auto header = mgs.size();
    mgs.resize(header + 0x28, 0);
    mgs[header] = 0x00;
    mgs[header + 4] = 0x28;
    mgs[header + 5] = 0x00;
    mgs.push_back(0x00);
    mgs.push_back(0x03);
    for (int index = 0; index < 8; ++index) {
        mgs.push_back(static_cast<std::uint8_t>(index + 1));
    }
    mgs.push_back(0xFF);
    const auto mgs_result = mgstc::engine::importTones(mgs, "mgs", "song.mgs");
    REQUIRE_EQ(mgs_result.valid(), true);
    REQUIRE_EQ(mgs_result.title, std::string("Storm"));
    REQUIRE_EQ(mgs_result.candidates.front().name, std::string("Storm - 03"));

    std::vector<std::uint8_t> sng(80, 0);
    const char sng_name[] = "SINEWAVE";
    std::copy(sng_name, sng_name + 8, sng.begin());
    for (int index = 0; index < 32; ++index) {
        sng[8 + index] = static_cast<std::uint8_t>(index + 1);
        sng[48 + index] = static_cast<std::uint8_t>(index + 2);
    }
    const auto sng_result = mgstc::engine::importTones(sng, "sng", "lead.sng");
    REQUIRE_EQ(sng_result.valid(), true);
    REQUIRE_EQ(sng_result.candidates.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(sng_result.candidates[0].name, std::string("lead - SINEWAVE"));
    REQUIRE_EQ(sng_result.candidates[1].name, std::string("lead - 01"));

    std::vector<std::uint8_t> vcd(0x10B8, 0);
    const char vcd_name[] = "LEAD";
    std::copy(vcd_name, vcd_name + 4, vcd.begin());
    vcd[0x05A0] = 0x21;
    vcd[0x05A8] = 0x11;
    const auto vcd_result = mgstc::engine::importTones(vcd, "vcd", "bank.vcd");
    REQUIRE_EQ(vcd_result.valid(), true);
    REQUIRE_EQ(vcd_result.candidates.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(vcd_result.candidates[0].name, std::string("bank - LEAD"));
    REQUIRE_EQ(vcd_result.candidates[1].name, std::string("bank - 01"));

    const auto first = opllWrites({1, 2, 3, 4, 5, 6, 7, 8});
    auto second = opllWrites({8, 7, 6, 5, 4, 3, 2, 1});
    std::vector<std::uint8_t> commands = first;
    commands.pop_back();
    commands.insert(commands.end(), second.begin(), second.end());
    const auto vgm_result = mgstc::engine::importTones(
        makeVgm(commands), "vgm", "chip.vgm");
    REQUIRE_EQ(vgm_result.valid(), true);
    REQUIRE_EQ(vgm_result.candidates.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(vgm_result.candidates[0].name, std::string("chip - 00"));
    REQUIRE_EQ(vgm_result.candidates[1].name, std::string("chip - 01"));
}

void testToneImportWidensHalfwidthKanaAndCircledNumbers() {
    std::string source = "#title ";
    source.push_back(static_cast<char>(0xB6));
    source.push_back(static_cast<char>(0x87));
    source.push_back(static_cast<char>(0x40));
    source += "\n@e0 = { f }\n1 @e0 c\n";
    const std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(result.title, std::string("\xE3\x82\xAB" "1"));
    REQUIRE_EQ(result.candidates.empty(), false);
    REQUIRE_EQ(
        result.candidates.front().name.find("\xE3\x82\xAB") != std::string::npos,
        true);
    REQUIRE_EQ(
        result.candidates.front().name.find("1") != std::string::npos, true);
}

void testToneImportMmlSequenceHeaderMixer() {
    const std::string source = "@e0 = { 2,9,f }\n1 @e0 c\n";
    const std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.empty(), false);
    REQUIRE_EQ(
        timbre->layers.front().volume_envelope.rate.tone_mode,
        static_cast<std::uint8_t>(2));
    REQUIRE_EQ(
        timbre->layers.front().volume_envelope.rate.noise,
        static_cast<std::uint8_t>(9));
}

void testToneImportMgsBytecodeLoopAfterStart() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header] = 0x00;
    bytes[header + 4] = 0x28;
    bytes[header + 5] = 0x00;
    bytes.push_back(0x02);
    bytes.push_back(0x00);
    bytes.push_back(0x49);
    const std::vector<std::uint8_t> env{0x0F, 0x40, 0x0A, 0x60};
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
        timbre->layers.front().volume_envelope.rate.tone_mode,
        static_cast<std::uint8_t>(2));
    REQUIRE_EQ(
        timbre->layers.front().volume_envelope.rate.noise,
        static_cast<std::uint8_t>(9));
    REQUIRE_EQ(
        timbre->layers.front().envelope_timeline.loop_start_count.has_value(),
        true);
    REQUIRE_EQ(
        *timbre->layers.front().envelope_timeline.loop_start_count,
        static_cast<std::uint32_t>(1));
    bool found_after = false;
    for (const auto& event : timbre->layers.front().volume_envelope.events) {
        if (event.kind == mgstc::engine::EnvelopeEventKind::Volume
            && event.value == 10
            && event.after_loop_start) {
            found_after = true;
        }
    }
    REQUIRE_EQ(found_after, true);
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

void requireTimedMmlImport(const mgstc::engine::ToneImportResult& result,
    std::size_t expected_count, std::string_view source) {
    const auto expected_errors = expected_count == 0
        ? std::vector<std::string>{"no complete tones were found"} : std::vector<std::string>{};
    if (result.errors != expected_errors || result.format != mgstc::engine::ToneImportFormat::MgsMml) {
        std::cerr << "MML timing input:\n" << source;
        std::cerr << "MML timing format=" << static_cast<int>(result.format)
            << " candidates=" << result.candidates.size() << '\n';
        for (const auto& error : result.errors) std::cerr << "MML timing error: " << error << '\n';
    }
    REQUIRE_EQ(result.format, mgstc::engine::ToneImportFormat::MgsMml);
    REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), expected_count);
    REQUIRE_EQ(result.errors, expected_errors);
    REQUIRE_EQ(result.valid(), expected_count != 0);
    for (const auto& candidate : result.candidates) REQUIRE_EQ(candidate.warnings.empty(), true);
}

void testToneImportMmlTimedRhythmUse() {
    const auto check = [](std::string_view tracks, std::size_t count) {
        const std::string source = "#tempo 120\n@e0 = { f }\n" + std::string(tracks);
        const std::vector<std::uint8_t> bytes(source.begin(), source.end());
        const auto result = mgstc::engine::importTones(bytes, "mus");
        requireTimedMmlImport(result, count, source);
    };
    check("15 c1 @e0\n16 c8c8c8c8c8c8c8c8 y14,32\n", 0);
    check("15 c8 @e0\n16 c1 y14,32\n", 1);
    check("F l8 c. ^16 @e0\nG r4 y14,32\n", 0);
    check("F l8.. c @e0\nG r%42 y14,32\n", 0);
    check("F l8 c^ @e0\nG r4 y14,32\n", 0);
    check("F c4^8 @e0\nG r4. y14,32\n", 0);
    check("F l8 c\nF c @e0\nG r4 y14,32\n", 0);
    check("F c%4 @e0\nG r%5 y14,32\n", 0); // distinct score positions, same interrupt tick
    check("F r%128^%128 @e0\nG r%256 y14,32\n", 0);
    check("FG r4\nF @e0\nG y14,32\n", 0);
    check("F y14,32 c4 y14,0 @e0\n", 1);

    const std::string rate = "@r0 = { 1, 0, 0, 0, 255, 0, 0, 255 }\n"
        "F c1 @r0\nG [c8]8 y14,32\n";
    const std::vector<std::uint8_t> rate_bytes(rate.begin(), rate.end());
    const auto rate_result = mgstc::engine::importTones(rate_bytes, "mus");
    requireTimedMmlImport(rate_result, 0, rate);
}

void testToneImportMmlFiniteRepeatTiming() {
    const auto check = [](std::string_view body, std::string_view peer) {
        const std::string source = "@e0 = { f }\nF " + std::string(body)
            + " @e0\nG " + std::string(peer) + " y14,32\n";
        const std::vector<std::uint8_t> bytes(source.begin(), source.end());
        const auto result = mgstc::engine::importTones(bytes, "mus");
        requireTimedMmlImport(result, 0, source);
    };
    check("[2 c8]", "r4");
    check("[c8]2", "r4");
    check("[c8]", "r4");
    check("[2 [3 c16]]", "r4.");
    check("[3 c8|d8]", "r%120");
    check("[[c16|d16]3]2", "r%120");
    check("[255 c%2]", "r%256 r%254");
    std::string nested;
    for (int depth = 0; depth < 16; ++depth) nested += "[1 ";
    nested += "c4";
    nested += std::string(16, ']');
    check(nested, "r4");

    const std::string continued = "#tempo 120\n@e0 = { f }\nF [2 c8\nF ] @e0\nG r4 y14,32\n";
    const std::vector<std::uint8_t> bytes(continued.begin(), continued.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    requireTimedMmlImport(result, 0, continued);
}

void testToneImportMmlPatchSourceOrder() {
    const auto source = [](std::string_view track) {
        const std::string text = "@e0 = { f }\n" + std::string(track);
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        return mgstc::engine::importTones(bytes, "mus");
    };
    const auto result = source("F @1 @e0 @2 c\n");
    const auto* timbre = firstCompositeTimbre(result);
    REQUIRE_EQ(timbre != nullptr, true);
    REQUIRE_EQ(timbre->layers.front().base_opll_rom.has_value(), true);
    REQUIRE_EQ(*timbre->layers.front().base_opll_rom, 1);
    const auto across_lines = source("F @1 @e0\nF @2 c\n");
    const auto* continued = firstCompositeTimbre(across_lines);
    REQUIRE_EQ(continued != nullptr, true);
    REQUIRE_EQ(*continued->layers.front().base_opll_rom, 1);
    const auto later_use = source("F @1 c4 @2 @e0 c\n");
    REQUIRE_EQ(*firstCompositeTimbre(later_use)->layers.front().base_opll_rom, 2);
}

void testToneImportMmlUnsupportedTimingWarnings() {
    const auto ambiguous = [](std::string_view track) {
        const std::string source = "@e0 = { f }\nF " + std::string(track) + " @e0\n";
        const std::vector<std::uint8_t> bytes(source.begin(), source.end());
        const auto result = mgstc::engine::importTones(bytes, "mus");
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 0);
        REQUIRE_EQ(result.errors.empty(), false);
        REQUIRE_EQ(result.errors.front().find("MML timing analysis") != std::string::npos, true);
    };
    ambiguous("[0 c4]");
    ambiguous("[c4]0");
    ambiguous("*0");
    ambiguous("t240 c4");
    ambiguous("c5");
    ambiguous("c64.");
    ambiguous("c%256^%1");
    ambiguous("[2 c4");
    ambiguous("c4]");
    ambiguous("|c4");
    ambiguous("[255 [255 c4]]");
    std::string nested;
    for (int depth = 0; depth < 17; ++depth) nested += "[1 ";
    nested += "c4";
    nested += std::string(17, ']');
    ambiguous(nested);

    const std::string partial = "@e0 = { f }\n@e1 = { f }\nF *0 @e0\n1 @e1 c\n";
    const std::vector<std::uint8_t> bytes(partial.begin(), partial.end());
    const auto result = mgstc::engine::importTones(bytes, "mus");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 1);
    REQUIRE_EQ(result.candidates.front().warnings.empty(), false);
    REQUIRE_EQ(firstCompositeTimbre(result)->layers.front().source, mgstc::engine::TimbreSource::Psg);
}

void testToneImportMmlIntegerOverflowWarnings() {
    const auto rejected = [](std::string_view header, std::string_view track) {
        const std::string source = std::string(header) + "\n@e0 = { f }\n@e1 = { f }\n"
            + std::string(track) + "\n";
        const std::vector<std::uint8_t> bytes(source.begin(), source.end());
        const auto result = mgstc::engine::importTones(bytes, "mus");
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 0);
        REQUIRE_EQ(result.errors.empty(), false);
        REQUIRE_EQ(result.errors.front().find("MML timing analysis") != std::string::npos, true);
    };
    rejected("#opll_mode 4294967297", "F @e1 c");
    rejected("#opll_mode $100000001", "F @e1 c");
    rejected("#tempo 4294967416", "F @e1 c");
    rejected("#tempo 120garbage", "F @e1 c");
    rejected("#tempo 120 240", "F @e1 c");
    rejected("#opll_mode 0garbage", "F @e1 c");
    rejected("#opll_mode 0 @e1", "F @e1 c");
    rejected("", "4294967297 @e1 c");
    rejected("", "F @e4294967297 c");
    rejected("", "F @e429496729700000000001 @e1 c");
    rejected("", "F @r4294967297 @e1 c");
    rejected("", "F @4294967297 @e1 c");
    rejected("", "F [4294967297 c4] @e1");
    rejected("", "F [c4]4294967297 @e1");
    rejected("", "F c4294967297 @e1");
    rejected("", "F y14,$100000020 @e1");
    rejected("", "F @e4294967295 @e1");
    rejected("", "F @e32 @e1");
    rejected("", "F y14,256 @e1");
    rejected("", "F (42949672960 @e1 c");
    rejected("", "F )42949672960 @e1 c");
    rejected("", "F ( $100000001 @e1 c");
    rejected("", "F )0 @e1 c");
    rejected("", "F (16 @e1 c");
    rejected("", "F @e" + std::string(4096, '9') + " @e1 c");

    const auto accepted = [](std::string_view header, std::string_view track) {
        const std::string source = std::string(header) + "\n@e0 = { f }\n"
            + std::string(track) + "\n";
        const std::vector<std::uint8_t> bytes(source.begin(), source.end());
        const auto result = mgstc::engine::importTones(bytes, "mus");
        REQUIRE_EQ(result.valid(), true);
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 1);
        REQUIRE_EQ(result.candidates.front().warnings.empty(), true);
    };
    accepted("#tempo 120\n#opll_mode 0", "F (15 )1 @e0 c");
    accepted("#tempo \t120 \t\n#opll_mode \t0 \t", "F ( 15 ) 1 @e0 c");
    accepted("#tempo 120; ignored\n#opll_mode 0; ignored", "F ( @e0 c");
}

void testToneImportMgsVoiceWarningsPreserveCompleteCandidates() {
    const auto make = [](const std::vector<std::uint8_t>& tail) {
        std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
        const auto header = bytes.size();
        bytes.resize(header + 0x28, 0);
        bytes[header + 4] = 0x28;
        bytes.insert(bytes.end(), {0x00, 0x00, 1, 2, 3, 4, 5, 6, 7, 8});
        bytes.insert(bytes.end(), {0x03, 0x00});
        for (int value = 0; value < 32; ++value) bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.insert(bytes.end(), {0x02, 0x01, 0x00, 0x01, 0x0F});
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        appendMgsMusicTrack(bytes, header, 4, {0x80, 0x49, 0x01, 0xFF});
        return bytes;
    };
    const auto has_voice_warning = [](const auto& candidate) {
        return std::any_of(candidate.warnings.begin(), candidate.warnings.end(), [](const auto& warning) {
            return warning.message.find("MGS voice definitions:") != std::string::npos;
        });
    };
    const std::vector<std::vector<std::uint8_t>> stopped{
        {}, // No FF before the next independently addressed music track.
        {0x00, 0x02}, {0x01, 0x02}, {0x02, 0x02},
        {0x02, 0x02, 0x80, 1, 2}, {0x02, 0x02, 0x00},
        {0x02, 0x02, 0x00, 5, 0x0F}, {0x03, 0x02, 0},
        {0x04, 0}, {0x05, 0}, {0x06, 0}, {0x06, 0, 'a', 'b'},
        // The apparent OPLL definition after the unknown command is not a
        // recovery target and must never become a second candidate.
        {0x7F, 0x00, 0x02, 1, 2, 3, 4, 5, 6, 7, 8, 0xFF},
    };
    for (const auto& tail : stopped) {
        const auto result = mgstc::engine::importTones(make(tail), "mgs");
        REQUIRE_EQ(result.valid(), true);
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Opll), 1);
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Scc), 1);
        REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 1);
        REQUIRE_EQ(std::all_of(result.candidates.begin(), result.candidates.end(), has_voice_warning), true);
        const auto* timbre = firstCompositeTimbre(result);
        REQUIRE_EQ(timbre != nullptr, true);
        REQUIRE_EQ(timbre->layers.front().source, mgstc::engine::TimbreSource::Scc);
    }
    std::vector<std::uint8_t> complete{0x01, 0x00, 0x0F, 0x04};
    complete.insert(complete.end(), 24, 0);
    complete.push_back(0x05);
    complete.insert(complete.end(), 24, 0);
    complete.insert(complete.end(), {0x06, 0x00, 'a', 0x00, 0xFF});
    const auto normal = mgstc::engine::importTones(make(complete), "mgs");
    REQUIRE_EQ(normal.valid(), true);
    REQUIRE_EQ(std::any_of(normal.candidates.begin(), normal.candidates.end(), has_voice_warning), false);
}

void testToneImportMgsBrokenEnvelopeSkipsOnlyItsDefinition() {
    std::vector<std::uint8_t> bytes{'M', 'G', 'S', '3', '\r', '\n', 0x1A};
    const auto header = bytes.size();
    bytes.resize(header + 0x28, 0);
    bytes[header + 4] = 0x28;
    // A fully delimited but internally truncated @e0 precedes a complete
    // @e1. Its length gives the exact next definition without guessing.
    bytes.insert(bytes.end(), {0x02, 0x00, 0x00, 0x01, 0x10});
    bytes.insert(bytes.end(), {0x02, 0x01, 0x00, 0x01, 0x0F, 0xFF});
    appendMgsMusicTrack(bytes, header, 1, {0x49, 0x01, 0xFF});
    const auto result = mgstc::engine::importTones(bytes, "mgs");
    REQUIRE_EQ(result.valid(), true);
    REQUIRE_EQ(countImportedType(result, mgstc::engine::ImportedToneType::Composite), 1);
    REQUIRE_EQ(result.candidates.front().name, std::string("01"));
    REQUIRE_EQ(std::any_of(result.candidates.front().warnings.begin(), result.candidates.front().warnings.end(),
        [](const auto& warning) { return warning.message.find("broken envelope definition 0 was skipped") != std::string::npos; }), true);

    auto only_broken = bytes;
    only_broken.resize(header + 0x28 + 5);
    only_broken[header + 6] = 0;
    only_broken[header + 7] = 0;
    only_broken.push_back(0xFF);
    const auto rejected = mgstc::engine::importTones(only_broken, "mgs");
    REQUIRE_EQ(rejected.candidates.empty(), true);
    REQUIRE_EQ(rejected.errors.empty(), false);
    REQUIRE_EQ(rejected.errors.front().find("broken envelope definition 0 was skipped") != std::string::npos, true);
}

void testToneImportSccSourceRejectsOversizedWaveDefinitions() {
    const auto definition = [](std::size_t count, bool spaced) {
        std::string source = "@s0={";
        for (std::size_t index = 0; index < count; ++index) source += spaced ? "00 " : "00";
        return source + '}';
    };
    for (const bool spaced : {false, true}) {
        REQUIRE_EQ(mgstc::engine::parseMgsSccDefinition(definition(31, spaced)).has_value(), false);
        REQUIRE_EQ(mgstc::engine::parseMgsSccDefinition(definition(32, spaced)).has_value(), true);
        REQUIRE_EQ(mgstc::engine::parseMgsSccDefinition(definition(33, spaced)).has_value(), false);
        REQUIRE_EQ(mgstc::engine::parseMgsSccDefinition(definition(65'536, spaced)).has_value(), false);
    }
}
