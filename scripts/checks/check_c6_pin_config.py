#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the C6 Kconfig defaults agree with the pin map they are derived from.

Why two files exist
-------------------
``coprocessor/esp32c6/pins.env`` is the SINGLE SOURCE OF TRUTH for the
RA8-host <-> ESP32-C6 SPI wiring. It is a plain ``KEY=value`` fragment because
its consumers are shell (``build.sh``, ``flash.sh``) and, on the RA8 side,
humans reading one file to learn the pinout.

``coprocessor/esp32c6/sdkconfig.defaults`` carries the SAME numbers again in
Kconfig syntax, because that is the only form esp-idf reads. It is a DERIVED
artifact, and it is deliberately byte-stable: the bench-proven C6 image was
built from exactly these lines, so it is verified against pins.env rather than
regenerated from it.

Two files holding one fact drift silently, and a drift here is expensive: the
build succeeds, the firmware flashes, and the SPI link simply never comes up
because the C6 is driving a different pin than the RA8 is. Nothing downstream
of the mistake can detect it -- which is what makes this a gate and not a
convention.

What is compared
----------------
Every SPI signal (CS, COPI/CIPO, SCK, DATA_READY, HANDSHAKE, RESET), the chip
target, the flash size, and the dev-board preset. The last two are the
interesting ones: esp-idf and esp-hosted-mcu encode both in the KEY
(``CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y``, ``CONFIG_ESP_HOST_DEV_BOARD_NONE=y``)
rather than in the value, so each is parsed out of the key name and compared
against ``C6_FLASH_SIZE`` / ``C6_DEV_BOARD``.

The dev-board preset is load-bearing and easy to miss. esp-hosted-mcu ships
presets for Espressif's own dev boards, and selecting one OVERRIDES the SPI pin
leaves -- so with any board but ``NONE`` selected, every pin this gate compares
is silently ignored by the build and the C6 comes up on the preset's pins
instead of ours. Comparing the enabled symbol's SUFFIX rather than merely
asserting the symbol is present catches the drift in both of its shapes: the
selection vanishing, and the selection moving to a different board.

Two of the data signals carry Espressif's legacy names in their Kconfig symbols.
Those symbols belong to upstream and cannot be renamed from here; the mapping
table is where they are tied to this project's COPI/CIPO vocabulary.

What is checked on the RA8 side
-------------------------------
pins.env also records where each signal LANDS on the EK-RA8D2 (MCU pin and
J26 hole) and which SW4 DIP positions the link needs. That map IS restated in
one other place -- ``port/esp-hosted/inc/ra8_esp_hosted_pins.h``, which the
esp-hosted port compiles against -- so the two are diffed here. They have
already drifted once: the port header was first written from the probe's
candidate list while the module was disconnected, and the rebuilt harness was
later characterised with HANDSHAKE and DATA_READY the other way round. A pin
map that only one file knows is a pin map that will be wrong again.

Independently of that diff, the map is checked for the failures that actually
happen to a pin map: a signal named at one end and not the other, a pin or
hole name that is not a pin or hole name, two signals silently claiming the
same pin after a copy-paste, and a missing SW4 position. That bank is not
incidental: SW4-4 ON with SW4-3 OFF holds the
Pmod1 bus switches open, so J26-1..J26-4 never reach the MCU, and misreading
it cost a full bench day chasing a harness that was fine.

This is a pure text comparison of two committed files -- no esp-idf, no
toolchain, no hardware -- so it runs in CI on any box. ``build.sh`` invokes
this same script before it builds, so the bench and CI apply one rule.

Non-vacuity
-----------
``--selftest`` drives the comparator with crafted file bodies: an agreeing
pair must be silent, and a disagreeing pair, a missing key on either side, a
missing flash-size key, and a dev-board preset that either moved to another
board or stopped being selected must each be reported.

Run::

    check_c6_pin_config.py             # gate (fail on any drift)
    check_c6_pin_config.py --selftest  # prove the comparator both ways

Exit 0 when the two files agree, 1 on drift or a failing selftest, 2 when a
file cannot be read.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
C6_DIR = REPO_ROOT / "coprocessor" / "esp32c6"
PINS_ENV = C6_DIR / "pins.env"
SDKCONFIG = C6_DIR / "sdkconfig.defaults"
HOST_FW_VER = (
    REPO_ROOT / "libs" / "third_party" / "esp-hosted" / "host" / "esp_hosted_host_fw_ver.h"
)

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

# (Kconfig symbol, pins.env key, human label for the failure text).
# The two data-signal symbols keep Espressif's legacy spelling because they are
# upstream esp-hosted-mcu Kconfig names and cannot be renamed from this
# repository; our own signal names are COPI/CIPO, which is what the labels use.
PIN_PAIRS: tuple[tuple[str, str, str], ...] = (
    ("CONFIG_ESP_SPI_HSPI_GPIO_CS", "C6_PIN_CS", "CS (Chip Select)"),
    ("CONFIG_ESP_SPI_HSPI_GPIO_MOSI", "C6_PIN_COPI", "COPI (Controller Out)"),
    ("CONFIG_ESP_SPI_HSPI_GPIO_MISO", "C6_PIN_CIPO", "CIPO (Controller In)"),
    ("CONFIG_ESP_SPI_HSPI_GPIO_CLK", "C6_PIN_SCK", "SCK (clock)"),
    ("CONFIG_ESP_SPI_GPIO_DATA_READY", "C6_PIN_DATA_READY", "DATA_READY"),
    ("CONFIG_ESP_SPI_GPIO_HANDSHAKE", "C6_PIN_HANDSHAKE", "HANDSHAKE"),
    ("CONFIG_ESP_SPI_GPIO_RESET", "C6_PIN_RESET", "RESET"),
)

