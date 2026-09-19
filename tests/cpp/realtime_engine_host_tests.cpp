// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_program_compiler.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/engine_command.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void loadDefaultProgram(
    mgstc::engine::RealtimeEngineHost& host,
    mgstc::engine::CompositePlaybackPlan& plan) {
    using namespace mgstc::engine;
    auto edit = host.beginProgramEdit();
    require(edit.valid(), "beginProgramEdit failed");
    require(
        compileCompositeProgram(
            *edit.engine,
            defaultCompositeTimbre(),
            {.polyphonic = true},
            &plan),
        "compileCompositeProgram failed");
    require(host.submitProgram(edit), "submitProgram failed");
    std::array<float, 2> drain{};
    host.drainPendingCommands(drain);
}

double peakOf(const std::array<float, 1600>& pcm) {
    double peak = 0.0;
    for (const auto sample : pcm) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    return peak;
}

void testRealtimeNoteOnRendersWithoutSpscNoteCommands() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    CompositePlaybackPlan plan;
    loadDefaultProgram(host, plan);
    require(!plan.audible_layers.empty(), "default plan has no layers");
    require(
        host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
        "realtimeNoteOn failed");
    std::array<float, 1600> pcm{};
    double peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto result = host.renderAudio(pcm);
        require(result.ok(), "renderAudio failed");
        peak = std::max(peak, peakOf(pcm));
    }
    require(peak > 0.001, "realtime note rendered silence");
}

void testRealtimeNoteOnDoesNotAffectPriorSamples() {
    using namespace mgstc::engine;
    RealtimeEngineHost host;
    CompositePlaybackPlan plan;
    loadDefaultProgram(host, plan);
    std::array<float, 256> prefix{};
    const auto before = host.renderAudio(prefix);
    require(before.ok(), "prefix render failed");
    double prefix_peak = 0.0;
    for (const auto sample : prefix) {
        prefix_peak = std::max(
            prefix_peak, static_cast<double>(std::fabs(sample)));
    }
    require(prefix_peak < 0.001, "gated program should be silent before note");
    require(
        host.realtimeNoteOn(plan.physicalTrack(plan.audible_layers.front(), 0), 60),
        "realtimeNoteOn failed");
    std::array<float, 1600> pcm{};
    double later_peak = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        const auto result = host.renderAudio(pcm);
        require(result.ok(), "post-note render failed");
        later_peak = std::max(later_peak, peakOf(pcm));
    }
    require(later_peak > 0.001, "note after prefix rendered silence");
}

void testTwoHostsDoNotShareNoteState() {
    using namespace mgstc::engine;
    RealtimeEngineHost a;
    RealtimeEngineHost b;
    CompositePlaybackPlan plan_a;
    CompositePlaybackPlan plan_b;
    loadDefaultProgram(a, plan_a);
    loadDefaultProgram(b, plan_b);
    require(
        a.realtimeNoteOn(plan_a.physicalTrack(plan_a.audible_layers.front(), 0), 60),
        "host A noteOn failed");
    std::array<float, 1600> pcm{};
    double peak_a = 0.0;
    double peak_b = 0.0;
    for (int frame = 0; frame < 60; ++frame) {
        require(a.renderAudio(pcm).ok(), "host A render");
        peak_a = std::max(peak_a, peakOf(pcm));
        require(b.renderAudio(pcm).ok(), "host B render");
        peak_b = std::max(peak_b, peakOf(pcm));
    }
    require(peak_a > 0.001, "host A should sound");
    require(peak_b < 0.001, "host B should stay silent");
}

}  // namespace

int main() {
    try {
        testRealtimeNoteOnRendersWithoutSpscNoteCommands();
        testRealtimeNoteOnDoesNotAffectPriorSamples();
        testTwoHostsDoNotShareNoteState();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
