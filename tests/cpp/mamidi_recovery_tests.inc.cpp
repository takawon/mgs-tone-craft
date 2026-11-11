struct RecoveryRpcServer {
    std::atomic<bool> blocked{false}, entered{false}, fail{false};
    std::mutex mutex;
    std::vector<MAmidiChipAccess> received;
    rpc::server server;
    explicit RecoveryRpcServer(std::uint16_t port) : server(port) {
        server.suppress_exceptions(true);
        server.bind(MAmidiRpcClient::kMethodName,
            [&](unsigned char device, unsigned char unit, unsigned int address, unsigned int data) {
                if (device == 0) return;
                entered.store(true, std::memory_order_release);
                while (blocked.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (fail.load(std::memory_order_acquire)) throw std::runtime_error("injected transport failure");
                std::lock_guard lock(mutex);
                received.push_back({device, unit, address, data});
            });
        server.async_run(1);
    }
    struct Release {
        RecoveryRpcServer& owner;
        ~Release() { owner.blocked.store(false, std::memory_order_release); }
    };
    void waitEntered() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE_EQ(entered.load(std::memory_order_acquire), true);
    }
    void fill(MAmidiMemoSoundOutput& output) {
        blocked.store(true, std::memory_order_release);
        entered.store(false, std::memory_order_release);
        REQUIRE_EQ(output.enqueueChipAccess({11, 0, 8, 15}), true);
        waitEntered();
        bool full = false;
        for (int index = 0; index < 20000; ++index)
            if (!output.enqueueChipAccess({11, 0, 8, 15})) { full = true; break; }
        REQUIRE_EQ(full, true);
    }
    std::vector<MAmidiChipAccess> copy() {
        std::lock_guard lock(mutex);
        return received;
    }
};

void beginFreshBackendTick(MAmidiMemoSoundOutput& remote, std::uint32_t tracks) {
    const auto token = remote.captureNoteToken();
    for (std::uint8_t track = 0; track < 17; ++track)
        if ((tracks & (1U << track)) != 0) remote.noteQueued(track, token);
    remote.beginTick(tracks);
}

