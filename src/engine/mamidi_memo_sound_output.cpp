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

    setError({});
    producer_error_.store(ProducerError::None, std::memory_order_release);
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
    emission_enabled_.store(true, std::memory_order_release);
    startWorker();
    open_.store(true, std::memory_order_release);
    connection_state_.store(MAmidiConnectionState::Connected, std::memory_order_release);
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
    return enqueueChipAccess(*mapped);
}

bool MAmidiMemoSoundOutput::writeRegisters(
    std::span<const RegisterWrite> writes) {
    if (!isOpen()) {
        producer_error_.store(ProducerError::Closed, std::memory_order_relaxed);
        return false;
    }

    for (const auto& write : writes) {
        if (!writeRegister(write)) {
            return false;
        }
    }
    return true;
}

void MAmidiMemoSoundOutput::reset() {
    allNotesOff();
}

void MAmidiMemoSoundOutput::allNotesOff() {
    if (!isOpen()) {
        return;
    }
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
    if (!queue_.tryPush({access, emission_generation_.load(std::memory_order_acquire)})) {
        dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        producer_error_.store(ProducerError::Full, std::memory_order_relaxed);
        return false;
    }
    submitted_writes_.fetch_add(1, std::memory_order_release);
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
    const auto error = lastError();
    switch (connectionState()) {
    case MAmidiConnectionState::Connected:
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
        setError(client_.lastError());
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
        static_cast<void>(enqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueOpllSilence() {
    MAmidiChipAccess silence[kMAmidiOpllSilenceWriteCount]{};
    fillOpllSilenceWrites(silence, unit_no_);
    for (const auto& access : silence) {
        static_cast<void>(enqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueSccSilence() {
    MAmidiChipAccess silence[kMAmidiSccSilenceWriteCount]{};
    fillSccSilenceWrites(silence, unit_no_, scc_plus_);
    for (const auto& access : silence) {
        static_cast<void>(enqueueChipAccess(access));
    }
}

void MAmidiMemoSoundOutput::enqueueSilence() {
    enqueuePsgSilence();
    enqueueOpllSilence();
    enqueueSccSilence();
}

}  // namespace mgstc::engine
