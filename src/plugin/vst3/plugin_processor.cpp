// SPDX-License-Identifier: AGPL-3.0-only

#include "plugin_processor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>

#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/opll_register_auto.hpp"

namespace mgstc::plugin {
namespace {

[[nodiscard]] std::uint64_t currentThreadToken() noexcept {
    static std::atomic<std::uint64_t> next{1};
    thread_local const std::uint64_t token =
        next.fetch_add(1, std::memory_order_relaxed);
    return token;
}

struct AudioThreadBinding {
    std::atomic<std::uint64_t>& token;
    explicit AudioThreadBinding(std::atomic<std::uint64_t>& token) noexcept
        : token(token) {
        token.store(currentThreadToken(), std::memory_order_relaxed);
    }
    ~AudioThreadBinding() {
        token.store(0, std::memory_order_relaxed);
    }
};

[[nodiscard]] int clampedMidiSample(int sample_position, int num_samples) noexcept {
    if (sample_position < 0) {
        return 0;
    }
    if (sample_position > num_samples) {
        return num_samples;
    }
    return sample_position;
}

[[nodiscard]] bool validPhysicalTrack(std::uint8_t track) noexcept {
    return track < LayerDelayScheduler::kPhysicalTrackCount;
}

[[nodiscard]] std::uint64_t nowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct NsAccum {
    std::uint64_t& dest;
    std::uint64_t start;
    explicit NsAccum(std::uint64_t& accumulator) noexcept
        : dest(accumulator), start(nowNs()) {}
    ~NsAccum() { dest += nowNs() - start; }
};

mgstc::engine::CompositeTimbre stageCSmokeTimbre() {
    using namespace mgstc::engine;
    auto timbre = defaultCompositeTimbre();
    // VST3 Stage C smoke only. Immediate PSG plus delayed SCC / OPLL so a
    // DAW can hear the scheduler. Standalone defaultCompositeTimbre() is
    // unchanged.
    if (timbre.layers.size() >= 2) {
        timbre.layers[1].start_delay_form = StartDelayForm::AbsoluteTicks;
        timbre.layers[1].start_delay_value = 12;  // 200 ms at 1/60 s
    }
    if (timbre.layers.size() >= 3) {
        timbre.layers[2].start_delay_form = StartDelayForm::AbsoluteTicks;
        timbre.layers[2].start_delay_value = 24;  // 400 ms
    }
    return timbre;
}

}  // namespace

MgstcAudioProcessor::MgstcAudioProcessor()
    : juce::AudioProcessor(
          BusesProperties().withOutput(
              "Output", juce::AudioChannelSet::stereo(), true)) {
    engine_.setOpllScopeEnabled(false);
    engine_.setSpectrumCaptureEnabled(false);
    engine_.setSpectrogramCaptureEnabled(false);
    PluginStateDocument document;
    document.sound = stageCSmokeTimbre();
    static_cast<void>(commitPluginState(std::move(document), CommitMode::Initial));
}

MgstcAudioProcessor::~MgstcAudioProcessor() {
    writeDiagnosticSnapshot("destructor");
}

void MgstcAudioProcessor::reportPluginLatency(int samples) noexcept {
    diag_latency_calls_.fetch_add(1, std::memory_order_relaxed);
    if (in_process_block_) {
        diag_latency_from_process_.fetch_add(1, std::memory_order_relaxed);
    }
    if (samples == getLatencySamples()) {
        diag_latency_same_.fetch_add(1, std::memory_order_relaxed);
    } else {
        diag_latency_changed_.fetch_add(1, std::memory_order_relaxed);
    }
    setLatencySamples(samples);
}

bool MgstcAudioProcessor::hostIsEngineRate() const noexcept {
    if (host_rate_hz_ != 0) {
        return host_rate_hz_
            == static_cast<std::uint32_t>(kEngineSampleRate);
    }
    const double sr = getSampleRate();
    return std::isfinite(sr) && std::abs(sr - kEngineSampleRate) < 1.0;
}

void MgstcAudioProcessor::recordProcessBlockContract(
    const juce::AudioBuffer<float>& buffer,
    const juce::MidiBuffer& midi) noexcept {
    diag_last_num_samples_.store(
        static_cast<std::uint32_t>(std::max(0, buffer.getNumSamples())),
        std::memory_order_relaxed);
    diag_last_buffer_channels_.store(
        static_cast<std::uint32_t>(std::max(0, buffer.getNumChannels())),
        std::memory_order_relaxed);
    diag_last_input_channels_.store(
        static_cast<std::uint32_t>(
            std::max(0, getTotalNumInputChannels())),
        std::memory_order_relaxed);
    diag_last_output_channels_.store(
        static_cast<std::uint32_t>(
            std::max(0, getTotalNumOutputChannels())),
        std::memory_order_relaxed);
    diag_last_midi_events_.store(
        static_cast<std::uint32_t>(std::max(0, midi.getNumEvents())),
        std::memory_order_relaxed);
    if (host_rate_hz_ != 0) {
        const auto previous = diag_process_sample_rate_hz_.exchange(
            host_rate_hz_, std::memory_order_relaxed);
        if (previous != 0 && previous != host_rate_hz_) {
            diag_sample_rate_changes_in_process_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    const double sr = getSampleRate();
    if (std::isfinite(sr) && sr > 0.0 && host_rate_hz_ != 0) {
        const auto rounded = static_cast<std::uint32_t>(std::llround(sr));
        if (rounded != host_rate_hz_) {
            diag_sample_rate_changes_in_process_.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void MgstcAudioProcessor::writeDiagnosticSnapshot(
    const char* reason) const noexcept {
    if constexpr (kVst3DiagMode == kVst3DiagOff) {
        return;
    }
    try {
        auto dir = juce::File::getSpecialLocation(
                       juce::File::windowsLocalAppData)
                       .getChildFile("MgsToneCraft");
        if (!dir.isDirectory() && !dir.createDirectory()) {
            return;
        }
        char text[2048];
        std::snprintf(
            text,
            sizeof(text),
            "mode=%s\n"
            "reason=%s\n"
            "processBlock=%llu\n"
            "prepareToPlay=%llu\n"
            "releaseResources=%llu\n"
            "reset=%llu\n"
            "setLatencySamples_calls=%llu\n"
            "setLatencySamples_same=%llu\n"
            "setLatencySamples_changed=%llu\n"
            "setLatencySamples_from_processBlock=%llu\n"
            "isBusesLayoutSupported=%llu\n"
            "numChannelsChanged=%llu\n"
            "numBusesChanged=%llu\n"
            "processorLayoutsChanged=%llu\n"
            "last_num_samples=%u\n"
            "last_buffer_channels=%u\n"
            "last_input_channels=%u\n"
            "last_output_channels=%u\n"
            "last_midi_events=%u\n"
            "process_sample_rate_hz=%u\n"
            "juce_getSampleRate_hz=%.1f\n"
            "sample_rate_changes_in_processBlock=%llu\n"
            "last_samples_per_block=%d\n"
            "host_rate_hz=%u\n"
            "reported_latency=%d\n"
            "use_src=%d\n"
            "scoped_no_denormals=%d\n",
            vst3DiagnosticModeName(),
            reason != nullptr ? reason : "",
            static_cast<unsigned long long>(
                diag_process_block_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_prepare_to_play_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_release_resources_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_reset_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_latency_calls_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_latency_same_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_latency_changed_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_latency_from_process_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_buses_layout_supported_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_num_channels_changed_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_num_buses_changed_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                diag_layouts_changed_.load(std::memory_order_relaxed)),
            diag_last_num_samples_.load(std::memory_order_relaxed),
            diag_last_buffer_channels_.load(std::memory_order_relaxed),
            diag_last_input_channels_.load(std::memory_order_relaxed),
            diag_last_output_channels_.load(std::memory_order_relaxed),
            diag_last_midi_events_.load(std::memory_order_relaxed),
            diag_process_sample_rate_hz_.load(std::memory_order_relaxed),
            getSampleRate(),
            static_cast<unsigned long long>(
                diag_sample_rate_changes_in_process_.load(
                    std::memory_order_relaxed)),
            diag_last_samples_per_block_.load(std::memory_order_relaxed),
            host_rate_hz_,
            getLatencySamples(),
            use_src_ ? 1 : 0,
            kProcessBlockUsesScopedNoDenormals ? 1 : 0);
        dir.getChildFile("vst3-d2-diag.txt").replaceWithText(text);
    } catch (...) {
    }
}

int MgstcAudioProcessor::srcLatencyHostSamples(
    std::uint32_t host_rate_hz) noexcept {
    if (host_rate_hz == 0
        || host_rate_hz
            == static_cast<std::uint32_t>(kEngineSampleRate)) {
        return 0;
    }
    return static_cast<int>(std::llround(
        static_cast<double>(
            mgstc::audio::StereoSampleRateConverter::kGroupDelaySourceFrames)
        * static_cast<double>(host_rate_hz)
        / kEngineSampleRate));
}

void MgstcAudioProcessor::resetPlaybackState(bool hard_stop) noexcept {
    scheduler_.reset();
    std::array<std::uint8_t, kMaxVoices> ignored{};
    static_cast<void>(voices_.allNotesOff(ignored));
    if (hard_stop) {
        engine_.realtimeStop();
    }
    track_state_.fill(TrackLayerState::Inactive);
    note_on_log_size_ = 0;
}

void MgstcAudioProcessor::resetProgramState() noexcept {
    resetPlaybackState(true);
    engine_frame_position_ = 0;
    host_frame_position_ = 0;
    src_.reset();
    if (use_src_ && sample_rate_ok_) {
        src_.primeWithSilence();
    }
}

void MgstcAudioProcessor::prepareToPlay(
    double sampleRate,
    int samplesPerBlock) {
    diag_prepare_to_play_.fetch_add(1, std::memory_order_relaxed);
    diag_last_samples_per_block_.store(
        samplesPerBlock, std::memory_order_relaxed);
    diag_process_sample_rate_hz_.store(0, std::memory_order_relaxed);
    diag_sample_rate_changes_in_process_.store(0, std::memory_order_relaxed);
    resetPlaybackState(true);
    engine_frame_position_ = 0;
    host_frame_position_ = 0;
    src_.reset();
    use_src_ = false;
    sample_rate_ok_ = false;
    host_rate_hz_ = 0;
    host_prepared_.store(true, std::memory_order_release);
    if (program_ready_.load(std::memory_order_acquire)) {
        const auto index = published_plan_.load(std::memory_order_acquire);
        const auto capacity = plan_slots_[index].voice_capacity;
        voices_.setActiveChannelCount(capacity == 0 ? 1 : capacity);
        voices_.setPolyphonic(true);
    }

    int latency = 0;
    if (program_ready_
        && std::isfinite(sampleRate)
        && sampleRate > 0.0) {
        if (std::abs(sampleRate - kEngineSampleRate) < 1.0) {
            sample_rate_ok_ = true;
            use_src_ = false;
            host_rate_hz_ = static_cast<std::uint32_t>(kEngineSampleRate);
            latency = 0;
        } else {
            const auto rounded = std::llround(sampleRate);
            if (rounded > 0
                && rounded <= static_cast<long long>(
                       std::numeric_limits<std::uint32_t>::max())) {
                host_rate_hz_ = static_cast<std::uint32_t>(rounded);
                if (src_.prepare(
                        static_cast<double>(host_rate_hz_),
                        kSrcChunkMaxFrames)) {
                    src_.primeWithSilence();
                    use_src_ = true;
                    sample_rate_ok_ = true;
                    latency = srcLatencyHostSamples(host_rate_hz_);
                } else {
                    host_rate_hz_ = 0;
                    latency = 0;
                }
            }
        }
    }
    if constexpr (vst3DiagForcesZeroLatency()) {
        latency = 0;
    }
    reportPluginLatency(latency);
}

void MgstcAudioProcessor::releaseResources() {
    diag_release_resources_.fetch_add(1, std::memory_order_relaxed);
    resetPlaybackState(true);
    engine_frame_position_ = 0;
    host_frame_position_ = 0;
    src_.reset();
    use_src_ = false;
    sample_rate_ok_ = false;
    host_rate_hz_ = 0;
    // Do not report latency 0 here. 7↔0 on deactivate/activate is the
    // Cubase restartComponent loop isolated in Stage D.2.
    host_prepared_.store(false, std::memory_order_release);
}

void MgstcAudioProcessor::reset() {
    diag_reset_.fetch_add(1, std::memory_order_relaxed);
    juce::AudioProcessor::reset();
}

bool MgstcAudioProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const {
    diag_buses_layout_supported_.fetch_add(1, std::memory_order_relaxed);
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::disabled()) {
        return false;
    }
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void MgstcAudioProcessor::numChannelsChanged() {
    diag_num_channels_changed_.fetch_add(1, std::memory_order_relaxed);
    juce::AudioProcessor::numChannelsChanged();
}

void MgstcAudioProcessor::numBusesChanged() {
    diag_num_buses_changed_.fetch_add(1, std::memory_order_relaxed);
    juce::AudioProcessor::numBusesChanged();
}

void MgstcAudioProcessor::processorLayoutsChanged() {
    diag_layouts_changed_.fetch_add(1, std::memory_order_relaxed);
    juce::AudioProcessor::processorLayoutsChanged();
}

void MgstcAudioProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi) {
    in_process_block_ = true;
    AudioThreadBinding audio_thread_binding(audio_thread_token_);
    diag_process_block_.fetch_add(1, std::memory_order_relaxed);
    recordProcessBlockContract(buffer, midi);
    struct ProcessFlag {
        bool& flag;
        ~ProcessFlag() { flag = false; }
    } clear_flag{in_process_block_};

    if constexpr (vst3DiagSilencesNon48Process()) {
        if (!hostIsEngineRate()) {
            buffer.clear();
            midi.clear();
            return;
        }
    }

    if (!sample_rate_ok_ || !program_ready_.load(std::memory_order_acquire)) {
        buffer.clear();
        midi.clear();
        return;
    }

    const int num_samples = buffer.getNumSamples();
    if (num_samples <= 0) {
        midi.clear();
        return;
    }

    if constexpr (vst3DiagEngineOnlyNon48()) {
        if (!hostIsEngineRate()) {
            applyCommittedPlaybackPlan();
            processEngineOnlyBlock(buffer, midi, num_samples);
            host_frame_position_ += static_cast<std::uint64_t>(num_samples);
            midi_it_ = {};
            midi_end_ = {};
            mapped_count_ = 0;
            mapped_index_ = 0;
            midi.clear();
            return;
        }
    }

    if constexpr (vst3DiagSrcZeroNon48()) {
        if (!hostIsEngineRate()) {
            processSrcZeroBlock(buffer, num_samples);
            host_frame_position_ += static_cast<std::uint64_t>(num_samples);
            midi.clear();
            return;
        }
    }

    applyCommittedPlaybackPlan();

    if (use_src_) {
        processResampledBlock(buffer, midi, num_samples);
    } else {
        processDirectBlock(buffer, midi, num_samples);
    }
    host_frame_position_ += static_cast<std::uint64_t>(num_samples);
    midi_it_ = {};
    midi_end_ = {};
    mapped_count_ = 0;
    mapped_index_ = 0;
    midi.clear();
}

void MgstcAudioProcessor::resetBlockDiagnostics() noexcept {
    src_callbacks_ = 0;
    src_fill_iters_ = 0;
    src_zero_progress_ = 0;
    src_known_end_rejects_ = 0;
    src_watchdog_trips_ = 0;
    render_audio_calls_ = 0;
    src_stall_engine_ = 0;
    src_stall_mapped_ = 0;
    src_stall_known_end_ = 0;
    src_process_ns_ = 0;
    src_fill_ns_ = 0;
    src_render_ns_ = 0;
    copy_ns_ = 0;
}

void MgstcAudioProcessor::processDirectBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi,
    int num_samples) noexcept {
    resetBlockDiagnostics();
    const auto block_start = engine_frame_position_;
    const auto block_end =
        block_start + static_cast<std::uint64_t>(num_samples);

    src_host_block_start_ = block_start;
    src_host_block_samples_ = num_samples;
    known_engine_end_ = block_end;
    collectHostMidi(midi, num_samples);
    renderDirectSpan(buffer, block_start);
}

void MgstcAudioProcessor::renderDirectSpan(
    juce::AudioBuffer<float>& buffer,
    std::uint64_t block_start) noexcept {
    const auto block_end = known_engine_end_;
    processEventsAt(block_start);

    while (engine_frame_position_ < block_end) {
        auto next = block_end;
        if (const auto due_midi = currentMidiEngineFrame();
            due_midi <= known_engine_end_) {
            next = std::min(next, due_midi);
        }
        if (const auto due =
                scheduler_.nextDueFrame(engine_frame_position_, next)) {
            next = std::min(next, *due);
        }

        const auto pos = engine_frame_position_;
        if (next > pos) {
            const int rel = static_cast<int>(pos - block_start);
            const int count = static_cast<int>(next - pos);
            renderTo(buffer, rel, count);
            if (engine_frame_position_ != next) {
                const int produced =
                    static_cast<int>(engine_frame_position_ - pos);
                silenceFrom(buffer, rel + produced);
                return;
            }
        }
        processEventsAt(engine_frame_position_);
    }
}

void MgstcAudioProcessor::processResampledBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi,
    int num_samples) noexcept {
    if (host_rate_hz_ == 0) {
        buffer.clear();
        return;
    }

    resetBlockDiagnostics();

    src_host_block_start_ = host_frame_position_;
    src_host_block_samples_ = num_samples;
    known_engine_end_ = hostFrameToEngineFrame(
        host_frame_position_ + static_cast<std::uint64_t>(num_samples),
        host_rate_hz_);
    collectHostMidi(midi, num_samples);

    processEventsAt(engine_frame_position_);

    auto* left = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1
        ? buffer.getWritePointer(1)
        : nullptr;
    {
        NsAccum clock(src_process_ns_);
        static_cast<void>(src_.process(
            left,
            right,
            static_cast<std::size_t>(num_samples),
            {
                .context = this,
                .render = nullptr,
                .render_frames = &MgstcAudioProcessor::renderSrcFramesCallback,
            }));
    }
}

std::size_t MgstcAudioProcessor::renderSrcFramesCallback(
    void* context,
    std::span<float> interleaved) noexcept {
    auto* self = static_cast<MgstcAudioProcessor*>(context);
    if (self == nullptr) {
        return 0;
    }
    ++self->src_callbacks_;
    return self->pullSrcSource(interleaved);
}

std::size_t MgstcAudioProcessor::renderSrcZeroCallback(
    void* context,
    std::span<float> interleaved) noexcept {
    auto* self = static_cast<MgstcAudioProcessor*>(context);
    if (self != nullptr) {
        ++self->src_callbacks_;
    }
    std::fill(interleaved.begin(), interleaved.end(), 0.0F);
    return interleaved.size() / 2;
}

void MgstcAudioProcessor::advanceEngineUntil(
    std::uint64_t end_frame) noexcept {
    processEventsAt(engine_frame_position_);
    int stalls = 0;
    while (engine_frame_position_ < end_frame) {
        auto next = end_frame;
        if (const auto due_midi = currentMidiEngineFrame();
            due_midi <= known_engine_end_) {
            next = std::min(next, due_midi);
        }
        if (const auto due =
                scheduler_.nextDueFrame(engine_frame_position_, next)) {
            next = std::min(next, *due);
        }
        const auto pos = engine_frame_position_;
        if (next > pos) {
            stalls = 0;
            int remaining = static_cast<int>(next - pos);
            while (remaining > 0) {
                const int chunk = std::min(remaining, kScratchFrames);
                std::span<float> interleaved(
                    scratch_.data(),
                    static_cast<std::size_t>(chunk) * 2);
                if (!renderEngineFramesToInterleaved(
                        interleaved, 0, chunk)) {
                    return;
                }
                remaining -= chunk;
            }
        } else {
            ++stalls;
            if (stalls >= 4) {
                ++src_watchdog_trips_;
                return;
            }
        }
        processEventsAt(engine_frame_position_);
    }
}

void MgstcAudioProcessor::processEngineOnlyBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& midi,
    int num_samples) noexcept {
    if (host_rate_hz_ == 0) {
        buffer.clear();
        return;
    }
    resetBlockDiagnostics();
    src_host_block_start_ = host_frame_position_;
    src_host_block_samples_ = num_samples;
    known_engine_end_ = hostFrameToEngineFrame(
        host_frame_position_ + static_cast<std::uint64_t>(num_samples),
        host_rate_hz_);
    collectHostMidi(midi, num_samples);
    advanceEngineUntil(known_engine_end_);
    buffer.clear();
}

void MgstcAudioProcessor::processSrcZeroBlock(
    juce::AudioBuffer<float>& buffer,
    int num_samples) noexcept {
    if (host_rate_hz_ == 0 || !use_src_) {
        buffer.clear();
        return;
    }
    resetBlockDiagnostics();
    auto* left = buffer.getNumChannels() > 0
        ? buffer.getWritePointer(0)
        : nullptr;
    auto* right = buffer.getNumChannels() > 1
        ? buffer.getWritePointer(1)
        : nullptr;
    static_cast<void>(src_.process(
        left,
        right,
        static_cast<std::size_t>(num_samples),
        {
            .context = this,
            .render = nullptr,
            .render_frames = &MgstcAudioProcessor::renderSrcZeroCallback,
        }));
}

void MgstcAudioProcessor::collectHostMidi(
    const juce::MidiBuffer& midi,
    int num_samples) noexcept {
    mapped_count_ = 0;
    mapped_index_ = 0;
    midi_it_ = midi.begin();
    midi_end_ = midi.end();
    while (midi_it_ != midi_end_ && mapped_count_ < mapped_events_.size()) {
        const auto metadata = *midi_it_;
        const auto host = src_host_block_start_
            + static_cast<std::uint64_t>(clampedMidiSample(
                metadata.samplePosition, num_samples));
        const auto engine = use_src_
            ? hostFrameToEngineFrame(host, host_rate_hz_)
            : host;
        const auto message = metadata.getMessage();
        MappedMidiEvent event;
        event.engine_frame = engine;
        bool stored = true;
        if (message.isNoteOn(false)) {
            if (message.getVelocity() == 0) {
                event.type = MappedMidiType::NoteOff;
            } else {
                event.type = MappedMidiType::NoteOn;
            }
            event.midi_note = static_cast<std::uint8_t>(
                message.getNoteNumber());
        } else if (message.isNoteOff()) {
            event.type = MappedMidiType::NoteOff;
            event.midi_note = static_cast<std::uint8_t>(
                message.getNoteNumber());
        } else if (message.isAllSoundOff()) {
            event.type = MappedMidiType::AllSoundOff;
        } else if (message.isAllNotesOff()) {
            event.type = MappedMidiType::AllNotesOff;
        } else {
            stored = false;
        }
        if (stored) {
            mapped_events_[mapped_count_++] = event;
        }
        ++midi_it_;
    }
}

void MgstcAudioProcessor::applyMappedEvent(
    const MappedMidiEvent& event) noexcept {
    switch (event.type) {
    case MappedMidiType::NoteOn:
        noteOn(event.midi_note);
        break;
    case MappedMidiType::NoteOff:
        noteOff(event.midi_note);
        break;
    case MappedMidiType::AllSoundOff:
        allSoundOff();
        break;
    case MappedMidiType::AllNotesOff:
        allNotesOff();
        break;
    }
}

bool MgstcAudioProcessor::pluginQuiescent() const noexcept {
    if (scheduler_.pendingCount() != 0 || engine_.hasRealtimeWork()) {
        return false;
    }
    for (const auto state : track_state_) {
        if (state != TrackLayerState::Inactive) {
            return false;
        }
    }
    if (mapped_index_ < mapped_count_) {
        return false;
    }
    return true;
}

std::uint64_t MgstcAudioProcessor::currentMidiEngineFrame() const noexcept {
    if (mapped_index_ < mapped_count_) {
        return mapped_events_[mapped_index_].engine_frame;
    }
    if (midi_it_ == midi_end_) {
        return known_engine_end_ + 1;
    }
    const auto host = src_host_block_start_
        + static_cast<std::uint64_t>(clampedMidiSample(
            (*midi_it_).samplePosition, src_host_block_samples_));
    if (!use_src_) {
        return host;
    }
    return hostFrameToEngineFrame(host, host_rate_hz_);
}

void MgstcAudioProcessor::processEventsAt(std::uint64_t frame) noexcept {
    // Queue order is chronological. A head at or before `frame` is applied
    // now; the engine clock is not rewound. Each consumed event advances
    // mapped_index_ or the MIDI cursor, so a past head cannot stall the loop.
    while (mapped_index_ < mapped_count_
        && mapped_events_[mapped_index_].engine_frame <= frame) {
        applyMappedEvent(mapped_events_[mapped_index_]);
        ++mapped_index_;
    }
    if (mapped_index_ >= mapped_count_) {
        while (midi_it_ != midi_end_ && currentMidiEngineFrame() <= frame) {
            applyMidiMessage((*midi_it_).getMessage());
            ++midi_it_;
        }
    }
    fireDueLayers(frame);
}

std::size_t MgstcAudioProcessor::pullSrcSource(
    std::span<float> interleaved) noexcept {
    NsAccum clock(src_fill_ns_);
    const auto want = static_cast<int>(interleaved.size() / 2);
    if (want <= 0) {
        return 0;
    }
    if (engine_frame_position_ >= known_engine_end_) {
        ++src_known_end_rejects_;
        return 0;
    }
    const auto remaining_known = static_cast<int>(
        known_engine_end_ - engine_frame_position_);
    const int cap = std::min(want, remaining_known);
    if (cap <= 0) {
        ++src_known_end_rejects_;
        return 0;
    }

    processEventsAt(engine_frame_position_);

    int dest = 0;
    int stalls = 0;
    while (dest < cap) {
        ++src_fill_iters_;
        auto next = engine_frame_position_
            + static_cast<std::uint64_t>(cap - dest);
        if (const auto due_midi = currentMidiEngineFrame();
            due_midi <= known_engine_end_) {
            next = std::min(next, due_midi);
        }
        if (const auto due =
                scheduler_.nextDueFrame(engine_frame_position_, next)) {
            next = std::min(next, *due);
        }

        const auto pos = engine_frame_position_;
        if (next > pos) {
            stalls = 0;
            const int count = static_cast<int>(next - pos);
            if (!renderEngineFramesToInterleaved(
                    interleaved, dest, count)) {
                return static_cast<std::size_t>(std::max(dest, 0));
            }
            dest += static_cast<int>(engine_frame_position_ - pos);
            if (engine_frame_position_ != next) {
                return static_cast<std::size_t>(std::max(dest, 0));
            }
        } else {
            ++src_zero_progress_;
            src_stall_engine_ = pos;
            src_stall_mapped_ = currentMidiEngineFrame();
            src_stall_known_end_ = known_engine_end_;
            ++stalls;
            if (stalls >= 4) {
                ++src_watchdog_trips_;
                return static_cast<std::size_t>(std::max(dest, 0));
            }
        }
        processEventsAt(engine_frame_position_);
    }
    return static_cast<std::size_t>(dest);
}

bool MgstcAudioProcessor::renderEngineFramesToInterleaved(
    std::span<float> interleaved,
    int start_frame,
    int frame_count) noexcept {
    if (frame_count <= 0) {
        return true;
    }
    int remaining = frame_count;
    int dest = start_frame;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kScratchFrames);
        const auto out = static_cast<std::size_t>(dest) * 2;
        const auto floats = static_cast<std::size_t>(chunk) * 2;
        if (out + floats > interleaved.size()) {
            return false;
        }
        mgstc::engine::RenderResult result;
        {
            NsAccum clock(src_render_ns_);
            ++render_audio_calls_;
            result = engine_.renderAudio(interleaved.subspan(out, floats));
        }
        const int produced = static_cast<int>(
            std::min(result.frames, static_cast<std::size_t>(chunk)));
        if (produced > 0) {
            engine_frame_position_ += static_cast<std::uint64_t>(produced);
        }
        if (!result.ok() || produced < chunk) {
            return false;
        }
        dest += produced;
        remaining -= produced;
    }
    return true;
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

void MgstcAudioProcessor::stopTrack(std::uint8_t track, bool hard) noexcept {
    if (!validPhysicalTrack(track)) {
        return;
    }
    switch (track_state_[track]) {
    case TrackLayerState::Inactive:
        break;
    case TrackLayerState::Pending:
        scheduler_.cancelTrack(track);
        track_state_[track] = TrackLayerState::Inactive;
        break;
    case TrackLayerState::Active:
        if (hard) {
            static_cast<void>(engine_.realtimeSilenceTrack(track));
        } else {
            static_cast<void>(engine_.realtimeNoteOff(track));
        }
        track_state_[track] = TrackLayerState::Inactive;
        break;
    }
}

void MgstcAudioProcessor::stopVoice(std::uint8_t voice, bool hard) noexcept {
    for (const auto& binding : activePlan().audible_layers) {
        stopTrack(activePlan().physicalTrack(binding, voice), hard);
    }
    scheduler_.cancelVoice(voice);
}

void MgstcAudioProcessor::startVoice(
    std::uint8_t voice,
    std::uint8_t midi_note) noexcept {
    for (const auto& binding : activePlan().audible_layers) {
        const auto note = mgstc::engine::transposedMidiNote(
            midi_note, binding.relative_semitones);
        if (!note) {
            continue;
        }
        const auto track = activePlan().physicalTrack(binding, voice);
        if (!validPhysicalTrack(track)) {
            continue;
        }
        const auto delay_frames =
            delayMillisecondsToEngineFrames(binding.start_delay_ms);
        if (delay_frames == 0) {
            static_cast<void>(engine_.realtimeNoteOn(track, *note));
            track_state_[track] = TrackLayerState::Active;
            recordNoteOn(track, *note);
            continue;
        }
        auto due = engine_frame_position_;
        constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
        if (delay_frames > kMax - due) {
            due = kMax;
        } else {
            due += delay_frames;
        }
        scheduler_.schedule(track, due, voice, *note);
        track_state_[track] = TrackLayerState::Pending;
    }
}

void MgstcAudioProcessor::fireDueLayers(std::uint64_t frame) noexcept {
    std::array<PendingLayerEvent, LayerDelayScheduler::kPhysicalTrackCount>
        due{};
    const auto count = scheduler_.takeDueAt(frame, due);
    const auto n = std::min(count, due.size());
    for (std::size_t index = 0; index < n; ++index) {
        const auto& event = due[index];
        if (!validPhysicalTrack(event.physical_track)) {
            continue;
        }
        if (track_state_[event.physical_track] != TrackLayerState::Pending) {
            continue;
        }
        static_cast<void>(
            engine_.realtimeNoteOn(event.physical_track, event.midi_note));
        track_state_[event.physical_track] = TrackLayerState::Active;
        recordNoteOn(event.physical_track, event.midi_note);
    }
}

void MgstcAudioProcessor::recordNoteOn(
    std::uint8_t physical_track,
    std::uint8_t midi_note) noexcept {
    if (note_on_log_size_ >= note_on_log_.size()) {
        return;
    }
    note_on_log_[note_on_log_size_++] = {
        .frame = engine_frame_position_,
        .physical_track = physical_track,
        .midi_note = midi_note,
    };
}

void MgstcAudioProcessor::allNotesOff() noexcept {
    std::array<std::uint8_t, kMaxVoices> voices{};
    const auto count = std::min(voices_.allNotesOff(voices), voices.size());
    for (std::size_t index = 0; index < count; ++index) {
        stopVoice(voices[index], false);
    }
    scheduler_.reset();
    for (auto& state : track_state_) {
        if (state == TrackLayerState::Pending) {
            state = TrackLayerState::Inactive;
        }
    }
}

void MgstcAudioProcessor::allSoundOff() noexcept {
    resetPlaybackState(true);
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
        mgstc::engine::RenderResult result;
        {
            NsAccum clock(src_render_ns_);
            ++render_audio_calls_;
            result = engine_.renderAudio(
                std::span<float>(
                    scratch_.data(),
                    static_cast<std::size_t>(chunk) * 2));
        }
        const int produced = static_cast<int>(
            std::min(result.frames, static_cast<std::size_t>(chunk)));
        {
            NsAccum copy(copy_ns_);
            for (int frame = 0; frame < produced; ++frame) {
                left[dest + frame] =
                    scratch_[static_cast<std::size_t>(frame) * 2];
                if (right != nullptr) {
                    right[dest + frame] =
                        scratch_[static_cast<std::size_t>(frame) * 2 + 1];
                }
            }
        }
        if (produced > 0) {
            engine_frame_position_ += static_cast<std::uint64_t>(produced);
        }
        if (!result.ok() || produced < chunk) {
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

bool MgstcAudioProcessor::hasEditor() const {
    return true;
}

const juce::String MgstcAudioProcessor::getName() const {
    if constexpr (kVst3DiagMode == kVst3DiagOff) {
        return "MGS Tone Craft";
    } else {
        return juce::String("MGS Tone Craft [")
            + vst3DiagnosticModeName()
            + "]";
    }
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

void MgstcAudioProcessor::setCurrentProgram(int /*index*/) {
    resetProgramState();
}

const juce::String MgstcAudioProcessor::getProgramName(int /*index*/) {
    return "Default";
}

void MgstcAudioProcessor::changeProgramName(
    int /*index*/,
    const juce::String& /*newName*/) {}

void MgstcAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (onAudioThread()) {
        return;
    }
    const auto bytes = serializePluginState(copyPluginState());
    if (bytes.empty()) {
        destData.reset();
        return;
    }
    destData.replaceAll(bytes.data(), bytes.size());
}

void MgstcAudioProcessor::setStateInformation(
    const void* data,
    int sizeInBytes) {
    if (onAudioThread()) {
        state_audio_reject_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto parsed = parsePluginState(
        data,
        sizeInBytes > 0 ? static_cast<std::size_t>(sizeInBytes) : 0);
    if (parsed.status != PluginStateStatus::Ok) {
        return;
    }
    const auto mode = host_prepared_.load(std::memory_order_acquire)
        ? CommitMode::Live
        : CommitMode::Offline;
    static_cast<void>(commitPluginState(parsed.document, mode));
}

PluginStateDocument MgstcAudioProcessor::copyPluginState() const {
    if (onAudioThread()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_mu_);
    PluginStateDocument document;
    document.editor = editor_;
    document.library_id = library_id_;
    document.sound = sound_;
    return document;
}

bool MgstcAudioProcessor::replacePluginState(PluginStateDocument document) {
    if (onAudioThread()) {
        state_audio_reject_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto mode = host_prepared_.load(std::memory_order_acquire)
        ? CommitMode::Live
        : CommitMode::Offline;
    return commitPluginState(std::move(document), mode);
}

bool MgstcAudioProcessor::onAudioThread() const noexcept {
    const auto key = audio_thread_token_.load(std::memory_order_relaxed);
    return key != 0 && key == currentThreadToken();
}

void MgstcAudioProcessor::poseAsAudioThreadForTest() noexcept {
    audio_thread_token_.store(currentThreadToken(), std::memory_order_relaxed);
}

void MgstcAudioProcessor::clearAudioThreadForTest() noexcept {
    audio_thread_token_.store(0, std::memory_order_relaxed);
}

std::uint8_t MgstcAudioProcessor::pickPlanSlot() const noexcept {
    const auto published = published_plan_.load(std::memory_order_acquire);
    const auto acknowledged =
        acknowledged_plan_.load(std::memory_order_acquire);
    for (std::uint8_t index = 0; index < plan_slots_.size(); ++index) {
        if (index != published && index != acknowledged) {
            return index;
        }
    }
    return static_cast<std::uint8_t>(
        (published + 1U) % plan_slots_.size());
}

const mgstc::engine::CompositePlaybackPlan&
MgstcAudioProcessor::activePlan() const noexcept {
    return plan_slots_[applied_plan_];
}

void MgstcAudioProcessor::applyCommittedPlaybackPlan() noexcept {
    const auto index = published_plan_.load(std::memory_order_acquire);
    acknowledged_plan_.store(index, std::memory_order_release);
    engine_.servicePendingControlCommands();
    if (index == applied_plan_) {
        return;
    }
    resetPlaybackState(true);
    applied_plan_ = index;
    const auto capacity = plan_slots_[index].voice_capacity;
    voices_.setActiveChannelCount(capacity == 0 ? 1 : capacity);
}

bool MgstcAudioProcessor::commitPluginState(
    PluginStateDocument document,
    CommitMode mode,
    bool polyphonic,
    std::uint8_t* voice_capacity) {
    if (onAudioThread()) {
        state_audio_reject_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    static_cast<void>(mgstc::engine::enforceOpllRegisterAutoExclusivity(
        document.sound));
    document.sound.playback_tempo = std::clamp(
        document.sound.playback_tempo,
        mgstc::engine::kMgscTempoMin,
        mgstc::engine::kMgscTempoMax);
    if (!validatePluginSoundSnapshot(document.sound)) {
        return false;
    }

    auto edit = engine_.beginProgramEdit();
    if (!edit.valid()) {
        engine_.discardStuckProgramEdits();
        edit = engine_.beginProgramEdit();
    }
    if (!edit.valid()) {
        return false;
    }

    mgstc::engine::CompositePlaybackPlan plan;
    state_compile_count_.fetch_add(1, std::memory_order_relaxed);
    const bool compiled = mgstc::engine::compileCompositeProgram(
        *edit.engine,
        document.sound,
        {.polyphonic = polyphonic},
        &plan);
    if (voice_capacity != nullptr) {
        *voice_capacity = plan.voice_capacity;
    }
    if (!compiled) {
        static_cast<void>(engine_.discardProgramEdit(edit));
        return false;
    }

    const auto slot = pickPlanSlot();
    plan_slots_[slot] = plan;

    PluginStateDocument previous = copyPluginState();
    {
        std::lock_guard<std::mutex> lock(state_mu_);
        sound_ = document.sound;
        editor_ = document.editor;
        library_id_ = document.library_id;
    }
    if (!engine_.submitProgram(edit)) {
        static_cast<void>(engine_.discardProgramEdit(edit));
        std::lock_guard<std::mutex> lock(state_mu_);
        sound_ = std::move(previous.sound);
        editor_ = previous.editor;
        library_id_ = previous.library_id;
        return false;
    }

    if (mode == CommitMode::Offline) {
        std::array<float, 64> drain{};
        engine_.drainPendingCommands(drain, 32, true);
    }

    published_plan_.store(slot, std::memory_order_release);
    program_ready_.store(true, std::memory_order_release);
    editor_runtime_program_temporary_ = false;
    if (mode != CommitMode::Live) {
        applied_plan_ = slot;
        acknowledged_plan_.store(slot, std::memory_order_release);
        const auto capacity = plan_slots_[slot].voice_capacity;
        voices_.setActiveChannelCount(capacity == 0 ? 1 : capacity);
        voices_.setPolyphonic(true);
    }
    return true;
}

bool MgstcAudioProcessor::replaceEditorComposite(
    mgstc::engine::CompositeTimbre sound,
    bool polyphonic,
    std::uint8_t& voice_capacity) {
    voice_capacity = 1;
    if (onAudioThread()) {
        state_audio_reject_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto document = copyPluginState();
    document.sound = std::move(sound);
    const auto mode = host_prepared_.load(std::memory_order_acquire)
        ? CommitMode::Live
        : CommitMode::Offline;
    return commitPluginState(
        std::move(document), mode, polyphonic, &voice_capacity);
}

bool MgstcAudioProcessor::editorSubmitEngine(
    const mgstc::engine::EngineCommand& command) {
    if (onAudioThread()) {
        return false;
    }
    if (!engine_.submit(command)) {
        return false;
    }
    if (!host_prepared_.load(std::memory_order_acquire)) {
        std::array<float, 64> drain{};
        engine_.drainPendingCommands(drain, 32, true);
    }
    return true;
}

bool MgstcAudioProcessor::editorNoteOn(
    std::uint8_t track,
    std::uint8_t note) {
    return editorSubmitEngine(
        mgstc::engine::EngineCommand::noteOn(track, note));
}

bool MgstcAudioProcessor::editorNoteOff(std::uint8_t track) {
    return editorSubmitEngine(
        mgstc::engine::EngineCommand::noteOff(track));
}

void MgstcAudioProcessor::editorSilenceTrack(std::uint8_t track) {
    static_cast<void>(editorSubmitEngine(
        mgstc::engine::EngineCommand::silenceTrack(track)));
}

void MgstcAudioProcessor::editorFlushPending() {
    if (onAudioThread()
        || host_prepared_.load(std::memory_order_acquire)) {
        return;
    }
    std::array<float, 256> drain{};
    engine_.drainPendingCommands(drain, 250, true);
}

void MgstcAudioProcessor::editorSetMasterVolumePercent(int percent) {
    if (onAudioThread()) {
        return;
    }
    editor_master_volume_percent_ = std::clamp(percent, 0, 100);
    ++editor_master_volume_revision_;
}

int MgstcAudioProcessor::editorMasterVolumePercent() const noexcept {
    return editor_master_volume_percent_;
}

std::uint64_t MgstcAudioProcessor::editorMasterVolumeRevision()
    const noexcept {
    return editor_master_volume_revision_;
}

void MgstcAudioProcessor::editorPublishSharedScc(
    const mgstc::engine::SccWaveform& waveform) {
    if (onAudioThread()) {
        return;
    }
    editor_shared_scc_ = waveform;
    ++editor_shared_scc_revision_;
}

void MgstcAudioProcessor::editorPublishSharedOpll(
    const mgstc::engine::OpllPatchParameters& patch) {
    if (onAudioThread()) {
        return;
    }
    editor_shared_opll_ = patch;
    ++editor_shared_opll_revision_;
}

bool MgstcAudioProcessor::editorAuditionShared(
    const mgstc::engine::SccWaveform& waveform,
    const mgstc::engine::OpllPatchParameters& patch,
    bool retrigger,
    std::uint8_t track,
    std::uint8_t note,
    bool commit_scc,
    bool commit_opll) {
    if (onAudioThread()) {
        return false;
    }
    if (commit_scc) {
        editorPublishSharedScc(waveform);
    }
    if (commit_opll) {
        editorPublishSharedOpll(patch);
    }
    auto edit = engine_.beginProgramEdit();
    if (!edit.valid()) {
        engine_.discardStuckProgramEdits();
        edit = engine_.beginProgramEdit();
    }
    if (!edit.valid()) {
        return false;
    }
    constexpr std::uint8_t kPsgTrack = mgstc::engine::kCompositePsgTrackBase;
    constexpr std::uint8_t kSccTrack = mgstc::engine::kCompositeSccTrackBase;
    constexpr std::uint8_t kOpllTrack = mgstc::engine::kCompositeOpllTrackBase;
    std::array<std::uint8_t, 32> raw_wave{};
    std::transform(
        waveform.begin(),
        waveform.end(),
        raw_wave.begin(),
        [](std::int8_t sample) {
            return static_cast<std::uint8_t>(sample);
        });
    const auto opll_registers = mgstc::engine::encodeOpllPatch(patch);
    bool configured =
        edit.engine->session().setSequenceEnvelope(
            kPsgTrack, {0x40, 0xEF, 0x01, 0x60})
        && edit.engine->session().setPsgToneNoise(kPsgTrack, 1, 0)
        && edit.engine->session().setPsgFixedVolume(kPsgTrack, 15)
        && edit.engine->session().mapper().defineSccPatch(0, raw_wave)
            == mgstc::engine::MapError::None
        && edit.engine->session().mapper().defineOpllOriginalPatch(
               16, opll_registers)
            == mgstc::engine::MapError::None;
    for (std::uint8_t scc = kSccTrack; scc < kSccTrack + 5; ++scc) {
        configured = configured
            && edit.engine->session().setSequenceEnvelope(
                scc, {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60})
            && edit.engine->session().setTrackVolume(scc, 15);
    }
    for (std::uint8_t opll = kOpllTrack; opll < kOpllTrack + 9; ++opll) {
        configured = configured
            && edit.engine->session().setSequenceEnvelope(
                opll, {0x10, 0x10, 0x40, 0xEF, 0x01, 0x60});
    }
    if (!configured) {
        static_cast<void>(engine_.discardProgramEdit(edit));
        return false;
    }
    const auto submitted = engine_.submitProgram(
        edit,
        {
            .retrigger = retrigger,
            .track = track,
            .midi_note = note,
        });
    if (!submitted && edit.valid()) {
        static_cast<void>(engine_.discardProgramEdit(edit));
        return false;
    }
    if (!host_prepared_.load(std::memory_order_acquire)) {
        std::array<float, 64> drain{};
        engine_.drainPendingCommands(drain, 32, true);
    }
    editor_shared_program_active_ = true;
    editor_runtime_program_temporary_ = true;
    return true;
}

bool MgstcAudioProcessor::editorRestoreCommittedProgram() {
    if (!editor_runtime_program_temporary_ || onAudioThread()) {
        return !editor_runtime_program_temporary_;
    }
    auto document = copyPluginState();
    const auto mode = host_prepared_.load(std::memory_order_acquire)
        ? CommitMode::Live
        : CommitMode::Offline;
    return commitPluginState(std::move(document), mode);
}

const mgstc::engine::SccWaveform&
MgstcAudioProcessor::editorSharedScc() const noexcept {
    return editor_shared_scc_;
}

std::uint64_t MgstcAudioProcessor::editorSharedSccRevision() const noexcept {
    return editor_shared_scc_revision_;
}

const mgstc::engine::OpllPatchParameters&
MgstcAudioProcessor::editorSharedOpll() const noexcept {
    return editor_shared_opll_;
}

std::uint64_t MgstcAudioProcessor::editorSharedOpllRevision() const noexcept {
    return editor_shared_opll_revision_;
}

bool MgstcAudioProcessor::editorSharedProgramActive() const noexcept {
    return editor_shared_program_active_;
}

bool MgstcAudioProcessor::editorProgramReady() const noexcept {
    return program_ready_.load(std::memory_order_acquire);
}

void MgstcAudioProcessor::editorNoteBlockedBackendCall() noexcept {
    ++editor_blocked_backend_calls_;
}

std::uint64_t MgstcAudioProcessor::editorBlockedBackendCalls() const noexcept {
    return editor_blocked_backend_calls_;
}

}  // namespace mgstc::plugin

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new mgstc::plugin::MgstcAudioProcessor();
}
