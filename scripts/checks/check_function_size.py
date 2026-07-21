#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: NASA Power-of-10 Rule 4 -- functions shall fit in one display
page (~60 source lines).

The project's `.clang-tidy` already configures
``readability-function-size.LineThreshold: 60``, but clang-tidy only
checks files that are present in the build's ``compile_commands.json``.
Because the host unit-test build (where ``clang_tidy.sh`` runs) drops
every ARM-cross-compiled translation unit -- anything that pulls in
ThreadX, USBX, NetX, GLCDC register headers, or MCU intrinsics --
those files never see a clang-tidy pass.  Around 90% of ``port/``,
large parts of ``libs/ra8_hal/``, and every cross-compiled example
``main.c`` were silently exempt.

This checker is a backstop that walks the source text directly so the
gate is enforced for **every** ``.c`` file under ``libs/``, ``src/``,
``port/``, ``examples/``, ``tools/``, and ``tests/`` regardless of which
compile database it ended up in.  Third-party vendor trees
(``libs/third_party/``, ``port/threadx/``) are excluded -- those are SOUP
and their function sizes are the upstream maintainer's call, not ours.

Run::

    check_function_size.py                      # scan the whole tree
    check_function_size.py path/to/file.c ...   # scan listed files

Exit 0 if every function is at or below the threshold, exit 1 (with a
diagnostic table) if any function is over.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# NASA Power-of-10 Rule 4: ~60 source lines per function.  Matches the
# value in ``.clang-tidy`` (readability-function-size.LineThreshold).
THRESHOLD_LINES = 60

# Maximum signature length printed in the diagnostic table before truncation.
SIG_DISPLAY_MAX = 70
SIG_DISPLAY_TRUNCATED = SIG_DISPLAY_MAX - 3  # reserve three chars for "..."

# Directories to scan when called with no argument.
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "tests")

# Path fragments that exclude the file from the scan.  Vendor / build
# trees are intentionally exempt; generated Vela model blobs are machine
# output; ``_unsupported/`` examples are kept off the bench pending
# hardware bring-up.
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "port/threadx/",
    "tools/vela/generated/",
    "/build/",
    "_unsupported/",
)


# Heuristic: a top-level function body opens with ``{`` on its own line
# (or, less commonly, at the end of the signature line).  The signature
# spans the lines immediately above ``{`` whose first non-whitespace
# character is *not* a control-flow keyword.
_CONTROL_PREFIXES = (
    "if ",
    "if(",
    "for ",
    "for(",
    "while ",
    "while(",
    "switch ",
    "switch(",
    "else",
    "do ",
    "do{",
    "do\t",
    "//",
    "/*",
    "*",
    "}",
)


def _brace_delta(line: str, in_block_comment: bool) -> tuple[int, int, bool]:
    """Count net ``{`` / ``}`` on `line`, skipping braces that sit inside
    string literals, character literals, or comments.

    A naive ``line.count("{")`` miscounts every brace that appears in a
    textual constant -- a JSON/CSS/JS blob written as a C string literal
    (``"{\\"$schema\\":..."``) or a ``case '{':`` character literal in a
    tokenizer.  Those braces do not open a real scope, so counting them
    runs the depth tracker off the true closing ``}`` and reports a short
    function (e.g. ``prof_write_speedscope``, 49 lines) as hundreds of
    lines.  This scanner walks the line character by character and ignores
    any brace that is not live C punctuation.

    Only ``in_block_comment`` persists across lines: C string and character
    literals do not span source lines in this codebase (no backslash-newline
    line-continued literals -- adjacent string concatenation is used
    instead), so they are always resolved within the one line.

    Returns ``(opens, closes, in_block_comment_after)``.
    """
    opens = 0
    closes = 0
    i = 0
    n = len(line)
    # "" while outside a literal; otherwise the opening quote (`"` or `'`).
    # Unifying string and character literals keeps the branch count down.
    in_quote = ""
    while i < n:
        c = line[i]
        if in_block_comment:
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_quote:
            if c == "\\":  # skip the escaped character (e.g. \" or \\)
                i += 2
                continue
            if c == in_quote:
                in_quote = ""
            i += 1
            continue
        # Outside any literal or comment: interpret the punctuation.
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break  # line comment: the remainder of the line is inert
        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_block_comment = True
            i += 2
            continue
        if c in ('"', "'"):
            in_quote = c
        elif c == "{":
            opens += 1
        elif c == "}":
            closes += 1
        i += 1
    return opens, closes, in_block_comment


def _looks_like_function_body_open(prev: str) -> bool:
    """Return True if `prev` is the last line of a function signature.

    Function signatures end in ``)`` (possibly followed by a trailing
    space).  Macros and control statements that open a brace also end
    in ``)`` -- those are filtered out by the caller via the keyword
    check.
    """
    stripped = prev.rstrip()
    if not stripped.endswith(")"):
        return False
    head = prev.lstrip()
    return all(not head.startswith(prefix) for prefix in _CONTROL_PREFIXES)


