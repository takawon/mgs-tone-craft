// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"

#include <algorithm>
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

    static constexpr int diagnosticMode() {
        return kVst3DiagMode;
    }
};

}  // namespace mgstc::plugin

namespace {

using Access = mgstc::plugin::MgstcAudioProcessorTestAccess;
using mgstc::plugin::kVst3DiagNon48Null;
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

void testNullModeCompilesAsNon48Null() {
    require(
        Access::diagnosticMode() == kVst3DiagNon48Null,
        "this executable must be built with NON48_NULL");
}

void test44100IsSilentAndDoesNotRunEngine() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    require(processor.getLatencySamples() == 7, "NULL keeps SRC latency");
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "44.1kHz NULL must be silent");
    require(Access::frame(processor) == 0, "44.1kHz NULL must not advance Engine");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "44.1kHz NULL must not schedule layers");
    require(
        Access::srcCallbacks(processor) == 0, "44.1kHz NULL must not run SRC");
    require(
        Access::renderAudioCalls(processor) == 0,
        "44.1kHz NULL must not call renderAudio");
}

void test96000IsSilentAndDoesNotRunEngine() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(96'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "96kHz NULL must be silent");
    require(Access::frame(processor) == 0, "96kHz NULL must not advance Engine");
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
        testNullModeCompilesAsNon48Null();
        test44100IsSilentAndDoesNotRunEngine();
        test96000IsSilentAndDoesNotRunEngine();
        test48kHzStillProducesAudio();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
