// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "mgstc/engine/realtime_engine_host.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mgstc::app {

enum class HardwareRequestStatus {
    Accepted,
    AlreadyCurrent,
    RejectedBusy,
    RejectedClosing,
    StartFailed,
};

struct HardwareRequestReceipt {
    HardwareRequestStatus status{HardwareRequestStatus::RejectedBusy};
    std::uint64_t id{};

    [[nodiscard]] bool accepted() const noexcept {
        return status == HardwareRequestStatus::Accepted
            || status == HardwareRequestStatus::AlreadyCurrent;
    }
};

struct HardwareControlRequest {
    enum class Kind { Mode, Reconnect, Settings };
    Kind kind{Kind::Mode};
    bool hardware{};
    engine::MAmidiOutputSettings settings{};
    int silence_seconds{};
};

struct HardwareControlResult {
    std::uint64_t lifetime_id{};
    std::uint64_t id{};
    HardwareControlRequest request{};
    bool host_accepted{};
    bool connection_succeeded{};
    bool close_acknowledged{true};
    engine::SoundOutputKind actual_kind{engine::SoundOutputKind::Emulator};
    engine::SoundOutputKind desired_kind{engine::SoundOutputKind::Emulator};
    bool owned{};
    bool connected{};
    std::string error{};
    std::string storage_error{};
};

// The service/processor owns this control-only object and its Host. It must
// shut down this object before destroying the Host or any persistence state.
// The worker never waits for a Message callback or this lifecycle mutex.
class HardwareOutputControl final {
public:
    using Persistence = std::function<void(HardwareControlResult&)>;

    explicit HardwareOutputControl(engine::RealtimeEngineHost& host) noexcept
        : host_(host), lifetime_id_(next_lifetime_.fetch_add(1, std::memory_order_relaxed)) {}

    ~HardwareOutputControl() { static_cast<void>(shutdown()); }
    HardwareOutputControl(const HardwareOutputControl&) = delete;
    HardwareOutputControl& operator=(const HardwareOutputControl&) = delete;

    [[nodiscard]] std::uint64_t lifetimeId() const noexcept { return lifetime_id_; }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }

    // Message/control only. The receipt says whether a request was admitted;
    // it does not say that the Host operation or connection has succeeded.
    [[nodiscard]] HardwareRequestReceipt request(
        HardwareControlRequest request, Persistence persist = {}) {
        if (closing_.load(std::memory_order_acquire))
            return {HardwareRequestStatus::RejectedClosing};
        std::unique_lock lock(lifecycle_, std::try_to_lock);
        if (!lock.owns_lock()) return {closing_.load(std::memory_order_acquire)
            ? HardwareRequestStatus::RejectedClosing : HardwareRequestStatus::RejectedBusy};
        if (closing_.load(std::memory_order_acquire))
            return {HardwareRequestStatus::RejectedClosing};
        if (pending_.load(std::memory_order_relaxed))
            return {HardwareRequestStatus::RejectedBusy};

        // A matching route can still require the owner's value persistence
        // (for example an explicit emulator choice after failed startup).
        if (request.kind == HardwareControlRequest::Kind::Mode && !persist) {
            engine::SoundOutputSnapshot snapshot;
            const auto requested = request.hardware
                ? engine::SoundOutputKind::MAmidiMemo : engine::SoundOutputKind::Emulator;
            if (host_.trySoundOutputSnapshot(snapshot) == engine::OutputSnapshotResult::Ready
                && !snapshot.switching && snapshot.kind == requested
                && snapshot.owned == request.hardware)
                return {HardwareRequestStatus::AlreadyCurrent};
        }

        const auto id = ++next_request_;
        result_ = {};
        result_.lifetime_id = lifetime_id_;
        result_.id = id;
        result_.request = std::move(request);
        done_.store(false, std::memory_order_relaxed);
        pending_.store(true, std::memory_order_release);
        try {
            worker_ = std::thread([this, persist = std::move(persist)] {
                try {
                    execute();
                    // Persistence consumes the accepted request's values, never
                    // a live Editor or a later draft. The owner supplies no UI
                    // callback here and remains alive until this worker exits.
                    if (persist) {
                        try { persist(result_); }
                        catch (const std::exception& error) { result_.storage_error = error.what(); }
                        catch (...) { result_.storage_error = "Output settings could not be saved"; }
                    }
                } catch (const std::exception& error) {
                    try { result_.error = error.what(); } catch (...) {}
                } catch (...) {
                    try { result_.error = "Hardware output operation failed"; } catch (...) {}
                }
                done_.store(true, std::memory_order_release);
            });
        } catch (...) {
            pending_.store(false, std::memory_order_release);
            result_ = {};
            return {HardwareRequestStatus::StartFailed, id};
        }
        return {HardwareRequestStatus::Accepted, id};
    }

    // No waiting: even a published result can precede native thread exit.
    // Keep the operation pending until the actual thread has terminated.
    [[nodiscard]] std::optional<HardwareControlResult> collect() {
        std::unique_lock lock(lifecycle_, std::try_to_lock);
        if (!lock.owns_lock() || !pending_.load(std::memory_order_acquire)
            || !done_.load(std::memory_order_acquire) || !nativeFinished()) return {};
        worker_.join();
        pending_.store(false, std::memory_order_release);
        return std::move(result_);
    }

    // Teardown may wait indefinitely for a non-returning DNS/RPC operation.
    // Do not call while holding a Host/lease lock or from realtime audio.
    [[nodiscard]] std::optional<HardwareControlResult> shutdown() {
        closing_.store(true, std::memory_order_release);
        std::unique_lock lock(lifecycle_);
        if (worker_.joinable()) worker_.join();
        if (!pending_.exchange(false, std::memory_order_acq_rel)) return {};
        return std::move(result_);
    }