# Same shape, for the non-pin settings that are also stated twice.
VALUE_PAIRS: tuple[tuple[str, str, str], ...] = (
    ("CONFIG_IDF_TARGET", "ESP_TARGET", "chip target"),
)

# esp-idf encodes the flash size in the SYMBOL NAME, not the value.
_FLASHSIZE_RE = re.compile(r"^CONFIG_ESPTOOLPY_FLASHSIZE_([0-9]+MB)$")
FLASH_SIZE_KEY = "C6_FLASH_SIZE"

# esp-hosted-mcu encodes the dev-board preset in the SYMBOL NAME too. Selecting
# a board OVERRIDES the SPI pin leaves above, which would make every pin
# comparison in this gate describe a build that does not happen.
_DEV_BOARD_RE = re.compile(r"^CONFIG_ESP_HOST_DEV_BOARD_([A-Z0-9_]+)$")
DEV_BOARD_KEY = "C6_DEV_BOARD"

# (label, C6 GPIO key, RA8 landing-pin key, RA8 J26 hole key). The RA8 side of
# the harness is recorded in pins.env too, because a pin map is only useful if
# it names BOTH ends: "GPIO4" alone does not tell anyone which MCU pin to read.
RA8_TRIPLES: tuple[tuple[str, str, str, str], ...] = (
    ("CS (Chip Select)", "C6_PIN_CS", "RA8_PIN_CS", "RA8_J26_CS"),
    ("COPI (Controller Out)", "C6_PIN_COPI", "RA8_PIN_COPI", "RA8_J26_COPI"),
    ("CIPO (Controller In)", "C6_PIN_CIPO", "RA8_PIN_CIPO", "RA8_J26_CIPO"),
    ("SCK (clock)", "C6_PIN_SCK", "RA8_PIN_SCK", "RA8_J26_SCK"),
    ("DATA_READY", "C6_PIN_DATA_READY", "RA8_PIN_DATA_READY", "RA8_J26_DATA_READY"),
    ("HANDSHAKE", "C6_PIN_HANDSHAKE", "RA8_PIN_HANDSHAKE", "RA8_J26_HANDSHAKE"),
    ("RESET", "C6_PIN_RESET", "RA8_PIN_RESET", "RA8_J26_RESET"),
)

# The four EK-RA8D2 DIP switches that decide whether J26-1..J26-4 reach the MCU
# at all. Misreading this bank was the whole 2026-07-26 C6 outage, so the
# required positions are recorded as data rather than as prose in one doc.
SW4_KEYS: tuple[str, ...] = ("RA8_SW4_1", "RA8_SW4_2", "RA8_SW4_3", "RA8_SW4_4")
SW4_VALUES: tuple[str, ...] = ("ON", "OFF")

# A signal with no wire: "none" on the RA8 side must pair with -1 on the C6
# side, in both directions. Half a disconnection recorded is a pin map that
# claims a link nobody built.
UNWIRED_RA8 = "none"
UNWIRED_C6 = "-1"

_RA8_PIN_RE = re.compile(r"^P[0-9]{3}$")

PORT_PIN_HEADER = REPO_ROOT / "port" / "esp-hosted" / "inc" / "ra8_esp_hosted_pins.h"
"""The esp-hosted port's copy of the RA8-side map, diffed against pins.env."""

PORT_PIN_ROWS: tuple[tuple[str, str, str], ...] = (
    ("CS", "k_ra8_esp_hosted_pin_chip_select", "RA8_PIN_CS"),
    ("COPI", "k_ra8_esp_hosted_pin_copi", "RA8_PIN_COPI"),
    ("CIPO", "k_ra8_esp_hosted_pin_cipo", "RA8_PIN_CIPO"),
    ("SCK", "k_ra8_esp_hosted_pin_sck", "RA8_PIN_SCK"),
    ("HANDSHAKE", "k_ra8_esp_hosted_pin_handshake", "RA8_PIN_HANDSHAKE"),
    ("DATA_READY", "k_ra8_esp_hosted_pin_data_ready", "RA8_PIN_DATA_READY"),
    ("RESET", "k_ra8_esp_hosted_pin_reset", "RA8_PIN_RESET"),
)
"""Signal, the port header's enumerator, and the pins.env key it must match."""

