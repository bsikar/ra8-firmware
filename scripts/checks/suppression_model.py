# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Data model shared by the suppression inventory scanner and renderers."""

from __future__ import annotations

import hashlib
from collections import Counter
from dataclasses import asdict, dataclass, field


@dataclass(frozen=True)
class Suppression:
    """One syntax-recognized suppression or waiver directive."""

    path: str
    line: int
    column: int
    family: str
    tool: str
    rule: str
    directive: str
    scope: str
    reason: str
    provenance: str
    owner: str
    concerns: tuple[str, ...] = ()
    fingerprint: str = ""
    evidence: tuple[str, ...] = ()
    match_count: int = 1
    recommendation: str = "manual-review"
    disposition: str = "unreviewed"
    site_id: str = ""
    binding_sha256: str = ""
    anchor: str = ""

    def __post_init__(self) -> None:
        """Derive a stable content identity when the scanner did not supply one."""
        if self.fingerprint:
            return
        fields = (
            self.path,
            str(self.line),
            str(self.column),
            self.family,
            self.tool,
            self.rule,
            self.directive,
            self.scope,
            self.provenance,
            self.reason,
        )
        digest = hashlib.sha256("\0".join(fields).encode("utf-8")).hexdigest()[:20]
        object.__setattr__(self, "fingerprint", digest)

    def as_dict(self) -> dict[str, object]:
        """Return a stable JSON-compatible representation."""
        data = asdict(self)
        data["concerns"] = list(self.concerns)
        data["evidence"] = list(self.evidence)
        return data

    def duplicate_key(self) -> tuple[str, int, int, str, str, str]:
        """Return fields that identify an accidental repeated directive."""
        return (self.path, self.line, self.column, self.family, self.rule, self.scope)


@dataclass(frozen=True)
class Finding:
    """A scanner problem that prevents the inventory from being authoritative."""

    code: str
    message: str
    path: str = ""
    line: int = 0

    def as_dict(self) -> dict[str, object]:
        """Return a stable JSON-compatible representation."""
        return asdict(self)


@dataclass
class Inventory:
    """Phase-one suppression inventory plus scanner integrity evidence."""

    suppressions: list[Suppression] = field(default_factory=list)
    findings: list[Finding] = field(default_factory=list)
    files_scanned: int = 0
    text_files: int = 0
    binary_files: int = 0

    def family_counts(self) -> dict[str, int]:
        """Count inventory rows by suppression family."""
        return dict(sorted(Counter(item.family for item in self.suppressions).items()))

    def owner_counts(self) -> dict[str, int]:
        """Count inventory rows by first-party, generated, or vendor ownership."""
        return dict(sorted(Counter(item.owner for item in self.suppressions).items()))

    def concern_counts(self) -> dict[str, int]:
        """Count review concerns carried by recognized inventory rows."""
        counts = Counter(concern for item in self.suppressions for concern in item.concerns)
        return dict(sorted(counts.items()))

    def as_dict(self) -> dict[str, object]:
        """Return the deterministic public inventory document."""
        ordered = sorted(self.suppressions, key=_suppression_sort_key)
        findings = sorted(self.findings, key=_finding_sort_key)
        return {
            "schema_version": "2-durable-site-identity",
            "summary": {
                "files_scanned": self.files_scanned,
                "text_files": self.text_files,
                "binary_files": self.binary_files,
                "suppressions": len(ordered),
                "findings": len(findings),
                "families": self.family_counts(),
                "owners": self.owner_counts(),
                "concerns": self.concern_counts(),
            },
            "suppressions": [item.as_dict() for item in ordered],
            "findings": [item.as_dict() for item in findings],
        }


def _suppression_sort_key(item: Suppression) -> tuple[object, ...]:
    """Return deterministic ordering fields for suppression rows."""
    return (item.path, item.line, item.column, item.family, item.rule, item.directive)


def _finding_sort_key(item: Finding) -> tuple[object, ...]:
    """Return deterministic ordering fields for findings."""
    return (item.path, item.line, item.code, item.message)
