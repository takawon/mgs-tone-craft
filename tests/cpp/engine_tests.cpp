#include <cstdint>
#include <array>
#include <algorithm>
#include <cmath>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/chip_rack.hpp"
#include "mgstc/engine/composite_timbre.hpp"
#include "mgstc/engine/composite_timbre_library.hpp"
#include "mgstc/engine/engine_core.hpp"
#include "mgstc/engine/mgs_envelope_io.hpp"
#include "mgstc/engine/mgs_timbre_io.hpp"
#include "mgstc/engine/note_pitch.hpp"
#include "mgstc/engine/opll_envelope_trace.hpp"
#include "mgstc/engine/opll_patch.hpp"
#include "mgstc/engine/pc_keyboard.hpp"
#include "mgstc/engine/register_mapper.hpp"
#include "mgstc/engine/realtime_engine_host.hpp"
#include "mgstc/engine/runtime_session.hpp"
#include "mgstc/engine/scc_waveform.hpp"
#include "mgstc/engine/timbre_library.hpp"
#include "mgstc/engine/spsc_queue.hpp"
#include "mgstc/engine/shared_state.hpp"
#include "mgstc/engine/tick_clock.hpp"
#include "mgstc/engine/volume.hpp"
#include "mgstc/engine/voice_allocator.hpp"
#include "mgstc/engine/wave_import.hpp"
#ifdef _WIN32
#include "mgstc/audio/wasapi_audio_sink.hpp"
#endif

namespace {

using mgstc::engine::EventBuffer;
using mgstc::engine::EngineCore;
using mgstc::engine::ChipId;
using mgstc::engine::ChipRack;
using mgstc::engine::MapError;
using mgstc::engine::MeaningEvent;
using mgstc::engine::MeaningEventKind;
using mgstc::engine::MixerGains;
using mgstc::engine::EngineCommand;
using mgstc::engine::EngineNotice;
using mgstc::engine::EngineNoticeType;
using mgstc::engine::RealtimeEngineHost;
using mgstc::engine::NineVoiceRhythmState;
using mgstc::engine::NotePitch;
using mgstc::engine::OpllScopeFrame;
using mgstc::engine::OpllEnvelopeTrace;
using mgstc::engine::PsgHardwareEnvelopeState;
using mgstc::engine::PsgTrackEnvelopeMode;
using mgstc::engine::ProgramAudition;
using mgstc::engine::ProgramEdit;
using mgstc::engine::RateEnvelopeDefinition;
using mgstc::engine::RateEnvelopeRuntime;
using mgstc::engine::RatePhase;
using mgstc::engine::RegisterMapper;
using mgstc::engine::RegisterWrite;
using mgstc::engine::RegisterWriteBuffer;
using mgstc::engine::RuntimeSession;
using mgstc::engine::SccHarmonic;
using mgstc::engine::SccApplyRange;
using mgstc::engine::SccMergeOptions;
using mgstc::engine::SccWavePreset;
using mgstc::engine::SccWaveform;
using mgstc::engine::SpscQueue;
using mgstc::engine::RenderError;
using mgstc::engine::SequenceEnvelopeRuntime;
using mgstc::engine::SequenceError;
using mgstc::engine::TickClock;
using mgstc::engine::TrackRuntime;
using mgstc::engine::WriteReason;
using mgstc::engine::notePitch;
using mgstc::engine::decodeOpllPatch;
using mgstc::engine::defaultOpllPatch;
using mgstc::engine::encodeOpllPatch;
using mgstc::engine::averageSccWaveform;
using mgstc::engine::applySccWaveformRange;
using mgstc::engine::generateSccPreset;
using mgstc::engine::formatMgsOpllDefinition;
using mgstc::engine::formatMgsSccDefinition;
using mgstc::engine::invertSccWaveform;
using mgstc::engine::mergeSccWaveforms;
using mgstc::engine::normalizeSccWaveform;
using mgstc::engine::parseMgsOpllDefinition;
using mgstc::engine::parseMgsSccDefinition;
using mgstc::engine::rotateSccWaveform;
using mgstc::engine::scaleSccWaveformVertically;
using mgstc::engine::shiftSccWaveformVertically;
using mgstc::engine::traceOpllEnvelope;
using mgstc::engine::ym2413RomPatch;
using mgstc::engine::analyzeWaveCycle;
using mgstc::engine::approximateWaveCycleWithOpll;
using mgstc::engine::approximateWavePcmCandidatesWithOpll;
using mgstc::engine::approximateWavePcmWithOpll;
using mgstc::engine::parseWavePcm;
using mgstc::engine::waveCycleToScc;
#ifdef _WIN32
using mgstc::audio::WasapiAudioSink;
#endif

static_assert(
    sizeof(RealtimeEngineHost) < 128 * 1024,
    "RealtimeEngineHost must remain safe to create on the UI stack");

template <typename Actual, typename Expected>
void requireEqual(
    const Actual& actual,
    const Expected& expected,
    const char* expression,
    const char* file,
    int line) {
    if (!(actual == expected)) {
        std::ostringstream message;
        message << file << ':' << line << ": " << expression;
        throw std::runtime_error(message.str());
    }
}

#define REQUIRE_EQ(actual, ...) \
    requireEqual( \
        (actual), \
        (__VA_ARGS__), \
        #actual " == " #__VA_ARGS__, \
        __FILE__, \
        __LINE__)

std::vector<MeaningEvent> tick(
    SequenceEnvelopeRuntime& runtime,
    EventBuffer& buffer) {
    buffer.clear();
    REQUIRE_EQ(runtime.processTick(buffer), SequenceError::None);
    return {buffer.events().begin(), buffer.events().end()};
}

std::uint32_t littleU32(
    const std::vector<std::uint8_t>& data,
    std::size_t offset) {
    return static_cast<std::uint32_t>(data.at(offset))
        | (static_cast<std::uint32_t>(data.at(offset + 1)) << 8)
        | (static_cast<std::uint32_t>(data.at(offset + 2)) << 16)
        | (static_cast<std::uint32_t>(data.at(offset + 3)) << 24);
}

std::vector<std::pair<int, int>> ayWritesInFrame(
    const std::filesystem::path& path,
    std::uint32_t wanted_frame) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open VGM fixture");
    }
    std::vector<std::uint8_t> data;
    char byte{};
    while (stream.get(byte)) {
        data.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(byte)));
    }
    if (data.size() < 0x40
        || std::string_view(
            reinterpret_cast<const char*>(data.data()),
            4) != "Vgm ") {
        throw std::runtime_error("invalid VGM fixture");
    }
    const auto version = littleU32(data, 0x08);
    const auto relative = littleU32(data, 0x34);
    std::size_t position = version < 0x150
        ? 0x40
        : static_cast<std::size_t>(0x34 + relative);
    std::uint32_t sample = 0;
    std::vector<std::pair<int, int>> writes;

    const auto skipOperands = [](std::uint8_t opcode) -> std::size_t {
        if (opcode == 0x4F || opcode == 0x50
            || opcode == 0x30 || opcode == 0x31) {
            return 1;
        }
        if ((opcode >= 0x40 && opcode <= 0x5F)
            || (opcode >= 0xA0 && opcode <= 0xBF)) {
            return 2;
        }
        if (opcode >= 0xC0 && opcode <= 0xDF) {
            return 3;
        }
        if (opcode >= 0xE0) {
            return 4;
        }
        throw std::runtime_error("unsupported VGM opcode");
    };

    while (position < data.size()) {
        const auto opcode = data[position++];
        if (opcode == 0x66) {
            break;
        }
        if (opcode == 0x61) {
            sample += static_cast<std::uint32_t>(
                data.at(position)
                | (static_cast<std::uint16_t>(data.at(position + 1)) << 8));
            position += 2;
        } else if (opcode == 0x62) {
            sample += 735;
        } else if (opcode == 0x63) {
            sample += 882;
        } else if (opcode >= 0x70 && opcode <= 0x7F) {
            sample += static_cast<std::uint32_t>((opcode & 0x0F) + 1);
        } else if (opcode >= 0x80 && opcode <= 0x8F) {
            sample += static_cast<std::uint32_t>(opcode & 0x0F);
        } else if (opcode == 0xA0) {
            if (sample / 735 == wanted_frame) {
                writes.emplace_back(
                    data.at(position),
                    data.at(position + 1));
            }
            position += 2;
        } else {
            position += skipOperands(opcode);
        }
    }
    return writes;
}

