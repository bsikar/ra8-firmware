#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exhaustive correctness + throughput test for a CDC ACM echo device.

Used by the HIL suite to validate both USBFS (J11) and USBHS (J7) on the
EK-RA8D2 against /dev/ttyACM*.

Every payload is drawn from a FIXED seed, so a failing run can be replayed
byte-for-byte: the seeds are named constants rather than literals precisely
so that a reported mismatch offset means the same thing on the next run.

Usage:
  python3 benchmark.py <device>            # all tests
  python3 benchmark.py <device> --quick    # short correctness pass only

Exit status: 0 if every test passes, non-zero otherwise.
"""

from __future__ import annotations

import argparse
import fcntl
import os
import random
import subprocess
import sys
import termios
import time
from pathlib import Path
from typing import NoReturn


def open_raw(device: str) -> tuple[int, int]:
    """Open a tty in fully raw mode and return ``(fd, original_flags)``.

    Every line-discipline transformation is cleared -- canonical mode, echo,
    signal generation, XON/XOFF, CR/LF translation and output post-processing.
    That is not tidiness: on a byte-exact echo test, ICRNL alone silently
    rewrites 0x0D to 0x0A and would surface as a data-corruption failure
    attributed to the device.

    O_NONBLOCK is used to get past a blocking open and then CLEARED, so the
    returned fd is blocking. The original flags come back with it because the
    read helpers toggle O_NONBLOCK on and off and need the value to restore.
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
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return fd, flags


def drain(fd: int, flags: int, quiet_for: float = 0.30) -> None:
    """Drain pending echo data until we see no new bytes for `quiet_for` seconds.

    Also calls tcflush so any kernel-side queued bytes from a previous test
    are discarded before the next test starts measuring.
    """
    termios.tcflush(fd, termios.TCIOFLUSH)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    last_data = time.monotonic()
    try:
        while time.monotonic() - last_data < quiet_for:
            try:
                d = os.read(fd, 4096)
                if d:
                    last_data = time.monotonic()
                else:
                    time.sleep(0.01)
            except BlockingIOError:  # noqa: PERF203  # non-blocking drain inside timed loop
                time.sleep(0.01)
    finally:
        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    termios.tcflush(fd, termios.TCIOFLUSH)


def echo_one(fd: int, flags: int, msg: bytes, settle: float = 0.25, timeout: float = 1.5) -> bytes:
    """Write one message and read back whatever echoes within ``timeout``.

    Returns the bytes received, NOT a pass/fail verdict -- comparison is the
    caller's job, so a short or corrupted read is reported with its actual
    content rather than collapsing to False.

    ``settle`` is an unconditional sleep before reading. It exists because the
    device needs time to turn the transfer around, and reading immediately
    would return empty and then race the deadline.
    """
    os.write(fd, msg)
    time.sleep(settle)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    total = b""
    deadline = time.monotonic() + timeout
    try:
        while len(total) < len(msg) and time.monotonic() < deadline:
            try:
                d = os.read(fd, 4096)
                if d:
                    total += d
                else:
                    time.sleep(0.05)
            except BlockingIOError:  # noqa: PERF203  # non-blocking read-back inside timed loop
                time.sleep(0.05)
    finally:
        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return total


def test_lengths(fd: int, flags: int, max_len: int) -> bool:
    """Echo every payload length from 1 to ``max_len`` and check each round-trips.

    Sweeping lengths one at a time is what catches an off-by-one at a packet
    boundary: a device that mishandles exactly the max-packet-size payload
    passes any test that only sends round numbers.

    Payload bytes are a counter mod 256, so a reported mismatch offset reads
    directly as a position rather than needing a lookup.

    Returns True only if every length round-tripped exactly.
    """
    print(f"=== Test: all lengths 1..{max_len} (exhaustive correctness) ===")
    passes = fails = 0
    for length in range(1, max_len + 1):
        msg = bytes([(i % 256) for i in range(length)])
        recv = echo_one(fd, flags, msg)
        if recv == msg:
            passes += 1
        else:
            fails += 1
            print(f"  FAIL L={length:3d}: sent {msg.hex()[:40]}... recv {recv.hex()[:40]}...")
    print(f"  -> {passes}/{passes + fails} pass")
    return fails == 0


RANDOM_SEED_CORRECTNESS = 0xCAFE  # fixed seed for reproducible test sequence
RANDOM_SEED_THROUGHPUT = 0xC0FFEE  # fixed seed for recognisable payload pattern
RANDOM_SEED_CHUNKED = 0xBEEF1234  # fixed seed for chunked throughput payload


