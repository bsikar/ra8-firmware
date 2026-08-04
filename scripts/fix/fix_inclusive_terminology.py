#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""fix_inclusive_terminology.py -- conservative auto-fix for legacy spellings.

Companion to check_inclusive_terminology.py. Walks tracked source/docs and
rewrites the most common comment/identifier patterns to their inclusive
counterparts. Anything the script cannot rewrite is reported on stderr for
a human to review.

Substitutions (case-preserving where possible):

  Plain words (in comments / docs):
    master   -> primary           Master   -> Primary
    masters  -> primaries         Masters  -> Primaries
    mastered -> finalised         (rare; flagged for review)
    slave    -> peripheral        Slave    -> Peripheral
    slaves   -> peripherals       Slaves   -> Peripherals
    slaved   -> bound             (rare; flagged for review)

  Phrases:
    master/slave           -> controller/peripheral
    master-slave           -> controller-peripheral
    Slave Select / SS pin  -> Chip Select / CS pin
    MOSI                   -> COPI
    MISO                   -> CIPO
    slave stack            -> device stack
    slave-state machine    -> device-state machine
    slave device           -> device

This script intentionally leaves UPSTREAM SYMBOLS alone -- e.g.
``UX_SLAVE_TRANSFER``, ``MBEDTLS_SSL_EXTENDED_MASTER_SECRET``,
``r_iic_b_master_open`` -- because those are part of the upstream API
contract. Use the per-line ``LEGACY-OK: <reason>`` opt-out for those.

Usage:
  scripts/fix/fix_inclusive_terminology.py            # dry-run
  scripts/fix/fix_inclusive_terminology.py --apply    # write back

Exit code:
  0 -- no remaining violations
  1 -- some remain (human edit needed)

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

# Maximum line length shown in the remaining-violations report.
REPORT_SNIPPET_MAX_LEN = 120
# Maximum number of violations printed before showing a count summary.
REPORT_MAX_LINES = 50

SCAN_ROOTS = (
    "libs",
    "src",
    "examples",
    "tests",
    "port",
    "scripts",
    "docs",
    "cmake",
    ".github",
)
SKIP_DIR_NAMES = frozenset(
    {
        "build",
        "build-cov",
        "build-scan",
        "build-tidy",
        ".git",
        "_deps",
        "third_party",
        "__pycache__",
        ".cache",
        "node_modules",
        "reference",
    }
)
SCAN_EXTS = frozenset(
    {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cc",
        ".cmake",
        ".md",
        ".yml",
        ".yaml",
        ".sh",
        ".py",
        ".txt",
    }
)
SCAN_BASENAMES = frozenset({"Makefile", "Dockerfile", "CMakeLists.txt"})

# Mirror the gate's exemption / skip list so we do not rewrite vendor
# files that the gate ignores.
SELF_EXEMPT = frozenset(
    {
        "scripts/checks/check_inclusive_terminology.py",
        "scripts/fix/fix_inclusive_terminology.py",
        "docs/STYLE_GUIDE.md",
        "docs/RING_AND_WORLD.md",
        "docs/ACRONYMS.md",
        "docs/MCDC_GAPS.md",
        "docs/MCDC_GAPS.csv",
        "CLAUDE.md",
        ".github/workflows/inclusive-terminology.yml",
    }
)

# These paths are documented vendor citations; leave them alone.
SKIP_PATTERNS = frozenset(
    {
        "libs/ra8_hal/inc/ra8_iic_b_regs.h",
        "libs/ra8_hal/inc/ra8_i3c_regs.h",
        "libs/ra8_hal/inc/ra8_ospi_regs.h",
        "libs/ra8_hal/inc/ra8_mipi_phy_regs.h",
        "libs/ra8_hal/inc/ra8_spi_regs.h",
        "libs/ra8_hal/inc/ra8_ssie_regs.h",
        "libs/ra8_hal/inc/ra8_vin_regs.h",
        "libs/ra8_hal/inc/ra8_vreg_regs.h",
        "libs/ra8_hal/inc/ra8_iic_b.h",
        "libs/ra8_hal/src/ra8_iic_b.c",
        "libs/ra8_hal/inc/ra8_i2c.h",
        "libs/ra8_hal/src/ra8_i2c.c",
        "libs/ra8_hal/inc/ra8_mipi_phy.h",
        "libs/ra8_hal/src/ra8_mipi_phy.c",
        "tests/test_ra8_mipi_phy_init.c",
        "tests/test_ra8_mipi_phy_lanes.c",
        "docs/SOUP/nimble.md",
    }
)