void testTickClockSplitsVariableCallbacks() {
    TickClock clock;
    REQUIRE_EQ(clock.currentTick(), 0ULL);
    REQUIRE_EQ(clock.tickDue(), true);
    REQUIRE_EQ(clock.beginTick(), true);
    REQUIRE_EQ(clock.consumeFrames(127), 127U);
    REQUIRE_EQ(clock.framesUntilNextTick(), 673U);
    REQUIRE_EQ(clock.consumeFrames(1'000), 673U);
    REQUIRE_EQ(clock.currentTick(), 1ULL);
    REQUIRE_EQ(clock.tickDue(), true);
    REQUIRE_EQ(clock.beginTick(), true);
    REQUIRE_EQ(clock.consumeFrames(800), 800U);
    REQUIRE_EQ(clock.currentTick(), 2ULL);
}

void testAdjacentVolumesAreOneTickApart() {
    SequenceEnvelopeRuntime runtime({0x0F, 0x0E});
    EventBuffer buffer(16);
    REQUIRE_EQ(
        tick(runtime, buffer),
        std::vector<MeaningEvent>({
            {0, MeaningEventKind::Volume, 15, 0},
        }));
    REQUIRE_EQ(
        tick(runtime, buffer),
        std::vector<MeaningEvent>({
            {1, MeaningEventKind::Volume, 14, 0},
        }));
    REQUIRE_EQ(
        tick(runtime, buffer),
        std::vector<MeaningEvent>({
            {2, MeaningEventKind::Volume, 14, 0},
        }));
}

void testObservedFourCountHolds() {
    SequenceEnvelopeRuntime runtime({
        0xEF, 0x04,
        0xED, 0x04,
        0xE8, 0x04,
        0xE4, 0x04,
        0x00,
    });
    EventBuffer buffer(16);
    std::vector<std::pair<std::uint64_t, std::int32_t>> volumes;
    for (int index = 0; index < 17; ++index) {
        for (const auto& event : tick(runtime, buffer)) {
            if (event.kind == MeaningEventKind::Volume) {
                volumes.emplace_back(event.tick, event.arg0);
            }
        }
    }
    REQUIRE_EQ(
        volumes,
        std::vector<std::pair<std::uint64_t, std::int32_t>>({
            {0, 15}, {4, 13}, {8, 8}, {12, 4}, {16, 0},
        }));
}

void testPatchAndRegisterWriteAreZeroTime() {
    SequenceEnvelopeRuntime runtime({
        0x10, 0x03,
        0x11, 0x02, 0x15,
        0x0F,
    });
    EventBuffer buffer(16);
    REQUIRE_EQ(
        tick(runtime, buffer),
        std::vector<MeaningEvent>({
            {0, MeaningEventKind::Patch, 3, 0},
            {0, MeaningEventKind::RegisterWrite, 2, 0x15},
            {0, MeaningEventKind::Volume, 15, 0},
        }));
}

void testRampUsesIntegerRemainderDistribution() {
    SequenceEnvelopeRuntime runtime({0x00, 0x2F, 0x04});
    EventBuffer buffer(16);
    std::vector<std::vector<MeaningEvent>> actual;
    for (int index = 0; index < 6; ++index) {
        actual.push_back(tick(runtime, buffer));
    }
    REQUIRE_EQ(
        actual,
        std::vector<std::vector<MeaningEvent>>({
            {{0, MeaningEventKind::Volume, 0, 0}},
            {{1, MeaningEventKind::Volume, 0, 0}},
            {{2, MeaningEventKind::Volume, 3, 0}},
            {{3, MeaningEventKind::Volume, 7, 0}},
            {{4, MeaningEventKind::Volume, 11, 0}},
            {
                {5, MeaningEventKind::Volume, 15, 0},
                {5, MeaningEventKind::Volume, 15, 0},
            },
        }));
}

void testFrequencyDeltasAreSignedAndCumulativeEvents() {
    SequenceEnvelopeRuntime runtime({
        0x12, 0x03,
        0x12, 0xFF,
        0x0F,
    });
    EventBuffer buffer(16);
    REQUIRE_EQ(
        tick(runtime, buffer),
        std::vector<MeaningEvent>({
            {0, MeaningEventKind::FrequencyDelta, 3, 0},
            {0, MeaningEventKind::FrequencyDelta, -1, 0},
            {0, MeaningEventKind::Volume, 15, 0},
        }));
}

void testNoWaitLoopHitsInstructionBudget() {
    SequenceEnvelopeRuntime runtime({0x40, 0x60});
    EventBuffer buffer(16);
    REQUIRE_EQ(
        runtime.processTick(buffer, 32),
        SequenceError::InstructionBudgetExceeded);
}

void testCompositeSequenceLanesLoopIndependently() {
    TrackRuntime runtime;
    REQUIRE_EQ(
        runtime.setCompositeSequenceEnvelopes(
            {0x40, 0x01, 0xE1, 0x01, 0x02, 0x60},
            {0x40, 0x12, 0x01, 0xE0, 0x02, 0x60},
            {0x40, 0x10, 0x02, 0xE0, 0x03, 0x60}),
        true);
    runtime.resetForKeyOn();
    EventBuffer events(32);

    REQUIRE_EQ(runtime.processTick(events), SequenceError::None);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(events.events()[0].kind, MeaningEventKind::Patch);
    REQUIRE_EQ(events.events()[0].arg0, 2);
    REQUIRE_EQ(events.events()[1].kind, MeaningEventKind::FrequencyDelta);
    REQUIRE_EQ(events.events()[1].arg0, 1);
    REQUIRE_EQ(events.events()[2].kind, MeaningEventKind::Volume);
    REQUIRE_EQ(events.events()[2].arg0, 1);

    events.clear();
    REQUIRE_EQ(runtime.processTick(events), SequenceError::None);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(events.events()[0].kind, MeaningEventKind::Volume);
    REQUIRE_EQ(events.events()[0].arg0, 1);

    events.clear();
    REQUIRE_EQ(runtime.processTick(events), SequenceError::None);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(events.events()[0].kind, MeaningEventKind::FrequencyDelta);
    REQUIRE_EQ(events.events()[1].kind, MeaningEventKind::Volume);
    REQUIRE_EQ(events.events()[1].arg0, 2);

    events.clear();
    REQUIRE_EQ(runtime.processTick(events), SequenceError::None);
    REQUIRE_EQ(events.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(events.events()[0].kind, MeaningEventKind::Patch);
    REQUIRE_EQ(events.events()[1].kind, MeaningEventKind::Volume);
    REQUIRE_EQ(events.events()[1].arg0, 1);
}

void testRateEnvelopeNativePhases() {
    RateEnvelopeRuntime runtime({
        .attack_level = 100,
        .attack_rate = 100,
        .decay_rate = 30,
        .sustain_level = 180,
        .sustain_rate = 20,
        .release_rate = 40,
    });
    REQUIRE_EQ(runtime.level(), static_cast<std::uint8_t>(100));
    REQUIRE_EQ(runtime.processTick().arg0, 200);
    REQUIRE_EQ(runtime.processTick().arg0, 255);
    REQUIRE_EQ(runtime.phase(), RatePhase::Decay);
    REQUIRE_EQ(runtime.processTick().arg0, 225);
    REQUIRE_EQ(runtime.processTick().arg0, 195);
    REQUIRE_EQ(runtime.processTick().arg0, 180);
    REQUIRE_EQ(runtime.phase(), RatePhase::Sustain);
    REQUIRE_EQ(runtime.processTick().arg0, 160);
    runtime.keyOff();
    const auto released = runtime.processTick();
    REQUIRE_EQ(released.arg0, 120);
    REQUIRE_EQ(released.arg1, 7);
    REQUIRE_EQ(runtime.phase(), RatePhase::Release);
}

void testOpllKeyOffDoesNotStartSoftwareRelease() {
    RateEnvelopeRuntime runtime({
        .attack_level = 255,
        .attack_rate = 0,
        .decay_rate = 0,
        .sustain_level = 255,
        .sustain_rate = 10,
        .release_rate = 20,
    });
    REQUIRE_EQ(runtime.processTick().arg0, 255);
    REQUIRE_EQ(runtime.processTick().arg0, 255);
    REQUIRE_EQ(runtime.phase(), RatePhase::Sustain);
    runtime.keyOff(false);
    REQUIRE_EQ(runtime.processTick().arg0, 245);
    REQUIRE_EQ(runtime.phase(), RatePhase::Sustain);
}

void testVolumeCombination() {
    using mgstc::engine::sequenceOutputVolume;
    REQUIRE_EQ(sequenceOutputVolume(15, 15), 15);
    REQUIRE_EQ(sequenceOutputVolume(12, 10), 7);
    REQUIRE_EQ(sequenceOutputVolume(4, 8), 0);
    REQUIRE_EQ(sequenceOutputVolume(15, 12, 2, 1), 9);
}

void testChipSpecificVolumeMapping() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(16);

    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            0,
            {0, MeaningEventKind::Volume, 12, 0},
            7,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            3,
            {0, MeaningEventKind::Volume, 11, 0},
            7,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            8,
            {0, MeaningEventKind::Volume, 10, 0},
            7,
            output),
        MapError::None);

    REQUIRE_EQ(
        std::vector<RegisterWrite>(
            output.writes().begin(),
            output.writes().end()),
        std::vector<RegisterWrite>({
            {7, 0, ChipId::Psg, 0, 8, 12, 0, WriteReason::Volume},
            {7, 1, ChipId::Scc, 2, 0, 11, 3, WriteReason::Volume},
            {7, 2, ChipId::Opll, 0, 0x30, 5, 8, WriteReason::Volume},
        }));
}

void testMgsdrvNoteTables() {
    NotePitch pitch{};
    REQUIRE_EQ(notePitch(60, pitch), true);
    REQUIRE_EQ(pitch.psg_scc_period, static_cast<std::uint16_t>(0x01AB));
    REQUIRE_EQ(pitch.opll.f_number, static_cast<std::uint16_t>(0x00AC));
    REQUIRE_EQ(pitch.opll.block, static_cast<std::uint8_t>(3));

    REQUIRE_EQ(notePitch(61, pitch), true);
    REQUIRE_EQ(pitch.psg_scc_period, static_cast<std::uint16_t>(0x0193));
    REQUIRE_EQ(pitch.opll.f_number, static_cast<std::uint16_t>(0x00B6));

    REQUIRE_EQ(notePitch(23, pitch), false);
    REQUIRE_EQ(notePitch(120, pitch), false);
}

void testRuntimePsgNoteOnOrderMatchesObservedBoundary() {
    RuntimeSession session(32, 32);
    REQUIRE_EQ(session.setSequenceEnvelope(0, {0xA3, 0x87, 0x12, 0x03, 0x0F}), true);
    REQUIRE_EQ(session.setPsgToneNoise(0, 2, 5), true);
    REQUIRE_EQ(session.queueNoteOn(0, 60), true);
    REQUIRE_EQ(session.processTick().ok(), true);

    const auto writes = session.writes();
    REQUIRE_EQ(writes.size(), static_cast<std::size_t>(9));
    const std::vector<std::pair<int, int>> actual{
        {writes[0].address, writes[0].value},
        {writes[1].address, writes[1].value},
        {writes[2].address, writes[2].value},
        {writes[3].address, writes[3].value},
        {writes[4].address, writes[4].value},
        {writes[5].address, writes[5].value},
        {writes[6].address, writes[6].value},
        {writes[7].address, writes[7].value},
        {writes[8].address, writes[8].value},
    };
    REQUIRE_EQ(
        actual,
        std::vector<std::pair<int, int>>({
            {7, 0xB1},
            {6, 5},
            {0, 0xAB},
            {1, 0x01},
            {7, 0xB0},
            {6, 7},
            {0, 0xA8},
            {1, 0x01},
            {8, 15},
        }));
}

void testRuntimeBoundaryWritesMatchVgmFixture() {
    const auto fixture = std::filesystem::path(MGSTC_SOURCE_DIR)
        / "tests"
        / "fixtures"
        / "mgsdrv_selftest"
        / "TEST_BOUNDARY.VGM";
    const auto golden = ayWritesInFrame(fixture, 1);

    RuntimeSession session(32, 32);
    REQUIRE_EQ(
        session.setSequenceEnvelope(
            0,
            {0xA3, 0x87, 0x12, 0x03, 0xEF, 0x02,
             0x20, 0x04, 0xE8, 0x02, 0x00}),
        true);
    REQUIRE_EQ(session.setPsgToneNoise(0, 2, 5), true);
    REQUIRE_EQ(session.queueNoteOn(0, 60), true);
    REQUIRE_EQ(session.processTick().ok(), true);

    std::vector<std::pair<int, int>> actual;
    for (const auto& write : session.writes()) {
        if (write.chip == ChipId::Psg) {
            actual.emplace_back(write.address, write.value);
        }
    }
    REQUIRE_EQ(actual, golden);
}

void testRuntimeSccKeyMaskPreservesOtherChannels() {
    RuntimeSession session(16, 32);
    REQUIRE_EQ(session.queueNoteOn(3, 60), true);
    REQUIRE_EQ(session.queueNoteOn(4, 64), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes()[2].port, static_cast<std::uint8_t>(3));
    REQUIRE_EQ(session.writes()[2].value, static_cast<std::uint8_t>(0x01));
    REQUIRE_EQ(session.writes()[5].port, static_cast<std::uint8_t>(3));
    REQUIRE_EQ(session.writes()[5].value, static_cast<std::uint8_t>(0x03));

    REQUIRE_EQ(session.queueKeyOff(3), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes()[0].port, static_cast<std::uint8_t>(3));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(0x02));
    REQUIRE_EQ(session.writes()[0].reason, WriteReason::KeyOff);
}

void testRuntimePsgSequenceKeyOffStaysSilent() {
    RuntimeSession session(16, 32);
    REQUIRE_EQ(
        session.setSequenceEnvelope(0, {0x40, 0xEF, 0x01, 0x60}),
        true);
    REQUIRE_EQ(session.queueNoteOn(0, 60), true);
    REQUIRE_EQ(session.processTick().ok(), true);

    REQUIRE_EQ(session.queueKeyOff(0), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(session.writes()[0].address, static_cast<std::uint8_t>(8));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(session.writes()[0].reason, WriteReason::Volume);

    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().empty(), true);
}

void testAuditionGateSuppressesEnvelopeUntilNoteOn() {
    RuntimeSession session(16, 32);
    REQUIRE_EQ(
        session.setSequenceEnvelope(0, {0x40, 0xEF, 0x01, 0x60}),
        true);
    session.gateUntilNoteOn();

    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().empty(), true);

    REQUIRE_EQ(session.queueNoteOn(0, 60), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().empty(), false);
    REQUIRE_EQ(session.writes().back().address, static_cast<std::uint8_t>(8));
    REQUIRE_EQ(session.writes().back().value, static_cast<std::uint8_t>(15));
}

void testRuntimeOpllKeyOnAndOffRegisters() {
    RuntimeSession session(16, 16);
    REQUIRE_EQ(session.queueNoteOn(8, 60), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(session.writes()[0].address, static_cast<std::uint8_t>(0x20));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(0x16));
    REQUIRE_EQ(session.writes()[0].reason, WriteReason::KeyOn);
    REQUIRE_EQ(session.writes()[1].address, static_cast<std::uint8_t>(0x10));
    REQUIRE_EQ(session.writes()[1].value, static_cast<std::uint8_t>(0xAC));

    REQUIRE_EQ(session.queueKeyOff(8), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(session.writes()[0].address, static_cast<std::uint8_t>(0x20));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(0x06));
    REQUIRE_EQ(session.writes()[0].reason, WriteReason::KeyOff);
}

void testChipRackRendersAllThreeChips() {
    ChipRack rack;
    REQUIRE_EQ(rack.valid(), true);

    std::vector<RegisterWrite> setup{
        {0, 0, ChipId::Psg, 0, 0, 0xAB, 0, WriteReason::Frequency},
        {0, 1, ChipId::Psg, 0, 1, 0x01, 0, WriteReason::Frequency},
        {0, 2, ChipId::Psg, 0, 7, 0xBE, 0, WriteReason::Patch},
        {0, 3, ChipId::Psg, 0, 8, 0x0F, 0, WriteReason::Volume},
    };
    std::uint32_t sequence = 4;
    for (std::uint8_t index = 0; index < 32; ++index) {
        setup.push_back({
            0,
            sequence++,
            ChipId::Scc,
            0,
            index,
            static_cast<std::uint8_t>(index < 16 ? 0x7F : 0x80),
            3,
            WriteReason::Patch,
        });
    }
    setup.push_back(
        {0, sequence++, ChipId::Scc, 1, 0, 0xAB, 3, WriteReason::Frequency});
    setup.push_back(
        {0, sequence++, ChipId::Scc, 1, 1, 0x01, 3, WriteReason::Frequency});
    setup.push_back(
        {0, sequence++, ChipId::Scc, 2, 0, 0x0F, 3, WriteReason::Volume});
    setup.push_back(
        {0, sequence++, ChipId::Scc, 3, 0, 0x01, 3, WriteReason::KeyOn});
    setup.push_back(
        {0, sequence++, ChipId::Opll, 0, 0x30, 0x10, 8, WriteReason::Patch});
    setup.push_back(
        {0, sequence++, ChipId::Opll, 0, 0x20, 0x16, 8, WriteReason::KeyOn});
    setup.push_back(
        {0, sequence++, ChipId::Opll, 0, 0x10, 0xAC, 8, WriteReason::Frequency});

    REQUIRE_EQ(rack.apply(setup), true);
    float psg_peak = 0.0F;
    float scc_peak = 0.0F;
    float opll_peak = 0.0F;
    for (int sample = 0; sample < 8'192; ++sample) {
        const auto rendered = rack.renderSample();
        psg_peak = std::max(psg_peak, std::abs(rendered.psg));
        scc_peak = std::max(scc_peak, std::abs(rendered.scc));
        opll_peak = std::max(opll_peak, std::abs(rendered.opll));
    }
    REQUIRE_EQ(psg_peak > 0.001F, true);
    REQUIRE_EQ(scc_peak > 0.001F, true);
    REQUIRE_EQ(opll_peak > 0.001F, true);
}

