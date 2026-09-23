// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"
#include "plugin_editor.hpp"
#include "plugin_editor_context.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "plugin_state.hpp"
#include "mgstc/engine/dc_blocker.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/register_write.hpp"
#include "mgstc/engine/sinc_rate_conv.hpp"

namespace mgstc::plugin {

struct MgstcAudioProcessorTestAccess {
    static std::uint64_t frame(const MgstcAudioProcessor& processor) {
        return processor.engine_frame_position_;
    }

    static const LayerDelayScheduler& scheduler(
        const MgstcAudioProcessor& processor) {
        return processor.scheduler_;
    }

    static const mgstc::engine::CompositePlaybackPlan& plan(
        const MgstcAudioProcessor& processor) {
        return processor.activePlan();
    }

    static std::size_t noteOnCount(const MgstcAudioProcessor& processor) {
        return processor.note_on_log_size_;
    }

    static std::uint64_t hostFrame(const MgstcAudioProcessor& processor) {
        return processor.host_frame_position_;
    }

    static std::uint32_t hostRate(const MgstcAudioProcessor& processor) {
        return processor.host_rate_hz_;
    }

    static bool usesSrc(const MgstcAudioProcessor& processor) {
        return processor.use_src_;
    }

    static bool logHas(
        const MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t track) {
        for (std::size_t index = 0; index < processor.note_on_log_size_;
             ++index) {
            const auto& entry = processor.note_on_log_[index];
            if (entry.frame == frame && entry.physical_track == track) {
                return true;
            }
        }
        return false;
    }

    static bool logHasTrack(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        for (std::size_t index = 0; index < processor.note_on_log_size_;
             ++index) {
            if (processor.note_on_log_[index].physical_track == track) {
                return true;
            }
        }
        return false;
    }

    static bool isInactive(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        return processor.track_state_[track]
            == MgstcAudioProcessor::TrackLayerState::Inactive;
    }

    static bool isActive(
        const MgstcAudioProcessor& processor,
        std::uint8_t track) {
        return processor.track_state_[track]
            == MgstcAudioProcessor::TrackLayerState::Active;
    }

    static std::uint32_t srcCallbacks(const MgstcAudioProcessor& processor) {
        return processor.src_callbacks_;
    }

    static std::uint32_t srcFillIters(const MgstcAudioProcessor& processor) {
        return processor.src_fill_iters_;
    }

    static std::uint32_t srcZeroProgress(const MgstcAudioProcessor& processor) {
        return processor.src_zero_progress_;
    }

    static std::uint32_t srcKnownEndRejects(
        const MgstcAudioProcessor& processor) {
        return processor.src_known_end_rejects_;
    }

    static std::uint32_t srcWatchdogTrips(
        const MgstcAudioProcessor& processor) {
        return processor.src_watchdog_trips_;
    }

    static std::uint64_t srcStallEngine(const MgstcAudioProcessor& processor) {
        return processor.src_stall_engine_;
    }

    static std::uint64_t srcStallMapped(const MgstcAudioProcessor& processor) {
        return processor.src_stall_mapped_;
    }

    static std::uint64_t srcStallKnownEnd(
        const MgstcAudioProcessor& processor) {
        return processor.src_stall_known_end_;
    }

    static std::uint64_t knownEngineEnd(
        const MgstcAudioProcessor& processor) {
        return processor.known_engine_end_;
    }

    static std::uint64_t srcProcessNs(const MgstcAudioProcessor& processor) {
        return processor.src_process_ns_;
    }

    static std::uint64_t srcFillNs(const MgstcAudioProcessor& processor) {
        return processor.src_fill_ns_;
    }

    static std::uint64_t srcRenderNs(const MgstcAudioProcessor& processor) {
        return processor.src_render_ns_;
    }

    static std::uint64_t copyNs(const MgstcAudioProcessor& processor) {
        return processor.copy_ns_;
    }

    static std::uint32_t renderAudioCalls(
        const MgstcAudioProcessor& processor) {
        return processor.render_audio_calls_;
    }

    static bool pluginQuiescent(const MgstcAudioProcessor& processor) {
        return processor.pluginQuiescent();
    }

    static bool hasRealtimeWork(const MgstcAudioProcessor& processor) {
        return processor.engine_.hasRealtimeWork();
    }

    static std::uint64_t processBlockCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_process_block_.load(std::memory_order_relaxed);
    }

    static std::uint64_t prepareToPlayCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_prepare_to_play_.load(std::memory_order_relaxed);
    }

    static std::uint64_t releaseResourcesCount(
        const MgstcAudioProcessor& processor) {
        return processor.diag_release_resources_.load(
            std::memory_order_relaxed);
    }

    static std::uint64_t resetCount(const MgstcAudioProcessor& processor) {
        return processor.diag_reset_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyCalls(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_calls_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencySame(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_same_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyChanged(const MgstcAudioProcessor& processor) {
        return processor.diag_latency_changed_.load(std::memory_order_relaxed);
    }

    static std::uint64_t latencyFromProcess(
        const MgstcAudioProcessor& processor) {
        return processor.diag_latency_from_process_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastNumSamples(const MgstcAudioProcessor& processor) {
        return processor.diag_last_num_samples_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastBufferChannels(
        const MgstcAudioProcessor& processor) {
        return processor.diag_last_buffer_channels_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t lastMidiEvents(const MgstcAudioProcessor& processor) {
        return processor.diag_last_midi_events_.load(
            std::memory_order_relaxed);
    }

    static std::uint32_t processSampleRateHz(
        const MgstcAudioProcessor& processor) {
        return processor.diag_process_sample_rate_hz_.load(
            std::memory_order_relaxed);
    }

    static std::uint64_t sampleRateChangesInProcess(
        const MgstcAudioProcessor& processor) {
        return processor.diag_sample_rate_changes_in_process_.load(
            std::memory_order_relaxed);
    }

    static constexpr int diagnosticMode() {
        return kVst3DiagMode;
    }

    static void setEngineFrame(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        processor.engine_frame_position_ = frame;
    }

    static std::size_t mappedIndex(const MgstcAudioProcessor& processor) {
        return processor.mapped_index_;
    }

    static std::size_t mappedCount(const MgstcAudioProcessor& processor) {
        return processor.mapped_count_;
    }

    static void resetMapped(MgstcAudioProcessor& processor) {
        processor.mapped_count_ = 0;
        processor.mapped_index_ = 0;
    }

    static bool plantMapped(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        MgstcAudioProcessor::MappedMidiType type,
        std::uint8_t note) {
        if (processor.mapped_count_ >= processor.mapped_events_.size()) {
            return false;
        }
        auto& event = processor.mapped_events_[processor.mapped_count_++];
        event.engine_frame = frame;
        event.type = type;
        event.midi_note = note;
        return true;
    }

    static bool plantNoteOn(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t note) {
        return plantMapped(
            processor, frame, MgstcAudioProcessor::MappedMidiType::NoteOn, note);
    }

    static bool plantNoteOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame,
        std::uint8_t note) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::NoteOff,
            note);
    }

    static bool plantAllNotesOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::AllNotesOff,
            0);
    }

    static bool plantAllSoundOff(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        return plantMapped(
            processor,
            frame,
            MgstcAudioProcessor::MappedMidiType::AllSoundOff,
            0);
    }

    static void processEventsAt(
        MgstcAudioProcessor& processor,
        std::uint64_t frame) {
        processor.engine_frame_position_ = frame;
        processor.processEventsAt(frame);
    }

    static void bindMidiIterator(
        MgstcAudioProcessor& processor,
        juce::MidiBuffer& midi,
        std::uint64_t host_block_start,
        int host_block_samples) {
        processor.mapped_count_ = 0;
        processor.mapped_index_ = 0;
        processor.midi_it_ = midi.begin();
        processor.midi_end_ = midi.end();
        processor.src_host_block_start_ = host_block_start;
        processor.src_host_block_samples_ = host_block_samples;
        processor.use_src_ = false;
    }

    static void clearMidiIterator(MgstcAudioProcessor& processor) {
        processor.midi_it_ = {};
        processor.midi_end_ = {};
    }

    static void renderDirect(
        MgstcAudioProcessor& processor,
        juce::AudioBuffer<float>& buffer,
        std::uint64_t block_start) {
        processor.engine_frame_position_ = block_start;
        processor.known_engine_end_ =
            block_start + static_cast<std::uint64_t>(buffer.getNumSamples());
        processor.resetBlockDiagnostics();
        processor.renderDirectSpan(buffer, block_start);
    }

    static std::size_t logSize(const MgstcAudioProcessor& processor) {
        return processor.note_on_log_size_;
    }

    static std::uint64_t logFrame(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].frame;
    }

    static std::uint8_t logTrack(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].physical_track;
    }

    static std::uint8_t logNote(
        const MgstcAudioProcessor& processor,
        std::size_t index) {
        return processor.note_on_log_[index].midi_note;
    }

    static std::uint64_t blockedBackendCalls(
        const MgstcAudioProcessor& processor) {
        return processor.editor_blocked_backend_calls_;
    }

    static bool scopeEnabled(const MgstcAudioProcessor& processor) {
        return processor.engine_.opllScopeEnabled();
    }

    static std::uint64_t stateCompileCount(
        const MgstcAudioProcessor& processor) {
        return processor.state_compile_count_.load(std::memory_order_relaxed);
    }

    static std::uint64_t stateAudioRejectCount(
        const MgstcAudioProcessor& processor) {
        return processor.state_audio_reject_count_.load(
            std::memory_order_relaxed);
    }

    static void poseAsAudioThread(MgstcAudioProcessor& processor) {
        processor.poseAsAudioThreadForTest();
    }

    static void clearAudioThread(MgstcAudioProcessor& processor) {
        processor.clearAudioThreadForTest();
    }
};

}  // namespace mgstc::plugin

namespace {

using Access = mgstc::plugin::MgstcAudioProcessorTestAccess;
using mgstc::plugin::delayMillisecondsToEngineFrames;
using mgstc::plugin::hostFrameToEngineFrame;
using mgstc::plugin::kVst3DiagOff;
using mgstc::plugin::MgstcAudioProcessor;
using mgstc::plugin::PluginEditorContext;

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

void prime(MgstcAudioProcessor& processor, int block) {
    processor.prepareToPlay(48'000.0, block);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
}

const mgstc::engine::CompositePlaybackLayer* layerOf(
    const mgstc::engine::CompositePlaybackPlan& plan,
    mgstc::engine::TimbreSource source) {
    for (const auto& layer : plan.audible_layers) {
        if (layer.source == source) {
            return &layer;
        }
    }
    return nullptr;
}

void processEmpty(MgstcAudioProcessor& processor, int frames) {
    while (frames > 0) {
        const int chunk = std::min(frames, 512);
        juce::AudioBuffer<float> buffer(2, chunk);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        frames -= chunk;
    }
}

void processUntilEngine(
    MgstcAudioProcessor& processor,
    std::uint64_t target,
    int host_block = 512) {
    while (Access::frame(processor) < target) {
        juce::AudioBuffer<float> buffer(2, host_block);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
}

void processUntilFrame(MgstcAudioProcessor& processor, std::uint64_t target) {
    while (Access::frame(processor) < target) {
        const auto remaining = target - Access::frame(processor);
        const int chunk = static_cast<int>(
            std::min<std::uint64_t>(remaining, 512));
        juce::AudioBuffer<float> buffer(2, chunk);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
    }
}

void testNoteOnAtSample128DoesNotAffectPrefix() {
    MgstcAudioProcessor processor;
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

void processSilence(
    MgstcAudioProcessor& processor,
    int host_frames,
    int block) {
    while (host_frames > 0) {
        const int n = std::min(host_frames, block);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        host_frames -= n;
    }
}

std::string srcDiag(const MgstcAudioProcessor& processor) {
    char line[768];
    std::snprintf(
        line,
        sizeof(line),
        "rate=%u host=%llu engine=%llu cb=%u fill_iters=%u zero_progress=%u "
        "known_end_rej=%u watchdog=%u stall_engine=%llu stall_mapped=%llu "
        "stall_known_end=%llu",
        Access::hostRate(processor),
        static_cast<unsigned long long>(Access::hostFrame(processor)),
        static_cast<unsigned long long>(Access::frame(processor)),
        Access::srcCallbacks(processor),
        Access::srcFillIters(processor),
        Access::srcZeroProgress(processor),
        Access::srcKnownEndRejects(processor),
        Access::srcWatchdogTrips(processor),
        static_cast<unsigned long long>(Access::srcStallEngine(processor)),
        static_cast<unsigned long long>(Access::srcStallMapped(processor)),
        static_cast<unsigned long long>(Access::srcStallKnownEnd(processor)));
    return line;
}

void testCubaseSilenceThenFirstNoteOn(
    double rate,
    int block,
    int silence_host,
    int note_offset = 0) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);
    processSilence(processor, silence_host, block);

    const int offset = std::clamp(note_offset, 0, std::max(block - 1, 0));
    const auto host_before = Access::hostFrame(processor);
    const auto engine_before = Access::frame(processor);
    const auto mapped_host = host_before + static_cast<std::uint64_t>(offset);
    const auto mapped_note = hostFrameToEngineFrame(
        mapped_host, Access::hostRate(processor));

    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        offset);
    const auto started = std::chrono::steady_clock::now();
    processor.processBlock(buffer, midi);
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();

    std::fprintf(
        stderr,
        "Cubase first-note rate=%.1f block=%d silence=%d offset=%d "
        "host_before=%llu engine_before=%llu mapped=%llu "
        "known_end=%llu elapsed_ms=%.3f %s\n",
        rate,
        block,
        silence_host,
        offset,
        static_cast<unsigned long long>(host_before),
        static_cast<unsigned long long>(engine_before),
        static_cast<unsigned long long>(mapped_note),
        static_cast<unsigned long long>(Access::knownEngineEnd(processor)),
        elapsed_ms,
        srcDiag(processor).c_str());

    require(
        elapsed_ms < 250.0,
        "first Note On processBlock did not return in bounded time");
    if (Access::srcWatchdogTrips(processor) != 0) {
        throw std::runtime_error(
            std::string("SRC zero-progress infinite loop at first Note On: ")
            + srcDiag(processor));
    }
    const auto fill = Access::srcFillIters(processor);
    require(
        fill < 64,
        "source generation iterated excessively at first Note On");
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    require(
        Access::logHasTrack(processor, track),
        "first Note On after silence must dispatch PSG");
}

void measureSilenceVersusNoteOnLoad(double rate, int block) {
    MgstcAudioProcessor silent;
    silent.prepareToPlay(rate, block);
    juce::AudioBuffer<float> silence_buf(2, block);
    juce::MidiBuffer empty;
    const auto silence_started = std::chrono::steady_clock::now();
    silent.processBlock(silence_buf, empty);
    const auto silence_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - silence_started)
            .count();
    const auto silence_cb = Access::srcCallbacks(silent);