def test_random(fd: int, flags: int, n_iters: int) -> bool:
    """Echo ``n_iters`` random payloads of random length and check each round-trips.

    Complements the exhaustive length sweep by varying content as well as
    size. The generator is seeded from a fixed constant, so "random" here means
    unpredictable-but-reproducible: a failure can be replayed exactly.

    Returns True only if every iteration round-tripped exactly.
    """
    print(f"=== Test: {n_iters} random payloads of length 1..255 ===")
    passes = fails = 0
    rng = random.Random(RANDOM_SEED_CORRECTNESS)  # noqa: S311  # non-crypto test data
    for i in range(n_iters):
        length = rng.randint(1, 255)
        msg = bytes(rng.randint(0, 255) for _ in range(length))
        recv = echo_one(fd, flags, msg, settle=0.3)
        if recv == msg:
            passes += 1
        else:
            fails += 1
            print(f"  FAIL #{i} L={length}: sent {msg.hex()[:40]}... recv {recv.hex()[:40]}...")
    print(f"  -> {passes}/{passes + fails} pass")
    return fails == 0


def _report_corruption(
    recv: bytes | bytearray, payload: bytes, chunk_size: int, total_bytes: int
) -> None:
    """Diagnose a failed throughput transfer by locating the first bad byte.

    Prints the nearest chunk boundary alongside the offset, which is the
    discriminating detail: corruption landing ON a boundary points at framing
    or a FIFO edge, while corruption in the middle of a chunk points at the
    data path.

    A transfer that is merely SHORT -- every received byte correct, but too few
    of them -- is reported as a short read instead, since there is no
    mismatching byte to point at and the two failures have different causes.
    """
    n = min(len(recv), len(payload))
    first_bad = next((i for i in range(n) if recv[i] != payload[i]), None)
    if first_bad is not None:
        print(f"  FIRST MISMATCH at offset {first_bad}:")
        lo = max(0, first_bad - 8)
        hi = min(n, first_bad + 16)
        print(f"    expected: {payload[lo:hi].hex()}")
        print(f"    received: {bytes(recv[lo:hi]).hex()}")
        print(f"    chunk_boundary nearest: {(first_bad // chunk_size) * chunk_size}")
    elif len(recv) != total_bytes:
        print(f"  short read: got {len(recv)}/{total_bytes}")


def test_throughput(fd: int, flags: int, total_bytes: int, chunk_size: int, label: str) -> bool:
    """Free-running write-and-read throughput measurement with integrity check.

    Writes and reads interleaved without waiting for each chunk to echo, which
    measures peak rate but can overrun the device IN FIFO for payloads past the
    bulk max-packet size -- ``test_throughput_chunked`` is the flow-controlled
    variant used for the pass/fail gate.

    On mismatch it locates the FIRST differing byte and prints the surrounding
    window plus the nearest chunk boundary, because a corruption that lands on
    a boundary points at framing while one that does not points at the data
    path.

    The reported KB/s is per direction; the aggregate figure alongside it is
    doubled since every byte traverses the link twice.

    Returns True only when the full payload came back byte-identical.
    """
    print(f"=== Test: throughput {label} ({total_bytes}B in {chunk_size}B chunks) ===")
    drain(fd, flags)
    # Use a recognisable pseudorandom payload so any data-shuffle is visible
    # in the first mismatching byte rather than blending into a counter
    # pattern that wraps at 256.
    rng = random.Random(RANDOM_SEED_THROUGHPUT)  # noqa: S311  # non-crypto test data
    payload = bytes(rng.randint(0, 255) for _ in range(total_bytes))
    sent_off = 0
    recv = bytearray()
    start = time.monotonic()
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    throughput_timeout_s = 30.0  # generous wall-time cap for large transfers
    try:
        while sent_off < total_bytes or len(recv) < total_bytes:
            if sent_off < total_bytes:
                chunk = payload[sent_off : sent_off + chunk_size]
                try:
                    n = os.write(fd, chunk)
                    sent_off += n
                except BlockingIOError:
                    pass
            try:
                d = os.read(fd, 4096)
                if d:
                    recv += d
            except BlockingIOError:
                pass
            if time.monotonic() - start > throughput_timeout_s:
                print("  TIMEOUT")
                break
    finally:
        fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    elapsed = time.monotonic() - start
    bps = total_bytes / elapsed if elapsed > 0 else 0
    integrity = bytes(recv) == payload[: len(recv)] and len(recv) == total_bytes
    print(
        f"  sent {sent_off}B, recv {len(recv)}B in {elapsed:.2f}s "
        f"-> {bps / 1024:.1f} KB/s (each direction, {2 * bps / 1024:.1f} KB/s aggregate)"
    )
    if not integrity:
        _report_corruption(recv, payload, chunk_size, total_bytes)
    print(f"  data integrity: {'OK' if integrity else 'FAIL'}")
    return integrity


