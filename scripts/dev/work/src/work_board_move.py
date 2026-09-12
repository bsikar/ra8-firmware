# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

"""Move an issue to a new status on the Kanban board."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from work_git import ToolMissingError, WorkError, run_process
from work_tracker import TrackerSchema, tracker_schema

EXPECTED_ARGC = 3
GH_TIMEOUT_S = 30


def _usage() -> int:
    """Report the fixed two-argument shape."""
    print("Usage: work_board_move.py <issue_number> <status>", file=sys.stderr)
    return 1


def _load_schema() -> TrackerSchema | None:
    """Return the tracker authority, reporting a load failure."""
    try:
        return tracker_schema()
    except WorkError as exc:
        print(f"error loading tracker schema: {exc}", file=sys.stderr)
        return None


def _resolve_status(schema: TrackerSchema, target: str) -> str | None:
    """Return the canonical status spelling, reporting an unknown one."""
    status_map = {status.lower(): status for status in schema.statuses}
    exact = status_map.get(target.lower())
    if exact is None:
        valid = ", ".join(f"'{status}'" for status in schema.statuses)
        print(f"error: invalid status '{target}'. Must be one of: {valid}", file=sys.stderr)
    return exact


def _move_command(schema: TrackerSchema, issue_number: str, status: str) -> list[str]:
    """Build one project item-edit argv for the validated move."""
    issue_url = f"https://{schema.github_host}/{schema.repository}/issues/{issue_number}"
    return [
        "gh",
        "project",
        "item-edit",
        str(schema.project_number),
        "--owner",
        schema.project_owner,
        "--url",
        issue_url,
        "--field",
        "Status",
        "--value",
        status,
    ]


def _run_move(command: list[str]) -> int:
    """Execute one item-edit, reporting transport and payload failures."""
    try:
        proc = run_process(command, timeout=GH_TIMEOUT_S)
    except ToolMissingError:
        print("error: 'gh' CLI is not installed", file=sys.stderr)
        return 1
    except WorkError as exc:
        print(f"error updating board: {exc}", file=sys.stderr)
        return 1
    if proc.ok:
        print("Success!")
        return 0
    message = proc.stderr.strip() or proc.stdout.strip() or f"gh exited {proc.returncode}"
    print(f"error updating board: {message}", file=sys.stderr)
    if "scopes" in message.lower():
        print("\nHint: Your gh token might lack the 'project' scope.", file=sys.stderr)
        print("Run 'gh auth refresh -s project' to update your scopes.", file=sys.stderr)
    return proc.returncode


def main() -> int:
    """Move one issue to a validated board status."""
    if len(sys.argv) != EXPECTED_ARGC:
        return _usage()
    schema = _load_schema()
    if schema is None:
        return 1
    status = _resolve_status(schema, sys.argv[2])
    if status is None:
        return 1
    print(f"Moving #{sys.argv[1]} to [{status}] on the Kanban board...")
    return _run_move(_move_command(schema, sys.argv[1], status))


if __name__ == "__main__":
    sys.exit(main())
