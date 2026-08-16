#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate the MISRA deviation register's derived claims against committed evidence.

``docs/qualification/MISRA_DEVIATIONS.md`` is the certification artefact that
says which MISRA-C 2012 findings the project has formally dispositioned.  Its
"current tree" inventories used to be live prose that nothing re-derived, and
they rotted spectacularly: D-005 claimed 166 Rule 8.4 findings while the
committed baseline held 1873 across 313 files (#632).  This checker makes that
class of rot a gate failure by re-deriving every machine-checkable claim in
the register from the two committed evidence files:

* ``.github/misra-baseline.txt`` -- the per-file-per-rule ratchet baseline
  (parsed by the one authoritative parser, ``misra_ratchet.load_baseline``).
* ``.cppcheck-suppressions`` -- the ``misra-c2012-*`` suppression rows the
  audit converts to ``--suppress=`` flags.

Machine-checked structures in the register (each is a SINGLE physical line,
so prose re-wrapping cannot silently detach a number from its pattern):

1. Provenance:   ``Baseline: <N> findings across <M> file/rule rows
   (Cppcheck <V>).`` -- N must equal the baseline's ``# total findings:``
   header AND the recomputed sum (a three-way check, so a truncated baseline
   is a refusal to compare, never a burn-down); M is the data-row count and V
   the ``# cppcheck:`` header value.
2. Register rows: ``| D-NNN | misra-c2012-R | <category> | <class> |
   <status> | <MAR> | <findings> | <files> |`` -- the deviation-index
   table, exactly one row per deviation, whose last two columns are
   re-derived per rule from the baseline.  Any ``| D-...`` line that
   does not parse as this 8-column shape -- including one with a
   non-three-digit ID like ``D-11`` -- is malformed, and so is any
   ``## D-`` heading that does not parse canonically, so the register
   cannot silently change shape out from under the checker.
3. Residual:     ``Residual (no deviation record): <R> rules, <N> findings,
   <M> rows.`` -- everything in the baseline outside the register.
4. Ownership:    ``- `misra-c2012-R` (<N> rows, <M> paths): <owner>`` -- one
   bullet per rule family present in ``.cppcheck-suppressions``, both counts
   re-derived; an unlisted family or a ghost bullet fails.
5. Excerpts:     ``Highest-count files for misra-c2012-R (top <N>, derived):``
   followed by ``| `path` | <count> |`` rows that must equal the true top-N
   (count descending, path ascending).  An excerpt-shaped row outside a
   parsed excerpt table -- the footprint of a wrapped or deleted intro
   line -- is malformed.
6. Populations:  any ``<N> findings across|in <M> files`` phrase is
   re-derived against the rule of the ``## D-NNN`` section it sits in, and
   is MALFORMED outside such a section, where no rule could derive it.  An
   unanchored population sentence in the top matter is how the #632 rot was
   worded, so the shape is refused rather than ignored.

Structural cross-checks: the ``## D-NNN: Rule R`` section headings and the
register rows must describe the same (ID, rule) set; that set must be exactly
the contiguous range the document header declares as active, so a deviation
cannot be retired, added or renumbered without amending the header; and every
row's rule must exist in the baseline, so an invented or typo'd rule id
cannot pass by claiming zero findings.

Non-vacuity floors (documented to SHRINK with the debt, never grow): the
baseline must parse to at least ``MIN_BASELINE_RULES`` distinct rules, and the
register to at least ``MIN_REGISTER_ROWS`` deviations,
``MIN_OWNERSHIP_FAMILIES`` ownership bullets and ``MIN_EXCERPTS`` excerpt
tables, else the scan is treated as collapsed (exit 2), not clean -- deleting
a claim class outright must never read as verifying it.  There is no
suppressive baseline of its own: every discrepancy is a failure the register
must absorb by being corrected.

What this checker does NOT verify, so the register's own wording must not
claim it does: the index's Category / Class / Status / MAR columns, prose that
does not use one of the shapes above, and the identity (as opposed to the
count) of the suppression rows behind each ownership bullet.

Exit codes: 0 clean, 1 drift (a claim disagrees with the evidence), 2
malformed input or collapsed/vacuous scan.

Usage::

    python3 scripts/checks/check_misra_deviations.py --check
    python3 scripts/checks/check_misra_deviations.py --selftest

``--selftest`` builds a fixture register + baseline + suppressions in a
temporary tree and drives ``run_check`` -- the identical entry point
``--check`` uses -- through every failure class: stale count, missing /
extra / misshapen register row, short deviation ID in a row or heading, a
row naming a rule absent from the baseline, a retirement that leaves the
header's ID range stale, a missing ID range, malformed and missing
documents, tampered / vacuous / two-column / below-floor baselines,
suppression drift, unowned family, ghost bullet, heading mismatch,
misordered excerpt, wrapped excerpt intro, a deleted excerpt block, a
deleted ownership list, and a population claim that is stale, reworded
``in``, or stranded outside any deviation section -- plus the
must-stay-quiet direction, and finally asserts the live tree clears the
floors.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from misra_ratchet import load_baseline

REPO_ROOT = Path(__file__).resolve().parents[2]
DOC_PATH = REPO_ROOT / "docs" / "qualification" / "MISRA_DEVIATIONS.md"
BASELINE_PATH = REPO_ROOT / ".github" / "misra-baseline.txt"
SUPPRESSIONS_PATH = REPO_ROOT / ".cppcheck-suppressions"

EXIT_OK = 0
EXIT_DRIFT = 1
EXIT_MALFORMED = 2

MIN_BASELINE_RULES = 30
"""Floor on distinct rules parsed from the baseline (63 at introduction).

A collapsed parse reports FEWER rules, which would otherwise read as a clean
register.  Shrink this constant with the debt as burn-down retires whole
rules; never raise it to paper over a parse regression.
"""

MIN_REGISTER_ROWS = 5
"""Floor on derived-population rows parsed from the register (10 today).

The register only ever loses rows when a deviation is formally retired, so a
parse that suddenly sees fewer than this is a collapsed scan, not progress.
The header's declared ID range is the tighter guard: a retirement that does
not also amend the range fails, so this floor only has to catch a collapse.
"""

MIN_OWNERSHIP_FAMILIES = 10
"""Floor on suppression-ownership bullets parsed from the register (15 today).

Without it, commenting out every ``misra-c2012-*`` suppression row and
deleting the bullet list verifies nothing and still announces PASS.  Shrink
it as families are genuinely retired; never raise it to hide a parse break.
"""

MIN_EXCERPTS = 1
"""Floor on highest-count-file excerpt tables parsed from the register.

An excerpt is the one claim class a maintainer could delete outright rather
than let it drift, so a floor is what keeps "0 excerpt(s) verified" from
reading as a clean run.
"""

RULE_PATTERN = r"misra-c2012-\d+\.\d+"

HEADER_RANGE_RE = re.compile(r"\(D-(\d{3})\.\.D-(\d{3}) active\)")
HEADING_RE = re.compile(r"^## (D-\d{3}): Rule (\d+\.\d+) ")
ANY_HEADING_RE = re.compile(r"^## D-")
ANY_REGISTER_ROW_RE = re.compile(r"^\|\s*D-\d+")
REGISTER_ROW_RE = re.compile(
    r"^\|\s*(D-\d{3})\s*\|\s*("
    + RULE_PATTERN
    + r")\s*\|(?:[^|]*\|){4}\s*(\d+)\s*\|\s*(\d+)\s*\|\s*$"
)
PROVENANCE_RE = re.compile(
    r"^Baseline: (\d+) findings across (\d+) file/rule rows \((Cppcheck [0-9.]+)\)\.$"
)
RESIDUAL_RE = re.compile(
    r"^Residual \(no deviation record\): (\d+) rules, (\d+) findings, (\d+) rows\.$"
)
OWNERSHIP_RE = re.compile(r"^- `(" + RULE_PATTERN + r")` \((\d+) rows?, (\d+) paths?\): \S")
EXCERPT_INTRO_RE = re.compile(
    r"^Highest-count files for (" + RULE_PATTERN + r") \(top (\d+), derived\):$"
)
EXCERPT_ROW_RE = re.compile(r"^\|\s*`([^`|]+)`\s*\|\s*(\d+)\s*\|$")
TABLE_DECOR_RE = re.compile(r"^\|[\s:|-]+\|$")
TOTAL_HEADER_RE = re.compile(r"^# total findings: (\d+)$")
POPULATION_RE = re.compile(r"\b(\d+)(?: spurious)? findings (?:across|in) (\d+) files?(?![\w/])")
"""A per-rule population restated in prose.

Matched ANYWHERE in the register: inside a ``## D-NNN`` section it is
re-derived against that section's rule, and outside one it is malformed --
there is no rule to derive it against, and an unanchored population sentence
in the top matter is exactly how the #632 rot was worded.  A deliberately
HISTORICAL figure must therefore not use this phrasing; the register says
"751 violations in the 2026-05-02 baseline" for those.
"""


@dataclass
class DocClaims:
    """Every machine-checkable claim parsed out of the deviation register."""

    provenance: list[tuple[int, int, str]] = field(default_factory=list)
    derived: list[tuple[str, str, int, int]] = field(default_factory=list)
    residual: list[tuple[int, int, int]] = field(default_factory=list)
    headings: dict[str, str] = field(default_factory=dict)
    declared_range: tuple[str, str] | None = None
    populations: list[tuple[str, int, int]] = field(default_factory=list)
    ownership: dict[str, tuple[int, int]] = field(default_factory=dict)
    excerpts: list[tuple[str, int, list[tuple[str, int]]]] = field(default_factory=list)
    malformed: list[str] = field(default_factory=list)


def _parse_excerpt_table(lines: list[str], start: int) -> tuple[list[tuple[str, int]], int]:
    """Consume one excerpt table after its intro line.

    Args:
        lines: The whole document as physical lines.
        start: Index of the first line after the intro sentence.

    Returns:
        The parsed ``(path, count)`` rows and the index of the first line
        after the table.
    """
    rows: list[tuple[str, int]] = []
    i = start
    while i < len(lines):
        line = lines[i]
        if not line.strip() or TABLE_DECOR_RE.match(line) or line.startswith("| File"):
            i += 1
            continue
        m = EXCERPT_ROW_RE.match(line)
        if m is None:
            break
        rows.append((m.group(1), int(m.group(2))))
        i += 1
    return rows, i


def _classify_line(claims: DocClaims, line: str) -> None:
    """Match one document line against every single-line claim pattern."""
    if m := PROVENANCE_RE.match(line):
        claims.provenance.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    elif m := RESIDUAL_RE.match(line):
        claims.residual.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    elif m := REGISTER_ROW_RE.match(line):
        claims.derived.append((m.group(1), m.group(2), int(m.group(3)), int(m.group(4))))
    elif ANY_REGISTER_ROW_RE.match(line):
        claims.malformed.append(f"register row does not match the 8-column shape: {line!r}")
    elif m := HEADING_RE.match(line):
        dev_id, rule = m.group(1), "misra-c2012-" + m.group(2)
        if dev_id in claims.headings:
            claims.malformed.append(f"duplicate section heading for {dev_id}")
        claims.headings[dev_id] = rule
    elif ANY_HEADING_RE.match(line):
        claims.malformed.append(f"deviation heading does not match '## D-NNN: Rule R': {line!r}")
    elif m := OWNERSHIP_RE.match(line):
        rule = m.group(1)
        if rule in claims.ownership:
            claims.malformed.append(f"duplicate suppression-ownership bullet for {rule}")
        claims.ownership[rule] = (int(m.group(2)), int(m.group(3)))
    elif EXCERPT_ROW_RE.match(line):
        claims.malformed.append(
            f"excerpt-shaped row outside an excerpt table (wrapped or missing intro?): {line!r}"
        )


def parse_doc(path: Path) -> DocClaims:
    """Parse the deviation register into its machine-checkable claims.

    Args:
        path: The register document (``MISRA_DEVIATIONS.md``).

    Returns:
        The parsed claims; structural defects land in ``claims.malformed``.
    """
    claims = DocClaims()
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    current_rule: str | None = None
    while i < len(lines):
        line = lines[i]
        if m := EXCERPT_INTRO_RE.match(line):
            rows, i = _parse_excerpt_table(lines, i + 1)
            if not rows:
                claims.malformed.append(f"excerpt for {m.group(1)} has an intro but no table rows")
            claims.excerpts.append((m.group(1), int(m.group(2)), rows))
            continue
        if line.startswith("## "):
            heading = HEADING_RE.match(line)
            current_rule = "misra-c2012-" + heading.group(2) if heading else None
        if claims.declared_range is None and (rm := HEADER_RANGE_RE.search(line)):
            claims.declared_range = ("D-" + rm.group(1), "D-" + rm.group(2))
        _classify_line(claims, line)
        for pm in POPULATION_RE.finditer(line):
            if current_rule is None:
                claims.malformed.append(
                    f"population claim outside any deviation section (no rule to derive "
                    f"it against): {line.strip()!r}"
                )
            else:
                claims.populations.append((current_rule, int(pm.group(1)), int(pm.group(2))))
        i += 1
    if len(claims.provenance) != 1:
        claims.malformed.append(
            f"expected exactly one 'Baseline: ...' provenance line, found {len(claims.provenance)}"
        )
    if len(claims.residual) != 1:
        claims.malformed.append(
            f"expected exactly one 'Residual ...' line, found {len(claims.residual)}"
        )
    seen: set[str] = set()
    for dev_id, _rule, _f, _n in claims.derived:
        if dev_id in seen:
            claims.malformed.append(f"duplicate derived-population row for {dev_id}")
        seen.add(dev_id)
    return claims


def read_baseline(path: Path) -> tuple[Counter[tuple[str, str]], str, int | None, list[str]]:
    """Load the committed baseline via the ratchet's own parser.

    Args:
        path: The committed ``misra-baseline.txt``.

    Returns:
        The per-(file, rule) counts, the recorded cppcheck version, the
        ``# total findings:`` header value (None when absent) and any
        malformed-input problems.
    """
    problems: list[str] = []
    header_total: int | None = None
    try:
        counts, version = load_baseline(path)
    except (SystemExit, ValueError):
        return Counter(), "unknown", None, [f"{path.name}: malformed baseline row"]
    except OSError as err:
        return Counter(), "unknown", None, [f"{path.name}: unreadable -- {err}"]
    for raw in path.read_text(encoding="utf-8").splitlines():
        if m := TOTAL_HEADER_RE.match(raw):
            header_total = int(m.group(1))
    if header_total is None:
        problems.append(f"{path.name}: missing '# total findings:' header")
    elif header_total != sum(counts.values()):
        problems.append(
            f"{path.name}: header claims {header_total} findings but the rows sum to "
            f"{sum(counts.values())} -- truncated or tampered; refusing to compare"
        )
    return counts, version, header_total, problems


def parse_suppressions(path: Path) -> tuple[dict[str, tuple[int, int]], list[str]]:
    """Derive per-rule row and path counts from the suppression list.

    Args:
        path: The committed ``.cppcheck-suppressions``.

    Returns:
        A map of rule id to ``(row count, distinct path-pattern count)`` and
        any malformed-input problems.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as err:
        return {}, [f"{path.name}: unreadable -- {err}"]
    rows: dict[str, int] = {}
    paths: dict[str, set[str]] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or not line.startswith("misra-c2012-"):
            continue
        parts = line.split(":")
        rule = parts[0]
        rows[rule] = rows.get(rule, 0) + 1
        if len(parts) > 1:
            paths.setdefault(rule, set()).add(parts[1])
    return {rule: (rows[rule], len(paths.get(rule, set()))) for rule in rows}, []


def _per_rule(counts: Counter[tuple[str, str]]) -> dict[str, tuple[int, int]]:
    """Aggregate the per-(file, rule) baseline into per-rule (findings, files)."""
    agg: dict[str, list[int]] = {}
    for (_fname, rule), count in counts.items():
        bucket = agg.setdefault(rule, [0, 0])
        bucket[0] += count
        bucket[1] += 1
    return {rule: (f, n) for rule, (f, n) in agg.items()}


def _range_problems(claims: DocClaims) -> list[str]:
    """Check the header's declared ID range against the rows actually present.

    Args:
        claims: The parsed register claims.

    Returns:
        Problems describing a silently retired, added or renumbered deviation.
    """
    if claims.declared_range is None:
        return ["header declares no '(D-NNN..D-NNN active)' range -- cannot bound the register"]
    first, last = claims.declared_range
    want = [f"D-{n:03d}" for n in range(int(first[2:]), int(last[2:]) + 1)]
    got = sorted({dev_id for dev_id, _r, _f, _n in claims.derived})
    if got != want:
        missing = [d for d in want if d not in got]
        extra = [d for d in got if d not in want]
        return [
            f"header declares {first}..{last} active but the index holds "
            f"{len(got)} row(s)"
            + (f"; missing {missing}" if missing else "")
            + (f"; unexpected {extra}" if extra else "")
        ]
    return []


def _register_problems(claims: DocClaims, per_rule: dict[str, tuple[int, int]]) -> list[str]:
    """Cross-check section headings and register rows against the baseline."""
    problems: list[str] = []
    derived = {dev_id: (rule, f, n) for dev_id, rule, f, n in claims.derived}
    for dev_id in sorted(set(claims.headings) | set(derived)):
        if dev_id not in claims.headings:
            problems.append(f"{dev_id}: register row without a section heading (extra row)")
        if dev_id not in derived:
            problems.append(f"{dev_id}: section heading without a register row (missing row)")
            continue
        rule, doc_f, doc_n = derived[dev_id]
        if claims.headings.get(dev_id, rule) != rule:
            problems.append(f"{dev_id}: rule mismatch between section heading and register row")
        if rule not in per_rule:
            problems.append(
                f"{dev_id}: rule {rule} appears nowhere in the baseline -- a typo, an "
                f"invented id, or a deviation whose debt is gone and which must be retired"
            )
            continue
        real_f, real_n = per_rule[rule]
        if (doc_f, doc_n) != (real_f, real_n):
            problems.append(
                f"{dev_id} ({rule}): register claims {doc_f} findings / {doc_n} files, "
                f"baseline derives {real_f} / {real_n} (stale count)"
            )
    return problems


def _population_problems(claims: DocClaims, per_rule: dict[str, tuple[int, int]]) -> list[str]:
    """Check in-section 'N findings across M files' restatements against the baseline."""
    problems = []
    for rule, doc_f, doc_n in claims.populations:
        real_f, real_n = per_rule.get(rule, (0, 0))
        if (doc_f, doc_n) != (real_f, real_n):
            problems.append(
                f"in-section population claim under {rule}: register says {doc_f} findings / "
                f"{doc_n} files, baseline derives {real_f} / {real_n} (stale count)"
            )
    return problems


def _residual_problems(claims: DocClaims, per_rule: dict[str, tuple[int, int]]) -> list[str]:
    """Check the residual line covers exactly the rules outside the register."""
    if not claims.residual or not claims.provenance:
        return []
    register_rules = {rule for _id, rule, _f, _n in claims.derived}
    resid = [rule for rule in per_rule if rule not in register_rules]
    want = (
        len(resid),
        sum(per_rule[r][0] for r in resid),
        sum(per_rule[r][1] for r in resid),
    )
    got = claims.residual[0]
    if got != want:
        return [
            f"residual line claims {got[0]} rules / {got[1]} findings / {got[2]} rows, "
            f"baseline derives {want[0]} / {want[1]} / {want[2]} (stale count)"
        ]
    return []


def _ownership_problems(claims: DocClaims, families: dict[str, tuple[int, int]]) -> list[str]:
    """Check the suppression-ownership bullets against the suppression list."""
    problems = []
    for rule in sorted(set(families) | set(claims.ownership)):
        if rule not in claims.ownership:
            problems.append(
                f"suppression family {rule} ({families[rule][0]} row(s)) has no ownership "
                f"bullet in the register (unowned waiver)"
            )
        elif rule not in families:
            problems.append(f"ownership bullet for {rule} matches no suppression row (ghost row)")
        elif claims.ownership[rule] != families[rule]:
            problems.append(
                f"ownership bullet for {rule} claims {claims.ownership[rule]}, suppression "
                f"list derives {families[rule]} (rows, paths)"
            )
    return problems


def _excerpt_problems(claims: DocClaims, counts: Counter[tuple[str, str]]) -> list[str]:
    """Check every highest-count-files excerpt equals the true top-N."""
    problems = []
    for rule, top_n, rows in claims.excerpts:
        per_file = [(fname, c) for (fname, r), c in counts.items() if r == rule]
        want = sorted(per_file, key=lambda fc: (-fc[1], fc[0]))[:top_n]
        if rows != want:
            problems.append(f"excerpt for {rule}: register lists {rows}, baseline derives {want}")
    return problems


def _provenance_problems(
    claims: DocClaims, counts: Counter[tuple[str, str]], version: str
) -> list[str]:
    """Check the provenance line's totals and tool version."""
    if not claims.provenance:
        return []
    doc_f, doc_rows, doc_version = claims.provenance[0]
    problems = []
    if (doc_f, doc_rows) != (sum(counts.values()), len(counts)):
        problems.append(
            f"provenance claims {doc_f} findings / {doc_rows} rows, baseline derives "
            f"{sum(counts.values())} / {len(counts)} (stale count)"
        )
    if doc_version != version:
        problems.append(
            f"provenance names {doc_version!r} but the baseline header records {version!r}"
        )
    return problems


def evaluate(
    doc_path: Path, baseline_path: Path, suppressions_path: Path
) -> tuple[list[str], list[str]]:
    """Re-derive every register claim from the committed evidence.

    Args:
        doc_path: The deviation register.
        baseline_path: The committed MISRA ratchet baseline.
        suppressions_path: The committed cppcheck suppression list.

    Returns:
        ``(drift, malformed)`` problem lists; both empty means clean.
    """
    try:
        claims = parse_doc(doc_path)
    except OSError as err:
        return [], [f"{doc_path.name}: unreadable -- {err}"]
    counts, version, _header_total, base_problems = read_baseline(baseline_path)
    families, supp_problems = parse_suppressions(suppressions_path)
    malformed = claims.malformed + base_problems + supp_problems
    per_rule = _per_rule(counts)
    if len(per_rule) < MIN_BASELINE_RULES:
        malformed.append(
            f"baseline parsed to {len(per_rule)} distinct rules, below the "
            f"non-vacuity floor of {MIN_BASELINE_RULES} -- collapsed scan, refusing to pass"
        )
    for parsed, floor, what in (
        (len(claims.derived), MIN_REGISTER_ROWS, "derived-population rows"),
        (len(claims.ownership), MIN_OWNERSHIP_FAMILIES, "suppression-ownership bullets"),
        (len(claims.excerpts), MIN_EXCERPTS, "excerpt tables"),
    ):
        if parsed < floor:
            malformed.append(
                f"register parsed to {parsed} {what}, below the non-vacuity floor of "
                f"{floor} -- collapsed scan, refusing to pass"
            )
    if malformed:
        return [], malformed
    drift = (
        _provenance_problems(claims, counts, version)
        + _range_problems(claims)
        + _register_problems(claims, per_rule)
        + _population_problems(claims, per_rule)
        + _residual_problems(claims, per_rule)
        + _ownership_problems(claims, families)
        + _excerpt_problems(claims, counts)
    )
    return drift, []


def run_check(doc_path: Path, baseline_path: Path, suppressions_path: Path) -> int:
    """Evaluate the register and report; the single entry point both modes use.

    Args:
        doc_path: The deviation register.
        baseline_path: The committed MISRA ratchet baseline.
        suppressions_path: The committed cppcheck suppression list.

    Returns:
        ``EXIT_OK``, ``EXIT_DRIFT`` or ``EXIT_MALFORMED``.
    """
    tag = "check_misra_deviations.py"
    drift, malformed = evaluate(doc_path, baseline_path, suppressions_path)
    for problem in malformed:
        print(f"{tag}: MALFORMED -- {problem}", file=sys.stderr)
    if malformed:
        return EXIT_MALFORMED
    for problem in drift:
        print(f"{tag}: DRIFT -- {problem}", file=sys.stderr)
    if drift:
        print(
            f"{tag}: {len(drift)} claim(s) in {doc_path.name} disagree with the committed "
            f"evidence. Re-derive the numbers from .github/misra-baseline.txt and "
            f".cppcheck-suppressions and correct the register -- never the evidence.",
            file=sys.stderr,
        )
        return EXIT_DRIFT
    claims = parse_doc(doc_path)
    print(
        f"{tag}: PASS -- {len(claims.derived)} deviations, {len(claims.ownership)} "
        f"suppression families and {len(claims.excerpts)} excerpt(s) verified against "
        f"the committed baseline."
    )
    return EXIT_OK


def _fixture_files() -> dict[str, str]:
    """Build a mutually consistent register + baseline + suppression fixture."""
    n_rules = 32
    rules = [f"misra-c2012-{i}.1" for i in range(1, n_rules + 1)]
    base_rows = [("a.c", rules[0], 3), ("b.c", rules[0], 1)]
    base_rows += [(f"f{i}.c", rules[i - 1], i) for i in range(2, n_rules + 1)]
    total = sum(c for _f, _r, c in base_rows)
    baseline = "\n".join(
        [
            "# fixture baseline",
            "# cppcheck: Cppcheck 9.9.9",
            f"# total findings: {total}",
            "# columns: file<TAB>rule<TAB>count",
            *[f"{f}\t{r}\t{c}" for f, r, c in sorted(base_rows)],
            "",
        ]
    )
    suppressions = "\n".join(
        [
            "# fixture suppressions",
            "unusedStructMember:inc/*.h",
            f"{rules[0]}:a.c:10",
            f"{rules[0]}:a.c:20",
            f"{rules[1]}:glob/*",
            *[f"{r}:filler{i}.c" for i, r in enumerate(rules[2:12])],
            "",
        ]
    )
    dev = [(f"D-{i:03d}", rules[i - 1]) for i in range(1, 7)]
    register_rows = [
        f"| {d} | {r} | Advisory | Fixture | Active | 2027-01-01 "
        f"| {4 if r == rules[0] else int(r.split('-')[2].split('.')[0])} "
        f"| {2 if r == rules[0] else 1} |"
        for d, r in dev
    ]
    resid_rules = rules[6:]
    resid_f = sum(int(r.split("-")[2].split(".")[0]) for r in resid_rules)
    doc = "\n".join(
        [
            "# Fixture register",
            "",
            "Fixture header (D-001..D-006 active).",
            "",
            "## Deviation index",
            "",
            "| ID    | Rule | Category | Class | Status | MAR | Findings | Files |",
            "|-------|------|----------|-------|--------|-----|---------:|------:|",
            *register_rows,
            "",
            "## Derived population",
            "",
            f"Baseline: {total} findings across {len(base_rows)} file/rule rows (Cppcheck 9.9.9).",
            "",
            f"Residual (no deviation record): {len(resid_rules)} rules, "
            f"{resid_f} findings, {len(resid_rules)} rows.",
            "",
            f"- `{rules[0]}` (2 rows, 1 path): fixture family one.",
            f"- `{rules[1]}` (1 row, 1 path): fixture family two.",
            *[f"- `{r}` (1 row, 1 path): fixture filler family." for r in rules[2:12]],
            "",
            f"Highest-count files for {rules[0]} (top 2, derived):",
            "",
            "| File | Findings |",
            "|------|---------:|",
            "| `a.c` | 3 |",
            "| `b.c` | 1 |",
            "",
            *[f"## {d}: Rule {r.removeprefix('misra-c2012-')} -- fixture\n" for d, r in dev],
            "Population note under the last section: 6 findings across 1 files.",
            "",
        ]
    )
    return {"doc": doc, "baseline": baseline, "suppressions": suppressions}


def _selftest_cases() -> list[tuple[str, str, str, str, int]]:
    """Enumerate the mutation cases as (name, file, old, new, expected exit)."""
    return [
        ("clean fixture passes", "doc", "", "", EXIT_OK),
        (
            "stale register count fires",
            "doc",
            "2027-01-01 | 4 | 2 |",
            "2027-01-01 | 5 | 2 |",
            EXIT_DRIFT,
        ),
        (
            "missing register row fires",
            "doc",
            "| D-003 | misra-c2012-3.1 | Advisory | Fixture | Active | 2027-01-01 | 3 | 1 |\n",
            "",
            EXIT_DRIFT,
        ),
        (
            "extra register row fires",
            "doc",
            "## Derived population",
            "| D-099 | misra-c2012-31.1 | Advisory | Fixture | Active | 2027-01-01 | 31 | 1 |\n"
            "\n## Derived population",
            EXIT_DRIFT,
        ),
        (
            "misshapen register row is malformed",
            "doc",
            "| D-002 | misra-c2012-2.1 | Advisory | Fixture | Active | 2027-01-01 | 2 | 1 |",
            "| D-002 | misra-c2012-2.1 | 2 | 1 |",
            EXIT_MALFORMED,
        ),
        ("missing provenance line is malformed", "doc", "Baseline: ", "Basel1ne: ", EXIT_MALFORMED),
        (
            "stale residual fires",
            "doc",
            "Residual (no deviation record): 26 rules,",
            "Residual (no deviation record): 27 rules,",
            EXIT_DRIFT,
        ),
        (
            "suppression row drift fires",
            "suppressions",
            "misra-c2012-1.1:a.c:20",
            "misra-c2012-1.1:a.c:20\nmisra-c2012-1.1:c.c:9",
            EXIT_DRIFT,
        ),
        (
            "unowned suppression family fires",
            "suppressions",
            "misra-c2012-2.1:glob/*",
            "misra-c2012-2.1:glob/*\nmisra-c2012-3.1:zzz.c",
            EXIT_DRIFT,
        ),
        (
            "ghost ownership bullet fires",
            "suppressions",
            "misra-c2012-2.1:glob/*\n",
            "",
            EXIT_DRIFT,
        ),
        (
            "heading rule mismatch fires",
            "doc",
            "## D-002: Rule 2.1 -- fixture",
            "## D-002: Rule 2.2 -- fixture",
            EXIT_DRIFT,
        ),
        (
            "misordered excerpt fires",
            "doc",
            "| `a.c` | 3 |\n| `b.c` | 1 |",
            "| `b.c` | 1 |\n| `a.c` | 3 |",
            EXIT_DRIFT,
        ),
        (
            "tampered baseline header is malformed",
            "baseline",
            "# total findings: ",
            "# total findings: 1",
            EXIT_MALFORMED,
        ),
        (
            "vacuous baseline is malformed",
            "baseline",
            "a.c\tmisra-c2012-1.1\t3",
            "a.c\tmisra-c2012-1.1\tX",
            EXIT_MALFORMED,
        ),
        (
            "two-column baseline row is malformed",
            "baseline",
            "b.c\tmisra-c2012-1.1\t1",
            "b.c\tmisra-c2012-1.1",
            EXIT_MALFORMED,
        ),
        (
            "stale in-section population fires",
            "doc",
            "6 findings across 1 files.",
            "7 findings across 1 files.",
            EXIT_DRIFT,
        ),
        (
            "wrapped excerpt intro is malformed",
            "doc",
            "Highest-count files for misra-c2012-1.1 (top 2, derived):",
            "Highest-count files for misra-c2012-1.1\n(top 2, derived):",
            EXIT_MALFORMED,
        ),
        (
            "short deviation-id register row is malformed",
            "doc",
            "## Derived population",
            "| D-11 | misra-c2012-31.1 | Advisory | Fixture | Active | 2027-01-01 | 31 | 1 |\n"
            "\n## Derived population",
            EXIT_MALFORMED,
        ),
        (
            "short deviation-id heading is malformed",
            "doc",
            "## D-002: Rule 2.1 -- fixture",
            "## D-11: Rule 2.1 -- fixture",
            EXIT_MALFORMED,
        ),
        (
            "deleting the whole excerpt block is malformed",
            "doc",
            "Highest-count files for misra-c2012-1.1 (top 2, derived):\n\n"
            "| File | Findings |\n|------|---------:|\n| `a.c` | 3 |\n| `b.c` | 1 |\n",
            "",
            EXIT_MALFORMED,
        ),
        (
            "deleting one ownership bullet fires as an unowned waiver",
            "doc",
            "- `misra-c2012-1.1` (2 rows, 1 path): fixture family one.",
            "",
            EXIT_DRIFT,
        ),
        (
            "population claim outside a deviation section is malformed",
            "doc",
            "## Derived population",
            "Rule 1.1 alone accounts for 4 findings across 2 files.\n\n## Derived population",
            EXIT_MALFORMED,
        ),
        (
            "population claim reworded with 'in' still fires",
            "doc",
            "6 findings across 1 files.",
            "7 findings in 1 file.",
            EXIT_DRIFT,
        ),
        (
            "retiring a deviation without amending the header range fires",
            "doc",
            "| D-006 | misra-c2012-6.1 | Advisory | Fixture | Active | 2027-01-01 | 6 | 1 |\n",
            "",
            EXIT_DRIFT,
        ),
        (
            "a register row naming a rule absent from the baseline fires",
            "doc",
            "| D-006 | misra-c2012-6.1 | Advisory | Fixture | Active | 2027-01-01 | 6 | 1 |",
            "| D-006 | misra-c2012-99.9 | Advisory | Fixture | Active | 2027-01-01 | 0 | 0 |",
            EXIT_DRIFT,
        ),
        (
            "a missing header range is malformed",
            "doc",
            "Fixture header (D-001..D-006 active).",
            "Fixture header.",
            EXIT_DRIFT,
        ),
    ]


def _run_fixture_case(tmp: Path, files: dict[str, str], name: str, expected: int) -> str | None:
    """Materialise one fixture variant and drive ``run_check`` over it.

    Args:
        tmp: Scratch directory to write the variant into.
        files: The three fixture file bodies, already mutated for this case.
        name: Case label for the failure report.
        expected: The exit code the case must produce.

    Returns:
        A failure description, or None when the case behaves as expected.
    """
    doc = tmp / "MISRA_DEVIATIONS.md"
    baseline = tmp / "misra-baseline.txt"
    suppressions = tmp / "cppcheck-suppressions"
    doc.write_text(files["doc"], encoding="utf-8")
    baseline.write_text(files["baseline"], encoding="utf-8")
    suppressions.write_text(files["suppressions"], encoding="utf-8")
    got = run_check(doc, baseline, suppressions)
    if got != expected:
        return f"{name}: expected exit {expected}, got {got}"
    return None


def selftest() -> int:
    """Drive every failure class and the quiet direction through ``run_check``."""
    tag = "check_misra_deviations.py --selftest"
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir)
        for name, key, old, new, expected in _selftest_cases():
            files = dict(_fixture_files())
            if old and old not in files[key]:
                failures.append(f"{name}: fixture mutation target {old!r} not found")
                continue
            if old:
                files[key] = files[key].replace(old, new)
            problem = _run_fixture_case(tmp, files, name, expected)
            if problem:
                failures.append(problem)
        floor_files = dict(_fixture_files())
        floor_files["baseline"] = (
            "# fixture baseline\n"
            "# cppcheck: Cppcheck 9.9.9\n"
            "# total findings: 3\n"
            "a.c\tmisra-c2012-1.1\t3\n"
        )
        problem = _run_fixture_case(tmp, floor_files, "below-floor baseline", EXIT_MALFORMED)
        if problem:
            failures.append(problem)
        stripped = dict(_fixture_files())
        stripped["doc"] = "\n".join(
            line for line in stripped["doc"].splitlines() if not line.startswith("- `misra-c2012-")
        )
        problem = _run_fixture_case(tmp, stripped, "no ownership bullets at all", EXIT_MALFORMED)
        if problem:
            failures.append(problem)
        good = _fixture_files()
        (tmp / "misra-baseline.txt").write_text(good["baseline"], encoding="utf-8")
        (tmp / "cppcheck-suppressions").write_text(good["suppressions"], encoding="utf-8")
        got = run_check(
            tmp / "absent.md", tmp / "misra-baseline.txt", tmp / "cppcheck-suppressions"
        )
        if got != EXIT_MALFORMED:
            failures.append(f"missing register document: expected exit {EXIT_MALFORMED}, got {got}")
    _live_drift, live_malformed = evaluate(DOC_PATH, BASELINE_PATH, SUPPRESSIONS_PATH)
    if live_malformed:
        failures.append(f"live tree trips the floors or fails to parse: {live_malformed[:3]}")
    for failure in failures:
        print(f"{tag}: FAIL: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"{tag}: PASS ({len(_selftest_cases()) + 3} both-direction cases + live floors)")
    return 0


def main(argv: list[str] | None = None) -> int:
    """Entry point: parse the mode flag and run the register check or selftest.

    Args:
        argv: Argument vector override for tests; None uses ``sys.argv``.

    Returns:
        The process exit code.
    """
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="re-derive every register claim from committed evidence (default)",
    )
    mode.add_argument(
        "--selftest",
        action="store_true",
        help="prove every failure class fires and the clean fixture stays quiet",
    )
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    return run_check(DOC_PATH, BASELINE_PATH, SUPPRESSIONS_PATH)


if __name__ == "__main__":
    sys.exit(main())
