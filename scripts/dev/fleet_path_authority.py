# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticate executable and plugin trees before fleet control-plane use."""

from __future__ import annotations

from pathlib import Path


def confined_link_errors(root: Path) -> list[str]:
    """Return every broken, absolute, or escaping symlink below ``root``."""
    try:
        authority = root.resolve(strict=True)
    except OSError as exc:
        return [f"collection tree is unavailable: {exc}"]
    if root.absolute() != authority or root.is_symlink() or not root.is_dir():
        return ["collection tree root is not a real owned directory"]
    errors: list[str] = []
    for entry in sorted(root.rglob("*")):
        try:
            entry.lstat()
        except OSError as exc:
            errors.append(f"cannot lstat {entry}: {exc}")
            continue
        if not entry.is_symlink():
            continue
        target_text = entry.readlink()
        try:
            target = entry.resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            errors.append(f"broken collection link {entry}: {exc}")
            continue
        if target_text.is_absolute() or not target.is_relative_to(authority):
            errors.append(f"collection link escapes its root: {entry}")
        if not (target.is_file() or target.is_dir()):
            errors.append(f"collection link target is not a file or directory: {entry}")
    return errors
