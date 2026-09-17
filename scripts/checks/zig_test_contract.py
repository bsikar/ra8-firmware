# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared lexical helpers for the Zig test-contract gate."""

from __future__ import annotations

import re
from pathlib import Path

TEST_DECL_RE = re.compile(r'(?m)^\s*test(?:\s+(?:"[^"\n]+"|[A-Za-z_][A-Za-z0-9_]*))?\s*\{')

ESCAPE = chr(92)
MULTILINE_STRING_OPEN = ESCAPE * 2
QUOTE_CHARS = (chr(34), chr(39))


def _copy_rest_of_line(text: str, index: int) -> tuple[str, int]:
    """Copy a multiline string literal, which runs to the end of its own line."""
    newline = text.find("\n", index)
    if newline < 0:
        return text[index:], len(text)
    return text[index : newline + 1], newline + 1


def _copy_quoted_literal(text: str, index: int) -> tuple[str, int]:
    """Copy a string or character literal verbatim, honouring backslash escapes."""
    quote = text[index]
    chunk = [quote]
    index += 1
    length = len(text)
    while index < length:
        char = text[index]
        if char == ESCAPE and index + 1 < length:
            chunk.append(text[index : index + 2])
            index += 2
            continue
        chunk.append(char)
        index += 1
        if char in (quote, "\n"):
            break
    return "".join(chunk), index


def without_zig_comments(text: str) -> str:
    """Remove line and nested block comments while preserving line structure.

    String, character and multiline-string literals are copied through
    verbatim. A comment marker inside a literal is data, not an opener, so
    reading one as an opener swallowed every declaration that followed it.
    """
    output: list[str] = []
    index = 0
    block_depth = 0
    length = len(text)
    while index < length:
        pair = text[index : index + 2]
        if block_depth:
            if pair == "/*":
                block_depth += 1
                index += 2
            elif pair == "*/":
                block_depth -= 1
                index += 2
            else:
                output.append("\n" if text[index] == "\n" else " ")
                index += 1
        elif pair == "//":
            newline = text.find("\n", index)
            if newline < 0:
                break
            output.append("\n")
            index = newline + 1
        elif pair == "/*":
            block_depth = 1
            index += 2
        elif pair == MULTILINE_STRING_OPEN:
            chunk, index = _copy_rest_of_line(text, index)
            output.append(chunk)
        elif text[index] in QUOTE_CHARS:
            chunk, index = _copy_quoted_literal(text, index)
            output.append(chunk)
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def test_declarations(path: Path) -> list[str]:
    """Return real Zig test declarations, excluding comment text."""
    return TEST_DECL_RE.findall(without_zig_comments(path.read_text(encoding="utf-8")))
