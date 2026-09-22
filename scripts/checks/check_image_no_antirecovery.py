#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Pre-flash IMAGE guard: refuse a firmware image that would brick recovery.

Owner policy (2026-07-23): a bad image must never be able to permanently
disable device recovery on the RA8D2 -- not even one that came from outside
this tree. This scanner inspects the ACTUAL bytes of the ``.hex`` / ``.elf``
about to be programmed and REFUSES it if it writes a lockdown value into the
anti-recovery option-setting / security / OTP region.

What it refuses
---------------
A *programmed* (non-erased) value landing in the permanent security / OTP
sub-region of the option-setting memory. On the RA8D2 the option-setting
memory (HUM Ch 7 "Option-Setting Memory") is erased to all-ones; a real
application leaves every one of these words at ``0xFFFFFFFF``. A byte that is
NOT ``0xFF`` in one of these windows means the image is trying to *set* it:

  * First-Stage Boot Loader control (FSBLCTRL) -- boot lock.
  * MRAM/secure attribution + access control (SAMR / SACC).
  * PERMANENT block protect (PBPS / PBPS_SEC) -- irreversible.
  * HUK zeroize (ZHUK) -- destroys the device-unique key.
  * The extra-MRAM OTP window (FSBL / code cert / PBPS / POFSPS / REVOKE /
    HUK-zeroize / anti-rollback, HUM Ch 59.7.4.5 Table 59.15) and the
    anti-rollback counters (ARCCS / ARC_SEC / ARC_NSEC).

None of the project's firmware programs any of these -- verified: every app's
``.option_setting_otp_*`` word is ``0xFFFFFFFF`` and no example overrides the
default. So any non-erased value there is, by construction, not something this
project produces, and the safe answer is to refuse (this is the conservative
policy the owner asked for: the firmware never touches the lockdown region, so
ANY programmed data there is rejected).

What it ALLOWS (must not be flagged)
------------------------------------
  * Normal code / rodata / vectors / data anywhere in MRAM, SRAM, or external
    memory.
  * The benign option bytes every app legitimately sets, AT ANY VALUE: OFS0 /
    OFS1 / OFS2 / OFS3 (watchdog, LVD, HOCO, extended clocks), SAS + the
    OFS*_SEC / OFS*_SEL TrustZone attribution selectors, and BPS / BPS_SEC
    block-protect for normal use. (e.g. ``bkup_survival_demo`` legitimately
    programs ``OFS1 = 0xFFFFFFF0`` to enable LVD0 -- that is allowed.)
  * Any word in the anti-recovery region that is still erased (``0xFFFFFFFF``)
    -- a real image carries those sections erased.

The addresses use the flash-programming view of the option-setting memory
(``0x0300A000`` block) -- the addresses that actually appear in a linked
``.hex`` / ``.elf`` (confirmed against a built ``uart_hello.elf``) -- plus the
extra-MRAM / config-set / anti-rollback windows a lockdown could otherwise
target.

Run::

    check_image_no_antirecovery.py IMAGE.hex        # or IMAGE.elf
    check_image_no_antirecovery.py --selftest       # prove both directions

Exit 0 when the image is safe to flash, 1 when it must be refused (or on a
bad/unreadable image, or a failing selftest).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The erased state of option-setting / OTP flash: all-ones. A byte that is not
# this is a programmed bit.
ERASED = 0xFF

# Intel HEX record layout + record types (Intel HEX specification).
IHEX_MIN_RECORD_LEN = 5  # length + addr(2) + type + checksum
IHEX_DATA = 0x00
IHEX_EOF = 0x01
IHEX_EXT_SEG_ADDR = 0x02
IHEX_START_SEG_ADDR = 0x03
IHEX_EXT_LIN_ADDR = 0x04
IHEX_START_LIN_ADDR = 0x05

