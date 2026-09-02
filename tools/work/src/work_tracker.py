# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Load the single machine-readable GitHub tracker authority."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import NoReturn

from work_git import WorkError

REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
LOGIN_RE = re.compile(r"^[A-Za-z0-9-]{1,39}$")
SCHEMA_VERSION = 1


def fail(message: str, cause: BaseException | None = None) -> NoReturn:
    """Raise one consistently constructed tracker-schema error."""
    error = WorkError(message)
    if cause is not None:
        raise error from cause
    raise error


@dataclass(frozen=True)
class TrackerSchema:
    """Exact GitHub target plus allowed project field names."""

    github_host: str
    repository: str
    project_owner: str
    project_number: int
    statuses: tuple[str, ...]
    tracks: tuple[str, ...]


def _string_list(payload: object, key: str) -> tuple[str, ...]:
    """Read one non-empty duplicate-free string list."""
    if not isinstance(payload, dict):
        fail("tracker schema root must be an object")
    raw = payload.get(key)
    if not isinstance(raw, list) or not raw:
        fail(f"tracker schema {key} must be a non-empty list")
    values = tuple(raw)
    if any(not isinstance(value, str) or not value or not value.isprintable() for value in values):
        fail(f"tracker schema {key} contains an invalid value")
    if len(set(values)) != len(values):
        fail(f"tracker schema {key} contains a duplicate")
    return values


@lru_cache(maxsize=1)
def tracker_schema() -> TrackerSchema:
    """Load and validate ``tools/work/tracker.json`` once per process."""
    path = Path(__file__).resolve().parents[1] / "tracker.json"
    try:
        payload = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"tracker schema could not be read: {exc}", exc)
    expected = {
        "schema_version",
        "github_host",
        "repository",
        "project_owner",
        "project_number",
        "statuses",
        "tracks",
    }
    valid_root = isinstance(payload, dict) and set(payload) == expected
    if not valid_root or payload.get("schema_version") != SCHEMA_VERSION:
        fail("tracker schema keys or version are unsupported")
    host = payload["github_host"]
    repository = payload["repository"]
    owner = payload["project_owner"]
    number = payload["project_number"]
    if host != "github.com":
        fail("tracker schema must pin github.com")
    if not isinstance(repository, str) or REPOSITORY_RE.fullmatch(repository) is None:
        fail("tracker schema repository must be owner/name")
    if not isinstance(owner, str) or LOGIN_RE.fullmatch(owner) is None:
        fail("tracker schema project_owner is invalid")
    if not isinstance(number, int) or isinstance(number, bool) or number < 1:
        fail("tracker schema project_number must be positive")
    return TrackerSchema(
        github_host=host,
        repository=repository,
        project_owner=owner,
        project_number=number,
        statuses=_string_list(payload, "statuses"),
        tracks=_string_list(payload, "tracks"),
    )
