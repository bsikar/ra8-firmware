#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

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

# Directories whose CONTENT is vendored or generated and not ours to police:
# docs/reference/ holds committed vendor PDFs and register maps (HUM register
# names literally spell MASTEREN and the like), and docs/**/doxygen,
# docs/**/html are generated Doxygen output. first_party_paths already drops
# third_party/ and build output; these three are the docs-side equivalents the
# old SKIP_DIR_NAMES carried. Matched by path component, as the walk did.
DOCS_VENDOR_DIRS: frozenset[str] = frozenset({"reference", "doxygen", "html"})

# File extensions we scan. Anything else is binary / not-our-source.
SCAN_EXTS: frozenset[str] = frozenset(
    {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cc",
        ".cmake",
        ".mk",
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
SCAN_BASENAMES: frozenset[str] = frozenset(
    {"Makefile", "GNUmakefile", "Dockerfile", "CMakeLists.txt"}
)

# A tree this size cannot legitimately collapse to a handful of files. A scan
# that enumerates almost nothing reports a clean tree because it read almost
# nothing -- the exact failure the gate-honesty epic (#190) exists to prevent.
# Measured 2026-08-02: 3415 first-party files in the derived scope. Same
# trip-wire as check_ruff.py.
FILE_FLOOR = 2500

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
# (`internal_spcr_master`, `make_master_i2s_cfg`,
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
#      the words they ban (CLAUDE.md, docs/STYLE_GUIDE.md, and the
#      style-reviewer subagent whose Terminology Standard section instructs
#      "Use Controller/Peripheral instead of master/slave").
SELF_EXEMPT_FILES: frozenset[str] = frozenset(
    {
        "scripts/checks/check_inclusive_terminology.py",
        "scripts/checks/check_inclusive_terminology_commits.py",
        "scripts/fix/fix_inclusive_terminology.py",
        "docs/STYLE_GUIDE.md",
        "CLAUDE.md",
        ".claude/agents/style-reviewer.md",
    }
)


# --------------------------------------------------------------------------
# Implementation
# --------------------------------------------------------------------------


def iter_source_files(root: Path) -> list[Path]:
    """Every in-scope first-party file, derived from git rather than a root list.

    Enumeration goes through ``lint_targets.first_party_paths`` -- the shared
    derived-scope primitive -- so ``infra/`` and ``mk/`` (the roots a hardcoded
    list silently dropped, #549) are covered, and any future top-level
    directory is in scope the day it lands. The only subtractions on top of what
    that primitive already exempts (third_party/, generated fonts, build output)
    are the docs-side vendored/generated directories in ``DOCS_VENDOR_DIRS``.
    """
    rels = set(first_party_paths(tuple(SCAN_EXTS)))
    for name in SCAN_BASENAMES:
        rels |= {rel for rel in first_party_paths((name,)) if Path(rel).name == name}
    out: list[Path] = []
    for rel in sorted(rels):
        if set(Path(rel).parts) & DOCS_VENDOR_DIRS:
            continue
        out.append(root / rel)
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


def selftest() -> int:
    """Prove the detector fires on legacy terms, spares the legitimate ones, and scans real files.

    Both directions plus a scope probe: a legacy identifier and the bare prose
    tokens must FIRE, while an inclusive rewrite, a vendored-namespace symbol
    and a hardware register-bit name must stay QUIET; the derived scope must
    clear ``FILE_FLOOR`` and reach the roots a hardcoded list had dropped
    (``infra/``, ``mk/``). A clean run over a scope that never sees those roots,
    or one whose detector had stopped matching, proves nothing.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    # Prose / identifier detection, driven through scan_line-equivalent logic.
    fire_lines = (
        ("a legacy identifier", "ra8_err_t ra8_spi_master_init(void);"),
        ("the bare MOSI token", "// route MOSI to the header"),
        ('the "Slave Select" phrase', "assert the Slave Select line"),
    )
    for label, line in fire_lines:
        prose = any(regex.search(line) for _, regex in PATTERNS)
        ident = identifier_violation(line) is not None
        expect(prose or ident, f"MUST FIRE: {label}", failures)

    quiet_lines = (
        ("an inclusive rewrite", "ra8_err_t ra8_spi_controller_init(void);"),
        ("a vendored ux_ symbol", "ux_device_class_storage_master_read();"),
        ("the MASTEREN register bit", "DPHYMDC.MASTEREN = 1U;  // silicon name"),
    )
    for label, line in quiet_lines:
        prose = any(regex.search(line) for _, regex in PATTERNS)
        ident = identifier_violation(line) is not None
        expect(not (prose or ident), f"MUST NOT FIRE: {label}", failures)

    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    rels = {str(p.relative_to(root)) for p in files if p.is_relative_to(root)}
    expect(
        len(files) >= FILE_FLOOR,
        f"derived scope sees {len(files)} file(s) (floor {FILE_FLOOR})",
        failures,
    )
    for root_name in ("infra", "mk"):
        expect(
            any(rel.startswith(root_name + "/") for rel in rels),
            f"the derived scope reaches {root_name}/ (previously omitted)",
            failures,
        )
    return report(failures)


def main() -> int:
    """Enforce OSHWA-inclusive terminology on first-party identifiers.

    Scoped to identifiers rather than all text on purpose: vendor manuals and
    external APIs still use the legacy terms, and comments mapping our names
    onto theirs are required elsewhere in this tree. The rule governs what
    this codebase NAMES, not what it may mention.

    Returns 1 listing each offending symbol, 0 when the tree is clean, 2 when
    the derived scope collapsed below FILE_FLOOR.
    """
    if "--selftest" in sys.argv[1:]:
        return selftest()
    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    if len(files) < FILE_FLOOR:
        sys.stderr.write(
            f"inclusive-terminology: FATAL -- only {len(files)} file(s) in scope, floor "
            f"is {FILE_FLOOR}. A collapsed scope reports a clean tree because it scanned "
            "nothing.\n"
        )
        return 2
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
