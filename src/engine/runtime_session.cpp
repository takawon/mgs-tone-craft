#include "mgstc/engine/runtime_session.hpp"

#include <utility>

#include "mgstc/engine/volume.hpp"

namespace mgstc::engine {
namespace {

[[nodiscard]] std::int32_t psgSccTrackMicroDetuneOffset(
    std::int32_t value) noexcept {
    const auto wide = static_cast<std::int64_t>(value);
    if (wide >= 0) {
        return static_cast<std::int32_t>((wide + 4) / 8);
    }
    return static_cast<std::int32_t>(-((-wide + 4) / 8));
}

[[nodiscard]] std::int32_t opllTrackMicroDetuneOffset(
    std::uint8_t midi_note,
    std::int32_t value) noexcept {
    if (value <= 0) {
        return 0;
    }
    NotePitch current{};
    NotePitch next{};
    if (!notePitch(midi_note, current)) {
        return 0;
    }
    int next_f_number{};
    if ((midi_note % 12) == 11) {
        if (!notePitch(static_cast<std::uint8_t>(midi_note - 11), next)) {
            return 0;
        }
        next_f_number = static_cast<int>(next.opll.f_number) * 2;
    } else {
        if (!notePitch(static_cast<std::uint8_t>(midi_note + 1), next)) {
            return 0;
        }
        next_f_number = next.opll.f_number;
    }
    const auto difference =
        next_f_number - static_cast<int>(current.opll.f_number);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(difference) * value / 255);
}

}  // namespace

RuntimeSession::RuntimeSession(
    std::size_t max_meaning_events,
    std::size_t max_register_writes)
    : meaning_events_(max_meaning_events),
      writes_(max_register_writes) {
    current_notes_.fill(60);
    psg_tone_noise_modes_.fill(1);
    psg_noise_periods_.fill(0);
}

bool RuntimeSession::setSequenceEnvelope(
    std::uint8_t track,
    std::vector<std::uint8_t> bytecode) {
    if (track >= kTrackCount) {
        return false;
    }
    return tracks_[track].setSequenceEnvelope(std::move(bytecode));
}

bool RuntimeSession::setCompositeSequenceEnvelopes(
    std::uint8_t track,
    std::vector<std::uint8_t> volume_bytecode,
    std::vector<std::uint8_t> pitch_bytecode,
    std::vector<std::uint8_t> timbre_bytecode) {
    if (track >= kTrackCount) {
        return false;
    }
    return tracks_[track].setCompositeSequenceEnvelopes(
        std::move(volume_bytecode),
        std::move(pitch_bytecode),
        std::move(timbre_bytecode));
}

bool RuntimeSession::setRateEnvelope(
    std::uint8_t track,
    RateEnvelopeDefinition definition,
    std::uint8_t track_volume) {
    if (track >= kTrackCount) {
        return false;
    }
    return tracks_[track].setRateEnvelope(definition, track_volume);
}

bool RuntimeSession::clearTrack(std::uint8_t track) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    tracks_[track].clear();
    track_detune_[track] = 0;
    track_micro_detune_[track] = 0;
    track_patch_[track].reset();
    track_lfo_[track].clear();
    track_pitch_sweep_[track].clear();
    track_key_off_hang_[track] = 0;
    key_off_hang_remaining_[track] = 0;
    track_opll_sustain_[track] = false;
    return true;
}

bool RuntimeSession::setTrackVolume(
    std::uint8_t track,
    std::uint8_t volume) noexcept {
    if (track >= kTrackCount || volume > 15) {
        return false;
    }
    tracks_[track].setTrackVolume(volume);
    if (track < 3) {
        psg_modes_[track].setFixedVolume(volume);
    }
    return true;
}

bool RuntimeSession::setTrackDetune(
    std::uint8_t track,
    std::int16_t detune,
    std::int32_t micro_detune) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    track_detune_[track] = detune;
    track_micro_detune_[track] = micro_detune;
    return true;
}

bool RuntimeSession::setTrackPatch(
    std::uint8_t track,
    std::optional<std::uint8_t> patch) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    if (patch && *patch > 31) {
        return false;
    }
    track_patch_[track] = patch;
    return true;
}