# Anti-recovery windows: (name, start, end_inclusive, hum_cite). A non-erased
# byte anywhere in one of these is a lockdown attempt and the image is refused.
#
# These are the permanent / security / OTP structures. The benign option words
# (OFS0-3 at 0x0300A100-0x0300A113, SAS, the OFS*_SEC/SEL selectors at
# 0x0300A200-0x0300A213, and BPS/BPS_SEC at 0x0300A300/0x0300A400) are
# deliberately NOT here -- apps set those legitimately.
ANTI_RECOVERY_WINDOWS: tuple[tuple[str, int, int, str], ...] = (
    (
        "OTP security/permanent block (FSBLCTRL/SAMR/SACC/PBPS/ZHUK)",
        0x0300A500,
        0x0300A9FF,
        "HUM Ch 7 Option-Setting Memory p 278-299",
    ),
    (
        "extra-MRAM OTP window (FSBL/cert/PBPS/POFSPS/REVOKE/HUK-zeroize)",
        0x02E07600,
        0x02E179FF,
        "HUM Ch 59.7.4.5 Table 59.15 p 3592",
    ),
    (
        "OFS config-set (MACI) window",
        0x02C9F000,
        0x02C9FFFF,
        "HUM Ch 7 Option-Setting Memory p 278",
    ),
    (
        "anti-rollback counters (ARCCS/ARC_SEC/ARC_NSEC)",
        0x02F27E00,
        0x02F27E0F,
        "HUM Ch 7.2.21-7.2.23 p 296-297",
    ),
)

MAX_FINDINGS_SHOWN = 40


def _objcopy() -> str:
    """Return the arm-none-eabi-objcopy path, or fail loudly if it is absent.

    An image guard that silently skipped the ELF because a tool was missing
    would be a guard that passes a brick image -- so this fails rather than
    degrading to a no-op (CLAUDE.md: gates fail loudly on a missing tool).
    """
    for cand in (
        os.environ.get("RA8_OBJCOPY", ""),
        "arm-none-eabi-objcopy",
        "objcopy",
    ):
        resolved = shutil.which(cand) if cand else None
        if resolved is not None:
            return resolved
    sys.exit(
        "check_image_no_antirecovery.py: FATAL -- no objcopy found to read an ELF.\n"
        "  Install arm-none-eabi-objcopy or set RA8_OBJCOPY=/path/to/objcopy.\n"
        "  Refusing to skip the ELF: a skipped image check is a brick waiting to happen."
    )


def parse_ihex(text: str) -> dict[int, int]:
    """Parse Intel HEX text into an ``{address: byte}`` map.

    Handles record types 00 (data), 01 (EOF), 02 (extended segment), 04
    (extended linear address) and 05 (start linear). The extended-address
    records are essential: a linked RA8D2 image sits at 0x0200_0000 and
    0x0300_A000, so without type-04 handling the option bytes would be read at
    the wrong address and silently missed.
    """
    mem: dict[int, int] = {}
    base = 0
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or not line.startswith(":"):
            continue
        try:
            rec = bytes.fromhex(line[1:])
        except ValueError:
            sys.exit(f"check_image_no_antirecovery.py: FATAL -- malformed HEX record: {line!r}")
        if len(rec) < IHEX_MIN_RECORD_LEN or (sum(rec) & 0xFF) != 0:
            sys.exit(f"check_image_no_antirecovery.py: FATAL -- bad HEX record: {line!r}")
        length, addr_hi, addr_lo, rectype = rec[0], rec[1], rec[2], rec[3]
        data = rec[4 : 4 + length]
        if rectype == IHEX_DATA:
            start = base + ((addr_hi << 8) | addr_lo)
            for i, byte in enumerate(data):
                mem[start + i] = byte
        elif rectype == IHEX_EXT_LIN_ADDR:
            base = ((data[0] << 8) | data[1]) << 16
        elif rectype == IHEX_EXT_SEG_ADDR:
            base = ((data[0] << 8) | data[1]) << 4
        elif rectype in (IHEX_EOF, IHEX_START_SEG_ADDR, IHEX_START_LIN_ADDR):
            continue
    return mem


