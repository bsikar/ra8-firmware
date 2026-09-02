#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Pure AP command, PSK, and transcript safety boundaries."""

from __future__ import annotations

import re
import shlex

AP_STATUS_MARKER = "__RA8_AP_RC__"
ASCII_CONTROL_LIMIT = 32
ASCII_DELETE = 127
PRINTABLE_ASCII_MAX = 126
WPA2_PSK_MIN = 8
WPA2_PSK_MAX = 63
AP_SETUP_COMMANDS = (
    "uci set wireless.radio1.channel='6'",
    "uci set wireless.radio1.htmode='HT20'",
    "uci set wireless.radio1.disabled='0'",
    "uci delete wireless.bench",
    "uci set wireless.bench='wifi-iface'",
    "uci set wireless.bench.device='radio1'",
    "uci set wireless.bench.mode='ap'",
    "uci set wireless.bench.network='lan'",
    "uci set wireless.bench.ssid='ra8-bench'",
    "uci set wireless.bench.encryption='psk2'",
    "uci set wireless.iot_5g.disabled='1'",
    "uci set wireless.iot_2g.disabled='1'",
    "uci set wireless.guest_5g.disabled='1'",
    "uci set wireless.guest_2g.disabled='1'",
    "uci set dhcp.lan.ignore='1'",
    "uci set dhcp.lan.dhcpv4='disabled'",
    "uci set dhcp.lan.dhcpv6='disabled'",
    "uci set dhcp.lan.ra='disabled'",
)


def mask_secrets(text: str, secrets: list[str]) -> str:
    """Mask unique credentials longest-first so prefixes cannot leak suffixes."""
    for secret in sorted(set(secrets), key=len, reverse=True):
        text = text.replace(secret, "<REDACTED>")
    return text


def uci_assignment(option: str, value: str) -> str:
    """Return one shell-safe UCI assignment with no serial control bytes."""
    if option != "wireless.bench.key":
        raise ValueError
    if any(ord(char) < ASCII_CONTROL_LIMIT or ord(char) == ASCII_DELETE for char in value):
        raise ValueError
    return f"uci set {option}={shlex.quote(value)}"


def validate_wpa2_psk(value: str) -> str:
    """Accept exactly a printable 8..63-byte WPA2 passphrase."""
    if not WPA2_PSK_MIN <= len(value) <= WPA2_PSK_MAX:
        raise ValueError
    if any(ord(char) < ASCII_CONTROL_LIMIT or ord(char) > PRINTABLE_ASCII_MAX for char in value):
        raise ValueError
    return value


def checked_command(command: str) -> str:
    """Append one fixed exit-status marker to an AP shell command."""
    return f"{command}; _ra8_rc=$?; printf '\\n{AP_STATUS_MARKER}%d\\n' \"$_ra8_rc\""


def status_succeeded(output: str) -> bool:
    """Accept exactly one zero exit-status marker in AP output."""
    return re.findall(rf"{AP_STATUS_MARKER}([0-9]+)", output) == ["0"]
