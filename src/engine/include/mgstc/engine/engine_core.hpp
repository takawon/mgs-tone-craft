#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "mgstc/engine/dc_blocker.hpp"
#include "mgstc/engine/spectrum_capture.hpp"
#include "mgstc/engine/emulator_sound_output.hpp"
#include "mgstc/engine/runtime_session.hpp"
#include "mgstc/engine/sound_output_backend.hpp"
#include "mgstc/engine/tick_clock.hpp"

namespace mgstc::engine {

struct MixerGains {
    float master{3.0F};
    float psg{1.0F};
    float scc{1.0F};
    float opll{1.0F};
};

enum class RenderError : std::uint8_t {
    None,
    InvalidBuffer,
    Runtime,
    ChipWrite,
};

struct RenderResult {
    std::size_t frames{};
    bool clipped{};
    RenderError error{RenderError::None};
    TickResult tick_result{};

    [[nodiscard]] bool ok() const noexcept {
        return error == RenderError::None;
    }
};

struct OpllScopeFrame {
    static constexpr std::size_t kSampleCount = 800;
    // Display metadata; physical PCM remains in its native chip units.
    std::array<MixerGains, kSampleCount> mixer_gains{};

    std::array<float, kSampleCount> psg_samples{};
    std::array<float, kSampleCount> scc_samples{};
    std::array<float, kSampleCount> samples{};
    std::array<float, kSampleCount> mixed_samples{};
    std::array<std::array<float, kSampleCount>, RuntimeSession::kTrackCount> physical_samples{};
    std::array<EnvelopePlaybackPosition, RuntimeSession::kTrackCount> playback_positions{};
    std::array<std::uint64_t, RuntimeSession::kTrackCount> track_note_generation{};
    std::array<std::int8_t, RuntimeSession::kTrackCount> layer_tracks = [] {
        std::array<std::int8_t, RuntimeSession::kTrackCount> result{};
        result.fill(-1);
        return result;
    }();
    std::uint32_t captured_track_mask{};
    std::uint64_t selection_context{};
    std::uint64_t scope_ui_context{};
    std::uint64_t program_generation{};
    std::uint8_t selection_note{60};
    std::uint64_t sequence{};
    // Exclusive end at the internal48kHz clock; resets with the program.
    std::uint64_t sample_end{};
    // RealtimeEngineHost annotates the chip frame for analysis views.  The
    // last note remains valid after key-off so release tails keep their guide.
    std::uint8_t guide_note{60};
    std::uint8_t guide_track{};
    bool note_active{};
};

class EngineCore {
public:
    EngineCore() = default;

    [[nodiscard]] bool valid() const noexcept {
        return emulator_.valid();
    }

    // Non-owning. When null, writes and render use the local simulator.
    // When set to MAmidiMemo, writes go to RPC; local mix stays silent.
    void setOutputBackend(SoundOutputBackend* backend) noexcept {
        output_backend_ = backend;
    }

    [[nodiscard]] SoundOutputBackend* outputBackend() const noexcept {
        return output_backend_;
    }

    // Show the continuously maintained emulator mirror in the waveform views
    // during remote output. Does not unmute PC speakers.
    void setWaveformMonitor(bool enabled) noexcept {
        waveform_monitor_ = enabled;
    }

    [[nodiscard]] bool waveformMonitor() const noexcept {
        return waveform_monitor_;
    }

    // Audio consumer only. Offline rendering uses the continuously maintained
    // emulator mirror without changing the selected output or opening RPC.
    void setOfflineRendering(bool enabled) noexcept {
        offline_rendering_ = enabled;
    }

    [[nodiscard]] SoundOutputKind outputKind() const noexcept {
        return output_backend_ != nullptr
            ? output_backend_->kind()
            : SoundOutputKind::Emulator;
    }

    [[nodiscard]] RuntimeSession& session() noexcept {
        return session_;
    }

    [[nodiscard]] const RuntimeSession& session() const noexcept {
        return session_;
    }

    [[nodiscard]] const TickClock& clock() const noexcept {
        return clock_;
    }

    [[nodiscard]] MixerGains gains() const noexcept {
        return gains_;
    }

    [[nodiscard]] bool setGains(MixerGains gains) noexcept;
    void hardReset() noexcept;
    [[nodiscard]] bool takeOpllScopeFrame(
        OpllScopeFrame& frame) noexcept;
    // Audio consumer only. A selection transition drops an incomplete frame
    // instead of mixing old/new voices into the same display history.
    void setScopeSelection(std::uint32_t mask, std::uint64_t context) noexcept;
    // Queued NoteOn is applied at the next runtime tick. Do not publish its
    // new display identity over samples from the preceding physical voice.
    void awaitScopeNoteOn(std::uint8_t track) noexcept;
    void resetScopeCapture() noexcept {
        opll_scope_position_ = 0;
        opll_scope_ready_ = false;
    }

    // Interleaved stereo: L, R, L, R... Audible mix is silent while a
    // remote backend is selected; scope can still track the simulator.
    void setOpllScopeEnabled(bool enabled) noexcept {
        if (opll_scope_enabled_ != enabled) resetScopeCapture();
        opll_scope_enabled_ = enabled;
    }

    [[nodiscard]] bool opllScopeEnabled() const noexcept {
        return opll_scope_enabled_;
    }

    [[nodiscard]] RenderResult render(
        std::span<float> interleaved_stereo, SpectrumCapture* capture = nullptr) noexcept;

    // Set only while editing an inactive program; physical voices map to UI channels.
    void setSpectrumChannelMap(const SpectrumChannelMap& map) noexcept { spectrum_channel_map_ = map; }
    [[nodiscard]] const SpectrumChannelMap& spectrumChannelMap() const noexcept { return spectrum_channel_map_; }

private:
    [[nodiscard]] static bool validGain(float value) noexcept;
    [[nodiscard]] bool writeActiveRegisters(
        std::span<const RegisterWrite> writes) noexcept;
    [[nodiscard]] ChipSamples renderScopeSample(std::uint32_t channel_mask) noexcept;
    [[nodiscard]] static ChipSamples silentSample() noexcept {
        return {};
    }

    SpectrumChannelMap spectrum_channel_map_{identitySpectrumChannelMap()};
    RuntimeSession session_{};
    EmulatorSoundOutput emulator_{};
    SoundOutputBackend* output_backend_{nullptr};
    bool waveform_monitor_{false};
    bool offline_rendering_{false};
    TickClock clock_{};
    MixerGains gains_{};
    DcBlocker mix_dc_blocker_{48'000.0F, 3.4F};
    DcBlocker analysis_mix_dc_blocker_{48'000.0F, 3.4F};
    std::uint64_t analysis_context_{};
    std::array<float, OpllScopeFrame::kSampleCount> psg_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> scc_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> opll_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> mixed_scope_work_{};
    std::array<MixerGains, OpllScopeFrame::kSampleCount> scope_gain_work_{};
    std::array<std::array<float, OpllScopeFrame::kSampleCount>, RuntimeSession::kTrackCount> physical_scope_work_{};
    std::uint32_t scope_track_mask_{};
    std::uint32_t scope_requested_track_mask_{};
    std::uint32_t scope_waiting_track_mask_{};
    std::array<std::uint64_t, RuntimeSession::kTrackCount> scope_waiting_generations_{};
    std::uint64_t scope_selection_context_{};
    OpllScopeFrame opll_scope_completed_{};
    std::size_t opll_scope_position_{};
    std::uint64_t opll_scope_sequence_{};
    bool opll_scope_ready_{};
    bool opll_scope_enabled_{true};
};

}  // namespace mgstc::engine
