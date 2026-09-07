#include "mgstc/engine/realtime_engine_host.hpp"

#include <type_traits>
#include <utility>

namespace mgstc::engine {

static_assert(std::is_nothrow_move_assignable_v<EngineCore>);

RealtimeEngineHost::RealtimeEngineHost()
    : opll_scope_frames_(
          std::make_unique<
              SpscQueue<OpllScopeFrame, kOpllScopeCapacity>>()),
      spectrogram_scope_frames_(
          std::make_unique<SpscQueue<
              OpllScopeFrame, kSpectrogramScopeCapacity>>()),
      programs_(std::make_unique<ProgramSlot[]>(kProgramSlotCount)),
      mamidi_(std::make_unique<MAmidiMemoSoundOutput>()) {
    programs_[0].state.store(
        ProgramSlotState::Active,
        std::memory_order_relaxed);
    applyOutputRouting(programs_[0].engine);
}

RealtimeEngineHost::~RealtimeEngineHost() {
    output_kind_ = SoundOutputKind::Emulator;
    applyOutputRouting();
    if (mamidi_ != nullptr) {
        mamidi_->close();
    }
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
    const auto command = EngineCommand::loadProgram(
        edit.slot,
        edit.generation,
        audition.retrigger,
        audition.track,
        audition.midi_note);
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
    return commands_.tryPush(command);
}

void RealtimeEngineHost::setMAmidiSettings(
    MAmidiOutputSettings settings) {
    const bool endpoint_changed =
        settings.host != mamidi_settings_.host
        || settings.port != mamidi_settings_.port
        || settings.unit_no != mamidi_settings_.unit_no
        || settings.scc_plus != mamidi_settings_.scc_plus;
    mamidi_settings_ = std::move(settings);
    // Waveform monitor is local-only and must update without reconnect.
    applyOutputRouting();

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

bool RealtimeEngineHost::setSoundOutputKind(SoundOutputKind kind) {
    if (kind == SoundOutputKind::Emulator) {
        output_kind_ = SoundOutputKind::Emulator;
        applyOutputRouting();
        if (mamidi_ != nullptr) {
            mamidi_->close();
        }
        return true;
    }

    if (mamidi_ == nullptr) {
        return false;
    }
    mamidi_->setEndpoint(mamidi_settings_.host, mamidi_settings_.port);
    mamidi_->setUnitNo(mamidi_settings_.unit_no);
    mamidi_->setSccPlus(mamidi_settings_.scc_plus);
    if (!mamidi_->isOpen() && !mamidi_->open()) {
        // Selection stays on emulator until connect succeeds.
        return false;
    }
    output_kind_ = SoundOutputKind::MAmidiMemo;
    applyOutputRouting();
    return true;
}

SoundOutputKind RealtimeEngineHost::soundOutputKind() const noexcept {
    return output_kind_;
}

bool RealtimeEngineHost::reconnectMAmidi() {
    if (mamidi_ == nullptr) {
        return false;
    }
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
    return true;
}

std::string RealtimeEngineHost::soundOutputStatus() const {
    if (output_kind_ == SoundOutputKind::Emulator) {
        return "Output: Emulator";
    }
    if (mamidi_ == nullptr) {
        return "Output: MAmidiMEmo (unavailable)";
    }
    return mamidi_->statusText();
}

std::string RealtimeEngineHost::soundOutputLastError() const {
    if (mamidi_ == nullptr) {
        return {};
    }
    return mamidi_->lastError();
}

bool RealtimeEngineHost::pollNotice(EngineNotice& notice) noexcept {
    return notices_.tryPop(notice);
}

bool RealtimeEngineHost::pollOpllScope(
    OpllScopeFrame& frame) noexcept {
    return opll_scope_frames_->tryPop(frame);
}

bool RealtimeEngineHost::pollLatestOpllScope(
    OpllScopeFrame& frame) noexcept {
    bool found = false;
    OpllScopeFrame next{};
    for (std::size_t count = 0;
         count < kOpllScopeCapacity
         && opll_scope_frames_->tryPop(next);
         ++count) {
        frame = next;
        found = true;
    }
    return found;
}

void RealtimeEngineHost::setSpectrogramCaptureEnabled(
    bool enabled) noexcept {
    spectrogram_capture_enabled_.store(enabled, std::memory_order_release);
}

bool RealtimeEngineHost::pollSpectrogramScope(
    OpllScopeFrame& frame) noexcept {
    return spectrogram_scope_frames_->tryPop(frame);
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
}

void RealtimeEngineHost::noteStopped(std::uint8_t track) noexcept {
    if (track < active_guide_notes_.size()) {
        active_guide_notes_[track].active = false;
    }
}

void RealtimeEngineHost::allNotesStopped() noexcept {
    for (auto& note : active_guide_notes_) {
        note.active = false;
    }
}

void RealtimeEngineHost::annotateGuideNote(
    OpllScopeFrame& frame) const noexcept {
    const ActiveGuideNote* newest = nullptr;
    std::uint8_t newest_track = last_guide_track_;
    for (std::size_t index = 0; index < active_guide_notes_.size(); ++index) {
        const auto& note = active_guide_notes_[index];
        if (note.active && (newest == nullptr || note.order > newest->order)) {
            newest = &note;
            newest_track = static_cast<std::uint8_t>(index);
        }
    }
    frame.guide_note = newest != nullptr ? newest->note : last_guide_note_;
    frame.guide_track = newest_track;
    frame.note_active = newest != nullptr;
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

void RealtimeEngineHost::applyOutputRouting() noexcept {
    for (std::uint8_t index = 0; index < kProgramSlotCount; ++index) {
        applyOutputRouting(programs_[index].engine);
    }
}

void RealtimeEngineHost::applyOutputRouting(EngineCore& engine) noexcept {
    // Keep the MAmidi backend selected even while disconnected so writes
    // fail closed instead of silently falling back to the simulator.
    if (output_kind_ == SoundOutputKind::MAmidiMemo && mamidi_ != nullptr) {
        engine.setOutputBackend(mamidi_.get());
    } else {
        engine.setOutputBackend(nullptr);
    }
    engine.setWaveformMonitor(mamidi_settings_.waveform_monitor);
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
        reject(command.type);
        return;
    }

    static_cast<void>(incoming.engine.setGains(current_gains_));
    applyOutputRouting(incoming.engine);
    incoming.engine.hardReset();
    incoming.engine.session().gateUntilNoteOn();
    const auto previous = active_program_;
    active_program_ = command.program_slot;
    allNotesStopped();
    incoming.state.store(ProgramSlotState::Active, std::memory_order_release);
    programs_[previous].state.store(
        ProgramSlotState::Free,
        std::memory_order_release);

    if (command.retrigger
        && !incoming.engine.session().queueNoteOn(
            command.track,
            command.midi_note)) {
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

void RealtimeEngineHost::applyPendingCommands() noexcept {
    EngineCommand command{};
    while (commands_.tryPop(command)) {
        switch (command.type) {
        case EngineCommandType::LoadProgram:
            loadProgram(command);
            break;
        case EngineCommandType::NoteOn:
            if (!programs_[active_program_].engine.session().queueNoteOn(
                    command.track,
                    command.midi_note)) {
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

RenderResult RealtimeEngineHost::render(
    std::span<float> interleaved_stereo) noexcept {
    applyPendingCommands();
    const auto result =
        programs_[active_program_].engine.render(interleaved_stereo);
    OpllScopeFrame scope{};
    if (programs_[active_program_].engine.takeOpllScopeFrame(scope)) {
        annotateGuideNote(scope);
        static_cast<void>(opll_scope_frames_->tryPush(scope));
        if (spectrogram_capture_enabled_.load(std::memory_order_acquire)) {
            static_cast<void>(spectrogram_scope_frames_->tryPush(scope));
        }
    }
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
