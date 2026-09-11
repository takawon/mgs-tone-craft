#pragma once

#include <array>
#include <cstddef>
#include <span>

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
    [[nodiscard]] float next(Generate&& generate, std::span<float> channels = {}) noexcept {
        if (!channels.empty() && !tracking_channels_) {
            channel_history_ = {};
        }
        tracking_channels_ = !channels.empty();
        while (input_hz_ > time_) {
            time_ += output_hz_;
            push(static_cast<float>(generate()));
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                auto& history = channel_history_[ch];
                for (std::size_t k = 1; k < kTapCount; ++k)
                    history[k - 1] = history[k];
                history.back() = channels[ch];
            }
        }
        time_ -= input_hz_;
        const float mixed = interpolate();
        // Read the same native instants with the mono converter's phase.
        // The generator is never called again for individual channels.
        constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
        std::array<float, kTapCount> coefficients{};
        if (!channels.empty())
            for (std::size_t k = 0; k < kTapCount; ++k)
                coefficients[k] = lookup(static_cast<double>(k) - center - frac_);
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            double sum = 0.0;
            for (std::size_t k = 0; k < kTapCount; ++k)
                sum += static_cast<double>(channel_history_[ch][k])
                    * static_cast<double>(coefficients[k]);
            channels[ch] = static_cast<float>(sum);
        }
        return mixed;
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
    std::array<std::array<float, kTapCount>, 14> channel_history_{};
    bool tracking_channels_{};
    std::array<float, kTableReso * kTapCount / 2> sinc_table_{};
};

}  // namespace mgstc::engine
