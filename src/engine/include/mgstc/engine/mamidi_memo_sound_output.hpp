#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "mgstc/engine/mamidi_rpc_client.hpp"
#include "mgstc/engine/sound_output_backend.hpp"
#include "mgstc/engine/spsc_queue.hpp"

namespace mgstc::engine {

// Forwards RegisterWrite events to MAmidiMEmo via msgpack-RPC.
// Phase 4–6: PSG / OPLL / SCC (MAmidi absolute SCC addresses).
class MAmidiMemoSoundOutput final : public SoundOutputBackend {
public:
    static constexpr const char* kDefaultHost = "localhost";
    static constexpr std::uint16_t kDefaultPort = 30000;
    static constexpr std::size_t kQueueCapacity = 4096;

    MAmidiMemoSoundOutput();
    ~MAmidiMemoSoundOutput() override;

    MAmidiMemoSoundOutput(const MAmidiMemoSoundOutput&) = delete;
    MAmidiMemoSoundOutput& operator=(
        const MAmidiMemoSoundOutput&) = delete;

    void setEndpoint(std::string host, std::uint16_t port);
    void setUnitNo(std::uint8_t unit_no) noexcept;
    void setSccPlus(bool enabled) noexcept;

    bool open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] bool writeRegister(
        const RegisterWrite& write) override;
    [[nodiscard]] bool writeRegisters(
        std::span<const RegisterWrite> writes) override;

    void reset() override;
    void allNotesOff() override;

    [[nodiscard]] SoundOutputKind kind() const override;

    // Enqueue a raw DirectAccessToChip payload (for tests / advanced use).
    // Non-blocking; returns false if not open or the queue is full.
    [[nodiscard]] bool enqueueChipAccess(MAmidiChipAccess access);

    // Wait until the send queue is drained or timeout_ms elapses.
    [[nodiscard]] bool waitForIdle(std::uint32_t timeout_ms) const;

    [[nodiscard]] const std::string& host() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::uint8_t unitNo() const noexcept;
    [[nodiscard]] bool sccPlus() const noexcept;
    [[nodiscard]] std::string lastError() const;
    [[nodiscard]] MAmidiConnectionState connectionState() const noexcept;
    [[nodiscard]] std::uint64_t droppedWrites() const noexcept;
    [[nodiscard]] std::string statusText() const;

private:
    void startWorker();
    void stopWorker() noexcept;
    void workerLoop();
    void clearQueue(bool count_as_dropped) noexcept;
    void setError(std::string message);
    void enqueuePsgSilence();
    void enqueueOpllSilence();
    void enqueueSccSilence();
    void enqueueSilence();

    std::string host_{kDefaultHost};
    std::uint16_t port_{kDefaultPort};
    std::uint8_t unit_no_{0};
    bool scc_plus_{false};
    std::string last_error_{};
    MAmidiRpcClient client_{};
    SpscQueue<MAmidiChipAccess, kQueueCapacity> queue_{};
    std::thread worker_{};
    std::mutex wake_mutex_{};
    std::condition_variable wake_cv_{};
    std::atomic<bool> open_{false};
    std::atomic<bool> worker_stop_{true};
    std::atomic<std::uint64_t> dropped_writes_{0};
    mutable std::mutex error_mutex_{};
};

}  // namespace mgstc::engine
