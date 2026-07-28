#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace mgstc::engine {

// Fixed-capacity single-producer/single-consumer queue.
// The producer and consumer must each be confined to one thread.
#ifdef _MSC_VER
#pragma warning(push)
// Cache-line separation intentionally adds padding around the indices.
#pragma warning(disable : 4324)
#endif
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    [[nodiscard]] bool tryPush(const T& value) noexcept {
        const auto write = write_index_.load(std::memory_order_relaxed);
        const auto read = read_index_.load(std::memory_order_acquire);
        if (write - read >= Capacity) {
            return false;
        }
        storage_[write % Capacity] = value;
        write_index_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPop(T& value) noexcept {
        const auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        value = storage_[read % Capacity];
        read_index_.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t approximateSize() const noexcept {
        const auto write = write_index_.load(std::memory_order_acquire);
        const auto read = read_index_.load(std::memory_order_acquire);
        return std::min(write - read, Capacity);
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Capacity;
    }

private:
    std::array<T, Capacity> storage_{};
    alignas(64) std::atomic<std::size_t> write_index_{};
    alignas(64) std::atomic<std::size_t> read_index_{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace mgstc::engine
