// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

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

void prime(mgstc::plugin::MgstcAudioProcessor& processor, int block) {
    processor.prepareToPlay(48'000.0, block);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

void testNoteOnAtSample128DoesNotAffectPrefix() {
    mgstc::plugin::MgstcAudioProcessor processor;
    constexpr int kBlock = 2048;
    prime(processor, kBlock);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        128);
    processor.processBlock(buffer, midi);
    require(
        channelPeak(buffer, 0, 128) < 0.001,
        "samples before MIDI note on were not silent");

    double later = channelPeak(buffer, 128, kBlock - 128);
    if (later <= 0.001) {
        juce::AudioBuffer<float> more(2, kBlock);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        later = channelPeak(more, 0, kBlock);
    }
    require(later > 0.001, "note on at sample 128 produced no audio");
}

void testNon48kHzIsSilentAndDoesNotAdvanceEngine() {
    mgstc::plugin::MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "44.1kHz output was not silent");

    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> live(2, 2048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(live, on);
    double peak = channelPeak(live, 0, 2048);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 2048);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        peak = channelPeak(more, 0, 2048);
    }
    require(peak > 0.001, "returning to 48kHz should play");
}

void testTwoProcessorsDoNotShareVoiceState() {
    mgstc::plugin::MgstcAudioProcessor a;
    mgstc::plugin::MgstcAudioProcessor b;
    prime(a, 512);
    prime(b, 512);

    juce::AudioBuffer<float> buffer_a(2, 2048);
    juce::AudioBuffer<float> buffer_b(2, 2048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    juce::MidiBuffer empty;
    a.processBlock(buffer_a, on);
    b.processBlock(buffer_b, empty);

    double peak_a = channelPeak(buffer_a, 0, 2048);
    if (peak_a <= 0.001) {
        juce::AudioBuffer<float> more(2, 2048);
        juce::MidiBuffer rest;
        a.processBlock(more, rest);
        peak_a = channelPeak(more, 0, 2048);
    }
    require(peak_a > 0.001, "processor A should sound");
    require(
        channelPeak(buffer_b, 0, 2048) < 0.001,
        "processor B should stay silent");
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce_init;
    try {
        testNoteOnAtSample128DoesNotAffectPrefix();
        testNon48kHzIsSilentAndDoesNotAdvanceEngine();
        testTwoProcessorsDoNotShareVoiceState();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
