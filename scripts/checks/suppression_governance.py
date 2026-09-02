# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Typed repository-governance controls that are not ordinary lint comments."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml
from check_gitignore_scope import marker_bindings
from suppression_catalog import ownership
from suppression_comment_lex import extract_comments
from suppression_model import Finding, Suppression

CI_DIR = Path(__file__).resolve().parents[1] / "ci"
if str(CI_DIR) not in sys.path:
    sys.path.insert(0, str(CI_DIR))
from check_ci_parity import (  # noqa: E402 -- follows the CI_DIR sys.path insert above
    classify_step,
    iter_run_steps,
)

ANSIBLE_CONFIG_NAMES = frozenset({".ansible-lint", ".ansible-lint.yml", ".ansible-lint.yaml"})
ANSIBLE_LIST_KEYS = frozenset({"skip_list", "warn_list", "exclude_paths"})

# No non-Ruff global exclusion authority exists in this tree. This typed
# registry is deliberately empty: adding a real tool config requires naming
# its exact file, table and key here rather than reviving a prose regex.
GLOBAL_EXCLUSION_AUTHORITIES: dict[str, tuple[str, str]] = {}

SECURITY_RULES = (
    (
        re.compile(
            r"^nosemgrep(?:\s*:\s*(?P<rule>[A-Za-z0-9_.-]+))?"
            r"(?:\s+--\s+(?P<reason>\S.*))?$",
            re.IGNORECASE,
        ),
        "semgrep",
    ),
    (re.compile(r"^NOSONAR(?:\s+--\s+(?P<reason>\S.*))?$"), "sonarqube"),
    (
        re.compile(
            r"^lgtm\[(?P<rule>[A-Za-z0-9_./-]+)\]"
            r"(?:\s+--\s+(?P<reason>\S.*))?$",
            re.IGNORECASE,
        ),
        "codeql-legacy",
    ),
)
OTHER_LANGUAGE_RULES = {
    ".java": (
        "java",
        re.compile(
            r'^@SuppressWarnings\(\s*"(?P<rule>[A-Za-z0-9_.-]+)"\s*\)\s*(?://\s*(?P<reason>\S.*))?$'
        ),
    ),
    ".kt": (
        "kotlin",
        re.compile(
            r'^@Suppress\(\s*"(?P<rule>[A-Za-z0-9_.-]+)"\s*\)\s*(?://\s*(?P<reason>\S.*))?$'
        ),
    ),
    ".rs": (
        "rust",
        re.compile(
            r"^#\[(?P<kind>allow|expect)\((?P<rule>[A-Za-z0-9_:.-]+)\)\]\s*(?://\s*(?P<reason>\S.*))?$"
        ),
    ),
    ".go": (
        "go",
        re.compile(r"^//nolint:(?P<rule>[A-Za-z0-9_,.-]+)\s+//\s*(?P<reason>\S.*)$"),
    ),
}


@dataclass(frozen=True)
class GovernanceSpec:
    """Normalized fields for one governance control."""

    family: str
    tool: str
    rule: str
    directive: str
    scope: str
    reason: str
    provenance: str


def _record(path: str, line: int, spec: GovernanceSpec) -> Suppression:
    """Build one deterministic governance row."""
    concerns = () if spec.reason else ("blank-reason",)
    return Suppression(
        path,
        line,
        1,
        spec.family,
        spec.tool,
        spec.rule,
        spec.directive,
        spec.scope,
        spec.reason,
        spec.provenance,
        ownership(path),
        concerns,
    )


def _ansible_key_records(
    path: str,
    key: str,
    values: object,
    lines: list[str],
    key_line: int,
) -> tuple[list[Suppression], list[Finding]]:
    """Parse one ansible-lint list authority and its item-local reasons."""
    if not isinstance(values, list):
        finding = Finding("malformed-ansible-lint-config", f"{key} is not a list", path)
        return [], [finding]
    records: list[Suppression] = []
    findings: list[Finding] = []
    for offset, value in enumerate(values):
        if not isinstance(value, str) or not value.strip():
            findings.append(
                Finding(
                    "malformed-ansible-lint-config",
                    f"{key} has non-string item",
                    path,
                    key_line,
                )
            )
            continue
        item_line = next(
            (
                number
                for number, raw in enumerate(lines[key_line - 1 :], start=key_line)
                if re.match(rf"^\s*-\s*{re.escape(value)}(?:\s*(?:#.*)?)$", raw)
            ),
            key_line + offset + 1,
        )
        raw = lines[item_line - 1] if item_line <= len(lines) else ""
        reason = raw.partition("#")[2].strip()
        spec = GovernanceSpec(
            "ansible-lint-config",
            "ansible-lint",
            value,
            key.replace("_", "-"),
            f"config:{key}",
            reason,
            "central-config",
        )
        records.append(_record(path, item_line, spec))
    return records, findings


