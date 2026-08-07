#include "mgstc/engine/mamidi_rpc_client.hpp"

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

#include "rpc/client.h"
#include "rpc/rpc_error.h"

namespace mgstc::engine {
namespace {

constexpr auto kConnectPollInterval = std::chrono::milliseconds(20);
constexpr auto kConnectWait = std::chrono::milliseconds(2000);

}  // namespace

struct MAmidiRpcClient::Impl {
    std::unique_ptr<rpc::client> client{};
    MAmidiConnectionState state{MAmidiConnectionState::Disconnected};
    std::string last_error{};
    std::string host{};
    std::uint16_t port{};
};

MAmidiRpcClient::MAmidiRpcClient()
    : impl_(std::make_unique<Impl>()) {}

MAmidiRpcClient::~MAmidiRpcClient() {
    disconnect();
}

MAmidiRpcClient::MAmidiRpcClient(MAmidiRpcClient&&) noexcept = default;

MAmidiRpcClient& MAmidiRpcClient::operator=(
    MAmidiRpcClient&&) noexcept = default;

bool MAmidiRpcClient::connect(
    const std::string& host,
    std::uint16_t port,
    std::int64_t timeout_ms) {
    disconnect();
    impl_->host = host;
    impl_->port = port;
    impl_->state = MAmidiConnectionState::Connecting;
    impl_->last_error.clear();

    try {
        impl_->client = std::make_unique<rpc::client>(host, port);
        impl_->client->set_timeout(timeout_ms);

        const auto deadline =
            std::chrono::steady_clock::now() + kConnectWait;
        while (impl_->client->get_connection_state()
                   != rpc::client::connection_state::connected
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(kConnectPollInterval);
        }

        // Probe: Manual / openMSX-for-MAMI wire fact — sync call with
        // zeros. Throws on refused / timeout / RPC error.
        static_cast<void>(impl_->client->call(
            kMethodName,
            static_cast<unsigned char>(0),
            static_cast<unsigned char>(0),
            static_cast<unsigned int>(0),
            static_cast<unsigned int>(0)));

        impl_->state = MAmidiConnectionState::Connected;
        return true;
    } catch (const rpc::timeout& error) {
        impl_->last_error = std::string("RPC timeout: ") + error.what();
    } catch (const rpc::rpc_error& error) {
        impl_->last_error = std::string("RPC error: ") + error.what();
    } catch (const std::exception& error) {
        impl_->last_error = std::string("RPC connect failed: ")
            + error.what();
    } catch (...) {
        impl_->last_error = "RPC connect failed: unknown error";
    }

    impl_->client.reset();
    impl_->state = MAmidiConnectionState::Faulted;
    return false;
}

void MAmidiRpcClient::disconnect() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    try {
        impl_->client.reset();
    } catch (...) {
        // Destructor of rpc::client may throw on outstanding calls;
        // never propagate from disconnect/noexcept paths.
    }
    // Keep last_error so callers can still explain a prior fault.
    impl_->state = MAmidiConnectionState::Disconnected;
}

bool MAmidiRpcClient::asyncWrite(const MAmidiChipAccess& access) {
    if (impl_->client == nullptr
        || impl_->state != MAmidiConnectionState::Connected) {
        if (impl_->last_error.empty()) {
            impl_->last_error = "RPC client is not connected";
        }
        return false;
    }

    try {
        static_cast<void>(impl_->client->async_call(
            kMethodName,
            static_cast<unsigned char>(access.device_id),
            static_cast<unsigned char>(access.unit_no),
            static_cast<unsigned int>(access.address),
            static_cast<unsigned int>(access.data)));
        return true;
    } catch (const std::exception& error) {
        impl_->last_error = std::string("RPC async write failed: ")
            + error.what();
        impl_->state = MAmidiConnectionState::Faulted;
        return false;
    } catch (...) {
        impl_->last_error = "RPC async write failed: unknown error";
        impl_->state = MAmidiConnectionState::Faulted;
        return false;
    }
}

MAmidiConnectionState MAmidiRpcClient::state() const noexcept {
    return impl_->state;
}

const std::string& MAmidiRpcClient::lastError() const noexcept {
    return impl_->last_error;
}

const std::string& MAmidiRpcClient::host() const noexcept {
    return impl_->host;
}

std::uint16_t MAmidiRpcClient::port() const noexcept {
    return impl_->port;
}

}  // namespace mgstc::engine
