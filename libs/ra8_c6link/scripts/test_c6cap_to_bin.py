#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Host tests for ``c6cap_to_bin`` capture-log parsing and fixture packing."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import c6cap_to_bin

TEST_PATTERN = b"\x01\x02\x03\x04"
FULL_FRAME = b"\xab" * c6cap_to_bin.FRAME_BYTES
OVERSIZED_HEX = "aa" * (c6cap_to_bin.FRAME_BYTES + 1)


class ExpectationError(AssertionError):
    """Raised when a capture-converter test assertion does not hold."""


def _expect_eq(actual: object, expected: object, label: str) -> None:
    """Fail with a contextual mismatch message when two values differ."""
    if actual != expected:
        detail = f"{label}: got {actual!r}, expected {expected!r}"
        raise ExpectationError(detail)


def _expect_in(needle: str, haystack: str, label: str) -> None:
    """Fail unless ``needle`` appears inside ``haystack``."""
    if needle not in haystack:
        detail = f"{label}: missing {needle!r} in {haystack!r}"
        raise ExpectationError(detail)


def _expect_capture_error(fn: Callable[[], object], fragment: str, label: str) -> None:
    """Fail unless invoking ``fn`` raises ``CaptureError`` matching ``fragment``."""
    try:
        fn()
    except c6cap_to_bin.CaptureError as err:
        _expect_in(fragment, str(err), label)
        return
    detail = f"{label}: CaptureError not raised"
    raise ExpectationError(detail)


