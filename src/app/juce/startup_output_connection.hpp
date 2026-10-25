// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace mgstc::app {

// The UI owns start/collect; only the completion flag crosses threads. A failed
// thread launch never falls back to blocking connection work on the UI thread.
class StartupOutputConnection final {
public:
    struct Result {
        bool connected{};
        std::string error;
    };

    ~StartupOutputConnection() { join(); }
    StartupOutputConnection() = default;
    StartupOutputConnection(const StartupOutputConnection&) = delete;
    StartupOutputConnection& operator=(const StartupOutputConnection&) = delete;

    void start(std::function<Result()> connect) {
        if (pending_ || started_) return;
        started_ = true;
        pending_ = true;
        try {
            thread_ = std::thread([this, connect = std::move(connect)] {
                try { result_ = connect(); }
                catch (const std::exception& error) { result_.error = error.what(); }
                catch (...) { result_.error = "MAmidiMEmo connection failed"; }
                done_.store(true, std::memory_order_release);
            });
        } catch (const std::exception& error) {
            result_.error = error.what();
            done_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] bool started() const noexcept { return started_; }

    [[nodiscard]] std::optional<Result> collect() {
        if (!pending_ || !done_.load(std::memory_order_acquire)) return {};
        join();
        pending_ = false;
        return std::move(result_);
    }

    // Teardown must keep the service and engine alive until the RPC timeout or
    // completion. It is never called from realtime audio or while polling UI.
    void join() noexcept { if (thread_.joinable()) thread_.join(); }

private:
    std::thread thread_;
    std::atomic<bool> done_{false};
    bool started_{}, pending_{};
    Result result_;
};

} // namespace mgstc::app
