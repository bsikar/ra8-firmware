#!/usr/bin/env python3
"""
fix_wave_references.py -- conservative auto-fix for "Wave N" session refs.

Companion to check_no_wave_references.py. Walks every tracked source/doc
file under the same scan roots and rewrites obvious session-bookkeeping
patterns:

  - "(Wave 70)"        -> ""           (delete the parenthetical)
  - "[wave-3]"         -> ""           (delete the bracketed)
  - " -- Wave 12"      -> ""           (trailing em-dash citation)
  - "see Wave N"       -> "see HUM"    (placeholder cite; human reviews)
  - "Wave 70 added X"  -> "X"          (drop the leading attribution)
  - "Wave 70 fixed X"  -> "Fixed: X"
  - "Wave 70's X"      -> "the X"
  - "the wave 70 X"    -> "the X"
  - "during Wave 11"   -> ""
  - "post-wave-7"      -> ""
  - "from Wave N"      -> ""

Anything else that the gate flagged but this script could not rewrite is
reported on stderr so a human can fix it manually.

Usage:
  scripts/fix/fix_wave_references.py            # dry-run (default)
  scripts/fix/fix_wave_references.py --apply    # write changes back

Exit code:
  0 -- no remaining violations after the rewrite (or none to start with)
  1 -- some violations remain that the script could not rewrite

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

SELF_EXEMPT = frozenset(
    {
        "scripts/checks/check_no_wave_references.py",
        "scripts/fix/fix_wave_references.py",
        "docs/STYLE_GUIDE.md",
        "CLAUDE.md",
    }
)

# Ordered: most-specific first so we strip larger phrases before residue.
REWRITES: list[tuple[re.Pattern[str], str]] = [
    # "(Wave 70)" or "[wave-7]" -- entire parenthetical/bracketed.
    (re.compile(r"\s*[\(\[][Ww]ave[\s_\-]?\d+[A-Za-z]?[\)\]]"), ""),
    # " -- Wave 12 ..." trailing comment citation up to end-of-line/period.
    (re.compile(r"\s*--\s*[Ww]ave[\s_\-]?\d+[A-Za-z]?[^.\n]*"), ""),
    # "; see Wave N" / ", see Wave N" -- inline see-also citation.
    (re.compile(r"[,;]\s*see\s+[Ww]ave[\s_\-]?\d+[A-Za-z]?", re.IGNORECASE), ""),
    # "see Wave N" -> "see the relevant HUM section" placeholder.
    (
        re.compile(r"\bsee\s+[Ww]ave[\s_\-]?\d+[A-Za-z]?\b", re.IGNORECASE),
        "see the relevant HUM section",
    ),
    # "the wave-N <thing>" -> "the <thing>"
    (re.compile(r"\bthe\s+[Ww]ave[\s_\-]?\d+[A-Za-z]?\s+", re.IGNORECASE), "the "),
    # "during Wave N" / "in Wave N" / "from Wave N" / "after Wave N" / "post-wave-N"
    (
        re.compile(
            r"\b(?:during|in|from|after|post[\s_\-]?)[Ww]ave[\s_\-]?\d+[A-Za-z]?\b", re.IGNORECASE
        ),
        "",
    ),
    # "pre-Wave-N" -> "previous"
    (re.compile(r"\bpre[\s_\-]?[Ww]ave[\s_\-]?\d+[A-Za-z]?\b", re.IGNORECASE), "previous"),
    # "Wave N's <thing>" -> "the <thing>" (handles possessive)
    (re.compile(r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?'s\b"), "the"),
    # "Wave N: " sentence prefix -> ""
    (re.compile(r"^\s*[\*\#\-/\s]*[Ww]ave[\s_\-]?\d+[A-Za-z]?\s*:\s*", re.MULTILINE), ""),
    # "Wave N added X" / "Wave N fixed X" -> drop the "Wave N " prefix.
    (
        re.compile(
            r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\s+(added|fixed|introduced|removed|"
            r"refactored|reworked|landed|backfilled|swept|cleaned|wired|spawned|"
            r"split|merged|deleted|created|reverted|enabled|disabled|brought\s+up)\b",
            re.IGNORECASE,
        ),
        r"\1",
    ),
    # Bare "Wave N" left over -- last resort, just delete the token + one
    # trailing space.
    (re.compile(r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\s?"), ""),
]

WAVE_RE = re.compile(r"\b[Ww]ave[\s_\-]?\d+[A-Za-z]?\b")


def _is_skip_dir(name: str) -> bool:
    return name in SKIP_DIR_NAMES or name == "build" or name.startswith("build-")


def should_scan(p: Path) -> bool:
    return p.name in SCAN_BASENAMES or p.suffix in SCAN_EXTS


def iter_files(root: Path) -> list[Path]:
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
    for rx, sub in REWRITES:
        line = rx.sub(sub, line)
    # Conservative: only clean up trailing whitespace and double-spaces
    # introduced by deletions WITHIN this line.
    return re.sub(r"  +", " ", line).rstrip()


def rewrite(text: str) -> str:
    out = []
    for ln in text.splitlines():
        if WAVE_RE.search(ln):
            out.append(rewrite_line(ln))
        else:
            out.append(ln)
    return "\n".join(out) + ("\n" if text.endswith("\n") else "")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="write changes back (default: dry-run)")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[2]
    files = iter_files(root)

    fixed_files = 0
    fixed_lines = 0
    remaining: list[tuple[Path, int, str]] = []

    for path in files:
        rel = str(path.relative_to(root))
        if rel in SELF_EXEMPT:
            continue
        try:
            old = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if not WAVE_RE.search(old):
            continue
        new = rewrite(old)
        if new != old:
            old_lines = old.splitlines()
            new_lines = new.splitlines()
            n_changed = sum(1 for a, b in zip(old_lines, new_lines) if a != b)
            n_changed += abs(len(old_lines) - len(new_lines))
            fixed_files += 1
            fixed_lines += n_changed
            if args.apply:
                path.write_text(new, encoding="utf-8")
        # Re-scan for residue after the in-memory rewrite.
        for ln, line in enumerate(new.splitlines(), start=1):
            if "WAVE-OK" in line:
                continue
            if WAVE_RE.search(line):
                remaining.append((path.relative_to(root), ln, line.rstrip()))

    mode = "applied" if args.apply else "dry-run"
    print(f"fix-wave-refs ({mode}): rewrote {fixed_lines} lines across {fixed_files} files.")
    if remaining:
        print(
            f"REMAINING: {len(remaining)} unfixable violations -- needs human edit:",
            file=sys.stderr,
        )
        for rel, ln, line in remaining[:REPORT_MAX_LINES]:
            snippet = line if len(line) <= REPORT_SNIPPET_MAX_LEN else line[:117] + "..."
            print(f"  {rel}:{ln} {snippet}", file=sys.stderr)
        if len(remaining) > REPORT_MAX_LINES:
            print(f"  ... {len(remaining) - REPORT_MAX_LINES} more", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
