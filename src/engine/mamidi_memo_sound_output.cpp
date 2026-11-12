#include "mgstc/engine/mamidi_memo_sound_output.hpp"

#include <chrono>
#include <utility>

#include "mgstc/engine/mamidi_register_map.hpp"

namespace mgstc::engine {

MAmidiMemoSoundOutput::MAmidiMemoSoundOutput() = default;

MAmidiMemoSoundOutput::~MAmidiMemoSoundOutput() {
    close();
}

void MAmidiMemoSoundOutput::setEndpoint(
    std::string host,
    std::uint16_t port) {
    if (isOpen()) {
        return;
    }
    host_ = std::move(host);
    port_ = port;
}

void MAmidiMemoSoundOutput::setUnitNo(std::uint8_t unit_no) noexcept {
    if (isOpen()) {
        return;
    }
    unit_no_ = unit_no;
}

void MAmidiMemoSoundOutput::setSccPlus(bool enabled) noexcept {
    if (isOpen()) {
        return;
    }
    scc_plus_ = enabled;
}

bool MAmidiMemoSoundOutput::open() {
    if (isOpen()) {
        return true;
    }
    // A faulted worker may have exited while its std::thread is still joinable.
    stopWorker();
    const bool quarantine = recovery_required_.load(std::memory_order_acquire)
        || restarted_tracks_ != kAllTracks;

    setError({});
    producer_error_.store(quarantine ? ProducerError::Full : ProducerError::None, std::memory_order_release);
    connection_state_.store(MAmidiConnectionState::Connecting, std::memory_order_release);
    if (!client_.connect(host_, port_)) {
        setError(client_.lastError());
        client_.disconnect();
        open_.store(false, std::memory_order_release);
        connection_state_.store(MAmidiConnectionState::Disconnected, std::memory_order_release);
        return false;
    }

    clearQueue(false);
    dropped_writes_.store(0, std::memory_order_relaxed);
    submitted_writes_.store(0, std::memory_order_relaxed);
    acknowledged_writes_.store(0, std::memory_order_relaxed);
    close_acknowledged_.store(true, std::memory_order_release);
    reset_requested_.store(0, std::memory_order_relaxed);
    reset_acknowledged_.store(0, std::memory_order_relaxed);
    recovery_required_.store(quarantine, std::memory_order_release);
    restarted_tracks_ = quarantine ? 0 : kAllTracks;
    pending_level_tracks_ = 0;
    queued_note_tokens_.fill(0);
    if (!quarantine) {
        desired_psg_ = {};
        desired_opll_ = {};
        desired_scc_ = {};
    }
    emission_enabled_.store(true, std::memory_order_release);
    startWorker();
    open_.store(true, std::memory_order_release);
    connection_state_.store(MAmidiConnectionState::Connected, std::memory_order_release);
    if (quarantine) allNotesOff();
    return true;
}

void MAmidiMemoSoundOutput::close() {
    if (isOpen()) {
        // The host excludes the realtime producer throughout close. Drain first
        // so a full queue cannot discard the terminal silence sequence.
        emission_enabled_.store(true, std::memory_order_release);
        bool acknowledged = waitForIdle(6000);
        if (acknowledged) {
            enqueueSilence();
            acknowledged = waitForIdle(6000);
        }
        close_acknowledged_.store(acknowledged, std::memory_order_release);
    } else if (connection_state_.load(std::memory_order_acquire)
               == MAmidiConnectionState::Faulted) {
        close_acknowledged_.store(false, std::memory_order_release);
    }
    open_.store(false, std::memory_order_release);
    stopWorker();
    clearQueue(true);
    client_.disconnect();
    connection_state_.store(MAmidiConnectionState::Disconnected, std::memory_order_release);
}

bool MAmidiMemoSoundOutput::isOpen() const {
    return open_.load(std::memory_order_acquire);
}

bool MAmidiMemoSoundOutput::writeRegister(const RegisterWrite& write) {
    if (!isOpen()) {
        producer_error_.store(ProducerError::Closed, std::memory_order_relaxed);
        return false;
    }

    const auto mapped = mapRegisterWriteToMAmidi(
        write,
        MAmidiMapOptions{
            .unit_no = unit_no_,
            .scc_plus = scc_plus_,
        });
    if (!mapped.has_value()) {
        producer_error_.store(ProducerError::InvalidWrite, std::memory_order_relaxed);
        return false;
    }
    const bool partial = !recovery_required_.load(std::memory_order_acquire)
        && restarted_tracks_ != kAllTracks;
    if (partial && (write.track >= queued_note_tokens_.size()
        || (restarted_tracks_ & (1U << write.track)) == 0)) return true;
    rememberDesired(*mapped);
    if (partial && (write.reason == WriteReason::EnvelopeY || write.reason == WriteReason::RhythmControl)) {
        // A fresh voice may explicitly control another physical channel. Its
        // Y value is intentional; retired voices must not overwrite it later.
        if (write.chip == ChipId::Psg) {
            if (write.address >= 8 && write.address <= 10 && write.value != 0)
                pending_level_tracks_ |= 1U << (write.address - 8);
            if (write.address == 7)
                for (std::uint8_t channel = 0; channel < 3; ++channel)
                    if (((write.value >> channel) & 0x09U) != 0x09U)
                        pending_level_tracks_ |= 1U << channel;
        } else if (write.chip == ChipId::Opll) {
            if (write.address >= 0x20 && write.address <= 0x28 && (write.value & 0x10U) != 0)
                pending_level_tracks_ |= 1U << (write.address - 0x20 + 8);
            if (write.address == 0x0E) {
                if ((write.value & 0x10U) != 0) pending_level_tracks_ |= 1U << 14;
                if ((write.value & 0x09U) != 0) pending_level_tracks_ |= 1U << 15;
                if ((write.value & 0x06U) != 0) pending_level_tracks_ |= 1U << 16;
            }
        }
        return enqueueChipAccess(*mapped);
    }
    auto access = maskUnstartedTracks(*mapped, restarted_tracks_);
    if (partial && write.chip == ChipId::Psg && write.address == 7 && write.track < 3) {
        const auto bits = (1U << write.track) | (1U << (write.track + 3));
        access.data = (queued_psg_mixer_ & ~bits) | (mapped->data & bits);
        rememberDesired(access);
    } else if (partial && write.chip == ChipId::Scc && write.port == 3
        && write.track >= 3 && write.track < 8) {
        const auto bit = 1U << (write.track - 3);
        access.data = (queued_scc_keys_ & ~bit) | (mapped->data & bit);
        rememberDesired(access);
    }
    return enqueueChipAccess(access);
}

bool MAmidiMemoSoundOutput::writeRegisters(
    std::span<const RegisterWrite> writes) {
    if (!isOpen()) {
        producer_error_.store(ProducerError::Closed, std::memory_order_relaxed);
        return false;
    }

    bool delivered = true;
    for (const auto& write : writes) {
        if (!writeRegister(write)) {
            delivered = false;
            // Keep the complete desired image, including writes after the
            // first dropped one. Invalid input remains a hard error.
            if (!recoverableWriteFailure()) return false;
        }
    }
    return delivered && restoreFreshLevels();
}

void MAmidiMemoSoundOutput::reset() {
    desired_psg_ = {};
    desired_opll_ = {};
    desired_scc_ = {};
    restarted_tracks_ = recovery_required_.load(std::memory_order_acquire) ? 0 : kAllTracks;
    pending_level_tracks_ = 0;
    producer_error_.store(recovery_required_.load(std::memory_order_acquire)
        ? ProducerError::Full : ProducerError::None, std::memory_order_relaxed);
    allNotesOff();
}

void MAmidiMemoSoundOutput::rememberDesired(MAmidiChipAccess access) noexcept {
    const auto save = [&](auto& image, std::uint32_t address) {
        if (address < image.size()) image[address] = {
            static_cast<std::uint8_t>(access.data), true};
    };
    if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Psg))
        save(desired_psg_, access.address);
    else if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Opll))
        save(desired_opll_, access.address);
    else if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Scc))
        save(desired_scc_, access.address - (scc_plus_ ? kMAmidiSccPlusOffset : 0));
}

