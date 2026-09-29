# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared build-root discovery and ownership helpers for the Zig gate.

A Zig build root is a directory holding a ``build.zig``. Roots nest: the
repository now carries a root graph (#857, the parity work #859 depends on)
above the per-library graphs that were already there, so "which sources does
this root own" stopped being "everything beneath it" and became "everything
beneath it that no nearer ``build.zig`` already owns".
"""

from __future__ import annotations

from pathlib import Path

from lint_targets import is_build_output_path


def build_roots(files: list[str], repo_root: Path) -> list[Path]:
    """Distinct directories holding a build.zig above each of `files`."""
    roots: set[Path] = set()
    for rel in files:
        directory = (repo_root / rel).parent
        while directory != directory.parent:
            if (directory / "build.zig").is_file():
                roots.add(directory)
                break
            if directory == repo_root:
                break
            directory = directory.parent
    return sorted(roots)


def is_owned_by(root: Path, path: Path) -> bool:
    """True when `root` is the NEAREST build root above `path`.

    A build root owns the sources its own ``build.zig`` can describe and stops
    at the first nested ``build.zig`` beneath it. Without this stop, a graph at
    the repository root claims every Zig file in the tree -- including the ones
    already owned, tested and contracted by their own build roots -- so its
    contract cannot be written and every nested source reports as an orphan.
    Each nested root is still discovered and validated in its own right by
    :func:`build_roots`, so nothing stops being checked.
    """
    directory = path.parent
    while directory != root:
        if (directory / "build.zig").is_file():
            return False
        if directory == directory.parent:
            return False
        directory = directory.parent
    return True


def owned_zig_sources(root: Path) -> set[Path]:
    """Every first-party Zig source this build root -- and no nested one -- owns."""
    return {
        path.resolve()
        for path in root.rglob("*.zig")
        if path.name != "build.zig"
        and not is_build_output_path(str(path.relative_to(root)))
        and is_owned_by(root, path)
    }
