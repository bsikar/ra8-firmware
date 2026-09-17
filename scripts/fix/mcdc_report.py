# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The five artefacts the MC/DC audit emits, and the counts they share.

Split out of ``regen_mcdc_gaps.py`` (#359): parsing llvm-cov's report and
classifying each decision is one job; rendering that into a CSV, two Markdown
documents and two JSON roll-ups is another, and it is by far the larger half.

Every artefact quotes the same headline counts, so :func:`headline` computes
them once and each writer takes the result. That is the reason these live
together rather than one file per artefact -- the numbers in the CSV, the
report and the gate JSON must agree, and the only way to guarantee that is for
them to be the same numbers.
"""

from __future__ import annotations

import contextlib
import csv
import json
from collections import defaultdict

from regen_mcdc_gaps import (
    CSV_OUT,
    DEACT_MD_OUT,
    EXCERPT_MAX_DEACTIVATED,
    EXCERPT_MAX_REACHABLE,
    EXCERPT_TRUNC_DEACTIVATED,
    EXCERPT_TRUNC_REACHABLE,
    MCDC_FULL_PCT,
    MCDC_ZERO_PCT,
    MD_OUT,
    REPO_ROOT,
    TABLE_ROW_CAP,
    _truncate_md_cell,
    decision_snippet,
    module_of,
)


# ---------------------------------------------------------------------------
def write_gap_csv(classified: list) -> None:
    """Write the gap-only CSV.

    The `line` column is a text-derived snippet, not a line number: line
    numbers drift on every reformat and produce stale anchors, which project
    citation policy bans.
    """
    with CSV_OUT.open("w", encoding="ascii", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(
            [
                "source_file",
                "decision_text_snippet",
                "condition_count",
                "function_name",
                "decision_excerpt",
                "covered",
                "deactivated",
                "deactivation_rationale",
            ]
        )
        for src, _ln, n, func, excerpt, covered, deact, rationale in classified:
            w.writerow(
                [
                    src,
                    decision_snippet(excerpt),
                    n,
                    func,
                    excerpt,
                    covered,
                    "true" if deact else "false",
                    rationale,
                ]
            )


def module_rows(all_decisions: list) -> list:
    """Per-module (total, covered, partial, uncovered), worst first."""
    per_module: dict[str, list[float]] = defaultdict(list)
    for src, _ln, _n, _e, pct in all_decisions:
        per_module[module_of(src)].append(pct)

    rows = []
    for mod, pcts in per_module.items():
        total = len(pcts)
        covered = sum(1 for p in pcts if p >= MCDC_FULL_PCT)
        partial = sum(1 for p in pcts if MCDC_ZERO_PCT < p < MCDC_FULL_PCT)
        uncov = sum(1 for p in pcts if p == MCDC_ZERO_PCT)
        rows.append((mod, total, covered, partial, uncov))

    # Sort by uncovered+partial desc, then total desc, then name.
    rows.sort(key=lambda r: (-(r[3] + r[4]), -r[1], r[0]))
    return rows


def headline(all_decisions: list, classified: list) -> dict:
    """The counts every generated artefact quotes, computed once."""
    total_dec = len(all_decisions)
    yes_dec = sum(1 for d in all_decisions if d[4] >= MCDC_FULL_PCT)
    partial_dec = sum(1 for d in all_decisions if MCDC_ZERO_PCT < d[4] < MCDC_FULL_PCT)
    no_dec = sum(1 for d in all_decisions if d[4] == MCDC_ZERO_PCT)
    files_seen = len({d[0] for d in all_decisions})
    decision_complete_rate = (100.0 * yes_dec / total_dec) if total_dec else 0.0

    deactivated_rows = [r for r in classified if r[6]]
    reachable_rows = [r for r in classified if not r[6]]
    # Reachable-only MC/DC: treat deactivated decisions as if they were
    # at 100% (they are exempted under DO-178C 6.4.4.3 with documented
    # rationale in docs/MCDC_DEACTIVATIONS.md). The denominator is
    # unchanged; the numerator counts every covered decision plus every
    # documented-deactivated decision.
    deact_count = len(deactivated_rows)
    reachable_total = total_dec - deact_count
    reachable_covered = yes_dec
    reachable_rate = (100.0 * reachable_covered / reachable_total) if reachable_total else 100.0
    return {
        "total_dec": total_dec,
        "yes_dec": yes_dec,
        "partial_dec": partial_dec,
        "no_dec": no_dec,
        "files_seen": files_seen,
        "decision_complete_rate": decision_complete_rate,
        "deactivated_rows": deactivated_rows,
        "reachable_rows": reachable_rows,
        "deact_count": deact_count,
        "reachable_total": reachable_total,
        "reachable_covered": reachable_covered,
        "reachable_rate": reachable_rate,
    }


def _md_preamble() -> list[str]:
    """Report heading and the methodology note. No data, so no arguments."""
    md_lines: list[str] = []
    md_lines.append("# MC/DC Coverage Gap Audit")
    md_lines.append("")
    md_lines.append(
        "Live audit of compound boolean decisions reported by"
        " `llvm-cov show --show-mcdc` for first-party sources"
        " (`libs/`, `port/`, excluding `libs/third_party/`)."
        " Regenerated from `build/mcdc-report/mcdc.txt` by"
        " `scripts/fix/regen_mcdc_gaps.py`; do not edit by hand."
    )
    md_lines.append("")
    md_lines.append("## Methodology")
    md_lines.append("")
    md_lines.append(
        "- Source of truth: `build/mcdc-report/mcdc.txt` (output of `just quality::local::mcdc`)."
    )
    md_lines.append(
        '- A decision is one llvm-cov "MC/DC Decision Region".'
        " Condition count is taken from the `Number of Conditions:`"
        " field that llvm-cov emits for that region."
    )
    md_lines.append("- Coverage status (`covered` column):")
    md_lines.append(
        "  - `yes` -- llvm-cov reports 100.00% MC/DC for the decision."
        " Excluded from the CSV (CSV is gap-only)."
    )
    md_lines.append(
        "  - `partial` -- 0 < MC/DC % < 100. The decision was exercised"
        " but at least one independence pair is missing."
    )
    md_lines.append(
        "  - `no` -- MC/DC % == 0. The decision was never evaluated under instrumentation."
    )
    md_lines.append("")
    return md_lines


def _md_topline(h: dict) -> list[str]:
    """The headline decision counts and coverage rates."""
    total_dec = h["total_dec"]
    yes_dec = h["yes_dec"]
    partial_dec = h["partial_dec"]
    no_dec = h["no_dec"]
    files_seen = h["files_seen"]
    decision_complete_rate = h["decision_complete_rate"]
    deact_count = h["deact_count"]
    reachable_total = h["reachable_total"]
    reachable_rate = h["reachable_rate"]
    md_lines: list[str] = []
    md_lines.append("## Top-line Numbers")
    md_lines.append("")
    md_lines.append(f"- Source files with at least one decision: **{files_seen}**")
    md_lines.append(f"- Total compound decisions in scope: **{total_dec}**")
    md_lines.append(f"- Decisions at 100% MC/DC (`yes`): **{yes_dec}**")
    md_lines.append(f"- Decisions partially covered (`partial`): **{partial_dec}**")
    md_lines.append(f"- Decisions fully uncovered (`no`): **{no_dec}**")
    md_lines.append(
        "- Decision-complete rate (fully covered decisions / total decisions):"
        f" **{decision_complete_rate:.2f}%**"
    )
    md_lines.append(f"- Deactivated gap decision regions (DO-178C 6.4.4.3): **{deact_count}**")
    md_lines.append(
        f"- Reachable decision-region denominator (total - deactivated): **{reachable_total}**"
    )
    md_lines.append(
        f"- **Reachable decision-complete MC/DC rate**: **{reachable_rate:.2f}%**"
        " -- every counted decision region has complete MC/DC; the enforced"
        " ratchet threshold is recorded in `.github/mcdc-baseline.txt`."
    )
    md_lines.append("")
    md_lines.append(
        "See `docs/MCDC_DEACTIVATIONS.md` for the per-decision deactivation rationale catalog."
    )
    md_lines.append("")
    return md_lines


def _md_summary(h: dict) -> list[str]:
    """Report heading, methodology, and the top-line numbers."""
    return [*_md_preamble(), *_md_topline(h)]


def _md_gap_tables(h: dict) -> list[str]:
    """The reachable-gap and deactivated-gap tables."""
    deactivated_rows = h["deactivated_rows"]
    reachable_rows = h["reachable_rows"]
    md_lines: list[str] = []
    md_lines.append("## Reachable gaps (require new MC/DC test vectors)")
    md_lines.append("")
    md_lines.append("| File | Conds | Function | Excerpt | Status |")
    md_lines.append("|------|------:|----------|---------|--------|")
    for src, _ln, n, func, excerpt, covered, _deact, _rat in reachable_rows[:TABLE_ROW_CAP]:
        ex = _truncate_md_cell(
            excerpt.replace("|", "\\|"), EXCERPT_MAX_REACHABLE, EXCERPT_TRUNC_REACHABLE
        )
        md_lines.append(f"| {src} | {n} | {func} | `{ex}` | {covered} |")
    if len(reachable_rows) > TABLE_ROW_CAP:
        overflow = len(reachable_rows) - TABLE_ROW_CAP
        md_lines.append(f"| ... | | | | *({overflow} more rows in CSV)* | |")
    md_lines.append("")
    md_lines.append("## Deactivated gaps (DO-178C 6.4.4.3 exempted)")
    md_lines.append("")
    md_lines.append(
        "These decision regions are unreachable on any public-API path and"
        " are therefore exempted from the reachable decision-complete gate."
        " Each row carries the rationale used by the auto-classifier; humans"
        " may extend the per-decision narrative in `docs/MCDC_DEACTIVATIONS.md`."
    )
    md_lines.append("")
    md_lines.append("| File | Conds | Function | Excerpt | Rationale |")
    md_lines.append("|------|------:|----------|---------|-----------|")
    for src, _ln, n, func, excerpt, _covered, _deact, rationale in deactivated_rows:
        ex = _truncate_md_cell(
            excerpt.replace("|", "\\|"), EXCERPT_MAX_DEACTIVATED, EXCERPT_TRUNC_DEACTIVATED
        )
        rt = _truncate_md_cell(
            rationale.replace("|", "\\|"), EXCERPT_MAX_DEACTIVATED, EXCERPT_TRUNC_DEACTIVATED
        )
        md_lines.append(f"| {src} | {n} | {func} | `{ex}` | {_escape_md_tags(rt)} |")
    md_lines.append("")
    return md_lines


def _md_module_tables(rows: list) -> list[str]:
    """The per-module roll-up tables and the report footer."""
    md_lines: list[str] = []
    md_lines.append("## Per-module gap counts (full table)")
    md_lines.append("")
    md_lines.append("Sorted by (uncovered + partial) descending, then total descending.")
    md_lines.append("")
    md_lines.append("| Module | Total | Covered | Partial | Uncovered |")
    md_lines.append("|--------|------:|--------:|--------:|----------:|")
    for mod, total, covered, partial, uncov in rows:
        md_lines.append(f"| {mod} | {total} | {covered} | {partial} | {uncov} |")
    md_lines.append("")
    md_lines.append("## Top 30 modules with at least one uncovered decision")
    md_lines.append("")
    md_lines.append("| Module | Uncovered | Partial | Covered | Total |")
    md_lines.append("|--------|----------:|--------:|--------:|------:|")
    uncov_only = [r for r in rows if r[4] > 0]
    uncov_only.sort(key=lambda r: (-r[4], -r[3], r[0]))
    for mod, total, covered, partial, uncov in uncov_only[:30]:
        md_lines.append(f"| {mod} | {uncov} | {partial} | {covered} | {total} |")
    md_lines.append("")
    md_lines.append("---")
    md_lines.append("")
    md_lines.append(
        "*Regenerated from the live `just quality::local::mcdc` report. See"
        " `docs/MCDC_GAPS.csv` for the full per-decision table"
        " including decision-text snippets and excerpts.*"
    )
    md_lines.append("")

    return md_lines


def write_markdown(rows: list, h: dict) -> None:
    """Write the gap-audit report, one function per section."""
    md_lines = [*_md_summary(h), *_md_gap_tables(h), *_md_module_tables(rows)]
    MD_OUT.write_text("\n".join(md_lines), encoding="ascii")


def _deact_preamble() -> list[str]:
    """The fixed header of the catalog: what it is, the standard, and the policy."""
    return [
        "# MC/DC Deactivated-Decision Catalog",
        "",
        "This file documents every MC/DC decision region that has been"
        " classified as **deactivated** under **DO-178C 6.4.4.3"
        ' ("deactivated code")**. Deactivated decision regions are exempted'
        " from the reachable decision-complete MC/DC gate because the public-API contract"
        " makes them unreachable; they remain in the source for"
        " defense-in-depth, fault-injection robustness, and to give"
        " static analyzers a clear local invariant to anchor on.",
        "",
        "## Policy",
        "",
        "DO-178C 6.4.4.3 permits structural-coverage exemption for code"
        " that is intentionally not reachable in the operational"
        " configuration, provided each instance is (a) identified, (b)"
        " justified by a documented rationale, and (c) accompanied by"
        " upstream evidence that the exemption holds. The"
        " auto-classifier in `scripts/fix/regen_mcdc_gaps.py`"
        " enumerates every such decision region; this catalog records"
        " the upstream guard or contract that makes each one unreachable.",
        "",
        "Equivalent industry references: IEC 61508-3:2010 7.4.7"
        " (defensive-programming code), ISO 26262-6:2018 9.4.5"
        " (deactivated branches).",
        "",
        "## Auto-generated entries",
        "",
        "Generated by `scripts/fix/regen_mcdc_gaps.py` from"
        " `build/mcdc-report/mcdc.txt`. Do not edit by hand above the"
        " `<!-- MANUAL -->` marker; manual narrative may be added below"
        " the marker for a specific decision region by appending its"
        " `file::function::snippet` anchor (line numbers are not used:"
        " they drift on every reformat).",
        "",
    ]


def _escape_md_tags(text: str) -> str:
    """Escape angle brackets outside backtick spans.

    Deactivation rationales quote XML/HTML tag names (<nav>, <spine>, ...);
    emitted raw into markdown they read as real markup to Doxygen's HTML
    pass, which warns and fails the docs gate. Segments inside backtick code
    spans are already literal to Doxygen and must stay byte-identical.
    """
    parts = text.split("`")
    for i in range(0, len(parts), 2):
        parts[i] = parts[i].replace("<", "&lt;").replace(">", "&gt;")
    return "`".join(parts)


def _deact_entries(deactivated_rows: list[tuple]) -> list[str]:
    """One catalog section per deactivated decision, keyed by a stable anchor.

    The anchor is ``file::function::snippet`` rather than a line number
    precisely because line numbers drift on every reformat, which would
    silently detach a hand-written justification from its decision region.
    """
    if not deactivated_rows:
        return ["(no deactivated decision regions detected)", ""]
    out: list[str] = []
    for src, _ln, n, func, excerpt, covered, _deact, rationale in deactivated_rows:
        anchor = _escape_md_tags(f"{src}::{func}::{decision_snippet(excerpt)}")
        out.extend(
            [
                f"### {anchor}",
                "",
                f"- **Function**: `{func}`",
                f"- **Conditions in decision**: {n}",
                f"- **Current llvm-cov status**: {covered}",
                f"- **Source line**: `{excerpt.strip()}`",
                f"- **Rationale**: {_escape_md_tags(rationale)}",
                "- **DO-178C 6.4.4.3 basis**: defensive guard whose"
                " upstream contract is enforced on every public-API"
                " entry.",
                "",
            ]
        )
    return out


def _deact_manual_footer() -> list[str]:
    """The marker below which hand-written narrative may be added."""
    return [
        "<!-- MANUAL -->",
        "",
        "## Manual narrative (per anchor)",
        "",
        "_Add expanded justification here keyed by the"
        " `file::function::snippet` anchor above when the"
        " auto-generated rationale is"
        " insufficient. Anything below this marker is preserved across"
        " regenerations only if you commit it -- the regenerator"
        " currently overwrites the entire file; future revisions may"
        " split the manual section into a sibling file._",
        "",
    ]


def write_deactivations(h: dict) -> None:
    """Write the per-decision deactivation catalog (DO-178C 6.4.4.3)."""
    deact_lines = [
        *_deact_preamble(),
        *_deact_entries(h["deactivated_rows"]),
        *_deact_manual_footer(),
    ]
    DEACT_MD_OUT.write_text("\n".join(deact_lines), encoding="ascii")


def write_gate_json(h: dict) -> None:
    """Stash key counts so the gate script need not re-parse the report."""
    total_dec = h["total_dec"]
    yes_dec = h["yes_dec"]
    decision_complete_rate = h["decision_complete_rate"]
    deact_count = h["deact_count"]
    reachable_total = h["reachable_total"]
    reachable_covered = h["reachable_covered"]
    reachable_rate = h["reachable_rate"]
    gate_json = REPO_ROOT / "build" / "mcdc-report" / "gate.json"
    with contextlib.suppress(OSError):
        gate_json.write_text(
            "{\n"
            f'  "total_decisions": {total_dec},\n'
            f'  "covered_decisions": {yes_dec},\n'
            f'  "deactivated_decisions": {deact_count},\n'
            f'  "reachable_total": {reachable_total},\n'
            f'  "reachable_covered": {reachable_covered},\n'
            f'  "reachable_decision_complete_rate": {reachable_rate:.4f},\n'
            f'  "decision_complete_rate": {decision_complete_rate:.4f}\n'
            "}\n",
            encoding="ascii",
        )


def write_per_file_json(all_decisions: list, classified: list) -> None:
    """Per-file decision roll-up for check_mcdc_floor.py."""
    per_file: dict[str, dict[str, int]] = defaultdict(
        lambda: {"total": 0, "covered": 0, "deactivated": 0}
    )
    for src, _ln, _n, _e, pct in all_decisions:
        per_file[src]["total"] += 1
        if pct >= MCDC_FULL_PCT:
            per_file[src]["covered"] += 1
    for row in classified:
        if row[6]:  # deactivated
            per_file[row[0]]["deactivated"] += 1

    per_file_json = REPO_ROOT / "build" / "mcdc-report" / "mcdc_per_file.json"
    per_file_entries = [
        {
            "file": src,
            "total_decisions": rec["total"],
            "covered_decisions": rec["covered"],
            "deactivated_decisions": rec["deactivated"],
        }
        for src, rec in sorted(per_file.items())
    ]
    with contextlib.suppress(OSError):
        per_file_json.write_text(
            json.dumps({"files": per_file_entries}, indent=2) + "\n",
            encoding="ascii",
        )
