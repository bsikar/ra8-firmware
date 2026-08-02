#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""extract_line_citations.py -- Emit a CSV of in-tree line citations.

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
from collections.abc import Iterator
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from line_citation_lex import (
    CITATION_RE,
    all_tracked_files,
    find_comment_spans,
    find_mcdc_reason_spans,
    is_exempt,
    line_of_offset,
)
from line_citation_lex import is_in_scope as _lex_in_scope

EXCLUDE_PREFIXES = ("libs/third_party/",)


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
    """Cached line list for a file, or None when it cannot be read.

    Cached because resolving the enclosing function for each citation re-reads
    the same few files repeatedly; the cache turns that quadratic re-read into
    one read per file.
    """
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


def is_in_scope(path: str) -> bool:
    """Whether a source path is subject to the ban, with this tool's exclusions."""
    return _lex_in_scope(path, EXCLUDE_PREFIXES)


def _rows_for_file(repo_root: Path, rel: str) -> Iterator[list[object]]:
    """Yield one CSV row per non-exempt line-citation in ``rel``.

    Exemptions come from line_citation_lex.is_exempt -- the same predicate the
    gate applies -- so this aid cannot list work the gate does not want, nor
    miss work it does. It used to carry its own hand-copied cascade.
    """
    path = repo_root / rel
    if not path.is_file():
        return
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    lines = text.splitlines()
    seen: set[tuple[int, str]] = set()
    # Comments AND RA8_MCDC_DEACTIVATED(...) reason strings, so this aid lists
    # exactly the set check_line_citations flags -- the gate scans both, and the
    # two tools must not describe different trees (#547).
    for start, end in find_comment_spans(text) + find_mcdc_reason_spans(text):
        for m in CITATION_RE.finditer(text[start:end]):
            line_no = line_of_offset(text, start + m.start())
            matched = m.group(0)
            line = lines[line_no - 1] if 1 <= line_no <= len(lines) else ""
            if is_exempt(matched, line):
                continue
            if (line_no, matched) in seen:
                continue
            seen.add((line_no, matched))
            yield [
                rel,
                line_no,
                matched,
                enclosing_function(lines, line_no),
                resolve_target(repo_root, m.group(1), int(m.group(2))),
                line.strip(),
            ]


def main() -> int:
    """List every in-tree ``file:line`` citation with the function that encloses it.

    A migration aid, not a gate: check_line_citations is what FAILS on these,
    and this prints the same set together with the symbol name each one should
    be rewritten to cite. Exits 0 regardless of what it finds.
    """
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
        for row in _rows_for_file(repo_root, f):
            writer.writerow(row)
            rows += 1

    print(f"# {rows} rows", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
