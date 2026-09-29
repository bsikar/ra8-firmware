#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Resolve every repository-rooted path a CMake file names against the tree.

WHY THIS EXISTS
===============
A library source can be attached to a target by PATH rather than through the
library's own build unit::

    target_sources(
      ereader_m33_cpu1.elf
      PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_text.c
    )

Nothing checked that the path on the right still names a file.  #1290 is the
worked example: the Zig port deleted ``libs/ra8_gfx/src/ra8_gfx_text.c`` and
that ``target_sources()`` line kept naming it.  Two independent reasons no
existing gate saw it, and both recur for every library the migration touches:

  * ``ra8_add_cpu1_image()`` returns early when the build is not a cross
    build, so a HOST configure never evaluates the block at all.  Only an ARM
    configure of that one ``hw_pending`` example would have failed, and no
    such configure runs on every change.
  * The greps that guard a port look for ``#include`` of a ``.c`` under
    ``tests/``, ``examples/`` and ``apps/``.  A CMake path is neither an
    include nor a member of the glob ``library_sources.cmake`` maintains, so
    it is invisible to both.

This gate is the missing half.  It is a pure text resolve over the CMake
files themselves, so it costs nothing and needs no configure, no toolchain
and no cross build to catch a path that has gone stale.

WHAT IT ENFORCES, PRECISELY
---------------------------
  * A ``${RA8_REPO_ROOT}/<path>`` token with no variable and no wildcard must
    resolve to a file or directory that exists.  A dangling one FAILS.
  * A token carrying a wildcard is a glob and must match at least one path.
    A glob matching nothing FAILS: that is how a whole directory disappears
    from a build without a single line of CMake changing.
  * A token still carrying a ``${...}`` after the prefix is NOT judged.  Its
    value depends on configure-time state this gate deliberately does not
    model (the selected board, the app name).  Those are counted and the
    count is reported, so the number is visible rather than silently zero.
  * A comment line is skipped entirely.  Prose inside a CMake comment names
    paths for the reader, including paths a change is about to remove, and
    holding prose to a resolve is the mistake that makes a gate get disabled.
  * Vendored trees are out of scope: their build files are upstream's and are
    not ours to hold to this.

SCOPE, HONESTLY
---------------
This resolves ``${RA8_REPO_ROOT}``-rooted paths and nothing else.  A relative
path, a path built from ``CMAKE_CURRENT_SOURCE_DIR``, or one assembled across
two lines is not seen.  That is not a claim those forms are safe; it is the
boundary of what a single-line text resolve can check without pretending to
be CMake.  ``${RA8_REPO_ROOT}`` is the form the tree actually uses to reach
across build units, which is exactly where the stale-path risk lives.

A vacuity guard fails the gate closed if the scan stops finding tokens: a
parser that has quietly stopped matching would otherwise report a clean tree
forever.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: The only prefix this gate resolves.  See SCOPE, HONESTLY.
ROOT_VARIABLE = "RA8_REPO_ROOT"

#: A token runs to the first character that cannot be part of a CMake path
#: argument.  ``)`` is stripped afterwards so a path closing a command reads
#: correctly.
TOKEN_PATTERN = re.compile(r"\$\{" + ROOT_VARIABLE + r"\}/([^\s\"')]+)")

#: Path fragments whose build files belong to upstream.
VENDOR_MARKERS = ("third_party/", "/build/")

#: Below this the parse is assumed broken rather than the tree clean.
MIN_RESOLVED_TOKENS = 120


@dataclass(frozen=True)
class Finding:
    """One CMake path that does not resolve."""

    path: str
    line: int
    token: str
    reason: str

    def render(self) -> str:
        """Return the one-line human form."""
        return f"{self.path}:{self.line}: {self.reason}: ${{{ROOT_VARIABLE}}}/{self.token}"


def is_scannable(relative: str) -> bool:
    """Return True when this CMake file is ours to hold to the resolve."""
    if relative.startswith("build/"):
        return False
    return not any(marker in relative for marker in VENDOR_MARKERS)


def cmake_files(root: Path) -> list[Path]:
    """Return every first-party CMake file, sorted for a stable report."""
    found: list[Path] = []
    for pattern in ("**/CMakeLists.txt", "**/*.cmake"):
        for path in root.glob(pattern):
            if path.is_file() and is_scannable(path.relative_to(root).as_posix()):
                found.append(path)
    return sorted(set(found))


def classify(token: str) -> str:
    """Return 'variable', 'glob' or 'literal' for one path token."""
    if "$" in token:
        return "variable"
    if "*" in token or "?" in token or "[" in token:
        return "glob"
    return "literal"


def scan_file(root: Path, path: Path) -> tuple[list[Finding], dict[str, int]]:
    """Resolve every token in one CMake file."""
    counts = {"literal": 0, "glob": 0, "variable": 0}
    findings: list[Finding] = []
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return findings, counts

    for number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        for match in TOKEN_PATTERN.finditer(line):
            token = match.group(1).rstrip(")")
            kind = classify(token)
            counts[kind] += 1
            if kind == "variable":
                continue
            if kind == "glob":
                if not list(root.glob(token)):
                    findings.append(Finding(relative, number, token, "glob matches nothing"))
                continue
            if not (root / token).exists():
                findings.append(Finding(relative, number, token, "path does not exist"))
    return findings, counts


