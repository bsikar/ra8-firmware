#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""ONE per-file coverage policy for every first-party translation unit.

There is one quality bar for this codebase and no tier gets a softer one. Every
enrolled unit -- ``libs/``, ``src/``, ``port/``, ``tools/``, ``apps/``,
``examples/`` alike -- carries exactly one row in
``.github/tree-coverage-baseline.txt``, and this checker is the only thing that
judges those rows. ``tree_coverage_model.py`` says what is enrolled and what
measures it; ``scripts/report/tree_coverage.sh`` produces the measurement.

WHAT REPLACED WHAT
------------------
Three overlapping regimes used to answer the same question differently: an
aggregate ``gcovr --fail-under-line 90 --fail-under-branch 80`` plus a
``libs/``+``src/``-only per-file line floor; a SECOND full coverage build
ratcheted against a two-number project-wide baseline; and a product-named
media_dl per-file ratchet with its own baseline and its own scope list. Three
policies meant three answers to "is this file covered enough", two coverage
builds of the same translation units, and -- because each had its own scope --
a large majority of the tree that no policy mentioned at all.

They are one policy now:

* **MEASURED** rows freeze the unit's measured line and branch counts.
  Uncovered debt may not grow and the ratio may not fall, so a unit standing at
  100% keeps it and a unit standing at 41% burns down toward the 90/80 floor
  instead of sliding. An improvement is silently welcome; ``--update`` is how
  it gets frozen in.
* A unit with **no row** is new. It must enter at the full 90% line / 80%
  branch floor -- historical debt is not something a new file can inherit.
* **UNMEASURED(<reason>)** rows are explicit. Nothing is silently absent: a
  unit no host build executes still has a row naming which of the four classes
  in ``tree_coverage_model`` it falls into, and the class is RE-DERIVED from
  the tree on every run, so it cannot be hand-written into something friendlier.
  Gaining measurement is one-way: a unit that starts producing execution data
  must move to MEASURED and can never move back.

MEASUREMENT
-----------
Every project in ``tree_coverage_model.PROJECTS`` is built and run separately
and reported into its own gcovr trace; the traces are then merged. The merge is
the point. The media_dl core is compiled by BOTH the host suite and the
media_dl form, and a single gcovr sweep over one build tree therefore reports
whichever half it happened to see -- ``mdl_config.c`` measures 77.3% from the
host suite alone and 90.1% from the union. One row per unit means one number
per unit, and that number is what every project together executed.

Run::

    check_tree_coverage.py             # the gate
    check_tree_coverage.py --update    # re-freeze the baseline (tighten only)
    check_tree_coverage.py --selftest  # prove both directions
    check_tree_coverage.py --projects  # the measurement projects, for the producer

