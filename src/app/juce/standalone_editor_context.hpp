// SPDX-License-Identifier: AGPL-3.0-only
//
// Include only from main.cpp, inside the anonymous namespace, after
// SharedMidiInputService. This file has no includes of its own: pulling
// headers in from inside that namespace would nest them there.

#pragma once

[[nodiscard]] mgstc::app::AuditionResult submitCompositeAudition(
    SharedAudioService& audio,
    const mgstc::app::CompositeAuditionRequest& request) {
    mgstc::app::AuditionResult result;
    if (request.timbre == nullptr) {
        return result;
    }
    auto& engine = audio.engine();
    audio.clearOpllKeyOffForceSilence();
    std::array<float, 256> drain{};
    engine.drainPendingCommands(drain, 250, !audio.running());
    auto edit = engine.beginProgramEdit();
    if (!edit.valid()) {
        engine.discardStuckProgramEdits();
        edit = engine.beginProgramEdit();
    }
    if (!edit.valid()) {
        return result;
    }
    static_cast<void>(
        mgstc::engine::enforceOpllRegisterAutoExclusivity(*request.timbre));
    mgstc::engine::CompositePlaybackPlan plan;
    const bool configured = mgstc::engine::compileCompositeProgram(
        *edit.engine,
        *request.timbre,
        {
            .polyphonic = request.polyphonic,
            .library = request.library,
        },
        &plan);
    result.update_allocator = true;
    result.voice_capacity = plan.voice_capacity;
    if (!configured) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return result;
    }
    if (!engine.submitProgram(edit)) {
        static_cast<void>(engine.discardProgramEdit(edit));
        return result;
    }
    audio.invalidateSharedEditorProgram();
    if (audio.running()) {
        static_cast<void>(engine.waitForPendingProgramActivation());
    } else {
        engine.drainPendingCommands(drain, 250, true);
    }
    result.ok = true;
    return result;
}

