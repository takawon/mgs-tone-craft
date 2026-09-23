// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

#include <juce_audio_processors/juce_audio_processors.h>

#include "host_engine_timeline.hpp"
#include "layer_delay_scheduler.hpp"
#include "plugin_state.hpp"
#include "vst3_diagnostic.hpp"
#include "mgstc/audio/stereo_sample_rate_converter.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/voice_allocator.hpp"

namespace mgstc::plugin {

class MgstcAudioProcessor final : public juce::AudioProcessor {
public:
    static constexpr double kEngineSampleRate = 48'000.0;
    static constexpr std::uint8_t kMaxVoices = 16;
    static constexpr int kScratchFrames = 2048;
    static constexpr std::size_t kSrcChunkMaxFrames = 2048;
    static constexpr std::size_t kMappedEventCapacity = 1024;
    static constexpr bool kProcessBlockUsesScopedNoDenormals = false;

    MgstcAudioProcessor();
    ~MgstcAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void numChannelsChanged() override;
    void numBusesChanged() override;
    void processorLayoutsChanged() override;
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

    // Message thread. The editor is not the state authority.
    [[nodiscard]] PluginStateDocument copyPluginState() const;
    [[nodiscard]] bool replacePluginState(PluginStateDocument document);

    // Message-thread editor delegation. Does not open OS audio, MIDI,
    // MAmidi, or the tone library. Shared-wave audition is runtime only
    // and is not written into Plugin State.
    [[nodiscard]] bool replaceEditorComposite(
        mgstc::engine::CompositeTimbre sound,
        bool polyphonic,
        std::uint8_t& voice_capacity);
    [[nodiscard]] bool editorNoteOn(
        std::uint8_t track,
        std::uint8_t note);
    [[nodiscard]] bool editorNoteOff(std::uint8_t track);
    void editorSilenceTrack(std::uint8_t track);
    void editorFlushPending();
    void editorSetMasterVolumePercent(int percent);
    [[nodiscard]] int editorMasterVolumePercent() const noexcept;
    [[nodiscard]] std::uint64_t editorMasterVolumeRevision() const noexcept;
    void editorPublishSharedScc(const mgstc::engine::SccWaveform& waveform);
    void editorPublishSharedOpll(
        const mgstc::engine::OpllPatchParameters& patch);
    [[nodiscard]] bool editorAuditionShared(
        const mgstc::engine::SccWaveform& waveform,
        const mgstc::engine::OpllPatchParameters& patch,
        bool retrigger,
        std::uint8_t track,
        std::uint8_t note,
        bool commit_scc,
        bool commit_opll);
    [[nodiscard]] bool editorRestoreCommittedProgram();
    [[nodiscard]] const mgstc::engine::SccWaveform&
    editorSharedScc() const noexcept;
    [[nodiscard]] std::uint64_t editorSharedSccRevision() const noexcept;
    [[nodiscard]] const mgstc::engine::OpllPatchParameters&
    editorSharedOpll() const noexcept;
    [[nodiscard]] std::uint64_t editorSharedOpllRevision() const noexcept;
    [[nodiscard]] bool editorSharedProgramActive() const noexcept;
    [[nodiscard]] bool editorProgramReady() const noexcept;
    void editorNoteBlockedBackendCall() noexcept;
    [[nodiscard]] std::uint64_t editorBlockedBackendCalls() const noexcept;

private:
    friend struct MgstcAudioProcessorTestAccess;

    enum class TrackLayerState : std::uint8_t {
        Inactive = 0,
        Pending,
        Active,
    };

    enum class MappedMidiType : std::uint8_t {
        NoteOn = 0,
        NoteOff,
        AllNotesOff,
        AllSoundOff,
    };

    struct MappedMidiEvent {
        std::uint64_t engine_frame{};
        MappedMidiType type{MappedMidiType::NoteOn};
        std::uint8_t midi_note{};
    };

    struct NoteOnLogEntry {
        std::uint64_t frame{};
        std::uint8_t physical_track{};
        std::uint8_t midi_note{};
    };

