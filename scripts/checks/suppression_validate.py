# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural validation for normalized suppression inventory rows."""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

from suppression_model import Finding, Inventory, Suppression


def deduplicate(inventory: Inventory) -> None:
    """Add findings for repeated rows without hiding either inventory record."""
    keys = (
        (item.path, 0, 0, item.family, item.rule, item.scope)
        if item.provenance == "central-list"
        else item.duplicate_key()
        for item in inventory.suppressions
    )
    counts = Counter(keys)
    for key, count in sorted(counts.items()):
        if count > 1:
            path, line, _column, family, rule, scope = key
            message = f"{count} identical {family}/{rule}/{scope} rows"
            inventory.findings.append(Finding("duplicate-directive", message, path, line))


def validate_fingerprints(inventory: Inventory) -> None:
    """Reject ambiguous public row identities before rendering inventory."""
    counts = Counter(item.fingerprint for item in inventory.suppressions)
    for fingerprint, count in sorted(counts.items()):
        if count > 1:
            message = f"{count} rows share fingerprint {fingerprint}"
            inventory.findings.append(Finding("duplicate-fingerprint", message))


def validate_cppcheck_anchors(inventory: Inventory, root: Path) -> None:
    """Reject literal cppcheck line anchors that no longer name source."""
    for item in inventory.suppressions:
        if item.provenance != "central-list":
            continue
        match = re.fullmatch(r"(?P<path>.+):(?P<line>[1-9][0-9]*)", item.scope)
        if match is None:
            continue
        rel = match.group("path")
        if any(char in rel for char in "*?["):
            continue
        target = root / rel
        try:
            line_count = len(target.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError:
            message = f"{item.scope} targets a missing source file"
            inventory.findings.append(
                Finding("dead-cppcheck-anchor", message, item.path, item.line)
            )
            continue
        source_line = int(match.group("line"))
        if source_line > line_count:
            message = f"{item.scope} is past EOF ({line_count} lines)"
            inventory.findings.append(
                Finding("dead-cppcheck-anchor", message, item.path, item.line)
            )


def _region_identity(family: str, rules: frozenset[str]) -> tuple[str, ...]:
    """Normalize only directional coverage suffixes for region pairing."""
    if family == "coverage":
        normalized = {re.sub(r"_(?:START|STOP)$", "", rule) for rule in rules}
        return family, *sorted(normalized)
    return family, *sorted(rules)


def _regions_compatible(
    family: str, start_rules: frozenset[str], end_rules: frozenset[str]
) -> bool:
    """Return whether one logical end closes one logical region start."""
    return _region_identity(family, start_rules) == _region_identity(family, end_rules)


def validate_regions(inventory: Inventory) -> None:
    """Reject invalid regions while honoring markdownlint state snapshots."""
    groups: dict[tuple[str, int, int, str, str, str], list[Suppression]] = {}
    for item in inventory.suppressions:
        if item.owner == "vendor" or item.provenance not in {
            "inline-comment",
            "compiler-pragma",
        }:
            continue
        if not item.scope.startswith(("region-", "state-")):
            continue
        key = (item.path, item.line, item.column, item.family, item.tool, item.scope)
        groups.setdefault(key, []).append(item)
    stack_type = list[tuple[Suppression, frozenset[str]]]
    stacks: dict[tuple[str, str, str], stack_type] = {}
    snapshots: dict[tuple[str, str, str], stack_type] = {}
    for group_key in sorted(groups):
        items = groups[group_key]
        item = items[0]
        rules = frozenset(member.rule for member in items)
        key = (item.path, item.family, item.tool)
        stack = stacks.setdefault(key, [])
        if item.scope == "state-capture":
            snapshots[key] = list(stack)
        elif item.scope == "state-restore":
            stack[:] = snapshots.get(key, [])
        elif item.scope == "region-start":
            stack.append((item, rules))
        elif not stack:
            inventory.findings.append(
                Finding(
                    "unmatched-region-end",
                    f"{item.family}/{sorted(rules)}",
                    item.path,
                    item.line,
                )
            )
        else:
            start, start_rules = stack.pop()
            if not _regions_compatible(item.family, start_rules, rules):
                message = f"{sorted(start_rules)} at line {start.line} closed by {sorted(rules)}"
                inventory.findings.append(
                    Finding("mismatched-region", message, item.path, item.line)
                )
    for stack in stacks.values():
        for item, rules in stack:
            message = f"{item.family}/{sorted(rules)} has no region end"
            inventory.findings.append(
                Finding("unmatched-region-start", message, item.path, item.line)
            )
