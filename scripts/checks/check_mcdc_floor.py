#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_mcdc_floor.py -- per-file MC/DC FLOOR gate (no allowlist).

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B" every compound boolean
decision in first-party code must be covered to full Modified
Condition/Decision Coverage. The historical MC/DC gate only looked at the
project-wide TOTAL row (`mcdc_report.sh` + `.github/mcdc-baseline.txt`),
which a well-covered majority can hold above a baseline while an individual
file rots: an aggregate is an average, and averages hide per-file holes.
This gate closes that hole the same way `check_tree_coverage.py` closes it
for line and branch coverage -- it fails CI if ANY first-party file drops below the
floor, and it deliberately has NO allowlist / no per-file exemption table.

Unit of measure: the llvm-cov "MC/DC Decision Region" (one compound `&&` /
`||` decision), the same unit `regen_mcdc_gaps.py` classifies. A file's
score is its *reachable* MC/DC rate:

    reachable_total   = total_decisions - deactivated_decisions
    reachable_covered = decisions at 100% MC/DC
    reachable_pct     = 100 * reachable_covered / reachable_total

Deactivated decisions (DO-178C 6.4.4.3 -- documented as unreachable on any
public-API path, catalogued in docs/MCDC_DEACTIVATIONS.md and driven by an
explicit `// mcdc-deactivated:` annotation or a conservative structural
classifier) are dropped from BOTH numerator and denominator, exactly as the
line-coverage floor drops `gcovr/excluded` lines. This is the deactivation
mechanism, not an allowlist: a decision cannot escape the floor without a
catalogued rationale that the whole tree can audit. A file whose only gaps
are deactivated therefore stays at 100% reachable and passes.

Input: `build/mcdc-report/mcdc_per_file.json`, written by
`regen_mcdc_gaps.py` from the live `make mcdc` report
(`build/mcdc-report/mcdc.txt`). Run the MC/DC build first (which runs the
regenerator), then this. Scope mirrors the line-coverage floor: files under
`libs/` or `src/`, excluding vendored SOUP (`libs/third_party/`) and
generated font tables (`libs/ra8_fonts/`).

Exit 0 if every in-scope file with at least one reachable decision is
>= FLOOR_PCT, else exit 1 with the offenders.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

FLOOR_PCT = 100.0
"""Per-file reachable-MC/DC floor, in percent. DO-178C Level B mandates full
MC/DC of every reachable compound decision; a reachable gap in any single
file is a hard CI failure. Deactivated (documented-unreachable) decisions are
excluded per DO-178C 6.4.4.3, so 100% is the honest bar the tree meets today.
No allowlist: a file below this is fixed at the root (a real MC/DC vector, or
a catalogued `// mcdc-deactivated:` rationale), never grandfathered."""

REPO_ROOT = Path(__file__).resolve().parents[2]
MCDC_JSON = REPO_ROOT / "build" / "mcdc-report" / "mcdc_per_file.json"

IN_SCOPE_PREFIXES = ("libs/", "src/")
"""First-party source roots held to the floor (mirrors the line-coverage floor)."""

OUT_OF_SCOPE_PREFIXES = ("libs/third_party/", "libs/ra8_fonts/")
"""Vendored SOUP and generated font tables -- exempt from first-party rules
per the CLAUDE.md coding-standards scope, so exempt from the floor too."""


def normalize(path: str) -> str:
    """Normalise a coverage-JSON ``file`` field to a repo-relative POSIX path.

    The field may arrive absolute or already relative, so both are handled.

    The absolute-path split marker is derived from the checkout directory
    basename (`REPO_ROOT.name`, itself resolved from this file's location)
    rather than a hardcoded project name, so the gate strips the prefix
    correctly from any clone regardless of what the repo directory is named.
    """
    p = path.replace("\\", "/")
    marker = "/" + REPO_ROOT.name + "/"
    if marker in p:
        p = p.split(marker, 1)[1]
    return p.lstrip("./")


