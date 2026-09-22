// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace mgstc::plugin {

struct MgstcAudioProcessorTestAccess {
    static std::uint64_t frame(const MgstcAudioProcessor& processor) {
        return processor.engine_frame_position_;
    }

    static std::uint32_t srcCallbacks(const MgstcAudioProcessor& processor) {
        return processor.src_callbacks_;
    }

    static std::uint32_t renderAudioCalls(
        const MgstcAudioProcessor& processor) {
        return processor.render_audio_calls_;
    }

    static const LayerDelayScheduler& scheduler(
        const MgstcAudioProcessor& processor) {
        return processor.scheduler_;
    }

    static std::uint64_t latencyCalls(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_calls_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyChanged(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_changed_.load(std::memory_order_relaxed);
    }

    static constexpr int diagnosticMode() {
        return kVst3DiagMode;
    }
};

}  // namespace mgstc::plugin

namespace {

using Access = mgstc::plugin::MgstcAudioProcessorTestAccess;
using mgstc::plugin::kVst3DiagNon48NullLat0;
using mgstc::plugin::MgstcAudioProcessor;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

double channelPeak(
    const juce::AudioBuffer<float>& buffer,
    int start,
    int count) {
    double peak = 0.0;
    const int end = std::min(start + count, buffer.getNumSamples());
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        const auto* samples = buffer.getReadPointer(channel);
        for (int i = start; i < end; ++i) {
            peak = std::max(peak, static_cast<double>(std::fabs(samples[i])));
        }
    }
    return peak;
}

void testLat0Mode() {
    require(
        Access::diagnosticMode() == kVst3DiagNon48NullLat0,
        "this executable must be built with NON48_NULL_LAT0");
}

void test44100IsSilentWithZeroLatency() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    require(processor.getLatencySamples() == 0, "LAT0 must report latency 0");
    require(
        Access::latencyChanged(processor) == 0,
        "LAT0 must not notify a non-zero latency change from the default 0");
    const auto after_prepare = Access::latencyCalls(processor);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "44.1kHz LAT0 must be silent");
    require(Access::frame(processor) == 0, "44.1kHz LAT0 must not advance Engine");
    require(
        Access::srcCallbacks(processor) == 0, "44.1kHz LAT0 must not run SRC");
    require(
        Access::renderAudioCalls(processor) == 0,
        "44.1kHz LAT0 must not call renderAudio");
    juce::AudioBuffer<float> more(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(more, empty);
    require(
        Access::latencyCalls(processor) == after_prepare,
        "LAT0 processBlock must not call setLatencySamples");
}

void test48kHzStillProducesAudio() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    require(processor.getLatencySamples() == 0, "48kHz latency stays 0");
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    double peak = channelPeak(buffer, 0, 512);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 512);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        peak = channelPeak(more, 0, 512);
    }
    require(peak > 0.001, "48kHz control path must still sound");
    require(Access::frame(processor) > 0, "48kHz must advance Engine");
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce_init;
    try {
        testLat0Mode();
        test44100IsSilentWithZeroLatency();
        test48kHzStillProducesAudio();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
