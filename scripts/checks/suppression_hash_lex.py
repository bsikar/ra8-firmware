# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Lexical helpers for languages whose ordinary comment marker is ``#``."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from suppression_catalog import is_shell_control
from suppression_model import Finding


@dataclass(frozen=True)
class HashLexLine:
    """Active code and optional comment from one hash-comment language line."""

    line: int
    code: str
    comment: str = ""
    comment_column: int = 0


@dataclass
class ShellLexState:
    """Cross-line quote and queued-heredoc state for shell source."""

    quote: str = ""
    arithmetic_depth: int = 0
    heredocs: list[tuple[str, bool]] = field(default_factory=list)


def _comment_index(raw: str, *, shell_words: bool = False) -> int | None:
    """Return the first unquoted hash-comment column on a shell-like line."""
    quote = ""
    escaped = False
    for index, char in enumerate(raw):
        if escaped:
            escaped = False
        elif char == "\\" and quote != "'":
            escaped = True
        elif quote and char == quote:
            quote = ""
        elif not quote and char in {'"', "'"}:
            quote = char
        elif (
            not quote
            and char == "#"
            and (
                not shell_words
                or index == 0
                or raw[index - 1].isspace()
                or raw[index - 1] in ";&|()<>"
            )
        ):
            return index
    return None


def _basic_lines(text: str, *, shell_words: bool) -> list[HashLexLine]:
    """Split ordinary hash-comment files into active code and comments."""
    lines: list[HashLexLine] = []
    for line_no, raw in enumerate(text.splitlines(), start=1):
        column = _comment_index(raw, shell_words=shell_words)
        if column is None or (line_no == 1 and raw.startswith("#!")):
            lines.append(HashLexLine(line_no, raw))
        else:
            lines.append(HashLexLine(line_no, raw[:column], raw[column + 1 :], column + 1))
    return lines


def _heredoc_word(raw: str, start: int) -> tuple[str, int, str]:
    """Parse one shell heredoc word, removing delimiter-only quoting."""
    index = start
    word: list[str] = []
    quote = ""
    while index < len(raw):
        char = raw[index]
        if quote:
            if char == quote:
                quote = ""
            elif char == "\\" and quote != "'" and index + 1 < len(raw):
                index += 1
                word.append(raw[index])
            else:
                word.append(char)
        elif char in {'"', "'"}:
            quote = char
        elif char == "\\" and index + 1 < len(raw):
            index += 1
            word.append(raw[index])
        elif char.isspace() or char in ";&|()<>":
            break
        else:
            word.append(char)
        index += 1
    error = "unterminated quote in heredoc delimiter" if quote else ""
    if not word and not error:
        error = "empty heredoc delimiter"
    return "".join(word), index, error


def _arithmetic_index(raw: str, index: int, state: ShellLexState) -> int | None:
    """Consume an arithmetic opener/closer, or report no state transition."""
    if raw.startswith("$((", index):
        state.arithmetic_depth += 1
        return index + 3
    if not state.arithmetic_depth and raw.startswith("((", index):
        state.arithmetic_depth += 1
        return index + 2
    if state.arithmetic_depth and raw.startswith("))", index):
        state.arithmetic_depth -= 1
        return index + 2
    return None


def _heredoc_index(raw: str, index: int, state: ShellLexState) -> tuple[int | None, Finding | None]:
    """Consume one real heredoc operator while ignoring shell here-strings."""
    if raw.startswith("<<<", index):
        return index + 3, None
    if state.arithmetic_depth or not raw.startswith("<<", index):
        return None, None
    word_start = index + 2
    strip_tabs = word_start < len(raw) and raw[word_start] == "-"
    word_start += int(strip_tabs)
    while word_start < len(raw) and raw[word_start] in " \t":
        word_start += 1
    delimiter, end, error = _heredoc_word(raw, word_start)
    if error:
        return max(end, word_start + 1), Finding("malformed-heredoc", error)
    state.heredocs.append((delimiter, strip_tabs))
    return max(end, word_start + 1), None


def _shell_code(raw: str, state: ShellLexState) -> tuple[str, str, int, list[Finding]]:
    """Split one active shell line and queue quote-aware heredoc delimiters."""
    index = 0
    findings: list[Finding] = []
    while index < len(raw):
        char = raw[index]
        if state.quote:
            if char == state.quote:
                state.quote = ""
            elif char == "\\" and state.quote != "'" and index + 1 < len(raw):
                index += 1
            index += 1
            continue
        if char in {'"', "'", "`"}:
            state.quote = char
            index += 1
            continue
        if char == "\\":
            index += 2
            continue
        arithmetic_index = _arithmetic_index(raw, index, state)
        if arithmetic_index is not None:
            index = arithmetic_index
            continue
        if char == "#" and (index == 0 or raw[index - 1].isspace() or raw[index - 1] in ";&|()<>"):
            return raw[:index], raw[index + 1 :], index + 1, findings
        heredoc_index, finding = _heredoc_index(raw, index, state)
        if heredoc_index is not None:
            if finding is not None:
                findings.append(finding)
            index = heredoc_index
            continue
        index += 1
    return raw, "", 0, findings


