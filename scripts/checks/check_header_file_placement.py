#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a header under a ``src/`` directory shall be module-private.

The repository splits every module into a public ``inc/`` directory (the
contract other translation units consume) and a private ``src/`` directory
(the implementation).  A header that lives under ``src/`` is therefore, by
construction, module-private -- and must announce that with an ``_internal``
suffix (``*_internal.h``).  A non-``_internal`` header under ``src/`` is one of
two mistakes:

  * it is actually a public interface that was filed in the wrong place and
    belongs in the module's ``inc/`` directory, or
  * it is genuinely private but was never given the ``_internal`` name that
    marks it as such.

Either way it is a defect.  This gate walks every first-party ``.h``/``.hpp``
under a ``src/`` directory and fails on any that do not end in ``_internal``.
Vendored trees (``libs/third_party/``) and generated font tables
(``libs/ra8_fonts/``) are out of scope, matching every other repo gate.

There is deliberately NO in-file waiver marker: the fix is to move the header
to ``inc/`` or rename it ``*_internal.h``, never to annotate an exception.

Run::

    check_header_file_placement.py                    # scan the whole tree
    check_header_file_placement.py path/to/file.h ... # scan listed files
    check_header_file_placement.py --selftest         # prove both directions

Exit 0 if every ``src/`` header is ``*_internal``, exit 1 (with a table)
otherwise.
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".hxx")
SCAN_ROOTS = ("libs", "port", "examples", "tools", "apps", "tests")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "apps/shared_libs/third_party/",
    # Generated font tables are data artifacts, not hand-authored module interfaces.
    "libs/ra8_fonts/",
)

# The suffix that marks a src/ header as intentionally module-private.
INTERNAL_STEM_SUFFIX = "_internal"

# A whole-tree pass below this measured population has lost scope and must fail.
MIN_PRIVATE_HEADERS = 100


def _is_excluded(path: Path) -> bool:
    return is_build_output_path(path) or any(frag in str(path) for frag in EXCLUDE_FRAGMENTS)


def _is_header(path: Path) -> bool:
    return path.suffix in HEADER_SUFFIXES


def _governing_dir(path: Path) -> str | None:
    """Return the nearest ``inc``/``src`` ancestor component, or None.

    A module may nest an ``inc`` inside a ``src`` tree (e.g.
    ``libs/ra8_secure_app/inc/key_vault.h``): the *closest* such component to the
    file decides whether it is public (``inc``) or private (``src``), so a
    higher ``src`` does not condemn a header that sits in a deeper ``inc``.
    """
    for part in reversed(path.parent.parts):
        if part in ("inc", "src"):
            return part
    return None


def _under_src(path: Path) -> bool:
    """True if the header's nearest inc/src ancestor is a private ``src``."""
    return _governing_dir(path) == "src"


def _is_internal(path: Path) -> bool:
    """True if the header's stem ends in the ``_internal`` marker."""
    return path.stem.endswith(INTERNAL_STEM_SUFFIX)


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    """Resolve the list of headers to scan from CLI arguments."""
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                for suffix in HEADER_SUFFIXES:
                    out.extend(path.rglob("*" + suffix))
            elif _is_header(path):
                out.append(path)
        return [p for p in out if not _is_excluded(p)]

    out = []
    for root in SCAN_ROOTS:
        for suffix in HEADER_SUFFIXES:
            out.extend((REPO_ROOT / root).rglob("*" + suffix))
    return [p for p in out if not _is_excluded(p)]


def _audit_targets(targets: Iterable[Path]) -> tuple[int, list[str]]:
    """Return the private-header count and misplaced relative paths."""
    scanned = 0
    offenders: list[str] = []
    for path in targets:
        if not _under_src(path):
            continue
        scanned += 1
        if not _is_internal(path):
            offenders.append(_rel(path))
    return scanned, sorted(offenders)


def _whole_tree_census_ok(scanned: int, *, explicit_paths: bool) -> bool:
    """Return whether the private-header census is non-vacuous for this mode."""
    return explicit_paths or scanned >= MIN_PRIVATE_HEADERS