BOARD_SYMBOL_TO_PIN: dict[str, str] = {
    "k_ra8_board_pmod1_spi_cs": "P804",
    "k_ra8_board_pmod1_spi_copi": "P801",
    "k_ra8_board_pmod1_spi_cipo": "P802",
    "k_ra8_board_pmod1_spi_sck": "P803",
    "k_ra8_board_pmod1_irq": "P006",
    "k_ra8_board_pmod1_reset": "P402",
    "k_ra8_board_pmod1_gpio_a": "P412",
    "k_ra8_board_pmod1_gpio_b": "P413",
    "k_ra8_pin_none": "none",
}
"""Board-layer Pmod1 enumerators and the MCU pin each names.

The port header cites board symbols rather than pin numbers -- that is the
point of the board layer -- so resolving them is what lets the two files be
compared at all. The values come from
``libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_connectors.h``, which carries
the board User's Manual citation for every row.
"""

_PORT_ROW_RE = re.compile(
    r"(?P<enum>k_ra8_esp_hosted_pin_[a-z_]+)\s*=\s*\(uint16_t\)(?P<sym>k_ra8_[a-z0-9_]+)"
)


def parse_port_header(text: str) -> dict[str, str]:
    """Return {enumerator: board symbol} for every row of the port pin map."""
    return {m.group("enum"): m.group("sym") for m in _PORT_ROW_RE.finditer(text)}


def check_port_header(pins: dict[str, str], header: str) -> list[str]:
    """Return one message per disagreement with the port's pin map.

    Args:
        pins: Parsed pins.env assignments (the source of truth).
        header: Text of ``ra8_esp_hosted_pins.h``.

    Returns:
        Human-readable findings; empty when the header agrees with pins.env.
    """
    rows = parse_port_header(header)
    findings: list[str] = []
    for label, enum_name, pin_key in PORT_PIN_ROWS:
        if enum_name not in rows:
            findings.append(f"{label}: the port pin header does not define {enum_name}")
            continue
        symbol = rows[enum_name]
        if symbol not in BOARD_SYMBOL_TO_PIN:
            findings.append(
                f"{label}: the port pin header names {symbol}, which this gate cannot "
                f"resolve to an MCU pin; add it to BOARD_SYMBOL_TO_PIN"
            )
            continue
        if pin_key not in pins:
            findings.append(f"{label}: pins.env is missing {pin_key}")
            continue
        resolved = BOARD_SYMBOL_TO_PIN[symbol]
        if resolved != pins[pin_key]:
            findings.append(
                f"{label}: the port pin header says {symbol} ({resolved}) "
                f"but pins.env says {pin_key}={pins[pin_key]}"
            )
    return findings


_RA8_HOLE_RE = re.compile(r"^J26-([0-9]{1,2})$")

_ASSIGN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$")

# Shortest value that can carry a matched pair of surrounding quotes ("" is 2).
_MIN_QUOTED_LEN = 2


def parse_assignments(text: str) -> dict[str, str]:
    """Parse ``KEY=value`` lines, ignoring comments and blanks.

    Handles both file formats: the shell fragment and Kconfig defaults share
    this syntax, and Kconfig's quoted string values are unquoted here so
    ``CONFIG_IDF_TARGET="esp32c6"`` compares equal to ``ESP_TARGET=esp32c6``.

    Args:
        text: File contents.

    Returns:
        Mapping of key to unquoted value.
    """
    out: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = _ASSIGN_RE.match(line)
        if match is None:
            continue
        key, value = match.group(1), match.group(2)
        if len(value) >= _MIN_QUOTED_LEN and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        out[key] = value
    return out


def _flash_size(sdk: dict[str, str]) -> str | None:
    """Return the flash size encoded in an enabled FLASHSIZE symbol.

    Args:
        sdk: Parsed sdkconfig assignments.

    Returns:
        The size text (e.g. "16MB"), or None when no such symbol is enabled.
    """
    for key, value in sdk.items():
        match = _FLASHSIZE_RE.match(key)
        if match is not None and value == "y":
            return match.group(1)
    return None


def _dev_board(sdk: dict[str, str]) -> str | None:
    """Return the dev-board preset encoded in an enabled DEV_BOARD symbol.

    Args:
        sdk: Parsed sdkconfig assignments.

    Returns:
        The board name lower-cased (e.g. "none"), or None when no such symbol
        is enabled -- which is itself a finding, because the preset decides
        whether our pin leaves are honoured at all.
    """
    for key, value in sdk.items():
        match = _DEV_BOARD_RE.match(key)
        if match is not None and value == "y":
            return match.group(1).lower()
    return None


def _check_one_signal(label: str, c6: str, ra8_pin: str, ra8_hole: str) -> list[str]:
    """Return findings for one signal's RA8-side entry.

    Args:
        label: Human-readable signal name.
        c6: C6 GPIO number as written in pins.env.
        ra8_pin: RA8 landing pin, or "none".
        ra8_hole: J26 hole, or "none".

    Returns:
        Human-readable findings; empty when the entry is well-formed.
    """
    unwired_c6 = c6 == UNWIRED_C6
    unwired_ra8 = ra8_pin == UNWIRED_RA8 and ra8_hole == UNWIRED_RA8
    if unwired_c6 != unwired_ra8:
        return [
            f"{label}: half-recorded connection -- C6 side is "
            f"{'disconnected' if unwired_c6 else c6} but RA8 side is "
            f"{ra8_pin}/{ra8_hole}"
        ]
    if unwired_ra8:
        return []

    findings: list[str] = []
    if _RA8_PIN_RE.match(ra8_pin) is None:
        findings.append(f"{label}: RA8 pin {ra8_pin!r} is not a Pnnn pin name or 'none'")
    if _RA8_HOLE_RE.match(ra8_hole) is None:
        findings.append(f"{label}: J26 hole {ra8_hole!r} is not a J26-n hole or 'none'")
    return findings


