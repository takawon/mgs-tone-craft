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

    setError({});
    if (!client_.connect(host_, port_)) {
        setError(client_.lastError());
        client_.disconnect();
        open_.store(false, std::memory_order_release);
        return false;
    }

    clearQueue(false);
    dropped_writes_.store(0, std::memory_order_relaxed);
    startWorker();
    open_.store(true, std::memory_order_release);
    return true;
}

void MAmidiMemoSoundOutput::close() {
    if (isOpen()) {
        enqueueSilence();
        static_cast<void>(waitForIdle(100));
    }
    open_.store(false, std::memory_order_release);
    stopWorker();
    clearQueue(true);
    client_.disconnect();
}

bool MAmidiMemoSoundOutput::isOpen() const {
    return open_.load(std::memory_order_acquire);
}

bool MAmidiMemoSoundOutput::writeRegister(const RegisterWrite& write) {
    if (!isOpen()) {
        setError("MAmidiMEmo output is not open");
        return false;
    }

    const auto mapped = mapRegisterWriteToMAmidi(
        write,
        MAmidiMapOptions{
            .unit_no = unit_no_,
            .scc_plus = scc_plus_,
        });
    if (!mapped.has_value()) {
        setError(std::string(mapRegisterWriteError(write)));
        return false;
    }
    return enqueueChipAccess(*mapped);
}

bool MAmidiMemoSoundOutput::writeRegisters(
    std::span<const RegisterWrite> writes) {
    if (!isOpen()) {
        setError("MAmidiMEmo output is not open");
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
    enqueueSilence();
}

SoundOutputKind MAmidiMemoSoundOutput::kind() const {
    return SoundOutputKind::MAmidiMemo;
}

bool MAmidiMemoSoundOutput::enqueueChipAccess(MAmidiChipAccess access) {
    if (!isOpen()) {
        setError("MAmidiMEmo output is not open");
        return false;
    }
    if (!queue_.tryPush(access)) {
        dropped_writes_.fetch_add(1, std::memory_order_relaxed);
        setError("MAmidiMEmo send queue is full");
        return false;
    }
    wake_cv_.notify_one();
    return true;
}

bool MAmidiMemoSoundOutput::waitForIdle(
    std::uint32_t timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (queue_.approximateSize() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Give the worker a beat to finish the last async_call post.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    return last_error_;
}

MAmidiConnectionState MAmidiMemoSoundOutput::connectionState()
    const noexcept {
    if (!isOpen()) {
        return client_.state() == MAmidiConnectionState::Faulted
            ? MAmidiConnectionState::Faulted
            : MAmidiConnectionState::Disconnected;
    }
    return client_.state();
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
    MAmidiChipAccess access{};
    while (!worker_stop_.load(std::memory_order_acquire)) {
        if (!queue_.tryPop(access)) {
            std::unique_lock lock(wake_mutex_);
            wake_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return worker_stop_.load(std::memory_order_acquire)
                    || queue_.approximateSize() > 0;
            });
            continue;
        }

        if (!client_.asyncWrite(access)) {
            setError(client_.lastError());
            open_.store(false, std::memory_order_release);
            // Discard remaining queued writes on fault (no emu fallback).
            clearQueue(true);
            break;
        }
    }
}

void MAmidiMemoSoundOutput::clearQueue(bool count_as_dropped) noexcept {
    MAmidiChipAccess discarded{};
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
