#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Doxygen documentation gap auditor for ra8-firmware.

Walks every .c/.h under libs/, src/, port/ (excluding libs/third_party/),
locates every function definition/prototype, and checks the immediately
preceding Doxygen block for the required tags listed in CLAUDE.md
("Doxygen Documentation Requirements").

KNOWN SCOPE GAP -- this function gate does NOT cover examples/, tools/ or
tests/, which CLAUDE.md ("these standards apply to EVERY first-party file")
says it should. ``--members`` already covers them (MEMBER_SCAN_DIRS); the
function mode does not. Measured on the current tree, adding all three to
SCAN_DIRS reports 9635 gaps -- tests/ 7052, examples/ 1500, tools/ 1083 --
across roughly a thousand functions in tools/ alone. Only 52 of the tools/
gaps are functions with no block at all; the rest carry a block that is
missing required tags, so closing them means writing real ``@pre`` / ``@post``
/ ``@retval`` / ``@note`` prose per function. That is a documentation
campaign, tracked in its own issue, and it must be closed by writing real
documentation, never by generating tag-shaped filler -- which is exactly what
the required-tag list rewards if applied mechanically.

Modes
-----
  (no args)         Function audit report -> docs/DOXYGEN_GAPS.csv + .md
  --check           Strict function gate (exit 1 on any gap). Wired into CI
                    and the pre-commit hook.
  --selftest        Regression-test the auditor itself, in both directions,
                    for both enforcing modes. Runs before the real check in
                    the gate: a parser-driven gate that stops recognising a
                    construct reports nothing and looks like a documented
                    tree.
  --members         REPORT-ONLY member audit. Enumerates every undocumented
                    enum value, struct/union member, and macro across the
                    first-party tree and prints an offender count per top-level
                    dir. This mode never fails the build. Optionally takes
                    explicit file paths to audit just those, and --out=PATH to
                    dump the full offender list as CSV.
  --members --check ENFORCING member gate (exit 1 on any undocumented enum
                    value, struct/union member, or macro). Wired into CI and
                    the pre-commit hook alongside the function gate (issue
                    #246). Optionally takes explicit file paths to gate just
                    those.

CLAUDE.md ("Doxygen Documentation Requirements") demands that *every* enum
value, struct/union member, and macro carry documentation -- an inline
``/**< ... */`` (or a preceding ``/** ... */`` block) on each aggregate member
and a ``@brief``/``@def`` block on each macro. ``--members --check`` enforces
exactly that (issue #246); plain ``--members`` is the sizing report.

Output:
  - docs/DOXYGEN_GAPS.csv  full row-per-function table
  - docs/DOXYGEN_GAPS.md   human-readable summary
  - (--members) stdout summary; optional --out=PATH member-gap CSV

Audit-only: never edits source files.
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from doxy_functions import audit_file
from doxy_members import audit_members_file
from doxy_report import run_report
from doxy_scope import _iter_member_files, _top_dir, iter_function_files, repo_root
from doxy_selftest import run_selftest

#: Offender lines printed before the gate truncates, so a hook stays readable.
OFFENDER_CAP = 50


def run_check() -> int:
    """Strict gate: exit 0 if zero gaps, else exit 1 with offender list.

    Used by the pre-commit hook to keep documentation coverage at 100%.
    Does not write CSV/MD outputs -- read-only audit.
    """
    all_rows = []
    for path in iter_function_files():
        all_rows.extend(audit_file(path))

    gap_rows = [r for r in all_rows if r[3]]
    if not gap_rows:
        print("doxy_audit --check: gaps=0 (PASS)")
        return 0

    print(f"doxy_audit --check: gaps={len(gap_rows)} (FAIL)")
    print("Offending functions (file:line  function  -- missing tags):")
    # cap output to 50 lines so the hook stays readable
    cap = OFFENDER_CAP
    for src, line, name, missing, _sev in gap_rows[:cap]:
        print(f"  {src}:{line}  {name}  --  {';'.join(missing)}")
    if len(gap_rows) > cap:
        print(f"  ... and {len(gap_rows) - cap} more")
    print()
    print("Refresh the audit report by running:")
    print("  python3 scripts/checks/doxy_audit.py")
    return 1


def run_members_report(explicit: list[str], out_csv: str | None) -> int:
    """REPORT-ONLY member/enum/macro audit. Always returns 0.

    Enumerates every undocumented enum value, struct/union member, and macro
    across the first-party tree and prints an offender count per top-level dir
    so the owner can size the fallout wave (issue #246). This mode intentionally
    never fails: promoting the member checks to a hard gate is a follow-up.
    """
    all_rows = []
    for p in _iter_member_files(explicit):
        all_rows.extend(audit_members_file(p))

    by_dir_kind = Counter((_top_dir(r[0]), r[2]) for r in all_rows)
    by_dir = Counter(_top_dir(r[0]) for r in all_rows)
    by_kind = Counter(r[2] for r in all_rows)
    by_file = Counter(r[0] for r in all_rows)

    if out_csv is not None:
        out_path = Path(out_csv)
        if not out_path.is_absolute():
            out_path = repo_root() / out_path
        with out_path.open("w", encoding="ascii") as f:
            f.write("source_file,line,element,name,reason\n")
            for rel, line, kind, name, reason in sorted(all_rows, key=lambda r: (r[0], r[1])):
                f.write(f"{rel},{line},{kind},{name},{reason}\n")

    kinds = ["enum-value", "member", "macro"]
    dirs = sorted(by_dir)

    print("doxy_audit --members: REPORT-ONLY member/enum/macro documentation audit")
    print("(this mode never fails the build; see issue #246)")
    print()
    print("Undocumented offenders per top-level dir:")
    header = f"  {'dir':<12}" + "".join(f"{k:>13}" for k in kinds) + f"{'total':>10}"
    print(header)
    print("  " + "-" * (len(header) - 2))
    for d in dirs:
        cells = "".join(f"{by_dir_kind[(d, k)]:>13}" for k in kinds)
        print(f"  {d:<12}{cells}{by_dir[d]:>10}")
    totals = "".join(f"{by_kind[k]:>13}" for k in kinds)
    print("  " + "-" * (len(header) - 2))
    print(f"  {'TOTAL':<12}{totals}{len(all_rows):>10}")
    print()
    print(f"Files scanned with at least one offender: {len(by_file)}")
    print("Worst 15 files by offender count:")
    for rel, cnt in by_file.most_common(15):
        print(f"  {cnt:>6}  {rel}")
    if out_csv is not None:
        print()
        print(f"Full offender list written to: {out_csv}")
    return 0


def run_members_check(explicit: list[str]) -> int:
    """ENFORCING member gate: exit 0 if zero offenders, else exit 1.

    Enforces the CLAUDE.md rule that every enum value, struct/union member, and
    macro carries documentation (issue #246). Wired into the pre-commit hook and
    CI alongside the function gate. Read-only -- writes no CSV/MD outputs.
    """
    all_rows = []
    for p in _iter_member_files(explicit):
        all_rows.extend(audit_members_file(p))
    all_rows.sort(key=lambda r: (r[0], r[1]))

    if not all_rows:
        print("doxy_audit --members --check: offenders=0 (PASS)")
        return 0

    by_kind = Counter(r[2] for r in all_rows)
    print(f"doxy_audit --members --check: offenders={len(all_rows)} (FAIL)")
    print(
        "  by kind: "
        + ", ".join(f"{k}={by_kind[k]}" for k in ("enum-value", "member", "macro") if by_kind[k])
    )
    print("Undocumented members (file:line  kind  name  -- reason):")
    cap = OFFENDER_CAP
    for rel, line, kind, name, reason in all_rows[:cap]:
        print(f"  {rel}:{line}  {kind}  {name}  --  {reason}")
    if len(all_rows) > cap:
        print(f"  ... and {len(all_rows) - cap} more")
    print()
    print("Every enum value, struct/union member, and macro needs a doc comment:")
    print("  - aggregate members: an inline /**< ... */ or a preceding /** ... */ block")
    print("  - macros: a preceding /** @brief ... */ (or @def) block")
    print("Size the full fallout with: python3 scripts/checks/doxy_audit.py --members")
    return 1


def _parse_members_args(argv: list[str]) -> tuple[list[str], str | None]:
    """Split --members argv into (explicit_paths, out_csv)."""
    explicit = []
    out_csv = None
    for a in argv:
        if a == "--members":
            continue
        if a.startswith("--out="):
            out_csv = a[len("--out=") :]
        elif not a.startswith("--"):
            explicit.append(a)
    return explicit, out_csv


def main() -> int:
    """Dispatch to the function gate, the member gate, or one of the report modes.

    The two member modes are NOT interchangeable: ``--members`` alone is
    report-only and always exits 0 (it exists to size the #246 fallout), while
    ``--members --check`` is the enforcing gate. CI must pass both flags, or
    the step measures the problem instead of failing on it.

    Returns 0 on a clean gate, a passing selftest, or any report-only run;
    1 when an enforcing mode found offenders.
    """
    args = sys.argv[1:]
    if "--selftest" in args:
        return run_selftest()
    if "--members" in args:
        explicit, out_csv = _parse_members_args(args)
        return (
            run_members_check(explicit)
            if "--check" in args
            else run_members_report(explicit, out_csv)
        )
    if "--check" in args:
        return run_check()
    return run_report()


if __name__ == "__main__":
    sys.exit(main())
