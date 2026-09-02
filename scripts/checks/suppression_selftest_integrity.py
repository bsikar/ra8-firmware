# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Identity, ownership, and split-integrity assertions for suppression selftests."""

from __future__ import annotations

import ast
import hashlib
import json
from dataclasses import replace
from pathlib import Path

from selftest_assert import expect
from suppression_model import Inventory
from suppression_scan import scan_paths, validate_fingerprints
from suppression_selftest_fixtures import (
    EXPECTED_FIXTURE_INVENTORY_SHA256,
    EXPECTED_FIXTURE_SHA256,
    FIXTURES,
    fixture_digest,
)
from suppression_validate import deduplicate, validate_cppcheck_anchors


def _assert_foundation_schema(inventory: Inventory, failures: list[str]) -> None:
    """Assert governance placeholders remain present and deliberately unresolved."""
    record = inventory.suppressions[0]
    expect(bool(record.fingerprint), "quiet: stable fingerprint is populated", failures)
    expect(record.match_count == 1, "quiet: match count is explicit", failures)
    expect(record.disposition == "unreviewed", "must fire: disposition is unresolved", failures)
    later = replace(record, line=record.line + 1, fingerprint="")
    expect(
        record.fingerprint != later.fingerprint,
        "quiet: interim foundation identity includes source location",
        failures,
    )
    duplicate = Inventory(suppressions=[record, replace(record)])
    validate_fingerprints(duplicate)
    expect(
        any(item.code == "duplicate-fingerprint" for item in duplicate.findings),
        "must fire: duplicate public fingerprints are rejected",
        failures,
    )
    same_line = Inventory(
        suppressions=[record, replace(record, column=record.column + 1, fingerprint="")]
    )
    deduplicate(same_line)
    expect(
        not any(item.code == "duplicate-directive" for item in same_line.findings),
        "quiet: distinct same-line source columns remain distinct controls",
        failures,
    )
    expect(
        inventory.as_dict()["schema_version"] == "2-durable-site-identity",
        "quiet: schema discloses durable site identity",
        failures,
    )
    hex_width = len(hashlib.sha256(b"").hexdigest())
    expect(
        all(
            len(item.site_id) == hex_width and len(item.binding_sha256) == hex_width
            for item in inventory.suppressions
        ),
        "must fire: every row carries a full-width site and binding identity",
        failures,
    )


def _assert_ownership(inventory: Inventory, failures: list[str]) -> None:
    """Assert canonical evidence handles mixed-owned and generated paths."""
    owners = {item.path: item.owner for item in inventory.suppressions}
    expect(
        owners["port/threadx/vendor.c"] == "vendor",
        "quiet: ThreadX C source is vendor-owned",
        failures,
    )
    expect(
        owners["port/threadx/CMakeLists.txt"] == "first-party",
        "quiet: ThreadX build glue remains first-party",
        failures,
    )
    generated = "libs/ra8_c6link/src/ra8_media_download.pb-c.c"
    expect(
        owners[generated] == "generated",
        "quiet: exact generated source uses canonical PATH_CLASS evidence",
        failures,
    )


def _assert_structural_split(root: Path, failures: list[str]) -> None:
    """Authenticate fixture bytes, scan parity, and module ownership after the split."""
    directory = Path(__file__).parent
    driver = ast.parse((directory / "suppression_selftest.py").read_text(encoding="utf-8"))
    scanner = ast.parse((directory / "suppression_scan.py").read_text(encoding="utf-8"))
    inline = ast.parse((directory / "suppression_inline_scan.py").read_text(encoding="utf-8"))
    assignments = {
        target.id
        for node in ast.walk(driver)
        if isinstance(node, (ast.Assign, ast.AnnAssign))
        for target in (node.targets if isinstance(node, ast.Assign) else [node.target])
        if isinstance(target, ast.Name)
    }
    expect(
        not {"FIXTURES", "CORE_FIXTURES"} & assignments,
        "quiet: fixture data lives outside the selftest driver",
        failures,
    )
    expect(
        fixture_digest(FIXTURES) == EXPECTED_FIXTURE_SHA256,
        "must fire: structural split preserves every fixture byte",
        failures,
    )
    inventory = scan_paths(root, sorted(FIXTURES))
    payload = json.dumps(
        inventory.as_dict(), sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode()
    expect(
        hashlib.sha256(payload).hexdigest() == EXPECTED_FIXTURE_INVENTORY_SHA256,
        "must fire: structural split preserves the exact fixture inventory bytes",
        failures,
    )
    inline_names = {node.name for node in ast.walk(inline) if isinstance(node, ast.FunctionDef)}
    scanner_names = {node.name for node in ast.walk(scanner) if isinstance(node, ast.FunctionDef)}
    expect(
        "_scan_comments" in inline_names and "_scan_comments" not in scanner_names,
        "quiet: inline recognizers live only in the bounded inline-scan module",
        failures,
    )


def _assert_cppcheck_anchor_validation(
    root: Path,
    inventory: Inventory,
    failures: list[str],
    expected_findings: int,
) -> None:
    """Assert literal central-list anchors cannot decay unnoticed."""
    central = next(item for item in inventory.suppressions if item.provenance == "central-list")
    anchor_root = root / "anchor-validation"
    anchor_root.mkdir()
    (anchor_root / "target.c").write_text("one\ntwo\n", encoding="ascii")
    probe = Inventory(
        suppressions=[
            replace(central, scope="target.c:2"),
            replace(central, scope="target.c:3"),
            replace(central, scope="missing.c:1"),
            replace(central, scope="src/*.c:999"),
        ]
    )
    validate_cppcheck_anchors(probe, anchor_root)
    messages = [item.message for item in probe.findings]
    expect(
        len(messages) == expected_findings
        and any("past EOF" in message for message in messages)
        and any("missing source" in message for message in messages),
        "must fire: missing and past-EOF cppcheck anchors are rejected",
        failures,
    )


def assert_identity_and_structure(
    root: Path,
    inventory: Inventory,
    failures: list[str],
    expected_dead_anchor_findings: int,
) -> None:
    """Run the identity, ownership, module-split, and anchor assertions."""
    _assert_structural_split(root, failures)
    _assert_cppcheck_anchor_validation(root, inventory, failures, expected_dead_anchor_findings)
    _assert_foundation_schema(inventory, failures)
    _assert_ownership(inventory, failures)
