# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware recognizers for inline suppression directives."""

from __future__ import annotations

import re
from dataclasses import dataclass, replace
from pathlib import Path

from suppression_catalog import (
    BANDIT_RE,
    COVERAGE_RE,
    CPPCHECK_RE,
    KNOWN_CPPCHECK_RULES,
    KNOWN_PROJECT_MARKERS,
    MYPY_RE,
    NOLINT_RAW_RE,
    NOLINT_RE,
    NOQA_RE,
    PROJECT_MARKER_RE,
    PYLINT_RE,
    PYRIGHT_RE,
    PYTHON_COVERAGE_RE,
    PYTHON_FORMATTER_RE,
    SHELLCHECK_RE,
    TYPE_IGNORE_RE,
    UNKNOWN_DIRECTIVE_RE,
    ownership,
)
from suppression_comment_lex import Comment
from suppression_model import Finding, Suppression
from suppression_tool_controls import (
    recognize_tool_comment,
    tool_control_finding,
    valid_tool_control,
)


@dataclass(frozen=True)
class Recognition:
    """Normalized fields produced by one directive recognizer."""

    family: str
    tool: str
    rule: str
    directive: str
    scope: str
    reason: str
    reason_required: bool = True


def _reason(tail: str) -> str:
    """Normalize the conventional separator before an inline rationale."""
    value = tail.strip()
    if value.startswith("--"):
        value = value[2:].strip()
    elif value.startswith((";", "#", ":")):
        value = value[1:].strip()
    elif value.startswith("- "):
        value = value[2:].strip()
    return value.strip()


def _concerns(rule: str, reason: str, *, reason_required: bool = True) -> tuple[str, ...]:
    """Return machine-observable review concerns for one recognized row."""
    concerns: list[str] = []
    if rule == "*":
        concerns.append("broad-rule")
    if reason_required and not reason:
        concerns.append("blank-reason")
    return tuple(concerns)


def _record(
    path: str,
    comment: Comment,
    recognition: Recognition,
) -> Suppression:
    """Build one normalized suppression row from a recognized comment."""
    return Suppression(
        path,
        comment.line,
        comment.column,
        recognition.family,
        recognition.tool,
        recognition.rule or "*",
        recognition.directive,
        recognition.scope,
        recognition.reason,
        "inline-comment",
        ownership(path),
        _concerns(
            recognition.rule or "*",
            recognition.reason,
            reason_required=recognition.reason_required,
        ),
    )


def _valid_rule_list(value: str | None) -> bool:
    """Return whether a comma-separated rule list has no empty or bogus IDs."""
    if value is None:
        return True
    rules = value.split(",")
    return bool(rules) and all(
        rule.strip() and re.fullmatch(r"[A-Za-z0-9_./-]+", rule.strip()) for rule in rules
    )


