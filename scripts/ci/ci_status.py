#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The one reader of the ci-monitor status file.

Split out of ``scripts/ci/monitor.sh`` (its ``_status_read`` shell function): the
reader had outgrown what a shell heredoc should carry, and holding the verdict
logic in a real module makes it lintable and directly testable. ``monitor.sh``
invokes it as ``python3 ci_status.py <state-file> <mode> [arg]`` and every field
and rendered view the tool prints comes from here -- one reader, one place.

The verdict rules encode two hard-won distinctions (see the matching notes in
monitor.sh):

* SKIPPED IS NOT SUCCESS -- an all-skipped sha ran no gate, so it is UNKNOWN,
  never PASS (#530).
* CANCELLED IS NOT FAILURE -- a superseded run is a non-result, not a red; a
  workflow is judged by its latest run that actually concluded (#561).

The daemon never calls this module (it computes its head verdict inline while
polling), so the stand-alone copy install-service deploys does not need it.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

Run = dict[str, object]

FAIL_CONC = {"failure", "timed_out"}
HEAD_ROWS = 6
SHA_ABBREV = 9


def matching(runs: list[Run], sha: str) -> list[Run]:
    """Return the runs whose sha starts with `sha`."""
    return [r for r in runs if str(r.get("sha") or "").startswith(sha)]


def render(run: Run, with_sha: bool = False) -> str:
    """Render one run as an indented ``name: status/conclusion`` line."""
    tail = "  " + str(run.get("sha") or "")[:SHA_ABBREV] if with_sha else ""
    return f"  {run.get('name')}: {run.get('status')}/{run.get('conclusion') or '-'}{tail}"


def _workflow_verdict(wf_runs: list[Run]) -> str:
    """Verdict for one workflow's runs of a sha: PASS / FAIL / RUNNING / NORESULT.

    The latest run that actually concluded success or failure decides it, so a
    re-run that succeeded clears an earlier failure and a superseded ``cancelled``
    (or a ``skipped``) run never overrides a real conclusion. Only cancelled /
    skipped runs are a NORESULT; a run still in flight keeps it RUNNING.
    """
    decisive = sorted(
        (
            r
            for r in wf_runs
            if r.get("conclusion") == "success" or r.get("conclusion") in FAIL_CONC
        ),
        key=lambda r: str(r.get("created") or ""),
    )
    if decisive:
        return "PASS" if decisive[-1].get("conclusion") == "success" else "FAIL"
    if any(r.get("status") != "completed" for r in wf_runs):
        return "RUNNING"
    return "NORESULT"


def verdict(runs: list[Run], sha: str) -> str:
    """Aggregate a sha's per-workflow verdicts into PASS / FAIL / UNKNOWN."""
    got = matching(runs, sha)
    if not got:
        return "UNKNOWN"
    by_wf: dict[object, list[Run]] = {}
    for r in got:
        by_wf.setdefault(r.get("name"), []).append(r)
    wf_verdicts = [_workflow_verdict(wf_runs) for wf_runs in by_wf.values()]
    if "FAIL" in wf_verdicts:
        return "FAIL"
    if "RUNNING" in wf_verdicts:
        return "UNKNOWN"
    if "PASS" in wf_verdicts:
        return "PASS"
    # every workflow was cancelled/skipped -- nothing actually ran
    return "UNKNOWN"


def _conclusion_count(runs: list[Run], sha: str, conclusion: str) -> int:
    """Count the sha's runs whose conclusion equals `conclusion`."""
    return sum(1 for r in matching(runs, sha) if r.get("conclusion") == conclusion)


def main(argv: list[str]) -> int:
    """Dispatch one read mode against the status file named in argv[1]."""
    path, mode, *rest = argv[1:]
    arg = rest[0] if rest else ""
    with Path(path).open(encoding="utf-8") as fh:
        doc = json.load(fh)
    runs = doc.get("runs") or []

    if mode == "field":
        print(doc.get(arg) or "")
    elif mode == "count":
        print(len(matching(runs, arg)))
    elif mode == "verdict":
        print(verdict(runs, arg))
    elif mode == "skipped-count":
        print(_conclusion_count(runs, arg, "skipped"))
    elif mode == "cancelled-count":
        print(_conclusion_count(runs, arg, "cancelled"))
    elif mode == "lines-sha":
        for r in matching(runs, arg):
            print(render(r))
    elif mode == "lines-head":
        for r in runs[:HEAD_ROWS]:
            print(render(r, with_sha=True))
    else:
        sys.exit("unknown mode: " + mode)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
