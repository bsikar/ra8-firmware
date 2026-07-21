# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shell function-body measurement for ``check_function_size.py``.

Shell has two body forms and this tree uses both deliberately:

    name() { ... }      the ordinary function
    name() ( ... )      a SUBSHELL body

The subshell form is load-bearing in ``scripts/ci.sh``: every multi-command
gate body is ``gate_x() ( set -e; ... )`` so that ``set -e`` is scoped to the
gate and a mid-body failure cannot be swallowed.  A parser that only knew
``{`` would silently measure nothing for the majority of gate bodies -- the
exact "gate that quietly stopped matching" failure this repository keeps
finding, so both forms are handled and the selftest asserts both.

Like the C side this is a textual scan, and for the same reason: there is no
shell equivalent of a compile database to lean on.  Depth tracking ignores
delimiters inside single and double quotes, inside ``#`` comments, and inside
heredoc bodies -- a heredoc carrying JSON or a CMake fragment is full of braces
that open no scope.
"""

from __future__ import annotations

import re
from pathlib import Path

# `name() {`, `name() (`, `function name {`, and the same with the delimiter on
# the following line.
_OPEN_RE = re.compile(r"^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_:.-]*)\s*(?:\(\))\s*([({])?\s*$")

_PAIRS = {"{": "}", "(": ")"}


def _strip_inert(line: str) -> str:
    """Blank out quoted spans and trailing comments, keeping length irrelevant.

    Only the delimiters matter downstream, so quoted text is replaced rather
    than removed. Escapes are honoured so ``\\"`` does not end a string.
    """
    out: list[str] = []
    quote = ""
    i = 0
    while i < len(line):
        ch = line[i]
        if quote:
            if ch == "\\" and quote == '"':
                i += 2
                continue
            if ch == quote:
                quote = ""
            i += 1
            continue
        if ch == "\\":
            i += 2
            continue
        if ch in ("'", '"'):
            quote = ch
            i += 1
            continue
        if ch == "#" and (not out or out[-1].isspace()):
            break
        out.append(ch)
        i += 1
    return "".join(out)


def _heredoc_tag(line: str) -> str | None:
    """The delimiter of a heredoc opened on `line`, or None."""
    match = re.search(r"<<-?\s*[\"']?([A-Za-z_][A-Za-z0-9_]*)[\"']?", line)
    return match.group(1) if match else None


def _body_end(lines: list[str], open_idx: int, opener: str) -> int:
    """Index one past the body whose opening delimiter is on `lines[open_idx]`."""
    closer = _PAIRS[opener]
    depth = 0
    heredoc: str | None = None
    idx = open_idx
    while idx < len(lines):
        raw = lines[idx]
        if heredoc is not None:
            if raw.strip() == heredoc:
                heredoc = None
            idx += 1
            continue
        clean = _strip_inert(raw)
        depth += clean.count(opener) - clean.count(closer)
        tag = _heredoc_tag(clean)
        if tag is not None:
            heredoc = tag
        if depth <= 0:
            return idx + 1
        idx += 1
    return len(lines)


def scan(path: Path, threshold: int) -> list[tuple[int, int, str]]:
    """Return (start_line, length, signature) for functions over `threshold`."""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []

    out: list[tuple[int, int, str]] = []
    idx = 0
    while idx < len(lines):
        match = _OPEN_RE.match(lines[idx])
        if match is None:
            idx += 1
            continue
        opener = match.group(2)
        open_idx = idx
        if opener is None:
            # The delimiter is on the next non-blank line, or this is not a
            # function definition at all.
            nxt = idx + 1
            while nxt < len(lines) and not lines[nxt].strip():
                nxt += 1
            if nxt >= len(lines) or lines[nxt].strip() not in ("{", "("):
                idx += 1
                continue
            opener = lines[nxt].strip()
            open_idx = nxt
        end = _body_end(lines, open_idx, opener)
        length = end - idx
        if length > threshold:
            out.append((idx + 1, length, lines[idx].strip()))
        idx = max(end, idx + 1)
    return out