def _function_signature_start(lines: list[str], brace_idx: int) -> int:
    """Walk backward from the line containing the opening ``{`` to find
    where the function signature began.

    Heuristic: keep walking while previous lines look like continuation
    of the same declaration (no terminator like ``;``, ``}``, ``*/``
    on the previous line).  Stop once we hit a blank line, a
    terminator, or the start of the file.
    """
    i = brace_idx - 1
    while i > 0:
        prev = lines[i - 1].rstrip()
        if (not prev) or prev.endswith((";", "}", "*/")):
            break
        # Stop on preprocessor directives -- `#pragma` and `#if` blocks
        # sit between functions and are not part of any signature.
        if prev.lstrip().startswith("#"):
            break
        i -= 1
    return i


def _measure_body(lines: list[str], brace_idx: int, n: int) -> int:
    """Return the line index one past the function body whose opening ``{`` is
    on ``lines[brace_idx]``.

    Tracks brace depth from the opening ``{``, ignoring braces that sit inside
    string / character literals or comments (via `_brace_delta`).  A
    preprocessor-conditional arm stack keeps a brace opened in a *later*
    ``#elif``/``#else`` arm from double-counting a brace the first arm already
    opened -- only one arm is ever compiled, so counting both would run the
    depth off the true closing ``}`` (e.g. the poll-vs-direct
    ``#if RA8_SIMULATOR_MODE { ... #else { ... #endif`` idiom).  Each stack
    entry is False in the first arm and True once an ``#elif``/``#else`` is
    seen; brace deltas are applied only while every entry is False.
    """
    depth = 1
    j = brace_idx + 1
    cpp_arms: list[bool] = []
    # Block comments straddle lines, so their state persists across the body
    # scan; string / character literals are always resolved within one line.
    in_block_comment = False
    while j < n and depth > 0:
        if not in_block_comment:
            stripped = lines[j].lstrip()
            if stripped.startswith("#"):
                directive = stripped[1:].lstrip()
                if directive.startswith("endif"):
                    if cpp_arms:
                        cpp_arms.pop()
                elif directive.startswith(("else", "elif")):
                    if cpp_arms:
                        cpp_arms[-1] = True
                elif directive.startswith("if"):  # if / ifdef / ifndef
                    cpp_arms.append(False)
                j += 1
                continue
        opens, closes, in_block_comment = _brace_delta(lines[j], in_block_comment)
        if not any(cpp_arms):
            depth += opens
            depth -= closes
        j += 1
    return j


def _scan_file(path: Path) -> list[tuple[int, int, str]]:
    """Return a list of (function_start_line, length, signature) tuples
    for every function in `path` whose body exceeds the threshold."""
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError):
        return []

    lines = text.splitlines()
    violations: list[tuple[int, int, str]] = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i].lstrip()
        if line.startswith("{") and i > 0 and _looks_like_function_body_open(lines[i - 1]):
            sig_start = _function_signature_start(lines, i)
            j = _measure_body(lines, i, n)
            length = j - i
            if length > THRESHOLD_LINES:
                signature = lines[sig_start].strip()
                # 1-based line for editor friendliness.
                violations.append((sig_start + 1, length, signature))
            i = j
            continue
        i += 1
    return violations


def _is_excluded(path: Path) -> bool:
    p = str(path)
    return any(frag in p for frag in EXCLUDE_FRAGMENTS)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    """Resolve the list of files to scan from CLI arguments."""
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            p = Path(raw)
            if not p.is_absolute():
                p = REPO_ROOT / p
            if p.is_dir():
                out.extend(p.rglob("*.c"))
            elif p.suffix == ".c":
                out.append(p)
        return [p for p in out if not _is_excluded(p)]

    out = []
    for root in SCAN_ROOTS:
        out.extend((REPO_ROOT / root).rglob("*.c"))
    return [p for p in out if not _is_excluded(p)]


def main(argv: list[str]) -> int:
    targets = _enumerate_targets(argv[1:])
    if not targets:
        print("check_function_size.py: no files to scan", file=sys.stderr)
        return 0

    findings: list[tuple[int, str, int, str]] = []
    for path in targets:
        for line_no, length, signature in _scan_file(path):
            rel = path.relative_to(REPO_ROOT) if path.is_relative_to(REPO_ROOT) else path
            findings.append((length, str(rel), line_no, signature))

    if not findings:
        print(
            f"check_function_size.py: {len(targets)} file(s) scanned, "
            f"no functions over {THRESHOLD_LINES} lines."
        )
        return 0

    findings.sort(reverse=True)
    print(
        f"check_function_size.py: {len(findings)} function(s) exceed the "
        f"{THRESHOLD_LINES}-line cap (NASA P10 Rule 4):\n",
        file=sys.stderr,
    )
    print("  lines  file:line  signature", file=sys.stderr)
    for length, path, line_no, signature in findings:
        # Trim the signature so the table stays readable on an 80-col
        # terminal -- the file:line anchor is enough to jump to it.
        sig = (
            signature
            if len(signature) <= SIG_DISPLAY_MAX
            else signature[:SIG_DISPLAY_TRUNCATED] + "..."
        )
        print(f"  {length:5d}  {path}:{line_no}  {sig}", file=sys.stderr)
    print(
        "\nEach function must fit in one display page (<=60 lines).  Extract "
        "helpers or hoist compile-time tables to file scope to shrink the "
        "body.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
