# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate first-party Markdown links, anchors, and repository paths.

Every tracked Markdown file is inventoried.  Local links and fragments must
resolve, as must repository paths in prose, code spans, and fenced examples.
Only proven-balanced inline-link destinations are masked, and HTML wrapper
separators cannot merge adjacent path claims. Parsed link destinations have one
structural owner. Generated, ignored, and placeholder paths require a current
owner. Historical changelog prose and vendored Markdown are not interpreted,
though links in the changelog and repository-authored vendor indexes remain
checked.
"""

from __future__ import annotations

import fnmatch
import functools
import hashlib
import re
import subprocess
import sys
import urllib.parse
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Never

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import trusted_git_executable
from line_citation_lex import CITES_OK_RE
from markdown_link_lex import inline_link_targets, mask_inline_link_targets, split_link_destination
from markdown_reference_policy import (
    ATX_HEADING_RE,
    AUTHORED_VENDOR_INDEXES,
    BARE_CODE_FILE_RE,
    BARE_MARKDOWN_PATTERN,
    COMPONENT_RELATIVE_PREFIXES,
    DECLARED_BARE_CODE_FILES,
    DECLARED_BARE_CONTEXT_SHA256,
    EXPLICIT_ANCHOR_RE,
    FENCE_RE,
    HTML_PATH_SEPARATOR_RE,
    HTML_TARGET_RE,
    LIBWEBP_ABSENCE_CLAUSE,
    LINE_CITATION_RE,
    LOCAL_LINE_FRAGMENT_RE,
    MIN_FIRST_PARTY_MARKDOWN,
    MIN_LINK_REFERENCES,
    MIN_PATH_REFERENCES,
    MIN_TRACKED_MARKDOWN,
    MIN_VENDOR_MARKDOWN,
    PATH_RE,
    QUALIFICATION_RELEASE_SOURCES,
    REFERENCE_DEF_RE,
    REFERENCE_USE_RE,
    REMOTE_SCHEMES,
    ROOT_FILE_PATTERN,
    SETEXT_HEADING_RE,
    SHORTCUT_PATH_REFERENCE_RE,
    SOUP_DECLARED_ABSENCES,
    SOUP_LOCAL_PATH_RE,
    SYMBOL_SUFFIX_RE,
    SYSTEM_HEADER_BASENAMES,
    TOOL_PRIVATE_CLAUSE_PATTERNS,
    TOOL_PRIVATE_OWNERSHIP_INDEXES,
    TOOL_PRIVATE_VENDOR_SOURCES,
    TRAILING_PATH_JUNK,
    VENDOR_PREFIXES,
    WORK_FIXTURE_PATH,
)

REPO_ROOT = Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class LinkRef:
    """One structurally parsed Markdown or HTML target."""

    line: int
    target: str


@dataclass(frozen=True)
class PathRef:
    """One repository path extracted from a code span or fenced example."""

    line: int
    column: int
    token: str
    source_line: str


@dataclass(frozen=True)
class AnchorRef:
    """One generated heading id or explicit HTML anchor."""

    line: int
    value: str


@dataclass(frozen=True)
class Finding:
    """One stale Markdown reference."""

    kind: str
    path: str
    line: int
    value: str
    detail: str

    def render(self) -> str:
        """Render an editor-jumpable diagnostic."""
        return f"{self.path}:{self.line}: {self.kind}: {self.value!r} -- {self.detail}"


@dataclass(frozen=True)
class Document:
    """Parsed Markdown structures needed by the checker."""

    anchors: frozenset[str]
    anchor_collisions: tuple[AnchorRef, ...]
    missing_references: tuple[LinkRef, ...]
    links: tuple[LinkRef, ...]
    paths: tuple[PathRef, ...]


@dataclass
class StructureSink:
    """Mutable collections populated while one Markdown document is parsed."""

    links: list[LinkRef]
    explicit_anchors: list[AnchorRef]
    heading_bases: list[AnchorRef]
    reference_definitions: dict[str, LinkRef]
    reference_uses: list[LinkRef]


class CheckError(RuntimeError):
    """The checker could not establish a trustworthy result."""


def _fail(message: str) -> Never:
    raise CheckError(message)


def _git(root: Path, *args: str, input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    git_bin = trusted_git_executable()
    return subprocess.run(  # noqa: S603 -- fixed executable and caller-owned argv
        [git_bin, *args],
        cwd=root,
        input=input_text,
        capture_output=True,
        text=True,
        check=False,
    )


def _git_files(root: Path, *, include_untracked: bool) -> tuple[list[str], list[str]]:
    """Return Markdown scope and the tracked-only population."""
    tracked_proc = _git(root, "ls-files", "-z")
    if tracked_proc.returncode != 0:
        _fail(tracked_proc.stderr.strip() or "git ls-files failed")
    tracked = sorted(
        path
        for path in tracked_proc.stdout.split("\0")
        if path and Path(path).suffix.lower() == ".md" and (root / path).is_file()
    )
    if not include_untracked:
        return tracked, tracked
    all_proc = _git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    if all_proc.returncode != 0:
        _fail(all_proc.stderr.strip() or "git ls-files including untracked failed")
    paths = sorted(
        path
        for path in all_proc.stdout.split("\0")
        if path and Path(path).suffix.lower() == ".md" and (root / path).is_file()
    )
    return paths, tracked


def _is_vendor(path: str) -> bool:
    return path not in AUTHORED_VENDOR_INDEXES and path.startswith(VENDOR_PREFIXES)


def _code_spans(line: str) -> tuple[list[tuple[int, str]], str]:
    """Extract same-run backtick spans and blank them in the visible text."""
    spans: list[tuple[int, str]] = []
    visible = list(line)
    cursor = 0
    while cursor < len(line):
        if line[cursor] != "`":
            cursor += 1
            continue
        end_run = cursor
        while end_run < len(line) and line[end_run] == "`":
            end_run += 1
        marker = line[cursor:end_run]
        close = line.find(marker, end_run)
        if close < 0:
            cursor = end_run
            continue
        content = line[end_run:close]
        spans.append((end_run, content))
        visible[cursor : close + len(marker)] = " " * (close + len(marker) - cursor)
        cursor = close + len(marker)
    return spans, "".join(visible)


def _prose_without_link_targets(visible: str) -> str:
    """Remove destinations owned by the link checker from ordinary prose."""
    masked = list(mask_inline_link_targets(visible))
    definition = REFERENCE_DEF_RE.match(visible)
    if definition is not None:
        masked[definition.start(2) : definition.end(2)] = " " * len(definition.group(2))
    for match in HTML_TARGET_RE.finditer(visible):
        masked[match.start(2) : match.end(2)] = " " * len(match.group(2))
    return "".join(masked)


def _reference_label(raw: str) -> str:
    """Normalize a reference-link label using Markdown's case/space rules."""
    return " ".join(raw.split()).casefold()


