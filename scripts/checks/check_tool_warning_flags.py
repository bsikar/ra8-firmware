#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Assert the host tools compile first-party sources at the project warning bar.

Why this exists
---------------
``tools/media_dl`` compiled ``libs/ra8_jof/*``, ``libs/ra8_hal/ra8_jpeg_sw*``,
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

Vendored SOUP (``libs/third_party/``) and generated data (``libs/fonts/``) are
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

Usage
-----
    check_tool_warning_flags.py COMPILE_COMMANDS_JSON [...]
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
EXEMPT_FRAGMENTS = ("libs/third_party/", "libs/fonts/")


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


def check_database(path: Path) -> list[str]:
    """Return violation lines for one compile_commands.json."""
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
    for entry in entries:
        source = str(entry.get("file", ""))
        if not source or is_exempt(source):
            continue
        problem = flag_problem(tokens_of(entry))
        if problem is not None:
            violations.append(f"{source}: {problem}")
    return violations


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
        "tools/media_dl/src/main.c",
        ["cc", "-Wall", "-Wextra", "-Werror=return-type", "-c"],
        True,
    ),
    (
        "first-party whose -Werror is cancelled later",
        "tools/media_dl/src/mdl_export.c",
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
        "libs/fonts/ra8_font_dejavu.c",
        ["cc", "-w", "-c"],
        False,
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

    if failures:
        sys.stderr.write("check_tool_warning_flags.py --selftest: FAILED\n")
        sys.stderr.write("\n".join(failures) + "\n")
        return 1

    fires = sum(1 for case in SELFTEST_CASES if case[3])
    quiet = len(SELFTEST_CASES) - fires
    print(
        f"check_tool_warning_flags.py --selftest: PASS "
        f"({fires} must-fire, {quiet} must-stay-quiet cases)"
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

    violations: list[str] = []
    checked = 0
    for name in args.databases:
        path = Path(name)
        if not path.is_file():
            sys.stderr.write(
                f"check_tool_warning_flags.py: FATAL -- {path} does not exist.\n"
                "  Configure the tool with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first.\n"
            )
            return 2
        violations += check_database(path)
        checked += 1

    if violations:
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
        return 1

    print(
        f"check_tool_warning_flags.py: OK ({checked} compile database(s), "
        "every first-party TU at -Wall -Wextra -Werror)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
