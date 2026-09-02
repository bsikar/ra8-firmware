# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware comment extraction for the suppression inventory."""

from __future__ import annotations

import io
import re
import tokenize
from dataclasses import dataclass
from pathlib import Path

from suppression_catalog import language
from suppression_hash_lex import hash_lines
from suppression_model import Finding


@dataclass(frozen=True)
class Comment:
    """One lexical comment with its source location."""

    line: int
    column: int
    text: str


@dataclass
class CLexState:
    """Cross-line lexical state for C comments, strings, and raw strings."""

    in_block_comment: bool = False
    in_line_comment: bool = False
    quote: str = ""
    raw_end: str = ""


def _python_comments(text: str) -> tuple[list[Comment], list[Finding]]:
    """Extract real Python comments while ignoring strings and docstrings."""
    comments: list[Comment] = []
    try:
        tokens = tokenize.generate_tokens(io.StringIO(text).readline)
        comments.extend(
            Comment(token.start[0], token.start[1] + 1, token.string[1:])
            for token in tokens
            if token.type == tokenize.COMMENT
        )
    except (IndentationError, tokenize.TokenError) as exc:
        return comments, [Finding("python-tokenize", str(exc))]
    return comments, []


def _raw_string_end(raw: str, index: int) -> tuple[str, int] | None:
    """Return a C++ raw-string terminator and body start at one source offset."""
    if index > 0 and (raw[index - 1].isalnum() or raw[index - 1] == "_"):
        return None
    match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t]{0,16})\(', raw[index:])
    if match is None:
        return None
    return ")" + match.group(1) + '"', index + match.end()


def _continue_block_comment(
    raw: str, line_no: int, index: int, state: CLexState, comments: list[Comment]
) -> int:
    """Consume one line fragment of an active C block comment."""
    end = raw.find("*/", index)
    stop = len(raw) if end < 0 else end
    comments.append(Comment(line_no, index + 1, raw[index:stop]))
    if end < 0:
        return len(raw)
    state.in_block_comment = False
    return end + 2


def _continue_raw_string(raw: str, index: int, state: CLexState) -> int:
    """Consume one line fragment of an active C++ raw string."""
    end = raw.find(state.raw_end, index)
    if end < 0:
        return len(raw)
    index = end + len(state.raw_end)
    state.raw_end = ""
    return index


def _continue_quoted_string(raw: str, index: int, state: CLexState) -> int:
    """Consume one token from an active ordinary C string or character literal."""
    char = raw[index]
    if char == "\\":
        return index + 2
    if char == state.quote:
        state.quote = ""
    return index + 1


def _continue_c_state(
    raw: str,
    line_no: int,
    index: int,
    state: CLexState,
    comments: list[Comment],
) -> tuple[int, bool]:
    """Consume an active block comment, raw string, or ordinary string state."""
    if state.in_block_comment:
        return _continue_block_comment(raw, line_no, index, state, comments), True
    if state.raw_end:
        return _continue_raw_string(raw, index, state), True
    if state.quote:
        return _continue_quoted_string(raw, index, state), True
    return index, False


def _scan_c_line(raw: str, line_no: int, state: CLexState) -> list[Comment]:
    """Extract C/C++ comments from one line and carry block-comment state."""
    comments: list[Comment] = []
    if state.in_line_comment:
        comments.append(Comment(line_no, 1, raw))
        state.in_line_comment = raw.endswith("\\")
        return comments
    index = 0
    while index < len(raw):
        index, handled = _continue_c_state(raw, line_no, index, state, comments)
        if handled:
            continue
        char = raw[index]
        raw_string = _raw_string_end(raw, index)
        if raw_string is not None:
            state.raw_end, index = raw_string
        elif char in {'"', "'"}:
            state.quote = char
            index += 1
        elif raw.startswith("//", index):
            comments.append(Comment(line_no, index + 1, raw[index + 2 :]))
            state.in_line_comment = raw.endswith("\\")
            return comments
        elif raw.startswith("/*", index):
            state.in_block_comment = True
            index += 2
        else:
            index += 1
    return comments


def _c_comments(text: str) -> tuple[list[Comment], list[Finding]]:
    """Extract lexical C/C++ comments without matching string literals."""
    comments: list[Comment] = []
    state = CLexState()
    for line_no, raw in enumerate(text.splitlines(), start=1):
        comments.extend(_scan_c_line(raw, line_no, state))
        if state.quote and not raw.endswith("\\"):
            state.quote = ""
    findings: list[Finding] = []
    if state.in_block_comment:
        findings.append(Finding("unterminated-comment", "unterminated C block comment"))
    if state.in_line_comment:
        findings.append(Finding("unterminated-line-comment-splice", "line splice reaches EOF"))
    if state.raw_end:
        findings.append(Finding("unterminated-string", "unterminated C++ raw string"))
    if state.quote:
        findings.append(Finding("unterminated-string", "unterminated backslash-spliced string"))
    return comments, findings


def _hash_comments(path: str, text: str) -> tuple[list[Comment], list[Finding]]:
    """Extract comments from syntax-aware shell, CMake, YAML, and config lines."""
    lines, findings = hash_lines(path, text)
    comments = [
        Comment(line.line, line.comment_column, line.comment) for line in lines if line.comment
    ]
    return comments, findings


def _html_comments(text: str) -> tuple[list[Comment], list[Finding]]:
    """Extract single- and multi-line HTML comments used by policy markers."""
    comments: list[Comment] = []
    buffer: list[str] = []
    start_line = 0
    start_column = 0
    for line_no, raw in enumerate(text.splitlines(), start=1):
        index = 0
        while index < len(raw):
            if buffer:
                end = raw.find("-->", index)
                if end < 0:
                    buffer.append(raw[index:])
                    break
                buffer.append(raw[index:end])
                comments.append(Comment(start_line, start_column, " ".join(buffer)))
                buffer = []
                index = end + 3
                continue
            start = raw.find("<!--", index)
            if start < 0:
                break
            end = raw.find("-->", start + 4)
            if end >= 0:
                comments.append(Comment(line_no, start + 1, raw[start + 4 : end]))
                index = end + 3
            else:
                start_line = line_no
                start_column = start + 1
                buffer = [raw[start + 4 :]]
                break
    findings = []
    if buffer:
        message = f"unterminated HTML comment starting at line {start_line}"
        findings.append(Finding("unterminated-html-comment", message, line=start_line))
    return comments, findings


def _mask_markdown_code(text: str) -> str:
    """Blank fenced and inline code while preserving source coordinates."""
    masked: list[str] = []
    fence = ""
    inline_ticks = 0
    for raw in text.splitlines(keepends=True):
        content = raw.rstrip("\r\n")
        newline = raw[len(content) :]
        fence_match = re.match(r"^ {0,3}(`{3,}|~{3,})", content)
        if fence:
            closes = re.match(rf"^ {{0,3}}{re.escape(fence[0])}{{{len(fence)},}}\s*$", content)
            if closes:
                fence = ""
            masked.append(" " * len(content) + newline)
            continue
        if not inline_ticks and fence_match:
            fence = fence_match.group(1)
            masked.append(" " * len(content) + newline)
            continue
        chars = list(content)
        index = 0
        while index < len(content):
            if inline_ticks:
                closing = "`" * inline_ticks
                end = content.find(closing, index)
                limit = len(content) if end < 0 else end + inline_ticks
                chars[index:limit] = " " * (limit - index)
                index = limit
                if end >= 0:
                    inline_ticks = 0
                continue
            if content[index] != "`":
                index += 1
                continue
            end = index
            while end < len(content) and content[end] == "`":
                end += 1
            inline_ticks = end - index
            chars[index:end] = " " * inline_ticks
            index = end
        masked.append("".join(chars) + newline)
    return "".join(masked)


def extract_comments(path: str, text: str) -> tuple[list[Comment], list[Finding]]:
    """Dispatch to the lexical comment extractor appropriate for one file."""
    first_line = text.partition("\n")[0]
    kind = language(path, first_line)
    if kind == "python":
        comments, findings = _python_comments(text)
    elif kind == "c-family":
        comments, findings = _c_comments(text)
    elif kind == "hash":
        comments, findings = _hash_comments(path, text)
    else:
        comments, findings = [], []
    if kind == "text":
        html_source = _mask_markdown_code(text) if Path(path).suffix == ".md" else text
        html_comments, html_findings = _html_comments(html_source)
        comments.extend(html_comments)
        findings.extend(html_findings)
    return comments, findings
