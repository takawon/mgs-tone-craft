#!/usr/bin/env python3
"""Independent behavioral reference for MGS sequence-envelope execution.

This project-authored model was written from observed input/output behavior.
It contains no MGSDRV source code, binary data, disassembly, or extracted code.
It emits logical events before track/master-volume conversion and before
chip-specific register mapping.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Event:
    tick: int
    kind: str
    args: tuple[int, ...]


@dataclass
class EnvelopeState:
    data: bytes
    position: int = 0
    loop_position: int = 0
    # A key-on reset sets this to 1. The envelope scheduler later in that same
    # tick decrements it to zero and runs the first opcode.
    wait: int = 1
    ramp_total: int = 0
    ramp_magnitude: int = 0
    ramp_direction: int = 0
    ramp_remainder: int = 0
    volume: int = 0
    tick_index: int = 0
    events: list[Event] = field(default_factory=list)

    def reset_for_key_on(self) -> None:
        """Apply the sequence-envelope part of the observed key-on behavior."""
        self.wait = 1
        self.ramp_total = 0
        self.position = 0
        self.volume = 0

    def emit(self, kind: str, *args: int) -> None:
        self.events.append(Event(self.tick_index, kind, tuple(args)))

    def tick(self) -> list[Event]:
        """Execute one driver tick worth of envelope work."""
        event_start = len(self.events)

        if self.wait:
            if self.ramp_total:
                numerator = self.ramp_remainder + self.ramp_magnitude
                quotient, self.ramp_remainder = divmod(
                    numerator, self.ramp_total
                )
                self.volume += self.ramp_direction * quotient
                self.emit("volume", self.volume)
            self.wait -= 1
            if self.wait:
                self.tick_index += 1
                return self.events[event_start:]
            self.ramp_total = 0

        while True:
            if self.position == len(self.data):
                # The observed behavior keeps applying the terminal volume.
                self.emit("volume", self.volume)
                break
            if not 0 <= self.position < len(self.data):
                raise ValueError(f"envelope position out of range: {self.position}")

            opcode = self.data[self.position]
            self.position += 1

            if opcode <= 0x0F:
                self.volume = opcode
                self.wait = 1
                self.emit("volume", self.volume)
                break
            if opcode == 0x10:
                self.emit("patch", self._read_byte())
                continue
            if opcode == 0x11:
                self.emit("register_write", self._read_byte(), self._read_byte())
                continue
            if opcode == 0x12:
                value = self._read_byte()
                self.emit(
                    "frequency_delta",
                    value - 256 if value >= 128 else value,
                )
                continue
            if 0x20 <= opcode <= 0x2F:
                target = opcode & 0x0F
                delta = target - self.volume
                count = self._read_byte()
                self.ramp_total = count
                self.ramp_magnitude = abs(delta)
                self.ramp_direction = 1 if delta >= 0 else -1
                self.ramp_remainder = 0
                self.wait = count
                self.emit("volume", self.volume)
                break
            if opcode == 0x40:
                self.loop_position = self.position
                continue
            if opcode == 0x60:
                self.position = self.loop_position
                continue
            if 0x80 <= opcode <= 0x9F:
                self.emit("noise", opcode & 0x1F)
                continue
            if 0xA0 <= opcode <= 0xA3:
                self.emit("tone_noise_mode", opcode & 0x03)
                continue
            if 0xE0 <= opcode <= 0xEF:
                self.volume = opcode & 0x0F
                self.wait = self._read_byte()
                self.emit("volume", self.volume)
                break
            raise ValueError(
                f"unknown envelope opcode 0x{opcode:02X} "
                f"at +0x{self.position - 1:X}"
            )

        self.tick_index += 1
        return self.events[event_start:]

    def _read_byte(self) -> int:
        if self.position >= len(self.data):
            raise ValueError("truncated envelope command")
        value = self.data[self.position]
        self.position += 1
        return value


def simulate(data: bytes, ticks: int) -> list[Event]:
    state = EnvelopeState(data)
    for _ in range(ticks):
        state.tick()
    return state.events


def apply_common_attenuation(
    level: int,
    master_attenuation: int = 0,
    track_attenuation: int = 0,
) -> int:
    """Apply the observed MGS attenuation behavior to a 0..15 level."""
    for name, value in (
        ("level", level),
        ("master_attenuation", master_attenuation),
        ("track_attenuation", track_attenuation),
    ):
        if not 0 <= value <= 15:
            raise ValueError(f"{name} must be in 0..15")
    return max(0, level - master_attenuation - track_attenuation)


def sequence_output_volume(
    envelope_volume: int,
    track_volume: int,
    master_attenuation: int = 0,
    track_attenuation: int = 0,
) -> int:
    """Return @e's final logical 0..15 volume."""
    if not 0 <= envelope_volume <= 15:
        raise ValueError("envelope_volume must be in 0..15")
    if not 0 <= track_volume <= 15:
        raise ValueError("track_volume must be in 0..15")
    premaster = max(0, envelope_volume + track_volume - 15)
    return apply_common_attenuation(
        premaster, master_attenuation, track_attenuation
    )


