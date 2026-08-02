#include "mgstc/engine/runtime_session.hpp"

#include <utility>

#include "mgstc/engine/volume.hpp"

namespace mgstc::engine {

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

void RuntimeSession::gateUntilNoteOn() noexcept {
    audition_gated_ = true;
    audition_track_running_.fill(false);
}

void RuntimeSession::resetForKeyOn() noexcept {
    tick_ = 0;
    mapper_.reset();
    meaning_events_.clear();
    writes_.clear();
    for (auto& track : tracks_) {
        track.resetForKeyOn();
    }
    pending_keys_.fill(PendingKey::None);
    audition_track_running_.fill(false);
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
        if (audition_gated_
            && !audition_track_running_[track]
            && pending_keys_[track] == PendingKey::None) {
            continue;
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
            } else {
                const auto map_error = mapper_.writeOpllPitch(
                    track,
                    pitch.opll,
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
            }
        } else if (pending_keys_[track] == PendingKey::Off) {
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
            } else if (track >= 8) {
                const auto map_error = mapper_.writeOpllPitch(
                    track,
                    pitch.opll,
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
            runtime.keyOff(track < 8);
            if (!runtime.rateEnvelope()) {
                audition_track_running_[track] = false;
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
    }

    const TickResult result{tick_, 0, SequenceError::None, MapError::None};
    ++tick_;
    return result;
}

}  // namespace mgstc::engine
