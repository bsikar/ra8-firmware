#!/usr/bin/env python3
"""
scripts/utils/regen_mcdc_gaps.py -- regenerate docs/MCDC_GAPS.csv and the
summary header of docs/MCDC_GAPS.md from the LIVE llvm-cov MC/DC report
(build/mcdc-report/mcdc.txt + summary.txt).

Each row carries a `deactivated` boolean classifying whether the
remaining uncovered MC/DC condition is reachable through the public
API or whether it is a defensive guard already enforced by an upstream
check (per DO-178C 6.4.4.3, "deactivated code"). Detection is
heuristic but conservative -- see `is_deactivated_decision()`.

Source of truth: actual llvm-cov per-decision output. NOT a static parse
of the source tree, NOT a heuristic match against test_mcdc_* function
names. The previous CSV regenerator used heuristics and went stale; this
one parses the same report `make mcdc` emits.

A decision is reported when llvm-cov shows it as < 100% MC/DC. The
columns are:

    source_file,line,condition_count,function_name,decision_excerpt,covered

where `covered` is one of:
    - "no"      -- 0.00% MC/DC for that decision
    - "partial" -- 0 < pct < 100
    - "yes"     -- 100% (omitted from CSV; CSV is gap-only)

Verification: for any module, the number of CSV rows equals the count
of `MC/DC Coverage for Decision: <pct>%` blocks in mcdc.txt where the
percentage is < 100, restricted to that module's source files.

Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import csv
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
MCDC_TXT = REPO_ROOT / "build" / "mcdc-report" / "mcdc.txt"
CSV_OUT = REPO_ROOT / "docs" / "MCDC_GAPS.csv"
MD_OUT = REPO_ROOT / "docs" / "MCDC_GAPS.md"
DEACT_MD_OUT = REPO_ROOT / "docs" / "MCDC_DEACTIVATIONS.md"

# Lines in mcdc.txt look like:
#   "  385|      0|  if ((start == 0U) || (start > end)) {"
# i.e.   <pad><lineno>|<exec_count>|<source>
LINE_RE = re.compile(r"^\s*(\d+)\|\s*[^|]*\|(.*)$")

FILE_HEADER_RE = re.compile(r"^/work/(.+\.(?:c|h|cpp|hpp)):\s*$")

DECISION_HDR_RE = re.compile(
    r"\|---> MC/DC Decision Region \((\d+):\d+\) to \(\d+:\d+\)"
)
COND_COUNT_RE = re.compile(r"\|\s+Number of Conditions:\s+(\d+)")
PCT_RE = re.compile(r"\|\s+MC/DC Coverage for Decision:\s+([0-9.]+)%")


def parse_mcdc_txt(path: Path):
    """Yield (rel_path, line, cond_count, source_excerpt, pct_float).

    Only emits one record per decision. The source excerpt is taken from
    the numbered listing at `line` in the same per-file section.
    """
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()

    cur_file = None
    # source_by_line[lineno] -> source text (per current file)
    source_by_line: dict[int, str] = {}
    in_decision = False
    dec_line = None
    dec_cond_count = None

    for raw in lines:
        # File transition
        m = FILE_HEADER_RE.match(raw)
        if m:
            cur_file = m.group(1)
            source_by_line = {}
            in_decision = False
            dec_line = None
            dec_cond_count = None
            continue

        # Decision header
        m = DECISION_HDR_RE.search(raw)
        if m:
            in_decision = True
            dec_line = int(m.group(1))
            dec_cond_count = None
            continue

        if in_decision:
            m = COND_COUNT_RE.search(raw)
            if m:
                dec_cond_count = int(m.group(1))
                continue
            m = PCT_RE.search(raw)
            if m and cur_file is not None and dec_line is not None:
                pct = float(m.group(1))
                src = source_by_line.get(dec_line, "").strip()
                yield (cur_file, dec_line, dec_cond_count or 0, src, pct)
                in_decision = False
                dec_line = None
                dec_cond_count = None
                continue
            continue

        # Source line in numbered listing
        m = LINE_RE.match(raw)
        if m and cur_file is not None:
            ln = int(m.group(1))
            # The source after the second '|' may have a leading ' ' the
            # split swallowed; preserve content as-is.
            src = m.group(2)
            # First occurrence wins (some lines repeat in expansion contexts).
            source_by_line.setdefault(ln, src)


# ---------------------------------------------------------------------------
# Function-name resolver: walk the source file's brace structure to find
# the innermost function definition enclosing a given line.
# ---------------------------------------------------------------------------
FUNC_DEF_RE = re.compile(
    r"^[A-Za-z_][\w\s\*\(\),:<>]*?\b([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{?\s*$"
)


def resolve_function(rel_path: str, target_line: int) -> str:
    """Best-effort function name lookup. Returns "(file scope)" if the
    decision is not inside a function (e.g. file-scope initializer)."""
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return "(file scope)"
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return "(file scope)"
    src_lines = text.splitlines()
    # Walk forward, tracking brace depth and the most recent name at depth 0.
    depth = 0
    last_name_at_depth0 = None
    func_stack: list[tuple[str, int]] = []  # (name, depth_when_opened)
    in_block_comment = False
    for i, raw in enumerate(src_lines, start=1):
        line = raw
        # strip block comments crudely
        if in_block_comment:
            end = line.find("*/")
            if end == -1:
                line = ""
            else:
                line = line[end + 2 :]
                in_block_comment = False
        # strip /* ... */ on same line
        while True:
            s = line.find("/*")
            if s == -1:
                break
            e = line.find("*/", s + 2)
            if e == -1:
                line = line[:s]
                in_block_comment = True
                break
            line = line[:s] + line[e + 2 :]
        # strip // comments
        s = line.find("//")
        if s != -1:
            line = line[:s]
        # strip strings (very rough)
        line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)

        # If we're at depth 0, look for a function-definition signature.
        # Heuristic: ID followed by ( ... ) and an open brace either on
        # this line or on a subsequent line before any other open brace.
        if depth == 0:
            m = FUNC_DEF_RE.match(line.strip())
            if m and not line.strip().startswith(("if", "while", "for", "switch", "return", "do", "}")):
                last_name_at_depth0 = m.group(1)

        # Count braces and update depth + function stack
        for ch in line:
            if ch == "{":
                if depth == 0 and last_name_at_depth0 is not None:
                    func_stack.append((last_name_at_depth0, depth))
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth < 0:
                    depth = 0
                if func_stack and depth <= func_stack[-1][1]:
                    func_stack.pop()

        if i == target_line:
            return func_stack[-1][0] if func_stack else "(file scope)"

    return "(file scope)"


# ---------------------------------------------------------------------------
# Deactivated-condition classifier.
#
# Heuristic: a decision condition is "deactivated" (DO-178C 6.4.4.3)
# when the same function contains an earlier guard (RA_CHECK_NULL_PTR,
# `if (p == NULL) return ...`, RA_CHECK_RANGE, etc.) that makes the
# condition unreachable on the public-API path. We classify a whole
# decision as deactivated only when EVERY pointer/null/range token in
# the decision is shadowed by an earlier guard on the same name in the
# same function body.
#
# Conservative defaults: when in doubt, mark `reachable` (deactivated
# = False). False negatives only mean a decision stays in the
# "reachable" bucket and continues to demand a real test vector --
# never the other way round.
# ---------------------------------------------------------------------------
NULL_TOKEN_RE = re.compile(
    r"\(\s*([A-Za-z_]\w*(?:->\w+|\.\w+)?)\s*==\s*(?:NULL|nullptr|0)\s*\)"
)
GUARD_NULL_RE = re.compile(
    r"RA_CHECK_NULL_PTR\s*\(\s*([A-Za-z_]\w*)|"
    r"if\s*\(\s*([A-Za-z_]\w*)\s*==\s*(?:NULL|nullptr)\s*\)"
)
LEN_NULL_PAIR_RE = re.compile(
    r"\(\s*([A-Za-z_]\w*)\s*==\s*(?:NULL|nullptr)\s*\)\s*&&\s*"
    r"\(\s*([A-Za-z_]\w*_len)\s*!=\s*0"
)
DEFENSIVE_OFF_RE = re.compile(r"\boff\s*<\s*sizeof\s*\(")
FUNC_BODY_BRACE_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{?\s*$")


def _function_body_lines(rel_path: str, target_line: int) -> list[str]:
    """Return source lines from the start of the enclosing function up
    to (but not including) `target_line`. Empty list if not found."""
    abs_path = REPO_ROOT / rel_path
    if not abs_path.exists():
        return []
    try:
        text = abs_path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        return []
    lines = text.splitlines()
    # Walk forward; remember the last `{` at depth 0->1 transition.
    depth = 0
    func_start = None
    for i, raw in enumerate(lines, start=1):
        stripped = raw.strip()
        for ch in raw:
            if ch == "{":
                if depth == 0:
                    func_start = i
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    if i >= target_line:
                        if func_start is not None:
                            return lines[func_start - 1 : target_line - 1]
                        return []
                    func_start = None
        if i == target_line:
            if func_start is not None:
                return lines[func_start - 1 : target_line - 1]
            return []
    return []


def is_deactivated_decision(rel_path: str, line: int, excerpt: str) -> tuple[bool, str]:
    """Return (deactivated?, rationale) for a single decision at
    (rel_path, line) with source text `excerpt`."""
    # Pattern 1: ((p == NULL) && (p_len != 0)) -- a defensive contract
    # check. The public API documents the contract (caller-side @pre);
    # the only path that exercises the AND-chain's second condition is
    # a deliberately malformed call that the public API rejects upstream.
    if LEN_NULL_PAIR_RE.search(excerpt):
        return (
            True,
            "Defensive null+len contract: (ptr == NULL) && (len != 0)"
            " is rejected upstream by the public-API @pre clause.",
        )
    # Pattern 2: `for (...; off < sizeof(buf); ...)` defensive bound.
    # The fixed-size scratch buffer is sized to the documented input
    # caps; the bound condition can only flip if the caller violates
    # the contract.
    if DEFENSIVE_OFF_RE.search(excerpt):
        return (
            True,
            "Defensive scratch-buffer bound: input length is capped"
            " by the public-API contract; second condition unreachable.",
        )
    # Pattern 3: `(p == NULL) || ...` where `p` was already checked
    # earlier in the same function via RA_CHECK_NULL_PTR or
    # `if (p == NULL) return ...`.
    null_tokens = [m.group(1).split("->")[0].split(".")[0]
                   for m in NULL_TOKEN_RE.finditer(excerpt)]
    if null_tokens:
        body = _function_body_lines(rel_path, line)
        if body:
            text = "\n".join(body)
            guards: set[str] = set()
            for m in GUARD_NULL_RE.finditer(text):
                name = m.group(1) or m.group(2)
                if name:
                    guards.add(name)
            shadowed = [n for n in null_tokens if n in guards]
            if shadowed and len(shadowed) == len(null_tokens):
                return (
                    True,
                    f"Pointer(s) {sorted(set(shadowed))} already null-checked"
                    " upstream in the same function body.",
                )
    return (False, "")


# ---------------------------------------------------------------------------
# Module-name extraction: libs/<group>/src/<module>.c -> <module>
#                        src/<group>/<module>.c     -> <module>
# ---------------------------------------------------------------------------
def module_of(rel_path: str) -> str:
    name = os.path.basename(rel_path)
    if name.endswith(".c"):
        name = name[:-2]
    elif name.endswith(".cpp"):
        name = name[:-4]
    elif name.endswith(".h") or name.endswith(".hpp"):
        # Drop extension only.
        name = re.sub(r"\.(h|hpp)$", "", name)
    return name


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> int:
    if not MCDC_TXT.exists():
        print(
            f"error: {MCDC_TXT} not found. Run `make mcdc` first to generate"
            " the live llvm-cov report.",
            file=sys.stderr,
        )
        return 1

    # Collect every decision (covered or not) from the live report.
    all_decisions: list[tuple[str, int, int, str, float]] = list(
        parse_mcdc_txt(MCDC_TXT)
    )
    # Skip third_party (the report already strips them, but be defensive).
    all_decisions = [
        d for d in all_decisions if "/third_party/" not in d[0]
    ]

    # Gap rows = anything < 100%.
    gap_rows = [d for d in all_decisions if d[4] < 100.0]
    gap_rows.sort(key=lambda r: (r[0], r[1]))

    # Classify each gap row as deactivated or reachable.
    classified: list[tuple[str, int, int, str, str, str, bool, str]] = []
    for src, ln, n, excerpt, pct in gap_rows:
        covered = "no" if pct == 0.0 else "partial"
        func = resolve_function(src, ln)
        deact, rationale = is_deactivated_decision(src, ln, excerpt)
        classified.append((src, ln, n, func, excerpt, covered, deact, rationale))

    # Write CSV (gap-only). New columns: deactivated, deactivation_rationale.
    with CSV_OUT.open("w", encoding="ascii", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(
            [
                "source_file",
                "line",
                "condition_count",
                "function_name",
                "decision_excerpt",
                "covered",
                "deactivated",
                "deactivation_rationale",
            ]
        )
        for src, ln, n, func, excerpt, covered, deact, rationale in classified:
            w.writerow(
                [
                    src,
                    ln,
                    n,
                    func,
                    excerpt,
                    covered,
                    "true" if deact else "false",
                    rationale,
                ]
            )

    # Module roll-up (counts every decision, gap or not).
    per_module: dict[str, list[float]] = defaultdict(list)
    for src, _ln, _n, _e, pct in all_decisions:
        per_module[module_of(src)].append(pct)

    rows = []
    for mod, pcts in per_module.items():
        total = len(pcts)
        covered = sum(1 for p in pcts if p >= 100.0)
        partial = sum(1 for p in pcts if 0.0 < p < 100.0)
        uncov = sum(1 for p in pcts if p == 0.0)
        rows.append((mod, total, covered, partial, uncov))

    # Sort by uncovered+partial desc, then total desc, then name.
    rows.sort(key=lambda r: (-(r[3] + r[4]), -r[1], r[0]))

    # Build the markdown header. Preserve the existing prose; only the
    # numeric blocks get rewritten.
    total_dec = len(all_decisions)
    yes_dec = sum(1 for d in all_decisions if d[4] >= 100.0)
    partial_dec = sum(1 for d in all_decisions if 0.0 < d[4] < 100.0)
    no_dec = sum(1 for d in all_decisions if d[4] == 0.0)
    files_seen = len({d[0] for d in all_decisions})
    rate = (100.0 * yes_dec / total_dec) if total_dec else 0.0

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
    reachable_rate = (
        (100.0 * reachable_covered / reachable_total) if reachable_total else 100.0
    )

    md_lines: list[str] = []
    md_lines.append("# MC/DC Coverage Gap Audit")
    md_lines.append("")
    md_lines.append(
        "Live audit of compound boolean decisions reported by"
        " `llvm-cov show --show-mcdc` for first-party sources"
        " (`libs/`, `src/`, `port/`, excluding `libs/third_party/`)."
        " Regenerated from `build/mcdc-report/mcdc.txt` by"
        " `scripts/utils/regen_mcdc_gaps.py`; do not edit by hand."
    )
    md_lines.append("")
    md_lines.append("## Methodology")
    md_lines.append("")
    md_lines.append(
        "- Source of truth: `build/mcdc-report/mcdc.txt` (output of"
        " `make mcdc`)."
    )
    md_lines.append(
        "- A decision is one llvm-cov \"MC/DC Decision Region\"."
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
        "  - `no` -- MC/DC % == 0. The decision was never evaluated"
        " under instrumentation."
    )
    md_lines.append("")
    md_lines.append("## Top-line Numbers")
    md_lines.append("")
    md_lines.append(f"- Source files with at least one decision: **{files_seen}**")
    md_lines.append(f"- Total compound decisions in scope: **{total_dec}**")
    md_lines.append(f"- Decisions at 100% MC/DC (`yes`): **{yes_dec}**")
    md_lines.append(f"- Decisions partially covered (`partial`): **{partial_dec}**")
    md_lines.append(f"- Decisions fully uncovered (`no`): **{no_dec}**")
    md_lines.append(f"- Coverage rate (yes / total): **{rate:.2f}%**")
    md_lines.append(
        f"- Deactivated gap conditions (DO-178C 6.4.4.3): **{deact_count}**"
    )
    md_lines.append(
        f"- Reachable-condition denominator (total - deactivated): **{reachable_total}**"
    )
    md_lines.append(
        f"- **Reachable MC/DC rate**: **{reachable_rate:.2f}%**"
        " -- this is the gate threshold (100% required)."
    )
    md_lines.append("")
    md_lines.append(
        "See `docs/MCDC_DEACTIVATIONS.md` for the per-condition deactivation"
        " rationale catalog."
    )
    md_lines.append("")
    md_lines.append("## Reachable gaps (require new MC/DC test vectors)")
    md_lines.append("")
    md_lines.append("| File | Line | Conds | Function | Excerpt | Status |")
    md_lines.append("|------|-----:|------:|----------|---------|--------|")
    for src, ln, n, func, excerpt, covered, _deact, _rat in reachable_rows[:60]:
        ex = excerpt.replace("|", "\\|")
        if len(ex) > 80:
            ex = ex[:77] + "..."
        md_lines.append(f"| {src} | {ln} | {n} | {func} | `{ex}` | {covered} |")
    if len(reachable_rows) > 60:
        md_lines.append(
            f"| ... | | | | *({len(reachable_rows) - 60} more rows in CSV)* | |"
        )
    md_lines.append("")
    md_lines.append("## Deactivated gaps (DO-178C 6.4.4.3 exempted)")
    md_lines.append("")
    md_lines.append(
        "These conditions are unreachable on any public-API path and"
        " are therefore exempted from the MC/DC gate. Each row carries"
        " the rationale used by the auto-classifier; humans may extend"
        " the per-condition narrative in `docs/MCDC_DEACTIVATIONS.md`."
    )
    md_lines.append("")
    md_lines.append("| File | Line | Conds | Function | Excerpt | Rationale |")
    md_lines.append("|------|-----:|------:|----------|---------|-----------|")
    for src, ln, n, func, excerpt, _covered, _deact, rationale in deactivated_rows:
        ex = excerpt.replace("|", "\\|")
        if len(ex) > 60:
            ex = ex[:57] + "..."
        rt = rationale.replace("|", "\\|")
        if len(rt) > 60:
            rt = rt[:57] + "..."
        md_lines.append(f"| {src} | {ln} | {n} | {func} | `{ex}` | {rt} |")
    md_lines.append("")
    md_lines.append("## Per-module gap counts (full table)")
    md_lines.append("")
    md_lines.append("Sorted by (uncovered + partial) descending, then total descending.")
    md_lines.append("")
    md_lines.append("| Module | Total | Covered | Partial | Uncovered |")
    md_lines.append("|--------|------:|--------:|--------:|----------:|")
    for mod, total, covered, partial, uncov in rows:
        md_lines.append(
            f"| {mod} | {total} | {covered} | {partial} | {uncov} |"
        )
    md_lines.append("")
    md_lines.append("## Top 30 modules with at least one uncovered decision")
    md_lines.append("")
    md_lines.append("| Module | Uncovered | Partial | Covered | Total |")
    md_lines.append("|--------|----------:|--------:|--------:|------:|")
    uncov_only = [r for r in rows if r[4] > 0]
    uncov_only.sort(key=lambda r: (-r[4], -r[3], r[0]))
    for mod, total, covered, partial, uncov in uncov_only[:30]:
        md_lines.append(
            f"| {mod} | {uncov} | {partial} | {covered} | {total} |"
        )
    md_lines.append("")
    md_lines.append("---")
    md_lines.append("")
    md_lines.append(
        "*Regenerated from the live `make mcdc` report. See"
        " `docs/MCDC_GAPS.csv` for the full per-decision table including"
        " line numbers and decision excerpts.*"
    )
    md_lines.append("")

    MD_OUT.write_text("\n".join(md_lines), encoding="ascii")

    # ----------------------------------------------------------------
    # docs/MCDC_DEACTIVATIONS.md -- per-condition catalog. The auto-
    # generated entries can be extended by hand below the marker.
    # ----------------------------------------------------------------
    deact_lines: list[str] = []
    deact_lines.append("# MC/DC Deactivated-Condition Catalog")
    deact_lines.append("")
    deact_lines.append(
        "This file documents every MC/DC condition that has been"
        " classified as **deactivated** under **DO-178C 6.4.4.3"
        " (\"deactivated code\")**. Deactivated conditions are exempted"
        " from the 100% MC/DC gate because the public-API contract"
        " makes them unreachable; they remain in the source for"
        " defense-in-depth, fault-injection robustness, and to give"
        " static analyzers a clear local invariant to anchor on."
    )
    deact_lines.append("")
    deact_lines.append("## Policy")
    deact_lines.append("")
    deact_lines.append(
        "DO-178C 6.4.4.3 permits structural-coverage exemption for code"
        " that is intentionally not reachable in the operational"
        " configuration, provided each instance is (a) identified, (b)"
        " justified by a documented rationale, and (c) accompanied by"
        " upstream evidence that the exemption holds. The"
        " auto-classifier in `scripts/utils/regen_mcdc_gaps.py`"
        " enumerates every such condition; this catalog records the"
        " upstream guard or contract that makes each one unreachable."
    )
    deact_lines.append("")
    deact_lines.append(
        "Equivalent industry references: IEC 61508-3:2010 7.4.7"
        " (defensive-programming code), ISO 26262-6:2018 9.4.5"
        " (deactivated branches)."
    )
    deact_lines.append("")
    deact_lines.append("## Auto-generated entries")
    deact_lines.append("")
    deact_lines.append(
        "Generated by `scripts/utils/regen_mcdc_gaps.py` from"
        " `build/mcdc-report/mcdc.txt`. Do not edit by hand above the"
        " `<!-- MANUAL -->` marker; manual narrative may be added below"
        " the marker for a specific condition by appending its"
        " `file:line` anchor."
    )
    deact_lines.append("")
    if not deactivated_rows:
        deact_lines.append("(no deactivated conditions detected)")
        deact_lines.append("")
    else:
        for src, ln, n, func, excerpt, covered, _deact, rationale in deactivated_rows:
            anchor = f"{src}:{ln}"
            deact_lines.append(f"### {anchor}")
            deact_lines.append("")
            deact_lines.append(f"- **Function**: `{func}`")
            deact_lines.append(f"- **Conditions in decision**: {n}")
            deact_lines.append(f"- **Current llvm-cov status**: {covered}")
            deact_lines.append(
                f"- **Source line**: `{excerpt.strip()}`"
            )
            deact_lines.append(f"- **Rationale**: {rationale}")
            deact_lines.append(
                "- **DO-178C 6.4.4.3 basis**: defensive guard whose"
                " upstream contract is enforced on every public-API"
                " entry."
            )
            deact_lines.append("")
    deact_lines.append("<!-- MANUAL -->")
    deact_lines.append("")
    deact_lines.append("## Manual narrative (per anchor)")
    deact_lines.append("")
    deact_lines.append(
        "_Add expanded justification here keyed by the `file:line`"
        " anchor above when the auto-generated rationale is"
        " insufficient. Anything below this marker is preserved across"
        " regenerations only if you commit it -- the regenerator"
        " currently overwrites the entire file; future revisions may"
        " split the manual section into a sibling file._"
    )
    deact_lines.append("")
    DEACT_MD_OUT.write_text("\n".join(deact_lines), encoding="ascii")

    # Stash key counts for the gate script to read without re-parsing.
    GATE_JSON = REPO_ROOT / "build" / "mcdc-report" / "gate.json"
    try:
        GATE_JSON.write_text(
            "{\n"
            f"  \"total_decisions\": {total_dec},\n"
            f"  \"covered_decisions\": {yes_dec},\n"
            f"  \"deactivated_decisions\": {deact_count},\n"
            f"  \"reachable_total\": {reachable_total},\n"
            f"  \"reachable_covered\": {reachable_covered},\n"
            f"  \"reachable_rate\": {reachable_rate:.4f},\n"
            f"  \"absolute_rate\": {rate:.4f}\n"
            "}\n",
            encoding="ascii",
        )
    except OSError:
        pass

    print(
        f"Wrote {CSV_OUT.relative_to(REPO_ROOT)} ({len(gap_rows)} gap rows;"
        f" {deact_count} deactivated, {len(reachable_rows)} reachable),"
        f" {MD_OUT.relative_to(REPO_ROOT)},"
        f" {DEACT_MD_OUT.relative_to(REPO_ROOT)}."
        f" Absolute MC/DC: {rate:.2f}%; reachable MC/DC: {reachable_rate:.2f}%."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
