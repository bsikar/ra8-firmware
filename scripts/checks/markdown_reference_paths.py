# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Resolve what a Markdown path reference names, and whether an owner exists.

The lower layer of the Markdown reference gate: brace and glob expansion,
placeholder segments, the build, generated and component owner lookups, and the
base directory a relative reference resolves against. It answers "what does this
token name and does something own it", never "is this document wrong", which
stays in ``markdown_references.py``.

Split out of ``markdown_references.py`` (#2791), which sat over the 1000-line
cap ``scripts/checks/check_file_size.py`` enforces. This layer calls nothing
above it, so imports run one way: the parent imports from here, never the
reverse. ``PathRef`` is used in annotations only and comes in under
``TYPE_CHECKING`` to keep it that way.
"""

from __future__ import annotations

import functools
import re
from pathlib import Path
from typing import TYPE_CHECKING

from markdown_reference_policy import (
    BARE_MARKDOWN_PATTERN,
    BRACE_ALTERNATION_RE,
    COMPONENT_RELATIVE_PREFIXES,
    LINE_CITATION_RE,
    MAX_BRACE_EXPANSIONS,
    SOUP_LOCAL_PATH_RE,
    SYMBOL_SUFFIX_RE,
    VENDOR_PREFIXES,
)

if TYPE_CHECKING:
    from markdown_references import PathRef


PLACEHOLDER_RE = re.compile(
    r"(?:<[a-z][a-z0-9_-]*>|\$\{[A-Z][A-Z0-9_]*}|\{[a-z][a-z0-9_]*}|\.\.\.)"
)


def _has_only_supported_dynamic_syntax(token: str) -> bool:
    """Reject unnamed interpolation while permitting checked glob syntax."""
    remaining = PLACEHOLDER_RE.sub("", token)
    remaining = re.sub(r"\{[A-Za-z0-9_./-]*(?:,[A-Za-z0-9_./-]*)+}", "", remaining)
    remaining = remaining.replace("*", "").replace("?", "")
    return not any(char in remaining for char in "{}$<>")


def _dynamic_glob(token: str) -> str:
    """Map exact named placeholders to an equivalent filesystem glob."""
    return PLACEHOLDER_RE.sub(lambda match: "**" if match.group(0) == "..." else "*", token)


def _before_build_output(token: str) -> str | None:
    """Return the owner prefix of a generated build path, if present."""
    segments = token.split("/")
    for index, segment in enumerate(segments):
        if segment == "build" or re.fullmatch(r"build(?:[-*?].*)", segment):
            return "/".join(segments[:index])
    return None


def _build_owner_exists(base: Path, token: str) -> bool:
    """Require the nearest static/dynamic owner before a build directory."""
    segments = token.rstrip("/").split("/")
    build_index = next(
        (index for index, segment in enumerate(segments) if segment.startswith("build")),
        None,
    )
    if build_index is None:
        return False
    owner_token = "/".join(segments[:build_index])
    if not owner_token:
        return (base / ".git").exists()
    owner = base / owner_token
    if owner.is_dir():
        return True
    if not _has_only_supported_dynamic_syntax(owner_token):
        return False
    return _glob_matches(base, _dynamic_glob(owner_token))


def _glob_matches(base: Path, token: str) -> bool:
    """Require a glob or brace pattern to select at least one current path."""
    brace_glob = re.search(r"\{[^{}]*,[^{}]*}", token) is not None
    if not any(char in token for char in "*?") and not brace_glob:
        return False
    for pattern in _brace_expansions(token.rstrip("/")):
        try:
            if next(base.glob(pattern), None) is not None:
                return True
        except (OSError, ValueError):
            return False
    return False


def _brace_expansions(token: str) -> tuple[str, ...]:
    """Expand ``a{x,y}b`` into every literal it names, bounded and order-stable."""
    results = [token]
    while BRACE_ALTERNATION_RE.search(results[0]) is not None:
        expanded: list[str] = []
        for candidate in results:
            hit = BRACE_ALTERNATION_RE.search(candidate)
            if hit is None:
                expanded.append(candidate)
                continue
            head, tail = candidate[: hit.start()], candidate[hit.end() :]
            expanded.extend(head + option.strip() + tail for option in hit.group(1).split(","))
        if len(expanded) > MAX_BRACE_EXPANSIONS:
            return (token,)
        results = expanded
    return tuple(results)


def _generated_owner_exists(base: Path, token: str) -> bool:
    """Check the committed authority for a build output or named placeholder."""
    if _before_build_output(token) is not None and _build_owner_exists(base, token):
        return True
    if _glob_matches(base, token):
        return True
    return _has_only_supported_dynamic_syntax(token) and _glob_matches(base, _dynamic_glob(token))


@functools.lru_cache(maxsize=1024)
def _component_root(root: Path, source: str) -> Path | None:
    """Return the closest enclosing CMake component for one document."""
    current = (root / source).parent
    resolved_root = root.resolve()
    while current.resolve() != resolved_root:
        if (current / "CMakeLists.txt").is_file():
            return current
        current = current.parent
    return None


def _soup_local_root(root: Path, source: str) -> Path | None:
    """Read a SOUP document's explicit, checked local-vendor authority."""
    if not source.startswith("docs/SOUP/"):
        return None
    match = SOUP_LOCAL_PATH_RE.search((root / source).read_text(encoding="utf-8", errors="replace"))
    if match is None:
        return None
    rel = match.group(1).rstrip("/")
    if not rel.startswith(VENDOR_PREFIXES):
        return None
    candidate = (root / rel).resolve()
    return candidate if candidate.is_dir() else None


def _normalized_path_token(ref: PathRef) -> tuple[str, bool]:
    """Strip citation/symbol syntax and report whether a line citation existed."""
    token = ref.token
    had_line_citation = LINE_CITATION_RE.search(token) is not None
    token = LINE_CITATION_RE.sub("", token)
    token = SYMBOL_SUFFIX_RE.sub("", token)
    token = token.partition("@")[0]
    while token.startswith("./"):
        token = token[2:]
    return token, had_line_citation


def _path_claimed(base: Path, token: str) -> bool:
    """Return whether one authority owns an in-bounds exact or generated path."""
    target = (base / token.rstrip("/")).resolve()
    try:
        target.relative_to(base.resolve())
    except ValueError:
        return False
    return target.exists() or _generated_owner_exists(base, token)


def _base_for_path(
    root: Path, source: str, token: str, soup_root: Path | None
) -> tuple[Path, str | None]:
    """Select one path authority, rejecting traversal and ambiguous ownership."""
    error = None
    if "/" not in token and re.fullmatch(BARE_MARKDOWN_PATTERN, token):
        base = (root / source).parent
    elif token.startswith("tests/"):
        local = soup_root or _component_root(root, source)
        base = local or root
        if ".." in Path(token).parts:
            error = "component-relative path contains traversal"
        else:
            bases = tuple(item for item in (root, local) if item is not None)
            claimed = tuple(item for item in bases if _path_claimed(item, token))
            if len({item.resolve() for item in claimed}) > 1:
                owners = ", ".join(item.relative_to(root).as_posix() or "." for item in claimed)
                error = f"is ambiguous between path authorities: {owners}"
            elif claimed:
                base = claimed[0]
    elif token.startswith(COMPONENT_RELATIVE_PREFIXES):
        base = soup_root or _component_root(root, source) or (root / source).parent
    elif token.startswith("../"):
        base = (root / source).parent
    else:
        base = root
    return base, error
