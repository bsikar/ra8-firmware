#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Ratchet media_dl line/branch coverage without exempting existing files.

The firmware host suite can enforce an absolute 90% line / 80% branch floor
because it entered CI with that coverage. media_dl did not. Its pre-existing
debt is frozen per file as covered/total counts; a file may not gain uncovered
lines or branches, and its coverage ratio may not fall. A new source file has
no historical debt and must enter at the full 90/80 bar.

This is stricter than an aggregate percentage: well-tested files cannot hide
an untested file, and a zero-coverage file cannot grow while remaining at the
same misleading 0%. Missing report files and stale baseline rows fail loudly.

media_dl is ONE product in TWO directories: its portable core at
``apps/shared/media_dl`` (which a second build form will also consume) and its
host composition root at ``apps/stand_alone/media_dl``. Both are production
media_dl code and both are ratcheted here. SOURCE_DIRS is the single list that
says so -- the report filter, the "which sources does the report owe us" scan
and the stale-row check all derive from it, so a root cannot be added to some
of them and silently omitted from the others. A root left out would not fail:
its files would simply stop being expected, which reads as a clean tree.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
COVERAGE_JSON = REPO_ROOT / "build" / "tool-coverage" / "media_dl" / "coverage.json"
BASELINE_FILE = REPO_ROOT / ".github" / "tool-coverage-baseline.txt"
PRODUCT_DIRS = ("apps/shared/media_dl", "apps/stand_alone/media_dl")
SOURCE_DIRS = tuple(REPO_ROOT / rel / "src" for rel in PRODUCT_DIRS)
SOURCE_PREFIXES = tuple(f"{rel}/src/" for rel in PRODUCT_DIRS)
TRACKED_INLINE_SOURCES = {"apps/shared/media_dl/src/mdl_extract_internal.h"}

LINE_FLOOR_PCT = 90
BRANCH_FLOOR_PCT = 80
BASELINE_COLUMNS = 5

Coverage = tuple[int, int, int, int]


def normalize(path: str) -> str:
    """Return a repo-relative POSIX path from a gcovr filename."""
    value = path.replace("\\", "/")
    for rel in PRODUCT_DIRS:
        marker = f"/{rel}/"
        if marker in value:
            return rel + "/" + value.split(marker, 1)[1]
    return value.lstrip("./")


def load_report(path: Path) -> dict[str, Coverage]:
    """Load gcovr summary JSON into per-file integer coverage counts."""
    data = json.loads(path.read_text(encoding="utf-8"))
    rows: dict[str, Coverage] = {}
    for entry in data.get("files", []):
        rel = normalize(str(entry.get("filename", "")))
        if not rel.startswith(SOURCE_PREFIXES):
            continue
        rows[rel] = (
            int(entry["line_covered"]),
            int(entry["line_total"]),
            int(entry["branch_covered"]),
            int(entry["branch_total"]),
        )
    return rows


def load_baseline(path: Path) -> dict[str, Coverage]:
    """Load the committed per-file coverage debt."""
    rows: dict[str, Coverage] = {}
    for raw in path.read_text(encoding="ascii").splitlines():
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != BASELINE_COLUMNS:
            message = f"malformed baseline row: {raw!r}"
            raise ValueError(message)
        rel, line_cov, line_total, branch_cov, branch_total = fields
        rows[rel] = (int(line_cov), int(line_total), int(branch_cov), int(branch_total))
    return rows


def expected_sources(roots: tuple[Path, ...] = SOURCE_DIRS) -> set[str]:
    """Return every instrumented production unit the coverage report owes."""
    sources: set[str] = set()
    for root, prefix in zip(roots, SOURCE_PREFIXES, strict=False):
        sources.update(f"{prefix}{path.name}" for path in root.glob("*.c"))
    if roots == SOURCE_DIRS:
        sources.update(TRACKED_INLINE_SOURCES)
    return sources


def _below_floor(covered: int, total: int, floor: int) -> bool:
    """Return whether an integer covered/total ratio is below ``floor`` percent."""
    return total > 0 and (covered * 100) < (floor * total)


def _metric_regressions(
    rel: str,
    label: str,
    current: tuple[int, int],
    frozen: tuple[int, int],
) -> list[str]:
    """Report uncovered-debt growth or a ratio drop for one metric."""
    covered, total = current
    base_covered, base_total = frozen
    failures: list[str] = []
    if (total - covered) > (base_total - base_covered):
        failures.append(
            f"{rel}: uncovered {label} debt grew {base_total - base_covered} -> {total - covered}"
        )
    if base_total > 0 and covered * base_total < base_covered * total:
        failures.append(
            f"{rel}: {label} ratio regressed {base_covered}/{base_total} -> {covered}/{total}"
        )
    if base_total == 0 and _below_floor(
        covered, total, LINE_FLOOR_PCT if label == "line" else BRANCH_FLOOR_PCT
    ):
        failures.append(f"{rel}: new {label} metric entered below the absolute floor")
    return failures