def _shell_lines(text: str) -> tuple[list[HashLexLine], list[Finding]]:
    """Split shell comments and fail closed on malformed heredoc/quote state."""
    lines: list[HashLexLine] = []
    findings: list[Finding] = []
    state = ShellLexState()
    for line_no, raw in enumerate(text.splitlines(), start=1):
        if state.heredocs:
            delimiter, strip_tabs = state.heredocs[0]
            candidate = raw.lstrip("\t") if strip_tabs else raw
            if candidate == delimiter:
                state.heredocs.pop(0)
            lines.append(HashLexLine(line_no, ""))
            continue
        code, comment, column, line_findings = _shell_code(raw, state)
        lines.append(HashLexLine(line_no, code, comment, column))
        findings.extend(Finding(item.code, item.message, line=line_no) for item in line_findings)
    if state.heredocs:
        delimiters = ", ".join(item[0] for item in state.heredocs)
        findings.append(Finding("unterminated-heredoc", delimiters))
    if state.quote:
        findings.append(Finding("unterminated-shell-quote", state.quote))
    if state.arithmetic_depth:
        findings.append(Finding("unterminated-shell-arithmetic", str(state.arithmetic_depth)))
    return lines, findings


def _yaml_lines(text: str) -> list[HashLexLine]:
    """Split YAML comments without treating block-scalar payload as YAML syntax."""
    lines: list[HashLexLine] = []
    block_indent: int | None = None
    for line_no, raw in enumerate(text.splitlines(), start=1):
        indent = len(raw) - len(raw.lstrip(" "))
        if block_indent is not None and (not raw.strip() or indent > block_indent):
            lines.append(HashLexLine(line_no, ""))
            continue
        block_indent = None
        column = _comment_index(raw, shell_words=True)
        code = raw if column is None else raw[:column]
        comment = "" if column is None else raw[column + 1 :]
        lines.append(HashLexLine(line_no, code, comment, 0 if column is None else column + 1))
        indicator = r"[>|](?:[+-]?[1-9]?|[1-9][+-]?)\s*$"
        if re.search(rf"(?:^\s*-\s+|:\s*){indicator}", code):
            block_indent = indent
    return lines


def _cmake_code_line(raw: str, bracket_state: str) -> tuple[str, str, int, str]:
    """Mask CMake bracket arguments and return code, comment, column, and state."""
    code: list[str] = []
    index = 0
    if bracket_state:
        bracket_end = bracket_state[1:]
        end = raw.find(bracket_end)
        if end < 0:
            return "", "", 0, bracket_state
        index = end + len(bracket_end)
        bracket_state = ""
    quote = ""
    while index < len(raw):
        char = raw[index]
        if quote:
            code.append(char)
            if char == "\\" and index + 1 < len(raw):
                index += 1
                code.append(raw[index])
            elif char == quote:
                quote = ""
            index += 1
            continue
        if char == '"':
            quote = char
            code.append(char)
            index += 1
            continue
        match = re.match(r"\[(=*)\[", raw[index:])
        if match is not None:
            bracket_end = "]" + match.group(1) + "]"
            end = raw.find(bracket_end, index + match.end())
            if end < 0:
                return "".join(code), "", 0, "A" + bracket_end
            index = end + len(bracket_end)
            continue
        if char == "#":
            match = re.match(r"#\[(=*)\[", raw[index:])
            if match is not None:
                bracket_end = "]" + match.group(1) + "]"
                end = raw.find(bracket_end, index + match.end())
                if end < 0:
                    return "".join(code), "", 0, "C" + bracket_end
                index = end + len(bracket_end)
                continue
            return "".join(code), raw[index + 1 :], index + 1, bracket_state
        code.append(char)
        index += 1
    return "".join(code), "", 0, bracket_state


def _cmake_lines(text: str) -> tuple[list[HashLexLine], list[Finding]]:
    """Split CMake comments while excluding balanced bracket regions."""
    lines: list[HashLexLine] = []
    bracket_end = ""
    for line_no, raw in enumerate(text.splitlines(), start=1):
        code, comment, column, bracket_end = _cmake_code_line(raw, bracket_end)
        lines.append(HashLexLine(line_no, code, comment, column))
    findings = []
    if bracket_end:
        kind = "comment" if bracket_end.startswith("C") else "argument"
        findings.append(
            Finding(
                "unterminated-cmake-bracket",
                f"unterminated CMake bracket {kind}; expected {bracket_end[1:]}",
            )
        )
    return lines, findings


def hash_lines(path: str, text: str) -> tuple[list[HashLexLine], list[Finding]]:
    """Return syntax-aware code/comment lines for one hash-comment language."""
    item = Path(path)
    first_line = text.partition("\n")[0]
    if is_shell_control(path, first_line):
        return _shell_lines(text)
    if item.suffix.lower() in {".yaml", ".yml"}:
        return _yaml_lines(text), []
    if item.name == "CMakeLists.txt" or item.suffix.lower() == ".cmake":
        return _cmake_lines(text)
    return _basic_lines(text, shell_words=False), []
