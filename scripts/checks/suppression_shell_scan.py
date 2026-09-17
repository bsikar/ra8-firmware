# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware inventory for shell status and global ShellCheck controls."""

from __future__ import annotations

import re
from dataclasses import replace
from pathlib import Path

from suppression_catalog import (
    SHELL_STATUS_MASK_RE,
    SHELLCHECK_GLOBAL_EXCLUDE_RE,
    is_shell_control,
    ownership,
)
from suppression_hash_lex import HashLexLine, hash_lines
from suppression_model import Finding, Suppression
from suppression_shell_lex import ShellOperatorState, mask_shell_operators

SHELLCHECK_OPTS_ASSIGN_RE = re.compile(
    r"^\s*(?:(?:export|readonly)\s+)?SHELLCHECK_OPTS\s*\+?=\s*(?P<value>.*)$"
)
MIN_QUOTED_VALUE_LENGTH = 2


def _shell_status_line_records(path: str, line: HashLexLine, code: str) -> list[Suppression]:
    """Return active local status masks from one syntax-masked shell line."""
    return [
        Suppression(
            path,
            line.line,
            match.start() + 1,
            "shell-status",
            "shell",
            "ignored-status",
            match.group(0),
            "command-list",
            line.comment.strip(),
            "active-shell-syntax",
            ownership(path),
        )
        for match in SHELL_STATUS_MASK_RE.finditer(code)
    ]


def _global_concerns(rule: str, reason: str) -> tuple[str, ...]:
    """Return concerns for one repository-wide ShellCheck exclusion."""
    concerns: list[str] = []
    if rule == "*":
        concerns.append("broad-rule")
    if not reason:
        concerns.append("blank-reason")
    return tuple(concerns)


def _shellcheck_options_records(path: str, line: HashLexLine) -> list[Suppression]:
    """Return active repository-wide exclusions passed through SHELLCHECK_OPTS."""
    assignment = SHELLCHECK_OPTS_ASSIGN_RE.match(line.code)
    if assignment is None:
        return []
    raw_value = assignment.group("value")
    leading = len(raw_value) - len(raw_value.lstrip())
    value = raw_value.strip()
    value_column = assignment.start("value") + leading + 1
    if len(value) >= MIN_QUOTED_VALUE_LENGTH and value[0] == value[-1] and value[0] in {'"', "'"}:
        value = value[1:-1]
        value_column += 1
    records: list[Suppression] = []
    reason = line.comment.strip()
    for match in SHELLCHECK_GLOBAL_EXCLUDE_RE.finditer(value):
        rules = (item.strip() for item in match.group("rules").split(","))
        records.extend(
            Suppression(
                path,
                line.line,
                value_column + match.start(),
                "shellcheck",
                "shellcheck",
                rule,
                "SHELLCHECK_OPTS exclude",
                "repository",
                reason,
                "active-shell-syntax",
                ownership(path),
                _global_concerns(rule, reason),
            )
            for rule in rules
        )
    return records


def _shellcheckrc_records(path: str, lines: list[HashLexLine]) -> list[Suppression]:
    """Return source-located exclusions from the central ShellCheck config."""
    records: list[Suppression] = []
    for line in lines:
        stripped = line.code.strip()
        if not stripped.lower().startswith("exclude="):
            continue
        reason = line.comment.strip()
        records.extend(
            Suppression(
                path,
                line.line,
                1,
                "shellcheck",
                "shellcheck",
                rule.strip(),
                ".shellcheckrc exclude",
                "repository",
                reason,
                "central-config",
                ownership(path),
                _global_concerns(rule.strip(), reason),
            )
            for rule in stripped.split("=", 1)[1].split(",")
        )
    return records


def _embedded_shell_status_records(
    path: str,
    text: str,
    lines: list[HashLexLine],
    active: list[Suppression],
) -> list[Suppression]:
    """Inventory status masks inside shell strings or heredoc payloads."""
    active_columns: dict[int, set[int]] = {}
    for record in active:
        active_columns.setdefault(record.line, set()).add(record.column)
    raw_lines = text.splitlines()
    records: list[Suppression] = []
    for line in lines:
        raw = raw_lines[line.line - 1]
        source = line.code
        if not source and raw.lstrip().startswith("#"):
            continue
        if not source:
            source = raw
        for match in SHELL_STATUS_MASK_RE.finditer(source):
            column = match.start() + 1
            if column in active_columns.get(line.line, set()):
                continue
            records.append(
                Suppression(
                    path,
                    line.line,
                    column,
                    "shell-status",
                    "shell",
                    "ignored-status",
                    match.group(0),
                    "embedded-shell-or-heredoc",
                    line.comment.strip(),
                    "embedded-text-audit",
                    ownership(path),
                )
            )
    return records