def _slug(text: str) -> str:
    """Return GitHub's practical heading-id form for this ASCII-first tree."""
    text = re.sub(r"<[^>]*>", "", text)
    text = re.sub(r"[`*_~]", "", text).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return re.sub(r"[ \t]+", "-", text)


def _path_refs(
    body: str,
    line: int,
    source_line: str,
    column_offset: int = 0,
    *,
    include_bare_code_files: bool = False,
) -> list[PathRef]:
    """Extract path tokens without letting HTML separators join neighbors."""
    refs: list[PathRef] = []
    boundaries = (0, *(match.end() for match in HTML_PATH_SEPARATOR_RE.finditer(body)))
    endings = (*(match.start() for match in HTML_PATH_SEPARATOR_RE.finditer(body)), len(body))
    for start, end in zip(boundaries, endings, strict=True):
        span = body[start:end]
        for match in PATH_RE.finditer(span):
            token = match.group(1).rstrip(TRAILING_PATH_JUNK)
            if token:
                refs.append(
                    PathRef(line, column_offset + start + match.start(1), token, source_line)
                )
        if include_bare_code_files:
            refs.extend(
                PathRef(
                    line,
                    column_offset + start + match.start(1),
                    match.group(1),
                    source_line,
                )
                for match in BARE_CODE_FILE_RE.finditer(span)
            )
    return sorted(refs, key=lambda ref: ref.column)


