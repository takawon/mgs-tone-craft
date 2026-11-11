#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/mamidi_memo_sound_output.hpp"
#include "mgstc/engine/program_edit.hpp"
#include "mgstc/engine/spsc_queue.hpp"

namespace mgstc::engine {

enum class EngineNoticeType : std::uint8_t {
    CommandRejected,
    RenderFailure,
    ClipStarted,
    ClipEnded,
    ProgramActivated,
};

struct EngineNotice {
    EngineNoticeType type{EngineNoticeType::CommandRejected};
    EngineCommandType command{EngineCommandType::Stop};
    RenderError render_error{RenderError::None};
    std::uint64_t generation{};
};

struct MAmidiOutputSettings {
    std::string host{MAmidiMemoSoundOutput::kDefaultHost};
    std::uint16_t port{MAmidiMemoSoundOutput::kDefaultPort};
    std::uint8_t unit_no{0};
    bool scc_plus{false};
    // Default off: RPC output does not drive local scope unless enabled.
    bool waveform_monitor{false};
};

class RealtimeEngineHost {
public:
    static constexpr std::size_t kCommandCapacity = 256;
    static constexpr std::size_t kNoticeCapacity = 128;
    static constexpr std::size_t kOpllScopeCapacity = 8;
    static constexpr std::size_t kSpectrogramScopeCapacity = 8;
    static constexpr std::size_t kProgramSlotCount = 3;

    RealtimeEngineHost();
    ~RealtimeEngineHost();

    RealtimeEngineHost(const RealtimeEngineHost&) = delete;
    RealtimeEngineHost& operator=(const RealtimeEngineHost&) = delete;

    // UI thread only. Construction and program editing may allocate.
    // Producer thread only, under its existing exclusion. Audio can only free
    // slots; reserve a free slot for restoring a temporary audition program.
    [[nodiscard]] std::size_t freeProgramSlotCount() const noexcept;
    [[nodiscard]] ProgramEdit beginProgramEdit();
    [[nodiscard]] bool submitProgram(
        ProgramEdit& edit,
        ProgramAudition audition = {}) noexcept;
    [[nodiscard]] bool discardProgramEdit(
        ProgramEdit& edit) noexcept;

    // UI thread only.
    [[nodiscard]] bool submit(const EngineCommand& command) noexcept;

