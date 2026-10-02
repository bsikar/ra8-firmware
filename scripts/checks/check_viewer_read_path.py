#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Prove every ra8_viewer_read_path() KEEP/DROP pair partitions its glob.

WHY THIS EXISTS
===============
``rabook_viewer`` links only a SUBSET of several firmware libraries: the
reader-core members, not the producers and encoders.  ``ra8_viewer_read_path()``
makes that explicit, globbing the library source directory and calling
``message(FATAL_ERROR ...)`` unless KEEP and DROP partition the glob exactly.
That check is good.  Its problem is WHEN it runs: only under a configure of
that one host tool.

The slab port is the worked example.  ``libs/ra8_mem/src/ra8_slab.c`` went Zig in
a4aff0d4a and the DROP list kept naming it.  A declared-but-absent member
breaks configure just as hard as an unclassified present one, so the tools
build was broken from that commit, and nothing said so: ``zig build`` never
reads a CMakeLists.txt, host tests link the Zig archive and pass, and the
porting lane's sandbox has no ``cmake`` to configure with.  It surfaces on
someone else's machine, in a target nobody porting was looking at.

This gate is the same partition rule, evaluated as a text resolve.  No
configure, no toolchain, no host compiler, so it runs everywhere the porting
work actually happens and fails the commit that deletes the source rather
than the build three days later.

WHAT IT ENFORCES, PRECISELY
---------------------------
For every ``ra8_viewer_read_path()`` call whose directory argument resolves:

  * a ``.c`` present in the directory and named in neither KEEP nor DROP
    FAILS -- a reader-core file was split, renamed or added;
  * a name in KEEP or DROP that is not present FAILS -- a member was renamed
    or deleted and its list was not updated;
  * a name in BOTH lists FAILS -- the lists are meant to partition, and CMake
    would silently take the KEEP side.

A call whose directory argument still carries an unresolved ``${...}`` is
counted and skipped, not guessed at.  The count is reported so the number is
visible rather than silently zero.

A vacuity guard fails the gate closed if the parse stops finding calls: a
parser that has quietly stopped matching would otherwise report a clean tree
forever.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_cmake_source_paths import (
    ROOT_VARIABLE,
    cmake_files,
    discover_roots,
)

REPO_ROOT = Path(__file__).resolve().parents[2]

#: The command whose KEEP/DROP contract this gate holds.
COMMAND = "ra8_viewer_read_path"

#: Below this the parse is assumed broken rather than the tree clean.
MIN_CALLS = 6


@dataclass(frozen=True)
class Finding:
    """One member the KEEP/DROP lists and the directory disagree about."""

    path: str
    line: int
    directory: str
    member: str
    reason: str

    def render(self) -> str:
        """Return the one-line human form."""
        return f"{self.path}:{self.line}: {self.reason}: {self.directory}/{self.member}"


def call_blobs(text: str) -> list[tuple[int, str]]:
    """Return every (line number, argument text) for a call to COMMAND.

    Parenthesis-balanced, because the calls span lines and the argument list
    is what carries the KEEP and DROP names.
    """
    blobs: list[tuple[int, str]] = []
    needle = COMMAND + "("
    position = 0
    while True:
        start = text.find(needle, position)
        if start < 0:
            return blobs
        # A call is only a call at the start of a statement: a definition
        # ("function(ra8_viewer_read_path ...") or prose in a comment is not.
        line_start = text.rfind("\n", 0, start) + 1
        prefix = text[line_start:start]
        if prefix.strip() or text[line_start:].lstrip().startswith("#"):
            position = start + len(needle)
            continue
        depth = 0
        index = start + len(needle) - 1
        while index < len(text):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        blobs.append((text.count("\n", 0, start) + 1, text[start + len(needle) : index]))
        position = index + 1


def parse_call(blob: str) -> tuple[str, list[str], list[str]]:
    """Return (directory token, KEEP names, DROP names) for one call."""
    words = [word for word in blob.replace("\n", " ").split(" ") if word]
    keep: list[str] = []
    drop: list[str] = []
    directory = ""
    section = None
    for index, word in enumerate(words):
        if word == "KEEP":
            section = keep
            continue
        if word == "DROP":
            section = drop
            continue
        if section is not None:
            section.append(word)
        elif index == 1:
            directory = word
    return directory, keep, drop


def resolve_directory(roots: dict[str, Path], token: str) -> Path | None:
    """Return the directory a call's second argument names, or None."""
    if not token.startswith("${"):
        return None
    close = token.find("}")
    if close < 0:
        return None
    name = token[2:close]
    remainder = token[close + 1 :].lstrip("/")
    if "$" in remainder or name not in roots:
        return None
    return roots[name] / remainder


def scan_file(root: Path, path: Path) -> tuple[list[Finding], dict[str, int]]:
    """Hold every call in one listfile to the partition rule."""
    counts = {"calls": 0, "resolved": 0, "unresolved": 0}
    findings: list[Finding] = []
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return findings, counts

    roots: dict[str, Path] = {ROOT_VARIABLE: root}
    roots.update(discover_roots(root, path, text))

    for number, blob in call_blobs(text):
        counts["calls"] += 1
        token, keep, drop = parse_call(blob)
        directory = resolve_directory(roots, token)
        if directory is None or not directory.is_dir():
            counts["unresolved"] += 1
            continue
        counts["resolved"] += 1
        present = {entry.name for entry in directory.glob("*.c")}
        shown = token
        declared = set(keep) | set(drop)
        for members, reason in (
            (sorted(set(keep) & set(drop)), "named in both KEEP and DROP"),
            (sorted(present - declared), "present but in neither KEEP nor DROP"),
            (sorted(declared - present), "declared in KEEP/DROP but absent"),
        ):
            findings.extend(Finding(relative, number, shown, member, reason) for member in members)
    return findings, counts


