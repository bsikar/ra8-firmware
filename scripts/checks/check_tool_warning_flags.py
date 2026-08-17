#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Assert the host tools compile first-party sources at the project warning bar.

Why this exists
---------------
``apps/stand_alone/media_dl`` compiled ``libs/ra8_jof/*``, ``libs/ra8_hal/ra8_jpeg_sw*``,
``libs/ra8_io/ra8_io_compress.c`` and three ``libs/ra8_core`` files under a
blanket ``-w``, with a comment claiming "the repo lints these via its own
build".  It does not: ``-w`` turns off every diagnostic, so the one build in
the tree that compiles those files for a 64-bit host could not report a
host-only finding, and neither tool carried ``-Werror`` at all (#309).
Removing the ``-w`` is a one-line edit that nothing stops a later change from
re-adding, and it would come back silently -- the build would still pass.

So the removal is checked, not just done.  The check reads the compile
database CMake emits, which records the exact argv per translation unit, and
holds every first-party source in it to NASA Power of 10 Rule 10:

  * blanket ``-Werror`` is present, and
  * blanket ``-w`` is absent.

Vendored SOUP (``libs/third_party/``) and generated data (``libs/ra8_fonts/``) are
exempt by construction -- they are not hand-authored here and CLAUDE.md scopes
the style bar to first-party code.

Precision
---------
The traps this deliberately avoids, each of which a substring test gets wrong:

  * ``-Werror=return-type`` promotes ONE diagnostic and is not blanket
    ``-Werror``; ``"-Werror" in command`` would wrongly accept it.
  * ``-Wno-error`` later on the line cancels an earlier ``-Werror``.
  * ``-Wall`` / ``-Wwrite-strings`` are not ``-w``; a prefix test would
    wrongly reject them.

Matching is therefore on exact argv arguments, in order, never on substrings.

Second compiler arm (#356)
--------------------------
gcc-14 catches warning families clang-18 misses -- its ``-Wformat-truncation``
found a silent PATH_MAX path-join truncation in ``apps/stand_alone/media_dl`` that clang
did not flag. The tools-build gate therefore compiles the host tools under BOTH
pinned compilers and passes both sets of databases here. ``--require-compilers
clang,gcc`` makes a silently-dropped arm a hard failure rather than a vacuous
pass (the #348/#355 class): each named family must drive at least one database.

Usage
-----
    check_tool_warning_flags.py COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --require-compilers clang,gcc COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --require-all-cmake-tools COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --selftest
"""

from __future__ import annotations

import argparse
import json
import shlex
import sys
from pathlib import Path

# Path fragments whose sources are not hand-authored under this project's
# style rules. Kept identical in spirit to check_no_silent_stubs.py's EXCLUDED.
EXEMPT_FRAGMENTS = ("libs/third_party/", "libs/ra8_fonts/")


def is_exempt(source: str) -> bool:
    """True when `source` is vendored SOUP or generated data."""
    normalised = source.replace("\\", "/")
    return any(fragment in normalised for fragment in EXEMPT_FRAGMENTS)


def tokens_of(entry: dict) -> list[str]:
    """Return the argv of one compile-database entry.

    CMake emits either `arguments` (a list) or `command` (a single string)
    depending on generator and version; both spellings are accepted.
    """
    if isinstance(entry.get("arguments"), list):
        return [str(arg) for arg in entry["arguments"]]
    return shlex.split(str(entry.get("command", "")))


def compiler_of(argv: list[str]) -> str:
    """Classify the compiler family driving one compile-database entry.

    Returns ``"clang"`` or ``"gcc"`` for the two pinned host-tool arms, or
    ``"other"`` for anything else. The first recognised driver token wins, so a
    launcher prefix (``ccache gcc-14 ...``) still classifies as gcc.
    """
    for token in argv:
        base = token.replace("\\", "/").rsplit("/", 1)[-1]
        if "clang" in base:
            return "clang"
        if base in ("gcc", "g++") or base.startswith(("gcc-", "g++-")):
            return "gcc"
    return "other"


def missing_required_compilers(seen: set[str], required: list[str]) -> list[str]:
    """Return the required compiler families that no database exercised.

    This is the #356 second-arm guard: the tools-build gate compiles the host
    tools under clang-18 AND gcc-14 and passes both sets of databases here. If
    the gcc arm is ever silently dropped, its family never appears in `seen`,
    and a silently-dropped arm otherwise reads as a pass -- the #348/#355
    failure class. ``--selftest`` asserts this fires when an arm is absent.
    """
    return sorted(set(required) - set(seen))


def missing_tool_projects(expected: set[str], database_paths: list[str]) -> list[str]:
    """Return CMake tool directories with no compile database in the gate."""
    observed = {Path(path).parent.name for path in database_paths}
    return sorted(expected - observed)


def cmake_tool_projects(repo_root: Path) -> set[str]:
    """Discover every host CMake project under ``tools/`` and ``apps/``.

    The two roots nest differently and the glob has to say so. A tool is a
    direct child of ``tools/``; a product sits one level deeper, beneath a
    category (``apps/stand_alone/media_dl``) so that a future sibling
    category such as ``apps/threadx_modules/`` can hold a different kind of
    build. Globbing ``apps/*/CMakeLists.txt`` would look only at the category
    directories, which carry none, and so discover nothing at all -- read
    here as "no product needs its warning flags checked" rather than as the
    scope collapse it is.
    """
    return {
        path.parent.name
        for root, pattern in (
            (repo_root / "tools", "*/CMakeLists.txt"),
            (repo_root / "apps", "*/*/CMakeLists.txt"),
        )
        for path in root.glob(pattern)
    }


def flag_problem(argv: list[str]) -> str | None:
    """Return a problem description for `argv`, or None when it is compliant.

    Blanket `-Werror` must be in force at the end of the line and blanket `-w`
    must never appear. Both are exact-token tests: `-Werror=xxx` is a targeted
    promotion rather than the blanket flag, and `-Wno-error` cancels a blanket
    `-Werror` that precedes it.
    """
    werror = False
    for arg in argv:
        if arg == "-Werror":
            werror = True
        elif arg in ("-Wno-error", "-Wno-error=all"):
            werror = False
        elif arg == "-w":
            return "compiled with -w (all diagnostics disabled)"
    if not werror:
        return "compiled without blanket -Werror"
    return None


def check_database(path: Path) -> tuple[list[str], set[str]]:
    """Return (violation lines, compiler families seen) for one database.

    The compiler set is the #356 second-arm evidence: every translation unit in
    one database is compiled by the same driver, so the union across the clang
    and gcc databases the gate passes must contain both families.
    """
    try:
        entries = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"check_tool_warning_flags.py: FATAL -- cannot read {path}: {exc}\n")
        raise SystemExit(2) from exc

    if not entries:
        sys.stderr.write(
            f"check_tool_warning_flags.py: FATAL -- {path} lists no translation\n"
            "  units. An empty compile database would let this gate report a\n"
            "  pass for a build that never happened.\n"
        )
        raise SystemExit(2)

    violations: list[str] = []
    compilers: set[str] = set()
    for entry in entries:
        argv = tokens_of(entry)
        compilers.add(compiler_of(argv))
        source = str(entry.get("file", ""))
        if not source or is_exempt(source):
            continue
        problem = flag_problem(argv)
        if problem is not None:
            violations.append(f"{source}: {problem}")
    return violations, compilers


# ---------------------------------------------------------------------------
# Selftest: the detector must fire on a genuinely broken input AND stay quiet
# on the legal-but-tricky forms. A checker asserted in only one direction can
# be vacuously green forever.
# ---------------------------------------------------------------------------
SELFTEST_CASES: list[tuple[str, str, list[str], bool]] = [
    # NOTE: this case carries -Werror deliberately. An earlier revision omitted
    # it, so the case fired on the MISSING -Werror and would have passed even
    # with the -w detection ripped out entirely -- a must-fire case that proved
    # nothing about the rule it was named for. Every must-fire case below is
    # compliant except for the one property it is testing.
    (
        "first-party with -w on top of an otherwise-compliant line",
        "libs/ra8_jof/src/ra8_jof.c",
        ["cc", "-Wall", "-Wextra", "-Werror", "-w", "-fno-strict-aliasing", "-c"],
        True,
    ),
    (
        "first-party without -Werror",
        "libs/ra8_core/src/ra8_log.c",
        ["cc", "-Wall", "-Wextra", "-c"],
        True,
    ),
    (
        "first-party with only a targeted -Werror=",
        "apps/stand_alone/media_dl/src/main.c",
        ["cc", "-Wall", "-Wextra", "-Werror=return-type", "-c"],
        True,
    ),
    (
        "first-party whose -Werror is cancelled later",
        "apps/stand_alone/media_dl/src/mdl_export.c",
        ["cc", "-Wall", "-Werror", "-Wno-error", "-c"],
        True,
    ),
    (
        "compliant first-party source",
        "libs/ra8_jof/src/ra8_jof_produce.c",
        ["cc", "-Wall", "-Wextra", "-Werror", "-c"],
        False,
    ),
    (
        "-Wall is not -w",
        "libs/ra8_io/src/ra8_io_compress.c",
        ["cc", "-Wall", "-Wwrite-strings", "-Werror", "-c"],
        False,
    ),
    (
        "vendored SOUP may keep -w",
        "libs/third_party/miniz/miniz.c",
        ["cc", "-w", "-fno-strict-aliasing", "-c"],
        False,
    ),
    (
        "generated font data is exempt",
        "libs/ra8_fonts/ra8_font_dejavu.c",
        ["cc", "-w", "-c"],
        False,
    ),
]

# Compiler-classification cases: compiler_of must name the driver family from
# an argv the way the real databases spell it (absolute path, versioned name,
# a launcher prefix). Asserted so the #356 second-arm guard cannot be defeated
# by a classifier that quietly folds gcc into clang or "other".
COMPILER_SELFTEST_CASES: list[tuple[str, list[str], str]] = [
    ("clang-18 absolute path", ["/usr/bin/clang-18", "-c", "f.c"], "clang"),
    ("clang++-18 driver", ["clang++-18", "-c", "f.cc"], "clang"),
    ("gcc-14 absolute path", ["/usr/local/bin/gcc-14", "-c", "f.c"], "gcc"),
    ("g++-14 driver", ["g++-14", "-c", "f.cc"], "gcc"),
    ("bare gcc", ["gcc", "-c", "f.c"], "gcc"),
    ("ccache-wrapped gcc-14", ["ccache", "gcc-14", "-c", "f.c"], "gcc"),
    ("unknown driver", ["tcc", "-c", "f.c"], "other"),
]

# Second-arm coverage cases: with both arms required, a missing family must be
# reported (fires) and a complete set must report nothing (quiet). This is the
# property the #356 acceptance names -- a silently-dropped gcc arm reads as a
# pass unless its absence is turned into a hard finding here.
COVERAGE_SELFTEST_CASES: list[tuple[str, set[str], list[str], list[str]]] = [
    ("only clang seen -> gcc missing", {"clang"}, ["clang", "gcc"], ["gcc"]),
    ("only gcc seen -> clang missing", {"gcc"}, ["clang", "gcc"], ["clang"]),
    ("both seen -> nothing missing", {"clang", "gcc"}, ["clang", "gcc"], []),
]

PROJECT_SELFTEST_CASES: list[tuple[str, set[str], list[str], list[str]]] = [
    (
        "an omitted CMake tool is reported",
        {"media_dl", "ra8_emulator"},
        ["build/media_dl/compile_commands.json"],
        ["ra8_emulator"],
    ),
    (
        "all CMake tools represented stays quiet",
        {"media_dl", "ra8_emulator"},
        [
            "build/media_dl/compile_commands.json",
            "build/ra8_emulator/compile_commands.json",
        ],
        [],
    ),
]


def selftest() -> int:
    """Prove the detector fires and stays quiet where it must."""
    failures: list[str] = []
    for name, source, argv, should_fire in SELFTEST_CASES:
        fired = (not is_exempt(source)) and (flag_problem(argv) is not None)
        if fired != should_fire:
            failures.append(
                f"  {name}: expected {'a violation' if should_fire else 'no violation'}, "
                f"got {'a violation' if fired else 'none'}"
            )

    for name, argv, want in COMPILER_SELFTEST_CASES:
        got = compiler_of(argv)
        if got != want:
            failures.append(f"  compiler_of {name}: expected {want!r}, got {got!r}")

    for name, seen, required, want_missing in COVERAGE_SELFTEST_CASES:
        got_missing = missing_required_compilers(seen, required)
        if got_missing != want_missing:
            failures.append(
                f"  coverage {name}: expected missing {want_missing}, got {got_missing}"
            )

    for name, expected, paths, want_missing in PROJECT_SELFTEST_CASES:
        got_missing = missing_tool_projects(expected, paths)
        if got_missing != want_missing:
            failures.append(
                f"  project scope {name}: expected missing {want_missing}, got {got_missing}"
            )

    if failures:
        sys.stderr.write("check_tool_warning_flags.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    fires = sum(1 for case in SELFTEST_CASES if case[3])
    quiet = len(SELFTEST_CASES) - fires
    print(
        f"check_tool_warning_flags.py --selftest: PASS "
        f"({fires} must-fire, {quiet} must-stay-quiet flag cases; "
        f"{len(COMPILER_SELFTEST_CASES)} classifier, "
        f"{len(COVERAGE_SELFTEST_CASES)} second-arm coverage, "
        f"{len(PROJECT_SELFTEST_CASES)} project-scope cases)"
    )
    return 0


def _scan_databases(paths: list[str]) -> tuple[list[str], set[str]]:
    """Scan every database path; return (violations, compiler families seen).

    Exits (SystemExit 2) when a path is missing, matching check_database's
    fatal handling of an empty or unreadable database -- a gate must never
    report a pass for a database it could not inspect.
    """
    violations: list[str] = []
    seen: set[str] = set()
    for name in paths:
        path = Path(name)
        if not path.is_file():
            sys.stderr.write(
                f"check_tool_warning_flags.py: FATAL -- {path} does not exist.\n"
                "  Configure the tool with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first.\n"
            )
            raise SystemExit(2)
        db_violations, db_compilers = check_database(path)
        violations += db_violations
        seen |= db_compilers
    return violations, seen


def _report_violations(violations: list[str]) -> None:
    """Print every first-party translation unit found below the warning bar."""
    sys.stderr.write(
        f"check_tool_warning_flags.py: {len(violations)} first-party "
        "translation unit(s) below the project warning bar:\n\n"
    )
    for line in sorted(violations):
        sys.stderr.write(f"  {line}\n")
    sys.stderr.write(
        "\nFirst-party sources are held to -Wall -Wextra -Werror everywhere\n"
        "else in this tree (NASA Power of 10 Rule 10). Only libs/third_party/\n"
        "SOUP may be compiled -w. Fix the warning; do not re-suppress it.\n"
    )


def _report_missing_arm(missing: list[str], seen: set[str]) -> None:
    """Print the silently-dropped compiler-arm failure (#356)."""
    sys.stderr.write(
        "check_tool_warning_flags.py: FATAL -- required compiler arm(s) "
        f"never exercised: {', '.join(missing)}\n"
        f"  databases were compiled by: {', '.join(sorted(seen)) or '(none)'}\n"
        "  The tools-build gate compiles the host tools under clang-18 AND\n"
        "  gcc-14 (#356) so the warnings each catches but the other misses\n"
        "  are both held. A silently-dropped arm would read as a pass.\n"
    )


def main() -> int:
    """Entry point."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("databases", nargs="*", help="compile_commands.json paths")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the detector fires and stays quiet, then exit",
    )
    parser.add_argument(
        "--require-compilers",
        default="",
        metavar="FAMILY[,FAMILY...]",
        help=(
            "comma-separated compiler families (e.g. clang,gcc) that must each "
            "drive at least one database; the #356 second-arm guard. A single "
            "comma-joined value keeps it order-independent of the database list."
        ),
    )
    parser.add_argument(
        "--require-all-cmake-tools",
        action="store_true",
        help="fail when any tools/*/CMakeLists.txt project has no database",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.databases:
        sys.stderr.write(
            "check_tool_warning_flags.py: FATAL -- no compile database given.\n"
            "  This gate must never run with nothing to inspect: that reports a\n"
            "  pass for work never done.\n"
        )
        return 2

    if args.require_all_cmake_tools:
        missing_projects = missing_tool_projects(cmake_tool_projects(Path.cwd()), args.databases)
        if missing_projects:
            sys.stderr.write(
                "check_tool_warning_flags.py: FATAL -- CMake tool project(s) "
                f"absent from tools-build: {', '.join(missing_projects)}\n"
            )
            return 2

    required = [fam for fam in args.require_compilers.split(",") if fam]
    violations, seen = _scan_databases(args.databases)
    if violations:
        _report_violations(violations)
        return 1
    missing = missing_required_compilers(seen, required)
    if missing:
        _report_missing_arm(missing, seen)
        return 1

    arms = f", arms: {', '.join(sorted(seen))}" if required else ""
    print(
        f"check_tool_warning_flags.py: OK ({len(args.databases)} compile "
        f"database(s), every first-party TU at -Wall -Wextra -Werror{arms})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