    MgstcAudioProcessor sounding;
    sounding.prepareToPlay(rate, block);
    juce::AudioBuffer<float> note_buf(2, block);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    const auto note_started = std::chrono::steady_clock::now();
    sounding.processBlock(note_buf, on);
    const auto note_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - note_started)
            .count();
    const auto note_cb = Access::srcCallbacks(sounding);

    std::fprintf(
        stderr,
        "SRC load rate=%.1f block=%d silence_ms=%.3f silence_cb=%u "
        "note_ms=%.3f note_cb=%u zero_progress=%u watchdog=%u\n",
        rate,
        block,
        silence_ms,
        silence_cb,
        note_ms,
        note_cb,
        Access::srcZeroProgress(sounding),
        Access::srcWatchdogTrips(sounding));

    require(note_cb <= silence_cb + 2, "first Note On must not explode SRC callbacks");
    require(
        Access::srcWatchdogTrips(sounding) == 0,
        "first Note On load probe tripped zero-progress watchdog");
}

void testCubaseFirstNoteOnAfterSilence() {
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 64, 44'100);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 44'100);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 64, 96'000);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 96'000);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 1);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 7);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 128);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 512, 0, 511);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0, 1);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 512, 0, 511);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 1, 0, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 1, 0, 0);
    testCubaseSilenceThenFirstNoteOn(44'100.0, 32, 0, 0);
    testCubaseSilenceThenFirstNoteOn(96'000.0, 32, 0, 0);
    measureSilenceVersusNoteOnLoad(44'100.0, 512);
    measureSilenceVersusNoteOnLoad(96'000.0, 512);
    measureSilenceVersusNoteOnLoad(48'000.0, 512);
}

void testEngineNeverLeadsHostMapping() {
    constexpr std::array rates{44'100.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto host_rate = Access::hostRate(processor);
        int remaining = static_cast<int>(rate);
        std::uint32_t rejects = 0;
        while (remaining > 0) {
            const int n = std::min(remaining, 512);
            juce::AudioBuffer<float> buffer(2, n);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            remaining -= n;
            rejects += Access::srcKnownEndRejects(processor);
            const auto host = Access::hostFrame(processor);
            const auto engine = Access::frame(processor);
            const auto mapped = hostFrameToEngineFrame(host, host_rate);
            if (engine > mapped) {
                throw std::runtime_error(
                    "engine lead host mapping: rate="
                    + std::to_string(rate)
                    + " host=" + std::to_string(host)
                    + " engine=" + std::to_string(engine)
                    + " mapped=" + std::to_string(mapped));
            }
        }
        std::fprintf(
            stderr,
            "SRC known_end cap rate=%.1f host=%llu engine=%llu mapped=%llu "
            "rejects=%u\n",
            rate,
            static_cast<unsigned long long>(Access::hostFrame(processor)),
            static_cast<unsigned long long>(Access::frame(processor)),
            static_cast<unsigned long long>(hostFrameToEngineFrame(
                Access::hostFrame(processor), host_rate)),
            rejects);
    }
}

enum class RtLoad {
    Idle,
    OneVoice,
    Poly,
    Release,
};

const char* rtLoadName(RtLoad load) {
    switch (load) {
    case RtLoad::Idle:
        return "idle";
    case RtLoad::OneVoice:
        return "1voice";
    case RtLoad::Poly:
        return "poly";
    case RtLoad::Release:
        return "release";
    }
    return "unknown";
}

void reportRealtimeDeadlineCost(
    double rate,
    int block,
    RtLoad load) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);

    constexpr int kWarmup = 16;
    constexpr int kMeasure = 64;
    processSilence(processor, kWarmup * block, block);

    if (load == RtLoad::OneVoice || load == RtLoad::Release) {
        juce::AudioBuffer<float> on_buf(2, block);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(on_buf, on);
        processSilence(processor, 8 * block, block);
    } else if (load == RtLoad::Poly) {
        juce::AudioBuffer<float> on_buf(2, block);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        on.addEvent(
            juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)),
            1);
        on.addEvent(
            juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)),
            2);
        processor.processBlock(on_buf, on);
        processSilence(processor, 8 * block, block);
    }
    if (load == RtLoad::Release) {
        juce::AudioBuffer<float> off_buf(2, block);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
        processor.processBlock(off_buf, off);
    }

    double process_sum_ms = 0.0;
    double process_max_ms = 0.0;
    double src_sum_ms = 0.0;
    double fill_sum_ms = 0.0;
    double render_sum_ms = 0.0;
    double copy_sum_ms = 0.0;
    std::uint32_t cb_sum = 0;
    std::uint32_t render_sum = 0;
    for (int i = 0; i < kMeasure; ++i) {
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        const auto started = std::chrono::steady_clock::now();
        processor.processBlock(buffer, midi);
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        process_sum_ms += elapsed_ms;
        process_max_ms = std::max(process_max_ms, elapsed_ms);
        src_sum_ms += static_cast<double>(Access::srcProcessNs(processor))
            / 1.0e6;
        fill_sum_ms += static_cast<double>(Access::srcFillNs(processor))
            / 1.0e6;
        render_sum_ms += static_cast<double>(Access::srcRenderNs(processor))
            / 1.0e6;
        copy_sum_ms += static_cast<double>(Access::copyNs(processor))
            / 1.0e6;
        cb_sum += Access::srcCallbacks(processor);
        render_sum += Access::renderAudioCalls(processor);
    }

    const double deadline_ms =
        1.0e3 * static_cast<double>(block) / rate;
    const double process_mean = process_sum_ms / kMeasure;
    const double src_mean = src_sum_ms / kMeasure;
    const double fill_mean = fill_sum_ms / kMeasure;
    const double render_mean = render_sum_ms / kMeasure;
    const double sinc_mean = std::max(0.0, src_mean - fill_mean);
    const double fill_overhead = std::max(0.0, fill_mean - render_mean);
    const double copy_mean = copy_sum_ms / kMeasure;
#ifdef NDEBUG
    const char* config = "Release";
#else
    const char* config = "Debug";
#endif
    std::fprintf(
        stderr,
        "RT cost config=%s rate=%.1f block=%d load=%s path=%s "
        "deadline_ms=%.3f process_mean_ms=%.3f process_max_ms=%.3f "
        "ratio_mean=%.3f ratio_max=%.3f src_ms=%.3f fill_ms=%.3f "
        "render_ms=%.3f copy_ms=%.3f sinc_ms=%.3f fill_overhead_ms=%.3f "
        "cb_mean=%.1f render_calls_mean=%.1f\n",
        config,
        rate,
        block,
        rtLoadName(load),
        Access::usesSrc(processor) ? "src-batch" : "direct-48k",
        deadline_ms,
        process_mean,
        process_max_ms,
        process_mean / deadline_ms,
        process_max_ms / deadline_ms,
        src_mean,
        fill_mean,
        render_mean,
        copy_mean,
        sinc_mean,
        fill_overhead,
        static_cast<double>(cb_sum) / kMeasure,
        static_cast<double>(render_sum) / kMeasure);
}

void testRealtimeDeadlineCost() {
    constexpr std::array blocks{64, 128, 256, 512, 1024};
    constexpr std::array loads{
        RtLoad::Idle,
        RtLoad::OneVoice,
        RtLoad::Poly,
        RtLoad::Release,
    };
    for (const auto block : blocks) {
        for (const auto load : loads) {
            reportRealtimeDeadlineCost(48'000.0, block, load);
        }
    }
    reportRealtimeDeadlineCost(44'100.0, 512, RtLoad::Idle);
    reportRealtimeDeadlineCost(44'100.0, 512, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(44'100.0, 256, RtLoad::Idle);
    reportRealtimeDeadlineCost(44'100.0, 256, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(96'000.0, 512, RtLoad::Idle);
    reportRealtimeDeadlineCost(96'000.0, 512, RtLoad::OneVoice);
    reportRealtimeDeadlineCost(96'000.0, 256, RtLoad::Idle);
    reportRealtimeDeadlineCost(96'000.0, 256, RtLoad::OneVoice);
}

void testInvalidSampleRateIsSilentAndDoesNotAdvanceEngine() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(0.0, 512);
    require(Access::frame(processor) == 0, "prepare at 0 Hz must not run");
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    require(channelPeak(buffer, 0, 512) < 0.001, "0 Hz output was not silent");
    require(
        Access::frame(processor) == 0,
        "invalid rate must not advance engine timeline");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "invalid rate must not schedule delayed layers");

    processor.prepareToPlay(
        std::numeric_limits<double>::quiet_NaN(), 512);
    juce::AudioBuffer<float> nan_buf(2, 512);
    juce::MidiBuffer nan_midi;
    nan_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(nan_buf, nan_midi);
    require(channelPeak(nan_buf, 0, 512) < 0.001, "NaN rate must stay silent");
    require(Access::frame(processor) == 0, "NaN rate must not advance engine");
}

double playPeakAtRate(double rate, int block) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(rate, block);
    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    double peak = channelPeak(buffer, 0, block);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, block);
        juce::MidiBuffer empty;
        processor.processBlock(more, empty);
        peak = channelPeak(more, 0, block);
    }
    return peak;
}