class C6CapToBinTest(unittest.TestCase):
    """Verify record parsing, frame padding, transaction grouping, and CLI."""

    def test_records_filters_prefixes_and_rejects_malformed(self) -> None:
        """Non-capture noise is ignored, prefixed lines parse, bad lines raise."""
        lines = [
            "booting ra8...\n",
            "[00:01.200] c6cap 0 hs 1\n",
            "c6cap 0 tx 0200\n",
            "c6cap 0 rx\n",
        ]
        parsed = list(c6cap_to_bin.records(lines))
        _expect_eq(
            parsed,
            [(2, 0, "hs", "1"), (3, 0, "tx", "0200"), (4, 0, "rx", "")],
            "parsed records",
        )
        _expect_capture_error(
            lambda: list(c6cap_to_bin.records(["c6cap nope tx aa\n"])),
            "malformed",
            "non-digit seq",
        )
        _expect_capture_error(
            lambda: list(c6cap_to_bin.records(["c6cap 1\n"])),
            "malformed",
            "too few fields",
        )
        _expect_capture_error(
            lambda: list(c6cap_to_bin.records(["c6cap 1 tx aa extra\n"])),
            "malformed",
            "too many fields",
        )

    def test_frame_pads_zeroes_and_rejects_invalid_hex_or_oversize(self) -> None:
        """Frames zero-pad to 1600 bytes and reject malformed hex or >1600 B."""
        _expect_eq(c6cap_to_bin.frame(1, ""), b"\x00" * c6cap_to_bin.FRAME_BYTES, "empty frame")
        _expect_eq(
            c6cap_to_bin.frame(2, TEST_PATTERN.hex()),
            TEST_PATTERN.ljust(c6cap_to_bin.FRAME_BYTES, b"\x00"),
            "padded frame",
        )
        _expect_eq(
            c6cap_to_bin.frame(3, FULL_FRAME.hex()),
            FULL_FRAME,
            "full 1600-byte frame",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.frame(4, "abc"),
            "bad hex",
            "odd-length hex",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.frame(5, OVERSIZED_HEX),
            "over 1600",
            "oversized frame",
        )

    def test_transactions_ignores_handshake_and_rejects_bad_or_dup_kinds(self) -> None:
        """Handshake edges are skipped while unknown/duplicate kinds fail."""
        txns = c6cap_to_bin.transactions(
            [
                "c6cap 7 hs 0",
                "c6cap 7 hs 1",
                "c6cap 7 tx deadbeef",
                "c6cap 7 rx cafe",
            ]
        )
        _expect_eq(list(txns.keys()), [7], "transaction keys")
        _expect_eq(
            txns[7]["tx"],
            b"\xde\xad\xbe\xef".ljust(c6cap_to_bin.FRAME_BYTES, b"\x00"),
            "tx bytes",
        )
        _expect_eq(
            txns[7]["rx"],
            b"\xca\xfe".ljust(c6cap_to_bin.FRAME_BYTES, b"\x00"),
            "rx bytes",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.transactions(["c6cap 1 foo aa"]),
            "unknown record kind",
            "unknown kind",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.transactions(["c6cap 1 tx aa", "c6cap 1 tx bb"]),
            "second tx",
            "duplicate tx",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.transactions(["c6cap 1 rx aa", "c6cap 1 rx bb"]),
            "second rx",
            "duplicate rx",
        )

    def test_fixture_slices_range_skips_missing_rx_and_rejects_gaps(self) -> None:
        """Range selection packs tx+rx in order, warns on missing rx, rejects gaps."""
        lines = [
            "c6cap 1 tx 11",
            "c6cap 1 rx 22",
            "c6cap 2 tx 33",
            "c6cap 2 rx 44",
            "c6cap 3 tx 55",
            "c6cap 4 tx 66",
            "c6cap 4 rx 77",
        ]
        warn_buf = io.StringIO()
        blob = c6cap_to_bin.fixture(lines, first=2, last=4, warn=warn_buf)
        _expect_eq(len(blob), 4 * c6cap_to_bin.FRAME_BYTES, "packed fixture length")
        _expect_eq(blob[0:1], b"\x33", "seq 2 tx byte 0")
        _expect_eq(blob[1600:1601], b"\x44", "seq 2 rx byte 0")
        _expect_eq(blob[3200:3201], b"\x66", "seq 4 tx byte 0")
        _expect_eq(blob[4800:4801], b"\x77", "seq 4 rx byte 0")
        _expect_in(
            "transaction 3 failed (no rx), skipped", warn_buf.getvalue(), "missing rx warning"
        )
        _expect_eq(
            len(c6cap_to_bin.fixture(lines[:4], first=2, warn=io.StringIO())),
            2 * c6cap_to_bin.FRAME_BYTES,
            "first-only bound",
        )
        _expect_eq(
            len(c6cap_to_bin.fixture(lines[:4], last=1, warn=io.StringIO())),
            2 * c6cap_to_bin.FRAME_BYTES,
            "last-only bound",
        )

        _expect_capture_error(
            lambda: c6cap_to_bin.fixture(lines, first=99, warn=io.StringIO()),
            "no transactions",
            "empty range",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.fixture(
                ["c6cap 1 tx aa", "c6cap 1 rx bb", "c6cap 3 tx cc"],
                warn=io.StringIO(),
            ),
            "2..2 are missing",
            "gap between 1 and 3",
        )
        _expect_capture_error(
            lambda: c6cap_to_bin.fixture(["c6cap 5 rx aa"], warn=io.StringIO()),
            "no tx",
            "rx without tx",
        )

    def test_cli_roundtrip_and_failure_exit_code(self) -> None:
        """The CLI writes packed fixtures on valid input and exits 1 on bad input."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            log_path = tmp_dir / "console.log"
            out_path = tmp_dir / "scan.bin"
            log_path.write_text(
                (
                    "c6_wifi: starting\n"
                    "c6cap 10 tx aa\n"
                    "c6cap 10 rx bb\n"
                    "c6cap 11 tx cc\n"
                    "c6cap 11 rx dd\n"
                ),
                encoding="utf-8",
            )
            err_ok = io.StringIO()
            with contextlib.redirect_stderr(err_ok):
                rc = c6cap_to_bin.main(
                    [str(log_path), str(out_path), "--first", "11", "--last", "11"]
                )
            _expect_eq(rc, 0, "valid CLI exit code")
            packed = out_path.read_bytes()
            _expect_eq(len(packed), 2 * c6cap_to_bin.FRAME_BYTES, "CLI output byte count")
            _expect_eq(packed[0:1], b"\xcc", "CLI filtered tx byte")
            _expect_eq(packed[1600:1601], b"\xdd", "CLI filtered rx byte")

            bad_log = tmp_dir / "bad.log"
            bad_log.write_text("c6cap 1 tx aa\nc6cap 1 rx 00\nc6cap 3 tx bb\n", encoding="utf-8")
            err_stream = io.StringIO()
            with contextlib.redirect_stderr(err_stream):
                bad_rc = c6cap_to_bin.main([str(bad_log), str(tmp_dir / "unused.bin")])
            _expect_eq(bad_rc, 1, "error CLI exit code")
            _expect_in("2..2 are missing", err_stream.getvalue(), "CLI error stderr")


if __name__ == "__main__":
    unittest.main()