def selftest() -> int:
    """Prove private/public placement fires and stays quiet without scope leaks."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-header-placement-") as temp:
        root = Path(temp)
        good = root / "tests/module/src/widget_internal.h"
        bad = root / "tests/module/src/widget.h"
        nested_public = root / "tests/module/src/sub/inc/public.h"
        vendor = root / "libs/third_party/vendor/src/public.h"
        app_vendor = root / "apps/shared_libs/third_party/vendor/src/public.h"
        generated_font = root / "libs/ra8_fonts/src/generated.h"
        build = root / "CMakeFiles/src/generated.h"
        for path in (good, bad, nested_public, vendor, app_vendor, generated_font, build):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="ascii")
        targets = _enumerate_targets([str(root)])
        scanned, offenders = _audit_targets(targets)
        expected_scanned = len((good, bad))
        if scanned != expected_scanned or offenders != [_rel(bad)]:
            failures.append(
                f"mixed fixture scanned={scanned}, offenders={offenders!r}; expected bad only"
            )
        quiet_scanned, quiet = _audit_targets([good, nested_public])
        if quiet_scanned != 1 or quiet:
            failures.append("private _internal.h or nearest nested inc/ did not stay quiet")
        excluded = {vendor, app_vendor, generated_font, build}
        if excluded & set(targets):
            failures.append("vendor, generated-font, or build exclusion leaked into the scan")
        if not _is_excluded(Path("tests/module/build/src/generated.h")):
            failures.append("tests/ build-tree output is not excluded")
        if "tests" not in SCAN_ROOTS:
            failures.append("tests/ is absent from the repository-wide scan roots")
        if _whole_tree_census_ok(MIN_PRIVATE_HEADERS - 1, explicit_paths=False):
            failures.append("collapsed whole-tree private-header census did not fail")
        if not _whole_tree_census_ok(0, explicit_paths=True):
            failures.append("explicit-file mode incorrectly requires the whole-tree floor")
    if failures:
        for failure in failures:
            print(f"  [FAIL] {failure}", file=sys.stderr)
        return 1
    print("check_header_file_placement.py --selftest: PASS (fire, quiet, tests, exclusions)")
    return 0


def _parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse the CLI so misspelled options fail instead of becoming paths."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("paths", nargs="*")
    return parser.parse_args(argv[1:])


def main(argv: list[str]) -> int:
    """Fail any header under a ``src/`` directory not named ``*_internal.h``.

    The scanned count reported on success is the number of headers actually
    UNDER a src/ directory, not the number handed in: everything else is
    filtered out first, so passing the whole staged file list is normal and
    the printed total will legitimately be far smaller than argv.

    Returns 1 listing each misplaced header, 0 when every src/ header is
    module-private or when nothing in scope reached the filter.
    """
    args = _parse_args(argv)
    if args.selftest:
        if args.paths:
            print("--selftest does not accept paths", file=sys.stderr)
            return 2
        return selftest()
    targets = _enumerate_targets(args.paths)
    if not targets and args.paths:
        print("check_header_file_placement.py: no headers to scan", file=sys.stderr)
        return 0

    scanned, offenders = _audit_targets(targets)
    if not _whole_tree_census_ok(scanned, explicit_paths=bool(args.paths)):
        print(
            "check_header_file_placement.py: whole-tree scan reached only "
            f"{scanned} private header(s), below floor {MIN_PRIVATE_HEADERS}",
            file=sys.stderr,
        )
        return 1

    if not offenders:
        print(
            f"check_header_file_placement.py: {scanned} src/ header(s) scanned, "
            "all module-private (*_internal.h)."
        )
        return 0

    print(
        f"check_header_file_placement.py: {len(offenders)} src/ header(s) are not *_internal.h:\n",
        file=sys.stderr,
    )
    for path in offenders:
        print(f"  {path}", file=sys.stderr)
    print(
        "\nA header under a src/ directory is module-private and must say so.\n"
        "For each offender, decide which it is and fix at the root:\n"
        "  - public interface (consumed outside the module) -> move it to the\n"
        "    module's inc/ directory;\n"
        "  - genuinely module-private -> rename it '*_internal.h'.\n"
        "Update every #include of the header in the same change.  There is no\n"
        "waiver marker -- placement is the contract.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
