# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

"""Print a kanban board representation of the project items."""

from __future__ import annotations

import json
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from work_git import ToolMissingError, WorkError, run_process
from work_tracker import TrackerSchema, tracker_schema

GH_TIMEOUT_S = 30
LANDED_RETENTION_DAYS = 7
NO_STATUS = "No Status"
LANDED_STATUS = "landed"


def _load_schema() -> TrackerSchema | None:
    """Return the tracker authority, reporting a load failure."""
    try:
        return tracker_schema()
    except WorkError as exc:
        print(f"error loading tracker schema: {exc}", file=sys.stderr)
        return None


def _fetch_items(schema: TrackerSchema) -> tuple[list, int]:
    """Fetch raw board items, reporting transport and parse failures."""
    try:
        proc = run_process(
            [
                "gh",
                "project",
                "item-list",
                str(schema.project_number),
                "--owner",
                schema.project_owner,
                "--limit",
                "2500",
                "--format",
                "json",
            ],
            timeout=GH_TIMEOUT_S,
        )
    except ToolMissingError:
        print("error: 'gh' CLI is not installed", file=sys.stderr)
        return [], 1
    except WorkError as exc:
        print(f"error fetching board: {exc}", file=sys.stderr)
        return [], 1
    if not proc.ok:
        message = proc.stderr.strip() or proc.stdout.strip() or f"gh exited {proc.returncode}"
        print(f"error fetching board: {message}", file=sys.stderr)
        return [], proc.returncode
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        print(f"error parsing GitHub JSON: {exc}", file=sys.stderr)
        return [], 1
    items = data.get("items", data) if isinstance(data, dict) else data
    if not isinstance(items, list):
        print("error: unexpected JSON structure from gh", file=sys.stderr)
        return [], 1
    return items, 0


def _normalize_status(status_map: dict[str, str], raw: object) -> str:
    """Match a raw status case-insensitively, falling back to the raw string."""
    text = raw if isinstance(raw, str) and raw else NO_STATUS
    return status_map.get(text.lower(), text)


def _is_stale_landed(status: str, item: dict, now: datetime) -> bool:
    """Decide whether a landed item aged past the retention window."""
    if status.lower() != LANDED_STATUS:
        return False
    updated_at = item.get("updatedAt", "")
    if not updated_at:
        return False
    try:
        updated = datetime.fromisoformat(updated_at)
    except ValueError:
        return False
    return now - updated > timedelta(days=LANDED_RETENTION_DAYS)


def _entry(item: dict) -> dict[str, str]:
    """Project one raw item onto its title, number, and repository."""
    content = item.get("content")
    if isinstance(content, dict):
        number = str(content.get("number", "?"))
        repo = content.get("repository", "")
    else:
        number = str(item.get("number", "?"))
        repo = ""
    return {"title": item.get("title", "Untitled"), "number": number, "repo": repo}


def _group_items(
    schema: TrackerSchema, items: list, now: datetime
) -> tuple[dict[str, list], dict[str, list]]:
    """Split items into schema columns and unexpected-status overflow."""
    status_map = {status.lower(): status for status in schema.statuses}
    board: dict[str, list] = {status: [] for status in schema.statuses}
    others: dict[str, list] = {}
    for item in items:
        status = _normalize_status(status_map, item.get("status", NO_STATUS))
        if _is_stale_landed(status, item, now):
            continue
        entry = _entry(item)
        if status in board:
            board[status].append(entry)
        else:
            others.setdefault(status, []).append(entry)
    return board, others


def _print_entries(entries: list) -> None:
    """Print one column with per-item number and repository suffixes."""
    if not entries:
        print("  (empty)")
    for item in entries:
        repo_part = f" {item['repo']}" if item["repo"] else ""
        num_part = f"#{item['number']}" if item["number"] != "?" else ""
        suffix = f" ({num_part}{repo_part})" if (num_part or repo_part) else ""
        print(f"  - {item['title']}{suffix}")


def _print_board(schema: TrackerSchema, board: dict[str, list], others: dict[str, list]) -> None:
    """Print schema columns in order, then any unexpected statuses."""
    print(f"=== Project Board: {schema.project_owner}/{schema.project_number} ===")
    for status in schema.statuses:
        print(f"\n[ {status.upper()} ] ({len(board[status])})")
        _print_entries(board[status])
    for status, entries in others.items():
        print(f"\n[ {status.upper()} ] ({len(entries)})")
        _print_entries(entries)
    print()


def main() -> int:
    """Print the project board grouped by status."""
    schema = _load_schema()
    if schema is None:
        return 1
    items, code = _fetch_items(schema)
    if code != 0:
        return code
    board, others = _group_items(schema, items, datetime.now(UTC))
    _print_board(schema, board, others)
    return 0


if __name__ == "__main__":
    sys.exit(main())
