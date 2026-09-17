# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Durable two-part identity for suppression inventory rows.

Line and column are display coordinates, not identity. Every row carries:

* ``site_id`` -- a full SHA-256 naming the occurrence. It hashes the path,
  family, tool, rule, directive, a line-normalized scope, the normalized
  suppressed construct (the source line's collapsed text with an attached
  rationale removed, or structural evidence for repository-bound rows), and a
  deterministic ordinal
  that separates otherwise identical repeated constructs. Inserting lines
  above a site or re-indenting it does not change its ``site_id``; changing
  the construct does.
* ``binding_sha256`` -- a full SHA-256 over the canonical JSON of everything
  a reviewer approved: path, family, tool, rule, directive, raw scope,
  reason, provenance, owner, concerns, normalized evidence, match count,
  recommendation, and the anchor. Any reason, target, rule, scope, owner,
  evidence, count, or construct change invalidates the binding while the
  site keeps its name.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import replace
from pathlib import Path

from suppression_model import Finding, Inventory, Suppression

IDENTITY_SCHEMA_VERSION = "2-durable-site-identity"
_LINE_REF_RE = re.compile(r"^decision-line:\d+$")
_LINE_REF_SUFFIX_RE = re.compile(
    r"^(?P<head>decision-line):\d+$|^(?P<blob>blob):sha256:[0-9a-f]{64}$"
)
_WS_RE = re.compile(r"\s+")
STRUCTURAL_PROVENANCE_PREFIXES = (
    "ratchet-baseline",
    "module-ast-authority",
    "checker-control-plane",
    "workflow-run-step",
    "generator",
    "vendor-boundary",
)


def _normalized_scope(scope: str) -> str:
    """Strip line references and content digests from a scope label.

    The stripped detail stays in the binding: a moved decision line keeps its
    site while a changed vendored blob breaks its reviewed binding.
    """
    match = _LINE_REF_SUFFIX_RE.match(scope)
    if match is None:
        return scope
    return match.group("head") or match.group("blob")


def _normalized_evidence(evidence: tuple[str, ...]) -> tuple[str, ...]:
    """Drop display-derived line references from structural evidence."""
    return tuple(item for item in evidence if not _LINE_REF_RE.match(item))


def _structural_anchor(row: Suppression) -> str | None:
    """Return a structural anchor for rows not located by a source line."""
    if row.provenance.split(":")[0] in STRUCTURAL_PROVENANCE_PREFIXES:
        parts = _normalized_evidence(row.evidence)
        return "|".join(parts) if parts else _normalized_scope(row.scope)
    return None


def _line_anchor(row: Suppression, lines: list[str]) -> str:
    """Return the construct without its review rationale."""
    if not 1 <= row.line <= len(lines):
        return ""
    source = lines[row.line - 1]
    if row.reason:
        reason_at = source.rfind(row.reason, max(row.column - 1, 0))
        if reason_at >= 0:
            source = source[:reason_at].rstrip()
    return _WS_RE.sub(" ", source).strip()


def compute_anchor(row: Suppression, lines: list[str]) -> str:
    """Return the normalized suppressed construct for one row."""
    structural = _structural_anchor(row)
    if structural is not None:
        return structural
    return _line_anchor(row, lines)


def _site_payload(row: Suppression, anchor: str, ordinal: int) -> bytes:
    """Serialize the durable occurrence identity inputs."""
    fields = (
        IDENTITY_SCHEMA_VERSION,
        row.path,
        row.family,
        row.tool,
        row.rule,
        row.directive,
        _normalized_scope(row.scope),
        anchor,
        str(ordinal),
    )
    return "\0".join(fields).encode("utf-8")


def binding_payload(row: Suppression, anchor: str) -> bytes:
    """Serialize the exact reviewed content of one row."""
    payload = {
        "anchor": anchor,
        "concerns": sorted(row.concerns),
        "directive": row.directive,
        "evidence": list(_normalized_evidence(row.evidence)),
        "family": row.family,
        "match_count": row.match_count,
        "owner": row.owner,
        "path": row.path,
        "provenance": row.provenance,
        "reason": row.reason,
        "recommendation": row.recommendation,
        "rule": row.rule,
        "scope": row.scope,
        "tool": row.tool,
    }
    return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")


def assign_identities(inventory: Inventory, root: Path) -> None:
    """Attach ``site_id`` and ``binding_sha256`` to every inventory row."""
    text_cache: dict[str, list[str]] = {}
    anchored: list[tuple[Suppression, str]] = []
    for row in inventory.suppressions:
        if row.path not in text_cache:
            try:
                text_cache[row.path] = (
                    (root / row.path).read_text(encoding="utf-8", errors="replace").splitlines()
                )
            except OSError:
                text_cache[row.path] = []
        anchored.append((row, compute_anchor(row, text_cache[row.path])))
    groups: dict[bytes, list[int]] = {}
    for index, (row, anchor) in enumerate(anchored):
        groups.setdefault(_site_payload(row, anchor, 0), []).append(index)
    site_ids: dict[int, str] = {}
    for members in groups.values():
        members.sort(key=lambda index: (anchored[index][0].line, anchored[index][0].column))
        for ordinal, index in enumerate(members):
            row, anchor = anchored[index]
            site_ids[index] = hashlib.sha256(_site_payload(row, anchor, ordinal)).hexdigest()
    seen: dict[str, int] = {}
    for index, (row, anchor) in enumerate(anchored):
        site_id = site_ids[index]
        if site_id in seen:
            inventory.findings.append(
                Finding(
                    "duplicate-site-identity",
                    f"site {site_id} names two rows",
                    row.path,
                    row.line,
                )
            )
        seen[site_id] = index
        binding = hashlib.sha256(binding_payload(row, anchor)).hexdigest()
        inventory.suppressions[index] = replace(
            row, site_id=site_id, binding_sha256=binding, anchor=anchor
        )