def _check_uniqueness(pins: dict[str, str]) -> list[str]:
    """Return findings for any RA8 pin or J26 hole claimed by two signals.

    Args:
        pins: Parsed pins.env assignments.

    Returns:
        Human-readable findings; empty when every wired entry is unique.
    """
    findings: list[str] = []
    for what, index in (("RA8 pin", 2), ("J26 hole", 3)):
        seen: dict[str, str] = {}
        for triple in RA8_TRIPLES:
            value = pins.get(triple[index], UNWIRED_RA8)
            if value == UNWIRED_RA8:
                continue
            if value in seen:
                findings.append(f"{what} {value} is claimed by both {seen[value]} and {triple[0]}")
            seen[value] = triple[0]
    return findings


def check_ra8_side(pins: dict[str, str]) -> list[str]:
    """Return one message per defect in the RA8-side half of the pin map.

    The C6-side numbers are checked against sdkconfig.defaults by ``compare``;
    nothing downstream can check the RA8 side that way, because no second file
    restates it. What is checkable is that the map is COMPLETE and INTERNALLY
    CONSISTENT: every signal names both ends or neither, the names are
    well-formed, no two signals claim one pin or one hole, and the SW4
    positions the link depends on are recorded.

    Args:
        pins: Parsed pins.env assignments.

    Returns:
        Human-readable findings; empty when the RA8 side is well-formed.
    """
    findings: list[str] = []
    for label, c6_key, pin_key, hole_key in RA8_TRIPLES:
        missing = [k for k in (c6_key, pin_key, hole_key) if k not in pins]
        if missing:
            findings.extend(f"{label}: pins.env is missing {k}" for k in missing)
            continue
        findings.extend(_check_one_signal(label, pins[c6_key], pins[pin_key], pins[hole_key]))

    findings.extend(_check_uniqueness(pins))

    for key in SW4_KEYS:
        if key not in pins:
            findings.append(f"SW4 positions: pins.env is missing {key}")
        elif pins[key] not in SW4_VALUES:
            findings.append(f"SW4 positions: {key}={pins[key]!r} is not one of {SW4_VALUES}")
    return findings


FW_VERSION_KEY = "ESP_HOSTED_MCU_FW_VERSION"

# The three #defines the vendored host driver states its own version with.
_FW_VER_MACROS: tuple[str, str, str] = (
    "ESP_HOSTED_VERSION_MAJOR_1",
    "ESP_HOSTED_VERSION_MINOR_1",
    "ESP_HOSTED_VERSION_PATCH_1",
)


def parse_host_fw_version(text: str) -> str | None:
    """Return the vendored host driver's own version as ``major.minor.patch``.

    Args:
        text: Body of ``host/esp_hosted_host_fw_ver.h`` from the vendor tree.

    Returns:
        The dotted version, or ``None`` when any of the three macros is absent
        (which means the vendored header changed shape and this check can no
        longer speak for it -- reported, never silently skipped).
    """
    parts: list[str] = []
    for macro in _FW_VER_MACROS:
        match = re.search(rf"^\s*#define\s+{re.escape(macro)}\s+(\d+)\s*$", text, re.MULTILINE)
        if match is None:
            return None
        parts.append(match.group(1))
    return ".".join(parts)


def check_fw_version_lock(pins: dict[str, str], header_text: str) -> list[str]:
    """Return findings when the co-processor image and the host driver disagree.

    ``pins.env`` names the esp-hosted-mcu firmware the CO-PROCESSOR image is
    built from; ``esp_hosted_host_fw_ver.h`` states the version of the HOST
    driver vendored into this tree. Both come from one pinned upstream commit,
    so they must be identical -- and upstream itself compares them at run time
    and warns about "RPC timeouts" when they are not.

    This is a gate rather than a convention because the mismatch is otherwise
    silent in CI and expensive on the bench:
    ``examples/ek_ra8d2/hw_validated/c6/c6_fw_version`` asserts the
    co-processor's reported version against these very macros, so bumping the
    vendor pin without reflashing the C6 turns a hardware test red with no
    hint, from here, of why.

    Args:
        pins: Parsed pins.env assignments.
        header_text: Body of the vendored host firmware-version header.

    Returns:
        Human-readable findings; empty when the two agree.
    """
    findings: list[str] = []
    host = parse_host_fw_version(header_text)
    if host is None:
        findings.append(
            "firmware version: could not read "
            f"{', '.join(_FW_VER_MACROS)} out of {HOST_FW_VER.name} "
            "-- the vendored header changed shape"
        )
        return findings
    if FW_VERSION_KEY not in pins:
        findings.append(f"firmware version: pins.env is missing {FW_VERSION_KEY}")
        return findings
    if pins[FW_VERSION_KEY] != host:
        findings.append(
            f"firmware version: pins.env {FW_VERSION_KEY}={pins[FW_VERSION_KEY]} "
            f"but the vendored host driver is {host} "
            "-- host and co-processor must come from one upstream commit"
        )
    return findings


