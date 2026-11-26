#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Turn the c6cap lines of a capture console log into a C6 link fixture.

A bench image that binds ra8_c6link_capture_bind prints one line per frame
(`c6cap <seq> tx|rx <hex>`) and per HANDSHAKE edge (`c6cap <seq> hs <0|1>`).
This script keeps the frames and writes them as a fixture: for each
transaction in order, the 1600-byte frame clocked out, then the 1600-byte
frame clocked in. Each frame is padded back to 1600 bytes with the trailing
zeros the capture dropped. HANDSHAKE edges are timing, not bytes, so they
are left out.

Text before `c6cap` on a line (a timestamp, a log tag) is ignored, and so
are lines without it. A transaction with no rx line (the transfer failed) is
skipped with a warning. A missing tx, a duplicate line, a frame over 1600
bytes or a gap in the sequence numbers is an error, since each one means
the console dropped or mangled output and the fixture would not be the
traffic that crossed the wire.

Usage:
    python3 libs/ra8_c6link/scripts/c6cap_to_bin.py console.log scan.bin
    python3 libs/ra8_c6link/scripts/c6cap_to_bin.py --first 12 --last 19 console.log scan.bin
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable
from pathlib import Path
from typing import TextIO

FRAME_BYTES = 1600
FRAME_FIELDS_SHORT = 3
FRAME_FIELDS_WITH_PAYLOAD = 4
MARK = "c6cap "


class CaptureError(ValueError):
    """The log is not a clean capture."""


def records(lines: Iterable[str]) -> Iterable[tuple[int, int, str, str]]:
    """Yield (line number, seq, kind, payload) for every c6cap line."""
    for number, raw in enumerate(lines, 1):
        at = raw.find(MARK)
        if at < 0:
            continue
        fields = raw[at:].split()
        if (
            len(fields) not in (FRAME_FIELDS_SHORT, FRAME_FIELDS_WITH_PAYLOAD)
            or not fields[1].isdigit()
        ):
            msg = f"line {number}: malformed c6cap record"
            raise CaptureError(msg)
        yield (
            number,
            int(fields[1]),
            fields[2],
            fields[3] if len(fields) == FRAME_FIELDS_WITH_PAYLOAD else "",
        )


def frame(number: int, payload: str) -> bytes:
    """The 1600-byte frame a hex payload stands for."""
    try:
        data = bytes.fromhex(payload)
    except ValueError as err:
        msg = f"line {number}: bad hex ({err})"
        raise CaptureError(msg) from err
    if len(data) > FRAME_BYTES:
        msg = f"line {number}: frame is {len(data)} bytes, over {FRAME_BYTES}"
        raise CaptureError(msg)
    return data.ljust(FRAME_BYTES, b"\0")


def transactions(lines: Iterable[str]) -> dict[int, dict[str, bytes]]:
    """Map seq to {"tx": frame, "rx": frame} for every frame line."""
    seen: dict[int, dict[str, bytes]] = {}
    for number, seq, kind, payload in records(lines):
        if kind == "hs":
            continue
        if kind not in ("tx", "rx"):
            msg = f"line {number}: unknown record kind {kind!r}"
            raise CaptureError(msg)
        entry = seen.setdefault(seq, {})
        if kind in entry:
            msg = f"line {number}: second {kind} for transaction {seq}"
            raise CaptureError(msg)
        entry[kind] = frame(number, payload)
    return seen


def fixture(
    lines: Iterable[str],
    first: int | None = None,
    last: int | None = None,
    warn: TextIO = sys.stderr,
) -> bytes:
    """The fixture bytes for transactions first..last (inclusive) of a log."""
    seen = transactions(lines)
    chosen = [
        seq
        for seq in sorted(seen)
        if (first is None or seq >= first) and (last is None or seq <= last)
    ]
    if not chosen:
        msg = "no transactions in range"
        raise CaptureError(msg)
    out = bytearray()
    for previous, seq in zip([chosen[0] - 1, *chosen[:-1]], chosen, strict=True):
        if seq != previous + 1:
            msg = f"transactions {previous + 1}..{seq - 1} are missing"
            raise CaptureError(msg)
        entry = seen[seq]
        if "tx" not in entry:
            msg = f"transaction {seq} has rx but no tx"
            raise CaptureError(msg)
        if "rx" not in entry:
            print(f"c6cap_to_bin: transaction {seq} failed (no rx), skipped", file=warn)
            continue
        out += entry["tx"] + entry["rx"]
    return bytes(out)


def main(argv: list[str] | None = None) -> int:
    """Parse CLI arguments and write the packed C6 capture fixture."""
    doc_line = (__doc__ or "").splitlines()[0]
    parser = argparse.ArgumentParser(description=doc_line)
    parser.add_argument("log", help="console log holding c6cap lines")
    parser.add_argument("out", help="fixture to write")
    parser.add_argument("--first", type=int, help="first transaction to keep")
    parser.add_argument("--last", type=int, help="last transaction to keep")
    args = parser.parse_args(argv)
    with Path(args.log).open(encoding="utf-8", errors="replace") as log:
        try:
            data = fixture(log, args.first, args.last)
        except CaptureError as err:
            print(f"c6cap_to_bin: {err}", file=sys.stderr)
            return 1
    with Path(args.out).open("wb") as out:
        out.write(data)
    print(f"c6cap_to_bin: {len(data) // (2 * FRAME_BYTES)} transactions, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
