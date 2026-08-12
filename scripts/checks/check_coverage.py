#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_coverage.py -- gate plain statement+branch coverage against a baseline.

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B Qualification":

  Statement and branch coverage are necessary but not sufficient
  for the highest safety integrity level (MC/DC is the gold
  standard). We still maintain a separate statement+branch
  baseline because it is fast, cheap, and provides a useful
  ratchet against accidental regressions.

This script reads `build/coverage/coverage.xml` (the Cobertura XML
produced by `bash scripts/report/coverage_report.sh`) and compares
the overall line-rate / branch-rate against the two numbers stored
in `.github/coverage-baseline.txt`. It fails (exit 1) if either
metric drops below baseline by more than the slack tolerance.

Modeled on scripts/checks/check_mcdc_block.py. Enforcing: a
regression below baseline (minus the slack band) FAILS the gate.
The baseline has been stable across CI runs, so there is no
warn-only mode -- a coverage regression blocks the run.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SLACK_PCT = 0.5
BASELINE_COLUMN_COUNT = 2  # baseline file has two numeric columns: statement% branch%
"""Allow a 0.5pp regression band before failing. gcovr is deterministic
across runs on a fixed source tree, but the test selection in CI may
differ slightly between Ubuntu image versions."""

REPO_ROOT = Path(__file__).resolve().parents[2]
COVERAGE_XML = REPO_ROOT / "build" / "coverage" / "coverage.xml"
BASELINE_FILE = REPO_ROOT / ".github" / "coverage-baseline.txt"


def parse_baseline(path: Path) -> tuple[float, float]:
    """Read the (statement %, branch %) pair from the baseline file.

    Comment lines are skipped so the baseline can carry a note explaining why
    a number is where it is -- which is the difference between a ratchet and
    an unexplained constant.
    """
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split()
        if len(parts) >= BASELINE_COLUMN_COUNT:
            return float(parts[0]), float(parts[1])
    msg = f"baseline file {path} has no numeric line"
    raise RuntimeError(msg)


def parse_cobertura(path: Path) -> tuple[float, float]:
    """Read line-rate and branch-rate from the Cobertura XML root element.

    Both attributes are 0..1 floats in the report and are converted to percent
    here, so every comparison downstream is in one unit.
    """
    tree = ET.parse(path)  # noqa: S314  # trusted CI-generated Cobertura XML, not user input
    root = tree.getroot()
    line_rate = float(root.attrib["line-rate"]) * 100.0
    branch_rate = float(root.attrib["branch-rate"]) * 100.0
    return line_rate, branch_rate


def main() -> int:
    """Gate coverage against the committed baseline.

    A missing baseline or a missing coverage XML each exit 1 rather than being
    treated as "nothing to compare": either one means the measurement did not
    happen, and passing on absent evidence is what this gate exists to
    prevent.

    Returns 0 when coverage meets the baseline, 1 otherwise.
    """
    if not BASELINE_FILE.exists():
        print(f"[ERROR] baseline file missing: {BASELINE_FILE}", file=sys.stderr)
        return 1

    if not COVERAGE_XML.exists():
        msg = (
            f"[ERROR] coverage XML missing: {COVERAGE_XML}\n"
            f"        Run `make coverage` (or "
            f"`bash scripts/report/coverage_report.sh`) first."
        )
        print(msg, file=sys.stderr)
        return 1

    base_line, base_branch = parse_baseline(BASELINE_FILE)
    line_pct, branch_pct = parse_cobertura(COVERAGE_XML)

    print(f"check_coverage.py: baseline statement={base_line:.1f}% branch={base_branch:.1f}%")
    print(f"check_coverage.py: measured statement={line_pct:.1f}% branch={branch_pct:.1f}%")

    failures: list[str] = []
    if line_pct + SLACK_PCT < base_line:
        failures.append(
            f"  statement coverage {line_pct:.1f}% < baseline "
            f"{base_line:.1f}% (slack {SLACK_PCT}pp)"
        )
    if branch_pct + SLACK_PCT < base_branch:
        failures.append(
            f"  branch coverage {branch_pct:.1f}% < baseline "
            f"{base_branch:.1f}% (slack {SLACK_PCT}pp)"
        )

    if failures:
        print("[FAIL] coverage regressed below baseline:")
        for f in failures:
            print(f)
        print("       Either add tests to restore coverage, or, if the")
        print("       regression is intentional, update")
        print("       .github/coverage-baseline.txt.")
        return 1

    # If we beat the baseline by more than 1pp on both metrics, suggest
    # ratcheting. This is a hint; it does not modify the file.
    if line_pct >= base_line + 1.0 and branch_pct >= base_branch + 1.0:
        print("check_coverage.py: HINT -- coverage exceeds baseline by >=1pp.")
        print(
            f"       Consider ratcheting .github/coverage-baseline.txt to "
            f"{line_pct:.1f} {branch_pct:.1f}."
        )

    print("check_coverage.py: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
