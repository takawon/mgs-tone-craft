#include "mgstc/engine/sinc_rate_conv.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mgstc::engine {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;

[[nodiscard]] double blackman(double normalized) noexcept {
    return 0.42
        - 0.5 * std::cos(2.0 * kPi * normalized)
        + 0.08 * std::cos(4.0 * kPi * normalized);
}

[[nodiscard]] double normalizedSinc(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }
    return std::sin(kPi * x) / (kPi * x);
}

[[nodiscard]] double windowedSinc(double x, double half_taps) noexcept {
    const double window = blackman(0.5 + 0.5 * x / half_taps);
    return window * normalizedSinc(x);
}

}  // namespace

SincRateConv::SincRateConv(double input_hz, double output_hz)
    : input_hz_(input_hz)
    , output_hz_(output_hz)
    , ratio_(input_hz / output_hz) {
    constexpr double half_taps = static_cast<double>(kTapCount) / 2.0;
    const bool downsample = output_hz < input_hz;
    for (std::size_t i = 0; i < sinc_table_.size(); ++i) {
        const double x =
            static_cast<double>(i) / static_cast<double>(kTableReso);
        const double kernel = downsample
            ? windowedSinc(x / ratio_, half_taps) / ratio_
            : windowedSinc(x, half_taps);
        sinc_table_[i] = static_cast<float>(kernel);
    }
    reset();
}

void SincRateConv::reset() noexcept {
    history_.fill(0.0F);
    channel_history_ = {};
    tracking_channels_ = false;
    time_ = 0.0;
    frac_ = 0.0;
}

void SincRateConv::push(float sample) noexcept {
    for (std::size_t i = 0; i + 1 < history_.size(); ++i) {
        history_[i] = history_[i + 1];
    }
    history_.back() = sample;
}

float SincRateConv::lookup(double x) const noexcept {
    const auto index = static_cast<int>(
        std::abs(x) * static_cast<double>(kTableReso));
    const auto clamped = std::min(
        static_cast<int>(sinc_table_.size()) - 1,
        index);
    return sinc_table_[static_cast<std::size_t>(clamped)];
}

float SincRateConv::interpolate() noexcept {
    frac_ += ratio_;
    frac_ -= std::floor(frac_);

    double sum = 0.0;
    constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
    for (std::size_t k = 0; k < history_.size(); ++k) {
        const double x = static_cast<double>(k) - center - frac_;
        sum += static_cast<double>(history_[k])
            * static_cast<double>(lookup(x));
    }
    return static_cast<float>(sum);
}

}  // namespace mgstc::engine
