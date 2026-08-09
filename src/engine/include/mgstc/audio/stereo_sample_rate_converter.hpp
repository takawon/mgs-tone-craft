#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace mgstc::audio {

struct StereoRenderCallback {
    void* context{};
    bool (*render)(
        void* context,
        std::span<float> interleaved_stereo) noexcept {};
};

// Streaming stereo sinc converter for realtime sinks. prepare() may allocate;
// process() is allocation-free and keeps one phase clock for both channels.
class StereoSampleRateConverter {
public:
    static constexpr double kSourceSampleRate = 48'000.0;
    static constexpr std::size_t kTapCount = 16;
    static constexpr std::size_t kTableResolution = 256;

    [[nodiscard]] bool prepare(
        double target_sample_rate,
        std::size_t source_chunk_frames = 256);
    void reset() noexcept;

    [[nodiscard]] bool process(
        float* output_left,
        float* output_right,
        std::size_t output_frames,
        StereoRenderCallback render) noexcept;

    [[nodiscard]] double targetSampleRate() const noexcept {
        return target_sample_rate_;
    }

private:
    [[nodiscard]] bool refill(StereoRenderCallback render) noexcept;
    [[nodiscard]] bool pushNext(StereoRenderCallback render) noexcept;
    [[nodiscard]] float interpolate(
        const std::array<float, kTapCount>& history) const noexcept;
    [[nodiscard]] float lookup(double distance) const noexcept;

    double target_sample_rate_{};
    double ratio_{1.0};
    double fraction_{};
    bool primed_{};
    std::array<float, kTapCount> left_history_{};
    std::array<float, kTapCount> right_history_{};
    std::array<float, kTableResolution * kTapCount / 2> sinc_table_{};
    std::vector<float> source_buffer_;
    std::size_t source_cursor_frames_{};
    std::size_t source_valid_frames_{};
};

}  // namespace mgstc::audio
