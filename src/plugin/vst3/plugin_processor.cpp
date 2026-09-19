// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"

#include <algorithm>
#include <cmath>

#include "mgstc/engine/composite_timbre.hpp"

namespace mgstc::plugin {
namespace {

constexpr std::uint8_t kMaxVoices = 16;

}  // namespace

MgstcAudioProcessor::MgstcAudioProcessor()
    : juce::AudioProcessor(
          BusesProperties().withOutput(
              "Output", juce::AudioChannelSet::stereo(), true)) {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    auto edit = engine_.beginProgramEdit();
    if (!edit.valid()) {
        engine_.discardStuckProgramEdits();
        edit = engine_.beginProgramEdit();
    }
    if (!edit.valid()) {
        return;
    }
    if (!mgstc::engine::compileCompositeProgram(
            *edit.engine,
            timbre,
            {.polyphonic = true},
            &plan_)) {
        static_cast<void>(engine_.discardProgramEdit(edit));
        return;
    }
    voices_.setChannelCount(plan_.voice_capacity);
    voices_.setPolyphonic(true);
    program_ready_ = engine_.submitProgram(edit);
}

void MgstcAudioProcessor::prepareToPlay(
    double sampleRate,
    int /*samplesPerBlock*/) {
    if (program_ready_) {
        voices_.setChannelCount(plan_.voice_capacity);
        voices_.setPolyphonic(true);
        engine_.realtimeStop();
        std::array<std::uint8_t, kMaxVoices> ignored{};
        static_cast<void>(voices_.allNotesOff(ignored));
    }
    sample_rate_ok_ =
        std::abs(sampleRate - kEngineSampleRate) < 1.0 && program_ready_;
}

void MgstcAudioProcessor::releaseResources() {
    allSoundOff();
    sample_rate_ok_ = false;
}

bool MgstcAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const {
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled()) {
        return false;
    }
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MgstcAudioProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi) {
    buffer.clear();
    if (!sample_rate_ok_ || !program_ready_) {
        midi.clear();
        return;
    }

    engine_.servicePendingControlCommands();

    const int num_samples = buffer.getNumSamples();
    int cursor = 0;
    for (const auto metadata : midi) {
        int pos = metadata.samplePosition;
        if (pos < 0) {
            pos = 0;
        } else if (pos > num_samples) {
            pos = num_samples;
        }
        if (pos > cursor) {
            renderTo(buffer, cursor, pos - cursor);
            cursor = pos;
        }
        applyMidiMessage(metadata.getMessage());
    }
    if (cursor < num_samples) {
        renderTo(buffer, cursor, num_samples - cursor);
    }
    midi.clear();
}

void MgstcAudioProcessor::applyMidiMessage(
    const juce::MidiMessage& message) noexcept {
    if (message.isNoteOn(false)) {
        if (message.getVelocity() == 0) {
            noteOff(static_cast<std::uint8_t>(message.getNoteNumber()));
        } else {
            noteOn(static_cast<std::uint8_t>(message.getNoteNumber()));
        }
        return;
    }
    if (message.isNoteOff()) {
        noteOff(static_cast<std::uint8_t>(message.getNoteNumber()));
        return;
    }
    if (message.isAllSoundOff()) {
        allSoundOff();
        return;
    }
    if (message.isAllNotesOff()) {
        allNotesOff();
    }
}

void MgstcAudioProcessor::noteOn(std::uint8_t midi_note) noexcept {
    const auto assignment = voices_.noteOn(midi_note);
    if (assignment.stolen_note) {
        stopVoice(assignment.channel, false);
    }
    startVoice(assignment.channel, midi_note);
}

void MgstcAudioProcessor::noteOff(std::uint8_t midi_note) noexcept {
    const auto voice = voices_.noteOff(midi_note);
    if (!voice) {
        return;
    }
    stopVoice(*voice, false);
}

