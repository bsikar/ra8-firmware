# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Source-located clang-tidy global exclusion inventory."""

from __future__ import annotations

import re

import yaml
from suppression_catalog import ownership
from suppression_model import Finding, Suppression

CLANG_TIDY_REASON_RE = re.compile(r"^\s*#\s+-(?P<rule>[A-Za-z0-9.*_-]+),?(?:\s+.*)?$")
CLANG_TIDY_RULE_RE = re.compile(r"[A-Za-z0-9.*_-]+")


def _concerns(rule: str, reason: str) -> tuple[str, ...]:
    """Return machine-observable review concerns for one global exclusion."""
    concerns: list[str] = []
    if rule == "*":
        concerns.append("broad-rule")
    if not reason:
        concerns.append("blank-reason")
    return tuple(concerns)


def _reasons(text: str) -> dict[str, str]:
    """Associate documented disable headings with their following comments."""
    result: dict[str, str] = {}
    active: list[str] = []
    notes: list[str] = []
    for raw in text.splitlines():
        if raw.startswith("Checks:"):
            break
        heading = CLANG_TIDY_REASON_RE.fullmatch(raw)
        if heading is not None:
            if active and notes:
                reason = " ".join(notes).strip()
                result.update(dict.fromkeys(active, reason))
                active = []
                notes = []
            active.append(heading.group("rule"))
            continue
        if not active:
            continue
        stripped = raw.strip()
        if not stripped.startswith("#"):
            continue
        note = stripped[1:].strip()
        if note:
            notes.append(note)
    if active:
        reason = " ".join(notes).strip()
        result.update(dict.fromkeys(active, reason))
    return result


def scan_clang_tidy_config(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory every negative clang-tidy Checks glob from active YAML."""
    if path != ".clang-tidy":
        return [], []
    try:
        parsed = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        return [], [Finding("malformed-clang-tidy-config", str(exc), path)]
    if not isinstance(parsed, dict) or not isinstance(parsed.get("Checks"), str):
        message = "top-level Checks must be a YAML string"
        return [], [Finding("malformed-clang-tidy-config", message, path)]
    entries = [entry.strip() for entry in parsed["Checks"].split(",") if entry.strip()]
    negative = [entry[1:].strip() for entry in entries if entry.startswith("-")]
    findings: list[Finding] = []
    if any(not rule or CLANG_TIDY_RULE_RE.fullmatch(rule) is None for rule in negative):
        findings.append(
            Finding("malformed-clang-tidy-config", "invalid negative Checks glob", path)
        )
    if len(negative) != len(set(negative)):
        findings.append(
            Finding("malformed-clang-tidy-config", "duplicate negative Checks glob", path)
        )
    reasons = _reasons(text)
    checks_offset = text.find("Checks:")
    records: list[Suppression] = []
    for rule in negative:
        offset = text.find("-" + rule, checks_offset)
        if offset < 0:
            findings.append(
                Finding(
                    "malformed-clang-tidy-config",
                    f"cannot source-locate negative Checks glob {rule}",
                    path,
                )
            )
            continue
        line = text.count("\n", 0, offset) + 1
        line_start = text.rfind("\n", 0, offset)
        column = offset - line_start
        normalized = "*" if rule == "*" else rule
        reason = reasons.get(rule, "")
        records.append(
            Suppression(
                path,
                line,
                column,
                "clang-tidy",
                "clang-tidy",
                normalized,
                "Checks exclude",
                "repository",
                reason,
                "central-config",
                ownership(path),
                _concerns(normalized, reason),
            )
        )
    return records, findings
