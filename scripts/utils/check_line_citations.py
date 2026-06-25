#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_line_citations.py -- Reject in-tree source citations with line numbers.

Per CLAUDE.md "Code Style / Comment citations":

  Comments must NOT reference files in this repo by line number
  (e.g. `libs/foo.c:776`). Line numbers go stale on the next reformat.
  Reference the function / symbol name instead.

  External / vendor citations (HUM, FSP, RFC, datasheet) remain
  MANDATORY for any HAL register access, ISR, or driver path.

Scope:
  Scans C / C++ source comments under libs/, src/, tests/, examples/,
  port/. Flags tokens matching `<file>.<ext>:<line>` inside `// ...`
  or `/* ... */` comments.

  Also scans Markdown (`.md`) and plain-text (`.txt`) files under
  `docs/` and the repo root. The same regex applies; in docs the
  whole line is treated as the "comment" (no comment-span extraction).

Exemptions:
  * docs/reference/* paths (HUM PDFs etc).
  * libs/third_party/* (SOUP -- not our citations to manage).
  * Any line containing `CITES-OK: <reason>` (reason text required).
  * CHANGELOG-style "moved from <file>:NNN to ..." historical notes.
  * `// SPDX-License-Identifier:` headers and `#include` directives.
  * In docs: lines inside fenced code blocks whose info-string mentions
    a tool name (`cppcheck`, `clang`, `clang-tidy`, `llvm-cov`, `gdb`,
    `objdump`, `readelf`) -- these are tool transcripts, not citations.
  * In docs: inline-code spans (backticked) whose first token is one
    of the same tool names.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Strict: cleanup wave landed; gate now blocks any new line-citation
# violation. See docs/CITATION_POLICY.md for the rule and the
# `// CITES-OK: <reason>` per-line opt-out.
WARN_ONLY_MODE = False

# Snippet display limit for violation output lines.
MAX_SNIPPET_LEN = 120
SNIPPET_TRUNCATE_LEN = 117

SCAN_ROOTS = ("libs/", "src/", "tests/", "examples/", "port/")
DOC_SCAN_ROOTS = ("docs/", "")  # "" => repo root (top-level *.md / *.txt)
EXCLUDE_PREFIXES = ("libs/third_party/", "docs/reference/")
SOURCE_EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")
DOC_EXTS = (".md", ".txt")
TOOL_OUTPUT_TOKENS = (
    "cppcheck",
    "clang-tidy",
    "clang",
    "llvm-cov",
    "gdb",
    "objdump",
    "readelf",
)

CITATION_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_/.-]*\.(?:c|h|cpp|hpp|cc):\d+\b")
CITES_OK_RE = re.compile(r"CITES-OK:\s*(\S.*?)\s*$")
MOVED_FROM_RE = re.compile(r"moved from\s+\S+:\d+\s+to\b", re.IGNORECASE)
DOCS_REFERENCE_RE = re.compile(r"\bdocs/reference/")
THIRD_PARTY_RE = re.compile(r"\blibs/third_party/")


def staged_files() -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def all_tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files"],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def is_in_scope(path: str) -> bool:
    if not path.endswith(SOURCE_EXTS):
        return False
    if not any(path.startswith(r) for r in SCAN_ROOTS):
        return False
    return not any(path.startswith(p) for p in EXCLUDE_PREFIXES)


def is_doc_in_scope(path: str) -> bool:
    """Markdown/plain-text under docs/ or at repo root."""
    if not path.endswith(DOC_EXTS):
        return False
    if any(path.startswith(p) for p in EXCLUDE_PREFIXES):
        return False
    if path.startswith("docs/"):
        return True
    # Top-level (no `/`) markdown / txt at repo root.
    return "/" not in path


def _line_is_tool_exempt(line: str) -> bool:
    """Heuristic: line looks like tool transcript output."""
    line.lstrip()
    # Inline-code span starting with a known tool token.
    # Examples:  `clang foo.c:12: warning: ...`
    #            "  `cppcheck libs/x.c:99 ...`"
    for tok in TOOL_OUTPUT_TOKENS:
        if tok in line:
            # Require the token to appear *before* the first file:line
            # match on the line (i.e. it owns/produced that citation).
            tok_pos = line.find(tok)
            m = CITATION_RE.search(line)
            if m and tok_pos < m.start():
                return True
    return False


def scan_doc_file(path: Path) -> list[tuple[int, str, str]]:
    """Scan a markdown/text doc file. Returns list of (line_no,
    matched_text, snippet) violations.

    Exemptions:
      * Fenced code blocks whose info-string contains a tool name.
      * Lines where a tool token precedes the citation (inline tool
        transcript).
      * `CITES-OK: <reason>` opt-out.
      * `moved from <file>:NN to ...` historical notes.
      * Anchor links / heading IDs of the form `(file:NN)` are still
        considered violations -- that is exactly what we want to ban.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    violations: list[tuple[int, str, str]] = []
    in_fence = False
    fence_is_tool = False
    fence_marker = ""
    for line_no, raw in enumerate(text.splitlines(), start=1):
        stripped = raw.lstrip()
        # Fence open/close tracking (``` or ~~~).
        if not in_fence and (stripped.startswith(("```", "~~~"))):
            in_fence = True
            fence_marker = stripped[:3]
            info = stripped[3:].strip().lower()
            fence_is_tool = any(tok in info for tok in TOOL_OUTPUT_TOKENS)
            continue
        if in_fence and stripped.startswith(fence_marker):
            in_fence = False
            fence_is_tool = False
            fence_marker = ""
            continue
        if in_fence and fence_is_tool:
            continue
        for m in CITATION_RE.finditer(raw):
            matched = m.group(0)
            if DOCS_REFERENCE_RE.search(matched) or DOCS_REFERENCE_RE.search(raw):
                continue
            if THIRD_PARTY_RE.search(matched) or THIRD_PARTY_RE.search(raw):
                continue
            cok = CITES_OK_RE.search(raw)
            if cok and cok.group(1).strip():
                continue
            if MOVED_FROM_RE.search(raw):
                continue
            if _line_is_tool_exempt(raw):
                continue
            snippet = raw.strip()
            if len(snippet) > MAX_SNIPPET_LEN:
                snippet = snippet[:SNIPPET_TRUNCATE_LEN] + "..."
            violations.append((line_no, matched, snippet))
    return violations


def find_comment_spans(text: str) -> list[tuple[int, int]]:  # noqa: PLR0912  # parser/gate dispatch, splitting hurts readability
    """Return list of (start, end) byte offsets that lie inside C/C++
    comments. Block comments and line comments. String literal aware
    enough to skip `"//foo"` and `"/* */"`."""
    spans: list[tuple[int, int]] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        # String literal
        if ch == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == "\n":
                    break
                i += 1
            i += 1
            continue
        if ch == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\" and i + 1 < n:
                    i += 2
                    continue
                if text[i] == "\n":
                    break
                i += 1
            i += 1
            continue
        if ch == "/" and i + 1 < n:
            if text[i + 1] == "/":
                start = i
                while i < n and text[i] != "\n":
                    i += 1
                spans.append((start, i))
                continue
            if text[i + 1] == "*":
                start = i
                i += 2
                while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                    i += 1
                i = min(n, i + 2)
                spans.append((start, i))
                continue
        i += 1
    return spans


