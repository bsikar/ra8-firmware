#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Assert the host tools compile first-party sources at the project warning bar.

Why this exists
---------------
``apps/host/mdl`` compiled ``apps/shared_libs/jof/*``, ``libs/ra8_hal/ra8_jpeg_sw*``,
``apps/shared_libs/compress/src/ra8_compress.c`` and three ``libs/ra8_core`` files under a
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
found a silent PATH_MAX path-join truncation in ``apps/host/mdl`` that clang
did not flag. The tools-build gate therefore compiles the host tools under BOTH
pinned compilers and passes both sets of databases here. ``--require-compilers
clang,gcc`` makes a silently-dropped arm a hard failure rather than a vacuous
pass (the #348/#355 class): each named family must drive at least one database.

The apps tier (#718)
--------------------
``--require-all-cmake-tools`` discovers the host CMake projects itself so the
gate cannot quietly stop covering one. When the products tier landed, that
discovery predated it: the ``apps/*/*/CMakeLists.txt`` glob swept in
``apps/board/stand_alone/ereader`` -- a cross-compiled TrustZone image no host build
produces -- and the gate failed demanding a tools-build database that must
never exist. Firmware products are now excluded by
``lint_targets.firmware_app_dirs()``, the tree's one definition of "linked into
an image rather than started by a C runtime", and representation is decided by
the sources the databases record rather than by build-tree names, because two
products may legitimately share a name across categories.

Usage
-----
    check_tool_warning_flags.py COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --require-compilers clang,gcc COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --require-all-cmake-tools COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --list-missing-cmake-tools COMPILE_COMMANDS_JSON [...]
    check_tool_warning_flags.py --selftest
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import shlex
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import firmware_app_dirs, is_build_output  # needs the sys.path line above

# Path fragments whose sources are not hand-authored under this project's
# style rules. Kept identical in spirit to check_no_silent_stubs.py's EXCLUDED.
EXEMPT_FRAGMENTS = ("libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/")

#: Non-vacuity floor on host-project discovery. Measured 12 on the current tree:
#: seven under ``tools/`` plus five real buildable projects under ``apps/``,
#: with firmware products and source-only libraries excluded. Source-only
#: libraries may retain a comment-only ``CMakeLists.txt`` as a composition note;
#: their consuming product/test is what compiles them. The floor exists because
#: a collapsed glob reports "no project is missing" and reads as a pass.
#: At-count, not below: this is a trip-wire on discovery, not a policy on
#: project count.
MIN_CMAKE_TOOL_PROJECTS = 12

CMAKE_COMMAND_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*[ \t]*\(")
CMAKE_BRACKET_COMMENT_RE = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)


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


def missing_tool_projects(projects: Iterable[str], sources: Iterable[str]) -> list[str]:
    """Return host CMake project directories whose code the gate never compiled.

    Representation is decided by the SOURCES the compile databases record, not
    by the name of the build tree a database sits in. Two things follow, and
    both are the point:

      * two products may legitimately share a name in different categories
        (``apps/shared_libs/mdl`` is the portable core, ``apps/host/mdl`` the
        CLI form). A basename key cannot tell them apart, so it
        silently reports the second covered because the first was built -- the
        same collapse that made scan_build.sh configure one into the other's
        cache;
      * a project a sibling COMPOSES with ``add_subdirectory`` has no build
        tree of its own, yet its translation units really are compiled and
        really are held to the warning bar here. That is exactly the shared
        mdl core, and a build-tree-name test would demand a standalone
        build that has no reason to exist.

    Paths are collapsed before matching. A form reaches its shared core through
    a repo-root variable spelled ``${CMAKE_CURRENT_SOURCE_DIR}/../../..``, so a
    database entry can name a source as ``.../apps/host/mdl/../../
    ../apps/shared_libs/mdl/src/x.c`` -- which contains BOTH project paths as
    substrings and would mark a project represented by a file that is not its
    own.

    Args:
        projects: Repo-relative host CMake project directories.
        sources: Every ``file`` entry across the databases handed to the gate.

    Returns:
        The project directories no database compiled a single file from,
        sorted.
    """
    normalised = {posixpath.normpath(str(source).replace("\\", "/")) for source in sources}
    return sorted(
        project
        for project in projects
        if not any(
            f"/{project}/" in source or source.startswith(f"{project}/") for source in normalised
        )
    )


