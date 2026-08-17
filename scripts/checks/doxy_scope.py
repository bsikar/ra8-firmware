# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Which files ``doxy_audit.py`` reads, in each of its two scopes.

The function gate covers every production C root, including all of ``tools/``.
The member gate is wider still because it also covers examples and tests. The
function debt that existed when tools entered the scope is frozen in the
function baseline; excluding a tool until it became perfect made new gaps in
that tool invisible and contradicted the repository-wide policy.

Each scope also owns its own vacuity floor, and :func:`function_files` /
:func:`member_files` are the materialising accessors every caller must use.
A generator that yields nothing costs an auditor nothing to consume and makes
it print ``gaps=0 (PASS)`` -- a perfectly documented tree and a completely
broken walk are the same output. The floors live beside the scope lists they
guard because that is the pair that has to stay consistent; both exit 2, so
"the scan broke" stays distinguishable from "the tree is undocumented".
"""

from __future__ import annotations

import contextlib
import os
import sys
from collections.abc import Iterator
from pathlib import Path

from lint_targets import is_build_output_path

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


SCAN_DIRS = ["libs", "src", "port", "tools", "apps"]

# ``generated`` joins the exclusions so machine-emitted tool code (e.g.
# tools/vela/generated/) is exempt exactly as vendored SOUP is: it is not
# hand-authored, so the hand-documentation bar does not apply to it. No
# directory named ``generated`` exists under libs/, src/ or port/, so adding it
# here changes only what the tools/ root contributes.
EXCLUDE_PARTS = {"third_party", "generated", "build", ".git"}

# Top-level dirs scanned by the report-only member audit (--members). Wider
# than SCAN_DIRS on purpose: the member/enum/macro documentation bar is
# repo-wide (CLAUDE.md "these standards apply to EVERY first-party file"), so
# the fallout report must cover examples/, tools/, and tests/ too. Vendored
# SOUP (libs/third_party/) and generated tables (libs/ra8_fonts/) stay exempt, the
# same as the function audit.
MEMBER_SCAN_DIRS = ["libs", "src", "port", "examples", "tools", "apps", "tests"]
MEMBER_EXCLUDE_PARTS = {"third_party", "ra8_fonts", "build", "build-cov", "_deps", ".git"}

# Exact files emitted by protoc-c from the reviewed schema. Generated protocol
# code is compiled and tested, but requiring hand-authored Doxygen on every
# generated declaration would be both unstable and overwritten on regeneration.
# Keep this as an exact allow-list: neighboring handwritten RPC code remains in
# both audits.
GENERATED_PROTOCOL_FILES = {
    "libs/ra8_c6link/inc/ra8_media_download.pb-c.h",
    "libs/ra8_c6link/src/ra8_media_download.pb-c.c",
}

# A tree this size cannot legitimately collapse to a handful of files. If
# either walk returns less than its floor, something broke (an unreachable
# repo root, a renamed SCAN_DIRS entry) and reporting zero gaps would be a
# lie -- an undocumented function cannot be found in a file nobody opened.
# Measured 2026-07-28: 885 files in the function scope, 2116 in the member
# scope. Same trip-wire as lint_targets.TRACKED_FLOOR.
FUNCTION_FILE_FLOOR = 700
MEMBER_FILE_FLOOR = 1700

# Minimum path depth to form a two-segment module label (e.g. "libs/ra8_hal").
MODULE_PATH_MIN_DEPTH = 2


def _is_generated_protocol_file(path: Path) -> bool:
    """Return whether ``path`` is one exact reviewed protoc-c output."""
    try:
        relative = path.resolve().relative_to(repo_root().resolve()).as_posix()
    except ValueError:
        return False
    return relative in GENERATED_PROTOCOL_FILES


def _is_build_output(path: Path) -> bool:
    """Return whether ``path`` is inside any recognized generated build tree."""
    try:
        relative = path.resolve().relative_to(repo_root().resolve()).as_posix()
    except ValueError:
        return False
    return is_build_output_path(relative)


def iter_function_files() -> Iterator[Path]:
    """Yield every first-party .c/.h the FUNCTION gate audits.

    Both the strict gate and the report generator walked this identically
    inline; sharing one iterator means the report can never describe a
    different set of files than the gate enforces over. ``tools/`` is a full
    top-level root, so adding a first-party tool cannot silently place it
    outside the documentation gate.
    """
    for top in SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [
                d
                for d in dirnames
                if d not in EXCLUDE_PARTS and not _is_build_output(Path(dirpath) / d)
            ]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = Path(dirpath) / fn
                if any(part in EXCLUDE_PARTS for part in p.relative_to(repo_root()).parts):
                    continue
                if _is_build_output(p) or _is_generated_protocol_file(p):
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
            # Accept CWD/absolute paths first, then resolve repo-relative input.
            candidate = p if p.is_file() else repo_root() / raw_path
            if (
                candidate.is_file()
                and not _is_build_output(candidate)
                and not _is_generated_protocol_file(candidate)
            ):
                yield candidate
        return
    for top in MEMBER_SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [
                d
                for d in dirnames
                if d not in MEMBER_EXCLUDE_PARTS and not _is_build_output(Path(dirpath) / d)
            ]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = Path(dirpath) / fn
                if any(part in MEMBER_EXCLUDE_PARTS for part in p.relative_to(repo_root()).parts):
                    continue
                if _is_build_output(p) or _is_generated_protocol_file(p):
                    continue
                yield p


def _fatal_below_floor(kind: str, count: int, floor: int) -> None:
    """Abort with exit 2 when a scope walk enumerated fewer files than its floor.

    Args:
        kind: Which scope collapsed, named as the caller's flag would spell it
            (e.g. ``"function"`` or ``"member"``).
        count: How many files the walk actually yielded.
        floor: The measured floor that count must reach.

    Raises:
        SystemExit: Always, with code 2, when ``count`` is below ``floor``.
    """
    if count >= floor:
        return
    sys.stderr.write(
        f"doxy_audit.py: FATAL -- only {count} file(s) in the {kind} scope, "
        f"floor is {floor}.\n"
        "  A collapsed scope reports a documented tree because it read nothing.\n"
    )
    sys.exit(2)


def function_files() -> list[Path]:
    """Every file the FUNCTION scope audits, materialised and floor-checked.

    The accessor exists so no caller can consume :func:`iter_function_files`
    lazily and never notice that it yielded nothing: a generator that produces
    no items looks exactly like a fully documented tree to the auditor
    downstream.

    Returns:
        The function-scope paths, in walk order.

    Raises:
        SystemExit: With code 2 when fewer than FUNCTION_FILE_FLOOR files were
            found -- a collapsed walk must fail, never read as clean.
    """
    files = list(iter_function_files())
    _fatal_below_floor("function", len(files), FUNCTION_FILE_FLOOR)
    return files


def member_files(explicit: list[str]) -> list[Path]:
    """Every file the MEMBER scope audits, materialised and floor-checked.

    The floor applies to the repo-wide walk only. An ``explicit`` list is a
    deliberately narrowed scope supplied on the command line -- it is allowed
    to be one file, or to filter to none -- while a collapsed walk is a broken
    enumeration reporting a documented tree.

    Args:
        explicit: User-supplied paths; empty means walk MEMBER_SCAN_DIRS.

    Returns:
        The member-scope paths, in walk order.

    Raises:
        SystemExit: With code 2 when the repo-wide walk found fewer than
            MEMBER_FILE_FLOOR files.
    """
    files = list(_iter_member_files(explicit))
    if not explicit:
        _fatal_below_floor("member", len(files), MEMBER_FILE_FLOOR)
    return files


def _top_dir(rel: str) -> str:
    """Top-level directory of a repo-relative path (e.g. "libs")."""
    return rel.split("/", 1)[0]
