#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a board's published memory map must match its linker script.

A board's `MEMORY{}` block is the authoritative statement of where everything
is, and it is readable by the linker and by nothing else. Every host-side
consumer that needs the same numbers has therefore retyped them: the emulator's
region table restates them as C literals, the emulator's own header restates
three of the bases a second time, and `tests/mocks/ra8_fake_mmap.c` declares a
third copy under a third set of names (#758). Four spellings, no pin between
them, and correcting one corrected nothing else.

`libs/ra8_board_<board>/inc/ra8_board_memmap.h` is the board layer's answer: the
same regions as ordinary C constants a host tool or a firmware translation unit
can include. This gate is what stops the answer becoming a second VERSION of the
map rather than a second spelling of it. For every board package that has both a
`MEMORY{}` block and a `ra8_board_memmap.h`, it reports:

`missing-region`
    The linker script declares a region the header does not publish. A consumer
    that reads the header would not know the window exists.

`stale-region`
    The header publishes a region the linker script no longer declares, so the
    constant names a window nothing places into any more.

`origin-drift` / `length-drift`
    Both declare the region and they disagree. This is the failure the gate
    exists for: the drift is silent today because no build compares them.

Scope is DERIVED, never a hardcoded list (#358): every `libs/ra8_board_*`
package is scanned, so a board added tomorrow is covered the day it lands. A
board with a linker script and no header yet is skipped and named in the report,
because publishing the descriptor is per-board work that can land separately.

Usage:
    python3 scripts/checks/check_board_memory_map.py [--root DIR]
    python3 scripts/checks/check_board_memory_map.py --selftest
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

K_SUFFIX_SCALE = {"K": 1024, "M": 1024 * 1024, "G": 1024 * 1024 * 1024}
K_CONST_PREFIX = "k_ra8_board_"
K_BOARD_GLOB = "libs/ra8_board_*"
K_LINKER_RELATIVE = Path("ld") / "linker_script.ld"
K_HEADER_RELATIVE = Path("inc") / "ra8_board_memmap.h"

# `MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K - 256`
K_REGION_RE = re.compile(
    r"^\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*"
    r"\((?P<attrs>[^)]*)\)\s*:\s*"
    r"ORIGIN\s*=\s*(?P<origin>[^,]+),\s*"
    r"LENGTH\s*=\s*(?P<length>[^\n/]+)",
    re.MULTILINE,
)
K_ENUM_RE = re.compile(
    r"^\s*(?P<name>k_ra8_board_[a-z0-9_]+)\s*=\s*(?P<value>0[xX][0-9a-fA-F]+|\d+)[uUlL]*\s*,",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Region:
    """One `MEMORY{}` entry or one published base/size pair."""

    name: str
    origin: int
    length: int


@dataclass(frozen=True)
class Finding:
    """One rule violation, reported against the board package."""

    board: str
    rule: str
    detail: str

    def render(self) -> str:
        return f"{self.board}: {self.rule}: {self.detail}"


def strip_comments(text: str) -> str:
    """Remove `/* ... */` comments so a commented-out region is not parsed."""
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)


def eval_ld_expr(expr: str) -> int:
    """Evaluate a linker-script size expression: `1024K - 256`, `64M`, `0x100`.

    Only the forms the tree actually uses are supported -- a sum of terms, each
    an integer with an optional K/M/G suffix. Anything else raises, because a
    gate that silently guesses at an expression it does not understand is worse
    than one that says so.
    """
    total = 0
    sign = 1
    for token in re.findall(r"[+-]|[^\s+-]+", expr.strip()):
        if token == "+":
            sign = 1
            continue
        if token == "-":
            sign = -1
            continue
        match = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)([KMG]?)", token)
        if match is None:
            raise ValueError(f"unsupported linker expression term: {token!r}")
        value = int(match.group(1), 0) * K_SUFFIX_SCALE.get(match.group(2), 1)
        total += sign * value
        sign = 1
    return total


def parse_linker_regions(text: str) -> dict[str, Region]:
    """Parse the `MEMORY{}` block of a linker script into regions by name."""
    body = strip_comments(text)
    start = body.find("MEMORY")
    if start < 0:
        return {}
    open_brace = body.find("{", start)
    if open_brace < 0:
        return {}
    close_brace = body.find("}", open_brace)
    block = body[open_brace + 1 : close_brace if close_brace > 0 else len(body)]
    regions: dict[str, Region] = {}
    for match in K_REGION_RE.finditer(block):
        name = match.group("name").lower()
        regions[name] = Region(
            name=name,
            origin=eval_ld_expr(match.group("origin")),
            length=eval_ld_expr(match.group("length")),
        )
    return regions


def parse_header_regions(text: str) -> tuple[dict[str, Region], list[str]]:
    """Parse `k_ra8_board_<region>_base` / `_size` pairs out of the header.

    Returns the regions plus the names of any constant whose partner is
    missing, so a half-published region is reported rather than dropped.
    """
    bases: dict[str, int] = {}
    sizes: dict[str, int] = {}
    for match in K_ENUM_RE.finditer(strip_comments(text)):
        name = match.group("name")[len(K_CONST_PREFIX) :]
        value = int(match.group("value"), 0)
        if name.endswith("_base"):
            bases[name[: -len("_base")]] = value
        elif name.endswith("_size"):
            sizes[name[: -len("_size")]] = value
    regions = {
        name: Region(name=name, origin=bases[name], length=sizes[name])
        for name in sorted(set(bases) & set(sizes))
    }
    unpaired = sorted(set(bases) ^ set(sizes))
    return regions, unpaired


def compare(board: str, linker: dict[str, Region], header: dict[str, Region],
            unpaired: list[str]) -> list[Finding]:
    """Apply the four rules to one board package."""
    findings: list[Finding] = []
    for name in unpaired:
        findings.append(Finding(board, "half-published",
                                f"{K_CONST_PREFIX}{name}_* declares a base or a size, not both"))
    for name in sorted(set(linker) - set(header)):
        region = linker[name]
        findings.append(Finding(board, "missing-region",
                                f"linker script declares {name.upper()} "
                                f"(origin {region.origin:#010x}, length {region.length:#x}) "
                                f"and the header does not publish it"))
    for name in sorted(set(header) - set(linker)):
        findings.append(Finding(board, "stale-region",
                                f"header publishes {K_CONST_PREFIX}{name}_base and the "
                                f"linker script declares no {name.upper()} region"))
    for name in sorted(set(header) & set(linker)):
        want, got = linker[name], header[name]
        if want.origin != got.origin:
            findings.append(Finding(board, "origin-drift",
                                    f"{name.upper()} origin is {want.origin:#010x} in the linker "
                                    f"script and {got.origin:#010x} in the header"))
        if want.length != got.length:
            findings.append(Finding(board, "length-drift",
                                    f"{name.upper()} length is {want.length:#x} in the linker "
                                    f"script and {got.length:#x} in the header"))
    return findings


def scan(root: Path) -> tuple[list[Finding], list[str], int]:
    """Scan every board package under `root`. Returns findings, skips, checked."""
    findings: list[Finding] = []
    skipped: list[str] = []
    checked = 0
    for package in sorted(root.glob(K_BOARD_GLOB)):
        if not package.is_dir():
            continue
        linker_path = package / K_LINKER_RELATIVE
        header_path = package / K_HEADER_RELATIVE
        if not linker_path.is_file():
            continue
        if not header_path.is_file():
            skipped.append(f"{package.name}: no {K_HEADER_RELATIVE} yet")
            continue
        linker = parse_linker_regions(linker_path.read_text(encoding="utf-8"))
        header, unpaired = parse_header_regions(header_path.read_text(encoding="utf-8"))
        findings.extend(compare(package.name, linker, header, unpaired))
        checked += 1
    return findings, skipped, checked


def selftest() -> int:
    """Prove each rule fires, and that a matching pair is silent."""
    linker_text = """
MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    /* SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 999K  commented out */
    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K - 256
    SDRAM (rwx) : ORIGIN = 0x68000000, LENGTH = 64M
}
"""
    regions = parse_linker_regions(linker_text)
    failures: list[str] = []

    def expect(condition: bool, label: str) -> None:
        if not condition:
            failures.append(label)

    expect(set(regions) == {"mram", "sram", "sdram"}, "region set")
    expect(regions["sram"].length == 1024 * 1024 - 256, "K-suffix arithmetic")
    expect(regions["sdram"].length == 64 * 1024 * 1024, "M suffix")
    expect(regions["mram"].origin == 0x02000000, "origin parse")

    good = """
typedef enum : uintptr_t {
  k_ra8_board_mram_base  = 0x02000000UL,
  k_ra8_board_sram_base  = 0x22000000UL,
  k_ra8_board_sdram_base = 0x68000000UL,
} t;
typedef enum : uint32_t {
  k_ra8_board_mram_size  = 0x00100000UL,
  k_ra8_board_sram_size  = 0x000FFF00UL,
  k_ra8_board_sdram_size = 0x04000000UL,
} u;
"""
    header, unpaired = parse_header_regions(good)
    expect(compare("fixture", regions, header, unpaired) == [], "clean pair is silent")

    rules = {
        "origin-drift": good.replace("k_ra8_board_sram_base  = 0x22000000UL",
                                     "k_ra8_board_sram_base  = 0x22800000UL"),
        "length-drift": good.replace("k_ra8_board_sdram_size = 0x04000000UL",
                                     "k_ra8_board_sdram_size = 0x02000000UL"),
        "stale-region": good.replace("} u;",
                                     "  k_ra8_board_ospi_size = 0x100UL,\n} u;").replace(
                                         "} t;", "  k_ra8_board_ospi_base = 0x80000000UL,\n} t;"),
    }
    for rule, text in rules.items():
        parsed, unpaired_case = parse_header_regions(text)
        fired = {f.rule for f in compare("fixture", regions, parsed, unpaired_case)}
        expect(rule in fired, f"{rule} fires")

    dropped, unpaired_case = parse_header_regions(
        good.replace("  k_ra8_board_sdram_base = 0x68000000UL,\n", "")
            .replace("  k_ra8_board_sdram_size = 0x04000000UL,\n", ""))
    fired = {f.rule for f in compare("fixture", regions, dropped, unpaired_case)}
    expect("missing-region" in fired, "missing-region fires")

    half, unpaired_case = parse_header_regions(
        good.replace("  k_ra8_board_sram_size  = 0x000FFF00UL,\n", ""))
    fired = {f.rule for f in compare("fixture", regions, half, unpaired_case)}
    expect("half-published" in fired, "half-published fires")

    if failures:
        for label in failures:
            print(f"selftest FAILED: {label}", file=sys.stderr)
        return 1
    print("check_board_memory_map selftest: 9 assertions pass")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root to scan")
    parser.add_argument("--selftest", action="store_true",
                        help="prove each rule fires against fixtures, then exit")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    findings, skipped, checked = scan(Path(args.root))
    for note in skipped:
        print(f"skipped {note}")
    if findings:
        print(f"board memory map drifted from the linker script in {len(findings)} place(s):",
              file=sys.stderr)
        for finding in findings:
            print(f"  {finding.render()}", file=sys.stderr)
        print("\nThe linker script is authoritative. Correct "
              "libs/ra8_board_<board>/inc/ra8_board_memmap.h to match it (#758).",
              file=sys.stderr)
        return 1
    print(f"board memory map: {checked} board package(s) match their linker script")
    return 0


if __name__ == "__main__":
    sys.exit(main())
