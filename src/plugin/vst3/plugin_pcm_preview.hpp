// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>
#include "mgstc/engine/composite_wav_analysis.hpp"

namespace mgstc::plugin {

// One message-thread producer, one audio consumer. Audio never copies or drops
// shared ownership. CAS ownership protects even superseded pending requests;
// reusable PCM is reclaimed only by a later message-thread submission/destructor.
class PluginPcmPreview final {
public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    bool play(std::shared_ptr<const mgstc::engine::SourcePcm> pcm,
        std::size_t begin, std::size_t end, float gain) {
        const auto request_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!pcm || !pcm->channels || !pcm->sample_rate || begin >= end
            || end > pcm->mono_samples.size() || !std::isfinite(gain) || gain < 0
            || pcm->interleaved_samples.size() / pcm->channels < end) return false;
        // Public boundary validates immutable input off the audio thread.
        for (std::size_t i = begin * pcm->channels; i < end * pcm->channels; ++i)
            if (!std::isfinite(pcm->interleaved_samples[i])) return false;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto state = slots_[i].state.load(std::memory_order_acquire);
            if (state != Reusable && state != Queued) continue;
            if (!slots_[i].state.compare_exchange_strong(state, Writing,
                    std::memory_order_acq_rel)) continue;
            auto& slot = slots_[i];
            slot.pcm = std::move(pcm);
            slot.begin = begin; slot.end = end; slot.gain = gain;
            slot.epoch = request_epoch;
            slot.serial = ++serial_;
            slot.state.store(Queued, std::memory_order_release);
            published_.store((slot.serial << 8) | i, std::memory_order_release);
            return true;
        }
        return false;
    }

    // Also safe for host reset/release. No PCM destruction or ownership transfer.
    void stop() noexcept { epoch_.fetch_add(1, std::memory_order_acq_rel); }

    void render(juce::AudioBuffer<float>& output, double host_rate) noexcept {
        const auto token = published_.load(std::memory_order_acquire);
        if (token != consumed_) {
            retireActive();
            consumed_ = token;
            const auto index = static_cast<std::size_t>(token & 0xff);
            if (index < slots_.size()) {
                auto state = Queued;
                if (slots_[index].state.compare_exchange_strong(state, Active,
                        std::memory_order_acq_rel)) {
                    if (slots_[index].serial == token >> 8) {
                        active_ = index;
                        position_ = static_cast<double>(slots_[index].begin);
                    } else {
                        // A newer request reused the queued slot before its
                        // publication. Leave that request available to claim.
                        slots_[index].state.store(Queued, std::memory_order_release);
                    }
                }
            }
        }
        if (active_ == slots_.size()) return;
        const auto& slot = slots_[active_];
        if (slot.epoch != epoch_.load(std::memory_order_acquire)
            || !std::isfinite(host_rate) || host_rate <= 0) {
            retireActive(); return;
        }
        const auto& pcm = *slot.pcm;
        const auto step = static_cast<double>(pcm.sample_rate) / host_rate;
        output.clear();
        for (int frame = 0; frame < output.getNumSamples()
                && position_ < static_cast<double>(slot.end); ++frame) {
            const auto left = static_cast<std::size_t>(position_);
            const auto right = std::min(left + 1, slot.end - 1);
            const auto fraction = position_ - static_cast<double>(left);
            for (int channel = 0; channel < output.getNumChannels(); ++channel) {
                const auto source_channel = std::min(static_cast<std::size_t>(channel),
                    static_cast<std::size_t>(pcm.channels - 1));
                const auto a = static_cast<double>(pcm.interleaved_samples[left * pcm.channels + source_channel]);
                const auto b = static_cast<double>(pcm.interleaved_samples[right * pcm.channels + source_channel]);
                output.setSample(channel, frame, static_cast<float>(std::clamp(
                    (a + fraction * (b - a)) * slot.gain, -1.0, 1.0)));
            }
            position_ += step;
        }
        if (position_ >= static_cast<double>(slot.end)) retireActive();
    }

private:
    enum State : unsigned { Reusable, Writing, Queued, Active };
    static_assert(std::atomic<State>::is_always_lock_free);
    struct Slot {
        std::atomic<State> state{Reusable};
        std::shared_ptr<const mgstc::engine::SourcePcm> pcm;
        std::size_t begin{}, end{};
        float gain{1};
        std::uint64_t epoch{}, serial{};
    };
    void retireActive() noexcept {
        if (active_ < slots_.size()) slots_[active_].state.store(Reusable, std::memory_order_release);
        active_ = slots_.size();
    }
    std::array<Slot, 4> slots_;
    std::atomic<std::uint64_t> epoch_{0}, published_{0};
    std::uint64_t serial_{}, consumed_{};
    std::size_t active_{4};
    double position_{};
};

} // namespace mgstc::plugin
