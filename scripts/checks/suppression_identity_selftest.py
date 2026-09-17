# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction fixtures for the durable site/binding identity."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from selftest_assert import expect
from suppression_identity import assign_identities
from suppression_model import Inventory
from suppression_scan import scan_paths


def _rows(root: Path, files: dict[str, str]) -> dict[tuple[str, int], object]:
    """Scan an explicit fixture tree and index rows by path and line."""
    for rel, text in files.items():
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="ascii")
    inventory = scan_paths(root, sorted(files))
    return {(item.path, item.line): item for item in inventory.suppressions}


def _fixture(marker_a: str, marker_b: str, prefix: str = "", indent: str = "") -> str:
    """Compose one two-marker shell fixture with optional prefix lines."""
    return (
        f"{prefix}#!/bin/sh\n"
        f"{indent}probe || true  # {marker_a}\n"
        "echo steady\n"
        f"other || true  # {marker_b}\n"
    )


def assert_identity_semantics(base: Path, failures: list[str]) -> None:
    """Assert site identity survives movement and binding tracks content."""
    left = base / "identity-a"
    right = base / "identity-b"
    files = {"tool.sh": _fixture("cleanup best effort", "second reason")}
    moved = {"tool.sh": _fixture("cleanup best effort", "second reason", prefix="# banner\n")}
    rows_a = _rows(left, files)
    rows_b = _rows(right, moved)
    first_a = rows_a[("tool.sh", 2)]
    first_b = rows_b[("tool.sh", 3)]
    expect(
        bool(first_a.site_id) and first_a.site_id == first_b.site_id,
        "quiet: a line inserted above a site preserves its site_id",
        failures,
    )
    expect(
        first_a.binding_sha256 == first_b.binding_sha256,
        "quiet: a line inserted above a site preserves its binding",
        failures,
    )
    indented = _rows(
        base / "identity-c",
        {"tool.sh": _fixture("cleanup best effort", "second reason", indent="   ")},
    )
    expect(
        indented[("tool.sh", 2)].site_id == first_a.site_id,
        "quiet: indentation-only movement preserves site identity",
        failures,
    )
    _assert_identity_content(base, first_a, failures)


def _assert_identity_content(base: Path, first_a: object, failures: list[str]) -> None:
    """Assert reason edits preserve sites while construct edits rename them."""
    reworded = _rows(
        base / "identity-d", {"tool.sh": _fixture("a different rationale", "second reason")}
    )
    changed = reworded[("tool.sh", 2)]
    expect(
        changed.site_id == first_a.site_id and changed.binding_sha256 != first_a.binding_sha256,
        "quiet: reason-only changes preserve the site and invalidate the binding",
        failures,
    )
    reflowed = _rows(
        base / "identity-reflow",
        {"tool.sh": "#!/bin/sh\n  probe || true # reformatted rationale\n"},
    )[("tool.sh", 2)]
    expect(
        reflowed.site_id == first_a.site_id,
        "quiet: reason and whitespace reflow preserve site identity",
        failures,
    )
    rewritten = _rows(
        base / "identity-e",
        {"tool.sh": "#!/bin/sh\nrewritten_probe || true  # cleanup best effort\n"},
    )
    expect(
        rewritten[("tool.sh", 2)].site_id != first_a.site_id,
        "must fire: changing the suppressed construct renames the site",
        failures,
    )
    _assert_scope_and_directive_rekey(base / "identity-a", first_a, failures)
    _assert_repeated_identity(base, failures)


def _assert_scope_and_directive_rekey(root: Path, original: object, failures: list[str]) -> None:
    """Assert approved identity dimensions other than reason still re-key."""
    variants = [
        replace(original, scope="changed-scope", site_id="", binding_sha256="", anchor=""),
        replace(original, directive="changed-directive", site_id="", binding_sha256="", anchor=""),
    ]
    inventory = Inventory(suppressions=variants)
    assign_identities(inventory, root)
    expect(
        all(item.site_id != original.site_id for item in inventory.suppressions),
        "must fire: scope and directive changes rename the site",
        failures,
    )


def _assert_repeated_identity(base: Path, failures: list[str]) -> None:
    """Assert ordinals separate repeats without making order semantic."""
    repeated = _rows(
        base / "identity-f",
        {"tool.sh": "#!/bin/sh\nprobe || true  # same\nprobe || true  # same\n"},
    )
    ids = {item.site_id for item in repeated.values()}
    expect(
        len(repeated) > 1 and len(ids) == len(repeated),
        "must fire: repeated identical directives stay individually represented",
        failures,
    )
    swapped_one = _rows(
        base / "identity-g",
        {"tool.sh": "#!/bin/sh\nalpha || true  # one\nbeta || true  # two\n"},
    )
    swapped_two = _rows(
        base / "identity-h",
        {"tool.sh": "#!/bin/sh\nbeta || true  # two\nalpha || true  # one\n"},
    )
    expect(
        {item.site_id for item in swapped_one.values()}
        == {item.site_id for item in swapped_two.values()},
        "quiet: reordering distinct sites preserves the identity set",
        failures,
    )