def _visible_structures(
    visible: str,
    line_no: int,
    previous_visible: str,
    sink: StructureSink,
) -> None:
    """Collect structures from one non-code Markdown line."""
    sink.links.extend(LinkRef(line_no, target) for target in inline_link_targets(visible) if target)
    definition = REFERENCE_DEF_RE.match(visible)
    if definition is not None:
        target = split_link_destination(definition.group(2))
        if target:
            sink.links.append(LinkRef(line_no, target))
            sink.reference_definitions[_reference_label(definition.group(1))] = LinkRef(
                line_no, target
            )
    sink.reference_uses.extend(
        LinkRef(line_no, _reference_label(match.group(2) or match.group(1)))
        for match in REFERENCE_USE_RE.finditer(visible)
    )
    sink.reference_uses.extend(
        LinkRef(line_no, _reference_label(match.group(1)))
        for match in SHORTCUT_PATH_REFERENCE_RE.finditer(visible)
        if REFERENCE_DEF_RE.match(visible) is None
    )
    sink.links.extend(
        LinkRef(line_no, match.group(2)) for match in HTML_TARGET_RE.finditer(visible)
    )
    sink.explicit_anchors.extend(
        AnchorRef(line_no, match.group(2)) for match in EXPLICIT_ANCHOR_RE.finditer(visible)
    )

    heading = ATX_HEADING_RE.match(visible)
    if heading is not None:
        sink.heading_bases.append(AnchorRef(line_no, _slug(heading.group(2))))
    elif SETEXT_HEADING_RE.match(visible) and previous_visible.strip():
        sink.heading_bases.append(AnchorRef(line_no - 1, _slug(previous_visible.strip())))


def _deduplicated_anchors(
    bases: Iterable[AnchorRef], explicit: Iterable[AnchorRef]
) -> tuple[frozenset[str], tuple[AnchorRef, ...]]:
    """Apply GitHub heading suffixes and reject duplicate document ids."""
    seen: Counter[str] = Counter()
    generated: list[AnchorRef] = []
    for ref in bases:
        occurrence = seen[ref.value]
        seen[ref.value] += 1
        value = ref.value if occurrence == 0 else f"{ref.value}-{occurrence}"
        generated.append(AnchorRef(ref.line, value))
    anchors: set[str] = set()
    collisions: list[AnchorRef] = []
    for ref in sorted((*explicit, *generated), key=lambda item: item.line):
        if ref.value in anchors:
            collisions.append(ref)
        anchors.add(ref.value)
    return frozenset(anchors), tuple(collisions)


def parse_document(text: str) -> Document:
    """Parse links, headings, code spans, and fenced examples from Markdown."""
    paths: list[PathRef] = []
    sink = StructureSink([], [], [], {}, [])
    in_fence = False
    fence_char = ""
    fence_len = 0
    previous_visible = ""
    lines = text.splitlines()
    for line_no, line in enumerate(lines, 1):
        fence_match = FENCE_RE.match(line)
        if fence_match is not None:
            marker = fence_match.group(1)
            if not in_fence:
                in_fence = True
                fence_char = marker[0]
                fence_len = len(marker)
            elif marker[0] == fence_char and len(marker) >= fence_len:
                in_fence = False
            previous_visible = ""
            continue
        if in_fence:
            paths.extend(_path_refs(line, line_no, line, include_bare_code_files=True))
            continue

        spans, visible = _code_spans(line)
        source_context = line
        if line_no < len(lines) and lines[line_no].strip():
            source_context += " " + lines[line_no].lstrip()
        for column, content in spans:
            paths.extend(
                _path_refs(
                    content,
                    line_no,
                    source_context,
                    column,
                    include_bare_code_files=True,
                )
            )
        paths.extend(_path_refs(_prose_without_link_targets(visible), line_no, source_context))

        _visible_structures(visible, line_no, previous_visible, sink)
        previous_visible = visible

    anchors, collisions = _deduplicated_anchors(sink.heading_bases, sink.explicit_anchors)
    missing = tuple(
        use for use in sink.reference_uses if use.target not in sink.reference_definitions
    )
    return Document(anchors, collisions, missing, tuple(sink.links), tuple(paths))


