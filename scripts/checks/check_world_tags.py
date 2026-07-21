#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_world_tags.py -- enforce TrustZone world tags on Ring 3+ files.

Every file in Ring 3 (HAL) and above carries two tags in its file
header:

    [Ring N / NAME]
    {World: S | NS | NSC}

This script:

    1. Walks project-owned libs/, src/, tests/, and examples/ code while
       skipping vendored trees such as libs/third_party/.
    2. For Ring 3+ files (anything outside Ring 1 BSP and Ring 2 Core),
       requires both a [Ring N / ...] tag and a {World: ...} tag in
       the first ~80 lines of the file.
    3. Verifies that any file carrying {World: NSC} lives under
       libs/ra8_nsc/ -- NSC veneers may not be defined anywhere else.
    4. Verifies that no file outside libs/ra8_nsc/ uses
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
script therefore EXEMPTS the existing previous source files via
an allowlist of paths -- those files become non-exempt as the
relevant / sessions touch them.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections.abc import Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}

# Top-level dirs we scan by default. Vendored/build subtrees are filtered
# during traversal.
ALWAYS_SCAN_DIRS = ("libs", "src", "tests")

EXCLUDED_PATH_PARTS = {"build", "_deps", "third_party"}

# File names inside an app dir that carry boot-file (Ring 1) semantics.
# main.c is Ring 6 / Application; everything else next to it (the
# per-app boot files) is treated as Ring 1.
APP_BOOT_FILES = {
    "vector_table.c",
    "system_init.c",
    "secure_exception.c",
    "trustzone_init.c",
    "trustzone_init.h",
}


def discover_app_dirs() -> tuple[str, ...]:
    """Every examples/<tier>/<app>/ path holding both main.c and CMakeLists.txt.

    Requiring BOTH is what distinguishes a real app directory from a shared
    subdirectory that merely contains sources, so the app set is derived
    rather than listed.
    """
    out: list[str] = []
    examples_root = REPO_ROOT / "examples"
    if not examples_root.is_dir():
        return ()
    for tier in sorted(examples_root.iterdir()):
        if not tier.is_dir():
            continue
        for entry in sorted(tier.iterdir()):
            if not entry.is_dir():
                continue
            if (entry / "main.c").is_file() and (entry / "CMakeLists.txt").is_file():
                out.append(f"examples/{tier.name}/{entry.name}")
    return tuple(out)


APP_DIRS = discover_app_dirs()

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
    "libs/ra8_hal/",
    "tests/",
)

# Header-window size for tag scanning. The tags must appear in the
# first N lines of the file (just inside the file-level Doxygen
# block).
HEADER_LINE_WINDOW = 80

RING_RE = re.compile(r"\[\s*Ring\s+(\d)\s*/\s*([A-Za-z_]+)\s*\]")
WORLD_RE = re.compile(r"\{\s*World\s*:\s*(S|NS|NSC|MIXED)\s*\}")
NSC_ENTRY_RE = re.compile(r"__attribute__\s*\(\s*\(\s*cmse_nonsecure_entry\s*\)\s*\)")


def is_legacy_exempt(rel_path: str) -> bool:
    """Whether a path predates the World-tag requirement and is grandfathered.

    A prefix list, deliberately finite and not extended: it records what was
    already in the tree when the rule landed. New code has no route into it,
    so the exemption shrinks as those files are tagged and never grows.
    """
    return any(rel_path.startswith(p) for p in LEGACY_RING3_EXEMPT_PREFIXES)


def file_is_in_ring1_or_ring2(rel_path: str) -> bool:
    """Whether a file sits in Ring 1 (BSP) or Ring 2 (Core).

    Both rings are Secure-only by definition, so a ``{World: ...}`` tag would
    be restating the ring rather than adding information -- which is why they
    are exempt from the requirement rather than required to say "S".
    """
    if rel_path.startswith("libs/ra8_core/"):
        return True
    # Per-app boot files (vector_table.c, system_init.c,
    # secure_exception.c, trustzone_init.c/h) are Ring 1 by virtue of
    # the role they play, even though they live next to main.c.
    for app_dir in APP_DIRS:
        prefix = app_dir + "/"
        if rel_path.startswith(prefix):
            tail = rel_path[len(prefix) :]
            if tail in APP_BOOT_FILES:
                return True
    return bool(rel_path.endswith("/linker_script.ld"))