def cmake_listfile_has_commands(path: Path) -> bool:
    """Return whether ``path`` contains an active CMake command.

    A source-only library can keep a ``CMakeLists.txt`` as a documented
    composition boundary without defining a standalone configuration. CMake
    accepts that comment-only file, but it emits no compile database, so
    treating its directory as a host project makes the tools gate demand a
    build that cannot represent anything.

    Command-looking text inside line or bracket comments is deliberately
    ignored. The first active command is enough: later project representation
    is still proved from compile-database sources by ``missing_tool_projects``.
    """
    text = CMAKE_BRACKET_COMMENT_RE.sub("", path.read_text())
    for raw_line in text.splitlines():
        line = raw_line.lstrip()
        if line.startswith("#"):
            continue
        if CMAKE_COMMAND_RE.match(line):
            return True
    return False


def cmake_tool_projects(repo_root: Path, firmware: Iterable[str] | None = None) -> tuple[str, ...]:
    """Discover every HOST CMake project under ``tools/`` and ``apps/``.

    The two roots nest differently and the discovery has to say so. A tool is a
    direct child of ``tools/``; products sit under ``apps/host/``,
    ``apps/shared_libs/``, or a deeper board form such as
    ``apps/board/stand_alone/``. A fixed-depth glob silently misses one of
    those layouts, so apps are discovered recursively and firmware products
    are then removed using the shared classifier below.

    Matching the glob is not enough to be a host tool, though, and that is the
    half this originally got wrong. A comment-only listfile documents a
    source-only library's composition but defines no standalone build, and
    therefore cannot emit the compile database this gate consumes. ``apps/``
    also carries BOTH kinds of active build:
    ``apps/board/stand_alone/ereader`` is a cross-compiled TrustZone image with a
    linker script and a reset path, built by the firmware gates and by nothing
    here, and demanding a tools-build database for it failed the gate on a
    project that must never have one. The discriminator is not re-derived --
    ``lint_targets.firmware_app_dirs()`` is the one definition of "linked into
    an image rather than started by a C runtime", shared with the tier gates.

    Args:
        repo_root: Tree to discover in.
        firmware: Repo-relative firmware app directories to exclude. Defaults
            to the live tree's; the parameter exists so the selftest can drive
            the rule with a fixture.

    Returns:
        The repo-relative host project directories, sorted.
    """
    excluded = set(firmware_app_dirs() if firmware is None else firmware)
    candidates = [
        *(repo_root / "tools").glob("*/CMakeLists.txt"),
        *(repo_root / "apps").glob("**/CMakeLists.txt"),
    ]
    found = {
        path.parent.relative_to(repo_root).as_posix()
        for path in candidates
        if not is_build_output(path.relative_to(repo_root).as_posix())
        and cmake_listfile_has_commands(path)
    }
    return tuple(sorted(found - excluded))


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