def _resolve_link(root: Path, source: str, raw: str) -> tuple[Path | None, str, str]:
    """Return a local target, fragment, and classification."""
    parsed = urllib.parse.urlsplit(raw)
    if parsed.scheme.lower() in REMOTE_SCHEMES or raw.startswith("//"):
        return None, "", "remote"
    path_text = urllib.parse.unquote(parsed.path)
    fragment = urllib.parse.unquote(parsed.fragment)
    source_path = root / source
    if raw.startswith("/"):
        target = (root / path_text.lstrip("/")).resolve()
    else:
        target = source_path if not path_text else (source_path.parent / path_text).resolve()
    return target, fragment, "local"


PLACEHOLDER_RE = re.compile(
    r"(?:<[a-z][a-z0-9_-]*>|\$\{[A-Z][A-Z0-9_]*}|\{[a-z][a-z0-9_]*}|\.\.\.)"
)


def _is_well_formed_dynamic_segment(segment: str) -> bool:
    """Accept only named, exact placeholder grammars inside a path segment."""
    replaced, count = PLACEHOLDER_RE.subn("value", segment)
    return count > 0 and not any(char in replaced for char in "*?{}$<>")


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


def _brace_expansions(pattern: str) -> tuple[str, ...]:
    """Expand comma braces without invoking a shell."""
    match = re.search(r"\{([^{}]*,[^{}]*)}", pattern)
    if match is None:
        return (pattern,)
    expanded: list[str] = []
    for choice in match.group(1).split(","):
        candidate = pattern[: match.start()] + choice + pattern[match.end() :]
        expanded.extend(_brace_expansions(candidate))
    return tuple(expanded)


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


def _ignore_owner_exists(root: Path, rel: str) -> bool:
    """Accept ignored local state only while the ignore rule's owner still exists."""
    proc = _git(root, "check-ignore", "--no-index", "--verbose", "--", rel)
    if proc.returncode != 0 or "\t" not in proc.stdout:
        return False
    rule, _matched = proc.stdout.rstrip("\n").split("\t", 1)
    source, _line, pattern = rule.rsplit(":", 2)
    pattern = pattern.lstrip("!")
    if "/" not in pattern.rstrip("/"):
        return False
    if any(segment.startswith("build") for segment in rel.split("/")):
        return _build_owner_exists(root, rel)
    base = (root / source).parent
    pattern_path = pattern.lstrip("/")
    static = re.split(r"[*?\[]", pattern_path, maxsplit=1)[0].rstrip("/")
    if pattern.endswith("/"):
        owner = (base / static).parent
    elif any(char in pattern for char in "*?["):
        owner = base / static
    else:
        owner = (base / pattern_path).parent
    return owner.is_dir()


def _declared_absence(source: str, ref: PathRef) -> bool:
    """Recognize the two exact policy grammars that name intentionally absent files."""
    if source != "CLAUDE.md":
        return False
    policy_placeholder = re.fullmatch(r"docs/SOMETHING_(?:TODO|ROADMAP|TICKET)\.md", ref.token)
    return (
        f"former `{ref.token}`" in ref.source_line
        or f"`{ref.token}` must remain absent" in ref.source_line
        or policy_placeholder is not None
    )


def _declared_work_fixture(source: str, ref: PathRef) -> bool:
    """Recognize the exact intentionally path-shaped workflow-key fixture."""
    return (
        source == "tools/work/tests/fixtures/bad_key.md"
        and ref.token == WORK_FIXTURE_PATH
        and "A key that looks like a path" in ref.source_line
    )


def _declared_planned_path(root: Path, source: str, ref: PathRef) -> bool:
    """Recognize exact future namespaces with a committed policy authority."""
    release = re.fullmatch(r"docs/qualification/release/<tag>/(?:conformance\.md)?", ref.token)
    if (
        source in QUALIFICATION_RELEASE_SOURCES
        and release is not None
        and (root / "docs/qualification/release/README.md").is_file()
    ):
        return True
    tool_private = re.fullmatch(r"tools/<tool>/third_party/<(?:component|dep)>", ref.token)
    clause_template = TOOL_PRIVATE_CLAUSE_PATTERNS.get(source)
    clause_match = (
        None
        if clause_template is None
        else re.search(clause_template.format(token=re.escape(ref.token)), ref.source_line)
    )
    policy_text = (root / source).read_text(encoding="utf-8", errors="replace")
    excludes_current = re.search(
        r"No (?:current dependency|dependency currently) qualifies", policy_text
    )
    current_exclusion_holds = (
        source not in TOOL_PRIVATE_OWNERSHIP_INDEXES or excludes_current is not None
    )
    return (
        source in TOOL_PRIVATE_VENDOR_SOURCES
        and tool_private is not None
        and clause_match is not None
        and current_exclusion_holds
    )


