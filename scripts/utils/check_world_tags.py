#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_world_tags.py -- enforce TrustZone world tags on Ring 3+ files.

Every file in Ring 3 (HAL) and above carries two tags in its file
header:

    [Ring N / NAME]
    {World: S | NS | NSC}

This script:

    1. Walks libs/ra_hal/, libs/ra_*_pal/, libs/ra_nsc/, libs/third_party/
       (Rings 3-5) and src/ (Ring 1 + Ring 6) and tests/.
    2. For Ring 3+ files (anything outside Ring 1 BSP and Ring 2 Core),
       requires both a [Ring N / ...] tag and a {World: ...} tag in
       the first ~80 lines of the file.
    3. Verifies that any file carrying {World: NSC} lives under
       libs/ra_nsc/ -- NSC veneers may not be defined anywhere else.
    4. Verifies that no file outside libs/ra_nsc/ uses
       __attribute__((cmse_nonsecure_entry)) -- the SG-instruction
       compiler attribute is the only legal way to mark a function
       as a Non-Secure entry point, and the project requires that
       only happen in NSC veneers.

Modes:

    --warn (default) -- exit 0, print findings
    --strict (onward) -- exit 1 on any finding

In there are no Ring 3+ files yet that carry these tags
(the existing 29 driver shells were written before the tag system
was introduced and will be retrofitted starting). The
script therefore EXEMPTS the existing pre-Wave-1 source files via
an allowlist of paths -- those files become non-exempt as the
relevant / sessions touch them.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Iterable


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}

# Files that lived in the tree before the world-tag system was
# introduced (baseline). They are exempt from world-tag
# enforcement until the wave that retrofits them. As soon as a file
# under one of these prefixes gains its [Ring N / NAME] +
# {World: ...} tag pair, it leaves the exemption automatically:
# the script enforces consistency on any file that already carries
# at least one of the two tags.
#
# Practical effect: starts with 0 findings; + adds tags
# incrementally and the script catches any mismatches.
LEGACY_RING3_EXEMPT_PREFIXES = (
    "libs/ra_hal/",
    "src/main.c",
    "tests/",
)

# Header-window size for tag scanning. The tags must appear in the
# first N lines of the file (just inside the file-level Doxygen
# block).
HEADER_LINE_WINDOW = 80

RING_RE = re.compile(r"\[\s*Ring\s+(\d)\s*/\s*([A-Za-z_]+)\s*\]")
WORLD_RE = re.compile(r"\{\s*World\s*:\s*(S|NS|NSC|MIXED)\s*\}")
NSC_ENTRY_RE = re.compile(
    r"__attribute__\s*\(\s*\(\s*cmse_nonsecure_entry\s*\)\s*\)"
)


def is_legacy_exempt(rel_path: str) -> bool:
    return any(rel_path.startswith(p) for p in LEGACY_RING3_EXEMPT_PREFIXES)


def file_is_in_ring1_or_ring2(rel_path: str) -> bool:
    """Files in Ring 1 (BSP) and Ring 2 (Core) skip the {World: ...}
    requirement -- both rings are Secure-only by definition.
    """
    if rel_path.startswith("libs/ra_core/"):
        return True
    if rel_path.startswith("src/boot/"):
        return True
    if rel_path == "src/linker_script.ld":
        return True
    return False


def file_is_in_ring3_plus(rel_path: str) -> bool:
    """Anything under libs/ra_hal/, libs/ra_*_pal/, libs/ra_nsc/,
    libs/third_party/, src/main.c (or src/secure_app/), tests/.
    """
    if rel_path.startswith("libs/ra_hal/"):
        return True
    if rel_path.startswith("libs/ra_") and "_pal/" in rel_path:
        return True
    if rel_path.startswith("libs/ra_nsc/"):
        return True
    if rel_path.startswith("libs/third_party/"):
        return True
    if rel_path.startswith("src/secure_app/"):
        return True
    if rel_path == "src/main.c":
        return True
    if rel_path.startswith("tests/"):
        return True
    return False


