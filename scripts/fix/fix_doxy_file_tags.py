#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
r"""Rewrite stale Doxygen ``@file`` tags to match each file's real path.

Rewrites stale ``@file`` (or ``\file``) Doxygen tags inside per-app boot
files so that the tag matches the file's actual repository-relative path.

The bulk of these mismatches were introduced when example apps were
copied from ``examples/<name>/`` into ``examples/ek_ra8d2/<name>/`` (and
later into ``examples/_unsupported/<name>/``) without updating the
``@file`` annotations. Doxygen then refuses to emit per-file pages for
them and prints one warning per file.

Usage:
    python3 scripts/fix/fix_doxy_file_tags.py            # apply fixes
    python3 scripts/fix/fix_doxy_file_tags.py --check    # report only
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Files we know were copied verbatim across apps and routinely have stale
# ``@file`` paths. The script restricts its rewrites to these basenames so
# we never touch hand-authored sources by accident.
BOOT_BASENAMES = {
    "vector_table.c",
    "system_init.c",
    "secure_exception.c",
    "trustzone_init.c",
    "trustzone_init.h",
    "main.c",
    # Per-app helpers that were also copied with stale @file paths.
    "power.c",
    "power.h",
}

# Directories under examples/ whose boot files we sweep.
SCAN_ROOTS = (
    REPO_ROOT / "examples" / "ek_ra8d2",
    REPO_ROOT / "examples" / "_unsupported",
)

# Match either "@file <path>" or "\file <path>" inside a comment block.
FILE_TAG_RE = re.compile(r"([@\\])file\s+([^\s\*]+)")


def iter_target_files() -> list[Path]:
    """Every per-app boot file that may carry a stale ``@file`` tag."""
    targets: list[Path] = []
    for root in SCAN_ROOTS:
        if not root.is_dir():
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            targets.extend(Path(dirpath) / name for name in filenames if name in BOOT_BASENAMES)
    return targets


def desired_tag_path(file_path: Path) -> str:
    """Return the path string we want the ``@file`` tag to contain."""
    return str(file_path.relative_to(REPO_ROOT))


def fix_file(file_path: Path, check_only: bool) -> bool:
    """Return True if the file changed (or would change in --check mode)."""
    text = file_path.read_text(encoding="ascii")
    desired = desired_tag_path(file_path)

    def repl(match: re.Match[str]) -> str:
        """Rewrite one ``@file`` tag, preserving its ``@``/backslash prefix.

        Returns the match untouched when the tag is already correct, so an
        up-to-date file is left byte-identical and shows no diff.
        """
        prefix = match.group(1)
        current = match.group(2)
        if current == desired:
            return match.group(0)
        return f"{prefix}file {desired}"

    new_text, count = FILE_TAG_RE.subn(repl, text, count=1)
    if new_text == text:
        return False
    if not check_only:
        file_path.write_text(new_text, encoding="ascii")
    print(
        f"{'would fix' if check_only else 'fixed'}: "
        f"{file_path.relative_to(REPO_ROOT)} ({count} tag)"
    )
    return True


def main() -> int:
    """Fix stale ``@file`` tags, or with ``--check`` report them without writing.

    The cost of a stale tag is silent: doxygen refuses to emit a per-file page
    for the mismatched file and warns once, so the page simply goes missing
    from the generated site rather than anything failing.

    Returns 1 when any tag was stale (in either mode), 0 when all match.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report files that would change but do not modify them",
    )
    args = parser.parse_args()

    changed = 0
    for path in iter_target_files():
        if fix_file(path, args.check):
            changed += 1

    print(f"\n{changed} file(s) {'would be ' if args.check else ''}updated")
    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