bool RuntimeSession::setTrackSoftwareLfo(
    std::uint8_t track,
    SoftwareLfoSettings settings) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    const bool extra_roughness = track < 8;
    track_lfo_[track].setSettings(
        clampSoftwareLfo(settings, extra_roughness), extra_roughness);
    if (track_lfo_[track].enabled()) {
        track_pitch_sweep_[track].clear();
    }
    return true;
}

bool RuntimeSession::setTrackPitchSweep(
    std::uint8_t track,
    PitchSweepSettings settings) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    if (track >= 8) {
        settings.enabled = false;
    }
    track_pitch_sweep_[track].setSettings(settings);
    if (track_pitch_sweep_[track].enabled()) {
        track_lfo_[track].clear();
    }
    return true;
}

bool RuntimeSession::setTrackKeyOffHang(
    std::uint8_t track,
    std::uint8_t hang_ticks) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    track_key_off_hang_[track] = track < 8 ? hang_ticks : 0;
    return true;
}

bool RuntimeSession::setTrackOpllSustain(
    std::uint8_t track,
    bool sustain) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    track_opll_sustain_[track] = track >= 8 && sustain;
    return true;
}

bool RuntimeSession::setTrackAttenuation(
    std::uint8_t track,
    std::uint8_t attenuation) noexcept {
    if (track >= kTrackCount || attenuation > 15) {
        return false;
    }
    track_attenuation_[track] = attenuation;
    return true;
}

bool RuntimeSession::setMasterAttenuation(
    std::uint8_t attenuation) noexcept {
    if (attenuation > 15) {
        return false;
    }
    master_attenuation_ = attenuation;
    return true;
}

bool RuntimeSession::selectPsgHardwareEnvelope(
    std::uint8_t track,
    std::uint8_t shape) noexcept {
    if (track >= 3) {
        return false;
    }
    if (!psg_modes_[track].selectHardware(psg_hardware_, shape)) {
        return false;
    }
    tracks_[track].setTrackVolume(15);
    return true;
}

bool RuntimeSession::setPsgFixedVolume(
    std::uint8_t track,
    std::uint8_t volume) noexcept {
    if (track >= 3 || volume > 15) {
        return false;
    }
    psg_modes_[track].setFixedVolume(volume);
    tracks_[track].setTrackVolume(volume);
    return true;
}

bool RuntimeSession::queuePsgHardwareEnvelopePeriod(
    std::uint8_t source_track,
    std::uint16_t period) noexcept {
    if (source_track >= 3 || period == 0) {
        return false;
    }
    psg_period_source_track_ = source_track;
    pending_psg_period_ = period;
    psg_period_pending_ = true;
    return true;
}

bool RuntimeSession::setPsgToneNoise(
    std::uint8_t track,
    std::uint8_t mode,
    std::uint8_t noise) noexcept {
    if (track >= 3 || mode > 3 || noise > 31) {
        return false;
    }
    psg_tone_noise_modes_[track] = mode;
    psg_noise_periods_[track] = noise;
    return true;
}

bool RuntimeSession::queueNoteOn(
    std::uint8_t track,
    std::uint8_t midi_note) noexcept {
    NotePitch pitch{};
    if (track >= kTrackCount || !notePitch(midi_note, pitch)) {
        return false;
    }
    current_notes_[track] = midi_note;
    audition_track_running_[track] = true;
    pending_keys_[track] = PendingKey::On;
    return true;
}

bool RuntimeSession::queueKeyOn(std::uint8_t track) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    audition_track_running_[track] = true;
    pending_keys_[track] = PendingKey::On;
    return true;
}

bool RuntimeSession::queueKeyOff(std::uint8_t track) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    pending_keys_[track] = PendingKey::Off;
    return true;
}

bool RuntimeSession::forceMuteTrack(std::uint8_t track) noexcept {
    if (track >= kTrackCount) {
        return false;
    }
    force_mute_pending_[track] = true;
    return true;
}