def in_scope(rel: str) -> bool:
    """True if `rel` is a first-party file subject to the MC/DC floor."""
    if not rel.startswith(IN_SCOPE_PREFIXES):
        return False
    return not rel.startswith(OUT_OF_SCOPE_PREFIXES)


def file_reachable(entry: dict) -> tuple[int, int]:
    """Return (reachable_covered, reachable_total) decisions for one file.

    Deactivated decisions (DO-178C 6.4.4.3) are removed from the denominator;
    covered decisions are never deactivated, so the numerator is just the
    count of decisions at 100% MC/DC.
    """
    total = int(entry.get("total_decisions", 0))
    covered = int(entry.get("covered_decisions", 0))
    deactivated = int(entry.get("deactivated_decisions", 0))
    reachable_total = total - deactivated
    return covered, reachable_total


def _collect_offenders(
    files: list[dict],
) -> tuple[list[tuple[float, str, int, int]], int]:
    """Return ``(offenders, checked)`` for every in-scope file with a decision.

    A file with no reachable decision is skipped rather than counted as a pass:
    it has nothing to measure, and scoring it 100% would dilute the floor.
    """
    offenders: list[tuple[float, str, int, int]] = []
    checked = 0
    for entry in files:
        rel = normalize(entry.get("file", ""))
        if not in_scope(rel):
            continue
        covered, reachable_total = file_reachable(entry)
        if reachable_total <= 0:
            continue
        checked += 1
        pct = 100.0 * covered / reachable_total
        if pct < FLOOR_PCT:
            offenders.append((pct, rel, covered, reachable_total))
    return offenders, checked


def _report_offenders(offenders: list[tuple[float, str, int, int]]) -> None:
    """Print the below-floor table, worst first, with the remedy."""
    offenders.sort()
    print(
        f"check_mcdc_floor.py: {len(offenders)} first-party file(s) below "
        f"the {FLOOR_PCT:.0f}% reachable-MC/DC floor (NO allowlist):"
    )
    print("  mc/dc  covered/reachable  file")
    for pct, rel, covered, reachable_total in offenders:
        print(f"  {pct:5.1f}%  {covered:5d}/{reachable_total:<5d}       {rel}")
    print(
        "Fix each at the root -- add the missing MC/DC vector (N+1 vectors "
        "for N conditions; see docs/MCDC.md), or, if the gap is genuinely "
        "unreachable on any public-API path, catalogue it with a "
        "`// mcdc-deactivated:` rationale per DO-178C 6.4.4.3. Do NOT add "
        "an allowlist."
    )


def main() -> int:
    """Fail when any in-scope file sits below the reachable MC/DC floor.

    The floor is REACHABLE coverage, not absolute: decisions classified as
    deactivated are excluded, so the number this gates is what tests could
    actually cover rather than a figure no test can ever reach.

    A missing JSON report exits 1 rather than passing vacuously.
    """
    if not MCDC_JSON.is_file():
        print(
            f"check_mcdc_floor.py: ERROR -- {MCDC_JSON} not found; "
            f"run `bash scripts/report/mcdc_report.sh` first."
        )
        return 1

    try:
        data = json.loads(MCDC_JSON.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_mcdc_floor.py: ERROR -- cannot read MC/DC JSON: {exc}")
        return 1

    files = data.get("files", [])
    if not files:
        print("check_mcdc_floor.py: ERROR -- MC/DC JSON has no files.")
        return 1

    offenders, checked = _collect_offenders(files)

    if checked == 0:
        print(
            "check_mcdc_floor.py: ERROR -- no in-scope files matched; check the JSON path / scope."
        )
        return 1

    if offenders:
        _report_offenders(offenders)
        return 1

    print(
        f"check_mcdc_floor.py: PASS -- all {checked} first-party file(s) "
        f"with a reachable decision are >= {FLOOR_PCT:.0f}% MC/DC."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
