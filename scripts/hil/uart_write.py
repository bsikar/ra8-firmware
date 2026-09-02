#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Write one bounded runtime-provisioning packet to an already owned UART."""

from __future__ import annotations

import argparse
import os
import sys
import termios
from pathlib import Path

MAX_PACKET_BYTES = 1225
DEFAULT_BAUD = 115200
PACKET_PREFIX = b"RA8NET1:"
PACKET_FIELD_COUNT = 3
HEX_DIGITS = frozenset(b"0123456789abcdef")


def validate_packet(packet: bytes) -> None:
    """Reject malformed or oversized input before opening the serial device."""
    if not packet or len(packet) > MAX_PACKET_BYTES:
        message = "runtime packet is empty or exceeds its protocol bound"
        raise ValueError(message)
    if not packet.startswith(PACKET_PREFIX) or not packet.endswith(b"\n"):
        message = "runtime packet has an invalid frame"
        raise ValueError(message)
    body = packet[len(PACKET_PREFIX) : -1]
    fields = body.split(b":")
    invalid_hex = any(any(byte not in HEX_DIGITS for byte in field) for field in fields)
    if len(fields) != PACKET_FIELD_COUNT or invalid_hex:
        message = "runtime packet fields must be lowercase hexadecimal"
        raise ValueError(message)


def termios_baud(baud: int) -> int:
    """Return the platform termios constant for one supported positive baud."""
    if baud <= 0:
        message = "baud must be a positive supported integer"
        raise ValueError(message)
    constant = getattr(termios, f"B{baud}", None)
    if constant is None:
        message = f"unsupported baud rate: {baud}"
        raise ValueError(message)
    return int(constant)


def argparse_baud(raw: str) -> int:
    """Parse and validate a baud value before any serial device is opened."""
    try:
        baud = int(raw, 10)
        termios_baud(baud)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    return baud


def write_packet(port: Path, packet: bytes, baud: int) -> None:
    """Configure the requested baud at 8N1 raw mode and write the packet."""
    validate_packet(packet)
    baud_flag = termios_baud(baud)
    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = (attrs[2] & ~termios.CSIZE) | termios.CS8 | termios.CLOCAL | termios.CREAD
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB)
        attrs[3] = 0
        attrs[4] = baud_flag
        attrs[5] = baud_flag
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        offset = 0
        while offset < len(packet):
            written = os.write(fd, packet[offset:])
            if written <= 0:
                message = "serial write made no progress"
                raise OSError(message)
            offset += written
        termios.tcdrain(fd)
    finally:
        os.close(fd)


def _packet_is_valid(packet: bytes) -> bool:
    """Return whether a packet passes validation."""
    try:
        validate_packet(packet)
    except ValueError:
        return False
    return True


def run_selftest() -> int:
    """Prove valid framing stays quiet and malformed frames fire."""
    exact_maximum = PACKET_PREFIX + (b"a" * 64) + b":" + (b"b" * 128) + b":" + (b"c" * 1022) + b"\n"
    valid = (
        b"RA8NET1:61:6262626262626262:\n",
        b"RA8NET1:61:6262626262626262:6874747073\n",
        exact_maximum,
    )
    invalid = (
        b"",
        b"RA8NET1:zz:62:\n",
        b"RA8NET1:61:62\n",
        b"OTHER:61:62:\n",
        exact_maximum[:-1] + b"cc\n",
    )
    failures = []
    failures.extend("valid packet was rejected" for packet in valid if not _packet_is_valid(packet))
    failures.extend("invalid packet was accepted" for packet in invalid if _packet_is_valid(packet))
    if len(exact_maximum) != MAX_PACKET_BYTES:
        failures.append("maximum packet fixture does not match the C protocol bound")
    try:
        if argparse_baud(str(DEFAULT_BAUD)) != DEFAULT_BAUD:
            failures.append("default baud changed during validation")
    except argparse.ArgumentTypeError:
        failures.append("default baud is unsupported on this platform")
    for invalid_baud in ("0", "-1", "115201", "1.5", "invalid"):
        try:
            argparse_baud(invalid_baud)
        except argparse.ArgumentTypeError:
            continue
        failures.append(f"invalid or unsupported baud was accepted: {invalid_baud}")
    if failures:
        for failure in failures:
            print(f"uart_write.py --selftest: FAIL: {failure}", file=sys.stderr)
        return 1
    print("uart_write.py --selftest: PASS (3 frames accepted, 5 frames + 5 bauds refused)")
    return 0


def main() -> int:
    """Parse arguments, read one packet from stdin, and write it to the UART."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", type=Path)
    parser.add_argument("--baud", type=argparse_baud, default=DEFAULT_BAUD)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        if args.port is not None:
            parser.error("--selftest accepts no port")
        return run_selftest()
    if args.port is None:
        parser.error("a serial port is required")
    packet = sys.stdin.buffer.read(MAX_PACKET_BYTES + 1)
    try:
        write_packet(args.port, packet, args.baud)
    except (OSError, ValueError) as exc:
        print(f"uart_write.py: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
