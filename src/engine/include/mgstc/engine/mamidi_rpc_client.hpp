#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace mgstc::engine {

// Thin wrapper around rpclib for MAmidiMEmo `-chip_server`.
// Spec: void DirectAccessToChip(DeviceID, UnitNo, address, data).
enum class MAmidiConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Faulted,
};

struct MAmidiChipAccess {
    std::uint8_t device_id{};
    std::uint8_t unit_no{};
    std::uint32_t address{};
    std::uint32_t data{};
};

class MAmidiRpcClient {
public:
    static constexpr const char* kMethodName = "DirectAccessToChip";
    static constexpr std::int64_t kDefaultTimeoutMs = 5000;

    MAmidiRpcClient();
    ~MAmidiRpcClient();

    MAmidiRpcClient(const MAmidiRpcClient&) = delete;
    MAmidiRpcClient& operator=(const MAmidiRpcClient&) = delete;
    MAmidiRpcClient(MAmidiRpcClient&&) noexcept;
    MAmidiRpcClient& operator=(MAmidiRpcClient&&) noexcept;

    // Connects, then probes with a sync DirectAccessToChip(0,0,0,0).
    // Returns false on connect/probe failure; lastError() explains why.
    [[nodiscard]] bool connect(
        const std::string& host,
        std::uint16_t port,
        std::int64_t timeout_ms = kDefaultTimeoutMs);

    void disconnect() noexcept;

    // Fire-and-forget write. Safe only while Connected.
    // Does not block on the server response.
    [[nodiscard]] bool asyncWrite(const MAmidiChipAccess& access);

    [[nodiscard]] MAmidiConnectionState state() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;
    [[nodiscard]] const std::string& host() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mgstc::engine
