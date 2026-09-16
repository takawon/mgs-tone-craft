// SPDX-License-Identifier: AGPL-3.0-only

#include "../../src/app/juce/composite_envelope_compile.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void requireNoEmptyLoop(const std::vector<std::uint8_t>& bytecode) {
    for (std::size_t i = 0; i + 1 < bytecode.size(); ++i) {
        require(
            !(bytecode[i] == 0x40 && bytecode[i + 1] == 0x60),
            "compiled a zero-wait [] loop");
    }
}

void testOneStepVolumeLoopCompilesWithWait() {
    using namespace mgstc::engine;
    auto layer = defaultCompositeTimbre().layers.front();
    layer.volume = 15;
    layer.envelope_timeline = {
        .length_counts = 1,
        .loop_start_count = 0,
        .loop_end_count = 1,
    };
    layer.volume_envelope.events = {
        {EnvelopeEventKind::Volume, 15, 0, 0},
        {EnvelopeEventKind::Volume, 15, 0, 1},
    };
    const TimbreNumberResolution numbers{};
    const auto bytecode = compileCompositeEnvelopeLane(
        layer, CompositeEnvelopeLane::Volume, numbers);
    require(!bytecode.empty(), "empty bytecode");
    requireNoEmptyLoop(bytecode);
    SequenceEnvelopeRuntime runtime(bytecode);
    runtime.resetForKeyOn();
    EventBuffer buffer(16);
    for (int tick = 0; tick < 16; ++tick) {
        buffer.clear();
        require(
            runtime.processTick(buffer) == SequenceError::None,
            "1-step [f] loop hit the instruction budget");
        require(runtime.volume() == 15, "1-step [f] lost volume 15");
    }
}

}  // namespace

int main() {
    try {
        testOneStepVolumeLoopCompilesWithWait();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
