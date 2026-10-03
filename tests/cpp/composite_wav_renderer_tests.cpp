// SPDX-License-Identifier: AGPL-3.0-only

#include "mgstc/engine/composite_wav_renderer.hpp"
#include "mgstc/engine/composite_program_compiler.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace mgstc::engine;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<float> directRender(const CompositeTimbre& source,
                               std::size_t on_frame,
                               std::size_t off_frame,
                               std::size_t frame_count) {
    EngineCore engine;
    CompositePlaybackPlan plan;
    require(compileCompositeProgram(engine, source, {}, &plan), "direct compile");
    engine.session().gateUntilNoteOn();
    std::vector<float> pcm(frame_count * 2);
    for (std::size_t frame = 0; frame < frame_count; ) {
        for (const auto& binding : plan.audible_layers) {
            const auto note = layerMidiNote(source.layers[binding.layer_index], 60);
            require(note.has_value(), "direct layer note");
            if (frame == on_frame) {
                require(engine.session().queueNoteOn(
                            plan.physicalTrack(binding, 0), *note), "direct key-on");
            }
            if (frame == off_frame) {
                require(engine.session().queueKeyOff(
                            plan.physicalTrack(binding, 0)), "direct key-off");
            }
        }
        auto count = std::min<std::size_t>(137, frame_count - frame);
        if (frame < on_frame) {
            count = std::min(count, on_frame - frame);
        }
        if (frame < off_frame) {
            count = std::min(count, off_frame - frame);
        }
        const auto rendered = engine.render(
            std::span<float>(pcm.data() + frame * 2, count * 2));
        require(rendered.ok() && rendered.frames == count, "direct render");
        frame += count;
    }
    return pcm;
}

