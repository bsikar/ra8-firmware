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
target, and the flash size. The flash size is the interesting one: esp-idf
encodes it in the KEY (``CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y``) rather than the
value, so it is parsed out of the key name and compared against
``C6_FLASH_SIZE``.

Two of the data signals carry Espressif's legacy names in their Kconfig symbols.
Those symbols belong to upstream and cannot be renamed from here; the mapping
table is where they are tied to this project's COPI/CIPO vocabulary.

This is a pure text comparison of two committed files -- no esp-idf, no
toolchain, no hardware -- so it runs in CI on any box. ``build.sh`` invokes
this same script before it builds, so the bench and CI apply one rule.

Non-vacuity
-----------
``--selftest`` drives the comparator with crafted file bodies: an agreeing
pair must be silent, and a disagreeing pair, a missing key on either side, and
a missing flash-size key must each be reported.

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

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_CONFIG = 2

# (Kconfig symbol, pins.env key, human label for the failure text).
# The two data-signal symbols keep Espressif's legacy spelling because they are
# upstream esp-hosted-mcu Kconfig names and cannot be renamed from this
# repository; our own signal names are COPI/CIPO, which is what the labels use.
PIN_PAIRS: tuple[tuple[str, str, str], ...] = (
    ("CONFIG_ESP_SPI_HSPI_GPIO_CS", "C6_PIN_CS", "CS (Chip Select)"),
    # LEGACY-OK: CONFIG_ESP_SPI_HSPI_GPIO_MOSI is an upstream Kconfig symbol; our signal is COPI
    ("CONFIG_ESP_SPI_HSPI_GPIO_MOSI", "C6_PIN_COPI", "COPI (Controller Out)"),
    # LEGACY-OK: CONFIG_ESP_SPI_HSPI_GPIO_MISO is an upstream Kconfig symbol; our signal is CIPO
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
C6_FLASH_SIZE=16MB
ESP_TARGET=esp32c6
"""

_GOOD_SDK = """
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_SPI_HSPI_GPIO_CS=0
CONFIG_ESP_SPI_HSPI_GPIO_MOSI=1
CONFIG_ESP_SPI_HSPI_GPIO_MISO=2
CONFIG_ESP_SPI_HSPI_GPIO_CLK=3
CONFIG_ESP_SPI_GPIO_DATA_READY=4
CONFIG_ESP_SPI_GPIO_HANDSHAKE=6
CONFIG_ESP_SPI_GPIO_RESET=-1
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
"""


def _selftest_cases() -> list[tuple[str, str, str, bool]]:
    """Return ``(label, pins_text, sdk_text, expect_finding)`` cases.

    Returns:
        One case per defect shape, plus the agreeing control.
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

    if failures:
        sys.stderr.write("check_c6_pin_config.py --selftest: FAILED\n\n")
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("\nThe comparator does not detect drift as claimed.\n")
        return EXIT_FAIL

    cases = _selftest_cases()
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

    for path in (PINS_ENV, SDKCONFIG):
        if not path.is_file():
            sys.stderr.write(f"check_c6_pin_config.py: FATAL -- missing {path}\n")
            return EXIT_CONFIG

    pins = parse_assignments(PINS_ENV.read_text(encoding="utf-8"))
    sdk = parse_assignments(SDKCONFIG.read_text(encoding="utf-8"))
    findings = compare(pins, sdk)

    if not findings:
        checked = len(PIN_PAIRS) + len(VALUE_PAIRS) + 1
        print(
            f"check_c6_pin_config.py: pins.env and sdkconfig.defaults agree ({checked} settings)."
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