def _declared_soup_absence(source: str, ref: PathRef) -> bool:
    """Recognize only exact reviewed SOUP paths in a bounded negative claim."""
    return (
        ref.token in SOUP_DECLARED_ABSENCES.get(source, frozenset())
        and source == "docs/SOUP/libwebp.md"
        and ref.source_line == LIBWEBP_ABSENCE_CLAUSE
    )


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


@functools.lru_cache(maxsize=8)
def _tracked_basename_index(root_text: str) -> dict[str, tuple[str, ...]]:
    """Index tracked paths by basename for bare code-file references."""
    root = Path(root_text)
    proc = _git(root, "ls-files", "-z")
    if proc.returncode != 0:
        _fail(proc.stderr.strip() or "git ls-files failed")
    index: dict[str, list[str]] = {}
    for path in proc.stdout.split("\0"):
        if path:
            index.setdefault(Path(path).name, []).append(path)
    return {name: tuple(paths) for name, paths in index.items()}


def _bare_code_file_exists(root: Path, source: str, token: str) -> bool:
    """Resolve a bare code filename in its component, then the tracked tree."""
    if not _has_only_supported_dynamic_syntax(token):
        return False
    patterns = _brace_expansions(_dynamic_glob(token))
    index = _tracked_basename_index(str(root.resolve()))
    names = tuple(
        name for name in index if any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)
    )
    if not names:
        return False
    component = _component_root(root, source)
    if component is not None:
        component_prefix = component.relative_to(root).as_posix().rstrip("/") + "/"
        if any(path.startswith(component_prefix) for name in names for path in index.get(name, ())):
            return True
    return any(name in index for name in names)


def _declared_bare_code_file(source: str, ref: PathRef) -> str | None:
    """Return the reason an exact absent bare filename is intentionally cited."""
    if ref.token in SYSTEM_HEADER_BASENAMES and f"<{ref.token}>" in ref.source_line:
        return "toolchain-provided system header"
    key = (source, ref.token)
    reason = DECLARED_BARE_CODE_FILES.get(key)
    contexts = DECLARED_BARE_CONTEXT_SHA256.get(key, ())
    if reason is None or _context_sha256(ref.source_line) not in contexts:
        return None
    return reason


def _context_sha256(source_line: str) -> str:
    """Hash normalized full source context for an exact absence declaration."""
    normalized = " ".join(source_line.split()).encode("utf-8")
    return hashlib.sha256(normalized).hexdigest()


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


def _reference_is_declared(root: Path, source: str, ref: PathRef, had_line_citation: bool) -> bool:
    """Return whether an exact policy declaration owns this absent reference."""
    return (
        (had_line_citation and CITES_OK_RE.search(ref.source_line) is not None)
        or source == "CHANGELOG.md"
        or _declared_absence(source, ref)
        or _declared_work_fixture(source, ref)
        or _declared_planned_path(root, source, ref)
    )


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


def _root_or_soup_file_exists(root: Path, source: str, token: str, soup_root: Path | None) -> bool:
    """Resolve exact root-authority tokens against sibling/SOUP locations."""
    if "/" in token or re.fullmatch(ROOT_FILE_PATTERN, token) is None:
        return False
    sibling = (root / source).parent / token
    soup_file = soup_root / token if soup_root is not None else None
    return sibling.is_file() or (soup_file is not None and soup_file.is_file())


