#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "mgstc/engine/dc_blocker.hpp"
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

    std::array<float, kSampleCount> psg_samples{};
    std::array<float, kSampleCount> scc_samples{};
    std::array<float, kSampleCount> samples{};
    std::array<float, kSampleCount> mixed_samples{};
    std::uint64_t sequence{};
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

    // When remote output is active, also drive the local simulator so
    // scope/waveform views keep updating. Does not unmute PC speakers.
    void setWaveformMonitor(bool enabled) noexcept {
        waveform_monitor_ = enabled;
    }

    [[nodiscard]] bool waveformMonitor() const noexcept {
        return waveform_monitor_;
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

    // Interleaved stereo: L, R, L, R... Audible mix is silent while a
    // remote backend is selected; scope can still track the simulator.
    [[nodiscard]] RenderResult render(
        std::span<float> interleaved_stereo) noexcept;

private:
    [[nodiscard]] static bool validGain(float value) noexcept;
    [[nodiscard]] bool writeActiveRegisters(
        std::span<const RegisterWrite> writes) noexcept;
    [[nodiscard]] ChipSamples renderScopeSample() noexcept;
    [[nodiscard]] static ChipSamples silentSample() noexcept {
        return {};
    }

    RuntimeSession session_{};
    EmulatorSoundOutput emulator_{};
    SoundOutputBackend* output_backend_{nullptr};
    bool waveform_monitor_{false};
    TickClock clock_{};
    MixerGains gains_{};
    DcBlocker mix_dc_blocker_{48'000.0F, 3.4F};
    std::array<float, OpllScopeFrame::kSampleCount> psg_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> scc_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> opll_scope_work_{};
    std::array<float, OpllScopeFrame::kSampleCount> mixed_scope_work_{};
    OpllScopeFrame opll_scope_completed_{};
    std::size_t opll_scope_position_{};
    std::uint64_t opll_scope_sequence_{};
    bool opll_scope_ready_{};
};

}  // namespace mgstc::engine
