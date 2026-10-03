// Focused host-side tests for the VST3 WAV comparison player.
// Included by vst3_processor_tests.cpp after its shared fixtures/helpers.

#include <memory>
#include <thread>

namespace {

std::shared_ptr<const mgstc::engine::SourcePcm> makeStereoPcmPreviewFixture() {
    auto pcm = std::make_shared<mgstc::engine::SourcePcm>();
    pcm->sample_rate = 48'000;
    pcm->channels = 2;
    pcm->bit_depth = 32;
    pcm->interleaved_samples = {
        0.0F, 0.0F,
        0.2F, -0.2F,
        0.4F, -0.4F,
        0.6F, -0.6F,
        0.8F, -0.8F,
        1.0F, -1.0F,
        -0.8F, 0.8F,
        -0.6F, 0.6F,
        -0.4F, 0.4F,
        -0.2F, 0.2F,
        0.0F, 0.0F,
        0.3F, -0.3F,
    };
    // A phase-cancelled mono analysis view makes it observable that host
    // preview uses the original stereo PCM, not the converter's mono view.
    pcm->mono_samples.assign(pcm->interleaved_samples.size() / 2, 0.0F);
    return pcm;
}

juce::AudioBuffer<float> processPcmPreviewBlock(
    MgstcAudioProcessor& processor,
    int frames) {
    juce::AudioBuffer<float> buffer(2, frames);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    return buffer;
}

void requirePreviewSample(
    const juce::AudioBuffer<float>& buffer,
    int channel,
    int frame,
    float expected,
    const char* message,
    float tolerance = 1.0e-6F) {
    require(std::fabs(buffer.getSample(channel, frame) - expected) <= tolerance,
        message);
}

void testVstPcmPreviewStereoRangeGainAndHostRate() {
    using mgstc::plugin::PluginEditorContext;
    const auto pcm = makeStereoPcmPreviewFixture();
    const auto original_pcm = pcm->interleaved_samples;
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 16);
    PluginEditorContext context(processor);
    const auto before_state = stateBytes(processor);
    const auto latency = processor.getLatencySamples();
    const auto latency_calls = Access::latencyCalls(processor);

    require(context.pcmPreview()->play(pcm, 1, 4, 0.5F),
        "VST PCM comparison accepts a valid original-frame range");
    const auto block = processPcmPreviewBlock(processor, 8);
    requirePreviewSample(block, 0, 0, 0.1F,
        "48 kHz preview begins at the inclusive source frame");
    requirePreviewSample(block, 1, 0, -0.1F,
        "48 kHz preview retains the original right channel");
    requirePreviewSample(block, 0, 1, 0.2F,
        "preview advances by one source frame at 48 kHz");
    requirePreviewSample(block, 0, 2, 0.3F,
        "preview includes the frame before the exclusive endpoint");
    for (int frame = 3; frame < block.getNumSamples(); ++frame) {
        requirePreviewSample(block, 0, frame, 0.0F,
            "preview range ends exactly at its exclusive source frame");
        requirePreviewSample(block, 1, frame, 0.0F,
            "preview does not loop after reaching the range end");
    }
    require(stateBytes(processor).getSize() == before_state.getSize()
            && sameBlock(stateBytes(processor), before_state),
        "playing a PCM preview leaves persistent plugin state unchanged");
    require(processor.getLatencySamples() == latency
            && Access::latencyCalls(processor) == latency_calls,
        "PCM preview does not change or re-report host latency");
    require(pcm->interleaved_samples == original_pcm,
        "PCM playback never modifies the immutable source samples");

    const auto ended = processPcmPreviewBlock(processor, 4);
    require(channelPeak(ended, 0, ended.getNumSamples()) == 0.0,
        "following host block falls back to the silent normal program");

    MgstcAudioProcessor resampled;
    resampled.prepareToPlay(44'100.0, 16);
    PluginEditorContext resampled_context(resampled);
    require(resampled_context.pcmPreview()->play(pcm, 1, 4, 1.0F),
        "44.1 kHz host accepts preview at the source sample rate");
    const auto at_44100 = processPcmPreviewBlock(resampled, 8);
    constexpr double step = 48'000.0 / 44'100.0;
    for (int frame = 0; frame < 3; ++frame) {
        const double source_position = 1.0 + step * frame;
        const auto left_index = static_cast<std::size_t>(source_position);
        const auto right_index = std::min(left_index + 1, std::size_t{3});
        const float fraction = static_cast<float>(source_position - left_index);
        const float left_sample = 0.2F * static_cast<float>(left_index);
        const float right_sample = 0.2F * static_cast<float>(right_index);
        const float expected_left = left_sample
            + fraction * (right_sample - left_sample);
        const float expected_right = -expected_left;
        requirePreviewSample(at_44100, 0, frame, expected_left,
            "44.1 kHz output linearly resamples the selected stereo range",
            2.0e-4F);
        requirePreviewSample(at_44100, 1, frame, expected_right,
            "44.1 kHz resampling keeps left/right separation", 2.0e-4F);
    }
    for (int frame = 3; frame < at_44100.getNumSamples(); ++frame) {
        requirePreviewSample(at_44100, 0, frame, 0.0F,
            "resampled preview is silent after the selected range");
    }
    requirePreviewSample(at_44100, 0, 2, 0.6F,
        "resampling clamps to the last selected frame without reading past the exclusive endpoint",
        2.0e-4F);
    require(resampled.getLatencySamples() > 0,
        "44.1 kHz host retains its existing prepared SRC latency");
}

void testVstPcmPreviewMuteClampAndInvalidRequests() {
    using mgstc::plugin::PluginEditorContext;
    const auto pcm = makeStereoPcmPreviewFixture();
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 8);
    PluginEditorContext context(processor);