void testNon48kHzProducesAudio() {
    require(playPeakAtRate(44'100.0, 512) > 0.001, "44.1kHz produced silence");
    require(playPeakAtRate(48'000.0, 512) > 0.001, "48kHz produced silence");
    require(playPeakAtRate(88'200.0, 512) > 0.001, "88.2kHz produced silence");
    require(playPeakAtRate(96'000.0, 512) > 0.001, "96kHz produced silence");
}

void test48kHzBypassesSrcAndHasZeroLatency() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    require(!Access::usesSrc(processor), "48kHz must stay on the direct path");
    require(processor.getLatencySamples() == 0, "48kHz latency must be 0");
    require(Access::hostRate(processor) == 48'000, "48kHz host rate");

    processor.prepareToPlay(44'100.0, 512);
    require(processor.getLatencySamples() == 7, "44.1kHz latency is 7");
    processor.prepareToPlay(88'200.0, 512);
    require(processor.getLatencySamples() == 15, "88.2kHz latency is 15");
    processor.prepareToPlay(96'000.0, 512);
    require(processor.getLatencySamples() == 16, "96kHz latency is 16");
}

void testHostEngineMapping() {
    require(
        hostFrameToEngineFrame(0, 44'100) == 0, "host 0 -> engine 0");
    require(
        hostFrameToEngineFrame(441, 44'100) == 480,
        "10 ms at 44.1kHz is 480 engine frames");
    require(
        hostFrameToEngineFrame(512, 96'000) == 256,
        "512 host frames at 96kHz are 256 engine frames");
    require(
        hostFrameToEngineFrame(8820, 44'100) == 9'600,
        "200 ms at 44.1kHz is 9600 engine frames");
    require(
        hostFrameToEngineFrame(2'646'000, 44'100) == 2'880'000,
        "60 s at 44.1kHz maps without drift");
    require(
        hostFrameToEngineFrame(128, 48'000) == 128,
        "48kHz mapping is identity");
}

void test44100MidiOffsetMapsToEngine480() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "smoke program has PSG");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);

    juce::AudioBuffer<float> first(2, 440);
    juce::MidiBuffer empty;
    processor.processBlock(first, empty);
    require(
        Access::noteOnCount(processor) == 0,
        "no MIDI yet, so no NoteOn");
    require(
        Access::frame(processor) < 480,
        "440 host samples must not reach engine frame 480");

    juce::AudioBuffer<float> second(2, 72);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        1);
    processor.processBlock(second, midi);
    require(
        Access::logHas(processor, 480, track),
        "441 host samples at 44.1kHz must NoteOn at engine 480");
    require(
        !Access::logHas(processor, 479, track),
        "must not apply the note before engine frame 480");
}

void testLayerDelayIs9600EngineFramesAtEveryHostRate() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto& plan = Access::plan(processor);
        const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        require(
            delayMillisecondsToEngineFrames(scc->start_delay_ms) == 9'600,
            "200 ms is 9600 engine frames");
        const auto scc_track = plan.physicalTrack(*scc, 0);

        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        processUntilEngine(processor, 9'600);
        require(
            Access::logHas(processor, 9'600, scc_track),
            "delayed SCC must fire at engine 9600 at every host rate");
    }
}

void testCancellationAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "note off before due cancels pending at 44.1kHz");
    processUntilEngine(processor, 9'664);
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled delayed layer must never NoteOn at 44.1kHz");
}

void testSameFrameCancelAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);

    juce::AudioBuffer<float> buffer(2, 9'000);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 8'820);
    processor.processBlock(buffer, midi);
    require(
        !Access::logHasTrack(processor, scc_track),
        "same-frame NoteOff at host 8820 must prevent SCC at engine 9600");
}

void testVoiceStealAndAllNotesOffAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> steal_buf(2, 512);
    juce::MidiBuffer steal;
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 1);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 2);
    steal.addEvent(
        juce::MidiMessage::noteOn(1, 63, static_cast<juce::uint8>(100)), 3);
    processor.processBlock(steal_buf, steal);
    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "stolen voice still has delayed SCC");
    require(event->midi_note == 63, "stolen voice carries the new note");
    const auto due_host3 = hostFrameToEngineFrame(3, 44'100) + scc_delay;
    require(event->due_frame == due_host3, "due uses mapped steal sample");

    MgstcAudioProcessor off_proc;
    off_proc.prepareToPlay(44'100.0, 512);
    const auto* scc_off = layerOf(
        Access::plan(off_proc), mgstc::engine::TimbreSource::Scc);
    require(scc_off != nullptr, "SCC layer");
    const auto scc_off_track = Access::plan(off_proc).physicalTrack(*scc_off, 0);
    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    off.addEvent(juce::MidiMessage::allNotesOff(1), 64);
    off_proc.processBlock(off_buf, off);
    require(
        Access::scheduler(off_proc).pendingCount() == 0,
        "All Notes Off cancels pending at 44.1kHz");
    processUntilEngine(off_proc, 10'000);
    require(
        !Access::logHasTrack(off_proc, scc_off_track),
        "All Notes Off must not let delayed layers start later at 44.1kHz");
}

void feedMidiTimeline(
    MgstcAudioProcessor& processor,
    int block,
    const juce::MidiMessage& message,
    int host_offset,
    int total_host_frames) {
    int pos = 0;
    while (pos < total_host_frames) {
        const int n = std::min(block, total_host_frames - pos);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        if (host_offset >= pos && host_offset < pos + n) {
            midi.addEvent(message, host_offset - pos);
        }
        processor.processBlock(buffer, midi);
        pos += n;
    }
}

void testBlockSizeInvarianceAt44100() {
    const auto message =
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100));
    MgstcAudioProcessor small;
    MgstcAudioProcessor large;
    small.prepareToPlay(44'100.0, 64);
    large.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(small), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(small).physicalTrack(*psg, 0);

    feedMidiTimeline(small, 64, message, 441, 1'024);
    feedMidiTimeline(large, 512, message, 441, 1'024);
    require(
        Access::logHas(small, 480, track) && Access::logHas(large, 480, track),
        "block size must not change mapped MIDI engine time");
}

void testLongRunDriftAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    constexpr int kSeconds = 60;
    constexpr int kHost = 44'100 * kSeconds;
    int remaining = kHost;
    while (remaining > 0) {
        const int n = std::min(remaining, 512);
        juce::AudioBuffer<float> buffer(2, n);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        remaining -= n;
    }
    require(Access::hostFrame(processor) == static_cast<std::uint64_t>(kHost),
        "host timeline must advance by 60 s");
    const auto mapped = hostFrameToEngineFrame(
        static_cast<std::uint64_t>(kHost), 44'100);
    const auto engine = Access::frame(processor);
    const auto delta = engine > mapped ? engine - mapped : mapped - engine;
    require(delta <= 1, "60 s at 44.1kHz must not accumulate timeline drift");
    require(
        engine <= mapped,
        "engine must not lead ceil host mapping after 60 s");

    juce::AudioBuffer<float> note_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    const auto started = std::chrono::steady_clock::now();
    processor.processBlock(note_buf, on);
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();
    std::fprintf(
        stderr,
        "Cubase first-note after 60s 44100 elapsed_ms=%.3f %s\n",
        elapsed_ms,
        srcDiag(processor).c_str());
    require(elapsed_ms < 250.0, "first Note On after 60 s must return");
    require(
        Access::srcWatchdogTrips(processor) == 0,
        "first Note On after 60 s must not spin in fillSrcSource");
}

void testRateChangeResetsSrcTimelineAndLatency() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    require(Access::usesSrc(processor), "44.1kHz uses SRC");
    require(processor.getLatencySamples() == 7, "44.1kHz SRC latency is 7");
    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    require(Access::scheduler(processor).pendingCount() > 0, "had pending");
    require(Access::frame(processor) > 0, "44.1kHz advanced engine");

    processor.prepareToPlay(96'000.0, 512);
    require(Access::usesSrc(processor), "96kHz uses SRC");
    require(processor.getLatencySamples() == 16, "96kHz SRC latency is 16");
    require(Access::frame(processor) == 0, "rate change resets engine");
    require(Access::hostFrame(processor) == 0, "rate change resets host");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "rate change drops pending delay");
    require(Access::noteOnCount(processor) == 0, "rate change drops log");

    juce::AudioBuffer<float> silent(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(silent, empty);
    require(
        channelPeak(silent, 0, 512) < 0.001,
        "no stale audio after 96kHz prepare");

    processor.prepareToPlay(48'000.0, 512);
    require(!Access::usesSrc(processor), "48kHz returns to direct path");
    require(processor.getLatencySamples() == 0, "48kHz latency returns to 0");
    juce::AudioBuffer<float> live(2, 2'048);
    juce::MidiBuffer live_on;
    live_on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(live, live_on);
    double peak = channelPeak(live, 0, 2'048);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 2'048);
        juce::MidiBuffer rest;
        processor.processBlock(more, rest);
        peak = channelPeak(more, 0, 2'048);
    }
    require(peak > 0.001, "returning to 48kHz should play");
}

void testTwoProcessorsDoNotShareSrcState() {
    MgstcAudioProcessor a;
    MgstcAudioProcessor b;
    a.prepareToPlay(44'100.0, 512);
    b.prepareToPlay(44'100.0, 512);

    juce::AudioBuffer<float> buffer_a(2, 2'048);
    juce::AudioBuffer<float> buffer_b(2, 2'048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    juce::MidiBuffer empty;
    a.processBlock(buffer_a, on);
    b.processBlock(buffer_b, empty);

    double peak_a = channelPeak(buffer_a, 0, 2'048);
    if (peak_a <= 0.001) {
        juce::AudioBuffer<float> more(2, 2'048);
        juce::MidiBuffer rest;
        a.processBlock(more, rest);
        peak_a = channelPeak(more, 0, 2'048);
    }
    require(peak_a > 0.001, "processor A should sound at 44.1kHz");
    require(
        channelPeak(buffer_b, 0, 2'048) < 0.001,
        "processor B should stay silent");
    require(
        Access::scheduler(a).pendingCount() > 0,
        "processor A should own delayed slots");
    require(
        Access::scheduler(b).pendingCount() == 0,
        "processor B scheduler must stay empty");
}

double estimateHz(
    const juce::AudioBuffer<float>& buffer,
    int start,
    int count,
    double sample_rate) {
    const auto* samples = buffer.getReadPointer(0);
    const int end = std::min(start + count, buffer.getNumSamples());
    int crossings = 0;
    for (int i = start + 1; i < end; ++i) {
        if (samples[i - 1] <= 0.0F && samples[i] > 0.0F) {
            ++crossings;
        }
    }
    const double seconds = static_cast<double>(end - start) / sample_rate;
    require(seconds > 0.0 && crossings > 4, "not enough pitch cycles");
    return static_cast<double>(crossings) / seconds;
}

double pitchAtRate(double rate) {
    MgstcAudioProcessor processor;
    const int frames = std::max(8'192, static_cast<int>(rate * 0.18));
    processor.prepareToPlay(rate, frames);
    juce::AudioBuffer<float> buffer(2, frames);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    const int start = static_cast<int>(rate * 0.04);
    const int count = static_cast<int>(rate * 0.10);
    return estimateHz(buffer, start, count, rate);
}

void testPitchIndependentOfHostRate() {
    const auto hz48 = pitchAtRate(48'000.0);
    const auto hz441 = pitchAtRate(44'100.0);
    const auto hz882 = pitchAtRate(88'200.0);
    const auto hz96 = pitchAtRate(96'000.0);
    require(std::abs(hz441 - hz48) / hz48 < 0.03, "44.1kHz pitch drifted");
    require(std::abs(hz882 - hz48) / hz48 < 0.03, "88.2kHz pitch drifted");
    require(std::abs(hz96 - hz48) / hz48 < 0.03, "96kHz pitch drifted");
}

void testDurationIndependentOfHostRate() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto host_rate = Access::hostRate(processor);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        processUntilEngine(processor, 9'600);
        const auto host_seconds = static_cast<double>(Access::hostFrame(processor))
            / static_cast<double>(host_rate);
        require(
            std::abs(host_seconds - 0.2) < 0.02,
            "200 ms delay must stay 200 ms in host time");
        require(
            Access::frame(processor) >= 9'600,
            "engine must reach the 200 ms due frame");
    }
}

void testAllSoundOffAt44100() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Sound Off cancels pending at 44.1kHz");
    processUntilEngine(processor, 10'000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Sound Off must not let delayed layers start later at 44.1kHz");
}

void testTwoProcessorsDoNotShareVoiceState() {
    MgstcAudioProcessor a;
    MgstcAudioProcessor b;
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
    require(
        Access::scheduler(a).pendingCount() > 0,
        "processor A should own delayed slots");
    require(
        Access::scheduler(b).pendingCount() == 0,
        "processor B scheduler must stay empty");
}

void testZeroDelayFiresAtMidiFrame() {
    MgstcAudioProcessor processor;
    prime(processor, 512);
    const auto* psg = layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "smoke program has PSG");
    require(
        delayMillisecondsToEngineFrames(psg->start_delay_ms) == 0,
        "PSG layer must stay zero-delay");

    const auto block_start = Access::frame(processor);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        128);
    processor.processBlock(buffer, midi);

    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    require(
        Access::logHas(processor, block_start + 128, track),
        "zero-delay layer must NoteOn at the MIDI sample");
    require(
        channelPeak(buffer, 0, 128) < 0.001,
        "zero-delay audio must not start before MIDI");
}

void testDelayedLayersFireAtExactDueAndAcrossBlocks() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    const auto* opll = layerOf(plan, mgstc::engine::TimbreSource::Opll);
    require(scc != nullptr && opll != nullptr, "smoke has SCC and OPLL");
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);
    const auto opll_delay =
        delayMillisecondsToEngineFrames(opll->start_delay_ms);
    require(scc_delay == 9600, "SCC smoke delay is 200 ms");
    require(opll_delay == 19200, "OPLL smoke delay is 400 ms");

    juce::AudioBuffer<float> first(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        400);
    processor.processBlock(first, on);

    const auto note_frame = static_cast<std::uint64_t>(400);
    const auto scc_due = note_frame + scc_delay;
    const auto opll_due = note_frame + opll_delay;
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto opll_track = plan.physicalTrack(*opll, 0);

    require(Access::scheduler(processor).isPending(scc_track), "SCC pending");
    require(Access::scheduler(processor).isPending(opll_track), "OPLL pending");
    require(
        Access::scheduler(processor).event(scc_track)->due_frame == scc_due,
        "SCC due is absolute note+delay");
    require(
        !Access::logHasTrack(processor, scc_track),
        "SCC must not fire in the first block");

    processUntilFrame(processor, scc_due);
    require(Access::frame(processor) == scc_due, "landed on SCC due");
    require(
        Access::logHas(processor, scc_due, scc_track),
        "SCC fires at exact due frame");
    require(
        !Access::logHasTrack(processor, opll_track),
        "OPLL must still be waiting");

    processUntilFrame(processor, opll_due);
    require(
        Access::logHas(processor, opll_due, opll_track),
        "OPLL fires at exact due frame");
    require(Access::scheduler(processor).pendingCount() == 0, "all due consumed");
}

void testNoteOffBeforeDueCancelsDelayedLayers() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_due =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    processor.processBlock(buffer, midi);

    require(
        Access::scheduler(processor).pendingCount() == 0,
        "note off before due cancels pending");
    require(
        Access::isInactive(processor, scc_track),
        "cancelled layer is inactive");

    processUntilFrame(processor, scc_due + 64);
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled delayed layer must never NoteOn");
}

void testNoteOffAfterDueLeavesReleasePath() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_due =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    processUntilFrame(processor, scc_due);
    require(Access::logHas(processor, scc_due, scc_track), "delayed layer sounded");
    require(
        Access::isActive(processor, scc_track),
        "sounded layer is Active");

    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(off_buf, off);
    require(
        Access::isInactive(processor, scc_track),
        "note off after due clears Active");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "no pending after sounding note off");
}

void testSameFrameNoteOffCancelsDueEvent() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const int due = static_cast<int>(
        delayMillisecondsToEngineFrames(scc->start_delay_ms));

    juce::AudioBuffer<float> buffer(2, due + 64);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), due);
    processor.processBlock(buffer, midi);

    require(
        !Access::logHasTrack(processor, scc_track),
        "same-frame NoteOff must prevent delayed NoteOn");
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "same-frame cancel leaves no pending");
}

void testVoiceStealCancelsOldPending() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto scc_delay =
        delayMillisecondsToEngineFrames(scc->start_delay_ms);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100)), 1);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)), 2);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 63, static_cast<juce::uint8>(100)), 3);
    processor.processBlock(buffer, midi);

    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "voice 0 still has a delayed SCC");
    require(event->midi_note == 63, "stolen voice carries the new note");
    require(
        event->due_frame == 3 + scc_delay,
        "new reservation uses the steal sample");

    processUntilFrame(processor, scc_delay);
    require(
        !Access::logHas(processor, scc_delay, scc_track),
        "stolen note's original due must not fire");
    processUntilFrame(processor, 3 + scc_delay);
    require(
        Access::logHas(processor, 3 + scc_delay, scc_track),
        "new note's delayed layer fires at its due");
}

void testAllNotesOffCancelsPending() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allNotesOff(1), 64);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Notes Off cancels pending");

    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    processEmpty(processor, 10000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Notes Off must not let delayed layers start later");
}

void testAllSoundOffCancelsPending() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 32);
    processor.processBlock(buffer, midi);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "All Sound Off cancels pending");

    const auto* scc = layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
    processEmpty(processor, 10000);
    require(
        !Access::logHasTrack(processor, scc_track),
        "All Sound Off must not let delayed layers start later");
}

void testLifecycleResetDropsPending() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    require(Access::scheduler(processor).pendingCount() > 0, "had pending");

    processor.releaseResources();
    require(Access::scheduler(processor).pendingCount() == 0, "release clears");
    require(Access::frame(processor) == 0, "release resets timeline");

    processor.prepareToPlay(48'000.0, 512);
    require(Access::scheduler(processor).pendingCount() == 0, "prepare clears");
    processEmpty(processor, 20000);
    require(
        Access::noteOnCount(processor) == 0,
        "old reservations must not fire after prepareToPlay");
}

void testProgramResetDropsPending() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    processor.setCurrentProgram(0);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "program reset cancels pending");
    processEmpty(processor, 20000);
    require(
        Access::noteOnCount(processor) == 0,
        "old reservations must not fire after program reset");
}

std::uint64_t fnvMix(std::uint64_t hash, float sample) {
    const auto bits = static_cast<std::uint32_t>(
        std::bit_cast<std::uint32_t>(sample));
    hash ^= bits;
    hash *= 1099511628211ULL;
    return hash;
}

template <typename Render>
double timeFrames(int frames, Render&& render, std::uint64_t* hash) {
    std::uint64_t mixed = 14695981039346656037ULL;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        mixed = fnvMix(mixed, render());
    }
    if (hash != nullptr) {
        *hash = mixed;
    }
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

// Pre-ring SincRateConv: linear history shift, std::abs / std::min lookup.
// The ring buffer must match this sample for sample, including channel taps.
class LinearShiftSinc {
public:
    static constexpr std::size_t kTapCount = 16;
    static constexpr std::size_t kTableReso = 256;

    LinearShiftSinc(double input_hz, double output_hz)
        : input_hz_(input_hz)
        , output_hz_(output_hz)
        , ratio_(input_hz / output_hz) {
        constexpr double half_taps = static_cast<double>(kTapCount) / 2.0;
        const bool downsample = output_hz < input_hz;
        for (std::size_t i = 0; i < sinc_table_.size(); ++i) {
            const double x =
                static_cast<double>(i) / static_cast<double>(kTableReso);
            const double kernel = downsample
                ? windowed(x / ratio_, half_taps) / ratio_
                : windowed(x, half_taps);
            sinc_table_[i] = static_cast<float>(kernel);
        }
    }

