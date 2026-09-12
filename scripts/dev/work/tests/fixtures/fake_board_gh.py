# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline GitHub CLI stand-in for executing emitted workflow scripts."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def _mutation_allowed() -> bool:
    """Count one mutation and fail on the configured ordinal."""
    state = Path(os.environ["FAKE_GH_STATE"] + ".mutations")
    ordinal = int(state.read_text(encoding="ascii")) + 1 if state.exists() else 1
    state.write_text(str(ordinal), encoding="ascii")
    if ordinal == int(os.environ.get("FAKE_GH_FAIL_MUTATION", "0")):
        print(f"fake mutation failure {ordinal}", file=sys.stderr)
        return False
    return True


def _issue_create() -> int:
    """Record exact argv and return a deterministic issue URL."""
    if not _mutation_allowed():
        return 55
    log = Path(os.environ["FAKE_GH_LOG"])
    with log.open("a", encoding="ascii") as handle:
        json.dump(sys.argv[3:], handle, ensure_ascii=True)
        handle.write("\n")
    state = Path(os.environ["FAKE_GH_STATE"])
    number = int(state.read_text(encoding="ascii")) + 1 if state.exists() else 1
    state.write_text(str(number), encoding="ascii")
    print(f"https://github.com/bsikar/ra8-firmware/issues/{number}")
    return 0


def _labels() -> int:
    """Return the configured label inventory."""
    labels = [
        "priority:P0",
        "epic:inj-epic",
        "area:$(rm -rf /)",
        "needs;review",
        "area:scripts",
    ]
    missing = os.environ.get("FAKE_GH_MISSING_LABEL")
    labels = [label for label in labels if label != missing]
    duplicate = os.environ.get("FAKE_GH_DUP_LABEL")
    if duplicate:
        labels.append(duplicate)
    for label in labels:
        print(json.dumps({"name": label}))
    return 0


def _field_rows() -> None:
    """Print configurable project-field rows."""
    options = {
        "Status": ["Ready"],
        "Track": ["CI health"],
        "Priority": ["P0"],
    }
    for index, (name, base_values) in enumerate(options.items(), start=1):
        values = list(base_values)
        if os.environ.get("FAKE_GH_DUP_OPTION") in values:
            values.append(os.environ["FAKE_GH_DUP_OPTION"])
        field_rows = 2 if os.environ.get("FAKE_GH_DUP_FIELD") == name else 1
        for field_row in range(field_rows):
            payload = {
                "id": f"FIELD{index}{field_row}",
                "name": name,
                "options": [
                    {"id": f"OPTION{index}{position}", "name": value}
                    for position, value in enumerate(values, start=1)
                ],
            }
            print(json.dumps(payload))


def _graphql(args: list[str]) -> int:
    """Return project discovery or mutation fixtures."""
    joined = " ".join(args)
    if "projectV2(number:" in joined:
        if os.environ.get("FAKE_GH_MISSING_PROJECT") == "1":
            print("null")
        else:
            print(
                json.dumps(
                    {
                        "id": "PROJECT",
                        "number": int(os.environ.get("FAKE_GH_PROJECT_NUMBER", "5")),
                        "title": "RA8 firmware",
                        "viewerCanUpdate": os.environ.get("FAKE_GH_PROJECT_UPDATE", "1") == "1",
                    }
                )
            )
    elif "fields(first:" in joined:
        _field_rows()
    elif "addProjectV2ItemById" in joined or "updateProjectV2ItemFieldValue" in joined:
        if not _mutation_allowed():
            return 55
        print("ITEM")
    return 0


def main() -> int:
    """Answer only the command shapes the generated script is allowed to use."""
    args = sys.argv[1:]
    result: int
    if args[:2] == ["auth", "status"]:
        result = 0
    elif args[:2] == ["issue", "create"]:
        result = _issue_create()
    elif args and args[0] == "api" and "graphql" in args:
        result = _graphql(args)
    elif args and args[0] == "api" and any(part.startswith("repos/") for part in args):
        if any(part.startswith("repos/bsikar/ra8-firmware/labels?") for part in args):
            result = _labels()
        elif any(part == "repos/bsikar/ra8-firmware" for part in args):
            print(
                json.dumps(
                    {
                        "full_name": os.environ.get("FAKE_GH_REPOSITORY", "bsikar/ra8-firmware"),
                        "has_issues": os.environ.get("FAKE_GH_ISSUES", "1") == "1",
                        "permissions": {"push": os.environ.get("FAKE_GH_REPO_WRITE", "1") == "1"},
                    }
                )
            )
            result = 0
        else:
            print("CONTENT")
            result = 0
    else:
        print(f"unexpected fake gh argv: {args!r}", file=sys.stderr)
        result = 2
    return result


if __name__ == "__main__":
    raise SystemExit(main())
