"""Validate envelope_compat_manifest.json against the reference implementation."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from tools.mgs_envelope_reference import EnvelopeState

MANIFEST = (
    Path(__file__).resolve().parent / "fixtures" / "envelope_compat_manifest.json"
)


def _normalize_tick_events(events):
    return [{"kind": kind, "args": args} for kind, args in events]


def _compact(state: EnvelopeState, ticks: int):
    result = []
    for _ in range(ticks):
        tick_events = [
            (event.kind, list(event.args)) for event in state.tick()
        ]
        result.append(_normalize_tick_events(tick_events))
    return result


def _volume_ticks(state: EnvelopeState, ticks: int):
    for _ in range(ticks):
        state.tick()
    return [
        [event.tick, event.args[0]]
        for event in state.events
        if event.kind == "volume"
    ]


class EnvelopeCompatManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def test_manifest_has_cases(self):
        self.assertGreaterEqual(len(self.manifest["cases"]), 7)

    def test_cases_match_reference(self):
        for case in self.manifest["cases"]:
            with self.subTest(case_id=case["id"], name=case["name"]):
                bytecode = bytes.fromhex(case["bytecode_hex"])
                state = EnvelopeState(bytecode)
                if "expect_ticks" in case:
                    actual = _compact(state, case["ticks"])
                    self.assertEqual(actual, case["expect_ticks"])
                elif "expect_volume_ticks" in case:
                    actual = _volume_ticks(state, case["ticks"])
                    self.assertEqual(actual, case["expect_volume_ticks"])
                else:
                    self.fail(f"case {case['id']} has no expectations")


if __name__ == "__main__":
    unittest.main()