MAmidiChipAccess MAmidiMemoSoundOutput::maskUnstartedTracks(
    MAmidiChipAccess access, std::uint32_t tracks) const noexcept {
    if (tracks == kAllTracks) return access;
    const auto address = access.address;
    if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Psg)) {
        if (address >= 8 && address <= 10 && (tracks & (1U << (address - 8))) == 0)
            access.data = 0;
        if (address == 7) {
            const auto muted = (~tracks) & 7U;
            access.data |= muted | (muted << 3U);
        }
    } else if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Opll)) {
        if (address >= 0x20 && address <= 0x28
            && (tracks & (1U << (address - 0x20 + 8))) == 0) access.data &= ~0x10U;
        if (address >= 0x30 && address <= 0x38
            && (tracks & (1U << (address - 0x30 + 8))) == 0) access.data |= 0x0FU;
        if (address == 0x0E) {
            std::uint32_t rhythm_keys = 0;
            if ((tracks & (1U << 14)) != 0) rhythm_keys |= 0x10;
            if ((tracks & (1U << 15)) != 0) rhythm_keys |= 0x09;
            if ((tracks & (1U << 16)) != 0) rhythm_keys |= 0x06;
            access.data &= 0xE0U | rhythm_keys;
        }
    } else if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Scc)) {
        const auto relative = address - (scc_plus_ ? kMAmidiSccPlusOffset : 0);
        if (relative >= kMAmidiSccVolumeBase && relative < kMAmidiSccKeyAddress
            && (tracks & (1U << (relative - kMAmidiSccVolumeBase + 3))) == 0)
            access.data = 0;
        if (relative == kMAmidiSccKeyAddress) access.data &= (tracks >> 3U) & 0x1FU;
    }
    return access;
}