@dataclass
class PSGHardwareEnvelopeState:
    """Shared YM2149 hardware-envelope state used by MGS playback.

    Period and shape belong to the chip. Whether hardware EG is selected is
    held independently by each PSG track.
    """

    period: int = 1
    shape: int = 0

    def set_period(self, period: int) -> list[Event]:
        if not 1 <= period <= 65535:
            raise ValueError("period must be in 1..65535")
        self.period = period
        return [
            Event(0, "psg_register", (11, period & 0xFF)),
            Event(0, "psg_register", (12, period >> 8)),
        ]


@dataclass
class PSGTrackEnvelopeMode:
    hardware_enabled: bool = False
    track_volume: int = 0

    def select_hardware(
        self, shared: PSGHardwareEnvelopeState, shape: int
    ) -> None:
        if not 0 <= shape <= 15:
            raise ValueError("shape must be in 0..15")
        shared.shape = shape
        self.hardware_enabled = True
        self.track_volume = 15

    def set_volume(self, volume: int) -> None:
        if not 0 <= volume <= 15:
            raise ValueError("volume must be in 0..15")
        self.track_volume = volume
        self.hardware_enabled = False

    def select_software_envelope(self) -> None:
        """MGSDRV preserves hardware mode when @e/@r is selected."""

    def key_on(
        self,
        shared: PSGHardwareEnvelopeState,
        master_attenuation: int = 0,
        track_attenuation: int = 0,
        output_enabled: bool = True,
    ) -> list[Event]:
        if not self.hardware_enabled:
            return []
        if not output_enabled:
            return []
        events: list[Event] = []
        if master_attenuation + track_attenuation < 8:
            events.append(Event(0, "psg_register", (13, shared.shape)))
        # Bit 4, rather than a 0..15 fixed level, selects the shared EG.
        events.append(Event(0, "psg_volume", (0x10,)))
        return events