def line_of_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def line_text(text: str, line_no: int) -> str:
    lines = text.splitlines()
    if 1 <= line_no <= len(lines):
        return lines[line_no - 1]
    return ""


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return list of (line_no, matched_text, comment_snippet) violations."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    violations: list[tuple[int, str, str]] = []
    spans = find_comment_spans(text)
    for start, end in spans:
        comment = text[start:end]
        # Per-line check: walk each match, validate against the
        # specific physical line it sits on (so CITES-OK: on the same
        # line excuses it).
        for m in CITATION_RE.finditer(comment):
            abs_off = start + m.start()
            line_no = line_of_offset(text, abs_off)
            matched = m.group(0)
            line = line_text(text, line_no)
            # Exemptions
            if "SPDX-License-Identifier" in line:
                continue
            stripped = line.lstrip()
            if stripped.startswith("#include"):
                continue
            if DOCS_REFERENCE_RE.search(matched) or DOCS_REFERENCE_RE.search(line):
                continue
            if THIRD_PARTY_RE.search(matched) or THIRD_PARTY_RE.search(line):
                continue
            cok = CITES_OK_RE.search(line)
            if cok and cok.group(1).strip():
                continue
            if MOVED_FROM_RE.search(line):
                continue
            snippet = line.strip()
            if len(snippet) > MAX_SNIPPET_LEN:
                snippet = snippet[:SNIPPET_TRUNCATE_LEN] + "..."
            violations.append((line_no, matched, snippet))
    return violations


def main() -> int:  # noqa: PLR0912  # parser/gate dispatch, splitting hurts readability
    repo_root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # trusted: fixed git argv
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    )

    full_scan = "--all" in sys.argv
    if full_scan:
        files = all_tracked_files()
    else:
        files = staged_files()
        if not files:
            files = all_tracked_files()
            full_scan = True

    src_files = [f for f in files if is_in_scope(f)]
    doc_files = [f for f in files if is_doc_in_scope(f)]

    total_violations = 0
    per_file_counts: dict[str, int] = {}
    for f in src_files:
        path = repo_root / f
        if not path.is_file():
            continue
        viols = scan_file(path)
        if not viols:
            continue
        per_file_counts[f] = len(viols)
        for line_no, matched, snippet in viols:
            print(f"{f}:{line_no}: line-citation found ('{matched}'): {snippet}")
            print("       fix: replace with function/symbol name, or add `// CITES-OK: <reason>`")
            total_violations += 1

    for f in doc_files:
        path = repo_root / f
        if not path.is_file():
            continue
        viols = scan_doc_file(path)
        if not viols:
            continue
        per_file_counts[f] = len(viols)
        for line_no, matched, snippet in viols:
            print(f"{f}:{line_no}: line-citation found ('{matched}'): {snippet}")
            print("       fix: replace with function/symbol name, or add `// CITES-OK: <reason>`")
            total_violations += 1

    if total_violations == 0:
        return 0

    print(file=sys.stderr)
    print(
        f"check_line_citations: {total_violations} violation(s) across "
        f"{len(per_file_counts)} file(s).",
        file=sys.stderr,
    )
    if WARN_ONLY_MODE:
        print(
            "check_line_citations: WAVE 0 -- warn-only, not blocking commit.",
            file=sys.stderr,
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