    require(context.pcmPreview()->play(pcm, 1, 4, 0.0F),
        "zero comparison gain is a valid mute");
    const auto muted = processPcmPreviewBlock(processor, 4);
    require(channelPeak(muted, 0, muted.getNumSamples()) == 0.0,
        "zero preview gain produces digital silence");

    require(context.pcmPreview()->play(pcm, 4, 6, 4.0F),
        "finite positive preview gain is accepted");
    const auto clipped = processPcmPreviewBlock(processor, 4);
    requirePreviewSample(clipped, 0, 0, 1.0F,
        "preview clips positive gain to the output range");
    requirePreviewSample(clipped, 1, 0, -1.0F,
        "preview clips negative gain to the output range");
    requirePreviewSample(clipped, 0, 1, 1.0F,
        "preview clips the second source frame independently");

    const auto before_invalid = stateBytes(processor);
    require(!context.pcmPreview()->play(pcm, 4, 4, 1.0F),
        "empty preview ranges are rejected");
    require(!context.pcmPreview()->play(pcm, 0, pcm->mono_samples.size() + 1, 1.0F),
        "ranges beyond decoded source frames are rejected");
    require(!context.pcmPreview()->play(pcm, 0, 1,
            std::numeric_limits<float>::quiet_NaN()),
        "non-finite preview gain is rejected");
    require(!context.pcmPreview()->play(pcm, 0, 1, -0.1F),
        "negative preview gain is rejected");
    require(sameBlock(before_invalid, stateBytes(processor)),
        "invalid preview requests do not alter plugin state");
}

