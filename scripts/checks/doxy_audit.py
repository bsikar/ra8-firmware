#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Doxygen documentation gap auditor for ra8-firmware.

Walks every .c/.h under libs/, src/, port/ and tools/ (excluding vendored and
generated code),
locates every function definition/prototype, and checks the immediately
preceding Doxygen block for the required tags listed in CLAUDE.md
("Doxygen Documentation Requirements").

The pre-existing function backlog is closed. Every function gap now fails
immediately; no documentation baseline or update path remains. Machine-emitted
tool code remains exempt like vendored SOUP.

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
  --style           ENFORCING gate for the two docs/STYLE_GUIDE.md tag rules
                    that attach to no symbol, so neither the function nor the
                    member gate ever saw them (#532): the file-header block
                    (@file present and naming this file, @brief, @details) and
                    the @param direction bracket. Unlike --members there is no
                    report-only twin -- a mode that measures instead of failing
                    is how the member gate was once mis-wired.

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

Exit 0 when clean or report-only, 1 when an enforcing mode found gaps, 2 when
a scope walk collapsed below its floor (doxy_scope.FUNCTION_FILE_FLOOR /
MEMBER_FILE_FLOOR).
"""

from __future__ import annotations

import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from doxy_functions import audit_file
from doxy_members import audit_members_file
from doxy_report import run_report
from doxy_scope import _top_dir, function_files, member_files, repo_root
from doxy_selftest import run_selftest
from doxy_style import run_check as run_style_check

#: Offender lines printed before the gate truncates, so a hook stays readable.
OFFENDER_CAP = 50


def run_check() -> int:
    """Strict gate: exit 0 if zero gaps, else exit 1 with offender list.

    Used by the pre-commit hook to keep documentation debt from growing.
    Does not write CSV/MD outputs -- read-only audit.

    Scope comes from :func:`doxy_scope.function_files`, which exits 2 rather
    than let a collapsed walk report ``gaps=0 (PASS)``.
    """
    all_rows = []
    for path in function_files():
        all_rows.extend(audit_file(path))

    gap_rows = [r for r in all_rows if r[3]]
    if not gap_rows:
        print("doxy_audit --check: gaps=0 (PASS; strict, no baseline)")
        return 0

    print(f"doxy_audit --check: gaps={len(gap_rows)} (FAIL; strict, no baseline)")
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

    "Never fails" covers findings, not infrastructure:
    :func:`doxy_scope.member_files` still exits 2 on a collapsed repo-wide
    walk, since a report sized from nothing is worse than no report.
    """
    all_rows = []
    for p in member_files(explicit):
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

    Scope comes from :func:`doxy_scope.member_files`, which exits 2 rather than
    let a collapsed repo-wide walk report ``offenders=0 (PASS)``.
    """
    all_rows = []
    for p in member_files(explicit):
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
    """Dispatch to the function gate, the member gate, the style gate, or a report.

    The two member modes are NOT interchangeable: ``--members`` alone is
    report-only and always exits 0 (it exists to size the #246 fallout), while
    ``--members --check`` is the enforcing gate. CI must pass both flags, or
    the step measures the problem instead of failing on it. ``--style`` has no
    such pair on purpose -- it is always enforcing, so there is no spelling of
    it that measures instead of failing.

    Returns 0 on a clean gate, a passing selftest, or any report-only run;
    1 when an enforcing mode found offenders. Exits 2 when a mode could not
    run at all -- either scope walk collapsing below its measured file floor
    (:mod:`doxy_scope`), or ``--style``'s file / ``@param`` floors
    (:mod:`doxy_style`). A scan that read nothing must never report a
    documented tree.
    """
    args = sys.argv[1:]
    unknown = [
        arg
        for arg in args
        if arg.startswith("--")
        and arg not in {"--check", "--members", "--selftest", "--style"}
        and not arg.startswith("--out=")
    ]
    if unknown:
        sys.stderr.write(f"doxy_audit: unknown option(s): {' '.join(unknown)}\n")
        return 2
    if "--selftest" in args:
        return run_selftest()
    if "--style" in args:
        return run_style_check()
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
