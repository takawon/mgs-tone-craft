// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "mgstc/engine/realtime_engine_host.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mgstc::engine {

class RealtimeEngineHostTestAccess {
public:
    static void duringOutputTransition(RealtimeEngineHost& host,
        const std::function<void()>& work) {
        RealtimeEngineHost::OutputTransition transition(host,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
        if (!transition) throw std::runtime_error("Test output transition timed out");
        work();
    }
    static void duringRealtimeAccess(RealtimeEngineHost& host,
        const std::function<void()>& work) {
        RealtimeEngineHost::RealtimeAccess access(host);
        if (!access) throw std::runtime_error("Test audio access was not admitted");
        work();
    }
    static std::uint32_t accessState(const RealtimeEngineHost& host) {
        return host.output_access_.load(std::memory_order_acquire);
    }
    static void duringBackendErrorLock(RealtimeEngineHost& host,
        const std::function<void()>& work) {
        std::lock_guard lock(host.mamidi_->error_mutex_);
        work();
    }
    static bool publishScopeFrame(RealtimeEngineHost& host, const OpllScopeFrame& frame,
        bool spectrogram = false) {
        return spectrogram ? host.spectrogram_scope_frames_->tryPush(frame)
            : host.opll_scope_frames_->tryPush(frame);
    }
};

namespace test {

// Holds a real production guard on a second thread. Releasing it is guaranteed
// during assertion unwinding; no hook is added to the product audio path.
class HeldHostAccess {
public:
    enum class Kind { Audio, Control, BackendError };
    HeldHostAccess(RealtimeEngineHost& host, Kind kind)
        : worker_([&, kind] {
            try {
                const auto hold = [&] {
                    std::unique_lock lock(mutex_);
                    entered_ = true;
                    wake_.notify_one();
                    wake_.wait(lock, [&] { return released_; });
                };
                if (kind == Kind::Audio)
                    RealtimeEngineHostTestAccess::duringRealtimeAccess(host, hold);
                else if (kind == Kind::BackendError)
                    RealtimeEngineHostTestAccess::duringBackendErrorLock(host, hold);
                else RealtimeEngineHostTestAccess::duringOutputTransition(host, hold);
            } catch (...) {
                std::lock_guard lock(mutex_);
                error_ = std::current_exception();
                entered_ = true;
                wake_.notify_one();
            }
        }) {
        std::unique_lock lock(mutex_);
        const bool entered = wake_.wait_for(lock, std::chrono::seconds(5), [&] { return entered_; });
        const auto error = error_;
        lock.unlock();
        if (!entered || error) {
            release();
            if (error) std::rethrow_exception(error);
            throw std::runtime_error("Test access holder did not start");
        }
    }
    ~HeldHostAccess() { release(); }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool entered_{}, released_{};
    std::exception_ptr error_;
    std::thread worker_;
};

}  // namespace test
}  // namespace mgstc::engine