def check_database(path: Path) -> tuple[list[str], set[str], set[str]]:
    """Return (violation lines, compiler families, compiled sources) for one database.

    The compiler set is the #356 second-arm evidence: every translation unit in
    one database is compiled by the same driver, so the union across the clang
    and gcc databases the gate passes must contain both families.

    The source set is the project-scope evidence: it is what
    ``missing_tool_projects`` reads to decide whether a host CMake project's
    code reached the gate at all.
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
    sources: set[str] = set()
    for entry in entries:
        argv = tokens_of(entry)
        compilers.add(compiler_of(argv))
        source = str(entry.get("file", ""))
        if not source:
            continue
        sources.add(source)
        if is_exempt(source):
            continue
        problem = flag_problem(argv)
        if problem is not None:
            violations.append(f"{source}: {problem}")
    return violations, compilers, sources


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
        "apps/shared_libs/jof/src/jof.c",
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
        "apps/host/mdl/src/main.c",
        ["cc", "-Wall", "-Wextra", "-Werror=return-type", "-c"],
        True,
    ),
    (
        "first-party whose -Werror is cancelled later",
        "apps/shared_libs/mdl/src/mdl_export.c",
        ["cc", "-Wall", "-Werror", "-Wno-error", "-c"],
        True,
    ),
    (
        "compliant first-party source",
        "apps/shared_libs/jof/src/jof_produce.c",
        ["cc", "-Wall", "-Wextra", "-Werror", "-c"],
        False,
    ),
    (
        "-Wall is not -w",
        "apps/shared_libs/compress/src/ra8_compress.c",
        ["cc", "-Wall", "-Wwrite-strings", "-Werror", "-c"],
        False,
    ),
    (
        "vendored SOUP may keep -w",
        "apps/shared_libs/third_party/miniz/miniz.c",
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

# Project-scope cases: a host CMake project whose code the gate never compiled
# must be reported, and one whose code it did -- including a shared core reached
# only through a sibling's build tree -- must stay quiet. The two same-named
# products are the case a basename key gets wrong, so they are asserted apart.
PROJECT_SELFTEST_CASES: list[tuple[str, list[str], list[str], list[str]]] = [
    (
        "a host project nothing compiled is reported",
        ["tools/ra8_emulator", "apps/host/mdl"],
        ["/w/apps/host/mdl/src/main.c"],
        ["tools/ra8_emulator"],
    ),
    (
        "a shared core composed into a sibling's build tree stays quiet",
        ["apps/shared_libs/mdl", "apps/host/mdl"],
        [
            "/w/apps/host/mdl/src/main.c",
            "/w/apps/shared_libs/mdl/src/mdl_cache.c",
        ],
        [],
    ),
    (
        "a same-named product in another category is NOT covered by its twin",
        ["apps/shared_libs/mdl", "apps/host/mdl"],
        ["/w/apps/host/mdl/src/main.c"],
        ["apps/shared_libs/mdl"],
    ),
    (
        "every project compiled stays quiet",
        ["tools/ra8_emulator", "tools/mkbookimg"],
        [
            "/w/tools/ra8_emulator/src/emu.c",
            "/w/tools/mkbookimg/src/mkbookimg.c",
        ],
        [],
    ),
    (
        "a source reached through .. counts only for the project it resolves to",
        ["apps/shared_libs/mdl", "apps/host/mdl"],
        ["/w/apps/host/mdl/../../../apps/shared_libs/mdl/src/mdl_cache.c"],
        ["apps/host/mdl"],
    ),
]


def _discovery_fixture(root: Path) -> None:
    """Write the smallest tree that exercises both discovery depths.

    A host tool at ``tools/<tool>/``, a host product one level deeper under a
    category, and a firmware product at the same depth carrying the linker
    script + vector table that mark an image.
    """
    for rel in (
        "tools/widget",
        "apps/shared_libs/core",
        "apps/board/stand_alone/blinky",
    ):
        (root / rel).mkdir(parents=True)
        (root / rel / "CMakeLists.txt").write_text("project(x C)\n")
    source_only = root / "apps/shared_libs/source_only"
    source_only.mkdir(parents=True)
    (source_only / "CMakeLists.txt").write_text(
        "# Source-only composition note.\n"
        "#[[ A bracket comment may mention a command without defining one.\n"
        "project(not_a_real_project C)\n"
        "]]\n"
    )
    (root / "apps/board/stand_alone/blinky/linker_script.ld").write_text("MEMORY {}\n")
    firmware_src = root / "apps/board/stand_alone/blinky/src"
    firmware_src.mkdir()
    (firmware_src / "vector_table.c").write_text("int v;\n")


def _discovery_selftest() -> list[str]:
    """Drive discovery over a fixture tree; return failure lines.

    Both directions, and through the REAL discriminator: the firmware set is
    computed by ``lint_targets.firmware_app_dirs()`` over the fixture's paths
    rather than hand-written here, so a change that stopped classifying
    firmware apps fails this test instead of sailing through it.
    """
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as name:
        root = Path(name)
        _discovery_fixture(root)
        paths = [str(path.relative_to(root)) for path in root.rglob("*") if path.is_file()]
        firmware = firmware_app_dirs(paths)
        if firmware != ("apps/board/stand_alone/blinky",):
            failures.append(f"  discovery: firmware apps {firmware}, want the blinky image")
        got = cmake_tool_projects(root, firmware=firmware)
        want = ("apps/shared_libs/core", "tools/widget")
        if got != want:
            failures.append(f"  discovery: host projects {got}, want {want}")
    return failures


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

    for name, projects, sources, want_missing in PROJECT_SELFTEST_CASES:
        got_missing = missing_tool_projects(projects, sources)
        if got_missing != want_missing:
            failures.append(
                f"  project scope {name}: expected missing {want_missing}, got {got_missing}"
            )

    failures += _discovery_selftest()

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
        f"{len(PROJECT_SELFTEST_CASES)} project-scope cases; "
        "discovery excludes firmware apps and comment-only source libraries "
        "while keeping host products at category depth)"
    )
    return 0


def _scan_databases(paths: list[str]) -> tuple[list[str], set[str], set[str]]:
    """Scan every database path.

    Returns (violations, compiler families seen, sources compiled).

    Exits (SystemExit 2) when a path is missing, matching check_database's
    fatal handling of an empty or unreadable database -- a gate must never
    report a pass for a database it could not inspect.
    """
    violations: list[str] = []
    seen: set[str] = set()
    sources: set[str] = set()
    for name in paths:
        path = Path(name)
        if not path.is_file():
            sys.stderr.write(
                f"check_tool_warning_flags.py: FATAL -- {path} does not exist.\n"
                "  Configure the tool with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first.\n"
            )
            raise SystemExit(2)
        db_violations, db_compilers, db_sources = check_database(path)
        violations += db_violations
        seen |= db_compilers
        sources |= db_sources
    return violations, seen, sources


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


def _discover_projects() -> tuple[str, ...]:
    """Host CMake projects in the tree, enforcing the non-vacuity floor.

    The floor is checked here rather than at the call site so a collapsed glob
    can never be mistaken for "nothing is missing": `cmake_tool_projects`
    returning an empty tuple makes `missing_tool_projects` return an empty
    list, which is indistinguishable from a clean pass.

    Raises:
        SystemExit: 2 when discovery falls below MIN_CMAKE_TOOL_PROJECTS,
            matching _scan_databases' fatal handling of an input it could not
            trust.
    """
    projects = cmake_tool_projects(Path.cwd())
    if len(projects) >= MIN_CMAKE_TOOL_PROJECTS:
        return projects
    sys.stderr.write(
        f"check_tool_warning_flags.py: FATAL -- discovered {len(projects)} host "
        f"CMake project(s); the floor is {MIN_CMAKE_TOOL_PROJECTS}.\n"
        f"  found: {', '.join(projects) or '(none)'}\n"
        "  Discovery collapsed. An empty scope reports no missing project and\n"
        "  reads as a pass, which is the failure this floor exists to catch.\n"
    )
    raise SystemExit(2)


def _report_missing_projects(missing: list[str]) -> None:
    """Print the host CMake projects whose code the gate never compiled."""
    sys.stderr.write(
        "check_tool_warning_flags.py: FATAL -- host CMake project(s) whose "
        f"sources tools-build never compiled: {', '.join(missing)}\n"
        "  Every host CMake project must reach the gate, either through its\n"
        "  own build tree or by being composed into a sibling's with\n"
        "  add_subdirectory. A project nothing builds is a project nothing\n"
        "  holds to -Wall -Wextra -Werror.\n"
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


def _report_verdict(
    *,
    databases: list[str],
    required: list[str],
    violations: list[str],
    seen: set[str],
    missing_projects: list[str],
) -> int:
    """Render one discovery, warning, and compiler-arm verdict."""
    if missing_projects:
        _report_missing_projects(missing_projects)
        return 2
    if violations:
        _report_violations(violations)
        return 1
    missing = missing_required_compilers(seen, required)
    if missing:
        _report_missing_arm(missing, seen)
        return 1

    arms = f", arms: {', '.join(sorted(seen))}" if required else ""
    print(
        f"check_tool_warning_flags.py: OK ({len(databases)} compile "
        f"database(s), every first-party TU at -Wall -Wextra -Werror{arms})"
    )
    return 0


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
        help="fail when any host CMake project's sources are absent from the databases",
    )
    parser.add_argument(
        "--list-missing-cmake-tools",
        action="store_true",
        help="print uncovered host CMake project directories, one per line",
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

    projects: tuple[str, ...] = ()
    if args.require_all_cmake_tools or args.list_missing_cmake_tools:
        projects = _discover_projects()

    required = [fam for fam in args.require_compilers.split(",") if fam]
    violations, seen, sources = _scan_databases(args.databases)
    missing_projects = missing_tool_projects(projects, sources)
    if args.list_missing_cmake_tools:
        for project in missing_projects:
            print(project)
        return 0
    return _report_verdict(
        databases=args.databases,
        required=required,
        violations=violations,
        seen=seen,
        missing_projects=missing_projects,
    )


if __name__ == "__main__":
    sys.exit(main())
