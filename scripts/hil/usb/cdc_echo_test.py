#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""USB CDC ACM round-trip echo verifier for the EK-RA8D2 USB demos.

Verification contract:
  Open the target tty in raw mode using a non-blocking POSIX file descriptor
  (NOT pyserial's Serial wrapper -- pyserial toggles DTR which can hang on
  the USB bridge during enumeration), write a fixed payload, then read back
  for ``--timeout`` seconds. The round-trip succeeds iff the bytes read
  exactly equal the bytes written.

Exit status:
  0 -- payload round-tripped intact
  1 -- no data received within --timeout seconds
  2 -- data received but does not match the payload
  3 -- usage / configuration / I/O error (no device, etc.)

Example:
  python3 scripts/hil/usb/cdc_echo_test.py --tty /dev/cu.usbmodem000000011

Auto-detect example (FS demo):
  python3 scripts/hil/usb/cdc_echo_test.py --auto fs
"""

from __future__ import annotations

import argparse
import contextlib
import errno
import os
import select
import subprocess
import sys
import termios
import time
from pathlib import Path

# Default payload chosen to be ASCII-only and short enough to fit in a single
# CDC bulk-OUT packet on FS (max 64 B) and HS (max 512 B).
DEFAULT_PAYLOAD = b"PING-RA8D2"
DEFAULT_TIMEOUT_S = 4.0

# Per-speed serial-number suffix programmed in the device descriptor.
# FS firmware reports iSerial "00000001"; HS reports "00000002".
SERIAL_FS = "00000001"
SERIAL_HS = "00000002"

PRODUCT_SUBSTR_FS = "EK-RA8D2 CDC Echo"
PRODUCT_SUBSTR_HS = "EK-RA8D2 HS CDC Echo"


def _ioreg_lookup(product_substr: str) -> str | None:
    """Return the /dev/cu.usbmodem* path whose USB Product string matches.

    Uses macOS ioreg to map the USB product description back to the BSD
    device node. Returns None if no matching device is found or ioreg is
    unavailable (e.g. on Linux).
    """
    try:
        out = subprocess.check_output(
            ["ioreg", "-p", "IOUSB", "-l", "-w", "0"],  # noqa: S607  # trusted: fixed ioreg argv
            stderr=subprocess.DEVNULL,
            timeout=5,
        ).decode("utf-8", errors="replace")
    except (FileNotFoundError, subprocess.SubprocessError):
        return None

    # Find every block that mentions the product substring; within each, look
    # for the BSD name. ioreg output is hierarchical; a coarse line scan is
    # sufficient because Product/BSD Name appear close together.
    lines = out.splitlines()
    for idx, line in enumerate(lines):
        if product_substr in line:
            window = lines[max(0, idx - 40) : idx + 40]
            for w in window:
                if "IOCalloutDevice" in w and "/dev/cu.usbmodem" in w:
                    # Extract the path between quotes.
                    start = w.find('"/dev/cu.')
                    if start == -1:
                        continue
                    end = w.find('"', start + 1)
                    if end == -1:
                        continue
                    return w[start + 1 : end]
    return None


def auto_detect(speed: str) -> str | None:
    """Resolve the EK-RA8D2 tty for the given speed ('fs' or 'hs').

    Strategy:
      1. ioreg lookup by USB Product string (most reliable).
      2. Fallback: scan /dev/cu.usbmodem* and pick the one whose serial
         suffix matches the speed-specific iSerial pattern.
    """
    if speed == "fs":
        prod, serial = PRODUCT_SUBSTR_FS, SERIAL_FS
    elif speed == "hs":
        prod, serial = PRODUCT_SUBSTR_HS, SERIAL_HS
    else:
        return None

    path = _ioreg_lookup(prod)
    if path:
        return path

    # Fallback: serial-suffix match. Apple-style nodes look like
    # /dev/cu.usbmodem000000011 (FS) or .../000000021 (HS), where the trailing
    # digit is the interface index appended by the kernel.
    for cand in sorted(Path("/dev").glob("cu.usbmodem*")):
        if serial in cand.name:
            return str(cand)
    return None


def _open_raw(tty_path: str) -> int:
    """Open the tty as a raw, non-blocking fd suitable for binary I/O."""
    fd = os.open(tty_path, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    # Configure as 8N1 raw: drop ICANON/ECHO/ISIG/etc so the kernel does not
    # cook the byte stream. CDC ACM ignores baud anyway.
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = (cflag & ~termios.CSIZE) | termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
    return fd


def _drain_stale(fd: int, window_s: float = 0.2) -> None:
    """Discard anything already in the RX buffer.

    Leftover boot chatter would otherwise contaminate the echo comparison.
    """
    deadline = time.monotonic() + window_s
    while time.monotonic() < deadline:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            return
        try:
            _ = os.read(fd, 4096)
        except OSError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise


def _write_all(fd: int, payload: bytes) -> None:
    """Write the whole payload, retrying on a would-block."""
    written = 0
    while written < len(payload):
        try:
            n = os.write(fd, payload[written:])
            if n <= 0:
                return
            written += n
        except OSError as exc:  # noqa: PERF203 -- retrying a would-block IS the loop; the write must be attempted per iteration
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                time.sleep(0.01)
                continue
            raise


def _read_until(fd: int, want: int, timeout_s: float) -> bytes:
    """Read until ``want`` bytes have arrived or ``timeout_s`` expires."""
    rx = bytearray()
    deadline = time.monotonic() + timeout_s
    while len(rx) < want and time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        r, _, _ = select.select([fd], [], [], min(0.25, max(0.0, remaining)))
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                continue
            raise
        if chunk:
            rx.extend(chunk)
    return bytes(rx)


def round_trip(tty_path: str, payload: bytes, timeout_s: float) -> tuple[int, bytes]:
    """Write payload, read back for timeout_s, return (exit_code, received).

    The function does not raise on I/O errors -- it converts them into the
    documented exit codes so the CLI surface stays predictable.
    """
    fd = _open_raw(tty_path)
    try:
        _drain_stale(fd)
        _write_all(fd, payload)
        received = _read_until(fd, len(payload), timeout_s)
    finally:
        with contextlib.suppress(OSError):
            os.close(fd)

    if not received:
        return 1, received
    if received[: len(payload)] != payload:
        return 2, received
    return 0, received


def main(argv: list[str]) -> int:  # noqa: PLR0911  # CLI gate; each return maps to a distinct exit code
    """Echo-test one CDC tty and map the outcome onto a three-way exit code.

    The exit codes are the interface the HIL suite consumes, and they separate
    the two failures that get confused: 3 means the test could not RUN (no
    tty given, auto-detect failed, path absent, non-ASCII payload, I/O error)
    while 1 and 2 mean it ran and the device failed it -- 1 for silence within
    the timeout, 2 for data that came back different. A rig problem therefore
    never reads as a firmware problem.

    Returns 0 on an exact round-trip, 1 on timeout, 2 on mismatch, 3 on any
    setup failure.
    """
    parser = argparse.ArgumentParser(
        description="Round-trip echo test for the EK-RA8D2 USB CDC demos.",
    )
    parser.add_argument(
        "--tty",
        help="Path to the CDC tty (e.g. /dev/cu.usbmodem000000011).",
    )
    parser.add_argument(
        "--auto",
        choices=("fs", "hs"),
        help="Auto-detect the EK-RA8D2 tty for the given USB speed.",
    )
    parser.add_argument(
        "--payload",
        default=DEFAULT_PAYLOAD.decode("ascii"),
        help="ASCII payload to send (default: %(default)s).",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_S,
        help="Seconds to wait for the echo (default: %(default)s).",
    )
    args = parser.parse_args(argv)

    tty_path = args.tty
    if not tty_path and args.auto:
        tty_path = auto_detect(args.auto)
        if not tty_path:
            print(f"ERROR: could not auto-detect EK-RA8D2 {args.auto.upper()} tty", file=sys.stderr)
            return 3
        print(f"auto-detected: {tty_path}")
    if not tty_path:
        print("ERROR: must pass --tty or --auto {fs,hs}", file=sys.stderr)
        return 3
    if not Path(tty_path).exists():
        print(f"ERROR: tty does not exist: {tty_path}", file=sys.stderr)
        return 3

    try:
        payload_bytes = args.payload.encode("ascii")
    except UnicodeEncodeError:
        print("ERROR: payload must be ASCII-only", file=sys.stderr)
        return 3

    try:
        code, received = round_trip(tty_path, payload_bytes, args.timeout)
    except OSError as exc:
        print(f"ERROR: I/O failure on {tty_path}: {exc}", file=sys.stderr)
        return 3

    if code == 0:
        print(f"OK: round-tripped {len(payload_bytes)} bytes")
        return 0
    if code == 1:
        print(
            f"FAIL: no data received within {args.timeout:.1f}s on {tty_path}",
            file=sys.stderr,
        )
        return 1
    print(
        f"FAIL: data mismatch on {tty_path}\n"
        f"  expected: {payload_bytes!r}\n"
        f"  received: {received!r}",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
