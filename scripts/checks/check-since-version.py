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

  2. **Value**: every ``@since`` tag in any tracked first-party
     file must use the exact version string in the project's
     top-level ``VERSION`` file. The tolerated variants are::

         @since 0.1.0
         @since Version 0.1.0   (legacy STAR-style; still accepted)

     Any other value is flagged.

The value half used to read only ``.c/.h/.cpp/.hpp``, which is the blind spot
#900 asks this gate to close: the repository now carries first-party Zig (with
``///`` doc comments), Rust host crates are planned, and the Markdown hub and
the shell/Python generators all write ``@since`` lines of their own. A tag is
authoritative-looking wherever it is written, so the value check now reads every
tracked first-party file (``lint_targets.first_party_paths``), independent of
language. Two real mismatches were hiding behind the old suffix list: a tag
naming a release the project never cut in the ``CLAUDE.md`` house-style example,
and the same tag baked into the ``library.h`` stub that
``scripts/builders/all_examples.sh`` emits, i.e. a generated header claiming a
version that does not exist.

**Presence** stays scoped to the public C ABI (``libs/ra8_*/inc/*.h``) on
purpose: that is the versioned contract. A Zig implementation behind an
unchanged C header makes no separate API promise, so demanding a tag on its
declarations would be noise, and ``check_zig_doc_comments.py`` already requires
``///`` prose there.

A tag that is deliberately illustrative -- a regex example in a docstring, a
selftest fixture, a templated placeholder -- carries an inline
``SINCE-EXAMPLE-OK: <reason>`` marker on the same line, following the tree's
``PATHREF-OK`` convention. The reason is mandatory and must start with a letter
or digit, so neither a bare marker nor one trailed by a comment closer can
switch the check off.

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

# The C-family suffixes the PRESENCE half understands. The value half is not
# suffix-scoped any more, see ANY_SUFFIX below.
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

# Tree-wide scope for the value half. ``first_party_paths`` matches with
# ``str.endswith``, so the empty string keeps every tracked first-party path:
# the enumeration is derived from git, never a suffix list that goes stale
# without failing (the defect lint_targets.py was written to end).
ANY_SUFFIX = ("",)

# Inline exemption for a deliberately illustrative tag. Mandatory reason on the
# same line, mirroring the tree's ``PATHREF-OK`` convention. The reason must
# START with a letter or digit: ``SINCE-EXAMPLE-OK: -->`` inside a Markdown
# comment is a bare marker wearing a comment closer, not a reason, and it must
# not switch the check off.
SINCE_EXAMPLE_OK = re.compile(r"SINCE-EXAMPLE-OK:\s*[A-Za-z0-9]")

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
# Match ``@since 1.2.3``/``@since Version 1.2.3``. SINCE-EXAMPLE-OK: regex forms
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
    """Any first-party file: every @since's value must match project_version."""
    problems: list[str] = []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return problems

    for line_no, line in enumerate(text.splitlines(), start=1):
        m = SINCE_VALUE.search(line)
        if not m:
            continue
        if SINCE_EXAMPLE_OK.search(line):
            # Illustrative, and it says why on this line. Never a bare opt-out.
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
    """Every tracked first-party file, for the ``--all`` whole-tree sweep.

    Derived from git ls-files via first_party_paths (#358): the value check --
    every ``@since`` must equal the single ``VERSION`` string -- reaches tools/,
    port/usbx and every future top-level directory, which the old hardcoded
    libs/src/tests + example-app list silently omitted.

    Language-independent since #900: the suffix filter is gone, so a ``@since``
    in a Zig ``///`` comment, a Rust crate, a Markdown page of the docs hub or a
    shell/Python generator is checked exactly like one in a C header. The
    presence check still fires only on libs/ra8_*/inc/ headers via
    ``is_under_lib_inc``, so nothing new is *required* to carry a tag -- only its
    value is validated. Without the flag the gate reads only the paths it is
    handed, which is how the pre-commit hook stays cheap.
    """
    return [REPO_ROOT / rel for rel in first_party_paths(ANY_SUFFIX)]


# ---------------------------------------------------------------------------
# Selftest -- both directions, plus a scope assertion under tools/, silently
# omitted by the old scan-dir list until #358.
# ---------------------------------------------------------------------------
def selftest() -> int:
    """Prove a wrong @since fires, a right one is quiet, and the scope holds."""
    print("check-since-version.py --selftest")
    failures: list[str] = []
    version = read_project_version()
    # One marked constant, so the fixture version appears exactly once in this
    # file. Every probe below interpolates it: a literal on each line would need
    # a marker on each line, and six opt-outs is how an exemption becomes a
    # habit. SINCE-EXAMPLE-OK: negative-direction fixture, never a real release
    wrong = "9.9" + ".9"
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.c"
        bad.write_text(f"/** @since {wrong} */\n", encoding="utf-8")
        good = pathlib.Path(tmp) / "good.c"
        good.write_text(f"/** @since {version} */\n", encoding="utf-8")
        expect(bool(check_values(bad, version)), "a wrong @since value fires", failures)
        expect(not check_values(good, version), "the correct @since value stays quiet", failures)
        hdr = pathlib.Path(tmp) / "decl.h"
        hdr.write_text("ra8_err_t ra8_foo(void);\n", encoding="utf-8")
        expect(bool(check_presence(hdr)), "a public decl missing @since fires", failures)

        # #900: the value half is language-independent. One probe per first-party
        # language that can carry a tag, because the old suffix list silently
        # exempted all of them.
        for name, body in (
            ("unit.zig", f"/// @since {wrong}\npub fn f() void {{}}\n"),
            ("page.md", f"` * @since Version {wrong}`\n"),
            ("gen.sh", f'printf " * @since {wrong}\\n"\n'),
            ("lib.rs", f"//! @since {wrong}\n"),
        ):
            probe = pathlib.Path(tmp) / name
            probe.write_text(body, encoding="utf-8")
            expect(
                bool(check_values(probe, version)),
                f"a wrong @since fires in {name} (was exempt before #900)",
                failures,
            )
        right_zig = pathlib.Path(tmp) / "right.zig"
        right_zig.write_text(f"/// @since {version}\npub fn f() void {{}}\n", encoding="utf-8")
        expect(not check_values(right_zig, version), "a correct @since in Zig is quiet", failures)
        expect(
            not check_presence(right_zig) and not is_under_lib_inc(right_zig),
            "presence stays scoped to the public C ABI, not Zig declarations",
            failures,
        )

        marked = pathlib.Path(tmp) / "marked.md"
        marked.write_text(
            f"@since {wrong}  <!-- SINCE-EXAMPLE-OK: doc example -->\n", encoding="utf-8"
        )
        expect(not check_values(marked, version), "a marker with a reason exempts a tag", failures)
        bare = pathlib.Path(tmp) / "bare.md"
        bare.write_text(f"@since {wrong}  <!-- SINCE-EXAMPLE-OK: -->\n", encoding="utf-8")
        expect(
            bool(check_values(bare, version)),
            "a marker with no reason does NOT exempt a tag",
            failures,
        )
        elsewhere = pathlib.Path(tmp) / "elsewhere.md"
        elsewhere.write_text(f"SINCE-EXAMPLE-OK: above\n@since {wrong}\n", encoding="utf-8")
        expect(
            bool(check_values(elsewhere, version)),
            "the marker only exempts its own line",
            failures,
        )

    scope = set(first_party_paths(ANY_SUFFIX))
    expect(
        any(s.endswith(".md") for s in scope) and any(s.endswith(".py") for s in scope),
        "the value scope is tree-wide, not the four C-family suffixes (#900)",
        failures,
    )
    expect(
        any(s.startswith("tools/") for s in scope),
        "tools/ is in scope (the scan-dir list omitted it before #358)",
        failures,
    )
    expect(
        not any(
            s.startswith(("libs/third_party/", "apps/shared_libs/third_party/")) for s in scope
        ),
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

    arguments = sys.argv[1:]
    if arguments and arguments[0] == "--all":
        paths = collect_repo_paths()
    elif arguments:
        paths = [pathlib.Path(p).resolve() for p in arguments]
    else:
        print("usage: check-since-version.py FILE [FILE ...] | --all", file=sys.stderr)
        return 2

    failures: list[str] = []
    for path in paths:
        if not path.is_file():
            continue
        if is_under_lib_inc(path):
            failures.extend(check_presence(path))
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