bool MAmidiMemoSoundOutput::restoreMutedImage() noexcept {
    const auto restore = [&](const auto& image, MAmidiDeviceId chip, std::uint32_t offset) {
        for (std::size_t address = 0; address < image.size(); ++address) {
            if (!image[address].present) continue;
            const MAmidiChipAccess desired{static_cast<std::uint8_t>(chip), unit_no_,
                static_cast<std::uint32_t>(address) + offset, image[address].value};
            if (!tryEnqueueChipAccess(maskUnstartedTracks(desired, 0))) return false;
        }
        return true;
    };
    return restore(desired_psg_, MAmidiDeviceId::Psg, 0)
        && restore(desired_opll_, MAmidiDeviceId::Opll, 0)
        && restore(desired_scc_, MAmidiDeviceId::Scc, scc_plus_ ? kMAmidiSccPlusOffset : 0);
}

bool MAmidiMemoSoundOutput::restoreFreshLevels() noexcept {
    if (pending_level_tracks_ == 0) return true;
    const auto send = [&](const auto& image, MAmidiDeviceId chip, std::uint32_t address,
                          std::uint32_t offset = 0) {
        if (!image[address].present) return true;
        return tryEnqueueChipAccess(maskUnstartedTracks({static_cast<std::uint8_t>(chip), unit_no_,
            address + offset, image[address].value}, restarted_tracks_ | pending_level_tracks_));
    };
    // Normal NoteOn usually writes these values itself. Complete a recovered
    // NoteOn that emits only pitch/key without replaying any previous key-on.
    if ((pending_level_tracks_ & 7U) != 0 && desired_psg_[7].present) {
        const auto channels = pending_level_tracks_ & 7U;
        const auto bits = channels | (channels << 3U);
        const auto mixer = (queued_psg_mixer_ & ~bits) | (desired_psg_[7].value & bits);
        if (!tryEnqueueChipAccess({static_cast<std::uint8_t>(MAmidiDeviceId::Psg), unit_no_, 7, mixer})) return false;
    }
    for (std::uint32_t track = 0; track < 17; ++track) {
        if ((pending_level_tracks_ & (1U << track)) == 0) continue;
        if (track < 3) {
            if (!send(desired_psg_, MAmidiDeviceId::Psg, 8 + track)) return false;
        } else if (track < 8) {
            if (!send(desired_scc_, MAmidiDeviceId::Scc, kMAmidiSccVolumeBase + track - 3,
                scc_plus_ ? kMAmidiSccPlusOffset : 0)) return false;
        } else if (!send(desired_opll_, MAmidiDeviceId::Opll, 0x30 + track - 8)) return false;
    }
    pending_level_tracks_ = 0;
    return true;
}