def _active_shell_status_records(
    path: str,
    lines: list[HashLexLine],
    *,
    include_shellcheck_options: bool = False,
) -> list[Suppression]:
    """Inventory direct and multiline status masks in executable shell code."""
    records: list[Suppression] = []
    state = ShellOperatorState()
    pending: Suppression | None = None
    for line in lines:
        active_code = mask_shell_operators(line.code, state)
        if pending is not None and active_code.strip():
            if re.match(r"^\s*(?:true\b|:(?![A-Za-z0-9_]))", active_code):
                records.append(pending)
            pending = None
        records.extend(_shell_status_line_records(path, line, active_code))
        if include_shellcheck_options:
            records.extend(_shellcheck_options_records(path, line))
        operator = re.search(r"\|\|\s*$", active_code)
        if operator is not None:
            pending = Suppression(
                path,
                line.line,
                operator.start() + 1,
                "shell-status",
                "shell",
                "ignored-status",
                "|| true",
                "command-list",
                line.comment.strip(),
                "active-shell-syntax",
                ownership(path),
            )
    return records


def shell_status_records(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory active shell status masks and global ShellCheck exclusions.

    Runtime status masks are behavior rather than analyzer waivers, so source
    comments are optional; every occurrence remains an explicit manual-review
    row even when its ``reason`` is blank.
    """
    first_line = text.partition("\n")[0]
    shell_source = is_shell_control(path, first_line)
    if not shell_source and path != ".shellcheckrc":
        return [], []
    lines, findings = hash_lines(path, text)
    if not shell_source:
        return _shellcheckrc_records(path, lines), findings
    records = _active_shell_status_records(path, lines, include_shellcheck_options=True)
    records.extend(_embedded_shell_status_records(path, text, lines, records))
    return records, findings


YAML_BLOCK_HEADER_RE = re.compile(
    r"^(?P<indent> *)(?:-\s+)?(?P<key>[A-Za-z0-9_.-]+):\s*[>|]"
    r"(?:[+-]?[1-9]?|[1-9][+-]?)?\s*(?:#.*)?$"
)


def yaml_shell_block_status_records(
    path: str, text: str
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory shell masks in executable YAML block scalars.

    Workflow ``run`` and Ansible ``shell`` blocks are executable by definition.
    Other block keys, such as ``copy.content``, are scanned only when their first
    nonblank payload line is a shell shebang.
    """
    if Path(path).suffix.lower() not in {".yaml", ".yml"}:
        return [], []
    raw_lines = text.splitlines()
    records: list[Suppression] = []
    findings: list[Finding] = []
    index = 0
    while index < len(raw_lines):
        header = YAML_BLOCK_HEADER_RE.match(raw_lines[index])
        if header is None:
            index += 1
            continue
        header_indent = len(header.group("indent"))
        end = index + 1
        while end < len(raw_lines):
            raw = raw_lines[end]
            indent = len(raw) - len(raw.lstrip(" "))
            if raw.strip() and indent <= header_indent:
                break
            end += 1
        payload = raw_lines[index + 1 : end]
        nonblank = [raw for raw in payload if raw.strip()]
        if not nonblank:
            index = end
            continue
        content_indent = min(len(raw) - len(raw.lstrip(" ")) for raw in nonblank)
        dedented = [raw[content_indent:] if raw.strip() else "" for raw in payload]
        first = next(raw.lstrip() for raw in dedented if raw.strip())
        key = header.group("key").rsplit(".", 1)[-1].lower()
        shell_payload = key in {"run", "shell"} or (
            first.startswith("#!") and any(word in first for word in ("sh", "bash", "zsh"))
        )
        if not shell_payload:
            index = end
            continue
        block_text = "\n".join(dedented)
        block_lines, _ = hash_lines("embedded-shell.sh", block_text)
        active = _active_shell_status_records(path, block_lines)
        active.extend(_embedded_shell_status_records(path, block_text, block_lines, active))
        records.extend(
            replace(
                record,
                line=index + 1 + record.line,
                column=content_indent + record.column,
                provenance="yaml-shell-block",
                fingerprint="",
            )
            for record in active
        )
        index = end
    return records, findings
