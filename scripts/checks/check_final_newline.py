#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: every first-party source file shall end in a trailing newline.

POSIX defines a text line as ending in a newline; tools that read the last
line (diff, cat, shell ``read``, parsers) behave better when the rule holds.
``.editorconfig`` declares ``insert_final_newline`` but only editors honour
it, and ``.clang-format``'s ``InsertNewlineAtEOF`` only covers C/C++ that
reaches the formatter.  Neither is a gate, and neither touches Python, shell,
CMake, or YAML.

This repo-wide backstop covers every first-party source file (C/C++, Python,
shell, CMake, just, YAML, linker scripts) and fails if any is non-empty and
does not end in a newline byte.  The whole-tree set is DERIVED from
``git ls-files`` via ``lint_targets.first_party_paths`` rather than a hardcoded
root list, so a new top-level directory is covered the day it lands; a hardcoded
list -- which this used to carry, omitting ``docs/``, ``just/``, ``infra/`` and
``coprocessor/`` (#549) -- does not fail when it goes stale, it just reports
success over a shrinking slice.  Vendor trees and generated font tables are
excluded.  There is no grandfathering.

Run::

    check_final_newline.py                      # scan the whole tree
    check_final_newline.py path/to/file ...     # scan listed files
    check_final_newline.py --selftest           # prove both directions

Exit 0 if every file ends in a newline, exit 1 (with a list) otherwise, exit 2
when the whole-tree sweep collapses below FILE_FLOOR.
"""

from __future__ import annotations

import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import first_party_paths, is_build_output_path
from selftest_assert import expect, report

REPO_ROOT = Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".cc",
    ".cxx",
    ".hh",
    ".hxx",
    ".m",
    ".mm",
    ".inl",
    ".py",
    ".sh",
    ".cmake",
    ".mk",
    ".just",
    ".yml",
    ".yaml",
    ".ld",
)
SOURCE_NAMES = ("CMakeLists.txt", "justfile", "Justfile")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
    "_unsupported/",
)

# A tree this size cannot legitimately collapse to a handful of files. If the
# whole-tree sweep returns less than this, something broke (an unreachable repo
# root, a collapsed git enumeration, a runaway EXCLUDE_FRAGMENTS) and reporting
# "all end in a newline" would be a lie. Measured 2026-08-02: 3116 first-party
# source files (the derived scope, up from 2843 when it was a hardcoded root
# list). Same trip-wire as check_ruff.py.
FILE_FLOOR = 2200


def _ends_in_newline(path: Path) -> bool:
    """Return True if `path` is empty or ends in a newline byte."""
    try:
        data = path.read_bytes()
    except OSError:
        return True  # unreadable: not this gate's problem
    return (not data) or data.endswith(b"\n")


def _is_excluded(path: Path) -> bool:
    return is_build_output_path(path) or any(frag in str(path) for frag in EXCLUDE_FRAGMENTS)


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _derived_sources() -> list[Path]:
    """Every first-party source file, derived from git rather than a root list.

    Enumeration goes through ``lint_targets.first_party_paths`` -- the shared
    derived-scope primitive -- so a newly added top-level directory (``docs/``,
    ``just/``, ``infra/`` and ``coprocessor/`` were the ones a hardcoded root
    list had silently omitted, #549) is in scope the day it lands. The suffix
    set is the sole language filter; ``SOURCE_NAMES`` catches the
    extensionless-by-convention listfiles (``justfile``, ``CMakeLists.txt``).
    """
    rels = set(first_party_paths(SOURCE_SUFFIXES))
    for name in SOURCE_NAMES:
        rels |= {rel for rel in first_party_paths((name,)) if Path(rel).name == name}
    return [REPO_ROOT / rel for rel in sorted(rels)]


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                out.extend(c for c in path.rglob("*") if c.is_file() and _is_source(c))
            elif _is_source(path):
                out.append(path)
        return [p for p in out if not _is_excluded(p)]

    return [p for p in _derived_sources() if not _is_excluded(p)]


def selftest() -> int:
    """Prove the detector fires on a missing newline and that the scope is real.

    Both directions plus a scope probe: a non-empty file with no trailing
    newline must FIRE, a newline-terminated file and an empty file must stay
    QUIET, the derived whole-tree scope must clear ``FILE_FLOOR``, and it must
    actually reach the roots a hardcoded list had dropped (``just/``, ``infra/``)
    -- a clean run over a scope that never sees those roots proves nothing.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        good = root / "good.py"
        good.write_text("x = 1\n", encoding="utf-8")
        bad = root / "bad.py"
        bad.write_bytes(b"x = 1")
        empty = root / "empty.py"
        empty.write_bytes(b"")
        expect(_ends_in_newline(good), "MUST NOT FIRE: a newline-terminated file", failures)
        expect(_ends_in_newline(empty), "MUST NOT FIRE: an empty file", failures)
        expect(not _ends_in_newline(bad), "MUST FIRE: a file with no trailing newline", failures)

    scope = _enumerate_targets([])
    rels = {str(p.relative_to(REPO_ROOT)) for p in scope if p.is_relative_to(REPO_ROOT)}
    expect(
        len(scope) >= FILE_FLOOR,
        f"derived scope sees {len(scope)} file(s) (floor {FILE_FLOOR})",
        failures,
    )
    for root_name in ("just", "infra"):
        expect(
            any(rel.startswith(root_name + "/") for rel in rels),
            f"the derived scope reaches {root_name}/ (previously omitted)",
            failures,
        )
    return report(failures)


def main(argv: list[str]) -> int:
    """Fail any first-party source file that does not end in a newline.

    Exists because the two existing mechanisms are not gates: .editorconfig's
    ``insert_final_newline`` is honoured only by editors, and clang-format's
    ``InsertNewlineAtEOF`` only reaches C/C++ that goes through the formatter,
    leaving Python, shell, CMake and YAML unenforced.

    An empty target set exits 0 rather than FATAL only when a file list was
    passed on argv: the caller there is the pre-commit hook handing over a
    staged file list, which legitimately filters to nothing when a commit
    touches only excluded paths. The WHOLE-TREE sweep gets the opposite
    treatment -- FILE_FLOOR, exit 2 -- because nothing about this tree can
    legitimately reduce it to a handful of files, and a sweep that read almost
    nothing reports a clean tree for exactly the wrong reason.

    Returns 1 with each offending path listed, 0 when clean or when an argv
    list filtered to nothing, 2 when the whole-tree sweep enumerated too few
    files to trust.
    """
    if "--selftest" in argv[1:]:
        return selftest()
    paths = argv[1:]
    targets = _enumerate_targets(paths)
    if not paths and len(targets) < FILE_FLOOR:
        print(
            f"check_final_newline.py: FATAL -- only {len(targets)} file(s) in scope, "
            f"floor is {FILE_FLOOR}. A collapsed sweep reports a clean tree because "
            "it scanned nothing.",
            file=sys.stderr,
        )
        return 2
    if not targets:
        print("check_final_newline.py: no files to scan", file=sys.stderr)
        return 0

    missing = sorted(_rel(p) for p in targets if not _ends_in_newline(p))
    if not missing:
        print(f"check_final_newline.py: {len(targets)} file(s) scanned, all end in a newline.")
        return 0

    print(
        f"check_final_newline.py: {len(missing)} file(s) missing a trailing newline:\n",
        file=sys.stderr,
    )
    for path in missing:
        print(f"  {path}", file=sys.stderr)
    print("\nAdd a single newline at end of file.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