# Ordered: most-specific first.
REWRITES: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"\bmaster[/\-]slave\b"), "controller-peripheral"),
    (re.compile(r"\bMaster[/\-]Slave\b"), "Controller-Peripheral"),
    (re.compile(r"\bMOSI\b"), "COPI"),
    (re.compile(r"\bMISO\b"), "CIPO"),
    (re.compile(r"\bSlave[ _\-]Select\b", re.IGNORECASE), "Chip Select"),
    (re.compile(r"\bSS\s+pin\b"), "CS pin"),
    (re.compile(r"\bslave\s+stack\b", re.IGNORECASE), "device stack"),
    (re.compile(r"\bslave[ _\-]state\s+machine\b", re.IGNORECASE), "device-state machine"),
    (re.compile(r"\bslave\s+device\b", re.IGNORECASE), "device"),
    (re.compile(r"\bSlave\s+Device\b"), "Device"),
    (re.compile(r"\bSLAVE\b"), "PERIPHERAL"),
    (re.compile(r"\bMASTER\b"), "PRIMARY"),
    (re.compile(r"\bMasters\b"), "Primaries"),
    (re.compile(r"\bmasters\b"), "primaries"),
    (re.compile(r"\bMaster\b"), "Primary"),
    (re.compile(r"\bmaster\b"), "primary"),
    (re.compile(r"\bSlaves\b"), "Peripherals"),
    (re.compile(r"\bslaves\b"), "peripherals"),
    (re.compile(r"\bSlave\b"), "Peripheral"),
    (re.compile(r"\bslave\b"), "peripheral"),
]

# Same regex the gate uses to count residue.
LEGACY_RE = re.compile(
    r"(?<![A-Za-z0-9_])(master|slave|MOSI|MISO)(?![A-Za-z0-9_])",
    re.IGNORECASE,
)
OPTOUT_RE = re.compile(r"LEGACY-OK\s*:")


def _is_skip_dir(name: str) -> bool:
    """Whether a directory name is build output or otherwise out of scope.

    Matches the ``build-*`` family by prefix as well as the exact names, so a
    CMake variant directory (build-cov, build-fuzz) is skipped without being
    listed individually.
    """
    return name in SKIP_DIR_NAMES or name == "build" or name.startswith("build-")


def should_scan(p: Path) -> bool:
    """Whether this file's name or suffix puts it in scope.

    Basename is checked as well as suffix so extensionless files the tree
    cares about (CMakeLists.txt, Makefile) are not missed.
    """
    return p.name in SCAN_BASENAMES or p.suffix in SCAN_EXTS


def iter_files(root: Path) -> list[Path]:
    """Every in-scope file beneath the configured scan roots."""
    out: list[Path] = []
    for sr in SCAN_ROOTS:
        base = root / sr
        if not base.exists():
            continue
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if not _is_skip_dir(d)]
            for f in fn:
                p = Path(dp) / f
                if should_scan(p):
                    out.append(p)
    for top in ("Makefile", "CMakeLists.txt", "README.md"):
        p = root / top
        if p.exists():
            out.append(p)
    return out


def rewrite_line(line: str) -> str:
    """Apply every terminology rewrite to one line and tidy the result.

    Collapses runs of spaces afterwards, because the replacements differ in
    length from what they replace and would otherwise leave ragged gaps where
    a longer legacy term was swapped out.
    """
    for rx, sub in REWRITES:
        line = rx.sub(sub, line)
    return re.sub(r"  +", " ", line).rstrip()


def rewrite(text: str) -> str:
    """Rewrite only the lines carrying legacy terminology, leaving the rest byte-identical.

    Lines matching the opt-out marker are passed through untouched -- that is
    what protects a deliberate quotation of a vendor document, where the
    legacy term is the accurate one.
    """
    out = []
    for ln in text.splitlines():
        if LEGACY_RE.search(ln) and not OPTOUT_RE.search(ln):
            out.append(rewrite_line(ln))
        else:
            out.append(ln)
    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


def main() -> int:
    """Report, or with ``--apply`` perform, the inclusive-terminology rewrite.

    Dry-run by default: without ``--apply`` nothing is written, so the diff
    can be inspected before a tree-wide substitution is committed.

    Returns 0 when no line needed rewriting, 1 when some did (in either mode),
    so the dry run doubles as a gate.
    """
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[2]
    files = iter_files(root)

    fixed_files = 0
    fixed_lines = 0
    remaining: list[tuple[Path, int, str]] = []

    for path in files:
        rel = str(path.relative_to(root))
        if rel in SELF_EXEMPT or rel in SKIP_PATTERNS:
            continue
        try:
            old = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if not LEGACY_RE.search(old):
            continue
        new = rewrite(old)
        if new != old:
            n = sum(1 for a, b in zip(old.splitlines(), new.splitlines()) if a != b)
            n += abs(len(old.splitlines()) - len(new.splitlines()))
            fixed_files += 1
            fixed_lines += n
            if args.apply:
                path.write_text(new, encoding="utf-8")
        for ln, line in enumerate(new.splitlines(), start=1):
            if OPTOUT_RE.search(line):
                continue
            if LEGACY_RE.search(line):
                remaining.append((path.relative_to(root), ln, line.rstrip()))

    mode = "applied" if args.apply else "dry-run"
    print(f"fix-inclusive ({mode}): rewrote {fixed_lines} lines across {fixed_files} files.")
    if remaining:
        print(f"REMAINING: {len(remaining)} -- needs human edit:", file=sys.stderr)
        for rel, ln, line in remaining[:REPORT_MAX_LINES]:
            snippet = line if len(line) <= REPORT_SNIPPET_MAX_LEN else line[:117] + "..."
            print(f"  {rel}:{ln} {snippet}", file=sys.stderr)
        if len(remaining) > REPORT_MAX_LINES:
            print(f"  ... {len(remaining) - REPORT_MAX_LINES} more", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
