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

import vela_gen

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
        """Keep the command stream after COP1 optimizer config and NOPs."""
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
        """Fail when payload does not start with the COP1 fourcc."""
        with self.assertRaisesRegex(ValueError, "fourcc"):
            vela_gen.unwrap_command_stream(_words(*STREAM))

    def test_rejects_a_partial_word(self) -> None:
        """Fail when payload byte length is not 4-byte aligned."""
        with self.assertRaisesRegex(ValueError, "whole number"):
            vela_gen.unwrap_command_stream(_payload(*_command_stream(STREAM)) + b"\x00")

    def test_rejects_an_unknown_action(self) -> None:
        """Fail on an unrecognized COP1 driver action tag."""
        with self.assertRaisesRegex(ValueError, "unknown driver action 8"):
            vela_gen.unwrap_command_stream(_payload(0x3008))

    def test_rejects_a_section_past_the_end(self) -> None:
        """Fail when COMMAND_STREAM length overruns the payload."""
        payload = _payload(vela_gen.COP_COMMAND_STREAM | (9 << 16), *STREAM)
        with self.assertRaisesRegex(ValueError, "runs past"):
            vela_gen.unwrap_command_stream(payload)

    def test_rejects_a_truncated_optimizer_config(self) -> None:
        """Fail when a trailing optimizer config entry lacks its words."""
        payload = _payload(*_command_stream(STREAM), vela_gen.COP_OPTIMIZER_CONFIG)
        with self.assertRaisesRegex(ValueError, "past the end"):
            vela_gen.unwrap_command_stream(payload)

    def test_rejects_no_stream(self) -> None:
        """Fail when no COMMAND_STREAM section appears."""
        with self.assertRaisesRegex(ValueError, "found 0"):
            vela_gen.unwrap_command_stream(_payload(vela_gen.COP_NOP))

    def test_rejects_two_streams(self) -> None:
        """Fail when more than one COMMAND_STREAM section appears."""
        payload = _payload(*_command_stream(STREAM), *_command_stream(STREAM))
        with self.assertRaisesRegex(ValueError, "found 2"):
            vela_gen.unwrap_command_stream(payload)


class PlaceIoTest(unittest.TestCase):
    """place_io maps the input and output into the scratch region."""

    def test_offline_offsets_land_in_the_scratch_slot(self) -> None:
        """Place input and output in the scratch BASEP slot when offsets exist."""
        slots = [("weights", -1), ("scratch", 0), ("scratch", 0), ("input", 256), ("output", 0)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (1, 256), "output": (1, 0)})

    def test_offsets_are_relative_to_the_scratch_tensor(self) -> None:
        """Subtract non-zero scratch base offset from I/O offsets."""
        slots = [("scratch", 64), ("input", 320), ("output", 64)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (0, 256), "output": (0, 0)})

    def test_no_metadata_keeps_each_tensor_in_its_own_slot(self) -> None:
        """Keep I/O tensors in their own slots when offline offsets are absent."""
        slots = [("weights", -1), ("scratch", -1), ("input", -1), ("output", -1)]
        self.assertEqual(vela_gen.place_io(slots), {"input": (2, 0), "output": (3, 0)})

    def test_rejects_a_tensor_before_the_scratch(self) -> None:
        """Reject an I/O tensor offset lower than the scratch arena base."""
        with self.assertRaisesRegex(ValueError, "before the scratch"):
            vela_gen.place_io([("scratch", 128), ("input", 0)])

    def test_rejects_two_inputs(self) -> None:
        """Reject networks declaring more than one input tensor."""
        with self.assertRaisesRegex(ValueError, "more than one input"):
            vela_gen.place_io([("scratch", 0), ("input", 0), ("input", 256)])


if __name__ == "__main__":
    unittest.main()