Exit 0 when the tree matches the baseline, 1 on a violation, 2 when the
measurement or the enumeration itself did not happen.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import firmware_app_dirs, first_party_paths
from tree_coverage_model import (
    CENSUS_SUFFIXES,
    MEASURED_FLOOR,
    PROJECTS,
    REASON_COMPILED,
    REASON_FIRMWARE,
    REASON_HOSTED,
    REASON_PLATFORM,
    REASONS,
    census_floor_failures,
    census_paths,
    coverage_capable_dirs,
    in_census,
    structural_reason,
    unclaimed_coverage_projects,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
REPORT_DIR = REPO_ROOT / "build" / "tree-coverage"
MERGED_SUMMARY = REPORT_DIR / "summary.json"
BASELINE_FILE = REPO_ROOT / ".github" / "tree-coverage-baseline.txt"

LINE_FLOOR_PCT = 90
BRANCH_FLOOR_PCT = 80

KIND_MEASURED = "MEASURED"
KIND_UNMEASURED = "UNMEASURED"

MEASURED_COLUMNS = 6
UNMEASURED_COLUMNS = 3

#: A finding that ``--update`` must refuse to write over: a real regression, a
#: new file below the floor, or a unit that lost its measurement.
HARD = "HARD"

#: A finding that says the baseline no longer describes the tree in a direction
#: ``--update`` may record: a new unit, a deleted unit, a unit that GAINED
#: measurement, a reason class the tree no longer supports. The gate still
#: fails on these -- a baseline that is out of date is not a true baseline.
DRIFT = "DRIFT"

#: line_covered, line_total, branch_covered, branch_total.
Counts = tuple[int, int, int, int]


@dataclass(frozen=True)
class Row:
    """One baseline row: a frozen measurement, or a named reason there is none."""

    kind: str
    """``KIND_MEASURED`` or ``KIND_UNMEASURED``."""

    counts: Counts
    """The frozen counts. ``NO_COUNTS`` on an UNMEASURED row."""

    reason: str
    """One of ``tree_coverage_model.REASONS``. Empty on a MEASURED row."""


@dataclass(frozen=True)
class Finding:
    """One reason the tree and the baseline disagree."""

    severity: str
    """``HARD`` or ``DRIFT``."""

    message: str
    """Path-first, human-readable, and specific enough to act on."""


#: The counts an UNMEASURED row carries. A sentinel rather than ``None`` so the
#: two row kinds have one shape and no rule needs a null check to read them.
NO_COUNTS: Counts = (0, 0, 0, 0)


def measured(counts: Counts) -> Row:
    """Build a MEASURED row from four gcovr counts."""
    return Row(KIND_MEASURED, counts, "")


def unmeasured(reason: str) -> Row:
    """Build an UNMEASURED row from a reason class."""
    return Row(KIND_UNMEASURED, NO_COUNTS, reason)


# ---------------------------------------------------------------------------
# Reading the measurement
# ---------------------------------------------------------------------------


def load_summary(path: Path) -> dict[str, Counts]:
    """Load a gcovr ``--json-summary`` report into per-file integer counts.

    Args:
        path: The summary written by ``scripts/report/tree_coverage.sh``.

    Returns:
        Repo-relative path -> counts, for census units only. Anything else in
        the report (headers, vendored SOUP, test sources) is dropped here so
        the policy never has to know they existed.

    Raises:
        SystemExit: When the file is missing or unreadable -- an absent
            measurement is not an empty one.
    """
    data = _read_json(path)
    rows: dict[str, Counts] = {}
    for entry in data.get("files", []):
        rel = str(entry.get("filename", "")).replace("\\", "/")
        if not rel.endswith(CENSUS_SUFFIXES):
            continue
        rows[rel] = (
            int(entry["line_covered"]),
            int(entry["line_total"]),
            int(entry["branch_covered"]),
            int(entry["branch_total"]),
        )
    return rows


def _read_json(path: Path) -> dict:
    """Read one JSON document, exiting 2 when it is absent or malformed."""
    if not path.is_file():
        print(
            f"check_tree_coverage.py: missing measurement: {path}\n"
            f"  Run `bash scripts/report/tree_coverage.sh` first.",
            file=sys.stderr,
        )
        raise SystemExit(2)
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_tree_coverage.py: cannot read {path}: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc


def compiled_sources() -> set[str]:
    """Every census-shaped source a measurement project's build compiled.

    Read from each project's ``compile_commands.json``. This is what separates
    "no host build touches this unit" from "a host build compiled it and no
    test ever pulled the archive member in" -- the second is invisible in a
    coverage report, because gcov writes a .gcno and never a .gcda.

    Raises:
        SystemExit: When a declared project left no compile database. Skipping
            it would silently collapse that distinction and reclassify every
            never-executed unit as one no host build reaches, which is the
            friendlier answer and the wrong one.
    """
    root = str(REPO_ROOT).replace("\\", "/")
    out: set[str] = set()
    for project in PROJECTS:
        db = REPORT_DIR / project.name / "compile_commands.json"
        if not db.is_file():
            print(
                f"check_tree_coverage.py: missing compile database: {db}\n"
                f"  Run `bash scripts/report/tree_coverage.sh` first.",
                file=sys.stderr,
            )
            raise SystemExit(2)
        for entry in _read_json_list(db):
            rel = _repo_relative(entry, root)
            if rel is not None and rel.endswith(CENSUS_SUFFIXES):
                out.add(rel)
    return out


def _read_json_list(path: Path) -> list[dict]:
    """Read a JSON array document, tolerating nothing but a real array."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return data if isinstance(data, list) else []


def _repo_relative(entry: dict, root: str) -> str | None:
    """Repo-relative source path from one compile-database entry, or None."""
    source = str(entry.get("file", "")).replace("\\", "/")
    if not source.startswith("/"):
        directory = str(entry.get("directory", "")).replace("\\", "/")
        source = f"{directory}/{source}"
    source = str(Path(source).resolve()).replace("\\", "/")
    if not source.startswith(root + "/"):
        return None
    return source[len(root) + 1 :]


def project_report_failures() -> list[str]:
    """Return one message per measurement project whose own report is vacuous.

    Every declared project must have produced a report, and that report must
    carry at least the project's ``min_files`` census units. A project that
    silently stopped instrumenting still merges cleanly into a healthy-looking
    total, so the floor has to be asserted per project.
    """
    failures: list[str] = []
    for project in PROJECTS:
        path = REPORT_DIR / "summaries" / f"{project.name}.json"
        seen = len(load_summary(path))
        if seen < project.min_files:
            failures.append(
                f"measurement project {project.name} reported {seen} census unit(s), "
                f"floor is {project.min_files}"
            )
    return failures


# ---------------------------------------------------------------------------
# Deriving what the tree says right now
# ---------------------------------------------------------------------------


def derive_rows(paths: list[str], report: dict[str, Counts], compiled: set[str]) -> dict[str, Row]:
    """Build the row every census unit is entitled to from this measurement.

    Args:
        paths: The census, from ``tree_coverage_model.census_paths``.
        report: Merged per-file counts from ``load_summary``.
        compiled: Sources a measurement build compiled, from
            ``compiled_sources``.

    Returns:
        Repo-relative path -> the row the tree currently supports.
    """
    firmware = firmware_app_dirs()
    rows: dict[str, Row] = {}
    for rel in paths:
        counts = report.get(rel)
        if counts is not None and counts[1] > 0:
            rows[rel] = measured(counts)
            continue
        reason = structural_reason(
            rel,
            compiled=rel in compiled or counts is not None,
            firmware_dirs=firmware,
        )
        rows[rel] = unmeasured(reason)
    return rows


# ---------------------------------------------------------------------------
# The policy
# ---------------------------------------------------------------------------


def _below_floor(covered: int, total: int, floor: int) -> bool:
    """True when an integer covered/total ratio is under ``floor`` percent."""
    return total > 0 and (covered * 100) < (floor * total)


def _new_file_findings(rel: str, counts: Counts) -> list[Finding]:
    """A unit with no baseline row must enter at the full floor."""
    line_cov, line_total, branch_cov, branch_total = counts
    out: list[Finding] = []
    if _below_floor(line_cov, line_total, LINE_FLOOR_PCT):
        out.append(Finding(HARD, f"{rel}: new unit enters below {LINE_FLOOR_PCT}% line coverage"))
    if _below_floor(branch_cov, branch_total, BRANCH_FLOOR_PCT):
        out.append(
            Finding(HARD, f"{rel}: new unit enters below {BRANCH_FLOOR_PCT}% branch coverage")
        )
    if not out:
        out.append(Finding(DRIFT, f"{rel}: new unit has no baseline row"))
    return out


def _metric_findings(
    rel: str, label: str, now: tuple[int, int], base: tuple[int, int]
) -> list[Finding]:
    """Ratchet one metric: uncovered debt may not grow, the ratio may not fall."""
    covered, total = now
    base_covered, base_total = base
    out: list[Finding] = []
    if (total - covered) > (base_total - base_covered):
        out.append(
            Finding(
                HARD,
                f"{rel}: uncovered {label} debt grew "
                f"{base_total - base_covered} -> {total - covered}",
            )
        )
    if base_total > 0 and covered * base_total < base_covered * total:
        out.append(
            Finding(
                HARD,
                f"{rel}: {label} ratio regressed {base_covered}/{base_total} -> {covered}/{total}",
            )
        )
    floor = LINE_FLOOR_PCT if label == "line" else BRANCH_FLOOR_PCT
    if base_total == 0 and _below_floor(covered, total, floor):
        out.append(Finding(HARD, f"{rel}: new {label} metric entered below the {floor}% floor"))
    return out


def _measured_findings(rel: str, now: Row, base: Row) -> list[Finding]:
    """Compare a MEASURED baseline row against what the tree now reports."""
    if now.kind != KIND_MEASURED:
        return [
            Finding(
                HARD,
                f"{rel}: lost its measurement (now {now.reason}); gaining measurement is one-way",
            )
        ]
    line_cov, line_total, branch_cov, branch_total = now.counts
    base_line_cov, base_line_total, base_branch_cov, base_branch_total = base.counts
    return [
        *_metric_findings(rel, "line", (line_cov, line_total), (base_line_cov, base_line_total)),
        *_metric_findings(
            rel, "branch", (branch_cov, branch_total), (base_branch_cov, base_branch_total)
        ),
    ]


def _unmeasured_findings(rel: str, now: Row, base: Row) -> list[Finding]:
    """Compare an UNMEASURED baseline row against what the tree now reports."""
    if base.reason not in REASONS:
        return [Finding(HARD, f"{rel}: baseline reason {base.reason!r} is not a known class")]
    if now.kind == KIND_MEASURED:
        return [Finding(DRIFT, f"{rel}: gained measurement; re-freeze it as MEASURED")]
    if now.reason != base.reason:
        return [
            Finding(DRIFT, f"{rel}: reason class is now {now.reason}, baseline says {base.reason}")
        ]
    return []


def evaluate(fresh: dict[str, Row], baseline: dict[str, Row]) -> list[Finding]:
    """Return every disagreement between the tree and the committed baseline.

    Args:
        fresh: What this measurement supports, from ``derive_rows``.
        baseline: What is committed, from ``load_baseline``.

    Returns:
        Findings sorted by severity then message, so a HARD violation is always
        read before the drift it may have caused.
    """
    out: list[Finding] = [
        Finding(DRIFT, f"{rel}: baseline row is stale -- the unit is gone from the census")
        for rel in sorted(set(baseline) - set(fresh))
    ]
    for rel in sorted(fresh):
        now = fresh[rel]
        base = baseline.get(rel)
        if base is None:
            if now.kind == KIND_MEASURED:
                out.extend(_new_file_findings(rel, now.counts))
            else:
                out.append(Finding(DRIFT, f"{rel}: new unit has no baseline row ({now.reason})"))
        elif base.kind == KIND_MEASURED:
            out.extend(_measured_findings(rel, now, base))
        else:
            out.extend(_unmeasured_findings(rel, now, base))
    return sorted(out, key=lambda f: (f.severity != HARD, f.message))


# ---------------------------------------------------------------------------
# The baseline file
# ---------------------------------------------------------------------------

BASELINE_HEADER = (
    "# ONE coverage baseline for every first-party translation unit.",
    "#",
    "# Emitted by `python3 scripts/checks/check_tree_coverage.py --update`.",
    "# Never hand-edit: every field is re-derived from the tree and the merged",
    "# gcovr measurement, so an edit is either a no-op or a lie the gate finds.",
    "#",
    "# MEASURED   <file> MEASURED <line-covered> <line-total> <branch-covered> <branch-total>",
    "#            Frozen debt. Uncovered lines/branches may not grow and the",
    "#            ratio may not fall; a NEW unit must enter at >=90% line and",
    "#            >=80% branch.",
    "# UNMEASURED <file> UNMEASURED <reason-class>",
    "#            No host execution path reaches it. The class is re-derived",
    "#            every run; gaining measurement is one-way.",
    "#",
    "# Columns are TAB-separated. Rows are sorted by path.",
)


def format_baseline(rows: dict[str, Row]) -> str:
    """Render the baseline deterministically: sorted, counted, no timestamps."""
    kinds = [row.kind for row in rows.values()]
    lines = [
        *BASELINE_HEADER,
        f"# rows: {len(rows)}"
        f"  measured: {kinds.count(KIND_MEASURED)}"
        f"  unmeasured: {kinds.count(KIND_UNMEASURED)}",
        "",
    ]
    for rel in sorted(rows):
        row = rows[rel]
        if row.kind == KIND_MEASURED:
            body = "\t".join(str(value) for value in row.counts)
            lines.append(f"{rel}\t{KIND_MEASURED}\t{body}")
        else:
            lines.append(f"{rel}\t{KIND_UNMEASURED}\t{row.reason}")
    return "\n".join([*lines, ""])


def parse_baseline(text: str) -> dict[str, Row]:
    """Parse baseline text into rows.

    Raises:
        ValueError: On a malformed row. A baseline that cannot be read is not
            an empty baseline.
    """
    rows: dict[str, Row] = {}
    for raw in text.splitlines():
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) == MEASURED_COLUMNS and fields[1] == KIND_MEASURED:
            values = tuple(int(field) for field in fields[2:])
            rows[fields[0]] = measured((values[0], values[1], values[2], values[3]))
        elif len(fields) == UNMEASURED_COLUMNS and fields[1] == KIND_UNMEASURED:
            rows[fields[0]] = unmeasured(fields[2])
        else:
            message = f"malformed baseline row: {raw!r}"
            raise ValueError(message)
    return rows


def load_baseline(path: Path = BASELINE_FILE) -> dict[str, Row]:
    """Read the committed baseline, or an empty mapping when it does not exist."""
    if not path.is_file():
        return {}
    return parse_baseline(path.read_text(encoding="ascii"))


# ---------------------------------------------------------------------------
# Scope guard: a measurable project that nothing measures
# ---------------------------------------------------------------------------


def scope_failures() -> list[str]:
    """Return one message per coverage-capable listfile no project claims.

    A CMake project that declares ``option(RA8_COVERAGE ...)`` can produce
    execution data. If no measurement project builds it, that data is never
    collected and every unit in it sits at UNMEASURED forever while the gate
    reports a clean tree -- the same shape as a scope list that quietly stopped
    describing the repository.
    """
    listfiles = {
        rel: (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        for rel in first_party_paths(("CMakeLists.txt", ".cmake"))
    }
    unclaimed = unclaimed_coverage_projects(coverage_capable_dirs(listfiles))
    return [
        f"{directory}/ declares option(RA8_COVERAGE ...) but no measurement "
        f"project in tree_coverage_model.PROJECTS builds it"
        for directory in unclaimed
    ]


# ---------------------------------------------------------------------------
# The gate
# ---------------------------------------------------------------------------


def _print_findings(findings: list[Finding]) -> None:
    """Print every finding, hard violations first, with the remedy."""
    hard = [f for f in findings if f.severity == HARD]
    drift = [f for f in findings if f.severity == DRIFT]
    print("check_tree_coverage.py: FAIL", file=sys.stderr)
    for finding in hard:
        print(f"  [regression] {finding.message}", file=sys.stderr)
    for finding in drift:
        print(f"  [stale row]  {finding.message}", file=sys.stderr)
    if hard:
        print(
            "\n  A regression is fixed with a test, never by editing the baseline.",
            file=sys.stderr,
        )
    if drift and not hard:
        print(
            "\n  Re-freeze with `python3 scripts/checks/check_tree_coverage.py --update`.",
            file=sys.stderr,
        )


def _fail_setup(failures: list[str]) -> int:
    """Report a collapsed enumeration or an uncollected project and exit 2."""
    print("check_tree_coverage.py: the measurement did not happen", file=sys.stderr)
    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    return 2


def _measure() -> tuple[list[str], dict[str, Row]] | int:
    """Return (census, fresh rows), or an exit code when the setup is broken."""
    paths = census_paths()
    setup = census_floor_failures(paths) + scope_failures() + project_report_failures()
    if setup:
        return _fail_setup(setup)
    fresh = derive_rows(paths, load_summary(MERGED_SUMMARY), compiled_sources())
    seen = sum(1 for row in fresh.values() if row.kind == KIND_MEASURED)
    if seen < MEASURED_FLOOR:
        return _fail_setup(
            [f"only {seen} census unit(s) carry execution data, floor is {MEASURED_FLOOR}"]
        )
    return paths, fresh


def run_gate(*, update: bool) -> int:
    """Judge the tree against the committed baseline, optionally re-freezing it."""
    outcome = _measure()
    if isinstance(outcome, int):
        return outcome
    _, fresh = outcome
    baseline = load_baseline()
    findings = evaluate(fresh, baseline) if baseline else []
    if update:
        hard = [f for f in findings if f.severity == HARD]
        if hard:
            _print_findings(hard)
            return 1
        BASELINE_FILE.write_text(format_baseline(fresh), encoding="ascii")
        print(f"check_tree_coverage.py: wrote {BASELINE_FILE} ({len(fresh)} rows)")
        return 0
    if not baseline:
        print("check_tree_coverage.py: no baseline; run --update once", file=sys.stderr)
        return 2
    if findings:
        _print_findings(findings)
        return 1
    kinds = [row.kind for row in fresh.values()]
    print(
        f"check_tree_coverage.py: PASS -- {len(fresh)} first-party unit(s): "
        f"{kinds.count(KIND_MEASURED)} measured (no regression), "
        f"{kinds.count(KIND_UNMEASURED)} unmeasured (all declared)."
    )
    return 0


# ---------------------------------------------------------------------------
# Selftest -- both directions, and a non-vacuity case for every floor.
#
# The cases drive `evaluate()`, `census_floor_failures()`,
# `unclaimed_coverage_projects()` and the baseline round trip: the four things
# that can silently stop working. One direction proves nothing, so every rule
# below has a must-fire case AND a must-stay-quiet one -- a checker whose scope
# collapsed to zero rows is also perfectly quiet.
# ---------------------------------------------------------------------------

#: The committed state the ratchet cases are measured against: one unit at the
#: floor, one deep in debt, one firmware composition, one host tool with no
#: coverage build. Every reason class and both row kinds are represented, so a
#: rule that stopped covering either kind is caught by a case rather than by
#: nobody.
SELFTEST_BASELINE: dict[str, Row] = {
    "libs/ra8_demo/src/frozen.c": measured((90, 100, 80, 100)),
    "apps/shared/media_dl/src/debt.c": measured((41, 100, 30, 100)),
    "examples/ek_ra8d2/demo/main.c": unmeasured(REASON_FIRMWARE),
    "tools/demo/src/host.c": unmeasured(REASON_HOSTED),
}

Case = tuple[str, dict[str, Row], bool]


def _swap(rel: str, row: Row) -> dict[str, Row]:
    """The baseline with one row replaced -- the shape every case needs."""
    return {**SELFTEST_BASELINE, rel: row}


def _ratchet_cases() -> list[Case]:
    """Cases for the MEASURED ratchet: debt, ratio, floors, improvement."""
    frozen = "libs/ra8_demo/src/frozen.c"
    debt = "apps/shared/media_dl/src/debt.c"
    return [
        ("an unchanged tree stays quiet", dict(SELFTEST_BASELINE), False),
        ("uncovered line debt growth fires", _swap(frozen, measured((90, 101, 80, 100))), True),
        ("a line ratio drop fires", _swap(frozen, measured((89, 100, 80, 100))), True),
        ("a branch ratio drop fires", _swap(frozen, measured((90, 100, 79, 100))), True),
        ("an improvement stays quiet", _swap(frozen, measured((97, 100, 88, 100))), False),
        ("burning debt down stays quiet", _swap(debt, measured((60, 100, 45, 100))), False),
        ("a debt unit sliding further fires", _swap(debt, measured((40, 100, 30, 100))), True),
    ]


def _kind_cases() -> list[Case]:
    """Cases for the row kinds: new units, one-way moves, reason classes."""
    frozen = "libs/ra8_demo/src/frozen.c"
    tool = "tools/demo/src/host.c"
    new = "libs/ra8_demo/src/new.c"
    return [
        (
            "a well-covered new unit still needs a row",
            {**SELFTEST_BASELINE, new: measured((9, 10, 8, 10))},
            True,
        ),
        (
            "a poorly-covered new unit fires",
            {**SELFTEST_BASELINE, new: measured((8, 10, 7, 10))},
            True,
        ),
        (
            "a new unmeasured unit fires",
            {**SELFTEST_BASELINE, new: unmeasured(REASON_PLATFORM)},
            True,
        ),
        ("losing measurement fires", _swap(frozen, unmeasured(REASON_PLATFORM)), True),
        ("gaining measurement fires", _swap(tool, measured((10, 10, 10, 10))), True),
        ("a changed reason class fires", _swap(tool, unmeasured(REASON_COMPILED)), True),
        (
            "a deleted unit's stale row fires",
            {k: v for k, v in SELFTEST_BASELINE.items() if k != tool},
            True,
        ),
    ]


def _evaluate_failures() -> list[str]:
    """Run every evaluate() case and name the ones that answered wrongly."""
    return [
        name
        for name, fresh, should_fire in _ratchet_cases() + _kind_cases()
        if bool(evaluate(fresh, SELFTEST_BASELINE)) != should_fire
    ]


def _severity_failures() -> list[str]:
    """Prove --update refuses a regression and accepts a pure staleness."""
    frozen = "libs/ra8_demo/src/frozen.c"
    tool = "tools/demo/src/host.c"
    regression = evaluate(_swap(frozen, measured((80, 100, 80, 100))), SELFTEST_BASELINE)
    staleness = evaluate(_swap(tool, measured((10, 10, 10, 10))), SELFTEST_BASELINE)
    out: list[str] = []
    if not any(f.severity == HARD for f in regression):
        out.append("a coverage regression must be HARD so --update refuses it")
    if any(f.severity == HARD for f in staleness):
        out.append("a unit that merely gained measurement must not be HARD")
    return out


def _scope_failures() -> list[str]:
    """Prove the census, the floors and the project-claim guard all still bite."""
    live = census_paths()
    out: list[str] = []
    if census_floor_failures(live):
        out.append("the live census must clear every root floor")
    if not census_floor_failures([rel for rel in live if not rel.startswith("tools/")]):
        out.append("a census with tools/ removed must fail its root floor")
    if unclaimed_coverage_projects(["tests", "apps/shared/media_dl"]):
        out.append("a claimed coverage project must not be reported unclaimed")
    if not unclaimed_coverage_projects(["tools/unwired"]):
        out.append("an unclaimed coverage project must be reported")
    if not in_census("libs/ra8_demo/src/a.c") or in_census("libs/ra8_demo/tests/a.c"):
        out.append("the census must take production units and reject test sources")
    return out


def _format_failures() -> list[str]:
    """Prove the baseline round-trips and renders byte-identically twice."""
    text = format_baseline(SELFTEST_BASELINE)
    out: list[str] = []
    if parse_baseline(text) != SELFTEST_BASELINE:
        out.append("the baseline must round-trip through format/parse unchanged")
    if format_baseline(SELFTEST_BASELINE) != text:
        out.append("the baseline must render identically on every call")
    try:
        parse_baseline("libs/a.c\tMEASURED\t1\t2\n")
    except ValueError:
        pass
    else:
        out.append("a malformed baseline row must raise, not be skipped")
    return out


def selftest() -> int:
    """Prove every rule fires and stays quiet, and that no scope collapsed."""
    cases = len(_ratchet_cases()) + len(_kind_cases())
    failures = _evaluate_failures() + _severity_failures() + _scope_failures() + _format_failures()
    if failures:
        for name in failures:
            print(f"check_tree_coverage.py --selftest: FAIL: {name}", file=sys.stderr)
        return 1
    print(
        f"check_tree_coverage.py --selftest: PASS "
        f"({cases} both-direction cases, 4 non-vacuity floors)"
    )
    return 0


def main() -> int:
    """Dispatch the selftest, the project list, or the gate."""
    parser = argparse.ArgumentParser(description="One coverage policy for the whole tree.")
    parser.add_argument("--selftest", action="store_true", help="prove both directions")
    parser.add_argument("--update", action="store_true", help="re-freeze the baseline")
    parser.add_argument(
        "--projects", action="store_true", help="print '<name> <cmake-dir>' per project"
    )
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.projects:
        for project in PROJECTS:
            print(f"{project.name} {project.cmake_dir}")
        return 0
    return run_gate(update=args.update)


if __name__ == "__main__":
    sys.exit(main())
