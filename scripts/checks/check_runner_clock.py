#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: no CI runner may move its wall clock underneath a job (#509).

GitHub records every step's ``started_at`` and ``completed_at`` from the
*runner's own clock*. So a runner whose clock steps backward writes the
evidence into the API itself: a step that finished before it started, or a step
that started before the previous one finished. Neither is physically possible;
both are cheap to look for; and looking turns "the fleet's clocks are fine"
from an assumption into an observation.

It is not a cosmetic complaint about log timestamps. Every gate whose contract
is a *duration* rather than a *result* is measured on that clock:

* ``fuzz-sweep`` -- libFuzzer derives ``-max_total_time`` from
  ``system_clock`` (compiler-rt ``FuzzerInternal.h``), as an unsigned second
  count. Step the clock back and the budget looks spent, the sweep stops after
  seconds, and it exits 0. A 60 s sweep was observed reporting
  ``Done 7363327 runs in 251 second(s)`` and, twice, a NEGATIVE duration --
  while the job was green. ``run_fuzz.sh`` now measures itself on
  ``CLOCK_MONOTONIC`` and refuses to certify such a run, but that protects one
  gate; this checker is what notices the *host* is broken.
* ``timeout-minutes`` on any job -- the runner enforces it.
* Any benchmark or latency gate.

The signature was first found on the ``win-ci-*`` WSL2 runners, where the VM's
RTC ran ~4 minutes fast and ``timesyncd`` stepped it back rather than slewing
it. A build farm wants slew.

Run::

    check_runner_clock.py                  # scan recent completed runs
    check_runner_clock.py --hours 6        # only runs that started recently
    check_runner_clock.py --runs 60        # widen the scan
    check_runner_clock.py --selftest       # prove the detector still detects

Exit 0 when every step on every runner is time-ordered, 1 when any is not, and
2 when the scan could not be performed at all -- which is a failure, not a
pass. A clock checker that quietly reports "clean" because it had no token is
worse than no checker: it also removes the reason to look.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

# The API is spoken directly over HTTPS rather than through `gh api`, and that
# is a deployment fact rather than a taste: the ra8-ci runner image does not
# ship the GitHub CLI, so a gate built on it would fail nightly with a
# provisioning error instead of a verdict. urllib is in the standard library
# and is on every host this could ever run on.
K_API_BASE = "https://api.github.com"

# Marker for the line after a _fail() call. _fail() exits, so these raises are
# unreachable -- they exist so a reader (and a type checker) can see that the
# function does not fall through to an implicit None.
K_UNREACHABLE = "unreachable: _fail() exits"

# Whole-second timestamps and ordinary scheduling jitter mean adjacent steps can
# appear to touch. A real clock step on the observed hosts is ~240 s wide, so a
# few seconds of slack costs nothing in detection and removes every false
# positive from rounding. Zero tolerance is reserved for the one comparison that
# needs none: a step cannot finish before it starts, at any tolerance.
K_OVERLAP_TOLERANCE_S = 5

# How many completed runs to look back over when --hours is not given. Each run
# costs one extra API call for its jobs, so this bounds the scan's request
# count at roughly K_DEFAULT_RUNS + 1.
K_DEFAULT_RUNS = 40


def _fail(message: str) -> None:
    """Print a fatal message and exit 2 (scan impossible, not scan clean)."""
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(2)


