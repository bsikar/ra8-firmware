#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ``tests/README.md`` documents exactly the subdirectories that exist.

The README explains what each subdirectory of ``tests/`` is for. A plain prose
README rots the moment someone adds ``tests/newthing/`` and forgets to describe
it, or deletes ``tests/oldthing/`` and leaves a paragraph describing a directory
that is gone -- and nothing notices. This gate makes that impossible in both
directions:

1. **Every immediate subdirectory of ``tests/`` is described.** A new one that
   the README does not name fails the gate, so it cannot be added silently.

2. **Every subdirectory the README names still exists.** A row describing a
   directory that has been removed or renamed fails the gate, so a stale entry
   cannot linger.

The README is machine-read the same way it is human-read: only the FIRST cell
of each Markdown table row counts, written as a code span with a trailing slash
(```bench/```). Keying on the first column -- never the prose, never a
description cell -- is what lets a description mention ``epub/real/`` without the
checker mistaking ``epub`` for a top-level subdirectory of ``tests/``.

Like every other detector in this tree it carries a **non-vacuity floor**: the
real ``tests/`` has eight subdirectories, so a scan that finds almost none did
not walk the tree it meant to, and reporting "no drift" against nothing would be
the exact silent-pass failure this gate exists to prevent.

``--selftest`` runs first in the gate. It builds throwaway ``tests/`` trees and
asserts an undocumented subdirectory fires, a stale README entry fires, an
in-sync tree stays quiet, and a collapsed scan is caught by the floor. Without
it, "0 problems" is indistinguishable from "checked nothing".
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests"
README = TESTS_DIR / "README.md"

# A documented subdirectory is the first cell of a table row, written as a code
# span ending in a slash: `name/`. Reading only the first column is deliberate
# -- a name that appears in a row's description or in prose is never counted.
ROW_NAME_RE = re.compile(r"^`([A-Za-z0-9._-]+)/`$")

# The real tree carries eight subdirectories. A scan that finds far fewer walked
# the wrong root or a tree that never checked out; refuse to certify it clean.
MIN_SUBDIRS = 5

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_VACUOUS = 2


def immediate_subdirs(tests_dir: Path) -> set[str]:
    """Return the names of the directories directly under `tests_dir`.

    Args:
        tests_dir: The ``tests/`` directory to scan.

    Returns:
        Every immediate child directory name, excluding dot-directories, which
        are tooling rather than test content.
    """
    return {p.name for p in tests_dir.iterdir() if p.is_dir() and not p.name.startswith(".")}


def documented_subdirs(readme_text: str) -> set[str]:
    """Return the subdirectory names the README's table claims to describe.

    Only the first cell of each table row is read, so a name appearing in a
    row's description or in surrounding prose is never mistaken for a documented
    top-level subdirectory.

    Args:
        readme_text: The full contents of ``tests/README.md``.

    Returns:
        Every ``name`` written as ```name/``` in a table's first column.
    """
    names: set[str] = set()
    for line in readme_text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        first_cell = stripped.strip("|").split("|", 1)[0].strip()
        match = ROW_NAME_RE.match(first_cell)
        if match:
            names.add(match.group(1))
    return names


def drift_problems(actual: set[str], documented: set[str]) -> list[str]:
    """Return every way the tree and the README disagree.

    Args:
        actual: Subdirectory names that exist under ``tests/``.
        documented: Subdirectory names the README describes.

    Returns:
        One message per undocumented subdirectory and per documented-but-absent
        entry; empty when the two sets are equal.
    """
    problems = [
        f"tests/{name}/ exists but is not documented in tests/README.md -- "
        "add a table row whose first cell is `" + name + "/`"
        for name in sorted(actual - documented)
    ]
    problems += [
        f"tests/README.md documents tests/{name}/ but no such subdirectory "
        "exists -- remove or rename that row"
        for name in sorted(documented - actual)
    ]
    return problems


def evaluate(
    tests_dir: Path, readme: Path, min_subdirs: int = MIN_SUBDIRS
) -> tuple[int, list[str]]:
    """Compare a ``tests/`` tree against its README.

    Args:
        tests_dir: The ``tests/`` directory to scan.
        readme: The README that must describe every subdirectory.
        min_subdirs: Non-vacuity floor; a scan below it is treated as collapsed.

    Returns:
        An ``(exit_code, messages)`` pair: ``EXIT_VACUOUS`` when fewer than
        ``min_subdirs`` directories were found, ``EXIT_DRIFT`` on any
        disagreement, ``EXIT_OK`` when the README matches the tree.
    """
    actual = immediate_subdirs(tests_dir)
    if len(actual) < min_subdirs:
        return EXIT_VACUOUS, [
            f"only {len(actual)} subdirectory(ies) found under {tests_dir} "
            f"(floor is {min_subdirs}); the scan collapsed rather than the tree"
        ]
    readme_text = readme.read_text(encoding="utf-8") if readme.exists() else ""
    problems = drift_problems(actual, documented_subdirs(readme_text))
    return (EXIT_DRIFT if problems else EXIT_OK), problems