    template <typename Generate>
    float next(Generate&& generate, std::span<float> channels = {}) noexcept {
        if (!channels.empty() && !tracking_) {
            channel_history_ = {};
        }
        tracking_ = !channels.empty();
        while (input_hz_ > time_) {
            time_ += output_hz_;
            push(static_cast<float>(generate()));
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                auto& history = channel_history_[ch];
                for (std::size_t k = 1; k < kTapCount; ++k) {
                    history[k - 1] = history[k];
                }
                history.back() = channels[ch];
            }
        }
        time_ -= input_hz_;
        const float mixed = interpolate();
        constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
        std::array<float, kTapCount> coefficients{};
        if (!channels.empty()) {
            for (std::size_t k = 0; k < kTapCount; ++k) {
                coefficients[k] = lookup(static_cast<double>(k) - center - frac_);
            }
        }
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            double sum = 0.0;
            for (std::size_t k = 0; k < kTapCount; ++k) {
                sum += static_cast<double>(channel_history_[ch][k])
                    * static_cast<double>(coefficients[k]);
            }
            channels[ch] = static_cast<float>(sum);
        }
        return mixed;
    }

    void reset() noexcept {
        history_.fill(0.0F);
        channel_history_ = {};
        tracking_ = false;
        time_ = 0.0;
        frac_ = 0.0;
    }

private:
    static double windowed(double x, double half_taps) noexcept {
        constexpr double pi = std::numbers::pi_v<double>;
        const double window = 0.42
            - 0.5 * std::cos(2.0 * pi * (0.5 + 0.5 * x / half_taps))
            + 0.08 * std::cos(4.0 * pi * (0.5 + 0.5 * x / half_taps));
        if (x == 0.0) {
            return window;
        }
        return window * (std::sin(pi * x) / (pi * x));
    }

    void push(float sample) noexcept {
        for (std::size_t i = 0; i + 1 < history_.size(); ++i) {
            history_[i] = history_[i + 1];
        }
        history_.back() = sample;
    }

    float lookup(double x) const noexcept {
        const auto index = static_cast<int>(
            std::abs(x) * static_cast<double>(kTableReso));
        const auto clamped = std::min(
            static_cast<int>(sinc_table_.size()) - 1,
            index);
        return sinc_table_[static_cast<std::size_t>(clamped)];
    }

    float interpolate() noexcept {
        frac_ += ratio_;
        frac_ -= std::floor(frac_);
        double sum = 0.0;
        constexpr double center = static_cast<double>(kTapCount) / 2.0 - 1.0;
        for (std::size_t k = 0; k < history_.size(); ++k) {
            const double x = static_cast<double>(k) - center - frac_;
            sum += static_cast<double>(history_[k])
                * static_cast<double>(lookup(x));
        }
        return static_cast<float>(sum);
    }

    double input_hz_{};
    double output_hz_{};
    double ratio_{};
    double time_{};
    double frac_{};
    std::array<float, kTapCount> history_{};
    std::array<std::array<float, kTapCount>, 14> channel_history_{};
    bool tracking_{};
    std::array<float, kTableReso * kTapCount / 2> sinc_table_{};
};

void requireSameBits(float left, float right, const char* label) {
    require(
        std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right),
        label);
}

double sincSource(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<double>(static_cast<std::int32_t>(state >> 16) - 32768)
        / 32768.0;
}