def compare(pins: dict[str, str], sdk: dict[str, str]) -> list[str]:
    """Return one message per disagreement between the two parsed files.

    Args:
        pins: Parsed pins.env assignments (the source of truth).
        sdk: Parsed sdkconfig.defaults assignments (the derived artifact).

    Returns:
        Human-readable findings; empty when the two agree.
    """
    findings: list[str] = []

    for sdk_key, pin_key, label in PIN_PAIRS + VALUE_PAIRS:
        if pin_key not in pins:
            findings.append(f"{label}: pins.env is missing {pin_key}")
            continue
        if sdk_key not in sdk:
            findings.append(f"{label}: sdkconfig.defaults is missing {sdk_key}")
            continue
        if pins[pin_key] != sdk[sdk_key]:
            findings.append(
                f"{label}: pins.env {pin_key}={pins[pin_key]} "
                f"but sdkconfig.defaults {sdk_key}={sdk[sdk_key]}"
            )

    size = _flash_size(sdk)
    if FLASH_SIZE_KEY not in pins:
        findings.append(f"flash size: pins.env is missing {FLASH_SIZE_KEY}")
    elif size is None:
        findings.append(
            "flash size: sdkconfig.defaults enables no CONFIG_ESPTOOLPY_FLASHSIZE_<n>MB symbol"
        )
    elif size != pins[FLASH_SIZE_KEY]:
        findings.append(
            f"flash size: pins.env {FLASH_SIZE_KEY}={pins[FLASH_SIZE_KEY]} "
            f"but sdkconfig.defaults enables CONFIG_ESPTOOLPY_FLASHSIZE_{size}"
        )

    board = _dev_board(sdk)
    if DEV_BOARD_KEY not in pins:
        findings.append(f"dev board: pins.env is missing {DEV_BOARD_KEY}")
    elif board is None:
        findings.append(
            "dev board: sdkconfig.defaults enables no CONFIG_ESP_HOST_DEV_BOARD_<X> "
            "symbol -- without an explicit selection the pin leaves above may be "
            "overridden by whatever preset upstream defaults to"
        )
    elif board != pins[DEV_BOARD_KEY]:
        findings.append(
            f"dev board: pins.env {DEV_BOARD_KEY}={pins[DEV_BOARD_KEY]} "
            f"but sdkconfig.defaults enables CONFIG_ESP_HOST_DEV_BOARD_{board.upper()} "
            "-- a dev-board preset OVERRIDES the SPI pin leaves, so every pin "
            "compared above would be ignored by the build"
        )

    findings.extend(check_ra8_side(pins))

    try:
        header = PORT_PIN_HEADER.read_text(encoding="utf-8")
    except OSError as exc:
        findings.append(f"port pin header: cannot read {PORT_PIN_HEADER}: {exc}")
    else:
        findings.extend(check_port_header(pins, header))
    return findings


# ---------------------------------------------------------------------------
# Selftest -- the comparator is driven with crafted bodies, so nothing is
# written into the tree and the real files are never modified.
# ---------------------------------------------------------------------------

_GOOD_PINS = """
# comment ignored
C6_PIN_CS=0
C6_PIN_COPI=1
C6_PIN_CIPO=2
C6_PIN_SCK=3
C6_PIN_DATA_READY=4
C6_PIN_HANDSHAKE=6
C6_PIN_RESET=-1
RA8_PIN_CS=P804
RA8_PIN_COPI=P801
RA8_PIN_CIPO=P802
RA8_PIN_SCK=P803
RA8_PIN_DATA_READY=P402
RA8_PIN_HANDSHAKE=P006
RA8_PIN_RESET=none
RA8_J26_CS=J26-1
RA8_J26_COPI=J26-2
RA8_J26_CIPO=J26-3
RA8_J26_SCK=J26-4
RA8_J26_DATA_READY=J26-8
RA8_J26_HANDSHAKE=J26-7
RA8_J26_RESET=none
RA8_SW4_1=OFF
RA8_SW4_2=OFF
RA8_SW4_3=ON
RA8_SW4_4=OFF
C6_FLASH_SIZE=16MB
C6_DEV_BOARD=none
ESP_TARGET=esp32c6
"""

_GOOD_SDK = """
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_HOST_DEV_BOARD_NONE=y
CONFIG_ESP_SPI_HSPI_GPIO_CS=0
CONFIG_ESP_SPI_HSPI_GPIO_MOSI=1
CONFIG_ESP_SPI_HSPI_GPIO_MISO=2
CONFIG_ESP_SPI_HSPI_GPIO_CLK=3
CONFIG_ESP_SPI_GPIO_DATA_READY=4
CONFIG_ESP_SPI_GPIO_HANDSHAKE=6
CONFIG_ESP_SPI_GPIO_RESET=-1
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
"""


