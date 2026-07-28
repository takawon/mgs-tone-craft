#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/runtime_session.hpp"
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

    std::array<float, kSampleCount> samples{};
    std::uint64_t sequence{};
};

class EngineCore {
public:
    EngineCore() = default;

    [[nodiscard]] bool valid() const noexcept {
        return chips_.valid();
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

    // Interleaved stereo: L, R, L, R... Both channels receive the same
    // MGSDRV-compatible mono mix.
    [[nodiscard]] RenderResult render(
        std::span<float> interleaved_stereo) noexcept;

private:
    [[nodiscard]] static bool validGain(float value) noexcept;

    RuntimeSession session_{};
    ChipRack chips_{};
    TickClock clock_{};
    MixerGains gains_{};
    std::array<float, OpllScopeFrame::kSampleCount> opll_scope_work_{};
    OpllScopeFrame opll_scope_completed_{};
    std::size_t opll_scope_position_{};
    std::uint64_t opll_scope_sequence_{};
    bool opll_scope_ready_{};
};

}  // namespace mgstc::engine
