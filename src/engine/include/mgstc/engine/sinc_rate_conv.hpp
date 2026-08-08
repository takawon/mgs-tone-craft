#pragma once

#include <array>
#include <cstddef>

namespace mgstc::engine {

// Mono rate converter: truncated Blackman-windowed sinc.
// Intended for chip-native → engine sample-rate conversion.
// No heap traffic after construction; safe for the audio thread.
class SincRateConv {
public:
    static constexpr std::size_t kTapCount = 16;
    static constexpr std::size_t kTableReso = 256;

    SincRateConv(double input_hz, double output_hz);

    void reset() noexcept;

    // Emit one sample at output_hz. `generate` yields the next sample at
    // input_hz whenever the converter needs fresh input.
    template <typename Generate>
    [[nodiscard]] float next(Generate&& generate) noexcept {
        while (input_hz_ > time_) {
            time_ += output_hz_;
            push(static_cast<float>(generate()));
        }
        time_ -= input_hz_;
        return interpolate();
    }

private:
    void push(float sample) noexcept;
    [[nodiscard]] float interpolate() noexcept;
    [[nodiscard]] float lookup(double x) const noexcept;

    double input_hz_{};
    double output_hz_{};
    double ratio_{};
    double time_{};
    double frac_{};
    std::array<float, kTapCount> history_{};
    std::array<float, kTableReso * kTapCount / 2> sinc_table_{};
};

}  // namespace mgstc::engine