void compareSincSpans(
    double input_hz,
    int frames,
    int channel_count,
    bool reset_midway) {
    mgstc::engine::SincRateConv ring(input_hz, 48'000.0);
    LinearShiftSinc linear(input_hz, 48'000.0);
    std::uint32_t ring_state = 1;
    std::uint32_t linear_state = 1;
    int ring_calls = 0;
    int linear_calls = 0;
    std::array<float, 3> ring_channels{};
    std::array<float, 3> linear_channels{};
    for (int frame = 0; frame < frames; ++frame) {
        if (reset_midway && frame == frames / 2) {
            ring.reset();
            linear.reset();
        }
        const int active = (frame % 5 == 0) ? 0 : channel_count;
        for (int ch = 0; ch < active; ++ch) {
            const float sample = static_cast<float>(frame * 3 + ch) * 0.001F;
            ring_channels[static_cast<std::size_t>(ch)] = sample;
            linear_channels[static_cast<std::size_t>(ch)] = sample;
        }
        const auto ring_span = std::span<float>(
            ring_channels.data(), static_cast<std::size_t>(active));
        const auto linear_span = std::span<float>(
            linear_channels.data(), static_cast<std::size_t>(active));
        const float ring_mixed = ring.next(
            [&] {
                ++ring_calls;
                return sincSource(ring_state);
            },
            ring_span);
        const float linear_mixed = linear.next(
            [&] {
                ++linear_calls;
                return sincSource(linear_state);
            },
            linear_span);
        requireSameBits(ring_mixed, linear_mixed, "sinc ring mono");
        for (int ch = 0; ch < active; ++ch) {
            requireSameBits(
                ring_channels[static_cast<std::size_t>(ch)],
                linear_channels[static_cast<std::size_t>(ch)],
                "sinc ring channel");
        }
    }
    require(ring_calls == linear_calls, "sinc generate count");
}

void testSincRingMatchesLinearShift() {
    constexpr std::array rates{3'579'545.0 / 16.0, 3'579'545.0 / 72.0};
    for (const double rate : rates) {
        compareSincSpans(rate, 4'800, 0, false);
        compareSincSpans(rate, 4'800, 3, false);
        compareSincSpans(rate, 800, 3, true);
    }
}

void profileEngineIdleBreakdown() {
    constexpr int kFrames = 48'000;
    mgstc::engine::ChipRack rack;
    require(rack.valid(), "ChipRack must construct");
    std::uint64_t rack_hash = 0;
    const auto chip_ms = timeFrames(
        kFrames,
        [&] { return rack.renderSample().psg; },
        &rack_hash);

    std::array<float, 1024> pcm{};
    auto render_core = [&](bool scope) {
        mgstc::engine::EngineCore core;
        core.setOpllScopeEnabled(scope);
        const auto started = std::chrono::steady_clock::now();
        int remain = kFrames;
        while (remain > 0) {
            const int n = std::min(remain, 512);
            static_cast<void>(core.render(
                std::span<float>(pcm.data(), static_cast<std::size_t>(n) * 2)));
            remain -= n;
        }
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    const auto core_scope_ms = render_core(true);
    const auto core_noscope_ms = render_core(false);

    mgstc::engine::DcBlocker blocker(48'000.0F, 3.4F);
    const auto mix_ms = timeFrames(
        kFrames,
        [&] {
            return std::clamp(blocker.process(0.0F), -1.0F, 1.0F);
        },
        nullptr);

    std::fprintf(
        stderr,
        "Idle breakdown 48k/1s ChipRack_ms=%.3f EngineCore_scope_ms=%.3f "
        "EngineCore_noscope_ms=%.3f mix_dc_ms=%.3f rack_hash=%llx\n",
        chip_ms,
        core_scope_ms,
        core_noscope_ms,
        mix_ms,
        static_cast<unsigned long long>(rack_hash));

#ifdef NDEBUG
    using mgstc::engine::ChipId;
    using mgstc::engine::WriteReason;
    constexpr double kPsgNative = 3'579'545.0 / 16.0;
    constexpr double kOpllNative = 3'579'545.0 / 72.0;
    auto time_sinc = [](double input_hz) {
        mgstc::engine::SincRateConv conv(input_hz, 48'000.0);
        std::uint32_t state = 1;
        return timeFrames(
            kFrames,
            [&] {
                return conv.next([&] {
                    state = state * 1664525U + 1013904223U;
                    return static_cast<float>(
                               static_cast<int>(state >> 16) - 32768)
                        / 32768.0F;
                });
            },
            nullptr);
    };

    mgstc::engine::Ym2149Adapter psg;
    mgstc::engine::SccAdapter scc;
    mgstc::engine::Ym2413Adapter opll;
    require(psg.valid() && scc.valid() && opll.valid(), "adapters construct");
    std::uint64_t psg_idle_hash = 0;
    std::uint64_t scc_idle_hash = 0;
    std::uint64_t opll_idle_hash = 0;
    const auto psg_idle = timeFrames(
        kFrames, [&] { return psg.renderSample(); }, &psg_idle_hash);
    const auto scc_idle = timeFrames(
        kFrames, [&] { return scc.renderSample(); }, &scc_idle_hash);
    const auto opll_idle = timeFrames(
        kFrames, [&] { return opll.renderSample(); }, &opll_idle_hash);

    mgstc::engine::Ym2149Adapter psg_tone;
    require(psg_tone.write(0, 0xAB), "psg period lo");
    require(psg_tone.write(1, 0x01), "psg period hi");
    require(psg_tone.write(7, 0xBE), "psg mixer");
    require(psg_tone.write(8, 0x0F), "psg volume");
    std::uint64_t psg_tone_hash = 0;
    const auto psg_tone_ms = timeFrames(
        kFrames, [&] { return psg_tone.renderSample(); }, &psg_tone_hash);

    mgstc::engine::SccAdapter scc_tone;
    for (std::uint8_t index = 0; index < 32; ++index) {
        require(
            scc_tone.write(
                0,
                index,
                static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80)),
            "scc wave");
    }
    require(scc_tone.write(1, 0, 0xAB), "scc freq lo");
    require(scc_tone.write(1, 1, 0x01), "scc freq hi");
    require(scc_tone.write(2, 0, 0x0F), "scc volume");
    require(scc_tone.write(3, 0, 0x01), "scc key");
    std::uint64_t scc_tone_hash = 0;
    const auto scc_tone_ms = timeFrames(
        kFrames, [&] { return scc_tone.renderSample(); }, &scc_tone_hash);

    mgstc::engine::Ym2413Adapter opll_tone;
    const std::array<std::uint8_t, 8> original_patch{
        0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00,
    };
    for (std::uint8_t address = 0; address < original_patch.size(); ++address) {
        require(
            opll_tone.write(address, original_patch[address]),
            "opll patch");
    }
    require(opll_tone.write(0x30, 0x00), "opll inst");
    require(opll_tone.write(0x20, 0x16), "opll key");
    require(opll_tone.write(0x10, 0xAC), "opll freq");
    std::uint64_t opll_tone_hash = 0;
    const auto opll_tone_ms = timeFrames(
        kFrames, [&] { return opll_tone.renderSample(); }, &opll_tone_hash);

    mgstc::engine::ChipRack sounding;
    const std::array<mgstc::engine::RegisterWrite, 4> psg_setup{{
        {0, 0, ChipId::Psg, 0, 0, 0xAB, 0, WriteReason::Frequency},
        {0, 1, ChipId::Psg, 0, 1, 0x01, 0, WriteReason::Frequency},
        {0, 2, ChipId::Psg, 0, 7, 0xBE, 0, WriteReason::Patch},
        {0, 3, ChipId::Psg, 0, 8, 0x0F, 0, WriteReason::Volume},
    }};
    require(sounding.apply(psg_setup), "sounding psg");
    std::uint32_t sequence = 4;
    for (std::uint8_t index = 0; index < 32; ++index) {
        require(
            sounding.apply({
                0,
                sequence++,
                ChipId::Scc,
                0,
                index,
                static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80),
                3,
                WriteReason::Patch,
            }),
            "sounding scc wave");
    }
    const std::array<mgstc::engine::RegisterWrite, 7> rest_setup{{
        {0, sequence++, ChipId::Scc, 1, 0, 0xAB, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 1, 1, 0x01, 3, WriteReason::Frequency},
        {0, sequence++, ChipId::Scc, 2, 0, 0x0F, 3, WriteReason::Volume},
        {0, sequence++, ChipId::Scc, 3, 0, 0x01, 3, WriteReason::KeyOn},
        {0, sequence++, ChipId::Opll, 0, 0x30, 0x10, 8, WriteReason::Patch},
        {0, sequence++, ChipId::Opll, 0, 0x20, 0x16, 8, WriteReason::KeyOn},
        {0, sequence++, ChipId::Opll, 0, 0x10, 0xAC, 8, WriteReason::Frequency},
    }};
    require(sounding.apply(rest_setup), "sounding scc/opll");
    const auto rack_sound_ms = timeFrames(
        kFrames, [&] { return sounding.renderSample().psg; }, nullptr);
    const auto rack_capture_ms = timeFrames(
        kFrames,
        [&] { return sounding.renderSample(true).psg; },
        nullptr);
    mgstc::engine::ChipRack warm;
    require(warm.valid(), "warm ChipRack");
    const auto rack_warm_ms = timeFrames(
        kFrames, [&] { return warm.renderSample().psg; }, nullptr);

    std::fprintf(
        stderr,
        "Chip split 48k/1s PSG_idle_ms=%.3f PSG_tone_ms=%.3f "
        "SCC_idle_ms=%.3f SCC_tone_ms=%.3f OPLL_idle_ms=%.3f "
        "OPLL_tone_ms=%.3f sinc_psg_ratio_ms=%.3f sinc_opll_ratio_ms=%.3f "
        "rack_sound_ms=%.3f rack_capture_ms=%.3f rack_warm_ms=%.3f\n",
        psg_idle,
        psg_tone_ms,
        scc_idle,
        scc_tone_ms,
        opll_idle,
        opll_tone_ms,
        time_sinc(kPsgNative),
        time_sinc(kOpllNative),
        rack_sound_ms,
        rack_capture_ms,
        rack_warm_ms);
    auto hash_fresh = [](auto construct, int frames) {
        auto chip = construct();
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            frames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    const auto psg_10 = hash_fresh(
        [] { return mgstc::engine::Ym2149Adapter(); }, kFrames * 10);
    const auto opll_10 = hash_fresh(
        [] { return mgstc::engine::Ym2413Adapter(); }, kFrames * 10);
    const auto psg_60 = hash_fresh(
        [] { return mgstc::engine::Ym2149Adapter(); }, kFrames * 60);
    const auto opll_60 = hash_fresh(
        [] { return mgstc::engine::Ym2413Adapter(); }, kFrames * 60);
    auto hash_psg_after_idle = [](int idle) {
        mgstc::engine::Ym2149Adapter chip;
        for (int i = 0; i < idle; ++i) {
            static_cast<void>(chip.renderSample());
        }
        require(chip.write(0, 0xAB) && chip.write(1, 0x01)
                    && chip.write(7, 0xBE) && chip.write(8, 0x0F),
            "psg after idle");
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            kFrames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    auto hash_opll_after_idle = [](int idle) {
        mgstc::engine::Ym2413Adapter chip;
        for (int i = 0; i < idle; ++i) {
            static_cast<void>(chip.renderSample());
        }
        const std::array<std::uint8_t, 8> patch{
            0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00,
        };
        for (std::uint8_t address = 0; address < patch.size(); ++address) {
            require(chip.write(address, patch[address]), "opll after idle");
        }
        require(chip.write(0x30, 0x00) && chip.write(0x20, 0x16)
                    && chip.write(0x10, 0xAC),
            "opll key after idle");
        std::uint64_t hash = 0;
        static_cast<void>(timeFrames(
            kFrames, [&] { return chip.renderSample(); }, &hash));
        return hash;
    };
    const auto psg_note0 = hash_psg_after_idle(0);
    const auto psg_note1 = hash_psg_after_idle(kFrames);
    const auto psg_note10 = hash_psg_after_idle(kFrames * 10);
    const auto psg_note60 = hash_psg_after_idle(kFrames * 60);
    const auto opll_note0 = hash_opll_after_idle(0);
    const auto opll_note1 = hash_opll_after_idle(kFrames);
    const auto opll_note10 = hash_opll_after_idle(kFrames * 10);
    const auto opll_note60 = hash_opll_after_idle(kFrames * 60);
    std::fprintf(
        stderr,
        "PCM hash psg_idle_1s=%016llx scc_idle_1s=%016llx opll_idle_1s=%016llx "
        "psg_tone_1s=%016llx scc_tone_1s=%016llx opll_tone_1s=%016llx "
        "psg_idle_10s=%016llx opll_idle_10s=%016llx "
        "psg_idle_60s=%016llx opll_idle_60s=%016llx "
        "psg_note_after_0=%016llx psg_note_after_1s=%016llx "
        "psg_note_after_10s=%016llx psg_note_after_60s=%016llx "
        "opll_note_after_0=%016llx opll_note_after_1s=%016llx "
        "opll_note_after_10s=%016llx opll_note_after_60s=%016llx\n",
        static_cast<unsigned long long>(psg_idle_hash),
        static_cast<unsigned long long>(scc_idle_hash),
        static_cast<unsigned long long>(opll_idle_hash),
        static_cast<unsigned long long>(psg_tone_hash),
        static_cast<unsigned long long>(scc_tone_hash),
        static_cast<unsigned long long>(opll_tone_hash),
        static_cast<unsigned long long>(psg_10),
        static_cast<unsigned long long>(opll_10),
        static_cast<unsigned long long>(psg_60),
        static_cast<unsigned long long>(opll_60),
        static_cast<unsigned long long>(psg_note0),
        static_cast<unsigned long long>(psg_note1),
        static_cast<unsigned long long>(psg_note10),
        static_cast<unsigned long long>(psg_note60),
        static_cast<unsigned long long>(opll_note0),
        static_cast<unsigned long long>(opll_note1),
        static_cast<unsigned long long>(opll_note10),
        static_cast<unsigned long long>(opll_note60));
    require(psg_tone_hash == 0x87fb8b985f6f522bULL, "psg tone pcm");
    require(scc_tone_hash == 0xb5847d925273a925ULL, "scc tone pcm");
    require(opll_tone_hash == 0x19aafbc80013f191ULL, "opll tone pcm");
    require(psg_note0 == 0x87fb8b985f6f522bULL, "psg note after 0");
    require(psg_note1 == 0x37becf2600dcf147ULL, "psg note after 1s");
    require(psg_note10 == 0xa98dcd45456d8911ULL, "psg note after 10s");
    require(psg_note60 == 0x098def64738b3ff5ULL, "psg note after 60s");
    require(opll_note0 == 0x19aafbc80013f191ULL, "opll note after 0");
    require(opll_note1 == 0x2a9082c034cdd2d0ULL, "opll note after 1s");
    require(opll_note10 == 0x0b3d8c04f8a70453ULL, "opll note after 10s");
    require(opll_note60 == 0xebfa32d4a20a4aeaULL, "opll note after 60s");
#else
    std::fprintf(stderr, "Chip split skipped in Debug\n");
#endif
}

std::vector<float> captureNotePcmAfterSilence(int silence_frames) {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    processSilence(processor, silence_frames, 512);
    juce::AudioBuffer<float> on_buf(2, 2048);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    std::vector<float> pcm(
        static_cast<std::size_t>(on_buf.getNumSamples()));
    const auto* left = on_buf.getReadPointer(0);
    for (int i = 0; i < on_buf.getNumSamples(); ++i) {
        pcm[static_cast<std::size_t>(i)] = left[i];
    }
    return pcm;
}

double maxAbsDiff(
    const std::vector<float>& a,
    const std::vector<float>& b) {
    require(a.size() == b.size(), "PCM compare size");
    double peak = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        peak = std::max(
            peak,
            static_cast<double>(std::fabs(a[i] - b[i])));
    }
    return peak;
}

std::uint64_t hashPcm(const std::vector<float>& pcm) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float sample : pcm) {
        hash = fnvMix(hash, sample);
    }
    return hash;
}

void testIdleDurationChangesNextNotePcm() {
    const auto pcm0 = captureNotePcmAfterSilence(0);
    const auto pcm1s = captureNotePcmAfterSilence(48'000);
#ifdef NDEBUG
    const auto pcm10s = captureNotePcmAfterSilence(480'000);
    const auto pcm60s = captureNotePcmAfterSilence(2'880'000);
#else
    const auto pcm10s = pcm1s;
    const auto pcm60s = pcm1s;
#endif
    const auto d1 = maxAbsDiff(pcm0, pcm1s);
    const auto d10 = maxAbsDiff(pcm0, pcm10s);
    std::fprintf(
        stderr,
        "Idle PCM next-note maxdiff 0vs1s=%.6g 0vs10s=%.6g "
        "hash0=%016llx hash1s=%016llx hash10s=%016llx hash60s=%016llx\n",
        d1,
        d10,
        static_cast<unsigned long long>(hashPcm(pcm0)),
        static_cast<unsigned long long>(hashPcm(pcm1s)),
        static_cast<unsigned long long>(hashPcm(pcm10s)),
        static_cast<unsigned long long>(hashPcm(pcm60s)));
    // Chip clocks keep running while silent. Skipping them would not match
    // full render, so idle synthesis is not skipped.
#ifdef NDEBUG
    require(hashPcm(pcm0) == 0xdf5a02060aa6e92bULL, "next note after 0");
    require(hashPcm(pcm1s) == 0x2b87be7c9976c757ULL, "next note after 1s");
    require(hashPcm(pcm10s) == 0xe55be4bed683f0aeULL, "next note after 10s");
    require(hashPcm(pcm60s) == 0xf311345bfc9d6f71ULL, "next note after 60s");
#endif
}

void testNeverPlayedIsQuiescentAndReleaseStillSounds() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    processSilence(processor, 512, 512);
    require(
        Access::pluginQuiescent(processor),
        "never-played silence must be plugin-quiescent");
    require(
        !Access::hasRealtimeWork(processor),
        "never-played session has no software envelope work");

    juce::AudioBuffer<float> on_buf(2, 512);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(on_buf, on);
    require(
        !Access::pluginQuiescent(processor),
        "active/pending layers are not quiescent");

    processEmpty(processor, 20'000);
    juce::AudioBuffer<float> off_buf(2, 512);
    juce::MidiBuffer off;
    off.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(off_buf, off);
    juce::AudioBuffer<float> tail(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(tail, empty);
    require(
        channelPeak(tail, 0, 512) > 0.0001,
        "release tail must keep rendering after Note Off");
}

void testBatchSourceRenderCallCount() {
    auto measure = [](double rate, int block, bool sounding) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, block);
        processSilence(processor, 8 * block, block);
        if (sounding) {
            juce::AudioBuffer<float> on_buf(2, block);
            juce::MidiBuffer on;
            on.addEvent(
                juce::MidiMessage::noteOn(
                    1, 60, static_cast<juce::uint8>(100)),
                block / 2);
            processor.processBlock(on_buf, on);
            const auto calls = Access::renderAudioCalls(processor);
            const auto cb = Access::srcCallbacks(processor);
            std::fprintf(
                stderr,
                "Batch calls rate=%.1f block=%d sounding=1 "
                "renderAudio=%u src_cb=%u fill_iters=%u\n",
                rate,
                block,
                calls,
                cb,
                Access::srcFillIters(processor));
            require(
                calls < static_cast<std::uint32_t>(block),
                "sounding block must not renderAudio per host sample");
            if (Access::usesSrc(processor)) {
                require(
                    cb <= 8,
                    "SRC must pull batched source, not one frame per callback");
            }
            return;
        }
        juce::AudioBuffer<float> buffer(2, block);
        juce::MidiBuffer midi;
        processor.processBlock(buffer, midi);
        const auto calls = Access::renderAudioCalls(processor);
        const auto cb = Access::srcCallbacks(processor);
        std::fprintf(
            stderr,
            "Batch calls rate=%.1f block=%d sounding=0 "
            "renderAudio=%u src_cb=%u fill_iters=%u\n",
            rate,
            block,
            calls,
            cb,
            Access::srcFillIters(processor));
        require(calls <= 4, "idle batch should be a handful of renderAudio calls");
        if (Access::usesSrc(processor)) {
            require(cb <= 4, "idle SRC callbacks should be buffer bounds, not 1-frame");
        }
    };
    measure(44'100.0, 512, false);
    measure(44'100.0, 512, true);
    measure(96'000.0, 512, false);
    measure(48'000.0, 512, false);
}

void testMidBlockNoteIsNotRoundedToBatchStart() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(44'100.0, 512);
    const auto* psg =
        layerOf(Access::plan(processor), mgstc::engine::TimbreSource::Psg);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        256);
    processor.processBlock(buffer, midi);
    const auto expected = hostFrameToEngineFrame(256, 44'100);
    require(
        Access::logHas(processor, expected, track),
        "mid-block MIDI must keep its mapped engine frame");
    require(
        !Access::logHas(processor, 0, track),
        "batch source must not snap MIDI to the block start");
}

bool bufferAllFinite(const juce::AudioBuffer<float>& buffer) {
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        const auto* samples = buffer.getReadPointer(channel);
        const int count = buffer.getNumSamples();
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(samples[i])) {
                return false;
            }
        }
    }
    return true;
}

void testNon48kHzOutputIsFinite() {
    constexpr std::array rates{44'100.0, 48'000.0, 88'200.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        for (int block = 0; block < 8; ++block) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            if (block == 0) {
                midi.addEvent(
                    juce::MidiMessage::noteOn(
                        1, 60, static_cast<juce::uint8>(100)),
                    0);
            } else if (block == 4) {
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
            }
            processor.processBlock(buffer, midi);
            require(
                bufferAllFinite(buffer),
                "non-48kHz path produced NaN or Inf");
        }
    }
}

void testLatencyReportedOnlyFromPrepareAndRelease() {
    require(
        Access::diagnosticMode() == kVst3DiagOff,
        "normal processor tests must compile with diagnostic OFF");
    require(
        !MgstcAudioProcessor::kProcessBlockUsesScopedNoDenormals,
        "fact: processBlock has no ScopedNoDenormals");

    MgstcAudioProcessor processor;
    require(Access::latencyCalls(processor) == 0, "no latency before prepare");
    processor.prepareToPlay(44'100.0, 512);
    require(
        Access::prepareToPlayCount(processor) == 1, "one prepareToPlay");
    require(
        Access::latencyCalls(processor) == 1,
        "44.1kHz prepare reports the final SRC group delay once");
    require(
        Access::latencyChanged(processor) == 1,
        "first 44.1kHz prepare notifies once (0 -> 7)");
    require(
        Access::latencySame(processor) == 0,
        "first 44.1kHz prepare must not bounce through 0");
    require(processor.getLatencySamples() == 7, "44.1kHz latency is 7");

    const auto after_prepare = Access::latencyCalls(processor);
    for (int i = 0; i < 64; ++i) {
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        if (i == 0) {
            midi.addEvent(
                juce::MidiMessage::noteOn(
                    1, 60, static_cast<juce::uint8>(100)),
                0);
        }
        processor.processBlock(buffer, midi);
        require(
            Access::lastNumSamples(processor) == 512,
            "host block size should stay 512 in this harness");
        require(
            Access::lastBufferChannels(processor) == 2,
            "host channel count should stay stereo in this harness");
        require(
            Access::processSampleRateHz(processor) == 44'100,
            "processBlock sample rate should stay 44100");
    }
    require(
        Access::processBlockCount(processor) == 64,
        "processBlock counter should track calls");
    require(
        Access::latencyCalls(processor) == after_prepare,
        "processBlock must not call setLatencySamples");
    require(
        Access::latencyFromProcess(processor) == 0,
        "Audio Thread must not notify latency");
    require(
        Access::sampleRateChangesInProcess(processor) == 0,
        "sample rate must not change per block");

    processor.prepareToPlay(48'000.0, 512);
    require(
        Access::prepareToPlayCount(processor) == 2, "second prepareToPlay");
    require(
        Access::latencyCalls(processor) == after_prepare + 1,
        "48kHz prepare reports final latency 0 once");
    require(processor.getLatencySamples() == 0, "48kHz latency returns to 0");
    juce::AudioBuffer<float> live(2, 512);
    juce::MidiBuffer empty;
    processor.processBlock(live, empty);
    require(
        Access::processSampleRateHz(processor) == 48'000,
        "48kHz processBlock should report 48000");

    const auto after_48 = Access::latencyCalls(processor);
    processor.releaseResources();
    require(
        Access::releaseResourcesCount(processor) == 1,
        "releaseResources counter");
    require(
        Access::latencyCalls(processor) == after_48,
        "releaseResources must not call setLatencySamples");
    require(
        processor.getLatencySamples() == 0,
        "release must leave the last reported latency");

    MgstcAudioProcessor bounce;
    bounce.prepareToPlay(44'100.0, 512);
    require(bounce.getLatencySamples() == 7, "re-prepare fixture starts at 7");
    const auto first_changed = Access::latencyChanged(bounce);
    const auto first_calls = Access::latencyCalls(bounce);
    bounce.releaseResources();
    require(
        bounce.getLatencySamples() == 7,
        "release at 44.1kHz must not bounce latency to 0");
    require(
        Access::latencyCalls(bounce) == first_calls,
        "release must not notify Host");
    bounce.prepareToPlay(44'100.0, 512);
    require(bounce.getLatencySamples() == 7, "second 44.1kHz prepare stays 7");
    require(
        Access::latencyChanged(bounce) == first_changed,
        "re-prepare at the same rate must not send kLatencyChanged");
}

void testHostBufferContractAcrossRates() {
    MgstcAudioProcessor processor;
    constexpr std::array rates{44'100.0, 48'000.0, 96'000.0};
    for (const auto rate : rates) {
        processor.prepareToPlay(rate, 512);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, midi);
        require(
            Access::lastNumSamples(processor) == 512, "numSamples 512");
        require(
            Access::lastBufferChannels(processor) == 2, "stereo buffer");
        require(
            Access::lastMidiEvents(processor) == 1, "one MIDI event");
        require(
            Access::sampleRateChangesInProcess(processor) == 0,
            "no per-block sample-rate rewrite");
    }
}

const mgstc::engine::CompositePlaybackLayer* psgLayer(
    const MgstcAudioProcessor& processor) {
    return layerOf(
        Access::plan(processor), mgstc::engine::TimbreSource::Psg);
}

void requireLoggedNote(
    const MgstcAudioProcessor& processor,
    std::size_t index,
    std::uint64_t frame,
    std::uint8_t note) {
    require(index < Access::logSize(processor), "log index");
    require(Access::logFrame(processor, index) == frame, "log frame");
    require(Access::logNote(processor, index) == note, "log note");
}

void testOverdueMappedEventsKeepOrderAndDoNotStick() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant 100");
    require(Access::plantNoteOn(processor, 101, 61), "plant 101");
    require(Access::plantNoteOn(processor, 102, 62), "plant 102");
    Access::processEventsAt(processor, 105);
    require(Access::mappedIndex(processor) == 3, "all overdue events consumed");
    require(Access::logSize(processor) == 3, "three immediate PSG notes");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 61);
    requireLoggedNote(processor, 2, 105, 62);
    require(Access::frame(processor) == 105, "timeline does not rewind");
    const auto logged = Access::logSize(processor);
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == logged, "second pass adds nothing");
    require(Access::mappedIndex(processor) == 3, "cursor stays past the queue");
}

void testExactFrameStillConsumedAndFutureStaysQueued() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 80, 60), "plant exact");
    require(Access::plantNoteOn(processor, 80, 64), "plant same frame");
    require(Access::plantNoteOn(processor, 90, 67), "plant future");
    Access::processEventsAt(processor, 80);
    require(Access::mappedIndex(processor) == 2, "same-frame pair consumed");
    require(Access::logSize(processor) == 2, "two notes at the exact frame");
    requireLoggedNote(processor, 0, 80, 60);
    requireLoggedNote(processor, 1, 80, 64);
    Access::processEventsAt(processor, 80);
    require(Access::logSize(processor) == 2, "future event stays queued");
    Access::processEventsAt(processor, 90);
    require(Access::mappedIndex(processor) == 3, "future event consumed later");
    requireLoggedNote(processor, 2, 90, 67);
}

void testOverdueThenSameFrameKeepsQueueOrder() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 90, 60), "plant overdue");
    require(Access::plantNoteOn(processor, 105, 61), "plant same a");
    require(Access::plantNoteOn(processor, 105, 62), "plant same b");
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == 3, "overdue and same-frame together");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 61);
    requireLoggedNote(processor, 2, 105, 62);
}

