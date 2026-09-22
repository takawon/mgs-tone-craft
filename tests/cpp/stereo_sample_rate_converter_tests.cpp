#include <algorithm>
#include <array>
#include <chrono>
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

struct CountingSource {
    std::size_t frame{};
    std::size_t callbacks{};
    std::size_t last_callback_frames{};
    std::size_t max_callback_frames{};
    std::size_t impulse_frame{std::numeric_limits<std::size_t>::max()};

    [[nodiscard]] static bool render(
        void* context,
        std::span<float> interleaved) noexcept {
        auto& source = *static_cast<CountingSource*>(context);
        const auto frames = interleaved.size() / 2;
        ++source.callbacks;
        source.last_callback_frames = frames;
        source.max_callback_frames =
            std::max(source.max_callback_frames, frames);
        for (std::size_t offset = 0; offset < interleaved.size();
             offset += 2) {
            const auto value =
                source.frame == source.impulse_frame ? 1.0F : 0.0F;
            interleaved[offset] = value;
            interleaved[offset + 1] = value;
            ++source.frame;
        }
        return true;
    }

    [[nodiscard]] StereoRenderCallback callback() noexcept {
        return {
            .context = this,
            .render = &CountingSource::render,
        };
    }
};

struct ConsumeTrace {
    std::size_t output_frames{};
    std::size_t source_frames{};
    std::size_t first_output_source_frames{};
    std::size_t max_source_ahead{};
    std::size_t max_callback_frames{};
    std::size_t callbacks{};
};

[[nodiscard]] ConsumeTrace traceConsumption(
    double target_rate,
    std::size_t chunk_frames,
    std::size_t output_frames) {
    StereoSampleRateConverter converter;
    require(converter.prepare(target_rate, chunk_frames), "prepare failed");
    CountingSource source;
    ConsumeTrace trace{};
    float left = 0.0F;
    float right = 0.0F;
    for (std::size_t output = 0; output < output_frames; ++output) {
        require(
            converter.process(&left, &right, 1, source.callback()),
            "one-sample conversion failed");
        if (output == 0) {
            trace.first_output_source_frames = source.frame;
        }
        const auto theoretical = static_cast<std::size_t>(
            static_cast<double>(output)
            * StereoSampleRateConverter::kSourceSampleRate
            / target_rate);
        if (source.frame > theoretical) {
            trace.max_source_ahead = std::max(
                trace.max_source_ahead, source.frame - theoretical);
        }
    }
    trace.output_frames = output_frames;
    trace.source_frames = source.frame;
    trace.max_callback_frames = source.max_callback_frames;
    trace.callbacks = source.callbacks;
    return trace;
}

[[nodiscard]] std::size_t impulsePeakOutputIndex(
    double target_rate,
    std::size_t chunk_frames,
    std::size_t impulse_source_frame,
    std::size_t output_frames) {
    StereoSampleRateConverter converter;
    require(converter.prepare(target_rate, chunk_frames), "prepare failed");
    CountingSource source;
    source.impulse_frame = impulse_source_frame;
    std::vector<float> left(output_frames);
    std::vector<float> right(output_frames);
    require(
        converter.process(
            left.data(), right.data(), output_frames, source.callback()),
        "impulse conversion failed");
    std::size_t peak_at = 0;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < output_frames; ++frame) {
        const auto magnitude = std::abs(left[frame]);
        require(
            std::abs(right[frame] - left[frame]) < 1.0e-6F,
            "impulse stereo mismatch");
        if (magnitude > peak) {
            peak = magnitude;
            peak_at = frame;
        }
    }
    require(peak > 0.1F, "impulse produced no peak");
    return peak_at;
}