def _selftest_cases_kconfig() -> list[tuple[str, str, str, bool]]:
    """Return the crafted cases for the pins.env <-> sdkconfig.defaults half.

    Returns:
        One case per drift shape between the two files, plus the agreeing
        control that must stay silent. The dev-board preset has its own group
        (``_selftest_cases_dev_board``).
    """
    return [
        ("agreeing pair", _GOOD_PINS, _GOOD_SDK, False),
        (
            "a pin number drifted",
            _GOOD_PINS,
            _GOOD_SDK.replace("GPIO_CLK=3", "GPIO_CLK=7"),
            True,
        ),
        (
            "COPI drifted (legacy upstream symbol)",
            _GOOD_PINS.replace("C6_PIN_COPI=1", "C6_PIN_COPI=9"),
            _GOOD_SDK,
            True,
        ),
        (
            "sdkconfig lost a symbol",
            _GOOD_PINS,
            _GOOD_SDK.replace("CONFIG_ESP_SPI_GPIO_HANDSHAKE=6\n", ""),
            True,
        ),
        (
            "pins.env lost a key",
            _GOOD_PINS.replace("C6_PIN_DATA_READY=4\n", ""),
            _GOOD_SDK,
            True,
        ),
        (
            "flash size drifted",
            _GOOD_PINS,
            _GOOD_SDK.replace("FLASHSIZE_16MB=y", "FLASHSIZE_4MB=y"),
            True,
        ),
        (
            "no flash size enabled at all",
            _GOOD_PINS,
            _GOOD_SDK.replace(
                "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=n"
            ),
            True,
        ),
        (
            "chip target drifted",
            _GOOD_PINS,
            _GOOD_SDK.replace('CONFIG_IDF_TARGET="esp32c6"', 'CONFIG_IDF_TARGET="esp32c3"'),
            True,
        ),
    ]


def _selftest_cases_dev_board() -> list[tuple[str, str, str, bool]]:
    """Return the crafted cases for the dev-board preset.

    Its two drift shapes are distinct and only one of them is obvious. A
    DIFFERENT board silently overrides every SPI pin leaf, so the build still
    succeeds and the C6 comes up on the preset's pins; NO board at all leaves
    the choice to whatever upstream happens to default to. Asserting merely
    that the symbol is present would miss the first, which is the worse one.

    Returns:
        One case per drift shape. There is no agreeing control here: the
        agreeing pair in ``_selftest_cases_kconfig`` already carries a matching
        ``C6_DEV_BOARD`` / ``CONFIG_ESP_HOST_DEV_BOARD_NONE`` pair, so this
        rule's quiet direction is proven there rather than restated.
    """
    return [
        (
            "dev board drifted to another preset",
            _GOOD_PINS,
            _GOOD_SDK.replace(
                "CONFIG_ESP_HOST_DEV_BOARD_NONE=y",
                "CONFIG_ESP_HOST_DEV_BOARD_ESP32_C6_DEVKITC=y",
            ),
            True,
        ),
        (
            "dev board selection turned off",
            _GOOD_PINS,
            _GOOD_SDK.replace(
                "CONFIG_ESP_HOST_DEV_BOARD_NONE=y", "CONFIG_ESP_HOST_DEV_BOARD_NONE=n"
            ),
            True,
        ),
        (
            "sdkconfig selects no dev board at all",
            _GOOD_PINS,
            _GOOD_SDK.replace("CONFIG_ESP_HOST_DEV_BOARD_NONE=y\n", ""),
            True,
        ),
        (
            "pins.env stopped declaring the dev board",
            _GOOD_PINS.replace("C6_DEV_BOARD=none\n", ""),
            _GOOD_SDK,
            True,
        ),
    ]


def _selftest_cases_ra8_signals() -> list[tuple[str, str, str, bool]]:
    """Return the crafted cases for one signal's RA8-side entry.

    Returns:
        One case per way a single signal can be recorded wrongly: named at
        one end only, disconnected on one side but not the other, or given a
        pin or hole name that is not one.
    """
    return [
        (
            "RA8 landing pin never recorded",
            _GOOD_PINS.replace("RA8_PIN_HANDSHAKE=P006\n", ""),
            _GOOD_SDK,
            True,
        ),
        (
            "RA8 J26 hole never recorded",
            _GOOD_PINS.replace("RA8_J26_SCK=J26-4\n", ""),
            _GOOD_SDK,
            True,
        ),
        (
            "wired on the C6 side, 'none' on the RA8 side",
            _GOOD_PINS.replace("RA8_PIN_DATA_READY=P402", "RA8_PIN_DATA_READY=none").replace(
                "RA8_J26_DATA_READY=J26-8", "RA8_J26_DATA_READY=none"
            ),
            _GOOD_SDK,
            True,
        ),
        (
            "RESET given an RA8 pin while the C6 side stays -1",
            _GOOD_PINS.replace("RA8_PIN_RESET=none", "RA8_PIN_RESET=P412").replace(
                "RA8_J26_RESET=none", "RA8_J26_RESET=J26-9"
            ),
            _GOOD_SDK,
            True,
        ),
        (
            "RA8 pin name malformed",
            _GOOD_PINS.replace("RA8_PIN_CS=P804", "RA8_PIN_CS=P80_4"),
            _GOOD_SDK,
            True,
        ),
        (
            "J26 hole malformed",
            _GOOD_PINS.replace("RA8_J26_CS=J26-1", "RA8_J26_CS=pin1"),
            _GOOD_SDK,
            True,
        ),
    ]