@dataclass
class OPLLRhythmState:
    """MGSDRV standard 6-melody + rhythm-track register model."""

    key_shadow: int = 0x20
    volume_attenuation: list[int] = field(
        default_factory=lambda: [15, 15, 15, 15, 15]
    )

    # MGSC target order: bass drum, snare, tom, cymbal, hi-hat.
    # Each item is (register, high_nibble).
    VOLUME_FIELDS = (
        (0x36, False),
        (0x37, False),
        (0x38, True),
        (0x38, False),
        (0x37, True),
    )

    def trigger(self, instrument_mask: int, clear_all_first: bool = False) -> list[Event]:
        if not 0 <= instrument_mask <= 0x1F:
            raise ValueError("instrument_mask must be in 0..31")
        if instrument_mask == 0:
            return []
        if clear_all_first:
            first = 0x20
        else:
            first = (self.key_shadow ^ instrument_mask) | 0x20
        second = first | instrument_mask
        self.key_shadow = second
        return [
            Event(0, "opll_register", (0x0E, first)),
            Event(0, "opll_register", (0x0E, second)),
        ]

    def set_instrument_volume(self, instrument: int, volume: int) -> None:
        if not 0 <= instrument < 5:
            raise ValueError("instrument must be in 0..4")
        if not 0 <= volume <= 15:
            raise ValueError("volume must be in 0..15")
        self.volume_attenuation[instrument] = 15 - volume

    def volume_register_events(
        self,
        master_attenuation: int = 0,
        track_attenuation: int = 0,
    ) -> list[Event]:
        if not 0 <= master_attenuation <= 15:
            raise ValueError("master_attenuation must be in 0..15")
        if not 0 <= track_attenuation <= 15:
            raise ValueError("track_attenuation must be in 0..15")
        registers = {0x36: 0, 0x37: 0, 0x38: 0}
        extra = master_attenuation + track_attenuation
        for instrument, (register, high_nibble) in enumerate(
            self.VOLUME_FIELDS
        ):
            attenuation = min(
                15, self.volume_attenuation[instrument] + extra
            )
            shift = 4 if high_nibble else 0
            registers[register] |= attenuation << shift
        return [
            Event(0, "opll_register", (register, registers[register]))
            for register in (0x36, 0x37, 0x38)
        ]


@dataclass
class NineVoiceRhythmState:
    """Editor shadow for the MGS 9-voice/YM2413-rhythm technique.

    The observed driver does not shadow `y` writes. This state exists to generate
    explicit, full-byte y commands while preserving paired drum-volume nibbles.
    """

    register_0e: int = 0
    volume_registers: dict[int, int] = field(
        default_factory=lambda: {0x36: 0, 0x37: 0, 0x38: 0}
    )

    PATCH_VOLUME_TARGETS = {
        8: (0x37, "hi_hat"),
        9: (0x38, "tom"),
    }
    LOWER_VOLUME_TARGETS = {
        7: (0x36, "bass_drum"),
        8: (0x37, "snare_drum"),
        9: (0x38, "cymbal"),
    }

    def enter_rhythm_mode(self) -> Event:
        self.register_0e |= 0x20
        return Event(0, "opll_y", (0x0E, self.register_0e))

    def trigger(self, instrument_mask: int) -> list[Event]:
        if not 0 <= instrument_mask <= 0x1F:
            raise ValueError("instrument_mask must be in 0..31")
        off = self.register_0e & ~instrument_mask
        on = off | 0x20 | instrument_mask
        self.register_0e = on
        return [
            Event(0, "opll_y", (0x0E, off | 0x20)),
            Event(0, "opll_y", (0x0E, on)),
        ]

    def apply_patch_volume(
        self,
        opll_channel: int,
        mml_patch: int,
    ) -> Event:
        """Model ordinary @0..14 on Ch.8/9 in 9-voice rhythm mode.

        MGSC maps @0..14 to YM2413 patch nibbles 1..15. In rhythm mode that
        nibble is the Hi-Hat/Tom attenuation. @15 would resolve to nibble 0
        (maximum drum volume), but enters the shared original-tone path and
        is therefore never a valid drum-volume control.
        """
        if not 0 <= mml_patch <= 14:
            raise ValueError(
                "@15 and later original-tone slots cannot be used as "
                "9-voice rhythm volume controls"
            )
        try:
            register, _ = self.PATCH_VOLUME_TARGETS[opll_channel]
        except KeyError as error:
            raise ValueError("patch-volume channel must be OPLL Ch.8 or Ch.9") from error
        current = self.volume_registers[register]
        resolved_patch_nibble = mml_patch + 1
        value = (resolved_patch_nibble << 4) | (current & 0x0F)
        self.volume_registers[register] = value
        return Event(0, "opll_patch_effect", (register, value))

    def apply_lower_volume(self, opll_channel: int, attenuation: int) -> Event:
        if not 0 <= attenuation <= 15:
            raise ValueError("attenuation must be in 0..15")
        try:
            register, _ = self.LOWER_VOLUME_TARGETS[opll_channel]
        except KeyError as error:
            raise ValueError("lower-volume channel must be OPLL Ch.7..Ch.9") from error
        current = self.volume_registers[register]
        value = (current & 0xF0) | attenuation
        self.volume_registers[register] = value
        return Event(0, "opll_volume_effect", (register, value))

    def silence_patch_controlled_drum(self, opll_channel: int) -> Event:
        """Silence an upper-nibble drum with y while avoiding unsafe @15."""
        try:
            register, _ = self.PATCH_VOLUME_TARGETS[opll_channel]
        except KeyError as error:
            raise ValueError("silence channel must be OPLL Ch.8 or Ch.9") from error
        value = self.volume_registers[register] | 0xF0
        self.volume_registers[register] = value
        return Event(0, "opll_y", (register, value))


