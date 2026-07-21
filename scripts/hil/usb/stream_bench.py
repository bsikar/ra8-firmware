#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# usb_stream_bench.py -- measure sustained one-way and round-trip throughput
# on a CDC ACM device WITHOUT the chunk-at-a-time serialization that the
# correctness benchmark uses.  This is the right tool for asking "what is
# the actual line rate?"
#
# Methodology:
#   * Writer thread fires the whole payload back-to-back, no waits.
#   * Reader thread drains in tight non-blocking select() loop, no sleeps.
#   * Wall-time runs from first byte written to last byte received.
#
# Usage:
#   python3 usb_stream_bench.py <device> [--bytes N]
#
# Prints one-way wire throughput (bytes/elapsed) plus a sanity check that
# every byte echoed back matches what was sent.

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


def open_raw(device):
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


def _drain(fd, window_s=0.3):
    """Discard anything already buffered so the timing starts clean."""
    end = time.monotonic() + window_s
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            with contextlib.suppress(BlockingIOError):
                os.read(fd, 4096)


def _spawn_writer(fd, payload, sent, err):
    """Start the background writer that keeps the bulk-OUT pipe full."""

    def writer():
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


def _read_back(fd, want, deadline):
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


def _report_mismatch(recv, payload, total_bytes):
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


def _report_throughput(sent, recv, payload, total_bytes, elapsed):
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


def stream_echo(device, total_bytes):
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


def find_cdc_device():
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


def main():
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
