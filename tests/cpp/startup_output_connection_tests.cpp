// SPDX-License-Identifier: AGPL-3.0-only
#include "startup_output_connection.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false, release = false;
        mgstc::app::StartupOutputConnection connection;
        connection.start([&] {
            std::unique_lock lock(mutex);
            entered = true;
            wake.notify_one();
            wake.wait_for(lock, std::chrono::seconds(5), [&] { return release; });
            return mgstc::app::StartupOutputConnection::Result{false, "probe failed"};
        });
        {
            std::unique_lock lock(mutex);
            require(wake.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }),
                "Connection worker did not start");
        }
        require(connection.pending(), "Pending connection was not exposed");
        // collect must return while the probe is still blocked.
        require(!connection.collect(), "Incomplete probe was published");
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        wake.notify_one();
        connection.join();
        const auto result = connection.collect();
        require(result && !result->connected && result->error == "probe failed",
            "Connection failure was not retained");
        require(!connection.pending() && !connection.collect(),
            "Completed connection was not consumed once");

        mgstc::app::StartupOutputConnection throwing;
        throwing.start([]() -> mgstc::app::StartupOutputConnection::Result {
            throw std::runtime_error("injected failure");
        });
        throwing.join();
        const auto failure = throwing.collect();
        require(failure && failure->error == "injected failure", "Worker exception escaped");
        std::cout << "Startup connection pending/result/exception checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