def _token() -> str:
    """Return an API token, or exit 2 naming every way one could have been given.

    In CI it is the workflow's own ``GITHUB_TOKEN``, whose rate budget is
    per-repository and separate from the shared user quota. On a developer box
    it falls back to whatever ``gh`` is already logged in as, so the gate runs
    locally with nothing exported. No token at all is a failure, never a clean
    scan.
    """
    for name in ("GH_TOKEN", "GITHUB_TOKEN"):
        value = os.environ.get(name, "").strip()
        if value:
            return value
    exe = shutil.which("gh")
    if exe is not None:
        proc = subprocess.run(  # noqa: S603  # fixed argv, no shell; gh via shutil.which
            [str(exe), "auth", "token"],
            capture_output=True,
            text=True,
            check=False,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip()
    _fail(
        "no GitHub API token. Set GH_TOKEN or GITHUB_TOKEN (in a workflow, "
        "`env: GH_TOKEN: ${{ github.token }}`), or log in with `gh auth login`. "
        "This gate reads step timestamps from the Actions API; without a token "
        "there is no scan to report on."
    )
    raise AssertionError(K_UNREACHABLE)


def _api(path: str, token: str) -> object:
    """GET one Actions API path and return the decoded JSON, or exit 2 saying why."""
    url = f"{K_API_BASE}/{path.lstrip('/')}"
    if not url.startswith(f"{K_API_BASE}/"):
        _fail(f"refusing to fetch a URL outside {K_API_BASE}: {url}")
    request = urllib.request.Request(  # noqa: S310  # scheme pinned to K_API_BASE above
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "User-Agent": "ra8-firmware-runner-clock",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:  # noqa: S310 -- HTTPS URL pinned above
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        _fail(f"GET {url} failed: HTTP {exc.code} {exc.reason}")
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        _fail(f"GET {url} failed: {exc}")
    raise AssertionError(K_UNREACHABLE)


def _parse_ts(value: str | None) -> dt.datetime | None:
    """Parse a GitHub ISO-8601 timestamp, or None when the field is absent."""
    if not value:
        return None
    try:
        return dt.datetime.fromisoformat(value)
    except ValueError:
        return None


def scan_job(job: dict) -> list[dict]:
    """Return every time-ordering violation in one job's step list.

    Pure: takes the job dictionary the API returns and reads nothing else, so
    ``--selftest`` can drive both directions without touching the network.

    Two rules, both physically impossible on a sane clock:

    * a step whose ``completed_at`` precedes its own ``started_at``;
    * a step whose ``started_at`` precedes the previous step's ``completed_at``
      by more than ``K_OVERLAP_TOLERANCE_S`` (steps within a job are strictly
      sequential).
    """
    findings: list[dict] = []
    previous_end: dt.datetime | None = None
    previous_name = ""
    for step in job.get("steps") or []:
        start = _parse_ts(step.get("started_at"))
        end = _parse_ts(step.get("completed_at"))
        name = step.get("name", "?")
        if start is not None and end is not None and end < start:
            findings.append(
                {
                    "kind": "finished-before-it-started",
                    "step": name,
                    "detail": f"{start:%Y-%m-%dT%H:%M:%SZ} -> {end:%Y-%m-%dT%H:%M:%SZ}",
                    "seconds": (end - start).total_seconds(),
                    "at": start,
                }
            )
        if start is not None and previous_end is not None:
            gap = (start - previous_end).total_seconds()
            if gap < -K_OVERLAP_TOLERANCE_S:
                findings.append(
                    {
                        "kind": "started-before-the-previous-step-finished",
                        "step": name,
                        "detail": (
                            f"'{previous_name}' ended {previous_end:%Y-%m-%dT%H:%M:%SZ}, "
                            f"'{name}' began {start:%Y-%m-%dT%H:%M:%SZ}"
                        ),
                        "seconds": gap,
                        "at": start,
                    }
                )
        if end is not None:
            previous_end = end
            previous_name = name
    return findings


def _runs(repo: str, token: str, limit: int, hours: int | None) -> list[dict]:
    """Fetch the most recent completed workflow runs, newest first."""
    payload = _api(f"repos/{repo}/actions/runs?status=completed&per_page={min(limit, 100)}", token)
    runs = payload.get("workflow_runs", []) if isinstance(payload, dict) else []
    if hours is None:
        return runs[:limit]
    cutoff = dt.datetime.now(dt.UTC) - dt.timedelta(hours=hours)
    kept = []
    for run in runs:
        started = _parse_ts(run.get("run_started_at") or run.get("created_at"))
        if started is not None and started >= cutoff:
            kept.append(run)
    return kept[:limit]


def _report(findings: list[dict], scanned: dict) -> int:
    """Print the scan result and return the process exit status."""
    print(
        f"runner clock scan: {scanned['runs']} completed runs, "
        f"{scanned['jobs']} jobs, {scanned['steps']} steps"
    )
    if not findings:
        print("every step on every runner is time-ordered.")
        return 0
    for item in sorted(findings, key=lambda f: f["at"], reverse=True):
        print()
        print(f"SKEW  runner={item['runner']}  {item['workflow']} / {item['job']}")
        print(f"      step '{item['step']}' {item['kind']}")
        print(f"      {item['detail']}  ({item['seconds']:+.0f}s)")
    per_runner: dict[str, int] = {}
    for item in findings:
        per_runner[item["runner"]] = per_runner.get(item["runner"], 0) + 1
    print()
    print("per runner:")
    for runner, count in sorted(per_runner.items(), key=lambda kv: -kv[1]):
        print(f"  {runner}: {count} skewed step(s)")
    print()
    print(
        f"FAIL: {len(findings)} step(s) on {len(per_runner)} runner(s) are not "
        f"time-ordered. Those runners moved their wall clock underneath a "
        f"running job, so every time-budgeted gate that lands on them measures "
        f"the wrong thing (#509). Fix the host's clock discipline -- slew, not "
        f"step."
    )
    return 1


def scan(repo: str, limit: int, hours: int | None) -> int:
    """Scan recent runs of ``repo`` and report every clock-skew finding."""
    token = _token()
    findings: list[dict] = []
    scanned = {"runs": 0, "jobs": 0, "steps": 0}
    for run in _runs(repo, token, limit, hours):
        scanned["runs"] += 1
        payload = _api(f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100", token)
        jobs = payload.get("jobs", []) if isinstance(payload, dict) else []
        for job in jobs:
            scanned["jobs"] += 1
            scanned["steps"] += len(job.get("steps") or [])
            for finding in scan_job(job):
                finding["runner"] = job.get("runner_name") or "(unknown runner)"
                finding["job"] = job.get("name", "?")
                finding["workflow"] = run.get("name", "?")
                findings.append(finding)
    if scanned["steps"] == 0:
        _fail(
            "the scan saw zero steps. That is not a clean fleet, it is a scan "
            "that did not happen -- widen --hours/--runs or check gh's auth."
        )
    return _report(findings, scanned)


def _case(name: str, job: dict, expected: int) -> bool:
    """Assert ``scan_job`` returns ``expected`` findings for ``job``."""
    got = len(scan_job(job))
    ok = got == expected
    print(f"  {'ok  ' if ok else 'FAIL'} {name}: expected {expected}, got {got}")
    return ok


def _step(name: str, start: str | None, end: str | None) -> dict:
    """Build one API-shaped step record for the selftest."""
    return {"name": name, "started_at": start, "completed_at": end}


def selftest() -> int:
    """Drive the detector in both directions with synthetic API records."""
    print("check_runner_clock.py selftest:")
    healthy = {
        "steps": [
            _step("Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:00:04Z"),
            _step("checkout", "2026-07-28T06:00:04Z", "2026-07-28T06:00:09Z"),
        ]
    }
    # The real win-ci-3 record from #509: checkout began the instant "Set up
    # job" ended and then finished four minutes earlier. Only the first rule
    # fires -- the step boundaries themselves are in order, which is exactly
    # why the second rule is not sufficient on its own.
    observed = {
        "steps": [
            _step("Set up job", "2026-07-28T06:38:16Z", "2026-07-28T06:38:18Z"),
            _step("checkout", "2026-07-28T06:38:18Z", "2026-07-28T06:34:12Z"),
        ]
    }
    backward_only = {"steps": [_step("gate", "2026-07-28T06:23:40Z", "2026-07-28T06:21:12Z")]}
    overlap_only = {
        "steps": [
            _step("a", "2026-07-28T06:00:00Z", "2026-07-28T06:05:00Z"),
            _step("b", "2026-07-28T06:01:00Z", "2026-07-28T06:06:00Z"),
        ]
    }
    rounding = {
        "steps": [
            _step("a", "2026-07-28T06:00:00Z", "2026-07-28T06:00:05Z"),
            _step("b", "2026-07-28T06:00:04Z", "2026-07-28T06:00:09Z"),
        ]
    }
    skipped = {"steps": [_step("never ran", None, None)]}
    results = [
        _case("a time-ordered job is clean", healthy, 0),
        _case("the observed win-ci-3 checkout (#509)", observed, 1),
        _case("a step that finished before it started", backward_only, 1),
        _case("a step that started before the previous finished", overlap_only, 1),
        _case("one second of rounding is not a fault", rounding, 0),
        _case("a skipped step has no timestamps to judge", skipped, 0),
        _case("a job with no steps at all", {}, 0),
    ]
    if not all(results):
        print("SELFTEST FAILED: the clock detector no longer behaves as documented.")
        return 1
    print(f"  {len(results)}/{len(results)} cases as documented.")
    return 0


def main() -> int:
    """Parse arguments and run either the selftest or a scan."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default="bsikar/ra8-firmware", help="owner/name")
    parser.add_argument("--runs", type=int, default=K_DEFAULT_RUNS, help="completed runs to scan")
    parser.add_argument("--hours", type=int, default=None, help="only runs started within N hours")
    parser.add_argument("--selftest", action="store_true", help="prove the detector, then exit")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    if args.runs < 1:
        _fail("--runs must be at least 1; a scan of nothing proves nothing.")
    return scan(args.repo, args.runs, args.hours)


if __name__ == "__main__":
    sys.exit(main())
