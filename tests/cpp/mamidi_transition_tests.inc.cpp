// SPDX-License-Identifier: AGPL-3.0-only

void testMAmidiTransitionTimeoutPreservesOwnerAndRetries() {
    using namespace mgstc::engine;
    using test::HeldHostAccess;
    RecoveryRpcServer mock(39131);
    auto a = std::make_unique<RealtimeEngineHost>();
    auto b = std::make_unique<RealtimeEngineHost>();
    RecoveryRpcServer::Release release{mock};
    auto settings = MAmidiOutputSettings{.host = "127.0.0.1", .port = 39131, .unit_no = 1};
    REQUIRE_EQ(a->setMAmidiSettings(settings), true);
    settings.unit_no = 2;
    REQUIRE_EQ(b->setMAmidiSettings(settings), true);
    REQUIRE_EQ(a->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    SoundOutputSnapshot snapshot;
    REQUIRE_EQ(a->trySoundOutputSnapshot(snapshot), OutputSnapshotResult::Ready);
    REQUIRE_EQ(snapshot.kind, SoundOutputKind::MAmidiMemo);
    REQUIRE_EQ(snapshot.desired_kind, SoundOutputKind::MAmidiMemo);
    REQUIRE_EQ(snapshot.owned && snapshot.connected, true);
    REQUIRE_EQ(snapshot.settings.host, settings.host);
    REQUIRE_EQ(snapshot.settings.unit_no, std::uint8_t{1});
    REQUIRE_EQ(snapshot.status, a->soundOutputStatus());
    REQUIRE_EQ(snapshot.error, a->soundOutputLastError());
    for (const auto kind : {HeldHostAccess::Kind::Control, HeldHostAccess::Kind::BackendError}) {
        HeldHostAccess held(*a, kind);
        const auto state_before = RealtimeEngineHostTestAccess::accessState(*a);
        snapshot.status = "previous confirmed status";
        const auto started = std::chrono::steady_clock::now();
        for (int attempt = 0; attempt < 32; ++attempt)
            REQUIRE_EQ(a->trySoundOutputSnapshot(snapshot), OutputSnapshotResult::Busy);
        REQUIRE_EQ(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(250), true);
        REQUIRE_EQ(snapshot.status, std::string("previous confirmed status"));
        REQUIRE_EQ(snapshot.settings.unit_no, std::uint8_t{1});
        REQUIRE_EQ(RealtimeEngineHostTestAccess::accessState(*a), state_before);
    }
    const auto revision_a = a->outputRoutingRevision();
    const auto revision_b = b->outputRoutingRevision();
    const auto original_settings = a->mamidiSettings();
    {
        HeldHostAccess audio(*a, HeldHostAccess::Kind::Audio);
        const auto start = std::chrono::steady_clock::now();
        REQUIRE_EQ(b->setSoundOutputKind(SoundOutputKind::MAmidiMemo), false);
        REQUIRE_EQ(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), true);
        REQUIRE_EQ(a->ownsHardwareOutput(), true);
        REQUIRE_EQ(a->hardwareOutputConnected(), true);
        REQUIRE_EQ(a->soundOutputKind(), SoundOutputKind::MAmidiMemo);
        REQUIRE_EQ(a->desiredSoundOutputKind(), SoundOutputKind::MAmidiMemo);
        REQUIRE_EQ(b->ownsHardwareOutput(), false);
        REQUIRE_EQ(b->soundOutputKind(), SoundOutputKind::Emulator);
        REQUIRE_EQ(b->desiredSoundOutputKind(), SoundOutputKind::Emulator);
        REQUIRE_EQ(a->outputRoutingRevision(), revision_a);
        REQUIRE_EQ(b->outputRoutingRevision(), revision_b);
        REQUIRE_EQ(a->outputSwitchPending(), false);
        REQUIRE_EQ(b->outputSwitchPending(), false);
        REQUIRE_EQ(RealtimeEngineHostTestAccess::accessState(*a), 1U);
        REQUIRE_EQ(a->reconnectMAmidi(), false);
        REQUIRE_EQ(a->hardwareOutputConnected(), true);
        auto changed = original_settings;
        changed.host = "127.0.0.2";
        changed.waveform_monitor = !changed.waveform_monitor;
        REQUIRE_EQ(a->setMAmidiSettings(changed), false);
        REQUIRE_EQ(a->mamidiSettings().host, original_settings.host);
        REQUIRE_EQ(a->mamidiSettings().waveform_monitor, original_settings.waveform_monitor);
    }
    REQUIRE_EQ(a->reconnectMAmidi(), true);
    REQUIRE_EQ(b->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    REQUIRE_EQ(a->ownsHardwareOutput(), false);
    REQUIRE_EQ(a->soundOutputKind(), SoundOutputKind::Emulator);
    REQUIRE_EQ(b->ownsHardwareOutput(), true);
    // Normal destruction must relinquish its registered owner before another
    // live Host acquires the same device. No stuck-worker safety is inferred.
    b.reset();
    REQUIRE_EQ(a->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    REQUIRE_EQ(a->setSoundOutputKind(SoundOutputKind::Emulator), true);
    mock.server.stop();
}

void testMAmidiSharedLeaseTimeoutPreservesOtherHost() {
    using namespace mgstc::engine;
    RecoveryRpcServer mock(39132);
    auto a = std::make_unique<RealtimeEngineHost>();
    auto b = std::make_unique<RealtimeEngineHost>();
    std::atomic<bool> switched{false};
    std::jthread switching;
    RecoveryRpcServer::Release release{mock};
    auto settings = MAmidiOutputSettings{.host = "127.0.0.1", .port = 39132};
    REQUIRE_EQ(a->setMAmidiSettings(settings), true);
    REQUIRE_EQ(a->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    auto edit = a->beginProgramEdit();
    REQUIRE_EQ(edit.valid(), true);
    auto* remote = dynamic_cast<MAmidiMemoSoundOutput*>(edit.engine->outputBackend());
    REQUIRE_EQ(remote != nullptr, true);
    REQUIRE_EQ(a->discardProgramEdit(edit), true);
    REQUIRE_EQ(remote->waitForIdle(2000), true);
    mock.blocked.store(true, std::memory_order_release);
    mock.entered.store(false, std::memory_order_release);
    REQUIRE_EQ(remote->enqueueChipAccess({11, 0, 8, 0}), true);
    mock.waitEntered();
    switching = std::jthread([&] {
        switched.store(a->setSoundOutputKind(SoundOutputKind::Emulator), std::memory_order_release);
    });
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!a->outputSwitchPending() && std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_EQ(a->outputSwitchPending(), true);
    SoundOutputSnapshot cached;
    cached.status = "retained while another instance closes";
    const auto snapshot_started = std::chrono::steady_clock::now();
    for (int attempt = 0; attempt < 32; ++attempt) {
        REQUIRE_EQ(a->trySoundOutputSnapshot(cached), OutputSnapshotResult::Busy);
        REQUIRE_EQ(b->trySoundOutputSnapshot(cached), OutputSnapshotResult::Busy);
    }
    REQUIRE_EQ(std::chrono::steady_clock::now() - snapshot_started < std::chrono::milliseconds(250), true);
    REQUIRE_EQ(cached.status, std::string("retained while another instance closes"));
    const auto revision = b->outputRoutingRevision();
    REQUIRE_EQ(b->submit(EngineCommand::noteOn(0, 60)), true);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_EQ(b->setMAmidiSettings(settings), true);
    REQUIRE_EQ(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), true);
    REQUIRE_EQ(b->setSoundOutputKind(SoundOutputKind::MAmidiMemo), false);
    REQUIRE_EQ(b->reconnectMAmidi(), false);
    REQUIRE_EQ(b->soundOutputLastError().find("timed out") != std::string::npos, true);
    REQUIRE_EQ(b->mamidiSettings().host, settings.host);
    REQUIRE_EQ(b->mamidiSettings().port, settings.port);
    REQUIRE_EQ(b->outputRoutingRevision(), revision);
    REQUIRE_EQ(b->soundOutputKind(), SoundOutputKind::Emulator);
    REQUIRE_EQ(b->desiredSoundOutputKind(), SoundOutputKind::Emulator);
    REQUIRE_EQ(b->ownsHardwareOutput(), false);
    REQUIRE_EQ(b->outputSwitchPending(), false);
    REQUIRE_EQ(b->pendingCommandCount(), std::size_t{1});
    mock.blocked.store(false, std::memory_order_release);
    switching.join();
    REQUIRE_EQ(switched.load(std::memory_order_acquire), true);
    REQUIRE_EQ(b->setMAmidiSettings(settings), true);
    REQUIRE_EQ(b->setSoundOutputKind(SoundOutputKind::MAmidiMemo), true);
    REQUIRE_EQ(b->setSoundOutputKind(SoundOutputKind::Emulator), true);
    mock.server.stop();
}