def _recognize_nolint(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize clang-tidy and cpplint NOLINT line and region directives."""
    match = NOLINT_RE.fullmatch(body)
    if match is None or not _valid_rule_list(match.group("rules")):
        return None
    suffix = match.group("scope") or ""
    tail = match.group("tail")
    if (
        not suffix
        and match.group("rules") is None
        and tail.strip()
        and not tail.lstrip().startswith("--")
    ):
        return None
    scope = {
        "": "line",
        "NEXTLINE": "next-line",
        "BEGIN": "region-start",
        "END": "region-end",
    }[suffix]
    reason = _reason(tail)
    rules = (match.group("rules") or "").strip()
    tool = "cpplint" if "/" in rules else "clang-tidy"
    return _record(
        path,
        comment,
        Recognition(
            "clang-tidy",
            tool,
            rules,
            "NOLINT" + suffix,
            scope,
            reason,
            reason_required=scope != "region-end",
        ),
    )


def _raw_nolint_reason(tail: str) -> str:
    """Return only an explicit raw-source rationale, not surrounding code."""
    value = tail.lstrip()
    if not value.startswith("--"):
        return ""
    reason = value[2:].strip()
    if reason.endswith("*/"):
        reason = reason[:-2].rstrip()
    return reason


def _scan_raw_nolint(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Match NOLINT using clang-tidy's raw-source, not comment-only, semantics."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for line_no, raw in enumerate(text.splitlines(), start=1):
        for match in NOLINT_RAW_RE.finditer(raw):
            rules = match.group("rules")
            bracket_rules = match.group("bracket_rules")
            selected = rules if bracket_rules is None else bracket_rules
            following = raw[match.end() :]
            malformed_group = following.startswith(("(", "["))
            if malformed_group or not _valid_rule_list(selected):
                findings.append(
                    Finding("unknown-directive", raw[match.start() :].strip(), path, line_no)
                )
                continue
            suffix = match.group("scope") or ""
            scope = {
                "": "line",
                "NEXTLINE": "next-line",
                "BEGIN": "region-start",
                "END": "region-end",
            }[suffix]
            normalized_rules = (selected or "").strip()
            bracketed = bracket_rules is not None
            tool = "cpplint" if bracketed or "/" in normalized_rules else "clang-tidy"
            record = _record(
                path,
                Comment(line_no, match.start() + 1, match.group(0)),
                Recognition(
                    "clang-tidy",
                    tool,
                    normalized_rules,
                    "NOLINT" + suffix,
                    scope,
                    _raw_nolint_reason(following),
                    reason_required=scope != "region-end",
                ),
            )
            records.extend(_split_rules(record))
            if bracketed:
                # cpplint owns the bracket rule, while clang-tidy recognizes
                # only the preceding bare NOLINT and therefore suppresses all
                # of its checks on the line. Preserve both real effects.
                records.append(
                    _record(
                        path,
                        Comment(line_no, match.start() + 1, match.group(0)),
                        Recognition(
                            "clang-tidy",
                            "clang-tidy",
                            "",
                            "NOLINT" + suffix,
                            scope,
                            _raw_nolint_reason(following),
                            reason_required=scope != "region-end",
                        ),
                    )
                )
    return records, findings


def _recognize_python(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Ruff/noqa and Python type-checker ignore directives."""
    match = NOQA_RE.fullmatch(body)
    if match is not None:
        reason = _reason(match.group("tail"))
        tool = "ansible-lint" if Path(path).suffix in {".yml", ".yaml"} else "ruff"
        return _record(
            path,
            comment,
            Recognition(
                "python",
                tool,
                (match.group("rules") or "").strip(),
                "noqa",
                "line",
                reason,
            ),
        )
    match = TYPE_IGNORE_RE.fullmatch(body)
    if match is None or not _valid_rule_list(match.group("rules")):
        return None
    reason = _reason(match.group("tail"))
    return _record(
        path,
        comment,
        Recognition(
            "python",
            "type-checker",
            (match.group("rules") or "").strip(),
            "type: ignore",
            "line",
            reason,
        ),
    )


def _recognize_python_coverage(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize coverage.py line/branch exclusions in Python comments."""
    match = PYTHON_COVERAGE_RE.fullmatch(body)
    if match is None:
        return None
    return _record(
        path,
        comment,
        Recognition(
            "coverage",
            "coverage.py",
            "no-cover",
            "pragma: no cover",
            "branch",
            _reason(match.group("tail")),
        ),
    )


def _recognize_pylint(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Pylint file and region controls."""
    match = PYLINT_RE.fullmatch(body)
    if match is None:
        return None
    control = match.group("control").lower()
    rules = (match.group("rules") or "").strip()
    if control != "skip-file" and not rules:
        return None
    scopes = {"disable": "following-code", "enable": "following-code", "skip-file": "file"}
    scope = scopes[control]
    return _record(
        path,
        comment,
        Recognition(
            "python",
            "pylint",
            rules,
            f"pylint: {control}",
            scope,
            _reason(match.group("tail")),
            reason_required=control != "enable",
        ),
    )


def _recognize_mypy(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Mypy file-level controls."""
    match = MYPY_RE.fullmatch(body)
    if match is None:
        return None
    control = match.group("control").lower()
    rules = (match.group("rules") or "").strip()
    if control != "ignore-errors" and not rules:
        return None
    return _record(
        path,
        comment,
        Recognition(
            "python",
            "mypy",
            rules,
            f"mypy: {control}",
            "file",
            _reason(match.group("tail")),
        ),
    )


def _recognize_pyright(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Pyright line and file controls."""
    match = PYRIGHT_RE.fullmatch(body)
    if match is None:
        return None
    setting = match.group("setting")
    rules = (match.group("rules") or setting or "").strip()
    directive = "pyright: ignore" if match.group("ignore") else f"pyright: {setting}"
    scope = "line" if match.group("ignore") else "file"
    return _record(
        path,
        comment,
        Recognition(
            "python",
            "pyright",
            rules,
            directive,
            scope,
            _reason(match.group("tail")),
        ),
    )


def _recognize_bandit(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Bandit line controls in both supported spellings."""
    match = BANDIT_RE.fullmatch(body)
    if match is None:
        return None
    rules = (match.group("nosec_rules") or match.group("bandit_rules") or "").strip()
    directive = "nosec" if match.group("nosec") else "bandit: skip"
    return _record(
        path,
        comment,
        Recognition(
            "python",
            "bandit",
            rules,
            directive,
            "line",
            _reason(match.group("tail")),
        ),
    )


def _recognize_python_formatter(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize Ruff, Black-compatible, and isort formatter controls."""
    match = PYTHON_FORMATTER_RE.fullmatch(body)
    if match is None:
        return None
    reason = _reason(match.group("tail"))
    if match.group("ruff"):
        return _record(
            path,
            comment,
            Recognition(
                "python",
                "ruff",
                (match.group("ruff_rules") or "").strip(),
                "ruff: noqa",
                "file",
                reason,
            ),
        )
    control = (match.group("fmt_control") or match.group("isort_control")).lower()
    is_isort = match.group("isort") is not None
    tool = "isort" if is_isort else "ruff-format"
    rule = "imports" if is_isort else "format"
    scope = (
        "region-start"
        if control == "off"
        else "region-end"
        if control == "on"
        else "file"
        if control == "skip_file"
        else "line"
    )
    return _record(
        path,
        comment,
        Recognition(
            "python",
            tool,
            rule,
            f"{match.group('isort') or match.group('fmt')}: {control}",
            scope,
            reason,
            reason_required=scope != "region-end",
        ),
    )


def _recognize_shellcheck(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize ShellCheck waiver and analysis-context controls."""
    match = SHELLCHECK_RE.fullmatch(body)
    if match is None:
        return None
    control = match.group("control").lower()
    return _record(
        path,
        comment,
        Recognition(
            "shellcheck",
            "shellcheck",
            match.group("value").strip(),
            "shellcheck " + control,
            "line",
            _reason(match.group("tail")),
            reason_required=control == "disable",
        ),
    )


def _recognize_coverage(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize GCOVR and LCOV line, branch, and region exclusions."""
    match = COVERAGE_RE.fullmatch(body)
    if match is None:
        return None
    marker = match.group("marker")
    scope = (
        "region-start"
        if marker.endswith("START")
        else "region-end"
        if marker.endswith("STOP")
        else "branch"
        if "_BR_" in marker
        else "line"
    )
    return _record(
        path,
        comment,
        Recognition(
            "coverage",
            marker.split("_", 1)[0].lower(),
            marker,
            marker,
            scope,
            _reason(match.group("tail")),
            reason_required=scope != "region-end",
        ),
    )


def _recognize_cppcheck(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize cppcheck inline line and region suppressions."""
    match = CPPCHECK_RE.fullmatch(body)
    if match is None or match.group("rule") not in KNOWN_CPPCHECK_RULES:
        return None
    suffix = match.group("scope") or ""
    scope = {
        "": "line",
        "-file": "file",
        "-begin": "region-start",
        "-end": "region-end",
    }[suffix]
    return _record(
        path,
        comment,
        Recognition(
            "cppcheck",
            "cppcheck",
            match.group("rule"),
            "cppcheck-suppress" + suffix,
            scope,
            _reason(match.group("tail")),
            reason_required=scope != "region-end",
        ),
    )


def _recognize_project(path: str, comment: Comment, body: str) -> Suppression | None:
    """Recognize one cataloged project policy waiver marker."""
    match = PROJECT_MARKER_RE.fullmatch(body)
    if match is None or match.group("marker") not in KNOWN_PROJECT_MARKERS:
        return None
    marker = match.group("marker")
    return _record(
        path,
        comment,
        Recognition(
            "project-policy",
            "repository-policy",
            marker,
            marker,
            "line",
            (match.group("reason") or "").strip(),
        ),
    )


RECOGNIZERS = (
    _recognize_nolint,
    _recognize_python,
    _recognize_python_coverage,
    _recognize_pylint,
    _recognize_mypy,
    _recognize_pyright,
    _recognize_bandit,
    _recognize_python_formatter,
    _recognize_shellcheck,
    _recognize_coverage,
    _recognize_cppcheck,
    _recognize_project,
)


def _scan_comments(
    path: str,
    comments: list[Comment],
    active_tools: frozenset[str],
    *,
    include_nolint: bool = True,
) -> tuple[list[Suppression], list[Finding]]:
    """Recognize directives and fail closed on directive-like unknown syntax."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    recognizers = RECOGNIZERS if include_nolint else RECOGNIZERS[1:]
    pending_reason, pending_line = "", 0
    for comment in comments:
        body = comment.text.strip().lstrip("*").strip()
        if body.startswith("Suppression rationale:"):
            pending_reason = body.removeprefix("Suppression rationale:").strip()
            pending_line = comment.line
            continue
        valid_control = valid_tool_control(path, body, active_tools)
        markdown_doxygen = Path(path).suffix.lower() == ".md" and re.match(
            r"^(?:@|\\)(?:cond|endcond)(?:\s|$)", body, re.IGNORECASE
        )
        record = next((item for fn in recognizers if (item := fn(path, comment, body))), None)
        record = record or recognize_tool_comment(path, comment, body, active_tools)
        if record is not None:
            if not record.reason and pending_reason and pending_line == comment.line - 1:
                record = replace(
                    record,
                    reason=pending_reason,
                    concerns=_concerns(record.rule, pending_reason),
                    fingerprint="",
                )
            records.extend(_split_rules(record))
        elif finding := tool_control_finding(path, comment, body, active_tools):
            findings.append(finding)
        elif (
            UNKNOWN_DIRECTIVE_RE.match(body)
            and (include_nolint or not body.startswith("NOLINT"))
            and not valid_control
            and not markdown_doxygen
        ):
            findings.append(Finding("unknown-directive", body, path, comment.line))
        pending_reason, pending_line = "", 0
    return records, findings


# A branch marker only excludes the branch on its own physical line, and gcov
# attributes a decision to the line where the controlling expression starts.
# These recognise a line that cannot be a statement head.
_BR_LINE_MARKER = re.compile(r"/\*\s*GCOVR_EXCL_BR_LINE\b")
_LINE_MARKER = re.compile(r"(?:/\*|//)\s*GCOVR_EXCL_LINE\b")
_STATEMENT_END = re.compile(r"[;{}]\s*$")
_LABEL_OR_DIRECTIVE = re.compile(r"^\s*(?:#|case\b|default\b)")


def _code_before_comment(line: str) -> str:
    """The line's code with any trailing block comment removed."""
    return line.split("/*", maxsplit=1)[0].rstrip()


def _closes_unopened_bracket(code: str) -> bool:
    """True when the line closes a parenthesis it never opened.

    A line that pops a bracket depth it never pushed is, by construction, the
    continuation of a statement that began further up, whatever punctuation it
    happens to END with. That distinction is exactly what ``_STATEMENT_END``
    alone cannot make, and the gap was not theoretical: a wrapped

    .. code-block:: c

        for (uint32_t i = 0U; i < limit;
             i++) { /* GCOVR_EXCL_BR_LINE -- hardware only */

    has a previous line ending in ``;``, so the backward walk read it as a
    finished statement and stayed quiet -- while gcov attributes the loop
    condition to the ``for`` line, leaving the marker excluding nothing. That
    single shape accounted for most of the exclusions this detector exists to
    catch (#790).

    Args:
        code: One physical line with any trailing comment already removed.

    Returns:
        True when bracket depth goes negative anywhere in ``code``.
    """
    depth = 0
    lowest = 0
    quote = ""
    index = 0
    while index < len(code):
        char = code[index]
        if quote:
            if char == "\\":
                index += 2
                continue
            if char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            lowest = min(lowest, depth)
        index += 1
    return lowest < 0


def stranded_branch_findings(path: str, text: str, comment_lines: frozenset[int]) -> list[Finding]:
    """Report every branch marker sitting on a wrapped statement's continuation.

    Args:
        path: Repository-relative path, used only to locate the finding.
        text: The file's full source text.
        comment_lines: One-based line numbers whose content is comment
            interior, so the backward walk does not mistake a multi-line
            ``/* ... */`` block for an unfinished statement.

    Returns:
        One finding per stranded marker; empty when every marker sits on the
        line that carries its branch.
    """
    findings: list[Finding] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not _BR_LINE_MARKER.search(line):
            continue
        own = _code_before_comment(line)
        if not own:
            continue
        if _closes_unopened_bracket(own):
            findings.append(
                Finding(
                    "stranded-branch-marker",
                    "GCOVR_EXCL_BR_LINE sits on a continuation line, so it excludes no "
                    "branch; use a GCOVR_EXCL_BR_START/STOP region instead",
                    path,
                    index + 1,
                )
            )
            continue
        previous = ""
        for back in range(index - 1, -1, -1):
            if (back + 1) in comment_lines:
                continue
            candidate = _code_before_comment(lines[back])
            if candidate.strip():
                previous = candidate
                break
        if not previous or _STATEMENT_END.search(previous):
            continue
        if _LABEL_OR_DIRECTIVE.match(previous):
            continue
        findings.append(
            Finding(
                "stranded-branch-marker",
                "GCOVR_EXCL_BR_LINE sits on a continuation line, so it excludes no "
                "branch; use a GCOVR_EXCL_BR_START/STOP region instead",
                path,
                index + 1,
            )
        )
    findings.extend(_stranded_line_findings(path, lines))
    return findings


def _stranded_line_findings(path: str, lines: list[str]) -> list[Finding]:
    """Report line-exclusion markers sitting on a line that holds no code.

    Args:
        path: Repository-relative path, used only to locate the finding.
        lines: The file's physical lines.

    Returns:
        One finding per marker on a comment-only line.
    """
    findings: list[Finding] = []
    for index, line in enumerate(lines):
        if not _LINE_MARKER.search(line):
            continue
        if _code_before_comment(line).strip():
            continue
        findings.append(
            Finding(
                "stranded-line-marker",
                "GCOVR_EXCL_LINE sits on a comment-only line, so it excludes a line "
                "gcov never counted; put it on the statement, or use a "
                "GCOVR_EXCL_START/STOP region",
                path,
                index + 1,
            )
        )
    return findings


def _split_rules(item: Suppression) -> list[Suppression]:
    """Split comma-separated rules into one stable inventory row per rule."""
    if "," not in item.rule:
        return [item]
    rules = [rule.strip() for rule in item.rule.split(",") if rule.strip()]
    return [replace(item, rule=rule, fingerprint="") for rule in rules]
