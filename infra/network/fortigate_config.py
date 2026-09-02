# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline loading, rendering, and validation for the FortiGate declaration."""

from __future__ import annotations

import shlex
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_CONF = Path(__file__).with_name("fortigate-bench.conf")

MIN_CONFIG_COMMANDS = 120
MIN_CONFIG_BLOCKS = 10
MAX_LAN_NAME_LENGTH = 35
EDIT_TOKEN_COUNT = 2
CONFIG_MIN_TOKEN_COUNT = 2
SET_MIN_TOKEN_COUNT = 3
COMMAND_TOKEN_COUNT = 1
AP_RESERVED_IP = "10.0.40.10"
AP_RESERVED_MAC = "00:18:0a:7b:dd:eb"
STAR_RESERVED_IP = "10.0.40.101"
STAR_RESERVED_MAC = "00:05:1b:db:75:d3"
CAM_RESERVED_IP = "10.0.41.102"
CAM_RESERVED_MAC = "88:a2:9e:9b:d0:ea"
PROHIBITED_TRANSIENT_WIN_IPS = frozenset(("10.0.40.100", "10.0.40.103"))
PROHIBITED_TRANSIENT_WIN_MAC = "bc:fc:e7:da:3f:61"
EXPECTED_RESERVATIONS = (
    ("AP", "1", "1", AP_RESERVED_IP, AP_RESERVED_MAC, "MR18-AP"),
    ("star", "1", "2", STAR_RESERVED_IP, STAR_RESERVED_MAC, "star-bench-wired"),
    ("camera relay", "3", "1", CAM_RESERVED_IP, CAM_RESERVED_MAC, "cam-relay"),
)


@dataclass
class ConfigFrame:
    """Track one nested FortiOS config block while linting."""

    name: str
    edit: str | None = None
    fields: dict[str, str] = field(default_factory=dict)
    seen_edits: set[str] = field(default_factory=set)
    parent_config: str = ""
    parent_edit: str = ""


@dataclass
class ConfigLintState:
    """Collect parser state and findings for one FortiOS declaration."""

    errors: list[str] = field(default_factory=list)
    stack: list[ConfigFrame] = field(default_factory=list)
    reservations: list[dict[str, str]] = field(default_factory=list)
    config_count: int = 0