def _ordinary_path_reason(root: Path, source: str, ref: PathRef, token: str) -> str | None:
    """Resolve a non-bare repository path and explain a missing target."""
    soup_root = _soup_local_root(root, source)
    if _root_or_soup_file_exists(root, source, token, soup_root):
        return None
    if any(char in token for char in "*?{}$<>") and not _has_only_supported_dynamic_syntax(token):
        reason = "contains unsupported dynamic syntax"
    else:
        base, reason = _base_for_path(root, source, token, soup_root)
        if reason is None:
            target = (base / token.rstrip("/")).resolve()
            try:
                rel = target.relative_to(root.resolve()).as_posix()
                if token.startswith(COMPONENT_RELATIVE_PREFIXES):
                    target.relative_to(base.resolve())
            except ValueError:
                reason = "resolves outside its path authority"
            else:
                exists = (
                    target.exists()
                    or _generated_owner_exists(base, token)
                    or _declared_soup_absence(source, ref)
                )
                ignore_probe = f"{rel}/.ra8-markdown-reference" if ref.token.endswith("/") else rel
                if not exists and not _ignore_owner_exists(root, ignore_probe):
                    reason = f"resolves to {rel}, which does not exist"
    return reason


def _path_reason(root: Path, source: str, ref: PathRef) -> str | None:
    """Return why a literal code path is stale, or ``None`` when it is sound."""
    token, had_line_citation = _normalized_path_token(ref)
    if _reference_is_declared(root, source, ref, had_line_citation):
        return None
    if had_line_citation:
        return "uses a rot-prone line-number citation; cite a symbol instead"

    if BARE_CODE_FILE_RE.fullmatch(token):
        if _bare_code_file_exists(root, source, token) or _declared_bare_code_file(source, ref):
            return None
        return f"no tracked file has a basename matching {token}"
    return _ordinary_path_reason(root, source, ref, token)


def _bare_declaration_findings(
    root: Path,
    parsed: dict[str, Document],
    declarations: dict[tuple[str, str], str] | None = None,
    contexts: dict[tuple[str, str], tuple[str, ...]] | None = None,
) -> list[Finding]:
    """Reject declarations whose source/token/reason binding became stale."""
    declarations = DECLARED_BARE_CODE_FILES if declarations is None else declarations
    contexts = DECLARED_BARE_CONTEXT_SHA256 if contexts is None else contexts
    findings: list[Finding] = []
    for source, token in sorted(declarations.keys() | contexts.keys()):
        reason = declarations.get((source, token), "")
        expected_contexts = contexts.get((source, token), ())
        document = parsed.get(source)
        extracted = (
            () if document is None else tuple(ref for ref in document.paths if ref.token == token)
        )
        observed_contexts = tuple(sorted(_context_sha256(ref.source_line) for ref in extracted))
        detail = ""
        if (source, token) not in declarations:
            detail = "semantic contexts have no declaration"
        elif not reason.strip():
            detail = "declaration reason is empty"
        elif not extracted:
            detail = "source no longer extracts this exact bare token"
        elif not expected_contexts:
            detail = "declaration has no semantic context binding"
        elif observed_contexts != expected_contexts:
            detail = "normalized semantic context binding drifted"
        elif _bare_code_file_exists(root, source, token):
            detail = "a tracked basename now exists; remove the absence declaration"
        if detail:
            findings.append(Finding("stale-bare-declaration", source, 1, token, detail))
    return findings


def _inventory(root: Path, tracked: Iterable[str]) -> list[dict[str, object]]:
    """Return a deterministic byte inventory of every tracked Markdown file."""
    rows: list[dict[str, object]] = []
    for path in tracked:
        data = (root / path).read_bytes()
        rows.append(
            {
                "path": path,
                "classification": "vendored" if _is_vendor(path) else "first_party",
                "sha256": hashlib.sha256(data).hexdigest(),
                "bytes": len(data),
                "lines": len(data.splitlines()),
            }
        )
    return rows


def _enforce_population(tracked: list[str]) -> tuple[list[str], list[str]]:
    """Fail closed if any tracked Markdown population collapses."""
    first_party = [path for path in tracked if not _is_vendor(path)]
    vendored = [path for path in tracked if _is_vendor(path)]
    populations = (
        ("tracked Markdown", len(tracked), MIN_TRACKED_MARKDOWN),
        ("first-party Markdown", len(first_party), MIN_FIRST_PARTY_MARKDOWN),
        ("vendor Markdown", len(vendored), MIN_VENDOR_MARKDOWN),
    )
    for label, actual, minimum in populations:
        if actual < minimum:
            _fail(f"{label} census collapsed: {actual} < {minimum}")
    return first_party, vendored


