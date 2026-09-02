# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural Markdown inline-link parsing without hiding malformed prose."""

from __future__ import annotations


def split_link_destination(raw: str) -> str:
    """Discard a Markdown link title while retaining an angle-wrapped target."""
    raw = raw.strip()
    if not raw:
        return ""
    if raw.startswith("<") and ">" in raw:
        return raw[1 : raw.index(">")]
    depth = 0
    for index, char in enumerate(raw):
        if char == "(":
            depth += 1
        elif char == ")" and depth:
            depth -= 1
        elif char.isspace() and depth == 0:
            return raw[:index]
    return raw


def _balanced_target_end(line: str, start: int) -> int | None:
    """Return the closing parenthesis for one proven-balanced destination."""
    index = start
    depth = 1
    angle = False
    escaped = False
    while index < len(line):
        char = line[index]
        if escaped:
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == "<" and depth == 1:
            angle = True
        elif char == ">" and angle:
            angle = False
        elif not angle and char == "(":
            depth += 1
        elif not angle and char == ")":
            depth -= 1
            if depth == 0:
                return index
        index += 1
    return None


def inline_link_targets(line: str) -> list[str]:
    """Parse ``](destination)`` pairs with balanced parentheses."""
    targets: list[str] = []
    cursor = 0
    while True:
        opener = line.find("](", cursor)
        if opener < 0:
            break
        start = opener + 2
        end = _balanced_target_end(line, start)
        if end is not None:
            targets.append(split_link_destination(line[start:end]))
        cursor = end + 1 if end is not None else start
    return targets


def mask_inline_link_targets(line: str) -> str:
    """Blank balanced inline-link destinations before prose path scanning."""
    masked = list(line)
    cursor = 0
    while True:
        opener = line.find("](", cursor)
        if opener < 0:
            break
        start = opener + 2
        end = _balanced_target_end(line, start)
        if end is not None:
            masked[start:end] = " " * (end - start)
        cursor = end + 1 if end is not None else start
    return "".join(masked)