void testEngineCoreSplitsAtEightHundredFrameBoundary() {
    EngineCore engine;
    REQUIRE_EQ(engine.valid(), true);
    REQUIRE_EQ(engine.session().setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(engine.session().queueNoteOn(0, 60), true);

    std::vector<float> output(801 * 2);
    const auto result = engine.render(output);
    REQUIRE_EQ(result.ok(), true);
    REQUIRE_EQ(result.frames, static_cast<std::size_t>(801));
    REQUIRE_EQ(engine.session().tick(), static_cast<std::uint64_t>(2));
    REQUIRE_EQ(engine.clock().currentTick(), static_cast<std::uint64_t>(1));
    REQUIRE_EQ(engine.clock().framesUntilNextTick(), 799U);

    float peak = 0.0F;
    for (std::size_t frame = 0; frame < 801; ++frame) {
        REQUIRE_EQ(output[frame * 2], output[frame * 2 + 1]);
        peak = std::max(peak, std::abs(output[frame * 2]));
    }
    REQUIRE_EQ(peak > 0.001F, true);
}

void testEngineCoreValidatesAndClampsOutput() {
    EngineCore engine;
    REQUIRE_EQ(engine.gains().master, 3.0F);
    REQUIRE_EQ(engine.gains().psg, 1.0F);
    REQUIRE_EQ(engine.gains().scc, 1.0F);
    REQUIRE_EQ(engine.gains().opll, 1.0F);
    REQUIRE_EQ(
        engine.setGains({-1.0F, 1.0F, 1.0F, 1.0F}),
        false);
    REQUIRE_EQ(
        engine.setGains(MixerGains{
            .master = 100.0F,
            .psg = 1.0F,
            .scc = 0.0F,
            .opll = 0.0F,
        }),
        true);
    REQUIRE_EQ(engine.session().setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(engine.session().queueNoteOn(0, 60), true);

    std::vector<float> output(4'096 * 2);
    const auto result = engine.render(output);
    REQUIRE_EQ(result.ok(), true);
    REQUIRE_EQ(result.clipped, true);
    REQUIRE_EQ(
        std::all_of(
            output.begin(),
            output.end(),
            [](float sample) {
                return sample >= -1.0F && sample <= 1.0F;
            }),
        true);

    std::array<float, 3> invalid{1.0F, 1.0F, 1.0F};
    const auto invalid_result = engine.render(invalid);
    REQUIRE_EQ(invalid_result.error, RenderError::InvalidBuffer);
    REQUIRE_EQ(invalid, std::array<float, 3>{0.0F, 0.0F, 0.0F});
}

void testEngineCorePublishesOneSixtiethOpllScope() {
    EngineCore engine;
    REQUIRE_EQ(
        engine.session().setSequenceEnvelope(
            8,
            {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60}),
        true);
    REQUIRE_EQ(engine.session().queueNoteOn(8, 60), true);
    std::vector<float> output(800 * 2);
    REQUIRE_EQ(engine.render(output).ok(), true);

    OpllScopeFrame scope{};
    REQUIRE_EQ(engine.takeOpllScopeFrame(scope), true);
    REQUIRE_EQ(scope.sequence, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(
        std::any_of(
            scope.samples.begin(),
            scope.samples.end(),
            [](float sample) {
                return std::abs(sample) > 0.00001F;
            }),
        true);
    REQUIRE_EQ(
        std::any_of(
            scope.mixed_samples.begin(),
            scope.mixed_samples.end(),
            [](float sample) {
                return std::abs(sample) > 0.00001F;
            }),
        true);
    REQUIRE_EQ(engine.takeOpllScopeFrame(scope), false);
}

void testSccPresetGenerationIncludesDocumentedHarmonics() {
    const auto one = generateSccPreset(
        SccWavePreset::Sine,
        SccHarmonic::One);
    const auto one_and_half = generateSccPreset(
        SccWavePreset::Sine,
        SccHarmonic::OneAndHalf);
    const auto two = generateSccPreset(
        SccWavePreset::Sine,
        SccHarmonic::Two);
    const auto three = generateSccPreset(
        SccWavePreset::Sine,
        SccHarmonic::Three);
    const auto four = generateSccPreset(
        SccWavePreset::Sine,
        SccHarmonic::Four);
    const auto pulse_25 = generateSccPreset(
        SccWavePreset::Pulse25,
        SccHarmonic::One);
    const auto pulse_12_5 = generateSccPreset(
        SccWavePreset::Pulse12_5,
        SccHarmonic::One);

    REQUIRE_EQ(one[0], static_cast<std::int8_t>(0));
    REQUIRE_EQ(two[0], static_cast<std::int8_t>(0));
    REQUIRE_EQ(two[8], static_cast<std::int8_t>(0));
    REQUIRE_EQ(one_and_half == one, false);
    REQUIRE_EQ(one_and_half == two, false);
    REQUIRE_EQ(three == two, false);
    REQUIRE_EQ(four == three, false);
    REQUIRE_EQ(pulse_25 == pulse_12_5, false);
    REQUIRE_EQ(static_cast<int>(pulse_25[0]) > 0, true);
    REQUIRE_EQ(static_cast<int>(pulse_12_5[4]) < 0, true);
    REQUIRE_EQ(
        *std::max_element(
            one_and_half.begin(),
            one_and_half.end()),
        static_cast<std::int8_t>(127));

    constexpr std::array presets{
        SccWavePreset::Sine,
        SccWavePreset::Square,
        SccWavePreset::Triangle,
        SccWavePreset::SawUp,
        SccWavePreset::SawDown,
        SccWavePreset::Pulse25,
        SccWavePreset::Pulse12_5,
    };
    constexpr std::array harmonics{
        SccHarmonic::One,
        SccHarmonic::OneAndHalf,
        SccHarmonic::Two,
        SccHarmonic::Three,
        SccHarmonic::Four,
    };
    for (const auto preset : presets) {
        for (const auto harmonic : harmonics) {
            const auto waveform = generateSccPreset(preset, harmonic);
            REQUIRE_EQ(
                *std::min_element(waveform.begin(), waveform.end()),
                static_cast<std::int8_t>(-128));
            REQUIRE_EQ(
                *std::max_element(waveform.begin(), waveform.end()),
                static_cast<std::int8_t>(127));
        }
    }
}

void testSccAverageUsesCircularThreeSampleWindow() {
    SccWaveform impulse{};
    impulse[0] = static_cast<std::int8_t>(90);
    const auto averaged = averageSccWaveform(impulse);
    REQUIRE_EQ(averaged[0], static_cast<std::int8_t>(30));
    REQUIRE_EQ(averaged[1], static_cast<std::int8_t>(30));
    REQUIRE_EQ(averaged[31], static_cast<std::int8_t>(30));
    REQUIRE_EQ(averaged[2], static_cast<std::int8_t>(0));
}

void testSccMergeFindsCircularPhaseAndPreservesLevel() {
    const auto current = generateSccPreset(
        SccWavePreset::Triangle,
        SccHarmonic::One);
    const auto rotated = rotateSccWaveform(current, 5);
    const auto result = mergeSccWaveforms(
        current,
        rotated,
        SccMergeOptions{
            .amount = 0.5,
            .auto_phase = true,
            .allow_polarity_inversion = true,
            .preserve_volume = true,
        });
    REQUIRE_EQ(result.circular_shift, static_cast<std::size_t>(5));
    REQUIRE_EQ(result.polarity_inverted, false);
    REQUIRE_EQ(result.waveform, current);
}

void testSccWaveformUtilityTransforms() {
    SccWaveform waveform{};
    waveform[0] = static_cast<std::int8_t>(-128);
    waveform[1] = static_cast<std::int8_t>(64);
    const auto inverted = invertSccWaveform(waveform);
    REQUIRE_EQ(inverted[0], static_cast<std::int8_t>(127));
    REQUIRE_EQ(inverted[1], static_cast<std::int8_t>(-64));

    const auto rotated = rotateSccWaveform(waveform, 1);
    REQUIRE_EQ(rotated[1], waveform[0]);
    REQUIRE_EQ(rotated[2], waveform[1]);

    const auto shifted_up = shiftSccWaveformVertically(waveform, 1);
    REQUIRE_EQ(shifted_up[0], static_cast<std::int8_t>(-127));
    REQUIRE_EQ(shifted_up[1], static_cast<std::int8_t>(65));

    auto near_ceiling = waveform;
    near_ceiling[4] = 127;
    const auto ceiling_limited =
        shiftSccWaveformVertically(near_ceiling, 1);
    REQUIRE_EQ(ceiling_limited, near_ceiling);

    const auto floor_limited =
        shiftSccWaveformVertically(waveform, -1);
    REQUIRE_EQ(floor_limited, waveform);

    const auto compressed =
        scaleSccWaveformVertically(waveform, 50);
    REQUIRE_EQ(compressed[0], static_cast<std::int8_t>(-64));
    REQUIRE_EQ(compressed[1], static_cast<std::int8_t>(32));
    const auto expanded =
        scaleSccWaveformVertically(waveform, 200);
    REQUIRE_EQ(expanded[0], static_cast<std::int8_t>(-128));
    REQUIRE_EQ(expanded[1], static_cast<std::int8_t>(127));

    SccWaveform candidate{};
    candidate.fill(42);
    const auto left = applySccWaveformRange(
        waveform,
        candidate,
        SccApplyRange::LeftHalf);
    const auto right = applySccWaveformRange(
        waveform,
        candidate,
        SccApplyRange::RightHalf);
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        REQUIRE_EQ(
            left[index],
            index < 16 ? candidate[index] : waveform[index]);
        REQUIRE_EQ(
            right[index],
            index < 16 ? waveform[index] : candidate[index]);
    }

    const auto normalized = normalizeSccWaveform(waveform);
    REQUIRE_EQ(
        *std::min_element(normalized.begin(), normalized.end()),
        static_cast<std::int8_t>(-128));
    REQUIRE_EQ(
        *std::max_element(normalized.begin(), normalized.end()),
        static_cast<std::int8_t>(127));

    SccWaveform positive_offset{};
    positive_offset.fill(40);
    positive_offset[0] = 10;
    positive_offset[1] = 80;
    const auto offset_normalized =
        normalizeSccWaveform(positive_offset);
    REQUIRE_EQ(
        *std::min_element(
            offset_normalized.begin(), offset_normalized.end()),
        static_cast<std::int8_t>(-128));
    REQUIRE_EQ(
        *std::max_element(
            offset_normalized.begin(), offset_normalized.end()),
        static_cast<std::int8_t>(127));

    SccWaveform constant{};
    constant.fill(42);
    REQUIRE_EQ(normalizeSccWaveform(constant), SccWaveform{});
}

void testRuntimeUsesMgsTrackOrder() {
    RuntimeSession session(16, 16);
    REQUIRE_EQ(session.setSequenceEnvelope(8, {0x0D}), true);
    REQUIRE_EQ(session.setSequenceEnvelope(3, {0x0E}), true);
    REQUIRE_EQ(session.setSequenceEnvelope(0, {0x0F}), true);

    const auto result = session.processTick();
    REQUIRE_EQ(result.ok(), true);
    REQUIRE_EQ(
        std::vector<RegisterWrite>(
            session.writes().begin(),
            session.writes().end()),
        std::vector<RegisterWrite>({
            {0, 0, ChipId::Psg, 0, 8, 15, 0, WriteReason::Volume},
            {0, 1, ChipId::Scc, 2, 0, 14, 3, WriteReason::Volume},
            {0, 2, ChipId::Opll, 0, 0x30, 2, 8, WriteReason::Volume},
        }));
}

void testPsgToneNoiseAndFrequencyMapping() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(16);

    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            0,
            {0, MeaningEventKind::RegisterWrite, 0, 0xAB},
            0,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            0,
            {0, MeaningEventKind::RegisterWrite, 1, 0x01},
            0,
            output),
        MapError::None);
    output.clear();
    mapper.beginTick();

    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            0,
            {0, MeaningEventKind::ToneNoiseMode, 3, 0},
            1,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            0,
            {0, MeaningEventKind::FrequencyDelta, 3, 0},
            1,
            output),
        MapError::None);

    REQUIRE_EQ(output.writes()[0].address, static_cast<std::uint8_t>(7));
    REQUIRE_EQ(output.writes()[0].value, static_cast<std::uint8_t>(0xB0));
    REQUIRE_EQ(output.writes()[1].address, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(output.writes()[1].value, static_cast<std::uint8_t>(0xA8));
    REQUIRE_EQ(output.writes()[2].address, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(output.writes()[2].value, static_cast<std::uint8_t>(0x01));
}

void testSccWaveUsesSharedChannelFourFiveRam() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(64);
    std::array<std::uint8_t, 32> wave{};
    for (std::size_t index = 0; index < wave.size(); ++index) {
        wave[index] = static_cast<std::uint8_t>(index * 3);
    }

    REQUIRE_EQ(
        mapper.writeSccWave(6, wave, 3, output),
        MapError::None);
    REQUIRE_EQ(output.size(), static_cast<std::size_t>(32));
    REQUIRE_EQ(output.writes().front().address, static_cast<std::uint8_t>(96));
    REQUIRE_EQ(output.writes().back().address, static_cast<std::uint8_t>(127));
    REQUIRE_EQ(output.writes().back().value, wave.back());

    output.clear();
    mapper.beginTick();
    REQUIRE_EQ(
        mapper.writeSccWave(7, wave, 4, output),
        MapError::None);
    REQUIRE_EQ(output.writes().front().address, static_cast<std::uint8_t>(96));
}