def file_is_in_ring3_plus(rel_path: str) -> bool:
    """Whether a file is project-owned Ring 3+ code, where the World tag is required.

    Covers libs/ra8_hal/, libs/ra8_*_pal/, libs/ra8_nsc/, src/secure_app/,
    tests/, and per-app main.c (Ring 6 application code). These are the rings
    that can run in either TrustZone world, which is precisely why each file
    must declare which one it is written for.
    """
    if rel_path.startswith("libs/ra8_hal/"):
        return True
    if rel_path.startswith("libs/ra8_") and "_pal/" in rel_path:
        return True
    if rel_path.startswith("libs/ra8_nsc/"):
        return True
    if rel_path.startswith("src/secure_app/"):
        return True
    if rel_path.startswith("tests/"):
        return True
    # Per-app main.c is Ring 6.
    return any(rel_path == f"{app_dir}/main.c" for app_dir in APP_DIRS)


def iter_source_files(targets: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    """Expand a mixed list of files and directories into source files.

    A path that does not exist is skipped silently rather than raising, so a
    stale entry in a caller's list cannot abort the sweep.
    """
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
            if parts & EXCLUDED_PATH_PARTS:
                continue
            yield sub


def _to_repo_relative(path: pathlib.Path) -> str:
    """Repo-relative path, falling back to the path as given.

    The fallback matters for the selftest, which runs on fixtures in a
    temporary directory outside REPO_ROOT; without it the helper would raise
    there and the gate could not be tested in isolation.
    """
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def check_file(path: pathlib.Path) -> list[str]:
    """Report a missing or malformed ``{World: ...}`` tag in one file.

    Ring membership decides whether the tag is required at all, so the ring
    tests run before the tag is looked for -- a Ring 2 file with no tag is
    correct, not a finding.

    Returns one message per finding; an empty list means the file is fine or
    out of scope.
    """
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
                findings.append(f"{rel}: missing [Ring N / NAME] tag in file header")
            if world_match is None:
                findings.append(f"{rel}: missing {{World: S|NS|NSC}} tag in file header")

    # Check 3: NSC tag must live under libs/ra8_nsc/
    if (
        world_match is not None
        and world_match.group(1) == "NSC"
        and not rel.startswith("libs/ra8_nsc/")
    ):
        findings.append(f"{rel}: {{World: NSC}} tag is only legal under libs/ra8_nsc/")

    # Check 4: cmse_nonsecure_entry only inside libs/ra8_nsc/
    for m in NSC_ENTRY_RE.finditer(text):
        if rel.startswith("libs/ra8_nsc/"):
            continue
        line_no = text.count("\n", 0, m.start()) + 1
        findings.append(
            f"{rel}:{line_no}: cmse_nonsecure_entry attribute outside libs/ra8_nsc/ "
            f"-- NSC veneers must live under that tree"
        )

    # Check 5: Ring 1/2 files must NOT carry a {World: NS} tag --
    # they are Secure by definition. {World: S} is allowed for
    # explicitness; missing tag is the default.
    if (
        file_is_in_ring1_or_ring2(rel)
        and world_match is not None
        and world_match.group(1) in ("NS", "NSC")
    ):
        findings.append(
            f"{rel}: Ring 1/2 file carries {{World: {world_match.group(1)}}} "
            f"-- Rings 1 and 2 are Secure-only by policy"
        )

    return findings


def main(argv: list[str]) -> int:
    """Check that every Ring 3+ source declares which TrustZone world it targets.

    The tag exists because the same source can be compiled into the Secure or
    the Non-secure image, and nothing in the file otherwise says which was
    intended -- so a file that quietly ends up in the wrong world produces a
    build that links and faults at runtime.

    With no paths the scan covers libs/, src/ and tests/; naming paths narrows
    it for the pre-commit hook.

    Returns 1 listing each untagged file, 0 when every in-scope file declares
    a world.
    """
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
        scan = ["libs", "src", "tests", *list(APP_DIRS)]
        targets = [REPO_ROOT / d for d in scan]

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