class StandaloneEditorContext final
    : public mgstc::app::EditorSession,
      private SharedMidiInputService::DrainListener {
public:
    StandaloneEditorContext(
        SharedAudioService& audio,
        SharedMidiInputService& midi)
        : audio_(audio),
          midi_service_(midi),
          output_(*this),
          midi_(*this),
          audition_(*this) {
        midi_service_.addDrainListener(this);
    }

    ~StandaloneEditorContext() override {
        midi_service_.removeDrainListener(this);
    }

    StandaloneEditorContext(const StandaloneEditorContext&) = delete;
    StandaloneEditorContext& operator=(const StandaloneEditorContext&) = delete;

    [[nodiscard]] mgstc::app::EditorSnapshot snapshot() override {
        mgstc::app::EditorSnapshot snap;
        snap.capabilities = mgstc::app::standaloneEditorCapabilities();
        snap.audio_running = audio_.running();
        snap.master_volume_percent = audio_.masterVolumePercent();
        snap.master_volume_revision = audio_.masterVolumeRevision();
        snap.opll_keyoff_silence_seconds =
            audio_.opllKeyOffForceSilenceSeconds();
        snap.pc_audio_backend = audio_.pcAudioBackend();
        snap.asio_driver_name = audio_.asioDriverName();
        snap.asio_control_panel_available =
            audio_.asioControlPanelAvailable();
        snap.pc_audio_status = audio_.pcAudioStatus();
        const auto kind = audio_.engine().soundOutputKind();
        snap.hardware_output =
            kind == mgstc::engine::SoundOutputKind::MAmidiMemo;
        const auto& mamidi = audio_.engine().mamidiSettings();
        snap.hardware_settings.host = mamidi.host;
        snap.hardware_settings.port = mamidi.port;
        snap.hardware_settings.unit_no = mamidi.unit_no;
        snap.hardware_settings.scc_plus = mamidi.scc_plus;
        snap.hardware_settings.waveform_monitor = mamidi.waveform_monitor;
        const auto& devices = midi_service_.devices();
        snap.midi_devices.reserve(
            static_cast<std::size_t>(devices.size()));
        for (const auto& device : devices) {
            snap.midi_devices.push_back(
                {device.name, device.identifier});
        }
        snap.midi_selected_combo_id = midi_service_.selectedComboId();
        snap.midi_status = midi_service_.statusText();
        snap.midi_revision = midi_service_.revision();
        snap.shared_program_active = audio_.sharedEditorProgramActive();
        snap.shared_scc_waveform = audio_.sharedSccWaveform();
        snap.shared_scc_revision = audio_.sharedSccWaveRevision();
        snap.shared_opll_patch = audio_.sharedOpllPatch();
        snap.shared_opll_revision = audio_.sharedOpllPatchRevision();
        return snap;
    }

    [[nodiscard]] mgstc::app::OutputBoundary& output() noexcept override {
        return output_;
    }

    [[nodiscard]] mgstc::app::MidiInputBoundary& midi() noexcept override {
        return midi_;
    }

    [[nodiscard]] mgstc::app::AuditionBoundary& audition() noexcept override {
        return audition_;
    }

    [[nodiscard]] bool pollOpllScope(
        mgstc::engine::OpllScopeFrame& frame) override {
        return audio_.engine().pollOpllScope(frame);
    }

    [[nodiscard]] bool pollLatestOpllScope(
        mgstc::engine::OpllScopeFrame& frame) override {
        return audio_.engine().pollLatestOpllScope(frame);
    }

private:
    class Output final : public mgstc::app::OutputBoundary {
    public:
        explicit Output(StandaloneEditorContext& self) noexcept
            : self_(self) {}

        void setMasterVolumePercent(int percent) override {
            self_.audio_.setMasterVolumePercent(percent);
        }

        [[nodiscard]] std::vector<std::string> availableAsioDrivers() override {
            return self_.audio_.asioDriverNames();
        }

        [[nodiscard]] bool applyPcAudio(
            mgstc::app::PcAudioBackend backend,
            std::string driver_name) override {
            return self_.audio_.applyPcAudioSettings(
                backend, std::move(driver_name));
        }

        [[nodiscard]] bool showAsioControlPanel() override {
            return self_.audio_.showAsioControlPanel();
        }

        [[nodiscard]] std::string hardwareStatus() override {
            return self_.audio_.engine().soundOutputStatus();
        }

        [[nodiscard]] bool applySoundRoute(
            bool hardware,
            mgstc::app::HardwareOutputSettings settings,
            int opll_keyoff_silence_seconds) override {
            mgstc::engine::MAmidiOutputSettings mamidi{};
            mamidi.host = settings.host.empty()
                ? std::string("localhost")
                : std::move(settings.host);
            mamidi.port = settings.port;
            mamidi.unit_no = settings.unit_no;
            mamidi.scc_plus = settings.scc_plus;
            mamidi.waveform_monitor = settings.waveform_monitor;
            self_.audio_.engine().setMAmidiSettings(std::move(mamidi));
            self_.audio_.setOpllKeyOffForceSilenceSeconds(
                opll_keyoff_silence_seconds);
            const auto kind = hardware
                ? mgstc::engine::SoundOutputKind::MAmidiMemo
                : mgstc::engine::SoundOutputKind::Emulator;
            const bool ok = self_.audio_.engine().setSoundOutputKind(kind);
            self_.audio_.saveSoundOutputSettings();
            return ok;
        }

        [[nodiscard]] bool reconnectHardware() override {
            return self_.audio_.engine().reconnectMAmidi();
        }

    private:
        StandaloneEditorContext& self_;
    };

    class Midi final : public mgstc::app::MidiInputBoundary {
    public:
        explicit Midi(StandaloneEditorContext& self) noexcept
            : self_(self) {}

        void addDrainListener(mgstc::app::MidiDrainListener* listener) override {
            self_.app_drains_.add(listener);
        }

        void removeDrainListener(
            mgstc::app::MidiDrainListener* listener) override {
            self_.app_drains_.remove(listener);
        }

        void refreshDevices(bool select_first) override {
            self_.midi_service_.refreshDevices(select_first);
        }

        void selectDevice(int combo_id, bool persist) override {
            self_.midi_service_.selectComboId(combo_id, persist);
        }

        [[nodiscard]] std::vector<juce::MidiMessage>
        takePendingMessages() override {
            return self_.midi_service_.takePendingMessages();
        }

        void clearPendingMessages() override {
            self_.midi_service_.clearPendingMessages();
        }

    private:
        StandaloneEditorContext& self_;
    };

    class Audition final : public mgstc::app::AuditionBoundary {
    public:
        explicit Audition(StandaloneEditorContext& self) noexcept
            : self_(self) {}

        [[nodiscard]] mgstc::app::AuditionResult submitComposite(
            const mgstc::app::CompositeAuditionRequest& request) override {
            return submitCompositeAudition(self_.audio_, request);
        }

        [[nodiscard]] bool submitShared(
            const mgstc::app::SharedAuditionRequest& request) override {
            if (request.scc == nullptr || request.opll == nullptr) {
                return false;
            }
            if (request.commit_scc) {
                self_.audio_.setSharedSccWaveform(*request.scc);
            }
            if (request.commit_opll) {
                self_.audio_.setSharedOpllPatch(*request.opll);
            }
            return self_.audio_.submitSharedEditorProgram(
                *request.scc,
                *request.opll,
                request.retrigger,
                request.track,
                request.note);
        }

        void publishSharedScc(
            const mgstc::engine::SccWaveform& waveform) override {
            self_.audio_.setSharedSccWaveform(waveform);
        }

        void publishSharedOpll(
            const mgstc::engine::OpllPatchParameters& patch) override {
            self_.audio_.setSharedOpllPatch(patch);
        }

        [[nodiscard]] bool noteOn(
            std::uint8_t track,
            std::uint8_t note) override {
            return self_.audio_.engine().submit(
                mgstc::engine::EngineCommand::noteOn(track, note));
        }

        [[nodiscard]] bool noteOff(std::uint8_t track) override {
            return self_.audio_.engine().submit(
                mgstc::engine::EngineCommand::noteOff(track));
        }

        void armOpllKeyOffSilence(std::uint8_t track) override {
            self_.audio_.armOpllKeyOffForceSilence(track);
        }

        void cancelOpllKeyOffSilence(std::uint8_t track) override {
            self_.audio_.cancelOpllKeyOffForceSilence(track);
        }

        void clearOpllKeyOffSilence() override {
            self_.audio_.clearOpllKeyOffForceSilence();
        }

        void flushPending() override {
            std::array<float, 256> drain{};
            self_.audio_.engine().drainPendingCommands(
                drain, 250, !self_.audio_.running());
        }

    private:
        StandaloneEditorContext& self_;
    };

    void midiMessagesPending() override {
        app_drains_.call(
            &mgstc::app::MidiDrainListener::midiMessagesPending);
    }

    SharedAudioService& audio_;
    SharedMidiInputService& midi_service_;
    Output output_;
    Midi midi_;
    Audition audition_;
    juce::ListenerList<mgstc::app::MidiDrainListener> app_drains_;
};
