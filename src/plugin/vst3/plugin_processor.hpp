// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstdint>

#include <juce_audio_processors/juce_audio_processors.h>

#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/voice_allocator.hpp"

namespace mgstc::plugin {

class MgstcAudioProcessor final : public juce::AudioProcessor {
public:
    static constexpr double kEngineSampleRate = 48'000.0;
    static constexpr int kScratchFrames = 512;

    MgstcAudioProcessor();
    ~MgstcAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    void applyMidiMessage(const juce::MidiMessage& message) noexcept;
    void noteOn(std::uint8_t midi_note) noexcept;
    void noteOff(std::uint8_t midi_note) noexcept;
    void stopVoice(std::uint8_t voice, bool hard) noexcept;
    void startVoice(std::uint8_t voice, std::uint8_t midi_note) noexcept;
    void allNotesOff() noexcept;
    void allSoundOff() noexcept;
    void renderTo(
        juce::AudioBuffer<float>& buffer,
        int start_frame,
        int frame_count) noexcept;
    void silenceFrom(
        juce::AudioBuffer<float>& buffer,
        int start_frame) noexcept;

    mgstc::engine::RealtimeEngineHost engine_{};
    mgstc::engine::SequentialVoiceAllocator voices_{1};
    mgstc::engine::CompositePlaybackPlan plan_{};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2>
        scratch_{};
    bool sample_rate_ok_{false};
    bool program_ready_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MgstcAudioProcessor)
};

}  // namespace mgstc::plugin
