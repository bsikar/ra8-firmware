#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: ``libs/ra8_core`` is the foundation layer and depends on no other lib.

``ra8_core`` (error codes, checks, logging, assert, attributes, ...) sits at the
bottom of the library stack: every other ``libs/*`` module is allowed to build
on it, but it must build on nothing above itself. An ``#include`` from a
``ra8_core`` file that resolves to a header owned by another first-party lib is
an *upward* dependency -- it inverts the layering and creates a cycle risk.

#243 was one such edge (``ra8_register_protection.h`` pulled ``ra8_hal``'s
system-register header into ``ra8_core``); it was fixed by moving the header
into ``ra8_hal``. This gate makes the invariant permanent: it fails if any file
under ``libs/ra8_core`` includes a header owned by a different ``libs/*``
module. System headers (``<...>``), vendored third-party headers, and
``ra8_core``'s own headers are all fine.

Run::

    check_core_layering.py                  # scan libs/ra8_core
    check_core_layering.py path/to/file.c   # scan listed files

Exit 0 if ``ra8_core`` has no upward lib dependency, exit 1 (with the
offending edges) otherwise.
"""

from __future__ import annotations

import re
import sys
from collections.abc import Iterable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]
LIBS_ROOT = REPO_ROOT / "libs"

FOUNDATION_LIB = "ra8_core"
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")
INCLUDE_RE = re.compile(r'#\s*include\s*[<"]([^">]+)[">]')
EXCLUDE_FRAGMENTS = ("/third_party/", "/fonts/")

# A lib header path under libs/ is <module>/<inc|src>/...: at least the module
# component and the inc/src component must be present to attribute an owner.
_MIN_LIB_PATH_PARTS = 2


def _header_owners() -> dict[str, set[str]]:
    """Map each first-party lib header basename to the lib module(s) owning it.

    A lib module is the first path component under ``libs/`` (e.g. ``ra8_hal``).
    """
    owners: dict[str, set[str]] = {}
    for header in LIBS_ROOT.rglob("*.h"):
        text = str(header)
        if is_build_output_path(text) or any(frag in text for frag in EXCLUDE_FRAGMENTS):
            continue
        rel = header.relative_to(LIBS_ROOT).parts
        # rel[0] is the lib module; only count headers under a lib's inc/ or src/
        if len(rel) < _MIN_LIB_PATH_PARTS or rel[1] not in ("inc", "src"):
            continue
        owners.setdefault(header.name, set()).add(rel[0])
    return owners


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                for suffix in SOURCE_SUFFIXES:
                    out.extend(path.rglob("*" + suffix))
            elif _is_source(path):
                out.append(path)
        # only files that actually live under the foundation lib matter
        base = REPO_ROOT / "libs" / FOUNDATION_LIB
        return [p for p in out if p.is_relative_to(base)]

    root = REPO_ROOT / "libs" / FOUNDATION_LIB
    out = []
    for suffix in SOURCE_SUFFIXES:
        out.extend(root.rglob("*" + suffix))
    return out


def main(argv: list[str]) -> int:
    """Fail if anything in the foundation lib includes a header another lib owns.

    ra8_core sits at the bottom of the stack, so an include pointing UP at a
    higher lib is a dependency inversion that would make the foundation
    unbuildable on its own. A header owned by several libs INCLUDING ra8_core
    is fine -- that is a shared name, not an upward edge -- which is why
    ownership is compared as a set difference rather than by first match.

    Paths given on argv are filtered down to those under libs/ra8_core before
    scanning, so pointing this at the whole staged file list is safe and cheap.

    Returns 1 with the offending include sites listed, 0 when no upward edge
    exists or when the argv filter left nothing under the foundation lib.
    """
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_core_layering.py: no ra8_core files to scan", file=sys.stderr)
        return 0

    owners = _header_owners()
    edges = []
    for path in targets:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            match = INCLUDE_RE.search(line)
            if not match:
                continue
            name = Path(match.group(1)).name
            mods = owners.get(name, set())
            # owned by some other lib, and NOT (also) owned by ra8_core
            if mods and FOUNDATION_LIB not in mods and mods - {FOUNDATION_LIB}:
                edges.append((_rel(path), lineno, name, sorted(mods)))

    if not edges:
        print(
            f"check_core_layering.py: {len(targets)} ra8_core file(s) scanned, "
            "no upward lib dependency."
        )
        return 0

    print(
        f"check_core_layering.py: {len(edges)} upward dependency(ies) out of {FOUNDATION_LIB}:\n",
        file=sys.stderr,
    )
    for path, lineno, name, mods in edges:
        print(f"  {path}:{lineno}  includes {name}  (owned by {', '.join(mods)})", file=sys.stderr)
    print(
        f"\n{FOUNDATION_LIB} is the foundation layer: every other lib may build on\n"
        "it, but it must depend on nothing above itself. Move the shared\n"
        "declaration down into ra8_core, or keep the consumer in the higher lib.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
