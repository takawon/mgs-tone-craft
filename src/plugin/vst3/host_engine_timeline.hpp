// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>

namespace mgstc::plugin {

inline constexpr std::uint32_t kEngineSampleRateHz = 48'000;

// Absolute Host frame → 48 kHz Engine frame.
// engine = ceil(host * 48000 / host_rate), computed without a running
// double accumulator so 60 s of 44.1 kHz does not drift.
// Ceil never maps a Host sample to an earlier Engine frame than its
// musical time. Exact values (441 Host @ 44.1 kHz → 480 Engine) stay exact.
[[nodiscard]] constexpr std::uint64_t hostFrameToEngineFrame(
    std::uint64_t host_frame,
    std::uint32_t host_rate_hz) noexcept {
    if (host_rate_hz == 0) {
        return 0;
    }
    if (host_rate_hz == kEngineSampleRateHz) {
        return host_frame;
    }
    const auto rate = static_cast<std::uint64_t>(host_rate_hz);
    const auto whole =
        (host_frame / rate) * static_cast<std::uint64_t>(kEngineSampleRateHz);
    const auto rem = host_frame % rate;
    const auto num =
        rem * static_cast<std::uint64_t>(kEngineSampleRateHz) + rate - 1U;
    return whole + num / rate;
}

}  // namespace mgstc::plugin