def check_tree(root: Path) -> tuple[list[Finding], dict[str, int]]:
    """Hold the whole tree to the partition rule."""
    findings: list[Finding] = []
    counts = {"calls": 0, "resolved": 0, "unresolved": 0, "files": 0}
    for path in cmake_files(root):
        file_findings, file_counts = scan_file(root, path)
        if file_counts["calls"]:
            counts["files"] += 1
        findings.extend(file_findings)
        for key, value in file_counts.items():
            counts[key] += value
    return findings, counts


def selftest(tmp_root: Path) -> list[str]:
    """Prove the detector fires and stays quiet, in both directions."""
    failures: list[str] = []
    source = tmp_root / "libs" / "ra8_mem" / "src"
    source.mkdir(parents=True)
    (source / "ra8_arena.c").write_text("/* keep */\n")
    (source / "ra8_vmem.c").write_text("/* drop */\n")

    cases: dict[str, tuple[str, int]] = {
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src\n"
        "  KEEP ra8_arena.c\n  DROP ra8_vmem.c\n)\n": (
            "an exact partition is clean",
            0,
        ),
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src\n"
        "  KEEP ra8_arena.c\n  DROP ra8_vmem.c ra8_slab.c\n)\n": (
            "a declared-but-deleted member fires",
            1,
        ),
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src\n  KEEP ra8_arena.c\n)\n": (
            "a present-but-unclassified member fires",
            1,
        ),
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src\n"
        "  KEEP ra8_arena.c ra8_vmem.c\n  DROP ra8_vmem.c\n)\n": (
            "a member in both lists fires",
            1,
        ),
        "ra8_viewer_read_path(\n  OUT ${FW_ROOT}/libs/ra8_mem/src\n  KEEP ra8_arena.c\n)\n": (
            "an unresolvable root is skipped, not guessed",
            0,
        ),
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_${WHICH}/src\n"
        "  KEEP ra8_arena.c\n)\n": (
            "a variable-interpolated directory is skipped",
            0,
        ),
        "# ra8_viewer_read_path(OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src KEEP gone.c)\n": (
            "prose in a comment is not judged",
            0,
        ),
        "function(ra8_viewer_read_path out_var lib_src_dir)\nendfunction()\n": (
            "the function definition is not a call",
            0,
        ),
    }

    listing = tmp_root / "CMakeLists.txt"
    for body, (label, expected) in cases.items():
        listing.write_text(body)
        findings, _ = scan_file(tmp_root, listing)
        if len(findings) != expected:
            failures.append(f"{label}: expected {expected} finding(s), got {len(findings)}")

    listing.write_text(
        "ra8_viewer_read_path(\n  OUT ${RA8_REPO_ROOT}/libs/ra8_mem/src\n"
        "  KEEP ra8_arena.c\n  DROP ra8_vmem.c\n)\n"
    )
    _, counts = scan_file(tmp_root, listing)
    if counts["calls"] != 1 or counts["resolved"] != 1:
        failures.append(f"census wrong: {counts}")
    return failures


def run_selftest() -> int:
    """Run the selftest in a throwaway tree."""
    with tempfile.TemporaryDirectory() as raw:
        failures = selftest(Path(raw))
    if failures:
        print("check_viewer_read_path.py: SELFTEST FAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("selftest: check_viewer_read_path.py OK (9 both-direction cases)")
    return 0


def main(argv: list[str]) -> int:
    """Hold the tree to the partition rule, or prove the detector."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--selftest", action="store_true", help="prove the detector instead of scanning"
    )
    args = parser.parse_args(argv[1:])
    if args.selftest:
        return run_selftest()

    findings, counts = check_tree(REPO_ROOT)
    if counts["calls"] < MIN_CALLS:
        print(
            f"check_viewer_read_path.py: FATAL -- found only {counts['calls']} "
            f"{COMMAND}() call(s), below the {MIN_CALLS} this tree carries. The parse "
            "is broken, not the tree clean.",
            file=sys.stderr,
        )
        return 2

    if findings:
        print(f"\n{len(findings)} KEEP/DROP partition error(s):\n", file=sys.stderr)
        for finding in sorted(findings, key=lambda item: (item.path, item.line, item.member)):
            print(f"  {finding.render()}", file=sys.stderr)
        print(
            f"\nEvery {COMMAND}() KEEP/DROP pair must partition its directory glob "
            "exactly. A port that deletes or renames a library source leaves the list "
            "behind, and configure then fails on a machine that builds the host tools "
            "(#2610). Classify the member, or drop it from its list.",
            file=sys.stderr,
        )
        return 1

    print(
        f"check_viewer_read_path.py: {counts['calls']} {COMMAND}() call(s) in "
        f"{counts['files']} file(s); {counts['resolved']} partition their glob exactly; "
        f"{counts['unresolved']} with an unresolved directory not judged."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