void testRefillRequestsWholeChunk() {
    StereoSampleRateConverter converter;
    constexpr std::size_t chunk = 64;
    require(converter.prepare(44'100.0, chunk), "prepare failed");
    CountingSource source;
    std::array<float, 8> left{};
    std::array<float, 8> right{};
    require(
        converter.process(
            left.data(), right.data(), left.size(), source.callback()),
        "chunked conversion failed");
    require(
        source.max_callback_frames == chunk,
        "render callback must fill the prepared source chunk");
    require(source.frame >= chunk, "first refill must pull a full chunk");
}

void testChunk1DoesNotPullLargeLookahead() {
    constexpr std::array rates{
        44'100.0,
        48'000.0,
        88'200.0,
        96'000.0,
    };
    for (const auto rate : rates) {
        const auto trace = traceConsumption(rate, 1, 512);
        require(
            trace.max_callback_frames == 1,
            "chunk=1 must request one source frame at a time");
        require(
            trace.first_output_source_frames <= 3,
            "chunk=1 first output must not pull a large future block");
        require(
            trace.max_source_ahead
                <= StereoSampleRateConverter::kTapCount,
            "source read-ahead exceeded the FIR tap window");
        std::cout
            << "SRC consume rate=" << rate
            << " out=512 src=" << trace.source_frames
            << " first=" << trace.first_output_source_frames
            << " max_ahead=" << trace.max_source_ahead
            << " callbacks=" << trace.callbacks << '\n';
    }
}

void testLargeChunkPullsFutureSourceImmediately() {
    const auto trace = traceConsumption(44'100.0, 256, 1);
    require(
        trace.source_frames == 256,
        "default-sized chunk pulls a full refill before any output");
    require(
        trace.first_output_source_frames == 256,
        "one host sample with chunk=256 already consumes 256 source frames");
}

void testImpulseGroupDelay() {
    constexpr std::array rates{
        44'100.0,
        48'000.0,
        88'200.0,
        96'000.0,
    };
    constexpr std::size_t impulse_at = 32;
    for (const auto rate : rates) {
        const auto peak = impulsePeakOutputIndex(rate, 1, impulse_at, 256);
        const auto expected = static_cast<std::size_t>(
            std::llround(
                static_cast<double>(
                    impulse_at + StereoSampleRateConverter::kTapCount / 2)
                * rate
                / StereoSampleRateConverter::kSourceSampleRate));
        const auto delta =
            peak > expected ? peak - expected : expected - peak;
        require(
            delta <= 2,
            "impulse peak is not the 8-source-frame FIR group delay");
        std::cout
            << "SRC impulse rate=" << rate
            << " peak_at=" << peak
            << " expected=" << expected
            << " delta=" << delta << '\n';
    }
}

void testResetClearsHistoryAndPhase() {
    StereoSampleRateConverter converter;
    require(converter.prepare(44'100.0, 1), "prepare failed");
    CountingSource first;
    first.impulse_frame = 0;
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    require(
        converter.process(
            left.data(), right.data(), left.size(), first.callback()),
        "pre-reset conversion failed");
    converter.reset();
    CountingSource second;
    std::array<float, 128> silent_left{};
    std::array<float, 128> silent_right{};
    require(
        converter.process(
            silent_left.data(),
            silent_right.data(),
            silent_left.size(),
            second.callback()),
        "post-reset conversion failed");
    for (std::size_t frame = 0; frame < silent_left.size(); ++frame) {
        require(
            std::abs(silent_left[frame]) < 1.0e-6F
                && std::abs(silent_right[frame]) < 1.0e-6F,
            "reset left a previous impulse in the history");
    }
}

void testPrimeWithSilenceConsumptionIsFloorRatio() {
    constexpr std::array rates{
        44'100.0,
        88'200.0,
        96'000.0,
    };
    constexpr std::size_t outputs = 512;
    for (const auto rate : rates) {
        StereoSampleRateConverter converter;
        require(converter.prepare(rate, 1), "prepare failed");
        converter.primeWithSilence();
        CountingSource source;
        float left = 0.0F;
        float right = 0.0F;
        for (std::size_t output = 0; output < outputs; ++output) {
            require(
                converter.process(&left, &right, 1, source.callback()),
                "primed conversion failed");
        }
        const auto expected = static_cast<std::size_t>(
            static_cast<double>(outputs)
            * StereoSampleRateConverter::kSourceSampleRate
            / rate);
        require(
            source.frame == expected,
            "primeWithSilence must consume floor(output*ratio) source frames");
        require(
            source.frame
                <= StereoSampleRateConverter::kGroupDelaySourceFrames
                    || source.max_callback_frames == 1,
            "primed chunk=1 still requests one frame at a time");
        std::cout
            << "SRC primed consume rate=" << rate
            << " out=" << outputs
            << " src=" << source.frame
            << " expected=" << expected << '\n';
    }
}

void testConsumptionMatchesRatioPlusPrime() {
    constexpr std::size_t outputs = 4'410;
    const auto trace = traceConsumption(44'100.0, 1, outputs);
    const auto expected = 1
        + static_cast<std::size_t>(
              static_cast<double>(outputs)
              * StereoSampleRateConverter::kSourceSampleRate
              / 44'100.0);
    const auto delta = trace.source_frames > expected
        ? trace.source_frames - expected
        : expected - trace.source_frames;
    require(delta <= 1, "source consumption drifted from ratio*output+prime");
    std::cout
        << "SRC 44100 consume formula src=" << trace.source_frames
        << " expected=" << expected << '\n';
}

void testProcessReturnsWhenSourceMakesNoProgress() {
    StereoSampleRateConverter converter;
    require(converter.prepare(44'100.0, 1), "prepare failed");
    converter.primeWithSilence();

    struct RefusingSource {
        std::size_t calls{};
        [[nodiscard]] static bool render(void* context, std::span<float>) noexcept {
            auto& source = *static_cast<RefusingSource*>(context);
            ++source.calls;
            return false;
        }
    };

    RefusingSource source;
    std::array<float, 512> left{};
    std::array<float, 512> right{};
    const auto started = std::chrono::steady_clock::now();
    const bool ok = converter.process(
        left.data(),
        right.data(),
        left.size(),
        {
            .context = &source,
            .render = &RefusingSource::render,
        });
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    std::cerr
        << "SRC 0-progress return elapsed_ms=" << elapsed_ms
        << " calls=" << source.calls
        << " ok=" << ok << '\n';
    require(!ok, "0-progress source must fail process()");
    require(source.calls <= 2, "0-progress source must not retry unboundedly");
    require(elapsed_ms < 50.0, "process() must return quickly on 0 progress");
}

void testProcessReturnsWhenSourceStopsMidBlock() {
    StereoSampleRateConverter converter;
    require(converter.prepare(96'000.0, 1), "prepare failed");
    converter.primeWithSilence();

    struct LimitedSource {
        std::size_t frames{};
        std::size_t calls{};
        [[nodiscard]] static bool render(
            void* context,
            std::span<float> interleaved) noexcept {
            auto& source = *static_cast<LimitedSource*>(context);
            ++source.calls;
            if (source.frames >= 8) {
                return false;
            }
            std::fill(interleaved.begin(), interleaved.end(), 0.0F);
            source.frames += interleaved.size() / 2;
            return true;
        }
    };

    LimitedSource source;
    std::array<float, 256> left{};
    std::array<float, 256> right{};
    const auto started = std::chrono::steady_clock::now();
    const bool ok = converter.process(
        left.data(),
        right.data(),
        left.size(),
        {
            .context = &source,
            .render = &LimitedSource::render,
        });
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    std::cerr
        << "SRC mid-block stop elapsed_ms=" << elapsed_ms
        << " calls=" << source.calls
        << " frames=" << source.frames
        << " ok=" << ok << '\n';
    require(!ok, "exhausted source must fail process()");
    require(source.calls < 64, "mid-block 0 progress must stop");
    require(elapsed_ms < 50.0, "process() must return after source exhaustion");
}

void testPrimedChunk1CallbackCountMatchesRatio() {
    struct CountingSource {
        std::size_t calls{};
        std::size_t frames{};
        [[nodiscard]] static bool render(
            void* context,
            std::span<float> interleaved) noexcept {
            auto& source = *static_cast<CountingSource*>(context);
            ++source.calls;
            std::fill(interleaved.begin(), interleaved.end(), 0.0F);
            source.frames += interleaved.size() / 2;
            return true;
        }
    };

    const struct Case {
        double rate;
        std::size_t outputs;
    } cases[] = {
        {44'100.0, 512},
        {96'000.0, 512},
    };
    for (const auto& test : cases) {
        StereoSampleRateConverter converter;
        require(converter.prepare(test.rate, 1), "prepare failed");
        converter.primeWithSilence();
        CountingSource source;
        std::vector<float> left(test.outputs);
        std::vector<float> right(test.outputs);
        const auto started = std::chrono::steady_clock::now();
        require(
            converter.process(
                left.data(),
                right.data(),
                left.size(),
                {
                    .context = &source,
                    .render = &CountingSource::render,
                }),
            "primed chunk=1 process failed");
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        const auto expected = static_cast<std::size_t>(
            static_cast<double>(test.outputs)
            * StereoSampleRateConverter::kSourceSampleRate
            / test.rate);
        std::cerr
            << "SRC primed chunk1 rate=" << test.rate
            << " out=" << test.outputs
            << " calls=" << source.calls
            << " frames=" << source.frames
            << " expected=" << expected
            << " elapsed_ms=" << elapsed_ms << '\n';
        require(source.calls <= expected + 2, "chunk=1 callbacks exceeded ratio");
        require(source.frames == source.calls, "chunk=1 must pull 1 source frame");
        require(elapsed_ms < 50.0, "successful process() must stay cheap");
    }
}

void testPartialRenderFramesRefill() {
    struct PartialSource {
        std::size_t frame{};
        std::size_t calls{};
        std::size_t last_request{};
        std::size_t last_provided{};

        [[nodiscard]] static std::size_t render_frames(
            void* context,
            std::span<float> interleaved) noexcept {
            auto& source = *static_cast<PartialSource*>(context);
            ++source.calls;
            const auto want = interleaved.size() / 2;
            source.last_request = want;
            const auto provided = std::min(want, static_cast<std::size_t>(17));
            source.last_provided = provided;
            for (std::size_t frame = 0; frame < provided; ++frame) {
                interleaved[frame * 2] = 0.0F;
                interleaved[frame * 2 + 1] = 0.0F;
                ++source.frame;
            }
            return provided;
        }
    };

    StereoSampleRateConverter converter;
    require(converter.prepare(44'100.0, 256), "prepare failed");
    converter.primeWithSilence();
    PartialSource source;
    std::array<float, 128> left{};
    std::array<float, 128> right{};
    require(
        converter.process(
            left.data(),
            right.data(),
            left.size(),
            {
                .context = &source,
                .render = nullptr,
                .render_frames = &PartialSource::render_frames,
            }),
        "partial refill conversion failed");
    require(source.calls >= 1, "partial refill was not used");
    require(
        source.last_request == 256,
        "prepared chunk is still the maximum refill request");
    require(
        source.last_provided == 17,
        "callback may return fewer frames than the prepared chunk");
    require(
        source.frame == source.calls * 17,
        "partial refill must consume only the provided frames");
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
        testRate(88'200.0);
        testRate(96'000.0);
        testNonFiniteInputIsSilenced();
        testUpsamplingPhaseMovesForward();
        testRefillRequestsWholeChunk();
        testChunk1DoesNotPullLargeLookahead();
        testLargeChunkPullsFutureSourceImmediately();
        testImpulseGroupDelay();
        testResetClearsHistoryAndPhase();
        testConsumptionMatchesRatioPlusPrime();
        testPrimeWithSilenceConsumptionIsFloorRatio();
        testProcessReturnsWhenSourceMakesNoProgress();
        testProcessReturnsWhenSourceStopsMidBlock();
        testPrimedChunk1CallbackCountMatchesRatio();
        testPartialRenderFramesRefill();
        std::cout << "Stereo sample-rate converter tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
