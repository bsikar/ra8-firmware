#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_obsolete_standards.py -- Reject references to obsolete safety standards.

Per the project's IEC 61508 SIL 3 / DO-178C target (CLAUDE.md):

- DO-178B was superseded by DO-178C in December 2011 and is no longer
  recognized by FAA / EASA / Transport Canada. Any new doc, test
  comment, or commit message that says "DO-178B" must be corrected to
  DO-178C (or to IEC 61508 SIL 3 if industry-neutral).

Two scan modes, and NEITHER is the default -- a bare invocation is an error.

    check_obsolete_standards.py --all       # the whole tracked tree (CI)
    check_obsolete_standards.py --staged    # the git index (the commit hook)
    check_obsolete_standards.py --selftest  # prove the detector both ways

That is deliberate. This checker read ``git diff --cached`` unconditionally,
and the ``pre-commit-checks`` CI gate invoked it with no arguments -- where
nothing is ever staged. It therefore scanned ZERO files on every CI run and
printed ``0 findings``, for as long as it had been wired there. The same
defect was found and fixed in ``check_mcdc_block.py`` (#325) and
``check_new_compound_has_mcdc.py`` (#355); this is the third instance, so the
remedy is the one those adopted: make the mode explicit so a caller cannot
silently get the vacuous one, and refuse an empty tree-wide scan outright.

Whitelists the project's own self-documentation (CLAUDE.md note, this script,
the migration memo) where the tokens appear in *explanatory* context.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import first_party_paths  # needs the sys.path line above
from selftest_assert import expect, report  # needs the sys.path line above

# Tokens that must not appear in new content. Match case-sensitively to
# avoid false positives on unrelated identifiers.
FORBIDDEN_PATTERNS = [
    re.compile(r"\bDO-178B\b"),
    re.compile(r"\bDO178B\b"),
]

# Files where the forbidden token may legitimately appear because they
# are *the* file documenting that the token is forbidden. Add new
# entries with care.
WHITELIST = {
    "CLAUDE.md",
    "scripts/checks/check_obsolete_standards.py",
    "scripts/git/pre-commit",
    "docs/MCDC.md",
    # Historical/explanatory reference: SQLite's test harness targeted
    # DO-178B, framed in-text as the direct ancestor of this project's
    # DO-178C target -- not a claim that anything here targets the
    # superseded bar.
    "docs/PHILOSOPHIES.md",
}


# Suffixes the tree-wide sweep enumerates. Mirrors scannable() below, which
# still filters the per-file decisions for both modes.
SCAN_SUFFIXES: tuple[str, ...] = (
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".md",
    ".py",
    ".sh",
    ".cmake",
    ".yml",
    ".yaml",
    ".txt",
)

# A tree-wide sweep that matched almost nothing has not found a clean tree; it
# has lost its file list. Measured at 2600+ tracked files in these suffixes.
TREE_FLOOR = 500


def staged_files() -> list[str]:
    """Return the list of files staged for the in-progress commit."""
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],  # noqa: S607  # trusted: git is a fixed dev-tool name
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def tracked_files() -> list[str]:
    """Return every tracked first-party path this checker scans tree-wide.

    Enumeration goes through ``lint_targets.first_party_paths`` -- the shared
    derived-scope primitive -- rather than a hand-written directory list, so a
    new top-level directory is covered the day it lands and cannot quietly
    fall out of scope the way a hardcoded tuple does.
    """
    named = ["Makefile", "CMakeLists.txt"]
    paths = list(first_party_paths(SCAN_SUFFIXES))
    paths.extend(path for path in first_party_paths(tuple(named)) if Path(path).name in set(named))
    return sorted(set(paths))


def scannable(path: Path) -> bool:
    """Decide whether a staged path is subject to the obsolete-standard scan.

    Three subtractions, in order of cost: a path that no longer exists (staged
    as a deletion, so there is nothing to read), an explicitly whitelisted
    file, and anything under a ``third_party`` directory -- vendored code may
    cite whatever standard it was written against and is not ours to correct.

    Suffix is matched case-insensitively, so a ``.MD`` or ``.YAML`` cannot
    duck the scan on spelling alone.
    """
    if not path.is_file():
        return False
    if str(path) in WHITELIST:
        return False
    if any(part == "third_party" for part in path.parts):
        return False
    suffix = path.suffix.lower()
    if suffix in {
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".md",
        ".py",
        ".sh",
        ".cmake",
        ".yml",
        ".yaml",
        ".txt",
    }:
        return True
    return path.name in {"Makefile", "CMakeLists.txt"}


