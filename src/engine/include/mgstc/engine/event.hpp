#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mgstc::engine {

using Tick = std::uint64_t;

enum class MeaningEventKind : std::uint8_t {
    Volume,
    Patch,
    RegisterWrite,
    FrequencyDelta,
    TrackMicroDetune,
    Noise,
    ToneNoiseMode,
    RateVolume,
};

struct MeaningEvent {
    Tick tick{};
    MeaningEventKind kind{};
    std::int32_t arg0{};
    std::int32_t arg1{};

    friend bool operator==(const MeaningEvent&, const MeaningEvent&) = default;
};

class EventBuffer {
public:
    explicit EventBuffer(std::size_t max_events)
        : max_events_(max_events) {
        events_.reserve(max_events_);
    }

    void clear() noexcept {
        events_.clear();
    }

    [[nodiscard]] bool push(MeaningEvent event) {
        if (events_.size() >= max_events_) {
            return false;
        }
        events_.push_back(event);
        return true;
    }

    [[nodiscard]] std::span<const MeaningEvent> events() const noexcept {
        return events_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return events_.size();
    }

    [[nodiscard]] std::size_t maxEvents() const noexcept {
        return max_events_;
    }

private:
    std::size_t max_events_{};
    std::vector<MeaningEvent> events_;
};

}  // namespace mgstc::engine
