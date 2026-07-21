# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""C / C++ function-body measurement for ``check_function_size.py``.

Split out of the checker when the gate grew Python and shell parsers (#359):
three languages in one file put it well over the 1000-line file cap it is
itself half of, and a size gate that cannot pass its own rule has no standing
to enforce it.

The measurement is textual, not a real parse.  That is deliberate: the point of
this backstop is to cover translation units that never reach
``compile_commands.json`` -- the ARM-cross-compiled ones clang-tidy's
``readability-function-size`` never sees -- so it cannot depend on a compile
database existing.  The heuristics below (brace-depth tracking that ignores
literals and comments, a preprocessor-arm stack) are what make that textual
scan agree with a real parse on this codebase.

``scan(path, threshold)`` returns ``(start_line, length, signature)`` for every
function over `threshold` lines.
"""

from __future__ import annotations

from pathlib import Path

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
    r"""Net brace delta for one line, ignoring braces inside literals and comments.

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
    """Find the line where the function signature began.

    Walks backward from the line carrying the opening ``{``.

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
    """Line index one past the end of the function body.

    The body is the one whose opening ``{`` sits on ``lines[brace_idx]``.

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
    # Seed the depth from the opening line itself rather than assuming it
    # contributes exactly one. An empty body written `{}` on one line closes
    # immediately; assuming depth 1 ran the tracker past it and swallowed the
    # whole enclosing scope -- a C++ constructor with an initialiser list and a
    # `{}` body was reported as its entire 124-line class.
    opens, closes, in_block_comment_seed = _brace_delta(lines[brace_idx], in_block_comment=False)
    depth = opens - closes
    if depth <= 0:
        return brace_idx + 1
    j = brace_idx + 1
    cpp_arms: list[bool] = []
    # Block comments straddle lines, so their state persists across the body
    # scan; string / character literals are always resolved within one line.
    in_block_comment = in_block_comment_seed
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


def scan(path: Path, threshold: int) -> list[tuple[int, int, str]]:
    """Every function in ``path`` whose body exceeds ``threshold`` lines.

    Returns ``(function_start_line, length, signature)`` per offender; an
    unreadable file yields an empty list rather than raising.
    """
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
            if length > threshold:
                signature = lines[sig_start].strip()
                # 1-based line for editor friendliness.
                violations.append((sig_start + 1, length, signature))
            i = j
            continue
        i += 1
    return violations
