# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

"""Frozen data models for the read-only terminal board explorer."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, kw_only=True)
class BoardItem:
    """Strict projection of one open Issue project item."""

    number: int
    title: str
    url: str
    repository: str
    content_type: str
    status: str | None
    track: str | None
    horizon: str | None
    project_priority: str | None
    priority_labels: tuple[str, ...]
    effort_labels: tuple[str, ...]
    epic_labels: tuple[str, ...]
    labels: tuple[str, ...]


@dataclass(frozen=True, kw_only=True)
class Relation:
    """One native GitHub issue relation."""

    number: int
    title: str
    url: str
    state: str


@dataclass(frozen=True, kw_only=True)
class RelationConnection:
    """One strict GitHub connection with pagination evidence."""

    total_count: int
    nodes: tuple[Relation, ...]


@dataclass(frozen=True, kw_only=True)
class IssueDetail:
    """Strict native relationship detail for one issue."""

    number: int
    title: str
    url: str
    state: str
    updated_at: str
    labels: tuple[str, ...]
    parent: Relation | None
    sub_issues: RelationConnection
    blocked_by: RelationConnection
    blocking: RelationConnection