void testMAmidiCongestionContinuesMirrorAndRecoversFreshNotes() {
    for (const bool monitor : {false, true}) {
        const std::uint16_t port = monitor ? 39122 : 39121;
        RecoveryRpcServer mock(port);
        MAmidiMemoSoundOutput remote;
        RecoveryRpcServer::Release release{mock};
        remote.setEndpoint("127.0.0.1", port);
        REQUIRE_EQ(remote.open(), true);
        auto engine = std::make_unique<EngineCore>();
        auto reference = std::make_unique<EngineCore>();
        engine->setOutputBackend(&remote);
        engine->setWaveformMonitor(monitor);
        for (auto* core : {engine.get(), reference.get()}) {
            for (const std::uint8_t track : {0, 3, 8}) {
                REQUIRE_EQ(core->session().setSequenceEnvelope(track, {0x0F}), true);
                REQUIRE_EQ(core->queueNoteOn(track, 60), true);
            }
        }
        std::array<float, 1600> pcm{}, reference_pcm{};
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(reference->render(reference_pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        mock.fill(remote);
        REQUIRE_EQ(remote.statusText().find("silencing") != std::string::npos, true);
        const auto tick_before = engine->session().tick();
        // A lost key-off must not halt the mirror or turn PC output on.
        REQUIRE_EQ(engine->session().queueKeyOff(8), true);
        REQUIRE_EQ(reference->session().queueKeyOff(8), true);
        for (int tick = 0; tick < 8; ++tick) {
            REQUIRE_EQ(engine->render(pcm).ok(), true);
            REQUIRE_EQ(reference->render(reference_pcm).ok(), true);
            REQUIRE_EQ(std::all_of(pcm.begin(), pcm.end(), [](float s) { return s == 0; }), true);
        }
        REQUIRE_EQ(engine->session().tick() > tick_before, true);
        REQUIRE_EQ(engine->session().tick(), reference->session().tick());
        auto scope = std::make_unique<OpllScopeFrame>();
        REQUIRE_EQ(engine->takeOpllScopeFrame(*scope), true);
        const bool visible = std::any_of(scope->mixed_samples.begin(), scope->mixed_samples.end(),
            [](float value) { return std::abs(value) > 0.001F; });
        REQUIRE_EQ(visible, monitor);
        engine->setOfflineRendering(true);
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(reference->render(reference_pcm).ok(), true);
        REQUIRE_EQ(pcm, reference_pcm);
        engine->setOfflineRendering(false);
        // A NoteOn processed before silence is acknowledged cannot wake later.
        REQUIRE_EQ(engine->session().setSequenceEnvelope(2, {0x0F}), true);
        REQUIRE_EQ(engine->queueNoteOn(2, 64), true);
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        // This one is accepted before the acknowledgement but processed after
        // it. Admission must use its original input token, not render time.
        REQUIRE_EQ(engine->session().setSequenceEnvelope(5, {0x0F}), true);
        REQUIRE_EQ(engine->queueNoteOn(5, 64), true);
        mock.blocked.store(false, std::memory_order_release);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        REQUIRE_EQ(remote.statusText().find("waiting for next Note On") != std::string::npos, true);
        const auto silenced = mock.copy();
        REQUIRE_EQ(silenced.back().address, 0x8FU);
        REQUIRE_EQ(silenced.back().data, 0U);
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(mock.copy().size(), silenced.size());
        // A tail write after congestion still belongs to the desired image.
        const std::array<RegisterWrite, 2> desired{{
            {.chip = ChipId::Opll, .address = 0, .value = 0x71, .track = 8, .reason = WriteReason::Patch},
            {.chip = ChipId::Opll, .address = 1, .value = 0x62, .track = 8, .reason = WriteReason::Patch}}};
        REQUIRE_EQ(remote.writeRegisters(desired), false);
        for (const std::uint8_t track : {1, 4, 9}) {
            REQUIRE_EQ(engine->session().setSequenceEnvelope(track, {0x0F}), true);
            REQUIRE_EQ(engine->queueNoteOn(track, 67), true);
        }
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(remote.statusText().find("queue is full") == std::string::npos, true);
        const auto resumed = mock.copy();
        REQUIRE_EQ(resumed.size() > silenced.size(), true);
        bool restored_tail = false, psg_new = false, scc_new = false, opll_new = false;
        for (auto it = resumed.begin() + static_cast<std::ptrdiff_t>(silenced.size()); it != resumed.end(); ++it) {
            if (it->device_id == 11 && (it->address == 8 || it->address == 10)) REQUIRE_EQ(it->data, 0U);
            if (it->device_id == 11 && it->address == 9 && it->data != 0) psg_new = true;
            if (it->device_id == 7 && it->address == 0x8F) {
                REQUIRE_EQ((it->data & ~2U), 0U);
                scc_new |= it->data == 2;
            }
            if (it->device_id == 9 && it->address == 0x20) REQUIRE_EQ(it->data & 0x10U, 0U);
            if (it->device_id == 9 && it->address == 0x21 && (it->data & 0x10U)) opll_new = true;
            if (it->device_id == 9 && it->address == 1 && it->data == 0x62) restored_tail = true;
        }
        REQUIRE_EQ(restored_tail && psg_new && scc_new && opll_new, true);
        // Old channel writes remain muted after the first recovered note.
        REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Psg, .address = 8, .value = 15}), true);
        REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Scc, .port = 3, .value = 0x1F}), true);
        REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Opll, .address = 0x20, .value = 0x10}), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        const auto masked = mock.copy();
        REQUIRE_EQ(masked.size(), resumed.size());
        // Leaving offline mode after only some voices have recovered must
        // create a new physical barrier, not strand the remaining tokens.
        remote.setEmissionEnabled(false);
        remote.setEmissionEnabled(true);
        REQUIRE_EQ(remote.captureNoteToken(), 0U);
        remote.beginTick(0);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        REQUIRE_EQ(engine->session().setSequenceEnvelope(6, {0x0F}), true);
        REQUIRE_EQ(engine->queueNoteOn(6, 69), true);
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(remote.statusText().find("queue is full") == std::string::npos, true);
        // Congestion during the first recovered tick invalidates even the
        // partially sent initialization. A further fresh note is required.
        mock.fill(remote);
        mock.blocked.store(false, std::memory_order_release);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        mock.blocked.store(true, std::memory_order_release);
        mock.entered.store(false, std::memory_order_release);
        beginFreshBackendTick(remote, 1U << 1);
        std::vector<RegisterWrite> large_tick(8192,
            {.chip = ChipId::Psg, .address = 9, .value = 15, .track = 1});
        REQUIRE_EQ(remote.writeRegisters(large_tick), false);
        mock.waitEntered();
        mock.blocked.store(false, std::memory_order_release);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        REQUIRE_EQ(remote.statusText().find("waiting for next Note On") != std::string::npos, true);
        const auto retry_silenced = mock.copy();
        REQUIRE_EQ(retry_silenced.back().address, 0x8FU);
        REQUIRE_EQ(retry_silenced.back().data, 0U);
        beginFreshBackendTick(remote, 1U << 1);
        REQUIRE_EQ(remote.writeRegisters({}), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        // Offline entry must not mistake a skipped physical silence for an ack.
        mock.fill(remote);
        remote.setEmissionEnabled(false);
        mock.blocked.store(false, std::memory_order_release);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        remote.setEmissionEnabled(true);
        remote.beginTick(0);
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        REQUIRE_EQ(remote.statusText().find("waiting for next Note On") != std::string::npos, true);
        remote.reset();
        REQUIRE_EQ(remote.waitForIdle(3000), true);
        remote.close();
        REQUIRE_EQ(remote.open(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(remote.statusText().find("waiting for next Note On") != std::string::npos, true);
        const auto reopened = mock.copy().size();
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(mock.copy().size(), reopened);
        REQUIRE_EQ(engine->queueNoteOn(1, 70), true);
        REQUIRE_EQ(engine->render(pcm).ok(), true);
        REQUIRE_EQ(remote.waitForIdle(2000), true);
        REQUIRE_EQ(remote.statusText().find("queue is full") == std::string::npos, true);
        REQUIRE_EQ(engine->waveformMonitor(), monitor);
        REQUIRE_EQ(engine->outputKind(), mgstc::engine::SoundOutputKind::MAmidiMemo);
        remote.close();
        mock.server.stop();
    }
}

void testMAmidiCongestionFailureClassification() {
    RecoveryRpcServer mock(39123);
    MAmidiMemoSoundOutput remote;
    RecoveryRpcServer::Release release{mock};
    REQUIRE_EQ(remote.writeRegister({}), false);
    REQUIRE_EQ(remote.lastError().find("not open") != std::string::npos, true);
    remote.setEndpoint("127.0.0.1", 39123);
    REQUIRE_EQ(remote.open(), true);
    REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Psg, .address = 14}), false);
    REQUIRE_EQ(remote.recoverableWriteFailure(), false);
    REQUIRE_EQ(remote.statusText().find("Invalid") != std::string::npos, true);
    remote.reset();
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    mock.fill(remote);
    mock.fail.store(true, std::memory_order_release);
    mock.blocked.store(false, std::memory_order_release);
    REQUIRE_EQ(remote.waitForIdle(3000), false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (remote.isOpen() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_EQ(remote.connectionState(), MAmidiConnectionState::Faulted);
    REQUIRE_EQ(remote.recoverableWriteFailure(), false);
    REQUIRE_EQ(remote.statusText().find("resynchronization/silence failed") != std::string::npos, true);
    remote.close();
    REQUIRE_EQ(remote.closeAcknowledged(), false);
    mock.fail.store(false, std::memory_order_release);
    REQUIRE_EQ(remote.open(), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    beginFreshBackendTick(remote, (1U << 17) - 1U);
    REQUIRE_EQ(remote.writeRegisters({}), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    mock.fail.store(true, std::memory_order_release);
    REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Psg, .address = 8, .value = 15}), true);
    REQUIRE_EQ(remote.waitForIdle(3000), false);
    REQUIRE_EQ(remote.connectionState(), MAmidiConnectionState::Faulted);
    REQUIRE_EQ(remote.statusText().find("resynchronization/silence failed") == std::string::npos, true);
    REQUIRE_EQ(remote.statusText().find("faulted") != std::string::npos, true);
    remote.close();
    mock.server.stop();
}