def scan_ansible_lint_config(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Parse exact ansible-lint list authorities, not task/docs substrings."""
    if Path(path).name not in ANSIBLE_CONFIG_NAMES:
        return [], []
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        return [], [Finding("malformed-ansible-lint-config", str(exc), path)]
    if not isinstance(doc, dict):
        return [], [Finding("malformed-ansible-lint-config", "top level is not a mapping", path)]
    lines = text.splitlines()
    records: list[Suppression] = []
    findings: list[Finding] = []
    for key in ANSIBLE_LIST_KEYS:
        if key not in doc:
            continue
        key_line = next(
            (
                number
                for number, raw in enumerate(lines, start=1)
                if re.match(rf"^\s*{key}\s*:", raw)
            ),
            1,
        )
        rows, problems = _ansible_key_records(path, key, doc[key], lines, key_line)
        records.extend(rows)
        findings.extend(problems)
    return records, findings


def scan_registered_global_exclusions(
    path: str, _text: str
) -> tuple[list[Suppression], list[Finding]]:
    """Parse only explicitly registered non-Ruff exclusion authorities."""
    authority = GLOBAL_EXCLUSION_AUTHORITIES.get(path)
    if authority is None:
        return [], []
    table, key = authority
    # No authority is registered today. Keep the branch executable and
    # fail-closed for the first future registration rather than accepting a
    # generic `exclude_files` substring anywhere in the tree.
    message = f"registered parser not implemented for {table}.{key}"
    return [], [Finding("malformed-global-exclusion-config", message, path)]


def scan_gitignore_exemptions(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory the exact marker-to-unanchored-pattern bindings the gate consumes."""
    if path != ".gitignore":
        return [], []
    bindings, errors = marker_bindings(text)
    records = [
        _record(
            path,
            item.marker_line,
            GovernanceSpec(
                "project-policy",
                "gitignore-scope",
                "unanchored-directory-exemption",
                "gitignore-scope-ok",
                f"pattern:{item.pattern}",
                item.reason,
                "bound-comment-block",
            ),
        )
        for item in bindings
    ]
    findings = [
        Finding("malformed-gitignore-scope-marker", message, path, line) for line, message in errors
    ]
    return records, findings


def scan_ci_parity_exemptions(
    root: Path, paths: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory active infra run steps through the parity checker's parser."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    workflows = [
        rel
        for rel in paths
        if rel.startswith(".github/workflows/") and Path(rel).suffix in {".yml", ".yaml"}
    ]
    for rel in workflows:
        workflow = root / rel
        try:
            text = workflow.read_text(encoding="utf-8")
            steps = list(iter_run_steps(workflow))
        except (OSError, yaml.YAMLError) as exc:
            findings.append(Finding("malformed-ci-parity-workflow", str(exc), rel))
            continue
        for step in steps:
            kind, _gates, reason = classify_step(step.body)
            if kind != "infra":
                continue
            label = step.label
            line = next(
                (
                    number
                    for number, raw in enumerate(text.splitlines(), start=1)
                    if re.match(rf"^\s*-?\s*name:\s*['\"]?{re.escape(label)}['\"]?\s*$", raw)
                ),
                1,
            )
            records.append(
                _record(
                    rel,
                    line,
                    GovernanceSpec(
                        "ci-parity-exemption",
                        "ci-parity",
                        "infrastructure-step",
                        "ci-parity: infra",
                        f"job:{step.job_name}/step:{label}",
                        reason or "",
                        "workflow-run-step",
                    ),
                )
            )
    return records, findings


def _doxygen_assignments(text: str) -> tuple[list[tuple[int, str, list[str], str]], list[Finding]]:
    """Return top-level Doxyfile assignments with continuation values/reasons."""
    rows: list[tuple[int, str, list[str], str]] = []
    findings: list[Finding] = []
    lines = text.splitlines()
    index = 0
    comments: list[str] = []
    while index < len(lines):
        raw = lines[index]
        stripped = raw.strip()
        if stripped.startswith("#"):
            comments.append(stripped[1:].strip())
            index += 1
            continue
        match = re.match(r"^(?P<key>[A-Z][A-Z0-9_]*)\s*(?P<op>\+?=)\s*(?P<value>.*)$", raw)
        if match is None:
            if stripped:
                comments = []
            index += 1
            continue
        line_no = index + 1
        values: list[str] = []
        part = match.group("value").strip()
        while True:
            continued = part.endswith("\\")
            if continued:
                part = part[:-1].rstrip()
            values.extend(part.split())
            if not continued:
                break
            index += 1
            if index >= len(lines):
                findings.append(
                    Finding(
                        "malformed-doxygen-config",
                        f"unterminated {match.group('key')}",
                        "Doxyfile",
                        line_no,
                    )
                )
                break
            part = lines[index].strip()
        reason = " ".join(item for item in comments if item).strip()
        rows.append((line_no, match.group("key"), values, reason))
        comments = []
        index += 1
    return rows, findings


def scan_doxygen_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory the fatality and exclusion assignments in the root Doxyfile."""
    if path != "Doxyfile":
        return [], []
    assignments, findings = _doxygen_assignments(text)
    keys = {
        "WARN_IF_UNDOCUMENTED",
        "WARN_NO_PARAMDOC",
        "WARN_AS_ERROR",
        "EXCLUDE",
        "EXCLUDE_PATTERNS",
    }
    records: list[Suppression] = []
    for line, key, assigned_values, assigned_reason in assignments:
        if key not in keys:
            continue
        if key.startswith("WARN_"):
            values = assigned_values[:1]
            reason = assigned_reason or (
                "Doxygen warning policy is explicitly configured at repository scope."
            )
        else:
            values = assigned_values
            reason = assigned_reason or (
                "Doxygen excludes non-product, generated, test, or vendored documentation inputs."
            )
        for value in values:
            spec = GovernanceSpec(
                "documentation-control",
                "doxygen",
                f"{key}:{value}",
                key,
                f"value:{value}",
                reason,
                "central-config",
            )
            records.append(_record(path, line, spec))
    return records, findings


def scan_security_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Recognize exact first-party security-analyzer comment directives."""
    if ownership(path) != "first-party":
        return [], []
    comments, lex_findings = extract_comments(path, text)
    records: list[Suppression] = []
    for comment in comments:
        body = comment.text.strip().lstrip("*").strip()
        for pattern, tool in SECURITY_RULES:
            match = pattern.fullmatch(body)
            if match is None:
                continue
            groups = match.groupdict()
            records.append(
                _record(
                    path,
                    comment.line,
                    GovernanceSpec(
                        "security-analysis-control",
                        tool,
                        groups.get("rule") or "all",
                        body.split()[0],
                        "line",
                        groups.get("reason") or "",
                        "inline-comment",
                    ),
                )
            )
            break
    findings = [Finding(item.code, item.message, path, item.line) for item in lex_findings]
    return records, findings


def scan_other_language_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Recognize exact Java/Kotlin/Rust/Go suppression syntax in matching files."""
    spec = OTHER_LANGUAGE_RULES.get(Path(path).suffix.lower())
    if spec is None or ownership(path) != "first-party":
        return [], []
    tool, pattern = spec
    records: list[Suppression] = []
    for line, raw in enumerate(text.splitlines(), start=1):
        match = pattern.fullmatch(raw.strip())
        if match is None:
            continue
        groups = match.groupdict()
        records.append(
            _record(
                path,
                line,
                GovernanceSpec(
                    "other-language-control",
                    tool,
                    groups["rule"],
                    groups.get("kind") or "suppress",
                    "declaration",
                    groups.get("reason") or "",
                    "language-syntax",
                ),
            )
        )
    return records, []


def scan_governance_file(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Run every typed per-file governance parser."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for parser in (
        scan_ansible_lint_config,
        scan_registered_global_exclusions,
        scan_gitignore_exemptions,
        scan_doxygen_controls,
        scan_security_controls,
        scan_other_language_controls,
    ):
        found, problems = parser(path, text)
        records.extend(found)
        findings.extend(problems)
    return records, findings
