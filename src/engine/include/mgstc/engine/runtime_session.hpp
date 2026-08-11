#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "mgstc/engine/envelope_rate.hpp"
#include "mgstc/engine/envelope_sequence.hpp"
#include "mgstc/engine/register_mapper.hpp"
#include "mgstc/engine/shared_state.hpp"
#include "mgstc/engine/track_runtime.hpp"

namespace mgstc::engine {

struct TickResult {
    Tick tick{};
    std::uint8_t track{};
    SequenceError sequence_error{SequenceError::None};
    MapError map_error{MapError::None};

    [[nodiscard]] bool ok() const noexcept {
        return sequence_error == SequenceError::None
            && map_error == MapError::None;
    }
};

class RuntimeSession {
public:
    static constexpr std::size_t kTrackCount = 17;
    static constexpr std::size_t kDefaultMaxMeaningEvents = 8'192;
    static constexpr std::size_t kDefaultMaxRegisterWrites = 8'192;

    explicit RuntimeSession(
        std::size_t max_meaning_events = kDefaultMaxMeaningEvents,
        std::size_t max_register_writes = kDefaultMaxRegisterWrites);

    [[nodiscard]] bool setSequenceEnvelope(
        std::uint8_t track,
        std::vector<std::uint8_t> bytecode);
    [[nodiscard]] bool setCompositeSequenceEnvelopes(
        std::uint8_t track,
        std::vector<std::uint8_t> volume_bytecode,
        std::vector<std::uint8_t> pitch_bytecode,
        std::vector<std::uint8_t> timbre_bytecode);
    [[nodiscard]] bool setRateEnvelope(
        std::uint8_t track,
        RateEnvelopeDefinition definition,
        std::uint8_t track_volume = 15);
    [[nodiscard]] bool clearTrack(std::uint8_t track) noexcept;

    [[nodiscard]] bool setTrackVolume(
        std::uint8_t track,
        std::uint8_t volume) noexcept;
    [[nodiscard]] bool setTrackAttenuation(
        std::uint8_t track,
        std::uint8_t attenuation) noexcept;
    [[nodiscard]] bool setMasterAttenuation(
        std::uint8_t attenuation) noexcept;

    [[nodiscard]] bool selectPsgHardwareEnvelope(
        std::uint8_t track,
        std::uint8_t shape) noexcept;
    [[nodiscard]] bool setPsgFixedVolume(
        std::uint8_t track,
        std::uint8_t volume) noexcept;
    [[nodiscard]] bool queuePsgHardwareEnvelopePeriod(
        std::uint8_t source_track,
        std::uint16_t period) noexcept;
    [[nodiscard]] bool setPsgToneNoise(
        std::uint8_t track,
        std::uint8_t mode,
        std::uint8_t noise) noexcept;
    [[nodiscard]] bool queueNoteOn(
        std::uint8_t track,
        std::uint8_t midi_note) noexcept;
    [[nodiscard]] bool queueKeyOn(std::uint8_t track) noexcept;
    [[nodiscard]] bool queueKeyOff(std::uint8_t track) noexcept;
    // Force the track silent after key-off hang (e.g. OPLL RR=0).
    [[nodiscard]] bool forceMuteTrack(std::uint8_t track) noexcept;

    // Program snapshots are prepared before they are auditioned.  Gating
    // prevents their envelopes from producing writes until NoteOn arrives.
    void gateUntilNoteOn() noexcept;
    void resetForKeyOn() noexcept;
    [[nodiscard]] TickResult processTick();

    [[nodiscard]] Tick tick() const noexcept {
        return tick_;
    }

    [[nodiscard]] std::span<const RegisterWrite> writes() const noexcept {
        return writes_.writes();
    }

    [[nodiscard]] RegisterMapper& mapper() noexcept {
        return mapper_;
    }

    [[nodiscard]] const RegisterMapper& mapper() const noexcept {
        return mapper_;
    }

private:
    enum class PendingKey : std::uint8_t {
        None,
        On,
        Off,
    };

    Tick tick_{};
    std::array<TrackRuntime, kTrackCount> tracks_{};
    std::array<std::uint8_t, kTrackCount> track_attenuation_{};
    std::array<PendingKey, kTrackCount> pending_keys_{};
    std::array<bool, kTrackCount> audition_track_running_{};
    std::array<bool, kTrackCount> force_mute_pending_{};
    std::array<bool, 3> psg_sequence_muted_{};
    std::array<std::uint8_t, kTrackCount> current_notes_{};
    std::array<std::uint8_t, 3> psg_tone_noise_modes_{};
    std::array<std::uint8_t, 3> psg_noise_periods_{};
    std::array<PsgTrackEnvelopeMode, 3> psg_modes_{};
    PsgHardwareEnvelopeState psg_hardware_{};
    std::uint8_t master_attenuation_{};
    bool psg_period_pending_{};
    std::uint8_t psg_period_source_track_{};
    std::uint16_t pending_psg_period_{1};
    bool audition_gated_{};
    EventBuffer meaning_events_;
    RegisterWriteBuffer writes_;
    RegisterMapper mapper_;
};

}  // namespace mgstc::engine
