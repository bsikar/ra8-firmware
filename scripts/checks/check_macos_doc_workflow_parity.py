#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the macOS page's claims about the macOS workflow agree with it.

The defect class
----------------
``docs/MACOS_HOST_BUILDS.md`` is the page a reader consults *before* spending
an Apple silicon machine on this lane, and it restates facts that live in
``.github/workflows/macos-host.yml``: which runner the job takes, when the
clock fires, which gate the job runs, and whether either trigger can fire at
all.  Every one of those is a second copy, and nothing in this tree compared
them.  A workflow edit therefore leaves the page asserting yesterday's
arrangement in confident prose, which is worse than saying nothing: the page
is the only thing standing between a reader and a wasted afternoon on the one
machine in the suite nobody can rent by the minute.

The instance that produced this check (#899) was the whole closing section.
The page said the workflow "runs the ``macos-host-build`` gate nightly ... and
can be started by hand with ``workflow_dispatch``".  Neither is true while the
file is off the default branch: GitHub runs ``schedule`` from the latest commit
on the default branch only, and offers ``workflow_dispatch`` only for workflows
that exist there.  ``macos-host.yml`` reaches ``zig/dev`` through the #899
stack and ``main`` later or never, so merging the stack starts no nightly and
produces no Run workflow button.  The workflow file itself carries that caveat
in a comment; the page told the reader to wait for a run that cannot start.

Four rules
----------
1. **runner** -- every ``macos-<n>`` runner label the page names must be the
   label the workflow actually takes.  The page's advice about Apple silicon is
   only true for an arm64 image, and ``macos-13`` is x86_64.
2. **clock** -- every ``HH:MM UTC`` time and every quoted five-field cron on the
   page must agree with the workflow's ``schedule``.  A page naming the wrong
   hour sends someone to read a log that does not exist yet.
3. **caveat** -- if the workflow declares ``schedule`` or ``workflow_dispatch``,
   both the workflow file and the page's clock section must say plainly that
   neither fires off the default branch.  This is the rule the defect above
   needs; the phrase is checked, not the sentiment, so deleting the caveat is a
   finding rather than a silent regression.
4. **gate** -- the gate the workflow runs must be named on the page and
   registered in ``scripts/ci.sh``.  A renamed gate otherwise leaves the page's
   manual-run runbook pointing at a recipe that answers "unknown gate".

Scope, and what is deliberately not checked
-------------------------------------------
Only this page and this workflow.  The rules read facts out of the workflow and
hold the prose to them, never the other way round: the workflow is the source
of truth, the page is the copy.  Nothing here asserts the nightly has run, or
can; that is exactly the claim rule 3 exists to keep off the page.

Non-vacuity
-----------
``--selftest`` builds a fixture violating each rule and asserts that rule fires
and no other, builds the compliant form and asserts silence, and asserts
against the live tree that the workflow really declares a cron, a ``macos-``
runner and a gate, so a green scan cannot come from finding nothing to read.

    python3 scripts/checks/check_macos_doc_workflow_parity.py --selftest
    python3 scripts/checks/check_macos_doc_workflow_parity.py
    python3 scripts/checks/check_macos_doc_workflow_parity.py --roster
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
DOC = REPO_ROOT / "docs" / "MACOS_HOST_BUILDS.md"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "macos-host.yml"
CI_SH = REPO_ROOT / "scripts" / "ci.sh"

CLOCK_HEADING = "## What runs on a clock"
CAVEAT_PHRASE = "default branch"

# A runner label, not the tail of a target triple or an archive name:
# "zig-aarch64-macos-0.14.1.tar.xz" carries "macos-0" and is not a runner.
RUNNER_RE = re.compile(r"(?<![A-Za-z0-9_.-])macos-(\d+)(?![\d.])")
UTC_TIME_RE = re.compile(r"\b(\d{1,2}):(\d{2})\s+UTC\b")
CRON_RE = re.compile(r"`([-\d*,/]+(?:\s+[-\d*,/]+){4})`")
GATE_RUN_RE = re.compile(r"just\s+quality::local::gate\s+([A-Za-z0-9_.:-]+)")


# ---------------------------------------------------------------- workflow


def workflow_runner(text: str) -> str | None:
    """The ``runs-on`` label of the workflow's single job, if it has one."""
    doc = yaml.safe_load(text) or {}
    jobs = doc.get("jobs") or {}
    for job in jobs.values():
        if isinstance(job, dict) and isinstance(job.get("runs-on"), str):
            return job["runs-on"]
    return None


def workflow_triggers(text: str) -> set[str]:
    """Trigger names the workflow declares.

    ``on:`` is parsed by PyYAML as the boolean ``True`` (YAML 1.1), so both
    spellings are accepted rather than requiring the file to quote the key.
    """
    doc = yaml.safe_load(text) or {}
    triggers = doc.get("on", doc.get(True))
    if isinstance(triggers, dict):
        return {str(name) for name in triggers}
    if isinstance(triggers, list):
        return {str(name) for name in triggers}
    if isinstance(triggers, str):
        return {triggers}
    return set()


def workflow_crons(text: str) -> list[str]:
    """Every cron expression under the workflow's ``schedule`` trigger."""
    doc = yaml.safe_load(text) or {}
    triggers = doc.get("on", doc.get(True))
    if not isinstance(triggers, dict):
        return []
    schedule = triggers.get("schedule") or []
    if not isinstance(schedule, list):
        return []
    out: list[str] = []
    for entry in schedule:
        if isinstance(entry, dict) and isinstance(entry.get("cron"), str):
            out.append(entry["cron"].strip())
    return out


def workflow_gates(text: str) -> list[str]:
    """Gate names the workflow invokes through the local gate recipe."""
    return GATE_RUN_RE.findall(text)


def cron_hhmm(cron: str) -> tuple[int, int] | None:
    """``(hour, minute)`` for a cron that fires at one fixed time, else None."""
    fields = cron.split()
    if len(fields) != 5:
        return None
    minute, hour = fields[0], fields[1]
    if not minute.isdigit() or not hour.isdigit():
        return None
    return int(hour), int(minute)


# --------------------------------------------------------------------- doc


def clock_section(doc_text: str) -> str | None:
    """The page's closing clock section, heading included."""
    start = doc_text.find(CLOCK_HEADING)
    if start < 0:
        return None
    rest = doc_text[start + len(CLOCK_HEADING) :]
    nxt = rest.find("\n## ")
    return CLOCK_HEADING + (rest if nxt < 0 else rest[:nxt])


def doc_runner_labels(doc_text: str) -> set[str]:
    """Every ``macos-<n>`` runner label the page names."""
    return {f"macos-{n}" for n in RUNNER_RE.findall(doc_text)}


# ------------------------------------------------------------------- rules


def scan(doc_text: str, wf_text: str, ci_text: str) -> list[str]:
    """Findings, one string each; empty means the page agrees with the workflow."""
    findings: list[str] = []
    runner = workflow_runner(wf_text)
    triggers = workflow_triggers(wf_text)
    crons = workflow_crons(wf_text)
    gates = workflow_gates(wf_text)

    # Rule 1 -- runner.
    named = doc_runner_labels(doc_text)
    if runner is not None:
        for label in sorted(named - {runner}):
            findings.append(
                f"runner: the page names the runner '{label}', but "
                f"{_rel(WORKFLOW)} runs on '{runner}'. The page's Apple silicon "
                "advice is only true for the image the job actually takes."
            )

    # Rule 2 -- clock.
    section = clock_section(doc_text)
    if crons:
        hhmm = cron_hhmm(crons[0])
        for hour, minute in _doc_times(doc_text):
            if hhmm is not None and (hour, minute) != hhmm:
                findings.append(
                    f"clock: the page says {hour:02d}:{minute:02d} UTC, but "
                    f"{_rel(WORKFLOW)} is scheduled at "
                    f"{hhmm[0]:02d}:{hhmm[1]:02d} UTC (cron '{crons[0]}')."
                )
        for quoted in CRON_RE.findall(doc_text):
            if quoted.split() != crons[0].split():
                findings.append(
                    f"clock: the page quotes the cron '{quoted}', but "
                    f"{_rel(WORKFLOW)} schedules '{crons[0]}'."
                )

    # Rule 3 -- caveat.
    if triggers & {"schedule", "workflow_dispatch"}:
        if CAVEAT_PHRASE not in wf_text:
            findings.append(
                f"caveat: {_rel(WORKFLOW)} declares "
                f"{sorted(triggers & {'schedule', 'workflow_dispatch'})} but never says "
                "that neither trigger fires while the file is off the default "
                "branch. GitHub runs 'schedule' from the default branch only and "
                "lists 'workflow_dispatch' only for workflows that exist there."
            )
        if section is None:
            findings.append(
                f"caveat: {_rel(DOC)} has no '{CLOCK_HEADING}' section, so the "
                "page cannot carry the default-branch caveat for the triggers "
                f"{_rel(WORKFLOW)} declares."
            )
        elif CAVEAT_PHRASE not in section:
            findings.append(
                f"caveat: '{CLOCK_HEADING}' in {_rel(DOC)} describes triggers "
                "without saying they do not fire off the default branch. A "
                "reader is then waiting for a nightly that cannot start."
            )

    # Rule 4 -- gate.
    for gate in sorted(set(gates)):
        if gate not in doc_text:
            findings.append(
                f"gate: {_rel(WORKFLOW)} runs the gate '{gate}', which "
                f"{_rel(DOC)} never names, so the page's manual-run runbook "
                "cannot be checked against it."
            )
        if gate not in ci_text:
            findings.append(
                f"gate: {_rel(WORKFLOW)} runs the gate '{gate}', which is not "
                f"registered in {_rel(CI_SH)}; the recipe answers with an "
                "unknown-gate error wherever it is run."
            )
    return findings


def _doc_times(doc_text: str) -> list[tuple[int, int]]:
    return [(int(h), int(m)) for h, m in UTC_TIME_RE.findall(doc_text)]


def _rel(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def scan_tree() -> list[str]:
    """Run the rules over the real page, workflow and gate registry."""
    for path in (DOC, WORKFLOW, CI_SH):
        if not path.is_file():
            return [f"missing: {_rel(path)} is not a file, so nothing can be compared."]
    return scan(
        DOC.read_text(encoding="utf-8"),
        WORKFLOW.read_text(encoding="utf-8"),
        CI_SH.read_text(encoding="utf-8"),
    )


# ---------------------------------------------------------------- selftest

_WF = """\
# NEITHER TRIGGER FIRES WHILE THIS FILE IS OFF THE DEFAULT BRANCH: GitHub runs
# schedule from the latest commit on the default branch only.
name: macos-host
on:
  schedule:
    - cron: "41 7 * * *"
  workflow_dispatch:
jobs:
  macos-host-build:
    runs-on: macos-14
    steps:
      - run: just quality::local::gate macos-host-build
"""

_DOC = """\
# macOS host builds

## What runs on a clock

`.github/workflows/macos-host.yml` asks a GitHub-hosted `macos-14` (arm64)
runner for the `macos-host-build` gate at 07:41 UTC, `41 7 * * *`. Neither
trigger fires while the file is off the default branch, so run the gate by
hand instead.
"""

_CI = 'gates+=("macos-host-build|manual|Zig host build roots on arm64 macOS")\n'


def _rule_of(finding: str) -> str:
    return finding.split(":", 1)[0]


def _selftest() -> int:
    failures: list[str] = []

    def case(name: str, doc: str, wf: str, ci: str, want: str | None) -> None:
        rules = {_rule_of(f) for f in scan(doc, wf, ci)}
        if want is None:
            if rules:
                failures.append(f"{name}: expected silence, got {sorted(rules)}")
            return
        if want not in rules:
            failures.append(f"{name}: expected rule '{want}', got {sorted(rules)}")
        elif rules != {want}:
            failures.append(f"{name}: rule '{want}' fired with others {sorted(rules)}")

    case("compliant page is quiet", _DOC, _WF, _CI, None)
    case(
        "wrong runner named",
        _DOC.replace("`macos-14` (arm64)", "`macos-13` (arm64)"),
        _WF,
        _CI,
        "runner",
    )
    case("wrong hour named", _DOC.replace("07:41 UTC", "03:41 UTC"), _WF, _CI, "clock")
    case("wrong cron quoted", _DOC.replace("`41 7 * * *`", "`41 3 * * *`"), _WF, _CI, "clock")
    case(
        "caveat deleted from the page",
        _DOC.replace(
            "Neither\ntrigger fires while the file is off the default branch, so run the gate by\nhand instead.",
            "It runs nightly and can be started by hand with workflow_dispatch.",
        ),
        _WF,
        _CI,
        "caveat",
    )
    case(
        "caveat deleted from the workflow",
        _DOC,
        _WF.replace("DEFAULT BRANCH", "SOME OTHER BRANCH").replace("default branch only", "there only"),
        _CI,
        "caveat",
    )
    case(
        "clock section removed entirely",
        _DOC.replace("## What runs on a clock", "## Something else"),
        _WF,
        _CI,
        "caveat",
    )
    case(
        "gate renamed in the workflow only",
        _DOC,
        _WF.replace("gate macos-host-build", "gate macos-host-verify"),
        _CI,
        "gate",
    )
    case(
        "an archive name is not a runner label",
        _DOC + "\nThe runner fetches `zig-aarch64-macos-0.14.1.tar.xz` at 07:41 UTC.\n",
        _WF,
        _CI,
        None,
    )
    case(
        "gate not registered in ci.sh",
        _DOC,
        _WF,
        'gates+=("something-else|manual|other")\n',
        "gate",
    )

    # A workflow with no schedule and no dispatch needs no caveat: rule 3 is
    # about claims the triggers create, not a phrase every file must carry.
    no_trigger_wf = _WF.replace(
        'on:\n  schedule:\n    - cron: "41 7 * * *"\n  workflow_dispatch:\n',
        "on:\n  push:\n",
    ).replace(
        "# NEITHER TRIGGER FIRES WHILE THIS FILE IS OFF THE DEFAULT BRANCH: GitHub runs\n"
        "# schedule from the latest commit on the default branch only.\n",
        "",
    )
    no_clock_doc = _DOC.replace("at 07:41 UTC, `41 7 * * *`", "on every push")
    case("no schedule, no dispatch, no caveat needed", no_clock_doc, no_trigger_wf, _CI, None)

    # Parser-level assertions: the rules are only as good as what they read.
    if workflow_runner(_WF) != "macos-14":
        failures.append("parser: runs-on not read from the fixture")
    if workflow_triggers(_WF) != {"schedule", "workflow_dispatch"}:
        failures.append(f"parser: triggers read as {sorted(workflow_triggers(_WF))}")
    if workflow_crons(_WF) != ["41 7 * * *"]:
        failures.append(f"parser: crons read as {workflow_crons(_WF)}")
    if cron_hhmm("41 7 * * *") != (7, 41):
        failures.append("parser: cron hour/minute misread")
    if cron_hhmm("*/5 * * * *") is not None:
        failures.append("parser: a non-fixed cron should yield no wall-clock time")
    if workflow_gates(_WF) != ["macos-host-build"]:
        failures.append(f"parser: gates read as {workflow_gates(_WF)}")

    # Live-tree assertions: a green scan must not come from an empty read.
    if WORKFLOW.is_file():
        live = WORKFLOW.read_text(encoding="utf-8")
        if not workflow_crons(live):
            failures.append(f"live: {_rel(WORKFLOW)} declares no cron, so rule 2 reads nothing")
        runner = workflow_runner(live)
        if runner is None or not runner.startswith("macos-"):
            failures.append(f"live: {_rel(WORKFLOW)} runs-on is {runner!r}, not a macOS image")
        if not workflow_gates(live):
            failures.append(f"live: {_rel(WORKFLOW)} invokes no gate, so rule 4 reads nothing")
    if DOC.is_file() and clock_section(DOC.read_text(encoding="utf-8")) is None:
        failures.append(f"live: {_rel(DOC)} has no '{CLOCK_HEADING}' section")

    total = 16
    if failures:
        sys.stderr.write("check_macos_doc_workflow_parity.py --selftest FAILED:\n")
        for failure in failures:
            sys.stderr.write(f"  {failure}\n")
        return 1
    sys.stdout.write(
        f"check_macos_doc_workflow_parity.py --selftest: {total} assertions pass "
        "(each rule fires on its own fixture and on no other; the compliant page "
        "is silent; the live workflow really declares a cron, a macOS runner and "
        "a gate).\n"
    )
    return 0


def _roster() -> int:
    wf_text = WORKFLOW.read_text(encoding="utf-8")
    doc_text = DOC.read_text(encoding="utf-8")
    crons = workflow_crons(wf_text)
    sys.stdout.write(f"workflow: {_rel(WORKFLOW)}\n")
    sys.stdout.write(f"  runs-on:  {workflow_runner(wf_text)}\n")
    sys.stdout.write(f"  triggers: {sorted(workflow_triggers(wf_text))}\n")
    sys.stdout.write(f"  schedule: {crons or '-'}\n")
    sys.stdout.write(f"  gates:    {sorted(set(workflow_gates(wf_text))) or '-'}\n")
    sys.stdout.write(f"page: {_rel(DOC)}\n")
    sys.stdout.write(f"  runner labels named: {sorted(doc_runner_labels(doc_text)) or '-'}\n")
    sys.stdout.write(
        "  UTC times named:     "
        + (", ".join(f"{h:02d}:{m:02d}" for h, m in _doc_times(doc_text)) or "-")
        + "\n"
    )
    section = clock_section(doc_text)
    carries = section is not None and CAVEAT_PHRASE in section
    sys.stdout.write(f"  clock section carries the default-branch caveat: {carries}\n")
    return 0


def main() -> int:
    """Entry point: ``--selftest`` proves non-vacuity, otherwise gate the tree."""
    args = sys.argv[1:]
    if "--selftest" in args:
        return _selftest()
    if "--roster" in args:
        return _roster()
    findings = scan_tree()
    if findings:
        sys.stderr.write("check_macos_doc_workflow_parity.py: page/workflow disagreement:\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n\n")
        sys.stderr.write(
            f"{len(findings)} finding(s). {_rel(WORKFLOW)} is the source of truth; "
            f"{_rel(DOC)}\nis the copy a reader trusts before spending an arm64 Mac "
            "on this lane (#899).\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
