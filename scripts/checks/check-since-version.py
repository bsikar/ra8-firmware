#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce ``@since`` Doxygen tags and check their values against VERSION.

Both halves matter: a missing tag is a documentation gap, and a tag naming a
version the project never released is worse, because it looks authoritative.

Two checks combined:

  1. **Presence**: every public declaration in a `.h` under
     ``libs/ra8_*/inc/`` (i.e. every ``ra8_*`` function or static
     inline accessor) must be preceded within the previous 30
     lines by a ``@since`` tag inside its Doxygen block.

  2. **Value**: every ``@since`` tag in any source / header /
     example / test file must use the exact version string in
     the project's top-level ``VERSION`` file. The tolerated
     variants are::

         @since 0.1.0
         @since Version 0.1.0   (legacy STAR-style; still accepted)

     Any other value is flagged.

Usage:

    # explicit file list (used by pre-commit hook):
    python3 scripts/checks/check-since-version.py path/to/file.h ...

    # full repo sweep (CI):
    python3 scripts/checks/check-since-version.py --all

The script always reads ``VERSION`` from the repo root, so a
single bump there propagates everywhere.

Exit code:
    0  no issues
    1  presence or value mismatch found
    2  CLI usage error
"""

from __future__ import annotations

import pathlib
import re
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSION_FILE = REPO_ROOT / "VERSION"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

PUBLIC_DECL = re.compile(
    r"""
    ^(?:\[\[nodiscard\]\]\s+)?
    (?:static\s+inline\s+)?
    \s*ra8_\w+(?:\s*\*)?\s+
    (ra8_\w+)\s*
    \(
""",
    re.VERBOSE,
)

SINCE_TAG_PRESENT = re.compile(r"@since")
# Match ``@since 1.2.3`` or ``@since Version 1.2.3``; capture the version.
SINCE_VALUE = re.compile(r"@since\s+(?:Version\s+)?([0-9]+(?:\.[0-9]+){1,2}[a-z]?)")


def read_project_version() -> str:
    """Read the single version string from the VERSION file.

    Raises rather than defaulting when the file is missing: every ``@since``
    comparison is against this value, so a default would silently validate
    every tag in the tree against a number nobody chose.
    """
    if not VERSION_FILE.is_file():
        msg = f"error: {VERSION_FILE} missing -- create it with a single semver line"
        raise SystemExit(msg)
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", text):
        msg = f"error: {VERSION_FILE} content '{text}' is not semver MAJOR.MINOR.PATCH"
        raise SystemExit(msg)
    return text


def check_presence(path: pathlib.Path) -> list[str]:
    """Header-only: every ra8_* declaration must have @since in lookback."""
    problems: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        return problems

    for i, line in enumerate(lines):
        match = PUBLIC_DECL.match(line)
        if not match:
            continue
        lookback = "\n".join(lines[max(0, i - 30) : i])
        if not SINCE_TAG_PRESENT.search(lookback):
            problems.append(f"{path}:{i + 1}: {match.group(1)} missing @since")
    return problems


def check_values(path: pathlib.Path, project_version: str) -> list[str]:
    """All sources: every @since's value must match project_version."""
    problems: list[str] = []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return problems

    for line_no, line in enumerate(text.splitlines(), start=1):
        m = SINCE_VALUE.search(line)
        if not m:
            continue
        if m.group(1) != project_version:
            problems.append(f"{path}:{line_no}: @since {m.group(1)} != project {project_version}")
    return problems


def is_under_lib_inc(path: pathlib.Path) -> bool:
    """Whether this is a public library header, where ``@since`` is mandatory.

    The tag is required only on the public contract: an ``inc/`` header of an
    ``ra8_*`` library. Implementation files carry no API promise, so demanding
    a version tag on them would be noise.
    """
    return "libs/ra8_" in str(path) and path.suffix == ".h" and "/inc/" in str(path)


def collect_repo_paths() -> list[pathlib.Path]:
    """Every first-party source file, for the ``--all`` whole-tree sweep.

    Derived from git ls-files via first_party_paths (#358): the value check --
    every ``@since`` must equal the single ``VERSION`` string -- now reaches
    tools/, port/usbx and every future top-level directory, which the old
    hardcoded libs/src/tests + example-app list silently omitted. The presence
    check still fires only on libs/ra8_*/inc/ headers via ``is_under_lib_inc``,
    so nothing else is newly *required* to carry a tag -- only its value is
    validated. Without the flag the gate reads only the paths it is handed,
    which is how the pre-commit hook stays cheap.
    """
    return [REPO_ROOT / rel for rel in first_party_paths(SOURCE_SUFFIXES)]


# ---------------------------------------------------------------------------
# Selftest -- both directions, plus a scope assertion under tools/, silently
# omitted by the old scan-dir list until #358.
# ---------------------------------------------------------------------------
def selftest() -> int:
    """Prove a wrong @since fires, a right one is quiet, and the scope holds."""
    print("check-since-version.py --selftest")
    failures: list[str] = []
    version = read_project_version()
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.c"
        bad.write_text("/** @since 9.9.9 */\n", encoding="utf-8")
        good = pathlib.Path(tmp) / "good.c"
        good.write_text(f"/** @since {version} */\n", encoding="utf-8")
        expect(bool(check_values(bad, version)), "a wrong @since value fires", failures)
        expect(not check_values(good, version), "the correct @since value stays quiet", failures)
        hdr = pathlib.Path(tmp) / "decl.h"
        hdr.write_text("ra8_err_t ra8_foo(void);\n", encoding="utf-8")
        expect(bool(check_presence(hdr)), "a public decl missing @since fires", failures)

    scope = set(first_party_paths(SOURCE_SUFFIXES))
    expect(
        any(s.startswith("tools/") for s in scope),
        "tools/ is in scope (the scan-dir list omitted it before #358)",
        failures,
    )
    expect(
        not any(s.startswith("libs/third_party/") for s in scope),
        "vendored SOUP stays out of scope",
        failures,
    )
    return report(failures)


def main() -> int:
    """Check ``@since`` tags on public headers, staged files or the whole tree.

    Resolves the project version FIRST, before any scanning, so a malformed
    VERSION file fails immediately rather than after a full sweep whose
    verdict would have been meaningless anyway.

    Returns 0 when every public declaration carries a correct tag, 1 otherwise.
    """
    if "--selftest" in sys.argv[1:]:
        return selftest()

    project_version = read_project_version()

    if len(sys.argv) >= 2 and sys.argv[1] == "--all":  # noqa: PLR2004  # argv[1] presence check
        paths = collect_repo_paths()
    elif len(sys.argv) >= 2:  # noqa: PLR2004  # argv[1] presence check
        paths = [pathlib.Path(p).resolve() for p in sys.argv[1:]]
    else:
        print("usage: check-since-version.py FILE [FILE ...] | --all", file=sys.stderr)
        return 2

    failures: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        if is_under_lib_inc(path):
            failures.extend(check_presence(path))
        if path.suffix in SOURCE_SUFFIXES:
            failures.extend(check_values(path, project_version))

    if failures:
        print(f"check-since-version.py: project version is {project_version}", file=sys.stderr)
        for line in failures:
            print(line, file=sys.stderr)
        print(f"\n{len(failures)} issue(s) found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
