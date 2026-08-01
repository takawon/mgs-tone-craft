#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/engine_core.hpp"
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

class RealtimeEngineHost {
public:
    static constexpr std::size_t kCommandCapacity = 256;
    static constexpr std::size_t kNoticeCapacity = 128;
    static constexpr std::size_t kOpllScopeCapacity = 8;
    static constexpr std::size_t kProgramSlotCount = 3;

    RealtimeEngineHost();

    // UI thread only. Construction and program editing may allocate.
    [[nodiscard]] ProgramEdit beginProgramEdit();
    [[nodiscard]] bool submitProgram(
        ProgramEdit& edit,
        ProgramAudition audition = {}) noexcept;
    [[nodiscard]] bool discardProgramEdit(
        ProgramEdit& edit) noexcept;

    // UI thread only.
    [[nodiscard]] bool submit(const EngineCommand& command) noexcept;

    // UI thread only.
    [[nodiscard]] bool pollNotice(EngineNotice& notice) noexcept;
    [[nodiscard]] bool pollOpllScope(
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

    SpscQueue<EngineCommand, kCommandCapacity> commands_{};
    SpscQueue<EngineNotice, kNoticeCapacity> notices_{};
    std::unique_ptr<SpscQueue<OpllScopeFrame, kOpllScopeCapacity>>
        opll_scope_frames_;
    std::unique_ptr<ProgramSlot[]> programs_;
    std::uint8_t active_program_{};
    MixerGains current_gains_{};
    bool clipping_{};
};

}  // namespace mgstc::engine
