# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Which files ``check_annotations.py`` analyses, and what module each belongs to.

Every other annotation module asks this one "is this path mine to judge?".
Keeping that question in one place matters more than it looks: the rules
disagree about scope on purpose -- the linkage rule judges all first-party
code, NASA Rule 3 judges firmware only, and RA8_PRIV is defined over module
boundaries -- and each of those predicates has been wrong at least once by
being restated at a call site instead of asked for here.

The repo root is a variable, not a constant
-------------------------------------------
``run_selftest()`` builds a synthetic tree in a temporary directory and needs
every scope predicate to resolve against *that* root rather than the real
checkout.  It used to do this by rebinding a module-global ``REPO_ROOT`` in
``check_annotations``.  That worked only while every predicate lived in the
same module: once the checker is split, a rebinding in one module is invisible
to the ``from ... import REPO_ROOT`` copies in the others, and the selftest
would quietly assert against the real tree -- passing, but proving nothing.

So the root lives here, behind :func:`repo_root`, and is overridden through
:func:`override_repo_root`.  Consumers call the function; there is no name to
import a stale copy of.
"""

from __future__ import annotations

import contextlib
import pathlib
from collections.abc import Iterator

#: The real checkout root. Never read directly outside `repo_root()` -- the
#: selftest override would not be visible through a bare import of it.
_REAL_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

#: Active root; `override_repo_root()` swaps this for the duration of a block.
_repo_root = _REAL_REPO_ROOT

#: Every first-party source root. CLAUDE.md ("Scope") holds `tools/` to the
#: same bar as the firmware -- "a file being a host tool or just a simulator
#: is NOT a reason to relax the rules" -- but `tools/` was absent here, so
#: ra8_emulator, media_dl, ra8_viewer and the rest were never annotation-checked
#: at all. `scripts/` holds no C. Vendored SOUP under `libs/third_party/` is
#: dropped by is_excluded(), not by omission from this tuple.
SCAN_DIRS = ("libs", "src", "examples", "tests", "port", "tools")

EXCLUDED_PATH_PARTS = {
    "build",
    "_deps",
    "third_party",
    "build-cov",
    "build-bench",
    "build-scan",
    "build-mcdc",
}

SOURCE_SUFFIXES = {".c", ".cpp"}

#: Directory names that only ever hold build output. Distinct from
#: EXCLUDED_PATH_PARTS, which also drops vendored trees: those must stay
#: off the *analysis* list but stay on the *include* path.
BUILD_OUTPUT_PARTS = frozenset(
    {"build", "_deps", "build-cov", "build-bench", "build-scan", "build-mcdc"}
)

#: Source roots that are compiled by the host toolchain and never cross-compiled
#: into a firmware image. `tests/` is the host unit-test suite; `tools/` is the
#: host-side tooling (ra8_emulator, media_dl, ra8_fmt, ...), which `scripts/ci.sh`
#: builds with ``CC=clang-18`` and which no `cmake/toolchain-ra8d2.cmake` target
#: ever references.
HOST_ONLY_ROOTS = frozenset({"tests", "tools"})

#: Roots whose immediate child directory is one module for RA8_PRIV purposes.
#: `libs/<module>` is the obvious one. `tools/<tool>` is the same shape: each
#: tool is one module split across several TUs with its own `*_internal.h`
#: (ra8_fmt says so in that header's own file comment), and ra8_emulator calling
#: ra8_fmt's private helper is the same boundary violation as one library
#: calling another's. Without this, an RA8_PRIV tag under tools/ is decorative
#: -- module_of() returned None, the rule hit `if callee_mod is None: continue`
#: and never compared anything.
MODULE_ROOTS = ("libs", "tools")


def repo_root() -> pathlib.Path:
    """Return the root every scope predicate resolves against."""
    return _repo_root


@contextlib.contextmanager
def override_repo_root(root: pathlib.Path) -> Iterator[None]:
    """Resolve scope against ``root`` for the duration of the block.

    Used only by the selftest, which parses a synthetic tree in a temporary
    directory. Restores the previous root even when the body raises, so a
    failing assertion cannot leave the process judging the real tree against
    a directory that no longer exists.
    """
    global _repo_root  # noqa: PLW0603  # single-homed override; see module docstring
    previous = _repo_root
    _repo_root = root
    try:
        yield
    finally:
        _repo_root = previous


def is_build_output(path: pathlib.Path) -> bool:
    """True when ``path`` sits inside a build-output directory."""
    return any(part in BUILD_OUTPUT_PARTS for part in path.parts)


def is_excluded(path: pathlib.Path) -> bool:
    """True when ``path`` must not be analysed (build output or vendored)."""
    return any(part in EXCLUDED_PATH_PARTS for part in path.parts)


def _root_part(path: str) -> str | None:
    """Return ``path``'s first repo-relative path component, or None."""
    if not path:
        return None
    try:
        rel = pathlib.Path(path).resolve().relative_to(repo_root())
    except (ValueError, OSError):
        return None
    if not rel.parts or is_excluded(rel):
        return None
    return rel.parts[0]