def check_tree(root: Path) -> tuple[list[Finding], dict[str, int]]:
    """Resolve the whole tree and return the findings plus the census."""
    findings: list[Finding] = []
    counts = {"literal": 0, "glob": 0, "variable": 0, "files": 0}
    for path in cmake_files(root):
        counts["files"] += 1
        file_findings, file_counts = scan_file(root, path)
        findings.extend(file_findings)
        for key, value in file_counts.items():
            counts[key] += value
    return findings, counts


def selftest(tmp_root: Path) -> list[str]:
    """Prove the detector fires and stays quiet, in both directions."""
    failures: list[str] = []
    real = tmp_root / "libs" / "ra8_gfx" / "src"
    real.mkdir(parents=True)
    (real / "ra8_gfx_blit.c").write_text("/* real */\n")
    (tmp_root / "libs" / "ra8_gfx" / "inc").mkdir()

    cases: dict[str, tuple[str, int]] = {
        # body                                                    -> (label, expected findings)
        "target_sources(a PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_blit.c)\n": (
            "a literal path that exists is clean",
            0,
        ),
        "target_sources(a PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_text.c)\n": (
            "a literal path that was deleted fires",
            1,
        ),
        "target_include_directories(a PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/inc)\n": (
            "a directory that exists is clean",
            0,
        ),
        "target_include_directories(a PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gone/inc)\n": (
            "a directory that does not exist fires",
            1,
        ),
        "file(GLOB s ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_*.c)\n": (
            "a glob with a match is clean",
            0,
        ),
        "file(GLOB s ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_nothing_*.c)\n": (
            "a glob matching nothing fires",
            1,
        ),
        'set(d "${RA8_REPO_ROOT}/libs/ra8_board_${BOARD}")\n': (
            "a variable-interpolated path is not judged",
            0,
        ),
        "# ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_text.c was deleted by #1277\n": (
            "prose in a comment is not judged",
            0,
        ),
        "target_sources(a PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_blit.c\n": (
            "an unterminated command still resolves its path",
            0,
        ),
    }

    for body, (label, expected) in cases.items():
        listing = tmp_root / "CMakeLists.txt"
        listing.write_text(body)
        findings, _ = scan_file(tmp_root, listing)
        if len(findings) != expected:
            failures.append(f"{label}: expected {expected} finding(s), got {len(findings)}")

    listing = tmp_root / "CMakeLists.txt"
    listing.unlink()
    vendored = tmp_root / "libs" / "third_party" / "zlib"
    vendored.mkdir(parents=True)
    (vendored / "CMakeLists.txt").write_text(
        "target_sources(z PRIVATE ${RA8_REPO_ROOT}/libs/ra8_gone/src/gone.c)\n"
    )
    findings, counts = check_tree(tmp_root)
    if findings:
        failures.append(f"a vendored build file is out of scope: got {len(findings)} finding(s)")
    if counts["files"]:
        failures.append(f"a vendored build file was scanned: {counts['files']} file(s)")
    return failures


def run_selftest() -> int:
    """Run the selftest in a throwaway tree."""
    import tempfile

    with tempfile.TemporaryDirectory() as raw:
        failures = selftest(Path(raw))
    if failures:
        print("check_cmake_source_paths.py: SELFTEST FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("selftest: check_cmake_source_paths.py OK (9 both-direction cases)")
    return 0


def main(argv: list[str]) -> int:
    """Resolve the tree, or prove the detector, and report."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--selftest", action="store_true", help="prove the detector instead of scanning"
    )
    args = parser.parse_args(argv[1:])
    if args.selftest:
        return run_selftest()

    findings, counts = check_tree(REPO_ROOT)
    resolved = counts["literal"] + counts["glob"]
    if resolved < MIN_RESOLVED_TOKENS:
        print(
            f"check_cmake_source_paths.py: FATAL -- resolved only {resolved} "
            f"${{{ROOT_VARIABLE}}} path(s) across {counts['files']} CMake file(s), "
            f"below the {MIN_RESOLVED_TOKENS} this tree carries. The scan is broken, "
            "not the tree clean.",
            file=sys.stderr,
        )
        return 2

    if findings:
        print(f"\n{len(findings)} unresolved CMake path(s):\n", file=sys.stderr)
        for finding in sorted(findings, key=lambda item: (item.path, item.line)):
            print(f"  {finding.render()}", file=sys.stderr)
        print(
            "\nEvery repository-rooted path a CMake file names must resolve. A source "
            "moved or deleted by a port leaves the path behind, and no host configure "
            "evaluates a cross-only block (#1290). Update the path, or drop the line.",
            file=sys.stderr,
        )
        return 1

    print(
        f"check_cmake_source_paths.py: {counts['files']} CMake file(s); "
        f"{counts['literal']} literal path(s) and {counts['glob']} glob(s) all resolve; "
        f"{counts['variable']} variable-interpolated path(s) not judged."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