void testOverdueNoteOffCancelsDueLayer() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    juce::AudioBuffer<float> buffer(2, 64);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, on);
    require(Access::scheduler(processor).isPending(scc_track), "SCC pending");

    Access::resetMapped(processor);
    require(Access::plantNoteOff(processor, 9'500, 60), "plant overdue note off");
    Access::processEventsAt(processor, 9'600);
    require(
        Access::scheduler(processor).pendingCount() == 0,
        "overdue NoteOff cancels the due layer");
    require(
        !Access::logHasTrack(processor, scc_track),
        "cancelled due layer must not NoteOn");
    require(Access::frame(processor) == 9'600, "cancel does not rewind");
}

void testOverdueVoiceStealReplacesDueLayer() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto& plan = Access::plan(processor);
    require(plan.voice_capacity == 3, "smoke polyphony is 3");
    const auto* scc = layerOf(plan, mgstc::engine::TimbreSource::Scc);
    require(scc != nullptr, "SCC layer");
    const auto scc_track = plan.physicalTrack(*scc, 0);
    const auto delay = delayMillisecondsToEngineFrames(scc->start_delay_ms);
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant 60");
    require(Access::plantNoteOn(processor, 101, 61), "plant 61");
    require(Access::plantNoteOn(processor, 102, 62), "plant 62");
    require(Access::plantNoteOn(processor, 103, 63), "plant steal");
    Access::processEventsAt(processor, 105);
    const auto* event = Access::scheduler(processor).event(scc_track);
    require(event != nullptr, "stolen voice keeps a delayed SCC");
    require(event->midi_note == 63, "overdue steal keeps the newest note");
    require(
        event->due_frame == 105 + delay,
        "delay is measured from the frame that applied the note");
}

void testOverdueAllNotesOffAndAllSoundOff() {
    {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(48'000.0, 512);
        const auto* scc = layerOf(
            Access::plan(processor), mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
        juce::AudioBuffer<float> buffer(2, 32);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        Access::resetMapped(processor);
        require(Access::plantAllNotesOff(processor, 10), "plant all notes off");
        Access::processEventsAt(processor, 40);
        require(
            Access::scheduler(processor).pendingCount() == 0,
            "overdue All Notes Off clears pending");
        processEmpty(processor, 10'000);
        require(
            !Access::logHasTrack(processor, scc_track),
            "overdue All Notes Off must not start the delayed layer");
    }
    {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(48'000.0, 512);
        const auto* scc = layerOf(
            Access::plan(processor), mgstc::engine::TimbreSource::Scc);
        require(scc != nullptr, "SCC layer");
        const auto scc_track = Access::plan(processor).physicalTrack(*scc, 0);
        juce::AudioBuffer<float> buffer(2, 32);
        juce::MidiBuffer on;
        on.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, on);
        Access::resetMapped(processor);
        require(Access::plantAllSoundOff(processor, 8), "plant all sound off");
        Access::processEventsAt(processor, 40);
        require(
            Access::scheduler(processor).pendingCount() == 0,
            "overdue All Sound Off clears pending");
        require(
            Access::isInactive(processor, scc_track),
            "overdue All Sound Off leaves the layer inactive");
        processEmpty(processor, 10'000);
        require(
            !Access::logHasTrack(processor, scc_track),
            "overdue All Sound Off must not start the delayed layer");
    }
}

void testDirectSpanConsumesOverdueWithoutStalling() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 128);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    Access::resetMapped(processor);
    require(Access::plantNoteOn(processor, 100, 60), "plant overdue");
    require(Access::plantNoteOn(processor, 140, 64), "plant inside the span");
    constexpr int kBlock = 128;
    constexpr std::uint64_t kStart = 105;
    juce::AudioBuffer<float> buffer(2, kBlock);
    Access::renderDirect(processor, buffer, kStart);
    require(
        Access::frame(processor) == kStart + static_cast<std::uint64_t>(kBlock),
        "direct span reaches the block end");
    require(Access::srcWatchdogTrips(processor) == 0, "no watchdog");
    require(Access::mappedIndex(processor) == 2, "both events consumed");
    requireLoggedNote(processor, 0, kStart, 60);
    requireLoggedNote(processor, 1, 140, 64);
    require(bufferAllFinite(buffer), "direct span PCM is finite");
}

void testIteratorOverdueIsConsumedInOrder() {
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)),
        2);
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 64);
    Access::bindMidiIterator(processor, midi, 100, 64);
    Access::processEventsAt(processor, 105);
    require(Access::logSize(processor) == 2, "iterator overdue notes fire");
    requireLoggedNote(processor, 0, 105, 60);
    requireLoggedNote(processor, 1, 105, 64);
    Access::clearMidiIterator(processor);

    juce::MidiBuffer later;
    later.addEvent(
        juce::MidiMessage::noteOn(1, 67, static_cast<juce::uint8>(100)),
        0);
    MgstcAudioProcessor span;
    span.prepareToPlay(48'000.0, 64);
    Access::bindMidiIterator(span, later, 100, 64);
    juce::AudioBuffer<float> buffer(2, 32);
    Access::renderDirect(span, buffer, 105);
    require(
        Access::frame(span) == 137,
        "iterator overdue does not stall the direct span");
    requireLoggedNote(span, 0, 105, 67);
    Access::clearMidiIterator(span);
}

void testBlockBoundaryMidiStaysOnItsFrame() {
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    const auto* psg = psgLayer(processor);
    require(psg != nullptr, "PSG layer");
    const auto track = Access::plan(processor).physicalTrack(*psg, 0);
    juce::AudioBuffer<float> tail(2, 512);
    juce::MidiBuffer end_midi;
    end_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        511);
    processor.processBlock(tail, end_midi);
    require(Access::logHas(processor, 511, track), "last sample of the block");
    juce::AudioBuffer<float> next(2, 512);
    juce::MidiBuffer start_midi;
    start_midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    processor.processBlock(next, start_midi);
    require(Access::frame(processor) == 1'024, "next block continues");
    require(
        Access::isInactive(processor, track),
        "NoteOff at the next block start releases the voice");
}

void testHostRateOverdueNoteIsAppliedNow() {
    constexpr std::array rates{44'100.0, 96'000.0};
    for (const auto rate : rates) {
        MgstcAudioProcessor processor;
        processor.prepareToPlay(rate, 512);
        const auto latency_calls = Access::latencyCalls(processor);
        const auto latency = processor.getLatencySamples();
        const auto* psg = psgLayer(processor);
        require(psg != nullptr, "PSG layer");
        const auto track = Access::plan(processor).physicalTrack(*psg, 0);
        Access::setEngineFrame(processor, 40);
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(
            juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
            0);
        processor.processBlock(buffer, midi);
        require(
            Access::logHas(processor, 40, track),
            "host-mapped frame 0 is applied at the current engine frame");
        require(
            !Access::logHas(processor, 0, track),
            "overdue note must not rewind to frame 0");
        require(Access::frame(processor) > 40, "engine keeps moving forward");
        require(Access::srcWatchdogTrips(processor) == 0, "SRC watchdog quiet");
        require(
            Access::latencyCalls(processor) == latency_calls,
            "overdue handling must not notify latency");
        require(
            processor.getLatencySamples() == latency,
            "plugin latency stays at the prepared value");
        require(bufferAllFinite(buffer), "overdue host block is finite");
    }
}

juce::MemoryBlock stateBytes(MgstcAudioProcessor& processor) {
    juce::MemoryBlock block;
    processor.getStateInformation(block);
    return block;
}

bool sameBlock(const juce::MemoryBlock& left, const juce::MemoryBlock& right) {
    if (left.getSize() != right.getSize()) {
        return false;
    }
    const auto* a = static_cast<const std::uint8_t*>(left.getData());
    const auto* b = static_cast<const std::uint8_t*>(right.getData());
    return std::equal(a, a + left.getSize(), b);
}

bool sameBuffer(
    const juce::AudioBuffer<float>& left,
    const juce::AudioBuffer<float>& right) {
    if (left.getNumChannels() != right.getNumChannels()
        || left.getNumSamples() != right.getNumSamples()) {
        return false;
    }
    for (int channel = 0; channel < left.getNumChannels(); ++channel) {
        const auto* a = left.getReadPointer(channel);
        const auto* b = right.getReadPointer(channel);
        if (!std::equal(a, a + left.getNumSamples(), b)) {
            return false;
        }
    }
    return true;
}

void renderMidiNote(
    MgstcAudioProcessor& processor,
    juce::AudioBuffer<float>& out) {
    processor.prepareToPlay(48'000.0, 256);
    juce::AudioBuffer<float> warm(2, 256);
    juce::MidiBuffer empty;
    processor.processBlock(warm, empty);
    juce::MidiBuffer on;
    on.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    out.setSize(2, 256);
    processor.processBlock(out, on);
}

std::string soundBytes(const mgstc::engine::CompositeTimbre& timbre) {
    return mgstc::engine::serializeCompositeSoundPayload(timbre);
}

mgstc::engine::CompositeTimbre psgOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    timbre.name = "PSG only";
    timbre.memo = "psg";
    timbre.tags = {"psg"};
    timbre.favorite = false;
    timbre.layers = {timbre.layers[0]};
    timbre.layers[0].volume = 11;
    timbre.layers[0].relative_semitones = -2;
    mgstc::engine::EnvelopeEvent pitch;
    pitch.kind = mgstc::engine::EnvelopeEventKind::Pitch;
    pitch.value = -3;
    pitch.count = 4;
    timbre.layers[0].pitch_envelope.events.push_back(pitch);
    return timbre;
}

mgstc::engine::CompositeTimbre sccOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    auto layer = timbre.layers[1];
    mgstc::engine::seedDefaultLayerTimbre(timbre, layer);
    for (std::size_t index = 0; index < layer.base_timbre->scc_waveform.size();
         ++index) {
        layer.base_timbre->scc_waveform[index] =
            static_cast<std::uint8_t>(index * 3U + 1U);
    }
    layer.relative_semitones = 3;
    layer.volume = 12;
    layer.start_delay_form = mgstc::engine::StartDelayForm::AbsoluteTicks;
    layer.start_delay_value = 6;
    layer.software_lfo.enabled = true;
    layer.software_lfo.delay = 1;
    layer.software_lfo.depth = 4;
    layer.software_lfo.speed = 2;
    timbre.name = "SCC only";
    timbre.layers = {std::move(layer)};
    return timbre;
}

mgstc::engine::CompositeTimbre opllOnlyTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    auto layer = timbre.layers[2];
    layer.base_opll_rom = std::uint8_t{4};
    layer.volume = 10;
    layer.relative_semitones = -5;
    layer.opll_sustain = true;
    layer.start_delay_form = mgstc::engine::StartDelayForm::AbsoluteTicks;
    layer.start_delay_value = 12;
    mgstc::engine::EnvelopeEvent slide;
    slide.kind = mgstc::engine::EnvelopeEventKind::Timbre;
    slide.timbre_pick = mgstc::engine::TimbrePick::OpllRom;
    slide.value = 8;
    slide.count = 24;
    layer.timbre_automation.push_back(slide);
    timbre.name = "OPLL only";
    timbre.favorite = true;
    timbre.layers = {std::move(layer)};
    return timbre;
}

mgstc::engine::CompositeTimbre compositeTimbre() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    timbre.name = "Stage E Snapshot";
    timbre.memo = "self-contained";
    timbre.tags = {"lead", "stage-e"};
    timbre.favorite = true;
    auto scc = sccOnlyTimbre().layers[0];
    auto opll = opllOnlyTimbre().layers[0];
    timbre.layers[0] = psgOnlyTimbre().layers[0];
    timbre.layers[1] = std::move(scc);
    timbre.layers[2] = std::move(opll);
    return timbre;
}