def _selftest_cases_ra8_map() -> list[tuple[str, str, str, bool]]:
    """Return the crafted cases for whole-map RA8-side defects.

    Returns:
        One case per defect that only shows up across signals -- two of them
        claiming one pin or one hole after a copy-paste -- plus the SW4 bank
        the link depends on.
    """
    return [
        (
            "two signals claim one RA8 pin",
            _GOOD_PINS.replace("RA8_PIN_COPI=P801", "RA8_PIN_COPI=P804"),
            _GOOD_SDK,
            True,
        ),
        (
            "two signals claim one J26 hole",
            _GOOD_PINS.replace("RA8_J26_CIPO=J26-3", "RA8_J26_CIPO=J26-2"),
            _GOOD_SDK,
            True,
        ),
        (
            "SW4 position never recorded",
            _GOOD_PINS.replace("RA8_SW4_3=ON\n", ""),
            _GOOD_SDK,
            True,
        ),
        (
            "SW4 position is not ON or OFF",
            _GOOD_PINS.replace("RA8_SW4_4=OFF", "RA8_SW4_4=maybe"),
            _GOOD_SDK,
            True,
        ),
    ]


_GOOD_PORT_HEADER = """
  k_ra8_esp_hosted_pin_chip_select = (uint16_t)k_ra8_board_pmod1_spi_cs,
  k_ra8_esp_hosted_pin_copi = (uint16_t)k_ra8_board_pmod1_spi_copi,
  k_ra8_esp_hosted_pin_cipo = (uint16_t)k_ra8_board_pmod1_spi_cipo,
  k_ra8_esp_hosted_pin_sck = (uint16_t)k_ra8_board_pmod1_spi_sck,
  k_ra8_esp_hosted_pin_handshake = (uint16_t)k_ra8_board_pmod1_irq,
  k_ra8_esp_hosted_pin_data_ready = (uint16_t)k_ra8_board_pmod1_reset,
  k_ra8_esp_hosted_pin_reset = (uint16_t)k_ra8_pin_none,
"""
"""A port pin map that agrees with ``_GOOD_PINS``."""


def _selftest_cases_port_header() -> list[tuple[str, str, str, bool]]:
    """Return crafted cases for the port-header half of the comparator.

    The port header is a second statement of the RA8-side map, so the cases
    here are the ways two copies drift: a swapped pair (the drift that really
    happened), a signal the header does not define, and a board symbol the
    gate cannot resolve.

    Returns:
        ``(label, header_text, expect_findings)`` triples, widened to the
        four-tuple shape the shared driver consumes.
    """
    swapped = _GOOD_PORT_HEADER.replace(
        "k_ra8_esp_hosted_pin_handshake = (uint16_t)k_ra8_board_pmod1_irq",
        "k_ra8_esp_hosted_pin_handshake = (uint16_t)k_ra8_board_pmod1_reset",
    )
    return [
        ("port header agrees", _GOOD_PORT_HEADER, "", False),
        ("port header swaps HANDSHAKE onto the DATA_READY pin", swapped, "", True),
        (
            "port header omits a signal",
            _GOOD_PORT_HEADER.replace(
                "  k_ra8_esp_hosted_pin_sck = (uint16_t)k_ra8_board_pmod1_spi_sck,\n", ""
            ),
            "",
            True,
        ),
        (
            "port header names an unresolvable board symbol",
            _GOOD_PORT_HEADER.replace("k_ra8_board_pmod1_spi_cs", "k_ra8_board_pmod9_spi_cs"),
            "",
            True,
        ),
    ]


_GOOD_FW_VER_HEADER = """
#define ESP_HOSTED_VERSION_MAJOR_1 2
#define ESP_HOSTED_VERSION_MINOR_1 12
#define ESP_HOSTED_VERSION_PATCH_1 11
"""
"""A vendored host firmware-version header stating 2.12.11."""


def _selftest_cases_fw_version() -> list[tuple[str, str, bool]]:
    """Return crafted cases for the host / co-processor version lock.

    The ways two statements of one version drift: a vendor bump with no
    co-processor reflash, a co-processor reflash with no vendor bump, a
    pins.env that stopped declaring the version at all, and a vendored header
    whose macros were renamed out from under the parser.

    Returns:
        ``(label, header_text, expect_findings)`` triples. The pins.env side is
        held at 2.12.11 except where the label says otherwise.
    """
    bumped = _GOOD_FW_VER_HEADER.replace("MINOR_1 12", "MINOR_1 13")
    renamed = _GOOD_FW_VER_HEADER.replace("ESP_HOSTED_VERSION_PATCH_1", "ESP_HOSTED_VER_PATCH")
    return [
        ("firmware version agrees", _GOOD_FW_VER_HEADER, False),
        ("vendored host bumped without a co-processor reflash", bumped, True),
        ("vendored header macros renamed", renamed, True),
    ]


