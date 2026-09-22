// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

// Stage D.2 Cubase isolation. Not a product feature.
// Default 0 keeps the Stage D.1 Release path. Diagnostic VST3 sets
// MGSTC_VST3_DIAG_MODE to one of the values below.

namespace mgstc::plugin {

constexpr int kVst3DiagOff = 0;
constexpr int kVst3DiagNon48Null = 1;
constexpr int kVst3DiagNon48NullLat0 = 2;
constexpr int kVst3DiagNon48EngineOnly = 3;
constexpr int kVst3DiagNon48SrcZero = 4;
constexpr int kVst3DiagNormalNoLatencyNotify = 5;

#ifndef MGSTC_VST3_DIAG_MODE
#define MGSTC_VST3_DIAG_MODE 0
#endif

constexpr int kVst3DiagMode = MGSTC_VST3_DIAG_MODE;

[[nodiscard]] constexpr const char* vst3DiagnosticModeName() noexcept {
    switch (kVst3DiagMode) {
    case kVst3DiagNon48Null:
        return "NON48_NULL";
    case kVst3DiagNon48NullLat0:
        return "NON48_NULL_LAT0";
    case kVst3DiagNon48EngineOnly:
        return "NON48_ENGINE_ONLY";
    case kVst3DiagNon48SrcZero:
        return "NON48_SRC_ZERO";
    case kVst3DiagNormalNoLatencyNotify:
        return "NORMAL_NO_LATENCY_NOTIFY";
    default:
        return "OFF";
    }
}

[[nodiscard]] constexpr bool vst3DiagSilencesNon48Process() noexcept {
    return kVst3DiagMode == kVst3DiagNon48Null
        || kVst3DiagMode == kVst3DiagNon48NullLat0;
}

[[nodiscard]] constexpr bool vst3DiagForcesZeroLatency() noexcept {
    return kVst3DiagMode == kVst3DiagNon48NullLat0
        || kVst3DiagMode == kVst3DiagNormalNoLatencyNotify;
}

[[nodiscard]] constexpr bool vst3DiagEngineOnlyNon48() noexcept {
    return kVst3DiagMode == kVst3DiagNon48EngineOnly;
}

[[nodiscard]] constexpr bool vst3DiagSrcZeroNon48() noexcept {
    return kVst3DiagMode == kVst3DiagNon48SrcZero;
}

}  // namespace mgstc::plugin
