// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "juce_chip_tracks.hpp"
#include "opll_conversion_task.hpp"
#include "mgstc/engine/engine_core.hpp"

namespace mgstc::app {

inline bool defineSingleAuditionScc(engine::RuntimeSession& runtime,
                                   const engine::SccWaveform& waveform) {
    std::array<std::uint8_t, 32> raw{};
    std::transform(waveform.begin(), waveform.end(), raw.begin(),
        [](std::int8_t sample) { return static_cast<std::uint8_t>(sample); });
    return runtime.mapper().defineSccPatch(0, raw) == engine::MapError::None;
}

inline bool configureSingleAuditionSccTrack(engine::RuntimeSession& runtime,
                                            std::uint8_t track) {
    return runtime.setSequenceEnvelope(track, {0x10, 0x00, 0x40, 0xEF, 0x01, 0x60})
        && runtime.setTrackVolume(track, 15);
}

inline constexpr double singleAuditionMilliseconds = 1000.0;

inline OpllConversionTask::Prepare prepareSccConversion(engine::SccWaveform waveform,
                                                         std::uint8_t note) {
    return [waveform, note](engine::WavePcm& pcm, OpllConversionTask::Original& original,
                            std::string& error) {
        engine::EngineCore core;
        if (!core.valid() || !defineSingleAuditionScc(core.session(), waveform)
            || !configureSingleAuditionSccTrack(core.session(), kSccTrack)
            || !core.queueNoteOn(kSccTrack, note)) {
            error.clear();
            return false;
        }
        auto decoded = std::make_shared<engine::SourcePcm>();
        decoded->sample_rate = 48'000;
        decoded->channels = 2;
        decoded->bit_depth = 32;
        decoded->sample_format = engine::SourceSampleFormat::FloatPcm;
        const auto frames = static_cast<std::size_t>(decoded->sample_rate * singleAuditionMilliseconds / 1000.0);
        decoded->interleaved_samples.resize(frames * 2);
        if (const auto result = core.render(decoded->interleaved_samples);
            !result.ok() || result.frames != frames) return false;
        decoded->mono_samples.resize(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto offset = frame * 2;
            decoded->mono_samples[frame] = (decoded->interleaved_samples[offset]
                + decoded->interleaved_samples[offset + 1]) * 0.5f;
        }
        pcm.sample_rate = decoded->sample_rate;
        pcm.mono_samples = decoded->mono_samples;
        original = std::move(decoded);
        return true;
    };
}

} // namespace mgstc::app
