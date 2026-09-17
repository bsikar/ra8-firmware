#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""MkDocs hook: resolve the repository links in GitHub-first Markdown.

The manuals under ``docs/`` are written for github.com first, so they link to
tree files with ordinary relative paths: ``../../CLAUDE.md``,
``../../scripts/hil/all.sh``, ``../SOUP/``. Those resolve for a reader on
GitHub and point at nothing once the prose is rendered as a standalone site.

Doxygen solves this with ``scripts/gen/doxygen_md_filter.py``. This is the
same idea for the hub (ADR-0005):

* A link escaping ``docs/`` becomes an absolute URL into the repository at
  the published branch, ``blob/`` for a file and ``tree/`` for a directory.
* A link to a directory *inside* ``docs/`` that carries a ``README.md``
  becomes a link to that README, which is the page the hub actually
  publishes.

It fails closed. A relative link whose target does not exist in the working
tree aborts the build rather than shipping a dead link, which is the same
promise ``mkdocs build --strict`` makes for links inside ``docs/``.

Fenced code blocks are left alone. Text inside a fence is an example a reader
is meant to read literally, not a link the site resolves, so rewriting it
would corrupt the sample and, worse, a sample path that does not exist in the
tree would abort the whole docs build.
"""

from __future__ import annotations

import pathlib
import posixpath
import re

try:
    from mkdocs.exceptions import PluginError
except ImportError:  # pragma: no cover - only when run outside mkdocs
    PluginError = RuntimeError  # type: ignore[assignment, misc]

ROOT = pathlib.Path(__file__).resolve().parents[2]
PUBLISHED_BRANCH = "main"

# [text](target) and [text](target "title"), skipping image embeds: those are
# site assets the hub copies, not repository source links.
_LINK = re.compile(r"(?<!!)\[(?P<text>[^\]]*)\]\((?P<target>[^)\s]+)(?P<title>\s+\"[^\"]*\")?\)")

_SKIP_PREFIXES = ("http://", "https://", "mailto:", "#", "//", "data:")


def _split_fragment(target: str) -> tuple[str, str]:
    if "#" in target:
        path, _, fragment = target.partition("#")
        return path, f"#{fragment}"
    return target, ""


# An opening fence is ``` or ~~~ (up to three leading spaces, any info string);
# the closing fence uses the same character and is at least as long.
_FENCE = re.compile(r"^(?P<indent> {0,3})(?P<fence>`{3,}|~{3,})(?P<info>.*)$")


def _fenced_spans(markdown: str) -> list[tuple[int, int]]:
    """Return [start, end) line indexes of every fenced code block."""
    spans: list[tuple[int, int]] = []
    open_at: int | None = None
    marker = ""
    for index, line in enumerate(markdown.splitlines()):
        match = _FENCE.match(line)
        if match is None:
            continue
        fence = match.group("fence")
        if open_at is None:
            open_at = index
            marker = fence
        elif fence[0] == marker[0] and len(fence) >= len(marker):
            spans.append((open_at, index + 1))
            open_at = None
            marker = ""
    if open_at is not None:
        spans.append((open_at, len(markdown.splitlines())))
    return spans


def on_page_markdown(markdown: str, page, config, files):  # noqa: ANN001, ARG001
    docs_dir = pathlib.Path(config["docs_dir"]).resolve()
    repo_url = str(config.get("repo_url") or "").rstrip("/")
    page_dir = posixpath.dirname(page.file.src_uri)
    problems: list[str] = []

    def replace(match: re.Match[str]) -> str:
        target = match.group("target")
        if target.startswith(_SKIP_PREFIXES) or target.startswith("/"):
            return match.group(0)

        path, fragment = _split_fragment(target)
        if not path:
            return match.group(0)

        rel_to_docs = posixpath.normpath(posixpath.join(page_dir, path))
        escapes = rel_to_docs.startswith("..")
        on_disk = (docs_dir / rel_to_docs).resolve()

        if not on_disk.exists():
            problems.append(f"{page.file.src_uri}: '{target}' does not exist in the tree")
            return match.group(0)

        if not escapes:
            # Inside docs/. A directory link only works on the site when it
            # lands on the page the hub publishes for that directory.
            if on_disk.is_dir() and (on_disk / "README.md").is_file():
                joined = posixpath.join(path.rstrip("/"), "README.md")
                return f"[{match.group('text')}]({joined}{fragment}{match.group('title') or ''})"
            return match.group(0)

        if not repo_url:
            problems.append(
                f"{page.file.src_uri}: '{target}' leaves docs/ but mkdocs.yml sets no repo_url"
            )
            return match.group(0)

        try:
            repo_rel = on_disk.relative_to(ROOT).as_posix()
        except ValueError:
            problems.append(f"{page.file.src_uri}: '{target}' resolves outside the repository")
            return match.group(0)
        kind = "tree" if on_disk.is_dir() else "blob"
        url = f"{repo_url}/{kind}/{PUBLISHED_BRANCH}/{repo_rel}{fragment}"
        return f"[{match.group('text')}]({url}{match.group('title') or ''})"

    lines = markdown.splitlines(keepends=True)
    in_fence = {
        index for start, end in _fenced_spans(markdown) for index in range(start, end)
    }
    rewritten = "".join(
        line if index in in_fence else _LINK.sub(replace, line)
        for index, line in enumerate(lines)
    )
    if problems:
        raise PluginError(
            "mkdocs_repo_links: relative links with no target in the tree:\n  "
            + "\n  ".join(problems)
        )

    return rewritten
