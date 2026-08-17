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

Exit 0 if every ``src/`` header is ``*_internal``, exit 1 (with a table)
otherwise.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".hxx")
SCAN_ROOTS = ("libs", "port", "examples", "tools", "apps")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
)

# The suffix that marks a src/ header as intentionally module-private.
INTERNAL_STEM_SUFFIX = "_internal"


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


def main(argv: list[str]) -> int:
    """Fail any header under a ``src/`` directory not named ``*_internal.h``.

    The scanned count reported on success is the number of headers actually
    UNDER a src/ directory, not the number handed in: everything else is
    filtered out first, so passing the whole staged file list is normal and
    the printed total will legitimately be far smaller than argv.

    Returns 1 listing each misplaced header, 0 when every src/ header is
    module-private or when nothing in scope reached the filter.
    """
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_header_file_placement.py: no headers to scan", file=sys.stderr)
        return 0

    scanned = 0
    offenders = []
    for path in targets:
        if not _under_src(path):
            continue
        scanned += 1
        if not _is_internal(path):
            offenders.append(_rel(path))

    if not offenders:
        print(
            f"check_header_file_placement.py: {scanned} src/ header(s) scanned, "
            "all module-private (*_internal.h)."
        )
        return 0

    offenders.sort()
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
