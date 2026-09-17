# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shell operator masking for suppression inventory scans."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class ShellFrame:
    """One active shell lexical context."""

    kind: str
    depth: int = 0


@dataclass
class ShellOperatorState:
    """Cross-line quote and command-substitution state."""

    frames: list[ShellFrame] = field(default_factory=lambda: [ShellFrame("normal")])


def _mask_escape(code: str, index: int, masked: list[str]) -> int:
    """Mask one escape and its following byte."""
    masked.append(" ")
    if index + 1 < len(code):
        masked.append(" ")
        return index + 2
    return index + 1


def _consume_single(code: str, index: int, state: ShellOperatorState, masked: list[str]) -> int:
    """Consume one byte from a single-quoted literal."""
    masked.append(" ")
    if code[index] == "'":
        state.frames.pop()
    return index + 1


def _consume_double(code: str, index: int, state: ShellOperatorState, masked: list[str]) -> int:
    """Consume one byte from double quotes, exposing command substitutions."""
    char = code[index]
    if char == "\\":
        return _mask_escape(code, index, masked)
    if code.startswith("$(", index):
        masked.extend("  ")
        state.frames.append(ShellFrame("command", 1))
        return index + 2
    masked.append(" ")
    if char == '"':
        state.frames.pop()
    return index + 1


def _consume_active(code: str, index: int, state: ShellOperatorState, masked: list[str]) -> int:
    """Consume one byte from ordinary or command-substitution shell syntax."""
    char = code[index]
    frame = state.frames[-1]
    if char == "\\":
        return _mask_escape(code, index, masked)
    if char == "'":
        masked.append(" ")
        state.frames.append(ShellFrame("single"))
    elif char == '"':
        masked.append(" ")
        state.frames.append(ShellFrame("double"))
    elif char == "`":
        masked.append(" ")
        if frame.kind == "backtick":
            state.frames.pop()
        else:
            state.frames.append(ShellFrame("backtick"))
    elif code.startswith("$(", index):
        masked.extend("  ")
        state.frames.append(ShellFrame("command", 1))
        return index + 2
    elif frame.kind == "command" and char == "(":
        masked.append(char)
        frame.depth += 1
    elif frame.kind == "command" and char == ")":
        masked.append(char)
        frame.depth -= 1
        if frame.depth == 0:
            state.frames.pop()
    else:
        masked.append(char)
    return index + 1


def mask_shell_operators(code: str, state: ShellOperatorState) -> str:
    """Mask literal data but retain operators executed by this shell line."""
    masked: list[str] = []
    index = 0
    while index < len(code):
        frame = state.frames[-1]
        if frame.kind == "single":
            index = _consume_single(code, index, state, masked)
        elif frame.kind == "double":
            index = _consume_double(code, index, state, masked)
        else:
            index = _consume_active(code, index, state, masked)
    return "".join(masked)