private:
    [[nodiscard]] bool nativeFinished() noexcept {
        if (!worker_.joinable()) return false;
#if defined(_WIN32)
        return WaitForSingleObject(worker_.native_handle(), 0) == WAIT_OBJECT_0;
#else
        // Windows is the product's native-exit polling platform. Elsewhere
        // keep a finished task owned until explicit teardown can join it.
        return false;
#endif
    }

    void execute() {
        switch (result_.request.kind) {
        case HardwareControlRequest::Kind::Mode:
            result_.host_accepted = host_.setSoundOutputKind(result_.request.hardware
                ? engine::SoundOutputKind::MAmidiMemo : engine::SoundOutputKind::Emulator);
            break;
        case HardwareControlRequest::Kind::Reconnect:
            result_.host_accepted = host_.reconnectMAmidi();
            break;
        case HardwareControlRequest::Kind::Settings:
            result_.host_accepted = host_.setMAmidiSettings(result_.request.settings);
            break;
        }
        // No other request for this Host is admitted until collection. For an
        // explicit emulator release there is no longer an owner callback that
        // can reopen its backend and overwrite this close acknowledgement.
        result_.close_acknowledged = host_.hardwareCloseAcknowledged();
        result_.actual_kind = host_.soundOutputKind();
        result_.desired_kind = host_.desiredSoundOutputKind();
        result_.owned = host_.ownsHardwareOutput();
        result_.connected = host_.hardwareOutputConnected();
        result_.connection_succeeded = result_.host_accepted
            && (result_.actual_kind == engine::SoundOutputKind::Emulator || result_.connected);
        if (!result_.host_accepted || !result_.connection_succeeded)
            result_.error = host_.soundOutputLastError(); // Worker only.
    }

    inline static std::atomic<std::uint64_t> next_lifetime_{1};
    engine::RealtimeEngineHost& host_;
    const std::uint64_t lifetime_id_;
    std::mutex lifecycle_;
    std::thread worker_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> done_{false};
    std::atomic<bool> closing_{false};
    std::uint64_t next_request_{};
    HardwareControlResult result_{};
};

} // namespace mgstc::app