def load_image(path: Path) -> dict[int, int]:
    """Load a ``.hex`` or ``.elf`` firmware image into an ``{address: byte}`` map.

    ``.hex`` is parsed directly. ``.elf`` is converted to Intel HEX with
    objcopy first, so load (LMA) addresses -- what actually gets programmed --
    are used, not the VMA.
    """
    suffix = path.suffix.lower()
    if suffix in (".hex", ".ihex", ".mot", ""):
        return parse_ihex(path.read_text(encoding="latin-1"))
    if suffix in (".elf", ".axf", ".out"):
        objcopy = _objcopy()
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "image.hex"
            proc = subprocess.run(  # noqa: S603  # fixed argv, resolved tool
                [objcopy, "-O", "ihex", str(path), str(out)],
                capture_output=True,
                text=True,
                check=False,
            )
            if proc.returncode != 0:
                sys.exit(
                    f"check_image_no_antirecovery.py: FATAL -- objcopy failed on {path}:\n"
                    f"{proc.stderr}"
                )
            return parse_ihex(out.read_text(encoding="latin-1"))
    # Unknown extension: try Intel HEX, which is the common flashing format.
    return parse_ihex(path.read_text(encoding="latin-1"))


def scan_memory(mem: dict[int, int]) -> list[dict]:
    """Return every non-erased byte that lands in an anti-recovery window."""
    findings: list[dict] = []
    for addr, byte in sorted(mem.items()):
        if byte == ERASED:
            continue
        for name, start, end, cite in ANTI_RECOVERY_WINDOWS:
            if start <= addr <= end:
                findings.append(
                    {"addr": addr, "byte": byte, "window": name, "start": start, "cite": cite}
                )
                break
    return findings


def _report(image: str, findings: list[dict]) -> None:
    """Explain which lockdown writes were found and why the flash is refused."""
    print(
        f"check_image_no_antirecovery.py: REFUSING to flash {image}\n"
        f"  {len(findings)} byte(s) program a lockdown value into the anti-recovery\n"
        f"  option-setting / security / OTP region:\n",
        file=sys.stderr,
    )
    for f in findings[:MAX_FINDINGS_SHOWN]:
        print(
            f"    0x{f['addr']:08X} = 0x{f['byte']:02X}  [{f['window']}]  ({f['cite']})",
            file=sys.stderr,
        )
    if len(findings) > MAX_FINDINGS_SHOWN:
        print(f"    ... {len(findings) - MAX_FINDINGS_SHOWN} more (truncated)", file=sys.stderr)
    print(
        "\nOwner policy (2026-07-23): this project must NEVER permanently disable\n"
        "device recovery. No first-party image programs this region -- every\n"
        "option-setting OTP word ships erased (0xFFFFFFFF). A non-erased value\n"
        "here is a boot-lock / permanent-block-protect / HUK-zeroize / anti-\n"
        "rollback lockdown, which the recovery scripts cannot undo.\n"
        "\nBenign option bytes (OFS0/OFS1/OFS2/OFS3, SAS, OFS*_SEC/SEL, BPS) are\n"
        "allowed at any value; only the security/OTP lockdown region is refused.\n"
        "\nIf you are DELIBERATELY provisioning a board and accept the brick risk,\n"
        "re-run with RA8_ALLOW_ANTIRECOVERY_FLASH=1 (loud, explicit override).",
        file=sys.stderr,
    )


# --- selftest ----------------------------------------------------------------


def _ihex_record(rectype: int, addr16: int, data: list[int]) -> str:
    """Build one Intel HEX record line (with checksum)."""
    body = [len(data), (addr16 >> 8) & 0xFF, addr16 & 0xFF, rectype, *data]
    checksum = (-sum(body)) & 0xFF
    return ":" + "".join(f"{b:02X}" for b in [*body, checksum])


def _ela(upper16: int) -> str:
    """Extended-linear-address record setting the upper 16 bits of the address."""
    return _ihex_record(0x04, 0, [(upper16 >> 8) & 0xFF, upper16 & 0xFF])


def _word_le(value: int) -> list[int]:
    """Little-endian 4-byte list for a 32-bit word."""
    return [value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF]


