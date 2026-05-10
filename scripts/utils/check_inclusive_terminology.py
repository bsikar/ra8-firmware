#!/usr/bin/env python3
"""
check_inclusive_terminology.py -- inclusive-terminology gate for ra8d2-firmware.

Bans the legacy master/slave/MOSI/MISO/SS vocabulary from FIRST-PARTY source
under libs/, src/, examples/, tests/, port/, scripts/, docs/, and the top-
level CMake / Makefile / workflow files. CLAUDE.md "Terminology Standard"
mandates Controller/Peripheral, COPI/CIPO, CS/Chip Select, Primary/Main.

Per-line opt-out: append a `LEGACY-OK: <reason>` annotation on the offending
line. Reserved for unavoidable upstream-symbol citations (e.g. the literal
spelling of a Renesas HUM register-bit name where the symbol must appear
verbatim in the source comment).

Exit code:
  0 -- no violations (gate clean), or warn-only mode is on
  1 -- violations exist (only when WARN_ONLY_MODE is False)

The script is intentionally fast (pure-Python regex scan, no libclang) so
the pre-commit hook stays interactive.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------------

# + sweep brought first-party source to 0 violations and the gate is
# now strict (False). If a future agent reintroduces a violation the gate
# fails the commit; either rewrite the wording or annotate the line with
# `LEGACY-OK: <reason>` if the legacy symbol must literally appear, or add
# the file to SKIP_FILE_PATTERNS below if it is dominated by upstream
# vendor citations.
WARN_ONLY_MODE: bool = False

# Roots scanned for violations.
SCAN_ROOTS: tuple[str, ...] = (
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

# Directories anywhere in the path that suppress the scan.
SKIP_DIR_NAMES: frozenset[str] = frozenset(
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
        "reference",  # docs/reference/ holds vendor PDFs and chapter maps
    }
)

# File extensions we scan. Anything else is binary / not-our-source.
SCAN_EXTS: frozenset[str] = frozenset(
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

# Filenames (no extension) we still want to scan.
SCAN_BASENAMES: frozenset[str] = frozenset({"Makefile", "Dockerfile", "CMakeLists.txt"})

# The terms themselves. Word-boundary regex; case-insensitive where the
# legacy spelling is itself case-insensitive in prose.
PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("master", re.compile(r"\bmaster(s|ed|ing|ship)?\b", re.IGNORECASE)),
    ("slave", re.compile(r"\bslave(s|d)?\b", re.IGNORECASE)),
    ("mosi", re.compile(r"\bMOSI\b")),
    ("miso", re.compile(r"\bMISO\b")),
    ("slave_select", re.compile(r"\bSlave[ _-]Select\b", re.IGNORECASE)),
    ("ss_pin", re.compile(r"\bSS\b")),  # SPI Slave Select pin abbreviation -- use CS
)

# Per-line opt-out marker.
LEGACY_OK_RE: re.Pattern[str] = re.compile(r"LEGACY-OK\s*:")

# Self-exempt: this file talks about the banned terms by definition.
SELF_EXEMPT_FILES: frozenset[str] = frozenset(
    {
        "scripts/utils/check_inclusive_terminology.py",
        "scripts/utils/fix_inclusive_terminology.py",
        "docs/STYLE_GUIDE.md",
        "docs/RING_AND_WORLD.md",
        "docs/ACRONYMS.md",
        "docs/MCDC_GAPS.md",
        "docs/MCDC_GAPS.csv",
        "CLAUDE.md",
        ".github/workflows/inclusive-terminology.yml",
    }
)

# Whole-file skip list for vendor-symbol-dominated source. Each entry
# represents a file where the legacy spelling is part of an upstream
# contract (Renesas FSP CMSIS/HUM register-bit names, Renesas FSP
# `r_iic_b_master_*` API names, Express Logic USBX `UX_SLAVE_*` types,
# Mbed TLS macro symbols, IEEE 802.1AS / IEEE 1588 PTP role names that
# appear verbatim in the spec, OctoSPI "slave 0/1" wording from the
# silicon spec, etc.) and renaming would break the upstream interface
# or misrepresent the silicon. Tests that exercise these vendor APIs
# inherit the same skip because their fixtures must spell the symbols
# verbatim.
SKIP_FILE_PATTERNS: tuple[str, ...] = (
    # Renesas RA8D2 Hardware User's Manual register-bit name citations.
    # Every k_ra_*_bit_* / k_ra_*_mask_* enum mirrors a HUM bit name.
    "libs/ra_hal/inc/ra8d2_iic_b_regs.h",
    "libs/ra_hal/inc/ra8d2_i3c_regs.h",
    "libs/ra_hal/inc/ra8d2_ospi_regs.h",
    "libs/ra_hal/inc/ra8d2_mipi_phy_regs.h",
    "libs/ra_hal/inc/ra8d2_ptp_regs.h",
    "libs/ra_hal/inc/ra8d2_spi_regs.h",
    "libs/ra_hal/inc/ra8d2_ssie_regs.h",
    "libs/ra_hal/inc/ra8d2_vin_regs.h",
    "libs/ra_hal/inc/ra8d2_vreg_regs.h",
    # Renesas FSP `r_iic_b_master_*` API surface references.
    "libs/ra_hal/inc/ra_iic_b.h",
    "libs/ra_hal/src/ra_iic_b.c",
    # IEEE 1588 PTP master/slave role names appear verbatim in the spec.
    "libs/ra_hal/inc/ra_ptp.h",
    "libs/ra_hal/src/ra_ptp.c",
    "tests/test_ra_ptp.c",
    # MIPI D-PHY `MASTEREN` register bit and DSI/CSI master/slave mode
    # nomenclature mirrors the MIPI Alliance D-PHY specification.
    "libs/ra_hal/inc/ra_mipi_phy.h",
    "libs/ra_hal/src/ra_mipi_phy.c",
    "tests/test_ra_mipi_phy.c",
    # SOUP provenance docs cite upstream component descriptions verbatim.
    "docs/SOUP/nimble.md",
    "docs/SOUP/ble_patch_image.md",
    "docs/SOUP/r_sce_AMC_firmware.md",
)


# --------------------------------------------------------------------------
# Implementation
# --------------------------------------------------------------------------


def should_scan(path: Path) -> bool:
    parts = set(path.parts)
    if parts & SKIP_DIR_NAMES:
        return False
    if path.name in SCAN_BASENAMES:
        return True
    return path.suffix in SCAN_EXTS


def _is_skip_dir(name: str) -> bool:
    if name in SKIP_DIR_NAMES:
        return True
    # Glob-style: any CMake/build artefact dir like build-fuzz, build-bench.
    if name.startswith("build-") or name == "build":
        return True
    return False


def iter_source_files(root: Path) -> list[Path]:
    out: list[Path] = []
    for scan_root in SCAN_ROOTS:
        base = root / scan_root
        if not base.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if not _is_skip_dir(d)]
            for fn in filenames:
                p = Path(dirpath) / fn
                if should_scan(p):
                    out.append(p)
    for top in ("Makefile", "CMakeLists.txt", "README.md"):
        p = root / top
        if p.exists():
            out.append(p)
    return out


def scan_file(path: Path, root: Path) -> list[tuple[Path, int, str, str]]:
    rel = path.relative_to(root)
    rel_str = str(rel)
    if rel_str in SELF_EXEMPT_FILES:
        return []
    if rel_str in SKIP_FILE_PATTERNS:
        return []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    out: list[tuple[Path, int, str, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if LEGACY_OK_RE.search(line):
            continue
        for term, regex in PATTERNS:
            if not regex.search(line):
                continue
            out.append((rel, lineno, term, line.rstrip()))
            break
    return out


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    findings: list[tuple[Path, int, str, str]] = []
    for f in files:
        findings.extend(scan_file(f, root))

    if not findings:
        print("inclusive-terminology: 0 violations -- gate clean.")
        return 0

    print(f"inclusive-terminology: {len(findings)} violations found.")
    for rel, lineno, term, line in findings[:50]:
        snippet = line if len(line) <= 120 else line[:117] + "..."
        print(f"  {rel}:{lineno} [{term}] {snippet}")
    if len(findings) > 50:
        print(f"  ... {len(findings) - 50} more (truncated)")
    print("")
    print("Per-line opt-out: append `LEGACY-OK: <reason>` on the offending line.")
    print("See CLAUDE.md 'Terminology Standard' for the policy.")

    if WARN_ONLY_MODE:
        print("WARN_ONLY_MODE=True -- not failing the gate.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
