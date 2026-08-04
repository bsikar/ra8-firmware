#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""misra_ratchet.py -- MISRA-C 2012 ratchet gate (compare vs committed baseline).

`scripts/checks/misra_check_inner.sh` (make misra) runs cppcheck + the bundled
misra.py addon and writes one finding per line to `build/misra/results.txt`.
Until now that audit was advisory-only: no CI job consumed it, so the
finding count could only grow (issue #240). This script turns the audit
into a one-way ratchet against a committed baseline:

* NEW findings (any per-file-per-rule count above the baseline, or any
  file/rule pair absent from the baseline) FAIL the gate.
* Shrinkage (findings burned down) PASSES with a notice to re-baseline via
  `make misra-baseline`, which locks the progress in so the debt can never
  quietly grow back.

Baseline normalization -- per-file-per-rule COUNTS, not raw finding lines:
raw misra.py findings carry line numbers, which churn on every unrelated
edit above them, and message text, which embeds identifiers. Fingerprinting
the violating source line churns whenever that line is touched for any
reason. A `(file, rule) -> count` map is invariant under both kinds of
drift, still trips on any new violation of a rule in a file (the bucket
count rises), keeps the committed baseline small and diffable, and is fully
deterministic. The accepted coarseness: within one bucket, adding one
violation while simultaneously fixing another nets zero and passes -- fine
for a burn-down ratchet whose end state is an empty baseline.

The baseline records the cppcheck version that generated it. cppcheck
majors disagree on rule coverage (2.13 has no C23 parse; newer versions
change false-positive sets), so the gate WARNS loudly on a version mismatch
-- regenerate the baseline on the CI-pinned toolchain (the self-hosted
runner / dev box cppcheck), never on a drifted local install.

Usage:
    python3 scripts/checks/misra_ratchet.py --check    # gate (default)
    python3 scripts/checks/misra_ratchet.py --update   # rewrite the baseline

Both modes read `build/misra/results.txt`; run the audit first:
    bash scripts/checks/misra_check_inner.sh

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RESULTS_TSV = REPO_ROOT / "build" / "misra" / "results.txt"
BASELINE_FILE = REPO_ROOT / ".github" / "misra-baseline.txt"

MAX_DETAIL_LINES = 10
"""Cap on raw finding lines echoed per offending (file, rule) bucket."""

RESULTS_COLUMNS = 5
"""Column count of one results.txt row: rule, severity, file, line, message."""

BASELINE_COLUMNS = 3
"""Column count of one baseline row: file, rule, count."""


def cppcheck_version() -> str:
    """Return the `cppcheck --version` string, or a placeholder when absent."""
    try:
        out = subprocess.run(
            ["cppcheck", "--version"],  # noqa: S607  # trusted: fixed cppcheck argv
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return out.stdout.strip().splitlines()[0] if out.stdout.strip() else "unknown"


def load_results(path: Path) -> tuple[Counter[tuple[str, str]], dict[tuple[str, str], list[str]]]:
    r"""Parse the misra_check.sh TSV into per-(file, rule) counts + raw lines.

    Each row is `rule \\t severity \\t file \\t line \\t message`. Rows that
    do not split into five columns are ignored (defensive: the awk parser in
    misra_check.sh already guarantees the shape).
    """
    counts: Counter[tuple[str, str]] = Counter()
    details: dict[tuple[str, str], list[str]] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        cols = raw.split("\t")
        if len(cols) != RESULTS_COLUMNS:
            continue
        rule, _severity, fname, line, message = cols
        key = (fname, rule)
        counts[key] += 1
        details.setdefault(key, []).append(f"{fname}:{line}: {message} [{rule}]")
    return counts, details


def load_baseline(path: Path) -> tuple[Counter[tuple[str, str]], str]:
    """Parse the committed baseline into counts + the recorded cppcheck version."""
    counts: Counter[tuple[str, str]] = Counter()
    version = "unknown"
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("# cppcheck:"):
            version = raw.split(":", 1)[1].strip()
            continue
        if not raw or raw.startswith("#"):
            continue
        cols = raw.split("\t")
        if len(cols) != BASELINE_COLUMNS:
            print(f"misra_ratchet.py: ERROR -- malformed baseline row: {raw!r}")
            sys.exit(1)
        fname, rule, count = cols
        counts[(fname, rule)] = int(count)
    return counts, version


def write_baseline(path: Path, counts: Counter[tuple[str, str]]) -> None:
    """Serialize `counts` (sorted by file, then rule) with a provenance header."""
    total = sum(counts.values())
    lines = [
        "# MISRA-C 2012 ratchet baseline -- per-file-per-rule finding counts.",
        "# Consumed by scripts/checks/misra_ratchet.py --check (CI job: misra).",
        "# Regenerate ON THE CI-PINNED CPPCHECK (self-hosted runner / dev box):",
        "#   make misra-baseline",
        "# and commit the result. Never regenerate to absorb NEW findings --",
        "# fix those at the root or record a deviation in",
        "# docs/qualification/MISRA_DEVIATIONS.md first.",
        f"# cppcheck: {cppcheck_version()}",
        f"# total findings: {total}",
        "# columns: file<TAB>rule<TAB>count",
    ]
    lines += [f"{fname}\t{rule}\t{count}" for (fname, rule), count in sorted(counts.items())]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def report_regressions(
    regressions: list[tuple[str, str, int, int]],
    details: dict[tuple[str, str], list[str]],
) -> None:
    """Print each offending (file, rule) bucket with its raw finding lines."""
    print(f"misra_ratchet.py: FAIL -- {len(regressions)} (file, rule) bucket(s) grew:")
    for fname, rule, base, cur in regressions:
        print(f"  {fname}  {rule}  baseline={base} -> current={cur}")
        for detail in details.get((fname, rule), [])[:MAX_DETAIL_LINES]:
            print(f"    {detail}")
    print(
        "Fix the new violation(s) at the root. If a finding is a cppcheck\n"
        "false positive, suppress it in .cppcheck-suppressions with a\n"
        "justification comment; if it is a formally accepted deviation,\n"
        "record it in docs/qualification/MISRA_DEVIATIONS.md. Only after\n"
        "one of those dispositions may the baseline be regenerated\n"
        "(make misra-baseline, on the CI-pinned cppcheck)."
    )


def check(counts: Counter[tuple[str, str]], details: dict[tuple[str, str], list[str]]) -> int:
    """Ratchet `counts` against the committed baseline; return the exit code."""
    if not BASELINE_FILE.is_file():
        print(
            f"misra_ratchet.py: ERROR -- baseline {BASELINE_FILE} missing; "
            f"generate it with `make misra-baseline` and commit it."
        )
        return 1
    baseline, base_version = load_baseline(BASELINE_FILE)

    current_version = cppcheck_version()
    if current_version != base_version:
        print(
            f"misra_ratchet.py: WARNING -- cppcheck version skew: baseline was "
            f"generated by {base_version!r} but this run used {current_version!r}. "
            f"Finding sets are not comparable across cppcheck versions; run the "
            f"gate on the CI-pinned toolchain before trusting a red or a green."
        )

    regressions = [
        (fname, rule, baseline.get((fname, rule), 0), count)
        for (fname, rule), count in sorted(counts.items())
        if count > baseline.get((fname, rule), 0)
    ]
    if regressions:
        report_regressions(regressions, details)
        return 1

    # No regressions past this point, so every current bucket is <= its
    # baseline bucket and the totals difference is exactly the burn-down.
    cur_total = sum(counts.values())
    base_total = sum(baseline.values())
    shrunk = base_total - cur_total
    if shrunk > 0:
        print(
            f"misra_ratchet.py: PASS -- no new findings, and {shrunk} baseline "
            f"finding(s) were burned down ({base_total} -> {cur_total}). Lock the "
            f"progress in: run `make misra-baseline` on the CI-pinned cppcheck "
            f"and commit the shrunken .github/misra-baseline.txt."
        )
    else:
        print(f"misra_ratchet.py: PASS -- {cur_total} finding(s), all within the baseline.")
    return 0


def main() -> int:
    """Entry point: parse the mode flag and run the ratchet or the update."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="fail on any finding above the committed baseline (default)",
    )
    mode.add_argument(
        "--update",
        action="store_true",
        help="rewrite the committed baseline from build/misra/results.txt",
    )
    args = parser.parse_args()

    if not RESULTS_TSV.is_file():
        print(
            f"misra_ratchet.py: ERROR -- {RESULTS_TSV} not found; "
            f"run `bash scripts/checks/misra_check_inner.sh` first."
        )
        return 1
    counts, details = load_results(RESULTS_TSV)
    if not counts:
        print(
            "misra_ratchet.py: ERROR -- results.txt parsed to zero findings; "
            "a clean tree writes an empty baseline via --update, but an empty "
            "parse in --check mode almost always means the audit itself broke "
            "(cppcheck missing, addon missing, or dump generation failed)."
        )
        return 1

    if args.update:
        write_baseline(BASELINE_FILE, counts)
        print(
            f"misra_ratchet.py: baseline updated -- {sum(counts.values())} finding(s) "
            f"across {len(counts)} (file, rule) bucket(s) -> {BASELINE_FILE}"
        )
        return 0
    return check(counts, details)


if __name__ == "__main__":
    sys.exit(main())
