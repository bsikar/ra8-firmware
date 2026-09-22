# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Semantic inventory for named-hardware waivers on canned silent stubs."""

from __future__ import annotations

from pathlib import Path

from check_no_silent_stubs import CANNED_ERRORS, EXCLUDED, ROOTS, real_definitions, scan_text
from suppression_catalog import ownership
from suppression_model import Finding, Suppression


def scan_hardware_todo_controls(
    root: Path, paths: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Bind TODO hardware names only to real CANNED candidates, never SHADOW."""
    rels = [
        rel
        for rel in paths
        if rel.endswith(".c")
        and rel.startswith(tuple(f"{prefix}/" for prefix in ROOTS))
        and not rel.startswith(EXCLUDED)
        and ownership(rel) == "first-party"
    ]
    pairs = [(rel, root / rel) for rel in rels if (root / rel).is_file()]
    files = [item[1] for item in pairs]
    definitions = real_definitions(files)
    records: list[Suppression] = []
    for rel, file_path in pairs:
        for candidate in scan_text(file_path.read_text(errors="replace"), rel):
            if candidate["returns"] not in CANNED_ERRORS or not candidate["waiver"]:
                continue
            shadowed = [
                item
                for item in definitions.get(candidate["name"], [])
                if item["path"] != rel and not item["internal"] and not candidate["internal"]
            ]
            if shadowed:
                continue
            reason = str(candidate["waiver"])
            records.append(
                Suppression(
                    rel,
                    int(candidate["line"]),
                    1,
                    "project-policy",
                    "check-no-silent-stubs",
                    "hardware-blocked-canned-stub",
                    "TODO(named-hardware)",
                    f"function:{candidate['name']}",
                    reason,
                    "semantic-function-binding",
                    ownership(rel),
                    (),
                )
            )
    return records, []