    void applyMidiMessage(const juce::MidiMessage& message) noexcept;
    void applyMappedEvent(const MappedMidiEvent& event) noexcept;
    void noteOn(std::uint8_t midi_note) noexcept;
    void noteOff(std::uint8_t midi_note) noexcept;
    void stopVoice(std::uint8_t voice, bool hard) noexcept;
    void stopTrack(std::uint8_t track, bool hard) noexcept;
    void startVoice(std::uint8_t voice, std::uint8_t midi_note) noexcept;
    void fireDueLayers(std::uint64_t frame) noexcept;
    void recordNoteOn(
        std::uint8_t physical_track,
        std::uint8_t midi_note) noexcept;
    void allNotesOff() noexcept;
    void allSoundOff() noexcept;
    void resetPlaybackState(bool hard_stop) noexcept;
    void resetProgramState() noexcept;
    void renderTo(
        juce::AudioBuffer<float>& buffer,
        int start_frame,
        int frame_count) noexcept;
    void silenceFrom(
        juce::AudioBuffer<float>& buffer,
        int start_frame) noexcept;
    void collectHostMidi(
        const juce::MidiBuffer& midi,
        int num_samples) noexcept;
    void processDirectBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi,
        int num_samples) noexcept;
    void renderDirectSpan(
        juce::AudioBuffer<float>& buffer,
        std::uint64_t block_start) noexcept;
    void processResampledBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi,
        int num_samples) noexcept;
    [[nodiscard]] std::uint64_t currentMidiEngineFrame() const noexcept;
    void processEventsAt(std::uint64_t frame) noexcept;
    std::size_t pullSrcSource(std::span<float> interleaved) noexcept;
    void resetBlockDiagnostics() noexcept;
    bool renderEngineFramesToInterleaved(
        std::span<float> interleaved,
        int start_frame,
        int frame_count) noexcept;
    static std::size_t renderSrcFramesCallback(
        void* context,
        std::span<float> interleaved) noexcept;
    static std::size_t renderSrcZeroCallback(
        void* context,
        std::span<float> interleaved) noexcept;
    [[nodiscard]] static int srcLatencyHostSamples(
        std::uint32_t host_rate_hz) noexcept;
    [[nodiscard]] bool pluginQuiescent() const noexcept;
    void reportPluginLatency(int samples) noexcept;
    void recordProcessBlockContract(
        const juce::AudioBuffer<float>& buffer,
        const juce::MidiBuffer& midi) noexcept;
    void writeDiagnosticSnapshot(const char* reason) const noexcept;
    void advanceEngineUntil(std::uint64_t end_frame) noexcept;
    void processEngineOnlyBlock(
        juce::AudioBuffer<float>& buffer,
        juce::MidiBuffer& midi,
        int num_samples) noexcept;
    void processSrcZeroBlock(
        juce::AudioBuffer<float>& buffer,
        int num_samples) noexcept;
    enum class CommitMode : std::uint8_t {
        Initial,
        Offline,
        Live,
    };

    [[nodiscard]] bool onAudioThread() const noexcept;
    void applyMasterVolume(
        juce::AudioBuffer<float>& buffer,
        int num_samples) noexcept;
    [[nodiscard]] bool commitPluginState(
        PluginStateDocument document,
        CommitMode mode,
        bool polyphonic = true,
        std::uint8_t* voice_capacity = nullptr);
    [[nodiscard]] bool editorSubmitEngine(
        const mgstc::engine::EngineCommand& command);
    void applyCommittedPlaybackPlan() noexcept;
    [[nodiscard]] const mgstc::engine::CompositePlaybackPlan&
    activePlan() const noexcept;
    [[nodiscard]] std::uint8_t pickPlanSlot() const noexcept;
    void poseAsAudioThreadForTest() noexcept;
    void clearAudioThreadForTest() noexcept;
    [[nodiscard]] bool hostIsEngineRate() const noexcept;

    mgstc::engine::RealtimeEngineHost engine_{};
    mgstc::engine::SequentialVoiceAllocator voices_{kMaxVoices};
    std::array<mgstc::engine::CompositePlaybackPlan, 3> plan_slots_{};
    std::atomic<std::uint8_t> published_plan_{0};
    std::atomic<std::uint8_t> acknowledged_plan_{0};
    std::uint8_t applied_plan_{0};
    mutable std::mutex state_mu_{};
    mgstc::engine::CompositeTimbre sound_{};
    PluginEditorState editor_{};
    std::uint64_t library_id_{0};
    std::atomic<std::uint64_t> audio_thread_token_{0};
    std::atomic<std::uint64_t> state_compile_count_{0};
    std::atomic<std::uint64_t> state_audio_reject_count_{0};
    std::atomic<bool> host_prepared_{false};
    LayerDelayScheduler scheduler_{};
    mgstc::audio::StereoSampleRateConverter src_{};
    std::array<TrackLayerState, LayerDelayScheduler::kPhysicalTrackCount>
        track_state_{};
    std::array<NoteOnLogEntry, 64> note_on_log_{};
    std::size_t note_on_log_size_{};
    std::uint64_t engine_frame_position_{};
    std::uint64_t host_frame_position_{};
    std::uint32_t host_rate_hz_{};
    std::array<float, static_cast<std::size_t>(kScratchFrames) * 2>
        scratch_{};
    bool sample_rate_ok_{false};
    std::atomic<bool> program_ready_{false};
    bool use_src_{false};

    std::array<MappedMidiEvent, kMappedEventCapacity> mapped_events_{};
    std::size_t mapped_count_{};
    std::size_t mapped_index_{};
    juce::MidiBufferIterator midi_it_{};
    juce::MidiBufferIterator midi_end_{};
    std::uint64_t src_host_block_start_{};
    int src_host_block_samples_{};
    std::uint64_t known_engine_end_{};
    std::uint32_t src_callbacks_{};
    std::uint32_t src_fill_iters_{};
    std::uint32_t src_zero_progress_{};
    std::uint32_t src_known_end_rejects_{};
    std::uint32_t src_watchdog_trips_{};
    std::uint32_t render_audio_calls_{};
    std::uint64_t src_stall_engine_{};
    std::uint64_t src_stall_mapped_{};
    std::uint64_t src_stall_known_end_{};
    std::uint64_t src_process_ns_{};
    std::uint64_t src_fill_ns_{};
    std::uint64_t src_render_ns_{};
    std::uint64_t copy_ns_{};

    bool editor_shared_program_active_{false};
    bool editor_runtime_program_temporary_{false};
    std::atomic<int> editor_master_volume_percent_{100};
    float master_volume_gain_{1.0F};
    std::uint64_t editor_master_volume_revision_{1};
    mgstc::engine::SccWaveform editor_shared_scc_{};
    std::uint64_t editor_shared_scc_revision_{1};
    mgstc::engine::OpllPatchParameters editor_shared_opll_{};
    std::uint64_t editor_shared_opll_revision_{1};
    std::uint64_t editor_blocked_backend_calls_{0};

    bool in_process_block_{false};
    std::atomic<std::uint64_t> diag_process_block_{};
    std::atomic<std::uint64_t> diag_prepare_to_play_{};
    std::atomic<std::uint64_t> diag_release_resources_{};
    std::atomic<std::uint64_t> diag_reset_{};
    std::atomic<std::uint64_t> diag_latency_calls_{};
    std::atomic<std::uint64_t> diag_latency_same_{};
    std::atomic<std::uint64_t> diag_latency_changed_{};
    std::atomic<std::uint64_t> diag_latency_from_process_{};
    mutable std::atomic<std::uint64_t> diag_buses_layout_supported_{};
    std::atomic<std::uint64_t> diag_num_channels_changed_{};
    std::atomic<std::uint64_t> diag_num_buses_changed_{};
    std::atomic<std::uint64_t> diag_layouts_changed_{};
    std::atomic<std::uint32_t> diag_last_num_samples_{};
    std::atomic<std::uint32_t> diag_last_buffer_channels_{};
    std::atomic<std::uint32_t> diag_last_input_channels_{};
    std::atomic<std::uint32_t> diag_last_output_channels_{};
    std::atomic<std::uint32_t> diag_last_midi_events_{};
    std::atomic<std::uint32_t> diag_process_sample_rate_hz_{};
    std::atomic<std::uint64_t> diag_sample_rate_changes_in_process_{};
    std::atomic<int> diag_last_samples_per_block_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MgstcAudioProcessor)
};

}  // namespace mgstc::plugin