def scan_text(text: str) -> list[tuple[int, str]]:
    """Return ``(lineno, line)`` for each line citing an obsolete standard.

    Only the first matching pattern per line is recorded, so a line naming two
    obsolete standards is reported once -- the fix is to rewrite the line, and
    a second finding on it would just be noise.
    """
    hits: list[tuple[int, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for pat in FORBIDDEN_PATTERNS:
            if pat.search(line):
                hits.append((lineno, line.rstrip()))
                break
    return hits


def scan_paths(names: list[str]) -> list[tuple[str, int, str]]:
    """Scan the given paths, returning every ``(path, lineno, line)`` finding."""
    findings: list[tuple[str, int, str]] = []
    for name in names:
        path = Path(name)
        if not scannable(path):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        findings.extend((name, lineno, line) for lineno, line in scan_text(text))
    return findings


def selftest() -> int:
    """Prove the detector fires on a citation and stays quiet without one.

    Also asserts the tree-wide enumeration is not empty. That last assertion
    is the point of this selftest: the scan was silently reduced to zero files
    for its whole life in CI, and a detector nobody has watched fire on a real
    file list is indistinguishable from one that has stopped looking.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    for label, text, must_fire in (
        ("a DO-178B citation", "/* Written to DO-178B Level B. */", True),
        ("the hyphen-less DO178B spelling", "# targets DO178B objectives", True),
        ("the current DO-178C citation", "/* Written to DO-178C Level B. */", False),
        ("an unrelated line naming no standard", "int x = 178;", False),
    ):
        fired = bool(scan_text(text))
        expectation = "must fire" if must_fire else "must stay quiet"
        expect(fired == must_fire, f"{label} ({expectation})", failures)

    tracked = tracked_files()
    expect(
        len(tracked) >= TREE_FLOOR,
        f"tree-wide enumeration sees {len(tracked)} file(s) (floor {TREE_FLOOR})",
        failures,
    )
    return report(failures)


def main() -> int:
    """Fail any file citing a superseded safety standard (e.g. DO-178B).

    The scan mode is mandatory rather than defaulted. ``--all`` sweeps the
    tracked tree and is what CI runs; ``--staged`` reads the git index and is
    what the commit hook runs. A bare invocation used to mean "staged", which
    made the CI gate scan nothing at all and report a clean tree forever.

    Returns:
        0 when the selected set is clean, 1 with each offending line quoted,
        and 2 when no mode was given or a tree-wide sweep found too few files
        to be believable.
    """
    parser = argparse.ArgumentParser(description="ban references to superseded safety standards")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--all", action="store_true", help="scan every tracked first-party file")
    mode.add_argument("--staged", action="store_true", help="scan the git index (commit hook)")
    parser.add_argument("--selftest", action="store_true", help="prove the detector both ways")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if not (args.all or args.staged):
        sys.stderr.write(
            "check_obsolete_standards.py: pass --all (CI, the whole tracked tree) or\n"
            "  --staged (the pre-commit hook, the git index). There is no default:\n"
            "  this checker silently defaulted to --staged and so scanned nothing at\n"
            "  all in CI, where the index is always empty.\n"
        )
        return 2

    names = tracked_files() if args.all else staged_files()
    if args.all and len(names) < TREE_FLOOR:
        sys.stderr.write(
            f"check_obsolete_standards.py: FATAL -- the tree-wide sweep enumerated only\n"
            f"  {len(names)} file(s), below the floor of {TREE_FLOOR}. An empty or\n"
            f"  collapsed file list reports success because it saw nothing.\n"
        )
        return 2

    findings = scan_paths(names)
    if findings:
        print("[FAIL] Obsolete standard reference detected (DO-178B was")
        print("       superseded by DO-178C in December 2011). Use")
        print("       DO-178C, IEC 61508 SIL 3, or ISO 26262 ASIL C/D")
        print("       per CLAUDE.md. Offending lines:")
        for name, lineno, line in findings:
            print(f"  {name}:{lineno}: {line}")
        return 1

    print(f"check_obsolete_standards.py: 0 findings across {len(names)} file(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
