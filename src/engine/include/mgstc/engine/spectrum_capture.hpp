// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/spsc_queue.hpp"

namespace mgstc::engine {
constexpr std::size_t kSpectrumChannels = 22;
constexpr std::size_t kSpectrumBlockSize = 512;
constexpr double kSpectrumSampleRate = 48000.0;
constexpr std::size_t spectrumSource(std::size_t channel) noexcept {
    return channel < 3 ? 0 : channel < 8 ? 1 : 2;
}
using SpectrumChannelMap = std::array<std::uint8_t, kSpectrumChannels>;
constexpr SpectrumChannelMap identitySpectrumChannelMap() noexcept {
    SpectrumChannelMap map{};
    for (std::size_t i = 0; i < map.size(); ++i) map[i] = static_cast<std::uint8_t>(i);
    return map;
}
struct SpectrumGuideNote {
    std::uint8_t note{60};
    std::uint8_t track{};
    bool active{};
};
struct SpectrumCaptureBlock {
    std::array<std::array<float, kSpectrumBlockSize>, 3> sources{};
    std::array<std::array<float, kSpectrumBlockSize>, kSpectrumChannels> channels{};
    std::array<float, kSpectrumBlockSize> mixed{};
    std::array<float, kSpectrumBlockSize> output_gain{};
    std::array<SpectrumGuideNote, kSpectrumBlockSize> notes{};
    std::array<bool, kSpectrumBlockSize> channels_valid = [] {
        std::array<bool, kSpectrumBlockSize> flags{}; flags.fill(true); return flags;
    }();
    SpectrumChannelMap channel_map{identitySpectrumChannelMap()};
    std::uint64_t first_sample{};
    std::uint64_t context{}; // low three bits: analysis source mask
    std::uint64_t pcm_epoch{}; // sound generator reset; does not erase history
    bool simulated_remote{};
};

// One audio producer and one analyzer consumer. Owned/preallocated by the host,
// independent of the OPLL display queue. Every complete block is published,
// including when a single render call spans several blocks.
class SpectrumCapture {
public:
    static constexpr std::size_t kQueueCapacity = 32;
    void begin(std::uint64_t sample, std::uint64_t context, std::uint64_t epoch,
               const SpectrumChannelMap& map, SpectrumGuideNote guide,
               float output_gain, bool simulated_remote, bool channels_enabled = true) noexcept {
        channels_enabled_ = channels_enabled;
        if (sample != next_sample_ || context != work_.context
            || epoch != work_.pcm_epoch || map != work_.channel_map
            || simulated_remote != work_.simulated_remote) {
            position_ = 0;
        }
        next_sample_ = sample;
        work_.context = context;
        work_.pcm_epoch = epoch;
        work_.channel_map = map;
        work_.simulated_remote = simulated_remote;
        guide_ = guide;
        output_gain_ = output_gain;
    }
    bool channelsEnabled() const noexcept { return channels_enabled_; }
    std::uint8_t sourceMask() const noexcept { return static_cast<std::uint8_t>(work_.context & 7); }
    std::uint64_t context() const noexcept { return work_.context; }
    void append(const ChipSamples& chips, float mixed,
                float master, float psg_gain, float scc_gain, float opll_gain) noexcept {
        if (position_ == 0) work_.first_sample = next_sample_;
        const auto i = position_++;
        work_.sources[0][i] = chips.psg;
        work_.sources[1][i] = chips.scc;
        work_.sources[2][i] = chips.opll;
        const std::array<float, 3> gains{master * psg_gain, master * scc_gain, master * opll_gain};
        work_.channels_valid[i] = channels_enabled_;
        if (channels_enabled_)
            for (std::size_t ch = 0; ch < kSpectrumChannels; ++ch)
                work_.channels[ch][i] = chips.channels[ch] * gains[spectrumSource(ch)];
        work_.mixed[i] = mixed * output_gain_;
        work_.output_gain[i] = output_gain_;
        work_.notes[i] = guide_;
        ++next_sample_;
        if (position_ == kSpectrumBlockSize) {
            if (!queue_.tryPush(work_)) dropped_.fetch_add(1, std::memory_order_relaxed);
            position_ = 0;
        }
    }
    bool poll(SpectrumCaptureBlock& block) noexcept { return queue_.tryPop(block); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    SpscQueue<SpectrumCaptureBlock, kQueueCapacity> queue_;
    SpectrumCaptureBlock work_{};
    std::atomic<std::uint64_t> dropped_{};
    std::uint64_t next_sample_{};
    std::size_t position_{};
    SpectrumGuideNote guide_{};
    float output_gain_{1.0F};
    bool channels_enabled_{true};
};
} // namespace mgstc::engine