void RuntimeSession::gateUntilNoteOn() noexcept {
    audition_gated_ = true;
    audition_track_running_.fill(false);
    force_mute_pending_.fill(false);
}

void RuntimeSession::resetForKeyOn() noexcept {
    tick_ = 0;
    mapper_.reset();
    meaning_events_.clear();
    writes_.clear();
    for (auto& track : tracks_) {
        track.resetForKeyOn();
    }
    for (auto& lfo : track_lfo_) {
        lfo.resetForKeyOn();
    }
    for (auto& sweep : track_pitch_sweep_) {
        sweep.resetForKeyOn();
    }
    key_off_hang_remaining_.fill(0);
    pending_keys_.fill(PendingKey::None);
    audition_track_running_.fill(false);
    force_mute_pending_.fill(false);
    psg_sequence_muted_.fill(false);
    psg_period_pending_ = false;
}

TickResult RuntimeSession::processTick() {
    writes_.clear();
    mapper_.beginTick();

    if (psg_period_pending_) {
        const auto map_error = psg_hardware_.setPeriod(
            psg_period_source_track_,
            pending_psg_period_,
            tick_,
            mapper_,
            writes_);
        if (map_error != MapError::None) {
            return {
                tick_,
                psg_period_source_track_,
                SequenceError::None,
                map_error,
            };
        }
        psg_period_pending_ = false;
    }

    for (std::uint8_t track = 0; track < kTrackCount; ++track) {
        auto& runtime = tracks_[track];
        if (force_mute_pending_[track]) {
            force_mute_pending_[track] = false;
            key_off_hang_remaining_[track] = 0;
            if (pending_keys_[track] == PendingKey::Off
                || pending_keys_[track] == PendingKey::On
                || audition_track_running_[track]) {
                pending_keys_[track] = PendingKey::Off;
            }
            if (pending_keys_[track] == PendingKey::Off
                && current_notes_[track] >= 24
                && current_notes_[track] <= 119) {
                NotePitch pitch{};
                if (notePitch(current_notes_[track], pitch)) {
                    if (track < 3 && !runtime.rateEnvelope()) {
                        psg_sequence_muted_[track] = true;
                    } else if (track >= 3 && track < 8) {
                        static_cast<void>(mapper_.writeSccKey(
                            track, false, tick_, writes_));
                    } else if (track >= 8) {
                        static_cast<void>(mapper_.writeOpllPitch(
                            track,
                            pitch.opll,
                            false,
                            tick_,
                            writes_,
                            track_opll_sustain_[track]));
                    }
                }
            }
            const auto map_error = mapper_.mapMeaningEvent(
                track,
                MeaningEvent{
                    tick_,
                    MeaningEventKind::Volume,
                    0,
                    0,
                },
                tick_,
                writes_);
            if (map_error != MapError::None) {
                return {
                    tick_,
                    track,
                    SequenceError::None,
                    map_error,
                };
            }
            runtime.keyOff(track < 8);
            audition_track_running_[track] = false;
            pending_keys_[track] = PendingKey::None;
            continue;
        }
        if (audition_gated_
            && !audition_track_running_[track]
            && pending_keys_[track] == PendingKey::None
            && key_off_hang_remaining_[track] == 0) {
            continue;
        }
        bool hang_expired = false;
        if (pending_keys_[track] != PendingKey::On
            && key_off_hang_remaining_[track] > 0) {
            --key_off_hang_remaining_[track];
            hang_expired = key_off_hang_remaining_[track] == 0;
        }
        if (pending_keys_[track] == PendingKey::On) {
            if (track < 3) {
                psg_sequence_muted_[track] = false;
            }
            NotePitch pitch{};
            if (!notePitch(current_notes_[track], pitch)) {
                return {
                    tick_,
                    track,
                    SequenceError::None,
                    MapError::InvalidValue,
                };
            }
            runtime.resetForKeyOn();
            track_lfo_[track].resetForKeyOn();
            track_pitch_sweep_[track].resetForKeyOn();
            key_off_hang_remaining_[track] = 0;
            if (track < 3) {
                auto map_error = mapper_.writePsgToneNoise(
                    track,
                    psg_tone_noise_modes_[track],
                    psg_noise_periods_[track],
                    tick_,
                    writes_);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = mapper_.writePsgPeriod(
                    track,
                    pitch.psg_scc_period,
                    tick_,
                    writes_);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackPatch(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackDetunes(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = psg_modes_[track].keyOn(
                    track,
                    psg_hardware_,
                    master_attenuation_,
                    track_attenuation_[track],
                    true,
                    tick_,
                    mapper_,
                    writes_);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
            } else if (track < 8) {
                auto map_error = mapper_.writeSccPeriod(
                    track,
                    pitch.psg_scc_period,
                    tick_,
                    writes_);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = mapper_.writeSccKey(
                    track,
                    true,
                    tick_,
                    writes_);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackPatch(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackDetunes(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
            } else {
                auto map_error = mapper_.writeOpllPitch(
                    track,
                    pitch.opll,
                    true,
                    tick_,
                    writes_,
                    track_opll_sustain_[track]);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackPatch(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
                map_error = applyTrackDetunes(track);
                if (map_error != MapError::None) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        map_error,
                    };
                }
            }
        } else if (pending_keys_[track] == PendingKey::Off
                   || hang_expired) {
            if (!hang_expired && keyOffHangApplies(track)) {
                key_off_hang_remaining_[track] = track_key_off_hang_[track];
            } else {
                key_off_hang_remaining_[track] = 0;
                NotePitch pitch{};
                if (!notePitch(current_notes_[track], pitch)) {
                    return {
                        tick_,
                        track,
                        SequenceError::None,
                        MapError::InvalidValue,
                    };
                }
                if (track < 3 && !runtime.rateEnvelope()) {
                    psg_sequence_muted_[track] = true;
                    const auto map_error = mapper_.mapMeaningEvent(
                        track,
                        MeaningEvent{
                            tick_,
                            MeaningEventKind::Volume,
                            0,
                            0,
                        },
                        tick_,
                        writes_);
                    if (map_error != MapError::None) {
                        return {
                            tick_,
                            track,
                            SequenceError::None,
                            map_error,
                        };
                    }
                } else if (track >= 3 && track < 8) {
                    // SCC has no hardware envelope.  An @r release is
                    // software volume automation, so keep the SCC key gate
                    // on while the RateEnvelopeRuntime ramps the volume
                    // down.  ForceMuteTrack above remains the hard-stop path.
                    if (!runtime.rateEnvelope()) {
                        const auto map_error = mapper_.writeSccKey(
                            track,
                            false,
                            tick_,
                            writes_);
                        if (map_error != MapError::None) {
                            return {
                                tick_,
                                track,
                                SequenceError::None,
                                map_error,
                            };
                        }
                    }
                } else if (track >= 8) {
                    const auto map_error = mapper_.writeOpllPitch(
                        track,
                        pitch.opll,
                        false,
                        tick_,
                        writes_,
                        track_opll_sustain_[track]);
                    if (map_error != MapError::None) {
                        return {
                            tick_,
                            track,
                            SequenceError::None,
                            map_error,
                        };
                    }
                }
                runtime.keyOff(track < 8);
                if (!runtime.rateEnvelope()) {
                    audition_track_running_[track] = false;
                }
            }
        }
        pending_keys_[track] = PendingKey::None;

        // MGSDRV sequence envelopes have no release phase.  A released PSG
        // audition key must therefore remain silent instead of letting the
        // looping sequence restore its volume on the next 60 Hz tick.
        if (track < 3 && psg_sequence_muted_[track]) {
            continue;
        }

        meaning_events_.clear();
        const auto sequence_error = runtime.processTick(meaning_events_);
        if (sequence_error != SequenceError::None) {
            return {tick_, track, sequence_error, MapError::None};
        }
        for (const auto& event : meaning_events_.events()) {
            auto mapped_event = event;
            if (event.kind == MeaningEventKind::Volume) {
                if (track < 3
                    && psg_modes_[track].hardwareEnabled()) {
                    continue;
                }
                mapped_event.arg0 = sequenceOutputVolume(
                    static_cast<std::uint8_t>(event.arg0),
                    runtime.trackVolume(),
                    master_attenuation_,
                    track_attenuation_[track]);
            } else if (event.kind == MeaningEventKind::RateVolume) {
                if (track < 3
                    && psg_modes_[track].hardwareEnabled()) {
                    continue;
                }
                mapped_event.arg1 = applyCommonAttenuation(
                    static_cast<std::uint8_t>(event.arg1),
                    master_attenuation_,
                    track_attenuation_[track]);
            }
            const auto map_error = mapper_.mapMeaningEvent(
                track,
                mapped_event,
                tick_,
                writes_);
            if (map_error != MapError::None) {
                return {tick_, track, SequenceError::None, map_error};
            }
        }
        if (audition_track_running_[track]) {
            if (track_pitch_sweep_[track].enabled()) {
                const auto sweep_error = applyTrackPitchSweep(track);
                if (sweep_error != MapError::None) {
                    return {tick_, track, SequenceError::None, sweep_error};
                }
            } else {
                const auto lfo_error = applyTrackLfo(track);
                if (lfo_error != MapError::None) {
                    return {tick_, track, SequenceError::None, lfo_error};
                }
            }
        }
    }

    const TickResult result{tick_, 0, SequenceError::None, MapError::None};
    ++tick_;
    return result;
}

MapError RuntimeSession::applyTrackDetunes(std::uint8_t track) {
    const auto apply_delta = [this, track](std::int32_t delta) -> MapError {
        if (delta == 0) {
            return MapError::None;
        }
        return mapper_.mapMeaningEvent(
            track,
            MeaningEvent{
                tick_,
                MeaningEventKind::FrequencyDelta,
                delta,
                0,
            },
            tick_,
            writes_);
    };
    auto error = apply_delta(track_detune_[track]);
    if (error != MapError::None) {
        return error;
    }
    const auto micro_offset = track < 8
        ? psgSccTrackMicroDetuneOffset(track_micro_detune_[track])
        : opllTrackMicroDetuneOffset(
            current_notes_[track], track_micro_detune_[track]);
    if (micro_offset != 0) {
        error = mapper_.mapMeaningEvent(
            track,
            MeaningEvent{
                tick_,
                MeaningEventKind::TrackMicroDetune,
                micro_offset,
                0,
            },
            tick_,
            writes_);
    }
    return error;
}

MapError RuntimeSession::applyTrackPatch(std::uint8_t track) {
    if (!track_patch_[track] || track < 3) {
        return MapError::None;
    }
    return mapper_.mapMeaningEvent(
        track,
        MeaningEvent{
            tick_,
            MeaningEventKind::Patch,
            *track_patch_[track],
            0,
        },
        tick_,
        writes_);
}

MapError RuntimeSession::applyTrackLfo(std::uint8_t track) {
    if (!track_lfo_[track].enabled()) {
        return MapError::None;
    }
    const auto delta = track_lfo_[track].advance();
    if (delta == 0) {
        return MapError::None;
    }
    return mapper_.mapMeaningEvent(
        track,
        MeaningEvent{
            tick_,
            MeaningEventKind::FrequencyDelta,
            delta,
            0,
        },
        tick_,
        writes_);
}

MapError RuntimeSession::applyTrackPitchSweep(std::uint8_t track) {
    if (!track_pitch_sweep_[track].enabled()) {
        return MapError::None;
    }
    const auto delta = track_pitch_sweep_[track].advance();
    if (delta == 0) {
        return MapError::None;
    }
    return mapper_.mapMeaningEvent(
        track,
        MeaningEvent{
            tick_,
            MeaningEventKind::FrequencyDelta,
            delta,
            0,
        },
        tick_,
        writes_);
}

bool RuntimeSession::keyOffHangApplies(std::uint8_t track) const noexcept {
    if (track >= 8 || track_key_off_hang_[track] == 0) {
        return false;
    }
    if (tracks_[track].rateEnvelope()) {
        return false;
    }
    if (track < 3 && psg_modes_[track].hardwareEnabled()) {
        return false;
    }
    return true;
}

}  // namespace mgstc::engine