void MAmidiMemoSoundOutput::beginTick(std::uint32_t new_note_tracks) noexcept {
    if (!isOpen() || !emission_enabled_.load(std::memory_order_acquire)) return;
    if (offline_silence_requested_.exchange(false, std::memory_order_acq_rel)
        && (recovery_required_.load(std::memory_order_acquire) || restarted_tracks_ != kAllTracks)) {
        recovery_required_.store(true, std::memory_order_release);
        producer_error_.store(ProducerError::Full, std::memory_order_relaxed);
        restarted_tracks_ = 0;
        pending_level_tracks_ = 0;
        allNotesOff();
        return;
    }
    new_note_tracks &= kAllTracks;
    if (recovery_required_.load(std::memory_order_acquire) || restarted_tracks_ != kAllTracks) {
        const auto token = captureNoteToken();
        for (std::uint8_t track = 0; track < queued_note_tokens_.size(); ++track)
            if (token == 0 || queued_note_tokens_[track] != token) new_note_tracks &= ~(1U << track);
    }
    if (recovery_required_.load(std::memory_order_acquire)) {
        if (new_note_tracks == 0
            || reset_acknowledged_.load(std::memory_order_acquire)
                < reset_requested_.load(std::memory_order_acquire)) return;
        // A partly queued image is invalidated by tryEnqueueChipAccess if
        // congestion recurs. Never admit its key-ons before a new silence ack.
        if (!restoreMutedImage()) return;
        restarted_tracks_ = new_note_tracks;
        pending_level_tracks_ = new_note_tracks;
        recovery_required_.store(false, std::memory_order_release);
        if (producer_error_.load(std::memory_order_relaxed) == ProducerError::Full)
            producer_error_.store(ProducerError::None, std::memory_order_relaxed);
    } else {
        pending_level_tracks_ |= new_note_tracks & ~restarted_tracks_;
        restarted_tracks_ |= new_note_tracks;
    }
}

std::uint64_t MAmidiMemoSoundOutput::captureNoteToken() const noexcept {
    if (!isOpen() || !emission_enabled_.load(std::memory_order_acquire)
        || offline_silence_requested_.load(std::memory_order_acquire)) return 0;
    const auto generation = emission_generation_.load(std::memory_order_acquire);
    // Also compare emission generation: a reset increments it before publishing
    // reset_requested_, and that publication gap must never look ready.
    if (reset_requested_.load(std::memory_order_acquire) != generation
        || reset_acknowledged_.load(std::memory_order_acquire) != generation
        || emission_generation_.load(std::memory_order_acquire) != generation) return 0;
    return generation + 1;
}

void MAmidiMemoSoundOutput::noteQueued(std::uint8_t track, std::uint64_t token) noexcept {
    if (track < queued_note_tokens_.size()) queued_note_tokens_[track] = token;
}

bool MAmidiMemoSoundOutput::recoverableWriteFailure() const noexcept {
    return isOpen()
        && producer_error_.load(std::memory_order_relaxed) == ProducerError::Full;
}

void MAmidiMemoSoundOutput::setEmissionEnabled(bool enabled) noexcept {
    const bool previous = emission_enabled_.exchange(enabled, std::memory_order_acq_rel);
    if (previous && !enabled) emission_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!previous && enabled) {
        // A silence request skipped during offline output is not proof that
        // the physical device is silent. Require a fresh realtime barrier.
        offline_silence_requested_.store(true, std::memory_order_release);
    }
}

void MAmidiMemoSoundOutput::allNotesOff() {
    if (!isOpen()) {
        return;
    }
    queued_psg_mixer_ = kMAmidiPsgMixerMask;
    queued_scc_keys_ = 0;
    // Capacity-independent control lane. The worker invalidates older queued
    // writes, acknowledges any already-posted batch, then sends full silence
    // before admitting the new generation. The realtime caller never waits.
    const auto generation = emission_generation_.fetch_add(1,
        std::memory_order_acq_rel) + 1;
    reset_requested_.store(generation, std::memory_order_release);
    wake_cv_.notify_one();
}

SoundOutputKind MAmidiMemoSoundOutput::kind() const {
    return SoundOutputKind::MAmidiMemo;
}

bool MAmidiMemoSoundOutput::enqueueChipAccess(MAmidiChipAccess access) {
    if (!emission_enabled_.load(std::memory_order_acquire)) return true;
    if (!isOpen()) {
        producer_error_.store(ProducerError::Closed, std::memory_order_relaxed);
        return false;
    }
    if (recovery_required_.load(std::memory_order_acquire)) {
        dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return tryEnqueueChipAccess(access);
}

bool MAmidiMemoSoundOutput::tryEnqueueChipAccess(MAmidiChipAccess access) {
    if (!queue_.tryPush({access, emission_generation_.load(std::memory_order_acquire)})) {
        dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        producer_error_.store(ProducerError::Full, std::memory_order_relaxed);
        recovery_required_.store(true, std::memory_order_release);
        restarted_tracks_ = 0;
        pending_level_tracks_ = 0;
        allNotesOff();
        return false;
    }
    submitted_writes_.fetch_add(1, std::memory_order_release);
    if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Psg) && access.address == 7)
        queued_psg_mixer_ = access.data;
    if (access.device_id == static_cast<std::uint8_t>(MAmidiDeviceId::Scc)
        && access.address == kMAmidiSccKeyAddress + (scc_plus_ ? kMAmidiSccPlusOffset : 0))
        queued_scc_keys_ = access.data;
    wake_cv_.notify_one();
    return true;
}

