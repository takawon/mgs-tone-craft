// SPDX-License-Identifier: AGPL-3.0-only
// Thin per-editor adapter. Plugin State stays on MgstcAudioProcessor.
// This object dies with the AudioProcessorEditor and does not open
// OS audio, standalone MIDI, MAmidi, VSIF, or the tone library.

#pragma once

#include <array>
#include <vector>

#include "editor_session.hpp"
#include "mgstc/engine/opll_register_auto.hpp"
#include "plugin_processor.hpp"

namespace mgstc::plugin {

class PluginEditorContext final : public mgstc::app::EditorSession {
public:
    explicit PluginEditorContext(MgstcAudioProcessor& processor)
        : processor_(processor),
          output_(*this),
          midi_(*this),
          audition_(*this) {
        processor_.editorRetainCompositeScope();
    }

    ~PluginEditorContext() override {
        processor_.editorReleaseCompositeScope();
        static_cast<void>(processor_.editorRestoreCommittedProgram());
    }

    PluginEditorContext(const PluginEditorContext&) = delete;
    PluginEditorContext& operator=(const PluginEditorContext&) = delete;

    [[nodiscard]] mgstc::app::EditorSnapshot snapshot() override {
        mgstc::app::EditorSnapshot snap;
        snap.capabilities = mgstc::app::pluginEditorCapabilities();
        snap.audio_running = processor_.editorProgramReady();
        snap.master_volume_percent =
            processor_.editorMasterVolumePercent();
        snap.master_volume_revision =
            processor_.editorMasterVolumeRevision();
        snap.shared_program_active =
            processor_.editorSharedProgramActive();
        snap.shared_scc_waveform = processor_.editorSharedScc();
        snap.shared_scc_revision = processor_.editorSharedSccRevision();
        snap.shared_opll_patch = processor_.editorSharedOpll();
        snap.shared_opll_revision = processor_.editorSharedOpllRevision();
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
        return processor_.editorPollCompositeScope(frame);
    }

    [[nodiscard]] bool pollLatestOpllScope(
        mgstc::engine::OpllScopeFrame&) override {
        return false;
    }

private:
    class Output final : public mgstc::app::OutputBoundary {
    public:
        explicit Output(PluginEditorContext& self) noexcept : self_(self) {}

        void setMasterVolumePercent(int percent) override {
            self_.processor_.editorSetMasterVolumePercent(percent);
        }

        [[nodiscard]] std::vector<std::string> availableAsioDrivers() override {
            self_.processor_.editorNoteBlockedBackendCall();
            return {};
        }

        [[nodiscard]] bool applyPcAudio(
            mgstc::app::PcAudioBackend,
            std::string) override {
            self_.processor_.editorNoteBlockedBackendCall();
            return false;
        }

        [[nodiscard]] bool showAsioControlPanel() override {
            self_.processor_.editorNoteBlockedBackendCall();
            return false;
        }

        [[nodiscard]] std::string hardwareStatus() override {
            self_.processor_.editorNoteBlockedBackendCall();
            return {};
        }

        [[nodiscard]] bool applySoundRoute(
            bool,
            mgstc::app::HardwareOutputSettings,
            int) override {
            self_.processor_.editorNoteBlockedBackendCall();
            return false;
        }

        [[nodiscard]] bool reconnectHardware() override {
            self_.processor_.editorNoteBlockedBackendCall();
            return false;
        }

    private:
        PluginEditorContext& self_;
    };

    class Midi final : public mgstc::app::MidiInputBoundary {
    public:
        explicit Midi(PluginEditorContext& self) noexcept : self_(self) {}

        void addDrainListener(mgstc::app::MidiDrainListener*) override {}

        void removeDrainListener(mgstc::app::MidiDrainListener*) override {}

        void refreshDevices(bool) override {
            self_.processor_.editorNoteBlockedBackendCall();
        }

        void selectDevice(int, bool) override {
            self_.processor_.editorNoteBlockedBackendCall();
        }

        [[nodiscard]] std::vector<juce::MidiMessage>
        takePendingMessages() override {
            return {};
        }

        void clearPendingMessages() override {}

    private:
        PluginEditorContext& self_;
    };

    class Audition final : public mgstc::app::AuditionBoundary {
    public:
        explicit Audition(PluginEditorContext& self) noexcept
            : self_(self) {}

        [[nodiscard]] mgstc::app::AuditionResult submitComposite(
            const mgstc::app::CompositeAuditionRequest& request) override {
            mgstc::app::AuditionResult result;
            if (request.timbre == nullptr) {
                return result;
            }
            static_cast<void>(
                mgstc::engine::enforceOpllRegisterAutoExclusivity(
                    *request.timbre));
            std::uint8_t capacity = 1;
            result.update_allocator = true;
            result.ok = self_.processor_.replaceEditorComposite(
                *request.timbre,
                request.polyphonic,
                capacity);
            result.voice_capacity = capacity;
            return result;
        }

        [[nodiscard]] bool submitShared(
            const mgstc::app::SharedAuditionRequest& request) override {
            if (request.scc == nullptr || request.opll == nullptr) {
                return false;
            }
            return self_.processor_.editorAuditionShared(
                *request.scc,
                *request.opll,
                request.retrigger,
                request.track,
                request.note,
                request.commit_scc,
                request.commit_opll);
        }

        void publishSharedScc(
            const mgstc::engine::SccWaveform& waveform) override {
            self_.processor_.editorPublishSharedScc(waveform);
        }

        void publishSharedOpll(
            const mgstc::engine::OpllPatchParameters& patch) override {
            self_.processor_.editorPublishSharedOpll(patch);
        }

        [[nodiscard]] bool noteOn(
            std::uint8_t track,
            std::uint8_t note) override {
            return self_.processor_.editorNoteOn(track, note);
        }

        [[nodiscard]] bool noteOff(std::uint8_t track) override {
            return self_.processor_.editorNoteOff(track);
        }

        void armOpllKeyOffSilence(std::uint8_t) override {}

        void cancelOpllKeyOffSilence(std::uint8_t) override {}

        void clearOpllKeyOffSilence() override {}

        void flushPending() override {
            self_.processor_.editorFlushPending();
        }

    private:
        PluginEditorContext& self_;
    };

    MgstcAudioProcessor& processor_;
    Output output_;
    Midi midi_;
    Audition audition_;
};

}  // namespace mgstc::plugin
