#!/usr/bin/env python3
"""check_coverage_floor.py -- per-file line-coverage FLOOR gate (no allowlist).

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B" and the project's
"no grandfather" coverage policy: EVERY first-party translation unit must
carry at least `FLOOR_PCT` line coverage on its own. The aggregate gate
(`scripts/checks/coverage.sh --gate`, gcovr --fail-under-line 90) can be satisfied
while an individual file rots far below the bar, because well-covered files
average it up. This gate closes that hole: it fails CI if ANY first-party
file drops below the floor, and it deliberately has NO allowlist / no
per-file exemption table -- if a file cannot reach the floor it must be
fixed at the root (a real test, or a documented GCOVR_EXCL on a line no host
input can reach), never grandfathered.

Input: the gcovr JSON `build/coverage/coverage/coverage.json` produced by
`scripts/checks/coverage.sh` (its `[4/4] Running gcovr` step writes `--json`). Run
that first, then this. Scope mirrors the aggregate gate: files under `libs/`
or `src/`, excluding vendored SOUP (`libs/third_party/`) and generated font
tables (`libs/fonts/`), which the coding-standards scope explicitly exempts.

Exit 0 if every in-scope file is >= FLOOR_PCT, else exit 1 with the offenders.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

FLOOR_PCT = 90.0
"""Per-file line-coverage floor, in percent. Matches the aggregate line gate
(gcovr --fail-under-line 90) but applied to EACH first-party file, not the
mean. No allowlist: a file below this is a hard CI failure."""

REPO_ROOT = Path(__file__).resolve().parents[2]
COVERAGE_JSON = REPO_ROOT / "build" / "coverage" / "coverage" / "coverage.json"

IN_SCOPE_PREFIXES = ("libs/", "src/")
"""First-party source roots held to the floor."""

OUT_OF_SCOPE_PREFIXES = ("libs/third_party/", "libs/fonts/")
"""Vendored SOUP and generated font tables -- exempt from first-party rules
per the CLAUDE.md coding-standards scope, so exempt from the floor too."""


def normalize(path: str) -> str:
    """Normalise a gcovr ``file`` field to a repo-root-relative POSIX path.

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
    """True if `rel` is a first-party file subject to the coverage floor."""
    if not rel.startswith(IN_SCOPE_PREFIXES):
        return False
    return not rel.startswith(OUT_OF_SCOPE_PREFIXES)


def file_line_pct(entry: dict) -> tuple[int, int]:
    """Return (covered, total) *countable* lines for one gcovr file entry.

    Lines carrying `gcovr/excluded: true` (a `GCOVR_EXCL` marker on a line no
    host input can reach) are dropped from BOTH the numerator and denominator,
    exactly as gcovr's own `line_total` does -- so an honest exclusion neither
    helps nor hurts, it simply is not counted. This makes the floor agree with
    the aggregate gate's per-file line-rate rather than a naive covered/len.
    """
    lines = [ln for ln in entry.get("lines", []) if not ln.get("gcovr/excluded", False)]
    total = len(lines)
    covered = sum(1 for ln in lines if ln.get("count", 0) > 0)
    return covered, total


def main() -> int:
    """Load the gcovr JSON and fail if any in-scope file is below the floor."""
    if not COVERAGE_JSON.is_file():
        print(
            f"check_coverage_floor.py: ERROR -- {COVERAGE_JSON} not found; "
            f"run `bash scripts/checks/coverage.sh` first."
        )
        return 1

    try:
        data = json.loads(COVERAGE_JSON.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_coverage_floor.py: ERROR -- cannot read coverage JSON: {exc}")
        return 1

    files = data.get("files", [])
    if not files:
        print("check_coverage_floor.py: ERROR -- coverage JSON has no files.")
        return 1

    offenders: list[tuple[float, str, int, int]] = []
    checked = 0
    for entry in files:
        rel = normalize(entry.get("file", ""))
        if not in_scope(rel):
            continue
        covered, total = file_line_pct(entry)
        if total == 0:
            continue
        checked += 1
        pct = 100.0 * covered / total
        if pct < FLOOR_PCT:
            offenders.append((pct, rel, covered, total))

    if checked == 0:
        print(
            "check_coverage_floor.py: ERROR -- no in-scope files matched; "
            "check the JSON path / scope."
        )
        return 1

    if offenders:
        offenders.sort()
        print(
            f"check_coverage_floor.py: {len(offenders)} first-party file(s) "
            f"below the {FLOOR_PCT:.0f}% line-coverage floor (NO allowlist):"
        )
        print("  cover  covered/total  file")
        for pct, rel, covered, total in offenders:
            print(f"  {pct:5.1f}%  {covered:5d}/{total:<5d}  {rel}")
        print(
            "Fix each at the root -- a real test, or a documented GCOVR_EXCL on "
            "a line no host input can reach. Do NOT add an allowlist."
        )
        return 1

    print(
        f"check_coverage_floor.py: PASS -- all {checked} first-party file(s) "
        f">= {FLOOR_PCT:.0f}% line coverage."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