bool MAmidiMemoSoundOutput::waitForIdle(
    std::uint32_t timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    const auto submitted = submitted_writes_.load(std::memory_order_acquire);
    const auto reset = reset_requested_.load(std::memory_order_acquire);
    while (acknowledged_writes_.load(std::memory_order_acquire) < submitted
        || reset_acknowledged_.load(std::memory_order_acquire) < reset) {
        if (!isOpen()) return false;
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

const std::string& MAmidiMemoSoundOutput::host() const noexcept {
    return host_;
}

std::uint16_t MAmidiMemoSoundOutput::port() const noexcept {
    return port_;
}

std::uint8_t MAmidiMemoSoundOutput::unitNo() const noexcept {
    return unit_no_;
}

bool MAmidiMemoSoundOutput::sccPlus() const noexcept {
    return scc_plus_;
}

std::string MAmidiMemoSoundOutput::lastError() const {
    std::lock_guard lock(error_mutex_);
    return lastErrorProtected();
}

std::string MAmidiMemoSoundOutput::lastErrorProtected() const {
    if (!last_error_.empty()) return last_error_;
    switch (producer_error_.load(std::memory_order_relaxed)) {
    case ProducerError::Closed: return "MAmidiMEmo output is not open";
    case ProducerError::Full: return "MAmidiMEmo send queue is full";
    case ProducerError::InvalidWrite: return "Invalid MAmidiMEmo register write";
    default: return {};
    }
}

MAmidiConnectionState MAmidiMemoSoundOutput::connectionState()
    const noexcept {
    return connection_state_.load(std::memory_order_acquire);
}

std::uint64_t MAmidiMemoSoundOutput::droppedWrites() const noexcept {
    return dropped_writes_.load(std::memory_order_relaxed);
}

std::string MAmidiMemoSoundOutput::statusText() const {
    return statusTextForError(lastError());
}

bool MAmidiMemoSoundOutput::tryStatusText(
    std::string& status, std::string& error) const {
    std::unique_lock lock(error_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto copied_error = lastErrorProtected();
    auto copied_status = statusTextForError(copied_error);
    error = std::move(copied_error);
    status = std::move(copied_status);
    return true;
}

std::string MAmidiMemoSoundOutput::statusTextForError(const std::string& error) const {
    switch (connectionState()) {
    case MAmidiConnectionState::Connected:
        if (producer_error_.load(std::memory_order_relaxed) == ProducerError::InvalidWrite)
            return "MAmidiMEmo connected: Invalid MAmidiMEmo register write";
        if (recovery_required_.load(std::memory_order_acquire)) {
            const bool silenced = reset_acknowledged_.load(std::memory_order_acquire)
                >= reset_requested_.load(std::memory_order_acquire);
            return silenced ? "MAmidiMEmo send queue is full; waiting for next Note On"
                : "MAmidiMEmo send queue is full; silencing for resynchronization";
        }
        if (!error.empty()) return "MAmidiMEmo connected: " + error;
        return "MAmidiMEmo connected (" + host_ + ":"
            + std::to_string(port_) + ")";
    case MAmidiConnectionState::Connecting:
        return "MAmidiMEmo connecting...";
    case MAmidiConnectionState::Faulted:
        return error.empty()
            ? std::string("MAmidiMEmo faulted")
            : ("MAmidiMEmo faulted: " + error);
    case MAmidiConnectionState::Disconnected:
    default:
        return error.empty()
            ? std::string("MAmidiMEmo disconnected")
            : ("MAmidiMEmo disconnected: " + error);
    }
}

void MAmidiMemoSoundOutput::startWorker() {
    worker_stop_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { workerLoop(); });
}

void MAmidiMemoSoundOutput::stopWorker() noexcept {
    worker_stop_.store(true, std::memory_order_release);
    wake_cv_.notify_all();
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
        }
    }
}