mgstc::plugin::PluginStateDocument documentFrom(
    mgstc::engine::CompositeTimbre timbre,
    std::uint64_t library_id,
    mgstc::plugin::PluginEditorState editor) {
    mgstc::plugin::PluginStateDocument document;
    document.sound = std::move(timbre);
    document.library_id = library_id;
    document.editor = editor;
    return document;
}

void requireSoundRoundTrip(const mgstc::plugin::PluginStateDocument& document) {
    const auto bytes = mgstc::plugin::serializePluginState(document);
    require(!bytes.empty(), "plugin state serializes");
    const auto parsed = mgstc::plugin::parsePluginState(
        bytes.data(), bytes.size());
    require(
        parsed.status == mgstc::plugin::PluginStateStatus::Ok,
        "plugin state parses");
    require(
        soundBytes(parsed.document.sound) == soundBytes(document.sound),
        "sound payload round-trip");
    require(parsed.document.sound.name == document.sound.name, "name");
    require(parsed.document.sound.memo == document.sound.memo, "memo");
    require(parsed.document.sound.tags == document.sound.tags, "tags");
    require(
        parsed.document.sound.favorite == document.sound.favorite,
        "favorite");
    require(parsed.document.library_id == document.library_id, "library id");
    require(
        parsed.document.master_volume_percent
            == document.master_volume_percent,
        "master volume");
    require(parsed.document.editor == document.editor, "editor state");
}

std::size_t sourceByteIndex(const mgstc::engine::CompositeTimbre& timbre) {
    return 2U + 4U + 4U + timbre.layers[0].name.size();
}

std::vector<std::uint8_t> toBytes(std::string_view text) {
    std::vector<std::uint8_t> out(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        out[index] = static_cast<std::uint8_t>(
            static_cast<unsigned char>(text[index]));
    }
    return out;
}

std::vector<std::uint8_t> replacePayload(
    std::vector<std::uint8_t> blob,
    const std::vector<std::uint8_t>& original,
    const std::vector<std::uint8_t>& replacement) {
    const auto found = std::search(
        blob.begin(), blob.end(), original.begin(), original.end());
    require(found != blob.end(), "sound payload is inside the plugin state");
    const auto at = static_cast<std::size_t>(std::distance(blob.begin(), found));
    require(at >= 4, "payload size prefix");
    const auto new_size = static_cast<std::uint32_t>(replacement.size());
    blob[at - 4] = static_cast<std::uint8_t>(new_size & 0xFFU);
    blob[at - 3] = static_cast<std::uint8_t>((new_size >> 8U) & 0xFFU);
    blob[at - 2] = static_cast<std::uint8_t>((new_size >> 16U) & 0xFFU);
    blob[at - 1] = static_cast<std::uint8_t>((new_size >> 24U) & 0xFFU);
    blob.erase(
        blob.begin() + static_cast<std::ptrdiff_t>(at),
        blob.begin() + static_cast<std::ptrdiff_t>(at + original.size()));
    blob.insert(
        blob.begin() + static_cast<std::ptrdiff_t>(at),
        replacement.begin(),
        replacement.end());
    return blob;
}

void requireRejected(
    MgstcAudioProcessor& processor,
    const std::vector<std::uint8_t>& bytes) {
    const auto before = stateBytes(processor);
    const auto compiles = Access::stateCompileCount(processor);
    processor.setStateInformation(
        bytes.empty() ? nullptr : bytes.data(),
        static_cast<int>(bytes.size()));
    require(
        Access::stateCompileCount(processor) == compiles,
        "rejected state does not compile");
    require(sameBlock(before, stateBytes(processor)), "rejected state keeps the previous snapshot");
}

