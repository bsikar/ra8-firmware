#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Measure sustained CDC ACM throughput without per-chunk serialization.

This is the right tool for asking "what is the actual line rate?".
``benchmark.py`` deliberately waits for each chunk to echo before sending the
next, which bounds it by round-trip latency rather than bandwidth; this one
keeps the pipe full and so measures the link.

Methodology:
  * Writer thread fires the whole payload back-to-back, no waits.
  * Reader thread drains in a tight non-blocking select() loop, no sleeps.
  * Wall-time runs from first byte written to last byte received.

Because both directions are in flight at once, the device must be able to
absorb the whole payload; this is a throughput measurement, not the
correctness gate.

Usage:
  python3 stream_bench.py <device> [--bytes N]

Prints one-way wire throughput (bytes/elapsed) plus a sanity check that
every byte echoed back matches what was sent.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import select
import subprocess
import sys
import termios
import threading
import time
from pathlib import Path
from typing import NoReturn


def open_raw(device: str) -> int:
    """Open a tty in fully raw mode and return the blocking fd.

    Clears every line-discipline transformation, ICRNL above all: on a
    byte-exact echo measurement it would rewrite 0x0D to 0x0A and show up as
    device-side corruption.

    Unlike the correctness benchmark's version this returns the fd alone and
    leaves O_NONBLOCK SET, because every reader and writer here is driven by
    select() and never wants to block.
    """
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[2] |= termios.CLOCAL
    attr[2] &= ~termios.CRTSCTS
    attr[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE | termios.ECHONL | termios.ISIG)
    attr[0] &= ~(
        termios.INPCK
        | termios.ISTRIP
        | termios.IXON
        | termios.IXOFF
        | termios.INLCR
        | termios.IGNCR
        | termios.ICRNL
    )
    attr[1] &= ~termios.OPOST
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def _drain(fd: int, window_s: float = 0.3) -> None:
    """Discard anything already buffered so the timing starts clean."""
    end = time.monotonic() + window_s
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            with contextlib.suppress(BlockingIOError):
                os.read(fd, 4096)


def _spawn_writer(
    fd: int, payload: bytes, sent: list[int], err: list[tuple[str, Exception]]
) -> threading.Thread:
    """Start the background writer that keeps the bulk-OUT pipe full."""

    def writer() -> None:
        try:
            while sent[0] < len(payload):
                _, w, _ = select.select([], [fd], [], 1.0)
                if not w:
                    continue
                with contextlib.suppress(BlockingIOError):
                    sent[0] += os.write(fd, payload[sent[0] : sent[0] + 65536])
        except Exception as e:  # noqa: BLE001 -- thread boundary: transport any failure to the joiner
            err.append(("writer", e))

    th = threading.Thread(target=writer, daemon=True)
    th.start()
    return th


def _read_back(fd: int, want: int, deadline: float) -> bytearray:
    """Read until ``want`` bytes have arrived or ``deadline`` passes."""
    recv = bytearray()
    while len(recv) < want and time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.5)
        if r:
            with contextlib.suppress(BlockingIOError):
                d = os.read(fd, 4096)
                if d:
                    recv += d
    return recv


def _report_mismatch(recv: bytes | bytearray, payload: bytes, total_bytes: int) -> None:
    """Print the first differing offset, with context on both sides."""
    n = min(len(recv), len(payload))
    for i in range(n):
        if recv[i] != payload[i]:
            lo, hi = max(0, i - 8), min(n, i + 16)
            print(f"    first mismatch at offset {i}")
            print(f"    expected: {payload[lo:hi].hex()}")
            print(f"    received: {bytes(recv[lo:hi]).hex()}")
            break
    if len(recv) != total_bytes:
        print(f"    short read: got {len(recv)}/{total_bytes}")


def _report_throughput(
    sent: int, recv: bytes | bytearray, payload: bytes, total_bytes: int, elapsed: float
) -> bool:
    """Print the timing and integrity verdict. Returns True when clean."""
    ok = bytes(recv) == payload[: len(recv)] and len(recv) == total_bytes
    oneway = (total_bytes / elapsed) if elapsed > 0 else 0.0
    aggreg = 2.0 * oneway
    print(f"  sent {sent} B, recv {len(recv)} B in {elapsed:.3f} s")
    print(f"  one-way wire throughput : {oneway / 1024:9.1f} KB/s  ({oneway / 1e6 * 8:.2f} Mbps)")
    print(f"  aggregate (both dirs)   : {aggreg / 1024:9.1f} KB/s  ({aggreg / 1e6 * 8:.2f} Mbps)")
    print(f"  data integrity          : {'OK' if ok else 'FAIL'}")
    if not ok:
        _report_mismatch(recv, payload, total_bytes)
    return ok


def stream_echo(device: str, total_bytes: int) -> bool:
    """Full-duplex echo bench: a writer thread fills OUT while we drain IN."""
    fd = open_raw(device)
    _drain(fd)

    payload = bytes((i * 131 + 17) & 0xFF for i in range(total_bytes))
    sent = [0]
    err = []

    t0 = time.monotonic()
    th = _spawn_writer(fd, payload, sent, err)
    recv = _read_back(fd, total_bytes, t0 + 30.0)
    elapsed = time.monotonic() - t0
    th.join(timeout=1.0)
    os.close(fd)

    if err:
        print(f"  ERROR: {err}")
        return False
    return _report_throughput(sent[0], recv, payload, total_bytes, elapsed)


def find_cdc_device() -> str | None:
    """Find the first /dev/ttyACM* whose USB vendor id is pid.codes (0x1209).

    Matches on vendor id via udevadm rather than on device name order, since
    ttyACM numbering depends on enumeration order and a J-Link or another
    board can take ttyACM0.

    Returns the device path, or None when no matching device is present --
    the caller distinguishes that from a test failure.
    """
    dev_dir = Path("/dev")
    for entry in sorted(dev_dir.iterdir()):
        if not entry.name.startswith("ttyACM"):
            continue
        try:
            info = subprocess.check_output(  # noqa: S603  # trusted: fixed udevadm argv
                ["udevadm", "info", str(entry)],  # noqa: S607  # trusted: fixed udevadm argv
                text=True,
            )
        except subprocess.CalledProcessError:
            continue
        if "ID_VENDOR_ID=1209" in info:
            return str(entry)
    return None


def main() -> NoReturn:
    """Stream-benchmark one CDC ACM device and exit with the verdict.

    Never returns. Exit 2 means no device was found (a rig fault), 1 means the
    echoed data did not match or the writer thread raised, 0 means clean.

    Default payload is 64 KiB, large enough to amortise start-up cost while
    still fitting comfortably in the 30 s read deadline on a slow link.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("device", nargs="?", default=None)
    ap.add_argument("--bytes", type=int, default=65536)
    args = ap.parse_args()
    dev = args.device or find_cdc_device()
    if dev is None:
        print("No /dev/ttyACMx with VID 1209 found.")
        sys.exit(2)
    print(f"Stream-benchmarking {dev}, payload = {args.bytes} B")
    ok = stream_echo(dev, args.bytes)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