def evaluate(
    current: dict[str, Coverage], baseline: dict[str, Coverage], expected: set[str]
) -> list[str]:
    """Return every scope, absolute-floor, and ratchet violation."""
    current_files = set(current)
    failures = [
        f"{rel}: source is missing from the coverage report" for rel in expected - current_files
    ]
    failures.extend(f"{rel}: stale baseline row" for rel in set(baseline) - current_files)

    for rel in sorted(current):
        line_cov, line_total, branch_cov, branch_total = current[rel]
        if rel not in expected:
            failures.append(f"{rel}: report contains an unexpected production source")
            continue
        if line_total == 0:
            failures.append(f"{rel}: report contains zero instrumented lines")
            continue
        if rel not in baseline:
            if _below_floor(line_cov, line_total, LINE_FLOOR_PCT):
                failures.append(f"{rel}: new file is below {LINE_FLOOR_PCT}% line coverage")
            if _below_floor(branch_cov, branch_total, BRANCH_FLOOR_PCT):
                failures.append(f"{rel}: new file is below {BRANCH_FLOOR_PCT}% branch coverage")
            continue
        base_line_cov, base_line_total, base_branch_cov, base_branch_total = baseline[rel]
        failures.extend(
            _metric_regressions(
                rel, "line", (line_cov, line_total), (base_line_cov, base_line_total)
            )
        )
        failures.extend(
            _metric_regressions(
                rel,
                "branch",
                (branch_cov, branch_total),
                (base_branch_cov, base_branch_total),
            )
        )
    return sorted(failures)


#: Frozen debt the selftest cases are measured against: one file in the shared
#: core and one in the host form, so a ratchet that stopped covering either
#: product root is caught by a case rather than by nobody.
SELFTEST_FROZEN: dict[str, Coverage] = {
    "apps/shared/media_dl/src/frozen.c": (90, 100, 80, 100),
    "apps/stand_alone/media_dl/src/frozen.c": (90, 100, 80, 100),
}


def _selftest_cases() -> list[tuple[str, dict[str, Coverage], set[str], bool]]:
    """Return every both-direction case: (name, report, expected, should_fire)."""
    frozen = SELFTEST_FROZEN
    expected = set(frozen)
    return [
        ("exact frozen coverage stays quiet", frozen, expected, False),
        (
            "uncovered line debt growth fires in the shared core",
            {**frozen, "apps/shared/media_dl/src/frozen.c": (90, 101, 80, 100)},
            expected,
            True,
        ),
        (
            "branch ratio regression fires in the host form",
            {**frozen, "apps/stand_alone/media_dl/src/frozen.c": (90, 100, 79, 100)},
            expected,
            True,
        ),
        (
            "well-covered new file stays quiet",
            {**frozen, "apps/shared/media_dl/src/new.c": (9, 10, 8, 10)},
            expected | {"apps/shared/media_dl/src/new.c"},
            False,
        ),
        (
            "poorly-covered new file fires",
            {**frozen, "apps/stand_alone/media_dl/src/new.c": (8, 10, 7, 10)},
            expected | {"apps/stand_alone/media_dl/src/new.c"},
            True,
        ),
        ("missing report source fires", {}, expected, True),
        (
            "zero-line report entry fires",
            {**frozen, "apps/shared/media_dl/src/frozen.c": (0, 0, 0, 0)},
            expected,
            True,
        ),
        (
            "missing tracked inline source fires",
            frozen,
            expected | TRACKED_INLINE_SOURCES,
            True,
        ),
        (
            "a report covering only one product root fires",
            {"apps/stand_alone/media_dl/src/frozen.c": (90, 100, 80, 100)},
            expected,
            True,
        ),
    ]


def selftest() -> int:
    """Prove the ratchet fires and stays quiet in both directions."""
    cases = _selftest_cases()
    failures = [
        name
        for name, current, case_expected, should_fire in cases
        if bool(evaluate(current, SELFTEST_FROZEN, case_expected)) != should_fire
    ]
    if failures:
        for name in failures:
            print(f"check_tool_coverage.py --selftest: FAIL: {name}", file=sys.stderr)
        return 1
    print(f"check_tool_coverage.py --selftest: PASS ({len(cases)} both-direction cases)")
    return 0


def write_baseline(path: Path, rows: dict[str, Coverage]) -> None:
    """Write the current per-file debt after a non-regressing measurement."""
    lines = [
        "# media_dl coverage ratchet: covered/total counts per production source.",
        "# New files must enter at >=90% line and >=80% branch coverage.",
        "# Existing files may neither gain uncovered debt nor lose coverage ratio.",
        "# columns: file<TAB>line-covered<TAB>line-total<TAB>branch-covered<TAB>branch-total",
        f"# rows: {len(rows)}",
        "",
    ]
    lines.extend(
        f"{rel}\t{values[0]}\t{values[1]}\t{values[2]}\t{values[3]}"
        for rel, values in sorted(rows.items())
    )
    path.write_text("\n".join([*lines, ""]), encoding="ascii")


def run_gate(update: bool) -> int:
    """Check the real report, optionally tightening the committed baseline."""
    if not COVERAGE_JSON.is_file():
        print(f"check_tool_coverage.py: missing report: {COVERAGE_JSON}", file=sys.stderr)
        return 2
    current = load_report(COVERAGE_JSON)
    if not current:
        print("check_tool_coverage.py: coverage report contains zero tool files", file=sys.stderr)
        return 2
    baseline = load_baseline(BASELINE_FILE) if BASELINE_FILE.is_file() else {}
    initial = not BASELINE_FILE.is_file()
    failures = [] if initial else evaluate(current, baseline, expected_sources())
    if failures:
        print("check_tool_coverage.py: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    if update:
        write_baseline(BASELINE_FILE, current)
        print(f"check_tool_coverage.py: updated {BASELINE_FILE} ({len(current)} files)")
        return 0
    if initial:
        print("check_tool_coverage.py: baseline is missing", file=sys.stderr)
        return 2
    print(f"check_tool_coverage.py: PASS ({len(current)} production files, no regression)")
    return 0


def main() -> int:
    """Run the selftest, coverage gate, or shrink-only baseline update."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()
    return selftest() if args.selftest else run_gate(args.update)


if __name__ == "__main__":
    sys.exit(main())
