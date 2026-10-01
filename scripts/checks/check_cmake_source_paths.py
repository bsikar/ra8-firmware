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
  * A ``${<root>}/<path>`` token with no variable and no wildcard must
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

WHICH ROOTS ARE RESOLVED
------------------------
``${RA8_REPO_ROOT}`` is the tree-wide spelling and is always the repository
root.  It is not the only one: this tree reaches across build units through
six more names, each defined by the listfile that uses it, and #2610 is what
resolving only the first one cost.  ``libs/ra8_num/src/ra8_num_decimal.c``
went Zig and left five dangling paths in ``apps/shared_libs/mdl``, spelled
``${MDL_REPO_ROOT}/...``.  This gate scanned that very file and reported it
clean, because the prefix was not the one name it knew.

So a root is also resolved when the SAME listfile defines it from its own
location::

    get_filename_component(FW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

``CMAKE_CURRENT_SOURCE_DIR`` is honoured only in a ``CMakeLists.txt``, where
the directory scope is that file's own directory.  In an included ``.cmake``
it is the INCLUDING directory, which is configure-time state, so a root
defined that way is not resolved: that is why the eleven ``tests/cmake``
files using ``${FW_ROOT}`` are left alone, since ``FW_ROOT`` is defined for
them in ``library_sources.cmake`` against whoever includes it.
``CMAKE_CURRENT_LIST_DIR`` is the listfile's own directory in either kind of
file, so it is honoured in both.

A name defined twice in one file with two different values is dropped rather
than guessed at.

SCOPE, HONESTLY
---------------
A relative path, a root defined in another file, or a path assembled across
two lines is not seen.  That is not a claim those forms are safe; it is the
boundary of what a single-line text resolve can check without pretending to
be CMake.

A vacuity guard fails the gate closed if the scan stops finding tokens: a
parser that has quietly stopped matching would otherwise report a clean tree
forever.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Always the repository root, tree-wide.  See WHICH ROOTS ARE RESOLVED.
ROOT_VARIABLE = "RA8_REPO_ROOT"

#: Trailing characters that close a path argument rather than belong to it:
#: ``)`` ends the command, ``\\`` is the string escape that ``string(CONCAT ...)``
#: puts straight after a path.  Stripped before the path is resolved.
PATH_ARGUMENT_TRAILERS = ")\\"

#: A listfile may define its own root from its own location.  Captured:
#: the name, the anchor variable, and the relative suffix (if any).
ROOT_DEFINITION = re.compile(
    r"^\s*(?:set|get_filename_component)\(\s*"
    r"([A-Za-z][A-Za-z0-9_]*)\s+"
    r"\"?\$\{(CMAKE_CURRENT_SOURCE_DIR|CMAKE_CURRENT_LIST_DIR)\}"
    r"(/[^\s\"')]*)?\"?"
)


def token_pattern(names: list[str]) -> re.Pattern[str]:
    """Return the token matcher for one file's set of root names."""
    alternatives = "|".join(sorted(names, key=len, reverse=True))
    return re.compile(r"\$\{(" + alternatives + r")\}/([^\s\"')]+)")


#: Path fragments whose build files belong to upstream.
VENDOR_MARKERS = ("third_party/", "/build/")

#: Below this the parse is assumed broken rather than the tree clean.
MIN_RESOLVED_TOKENS = 400


@dataclass(frozen=True)
class Finding:
    """One CMake path that does not resolve."""

    path: str
    line: int
    variable: str
    token: str
    reason: str

    def render(self) -> str:
        """Return the one-line human form."""
        return f"{self.path}:{self.line}: {self.reason}: ${{{self.variable}}}/{self.token}"


def is_scannable(relative: str) -> bool:
    """Return True when this CMake file is ours to hold to the resolve."""
    if relative.startswith("build/"):
        return False
    return not any(marker in relative for marker in VENDOR_MARKERS)


def cmake_files(root: Path) -> list[Path]:
    """Return every first-party CMake file, sorted for a stable report."""
    found: list[Path] = []
    for pattern in ("**/CMakeLists.txt", "**/*.cmake"):
        found.extend(
            path
            for path in root.glob(pattern)
            if path.is_file() and is_scannable(path.relative_to(root).as_posix())
        )
    return sorted(set(found))


def classify(token: str) -> str:
    """Return 'variable', 'glob' or 'literal' for one path token."""
    if "$" in token:
        return "variable"
    if "*" in token or "?" in token or "[" in token:
        return "glob"
    return "literal"


def discover_roots(root: Path, path: Path, text: str) -> dict[str, Path]:
    """Return the root names this listfile defines from its own location.

    ``CMAKE_CURRENT_SOURCE_DIR`` is the file's own directory only in a
    ``CMakeLists.txt``; in an included ``.cmake`` it is the including
    directory, which this gate does not model.  A name defined twice with two
    values is dropped rather than guessed at.
    """
    own_directory_scope = path.name == "CMakeLists.txt"
    resolved: dict[str, Path] = {}
    ambiguous: set[str] = set()
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        match = ROOT_DEFINITION.match(line)
        if match is None:
            continue
        name, anchor, suffix = match.group(1), match.group(2), match.group(3) or ""
        if name == ROOT_VARIABLE:
            continue
        if anchor == "CMAKE_CURRENT_SOURCE_DIR" and not own_directory_scope:
            continue
        target = (path.parent / suffix.lstrip("/")).resolve()
        if not target.is_dir():
            ambiguous.add(name)
            continue
        try:
            target.relative_to(root.resolve())
        except ValueError:
            ambiguous.add(name)
            continue
        if name in resolved and resolved[name] != target:
            ambiguous.add(name)
        resolved[name] = target
    for name in ambiguous:
        resolved.pop(name, None)
    return resolved


def scan_file(root: Path, path: Path) -> tuple[list[Finding], dict[str, int]]:
    """Resolve every token in one CMake file."""
    counts = {"literal": 0, "glob": 0, "variable": 0}
    findings: list[Finding] = []
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return findings, counts

    roots: dict[str, Path] = {ROOT_VARIABLE: root}
    roots.update(discover_roots(root, path, text))
    pattern = token_pattern(list(roots))

    for number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("#"):
            continue
        for match in pattern.finditer(line):
            name = match.group(1)
            base = roots[name]
            token = match.group(2).rstrip(PATH_ARGUMENT_TRAILERS)
            kind = classify(token)
            counts[kind] += 1
            if kind == "variable":
                continue
            if kind == "glob":
                if not list(base.glob(token)):
                    findings.append(Finding(relative, number, name, token, "glob matches nothing"))
                continue
            if not (base / token).exists():
                findings.append(Finding(relative, number, name, token, "path does not exist"))
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

    # --- roots the listfile defines itself ------------------------
    nested = tmp_root / "apps" / "shared_libs" / "mdl"
    nested.mkdir(parents=True)
    alias_cases: dict[str, tuple[str, int]] = {
        "set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)\n"
        "target_sources(m PRIVATE ${MDL_REPO_ROOT}/libs/ra8_gfx/src/ra8_gfx_blit.c)\n": (
            "a self-defined root resolving an existing path is clean",
            0,
        ),
        "set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)\n"
        "target_sources(m PRIVATE ${MDL_REPO_ROOT}/libs/ra8_num/src/ra8_num_decimal.c)\n": (
            "a self-defined root resolving a deleted path fires",
            1,
        ),
        'get_filename_component(FW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)\n'
        "target_sources(m PRIVATE ${FW_ROOT}/libs/ra8_num/src/ra8_num_decimal.c)\n": (
            "CMAKE_CURRENT_LIST_DIR is the listfile's own directory",
            1,
        ),
        "set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)\n"
        "set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)\n"
        "target_sources(m PRIVATE ${MDL_REPO_ROOT}/libs/ra8_num/src/ra8_num_decimal.c)\n": (
            "a root defined twice with two values is not judged",
            0,
        ),
        "target_sources(m PRIVATE ${MDL_REPO_ROOT}/libs/ra8_num/src/ra8_num_decimal.c)\n": (
            "an undefined root is not judged",
            0,
        ),
    }
    for body, (label, expected) in alias_cases.items():
        listing = nested / "CMakeLists.txt"
        listing.write_text(body)
        findings, _ = scan_file(tmp_root, listing)
        if len(findings) != expected:
            failures.append(f"{label}: expected {expected} finding(s), got {len(findings)}")
    (nested / "CMakeLists.txt").unlink()

    included = nested / "included.cmake"
    included.write_text(
        "set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)\n"
        "target_sources(m PRIVATE ${MDL_REPO_ROOT}/libs/ra8_num/src/ra8_num_decimal.c)\n"
    )
    findings, _ = scan_file(tmp_root, included)
    if findings:
        failures.append(
            "CMAKE_CURRENT_SOURCE_DIR in an included .cmake must not be resolved: "
            f"got {len(findings)} finding(s)"
        )
    included.unlink()

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
    with tempfile.TemporaryDirectory() as raw:
        failures = selftest(Path(raw))
    if failures:
        print("check_cmake_source_paths.py: SELFTEST FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("selftest: check_cmake_source_paths.py OK (15 both-direction cases)")
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
            "evaluates a cross-only block. Update the path, or drop the "
            "line.",
            file=sys.stderr,
        )
        return 1

    print(
        f"check_cmake_source_paths.py: {counts['files']} CMake file(s); "
        f"{counts['literal']} literal path(s) and {counts['glob']} glob(s) all resolve "
        f"against {ROOT_VARIABLE} and the roots each listfile defines itself; "
        f"{counts['variable']} variable-interpolated path(s) not judged."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
