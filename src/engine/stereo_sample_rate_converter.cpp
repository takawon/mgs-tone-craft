#include "mgstc/audio/stereo_sample_rate_converter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace mgstc::audio {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;

[[nodiscard]] double normalizedSinc(double value) noexcept {
    if (value == 0.0) {
        return 1.0;
    }
    return std::sin(kPi * value) / (kPi * value);
}

[[nodiscard]] double blackman(double normalized) noexcept {
    return 0.42
        - 0.5 * std::cos(2.0 * kPi * normalized)
        + 0.08 * std::cos(4.0 * kPi * normalized);
}

[[nodiscard]] double windowedSinc(
    double value,
    double half_taps) noexcept {
    return blackman(0.5 + 0.5 * value / half_taps)
        * normalizedSinc(value);
}

[[nodiscard]] float finiteOrSilence(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0F;
}

}  // namespace

bool StereoSampleRateConverter::prepare(
    double target_sample_rate,
    std::size_t source_chunk_frames) {
    if (!std::isfinite(target_sample_rate)
        || target_sample_rate <= 0.0
        || source_chunk_frames == 0
        || source_chunk_frames
            > std::numeric_limits<std::size_t>::max() / 2) {
        return false;
    }

    target_sample_rate_ = target_sample_rate;
    ratio_ = kSourceSampleRate / target_sample_rate_;
    source_buffer_.assign(source_chunk_frames * 2, 0.0F);

    constexpr double half_taps =
        static_cast<double>(kTapCount) / 2.0;
    const bool downsample = target_sample_rate_ < kSourceSampleRate;
    for (std::size_t index = 0; index < sinc_table_.size(); ++index) {
        const auto distance =
            static_cast<double>(index)
            / static_cast<double>(kTableResolution);
        const auto kernel = downsample
            ? windowedSinc(distance / ratio_, half_taps) / ratio_
            : windowedSinc(distance, half_taps);
        sinc_table_[index] = static_cast<float>(kernel);
    }

    reset();
    return true;
}

void StereoSampleRateConverter::reset() noexcept {
    left_history_.fill(0.0F);
    right_history_.fill(0.0F);
    fraction_ = 0.0;
    primed_ = false;
    source_cursor_frames_ = 0;
    source_valid_frames_ = 0;
}

bool StereoSampleRateConverter::refill(
    StereoRenderCallback render) noexcept {
    if (source_buffer_.empty()
        || render.render == nullptr
        || !render.render(render.context, source_buffer_)) {
        source_cursor_frames_ = 0;
        source_valid_frames_ = 0;
        return false;
    }
    source_cursor_frames_ = 0;
    source_valid_frames_ = source_buffer_.size() / 2;
    return true;
}

bool StereoSampleRateConverter::pushNext(
    StereoRenderCallback render) noexcept {
    if (source_cursor_frames_ == source_valid_frames_
        && !refill(render)) {
        return false;
    }

    std::move(
        left_history_.begin() + 1,
        left_history_.end(),
        left_history_.begin());
    std::move(
        right_history_.begin() + 1,
        right_history_.end(),
        right_history_.begin());

    const auto offset = source_cursor_frames_ * 2;
    left_history_.back() = finiteOrSilence(source_buffer_[offset]);
    right_history_.back() = finiteOrSilence(source_buffer_[offset + 1]);
    ++source_cursor_frames_;
    return true;
}

float StereoSampleRateConverter::lookup(double distance) const noexcept {
    const auto index = static_cast<std::size_t>(
        std::abs(distance) * static_cast<double>(kTableResolution));
    return sinc_table_[std::min(index, sinc_table_.size() - 1)];
}

float StereoSampleRateConverter::interpolate(
    const std::array<float, kTapCount>& history) const noexcept {
    constexpr double center =
        static_cast<double>(kTapCount) / 2.0 - 1.0;
    double sum = 0.0;
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto distance =
            static_cast<double>(index) - center - fraction_;
        sum += static_cast<double>(history[index])
            * static_cast<double>(lookup(distance));
    }
    return finiteOrSilence(static_cast<float>(sum));
}

bool StereoSampleRateConverter::process(
    float* output_left,
    float* output_right,
    std::size_t output_frames,
    StereoRenderCallback render) noexcept {
    if (target_sample_rate_ <= 0.0
        || (output_frames != 0
            && output_left == nullptr
            && output_right == nullptr)) {
        return false;
    }

    if (!primed_) {
        if (!pushNext(render)) {
            if (output_left != nullptr) {
                std::fill_n(output_left, output_frames, 0.0F);
            }
            if (output_right != nullptr) {
                std::fill_n(output_right, output_frames, 0.0F);
            }
            return false;
        }
        primed_ = true;
    }

    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        if (output_left != nullptr) {
            output_left[frame] = interpolate(left_history_);
        }
        if (output_right != nullptr) {
            output_right[frame] = interpolate(right_history_);
        }

        fraction_ += ratio_;
        while (fraction_ >= 1.0) {
            if (!pushNext(render)) {
                if (output_left != nullptr) {
                    std::fill(
                        output_left + frame + 1,
                        output_left + output_frames,
                        0.0F);
                }
                if (output_right != nullptr) {
                    std::fill(
                        output_right + frame + 1,
                        output_right + output_frames,
                        0.0F);
                }
                return false;
            }
            fraction_ -= 1.0;
        }
    }
    return true;
}

}  // namespace mgstc::audio
