#!/usr/bin/env python3
r"""check_no_wave_references.py -- ban session-bookkeeping "Wave N" references.

Rationale: comments and commit messages that cite "Wave 70 fixed FRDY" or
"see Wave 43b" leak internal session bookkeeping into the source tree.
Future readers do not care WHEN a fix was found, only WHY. Reference the
function or symbol or HUM section instead.

What this gate flags (case-insensitive):
  - "Wave 70", "wave 3", "WAVE-12", "wave_43b" -- a "wave" token immediately
    followed (after at most one [\\s_-]) by a digit. Optional trailing
    letter(s) for sub-numbering ("wave 43b").
  - "(Wave 12)" / "[wave-7]" -- same pattern wrapped in punctuation.

What this gate does NOT flag (legitimate domain usage):
  - "waveform", "wave shape", "saw wave", "sine wave" (no digit follows).
  - Identifiers like "k_ra8_pdg_wave_saw", "wave_select", "wave_table".
  - Renesas / MIPI / vendor symbols that happen to contain "wave".

Per-line opt-out: append "WAVE-OK: <reason>" on the offending line. Reserve
for unavoidable upstream-symbol citations (e.g. a Renesas register name
that literally encodes the wording).

Exit code:
  0 -- gate clean
  1 -- violations exist

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

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
        "reference",
    }
)

SCAN_EXTS: frozenset[str] = frozenset(
    {".c", ".h", ".cpp", ".hpp", ".cc", ".cmake", ".md", ".yml", ".yaml", ".sh", ".py", ".txt"}
)

SCAN_BASENAMES: frozenset[str] = frozenset({"Makefile", "Dockerfile", "CMakeLists.txt"})

# Maximum number of violations to print before truncating output.
MAX_FINDINGS_SHOWN = 50
# Maximum line length (chars) for a snippet printed in the report.
SNIPPET_MAX_LEN = 120
# Truncated snippet suffix consumes 3 chars ("..."), so trim to this length.
SNIPPET_TRIM_LEN = SNIPPET_MAX_LEN - 3

# "Wave 70", "wave-3", "WAVE_43b", "(Wave 12)" -- token + optional sep +
# digit(s) + optional letter(s).
WAVE_RE: re.Pattern[str] = re.compile(r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\b")

OPTOUT_RE: re.Pattern[str] = re.compile(r"WAVE-OK\s*:")

SELF_EXEMPT_FILES: frozenset[str] = frozenset(
    {
        "scripts/checks/check_no_wave_references.py",
        "scripts/fix/fix_wave_references.py",
        "docs/STYLE_GUIDE.md",
        "CLAUDE.md",
    }
)


def _is_skip_dir(name: str) -> bool:
    """Whether a directory is build output and therefore skipped.

    Matches the ``build-*`` family by prefix so CMake variant directories are
    excluded without being enumerated.
    """
    if name in SKIP_DIR_NAMES:
        return True
    return bool(name.startswith("build-") or name == "build")


def should_scan(path: Path) -> bool:
    """Whether a file's basename or suffix puts it in scope."""
    if path.name in SCAN_BASENAMES:
        return True
    return path.suffix in SCAN_EXTS


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


def scan_file(path: Path, root: Path) -> list[tuple[Path, int, str]]:
    """Report every "Wave N" session reference in one file.

    Self-exempt files are skipped whole: this checker and the policy doc must
    spell the banned pattern to describe it, and tagging every such line
    individually would bury them.
    """
    rel = path.relative_to(root)
    if str(rel) in SELF_EXEMPT_FILES:
        return []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return []
    out: list[tuple[Path, int, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if OPTOUT_RE.search(line):
            continue
        if WAVE_RE.search(line):
            out.append((rel, lineno, line.rstrip()))
    return out


def main() -> int:
    """Ban session-bookkeeping "Wave N" references from the tree.

    These leak an internal working chronology into source that outlives it: a
    future reader cannot resolve "see Wave 43b" to anything, whereas the
    function, symbol or HUM section the fix touched stays findable.

    Returns 1 listing each reference, 0 when the tree is clean.
    """
    root = Path(__file__).resolve().parents[2]
    files = iter_source_files(root)
    findings: list[tuple[Path, int, str]] = []
    for f in files:
        findings.extend(scan_file(f, root))

    if not findings:
        print("no-wave-refs: 0 violations -- gate clean.")
        return 0

    print(f"no-wave-refs: {len(findings)} violations found.")
    for rel, lineno, line in findings[:MAX_FINDINGS_SHOWN]:
        snippet = line if len(line) <= SNIPPET_MAX_LEN else line[:SNIPPET_TRIM_LEN] + "..."
        print(f"  {rel}:{lineno} {snippet}")
    if len(findings) > MAX_FINDINGS_SHOWN:
        print(f"  ... {len(findings) - MAX_FINDINGS_SHOWN} more (truncated)")
    print()
    print('Per-line opt-out: append "WAVE-OK: <reason>" on the offending line.')
    print("Auto-fix helper: scripts/fix/fix_wave_references.py --apply")
    return 1


if __name__ == "__main__":
    sys.exit(main())
