# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Parse MC/DC deactivations and native C/C++ test-skip controls."""

from __future__ import annotations

import re
from dataclasses import dataclass

from suppression_catalog import ownership
from suppression_comment_lex import Comment
from suppression_model import Finding, Suppression

MCDC_COMMENT_RE = re.compile(r"mcdc-deactivated\s*:\s*(?P<reason>.+)", re.IGNORECASE)
MCDC_COMMENT_HINT_RE = re.compile(r"^mcdc-deactivated\b", re.IGNORECASE)
MCDC_MACRO_RE = re.compile(r"\bRA8_MCDC_(?:DEACTIVATED|EXEMPT|OK)\s*\(")
MCDC_DEACTIVATED_INVOCATION_RE = re.compile(
    r'RA8_MCDC_DEACTIVATED\s*\(\s*(?P<strings>"(?:\\.|[^"\\])*"'
    r'(?:\s*"(?:\\.|[^"\\])*")*)\s*\)\Z'
)
COMPOUND_RE = re.compile(r"&&|\|\|")
GTEST_RE = re.compile(r"\bGTEST_SKIP\s*\(")
UNITY_RE = re.compile(r"\bTEST_IGNORE(?:_MESSAGE)?\s*\(")
C_CONTROL_HINT_RE = re.compile(
    r"mcdc-deactivated|RA8_MCDC_(?:DEACTIVATED|EXEMPT|OK)|GTEST_SKIP|TEST_IGNORE",
    re.IGNORECASE,
)
PAIRING_WINDOW = 8
RAW_STRING_RE = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t]{0,16})\(')


@dataclass
class MaskState:
    """Cross-line state for blanking C/C++ comments and literals."""

    quote: str = ""
    block_comment: bool = False
    line_comment: bool = False
    raw_end: str = ""


def _blank(chars: list[str], start: int, stop: int) -> None:
    """Blank a source range without changing newline coordinates."""
    for offset in range(start, stop):
        if chars[offset] != "\n":
            chars[offset] = " "


def _consume_masked(text: str, chars: list[str], index: int, state: MaskState) -> int | None:
    """Consume one token while already inside a non-code lexical state."""
    char = chars[index]
    if state.line_comment:
        state.line_comment = char != "\n"
        if char != "\n":
            chars[index] = " "
        return index + 1
    if state.block_comment:
        if text.startswith("*/", index):
            _blank(chars, index, index + 2)
            state.block_comment = False
            stop = index + 2
        else:
            _blank(chars, index, index + 1)
            stop = index + 1
        return stop
    if state.raw_end:
        end = text.find(state.raw_end, index)
        stop = len(chars) if end < 0 else end + len(state.raw_end)
        _blank(chars, index, stop)
        state.raw_end = "" if end >= 0 else state.raw_end
        return stop
    if not state.quote:
        return None
    if char == "\\" and index + 1 < len(chars):
        _blank(chars, index, index + 2)
        return index + 2
    if char == state.quote:
        state.quote = ""
    _blank(chars, index, index + 1)
    return index + 1


def _open_masked(text: str, chars: list[str], index: int, state: MaskState) -> int | None:
    """Open a comment or literal state at one active-code offset."""
    raw_match = RAW_STRING_RE.match(text, index)
    if raw_match is not None:
        state.raw_end = ")" + raw_match.group(1) + '"'
        stop = raw_match.end()
        _blank(chars, index, stop)
        return stop
    if text.startswith("//", index):
        _blank(chars, index, index + 2)
        state.line_comment = True
        return index + 2
    if text.startswith("/*", index):
        _blank(chars, index, index + 2)
        state.block_comment = True
        return index + 2
    if chars[index] in {'"', "'"}:
        state.quote = chars[index]
        chars[index] = " "
        return index + 1
    return None


def _mask_noncode(text: str) -> str:
    """Blank comments and literals while preserving code coordinates."""
    chars = list(text)
    state = MaskState()
    index = 0
    while index < len(chars):
        next_index = _consume_masked(text, chars, index, state)
        if next_index is None:
            next_index = _open_masked(text, chars, index, state)
        index = index + 1 if next_index is None else next_index
    return "".join(chars)


def _target_decision(code_lines: list[str], marker_line: int) -> int | None:
    """Pair one marker with its same-line or immediately-following decision."""
    same_line = code_lines[marker_line - 1]
    if COMPOUND_RE.search(same_line):
        return marker_line
    statement = ""
    for line_no in range(marker_line + 1, min(len(code_lines), marker_line + PAIRING_WINDOW) + 1):
        code = code_lines[line_no - 1].strip()
        if not code:
            continue
        statement += " " + code
        if COMPOUND_RE.search(statement):
            return line_no
        if ";" in code or "{" in code or "}" in code:
            break
    return None


def _mcdc_record(path: str, comment: Comment, reason: str, target_line: int) -> Suppression:
    """Build one decision-scoped MC/DC deactivation row."""
    return Suppression(
        path,
        comment.line,
        comment.column,
        "mcdc-deactivation",
        "llvm-cov",
        "deactivated-condition",
        "mcdc-deactivated",
        f"decision-line:{target_line}",
        reason,
        "inline-comment",
        ownership(path),
        (),
        evidence=(f"decision-line:{target_line}", "standard:DO-178C-6.4.4.3"),
        recommendation="revalidate-invariant",
    )


def _scan_mcdc_comments(
    path: str, code_lines: list[str], comments: list[Comment]
) -> tuple[list[Suppression], list[Finding]]:
    """Parse exact legacy marker grammar and enforce one-to-one decision pairing."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    targets: dict[int, int] = {}
    for comment in comments:
        if comment.text.lstrip().startswith("*"):
            continue
        body = comment.text.strip().lstrip("*").strip()
        match = MCDC_COMMENT_RE.fullmatch(body)
        if match is None:
            if MCDC_COMMENT_HINT_RE.match(body):
                findings.append(Finding("malformed-mcdc-deactivation", body, path, comment.line))
            continue
        reason = match.group("reason").strip()
        if not reason:
            findings.append(Finding("blank-mcdc-reason", body, path, comment.line))
            continue
        target_line = _target_decision(code_lines, comment.line)
        if target_line is None:
            findings.append(
                Finding(
                    "unpaired-mcdc-deactivation",
                    "marker does not immediately govern a compound decision",
                    path,
                    comment.line,
                )
            )
            continue
        if target_line in targets:
            message = f"decision line {target_line} already governed by line {targets[target_line]}"
            findings.append(Finding("duplicate-mcdc-deactivation", message, path, comment.line))
            continue
        targets[target_line] = comment.line
        record = _mcdc_record(path, comment, reason, target_line)
        records.append(record)
        if record.owner != "first-party":
            findings.append(
                Finding(
                    "mcdc-owner-mismatch",
                    f"deactivation appears in {record.owner} code",
                    path,
                    comment.line,
                )
            )
    return records, findings


def _balanced_invocation_end(code: str, open_index: int) -> int | None:
    """Return the exclusive end of one balanced call in masked C/C++ code."""
    if open_index >= len(code) or code[open_index] != "(":
        return None
    depth = 0
    for index in range(open_index, len(code)):
        if code[index] == "(":
            depth += 1
        elif code[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
    return None


def _macro_reason(raw: str, code: str, macro: re.Match[str]) -> str | None:
    """Return the literal reason bound to one exact balanced annotation call."""
    end = _balanced_invocation_end(code, macro.end() - 1)
    if end is None:
        return None
    invocation = raw[macro.start() : end]
    invocation_match = MCDC_DEACTIVATED_INVOCATION_RE.fullmatch(invocation)
    if invocation_match is None:
        return None
    parts = re.findall(r'"((?:\\.|[^"\\])*)"', invocation_match.group("strings"))
    return " ".join(part.replace(r"\"", '"').replace(r"\\", "\\") for part in parts).strip()


def _scan_mcdc_macros(
    path: str, text_lines: list[str], code_lines: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory active annotation macros and reject aliases or nonliteral reasons."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for line_no, code in enumerate(code_lines, start=1):
        if code.lstrip().startswith("#define"):
            continue
        for match in MCDC_MACRO_RE.finditer(code):
            macro = re.match(r"RA8_MCDC_[A-Z]+", match.group(0))
            name = macro.group(0) if macro is not None else ""
            if name != "RA8_MCDC_DEACTIVATED":
                findings.append(Finding("unknown-mcdc-macro", name, path, line_no))
                continue
            reason = _macro_reason(text_lines[line_no - 1], code, match)
            if reason is None:
                findings.append(
                    Finding(
                        "malformed-mcdc-macro",
                        "reason must be one or more string literals on the annotation line",
                        path,
                        line_no,
                    )
                )
                continue
            concerns = ("blank-reason",) if not reason else ("broad-function-scope",)
            records.append(
                Suppression(
                    path,
                    line_no,
                    match.start() + 1,
                    "mcdc-deactivation",
                    "llvm-cov",
                    "deactivated-function",
                    name,
                    "function",
                    reason,
                    "c-annotation",
                    ownership(path),
                    concerns,
                    recommendation="replace-with-decision-scoped-marker",
                )
            )
    return records, findings


@dataclass(frozen=True)
class NativeSkip:
    """Normalized native-test skip macro fields."""

    column: int
    tool: str
    directive: str
    reason: str


def _skip_record(path: str, line: int, skip: NativeSkip) -> Suppression:
    """Build one native test-control inventory row."""
    return Suppression(
        path,
        line,
        skip.column,
        "test-control",
        skip.tool,
        "skip",
        skip.directive,
        "test-case",
        skip.reason,
        "c-macro",
        ownership(path),
        () if skip.reason else ("blank-reason",),
    )


def _scan_native_skips(
    path: str, text_lines: list[str], code_lines: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Parse GTest and Unity skip macros without matching strings or comments."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for line_no, code in enumerate(code_lines, start=1):
        raw = text_lines[line_no - 1]
        for match in GTEST_RE.finditer(code):
            valid = re.search(r'GTEST_SKIP\s*\(\s*\)\s*(?:<<\s*"(?P<reason>[^"]+)")?\s*;', raw)
            if valid is None:
                findings.append(Finding("malformed-native-test-skip", "GTEST_SKIP", path, line_no))
                continue
            records.append(
                _skip_record(
                    path,
                    line_no,
                    NativeSkip(
                        match.start() + 1,
                        "google-test",
                        "GTEST_SKIP",
                        (valid.group("reason") or "").strip(),
                    ),
                )
            )
        for match in UNITY_RE.finditer(code):
            name_match = re.match(r"TEST_IGNORE(?:_MESSAGE)?", match.group(0))
            name = name_match.group(0) if name_match is not None else ""
            if name == "TEST_IGNORE":
                valid = re.search(r"TEST_IGNORE\s*\(\s*\)\s*;", raw)
                reason = ""
            else:
                valid = re.search(r'TEST_IGNORE_MESSAGE\s*\(\s*"(?P<reason>[^"]+)"\s*\)\s*;', raw)
                reason = "" if valid is None else valid.group("reason").strip()
            if valid is None:
                findings.append(Finding("malformed-native-test-skip", name, path, line_no))
                continue
            records.append(
                _skip_record(
                    path,
                    line_no,
                    NativeSkip(
                        match.start() + 1,
                        "unity",
                        name,
                        reason,
                    ),
                )
            )
    return records, findings


def scan_c_controls(
    path: str, text: str, comments: list[Comment]
) -> tuple[list[Suppression], list[Finding]]:
    """Parse active MC/DC and native-test controls from one C-family file."""
    text_lines = text.splitlines()
    code_lines = _mask_noncode(text).splitlines()
    records, findings = _scan_mcdc_comments(path, code_lines, comments)
    macro_records, macro_findings = _scan_mcdc_macros(path, text_lines, code_lines)
    skip_records, skip_findings = _scan_native_skips(path, text_lines, code_lines)
    records.extend(macro_records)
    records.extend(skip_records)
    findings.extend(macro_findings)
    findings.extend(skip_findings)
    return records, findings