@dataclass(frozen=True)
class _Case:
    """One selftest fixture and the verdict it must produce."""

    name: str
    subdirs: list[str]
    documented: list[str]
    floor: int
    want_code: int
    needle: str | None


def _selftest_cases() -> list[_Case]:
    """Return the fixtures the selftest asserts, one per direction.

    Returns:
        A case for the in-sync tree, the undocumented-subdir direction, the
        stale-entry direction, and the non-vacuity floor. ``needle`` is a
        substring one message must contain, or ``None`` when the case must
        produce no messages.
    """
    trio = ["alpha", "beta", "gamma"]
    return [
        _Case("in-sync stays quiet", trio, trio, 3, EXIT_OK, None),
        _Case("undocumented subdir fires", trio, ["alpha", "beta"], 3, EXIT_DRIFT, "tests/gamma/"),
        _Case("stale doc entry fires", trio, [*trio, "ghost"], 3, EXIT_DRIFT, "ghost"),
        _Case("collapsed scan is vacuous", ["alpha"], ["alpha"], 3, EXIT_VACUOUS, None),
    ]


def _write_fixture(root: Path, case: _Case) -> tuple[Path, Path]:
    """Build a throwaway ``tests/`` tree and README for one selftest case.

    Args:
        root: Temporary directory to build inside.
        case: The fixture to materialise.

    Returns:
        The ``(tests_dir, readme_path)`` pair to hand to :func:`evaluate`.
    """
    tests_dir = root / "tests"
    tests_dir.mkdir()
    for name in case.subdirs:
        (tests_dir / name).mkdir()
    rows = "".join(f"| `{name}/` | fixture description |\n" for name in case.documented)
    readme = tests_dir / "README.md"
    readme.write_text(
        "# tests/\n\n| Subdirectory | What it holds |\n|---|---|\n" + rows,
        encoding="utf-8",
    )
    return tests_dir, readme


def _run_case(root: Path, case: _Case) -> list[str]:
    """Run one selftest case and return the ways it misbehaved.

    Args:
        root: Fresh temporary directory for this case.
        case: The fixture and its expected verdict.

    Returns:
        A message per assertion the case failed; empty when it behaved.
    """
    tests_dir, readme = _write_fixture(root, case)
    code, messages = evaluate(tests_dir, readme, min_subdirs=case.floor)
    failures = []
    if code != case.want_code:
        failures.append(f"  {case.name}: exit {code}, expected {case.want_code}")
    if case.needle is None:
        if case.want_code == EXIT_OK and messages:
            failures.append(f"  {case.name}: expected no messages, got {messages}")
    elif not any(case.needle in message for message in messages):
        failures.append(f"  {case.name}: no message mentioned '{case.needle}': {messages}")
    return failures


def _selftest() -> int:
    """Prove both drift directions fire, a clean tree stays quiet, the floor holds.

    Returns:
        0 when every fixture behaves, 1 otherwise.
    """
    cases = _selftest_cases()
    failures: list[str] = []
    for case in cases:
        with tempfile.TemporaryDirectory() as tmp:
            failures += _run_case(Path(tmp), case)
    if failures:
        print("check_tests_readme selftest FAILED:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"selftest OK: {len(cases)} cases (both drift directions + non-vacuity floor)")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point.

    Args:
        argv: Command line, defaulting to ``sys.argv[1:]``.

    Returns:
        0 when the README matches the tree, 1 on drift, 2 on a collapsed scan.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true", help="prove both drift directions fire")
    args = parser.parse_args(argv)
    if args.selftest:
        return _selftest()
    code, messages = evaluate(TESTS_DIR, README)
    if code == EXIT_OK:
        count = len(immediate_subdirs(TESTS_DIR))
        print(f"tests/README.md OK: {count} subdirectory(ies) documented, none stale")
        return EXIT_OK
    label = "collapsed scan" if code == EXIT_VACUOUS else "drift"
    print(f"tests/README.md {label}: {len(messages)} problem(s):", file=sys.stderr)
    for message in messages:
        print(f"  {message}", file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
