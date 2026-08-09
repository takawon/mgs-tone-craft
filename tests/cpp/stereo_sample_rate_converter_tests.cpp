#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "mgstc/audio/stereo_sample_rate_converter.hpp"

namespace {

using mgstc::audio::StereoRenderCallback;
using mgstc::audio::StereoSampleRateConverter;

struct StereoSource {
    std::size_t frame{};

    [[nodiscard]] static bool render(
        void* context,
        std::span<float> interleaved) noexcept {
        auto& source = *static_cast<StereoSource*>(context);
        for (std::size_t offset = 0; offset < interleaved.size();
             offset += 2) {
            const auto phase =
                static_cast<double>(source.frame) * 0.03125;
            const auto left = static_cast<float>(std::sin(phase));
            interleaved[offset] = left;
            interleaved[offset + 1] = left * -0.5F;
            ++source.frame;
        }
        return true;
    }

    [[nodiscard]] StereoRenderCallback callback() noexcept {
        return {
            .context = this,
            .render = &StereoSource::render,
        };
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ConvertedStereo {
    std::vector<float> left;
    std::vector<float> right;
};

[[nodiscard]] ConvertedStereo convert(
    double target_rate,
    std::span<const std::size_t> block_sizes) {
    StereoSampleRateConverter converter;
    require(converter.prepare(target_rate, 37), "prepare failed");
    StereoSource source;

    std::size_t total_frames{};
    for (const auto block : block_sizes) {
        total_frames += block;
    }
    ConvertedStereo result{
        .left = std::vector<float>(total_frames),
        .right = std::vector<float>(total_frames),
    };

    std::size_t offset{};
    for (const auto block : block_sizes) {
        require(
            converter.process(
                result.left.data() + offset,
                result.right.data() + offset,
                block,
                source.callback()),
            "conversion failed");
        offset += block;
    }
    return result;
}

void testRate(double target_rate) {
    constexpr std::size_t frame_count = 4'097;
    const std::array one_block{frame_count};
    constexpr std::array irregular_blocks{
        std::size_t{1},
        std::size_t{17},
        std::size_t{511},
        std::size_t{3},
        std::size_t{1'024},
        std::size_t{2'541},
    };
    static_assert(
        1 + 17 + 511 + 3 + 1'024 + 2'541 == frame_count);

    const auto contiguous = convert(target_rate, one_block);
    const auto split = convert(target_rate, irregular_blocks);
    require(
        contiguous.left.size() == split.left.size(),
        "output size mismatch");

    bool found_signal = false;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto left = contiguous.left[frame];
        const auto right = contiguous.right[frame];
        require(
            std::isfinite(left) && std::isfinite(right),
            "non-finite output");
        require(
            std::abs(right + left * 0.5F) < 1.0e-5F,
            "left/right synchronization lost");
        require(
            std::abs(left - split.left[frame]) < 1.0e-6F
                && std::abs(right - split.right[frame]) < 1.0e-6F,
            "block boundary discontinuity");
        found_signal = found_signal || std::abs(left) > 1.0e-4F;
    }
    require(found_signal, "converter produced only silence");
}

void testNonFiniteInputIsSilenced() {
    struct NonFiniteSource {
        [[nodiscard]] static bool render(
            void*,
            std::span<float> interleaved) noexcept {
            for (std::size_t offset = 0; offset < interleaved.size();
                 offset += 2) {
                interleaved[offset] =
                    std::numeric_limits<float>::infinity();
                interleaved[offset + 1] =
                    std::numeric_limits<float>::quiet_NaN();
            }
            return true;
        }
    };

    StereoSampleRateConverter converter;
    require(converter.prepare(44'100.0, 11), "prepare failed");
    std::array<float, 257> left{};
    std::array<float, 257> right{};
    require(
        converter.process(
            left.data(),
            right.data(),
            left.size(),
            {
                .context = nullptr,
                .render = &NonFiniteSource::render,
            }),
        "non-finite conversion failed");
    require(
        std::ranges::all_of(left, [](float value) {
            return std::isfinite(value);
        })
            && std::ranges::all_of(right, [](float value) {
                return std::isfinite(value);
            }),
        "non-finite source escaped converter");
}

void testUpsamplingPhaseMovesForward() {
    struct RampSource {
        std::size_t frame{};

        [[nodiscard]] static bool render(
            void* context,
            std::span<float> interleaved) noexcept {
            auto& source = *static_cast<RampSource*>(context);
            for (std::size_t offset = 0; offset < interleaved.size();
                 offset += 2) {
                const auto value =
                    static_cast<float>(source.frame++) * 0.001F;
                interleaved[offset] = value;
                interleaved[offset + 1] = value;
            }
            return true;
        }
    };

    StereoSampleRateConverter converter;
    require(converter.prepare(96'000.0, 31), "prepare failed");
    RampSource source;
    std::array<float, 512> left{};
    std::array<float, 512> right{};
    require(
        converter.process(
            left.data(),
            right.data(),
            left.size(),
            {
                .context = &source,
                .render = &RampSource::render,
            }),
        "upsampling failed");
    for (std::size_t frame = 64; frame + 1 < left.size(); ++frame) {
        require(
            left[frame + 1] >= left[frame] - 1.0e-6F,
            "upsampling phase moved backward");
    }
    const auto average_step =
        (left.back() - left[64])
        / static_cast<float>(left.size() - 65);
    require(
        std::abs(average_step - 0.0005F) < 2.0e-5F,
        "upsampling phase has the wrong rate");
}

}  // namespace

int main() {
    try {
        testRate(44'100.0);
        testRate(48'000.0);
        testRate(96'000.0);
        testNonFiniteInputIsSilenced();
        testUpsamplingPhaseMovesForward();
        std::cout << "Stereo sample-rate converter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