    // Serialized non-realtime control only. The standalone startup worker may
    // select a mode while UI status/control is proxied until completion; it
    // must not race settings changes. Never call these operations from audio.
    // Does not auto-fall back to emulator on failure.
    // False means control exclusion timed out or was unavailable; settings
    // remain unchanged. Connection errors are reported separately in status.
    bool setMAmidiSettings(MAmidiOutputSettings settings);
    [[nodiscard]] const MAmidiOutputSettings& mamidiSettings() const noexcept;
    [[nodiscard]] bool setSoundOutputKind(SoundOutputKind kind);
    [[nodiscard]] SoundOutputKind soundOutputKind() const noexcept;
    [[nodiscard]] bool reconnectMAmidi();
    [[nodiscard]] std::string soundOutputStatus() const;
    [[nodiscard]] std::string soundOutputLastError() const;
    [[nodiscard]] SoundOutputKind desiredSoundOutputKind() const noexcept {
        return desired_output_kind_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool ownsHardwareOutput() const noexcept {
        return hardware_owned_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool hardwareOutputConnected() const noexcept {
        return mamidi_->connectionState() == MAmidiConnectionState::Connected;
    }
    [[nodiscard]] bool outputSwitchPending() const noexcept {
        return output_switch_pending_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t outputRoutingRevision() const noexcept {
        return output_routing_revision_.load(std::memory_order_acquire);
    }
    // Audio consumer only, before service/note/render calls for the host block.
    // Changes emission policy and PCM only, never the selected mode or lease.
    void setRealtimeOfflineRendering(bool enabled) noexcept;

    // UI thread only.
    [[nodiscard]] bool pollNotice(EngineNotice& notice) noexcept;
    [[nodiscard]] bool pollOpllScope(
        OpllScopeFrame& frame) noexcept;
    // UI thread only. Drain at most one full queue and return only the newest
    // frame so a live producer cannot keep the message thread in a poll loop.
    [[nodiscard]] bool pollLatestOpllScope(
        OpllScopeFrame& frame) noexcept;
    // Queue before the corresponding NoteOns. Only subsequent actual note
    // allocations enter capture, so delayed layers cannot show an old voice.
    [[nodiscard]] bool setScopeSelection(std::uint32_t track_mask,
        std::uint64_t context) noexcept;
    void realtimeSetScopeSelection(std::uint32_t track_mask,
        std::uint64_t context,
        const std::array<std::int8_t, RuntimeSession::kTrackCount>& layer_tracks,
        std::uint8_t note = 60) noexcept;
    [[nodiscard]] std::uint64_t scopeDroppedFrames() const noexcept {
        return scope_dropped_frames_.load(std::memory_order_relaxed);
    }

    // UI controls capture; the analysis thread is the queue's sole consumer.
    // The audio thread performs one bounded lock-free copy per completed
    // 800-sample frame while enabled.
    void setSpectrogramCaptureEnabled(bool enabled) noexcept;
    [[nodiscard]] bool pollSpectrogramScope(
        OpllScopeFrame& frame) noexcept;

    // UI controls. The low context bits identify sources; the high bits change
    // whenever capture resumes or the editor changes. Display mode is independent.
    void setSpectrumCaptureEnabled(bool enabled) noexcept;
    void setSpectrumSourceMask(std::uint8_t mask) noexcept;
    void setSpectrumOutputGain(float gain) noexcept;
    void setSpectrumChannelsEnabled(bool enabled) noexcept {
        spectrum_channels_enabled_.store(enabled, std::memory_order_release);
    }
    [[nodiscard]] bool pollSpectrumCapture(SpectrumCaptureBlock& block) noexcept;
    [[nodiscard]] std::uint64_t spectrumDroppedBlocks() const noexcept;
    [[nodiscard]] std::uint64_t spectrumContext() const noexcept {
        return spectrum_context_.load(std::memory_order_acquire);
    }

    // Independent OPLL Waveform Scope stays off in VST. Composite Editor
    // playback waveform uses this ring only while an editor context is live.
    void setOpllScopeEnabled(bool enabled) noexcept;
    [[nodiscard]] bool opllScopeEnabled() const noexcept {
        return (opll_scope_request_.load(std::memory_order_acquire) & 1U) != 0;
    }

    // Software envelope / pending key work on the active program. Does not
    // include chip-hardware release tails after key-off.
    [[nodiscard]] bool hasRealtimeWork() const noexcept;

    // Audio thread only. Drains the control SPSC (program load, mixer, …).
    // DAW MIDI must not be pushed into that queue; use the realtime note
    // APIs below and then renderAudio().
    void servicePendingControlCommands() noexcept;

    // Audio thread only. Renders the active program. Does not touch the
    // control SPSC.
    [[nodiscard]] RenderResult renderAudio(
        std::span<float> interleaved_stereo) noexcept;

    // Audio thread only. Direct RuntimeSession note ops on the active
    // program. Not routed through EngineCommand SPSC.
    [[nodiscard]] bool realtimeNoteOn(
        std::uint8_t track,
        std::uint8_t midi_note) noexcept;
    [[nodiscard]] bool realtimeNoteOff(std::uint8_t track) noexcept;
    [[nodiscard]] bool realtimeSilenceTrack(std::uint8_t track) noexcept;
    void realtimeStop() noexcept;

    // Audio thread only. Commands are drained before the first sample.
    // Compatible facade: servicePendingControlCommands() then renderAudio().
    [[nodiscard]] RenderResult render(
        std::span<float> interleaved_stereo) noexcept;

    [[nodiscard]] std::size_t pendingCommandCount() const noexcept {
        return commands_.approximateSize();
    }

    // UI / test thread only. When `process_on_caller_thread` is true (no live
    // audio consumer), applies queued commands synchronously. When false, only
    // waits for the audio thread to activate Pending programs — never calls
    // render() here (SPSC command queue has a single consumer).
    void drainPendingCommands(
        std::span<float> interleaved_stereo,
        std::size_t max_waits = 250,
        bool process_on_caller_thread = true) noexcept;

    // UI thread only. Drop orphaned Editing slots (never Active/Pending).
    void discardStuckProgramEdits() noexcept;

    // UI thread only. Block until the audio thread activates a Pending
    // program or the timeout elapses. Never calls render().
    [[nodiscard]] bool waitForPendingProgramActivation(
        std::chrono::milliseconds timeout
            = std::chrono::milliseconds(500)) noexcept;

private:
    friend class RealtimeEngineHostTestAccess;
    // A control transition closes the gate and waits outside realtime. Audio
    // makes one bounded CAS attempt and skips work while the gate is closed.
    class RealtimeAccess {
    public:
        explicit RealtimeAccess(const RealtimeEngineHost& host) noexcept;
        ~RealtimeAccess();
        explicit operator bool() const noexcept { return entered_; }
    private:
        const RealtimeEngineHost& host_;
        bool entered_{};
    };
    class OutputTransition {
    public:
        enum class WaitPolicy { Destruction };
        OutputTransition(RealtimeEngineHost& host,
            std::chrono::steady_clock::time_point deadline);
        OutputTransition(RealtimeEngineHost& host, WaitPolicy);
        ~OutputTransition();
        explicit operator bool() const noexcept { return entered_; }
    private:
        void closeGate();
        RealtimeEngineHost& host_;
        std::unique_lock<std::timed_mutex> lock_;
        bool entered_{};
    };
    [[nodiscard]] bool relinquishHardwareOutput();
    [[nodiscard]] bool relinquishHardwareOutputProtected();
    void applyMAmidiSettingsProtected(MAmidiOutputSettings settings);
    enum class OutputControlError : std::uint8_t { None, Unavailable, TimedOut };
    [[nodiscard]] const char* outputControlErrorText() const noexcept;
    void applyRealtimePolicy(EngineCore& engine) noexcept;
    [[nodiscard]] bool hasPendingProgramLoad() const noexcept;
    enum class ProgramSlotState : std::uint8_t {
        Free,
        Editing,
        Pending,
        Active,
    };
    static_assert(std::atomic<ProgramSlotState>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    struct ProgramSlot {
        EngineCore engine{};
        std::atomic<ProgramSlotState> state{ProgramSlotState::Free};
        std::atomic<std::uint64_t> generation{};
    };

    void applyPendingCommands() noexcept;
    void loadProgram(const EngineCommand& command) noexcept;
    void reject(EngineCommandType command) noexcept;
    void notify(const EngineNotice& notice) noexcept;
    void applyOutputRouting(bool changed = true) noexcept;
    void applyOutputRouting(EngineCore& engine) noexcept;
    void noteStarted(std::uint8_t track, std::uint8_t note) noexcept;
    void noteStopped(std::uint8_t track) noexcept;
    void allNotesStopped() noexcept;
    SpectrumGuideNote currentGuideNote() const noexcept;
    void annotateGuideNote(OpllScopeFrame& frame) const noexcept;
    [[nodiscard]] bool isCurrentScopeFrame(const OpllScopeFrame& frame) const noexcept;
    void selectScope(std::uint32_t mask, std::uint64_t context) noexcept;

    struct ActiveGuideNote {
        std::uint64_t order{};
        std::uint8_t note{60};
        bool active{};
    };

    SpscQueue<EngineCommand, kCommandCapacity> commands_{};
    SpscQueue<EngineNotice, kNoticeCapacity> notices_{};
    std::unique_ptr<SpscQueue<OpllScopeFrame, kOpllScopeCapacity>>
        opll_scope_frames_;
    std::unique_ptr<
        SpscQueue<OpllScopeFrame, kSpectrogramScopeCapacity>>
        spectrogram_scope_frames_;
    // Allocated during host construction; only the audio consumer uses it.
    std::unique_ptr<OpllScopeFrame> scope_frame_work_;
    std::unique_ptr<ProgramSlot[]> programs_;
    std::unique_ptr<MAmidiMemoSoundOutput> mamidi_;
    MAmidiOutputSettings mamidi_settings_{};
    std::atomic<SoundOutputKind> output_kind_{SoundOutputKind::Emulator};
    std::atomic<SoundOutputKind> desired_output_kind_{SoundOutputKind::Emulator};
    std::atomic<bool> hardware_owned_{false};
    std::atomic<bool> output_switch_pending_{false};
    std::atomic<bool> waveform_monitor_{false};
    std::atomic<bool> offline_rendering_{false};
    std::atomic<std::uint64_t> output_routing_revision_{0};
    mutable std::atomic<std::uint32_t> output_access_{0};
    mutable std::timed_mutex output_control_mutex_;
    std::atomic<OutputControlError> output_control_error_{OutputControlError::None};
    std::string output_error_;
    std::uint8_t active_program_{};
    MixerGains current_gains_{};
    std::array<ActiveGuideNote, RuntimeSession::kTrackCount>
        active_guide_notes_{};
    std::atomic<bool> spectrogram_capture_enabled_{false};
    std::uint64_t guide_note_order_{};
    std::uint8_t last_guide_note_{60};
    std::uint8_t last_guide_track_{};
    std::uint32_t scope_requested_mask_{};
    std::uint32_t scope_started_mask_{};
    std::uint64_t scope_selection_context_{};
    // Ordered display identity across slot reuse and runtime resets. Program
    // edit handles retain their independent per-slot generation contract.
    std::uint64_t scope_program_epoch_{1};
    std::uint64_t scope_ui_context_{};
    std::uint8_t scope_selection_note_{60};
    std::atomic<std::uint64_t> scope_dropped_frames_{};
    std::array<std::int8_t, RuntimeSession::kTrackCount> scope_layer_tracks_ = [] {
        std::array<std::int8_t, RuntimeSession::kTrackCount> result{};
        result.fill(-1);
        return result;
    }();
    std::unique_ptr<SpectrumCapture> spectrum_capture_;
    std::atomic<bool> spectrum_enabled_{false};
    std::atomic<bool> spectrum_channels_enabled_{true};
    std::atomic<std::uint64_t> spectrum_context_{15};
    std::atomic<float> spectrum_output_gain_{1.0F};
    std::uint64_t spectrum_sample_clock_{};
    std::uint64_t spectrum_pcm_epoch_{};
    bool clipping_{};
    // UI request: low bit enabled, upper bits capture lifetime. Only the audio
    // consumer applies this request to the active core under RealtimeAccess.
    std::atomic<std::uint64_t> opll_scope_request_{1};
    std::uint64_t applied_opll_scope_request_{1};
};

}  // namespace mgstc::engine