void testRealEngineTimingAndExactEnd() {
    const auto source = defaultCompositeTimbre();
    const auto authored = source;
    const auto result = renderCompositeWav(
        source, {.key_off_frame = 901, .frame_count = 2'403});
    require(result.ok(), "ordinary composite renders");
    require(result.requested_key_off_frame == 901
                && result.effective_key_off_frame == 1'600,
            "key-off preserves requested position and next tick");
    require(result.mono_pcm.size() == 2'403 && result.stereo_pcm.size() == 4'806,
            "selected end is exact including partial tick");
    require(result.stereo_pcm == directRender(source, 0, 1'600, 2'403),
            "offline PCM equals ordinary runtime across arbitrary chunk sizes");
    require(source == authored, "authoring state remains immutable");
    double peak = 0.0;
    for (std::size_t frame = 0; frame < result.mono_pcm.size(); ++frame) {
        require(result.mono_pcm[frame] == result.stereo_pcm[frame * 2]
                    && result.mono_pcm[frame] == result.stereo_pcm[frame * 2 + 1],
                "mono is stereo average");
        peak = std::max(peak, static_cast<double>(std::abs(result.mono_pcm[frame])));
    }
    require(peak > 0.001, "composite has audible PCM");
}

void testShiftedLayerLifetime() {
    auto source = defaultCompositeTimbre();
    source.layers.resize(1);
    // At tempo120 r%1 is 500 engine samples; queued keys therefore begin
    // at800 and release at ceil((500+901)/800)*800 =1600.
    source.layers[0].start_delay_form = StartDelayForm::AbsoluteTicks;
    source.layers[0].start_delay_value = 1;
    const auto result = renderCompositeWav(
        source, {.key_off_frame = 901, .frame_count = 2'403});
    require(result.ok(), "delayed layer renders");
    require(result.stereo_pcm == directRender(source, 800, 1'600, 2'403),
            "layer delay shifts both keys while retaining runtime tick order");
    require(std::all_of(result.mono_pcm.begin(), result.mono_pcm.begin() + 800,
                       [](float value) { return value == 0.0F; }),
            "gated layer has no writes before delayed key-on");
}

void testMorphUsesOrdinaryCompilerWithoutChangingSource() {
    auto source = defaultCompositeTimbre();
    source.layers.erase(source.layers.begin() + 2);
    source.layers.erase(source.layers.begin());
    auto& layer = source.layers.front();
    seedDefaultLayerTimbre(source, layer);
    require(layer.base_timbre.has_value(), "default SCC has snapshot");
    auto destination = *layer.base_timbre;
    destination.library_id = allocateCompositeOwnedTimbreId(source);
    for (std::size_t index = 0; index < destination.scc_waveform.size(); ++index) {
        destination.scc_waveform[index] = static_cast<std::uint8_t>(
            static_cast<std::int8_t>(static_cast<int>(index) * 7 - 112));
    }
    source.embedded_timbres.push_back(destination);
    layer.envelope_timeline.length_counts = 8;
    layer.timbre_automation = {
        {.kind = EnvelopeEventKind::Timbre,
         .count = 0,
         .target_library_id = layer.base_timbre->library_id},
        {.kind = EnvelopeEventKind::Timbre,
         .count = 4,
         .target_library_id = destination.library_id,
         .scc_morph = {.enabled = true, .intermediate_count = 2, .curve = 1.0}},
    };
    const auto authored = source;
    const auto rendered = renderCompositeWav(
        source, {.key_off_frame = 5'600, .frame_count = 6'401});
    require(rendered.ok(), "morph candidate renders");
    require(rendered.stereo_pcm == directRender(source, 0, 5'600, 6'401),
            "offline uses identical finalized morph commands as normal engine");
    require(source == authored, "morph renderer does not materialize authoring source");
}

void testFreshStateAndFailures() {
    auto source = defaultCompositeTimbre();
    source.layers.back().base_opll_rom = 2;
    const CompositeWavRenderOptions options{
        .key_off_frame = 1'600, .frame_count = 3'201};
    const auto first = renderCompositeWav(source, options);
    auto other_source = source;
    other_source.layers.back().relative_semitones = 7;
    other_source.layers[1].relative_semitones = -5;
    const auto other = renderCompositeWav(other_source, options);
    const auto repeated = renderCompositeWav(source, options);
    require(first.ok() && other.ok() && repeated.ok(), "candidate renders succeed");
    require(first.stereo_pcm == repeated.stereo_pcm,
            "intervening candidate leaves no FM or SCC phase state");
    require(first.stereo_pcm != other.stereo_pcm,
            "intervening candidate actually changes sound");

    std::atomic_bool cancel{true};
    auto cancelled_options = options;
    cancelled_options.cancel = &cancel;
    const auto cancelled = renderCompositeWav(source, cancelled_options);
    require(cancelled.error == CompositeWavRenderError::Cancelled
                && cancelled.mono_pcm.empty() && cancelled.stereo_pcm.empty(),
            "cancelled work publishes no partial PCM");

    auto invalid = options;
    invalid.frame_count = CompositeWavRenderOptions::kMaximumFrameCount + 1;
    require(renderCompositeWav(source, invalid).error
                == CompositeWavRenderError::InvalidOptions,
            "render duration allocation is bounded");
    invalid = options;
    invalid.key_off_frame = 0;
    require(!renderCompositeWav(source, invalid).ok(), "zero gate rejected");
    invalid.key_off_frame = invalid.frame_count + 1;
    require(!renderCompositeWav(source, invalid).ok(), "key-off beyond end rejected");
    invalid = options;
    invalid.midi_note = 23;
    require(renderCompositeWav(source, invalid).error == CompositeWavRenderError::InvalidNote,
            "unsupported pitch rejected");
    invalid = options;
    invalid.gains.master = std::numeric_limits<float>::quiet_NaN();
    require(renderCompositeWav(source, invalid).error
                == CompositeWavRenderError::InvalidOptions,
            "nonfinite mixer gain rejected");
    const auto empty = renderCompositeWav(CompositeTimbre{}, options);
    require(empty.error == CompositeWavRenderError::ProgramCompile
                && empty.stereo_pcm.empty(), "empty program cannot pass as silence");
}
}  // namespace

int main() {
    try {
        testRealEngineTimingAndExactEnd();
        testShiftedLayerLifetime();
        testMorphUsesOrdinaryCompilerWithoutChangingSource();
        testFreshStateAndFailures();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