@dataclass
class RateEnvelopeState:
    """Independent model of the observed MGS @r envelope behavior.

    The six parameters remain in their native 0..255 domain. `phase` is an
    editor-facing name for the observed phase state; it is not serialized.
    """

    attack_level: int
    attack_rate: int
    decay_rate: int
    sustain_level: int
    sustain_rate: int
    release_rate: int
    track_volume: int = 15
    level: int = 0
    phase: str = "attack"
    tick_index: int = 0
    key_off_pending: bool = False

    def __post_init__(self) -> None:
        for name in (
            "attack_level",
            "attack_rate",
            "decay_rate",
            "sustain_level",
            "sustain_rate",
            "release_rate",
        ):
            value = getattr(self, name)
            if not 0 <= value <= 255:
                raise ValueError(f"{name} must be in 0..255")
        if not 0 <= self.track_volume <= 15:
            raise ValueError("track_volume must be in 0..15")
        self.reset_for_key_on()

    def reset_for_key_on(self) -> None:
        self.level = self.attack_level
        self.phase = "attack"
        self.tick_index = 0
        self.key_off_pending = False

    def key_off(self, software_release: bool = True) -> None:
        """Apply the observed source-specific key-off behavior.

        PSG/SCC set a pending software-release flag. If key-off happens during
        attack or decay, those phases finish through SL before RR begins.
        OPLL uses the YM2413 hardware key-off path and does not set this flag.
        """
        if software_release:
            self.key_off_pending = True

    def tick(self) -> Event:
        if self.key_off_pending and self.phase == "sustain":
            self.phase = "release"
            self.key_off_pending = False

        if self.phase == "attack":
            self.level = min(255, self.level + self.attack_rate)
            if self.level == 255:
                self.phase = "decay"
        elif self.phase == "decay":
            self.level = max(
                self.sustain_level, self.level - self.decay_rate
            )
            if self.level == self.sustain_level:
                self.phase = "sustain"
        elif self.phase == "sustain":
            self.level = max(0, self.level - self.sustain_rate)
        elif self.phase == "release":
            self.level = max(0, self.level - self.release_rate)
        else:
            raise ValueError(f"unknown rate-envelope phase: {self.phase}")

        event = Event(
            self.tick_index,
            "rate_volume",
            (self.level, self.quantized_volume()),
        )
        self.tick_index += 1
        return event

    def quantized_volume(
        self,
        master_attenuation: int = 0,
        track_attenuation: int = 0,
    ) -> int:
        """Return the observed final logical 0..15 conversion.

        The 8-bit @r level is scaled by (track volume + 1), quantized to its
        high byte, then passed through the common attenuation path.
        """
        premaster = (self.level * (self.track_volume + 1)) >> 8
        return apply_common_attenuation(
            premaster, master_attenuation, track_attenuation
        )