def selftest() -> int:
    """Assert the scanner refuses lockdown images and passes benign ones.

    Both directions: a synthetic image that programs the disable-initialize /
    permanent-lock OTP region is REFUSED, and one that carries only code plus
    benign (even non-erased) OFS bytes plus erased OTP words is ALLOWED.
    """
    eof = _ihex_record(0x01, 0, [])

    # BAD: programs PBPS (permanent block protect, 0x0300A800) and ZHUK
    # (HUK zeroize, 0x0300A900) with non-erased values -> must REFUSE.
    bad = "\n".join(
        [
            _ela(0x0200),
            _ihex_record(0x00, 0x0000, _word_le(0x20001000)),  # a reset vector
            _ela(0x0300),
            _ihex_record(0x00, 0xA800, _word_le(0x00000000)),  # PBPS lockdown
            _ihex_record(0x00, 0xA900, _word_le(0x00000000)),  # ZHUK lockdown
            eof,
        ]
    )
    # BAD-2: a lockdown in the extra-MRAM OTP window (0x02E07600).
    bad2 = "\n".join(
        [
            _ela(0x02E0),
            _ihex_record(0x00, 0x7600, _word_le(0x00000000)),
            eof,
        ]
    )
    # GOOD: code at 0x02000000, OFS1 programmed to 0xFFFFFFF0 (benign LVD
    # enable), and the OTP words left erased (0xFFFFFFFF) -- must ALLOW.
    good = "\n".join(
        [
            _ela(0x0200),
            _ihex_record(0x00, 0x0000, _word_le(0x20001000)),
            _ihex_record(0x00, 0x0200, [0x00, 0xBF, 0x00, 0xBF]),  # nop; nop
            _ela(0x0300),
            _ihex_record(0x00, 0xA104, _word_le(0xFFFFFFF0)),  # OFS1 (benign)
            _ihex_record(0x00, 0xA800, _word_le(0xFFFFFFFF)),  # PBPS erased
            _ihex_record(0x00, 0xA900, _word_le(0xFFFFFFFF)),  # ZHUK erased
            eof,
        ]
    )

    cases: list[tuple[str, str, bool]] = [
        ("lockdown: PBPS + ZHUK programmed", bad, True),
        ("lockdown: extra-MRAM OTP programmed", bad2, True),
        ("benign: code + OFS1=0xFFFFFFF0 + erased OTP", good, False),
    ]
    failures: list[str] = []
    for label, text, should_refuse in cases:
        refused = bool(scan_memory(parse_ihex(text)))
        if refused != should_refuse:
            verb = "did not refuse" if should_refuse else "refused"
            failures.append(f"  {label}: scanner {verb} (unexpected)")

    if failures:
        print("check_image_no_antirecovery.py --selftest: FAILED\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    refused_n = sum(1 for c in cases if c[2])
    print(
        f"check_image_no_antirecovery.py --selftest: PASS "
        f"({len(cases)} cases: {refused_n} must refuse, {len(cases) - refused_n} must allow)"
    )
    return 0


def main(argv: list[str]) -> int:
    """Scan a firmware image for anti-recovery lockdown writes, or run the selftest.

    Returns 0 when the image is safe to flash, 1 when it must be refused (a
    lockdown value in the security/OTP region), on an unreadable image, or on a
    failing selftest.
    """
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", help="path to a .hex or .elf image")
    ap.add_argument("--selftest", action="store_true", help="prove both directions")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return selftest()

    if not args.image:
        ap.error("an image path is required (or use --selftest)")
    path = Path(args.image)
    if not path.is_file():
        print(f"check_image_no_antirecovery.py: FATAL -- image not found: {path}", file=sys.stderr)
        return 1

    mem = load_image(path)
    findings = scan_memory(mem)
    if not findings:
        print(
            f"check_image_no_antirecovery.py: OK ({path} -- "
            f"{len(mem)} bytes, no anti-recovery lockdown writes)"
        )
        return 0

    _report(str(path), findings)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
