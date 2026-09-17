# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared lexical helpers for the Zig test-contract gate."""

from __future__ import annotations

import re
from pathlib import Path

TEST_DECL_RE = re.compile(r'(?m)^\s*test(?:\s+(?:"[^"\n]+"|[A-Za-z_][A-Za-z0-9_]*))?\s*\{')


def without_zig_comments(text: str) -> str:
    """Remove line and nested block comments while preserving line structure."""
    output: list[str] = []
    index = 0
    block_depth = 0
    while index < len(text):
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
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def test_declarations(path: Path) -> list[str]:
    """Return real Zig test declarations, excluding comment text."""
    return TEST_DECL_RE.findall(without_zig_comments(path.read_text(encoding="utf-8")))
