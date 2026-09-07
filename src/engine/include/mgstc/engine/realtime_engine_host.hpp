#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    [[nodiscard]] ProgramEdit beginProgramEdit();
    [[nodiscard]] bool submitProgram(
        ProgramEdit& edit,
        ProgramAudition audition = {}) noexcept;
    [[nodiscard]] bool discardProgramEdit(
        ProgramEdit& edit) noexcept;

    // UI thread only.
    [[nodiscard]] bool submit(const EngineCommand& command) noexcept;

    // UI thread only. Does not auto-fall back to emulator on failure.
    void setMAmidiSettings(MAmidiOutputSettings settings);
    [[nodiscard]] const MAmidiOutputSettings& mamidiSettings() const noexcept;
    [[nodiscard]] bool setSoundOutputKind(SoundOutputKind kind);
    [[nodiscard]] SoundOutputKind soundOutputKind() const noexcept;
    [[nodiscard]] bool reconnectMAmidi();
    [[nodiscard]] std::string soundOutputStatus() const;
    [[nodiscard]] std::string soundOutputLastError() const;

    // UI thread only.
    [[nodiscard]] bool pollNotice(EngineNotice& notice) noexcept;
    [[nodiscard]] bool pollOpllScope(
        OpllScopeFrame& frame) noexcept;
    // UI thread only. Drain at most one full queue and return only the newest
    // frame so a live producer cannot keep the message thread in a poll loop.
    [[nodiscard]] bool pollLatestOpllScope(
        OpllScopeFrame& frame) noexcept;

    // UI controls capture; the analysis thread is the queue's sole consumer.
    // The audio thread performs one bounded lock-free copy per completed
    // 800-sample frame while enabled.
    void setSpectrogramCaptureEnabled(bool enabled) noexcept;
    [[nodiscard]] bool pollSpectrogramScope(
        OpllScopeFrame& frame) noexcept;

    // Audio thread only. Commands are drained before the first sample.
    [[nodiscard]] RenderResult render(
        std::span<float> interleaved_stereo) noexcept;

    [[nodiscard]] std::size_t pendingCommandCount() const noexcept {
        return commands_.approximateSize();
    }

private:
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
    void applyOutputRouting() noexcept;
    void applyOutputRouting(EngineCore& engine) noexcept;
    void noteStarted(std::uint8_t track, std::uint8_t note) noexcept;
    void noteStopped(std::uint8_t track) noexcept;
    void allNotesStopped() noexcept;
    void annotateGuideNote(OpllScopeFrame& frame) const noexcept;

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
    std::unique_ptr<ProgramSlot[]> programs_;
    std::unique_ptr<MAmidiMemoSoundOutput> mamidi_;
    MAmidiOutputSettings mamidi_settings_{};
    SoundOutputKind output_kind_{SoundOutputKind::Emulator};
    std::uint8_t active_program_{};
    MixerGains current_gains_{};
    std::array<ActiveGuideNote, RuntimeSession::kTrackCount>
        active_guide_notes_{};
    std::atomic<bool> spectrogram_capture_enabled_{false};
    std::uint64_t guide_note_order_{};
    std::uint8_t last_guide_note_{60};
    std::uint8_t last_guide_track_{};
    bool clipping_{};
};

}  // namespace mgstc::engine