void testOpllOriginalPatchThenYThenVolume() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(16);
    const std::array<std::uint8_t, 8> patch{
        0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26,
    };
    REQUIRE_EQ(
        mapper.defineOpllOriginalPatch(17, patch),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            8,
            {0, MeaningEventKind::Patch, 17, 0},
            5,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            8,
            {0, MeaningEventKind::RegisterWrite, 2, 0x14},
            5,
            output),
        MapError::None);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            8,
            {0, MeaningEventKind::Volume, 12, 0},
            5,
            output),
        MapError::None);

    REQUIRE_EQ(output.size(), static_cast<std::size_t>(11));
    for (std::size_t index = 0; index < patch.size(); ++index) {
        REQUIRE_EQ(output.writes()[index].address, index);
        REQUIRE_EQ(output.writes()[index].value, patch[index]);
    }
    REQUIRE_EQ(output.writes()[8].address, static_cast<std::uint8_t>(0x30));
    REQUIRE_EQ(output.writes()[8].value, static_cast<std::uint8_t>(0x0F));
    REQUIRE_EQ(output.writes()[9].address, static_cast<std::uint8_t>(2));
    REQUIRE_EQ(output.writes()[9].value, static_cast<std::uint8_t>(0x14));
    REQUIRE_EQ(output.writes()[10].address, static_cast<std::uint8_t>(0x30));
    REQUIRE_EQ(output.writes()[10].value, static_cast<std::uint8_t>(0x03));
}

void testSccFrequencyDeltaAndIgnoredY() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(8);
    mapper.setSccPeriod(0, 0x01AB);

    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            3,
            {0, MeaningEventKind::RegisterWrite, 2, 0x55},
            0,
            output),
        MapError::None);
    REQUIRE_EQ(output.size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(
        mapper.mapMeaningEvent(
            3,
            {0, MeaningEventKind::FrequencyDelta, 3, 0},
            0,
            output),
        MapError::None);
    REQUIRE_EQ(output.writes()[0].port, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(output.writes()[0].value, static_cast<std::uint8_t>(0xA8));
    REQUIRE_EQ(output.writes()[1].value, static_cast<std::uint8_t>(0x01));
}

void testRegisterWriteBufferPreservesDuplicatesAndStopsAtCapacity() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(2);
    const MeaningEvent volume{0, MeaningEventKind::Volume, 9, 0};

    REQUIRE_EQ(mapper.mapMeaningEvent(0, volume, 0, output), MapError::None);
    REQUIRE_EQ(mapper.mapMeaningEvent(0, volume, 0, output), MapError::None);
    REQUIRE_EQ(output.writes()[0].value, output.writes()[1].value);
    REQUIRE_EQ(
        mapper.mapMeaningEvent(0, volume, 0, output),
        MapError::BufferOverflow);
    REQUIRE_EQ(output.size(), static_cast<std::size_t>(2));
}