def is_first_party(path: str) -> bool:
    """True when ``path`` is hand-written source this project owns.

    Definitions reached through the include path are not automatically in
    scope. Parsing the ``.cpp`` translation units as C++ pulls in
    libstdc++, whose headers define hundreds of non-static inline
    functions; vendored trees under ``libs/third_party/`` are SOUP. Only
    files under the scan roots are ours to hold to the linkage rule.
    """
    return _root_part(path) in SCAN_DIRS


def is_test_path(path: str) -> bool:
    """True when ``path`` is a host unit-test translation unit."""
    return "/tests/" in path.replace("\\", "/")


def is_host_only_path(path: str) -> bool:
    """True when ``path`` is host-only code, i.e. not part of any firmware image.

    NASA Power of 10 Rule 3 is a claim about *firmware*: CLAUDE.md states it
    as "zero dynamic memory after initialization (zero malloc/free in
    firmware)". The hazard it guards -- heap fragmentation and an allocator
    failing unpredictably in a long-running image with no operator -- does not
    exist for a host program that runs for a moment on Linux and exits.

    So the rule's real question is "is this translation unit firmware", and
    this predicate is where that gets decided. It used to be decided by a bare
    ``"/tests/" in path`` substring at the one call site that needed it. That
    was the right intent expressed too narrowly: when `tools/` came into scope
    it added 216 findings telling a CPU emulator and a libcurl downloader not
    to call ``malloc``, none of which a developer can act on. A gate that
    cries wolf gets switched off, so the predicate is stated in terms of what
    actually distinguishes the code -- which toolchain compiles it -- and is
    matched on the repo-relative root rather than by substring, so a directory
    named ``tests`` nested anywhere else cannot silently claim the exemption.

    This narrows nothing for firmware: `libs/`, `src/`, `port/` and
    `examples/` are all still held to Rule 3, and in fact carry zero
    ``RA8_NASA_RULE_3_OK`` waivers tree-wide because the firmware genuinely
    does not allocate.
    """
    return _root_part(path) in HOST_ONLY_ROOTS


def module_of(path: str) -> str | None:
    """libs/<module>/... or tools/<tool>/... -> the module name; else None."""
    parts = pathlib.Path(path).parts
    for root in MODULE_ROOTS:
        try:
            idx = parts.index(root)
        except ValueError:
            continue
        if idx + 1 < len(parts):
            return f"{root}/{parts[idx + 1]}"
    return None


def relative(path: str) -> str:
    """Return ``path`` relative to the repo root when it lies inside it."""
    with contextlib.suppress(ValueError):
        return str(pathlib.Path(path).resolve().relative_to(repo_root()))
    return path


def discover_translation_units() -> list[pathlib.Path]:
    """Return every .c/.cpp file under SCAN_DIRS, excluding vendored trees."""
    out: list[pathlib.Path] = []
    for top in SCAN_DIRS:
        root = repo_root() / top
        if not root.is_dir():
            continue
        out.extend(
            path
            for path in root.rglob("*")
            if path.suffix in SOURCE_SUFFIXES and not is_excluded(path)
        )
    return sorted(out)
