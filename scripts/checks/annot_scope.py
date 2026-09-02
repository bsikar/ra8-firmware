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

from lint_coverage_rules import PATH_CLASS

#: The real checkout root. Never read directly outside `repo_root()` -- the
#: selftest override would not be visible through a bare import of it.
_REAL_REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

#: Active root; `override_repo_root()` swaps this for the duration of a block.
_repo_root = _REAL_REPO_ROOT

#: Every first-party source root. CLAUDE.md ("Scope") holds `tools/` to the
#: same bar as the firmware -- "a file being a host tool or just an emulator
#: is NOT a reason to relax the rules" -- but `tools/` was absent here, so
#: ra8_emulator, mdl, ra8_viewer and the rest were never annotation-checked
#: at all. `scripts/` holds no C. Vendored SOUP under either canonical
#: third-party root is dropped by is_excluded(), not by omission from this
#: tuple.
SCAN_DIRS = ("libs", "examples", "tests", "port", "tools", "apps")

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

#: Classification assigned by the lint-coverage registry to reproducible,
#: machine-owned source.  Reusing that exact registry keeps every gate on the
#: same allow-list: a neighboring ``*.pb-c.c`` remains hand-authored C until it
#: receives its own reviewed generator and reproducibility contract.
GENERATED_SOURCE_CLASS = "generated-source"

#: Source regions compiled only by the host toolchain. ``apps/`` cannot be
#: exempted as a root: ``apps/board/`` is firmware and ``apps/shared_libs/`` is
#: linked into firmware. Only the hosted product form is host-only.
HOST_ONLY_PREFIXES = ("tests/", "tools/", "apps/host/")

#: Roots whose immediate child directory is one module for RA8_PRIV purposes.
#: `libs/<module>` is the obvious one. `tools/<tool>` is the same shape: each
#: tool is one module split across several TUs with its own `*_internal.h`
#: (ra8_fmt says so in that header's own file comment), and ra8_emulator calling
#: ra8_fmt's private helper is the same boundary violation as one library
#: calling another's. Without this, an RA8_PRIV tag under tools/ is decorative
#: -- module_of() returned None, the rule hit `if callee_mod is None: continue`
#: and never compared anything.
MODULE_ROOTS = ("libs", "tools", "apps")

#: How deep below its root a module's own directory sits. `libs/<module>`
#: and `tools/<tool>` are one level down. `apps/` is TWO, and the level it
#: skips is deliberately not part of the module identity: under `apps/` the
#: first component is a BUILD FORM of a product, not a library.
#: `apps/host/mdl` is the host CLI form,
#: `apps/board/threadx_modules/mdl` can be its loadable on-device form, and
#: `apps/shared_libs/mdl` is the portable core BOTH forms link. Those are
#: packagings of ONE module: they share the `mdl_` symbol namespace, and a
#: form's composition root exists precisely to drive the core's promoted
#: RA8_PRIV seams. So the key is `apps/<product>` and the category is
#: dropped from it, which makes every form of one product the same module
#: and any OTHER product a different one.
#:
#: Keying on the category instead -- which is what a flat depth of 1 did --
#: was wrong in both directions at once: two unrelated products sharing a
#: category could reach into each other's internals unreported, and the day
#: the portable core moved to `apps/shared_libs/` it reported cross-module
#: calls that are the composition root doing its job.
#:
#: The one-way rule this does NOT relax, because it is a different rule
#: entirely: `apps/shared_libs` must never include from a form. That is enforced
#: by the core configuring, building and testing standalone -- it has no
#: form on its include path at all -- not by this key.
APP_BOARD_FORM = "board"


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


def is_generated_source(path: pathlib.Path) -> bool:
    """Return whether lint coverage classifies this exact path as generated.

    ``PATH_CLASS`` is deliberately exact-path based.  The annotation checker
    therefore ignores the two pinned protoc-c outputs, whose regenerated
    identifiers cannot satisfy project spelling rules, without creating a
    blanket exemption for future protobuf or other generated-looking files.
    """
    candidate = path if path.is_absolute() else repo_root() / path
    try:
        relative = candidate.resolve().relative_to(repo_root().resolve()).as_posix()
    except (ValueError, OSError):
        return False
    return PATH_CLASS.get(relative) == GENERATED_SOURCE_CLASS


def is_excluded(path: pathlib.Path) -> bool:
    """True when ``path`` is build output, vendored, or exact generated source."""
    return any(part in EXCLUDED_PATH_PARTS for part in path.parts) or is_generated_source(path)


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
    functions; vendored trees under either canonical third-party root are
    SOUP. Only files under the scan roots are ours to hold to the linkage
    rule.
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

    This narrows nothing for firmware: `libs/`, `port/`, `examples/`,
    `apps/shared_libs/`, and `apps/board/` remain held to Rule 3. They carry
    zero ``RA8_NASA_RULE_3_OK`` waivers tree-wide because the firmware
    genuinely does not allocate.
    """
    try:
        rel = pathlib.Path(path).resolve().relative_to(repo_root()).as_posix()
    except (ValueError, OSError):
        return False
    return rel.startswith(HOST_ONLY_PREFIXES)


def module_of(path: str) -> str | None:
    """Return the owning module of a path, or None when it is outside one.

    ``libs/<module>/...`` and ``tools/<tool>/...`` name their module one level
    below the root. Apps drop their build-form components: host and shared
    products are ``apps/<form>/<product>``, while board products are
    ``apps/board/<form>/<product>``.
    """
    parts = pathlib.Path(path).parts
    for root in MODULE_ROOTS:
        try:
            idx = parts.index(root)
        except ValueError:
            continue
        depth = 1
        if root == "apps":
            tail = parts[idx + 1 :]
            depth = 3 if tail and tail[0] == APP_BOARD_FORM else 2
        # Require something below the module directory: a loose file sitting
        # directly in a category is not a product and must not name one.
        if idx + depth < len(parts) - 1:
            return f"{root}/{parts[idx + depth]}"
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