def _link_finding(
    root: Path, source: str, link: LinkRef, parsed: dict[str, Document]
) -> Finding | None:
    """Validate one structurally parsed local link."""
    target, fragment, classification = _resolve_link(root, source, link.target)
    if classification != "local" or target is None:
        return None
    try:
        rel_target = target.relative_to(root.resolve()).as_posix()
    except ValueError:
        return Finding("link-outside-repo", source, link.line, link.target, str(target))
    if not target.exists():
        return Finding("missing-link-target", source, link.line, link.target, rel_target)
    finding = None
    if LOCAL_LINE_FRAGMENT_RE.fullmatch(fragment):
        detail = "local line anchors rot when surrounding source moves; cite a symbol instead"
        finding = Finding("rot-prone-line-link", source, link.line, link.target, detail)
    elif fragment and target.is_file() and target.suffix.lower() == ".md":
        target_doc = parsed.get(rel_target)
        if target_doc is None:
            target_doc = parse_document(target.read_text(encoding="utf-8", errors="replace"))
        if fragment not in target_doc.anchors:
            detail = f"{rel_target} has no anchor {fragment!r}"
            finding = Finding("missing-link-anchor", source, link.line, link.target, detail)
    return finding


def _check_documents(root: Path, parsed: dict[str, Document]) -> tuple[list[Finding], int, int]:
    """Validate all parsed references and return findings plus exact censuses."""
    findings: list[Finding] = []
    links = 0
    paths = 0
    for source, document in parsed.items():
        findings.extend(
            Finding(
                "duplicate-anchor",
                source,
                collision.line,
                collision.value,
                "document id is already defined",
            )
            for collision in document.anchor_collisions
        )
        findings.extend(
            Finding(
                "missing-reference-definition",
                source,
                reference.line,
                reference.target,
                "reference-style link has no definition",
            )
            for reference in document.missing_references
        )
        for link in document.links:
            links += 1
            if finding := _link_finding(root, source, link, parsed):
                findings.append(finding)
        for path_ref in document.paths:
            paths += 1
            if reason := _path_reason(root, source, path_ref):
                findings.append(
                    Finding("missing-code-path", source, path_ref.line, path_ref.token, reason)
                )
    return findings, links, paths


def check_tree(
    root: Path, *, enforce_census: bool = True, include_untracked: bool = True
) -> tuple[list[Finding], dict[str, int], list[dict[str, object]]]:
    """Check every first-party Markdown document and return evidence counts."""
    scope, tracked = _git_files(root, include_untracked=include_untracked)
    tracked_first = [path for path in tracked if not _is_vendor(path)]
    tracked_vendor = [path for path in tracked if _is_vendor(path)]
    if enforce_census:
        tracked_first, tracked_vendor = _enforce_population(tracked)
    authored = [path for path in scope if not _is_vendor(path)]
    parsed = {
        path: parse_document((root / path).read_text(encoding="utf-8", errors="replace"))
        for path in authored
    }
    findings, link_count, path_count = _check_documents(root, parsed)
    if enforce_census:
        findings.extend(_bare_declaration_findings(root, parsed))
    if enforce_census:
        if link_count < MIN_LINK_REFERENCES:
            _fail(f"link-reference census collapsed: {link_count} < {MIN_LINK_REFERENCES}")
        if path_count < MIN_PATH_REFERENCES:
            _fail(f"code-path census collapsed: {path_count} < {MIN_PATH_REFERENCES}")
    counts = {
        "tracked_markdown": len(tracked),
        "tracked_first_party": len(tracked_first),
        "tracked_vendored": len(tracked_vendor),
        "scanned_first_party": len(authored),
        "links": link_count,
        "code_paths": path_count,
    }
    return findings, counts, _inventory(root, tracked)


# Public selftest seam.  Production uses ``check_tree``; the companion
# both-direction test module uses these aliases without importing private APIs.
bare_declaration_findings = _bare_declaration_findings
context_sha256 = _context_sha256
declared_bare_code_file = _declared_bare_code_file
declared_work_fixture = _declared_work_fixture
fail = _fail
git = _git
is_vendor = _is_vendor
path_reason = _path_reason
tracked_basename_index = _tracked_basename_index