void testPsgHardwareEnvelopeSharedState() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(16);
    PsgHardwareEnvelopeState shared;
    PsgTrackEnvelopeMode track;

    REQUIRE_EQ(
        shared.setPeriod(0, 0x1234, 2, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.writes()[0].address, static_cast<std::uint8_t>(11));
    REQUIRE_EQ(output.writes()[0].value, static_cast<std::uint8_t>(0x34));
    REQUIRE_EQ(output.writes()[1].address, static_cast<std::uint8_t>(12));
    REQUIRE_EQ(output.writes()[1].value, static_cast<std::uint8_t>(0x12));
    REQUIRE_EQ(
        output.writes()[0].reason,
        WriteReason::HardwareEnvelope);

    REQUIRE_EQ(track.selectHardware(shared, 10), true);
    REQUIRE_EQ(track.hardwareEnabled(), true);
    REQUIRE_EQ(track.trackVolume(), static_cast<std::uint8_t>(15));
    REQUIRE_EQ(
        track.keyOn(0, shared, 0, 0, true, 3, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.writes()[2].address, static_cast<std::uint8_t>(13));
    REQUIRE_EQ(output.writes()[2].value, static_cast<std::uint8_t>(10));
    REQUIRE_EQ(output.writes()[3].address, static_cast<std::uint8_t>(8));
    REQUIRE_EQ(output.writes()[3].value, static_cast<std::uint8_t>(0x10));

    track.setFixedVolume(7);
    REQUIRE_EQ(track.hardwareEnabled(), false);
    const auto before = output.size();
    REQUIRE_EQ(
        track.keyOn(0, shared, 0, 0, true, 4, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.size(), before);
}

void testPsgHardwareShapeRestartSuppression() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(8);
    PsgHardwareEnvelopeState shared;
    PsgTrackEnvelopeMode track;
    REQUIRE_EQ(track.selectHardware(shared, 12), true);

    REQUIRE_EQ(
        track.keyOn(1, shared, 3, 5, true, 0, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(output.writes()[0].address, static_cast<std::uint8_t>(9));
    REQUIRE_EQ(output.writes()[0].value, static_cast<std::uint8_t>(0x10));

    output.clear();
    REQUIRE_EQ(
        track.keyOn(1, shared, 0, 0, false, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.size(), static_cast<std::size_t>(0));
}

void testNineVoiceRhythmSharedRegisters() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(16);
    NineVoiceRhythmState rhythm;

    REQUIRE_EQ(
        rhythm.enterRhythmMode(0, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.writes()[0].address, static_cast<std::uint8_t>(0x0E));
    REQUIRE_EQ(output.writes()[0].value, static_cast<std::uint8_t>(0x20));

    REQUIRE_EQ(
        rhythm.applyLowerVolume(8, 3, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(
        rhythm.applyPatchVolume(8, 4, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(rhythm.volumeRegister(0x37), static_cast<std::uint8_t>(0x53));
    REQUIRE_EQ(output.writes()[2].address, static_cast<std::uint8_t>(0x37));
    REQUIRE_EQ(output.writes()[2].value, static_cast<std::uint8_t>(0x53));

    REQUIRE_EQ(
        rhythm.applyLowerVolume(9, 5, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(
        rhythm.applyPatchVolume(9, 7, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(rhythm.volumeRegister(0x38), static_cast<std::uint8_t>(0x85));

    const auto before = output.size();
    REQUIRE_EQ(
        rhythm.applyPatchVolume(8, 15, 1, mapper, output),
        MapError::InvalidValue);
    REQUIRE_EQ(output.size(), before);

    REQUIRE_EQ(
        rhythm.silencePatchControlledDrum(8, 2, mapper, output),
        MapError::None);
    REQUIRE_EQ(rhythm.volumeRegister(0x37), static_cast<std::uint8_t>(0xF3));
}

void testNineVoiceRhythmRetriggerWritesOffThenOn() {
    RegisterMapper mapper;
    RegisterWriteBuffer output(8);
    NineVoiceRhythmState rhythm;
    REQUIRE_EQ(
        rhythm.enterRhythmMode(0, mapper, output),
        MapError::None);
    REQUIRE_EQ(
        rhythm.trigger(0x01, 14, 1, mapper, output),
        MapError::None);
    REQUIRE_EQ(output.writes()[1].value, static_cast<std::uint8_t>(0x20));
    REQUIRE_EQ(output.writes()[2].value, static_cast<std::uint8_t>(0x21));
    REQUIRE_EQ(rhythm.rhythmRegister(), static_cast<std::uint8_t>(0x21));
}

void testRuntimeAppliesTrackAndCommonAttenuation() {
    RuntimeSession session(16, 16);
    REQUIRE_EQ(session.setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(session.setTrackVolume(0, 12), true);
    REQUIRE_EQ(session.setMasterAttenuation(2), true);
    REQUIRE_EQ(session.setTrackAttenuation(0, 1), true);

    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(9));
}

void testRuntimeRetriggerRestartsSequenceOnSameTick() {
    RuntimeSession session(16, 16);
    REQUIRE_EQ(session.setSequenceEnvelope(3, {0x0F, 0x08}), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(15));
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(8));

    REQUIRE_EQ(session.queueKeyOn(3), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes()[3].value, static_cast<std::uint8_t>(15));
}

void testRuntimePsgHardwareEnvelopeOverridesSoftwareVolume() {
    RuntimeSession session(16, 16);
    REQUIRE_EQ(session.setSequenceEnvelope(0, {0x0F}), true);
    REQUIRE_EQ(session.selectPsgHardwareEnvelope(0, 10), true);
    REQUIRE_EQ(session.queuePsgHardwareEnvelopePeriod(0, 0x1234), true);
    REQUIRE_EQ(session.queueKeyOn(0), true);

    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(8));
    REQUIRE_EQ(session.writes()[0].address, static_cast<std::uint8_t>(11));
    REQUIRE_EQ(session.writes()[1].address, static_cast<std::uint8_t>(12));
    REQUIRE_EQ(session.writes()[2].address, static_cast<std::uint8_t>(7));
    REQUIRE_EQ(session.writes()[3].address, static_cast<std::uint8_t>(6));
    REQUIRE_EQ(session.writes()[4].address, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(session.writes()[5].address, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(session.writes()[6].address, static_cast<std::uint8_t>(13));
    REQUIRE_EQ(session.writes()[7].address, static_cast<std::uint8_t>(8));
    REQUIRE_EQ(session.writes()[7].value, static_cast<std::uint8_t>(0x10));

    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(session.setPsgFixedVolume(0, 15), true);
    REQUIRE_EQ(session.processTick().ok(), true);
    REQUIRE_EQ(session.writes().size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(session.writes()[0].value, static_cast<std::uint8_t>(15));
}

void testSpscQueuePreservesFifoAcrossWrap() {
    SpscQueue<std::uint32_t, 3> queue;
    REQUIRE_EQ(queue.capacity(), static_cast<std::size_t>(3));
    REQUIRE_EQ(queue.approximateSize(), static_cast<std::size_t>(0));
    REQUIRE_EQ(queue.tryPush(10), true);
    REQUIRE_EQ(queue.tryPush(20), true);
    REQUIRE_EQ(queue.tryPush(30), true);
    REQUIRE_EQ(queue.tryPush(40), false);

    std::uint32_t value{};
    REQUIRE_EQ(queue.tryPop(value), true);
    REQUIRE_EQ(value, static_cast<std::uint32_t>(10));
    REQUIRE_EQ(queue.tryPush(40), true);
    REQUIRE_EQ(queue.approximateSize(), static_cast<std::size_t>(3));
    REQUIRE_EQ(queue.tryPop(value), true);
    REQUIRE_EQ(value, static_cast<std::uint32_t>(20));
    REQUIRE_EQ(queue.tryPop(value), true);
    REQUIRE_EQ(value, static_cast<std::uint32_t>(30));
    REQUIRE_EQ(queue.tryPop(value), true);
    REQUIRE_EQ(value, static_cast<std::uint32_t>(40));
    REQUIRE_EQ(queue.tryPop(value), false);
}

void testSpscQueueTransfersConcurrently() {
    constexpr std::uint32_t kCount = 20'000;
    SpscQueue<std::uint32_t, 64> queue;
    std::thread producer([&queue]() {
        for (std::uint32_t value = 0; value < kCount; ++value) {
            while (!queue.tryPush(value)) {
                std::this_thread::yield();
            }
        }
    });

    bool ordered = true;
    for (std::uint32_t expected = 0; expected < kCount; ++expected) {
        std::uint32_t actual{};
        while (!queue.tryPop(actual)) {
            std::this_thread::yield();
        }
        ordered = ordered && actual == expected;
    }
    producer.join();
    REQUIRE_EQ(ordered, true);
    REQUIRE_EQ(queue.approximateSize(), static_cast<std::size_t>(0));
}

void testRealtimeHostDrainsCommandsAndReportsRejections() {
    RealtimeEngineHost host;
    REQUIRE_EQ(host.submit(EngineCommand::noteOn(17, 60)), true);
    REQUIRE_EQ(
        host.submit(EngineCommand::setMixerGains({
            .master = -1.0F,
            .psg = 1.0F,
            .scc = 1.0F,
            .opll = 1.0F,
        })),
        true);
    REQUIRE_EQ(host.submit(EngineCommand::stop()), true);
    REQUIRE_EQ(host.pendingCommandCount(), static_cast<std::size_t>(3));

    std::array<float, 2> output{};
    const auto result = host.render(output);
    REQUIRE_EQ(result.ok(), true);
    REQUIRE_EQ(host.pendingCommandCount(), static_cast<std::size_t>(0));

    EngineNotice notice{};
    REQUIRE_EQ(host.pollNotice(notice), true);
    REQUIRE_EQ(notice.type, EngineNoticeType::CommandRejected);
    REQUIRE_EQ(
        notice.command,
        mgstc::engine::EngineCommandType::NoteOn);
    REQUIRE_EQ(host.pollNotice(notice), true);
    REQUIRE_EQ(notice.type, EngineNoticeType::CommandRejected);
    REQUIRE_EQ(
        notice.command,
        mgstc::engine::EngineCommandType::SetMixerGains);
    REQUIRE_EQ(host.pollNotice(notice), false);
}

void testRealtimeHostCommandQueueHasFixedCapacity() {
    RealtimeEngineHost host;
    for (std::size_t index = 0;
         index < RealtimeEngineHost::kCommandCapacity;
         ++index) {
        REQUIRE_EQ(host.submit(EngineCommand::stop()), true);
    }
    REQUIRE_EQ(host.submit(EngineCommand::stop()), false);

    std::array<float, 2> output{};
    REQUIRE_EQ(host.render(output).ok(), true);
    REQUIRE_EQ(host.pendingCommandCount(), static_cast<std::size_t>(0));
    REQUIRE_EQ(host.submit(EngineCommand::stop()), true);
}

void testProgramPoolLimitsEditingAndReusesReleasedSlot() {
    RealtimeEngineHost host;
    auto first = host.beginProgramEdit();
    auto second = host.beginProgramEdit();
    auto unavailable = host.beginProgramEdit();
    REQUIRE_EQ(first.valid(), true);
    REQUIRE_EQ(second.valid(), true);
    REQUIRE_EQ(unavailable.valid(), false);

    const auto released_slot = first.slot;
    const auto released_generation = first.generation;
    REQUIRE_EQ(host.discardProgramEdit(first), true);
    REQUIRE_EQ(first.valid(), false);

    auto reused = host.beginProgramEdit();
    REQUIRE_EQ(reused.valid(), true);
    REQUIRE_EQ(reused.slot, released_slot);
    REQUIRE_EQ(reused.generation > released_generation, true);
    REQUIRE_EQ(host.discardProgramEdit(reused), true);
    REQUIRE_EQ(host.discardProgramEdit(second), true);
}

void testProgramSnapshotActivatesAndRetriggersWithoutAudioAllocation() {
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    REQUIRE_EQ(edit.valid(), true);
    REQUIRE_EQ(
        edit.engine->session().setSequenceEnvelope(0, {0x0F}),
        true);
    REQUIRE_EQ(edit.engine->session().setPsgToneNoise(0, 1, 0), true);
    const auto generation = edit.generation;
    REQUIRE_EQ(
        host.submitProgram(
            edit,
            ProgramAudition{
                .retrigger = true,
                .track = 0,
                .midi_note = 60,
            }),
        true);
    REQUIRE_EQ(edit.valid(), false);

    std::vector<float> output(4'096 * 2);
    const auto result = host.render(output);
    REQUIRE_EQ(result.ok(), true);
    REQUIRE_EQ(
        std::any_of(
            output.begin(),
            output.end(),
            [](float sample) {
                return std::abs(sample) > 0.001F;
            }),
        true);

    EngineNotice notice{};
    REQUIRE_EQ(host.pollNotice(notice), true);
    REQUIRE_EQ(notice.type, EngineNoticeType::ProgramActivated);
    REQUIRE_EQ(notice.generation, generation);
    REQUIRE_EQ(host.pollNotice(notice), false);

    // The previously active slot becomes UI-editable only after activation.
    auto next = host.beginProgramEdit();
    REQUIRE_EQ(next.valid(), true);
    REQUIRE_EQ(host.discardProgramEdit(next), true);
}

void testRealtimeHostPublishesOpllScopeToUiQueue() {
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    REQUIRE_EQ(edit.valid(), true);
    REQUIRE_EQ(
        edit.engine->session().setSequenceEnvelope(
            8,
            {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60}),
        true);
    REQUIRE_EQ(
        host.submitProgram(
            edit,
            ProgramAudition{
                .retrigger = true,
                .track = 8,
                .midi_note = 60,
            }),
        true);

    std::vector<float> output(OpllScopeFrame::kSampleCount * 2);
    REQUIRE_EQ(host.render(output).ok(), true);

    OpllScopeFrame scope{};
    REQUIRE_EQ(host.pollOpllScope(scope), true);
    REQUIRE_EQ(scope.sequence, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(
        std::any_of(
            scope.samples.begin(),
            scope.samples.end(),
            [](float sample) {
                return std::abs(sample) > 0.00001F;
            }),
        true);
    REQUIRE_EQ(
        std::any_of(
            scope.mixed_samples.begin(),
            scope.mixed_samples.end(),
            [](float sample) {
                return std::abs(sample) > 0.00001F;
            }),
        true);
    REQUIRE_EQ(host.pollOpllScope(scope), false);
}

void testInvalidAuditionKeepsProgramEditable() {
    RealtimeEngineHost host;
    auto edit = host.beginProgramEdit();
    REQUIRE_EQ(edit.valid(), true);
    REQUIRE_EQ(
        host.submitProgram(
            edit,
            ProgramAudition{
                .retrigger = true,
                .track = 0,
                .midi_note = 1,
            }),
        false);
    REQUIRE_EQ(edit.valid(), true);
    REQUIRE_EQ(host.discardProgramEdit(edit), true);
}

void testOpllPatchSemanticRoundTrip() {
    const std::array<std::uint8_t, 8> registers{
        0xFF, 0xAB, 0xDE, 0xD9, 0x12, 0x34, 0x56, 0x78};
    const auto patch = decodeOpllPatch(registers);

    REQUIRE_EQ(patch.modulator.amplitude_modulation, true);
    REQUIRE_EQ(patch.modulator.pitch_modulation, true);
    REQUIRE_EQ(patch.modulator.sustained_tone, true);
    REQUIRE_EQ(patch.modulator.key_rate_scaling, true);
    REQUIRE_EQ(patch.modulator.multiplier, static_cast<std::uint8_t>(15));
    REQUIRE_EQ(
        patch.modulator.key_scale_level,
        static_cast<std::uint8_t>(3));
    REQUIRE_EQ(patch.modulator.total_level, static_cast<std::uint8_t>(30));
    REQUIRE_EQ(patch.modulator.waveform, true);
    REQUIRE_EQ(patch.modulator.attack_rate, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(patch.modulator.decay_rate, static_cast<std::uint8_t>(2));
    REQUIRE_EQ(patch.modulator.sustain_level, static_cast<std::uint8_t>(5));
    REQUIRE_EQ(patch.modulator.release_rate, static_cast<std::uint8_t>(6));
    REQUIRE_EQ(patch.carrier.waveform, true);
    REQUIRE_EQ(patch.feedback, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(encodeOpllPatch(patch), registers);

    const std::array<std::uint8_t, 8> expected_default{
        0x21, 0x21, 0x00, 0x00, 0xF0, 0xF0, 0x0F, 0x0F};
    REQUIRE_EQ(encodeOpllPatch(defaultOpllPatch()), expected_default);
}

void testYm2413RomPatchesComeFromEmu2413Table() {
    REQUIRE_EQ(ym2413RomPatch(0).has_value(), false);
    REQUIRE_EQ(ym2413RomPatch(16).has_value(), false);

    const auto violin = ym2413RomPatch(1);
    REQUIRE_EQ(violin.has_value(), true);
    REQUIRE_EQ(
        encodeOpllPatch(*violin),
        (std::array<std::uint8_t, 8>{
            0x71, 0x61, 0x1E, 0x17,
            0xD0, 0x78, 0x00, 0x17}));

    const auto electric_guitar = ym2413RomPatch(15);
    REQUIRE_EQ(electric_guitar.has_value(), true);
    REQUIRE_EQ(
        encodeOpllPatch(*electric_guitar),
        (std::array<std::uint8_t, 8>{
            0x41, 0x41, 0x89, 0x03,
            0xF1, 0xE4, 0xC0, 0x13}));
}

void testOpllEnvelopeTraceUsesEmu2413EgAndKeyOff() {
    const auto trace = traceOpllEnvelope(defaultOpllPatch(), 60);
    REQUIRE_EQ(trace.valid, true);
    REQUIRE_EQ(trace.midi_note, static_cast<std::uint8_t>(60));
    const auto key_off_point = static_cast<std::size_t>(
        OpllEnvelopeTrace::kPointCount
        * OpllEnvelopeTrace::kKeyOffSeconds
        / OpllEnvelopeTrace::kDurationSeconds);
    const float before_key_off = *std::max_element(
        trace.carrier.begin(),
        trace.carrier.begin()
            + static_cast<std::ptrdiff_t>(key_off_point));
    REQUIRE_EQ(before_key_off > 0.9F, true);
    REQUIRE_EQ(trace.carrier.back() < 0.001F, true);

    auto no_attack = defaultOpllPatch();
    no_attack.carrier.attack_rate = 0;
    const auto silent_trace = traceOpllEnvelope(no_attack, 60);
    REQUIRE_EQ(silent_trace.valid, true);
    REQUIRE_EQ(
        *std::max_element(
            silent_trace.carrier.begin(),
            silent_trace.carrier.begin()
                + static_cast<std::ptrdiff_t>(key_off_point))
            < 0.001F,
        true);
}

void testMgsOpllDefinitionImportExportRoundTrip() {
    constexpr std::string_view source = R"(
; @v99 = { ignored }
@v17 = {
  10, 2,
  15, 3, 6, 0, 0, 1, 0, 0, 1, 0, 0,
  10,15, 2, 6, 0, 4, 0, 0, 1, 0, 0 ; carrier
}
)";
    const auto imported = parseMgsOpllDefinition(source);
    REQUIRE_EQ(imported.has_value(), true);
    REQUIRE_EQ(imported->number, static_cast<std::uint16_t>(17));
    REQUIRE_EQ(
        encodeOpllPatch(imported->patch),
        (std::array<std::uint8_t, 8>{
            0x21, 0x24, 0x0A, 0x02,
            0xF3, 0xAF, 0x60, 0x26}));

    const auto exported =
        formatMgsOpllDefinition(imported->patch, 31);
    REQUIRE_EQ(exported.find("\r\n") != std::string::npos, true);
    const auto reimported = parseMgsOpllDefinition(exported);
    REQUIRE_EQ(reimported.has_value(), true);
    REQUIRE_EQ(reimported->number, static_cast<std::uint16_t>(31));
    REQUIRE_EQ(reimported->patch, imported->patch);
    REQUIRE_EQ(
        parseMgsOpllDefinition("@v16={64,0}").has_value(),
        false);
    REQUIRE_EQ(
        parseMgsOpllDefinition(
            "@v32={0,0,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,0,0,0,0,0,0,0,0}").has_value(),
        false);
}

void testMgsSccDefinitionImportExportRoundTrip() {
    SccWaveform waveform{};
    for (std::size_t index = 0; index < waveform.size(); ++index) {
        waveform[index] = static_cast<std::int8_t>(
            static_cast<int>(index) * 8 - 128);
    }
    const auto exported = formatMgsSccDefinition(waveform, 7);
    REQUIRE_EQ(exported.starts_with("@s7 = {\r\n"), true);
    const auto imported = parseMgsSccDefinition(
        std::string("; leading comment\r\n") + exported);
    REQUIRE_EQ(imported.has_value(), true);
    REQUIRE_EQ(imported->number, static_cast<std::uint16_t>(7));
    REQUIRE_EQ(imported->waveform, waveform);
    const auto compact = parseMgsSccDefinition(
        "@s02={"
        "00010203 04050607 08090a0b 0c0d0e0f "
        "10111213 14151617 18191a1b 1c1d1e1f}");
    REQUIRE_EQ(compact.has_value(), true);
    REQUIRE_EQ(compact->waveform[0], static_cast<std::int8_t>(0));
    REQUIRE_EQ(compact->waveform[31], static_cast<std::int8_t>(31));
    REQUIRE_EQ(
        parseMgsSccDefinition("@s0={00 ff}").has_value(),
        false);
    REQUIRE_EQ(
        parseMgsSccDefinition(
            "@s32={00 00 00 00 00 00 00 00 "
            "00 00 00 00 00 00 00 00 "
            "00 00 00 00 00 00 00 00 "
            "00 00 00 00 00 00 00 00}").has_value(),
        false);
}

void testTimbreLibraryCrudAndVersionedRoundTrip() {
    mgstc::engine::TimbreLibrary library;
    mgstc::engine::TimbreLibraryEntry opll;
    opll.category = mgstc::engine::TimbreCategory::Opll;
    opll.name = "\xE3\x83\xAA\xE3\x83\xBC\xE3\x83\x89";
    opll.tags = "bright,favorite";
    opll.memo = "line 1\nline 2\twith tab";
    opll.favorite = true;
    opll.opll_registers = {
        0x21, 0x24, 0x0A, 0x02, 0xF3, 0xAF, 0x60, 0x26};
    const auto opll_id = library.add(opll, 100);
    REQUIRE_EQ(opll_id, static_cast<std::uint64_t>(1));

    mgstc::engine::TimbreLibraryEntry scc;
    scc.category = mgstc::engine::TimbreCategory::Scc;
    scc.name = "SCC Bass";
    for (std::size_t index = 0; index < scc.scc_waveform.size();
         ++index) {
        scc.scc_waveform[index] =
            static_cast<std::uint8_t>(index * 7);
    }
    const auto scc_id = library.add(scc, 101);
    REQUIRE_EQ(scc_id, static_cast<std::uint64_t>(2));
    REQUIRE_EQ(library.entries().size(), static_cast<std::size_t>(2));

    auto replacement = *library.find(opll_id);
    replacement.name = "Updated";
    replacement.category = mgstc::engine::TimbreCategory::Opll;
    REQUIRE_EQ(library.update(opll_id, replacement, 200), true);
    REQUIRE_EQ(library.find(opll_id)->created_unix_seconds, 100);
    REQUIRE_EQ(library.find(opll_id)->updated_unix_seconds, 200);
    REQUIRE_EQ(
        library.find(opll_id)->revision,
        static_cast<std::uint32_t>(2));

    std::string error;
    const auto restored =
        mgstc::engine::TimbreLibrary::deserialize(
            library.serialize(), &error);
    REQUIRE_EQ(restored.has_value(), true);
    REQUIRE_EQ(error.empty(), true);
    REQUIRE_EQ(restored->entries().size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(restored->find(opll_id)->name, std::string("Updated"));
    REQUIRE_EQ(restored->find(opll_id)->memo, opll.memo);
    REQUIRE_EQ(
        restored->find(opll_id)->revision,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        restored->find(opll_id)->opll_registers,
        opll.opll_registers);
    REQUIRE_EQ(
        restored->find(scc_id)->scc_waveform,
        scc.scc_waveform);

    auto mutable_restored = *restored;
    REQUIRE_EQ(mutable_restored.erase(opll_id), true);
    REQUIRE_EQ(mutable_restored.erase(opll_id), false);
    const auto next_id = mutable_restored.add(opll, 300);
    REQUIRE_EQ(next_id, static_cast<std::uint64_t>(3));

    REQUIRE_EQ(
        mgstc::engine::TimbreLibrary::deserialize(
            "MGSTC_TIMBRE_LIBRARY\t3\r\n", &error).has_value(),
        false);
    const auto legacy =
        mgstc::engine::TimbreLibrary::deserialize(
            "MGSTC_TIMBRE_LIBRARY\t1\r\n", &error);
    REQUIRE_EQ(legacy.has_value(), true);
    REQUIRE_EQ(
        mgstc::engine::TimbreLibrary::deserialize(
            "MGSTC_TIMBRE_LIBRARY\t1\r\nO\tbroken\r\n",
            &error).has_value(),
        false);
}

void testTimbreLibrarySelectedExportAndNonDestructiveImport() {
    mgstc::engine::TimbreLibrary source;
    mgstc::engine::TimbreLibraryEntry opll;
    opll.category = mgstc::engine::TimbreCategory::Opll;
    opll.name = "Lead";
    opll.tags = "bright";
    opll.opll_registers = {
        0x71, 0x61, 0x1E, 0x17, 0xD0, 0x78, 0x00, 0x17};
    const auto exported_id = source.add(opll, 10);

    mgstc::engine::TimbreLibraryEntry unselected;
    unselected.category = mgstc::engine::TimbreCategory::Scc;
    unselected.name = "Do not export";
    source.add(unselected, 11);

    const auto selected_text = source.serializeEntry(exported_id);
    REQUIRE_EQ(selected_text.has_value(), true);
    const auto selected_file =
        mgstc::engine::TimbreLibrary::deserialize(*selected_text);
    REQUIRE_EQ(selected_file.has_value(), true);
    REQUIRE_EQ(
        selected_file->entries().size(),
        static_cast<std::size_t>(1));
    REQUIRE_EQ(selected_file->entries()[0].name, std::string("Lead"));
    REQUIRE_EQ(
        source.serializeEntry(9999).has_value(),
        false);

    mgstc::engine::TimbreLibrary destination;
    const auto original_id = destination.add(opll, 20);
    auto second = opll;
    second.name = "Lead(1)";
    destination.add(second, 21);
    mgstc::engine::TimbreLibraryEntry same_name_other_category;
    same_name_other_category.category =
        mgstc::engine::TimbreCategory::Scc;
    same_name_other_category.name = "Lead";
    destination.add(same_name_other_category, 22);

    std::string error;
    const auto imported_ids = destination.importSerialized(
        *selected_text, 30, &error);
    REQUIRE_EQ(imported_ids.has_value(), true);
    REQUIRE_EQ(imported_ids->size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(
        destination.find(original_id)->name,
        std::string("Lead"));
    const auto* imported = destination.find((*imported_ids)[0]);
    REQUIRE_EQ(imported != nullptr, true);
    REQUIRE_EQ(imported->name, std::string("Lead(2)"));
    REQUIRE_EQ(imported->created_unix_seconds, 30);
    REQUIRE_EQ(imported->updated_unix_seconds, 30);
    REQUIRE_EQ(imported->tags, std::string("bright"));
    REQUIRE_EQ(imported->opll_registers, opll.opll_registers);

    const auto size_before_invalid = destination.entries().size();
    REQUIRE_EQ(
        destination.importSerialized("broken", 40, &error).has_value(),
        false);
    REQUIRE_EQ(destination.entries().size(), size_before_invalid);
}

void testWavePcmCycleConvertsToScc() {
    constexpr std::uint32_t sample_rate = 8000;
    constexpr std::uint16_t sample_count = 64;
    std::vector<std::uint8_t> wav(44 + sample_count * 2, 0);
    const auto put16 = [&](std::size_t offset, std::uint16_t value) {
        wav[offset] = static_cast<std::uint8_t>(value);
        wav[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            wav[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8));
        }
    };
    std::copy_n("RIFF", 4, wav.begin());
    put32(4, static_cast<std::uint32_t>(wav.size() - 8));
    std::copy_n("WAVEfmt ", 8, wav.begin() + 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, sample_rate);
    put32(28, sample_rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::copy_n("data", 4, wav.begin() + 36);
    put32(40, sample_count * 2);
    for (std::size_t index = 0; index < sample_count; ++index) {
        const auto sample = static_cast<std::int16_t>(std::lround(
            std::sin(2.0 * std::numbers::pi * index / sample_count)
            * 30000.0));
        put16(44 + index * 2, static_cast<std::uint16_t>(sample));
    }
    mgstc::engine::WavePcm pcm;
    std::string error;
    REQUIRE_EQ(parseWavePcm(wav, pcm, &error), true);
    REQUIRE_EQ(pcm.sample_rate, sample_rate);
    const auto analysis = analyzeWaveCycle(pcm);
    REQUIRE_EQ(analysis.cycle.size(), static_cast<std::size_t>(sample_count));
    const auto scc = waveCycleToScc(analysis.cycle);
    REQUIRE_EQ(std::abs(static_cast<int>(scc[0])) <= 1, true);
    REQUIRE_EQ(static_cast<int>(scc[8]) >= 125, true);
    REQUIRE_EQ(std::abs(static_cast<int>(scc[16])) <= 1, true);
    REQUIRE_EQ(static_cast<int>(scc[24]) <= -125, true);
}

void testWaveCycleProducesValidOpllApproximation() {
    std::array<float, 64> cycle{};
    for (std::size_t index = 0; index < cycle.size(); ++index) {
        cycle[index] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * index / cycle.size())
            + 0.35 * std::sin(
                4.0 * std::numbers::pi * index / cycle.size()));
    }
    const auto patch = approximateWaveCycleWithOpll(cycle);
    REQUIRE_EQ(patch.modulator.multiplier <= 15, true);
    REQUIRE_EQ(patch.carrier.multiplier >= 1, true);
    REQUIRE_EQ(patch.carrier.multiplier <= 15, true);
    REQUIRE_EQ(patch.modulator.total_level <= 63, true);
    REQUIRE_EQ(patch.feedback <= 7, true);
}

void testWavePcmProducesDeterministicTimedOpllApproximation() {
    mgstc::engine::WavePcm pcm;
    pcm.sample_rate = 8000;
    pcm.mono_samples.resize(800);
    for (std::size_t index = 0; index < pcm.mono_samples.size(); ++index) {
        const float time = static_cast<float>(index) / pcm.sample_rate;
        const float attack = std::min(1.0F, time / 0.012F);
        const float decay = std::exp(-time * 7.0F);
        pcm.mono_samples[index] =
            attack * decay
            * static_cast<float>(
                std::sin(2.0 * std::numbers::pi * 200.0 * time)
                + 0.28 * std::sin(
                    2.0 * std::numbers::pi * 400.0 * time));
    }
    const auto first_candidates =
        approximateWavePcmCandidatesWithOpll(pcm);
    const auto second_candidates =
        approximateWavePcmCandidatesWithOpll(pcm);
    REQUIRE_EQ(first_candidates.empty(), false);
    REQUIRE_EQ(first_candidates, second_candidates);
    REQUIRE_EQ(first_candidates.size() <= 6, true);
    for (std::size_t first_index = 0;
         first_index < first_candidates.size();
         ++first_index) {
        for (std::size_t second_index = first_index + 1;
             second_index < first_candidates.size();
             ++second_index) {
            REQUIRE_EQ(
                encodeOpllPatch(first_candidates[first_index])
                    == encodeOpllPatch(first_candidates[second_index]),
                false);
        }
    }
    const auto& first = first_candidates.front();
    const auto second = approximateWavePcmWithOpll(
        mgstc::engine::WavePcm{});
    REQUIRE_EQ(second, defaultOpllPatch());
    REQUIRE_EQ(first.modulator.multiplier <= 15, true);
    REQUIRE_EQ(first.carrier.multiplier <= 15, true);
    REQUIRE_EQ(first.modulator.total_level <= 63, true);
    REQUIRE_EQ(first.feedback <= 7, true);
    REQUIRE_EQ(first.modulator.attack_rate <= 15, true);
    REQUIRE_EQ(first.modulator.decay_rate <= 15, true);
    REQUIRE_EQ(first.modulator.sustain_level <= 15, true);
    REQUIRE_EQ(first.modulator.release_rate <= 15, true);
    REQUIRE_EQ(first.carrier.attack_rate <= 15, true);
    REQUIRE_EQ(first.carrier.decay_rate <= 15, true);
    REQUIRE_EQ(first.carrier.sustain_level <= 15, true);
    REQUIRE_EQ(first.carrier.release_rate <= 15, true);
}

void testDefaultCompositeTimbreHasThreeAudibleSources() {
    const auto timbre = mgstc::engine::defaultCompositeTimbre();
    REQUIRE_EQ(timbre.layers.size(), 3U);
    REQUIRE_EQ(
        timbre.layers[0].source,
        mgstc::engine::TimbreSource::Psg);
    REQUIRE_EQ(
        timbre.layers[1].source,
        mgstc::engine::TimbreSource::Scc);
    REQUIRE_EQ(
        timbre.layers[2].source,
        mgstc::engine::TimbreSource::Opll);
    REQUIRE_EQ(
        mgstc::engine::layerIsAudible(timbre, 0), true);
    REQUIRE_EQ(
        mgstc::engine::layerIsAudible(timbre, 1), true);
    REQUIRE_EQ(
        mgstc::engine::layerIsAudible(timbre, 2), true);

    const auto validation =
        mgstc::engine::validateCompositeTimbre(timbre);
    REQUIRE_EQ(validation.valid(), true);
    REQUIRE_EQ(validation.psg_channels, 1);
    REQUIRE_EQ(validation.scc_channels, 1);
    REQUIRE_EQ(validation.opll_channels, 1);
}

void testCompositeLayerRemovalReusesFreedChannel() {
    using namespace mgstc::engine;

    auto timbre = defaultCompositeTimbre();
    auto second_scc = timbre.layers[1];
    second_scc.name = "SCC Ch.2";
    second_scc.channel = 1;
    timbre.layers.push_back(second_scc);

    REQUIRE_EQ(
        firstAvailableChannel(timbre, TimbreSource::Scc),
        std::optional<std::uint8_t>{2});
    REQUIRE_EQ(removeCompositeLayer(timbre, 3), true);
    REQUIRE_EQ(timbre.layers.size(), 3U);
    REQUIRE_EQ(
        firstAvailableChannel(timbre, TimbreSource::Scc),
        std::optional<std::uint8_t>{1});

    const auto unchanged_size = timbre.layers.size();
    REQUIRE_EQ(removeCompositeLayer(timbre, 99), false);
    REQUIRE_EQ(timbre.layers.size(), unchanged_size);

    auto second_psg = timbre.layers[0];
    second_psg.channel = 1;
    timbre.layers.push_back(second_psg);
    auto third_psg = second_psg;
    third_psg.channel = 2;
    timbre.layers.push_back(third_psg);
    REQUIRE_EQ(
        firstAvailableChannel(timbre, TimbreSource::Psg).has_value(),
        false);
}

void testEnvelopeTimelineInspectorRangeNormalization() {
    using namespace mgstc::engine;

    EnvelopeTimeline timeline;
    setEnvelopeTimelineRange(timeline, 200, 150, 40);
    REQUIRE_EQ(timeline.length_counts, std::uint32_t{200});
    REQUIRE_EQ(
        timeline.loop_start_count,
        std::optional<std::uint32_t>{40});
    REQUIRE_EQ(
        timeline.loop_end_count,
        std::optional<std::uint32_t>{150});

    setEnvelopeTimelineRange(timeline, 0, std::nullopt, 10);
    REQUIRE_EQ(timeline.length_counts, std::uint32_t{1});
    REQUIRE_EQ(timeline.loop_start_count.has_value(), false);
    REQUIRE_EQ(
        timeline.loop_end_count,
        std::optional<std::uint32_t>{1});

    setEnvelopeTimelineRange(timeline, 70'000, 70'000, std::nullopt);
    REQUIRE_EQ(
        timeline.length_counts,
        EnvelopeTimeline::kMaximumLengthCounts);
    REQUIRE_EQ(
        timeline.loop_start_count,
        std::optional<std::uint32_t>{
            EnvelopeTimeline::kMaximumLengthCounts});
    REQUIRE_EQ(timeline.loop_end_count.has_value(), false);
}

void testCompositeEnvelopeFormatsOneSharedMgscLoop() {
    using namespace mgstc::engine;

    auto layer = defaultCompositeTimbre().layers[1];
    layer.volume = 15;
    layer.envelope_timeline = {
        .length_counts = 10,
    };
    layer.volume_envelope.events = {
        {EnvelopeEventKind::Volume, 15, 0, 0},
        {EnvelopeEventKind::Volume, 8, 0, 10},
    };
    layer.pitch_envelope.events = {
        {EnvelopeEventKind::Pitch, -3, 0, 4},
    };
    layer.timbre_automation = {
        {EnvelopeEventKind::Timbre, 17, 0, 4},
    };

    const auto linear = formatMgsCompositeEnvelope(layer, 3);
    REQUIRE_EQ(linear.valid(), true);
    REQUIRE_EQ(
        linear.body,
        std::string(",,f:4.@17.\\-3.f:6.8"));
    REQUIRE_EQ(
        linear.definition,
        std::string("@e3 = { ,,f:4.@17.\\-3.f:6.8 }\r\n"));

    layer.envelope_timeline = {
        .length_counts = 8,
        .loop_start_count = 2,
        .loop_end_count = 6,
    };
    layer.volume_envelope.events = {
        {EnvelopeEventKind::Volume, 15, 0, 0},
        {EnvelopeEventKind::Volume, 12, 0, 2},
    };
    layer.pitch_envelope.events = {
        {EnvelopeEventKind::Pitch, 2, 0, 3},
    };
    layer.timbre_automation = {
        {EnvelopeEventKind::Timbre, 4, 0, 5},
    };
    const auto looped = formatMgsCompositeEnvelope(layer, 4);
    REQUIRE_EQ(looped.valid(), true);
    REQUIRE_EQ(looped.body.find('[') != std::string::npos, true);
    REQUIRE_EQ(looped.body.find(']') != std::string::npos, true);
    REQUIRE_EQ(
        std::count(looped.body.begin(), looped.body.end(), '['),
        static_cast<std::ptrdiff_t>(1));
    REQUIRE_EQ(
        std::count(looped.body.begin(), looped.body.end(), ']'),
        static_cast<std::ptrdiff_t>(1));

    const auto too_long = formatMgsCompositeEnvelope(layer, 4, 8);
    REQUIRE_EQ(
        too_long.hasIssue(
            MgsEnvelopeIssue::DefinitionLengthExceeded),
        true);

    layer.envelope_timeline.loop_end_count.reset();
    const auto incomplete = formatMgsCompositeEnvelope(layer, 4);
    REQUIRE_EQ(
        incomplete.hasIssue(MgsEnvelopeIssue::IncompleteLoop),
        true);
}

void testCompositeSoloPitchAndChannelValidation() {
    auto timbre = mgstc::engine::defaultCompositeTimbre();
    timbre.layers[1].solo = true;
    REQUIRE_EQ(
        mgstc::engine::layerIsAudible(timbre, 0), false);
    REQUIRE_EQ(
        mgstc::engine::layerIsAudible(timbre, 1), true);

    timbre.layers[1].relative_semitones = 12;
    REQUIRE_EQ(
        mgstc::engine::layerMidiNote(timbre.layers[1], 60),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(72)});
    timbre.layers[1].relative_semitones = 80;
    REQUIRE_EQ(
        mgstc::engine::layerMidiNote(timbre.layers[1], 60)
            .has_value(),
        false);

    auto duplicate = timbre.layers[0];
    duplicate.name = "Duplicate PSG";
    timbre.layers.push_back(duplicate);
    const auto validation =
        mgstc::engine::validateCompositeTimbre(timbre);
    REQUIRE_EQ(validation.valid(), false);
    REQUIRE_EQ(validation.warnings.empty(), false);

    auto out_of_range = mgstc::engine::defaultCompositeTimbre();
    out_of_range.layers[0].channel = 3;
    out_of_range.layers[1].channel = 5;
    out_of_range.layers[2].channel = 9;
    REQUIRE_EQ(
        mgstc::engine::validateCompositeTimbre(out_of_range).valid(),
        false);
}

void testCompositeSavedTimbreRevisionAndNumberAssignment() {
    using namespace mgstc::engine;

    TimbreLibrary library;
    TimbreLibraryEntry scc;
    scc.category = TimbreCategory::Scc;
    scc.name = "SCC Lead";
    scc.scc_waveform[0] = 0x7F;
    const auto scc_id = library.add(scc, 10);
    TimbreLibraryEntry opll;
    opll.category = TimbreCategory::Opll;
    opll.name = "OPLL Lead";
    opll.opll_registers[0] = 0x31;
    const auto opll_id = library.add(opll, 11);

    auto composite = defaultCompositeTimbre();
    composite.layers[1].base_timbre =
        makeSavedTimbreReference(*library.find(scc_id));
    composite.layers[1].base_timbre->number_mode =
        TimbreNumberMode::Manual;
    composite.layers[1].base_timbre->manual_number =
        static_cast<std::uint8_t>(25);
    composite.layers[2].base_timbre =
        makeSavedTimbreReference(*library.find(opll_id));

    auto duplicate_scc = composite.layers[1];
    duplicate_scc.name = "SCC Echo";
    duplicate_scc.channel = 1;
    composite.layers.push_back(duplicate_scc);

    const auto numbers = resolveTimbreNumbers(composite);
    REQUIRE_EQ(numbers.valid(), true);
    REQUIRE_EQ(numbers.assignments.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(numbers.assignments[0].number, static_cast<std::uint8_t>(25));
    REQUIRE_EQ(numbers.assignments[1].number, static_cast<std::uint8_t>(15));
    REQUIRE_EQ(numbers.assignments[2].number, static_cast<std::uint8_t>(25));
    REQUIRE_EQ(numbers.assignments[0].manually_assigned, true);
    REQUIRE_EQ(numbers.assignments[1].manually_assigned, false);

    auto second_scc = duplicate_scc;
    TimbreLibraryEntry other_scc = scc;
    other_scc.name = "Other SCC";
    const auto other_id = library.add(other_scc, 12);
    second_scc.base_timbre =
        makeSavedTimbreReference(*library.find(other_id));
    second_scc.base_timbre->number_mode =
        TimbreNumberMode::Manual;
    second_scc.base_timbre->manual_number =
        static_cast<std::uint8_t>(25);
    composite.layers.push_back(second_scc);
    REQUIRE_EQ(resolveTimbreNumbers(composite).valid(), false);
}

void testCompositeTimbreDependencyUpdatePreservesAssignment() {
    using namespace mgstc::engine;

    TimbreLibrary library;
    TimbreLibraryEntry scc;
    scc.category = TimbreCategory::Scc;
    scc.name = "Bass";
    scc.scc_waveform[0] = 1;
    const auto id = library.add(scc, 10);
    auto composite = defaultCompositeTimbre();
    composite.name = "Layered Bass";
    composite.layers[1].base_timbre =
        makeSavedTimbreReference(*library.find(id));
    composite.layers[1].base_timbre->number_mode =
        TimbreNumberMode::Manual;
    composite.layers[1].base_timbre->manual_number =
        static_cast<std::uint8_t>(19);
    std::vector<CompositeTimbre> composites{composite};

    const auto uses = findTimbreUses(composites, id);
    REQUIRE_EQ(uses.size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(uses[0].composite_name, std::string("Layered Bass"));
    REQUIRE_EQ(uses[0].layer_name, std::string("SCC Layer"));

    auto updated = *library.find(id);
    updated.scc_waveform[0] = 0x7F;
    REQUIRE_EQ(library.update(id, updated, 20), true);
    REQUIRE_EQ(
        updateTimbreReferences(composites, *library.find(id)),
        static_cast<std::size_t>(1));
    const auto& reference =
        *composites[0].layers[1].base_timbre;
    REQUIRE_EQ(reference.revision, static_cast<std::uint32_t>(2));
    REQUIRE_EQ(reference.scc_waveform[0], static_cast<std::uint8_t>(0x7F));
    REQUIRE_EQ(reference.number_mode, TimbreNumberMode::Manual);
    REQUIRE_EQ(
        reference.manual_number,
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(19)});
}

void testCompositeTimbreLibraryRoundTripAndRevision() {
    using namespace mgstc::engine;

    TimbreLibrary timbres;
    TimbreLibraryEntry scc;
    scc.category = TimbreCategory::Scc;
    scc.name = "Library SCC";
    scc.scc_waveform[0] = 0x80;
    const auto scc_id = timbres.add(scc, 10);

    auto composite = defaultCompositeTimbre();
    composite.name = "Unicode Composite";
    composite.tags = "lead layered";
    composite.memo = "round-trip\nmemo";
    composite.layers[1].base_timbre =
        makeSavedTimbreReference(*timbres.find(scc_id));
    composite.layers[1].base_timbre->number_mode =
        TimbreNumberMode::Manual;
    composite.layers[1].base_timbre->manual_number =
        static_cast<std::uint8_t>(22);
    composite.layers[1].relative_semitones = -12;
    composite.layers[1].detune = -37;
    composite.layers[1].pitch_envelope.events = {
        {
            .kind = EnvelopeEventKind::Pitch,
            .value = -5,
            .secondary = 7,
            .count = 12,
        },
    };
    composite.layers[1].envelope_timeline = {
        .length_counts = 4096,
        .loop_start_count = 128,
        .loop_end_count = 3072,
    };
    composite.layers[1].timbre_automation = {
        {
            .kind = EnvelopeEventKind::Timbre,
            .value = 17,
            .count = 30,
        },
    };

    CompositeTimbreLibrary library;
    const auto id = library.add(composite, 100);
    REQUIRE_EQ(id, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(
        library.uniqueName(composite.name),
        std::string("Unicode Composite(1)"));

    const auto serialized = library.serialize();
    std::string error;
    auto loaded =
        CompositeTimbreLibrary::deserialize(serialized, &error);
    REQUIRE_EQ(loaded.has_value(), true);
    REQUIRE_EQ(error.empty(), true);
    REQUIRE_EQ(loaded->entries().size(), static_cast<std::size_t>(1));
    REQUIRE_EQ(loaded->find(id)->timbre, composite);
    REQUIRE_EQ(
        loaded->find(id)->revision,
        static_cast<std::uint32_t>(1));

    auto renamed = composite;
    renamed.name = "Updated Composite";
    REQUIRE_EQ(loaded->update(id, renamed, 120), true);
    REQUIRE_EQ(
        loaded->find(id)->revision,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        loaded->find(id)->created_unix_seconds,
        static_cast<std::int64_t>(100));
    REQUIRE_EQ(
        loaded->find(id)->updated_unix_seconds,
        static_cast<std::int64_t>(120));

    REQUIRE_EQ(
        CompositeTimbreLibrary::deserialize(
            "MGSTC_COMPOSITE_TIMBRE_LIBRARY\t2\r\n",
            &error)
            .has_value(),
        false);
    REQUIRE_EQ(
        CompositeTimbreLibrary::deserialize(
            "MGSTC_COMPOSITE_TIMBRE_LIBRARY\t1\r\n"
            "C\t1\t2\t3\t1\t0Z\r\n",
            &error)
            .has_value(),
        false);
}

void testCompositeLibraryDependencyIndexAndPropagation() {
    using namespace mgstc::engine;

    TimbreLibrary timbres;
    TimbreLibraryEntry scc;
    scc.category = TimbreCategory::Scc;
    scc.name = "Shared SCC";
    scc.scc_waveform[0] = 1;
    const auto timbre_id = timbres.add(scc, 10);

    auto first = defaultCompositeTimbre();
    first.name = "First Composite";
    first.layers[1].base_timbre =
        makeSavedTimbreReference(*timbres.find(timbre_id));
    auto second = defaultCompositeTimbre();
    second.name = "Second Composite";
    second.layers[1].base_timbre =
        makeSavedTimbreReference(*timbres.find(timbre_id));
    second.layers.push_back(second.layers[1]);
    second.layers.back().name = "Second SCC Echo";
    second.layers.back().channel = 1;

    CompositeTimbreLibrary composites;
    const auto first_id = composites.add(first, 20);
    const auto second_id = composites.add(second, 21);
    const auto uses = composites.findTimbreUses(timbre_id);
    REQUIRE_EQ(uses.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(uses[0].composite_name, std::string("First Composite"));
    REQUIRE_EQ(uses[2].layer_name, std::string("Second SCC Echo"));

    auto replacement = *timbres.find(timbre_id);
    replacement.scc_waveform[0] = 0x7F;
    REQUIRE_EQ(timbres.update(timbre_id, replacement, 30), true);
    REQUIRE_EQ(
        composites.updateTimbreReferences(
            *timbres.find(timbre_id), 31),
        static_cast<std::size_t>(3));
    REQUIRE_EQ(
        composites.find(first_id)->revision,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        composites.find(second_id)->revision,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        composites.find(second_id)
            ->timbre.layers.back()
            .base_timbre
            ->revision,
        static_cast<std::uint32_t>(2));
    REQUIRE_EQ(
        composites.find(first_id)
            ->timbre.layers[1]
            .base_timbre
            ->scc_waveform[0],
        static_cast<std::uint8_t>(0x7F));
}

void testPcKeyboardPhysicalLayoutAndOctaveSlide() {
    using mgstc::engine::pcKeyboardMidiNoteFromScanCode;

    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x2C, 4),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(60)});
    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x10, 4),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(72)});
    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x03, 4),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(73)});
    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x10, 5),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(84)});
    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x03, 5),
        std::optional<std::uint8_t>{
            static_cast<std::uint8_t>(85)});
    REQUIRE_EQ(
        pcKeyboardMidiNoteFromScanCode(0x01, 4).has_value(),
        false);
}

void testSequentialVoiceAllocatorPolyMonoAndStealing() {
    mgstc::engine::SequentialVoiceAllocator voices(3);
    const auto first = voices.noteOn(60);
    const auto second = voices.noteOn(64);
    const auto third = voices.noteOn(67);
    REQUIRE_EQ(first.channel, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(second.channel, static_cast<std::uint8_t>(1));
    REQUIRE_EQ(third.channel, static_cast<std::uint8_t>(2));
    REQUIRE_EQ(voices.activeVoiceCount(), static_cast<std::size_t>(3));

    const auto stolen = voices.noteOn(69);
    REQUIRE_EQ(stolen.channel, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(stolen.stolen_note, std::optional<std::uint8_t>{60});
    REQUIRE_EQ(voices.noteOff(64), std::optional<std::uint8_t>{1});
    REQUIRE_EQ(voices.noteOn(71).channel, static_cast<std::uint8_t>(1));

    voices.setPolyphonic(false);
    REQUIRE_EQ(voices.activeVoiceCount(), static_cast<std::size_t>(0));
    REQUIRE_EQ(voices.noteOn(72).channel, static_cast<std::uint8_t>(0));
    const auto mono_replacement = voices.noteOn(74);
    REQUIRE_EQ(mono_replacement.channel, static_cast<std::uint8_t>(0));
    REQUIRE_EQ(
        mono_replacement.stolen_note,
        std::optional<std::uint8_t>{72});
    REQUIRE_EQ(voices.noteOff(72).has_value(), false);
    REQUIRE_EQ(voices.noteOff(74), std::optional<std::uint8_t>{0});
}

#ifdef _WIN32
void testWasapiSinkConstructsWithoutOpeningDevice() {
    WasapiAudioSink sink;
    REQUIRE_EQ(sink.running(), false);
    REQUIRE_EQ(sink.masterVolumePercent(), std::uint32_t{100});
    sink.setMasterVolumePercent(37);
    REQUIRE_EQ(sink.masterVolumePercent(), std::uint32_t{37});
    sink.setMasterVolumePercent(101);
    REQUIRE_EQ(sink.masterVolumePercent(), std::uint32_t{100});
    mgstc::audio::AudioSinkStatus status{};
    REQUIRE_EQ(sink.pollStatus(status), false);
    sink.stop();
}
#endif

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"TickClockSplitsVariableCallbacks", testTickClockSplitsVariableCallbacks},
        {"AdjacentVolumesAreOneTickApart", testAdjacentVolumesAreOneTickApart},
        {"ObservedFourCountHolds", testObservedFourCountHolds},
        {"PatchAndRegisterWriteAreZeroTime", testPatchAndRegisterWriteAreZeroTime},
        {"RampUsesIntegerRemainderDistribution", testRampUsesIntegerRemainderDistribution},
        {"FrequencyDeltasAreSignedAndCumulativeEvents", testFrequencyDeltasAreSignedAndCumulativeEvents},
        {"NoWaitLoopHitsInstructionBudget", testNoWaitLoopHitsInstructionBudget},
        {"CompositeSequenceLanesLoopIndependently", testCompositeSequenceLanesLoopIndependently},
        {"RateEnvelopeNativePhases", testRateEnvelopeNativePhases},
        {"OpllKeyOffDoesNotStartSoftwareRelease", testOpllKeyOffDoesNotStartSoftwareRelease},
        {"VolumeCombination", testVolumeCombination},
        {"ChipSpecificVolumeMapping", testChipSpecificVolumeMapping},
        {"MgsdrvNoteTables", testMgsdrvNoteTables},
        {"RuntimePsgNoteOnOrderMatchesObservedBoundary", testRuntimePsgNoteOnOrderMatchesObservedBoundary},
        {"RuntimeBoundaryWritesMatchVgmFixture", testRuntimeBoundaryWritesMatchVgmFixture},
        {"RuntimeSccKeyMaskPreservesOtherChannels", testRuntimeSccKeyMaskPreservesOtherChannels},
        {"RuntimePsgSequenceKeyOffStaysSilent", testRuntimePsgSequenceKeyOffStaysSilent},
        {"AuditionGateSuppressesEnvelopeUntilNoteOn", testAuditionGateSuppressesEnvelopeUntilNoteOn},
        {"RuntimeOpllKeyOnAndOffRegisters", testRuntimeOpllKeyOnAndOffRegisters},
        {"ChipRackRendersAllThreeChips", testChipRackRendersAllThreeChips},
        {"EngineCoreSplitsAtEightHundredFrameBoundary", testEngineCoreSplitsAtEightHundredFrameBoundary},
        {"EngineCoreValidatesAndClampsOutput", testEngineCoreValidatesAndClampsOutput},
        {"EngineCorePublishesOneSixtiethOpllScope", testEngineCorePublishesOneSixtiethOpllScope},
        {"SccPresetGenerationIncludesDocumentedHarmonics", testSccPresetGenerationIncludesDocumentedHarmonics},
        {"SccAverageUsesCircularThreeSampleWindow", testSccAverageUsesCircularThreeSampleWindow},
        {"SccMergeFindsCircularPhaseAndPreservesLevel", testSccMergeFindsCircularPhaseAndPreservesLevel},
        {"SccWaveformUtilityTransforms", testSccWaveformUtilityTransforms},
        {"RuntimeUsesMgsTrackOrder", testRuntimeUsesMgsTrackOrder},
        {"PsgToneNoiseAndFrequencyMapping", testPsgToneNoiseAndFrequencyMapping},
        {"SccWaveUsesSharedChannelFourFiveRam", testSccWaveUsesSharedChannelFourFiveRam},
        {"OpllOriginalPatchThenYThenVolume", testOpllOriginalPatchThenYThenVolume},
        {"SccFrequencyDeltaAndIgnoredY", testSccFrequencyDeltaAndIgnoredY},
        {"RegisterWriteBufferPreservesDuplicatesAndStopsAtCapacity", testRegisterWriteBufferPreservesDuplicatesAndStopsAtCapacity},
        {"PsgHardwareEnvelopeSharedState", testPsgHardwareEnvelopeSharedState},
        {"PsgHardwareShapeRestartSuppression", testPsgHardwareShapeRestartSuppression},
        {"NineVoiceRhythmSharedRegisters", testNineVoiceRhythmSharedRegisters},
        {"NineVoiceRhythmRetriggerWritesOffThenOn", testNineVoiceRhythmRetriggerWritesOffThenOn},
        {"RuntimeAppliesTrackAndCommonAttenuation", testRuntimeAppliesTrackAndCommonAttenuation},
        {"RuntimeRetriggerRestartsSequenceOnSameTick", testRuntimeRetriggerRestartsSequenceOnSameTick},
        {"RuntimePsgHardwareEnvelopeOverridesSoftwareVolume", testRuntimePsgHardwareEnvelopeOverridesSoftwareVolume},
        {"SpscQueuePreservesFifoAcrossWrap", testSpscQueuePreservesFifoAcrossWrap},
        {"SpscQueueTransfersConcurrently", testSpscQueueTransfersConcurrently},
        {"RealtimeHostDrainsCommandsAndReportsRejections", testRealtimeHostDrainsCommandsAndReportsRejections},
        {"RealtimeHostCommandQueueHasFixedCapacity", testRealtimeHostCommandQueueHasFixedCapacity},
        {"ProgramPoolLimitsEditingAndReusesReleasedSlot", testProgramPoolLimitsEditingAndReusesReleasedSlot},
        {"ProgramSnapshotActivatesAndRetriggersWithoutAudioAllocation", testProgramSnapshotActivatesAndRetriggersWithoutAudioAllocation},
        {"RealtimeHostPublishesOpllScopeToUiQueue", testRealtimeHostPublishesOpllScopeToUiQueue},
        {"InvalidAuditionKeepsProgramEditable", testInvalidAuditionKeepsProgramEditable},
        {"OpllPatchSemanticRoundTrip", testOpllPatchSemanticRoundTrip},
        {"Ym2413RomPatchesComeFromEmu2413Table", testYm2413RomPatchesComeFromEmu2413Table},
        {"OpllEnvelopeTraceUsesEmu2413EgAndKeyOff", testOpllEnvelopeTraceUsesEmu2413EgAndKeyOff},
        {"MgsOpllDefinitionImportExportRoundTrip", testMgsOpllDefinitionImportExportRoundTrip},
        {"MgsSccDefinitionImportExportRoundTrip", testMgsSccDefinitionImportExportRoundTrip},
        {"TimbreLibraryCrudAndVersionedRoundTrip", testTimbreLibraryCrudAndVersionedRoundTrip},
        {"TimbreLibrarySelectedExportAndNonDestructiveImport", testTimbreLibrarySelectedExportAndNonDestructiveImport},
        {"WavePcmCycleConvertsToScc", testWavePcmCycleConvertsToScc},
        {"WaveCycleProducesValidOpllApproximation", testWaveCycleProducesValidOpllApproximation},
        {"WavePcmProducesDeterministicTimedOpllApproximation", testWavePcmProducesDeterministicTimedOpllApproximation},
        {"DefaultCompositeTimbreHasThreeAudibleSources", testDefaultCompositeTimbreHasThreeAudibleSources},
        {"CompositeLayerRemovalReusesFreedChannel", testCompositeLayerRemovalReusesFreedChannel},
        {"EnvelopeTimelineInspectorRangeNormalization", testEnvelopeTimelineInspectorRangeNormalization},
        {"CompositeEnvelopeFormatsOneSharedMgscLoop", testCompositeEnvelopeFormatsOneSharedMgscLoop},
        {"CompositeSoloPitchAndChannelValidation", testCompositeSoloPitchAndChannelValidation},
        {"CompositeSavedTimbreRevisionAndNumberAssignment", testCompositeSavedTimbreRevisionAndNumberAssignment},
        {"CompositeTimbreDependencyUpdatePreservesAssignment", testCompositeTimbreDependencyUpdatePreservesAssignment},
        {"CompositeTimbreLibraryRoundTripAndRevision", testCompositeTimbreLibraryRoundTripAndRevision},
        {"CompositeLibraryDependencyIndexAndPropagation", testCompositeLibraryDependencyIndexAndPropagation},
        {"PcKeyboardPhysicalLayoutAndOctaveSlide", testPcKeyboardPhysicalLayoutAndOctaveSlide},
        {"SequentialVoiceAllocatorPolyMonoAndStealing", testSequentialVoiceAllocatorPolyMonoAndStealing},
#ifdef _WIN32
        {"WasapiSinkConstructsWithoutOpeningDevice", testWasapiSinkConstructsWithoutOpeningDevice},
#endif
    };

    int failures = 0;
    for (const auto& [name, body] : tests) {
        try {
            body();
            std::cout << "[PASS] " << name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what()
                      << std::endl;
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
