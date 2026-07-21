#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
extract_line_citations.py -- Emit a CSV of in-tree line citations.

Companion to scripts/checks/check_line_citations.py. Walks the same set
of files (libs/, src/, tests/, examples/, port/), finds every
`<file>.<ext>:<line>` token inside C/C++ comments, and emits one row
per violation:

    file,line_number,matched_citation,enclosing_function,
    suggested_replacement,full_comment_snippet

`enclosing_function` is the most recent function definition signature
above the citation in the same file (or "(file scope)" if none).

`suggested_replacement` resolves the citation target -- opens
`<file>` at `<line>` and reports `<basename>::<func_name>` so the
caller can rewrite `libs/foo/src/bar.c:776` as `bar.c::priv_foo`.

Output: CSV on stdout. One-shot agent tool, not wired into pre-commit.
"""

from __future__ import annotations

import csv
import re
import subprocess
import sys
from pathlib import Path

SCAN_ROOTS = ("libs/", "src/", "tests/", "examples/", "port/")
EXCLUDE_PREFIXES = ("libs/third_party/",)
SOURCE_EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")

CITATION_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_/.-]*\.(?:c|h|cpp|hpp|cc)):(\d+)\b")
CITES_OK_RE = re.compile(r"CITES-OK:\s*(\S.*?)\s*$")
MOVED_FROM_RE = re.compile(r"moved from\s+\S+:\d+\s+to\b", re.IGNORECASE)
DOCS_REFERENCE_RE = re.compile(r"\bdocs/reference/")
THIRD_PARTY_RE = re.compile(r"\blibs/third_party/")

# Heuristic: a function definition has a return type, name, params, and
# an opening brace either on the signature line or on the next line.
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{?\s*$")
# Common keywords that look like function defs but aren't.
FUNC_DEF_DENYLIST = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "static_assert",
    "typeof",
    "do",
}


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


def find_comment_spans(text: str) -> list[tuple[int, int]]:  # noqa: PLR0912  # parser/gate dispatch, splitting hurts readability
    spans: list[tuple[int, int]] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
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


def enclosing_function(lines: list[str], target_line: int) -> str:
    """Walk backward from target_line looking for a function definition."""
    for idx in range(min(target_line - 1, len(lines) - 1), -1, -1):
        line = lines[idx]
        # Skip lines inside comments / preprocessor / blank
        stripped = line.strip()
        if not stripped or stripped.startswith(("//", "/*", "*", "#")):
            continue
        m = FUNC_DEF_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name in FUNC_DEF_DENYLIST:
            continue
        # Sanity: avoid matching variable declarations like `int x = foo();`
        if "=" in line.split("(")[0]:
            continue
        return name
    return "(file scope)"


_FILE_CACHE: dict[Path, list[str]] = {}


def file_lines(path: Path) -> list[str] | None:
    if path in _FILE_CACHE:
        return _FILE_CACHE[path]
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = text.splitlines()
    _FILE_CACHE[path] = lines
    return lines


def resolve_target(repo_root: Path, citation_path: str, citation_line: int) -> str:
    """Try to find the target file and report basename::func."""
    # Try as repo-relative
    candidates: list[Path] = []
    p = repo_root / citation_path
    if p.is_file():
        candidates.append(p)
    else:
        # Search by basename across tracked files
        base = Path(citation_path).name
        for f in all_tracked_files():
            if Path(f).name == base and is_in_scope(f):
                candidates.append(repo_root / f)
                break
    if not candidates:
        return f"{Path(citation_path).name}::(unknown)"
    target = candidates[0]
    lines = file_lines(target)
    if lines is None:
        return f"{target.name}::(unreadable)"
    func = enclosing_function(lines, citation_line)
    return f"{target.name}::{func}"


def main() -> int:
    repo_root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # trusted: fixed git argv
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    )

    files = [f for f in all_tracked_files() if is_in_scope(f)]

    writer = csv.writer(sys.stdout)
    writer.writerow(
        [
            "file",
            "line_number",
            "matched_citation",
            "enclosing_function",
            "suggested_replacement",
            "full_comment_snippet",
        ]
    )

    rows = 0
    for f in files:
        path = repo_root / f
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        lines = text.splitlines()
        spans = find_comment_spans(text)
        for start, end in spans:
            comment = text[start:end]
            for m in CITATION_RE.finditer(comment):
                abs_off = start + m.start()
                line_no = line_of_offset(text, abs_off)
                matched = m.group(0)
                cite_path = m.group(1)
                cite_line = int(m.group(2))
                line = lines[line_no - 1] if 1 <= line_no <= len(lines) else ""
                # Apply the same exemptions as the gate
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
                enclosing = enclosing_function(lines, line_no)
                suggested = resolve_target(repo_root, cite_path, cite_line)
                snippet = line.strip()
                writer.writerow(
                    [
                        f,
                        line_no,
                        matched,
                        enclosing,
                        suggested,
                        snippet,
                    ]
                )
                rows += 1

    print(f"# {rows} rows", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
