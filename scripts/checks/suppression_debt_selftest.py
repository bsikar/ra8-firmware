# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Adversarial fixtures for immutable suppression-debt ceilings."""

from __future__ import annotations

from pathlib import Path

from selftest_assert import expect
from suppression_baseline_scan import ceiling_snapshot, scan_baseline_repository
from suppression_model import Suppression

TIDY = ".github/tidy-baseline.txt"
COMPOUND = ".github/mcdc-compound-baseline.txt"
TREE = ".github/tree-coverage-baseline.txt"
TIDY_HEADER = (
    "# clang-tidy ratchet baseline -- per-file-per-check finding counts.\n"
    "# Consumed by scripts/checks/tidy_ratchet.py --check (CI gate: tidy).\n"
)
COMPOUND_HEADER = (
    "# MC/DC compound-decision ratchet baseline -- per-file-per-function counts.\n"
    "# Consumed by scripts/checks/mcdc_compound_ratchet.py --check\n"
    "# Total at this baseline: 1 uncovered compound decision\n"
)
TREE_HEADER = (
    "# ONE coverage baseline for every first-party translation unit.\n"
    "# Emitted by `python3 scripts/checks/check_tree_coverage.py --update`.\n"
)


def _write_probe(root: Path, rel: str, text: str = "") -> None:
    """Write one ASCII baseline-integrity probe file."""
    target = root / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding="ascii")


def _scan_probe(
    root: Path, paths: list[str], ledger: dict[str, tuple[str, str]] | None = None
) -> tuple[list[Suppression], set[str]]:
    """Run the production baseline scanner and return rows plus finding codes."""
    rows, findings = scan_baseline_repository(
        root,
        paths,
        enforce_floors=False,
        ceiling_ledger=ledger,
    )
    return rows, {item.code for item in findings}


def _assert_key_swaps(root: Path, failures: list[str]) -> None:
    """Assert debt cannot move within or between baseline authorities."""
    original_tidy = TIDY_HEADER + "libs/a.c\tmisc-a\t1\nlibs/b.c\tmisc-b\t4\n"
    original_compound = COMPOUND_HEADER + "libs/c.c\tfn_c\t1\n"
    _write_probe(root, TIDY, original_tidy)
    _write_probe(root, COMPOUND, original_compound)
    rows, original_codes = _scan_probe(root, [TIDY, COMPOUND])
    ledger = ceiling_snapshot(rows)
    _rows, frozen_codes = _scan_probe(root, [TIDY, COMPOUND], ledger)
    expect(
        not original_codes and not frozen_codes,
        "quiet: unchanged canonical per-key ceilings validate",
        failures,
    )
    changed = TIDY_HEADER + "libs/a.c\tmisc-new\t2\nlibs/b.c\tmisc-b\t3\n"
    _write_probe(root, TIDY, changed)
    _rows, within_codes = _scan_probe(root, [TIDY, COMPOUND], ledger)
    expect(
        "baseline-growth" in within_codes,
        "must fire: same-total within-baseline bucket swaps cannot absorb debt",
        failures,
    )
    changed = TIDY_HEADER + "libs/c.c\tfn_c\t1\nlibs/b.c\tmisc-b\t4\n"
    _write_probe(root, TIDY, changed)
    _write_probe(root, COMPOUND, COMPOUND_HEADER + "libs/a.c\tmisc-a\t1\n")
    _rows, swap_codes = _scan_probe(root, [TIDY, COMPOUND], ledger)
    expect(
        "baseline-growth" in swap_codes,
        "must fire: equal-count cross-baseline bucket swaps cannot absorb debt",
        failures,
    )


def _assert_path_aliases(root: Path, failures: list[str]) -> None:
    """Assert noncanonical spellings cannot mint alternate debt keys."""
    for alias in ("./libs/a.c", "libs\\a.c", "."):
        _write_probe(root, TIDY, TIDY_HEADER + f"{alias}\tmisc-a\t1\n")
        _rows, alias_codes = _scan_probe(root, [TIDY])
        expect(
            "malformed-baseline-row" in alias_codes,
            f"must fire: noncanonical path alias {alias!r} is rejected",
            failures,
        )


def _assert_tree_controls(root: Path, failures: list[str]) -> None:
    """Assert tree-coverage dimensions and path identity stay independent."""
    _write_probe(
        root,
        TREE,
        TREE_HEADER + "# rows: 1\nlibs/a.c\tMEASURED\t90\t100\t70\t80\n",
    )
    tree_rows, tree_original_codes = _scan_probe(root, [TREE])
    tree_ledger = ceiling_snapshot(tree_rows)
    _write_probe(
        root,
        TREE,
        TREE_HEADER + "# rows: 1\nlibs/a.c\tMEASURED\t89\t100\t71\t80\n",
    )
    _rows, tree_swap_codes = _scan_probe(root, [TREE], tree_ledger)
    expect(
        not tree_original_codes and "baseline-growth" in tree_swap_codes,
        "must fire: tree line debt cannot trade against branch burn-down",
        failures,
    )
    _write_probe(
        root,
        TREE,
        TREE_HEADER
        + "# rows: 2\n"
        + "libs/a.c\tMEASURED\t1\t2\t1\t1\n"
        + "libs/a.c\tUNMEASURED\tplatform-cross-only\n",
    )
    _rows, duplicate_codes = _scan_probe(root, [TREE])
    expect(
        "duplicate-baseline-row" in duplicate_codes,
        "must fire: tree coverage MEASURED/UNMEASURED share one path identity",
        failures,
    )


def _assert_authority_controls(root: Path, failures: list[str]) -> None:
    """Assert missing provenance and invented authorities fail closed."""
    _write_probe(root, TIDY, "# header removed\nlibs/a.c\tmisc-a\t1\n")
    _write_probe(root, ".github/future-ratchet.txt", "libs/a.c\tmisc-a\t1\n")
    _rows, authority_codes = _scan_probe(root, [TIDY, ".github/future-ratchet.txt"])
    expect(
        {"missing-baseline-provenance", "unknown-baseline-file"} <= authority_codes,
        "must fire: provenance removal and noncanonical ratchet authorities fail closed",
        failures,
    )


def assert_baseline_ceiling_controls(base: Path, failures: list[str]) -> None:
    """Assert canonical per-key ceilings reject swaps, aliases, and authority drift."""
    root = base / "baseline-ceiling-probe"
    for rel in (
        "scripts/checks/tidy_ratchet.py",
        "scripts/checks/mcdc_compound_ratchet.py",
        "scripts/checks/check_tree_coverage.py",
        "libs/a.c",
        "libs/b.c",
        "libs/c.c",
    ):
        _write_probe(root, rel)
    _assert_key_swaps(root, failures)
    _assert_path_aliases(root, failures)
    _assert_tree_controls(root, failures)
    _assert_authority_controls(root, failures)
