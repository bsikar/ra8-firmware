# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Which files ``doxy_audit.py`` reads, in each of its two scopes.

The auditor deliberately has two: the function gate covers ``libs/``, ``src/``
and ``port/``, while the member gate is repo-wide.  That difference is a
recorded decision, not an oversight -- see the KNOWN SCOPE GAP note in
``doxy_audit``'s docstring, which measures what closing it would cost -- so
both lists live here, side by side, where the asymmetry is visible rather than
buried 800 lines apart.
"""

from __future__ import annotations

import contextlib
import os
from collections.abc import Iterator
from pathlib import Path

#: The real checkout root. Never read directly outside `repo_root()` -- the
#: selftest override would not be visible through a bare import of it.
_REAL_REPO_ROOT = Path(__file__).resolve().parents[2]

#: Active root; `override_repo_root()` swaps this for the duration of a block.
_repo_root = _REAL_REPO_ROOT


def repo_root() -> Path:
    """Return the root every path in this auditor is reported relative to."""
    return _repo_root


@contextlib.contextmanager
def override_repo_root(root: Path) -> Iterator[None]:
    """Report paths relative to ``root`` for the duration of the block.

    Used only by the selftest, which audits a synthetic tree in a temporary
    directory. It used to rebind a module-global ``repo_root()`` instead, which
    stopped working the moment the auditor was split across modules: the other
    modules hold their own imported copy, so the rebinding would be invisible
    and the suite would silently assert against the real checkout.
    """
    global _repo_root  # noqa: PLW0603  # single-homed override; see docstring
    previous = _repo_root
    _repo_root = root
    try:
        yield
    finally:
        _repo_root = previous


SCAN_DIRS = ["libs", "src", "port"]
EXCLUDE_PARTS = {"third_party", "build", ".git"}

# Top-level dirs scanned by the report-only member audit (--members). Wider
# than SCAN_DIRS on purpose: the member/enum/macro documentation bar is
# repo-wide (CLAUDE.md "these standards apply to EVERY first-party file"), so
# the fallout report must cover examples/, tools/, and tests/ too. Vendored
# SOUP (libs/third_party/) and generated tables (libs/fonts/) stay exempt, the
# same as the function audit.
MEMBER_SCAN_DIRS = ["libs", "src", "port", "examples", "tools", "tests"]
MEMBER_EXCLUDE_PARTS = {"third_party", "fonts", "build", "build-cov", "_deps", ".git"}

# Minimum path depth to form a two-segment module label (e.g. "libs/ra8_hal").
MODULE_PATH_MIN_DEPTH = 2


def iter_function_files() -> Iterator[Path]:
    """Yield every first-party .c/.h the FUNCTION gate audits.

    Both the strict gate and the report generator walked this identically
    inline; sharing one iterator means the report can never describe a
    different set of files than the gate enforces over.
    """
    for top in SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in EXCLUDE_PARTS]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = Path(dirpath) / fn
                if any(part in EXCLUDE_PARTS for part in p.relative_to(repo_root()).parts):
                    continue
                yield p


def _iter_member_files(explicit: list[str]) -> Iterator[Path]:
    """Yield first-party .c/.h paths for the member audit.

    ``explicit`` is a list of user-supplied paths; if non-empty those exact
    files are audited, otherwise MEMBER_SCAN_DIRS is walked.
    """
    if explicit:
        for raw_path in explicit:
            p = Path(raw_path)
            if p.is_file():  # relative to CWD or absolute
                yield p
            elif (repo_root() / raw_path).is_file():  # relative to the repo root
                yield repo_root() / raw_path
        return
    for top in MEMBER_SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in MEMBER_EXCLUDE_PARTS]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = Path(dirpath) / fn
                if any(part in MEMBER_EXCLUDE_PARTS for part in p.relative_to(repo_root()).parts):
                    continue
                yield p


def _top_dir(rel: str) -> str:
    """Top-level directory of a repo-relative path (e.g. "libs")."""
    return rel.split("/", 1)[0]