void MAmidiMemoSoundOutput::workerLoop() {
    QueuedWrite write{};
    std::uint64_t pending_count = 0;
    std::size_t posted_in_batch = 0;
    auto acknowledge = [&] {
        if (pending_count == 0) return true;
        if (!client_.flushWrites()) return false;
        acknowledged_writes_.fetch_add(pending_count, std::memory_order_release);
        pending_count = 0;
        posted_in_batch = 0;
        return true;
    };
    auto fail = [&] {
        setError(recovery_required_.load(std::memory_order_acquire)
            ? "Hardware resynchronization/silence failed: " + client_.lastError()
            : client_.lastError());
        connection_state_.store(MAmidiConnectionState::Faulted, std::memory_order_release);
        open_.store(false, std::memory_order_release);
        clearQueue(true);
    };
    auto service_reset = [&] {
        const auto requested = reset_requested_.load(std::memory_order_acquire);
        if (requested <= reset_acknowledged_.load(std::memory_order_acquire)) return true;
        if (!acknowledge()) return false;
        auto send = [&](const auto& writes) {
            for (const auto& access : writes) {
                if (!emission_enabled_.load(std::memory_order_acquire)
                    || requested != emission_generation_.load(std::memory_order_acquire)) break;
                if (!client_.asyncWrite(access)) return false;
            }
            return true;
        };
        MAmidiChipAccess psg[kMAmidiPsgSilenceWriteCount]{};
        MAmidiChipAccess opll[kMAmidiOpllSilenceWriteCount]{};
        MAmidiChipAccess scc[kMAmidiSccSilenceWriteCount]{};
        fillPsgSilenceWrites(psg, unit_no_);
        fillOpllSilenceWrites(opll, unit_no_);
        fillSccSilenceWrites(scc, unit_no_, scc_plus_);
        if (!send(psg) || !send(opll) || !send(scc) || !client_.flushWrites()) return false;
        reset_acknowledged_.store(requested, std::memory_order_release);
        return true;
    };
    while (!worker_stop_.load(std::memory_order_acquire)) {
        if (!service_reset()) { fail(); break; }
        if (!queue_.tryPop(write)) {
            if (!acknowledge()) { fail(); break; }
            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return worker_stop_.load(std::memory_order_acquire)
                    || queue_.approximateSize() > 0
                    || reset_requested_.load(std::memory_order_acquire)
                        > reset_acknowledged_.load(std::memory_order_acquire);
            });
            continue;
        }
        // Revalidate after every acknowledgement wait: offline or reset may
        // have invalidated this popped command while the server was blocked.
        if (posted_in_batch >= 128 && !acknowledge()) { fail(); break; }
        if (!service_reset()) { fail(); break; }
        ++pending_count;
        if (!emission_enabled_.load(std::memory_order_acquire)
            || write.generation != emission_generation_.load(std::memory_order_acquire)) {
            dropped_writes_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!client_.asyncWrite(write.access)) { fail(); break; }
        ++posted_in_batch;
    }
}

void MAmidiMemoSoundOutput::clearQueue(bool count_as_dropped) noexcept {
    QueuedWrite discarded{};
    while (queue_.tryPop(discarded)) {
        if (count_as_dropped) {
            dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MAmidiMemoSoundOutput::setError(std::string message) {
    std::lock_guard lock(error_mutex_);
    last_error_ = std::move(message);
}

void MAmidiMemoSoundOutput::enqueuePsgSilence() {
    MAmidiChipAccess silence[kMAmidiPsgSilenceWriteCount]{};
    fillPsgSilenceWrites(silence, unit_no_);
    for (const auto& access : silence) {
        static_cast<void>(tryEnqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueOpllSilence() {
    MAmidiChipAccess silence[kMAmidiOpllSilenceWriteCount]{};
    fillOpllSilenceWrites(silence, unit_no_);
    for (const auto& access : silence) {
        static_cast<void>(tryEnqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueSccSilence() {
    MAmidiChipAccess silence[kMAmidiSccSilenceWriteCount]{};
    fillSccSilenceWrites(silence, unit_no_, scc_plus_);
    for (const auto& access : silence) {
        static_cast<void>(tryEnqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueSilence() {
    enqueuePsgSilence();
    enqueueOpllSilence();
    enqueueSccSilence();
}

}  // namespace mgstc::engine
