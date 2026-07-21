#!/usr/bin/env python3
"""check_inclusive_terminology.py -- inclusive-terminology gate for ra8-firmware.

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
# fails the commit; either rewrite the wording (preferred for our own
# prose and symbols) or, only where an upstream string must appear
# verbatim (a Renesas HUM section title, a literal FSP / USBX API symbol),
# annotate that single line with `LEGACY-OK: <reason>`. There is no
# whole-file escape hatch -- every line stands on its own.
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
    "esp32",  # first-party ESP32-C6 companion sub-project (drivers/ours, src, hal)
    "tools",  # first-party host tools incl. tools/board_sim (same bar per CLAUDE.md)
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
        "doxygen",  # docs/doxygen/ is generated Doxygen output, not authored
        "html",  # generated HTML (e.g. docs/**/html) is not first-party source
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
        ".csv",  # docs/MCDC_GAPS.csv and other generated/authored tables
    }
)

# Filenames (no extension) we still want to scan.
SCAN_BASENAMES: frozenset[str] = frozenset({"Makefile", "Dockerfile", "CMakeLists.txt"})

# Prose patterns: word-boundary matches that catch the legacy vocabulary
# wherever it appears as a standalone word -- prose comments ("master
# enable"), HUM section titles, "Slave Select", the bare "SS" pin name.
PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("master", re.compile(r"\bmaster(s|ed|ing|ship)?\b", re.IGNORECASE)),
    ("slave", re.compile(r"\bslave(s|d)?\b", re.IGNORECASE)),
    ("mosi", re.compile(r"\bMOSI\b")),
    ("miso", re.compile(r"\bMISO\b")),
    ("slave_select", re.compile(r"\bSlave[ _-]Select\b", re.IGNORECASE)),
    ("ss_pin", re.compile(r"\bSS\b")),  # SPI Slave Select pin abbreviation -- use CS
)

# Identifier-embedded detection.
#
# Python's `\b` does not fire between an underscore and a letter (`_` is a
# word character), so the prose patterns above miss the legacy words when
# they are welded into a snake_case / SCREAMING_CASE symbol
# (`k_ra8_ptp_role_master`, `internal_spcr_master`, `make_master_i2s_cfg`,
# ...). Instead of enumerating our own prefixes, flag ANY identifier that
# carries `master`/`slave` as a leading component and is NOT part of a
# vendored upstream namespace -- those APIs are referenced verbatim in our
# glue code and cannot be renamed.
IDENT_RE: re.Pattern[str] = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
# `master`/`slave` as a component: at the identifier start or just after an
# underscore (so `MASTEREN`, `ra8_spi_master_init`, `_slave` all match, while
# camelCase `offsetFromMaster` and substrings like `enslave` do not).
IDENT_TERM_RE: re.Pattern[str] = re.compile(r"(?:^|_)(?:master|slave)", re.IGNORECASE)
# Upstream namespaces whose symbols legitimately appear in first-party glue
# and are cited verbatim: Express Logic USBX / ThreadX / NetX / FileX /
# LevelX, Renesas FSP (r_iic_* / r_sce_*), Mbed TLS, NimBLE / Mynewt, and
# Espressif esp-hosted. Matched at the identifier head.
VENDOR_IDENT_RE: re.Pattern[str] = re.compile(
    r"^(?:_?ux_|r_iic|r_sce|_?nx_|ble_|mynewt|mbedtls|tls_|pre_?master"
    r"|premaster|resumption_master|_?lx_|_?tx_|_?gx_|_?fx_|netx|threadx"
    r"|usbx|filex|levelx|esp_hosted|dcd_sim_slave|hcd_sim_host)",
    re.IGNORECASE,
)
# Hardware register-bit names that spell a legacy token verbatim (Renesas
# MIPI D-PHY DPHYMDC.MASTEREN); these are silicon names, not our symbols.
HW_TOKENS: frozenset[str] = frozenset({"MASTEREN"})

# Per-line opt-out marker.
LEGACY_OK_RE: re.Pattern[str] = re.compile(r"LEGACY-OK\s*:")


def identifier_violation(line: str) -> str | None:
    """The offending identifier when a line names a symbol with legacy terminology.

    Checks IDENTIFIERS, not prose: a comment discussing the legacy term --
    often required when mapping a vendor document onto our names -- is fine,
    while a symbol carrying it is not, because the symbol propagates.

    Returns None when the line is clean.

    Vendor-namespace identifiers and hardware register-bit names are
    skipped: they are upstream contracts spelled verbatim, not symbols
    this project is free to rename.
    """
    for m in IDENT_RE.finditer(line):
        tok = m.group(0)
        if not IDENT_TERM_RE.search(tok):
            continue
        if tok in HW_TOKENS:
            continue
        if VENDOR_IDENT_RE.match(tok):
            continue
        return tok
    return None


# Output display limits.
MAX_SNIPPET_LEN = 120
SNIPPET_TRUNCATE_LEN = 117
MAX_FINDINGS_SHOWN = 50

# Self-exempt: a short, closed list of files that MUST spell the banned
# vocabulary to do their job. There is NO vendor-file escape hatch here --
# a file dominated by upstream symbols annotates each such line with
# `LEGACY-OK: <reason>` instead of exempting the whole file.
#
# Only two kinds of file qualify:
#   1. The detection scripts, which hold the banned terms as regex literals.
#   2. The policy documents that DEFINE the terminology standard by quoting
#      the words they ban (CLAUDE.md, docs/STYLE_GUIDE.md).
SELF_EXEMPT_FILES: frozenset[str] = frozenset(
    {
        "scripts/checks/check_inclusive_terminology.py",
        "scripts/checks/check_inclusive_terminology_commits.py",
        "scripts/fix/fix_inclusive_terminology.py",
        "docs/STYLE_GUIDE.md",
        "CLAUDE.md",
    }
)


# --------------------------------------------------------------------------
# Implementation
# --------------------------------------------------------------------------


def should_scan(path: Path) -> bool:
    """Whether a file is in scope, by directory, basename and suffix."""
    parts = set(path.parts)
    if parts & SKIP_DIR_NAMES:
        return False
    if path.name in SCAN_BASENAMES:
        return True
    return path.suffix in SCAN_EXTS


def _is_skip_dir(name: str) -> bool:
    """Whether a directory is build output and therefore skipped.

    Matches the ``build-*`` family by prefix so CMake variant directories are
    excluded without being enumerated.
    """
    if name in SKIP_DIR_NAMES:
        return True
    # Glob-style: any CMake/build artefact dir like build-fuzz, build-bench.
    return bool(name.startswith("build-") or name == "build")


def iter_source_files(root: Path) -> list[Path]:
    """Every in-scope file beneath the configured scan roots."""
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
    """Report every legacy-terminology identifier in one file.

    Self-exempt files -- this checker, the terminology policy -- are skipped
    whole, since they must name the banned terms to define them.
    """
    rel = path.relative_to(root)
    rel_str = str(rel)
    if rel_str in SELF_EXEMPT_FILES:
        return []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    out: list[tuple[Path, int, str, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if LEGACY_OK_RE.search(line):
            continue
        matched = False
        for term, regex in PATTERNS:
            if not regex.search(line):
                continue
            out.append((rel, lineno, term, line.rstrip()))
            matched = True
            break
        if matched:
            continue
        ident = identifier_violation(line)
        if ident is not None:
            out.append((rel, lineno, f"symbol:{ident}", line.rstrip()))
    return out


def main() -> int:
    """Enforce OSHWA-inclusive terminology on first-party identifiers.

    Scoped to identifiers rather than all text on purpose: vendor manuals and
    external APIs still use the legacy terms, and comments mapping our names
    onto theirs are required elsewhere in this tree. The rule governs what
    this codebase NAMES, not what it may mention.

    Returns 1 listing each offending symbol, 0 when the tree is clean.
    """
    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    findings: list[tuple[Path, int, str, str]] = []
    for f in files:
        findings.extend(scan_file(f, root))

    if not findings:
        print("inclusive-terminology: 0 violations -- gate clean.")
        return 0

    print(f"inclusive-terminology: {len(findings)} violations found.")
    for rel, lineno, term, line in findings[:MAX_FINDINGS_SHOWN]:
        snippet = line if len(line) <= MAX_SNIPPET_LEN else line[:SNIPPET_TRUNCATE_LEN] + "..."
        print(f"  {rel}:{lineno} [{term}] {snippet}")
    if len(findings) > MAX_FINDINGS_SHOWN:
        print(f"  ... {len(findings) - MAX_FINDINGS_SHOWN} more (truncated)")
    print()
    print("Per-line opt-out: append `LEGACY-OK: <reason>` on the offending line.")
    print("See CLAUDE.md 'Terminology Standard' for the policy.")

    if WARN_ONLY_MODE:
        print("WARN_ONLY_MODE=True -- not failing the gate.")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
