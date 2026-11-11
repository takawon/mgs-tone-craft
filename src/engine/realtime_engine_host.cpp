#include "mgstc/engine/realtime_engine_host.hpp"

#if defined(MGSTC_HOST_RENDER_GAP_TESTING)
// Only the isolated lifetime test compiles this probe; product audio has none.
extern void mgstcHostRenderGapTestHook() noexcept;
#endif

#include <type_traits>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mgstc::engine {

namespace {
constexpr std::uint32_t kOutputClosed = 0x80000000U;
constexpr auto kOutputControlWait = std::chrono::milliseconds(500);
using OutputDeadline = std::chrono::steady_clock::time_point;

// This is a control-thread coordinator. The mapping contains only pointers
// belonging to this process, allowing two loaded copies of the plugin module
// to transfer the same lease. The file handle is an OS lifetime lock; unlike a
// mutex owned by a UI thread it also survives that thread's termination.
struct OutputLease {
    struct State {
        void* owner{};
        bool (*relinquish)(void*){};
#ifdef _WIN32
        HANDLE device_lock{};
#endif
    };
#ifdef _WIN32
    HANDLE mutex{};
    HANDLE mapping{};
    State* state{};
    OutputLease() {
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        mutex = CreateMutexW(nullptr, FALSE,
            (L"Local\\MGSTC.OutputControl.v1." + suffix).c_str());
        if (mutex == nullptr) return;
        // Named page-file mappings are atomically created/opened by Windows
        // and begin zero-filled. No shared State is accessed during setup.
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, sizeof(State),
            (L"Local\\MGSTC.OutputOwner.v1." + suffix).c_str());
        if (mapping != nullptr)
            state = static_cast<State*>(MapViewOfFile(mapping,
                FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
    }
    bool lock(OutputDeadline deadline) {
        if (state == nullptr) return false;
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto timeout = static_cast<DWORD>(std::max<std::int64_t>(0,
            remaining.count()));
        return WaitForSingleObject(mutex, timeout) <= WAIT_ABANDONED;
    }
    bool lockForDestruction() { return state != nullptr
        && WaitForSingleObject(mutex, INFINITE) <= WAIT_ABANDONED; }
    void unlock() { ReleaseMutex(mutex); }
    bool lockDevice() {
        if (state->device_lock != nullptr) return true;
        // Resolve the user's actual folder rather than a process-specific TMP
        // or environment override (DAWs may customize either).
        wchar_t path[MAX_PATH + 1]{};
        const auto shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (shell == nullptr) return false;
        using FolderPath = HRESULT (WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
        const auto folder = reinterpret_cast<FolderPath>(GetProcAddress(shell, "SHGetFolderPathW"));
        const bool found = folder != nullptr && SUCCEEDED(folder(nullptr, 0x001c, nullptr, 0, path));
        FreeLibrary(shell);
        if (!found) return false;
        const auto file = std::wstring(path) + L"\\MGSTC.MAmidiMEmo.output.v1.lock";
        const auto handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        state->device_lock = handle;
        return true;
    }
    void unlockDevice() {
        if (state->device_lock != nullptr) CloseHandle(state->device_lock);
        state->device_lock = nullptr;
    }
#else
    std::timed_mutex mutex;
    State local{};
    State* state{&local};
    bool lock(OutputDeadline deadline) { return mutex.try_lock_until(deadline); }
    bool lockForDestruction() { mutex.lock(); return true; }
    void unlock() { mutex.unlock(); }
    bool lockDevice() { return true; }
    void unlockDevice() {}
#endif
};

OutputLease& outputLease() {
    // Process lifetime, including late destruction of global EngineHosts and
    // module unload/reload. The OS releases the mapping/handles on process exit.
    static auto* lease = new OutputLease;
    return *lease;
}
struct LeaseLock {
    enum class WaitPolicy { Destruction };
    OutputLease& lease{outputLease()};
    bool locked{};
    explicit LeaseLock(OutputDeadline deadline
        = std::chrono::steady_clock::now() + kOutputControlWait)
        : locked(lease.lock(deadline)) {}
    explicit LeaseLock(WaitPolicy) : locked(lease.lockForDestruction()) {}
    ~LeaseLock() { if (locked) lease.unlock(); }
};
} // namespace

RealtimeEngineHost::RealtimeAccess::RealtimeAccess(
    const RealtimeEngineHost& host) noexcept : host_(host) {
    std::uint32_t expected = 0;
    entered_ = host_.output_access_.compare_exchange_strong(expected, 1,
        std::memory_order_acquire, std::memory_order_relaxed);
}
RealtimeEngineHost::RealtimeAccess::~RealtimeAccess() {
    if (entered_) host_.output_access_.fetch_sub(1, std::memory_order_release);
}
void RealtimeEngineHost::OutputTransition::closeGate() {
    host_.output_switch_pending_.store(true, std::memory_order_release);
    host_.output_access_.fetch_or(kOutputClosed, std::memory_order_acq_rel);
}
RealtimeEngineHost::OutputTransition::OutputTransition(RealtimeEngineHost& host,
    OutputDeadline deadline)
    : host_(host), lock_(host.output_control_mutex_, std::defer_lock) {
    if (!lock_.try_lock_until(deadline)) {
        host_.output_control_error_.store(OutputControlError::TimedOut, std::memory_order_release);
        return;
    }
    closeGate();
    const auto timed_out = [&] {
        if (std::chrono::steady_clock::now() >= deadline) {
            // Keep an in-flight audio reader counted when undoing our gate.
            host_.output_access_.fetch_and(~kOutputClosed, std::memory_order_release);
            host_.output_switch_pending_.store(false, std::memory_order_release);
            host_.output_control_error_.store(OutputControlError::TimedOut, std::memory_order_release);
            return true;
        }
        return false;
    };
    while (host_.output_access_.load(std::memory_order_acquire) != kOutputClosed) {
        if (timed_out()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (timed_out()) return;
    entered_ = true;
    host_.output_control_error_.store(OutputControlError::None, std::memory_order_release);
}
RealtimeEngineHost::OutputTransition::OutputTransition(RealtimeEngineHost& host, WaitPolicy)
    : host_(host), lock_(host.output_control_mutex_) {
    // Destruction cannot return while an admitted audio reader still uses us.
    closeGate();
    while (host_.output_access_.load(std::memory_order_acquire) != kOutputClosed)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    entered_ = true;
}
RealtimeEngineHost::OutputTransition::~OutputTransition() {
    if (!entered_) return;
    host_.output_access_.fetch_and(~kOutputClosed, std::memory_order_release);
    host_.output_switch_pending_.store(false, std::memory_order_release);
}

const char* RealtimeEngineHost::outputControlErrorText() const noexcept {
    switch (output_control_error_.load(std::memory_order_acquire)) {
    case OutputControlError::Unavailable: return "MAmidiMEmo ownership coordinator is unavailable";
    case OutputControlError::TimedOut: return "Output control transition timed out";
    default: return nullptr;
    }
}

static_assert(std::is_nothrow_move_assignable_v<EngineCore>);

RealtimeEngineHost::RealtimeEngineHost()
    : opll_scope_frames_(
          std::make_unique<
              SpscQueue<OpllScopeFrame, kOpllScopeCapacity>>()),
      spectrogram_scope_frames_(
          std::make_unique<SpscQueue<
              OpllScopeFrame, kSpectrogramScopeCapacity>>()),
      scope_frame_work_(std::make_unique<OpllScopeFrame>()),
      programs_(std::make_unique<ProgramSlot[]>(kProgramSlotCount)),
      mamidi_(std::make_unique<MAmidiMemoSoundOutput>()),
      spectrum_capture_(std::make_unique<SpectrumCapture>()) {
    programs_[0].state.store(
        ProgramSlotState::Active,
        std::memory_order_relaxed);
    applyOutputRouting(programs_[0].engine);
}

void RealtimeEngineHost::setOpllScopeEnabled(bool enabled) noexcept {
    auto previous = opll_scope_request_.load(std::memory_order_acquire);
    while (((previous & 1U) != 0) != enabled) {
        const auto next = ((previous & ~std::uint64_t{1}) + 2U) | (enabled ? 1U : 0U);
        if (opll_scope_request_.compare_exchange_weak(previous, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}

bool RealtimeEngineHost::hasRealtimeWork() const noexcept {
    RealtimeAccess access(*this);
    if (!access) return false;
    return programs_[active_program_].engine.session().hasRealtimeWork();
}

RealtimeEngineHost::~RealtimeEngineHost() {
    // Keep lifetime drain distinct from the bounded interactive operations.
    LeaseLock lease(LeaseLock::WaitPolicy::Destruction);
    OutputTransition transition(*this, OutputTransition::WaitPolicy::Destruction);
    if (lease.locked && lease.lease.state->owner == this) {
        static_cast<void>(relinquishHardwareOutputProtected());
        lease.lease.state->owner = nullptr;
        lease.lease.state->relinquish = nullptr;
        lease.lease.unlockDevice();
    } else {
        mamidi_->close();
    }
}

std::size_t RealtimeEngineHost::freeProgramSlotCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < kProgramSlotCount; ++index)
        if (programs_[index].state.load(std::memory_order_acquire) == ProgramSlotState::Free)
            ++count;
    return count;
}

ProgramEdit RealtimeEngineHost::beginProgramEdit() {
    // All construction and destruction stays on the UI thread.
    // Skip EngineCore construction when every slot is busy so rapid 1s
    // preview edits cannot stall the message thread.
    for (std::uint8_t index = 0; index < kProgramSlotCount; ++index) {
        auto& slot = programs_[index];
        if (slot.state.load(std::memory_order_relaxed)
            != ProgramSlotState::Free) {
            continue;
        }
        EngineCore fresh;
        applyOutputRouting(fresh);
        auto expected = ProgramSlotState::Free;
        if (!slot.state.compare_exchange_strong(
                expected,
                ProgramSlotState::Editing,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            continue;
        }
        slot.engine = std::move(fresh);
        const auto generation = slot.generation.fetch_add(
            1,
            std::memory_order_relaxed) + 1;
        return {
            .slot = index,
            .generation = generation,
            .engine = &slot.engine,
        };
    }
    return {};
}

bool RealtimeEngineHost::submitProgram(
    ProgramEdit& edit,
    ProgramAudition audition) noexcept {
    NotePitch pitch{};
    if (!edit.valid()
        || edit.slot >= kProgramSlotCount
        || audition.track >= RuntimeSession::kTrackCount
        || (audition.retrigger
            && !notePitch(audition.midi_note, pitch))) {
        return false;
    }
    auto& slot = programs_[edit.slot];
    if (slot.generation.load(std::memory_order_relaxed)
            != edit.generation
        || slot.state.load(std::memory_order_acquire)
            != ProgramSlotState::Editing) {
        return false;
    }

    slot.state.store(ProgramSlotState::Pending, std::memory_order_release);
    auto command = EngineCommand::loadProgram(
        edit.slot,
        edit.generation,
        audition.retrigger,
        audition.track,
        audition.midi_note);
    if (audition.retrigger) command.output_note_token = mamidi_->captureNoteToken();
    if (!commands_.tryPush(command)) {
        slot.state.store(
            ProgramSlotState::Editing,
            std::memory_order_release);
        return false;
    }
    edit = {};
    return true;
}

bool RealtimeEngineHost::discardProgramEdit(
    ProgramEdit& edit) noexcept {
    if (!edit.valid() || edit.slot >= kProgramSlotCount) {
        return false;
    }
    auto& slot = programs_[edit.slot];
    if (slot.generation.load(std::memory_order_relaxed)
            != edit.generation
        || slot.state.load(std::memory_order_acquire)
            != ProgramSlotState::Editing) {
        return false;
    }
    slot.state.store(ProgramSlotState::Free, std::memory_order_release);
    edit = {};
    return true;
}

bool RealtimeEngineHost::submit(
    const EngineCommand& command) noexcept {
    auto stamped = command;
    if (command.type == EngineCommandType::NoteOn
        || (command.type == EngineCommandType::LoadProgram && command.retrigger))
        stamped.output_note_token = mamidi_->captureNoteToken();
    return commands_.tryPush(stamped);
}

bool RealtimeEngineHost::setMAmidiSettings(
    MAmidiOutputSettings settings) {
    const auto deadline = std::chrono::steady_clock::now() + kOutputControlWait;
    {
        OutputTransition local(*this, deadline);
        if (!local) return false;
        // Inactive connection preferences are local state. Do not let another
        // instance's RPC shutdown delay construction or discard saved settings.
        if (output_kind_ == SoundOutputKind::Emulator
            && !hardware_owned_.load(std::memory_order_acquire) && !mamidi_->isOpen()) {
            applyMAmidiSettingsProtected(std::move(settings));
            return true;
        }
    }
    // Release the local probe before the shared lease: every live remote
    // operation keeps the same Lease -> Host lock order.
    LeaseLock lease(deadline);
    if (!lease.locked) {
        output_control_error_.store(lease.lease.state == nullptr
            ? OutputControlError::Unavailable : OutputControlError::TimedOut, std::memory_order_release);
        return false;
    }
    OutputTransition transition(*this, deadline);
    if (!transition) return false;
    applyMAmidiSettingsProtected(std::move(settings));
    return true;
}

void RealtimeEngineHost::applyMAmidiSettingsProtected(MAmidiOutputSettings settings) {
    const bool endpoint_changed =
        settings.host != mamidi_settings_.host
        || settings.port != mamidi_settings_.port
        || settings.unit_no != mamidi_settings_.unit_no
        || settings.scc_plus != mamidi_settings_.scc_plus;
    mamidi_settings_ = std::move(settings);
    waveform_monitor_.store(mamidi_settings_.waveform_monitor, std::memory_order_release);
    // Waveform monitor is local-only and must update without reconnect.
    applyOutputRouting(false);

    if (mamidi_ == nullptr || !endpoint_changed) {
        return;
    }
    const bool was_open = mamidi_->isOpen();
    if (was_open) {
        mamidi_->close();
    }
    mamidi_->setEndpoint(mamidi_settings_.host, mamidi_settings_.port);
    mamidi_->setUnitNo(mamidi_settings_.unit_no);
    mamidi_->setSccPlus(mamidi_settings_.scc_plus);
    if (was_open || output_kind_ == SoundOutputKind::MAmidiMemo) {
        if (!mamidi_->open()) {
            // Keep MAmidi selected; do not silently fall back.
            applyOutputRouting();
            return;
        }
        applyOutputRouting();
    }
}

const MAmidiOutputSettings& RealtimeEngineHost::mamidiSettings()
    const noexcept {
    return mamidi_settings_;
}

bool RealtimeEngineHost::relinquishHardwareOutput() {
    // Keep the process-shared v1 callback ABI unchanged. This opaque old-owner
    // boundary has its own bounded gate deadline.
    OutputTransition transition(*this, std::chrono::steady_clock::now() + kOutputControlWait);
    if (!transition) return false;
    return relinquishHardwareOutputProtected();
}

bool RealtimeEngineHost::relinquishHardwareOutputProtected() {
    mamidi_->close();
    if (!mamidi_->closeAcknowledged()) {
        output_error_ = "Previous MAmidiMEmo writes were not acknowledged; release the previous owner explicitly";
        return false;
    }
    output_kind_.store(SoundOutputKind::Emulator, std::memory_order_release);
    desired_output_kind_.store(SoundOutputKind::Emulator, std::memory_order_release);
    hardware_owned_.store(false, std::memory_order_release);
    applyOutputRouting();
    // Retire pre-transfer UI notes as well as the active voices. Program/mixer
    // commands still take effect, then the old runtime is gated until a new note.
    applyPendingCommands();
    auto& core = programs_[active_program_].engine;
    applyRealtimePolicy(core);
    core.hardReset();
    core.session().gateUntilNoteOn();
    allNotesStopped();
    ++spectrum_pcm_epoch_;
    return true;
}

bool RealtimeEngineHost::setSoundOutputKind(SoundOutputKind kind) {
    const auto deadline = std::chrono::steady_clock::now() + kOutputControlWait;
    LeaseLock lease(deadline);
    if (!lease.locked) {
        output_control_error_.store(lease.lease.state == nullptr
            ? OutputControlError::Unavailable : OutputControlError::TimedOut, std::memory_order_release);
        return false;
    }
    // Acquire our own exclusion before changing the previous owner. A later
    // local timeout must never leave a completed handoff only half applied.
    OutputTransition transition(*this, deadline);
    if (!transition) return false;
    if (kind == SoundOutputKind::Emulator) {
        mamidi_->close();
        desired_output_kind_.store(kind, std::memory_order_release);
        output_kind_.store(kind, std::memory_order_release);
        hardware_owned_.store(false, std::memory_order_release);
        applyOutputRouting();
        applyPendingCommands();
        auto& core = programs_[active_program_].engine;
        applyRealtimePolicy(core);
        core.hardReset();
        core.session().gateUntilNoteOn();
        allNotesStopped();
        ++spectrum_pcm_epoch_;
        if (lease.lease.state->owner == this) {
            lease.lease.state->owner = nullptr;
            lease.lease.state->relinquish = nullptr;
            lease.lease.unlockDevice();
        }
        output_error_.clear();
        return true;
    }
    if (lease.lease.state->owner == this) return true;
    if (lease.lease.state->owner != nullptr) {
        if (!lease.lease.state->relinquish(lease.lease.state->owner)) {
            output_error_ = "The previous MAmidiMEmo owner could not stop safely";
            return false;
        }
        lease.lease.state->owner = nullptr;
        lease.lease.state->relinquish = nullptr;
    }
    if (!lease.lease.lockDevice()) {
        output_error_ = "MAmidiMEmo is owned by another MGSTC process";
        return false;
    }
    mamidi_->setEndpoint(mamidi_settings_.host, mamidi_settings_.port);
    mamidi_->setUnitNo(mamidi_settings_.unit_no);
    mamidi_->setSccPlus(mamidi_settings_.scc_plus);
    if (!mamidi_->open()) {
        output_error_ = mamidi_->lastError();
        lease.lease.unlockDevice();
        return false;
    }
    output_error_.clear();
    lease.lease.state->owner = this;
    lease.lease.state->relinquish = [](void* owner) {
        return static_cast<RealtimeEngineHost*>(owner)->relinquishHardwareOutput();
    };
    hardware_owned_.store(true, std::memory_order_release);
    output_kind_.store(kind, std::memory_order_release);
    desired_output_kind_.store(kind, std::memory_order_release);
    applyOutputRouting();
    // Old terminal silence is acknowledged before new initialization begins.
    auto& core = programs_[active_program_].engine;
    applyRealtimePolicy(core);
    core.hardReset();
    core.session().gateUntilNoteOn();
    allNotesStopped();
    ++spectrum_pcm_epoch_;
    return true;
}

SoundOutputKind RealtimeEngineHost::soundOutputKind() const noexcept {
    return output_kind_.load(std::memory_order_acquire);
}

bool RealtimeEngineHost::reconnectMAmidi() {
    const auto deadline = std::chrono::steady_clock::now() + kOutputControlWait;
    LeaseLock lease(deadline);
    if (!lease.locked) {
        output_control_error_.store(lease.lease.state == nullptr
            ? OutputControlError::Unavailable : OutputControlError::TimedOut, std::memory_order_release);
        return false;
    }
    output_control_error_.store(OutputControlError::None, std::memory_order_release);
    if (lease.lease.state->owner != this) {
        output_error_ = "Select hardware output before reconnecting";
        return false;
    }
    OutputTransition transition(*this, deadline);
    if (!transition) return false;
    mamidi_->close();
    mamidi_->setEndpoint(mamidi_settings_.host, mamidi_settings_.port);
    mamidi_->setUnitNo(mamidi_settings_.unit_no);
    mamidi_->setSccPlus(mamidi_settings_.scc_plus);
    if (!mamidi_->open()) {
        // Remain on MAmidiMemo if already selected; no emu auto-switch.
        applyOutputRouting();
        return false;
    }
    if (output_kind_ == SoundOutputKind::MAmidiMemo) {
        applyOutputRouting();
    }
    output_error_.clear();
    return true;
}

std::string RealtimeEngineHost::soundOutputStatus() const {
    const auto deadline = std::chrono::steady_clock::now() + kOutputControlWait;
    LeaseLock lease(deadline);
    if (!lease.locked) return lease.lease.state == nullptr
        ? "MAmidiMEmo ownership coordinator is unavailable" : "Output control transition timed out";
    std::unique_lock control(output_control_mutex_, std::defer_lock);
    if (!control.try_lock_until(deadline)) return "Output control transition timed out";
    if (const auto error = outputControlErrorText()) return error;
    if (!output_error_.empty()) return output_error_;
    if (output_kind_ == SoundOutputKind::Emulator) {
        return "Output: Emulator";
    }
    if (mamidi_ == nullptr) {
        return "Output: MAmidiMEmo (unavailable)";
    }
    return mamidi_->statusText();
}

std::string RealtimeEngineHost::soundOutputLastError() const {
    const auto deadline = std::chrono::steady_clock::now() + kOutputControlWait;
    LeaseLock lease(deadline);
    if (!lease.locked) return lease.lease.state == nullptr
        ? "MAmidiMEmo ownership coordinator is unavailable" : "Output control transition timed out";
    std::unique_lock control(output_control_mutex_, std::defer_lock);
    if (!control.try_lock_until(deadline)) return "Output control transition timed out";
    if (const auto error = outputControlErrorText()) return error;
    if (!output_error_.empty()) return output_error_;
    if (mamidi_ == nullptr) {
        return {};
    }
    return mamidi_->lastError();
}

void RealtimeEngineHost::setRealtimeOfflineRendering(bool enabled) noexcept {
    offline_rendering_.store(enabled, std::memory_order_release);
    mamidi_->setEmissionEnabled(!enabled);
}

void RealtimeEngineHost::applyRealtimePolicy(EngineCore& core) noexcept {
    core.setOfflineRendering(offline_rendering_.load(std::memory_order_acquire));
}

bool RealtimeEngineHost::pollNotice(EngineNotice& notice) noexcept {
    return notices_.tryPop(notice);
}

bool RealtimeEngineHost::pollOpllScope(
    OpllScopeFrame& frame) noexcept {
    for (std::size_t count = 0; count < kOpllScopeCapacity; ++count) {
        if (!opll_scope_frames_->tryPop(frame)) return false;
        if (isCurrentScopeFrame(frame)) return true;
    }
    return false;
}

bool RealtimeEngineHost::isCurrentScopeFrame(const OpllScopeFrame& frame) const noexcept {
    const auto request = opll_scope_request_.load(std::memory_order_acquire);
    return (request & 1U) != 0 && frame.capture_generation == (request >> 1U);
}

bool RealtimeEngineHost::setScopeSelection(std::uint32_t track_mask,
    std::uint64_t context) noexcept {
    EngineCommand command{.type = EngineCommandType::SetScopeSelection};
    command.generation = context;
    command.scope_track_mask = track_mask;
    return submit(command);
}

void RealtimeEngineHost::selectScope(std::uint32_t mask, std::uint64_t context) noexcept {
    scope_requested_mask_ = mask & ((1U << RuntimeSession::kTrackCount) - 1U);
    scope_started_mask_ = 0;
    scope_selection_context_ = context;
    scope_layer_tracks_.fill(-1);
    programs_[active_program_].engine.setScopeSelection(0, context);
}

void RealtimeEngineHost::realtimeSetScopeSelection(std::uint32_t track_mask,
    std::uint64_t context,
    const std::array<std::int8_t, RuntimeSession::kTrackCount>& layer_tracks,
    std::uint8_t note) noexcept {
    RealtimeAccess access(*this);
    if (!access) return;
    selectScope(track_mask, context);
    scope_layer_tracks_ = layer_tracks;
    scope_selection_note_ = note;
}

bool RealtimeEngineHost::pollLatestOpllScope(
    OpllScopeFrame& frame) noexcept {
    bool found = false;
    OpllScopeFrame next{};
    for (std::size_t count = 0;
         count < kOpllScopeCapacity
         && opll_scope_frames_->tryPop(next);
         ++count) {
        if (isCurrentScopeFrame(next)) {
            frame = next;
            found = true;
        }
    }
    return found;
}

void RealtimeEngineHost::setSpectrogramCaptureEnabled(
    bool enabled) noexcept {
    spectrogram_capture_enabled_.store(enabled, std::memory_order_release);
}

bool RealtimeEngineHost::pollSpectrogramScope(
    OpllScopeFrame& frame) noexcept {
    for (std::size_t count = 0; count < kSpectrogramScopeCapacity; ++count) {
        if (!spectrogram_scope_frames_->tryPop(frame)) return false;
        if (isCurrentScopeFrame(frame)) return true;
    }
    return false;
}

void RealtimeEngineHost::setSpectrumCaptureEnabled(bool enabled) noexcept {
    if (spectrum_enabled_.load(std::memory_order_acquire) == enabled) return;
    spectrum_context_.fetch_add(8, std::memory_order_acq_rel);
    spectrum_enabled_.store(enabled, std::memory_order_release);
}
void RealtimeEngineHost::setSpectrumSourceMask(std::uint8_t mask) noexcept {
    mask &= 7;
    const auto current = spectrum_context_.load(std::memory_order_acquire);
    if ((current & 7) != mask)
        spectrum_context_.store(((current & ~std::uint64_t{7}) + 8) | mask, std::memory_order_release);
}
void RealtimeEngineHost::setSpectrumOutputGain(float gain) noexcept {
    spectrum_output_gain_.store(std::isfinite(gain) ? std::clamp(gain, 0.0F, 1.0F) : 1.0F,
                               std::memory_order_release);
}
bool RealtimeEngineHost::pollSpectrumCapture(SpectrumCaptureBlock& block) noexcept {
    return spectrum_capture_->poll(block);
}
std::uint64_t RealtimeEngineHost::spectrumDroppedBlocks() const noexcept {
    return spectrum_capture_->dropped();
}

void RealtimeEngineHost::noteStarted(
    std::uint8_t track,
    std::uint8_t note) noexcept {
    if (track >= active_guide_notes_.size()) {
        return;
    }
    auto& active = active_guide_notes_[track];
    active.active = true;
    active.note = note;
    active.order = ++guide_note_order_;
    last_guide_note_ = note;
    last_guide_track_ = track;
    if ((scope_requested_mask_ & (1U << track)) != 0) {
        scope_started_mask_ |= 1U << track;
        programs_[active_program_].engine.resetScopeCapture();
        programs_[active_program_].engine.setScopeSelection(
            scope_started_mask_, scope_selection_context_);
        programs_[active_program_].engine.awaitScopeNoteOn(track);
    }
}

void RealtimeEngineHost::noteStopped(std::uint8_t track) noexcept {
    if (track < active_guide_notes_.size()) {
        active_guide_notes_[track].active = false;
    }
}

void RealtimeEngineHost::allNotesStopped() noexcept {
    ++scope_program_epoch_;
    for (auto& note : active_guide_notes_) {
        note.active = false;
    }
    scope_requested_mask_ = 0;
    scope_started_mask_ = 0;
    scope_layer_tracks_.fill(-1);
    programs_[active_program_].engine.setScopeSelection(0, scope_selection_context_);
}

SpectrumGuideNote RealtimeEngineHost::currentGuideNote() const noexcept {
    const ActiveGuideNote* newest = nullptr;
    std::uint8_t newest_track = last_guide_track_;
    for (std::size_t index = 0; index < active_guide_notes_.size(); ++index) {
        const auto& note = active_guide_notes_[index];
        if (note.active && (newest == nullptr || note.order > newest->order)) {
            newest = &note;
            newest_track = static_cast<std::uint8_t>(index);
        }
    }
    return {newest != nullptr ? newest->note : last_guide_note_, newest_track, newest != nullptr};
}

void RealtimeEngineHost::annotateGuideNote(OpllScopeFrame& frame) const noexcept {
    const auto note = currentGuideNote();
    frame.guide_note = note.note;
    frame.guide_track = note.track;
    frame.note_active = note.active;
}

void RealtimeEngineHost::notify(const EngineNotice& notice) noexcept {
    static_cast<void>(notices_.tryPush(notice));
}

void RealtimeEngineHost::reject(
    EngineCommandType command) noexcept {
    notify({
        .type = EngineNoticeType::CommandRejected,
        .command = command,
    });
}

void RealtimeEngineHost::applyOutputRouting(bool changed) noexcept {
    // Inactive Editing slots belong to the producer; refresh at activation.
    applyOutputRouting(programs_[active_program_].engine);
    if (changed) output_routing_revision_.fetch_add(1, std::memory_order_release);
}

void RealtimeEngineHost::applyOutputRouting(EngineCore& engine) noexcept {
    // Keep the MAmidi backend selected even while disconnected so writes
    // fail closed instead of silently falling back to the simulator.
    if (output_kind_ == SoundOutputKind::MAmidiMemo && mamidi_ != nullptr) {
        engine.setOutputBackend(mamidi_.get());
    } else {
        engine.setOutputBackend(nullptr);
    }
    engine.setWaveformMonitor(waveform_monitor_.load(std::memory_order_acquire));
}

void RealtimeEngineHost::loadProgram(
    const EngineCommand& command) noexcept {
    if (command.program_slot >= kProgramSlotCount) {
        reject(command.type);
        return;
    }
    auto& incoming = programs_[command.program_slot];
    if (incoming.generation.load(std::memory_order_relaxed)
            != command.generation
        || incoming.state.load(std::memory_order_acquire)
            != ProgramSlotState::Pending) {
        if (incoming.state.load(std::memory_order_acquire)
            == ProgramSlotState::Pending) {
            incoming.state.store(
                ProgramSlotState::Free,
                std::memory_order_release);
        }
        reject(command.type);
        return;
    }

    static_cast<void>(incoming.engine.setGains(current_gains_));
    incoming.engine.setOpllScopeEnabled(opllScopeEnabled());
    applyOutputRouting(incoming.engine);
    applyRealtimePolicy(incoming.engine);
    incoming.engine.hardReset();
    ++spectrum_pcm_epoch_;
    incoming.engine.session().gateUntilNoteOn();
    const auto previous = active_program_;
    active_program_ = command.program_slot;
    scope_requested_mask_ = 0;
    scope_started_mask_ = 0;
    scope_layer_tracks_.fill(-1);
    incoming.engine.setScopeSelection(0, scope_selection_context_);
    allNotesStopped();
    incoming.state.store(ProgramSlotState::Active, std::memory_order_release);
    programs_[previous].state.store(
        ProgramSlotState::Free,
        std::memory_order_release);

    if (command.retrigger
        && !incoming.engine.queueNoteOn(
            command.track,
            command.midi_note, command.output_note_token)) {
        reject(command.type);
    } else if (command.retrigger) {
        noteStarted(command.track, command.midi_note);
    }
    notify({
        .type = EngineNoticeType::ProgramActivated,
        .command = command.type,
        .generation = command.generation,
    });
}

bool RealtimeEngineHost::hasPendingProgramLoad() const noexcept {
    for (std::uint8_t index = 0; index < kProgramSlotCount; ++index) {
        if (programs_[index].state.load(std::memory_order_acquire)
            == ProgramSlotState::Pending) {
            return true;
        }
    }
    return false;
}

void RealtimeEngineHost::drainPendingCommands(
    std::span<float> /*interleaved_stereo*/,
    std::size_t max_waits,
    bool process_on_caller_thread) noexcept {
    if (process_on_caller_thread) {
        while (pendingCommandCount() > 0 || hasPendingProgramLoad()) {
            // A non-realtime output connection can hold the audio gate for a
            // complete RPC timeout. Leave commands queued instead of spinning
            // the caller/UI until that external operation finishes.
            if (outputSwitchPending()) return;
            servicePendingControlCommands();
        }
        return;
    }
    for (std::size_t pass = 0;
         pass < max_waits && hasPendingProgramLoad();
         ++pass) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool RealtimeEngineHost::waitForPendingProgramActivation(
    std::chrono::milliseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool load_rejected = false;
    for (;;) {
        EngineNotice notice{};
        while (pollNotice(notice)) {
            if (notice.type == EngineNoticeType::CommandRejected
                && notice.command == EngineCommandType::LoadProgram) {
                load_rejected = true;
            }
        }
        const bool pending = hasPendingProgramLoad();
        if (load_rejected || !pending
            || std::chrono::steady_clock::now() >= deadline) {
            if (load_rejected) {
                return false;
            }
            return !pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void RealtimeEngineHost::discardStuckProgramEdits() noexcept {
    for (std::uint8_t index = 0; index < kProgramSlotCount; ++index) {
        if (index == active_program_) {
            continue;
        }
        auto& slot = programs_[index];
        if (slot.state.load(std::memory_order_acquire)
            == ProgramSlotState::Editing) {
            slot.state.store(
                ProgramSlotState::Free,
                std::memory_order_release);
        }
    }
}

void RealtimeEngineHost::applyPendingCommands() noexcept {
    EngineCommand command{};
    while (commands_.tryPop(command)) {
        switch (command.type) {
        case EngineCommandType::SetScopeSelection:
            scope_ui_context_ = command.generation;
            selectScope(command.scope_track_mask, command.generation);
            break;
        case EngineCommandType::LoadProgram:
            loadProgram(command);
            break;
        case EngineCommandType::NoteOn:
            if (!programs_[active_program_].engine.queueNoteOn(
                    command.track,
                    command.midi_note, command.output_note_token)) {
                reject(command.type);
            } else {
                noteStarted(command.track, command.midi_note);
            }
            break;
        case EngineCommandType::NoteOff:
            if (!programs_[active_program_].engine.session().queueKeyOff(
                    command.track)) {
                reject(command.type);
            } else {
                noteStopped(command.track);
            }
            break;
        case EngineCommandType::SilenceTrack:
            if (!programs_[active_program_].engine.session().forceMuteTrack(
                    command.track)) {
                reject(command.type);
            } else {
                noteStopped(command.track);
            }
            break;
        case EngineCommandType::Stop:
        case EngineCommandType::HardReset:
            programs_[active_program_].engine.hardReset();
            ++spectrum_pcm_epoch_;
            allNotesStopped();
            break;
        case EngineCommandType::SetMixerGains:
            if (!programs_[active_program_].engine.setGains(
                    command.gains)) {
                reject(command.type);
            } else {
                current_gains_ = command.gains;
            }
            break;
        }
    }
}

void RealtimeEngineHost::servicePendingControlCommands() noexcept {
    RealtimeAccess access(*this);
    if (!access) return;
    applyRealtimePolicy(programs_[active_program_].engine);
    applyPendingCommands();
}

bool RealtimeEngineHost::realtimeNoteOn(
    std::uint8_t track,
    std::uint8_t midi_note) noexcept {
    RealtimeAccess access(*this);
    if (!access) return false;
    if (!programs_[active_program_].engine.queueNoteOn(
            track, midi_note)) {
        return false;
    }
    noteStarted(track, midi_note);
    return true;
}

bool RealtimeEngineHost::realtimeNoteOff(std::uint8_t track) noexcept {
    RealtimeAccess access(*this);
    if (!access) return false;
    if (!programs_[active_program_].engine.session().queueKeyOff(track)) {
        return false;
    }
    noteStopped(track);
    return true;
}

bool RealtimeEngineHost::realtimeSilenceTrack(std::uint8_t track) noexcept {
    RealtimeAccess access(*this);
    if (!access) return false;
    if (!programs_[active_program_].engine.session().forceMuteTrack(track)) {
        return false;
    }
    noteStopped(track);
    return true;
}

void RealtimeEngineHost::realtimeStop() noexcept {
    RealtimeAccess access(*this);
    if (!access) return;
    applyRealtimePolicy(programs_[active_program_].engine);
    programs_[active_program_].engine.hardReset();
    ++spectrum_pcm_epoch_;
    allNotesStopped();
}

RenderResult RealtimeEngineHost::render(
    std::span<float> interleaved_stereo) noexcept {
    servicePendingControlCommands();
#if defined(MGSTC_HOST_RENDER_GAP_TESTING)
    ::mgstcHostRenderGapTestHook();
#endif
    return renderAudio(interleaved_stereo);
}

RenderResult RealtimeEngineHost::renderAudio(
    std::span<float> interleaved_stereo) noexcept {
    RealtimeAccess access(*this);
    if (!access) {
        std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0.0F);
        return {.frames = interleaved_stereo.size() / 2};
    }
    auto& core = programs_[active_program_].engine;
    applyRealtimePolicy(core);
    const auto scope_request = opll_scope_request_.load(std::memory_order_acquire);
    const bool scope_enabled = (scope_request & 1U) != 0;
    if (applied_opll_scope_request_ != scope_request) {
        // A close/reopen between callbacks also starts a fresh 800-sample
        // window, even when the current enabled bit has returned to true.
        core.resetScopeCapture();
        applied_opll_scope_request_ = scope_request;
    }
    core.setOpllScopeEnabled(scope_enabled);
    SpectrumCapture* capture = nullptr;
    if (spectrum_enabled_.load(std::memory_order_acquire)) {
        spectrum_capture_->begin(spectrum_sample_clock_,
            spectrum_context_.load(std::memory_order_acquire), spectrum_pcm_epoch_,
            core.spectrumChannelMap(),
            currentGuideNote(),
            spectrum_output_gain_.load(std::memory_order_acquire),
            core.outputKind() != SoundOutputKind::Emulator,
            spectrum_channels_enabled_.load(std::memory_order_acquire));
        capture = spectrum_capture_.get();
    }
    RenderResult result{};
    // Preserve every completed window even when a host callback spans several
    // ticks. Commands and capture context are serviced once for the whole call.
    const bool split_scope = scope_enabled
        && interleaved_stereo.size() > OpllScopeFrame::kSampleCount * 2
        && interleaved_stereo.size() % 2 == 0;
    std::size_t offset = 0;
    do {
        const auto remaining = interleaved_stereo.size() - offset;
        const auto run = split_scope ? std::min(remaining, OpllScopeFrame::kSampleCount * 2) : remaining;
        const auto tick_before = core.session().tick();
        const auto part = core.render(interleaved_stereo.subspan(offset, run), capture);
        result.frames += part.frames;
        result.clipped = result.clipped || part.clipped;
        if (!part.ok() || core.session().tick() != tick_before)
            result.tick_result = part.tick_result;
        if (!part.ok()) result.error = part.error;
        offset += run;
        if (scope_enabled) {
            auto& scope = *scope_frame_work_;
            if (programs_[active_program_].engine.takeOpllScopeFrame(scope)
                && opll_scope_request_.load(std::memory_order_acquire) == scope_request) {
                scope.capture_generation = scope_request >> 1U;
                annotateGuideNote(scope);
                scope.scope_ui_context = scope_ui_context_;
                scope.selection_note = scope_selection_note_;
                scope.layer_tracks = scope_layer_tracks_;
                scope.program_generation = scope_program_epoch_;
                for (std::size_t track = 0; track < active_guide_notes_.size(); ++track)
                    scope.track_note_generation[track] = active_guide_notes_[track].order;
                if (!opll_scope_frames_->tryPush(scope))
                    scope_dropped_frames_.fetch_add(1, std::memory_order_relaxed);
                if (spectrogram_capture_enabled_.load(std::memory_order_acquire)) {
                    static_cast<void>(spectrogram_scope_frames_->tryPush(scope));
                }
            }
        }
        if (!part.ok()) {
            std::fill(interleaved_stereo.begin() + static_cast<std::ptrdiff_t>(offset),
                interleaved_stereo.end(), 0.0F);
            break;
        }
    } while (offset < interleaved_stereo.size());
    spectrum_sample_clock_ += result.frames;
    if (!result.ok()) {
        notify({
            .type = EngineNoticeType::RenderFailure,
            .render_error = result.error,
        });
    }

    if (result.clipped && !clipping_) {
        clipping_ = true;
        notify({.type = EngineNoticeType::ClipStarted});
    } else if (!result.clipped && clipping_) {
        clipping_ = false;
        notify({.type = EngineNoticeType::ClipEnded});
    }
    return result;
}

}  // namespace mgstc::engine
