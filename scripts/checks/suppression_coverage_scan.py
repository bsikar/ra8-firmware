# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Parse active gcovr result-masking controls and their local provenance."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from suppression_catalog import is_build_control, ownership
from suppression_hash_lex import HashLexLine, hash_lines
from suppression_model import Finding, Suppression

PARSE_ERROR_VALUES = frozenset({"all", "negative_hits.warn", "negative_hits.warn_once_per_file"})
FLAG_RE = re.compile(
    r"(?P<parse>--gcov-ignore-parse-errors(?:=|\s+)(?P<value>[^\s)]+))"
    r"|(?P<unreachable>--exclude-unreachable-branches)"
    r"|(?P<throw>--exclude-throw-branches)"
)
CONFIG_KEYS = frozenset(
    {"gcov-ignore-parse-errors", "exclude-unreachable-branches", "exclude-throw-branches"}
)


def _reason(line: HashLexLine) -> str:
    """Return the same-line rationale for one active coverage mask."""
    return line.comment.strip()


@dataclass(frozen=True)
class CoverageControl:
    """Normalized gcovr coverage-mask fields."""

    column: int
    rule: str
    reason: str
    broad: bool


def _record(path: str, line: HashLexLine, control: CoverageControl) -> Suppression:
    """Build one gcovr coverage-control inventory row."""
    concerns: list[str] = []
    if not control.reason:
        concerns.append("blank-reason")
    if control.broad:
        concerns.append("broad-coverage-mask")
    return Suppression(
        path,
        line.line,
        control.column,
        "coverage-mask",
        "gcovr",
        control.rule,
        control.rule,
        "coverage-report",
        control.reason,
        "coverage-config",
        ownership(path),
        tuple(concerns),
        evidence=("producer:gcovr-7.0",),
        recommendation="retain-only-with-producer-evidence",
    )


def _is_data_only(prefix: str) -> bool:
    """Reject quoted examples and assignments that do not invoke gcovr."""
    stripped = prefix.strip()
    if re.match(r"^(?:echo|printf|message)\b", stripped):
        return True
    return re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", stripped) is not None


def _scan_flags(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Parse active shell/CMake gcovr option tokens."""
    first_line = text.partition("\n")[0]
    if not is_build_control(path, first_line):
        return [], []
    lines, lex_findings = hash_lines(path, text)
    records: list[Suppression] = []
    findings = [Finding(item.code, item.message, path, item.line) for item in lex_findings]
    for line in lines:
        for match in FLAG_RE.finditer(line.code):
            if _is_data_only(line.code[: match.start()]):
                continue
            reason = _reason(line)
            if match.group("parse"):
                value = match.group("value").strip("'\"")
                if value not in PARSE_ERROR_VALUES:
                    message = f"unsupported gcovr 7.0 parse-error class {value!r}"
                    findings.append(Finding("malformed-coverage-mask", message, path, line.line))
                    continue
                rule = f"gcov-ignore-parse-errors={value}"
                records.append(
                    _record(
                        path,
                        line,
                        CoverageControl(match.start() + 1, rule, reason, value == "all"),
                    )
                )
            elif match.group("unreachable"):
                records.append(
                    _record(
                        path,
                        line,
                        CoverageControl(
                            match.start() + 1,
                            "exclude-unreachable-branches",
                            reason,
                            broad=False,
                        ),
                    )
                )
            else:
                records.append(
                    _record(
                        path,
                        line,
                        CoverageControl(
                            match.start() + 1,
                            "exclude-throw-branches",
                            reason,
                            broad=False,
                        ),
                    )
                )
    return records, findings


def _config_bool(value: str) -> bool | None:
    """Parse gcovr's accepted boolean spellings."""
    lowered = value.lower()
    if lowered in {"yes", "true", "1"}:
        return True
    if lowered in {"no", "false", "0"}:
        return False
    return None


def _scan_config(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Parse an authoritative gcovr.cfg without accepting unknown mask syntax."""
    if Path(path).name != "gcovr.cfg":
        return [], []
    records: list[Suppression] = []
    findings: list[Finding] = []
    seen: dict[str, int] = {}
    for line_no, raw in enumerate(text.splitlines(), start=1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#") or "=" not in raw:
            continue
        key, value_and_comment = (part.strip() for part in raw.split("=", 1))
        value, _separator, comment = value_and_comment.partition("#")
        value = value.strip()
        if key not in CONFIG_KEYS:
            continue
        if key in seen:
            message = f"{key} duplicates line {seen[key]}"
            findings.append(Finding("duplicate-coverage-mask", message, path, line_no))
            continue
        seen[key] = line_no
        if key == "gcov-ignore-parse-errors":
            if value not in PARSE_ERROR_VALUES:
                findings.append(Finding("malformed-coverage-mask", f"{key}={value}", path, line_no))
                continue
            rule = f"{key}={value}"
            broad = value == "all"
        else:
            active = _config_bool(value)
            if active is None:
                findings.append(Finding("malformed-coverage-mask", f"{key}={value}", path, line_no))
                continue
            if not active:
                continue
            rule = key
            broad = False
        line = HashLexLine(line_no, raw, comment.strip(), raw.find("#") + 1)
        records.append(_record(path, line, CoverageControl(1, rule, comment.strip(), broad)))
    return records, findings


def scan_coverage_masks(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory active gcovr masks from command lines and central config."""
    records, findings = _scan_flags(path, text)
    config_records, config_findings = _scan_config(path, text)
    records.extend(config_records)
    findings.extend(config_findings)
    return records, findings