def test_throughput_chunked(
    fd: int, flags: int, total_bytes: int, chunk_bytes: int, label: str
) -> bool:
    """Measure sustainable echo rate one chunk at a time, verifying as it goes.

    Writes a chunk, waits for that chunk to echo back, verifies it, and only
    then sends the next. The single-buffering is the point: a free-running
    "write all, then read all" overruns the device-side IN FIFO for any
    payload larger than the bulk-IN max-packet size, so it would measure the
    overflow rather than the link.

    Mismatch reporting is capped at a few chunks -- once framing has slipped
    every subsequent chunk mismatches, and printing them all buries the first
    and most informative one.

    Returns True only when every chunk round-tripped exactly.
    """
    print(f"=== Test: chunked throughput {label} ({total_bytes}B in {chunk_bytes}B chunks) ===")
    drain(fd, flags)
    rng = random.Random(RANDOM_SEED_CHUNKED)  # noqa: S311  # non-crypto test data
    payload = bytes(rng.randint(0, 255) for _ in range(total_bytes))
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    start = time.monotonic()
    offset = 0
    chunk_failures = 0
    while offset < total_bytes:
        n = min(chunk_bytes, total_bytes - offset)
        sent = payload[offset : offset + n]
        os.write(fd, sent)
        recv = b""
        deadline = time.monotonic() + 2.0
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        try:
            while len(recv) < n and time.monotonic() < deadline:
                try:
                    d = os.read(fd, 4096)
                    if d:
                        recv += d
                    else:
                        time.sleep(0.001)
                except BlockingIOError:  # noqa: PERF203  # non-blocking read inside timed loop
                    time.sleep(0.001)
        finally:
            fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
        chunk_report_limit = 3  # limit repeated mismatch output
        if recv != sent:
            chunk_failures += 1
            if chunk_failures <= chunk_report_limit:
                sent_hex = sent.hex()[:40]
                recv_hex = recv.hex()[:40]
                print(f"  chunk@{offset} mismatch ({n}B): sent {sent_hex}... recv {recv_hex}...")
        offset += n
    elapsed = time.monotonic() - start
    bps = total_bytes / elapsed if elapsed > 0 else 0
    print(
        f"  {total_bytes}B round-trip in {elapsed:.3f}s -> {bps / 1024:.1f} KB/s "
        f"({2 * bps / 1024:.1f} KB/s aggregate); chunk failures = {chunk_failures}"
    )
    return chunk_failures == 0


def find_cdc_device() -> str | None:
    """Return /dev/ttyACMx that maps to a 1209:xxxx (pid.codes) device."""
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
    """Run the correctness and throughput suite against one CDC ACM device.

    Never returns -- every path exits. Exit 2 means the device could not be
    found at all (a rig fault, distinct from a test failure), exit 1 means at
    least one test failed, exit 0 means all passed. The HIL suite depends on
    that distinction to tell a broken bench from broken firmware.

    ``--quick`` runs only a short length sweep and skips the random and
    throughput passes; it is for iterating on enumeration, not for validating
    a build.

    Only the chunked throughput test contributes to the verdict. The
    free-running one is available but not called here, since its FIFO overrun
    makes it a diagnostic rather than a gate.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("device", nargs="?", help="/dev/ttyACMx (auto-detected if omitted)")
    parser.add_argument("--quick", action="store_true", help="only run lengths 1..32")
    parser.add_argument("--throughput-bytes", type=int, default=4096)
    parser.add_argument("--throughput-chunk", type=int, default=64)
    args = parser.parse_args()

    device = args.device or find_cdc_device()
    if device is None:
        print("No /dev/ttyACMx with VID 1209 found; pass --device explicitly.")
        sys.exit(2)
    print(f"Benchmarking {device}")

    fd, flags = open_raw(device)
    time.sleep(0.5)
    drain(fd, flags)

    if args.quick:
        ok = test_lengths(fd, flags, 32)
        os.close(fd)
        sys.exit(0 if ok else 1)

    r1 = test_lengths(fd, flags, 64)
    drain(fd, flags)
    r2 = test_random(fd, flags, 50)
    drain(fd, flags)
    r3 = test_throughput_chunked(fd, flags, args.throughput_bytes, args.throughput_chunk, "echo")

    print()
    print(f"  Test 1 (lengths 1..64)            :  {'PASS' if r1 else 'FAIL'}")
    print(f"  Test 2 (50 random 1..255B)         :  {'PASS' if r2 else 'FAIL'}")
    print(f"  Test 3 (chunked throughput)        :  {'PASS' if r3 else 'FAIL'}")
    print()
    overall = r1 and r2 and r3
    print("OVERALL: " + ("ALL PASS" if overall else "FAIL"))
    os.close(fd)
    sys.exit(0 if overall else 1)


if __name__ == "__main__":
    main()