def _selftest_fw_version_failures() -> list[str]:
    """Return one message per version-lock selftest case that misbehaved."""
    # Built here rather than by widening the shared _GOOD_PINS fixture, which
    # exists to exercise the PIN comparator: a version key bolted onto it would
    # couple two independent halves of this gate for no benefit.
    good = parse_assignments(_GOOD_PINS)
    good[FW_VERSION_KEY] = "2.12.11"
    out: list[str] = []
    for label, header, expect in _selftest_cases_fw_version():
        findings = check_fw_version_lock(good, header)
        if bool(findings) != expect:
            verb = "reported nothing" if expect else f"reported {findings}"
            out.append(f"  {label}: {verb}")
    # The other direction: the co-processor reflashed past the vendored host.
    stale = dict(good)
    stale[FW_VERSION_KEY] = "2.13.0"
    if not check_fw_version_lock(stale, _GOOD_FW_VER_HEADER):
        out.append("  co-processor reflashed past the vendored host: reported nothing")
    missing = {k: v for k, v in good.items() if k != FW_VERSION_KEY}
    if not check_fw_version_lock(missing, _GOOD_FW_VER_HEADER):
        out.append(f"  pins.env missing {FW_VERSION_KEY}: reported nothing")
    return out


def _selftest_cases() -> list[tuple[str, str, str, bool]]:
    """Return every crafted case, across both halves of the comparator.

    Returns:
        The concatenation of the four case groups.
    """
    return (
        _selftest_cases_kconfig()
        + _selftest_cases_dev_board()
        + _selftest_cases_ra8_signals()
        + _selftest_cases_ra8_map()
    )


def _selftest_port_failures() -> list[str]:
    """Return one message per port-header selftest case that misbehaved."""
    good = parse_assignments(_GOOD_PINS)
    out: list[str] = []
    for label, header, _unused, expect in _selftest_cases_port_header():
        findings = check_port_header(good, header)
        if bool(findings) != expect:
            verb = "reported nothing" if expect else f"reported {findings}"
            out.append(f"  {label}: {verb}")
    return out


def selftest() -> int:
    """Prove the comparator reports drift and stays silent on agreement.

    Returns:
        EXIT_OK when every crafted case yields the expected verdict, else
        EXIT_FAIL.
    """
    failures: list[str] = []
    for label, pins_text, sdk_text, expect in _selftest_cases():
        findings = compare(parse_assignments(pins_text), parse_assignments(sdk_text))
        if bool(findings) != expect:
            verb = "reported nothing" if expect else f"reported {findings}"
            failures.append(f"  {label}: {verb}")
    failures.extend(_selftest_port_failures())
    failures.extend(_selftest_fw_version_failures())

    if failures:
        sys.stderr.write("check_c6_pin_config.py --selftest: FAILED\n\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("\nThe comparator does not detect drift as claimed.\n")
        return EXIT_FAIL

    cases = _selftest_cases() + _selftest_cases_port_header()
    cases += [(a, b, "", c) for a, b, c in _selftest_cases_fw_version()]
    fires = sum(1 for c in cases if c[3])
    print(
        f"check_c6_pin_config.py --selftest: OK "
        f"({len(cases)} cases: {fires} must fire, {len(cases) - fires} must stay quiet)."
    )
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Compare the committed pin map against the committed Kconfig defaults.

    Returns:
        EXIT_OK when they agree, EXIT_FAIL on drift or a failing selftest,
        EXIT_CONFIG when either file is missing.
    """
    if "--selftest" in argv[1:]:
        return selftest()

    for path in (PINS_ENV, SDKCONFIG, HOST_FW_VER):
        if not path.is_file():
            sys.stderr.write(f"check_c6_pin_config.py: FATAL -- missing {path}\n")
            return EXIT_CONFIG

    pins = parse_assignments(PINS_ENV.read_text(encoding="utf-8"))
    sdk = parse_assignments(SDKCONFIG.read_text(encoding="utf-8"))
    findings = compare(pins, sdk)
    findings.extend(check_fw_version_lock(pins, HOST_FW_VER.read_text(encoding="utf-8")))

    if not findings:
        # The three settings compared outside the two PAIR tables: the flash
        # size, the dev-board preset (both encoded in a symbol NAME) and the
        # host / co-processor firmware version lock.
        key_encoded_and_version = 3
        checked = len(PIN_PAIRS) + len(VALUE_PAIRS) + key_encoded_and_version
        ra8 = (len(RA8_TRIPLES) * 2) + len(SW4_KEYS)
        print(
            f"check_c6_pin_config.py: pins.env and sdkconfig.defaults agree "
            f"({checked} settings); dev board "
            f"{pins.get(DEV_BOARD_KEY, '?')} (no preset overriding our pins); "
            f"RA8-side map well-formed ({ra8} entries); "
            f"co-processor firmware {pins.get(FW_VERSION_KEY, '?')} matches the "
            f"vendored host driver."
        )
        return EXIT_OK

    sys.stderr.write(f"check_c6_pin_config.py: {len(findings)} C6 config drift(s):\n")
    for message in findings:
        sys.stderr.write(f"  {message}\n")
    sys.stderr.write(
        "\ncoprocessor/esp32c6/pins.env is the source of truth. Update\n"
        "sdkconfig.defaults to match it (and reflash the C6 -- a stale image on\n"
        "the board still carries the old pins).\n"
    )
    return EXIT_FAIL


if __name__ == "__main__":
    sys.exit(main(sys.argv))
