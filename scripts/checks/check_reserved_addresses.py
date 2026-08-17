#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject first-party address constants that point into a Reserved memory window.

WHY THIS EXISTS
===============
Three times now a driver has named an address that describes hardware which is
not there, compiled clean, and failed only on silicon:

  * the `ra8_rsip` crypto family -- invented registers, whole driver rewritten;
  * `ra8_ptp` -- addressed a reserved window, and its `gptp: clock PASS` was a
    reserved aperture echoing back a write (#498);
  * `ra8_wdt_regs.h` -- read OFS0/OFS3 at 0x03001E04 / 0x03001E20, which appear
    nowhere in either Hardware User's Manual and land in a Reserved area, and
    dereferenced them at runtime (#545).

Nothing catches this class. `check_hum_register_map.py` (#540) cross-checks
register SYMBOLS and struct/window OFFSETS against the manual's tables, but an
absolute-address enumerator like `k_ra8_wdt_ofs0_addr` is neither, so it falls
outside that gate's rules. `check_linker_scripts.py` rule LD007 guards the
phantom data-flash base on the LINKER side only -- the C side was unguarded,
which is exactly how #545 survived.

WHAT IT CHECKS
==============
  RA001  an enumerator in a `uintptr_t`-typed enum resolves into a window the
         HUM memory map marks Reserved.

The rule is deliberately narrow. `uintptr_t` enums are, by the CLAUDE.md
constants hierarchy, THE way this tree spells a hardware address ("Use
`uintptr_t` for any enum whose values are hardware memory-mapped addresses"),
so the scan surface is precisely the set of things that claim to be addresses.
A value that is not an address does not belong in one of these enums, and an
address that lands in a Reserved window is wrong by construction -- reading it
is undefined: it may fault, or return garbage the caller then trusts.

WHAT IT DELIBERATELY DOES NOT CHECK
===================================
It does not try to validate that a non-reserved address is CORRECT -- that
needs the manual's register tables, which is #540's job. This gate answers the
much cheaper question that #540 cannot: "is this address inside a hole?"
"""

from __future__ import annotations

import argparse
import contextlib
import io
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass

# Reserved windows of the RA8 address map. Both supported parts agree
# line-for-line: RA8D2 HUM R01UH1065EJ0130 and RA8P1 HUM R01UH1064EJ0130,
# Ch 3 "Address Space" memory map.
#
# Keep this table SMALL and primary-sourced. A window listed here must be
# reserved on every supported part, because a hit is a hard failure.
RESERVED_WINDOWS: list[tuple[int, int, str]] = [
    # Between the Extra MRAM option-setting region and the SiP-Flash area.
    # This is the window #545's invented OFS addresses landed in.
    (0x03000000, 0x07FFFFFF, "Reserved area between Extra MRAM and SiP-Flash (HUM Ch 3 map)"),
    # The same window through the non-secure alias (BASE_MC 0x1200_0000). The
    # option-setting words have a full non-secure mirror -- OFS3 is genuinely
    # read at 0x12C9_F4C4 -- so the hole above it is just as reachable by a
    # wrong constant, and omitting it would leave the alias half unguarded.
    (0x13000000, 0x17FFFFFF, "Reserved area, non-secure alias of 0x0300_0000 (HUM Ch 3 map)"),
    # The conventional RA-family data-flash base. The RA8 has no such array;
    # it faults on this silicon (#397). LD007 guards the linker side.
    (0x27000000, 0x27FFFFFF, "phantom data-flash -- the RA8 has no data-flash array (#397)"),
]

# Directories holding hand-written first-party code. `libs/third_party` is SOUP
# and exempt per CLAUDE.md; it is filtered below rather than listed here.
SCAN_ROOTS = ("libs", "examples", "port", "tools", "apps", "tests")

# Below this many `uintptr_t` enumerators the scan has plainly stopped
# matching -- a syntax change in how the tree spells address enums would
# otherwise leave this gate quietly enforcing nothing. Measured 2026-07-28:
# 151 enum blocks across 136 files, well over a thousand enumerators. The
# floor sits far under a healthy tree but far above a collapsed scan.
ENUMERATOR_FLOOR = 300

ENUM_OPEN = re.compile(r"\benum\s*:\s*uintptr_t\s*\{", re.MULTILINE)
ENUMERATOR = re.compile(r"(\bk_[A-Za-z0-9_]+)\s*=\s*(0[xX][0-9A-Fa-f_]+)")


@dataclass(frozen=True)
class Finding:
    """One RA001 hit: an address enumerator inside a Reserved window."""

    path: pathlib.Path
    line: int
    symbol: str
    value: int
    why: str

    def render(self) -> str:
        """Format the finding as a single `path:line: [RA001] ...` line."""
        return (
            f"{self.path}:{self.line}: [RA001] {self.symbol} = 0x{self.value:08X} "
            f"lands in a Reserved window -- {self.why}"
        )


def strip_comments(text: str) -> str:
    """Blank out block and line comments, preserving line structure.

    Newlines are kept so reported line numbers stay true to the source. A
    commented-out address is documentation, not a dereference, so it must not
    be flagged -- both remediated headers cite the old bad values in prose.
    """
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def enum_bodies(code: str) -> list[tuple[int, str]]:
    """Yield `(offset, body)` for every `enum : uintptr_t { ... }` block.

    Brace-matched rather than regex-terminated so a nested initialiser cannot
    truncate the body and silently drop the enumerators after it.
    """
    out: list[tuple[int, str]] = []
    for m in ENUM_OPEN.finditer(code):
        start = m.end()
        depth = 1
        i = start
        while i < len(code) and depth > 0:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
            i += 1
        out.append((start, code[start : i - 1]))
    return out


def reserved_hit(value: int) -> str | None:
    """Return the description of the Reserved window containing `value`, if any."""
    for lo, hi, why in RESERVED_WINDOWS:
        if lo <= value <= hi:
            return why
    return None


def check_text(path: pathlib.Path, text: str) -> tuple[list[Finding], int]:
    """Apply RA001 to one file; also return how many enumerators were seen.

    The enumerator count feeds the vacuity floor, and is produced by the same
    pass that applies the rule so the two cannot drift.
    """
    code = strip_comments(text)
    findings: list[Finding] = []
    seen = 0
    for offset, body in enum_bodies(code):
        for m in ENUMERATOR.finditer(body):
            seen += 1
            value = int(m.group(2).replace("_", ""), 16)
            why = reserved_hit(value)
            if why is not None:
                line = code[: offset + m.start()].count("\n") + 1
                findings.append(Finding(path, line, m.group(1), value, why))
    return findings, seen


def repo_root() -> pathlib.Path:
    """Resolve the repository root via git."""
    return pathlib.Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # git from PATH is intended
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )


def tracked_sources(root: pathlib.Path) -> list[pathlib.Path]:
    """List tracked first-party C sources and headers, excluding SOUP."""
    out = subprocess.run(  # noqa: S603  # fixed argv, no shell
        ["git", "ls-files", "--", *(f"{d}/**/*.[ch]" for d in SCAN_ROOTS)],  # noqa: S607
        capture_output=True,
        text=True,
        check=True,
        cwd=root,
    ).stdout.split()
    return [root / p for p in out if not p.startswith("libs/third_party/")]


def scan(paths: list[pathlib.Path]) -> tuple[list[Finding], int]:
    """Run RA001 over every path, returning findings and the enumerator count."""
    findings: list[Finding] = []
    total = 0
    for p in paths:
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        f, seen = check_text(p, text)
        findings.extend(f)
        total += seen
    return findings, total


def floor_breached(seen: int) -> bool:
    """Report and fail when the scan matched too few enumerators to mean anything."""
    if seen >= ENUMERATOR_FLOOR:
        return False
    print(
        f"ERROR: matched only {seen} uintptr_t enumerator(s), below the floor of "
        f"{ENUMERATOR_FLOOR}. Either the tree stopped spelling addresses as "
        f"`enum : uintptr_t` (RA001 now enforces nothing) or the scan scope "
        f"collapsed. Refusing to report success.",
        file=sys.stderr,
    )
    return True


# Values the selftest fixtures below encode, named so the assertions do not
# repeat bare literals.
_FIXTURE_BAD_OFS0 = 0x03001E04
_FIXTURE_BAD_DATAFLASH = 0x27000000
_FIXTURE_QUIET_ENUMERATORS = 3

_MUST_FIRE = """
typedef enum : uintptr_t {
  k_ra8_bad_addr = 0x03001E04UL,
} bad_t;
"""

_MUST_FIRE_DATAFLASH = """
typedef enum : uintptr_t {
  k_ra8_df_addr = 0x27000000UL,
} df_t;
"""

_MUST_STAY_QUIET = """
/* A commented-out 0x03001E04 is prose, not a dereference. */
typedef enum : uintptr_t {
  k_ra8_ofs0_addr = 0x02C9F040UL,
  k_ra8_ofs3_addr = 0x12C9F4C4UL,
  k_ra8_wdt_base_addr = 0x40202600UL,
} good_t;

/* Not a uintptr_t enum: a mask that happens to look like a reserved address. */
typedef enum : uint32_t {
  k_ra8_mask_thing = 0x03001E04UL,
} mask_t;
"""


def selftest() -> int:
    """Assert RA001 fires on a reserved address and stays quiet on a real one.

    Both directions, driving `check_text` -- the same entry point `scan` uses.
    A checker that only proves it can fire is indistinguishable from one whose
    scope has collapsed to nothing.
    """
    rc = 0
    fake = pathlib.Path("selftest.h")

    hits, seen = check_text(fake, _MUST_FIRE)
    if len(hits) != 1 or hits[0].value != _FIXTURE_BAD_OFS0:
        print("SELFTEST FAIL: RA001 did not fire on the #545 reserved address", file=sys.stderr)
        rc = 1
    if seen != 1:
        print(f"SELFTEST FAIL: expected 1 enumerator, counted {seen}", file=sys.stderr)
        rc = 1

    hits, _ = check_text(fake, _MUST_FIRE_DATAFLASH)
    if len(hits) != 1 or hits[0].value != _FIXTURE_BAD_DATAFLASH:
        print("SELFTEST FAIL: RA001 did not fire on phantom data-flash", file=sys.stderr)
        rc = 1

    hits, seen = check_text(fake, _MUST_STAY_QUIET)
    if hits:
        print(
            f"SELFTEST FAIL: RA001 over-fired on legitimate addresses: "
            f"{[h.render() for h in hits]}",
            file=sys.stderr,
        )
        rc = 1
    if seen != _FIXTURE_QUIET_ENUMERATORS:
        print(
            f"SELFTEST FAIL: expected {_FIXTURE_QUIET_ENUMERATORS} uintptr_t "
            f"enumerators in the quiet fixture, counted {seen} -- the uint32_t "
            f"enum must not be scanned",
            file=sys.stderr,
        )
        rc = 1

    # The floor must itself be able to fail, or it is decoration. Its diagnostic
    # is swallowed here so a PASSING selftest never prints a line starting
    # "ERROR:" -- a gate whose success output reads like a failure costs the
    # next reader real time.
    sink = io.StringIO()
    with contextlib.redirect_stderr(sink):
        floor_bites = floor_breached(0)
    if not floor_bites:
        print("SELFTEST FAIL: vacuity floor accepted an empty scan", file=sys.stderr)
        rc = 1

    if rc == 0:
        print("check_reserved_addresses selftest: OK (fires, stays quiet, floor bites)")
    return rc


def main() -> int:
    """Scan first-party C for address enumerators inside Reserved windows."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    ap.add_argument("--list-files", action="store_true", help="print the scanned file list")
    ap.add_argument("paths", nargs="*", help="files to check (default: all tracked)")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    root = repo_root()
    paths = [pathlib.Path(p) for p in args.paths] if args.paths else tracked_sources(root)

    if args.list_files:
        for p in paths:
            print(p)
        return 0

    findings, seen = scan(paths)
    for f in findings:
        print(f.render(), file=sys.stderr)

    # Only floor a whole-tree run; a positional subset legitimately sees few.
    if not args.paths and floor_breached(seen):
        return 1

    if findings:
        print(
            f"\n{len(findings)} address constant(s) point into a Reserved window. "
            f"An address the manual does not define is not a citation problem -- "
            f"reading it is undefined behaviour on silicon.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
