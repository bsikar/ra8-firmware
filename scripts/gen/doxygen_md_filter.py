#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Doxygen input filter for Markdown pages on the published docs site.

Markdown in this repository is written for github.com first; two GitHub
idioms degrade on the public Doxygen site, and this filter maps them to
their docs-site equivalent at build time (the files on GitHub are
untouched):

1. **GitHub Actions status badges** are dropped. The repository is
   private but the gh-pages site is public, so a badge image URL
   (``.../actions/workflows/<wf>.yml/badge.svg``) answers 404 without
   repo credentials and renders as a broken-image icon for every site
   visitor.

2. **Links to ``<dir>/README.md`` become ``@ref <dir>``.** Doxygen merges
   each ``README.md`` into its directory's page, so such links already
   land on the directory page -- but when resolved from the raw
   ``.md`` target, doxygen emits the Markdown file's *absolute build
   path* as the link tooltip, leaking the workspace path of whichever
   machine built the docs into the published HTML. The equivalent
   ``@ref`` form resolves to the same directory page with a clean,
   repo-relative tooltip. Only links whose target provably exists in
   the repo are rewritten; anchored links (``README.md#...``) and
   external URLs pass through untouched.

Both transformations skip fenced code blocks, where such text is a
literal example rather than a live link.

Wired up via ``FILTER_PATTERNS`` in the top-level ``Doxyfile``::

    FILTER_PATTERNS = *.md="python3 scripts/gen/doxygen_md_filter.py"

Doxygen invokes the filter once per matching input file with the file
path as the single argument and reads the filtered content from stdout.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[2]

# A fenced-code-block delimiter line (``` or ~~~, optionally indented).
FENCE = re.compile(r"^\s*(?:```|~~~)")

# A Markdown image (plain or link-wrapped) whose image URL is a GitHub
# Actions workflow badge. Matched occurrences are removed from the line;
# lines left empty by the removal are dropped entirely.
ACTIONS_BADGE = re.compile(
    r"\[?!\[[^\]]*\]\([^)]*/actions/workflows/[^)]*badge\.svg[^)]*\)(?:\]\([^)]*\))?"
)

# The target of a Markdown link written as ``[text](<target>)``.
MD_LINK = re.compile(r"(?<!\!)(\[[^\]]*\])\(([^)\s]+)\)")


def rewrite_readme_link(source_dir: PurePosixPath, target: str) -> str | None:
    """Return the ``@ref`` form of a relative ``<dir>/README.md`` target.

    ``source_dir`` is the repo-relative directory of the Markdown file
    being filtered. Returns ``None`` when the target is external, is
    anchored, escapes the repo, or does not resolve to an existing
    ``README.md`` -- callers must then leave the link untouched.
    """
    if not target.endswith("README.md"):
        return None
    if target.startswith(("http://", "https://", "mailto:", "/", "#")):
        return None
    resolved = PurePosixPath(*(source_dir / target).parts)
    parts: list[str] = []
    for part in resolved.parts:
        if part == "..":
            if not parts:
                return None  # escapes the repository root
            parts.pop()
        elif part != ".":
            parts.append(part)
    normalized = PurePosixPath(*parts)
    if not (REPO_ROOT / normalized).is_file():
        return None
    ref_dir = normalized.parent
    if not ref_dir.parts:
        return None  # the top-level README.md has no directory page
    return f"@ref {ref_dir}"


def filter_markdown(text: str, source_dir: PurePosixPath) -> str:
    """Return ``text`` with badges removed and README links rewritten."""

    def replace_link(match: re.Match[str]) -> str:
        ref = rewrite_readme_link(source_dir, match.group(2))
        if ref is None:
            return match.group(0)
        return f"{match.group(1)}({ref})"

    kept: list[str] = []
    in_fence = False
    for line in text.splitlines(keepends=True):
        if FENCE.match(line):
            in_fence = not in_fence
            kept.append(line)
            continue
        if in_fence:
            kept.append(line)
            continue
        stripped_line = ACTIONS_BADGE.sub("", line)
        if not stripped_line.strip() and ACTIONS_BADGE.search(line):
            continue  # the line held nothing but a badge -- drop it
        kept.append(MD_LINK.sub(replace_link, stripped_line))
    return "".join(kept)


def main(argv: list[str]) -> int:
    """Filter the file named in ``argv`` to stdout; 0 on success."""
    args = argv[1:]
    try:
        (source_name,) = args
    except ValueError:
        print("usage: doxygen_md_filter.py <file.md>", file=sys.stderr)
        return 2
    source = Path(source_name).resolve()
    try:
        source_dir = PurePosixPath(source.parent.relative_to(REPO_ROOT).as_posix())
    except ValueError:
        source_dir = PurePosixPath()  # outside the repo: badge-strip only
    sys.stdout.write(filter_markdown(source.read_text(encoding="utf-8"), source_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
