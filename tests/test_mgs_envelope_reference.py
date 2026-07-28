import unittest

from tools.mgs_envelope_reference import (
    EnvelopeState,
    NineVoiceRhythmState,
    OPLLRhythmState,
    PSGHardwareEnvelopeState,
    PSGTrackEnvelopeMode,
    RateEnvelopeState,
    sequence_output_volume,
)


class EnvelopeReferenceTests(unittest.TestCase):
    @staticmethod
    def compact(state: EnvelopeState, ticks: int):
        result = []
        for _ in range(ticks):
            result.append([(event.kind, event.args) for event in state.tick()])
        return result

    def test_adjacent_single_count_volumes_are_one_tick_apart(self):
        state = EnvelopeState(bytes([0x0F, 0x0E]))
        self.assertEqual(
            self.compact(state, 3),
            [
                [("volume", (15,))],
                [("volume", (14,))],
                [("volume", (14,))],
            ],
        )

    def test_vgm_observed_key_on_and_four_count_holds(self):
        # TEST.VGM: key-on-relative frames 0, 4, 8, 12, 16.
        state = EnvelopeState(
            bytes.fromhex("EF 04 ED 04 E8 04 E4 04 00")
        )
        for _ in range(17):
            state.tick()
        self.assertEqual(
            [
                (event.tick, event.args[0])
                for event in state.events
                if event.kind == "volume"
            ],
            [(0, 15), (4, 13), (8, 8), (12, 4), (16, 0)],
        )

    def test_hold_count_executes_next_command_when_count_reaches_zero(self):
        state = EnvelopeState(bytes([0xEF, 0x02, 0x0E]))
        self.assertEqual(
            self.compact(state, 4),
            [
                [("volume", (15,))],
                [],
                [("volume", (14,))],
                [("volume", (14,))],
            ],
        )

    def test_patch_and_register_write_do_not_advance_time(self):
        state = EnvelopeState(
            bytes([0x10, 0x03, 0x11, 0x02, 0x15, 0x0F])
        )
        self.assertEqual(
            self.compact(state, 2),
            [
                [
                    ("patch", (3,)),
                    ("register_write", (2, 0x15)),
                    ("volume", (15,)),
                ],
                [("volume", (15,))],
            ],
        )

    def test_ramp_uses_remainder_distribution_and_starts_next_tick(self):
        # Set volume 0, then ramp to 15 over four counts.
        state = EnvelopeState(bytes([0x00, 0x2F, 0x04]))
        self.assertEqual(
            self.compact(state, 6),
            [
                [("volume", (0,))],
                [("volume", (0,))],
                [("volume", (3,))],
                [("volume", (7,))],
                [("volume", (11,))],
                # Final update and terminal application share the tick.
                [("volume", (15,)), ("volume", (15,))],
            ],
        )

    def test_loop_has_no_implicit_wait(self):
        state = EnvelopeState(bytes([0x40, 0x0F, 0x60]))
        self.assertEqual(
            self.compact(state, 4),
            [
                [("volume", (15,))],
                [("volume", (15,))],
                [("volume", (15,))],
                [("volume", (15,))],
            ],
        )

    def test_frequency_changes_are_zero_time_and_cumulative_in_mapping_layer(self):
        state = EnvelopeState(bytes([0x12, 0x03, 0x12, 0xFF, 0x0F]))
        self.assertEqual(
            self.compact(state, 2),
            [
                [
                    ("frequency_delta", (3,)),
                    ("frequency_delta", (-1,)),
                    ("volume", (15,)),
                ],
                [("volume", (15,))],
            ],
        )

    def test_rate_envelope_uses_native_six_parameter_phases(self):
        state = RateEnvelopeState(
            attack_level=100,
            attack_rate=100,
            decay_rate=30,
            sustain_level=180,
            sustain_rate=20,
            release_rate=40,
        )
        self.assertEqual((state.level, state.phase), (100, "attack"))
        self.assertEqual(state.tick().args, (200, 12))
        self.assertEqual(state.tick().args, (255, 15))
        self.assertEqual(state.phase, "decay")
        self.assertEqual(state.tick().args, (225, 14))
        self.assertEqual(state.tick().args, (195, 12))
        self.assertEqual(state.tick().args, (180, 11))
        self.assertEqual(state.phase, "sustain")
        self.assertEqual(state.tick().args, (160, 10))
        state.key_off()
        self.assertEqual(state.tick().args, (120, 7))
        self.assertEqual(state.phase, "release")

    def test_rate_volume_is_scaled_by_track_volume_before_master_volume(self):
        state = RateEnvelopeState(255, 0, 0, 255, 0, 0, track_volume=7)
        self.assertEqual(state.quantized_volume(), 7)
        self.assertEqual(
            state.quantized_volume(
                master_attenuation=2, track_attenuation=1
            ),
            4,
        )

    def test_sequence_volume_uses_additive_track_volume_then_attenuation(self):
        self.assertEqual(sequence_output_volume(15, 15), 15)
        self.assertEqual(sequence_output_volume(12, 10), 7)
        self.assertEqual(sequence_output_volume(4, 8), 0)
        self.assertEqual(
            sequence_output_volume(
                15, 12, master_attenuation=2, track_attenuation=1
            ),
            9,
        )

    def test_rate_key_off_during_attack_waits_until_sustain_for_release(self):
        state = RateEnvelopeState(
            attack_level=0,
            attack_rate=128,
            decay_rate=64,
            sustain_level=128,
            sustain_rate=1,
            release_rate=16,
        )
        state.key_off()
        self.assertEqual(state.tick().args, (128, 8))
        self.assertEqual(state.phase, "attack")
        self.assertEqual(state.tick().args, (255, 15))
        self.assertEqual(state.phase, "decay")
        self.assertEqual(state.tick().args, (191, 11))
        self.assertEqual(state.tick().args, (128, 8))
        self.assertEqual(state.phase, "sustain")
        self.assertEqual(state.tick().args, (112, 7))
        self.assertEqual(state.phase, "release")

    def test_opll_hardware_key_off_does_not_start_software_release(self):
        state = RateEnvelopeState(255, 0, 0, 255, 10, 20)
        state.tick()
        state.tick()
        self.assertEqual(state.phase, "sustain")
        state.key_off(software_release=False)
        self.assertEqual(state.tick().args, (245, 15))
        self.assertEqual(state.phase, "sustain")

    def test_rate_re_key_on_resets_level_phase_and_pending_release(self):
        state = RateEnvelopeState(10, 100, 20, 180, 4, 8)
        state.key_off()
        state.tick()
        state.reset_for_key_on()
        self.assertEqual(state.level, 10)
        self.assertEqual(state.phase, "attack")
        self.assertFalse(state.key_off_pending)
        self.assertEqual(state.tick_index, 0)

    def test_psg_hardware_period_is_shared_and_written_immediately(self):
        shared = PSGHardwareEnvelopeState()
        events = shared.set_period(0x1234)
        self.assertEqual(
            [(event.kind, event.args) for event in events],
            [
                ("psg_register", (11, 0x34)),
                ("psg_register", (12, 0x12)),
            ],
        )

    def test_psg_shape_restarts_on_key_on_and_v_restores_fixed_volume(self):
        shared = PSGHardwareEnvelopeState()
        track = PSGTrackEnvelopeMode()
        track.select_hardware(shared, 10)
        self.assertEqual(track.track_volume, 15)
        track.select_software_envelope()
        self.assertTrue(track.hardware_enabled)
        self.assertEqual(
            [(event.kind, event.args) for event in track.key_on(shared)],
            [
                ("psg_register", (13, 10)),
                ("psg_volume", (0x10,)),
            ],
        )
        track.set_volume(7)
        self.assertFalse(track.hardware_enabled)
        self.assertEqual(track.key_on(shared), [])

    def test_psg_hardware_shape_restart_is_suppressed_at_attenuation_8(self):
        shared = PSGHardwareEnvelopeState(shape=12)
        track = PSGTrackEnvelopeMode(hardware_enabled=True, track_volume=15)
        events = track.key_on(
            shared, master_attenuation=3, track_attenuation=5
        )
        self.assertEqual(
            [(event.kind, event.args) for event in events],
            [("psg_volume", (0x10,))],
        )
        self.assertEqual(track.key_on(shared, output_enabled=False), [])

    def test_opll_rhythm_retrigger_toggles_shared_key_bit_then_sets_it(self):
        rhythm = OPLLRhythmState(key_shadow=0x21)
        events = rhythm.trigger(0x01)
        self.assertEqual(
            [(event.kind, event.args) for event in events],
            [
                ("opll_register", (0x0E, 0x20)),
                ("opll_register", (0x0E, 0x21)),
            ],
        )
        self.assertEqual(rhythm.key_shadow, 0x21)

    def test_opll_rhythm_clear_all_path_writes_mode_then_selected_bits(self):
        rhythm = OPLLRhythmState(key_shadow=0x3F)
        events = rhythm.trigger(0x12, clear_all_first=True)
        self.assertEqual(
            [event.args for event in events],
            [(0x0E, 0x20), (0x0E, 0x32)],
        )

    def test_opll_rhythm_volumes_share_three_register_nibbles(self):
        rhythm = OPLLRhythmState()
        for instrument, volume in enumerate((15, 14, 13, 12, 11)):
            rhythm.set_instrument_volume(instrument, volume)
        events = rhythm.volume_register_events(
            master_attenuation=1, track_attenuation=1
        )
        self.assertEqual(
            [event.args for event in events],
            [
                (0x36, 0x02),
                (0x37, 0x63),
                (0x38, 0x45),
            ],
        )

    def test_nine_voice_rhythm_maps_patch_and_volume_to_paired_nibbles(self):
        rhythm = NineVoiceRhythmState()
        rhythm.apply_lower_volume(8, 3)
        patch_event = rhythm.apply_patch_volume(
            opll_channel=8, mml_patch=4
        )
        self.assertEqual(patch_event.args, (0x37, 0x53))
        rhythm.apply_lower_volume(9, 5)
        patch_event = rhythm.apply_patch_volume(
            opll_channel=9, mml_patch=7
        )
        self.assertEqual(patch_event.args, (0x38, 0x85))

    def test_nine_voice_rhythm_rejects_at_15_for_drum_volume(self):
        rhythm = NineVoiceRhythmState()
        with self.assertRaisesRegex(ValueError, "@15"):
            rhythm.apply_patch_volume(8, 15)
        with self.assertRaisesRegex(ValueError, "@15"):
            rhythm.apply_patch_volume(9, 16)

    def test_nine_voice_rhythm_silences_upper_drum_with_y_not_at_15(self):
        rhythm = NineVoiceRhythmState(
            volume_registers={0x36: 0, 0x37: 0x23, 0x38: 0}
        )
        event = rhythm.silence_patch_controlled_drum(8)
        self.assertEqual(event.kind, "opll_y")
        self.assertEqual(event.args, (0x37, 0xF3))

    def test_nine_voice_rhythm_retrigger_emits_explicit_y_off_on_pair(self):
        rhythm = NineVoiceRhythmState(register_0e=0x21)
        events = rhythm.trigger(0x01)
        self.assertEqual(
            [event.args for event in events],
            [(0x0E, 0x20), (0x0E, 0x21)],
        )


if __name__ == "__main__":
    unittest.main()
