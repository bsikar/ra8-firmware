#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# usb_benchmark.py -- exhaustive correctness + throughput test for a CDC ACM
# echo device.  Used by the HIL suite to validate both USBFS (J11) and USBHS
# (J7) on the EK-RA8D2 against /dev/ttyACM*.
#
# Usage:
#   python3 usb_benchmark.py <device>            # all tests
#   python3 usb_benchmark.py <device> --quick    # short correctness pass only
#
# Exit status: 0 if every test passes, non-zero otherwise.

import argparse
import fcntl
import os
import random
import subprocess
import sys
import termios
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
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return fd, flags


def drain(fd, flags, quiet_for=0.30):
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


def echo_one(fd, flags, msg, settle=0.25, timeout=1.5):
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


def test_lengths(fd, flags, max_len):
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


def test_random(fd, flags, n_iters):
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


def test_throughput(fd, flags, total_bytes, chunk_size, label):
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
        # Find first mismatching byte to help diagnose
        n = min(len(recv), len(payload))
        first_bad = None
        for i in range(n):
            if recv[i] != payload[i]:
                first_bad = i
                break
        if first_bad is not None:
            print(f"  FIRST MISMATCH at offset {first_bad}:")
            lo = max(0, first_bad - 8)
            hi = min(n, first_bad + 16)
            print(f"    expected: {payload[lo:hi].hex()}")
            print(f"    received: {bytes(recv[lo:hi]).hex()}")
            print(f"    chunk_boundary nearest: {(first_bad // chunk_size) * chunk_size}")
        elif len(recv) != total_bytes:
            print(f"  short read: got {len(recv)}/{total_bytes}")
    print(f"  data integrity: {'OK' if integrity else 'FAIL'}")
    return integrity


def test_throughput_chunked(fd, flags, total_bytes, chunk_bytes, label):
    """Echo round-trip in chunk_bytes-sized pieces: write chunk, drain echo,
    verify integrity, repeat. Measures sustainable end-to-end echo rate.
    Single-buffered chunk-by-chunk avoids the device-side IN FIFO overflow
    that a free-running "write all, then read all" would cause for any
    payload larger than the bulk-IN max-packet size.
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


def find_cdc_device():
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


def main():
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