void MgstcAudioProcessor::stopVoice(std::uint8_t voice, bool hard) noexcept {
    for (const auto& binding : plan_.audible_layers) {
        const auto track = plan_.physicalTrack(binding, voice);
        if (hard) {
            static_cast<void>(engine_.realtimeSilenceTrack(track));
        } else {
            static_cast<void>(engine_.realtimeNoteOff(track));
        }
    }
}

void MgstcAudioProcessor::startVoice(
    std::uint8_t voice,
    std::uint8_t midi_note) noexcept {
    for (const auto& binding : plan_.audible_layers) {
        if (binding.start_delay_ms > 0.0) {
            continue;
        }
        const auto note = mgstc::engine::transposedMidiNote(
            midi_note, binding.relative_semitones);
        if (!note) {
            continue;
        }
        const auto track = plan_.physicalTrack(binding, voice);
        static_cast<void>(engine_.realtimeNoteOn(track, *note));
    }
}

void MgstcAudioProcessor::allNotesOff() noexcept {
    std::array<std::uint8_t, kMaxVoices> voices{};
    const auto count = std::min(voices_.allNotesOff(voices), voices.size());
    for (std::size_t index = 0; index < count; ++index) {
        stopVoice(voices[index], false);
    }
}

void MgstcAudioProcessor::allSoundOff() noexcept {
    std::array<std::uint8_t, kMaxVoices> voices{};
    static_cast<void>(voices_.allNotesOff(voices));
    engine_.realtimeStop();
}

void MgstcAudioProcessor::renderTo(
    juce::AudioBuffer<float>& buffer,
    int start_frame,
    int frame_count) noexcept {
    if (frame_count <= 0) {
        return;
    }
    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1
        ? buffer.getWritePointer(1)
        : nullptr;
    int remaining = frame_count;
    int dest = start_frame;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kScratchFrames);
        const auto result = engine_.renderAudio(
            std::span<float>(
                scratch_.data(),
                static_cast<std::size_t>(chunk) * 2));
        const int produced = static_cast<int>(
            std::min(result.frames, static_cast<std::size_t>(chunk)));
        for (int frame = 0; frame < produced; ++frame) {
            left[dest + frame] = scratch_[static_cast<std::size_t>(frame) * 2];
            if (right != nullptr) {
                right[dest + frame] =
                    scratch_[static_cast<std::size_t>(frame) * 2 + 1];
            }
        }
        if (!result.ok() || produced < chunk) {
            silenceFrom(buffer, dest + produced);
            return;
        }
        dest += produced;
        remaining -= produced;
    }
}

void MgstcAudioProcessor::silenceFrom(
    juce::AudioBuffer<float>& buffer,
    int start_frame) noexcept {
    const int count = buffer.getNumSamples() - start_frame;
    if (count <= 0) {
        return;
    }
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        buffer.clear(channel, start_frame, count);
    }
}

juce::AudioProcessorEditor* MgstcAudioProcessor::createEditor() {
    return nullptr;
}

bool MgstcAudioProcessor::hasEditor() const {
    return false;
}

const juce::String MgstcAudioProcessor::getName() const {
    return "MGS Tone Craft";
}

bool MgstcAudioProcessor::acceptsMidi() const {
    return true;
}

bool MgstcAudioProcessor::producesMidi() const {
    return false;
}

bool MgstcAudioProcessor::isMidiEffect() const {
    return false;
}

double MgstcAudioProcessor::getTailLengthSeconds() const {
    return 0.0;
}

int MgstcAudioProcessor::getNumPrograms() {
    return 1;
}

int MgstcAudioProcessor::getCurrentProgram() {
    return 0;
}

void MgstcAudioProcessor::setCurrentProgram(int /*index*/) {}

const juce::String MgstcAudioProcessor::getProgramName(int /*index*/) {
    return "Default";
}

void MgstcAudioProcessor::changeProgramName(
    int /*index*/,
    const juce::String& /*newName*/) {}

void MgstcAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    destData.reset();
}

void MgstcAudioProcessor::setStateInformation(
    const void* /*data*/,
    int /*sizeInBytes*/) {}

}  // namespace mgstc::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new mgstc::plugin::MgstcAudioProcessor();
}
