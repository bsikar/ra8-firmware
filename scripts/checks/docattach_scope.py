# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Which files ``check_doc_attachment.py`` reads.

Split out of the checker (#359) so the scope is one small, readable thing
rather than five constants scattered above 1600 lines of rules.  A scope that
is hard to find is a scope nobody re-reads, and a checker whose roots quietly
stopped describing the tree is the defect this repository has now hit five
times -- see the ``check_lint_coverage.py`` docstring for the tally.
"""

from __future__ import annotations

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: First-party roots.  Mirrors the CLAUDE.md "Scope" note: every first-party
#: file, not just the firmware.  ``tools/`` and ``tests/`` are in deliberately
#: -- leaving them out is how a whole host emulator went ungated before.
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "apps", "tests")

#: Path fragments that mark non-first-party or generated trees.
EXCLUDED_PARTS = frozenset(
    {
        "third_party",
        "ra8_fonts",
        "build",
        "build-cov",
        "build-bench",
        "build-scan",
        "build-mcdc",
        "build-emu",
        "_deps",
        "CMakeFiles",
    }
)

SOURCE_SUFFIXES = (".c", ".h")


def iter_sources(targets: list[Path]) -> list[Path]:
    """Every in-scope source file under ``targets``."""
    out: list[Path] = []
    for target in targets:
        if target.is_file():
            if target.suffix in SOURCE_SUFFIXES:
                out.append(target)
            continue
        for dirpath, dirnames, filenames in os.walk(target):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDED_PARTS]
            for fn in filenames:
                if not fn.endswith(SOURCE_SUFFIXES):
                    continue
                p = Path(dirpath) / fn
                if EXCLUDED_PARTS & set(p.relative_to(REPO_ROOT).parts):
                    continue
                out.append(p)
    return sorted(set(out))


def default_targets() -> list[Path]:
    """Every first-party root that exists in this checkout."""
    return [REPO_ROOT / r for r in SCAN_ROOTS if (REPO_ROOT / r).is_dir()]
