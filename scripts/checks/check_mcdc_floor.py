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
`regen_mcdc_gaps.py` from the live `just quality::local::mcdc` report
(`build/mcdc-report/mcdc.txt`). Run the MC/DC build first (which runs the
regenerator), then this. Scope covers every first-party production root the
report contains: `libs/`, `apps/shared_libs/`, `examples/`, `port/`, and
`tools/`. Vendored SOUP, generated font tables, nested test suites, and build
outputs are excluded. Each production root must contribute at least one
reachable decision so a missing report subtree cannot pass vacuously.

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

IN_SCOPE_PREFIXES = ("libs/", "apps/shared_libs/", "examples/", "port/", "tools/")
"""First-party production roots represented in the live MC/DC report."""

OUT_OF_SCOPE_PREFIXES = ("libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/")
"""Vendored SOUP and generated font tables -- exempt from first-party rules
per the CLAUDE.md coding-standards scope, so exempt from the floor too."""

OUT_OF_SCOPE_PARTS = frozenset({"tests", "test", "_deps"})
"""Nested test suites and dependency-fetch output directory names."""


def is_generated_part(part: str) -> bool:
    """True for a CMake/build output directory component."""
    return part == "build" or part.startswith("build-")


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
    if rel.startswith(OUT_OF_SCOPE_PREFIXES):
        return False
    parts = Path(rel).parts
    if OUT_OF_SCOPE_PARTS.intersection(parts):
        return False
    return not any(is_generated_part(part) for part in parts)


def scope_prefix(rel: str) -> str | None:
    """Return the first-party scope root for ``rel``, or ``None`` if exempt."""
    if not in_scope(rel):
        return None
    return next(prefix for prefix in IN_SCOPE_PREFIXES if rel.startswith(prefix))


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
) -> tuple[list[tuple[float, str, int, int]], dict[str, int]]:
    """Return offenders and per-root counts for in-scope decision-bearing files.

    A file with no reachable decision is skipped rather than counted as a pass:
    it has nothing to measure, and scoring it 100% would dilute the floor.
    """
    offenders: list[tuple[float, str, int, int]] = []
    checked_by_scope = dict.fromkeys(IN_SCOPE_PREFIXES, 0)
    for entry in files:
        rel = normalize(entry.get("file", ""))
        prefix = scope_prefix(rel)
        if prefix is None:
            continue
        covered, reachable_total = file_reachable(entry)
        if reachable_total <= 0:
            continue
        checked_by_scope[prefix] += 1
        pct = 100.0 * covered / reachable_total
        if pct < FLOOR_PCT:
            offenders.append((pct, rel, covered, reachable_total))
    return offenders, checked_by_scope


def _missing_scopes(checked_by_scope: dict[str, int]) -> list[str]:
    """Return required first-party roots absent from a coverage report."""
    return [prefix for prefix in IN_SCOPE_PREFIXES if checked_by_scope.get(prefix, 0) == 0]


def _entry(path: str, covered: int = 1, total: int = 1) -> dict:
    """Build a minimal per-file fixture for the embedded scope self-test."""
    return {"file": path, "covered_decisions": covered, "total_decisions": total}


def selftest() -> int:
    """Prove every production root is required and every exemption stays out."""
    failures: list[str] = []
    scope_cases = {
        "libs/ra8_core/src/core.c": True,
        "apps/shared_libs/book/src/book.c": True,
        "examples/ek_ra8d2/demo/src/main.c": True,
        "port/posix/src/io.c": True,
        "tools/ra8_emulator/src/main.c": True,
        "libs/third_party/soup.c": False,
        "apps/shared_libs/third_party/soup/source.c": False,
        "libs/ra8_fonts/src/generated.c": False,
        "apps/shared_libs/book/tests/src/test_book.c": False,
        "examples/ek_ra8d2/demo/build/generated.c": False,
        "examples/ek_ra8d2/demo/build-reflow-v2/generated.c": False,
        "port/esp-hosted/build-mcdc/shim.c": False,
        "tools/demo/_deps/vendor.c": False,
        "src/legacy.c": False,
    }
    for path, expected in scope_cases.items():
        if in_scope(path) is not expected:
            failures.append(f"scope mismatch for {path}: expected {expected}")

    covered = [
        _entry("libs/ra8_core/src/core.c"),
        _entry("apps/shared_libs/book/src/book.c"),
        _entry("examples/ek_ra8d2/demo/src/main.c"),
        _entry("port/posix/src/io.c"),
        _entry("tools/ra8_emulator/src/main.c"),
    ]
    offenders, counts = _collect_offenders(covered)
    if offenders or counts != dict.fromkeys(IN_SCOPE_PREFIXES, 1):
        failures.append("covered fixtures did not populate every required production root")

    below_floor = [
        _entry(entry["file"], 0, 1) if entry["file"].startswith("port/") else entry
        for entry in covered
    ]
    offenders, _counts = _collect_offenders(below_floor)
    if len(offenders) != 1 or offenders[0][1] != "port/posix/src/io.c":
        failures.append("below-floor production fixture did not become an offender")

    missing_tools = [entry for entry in covered if not entry["file"].startswith("tools/")]
    offenders, counts = _collect_offenders(missing_tools)
    if offenders or _missing_scopes(counts) != ["tools/"]:
        failures.append("a missing production root did not fail its non-vacuity check")

    vendor_only = [
        _entry("libs/third_party/soup.c", 0),
        _entry("apps/shared_libs/third_party/soup.c", 0),
    ]
    offenders, counts = _collect_offenders(vendor_only)
    if offenders or _missing_scopes(counts) != list(IN_SCOPE_PREFIXES):
        failures.append("exempt-only input did not fail every production non-vacuity check")

    if failures:
        for failure in failures:
            print(f"check_mcdc_floor.py selftest: FAIL -- {failure}")
        return 1
    print("check_mcdc_floor.py selftest: PASS -- scope and non-vacuity checks hold.")
    return 0


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

    offenders, checked_by_scope = _collect_offenders(files)

    missing_scopes = _missing_scopes(checked_by_scope)
    if missing_scopes:
        print(
            "check_mcdc_floor.py: ERROR -- no reachable decisions matched required "
            f"scope(s): {', '.join(missing_scopes)}; check the JSON path / scope."
        )
        return 1

    if offenders:
        _report_offenders(offenders)
        return 1

    checked = sum(checked_by_scope.values())
    print(
        f"check_mcdc_floor.py: PASS -- all {checked} first-party file(s) "
        f"with a reachable decision are >= {FLOOR_PCT:.0f}% MC/DC."
    )
    return 0


if __name__ == "__main__":
    sys.exit(selftest() if sys.argv[1:] == ["--selftest"] else main())