def iter_source_files(targets: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    for t in targets:
        if not t.exists():
            continue
        if t.is_file():
            if t.suffix.lower() in SOURCE_SUFFIXES:
                yield t
            continue
        for sub in t.rglob("*"):
            if not sub.is_file():
                continue
            if sub.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            parts = set(sub.parts)
            if "build" in parts or "_deps" in parts:
                continue
            yield sub


def _to_repo_relative(path: pathlib.Path) -> str:
    """Best-effort repo-relative path. Falls back to as-given when
    the file lives outside REPO_ROOT (e.g. in a unit-test sandbox)."""
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def check_file(path: pathlib.Path) -> list[str]:
    findings: list[str] = []
    rel = _to_repo_relative(path)

    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{rel}: read error: {exc}"]

    head_lines = text.splitlines()[:HEADER_LINE_WINDOW]
    head = "\n".join(head_lines)

    ring_match = RING_RE.search(head)
    world_match = WORLD_RE.search(head)

    if file_is_in_ring3_plus(rel):
        # A legacy-exempt file is only exempt while it carries
        # NEITHER tag. As soon as it grows one, both are enforced.
        legacy = is_legacy_exempt(rel)
        has_any_tag = ring_match is not None or world_match is not None
        if (not legacy) or has_any_tag:
            if ring_match is None:
                findings.append(
                    f"{rel}: missing [Ring N / NAME] tag in file header"
                )
            if world_match is None:
                findings.append(
                    f"{rel}: missing {{World: S|NS|NSC}} tag in file header"
                )

    # Check 3: NSC tag must live under libs/ra_nsc/
    if world_match is not None and world_match.group(1) == "NSC":
        if not rel.startswith("libs/ra_nsc/"):
            findings.append(
                f"{rel}: {{World: NSC}} tag is only legal under libs/ra_nsc/"
            )

    # Check 4: cmse_nonsecure_entry only inside libs/ra_nsc/
    for m in NSC_ENTRY_RE.finditer(text):
        if rel.startswith("libs/ra_nsc/"):
            continue
        line_no = text.count("\n", 0, m.start()) + 1
        findings.append(
            f"{rel}:{line_no}: cmse_nonsecure_entry attribute outside libs/ra_nsc/ "
            f"-- NSC veneers must live under that tree"
        )

    # Check 5: Ring 1/2 files must NOT carry a {World: NS} tag --
    # they are Secure by definition. {World: S} is allowed for
    # explicitness; missing tag is the default.
    if file_is_in_ring1_or_ring2(rel) and world_match is not None:
        if world_match.group(1) in ("NS", "NSC"):
            findings.append(
                f"{rel}: Ring 1/2 file carries {{World: {world_match.group(1)}}} "
                f"-- Rings 1 and 2 are Secure-only by policy"
            )

    return findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files or directories to scan (default: libs/ src/ tests/)",
    )
    parser.add_argument(
        "--warn",
        action="store_true",
        help="warn-only mode: print findings, exit 0 (default)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="strict mode: exit 1 on any finding ",
    )
    args = parser.parse_args(argv)

    if args.warn and args.strict:
        print(
            "check_world_tags.py: --warn and --strict are mutually exclusive",
            file=sys.stderr,
        )
        return 2

    strict = args.strict and not args.warn

    if args.paths:
        targets = [pathlib.Path(p) for p in args.paths]
    else:
        targets = [REPO_ROOT / d for d in ("libs", "src", "tests")]

    findings: list[str] = []
    file_count = 0
    for f in iter_source_files(targets):
        file_count += 1
        findings.extend(check_file(f))

    if findings:
        for line in findings:
            print(line, file=sys.stderr)
        verdict = "strict" if strict else "warn"
        print(
            f"check_world_tags.py: {len(findings)} finding(s) "
            f"across {file_count} file(s) [{verdict}]",
            file=sys.stderr,
        )
        return 1 if strict else 0

    print(
        f"check_world_tags.py: 0 findings across {file_count} file(s)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
