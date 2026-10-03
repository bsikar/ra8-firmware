# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Tests for vela_gen's COP1 unwrap and input/output placement.

Run with: python3 -m unittest discover -s tools/vela/tests
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import vela_gen  # noqa: E402

STOP = 0xFFFF0000
STREAM = [0x0001010F, 0x00004000, STOP]


def _words(*values: int) -> bytes:
    return struct.pack(f"<{len(values)}I", *values)


def _payload(*actions: int) -> bytes:
    return _words(vela_gen.COP_FOURCC, *actions)


def _command_stream(stream: list[int]) -> list[int]:
    return [vela_gen.COP_COMMAND_STREAM | (len(stream) << 16), *stream]


class UnwrapCommandStreamTest(unittest.TestCase):
    """unwrap_command_stream keeps only the COMMAND_STREAM section."""

    def test_vela_layout_yields_the_register_stream(self) -> None:
        payload = _payload(
            vela_gen.COP_OPTIMIZER_CONFIG | (0x10 << 16),
            0x3008,
            0x10066001,
            vela_gen.COP_NOP,
            vela_gen.COP_NOP,
            vela_gen.COP_NOP,
            *_command_stream(STREAM),
        )
        self.assertEqual(vela_gen.unwrap_command_stream(payload), _words(*STREAM))

    def test_rejects_a_missing_fourcc(self) -> None:
        with self.assertRaisesRegex(ValueError, "fourcc"):
            vela_gen.unwrap_command_stream(_words(*STREAM))

    def test_rejects_a_partial_word(self) -> None:
        with self.assertRaisesRegex(ValueError, "whole number"):
            vela_gen.unwrap_command_stream(_payload(*_command_stream(STREAM)) + b"\x00")

    def test_rejects_an_unknown_action(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown driver action 8"):
            vela_gen.unwrap_command_stream(_payload(0x3008))

    def test_rejects_a_section_past_the_end(self) -> None:
        payload = _payload(vela_gen.COP_COMMAND_STREAM | (9 << 16), *STREAM)
        with self.assertRaisesRegex(ValueError, "runs past"):
            vela_gen.unwrap_command_stream(payload)

    def test_rejects_a_truncated_optimizer_config(self) -> None:
        payload = _payload(*_command_stream(STREAM), vela_gen.COP_OPTIMIZER_CONFIG)
        with self.assertRaisesRegex(ValueError, "past the end"):
            vela_gen.unwrap_command_stream(payload)

    def test_rejects_no_stream(self) -> None:
        with self.assertRaisesRegex(ValueError, "found 0"):
            vela_gen.unwrap_command_stream(_payload(vela_gen.COP_NOP))

    def test_rejects_two_streams(self) -> None:
        payload = _payload(*_command_stream(STREAM), *_command_stream(STREAM))
        with self.assertRaisesRegex(ValueError, "found 2"):
            vela_gen.unwrap_command_stream(payload)



class PlaceIoTest(unittest.TestCase):
    """place_io maps the input and output into the scratch region."""

    def test_offline_offsets_land_in_the_scratch_slot(self) -> None:
        slots = [("weights", -1), ("scratch", 0), ("scratch", 0), ("input", 256), ("output", 0)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (1, 256), "output": (1, 0)})

    def test_offsets_are_relative_to_the_scratch_tensor(self) -> None:
        slots = [("scratch", 64), ("input", 320), ("output", 64)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (0, 256), "output": (0, 0)})

    def test_no_metadata_keeps_each_tensor_in_its_own_slot(self) -> None:
        slots = [("weights", -1), ("scratch", -1), ("input", -1), ("output", -1)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (2, 0), "output": (3, 0)})

    def test_rejects_a_tensor_before_the_scratch(self) -> None:
        with self.assertRaisesRegex(ValueError, "before the scratch"):
            vela_gen.place_io([("scratch", 128), ("input", 0)])

    def test_rejects_two_inputs(self) -> None:
        with self.assertRaisesRegex(ValueError, "more than one input"):
            vela_gen.place_io([("scratch", 0), ("input", 0), ("input", 256)])


if __name__ == "__main__":
    unittest.main()
