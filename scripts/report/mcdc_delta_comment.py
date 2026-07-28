#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Render the per-file MC/DC delta comment posted on a pull request.

The PR job builds an MC/DC report for the head commit and, best-effort, one
for the base branch; this renders the markdown table comparing them.

It exists as a script rather than as inline workflow JavaScript because of the
failure it is written to prevent.  The delta used to be computed by ~70 lines
of JS inside an ``actions/github-script`` step whose loader treated a missing
base report as an empty result::

    if (!fs.existsSync(path)) return new Map();

The base build is deliberately best-effort (``continue-on-error``), so when it
failed every row's delta rendered as ``n/a`` -- and a reviewer saw a posted,
complete-looking MC/DC delta table and concluded the change was clean, when no
comparison had been performed at all (#536).  That is the gate-honesty defect
class in its most consequential form: not a gate that silently passes, but one
that silently produces EVIDENCE nobody checked.

Two changes make that impossible here:

  * **An unavailable base is stated, not implied.**  When the base report is
    missing, empty or explicitly flagged unavailable, the body says so in a
    banner and emits NO delta table.  A reader cannot mistake absence of
    comparison for absence of regression, because there is no table to
    misread.
  * **The logic is in scope for the parity guard.**  ``check_ci_parity.py``
    only inspects workflow steps with a ``run:`` key, so the JS in a ``uses:``
    step was outside the very guard that exists to stop check logic growing a
    second home in YAML.  Under ``scripts/`` it is back in scope, and it has a
    ``--selftest``.

Run::

    mcdc_delta_comment.py --pr pr-mcdc/summary.txt --base base-summary.txt \
        --base-status base-summary.status --out mcdc-delta-comment.md
    mcdc_delta_comment.py --selftest

Exit 0 on success, 2 on a failed selftest or an unreadable PR report.  The PR
report is required: if THAT is missing the job has nothing to say and must
fail rather than post an empty comment.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

# Marker the workflow uses to find and update its own prior comment.
COMMENT_MARKER = "<!-- mcdc-delta-comment -->"

# Rows shown, sorted by delta with regressions first, so the comment stays
# readable on a large change.
MAX_ROWS = 50

# A percentage column in an llvm-cov summary row.
PCT_RE = re.compile(r"^[0-9]+(\.[0-9]+)?%$")

# A data row needs at least a path and one measurement column.
MIN_ROW_COLUMNS = 2

# Header / rule / total lines that are not per-file data.
SKIP_PREFIXES = ("---", "Filename", "TOTAL")

EXIT_OK = 0
EXIT_VACUOUS = 2


def load_summary(path: Path) -> dict[str, float]:
    """Parse an llvm-cov summary into ``file -> MC/DC percent``.

    A typical row is ``path/to/file.c 1 2 50.00% 3 4 75.00% ... 12.34%``: the
    first whitespace-delimited token is the path and the last percent-suffixed
    token is the MC/DC figure.

    Args:
        path: Summary file to read.

    Returns:
        One entry per data row; empty when the file does not exist.
    """
    out: dict[str, float] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        trimmed = line.strip()
        if not trimmed or trimmed.startswith(SKIP_PREFIXES):
            continue
        cols = trimmed.split()
        if len(cols) < MIN_ROW_COLUMNS:
            continue
        pcts = [col for col in cols if PCT_RE.match(col)]
        if not pcts:
            continue
        out[cols[0]] = float(pcts[-1].rstrip("%"))
    return out


def base_is_available(base: dict[str, float], status: str | None) -> bool:
    """Report whether a real base measurement exists to compare against.

    Args:
        base: Parsed base summary.
        status: Contents of the base status file, or None when absent.

    Returns:
        True only when the base build reported success AND produced rows.
        Either half alone is insufficient: an empty summary and a failed build
        are the same thing to a reader, and both must suppress the table.
    """
    if status is not None and status.strip() != "ok":
        return False
    return bool(base)


def _unavailable_body(reason: str, pr_files: int) -> str:
    """Build the body used when no comparison could be performed.

    Args:
        reason: Human-readable cause.
        pr_files: Number of files measured on the PR head.

    Returns:
        The markdown body, with no delta table.
    """
    return (
        "## MC/DC delta (PR vs base)\n\n"
        "**No comparison was performed.** The base-branch MC/DC report is "
        f"unavailable ({reason}).\n\n"
        f"The PR head measured {pr_files} file(s), but there is nothing to "
        "diff it against, so this comment states no verdict about "
        "regressions. It is NOT evidence that MC/DC held.\n\n"
        "The base build is best-effort; re-run the `MC/DC delta comment` job "
        "if the delta is needed.\n"
    )


def _table_body(pr: dict[str, float], base: dict[str, float]) -> str:
    """Build the body containing the per-file delta table.

    Args:
        pr: Parsed PR-head summary.
        base: Parsed base-branch summary.

    Returns:
        The markdown body.
    """
    rows = []
    for name in set(pr) | set(base):
        before = base.get(name)
        after = pr.get(name)
        delta = (after - before) if (before is not None and after is not None) else None
        rows.append((name, before, after, delta))
    rows.sort(key=lambda row: float("-inf") if row[3] is None else row[3])

    def fmt(value: float | None) -> str:
        return "n/a" if value is None else f"{value:.2f}%"

    body = "## MC/DC delta (PR vs base)\n\n"
    body += f"Showing up to {MAX_ROWS} files sorted by delta (regressions first).\n\n"
    body += "| file | base | PR | delta |\n|---|---|---|---|\n"
    shown = rows[:MAX_ROWS]
    for name, before, after, delta in shown:
        sign = "" if delta is None or delta <= 0 else "+"
        body += f"| `{name}` | {fmt(before)} | {fmt(after)} | {sign}{fmt(delta)} |\n"
    if not shown:
        body += "| _no rows_ | | | |\n"
    return body


def render(pr: dict[str, float], base: dict[str, float], status: str | None) -> str:
    """Render the full comment body for the given reports.

    Args:
        pr: Parsed PR-head summary.
        base: Parsed base-branch summary.
        status: Contents of the base status file, or None when absent.

    Returns:
        The markdown body, prefixed with `COMMENT_MARKER`.
    """
    if base_is_available(base, status):
        body = _table_body(pr, base)
    else:
        reason = (
            "the base build failed"
            if status is not None and status.strip() != "ok"
            else "the base report is missing or measured nothing"
        )
        body = _unavailable_body(reason, len(pr))
    return COMMENT_MARKER + "\n" + body


def _read_status(path: Path | None) -> str | None:
    """Return the base status text, or None when no status file was given.

    Args:
        path: Status file path, or None.

    Returns:
        The file's text, or None when absent.
    """
    if path is None or not path.is_file():
        return None
    return path.read_text(encoding="utf-8")


def _selftest_cases(root: Path) -> list[tuple[str, bool]]:
    """Build every selftest assertion against fixtures written under `root`.

    Args:
        root: Temporary directory to materialise the two summaries in.

    Returns:
        ``(label, passed)`` pairs, both directions covered.
    """
    pr_file = root / "pr.txt"
    pr_file.write_text(
        "Filename  Regions  Miss  Cover  MC/DC\nlibs/a.c  10  1  90.00%  75.00%\n",
        encoding="utf-8",
    )
    base_file = root / "base.txt"
    base_file.write_text(
        "Filename  Regions  Miss  Cover  MC/DC\nlibs/a.c  10  1  90.00%  50.00%\n",
        encoding="utf-8",
    )
    pr = load_summary(pr_file)
    base = load_summary(base_file)
    good = render(pr, base, "ok\n")
    failed = render(pr, base, "unavailable\n")
    empty = render(pr, {}, None)
    absent = "No comparison was performed"
    return [
        ("the PR summary parses to one row", pr == {"libs/a.c": 75.0}),
        ("MUST NOT FIRE: a real base renders a delta table", "| delta |" in good),
        ("a real base computes the delta", "+25.00%" in good),
        ("a real base does not claim unavailability", absent not in good),
        ("MUST FIRE: a FAILED base build states no comparison happened", absent in failed),
        ("MUST FIRE: a failed base emits NO delta table", "| delta |" not in failed),
        ("MUST FIRE: a failed base emits no n/a rows to be misread", "n/a" not in failed),
        ("a failed base says it is not evidence MC/DC held", "NOT evidence" in failed),
        (
            "MUST FIRE: a MISSING base report also states no comparison",
            absent in empty and "| delta |" not in empty,
        ),
        (
            "every body carries the update marker",
            all(b.startswith(COMMENT_MARKER) for b in (good, failed, empty)),
        ),
        (
            "a missing summary file parses to nothing rather than raising",
            load_summary(root / "absent.txt") == {},
        ),
    ]


def selftest() -> int:
    """Prove an unavailable base suppresses the table, and a real one keeps it.

    The must-fire direction here is unusual and is the whole point: the defect
    was a body that looked complete. So the assertions are about what the body
    must NOT contain when no comparison happened.

    Returns:
        ``EXIT_OK`` when every case holds, ``EXIT_VACUOUS`` otherwise.
    """
    with tempfile.TemporaryDirectory() as tmp:
        cases = _selftest_cases(Path(tmp))

    for label, ok in cases:
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
    if not all(ok for _, ok in cases):
        print("mcdc_delta_comment.py: selftest FAILED", file=sys.stderr)
        return EXIT_VACUOUS
    print(f"mcdc_delta_comment.py: selftest passed ({len(cases)} cases, both directions).")
    return EXIT_OK


def main(argv: list[str]) -> int:
    """Parse arguments and write the rendered comment body.

    Returns:
        0 on success, 2 on a failed selftest or an unreadable PR report.
    """
    parser = argparse.ArgumentParser(description="render the PR MC/DC delta comment body")
    parser.add_argument("--pr", type=Path, help="PR-head llvm-cov summary")
    parser.add_argument("--base", type=Path, help="base-branch llvm-cov summary")
    parser.add_argument("--base-status", type=Path, help="base build status file")
    parser.add_argument("--out", type=Path, help="markdown body to write")
    parser.add_argument("--selftest", action="store_true", help="prove both directions, then exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if args.pr is None or args.out is None:
        parser.error("--pr and --out are required unless --selftest is given")
    if not args.pr.is_file():
        print(
            f"mcdc_delta_comment.py: FATAL -- the PR report '{args.pr}' is missing. "
            "There is nothing to report; refusing to post an empty comment.",
            file=sys.stderr,
        )
        return EXIT_VACUOUS

    pr = load_summary(args.pr)
    base = load_summary(args.base) if args.base is not None else {}
    body = render(pr, base, _read_status(args.base_status))
    args.out.write_text(body, encoding="utf-8")
    available = base_is_available(base, _read_status(args.base_status))
    verdict = "delta table" if available else "UNAVAILABLE banner"
    print(f"mcdc_delta_comment.py: wrote {args.out} ({len(pr)} PR file(s), {verdict})")
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
