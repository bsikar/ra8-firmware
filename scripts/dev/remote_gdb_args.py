# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate and serialize remote-GDB public arguments without side effects."""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

ASCII_MIN = 0x21
ASCII_MAX = 0x7E
DNS_MAX = 253
MAX_HOST_BYTES = 320
MAX_SELECTOR_BYTES = 256
PORT_MIN = 1024
PORT_MAX = 65535
IPV4_FIELDS = 4
IPV4_FIELD_BYTES = 3
IPV4_MAX = 255
USER_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")
DNS_LABEL_RE = re.compile(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?")
IDENTIFIER_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}")
APP_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_:@.-]{0,255}")


class ArgumentError(ValueError):
    """One public value cannot safely cross the remote shell boundary."""


def _ascii(value: str, label: str, maximum: int) -> None:
    """Require one nonempty bounded printable-ASCII field."""
    try:
        encoded = value.encode("ascii", "strict")
    except UnicodeEncodeError as exc:
        message = f"{label} must be ASCII"
        raise ArgumentError(message) from exc
    if (
        not encoded
        or len(encoded) > maximum
        or any(byte < ASCII_MIN or byte > ASCII_MAX for byte in encoded)
    ):
        message = f"{label} contains whitespace, control bytes, or excessive data"
        raise ArgumentError(message)


def validate_host(value: str) -> str:
    """Validate the shared rig contract's optional user plus DNS/IPv4 host."""
    _ascii(value, "PI_HOST", MAX_HOST_BYTES)
    if value.startswith("-") or value.count("@") > 1:
        message = "PI_HOST is not a destination"
        raise ArgumentError(message)
    user, separator, host = value.rpartition("@")
    if not separator:
        host = value
    elif USER_RE.fullmatch(user) is None:
        message = "PI_HOST user is invalid"
        raise ArgumentError(message)
    if host.startswith("-") or not host:
        message = "PI_HOST host is invalid"
        raise ArgumentError(message)
    if "." in host and all(character in "0123456789." for character in host):
        octets = host.split(".")
        if len(octets) != IPV4_FIELDS or any(
            not octet or len(octet) > IPV4_FIELD_BYTES or int(octet, 10) > IPV4_MAX
            for octet in octets
        ):
            message = "PI_HOST IPv4 address is invalid"
            raise ArgumentError(message)
        return value
    if len(host) > DNS_MAX or any(
        DNS_LABEL_RE.fullmatch(label) is None for label in host.split(".")
    ):
        message = "PI_HOST DNS name is invalid"
        raise ArgumentError(message)
    return value


def validate_serial(value: str) -> str:
    """Validate the shared rig contract's serial identifier."""
    if IDENTIFIER_RE.fullmatch(value) is None:
        message = "JLINK_SN has invalid rig identifier syntax"
        raise ArgumentError(message)
    return value


def validate_device(value: str) -> str:
    """Validate the shared rig contract's bounded SEGGER device identifier."""
    if IDENTIFIER_RE.fullmatch(value) is None:
        message = "JLINK_DEVICE has invalid rig identifier syntax"
        raise ArgumentError(message)
    return value


def validate_port(value: str) -> int:
    """Validate the existing unprivileged TCP port contract."""
    if not value.isascii() or not value.isdecimal():
        message = "port must be decimal"
        raise ArgumentError(message)
    port = int(value, 10)
    if not PORT_MIN <= port <= PORT_MAX:
        message = "port must be between 1024 and 65535"
        raise ArgumentError(message)
    return port


def remote_command(serial: str, port: str, *, device: str) -> str:
    """Build one exact OpenSSH remote-shell command from validated fields."""
    validate_serial(serial)
    validate_port(port)
    fields = [validate_device(device), serial, port]
    return shlex.join(["/usr/bin/python3", "-I", "-", "--", *fields])


def canonical_app(
    root: Path,
    selector: str,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    """Resolve exactly one app through the canonical option-safe CLI."""
    _ascii(selector, "application selector", MAX_SELECTOR_BYTES)
    argv = [
        "/usr/bin/python3",
        "-I",
        str(root / "scripts/dev/ra8_apps.py"),
        "id",
        "--",
        selector,
    ]
    result = runner(
        argv,
        cwd=root,
        env={
            "HOME": "/nonexistent",
            "LC_ALL": "C",
            "PATH": "/usr/bin:/bin",
            "PYTHONNOUSERSITE": "1",
        },
        capture_output=True,
        text=True,
        timeout=15,
        check=False,
    )
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 1 or APP_RE.fullmatch(lines[0]) is None:
        message = "application selector did not resolve to one canonical id"
        raise ArgumentError(message)
    return lines[0]


def _parser() -> argparse.ArgumentParser:
    """Build the closed argument-only command interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    for name in ("host", "serial", "device", "port"):
        validate.add_argument(f"--{name}", required=True)
    port = commands.add_parser("validate-port")
    port.add_argument("--port", required=True)
    remote = commands.add_parser("remote-command")
    remote.add_argument("--device", required=True)
    remote.add_argument("--serial", required=True)
    remote.add_argument("--port", required=True)
    app = commands.add_parser("canonical-app")
    app.add_argument("--root", required=True)
    app.add_argument("--selector", required=True)
    return parser


def main() -> int:
    """Dispatch validation without importing repository-local code."""
    args = _parser().parse_args()
    try:
        if args.command == "validate":
            validate_host(args.host)
            validate_serial(args.serial)
            validate_device(args.device)
            validate_port(args.port)
        elif args.command == "validate-port":
            validate_port(args.port)
        elif args.command == "remote-command":
            print(remote_command(args.serial, args.port, device=args.device))
        else:
            print(canonical_app(Path(args.root), args.selector))
    except ArgumentError as exc:
        print(f"remote_gdb_args: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