def load_config_lines(path: Path) -> list[str]:
    """Load replayable commands from a FortiOS declaration.

    Blank lines and full-line comments are documentation and are never sent to
    the console. ASCII decoding is deliberate: the repository accepts only
    7-bit source, and rejecting any other byte keeps dry-run and live replay
    byte-for-byte consistent.
    """
    text = path.read_text(encoding="ascii")
    return [
        line.rstrip()
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def render_config_lines(lines: list[str], lan: str) -> list[str]:
    """Render declaration commands for the detected primary LAN name."""
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
    if not lan or len(lan) > MAX_LAN_NAME_LENGTH or any(char not in allowed for char in lan):
        message = f"invalid FortiOS interface name {lan!r}"
        raise ValueError(message)
    if lan == "internal":
        return list(lines)
    return [line.replace('"internal"', f'"{lan}"') for line in lines]


def _lint_edit(state: ConfigLintState, tokens: list[str], line_number: int) -> None:
    """Validate and open one edit context."""
    if not state.stack:
        state.errors.append(f"line {line_number}: edit outside a config block")
        return
    if len(tokens) != EDIT_TOKEN_COUNT:
        state.errors.append(f"line {line_number}: edit requires exactly one key")
        return
    frame = state.stack[-1]
    if frame.edit is not None:
        state.errors.append(
            f"line {line_number}: edit {tokens[1]!r} opened before "
            f"edit {frame.edit!r} was closed with next"
        )
        return
    if tokens[1] in frame.seen_edits:
        state.errors.append(
            f"line {line_number}: duplicate edit {tokens[1]!r} in config {frame.name}"
        )
    frame.seen_edits.add(tokens[1])
    frame.edit = tokens[1]
    frame.fields = {}


def _lint_set(state: ConfigLintState, tokens: list[str], line_number: int) -> None:
    """Validate a set command and retain reservation fields."""
    if not state.stack:
        state.errors.append(f"line {line_number}: set outside a config block")
        return
    if len(tokens) < SET_MIN_TOKEN_COUNT:
        state.errors.append(f"line {line_number}: set requires a key and value")
        return
    frame = state.stack[-1]
    if frame.name != "reserved-address" or frame.edit is None:
        return
    key = tokens[1]
    if key in frame.fields:
        state.errors.append(
            f"line {line_number}: reservation edit {frame.edit!r} sets {key!r} more than once"
        )
    frame.fields[key] = " ".join(tokens[2:])


def _lint_next(state: ConfigLintState, tokens: list[str], line_number: int) -> None:
    """Validate and close one edit context."""
    if len(tokens) != COMMAND_TOKEN_COUNT:
        state.errors.append(f"line {line_number}: next accepts no arguments")
        return
    if not state.stack or state.stack[-1].edit is None:
        state.errors.append(f"line {line_number}: next without an open edit")
        return
    frame = state.stack[-1]
    if frame.name == "reserved-address":
        state.reservations.append(
            {
                "edit": frame.edit,
                "parent_config": frame.parent_config,
                "parent_edit": frame.parent_edit,
                **frame.fields,
            }
        )
    frame.edit = None
    frame.fields = {}


def _lint_end(state: ConfigLintState, tokens: list[str], line_number: int) -> None:
    """Validate and close one config block."""
    if len(tokens) != COMMAND_TOKEN_COUNT:
        state.errors.append(f"line {line_number}: end accepts no arguments")
        return
    if not state.stack:
        state.errors.append(f"line {line_number}: end without an open config block")
        return
    frame = state.stack.pop()
    if frame.edit is not None:
        state.errors.append(
            f"line {line_number}: config {frame.name} ended before "
            f"edit {frame.edit!r} was closed with next"
        )


def _lint_command(state: ConfigLintState, tokens: list[str], line_number: int) -> None:
    """Dispatch one tokenized FortiOS declaration command."""
    command = tokens[0]
    if command == "config":
        if len(tokens) < CONFIG_MIN_TOKEN_COUNT:
            state.errors.append(f"line {line_number}: config requires a block name")
            return
        parent = state.stack[-1] if state.stack else None
        state.stack.append(
            ConfigFrame(
                name=" ".join(tokens[1:]),
                parent_config=parent.name if parent else "",
                parent_edit=parent.edit if parent and parent.edit else "",
            )
        )
        state.config_count += 1
    elif command == "edit":
        _lint_edit(state, tokens, line_number)
    elif command == "set":
        _lint_set(state, tokens, line_number)
    elif command == "next":
        _lint_next(state, tokens, line_number)
    elif command == "end":
        _lint_end(state, tokens, line_number)
    else:
        state.errors.append(f"line {line_number}: unsupported replay command {command!r}")


def _lint_structure(lines: list[str]) -> ConfigLintState:
    """Parse structural commands and collect reservation records."""
    state = ConfigLintState()
    for line_number, line in enumerate(lines, start=1):
        try:
            tokens = shlex.split(line, comments=False, posix=True)
        except ValueError as exc:
            state.errors.append(f"line {line_number}: cannot parse {line!r}: {exc}")
            continue
        if not tokens:
            state.errors.append(f"line {line_number}: empty command reached replay stream")
            continue
        _lint_command(state, tokens, line_number)
    if state.stack:
        names = ", ".join(frame.name for frame in state.stack)
        state.errors.append(f"unclosed config block(s): {names}")
    return state


def _reservation_inventory_errors(reservations: list[dict[str, str]]) -> list[str]:
    """Reject incomplete, duplicate, and transient-client reservations."""
    errors: list[str] = []
    seen_ips: dict[str, str] = {}
    seen_macs: dict[str, str] = {}
    for reservation in reservations:
        edit = reservation["edit"]
        ip = reservation.get("ip", "")
        mac = reservation.get("mac", "").lower()
        if not ip or not mac:
            errors.append(f"reserved-address edit {edit!r} must set both ip and mac")
            continue
        if ip in seen_ips:
            errors.append(
                f"reserved address {ip} is duplicated by edits {seen_ips[ip]!r} and {edit!r}"
            )
        else:
            seen_ips[ip] = edit
        if mac in seen_macs:
            errors.append(
                f"reserved MAC {mac} is duplicated by edits {seen_macs[mac]!r} and {edit!r}"
            )
        else:
            seen_macs[mac] = edit
        if ip in PROHIBITED_TRANSIENT_WIN_IPS:
            errors.append(f"transient Win11 address {ip} must not be reserved")
        if mac == PROHIBITED_TRANSIENT_WIN_MAC:
            errors.append(f"transient Win11 MAC {mac} must not be reserved")
    return errors


def _required_reservation_errors(reservations: list[dict[str, str]]) -> list[str]:
    """Require the complete reviewed reservation inventory exactly once."""
    errors: list[str] = []
    for (
        label,
        server_edit,
        reservation_edit,
        expected_ip,
        expected_mac,
        description,
    ) in EXPECTED_RESERVATIONS:
        matches = [
            item
            for item in reservations
            if item.get("ip") == expected_ip
            and item.get("mac", "").lower() == expected_mac
            and item.get("description") == description
            and item.get("parent_config") == "system dhcp server"
            and item.get("parent_edit") == server_edit
            and item.get("edit") == reservation_edit
        ]
        if len(matches) != 1:
            errors.append(
                f"{label} reservation must occur exactly once as "
                f"DHCP server {server_edit} reservation {reservation_edit}, "
                f"{expected_mac} -> {expected_ip}, description {description!r}; "
                f"found {len(matches)}"
            )
    if len(reservations) != len(EXPECTED_RESERVATIONS):
        errors.append(
            f"reservation inventory contains {len(reservations)} entries; "
            f"expected exactly {len(EXPECTED_RESERVATIONS)}"
        )
    return errors


def config_lint_errors(lines: list[str]) -> list[str]:
    """Return structural and bench-reservation errors in a replay stream."""
    state = _lint_structure(lines)
    errors = list(state.errors)
    if len(lines) < MIN_CONFIG_COMMANDS:
        errors.append(
            f"non-vacuity: only {len(lines)} replay commands; expected at least "
            f"{MIN_CONFIG_COMMANDS}"
        )
    if state.config_count < MIN_CONFIG_BLOCKS:
        errors.append(
            f"non-vacuity: only {state.config_count} config blocks; expected at least "
            f"{MIN_CONFIG_BLOCKS}"
        )
    errors.extend(_reservation_inventory_errors(state.reservations))
    errors.extend(_required_reservation_errors(state.reservations))
    return errors


def require_valid_config(lines: list[str], source: Path) -> None:
    """Raise ValueError when a rendered declaration cannot be replayed safely."""
    errors = config_lint_errors(lines)
    if errors:
        details = "\n".join(f"  - {error}" for error in errors)
        message = f"{source}: FortiOS declaration is invalid:\n{details}"
        raise ValueError(message)


def read_valid_config(path: Path, lan: str = "internal") -> list[str]:
    """Load, render, and validate the command stream used by live replay."""
    lines = render_config_lines(load_config_lines(path), lan)
    require_valid_config(lines, path)
    return lines
