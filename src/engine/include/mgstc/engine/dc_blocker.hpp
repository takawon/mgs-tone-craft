#pragma once

#include <cmath>

namespace mgstc::engine {

// One-pole DC blocker (high-pass). Models a series capacitor + shunt
// resistor at the mixed float output. Allocation-free; audio-thread safe.
class DcBlocker {
public:
    // cutoff_hz: approximate -3 dB point. MSX-player-like output stages
    // use a few hertz so DC drifts vanish without audible tone change.
    explicit DcBlocker(
        float sample_rate_hz,
        float cutoff_hz = 3.4F) noexcept
        : pole_(std::exp(
              -2.0F * 3.14159265F * cutoff_hz / sample_rate_hz)) {}

    void reset() noexcept {
        previous_in_ = 0.0F;
        previous_out_ = 0.0F;
    }

    [[nodiscard]] float process(float input) noexcept {
        const float output =
            input - previous_in_ + pole_ * previous_out_;
        previous_in_ = input;
        previous_out_ = output;
        return output;
    }

private:
    float pole_{};
    float previous_in_{};
    float previous_out_{};
};

}  // namespace mgstc::engine
