#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Hold every Non-Secure image of a dual-image app to the first-party warning profile.

WHY THIS EXISTS
===============
``ra8_add_app()`` applies the canonical first-party warning profile --
``-Wall -Wextra -Werror`` plus the stack-usage bar -- to the Secure image it
creates.  A dual-image (TrustZone) app builds its Non-Secure half as a raw
``add_executable()`` instead, and a raw target inherits none of that: it
compiles with no warning flags at all.  The author of every such app has had
to know this and write the profile call by hand, in a comment they also had
to write:

    # The NS image is a raw add_executable, so unlike the Secure side (which
    # gets the profile via ra8_add_app) it would otherwise compile WITHOUT
    # -Wall/-Wextra/-Werror -- ... a discarded [[nodiscard]] ra8_err_t in
    # ns_main.c could never fail the build.

All four dual-image apps in the tree do currently make that call.  Nothing
enforced it, so a fifth app that forgot the line would silently lose -Werror
across half its code, with no diagnostic and nothing to notice it.
This checker is the enforcement half.

WHAT IT ENFORCES, PRECISELY
---------------------------
  * A listfile counts as dual-image when it carries one of the markers that
    only a two-image build has: ``--out-implib`` / ``--cmse-implib`` (the CMSE
    import-library handshake), or a ``merge_ihex.py`` / ``sign_and_merge.py``
    staple.  Nothing else in the tree produces those.
  * In such a listfile, EVERY target created by a raw ``add_executable()``
    must also be named by a ``ra8_target_enable_project_warnings()`` call in
    the same listfile.  A target that is not FAILS.
  * Targets created by ``ra8_add_app()`` are not judged: that helper applies
    the profile itself.  This checker only looks at raw ``add_executable()``.

SCOPE, HONESTLY
---------------
This is a listfile-text check, not a build-graph check.  It proves the call is
written, not that CMake reached it: a call parked behind a false ``if()``
would satisfy this checker, and it is worth stating plainly rather than
implying more.  It also does not read the stack-usage argument;
the bar per app is a judgement call, and the profile being applied at all is
what this gate is for.

The real fix is ``ra8_add_ns_image()``, which would own the NS
target's profile the way ``ra8_add_app()`` owns the Secure one, and close the
hole by construction instead of by inspection.  Until that lands, this is the
measurement.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SEARCH_ROOTS = ("apps", "examples")

DUAL_IMAGE_MARKERS = (
    "--out-implib",
    "--cmse-implib",
    "merge_ihex.py",
    "sign_and_merge.py",
)

ADD_EXECUTABLE_RE = re.compile(r"\badd_executable\s*\(\s*([A-Za-z0-9_.\-${}]+)")
WARNING_CALL_RE = re.compile(
    r"\bra8_target_enable_project_warnings\s*\(\s*([A-Za-z0-9_.\-${}]+)"
)


def is_dual_image(text: str) -> bool:
    """Report whether a listfile builds two images."""
    return any(marker in text for marker in DUAL_IMAGE_MARKERS)


def audit_text(text: str) -> list[str]:
    """Return the raw-add_executable targets in `text` lacking the profile call."""
    if not is_dual_image(text):
        return []
    made = ADD_EXECUTABLE_RE.findall(text)
    profiled = set(WARNING_CALL_RE.findall(text))
    return [t for t in made if t not in profiled]


def listfiles() -> list[Path]:
    """Every CMakeLists.txt under the search roots, sorted for stable output."""
    found: list[Path] = []
    for root in SEARCH_ROOTS:
        found.extend((REPO_ROOT / root).rglob("CMakeLists.txt"))
    return sorted(found)


def selftest() -> int:
    """Prove the checker fails a missing call and passes a present one."""
    missing = """
        target_link_options(app.elf PRIVATE -Wl,--out-implib=${_implib})
        add_executable(app_ns.elf src/ns_main.c)
    """
    present = missing + """
        ra8_target_enable_project_warnings(app_ns.elf STACK_USAGE_BYTES 2200)
    """
    single = "add_executable(host_tool.c) # no dual-image marker anywhere"
    multiline = """
        # -Wl,--cmse-implib
        add_executable(
          long_name_ns.elf ${CMAKE_CURRENT_SOURCE_DIR}/src/ns_main.c
        )
        ra8_target_enable_project_warnings(
          long_name_ns.elf STACK_USAGE_BYTES 2200
        )
    """
    cases = [
        ("missing call is caught", audit_text(missing) == ["app_ns.elf"]),
        ("present call passes", audit_text(present) == []),
        ("single-image listfile is not judged", audit_text(single) == []),
        ("multi-line forms are matched", audit_text(multiline) == []),
    ]
    bad = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}")
    if bad:
        print(f"{Path(__file__).name}: selftest FAILED")
        return 1
    print(f"{Path(__file__).name}: selftest passed ({len(cases)} case(s))")
    return 0


def main() -> int:
    """Audit every dual-image app and report the ones off the warning profile."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--selftest", action="store_true", help="run the checker's own test cases"
    )
    args = parser.parse_args()
    if args.selftest:
        return selftest()

    scanned = 0
    dual = 0
    failures: list[tuple[Path, str]] = []
    for path in listfiles():
        text = path.read_text(encoding="utf-8", errors="replace")
        scanned += 1
        if not is_dual_image(text):
            continue
        dual += 1
        failures.extend((path, target) for target in audit_text(text))

    name = Path(__file__).name
    if failures:
        print(f"\n{name}: Non-Secure image(s) outside the first-party warning profile\n")
        for path, target in failures:
            print(f"  {path.relative_to(REPO_ROOT)}: {target}")
        print(
            "\nA raw add_executable() inherits no warning flags, so this target\n"
            "compiles without -Wall/-Wextra/-Werror: a discarded [[nodiscard]]\n"
            "ra8_err_t in it can never fail the build. Add, in the same listfile:\n"
            "\n    ra8_target_enable_project_warnings(<target> STACK_USAGE_BYTES <n>)\n"
        )
        return 1

    print(f"{name}: {scanned} listfile(s) scanned, {dual} dual-image, all NS images profiled.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