void testPluginStateFoundation() {
    require(MgstcAudioProcessor{}.hasEditor(), "plugin exposes the shared editor");

    MgstcAudioProcessor original;
    const auto original_bytes = stateBytes(original);
    require(!original_bytes.isEmpty(), "default state is not empty");
    MgstcAudioProcessor restored;
    restored.setStateInformation(
        original_bytes.getData(),
        static_cast<int>(original_bytes.getSize()));
    require(sameBlock(original_bytes, stateBytes(restored)), "default round-trip");
    const auto smoke = restored.copyPluginState().sound;
    require(smoke.layers.size() == 3, "default composite has three layers");
    require(smoke.layers[1].start_delay_value == 12, "SCC delay snapshot");
    require(smoke.layers[2].start_delay_value == 24, "OPLL delay snapshot");

    const mgstc::plugin::PluginEditorState editor{
        .has_selected_layer = true,
        .selected_layer = 1,
        .editor_tab = 3,
    };
    const auto psg = documentFrom(psgOnlyTimbre(), 0, {});
    const auto scc = documentFrom(sccOnlyTimbre(), 0, editor);
    const auto opll = documentFrom(opllOnlyTimbre(), 382, editor);
    auto composite = documentFrom(compositeTimbre(), 382, editor);
    requireSoundRoundTrip(psg);
    requireSoundRoundTrip(scc);
    requireSoundRoundTrip(opll);
    requireSoundRoundTrip(composite);

    const auto scc_parsed = mgstc::plugin::parsePluginState(
        mgstc::plugin::serializePluginState(scc).data(),
        mgstc::plugin::serializePluginState(scc).size());
    require(
        scc_parsed.document.sound.layers[0].base_timbre->scc_waveform
            == scc.sound.layers[0].base_timbre->scc_waveform,
        "SCC waveform snapshot");
    require(
        scc_parsed.document.sound.layers[0].software_lfo.enabled
            && scc_parsed.document.sound.layers[0].software_lfo.depth == 4,
        "LFO snapshot");
    require(
        scc_parsed.document.sound.layers[0].start_delay_value == 6,
        "layer delay snapshot");
    const auto opll_parsed = mgstc::plugin::parsePluginState(
        mgstc::plugin::serializePluginState(opll).data(),
        mgstc::plugin::serializePluginState(opll).size());
    require(
        mgstc::engine::layerEnvelopeHasPatchSlide(
            opll_parsed.document.sound.layers[0], nullptr),
        "patch slide snapshot");
    require(
        !opll_parsed.document.sound.layers[0].pitch_envelope.events.empty()
            || opll_parsed.document.sound.layers[0].volume_envelope.events.size()
                > 0,
        "envelope snapshot");
    require(
        psg.sound.layers[0].pitch_envelope.events.back().value == -3,
        "pitch envelope value");

    MgstcAudioProcessor player_a;
    require(player_a.replacePluginState(composite), "commit composite");
    const auto committed = stateBytes(player_a);
    MgstcAudioProcessor player_b;
    player_b.setStateInformation(
        committed.getData(), static_cast<int>(committed.getSize()));
    juce::AudioBuffer<float> pcm_a;
    juce::AudioBuffer<float> pcm_b;
    renderMidiNote(player_a, pcm_a);
    renderMidiNote(player_b, pcm_b);
    require(sameBuffer(pcm_a, pcm_b), "restored program matches PCM");
    require(bufferAllFinite(pcm_a), "restored PCM is finite");

    auto library_a = composite;
    library_a.library_id = 0;
    auto library_b = composite;
    library_b.library_id = 382;
    require(
        soundBytes(library_a.sound) == soundBytes(library_b.sound),
        "library id is not part of the sound payload");
    MgstcAudioProcessor lib_left;
    MgstcAudioProcessor lib_right;
    require(lib_left.replacePluginState(library_a), "library 0");
    require(lib_right.replacePluginState(library_b), "library 382");
    require(lib_right.copyPluginState().library_id == 382, "library metadata");
    juce::AudioBuffer<float> lib_pcm_a;
    juce::AudioBuffer<float> lib_pcm_b;
    renderMidiNote(lib_left, lib_pcm_a);
    renderMidiNote(lib_right, lib_pcm_b);
    require(sameBuffer(lib_pcm_a, lib_pcm_b), "library id does not change PCM");

    MgstcAudioProcessor instance_a;
    MgstcAudioProcessor instance_b;
    require(instance_a.replacePluginState(psg), "instance A");
    require(instance_b.replacePluginState(opll), "instance B");
    const auto instance_b_bytes = stateBytes(instance_b);
    require(instance_a.replacePluginState(scc), "instance A changes");
    require(
        sameBlock(instance_b_bytes, stateBytes(instance_b)),
        "instance B is unchanged");
    require(
        instance_a.copyPluginState().sound.name
            != instance_b.copyPluginState().sound.name,
        "instances keep different programs");

    const auto valid = mgstc::plugin::serializePluginState(psg);
    require(
        mgstc::plugin::parsePluginState(nullptr, 0).status
            == mgstc::plugin::PluginStateStatus::Empty,
        "empty");
    require(
        mgstc::plugin::parsePluginState(valid.data(), 4).status
            == mgstc::plugin::PluginStateStatus::Truncated,
        "truncated header");
    std::vector<std::uint8_t> bad_magic(8, static_cast<std::uint8_t>('X'));
    require(
        mgstc::plugin::parsePluginState(bad_magic.data(), bad_magic.size()).status
            == mgstc::plugin::PluginStateStatus::BadMagic,
        "bad magic");
    auto unknown = valid;
    unknown[8] = 3;
    require(
        mgstc::plugin::parsePluginState(unknown.data(), unknown.size()).status
            == mgstc::plugin::PluginStateStatus::UnsupportedVersion,
        "unknown version");
    auto trailing = valid;
    trailing.push_back(0);
    require(
        mgstc::plugin::parsePluginState(trailing.data(), trailing.size()).status
            == mgstc::plugin::PluginStateStatus::BrokenPayload,
        "trailing bytes");

    auto payload = toBytes(soundBytes(psg.sound));
    const auto source_at = sourceByteIndex(psg.sound);
    require(payload.size() > source_at + 1, "payload has a source byte");
    require(
        payload[source_at]
            == static_cast<std::uint8_t>(psg.sound.layers[0].source),
        "source byte layout");
    auto invalid_enum = payload;
    invalid_enum[source_at] = 9;
    auto oversized = payload;
    oversized[2] = 0xFF;
    oversized[3] = 0xFF;
    auto bad_channel = payload;
    bad_channel[source_at + 1] = 255;
    auto short_wave = payload;
    short_wave.resize(short_wave.size() - 8);

    MgstcAudioProcessor guard;
    requireRejected(guard, {});
    requireRejected(guard, std::vector<std::uint8_t>(valid.begin(), valid.begin() + 12));
    requireRejected(guard, bad_magic);
    requireRejected(guard, unknown);
    requireRejected(guard, trailing);
    requireRejected(guard, replacePayload(valid, payload, invalid_enum));
    requireRejected(guard, replacePayload(valid, payload, oversized));
    requireRejected(guard, replacePayload(valid, payload, bad_channel));
    requireRejected(guard, replacePayload(valid, payload, short_wave));

    auto corrupt = guard.copyPluginState();
    corrupt.sound.layers[0].channel = 255;
    const auto compiles = Access::stateCompileCount(guard);
    const auto before_direct = stateBytes(guard);
    require(!guard.replacePluginState(corrupt), "invalid channel is rejected");
    require(Access::stateCompileCount(guard) == compiles, "invalid channel does not compile");
    require(sameBlock(before_direct, stateBytes(guard)), "direct reject keeps state");

    MgstcAudioProcessor audio_guard;
    const auto audio_before = stateBytes(audio_guard);
    const auto audio_compiles = Access::stateCompileCount(audio_guard);
    Access::poseAsAudioThread(audio_guard);
    audio_guard.setStateInformation(valid.data(), static_cast<int>(valid.size()));
    require(
        Access::stateAudioRejectCount(audio_guard) == 1,
        "audio thread rejects state");
    require(
        Access::stateCompileCount(audio_guard) == audio_compiles,
        "audio thread does not compile");
    Access::clearAudioThread(audio_guard);
    require(sameBlock(audio_before, stateBytes(audio_guard)), "audio-thread reject keeps state");

    MgstcAudioProcessor timed;
    timed.prepareToPlay(44'100.0, 128);
    juce::AudioBuffer<float> attack(2, 128);
    juce::MidiBuffer note;
    note.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    timed.processBlock(attack, note);
    require(Access::noteOnCount(timed) > 0, "note is sounding");
    require(Access::scheduler(timed).pendingCount() > 0, "delayed layer is pending");
    require(timed.replacePluginState(psg), "replace while prepared");
    processEmpty(timed, 48'000);
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Psg) != nullptr, "PSG remains");
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Scc) == nullptr, "old SCC delay is gone");
    require(layerOf(Access::plan(timed), mgstc::engine::TimbreSource::Opll) == nullptr, "old OPLL delay is gone");
    require(Access::scheduler(timed).pendingCount() == 0, "old pending layers are dropped");

    MgstcAudioProcessor rates;
    require(rates.replacePluginState(psg), "program before rate changes");
    rates.prepareToPlay(44'100.0, 512);
    const auto latency_calls = Access::latencyCalls(rates);
    const auto latency = rates.getLatencySamples();
    require(latency == 7, "44.1 kHz latency stays 7");
    require(rates.replacePluginState(scc), "restore does not touch latency");
    processEmpty(rates, 512);
    require(Access::latencyCalls(rates) == latency_calls, "setState does not report latency");
    require(rates.getLatencySamples() == latency, "latency value unchanged by setState");
    const auto at_441 = stateBytes(rates);
    rates.prepareToPlay(48'000.0, 512);
    require(rates.getLatencySamples() == 0, "48 kHz latency is still 0");
    require(sameBlock(at_441, stateBytes(rates)), "48 kHz keeps the sound snapshot");
    rates.prepareToPlay(96'000.0, 256);
    require(sameBlock(at_441, stateBytes(rates)), "96 kHz keeps the sound snapshot");
    rates.prepareToPlay(88'200.0, 256);
    require(sameBlock(at_441, stateBytes(rates)), "88.2 kHz keeps the sound snapshot");
    require(rates.getLatencySamples() == 15, "88.2 kHz latency is unchanged by state");

    MgstcAudioProcessor audible;
    audible.setStateInformation(valid.data(), static_cast<int>(valid.size()));
    audible.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> heard(2, 512);
    juce::MidiBuffer heard_midi;
    heard_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    audible.processBlock(heard, heard_midi);
    double peak = channelPeak(heard, 0, 512);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 512);
        juce::MidiBuffer empty;
        audible.processBlock(more, empty);
        peak = channelPeak(more, 0, 512);
    }
    require(peak > 0.001, "restored PSG is audible at 48 kHz");
    audible.prepareToPlay(44'100.0, 512);
    juce::AudioBuffer<float> heard_44(2, 512);
    juce::MidiBuffer midi_44;
    midi_44.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    audible.processBlock(heard_44, midi_44);
    peak = channelPeak(heard_44, 0, 512);
    if (peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 512);
        juce::MidiBuffer empty;
        audible.processBlock(more, empty);
        peak = channelPeak(more, 0, 512);
    }
    require(peak > 0.001, "restored PSG is audible at 44.1 kHz");
}

std::vector<float> captureHostNotePcm(
    MgstcAudioProcessor& processor,
    int frames = 2048) {
    juce::AudioBuffer<float> buffer(2, frames);
    juce::MidiBuffer midi;
    midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    processor.processBlock(buffer, midi);
    std::vector<float> pcm(static_cast<std::size_t>(frames));
    const auto* left = buffer.getReadPointer(0);
    for (int index = 0; index < frames; ++index) {
        pcm[static_cast<std::size_t>(index)] = left[index];
    }
    return pcm;
}

double peakOf(const std::vector<float>& pcm) {
    double peak = 0.0;
    for (const auto sample : pcm) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    return peak;
}

void requireWorkingCompositeMatchesProcessor(
    juce::AudioProcessorEditor* editor,
    MgstcAudioProcessor& processor,
    const char* message) {
    auto* plugin_editor =
        dynamic_cast<mgstc::plugin::MgstcAudioProcessorEditor*>(editor);
    require(plugin_editor != nullptr, "plugin editor type");
    require(
        soundBytes(plugin_editor->copyWorkingComposite())
            == soundBytes(processor.copyPluginState().sound),
        message);
}

std::vector<std::uint8_t> asLegacyV1(std::vector<std::uint8_t> v2) {
    require(v2.size() > 36, "v2 blob has master volume field");
    require(v2[8] == 2, "current serializer writes schema v2");
    v2[8] = 1;
    v2.erase(v2.begin() + 32, v2.begin() + 36);
    return v2;
}

void testPluginProjectRestore() {
    auto composite = documentFrom(compositeTimbre(), 7, {});
    composite.master_volume_percent = 40;
    composite.sound.layers[1].enabled = false;
    requireSoundRoundTrip(composite);

    MgstcAudioProcessor saved;
    require(saved.replacePluginState(composite), "commit E4 composite");
    require(saved.editorMasterVolumePercent() == 40, "processor stores master 40");
    const auto blob = stateBytes(saved);

    MgstcAudioProcessor before_prepare;
    before_prepare.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    require(
        before_prepare.editorMasterVolumePercent() == 40,
        "restore before prepareToPlay keeps master volume");
    require(
        soundBytes(before_prepare.copyPluginState().sound)
            == soundBytes(composite.sound),
        "restore before prepareToPlay keeps sound");
    require(
        !before_prepare.copyPluginState().sound.layers[1].enabled,
        "disabled SCC layer round-trips");
    before_prepare.prepareToPlay(48'000.0, 512);
    const auto restored_pcm = captureHostNotePcm(before_prepare);
    require(peakOf(restored_pcm) > 0.001, "editor-less restore is audible");

    MgstcAudioProcessor after_prepare;
    after_prepare.prepareToPlay(48'000.0, 512);
    after_prepare.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    require(
        after_prepare.editorMasterVolumePercent() == 40,
        "restore after prepareToPlay keeps master volume");
    processEmpty(after_prepare, 512);
    processEmpty(after_prepare, 512);
    const auto live_pcm = captureHostNotePcm(after_prepare);
    require(peakOf(live_pcm) > 0.001, "restore after prepareToPlay is audible");

    MgstcAudioProcessor reference;
    require(reference.replacePluginState(composite), "reference 40%");
    reference.prepareToPlay(48'000.0, 512);
    const auto reference_pcm = captureHostNotePcm(reference);
    require(
        std::abs(peakOf(restored_pcm) / peakOf(reference_pcm) - 1.0) < 0.05,
        "restored master volume matches a fresh processor");

    MgstcAudioProcessor hydrate;
    hydrate.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    const auto compiles = Access::stateCompileCount(hydrate);
    auto* editor = hydrate.createEditor();
    requireWorkingCompositeMatchesProcessor(
        editor,
        hydrate,
        "createEditor after restore hydrates Processor composite");
    require(
        hydrate.copyPluginState().sound.name == composite.sound.name,
        "editor open does not replace restored sound");
    require(
        hydrate.editorMasterVolumePercent() == 40,
        "editor open does not reset restored master volume");
    require(
        Access::stateCompileCount(hydrate) == compiles,
        "editor open after restore does not compile");
    delete editor;

    const auto v1 = asLegacyV1(
        mgstc::plugin::serializePluginState(composite));
    const auto v1_parsed = mgstc::plugin::parsePluginState(
        v1.data(), v1.size());
    require(
        v1_parsed.status == mgstc::plugin::PluginStateStatus::Ok,
        "schema v1 still parses");
    require(
        v1_parsed.document.master_volume_percent == 100,
        "schema v1 restores master volume as 100");
    require(
        soundBytes(v1_parsed.document.sound) == soundBytes(composite.sound),
        "schema v1 keeps the sound payload");

    MgstcAudioProcessor v1_host;
    v1_host.editorSetMasterVolumePercent(25);
    v1_host.setStateInformation(v1.data(), static_cast<int>(v1.size()));
    require(
        v1_host.editorMasterVolumePercent() == 100,
        "v1 project restore applies default master 100");

    const auto* blob_begin =
        static_cast<const std::uint8_t*>(blob.getData());
    auto future = std::vector<std::uint8_t>(
        blob_begin, blob_begin + blob.getSize());
    future[8] = 3;
    requireRejected(saved, future);

    auto impossible = future;
    impossible[8] = 2;
    impossible[32] = 101;
    impossible[33] = 0;
    impossible[34] = 0;
    impossible[35] = 0;
    require(
        saved.editorMasterVolumePercent() == 40, "guard still at 40");
    requireRejected(saved, impossible);
    require(
        saved.editorMasterVolumePercent() == 40,
        "impossible master volume does not partial-apply");

    MgstcAudioProcessor instance_a;
    MgstcAudioProcessor instance_b;
    auto left = documentFrom(psgOnlyTimbre(), 0, {});
    left.master_volume_percent = 30;
    auto right = documentFrom(opllOnlyTimbre(), 0, {});
    right.master_volume_percent = 70;
    require(instance_a.replacePluginState(left), "instance A E4");
    require(instance_b.replacePluginState(right), "instance B E4");
    const auto a_bytes = stateBytes(instance_a);
    const auto b_bytes = stateBytes(instance_b);
    MgstcAudioProcessor restore_a;
    MgstcAudioProcessor restore_b;
    restore_a.setStateInformation(
        a_bytes.getData(), static_cast<int>(a_bytes.getSize()));
    restore_b.setStateInformation(
        b_bytes.getData(), static_cast<int>(b_bytes.getSize()));
    require(
        restore_a.editorMasterVolumePercent() == 30
            && restore_b.editorMasterVolumePercent() == 70,
        "multi-instance master volumes stay independent");
    require(
        restore_a.copyPluginState().sound.name
            != restore_b.copyPluginState().sound.name,
        "multi-instance composites stay independent");

    MgstcAudioProcessor at_441;
    at_441.setStateInformation(
        blob.getData(), static_cast<int>(blob.getSize()));
    at_441.prepareToPlay(44'100.0, 512);
    require(
        peakOf(captureHostNotePcm(at_441)) > 0.001,
        "44.1 kHz restore is audible");
    require(
        at_441.getLatencySamples() == 7,
        "44.1 kHz restore does not touch latency");
}

void testPluginEditor() {
    MgstcAudioProcessor processor;
    const auto before = stateBytes(processor);
    const auto compiles = Access::stateCompileCount(processor);
    const auto latency = processor.getLatencySamples();
    require(!processor.producesMidi(), "plugin does not produce MIDI");

    auto* editor = processor.createEditor();
    require(editor != nullptr, "createEditor");
    require(
        Access::stateCompileCount(processor) == compiles,
        "opening the editor does not compile");
    require(sameBlock(before, stateBytes(processor)), "opening the editor keeps state");
    require(
        processor.getLatencySamples() == latency,
        "opening the editor keeps latency");
    require(
        Access::blockedBackendCalls(processor) == 0,
        "opening the editor does not enumerate devices");
    require(!Access::scopeEnabled(processor), "waveform scope stays off");

    bool spectrum_disabled = false;
    bool library_disabled = false;
    const auto walk = [&](auto&& self, juce::Component* component) -> void {
        if (auto* button = dynamic_cast<juce::Button*>(component)) {
            const auto text = button->getButtonText();
            if (text == juce::String::fromUTF8("スペアナ")) {
                require(!button->isEnabled(), "spectrum control stays in place and is grey");
                spectrum_disabled = true;
            }
            if (text == juce::String::fromUTF8("ライブラリ管理")) {
                require(!button->isEnabled(), "tone library control stays in place and is grey");
                library_disabled = true;
            }
        }
        for (int index = 0; index < component->getNumChildComponents(); ++index) {
            self(self, component->getChildComponent(index));
        }
    };
    walk(walk, editor);
    require(spectrum_disabled, "spectrum button is present");
    require(library_disabled, "library button is present");
    delete editor;
    require(sameBlock(before, stateBytes(processor)), "closing the editor keeps state");
    require(
        Access::stateCompileCount(processor) == compiles,
        "closing the editor does not recompile");
    require(
        Access::blockedBackendCalls(processor) == 0,
        "closing the editor does not enumerate devices");

    MgstcAudioProcessor other;
    const auto other_before = stateBytes(other);
    {
        PluginEditorContext context(processor);
        auto timbre = processor.copyPluginState().sound;
        timbre.name = "instance-a";
        mgstc::app::CompositeAuditionRequest request;
        request.timbre = &timbre;
        request.polyphonic = true;
        const auto applied = context.audition().submitComposite(request);
        require(applied.ok, "composite edit reaches this instance");
        require(
            processor.copyPluginState().sound.name == "instance-a",
            "edited name is stored on this instance");
        require(
            sameBlock(other_before, stateBytes(other)),
            "another instance is unchanged");
        require(
            context.snapshot().capabilities.spectrum_analyzer == false
                && context.snapshot().capabilities.tone_library == false
                && context.snapshot().capabilities.on_screen_keyboard,
            "plugin capabilities stay off except the keyboard");
        require(
            context.output().availableAsioDrivers().empty(),
            "plugin does not list ASIO drivers");
    }
    require(
        Access::blockedBackendCalls(processor) == 1,
        "ASIO enumeration is the rejected backend call");

    const auto edited = stateBytes(processor);
    editor = processor.createEditor();
    require(sameBlock(edited, stateBytes(processor)), "reopen does not revert the edit");
    delete editor;

    processor.prepareToPlay(48'000.0, 512);
    const auto daw_notes = Access::noteOnCount(processor);
    {
        PluginEditorContext context(processor);
        require(context.audition().noteOn(0, 60), "editor keyboard note");
        double peak = 0.0;
        for (int block = 0; block < 8 && peak <= 0.001; ++block) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            processor.processBlock(buffer, midi);
            require(midi.isEmpty(), "editor keyboard does not write host MIDI");
            peak = std::max(peak, channelPeak(buffer, 0, 512));
        }
        require(peak > 0.001, "editor keyboard is audible");
        require(
            Access::noteOnCount(processor) == daw_notes,
            "editor keyboard does not use the DAW note path");
    }

    MgstcAudioProcessor headless;
    headless.prepareToPlay(48'000.0, 512);
    juce::AudioBuffer<float> heard(2, 512);
    juce::MidiBuffer host_midi;
    host_midi.addEvent(
        juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)),
        0);
    headless.processBlock(heard, host_midi);
    double headless_peak = channelPeak(heard, 0, 512);
    if (headless_peak <= 0.001) {
        juce::AudioBuffer<float> more(2, 2048);
        juce::MidiBuffer empty;
        headless.processBlock(more, empty);
        headless_peak = channelPeak(more, 0, 2048);
    }
    require(headless_peak > 0.001, "audio works with no editor");
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce_init;
    try {
        testSincRingMatchesLinearShift();
        testNoteOnAtSample128DoesNotAffectPrefix();
        testInvalidSampleRateIsSilentAndDoesNotAdvanceEngine();
        testHostEngineMapping();
        testCubaseFirstNoteOnAfterSilence();
        testEngineNeverLeadsHostMapping();
        testRealtimeDeadlineCost();
        profileEngineIdleBreakdown();
        testIdleDurationChangesNextNotePcm();
        testNeverPlayedIsQuiescentAndReleaseStillSounds();
        testBatchSourceRenderCallCount();
        testMidBlockNoteIsNotRoundedToBatchStart();
        testNon48kHzProducesAudio();
        testNon48kHzOutputIsFinite();
        testLatencyReportedOnlyFromPrepareAndRelease();
        testHostBufferContractAcrossRates();
        test48kHzBypassesSrcAndHasZeroLatency();
        test44100MidiOffsetMapsToEngine480();
        testLayerDelayIs9600EngineFramesAtEveryHostRate();
        testCancellationAt44100();
        testSameFrameCancelAt44100();
        testVoiceStealAndAllNotesOffAt44100();
        testAllSoundOffAt44100();
        testBlockSizeInvarianceAt44100();
        testLongRunDriftAt44100();
        testRateChangeResetsSrcTimelineAndLatency();
        testTwoProcessorsDoNotShareSrcState();
        testPitchIndependentOfHostRate();
        testDurationIndependentOfHostRate();
        testTwoProcessorsDoNotShareVoiceState();
        testZeroDelayFiresAtMidiFrame();
        testDelayedLayersFireAtExactDueAndAcrossBlocks();
        testNoteOffBeforeDueCancelsDelayedLayers();
        testNoteOffAfterDueLeavesReleasePath();
        testSameFrameNoteOffCancelsDueEvent();
        testVoiceStealCancelsOldPending();
        testAllNotesOffCancelsPending();
        testAllSoundOffCancelsPending();
        testLifecycleResetDropsPending();
        testProgramResetDropsPending();
        testOverdueMappedEventsKeepOrderAndDoNotStick();
        testExactFrameStillConsumedAndFutureStaysQueued();
        testOverdueThenSameFrameKeepsQueueOrder();
        testOverdueNoteOffCancelsDueLayer();
        testOverdueVoiceStealReplacesDueLayer();
        testOverdueAllNotesOffAndAllSoundOff();
        testDirectSpanConsumesOverdueWithoutStalling();
        testIteratorOverdueIsConsumedInOrder();
        testBlockBoundaryMidiStaysOnItsFrame();
        testHostRateOverdueNoteIsAppliedNow();
        testPluginStateFoundation();
        testPluginProjectRestore();
        testPluginEditor();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