void testVstPcmPreviewReplaceStopAndLifecycle() {
    using mgstc::plugin::PluginEditorContext;
    const auto pcm = makeStereoPcmPreviewFixture();
    MgstcAudioProcessor processor;
    processor.prepareToPlay(48'000.0, 8);
    PluginEditorContext context(processor);

    require(context.pcmPreview()->play(pcm, 1, 10, 1.0F),
        "first preview request publishes");
    const auto prefix = processPcmPreviewBlock(processor, 2);
    requirePreviewSample(prefix, 0, 0, 0.2F, "first preview starts at its requested frame");
    requirePreviewSample(prefix, 0, 1, 0.4F, "first preview position advances");

    require(context.pcmPreview()->play(pcm, 4, 6, 0.5F),
        "a later preview replaces active playback");
    const auto replacement = processPcmPreviewBlock(processor, 4);
    requirePreviewSample(replacement, 0, 0, 0.4F,
        "replacement begins at its own selected start");
    requirePreviewSample(replacement, 0, 1, 0.5F,
        "replacement applies its own gain");
    requirePreviewSample(replacement, 0, 2, 0.0F,
        "replacement ends at its exclusive endpoint");

    for (std::size_t start = 1; start <= 6; ++start) {
        require(context.pcmPreview()->play(pcm, start, start + 2, 0.5F),
            "rapid comparison requests reuse the bounded preview slots");
    }
    const auto latest = processPcmPreviewBlock(processor, 4);
    requirePreviewSample(latest, 0, 0, -0.4F,
        "latest queued comparison wins after repeated replacements");
    requirePreviewSample(latest, 0, 1, -0.3F,
        "latest replacement retains its own frame position and gain");
    requirePreviewSample(latest, 0, 2, 0.0F,
        "latest queued comparison still respects its exclusive endpoint");

    require(context.pcmPreview()->play(pcm, 1, 10, 1.0F),
        "preview can be queued again after completion");
    context.pcmPreview()->stop();
    const auto stopped = processPcmPreviewBlock(processor, 4);
    require(channelPeak(stopped, 0, stopped.getNumSamples()) == 0.0,
        "stop invalidates queued PCM before the next audio block");

    require(context.pcmPreview()->play(pcm, 1, 10, 1.0F),
        "preview can be queued before host prepare reset");
    processor.prepareToPlay(48'000.0, 8);
    const auto after_prepare = processPcmPreviewBlock(processor, 4);
    require(channelPeak(after_prepare, 0, after_prepare.getNumSamples()) == 0.0,
        "prepareToPlay invalidates stale comparison audio");

    require(context.pcmPreview()->play(pcm, 1, 10, 1.0F),
        "preview can be queued before host release");
    processor.releaseResources();
    processor.prepareToPlay(48'000.0, 8);
    const auto after_release = processPcmPreviewBlock(processor, 4);
    require(channelPeak(after_release, 0, after_release.getNumSamples()) == 0.0,
        "releaseResources invalidates comparison audio across reactivation");

    const auto saved_state = stateBytes(processor);
    require(context.pcmPreview()->play(pcm, 1, 10, 1.0F),
        "preview can be queued before restoring program state");
    processor.setStateInformation(saved_state.getData(),
        static_cast<int>(saved_state.getSize()));
    const auto after_restore = processPcmPreviewBlock(processor, 4);
    require(channelPeak(after_restore, 0, after_restore.getNumSamples()) == 0.0,
        "successful state restore cancels pending comparison audio");
    require(sameBlock(saved_state, stateBytes(processor)),
        "state restore preserves exactly the serialized program state");
}

mgstc::plugin::PluginStateDocument makeVstSccMorphDocument(bool enabled) {
    using namespace mgstc::engine;
    auto document = documentFrom(sccOnlyTimbre(), 0, {});
    auto& sound = document.sound;
    auto& layer = sound.layers.front();
    layer.volume = 15;
    layer.start_delay_form = StartDelayForm::AbsoluteTicks;
    layer.start_delay_value = 0;
    layer.software_lfo = {};
    layer.volume_envelope.events.clear();
    layer.pitch_envelope.events.clear();
    layer.timbre_automation.clear();
    layer.scc_output_allocation = SccOutputAllocationMode::Contiguous;
    layer.scc_output_start = 10;

    auto arrival = *layer.base_timbre;
    arrival.library_id = allocateCompositeOwnedTimbreId(sound);
    arrival.name = "VST morph arrival";
    for (std::size_t index = 0; index < arrival.scc_waveform.size(); ++index) {
        arrival.scc_waveform[index] = index < 16 ? 230 : 20;
    }
    sound.embedded_timbres = {arrival};
    layer.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre,
         .count = 0,
         .target_library_id = layer.base_timbre->library_id},
        {.kind = EnvelopeEventKind::Timbre,
         .count = 12,
         .target_library_id = arrival.library_id,
         .scc_morph = {.enabled = enabled,
                       .intermediate_count = 3,
                       .curve = 0.5}},
    };
    return document;
}

std::vector<float> renderVstSccMorphHostPcm(
    const mgstc::plugin::PluginStateDocument& document) {
    MgstcAudioProcessor processor;
    require(processor.replacePluginState(document),
        "VST accepts and compiles owned SCC morph sources");
    processor.prepareToPlay(48'000.0, 512);
    std::vector<float> rendered;
    rendered.reserve(14'336);
    for (int block_index = 0; block_index < 28; ++block_index) {
        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        if (block_index == 0) {
            midi.addEvent(juce::MidiMessage::noteOn(
                1, 60, static_cast<juce::uint8>(100)), 0);
        }
        processor.processBlock(buffer, midi);
        const auto* left = buffer.getReadPointer(0);
        rendered.insert(rendered.end(), left, left + buffer.getNumSamples());
    }
    return rendered;
}

struct VstConverterWorkerProbe {
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> allow_finish{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> joined{false};
    std::atomic<bool> processor_destroying{false};
    std::atomic<std::uint64_t> worker_steps{0};
    std::atomic<std::uint32_t> cancel_calls{0};
    std::atomic<std::uint32_t> completion_callbacks{0};
};

class SyntheticVstConverterWorker final
    : public mgstc::app::EditorBackgroundTask {
public:
    explicit SyntheticVstConverterWorker(
        std::shared_ptr<VstConverterWorkerProbe> probe)
        : probe_(std::move(probe)) {}

    ~SyntheticVstConverterWorker() override {
        cancel();
        join();
    }

    void start() {
        worker_ = std::jthread([probe = probe_](std::stop_token token) {
            probe->started.store(true, std::memory_order_release);
            while (!token.stop_requested()
                && !probe->cancelled.load(std::memory_order_acquire)) {
                probe->worker_steps.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            while (!probe->allow_finish.load(std::memory_order_acquire)) {
                probe->worker_steps.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            probe->completion_callbacks.fetch_add(1, std::memory_order_relaxed);
            probe->finished.store(true, std::memory_order_release);
        });
    }

    void cancel() noexcept override {
        probe_->cancel_calls.fetch_add(1, std::memory_order_relaxed);
        probe_->cancelled.store(true, std::memory_order_release);
        worker_.request_stop();
        if (probe_->processor_destroying.load(std::memory_order_acquire))
            probe_->allow_finish.store(true, std::memory_order_release);
    }

    void join() noexcept override {
        if (worker_.joinable()) worker_.join();
        probe_->joined.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool completed() const noexcept override {
        return probe_->finished.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<VstConverterWorkerProbe> probe_;
    std::jthread worker_;
};

void testVstConverterWorkerQuiescesAtProcessorTeardown() {
    using mgstc::plugin::PluginEditorContext;
    auto processor = std::make_unique<MgstcAudioProcessor>();
    auto context = std::make_unique<PluginEditorContext>(*processor);
    auto probe = std::make_shared<VstConverterWorkerProbe>();
    auto task = std::make_shared<SyntheticVstConverterWorker>(probe);
    require(context->backgroundTasks() != nullptr
            && context->backgroundTasks()->registerTask(task),
        "VST editor registers conversion work with its processor before start");
    task->start();
    const auto start_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (!probe->started.load(std::memory_order_acquire)
        && std::chrono::steady_clock::now() < start_deadline) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    require(probe->started.load(std::memory_order_acquire),
        "registered converter worker starts");
    require(probe->worker_steps.load(std::memory_order_relaxed) > 0,
        "synthetic worker performs bounded background work");

    // Closing the editor session must not synchronously wait for conversion.
    const auto close_start = std::chrono::steady_clock::now();
    context.reset();
    const auto close_elapsed = std::chrono::steady_clock::now() - close_start;
    require(close_elapsed < std::chrono::milliseconds(250),
        "closing the VST editor context does not join its converter worker");
    require(!probe->cancelled.load(std::memory_order_acquire)
            && !probe->finished.load(std::memory_order_acquire),
        "editor closure leaves processor-owned work alive for cancellation at shutdown");

    probe->processor_destroying.store(true, std::memory_order_release);
    const auto destroy_start = std::chrono::steady_clock::now();
    processor.reset();
    const auto destroy_elapsed = std::chrono::steady_clock::now() - destroy_start;
    require(destroy_elapsed < std::chrono::seconds(2),
        "processor destruction cancels and joins a bounded converter worker");
    require(probe->cancel_calls.load(std::memory_order_relaxed) > 0
            && probe->finished.load(std::memory_order_acquire)
            && probe->joined.load(std::memory_order_acquire),
        "processor teardown requests cancellation and waits for worker completion");

    const auto steps_after_join = probe->worker_steps.load(std::memory_order_relaxed);
    const auto callbacks_after_join = probe->completion_callbacks.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(probe->worker_steps.load(std::memory_order_relaxed) == steps_after_join
            && probe->completion_callbacks.load(std::memory_order_relaxed) == callbacks_after_join,
        "no converter worker activity or completion callback occurs after processor destruction returns");
}

void testVstSharedSccMorphHostAudio() {
    auto morphed = makeVstSccMorphDocument(true);
    const auto& layer = morphed.sound.layers.front();
    require(layer.scc_output_allocation
                == mgstc::engine::SccOutputAllocationMode::Contiguous
            && layer.scc_output_start == 10,
        "VST morph fixture requests the same contiguous SCC allocation as standalone");
    require(morphed.sound.embedded_timbres.size() == 1
            && layer.timbre_automation.size() == 2
            && layer.timbre_automation.back().scc_morph.enabled
            && layer.timbre_automation.back().scc_morph.intermediate_count == 3
            && layer.timbre_automation.back().scc_morph.curve == 0.5,
        "VST fixture contains owned base/arrival sources and gamma morph settings");
    requireSoundRoundTrip(morphed);

    MgstcAudioProcessor state_probe;
    require(state_probe.replacePluginState(morphed),
        "ordinary VST program compilation accepts SCC morph settings");
    const auto committed = state_probe.copyPluginState();
    requireSoundRoundTrip(committed);
    require(committed.sound.embedded_timbres.size() == 1
            && committed.sound.layers.front().scc_output_start == 10
            && committed.sound.layers.front().timbre_automation.back()
                .scc_morph.enabled,
        "committed VST state preserves owned morph sources and output settings");

    const auto morphed_pcm = renderVstSccMorphHostPcm(morphed);
    auto stepped = makeVstSccMorphDocument(false);
    const auto stepped_pcm = renderVstSccMorphHostPcm(stepped);
    require(peakOf(morphed_pcm) > 0.001,
        "ordinary VST MIDI through the shared compiler produces SCC audio");
    require(morphed_pcm.size() == stepped_pcm.size()
            && hashPcm(morphed_pcm) != hashPcm(stepped_pcm),
        "enabled SCC morph changes actual VST host PCM versus the same abrupt timbre step");
}

}  // namespace
