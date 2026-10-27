// SPDX-License-Identifier: AGPL-3.0-only
// Thin per-editor adapter. Plugin State stays on MgstcAudioProcessor.
// This object dies with the AudioProcessorEditor and does not open
// OS audio or standalone MIDI. Hardware ownership stays on the processor.

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
          audition_(*this),
          pcm_preview_(*this),
          background_tasks_(*this) {
        acknowledged_restore_revision_ = processor_.editorStateRestoreRevision();
        processor_.editorRetainCompositeScope();
    }

    ~PluginEditorContext() override {
        processor_.editorStopPcmPreview();
        processor_.editorReleaseCompositeScope();
        static_cast<void>(processor_.editorRestoreCommittedProgram());
    }

    PluginEditorContext(const PluginEditorContext&) = delete;
    PluginEditorContext& operator=(const PluginEditorContext&) = delete;

    mgstc::app::EditorAuditionPreferences& auditionPreferences() noexcept override {
        return processor_.editorAuditionPreferences();
    }
    std::shared_ptr<mgstc::app::PatchSlideStock> patchSlideStock() override {
        return processor_.editorPatchSlideStock();
    }
    std::uint64_t stateRestoreRevision() const noexcept override {
        return processor_.editorStateRestoreRevision();
    }
    void acknowledgeStateRestore(std::uint64_t revision) noexcept {
        restore_tracking_ = true;
        acknowledged_restore_revision_ = revision;
    }

    [[nodiscard]] mgstc::app::EditorSnapshot snapshot() override {
        mgstc::app::EditorSnapshot snap;
        snap.capabilities = mgstc::app::pluginEditorCapabilities();
        snap.audio_running = processor_.editorProgramReady();
        processor_.editorPopulateOutputSnapshot(snap);
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

    [[nodiscard]] mgstc::app::PcmPreviewBoundary* pcmPreview() noexcept override {
        return &pcm_preview_;
    }

    [[nodiscard]] mgstc::app::BackgroundTaskBoundary* backgroundTasks() noexcept override {
        return &background_tasks_;
    }

    [[nodiscard]] bool pollOpllScope(
        mgstc::engine::OpllScopeFrame& frame) override {
        return processor_.editorPollCompositeScope(frame, scope_cursor_);
    }

    [[nodiscard]] bool pollLatestOpllScope(
        mgstc::engine::OpllScopeFrame& frame) override {
        return processor_.editorPollLatestOpllScope(frame, latest_scope_cursor_);
    }

private:
    class BackgroundTasks final : public mgstc::app::BackgroundTaskBoundary {
    public:
        explicit BackgroundTasks(PluginEditorContext& self) noexcept : self_(self) {}
        bool registerTask(std::shared_ptr<mgstc::app::EditorBackgroundTask> task) override {
            return self_.processor_.editorRegisterBackgroundTask(std::move(task));
        }
    private:
        PluginEditorContext& self_;
    };
    class PcmPreview final : public mgstc::app::PcmPreviewBoundary {
    public:
        explicit PcmPreview(PluginEditorContext& self) noexcept : self_(self) {}
        bool play(std::shared_ptr<const mgstc::engine::SourcePcm> pcm,
            std::size_t begin, std::size_t end, float gain) override {
            return self_.processor_.editorPlayPcmPreview(std::move(pcm), begin, end, gain);
        }
        void stop() noexcept override { self_.processor_.editorStopPcmPreview(); }
    private:
        PluginEditorContext& self_;
    };
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
            return self_.processor_.editorHardwareStatus();
        }

        [[nodiscard]] bool applyHardwareSettings(
            mgstc::app::HardwareOutputSettings settings,
            int keyoff_seconds) override {
            return self_.processor_.editorApplyHardwareSettings(
                std::move(settings), keyoff_seconds);
        }

        [[nodiscard]] bool requestHardwareMode(bool hardware) override {
            return self_.processor_.editorRequestHardwareMode(hardware);
        }

        [[nodiscard]] bool reconnectHardware() override {
            return self_.processor_.editorReconnectHardware();
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
            if (self_.restore_tracking_
                && self_.acknowledged_restore_revision_ != self_.stateRestoreRevision())
                return {};
            mgstc::app::AuditionResult result;
            if (request.timbre == nullptr) {
                return result;
            }
            static_cast<void>(
                mgstc::engine::enforceOpllRegisterAutoExclusivity(
                    *request.timbre));
            std::uint8_t capacity = 1;
            result.update_allocator = true;
            const auto revision = self_.restore_tracking_
                ? std::optional<std::uint64_t>(self_.acknowledged_restore_revision_)
                : std::nullopt;
            result.ok = request.temporary
                ? self_.processor_.previewEditorComposite(
                    *request.timbre, request.polyphonic, capacity, revision,
                    request.reserve_restore_slot, &result.deferred)
                : self_.processor_.replaceEditorComposite(
                    *request.timbre, request.polyphonic, capacity, revision);
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
            return self_.processor_.editorNoteOn(track, note, restoreRevision());
        }

        [[nodiscard]] bool noteOff(std::uint8_t track) override {
            return self_.processor_.editorNoteOff(track, restoreRevision());
        }

        void armOpllKeyOffSilence(std::uint8_t track) override {
            self_.processor_.editorArmOpllKeyoffSilence(track, restoreRevision());
        }

        void cancelOpllKeyOffSilence(std::uint8_t track) override {
            self_.processor_.editorCancelOpllKeyoffSilence(track, restoreRevision());
        }

        void clearOpllKeyOffSilence() override {
            self_.processor_.editorClearOpllKeyoffSilence(restoreRevision());
        }

        void flushPending() override {
            self_.processor_.editorFlushPending();
        }

    private:
        std::optional<std::uint64_t> restoreRevision() const noexcept {
            return self_.restore_tracking_
                ? std::optional<std::uint64_t>(self_.acknowledged_restore_revision_)
                : std::nullopt;
        }
        PluginEditorContext& self_;
    };

    MgstcAudioProcessor& processor_;
    std::uint64_t acknowledged_restore_revision_{};
    bool restore_tracking_{};
    std::uint64_t scope_cursor_{};
    std::uint64_t latest_scope_cursor_{};
    Output output_;
    Midi midi_;
    Audition audition_;
    PcmPreview pcm_preview_;
    BackgroundTasks background_tasks_;
};

}  // namespace mgstc::plugin