void testMAmidiRecoveryRestoresLevelsWithoutRetriggering() {
    RecoveryRpcServer mock(39126);
    MAmidiMemoSoundOutput remote;
    RecoveryRpcServer::Release release{mock};
    remote.setEndpoint("127.0.0.1", 39126);
    REQUIRE_EQ(remote.open(), true);
    const std::array<RegisterWrite, 5> desired{{
        {.chip = ChipId::Psg, .address = 7, .value = 0x38},
        {.chip = ChipId::Psg, .address = 8, .value = 12},
        {.chip = ChipId::Psg, .address = 9, .value = 15},
        {.chip = ChipId::Opll, .address = 0x30, .value = 0x24, .track = 8},
        {.chip = ChipId::Opll, .address = 0x31, .value = 0x13, .track = 9}}};
    REQUIRE_EQ(remote.writeRegisters(desired), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    mock.fill(remote);
    mock.blocked.store(false, std::memory_order_release);
    REQUIRE_EQ(remote.waitForIdle(3000), true);
    const auto before = mock.copy().size();
    beginFreshBackendTick(remote, (1U << 0) | (1U << 8));
    const std::array<RegisterWrite, 2> keys{{
        {.chip = ChipId::Psg, .address = 0, .value = 0x44, .reason = WriteReason::Frequency},
        {.chip = ChipId::Opll, .address = 0x20, .value = 0x16, .track = 8, .reason = WriteReason::KeyOn}}};
    REQUIRE_EQ(remote.writeRegisters(keys), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    const auto values = mock.copy();
    REQUIRE_EQ(values.size() >= before + 3, true);
    REQUIRE_EQ(values[values.size() - 3].address, 7U);
    REQUIRE_EQ(values[values.size() - 3].data, 0x3EU);
    REQUIRE_EQ(values[values.size() - 2].address, 8U);
    REQUIRE_EQ(values[values.size() - 2].data, 12U);
    REQUIRE_EQ(values.back().address, 0x30U);
    REQUIRE_EQ(values.back().data, 0x24U);
    for (auto it = values.begin() + static_cast<std::ptrdiff_t>(before); it != values.end(); ++it) {
        if (it->device_id == 11 && it->address == 9) REQUIRE_EQ(it->data, 0U);
        if (it->device_id == 9 && it->address == 0x31) REQUIRE_EQ(it->data & 15U, 15U);
    }
    remote.close();
    mock.server.stop();
}

void testMAmidiHostCongestionMaintainsSelection() {
    using namespace mgstc::engine;
    RecoveryRpcServer mock(39124);
    auto host = std::make_unique<RealtimeEngineHost>();
    RecoveryRpcServer::Release release{mock};
    host->setMAmidiSettings({.host = "127.0.0.1", .port = 39124, .waveform_monitor = true});
    REQUIRE_EQ(host->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    auto edit = host->beginProgramEdit();
    REQUIRE_EQ(edit.valid(), true);
    auto* remote = dynamic_cast<MAmidiMemoSoundOutput*>(edit.engine->outputBackend());
    REQUIRE_EQ(remote != nullptr, true);
    REQUIRE_EQ(edit.engine->session().setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(host->submitProgram(edit), true);
    host->servicePendingControlCommands();
    REQUIRE_EQ(host->realtimeNoteOn(0, 60), true);
    std::array<float, 1600> pcm{};
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(remote->waitForIdle(2000), true);
    mock.fill(*remote);
    REQUIRE_EQ(host->realtimeNoteOff(0), true);
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(host->realtimeNoteOn(0, 62), true);
    REQUIRE_EQ(host->submit(EngineCommand::noteOn(1, 64)), true);
    REQUIRE_EQ(host->soundOutputStatus().find("queue is full") != std::string::npos, true);
    REQUIRE_EQ(host->soundOutputKind(), SoundOutputKind::MAmidiMemo);
    REQUIRE_EQ(host->ownsHardwareOutput(), true);
    REQUIRE_EQ(host->mamidiSettings().waveform_monitor, true);
    REQUIRE_EQ(std::all_of(pcm.begin(), pcm.end(), [](float value) { return value == 0; }), true);
    mock.blocked.store(false, std::memory_order_release);
    REQUIRE_EQ(remote->waitForIdle(3000), true);
    const auto before_late_processing = mock.copy().size();
    host->servicePendingControlCommands();
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(remote->waitForIdle(2000), true);
    REQUIRE_EQ(mock.copy().size(), before_late_processing);
    REQUIRE_EQ(host->soundOutputStatus().find("waiting for next Note On") != std::string::npos, true);
    // Control-side acceptance after the barrier is allowed to resume.
    REQUIRE_EQ(host->submit(EngineCommand::noteOn(0, 65)), true);
    host->servicePendingControlCommands();
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(remote->waitForIdle(2000), true);
    REQUIRE_EQ(host->soundOutputStatus().find("queue is full") == std::string::npos, true);
    mock.fill(*remote);
    auto next = host->beginProgramEdit();
    REQUIRE_EQ(next.valid(), true);
    REQUIRE_EQ(next.engine->session().setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(host->submitProgram(next, {.retrigger = true, .track = 0, .midi_note = 67}), true);
    mock.blocked.store(false, std::memory_order_release);
    REQUIRE_EQ(remote->waitForIdle(3000), true);
    host->servicePendingControlCommands();
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(remote->waitForIdle(3000), true);
    REQUIRE_EQ(host->soundOutputStatus().find("waiting for next Note On") != std::string::npos, true);
    // A new realtime input after the new-program silence barrier is ready.
    REQUIRE_EQ(host->realtimeNoteOn(0, 69), true);
    REQUIRE_EQ(host->renderAudio(pcm).ok(), true);
    REQUIRE_EQ(remote->waitForIdle(2000), true);
    REQUIRE_EQ(host->soundOutputStatus().find("queue is full") == std::string::npos, true);
    REQUIRE_EQ(host->setSoundOutputKind(SoundOutputKind::Emulator), true);
    mock.server.stop();
}

void testMAmidiRecoveryPreservesFreshManualWrites() {
    RecoveryRpcServer mock(39127);
    MAmidiMemoSoundOutput remote;
    RecoveryRpcServer::Release release{mock};
    remote.setEndpoint("127.0.0.1", 39127);
    REQUIRE_EQ(remote.open(), true);
    const std::array<RegisterWrite, 7> desired{{
        {.chip = ChipId::Psg, .address = 7, .value = 0x38},
        {.chip = ChipId::Psg, .address = 8, .value = 12},
        {.chip = ChipId::Psg, .address = 9, .value = 11},
        {.chip = ChipId::Psg, .address = 10, .value = 10},
        {.chip = ChipId::Opll, .address = 0x36, .value = 0x25},
        {.chip = ChipId::Opll, .address = 0x37, .value = 0x43},
        {.chip = ChipId::Opll, .address = 0x38, .value = 0x67}}};
    REQUIRE_EQ(remote.writeRegisters(desired), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    mock.fill(remote);
    mock.blocked.store(false, std::memory_order_release);
    REQUIRE_EQ(remote.waitForIdle(3000), true);
    beginFreshBackendTick(remote, (1U << 0) | (1U << 3) | (1U << 8));
    REQUIRE_EQ(remote.writeRegisters({}), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    const std::array<RegisterWrite, 1> cross_key{{
        {.chip = ChipId::Opll, .address = 0x26, .value = 0x10, .track = 8, .reason = WriteReason::EnvelopeY}}};
    REQUIRE_EQ(remote.writeRegisters(cross_key), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    REQUIRE_EQ(mock.copy().back().address, 0x36U);
    REQUIRE_EQ(mock.copy().back().data, 0x25U);
    const auto before_y = mock.copy().size();
    const std::array<RegisterWrite, 4> manual{{
        {.chip = ChipId::Opll, .address = 0x26, .value = 0x10, .track = 8, .reason = WriteReason::EnvelopeY},
        {.chip = ChipId::Opll, .address = 0x0E, .value = 0x3F, .track = 8, .reason = WriteReason::RhythmControl},
        {.chip = ChipId::Psg, .address = 7, .value = 0x38, .track = 0, .reason = WriteReason::EnvelopeY},
        {.chip = ChipId::Psg, .address = 9, .value = 7, .track = 0, .reason = WriteReason::EnvelopeY}}};
    REQUIRE_EQ(remote.writeRegisters(manual), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    const auto y_values = mock.copy();
    REQUIRE_EQ(y_values[before_y].address, 0x26U);
    REQUIRE_EQ(y_values[before_y].data, 0x10U);
    REQUIRE_EQ(y_values[before_y + 1].address, 0x0EU);
    REQUIRE_EQ(y_values[before_y + 1].data, 0x3FU);
    REQUIRE_EQ(y_values[before_y + 2].address, 7U);
    REQUIRE_EQ(y_values[before_y + 2].data, 0x38U);
    bool psg_level = false, psg_other_level = false, level6 = false, level7 = false, level8 = false;
    for (auto it = y_values.begin() + static_cast<std::ptrdiff_t>(before_y); it != y_values.end(); ++it) {
        psg_level |= it->device_id == 11 && it->address == 9 && it->data == 7;
        psg_other_level |= it->device_id == 11 && it->address == 10 && it->data == 10;
        level6 |= it->device_id == 9 && it->address == 0x36 && it->data == 0x25;
        level7 |= it->device_id == 9 && it->address == 0x37 && it->data == 0x43;
        level8 |= it->device_id == 9 && it->address == 0x38 && it->data == 0x67;
    }
    REQUIRE_EQ(psg_level && psg_other_level && level6 && level7 && level8, true);
    const std::array<RegisterWrite, 5> retired{{
        {.chip = ChipId::Opll, .address = 0x26, .value = 0, .track = 14, .reason = WriteReason::Frequency},
        {.chip = ChipId::Opll, .address = 0x36, .value = 15, .track = 14, .reason = WriteReason::Volume},
        {.chip = ChipId::Psg, .address = 7, .value = 0x3F, .track = 1, .reason = WriteReason::Patch},
        {.chip = ChipId::Psg, .address = 9, .value = 0, .track = 1, .reason = WriteReason::Volume},
        {.chip = ChipId::Scc, .port = 3, .value = 0, .track = 4, .reason = WriteReason::KeyOff}}};
    REQUIRE_EQ(remote.writeRegisters(retired), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    REQUIRE_EQ(mock.copy().size(), y_values.size());
    REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Psg, .address = 7, .value = 0x3F,
        .track = 0, .reason = WriteReason::Patch}), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    REQUIRE_EQ(mock.copy().back().data, 0x39U);
    const std::array<RegisterWrite, 1> other_volume{{
        {.chip = ChipId::Psg, .address = 9, .value = 8, .track = 0, .reason = WriteReason::EnvelopeY}}};
    REQUIRE_EQ(remote.writeRegisters(other_volume), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    const auto after_volume = mock.copy();
    REQUIRE_EQ(after_volume[after_volume.size() - 2].address, 7U);
    REQUIRE_EQ(after_volume[after_volume.size() - 2].data, 0x39U);
    REQUIRE_EQ(after_volume.back().address, 9U);
    REQUIRE_EQ(after_volume.back().data, 8U);
    beginFreshBackendTick(remote, 1U << 4);
    REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Scc, .port = 3, .value = 3,
        .track = 4, .reason = WriteReason::KeyOn}), true);
    REQUIRE_EQ(remote.writeRegister({.chip = ChipId::Scc, .port = 3, .value = 0x10,
        .track = 3, .reason = WriteReason::KeyOff}), true);
    REQUIRE_EQ(remote.waitForIdle(2000), true);
    REQUIRE_EQ(mock.copy().back().data, 2U);
    remote.close();
    mock.server.stop();
}
