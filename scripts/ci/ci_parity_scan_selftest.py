# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""End-to-end workflow scan fixtures for ``check_ci_parity.py``.

The production checker owns the policy.  This companion owns the temporary
workflow trees that prove the complete scan fires in both directions.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import check_ci_parity as parity
import yaml


def _workflow_yaml(trigger_block: str, *, soft: str = "", job_if: str = "") -> str:
    """Render a minimal one-gate workflow for the scan selftest."""
    return (
        "name: probe\n"
        f"{trigger_block}"
        "jobs:\n"
        "  probe:\n"
        f"{job_if}"
        "    runs-on: ubuntu-latest\n"
        "    steps:\n"
        "      - name: probe gate\n"
        f"{soft}"
        "        run: just quality::local::gate probe-gate\n"
    )


def _cases() -> list[tuple[str, str, str, bool]]:
    """Return the automatic/manual reachability fixtures."""
    push = "on:\n  push:\n    branches: [dev]\n"
    dispatch = "on:\n  workflow_dispatch:\n"
    no_trigger = "# on:\n#   push:\n"

    return [
        # (label, registry speed, workflow text, must_fire)
        ("push-triggered gate", "fast", _workflow_yaml(push), False),
        (
            "triggers all commented out (hil-all's shape)",
            "fast",
            _workflow_yaml(no_trigger),
            True,
        ),
        (
            "dispatch-only workflow for a fast gate",
            "fast",
            _workflow_yaml(dispatch),
            True,
        ),
        (
            "dispatch-only workflow for a manual gate",
            "manual",
            _workflow_yaml(dispatch),
            False,
        ),
        (
            "continue-on-error step for a fast gate",
            "fast",
            _workflow_yaml(push, soft="        continue-on-error: true\n"),
            True,
        ),
        (
            "job disabled by `if: false`",
            "fast",
            _workflow_yaml(push, job_if="    if: false\n"),
            True,
        ),
    ]


def scan_selftest() -> int:
    """Prove the real workflow scan and reachability checks both ways."""
    failures = 0
    registry_name = "probe-gate"
    for label, speed, workflow_text, must_fire in _cases():
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "probe.yml").write_text(workflow_text, encoding="utf-8")
            registry = {registry_name: speed}
            errors, bindings = parity.check_workflows(registry, directory)
            errors.extend(parity.reachability_errors(registry, bindings))
            fired = bool(errors)
        ok = fired == must_fire
        failures += 0 if ok else 1
        expectation = "must fire" if must_fire else "must stay quiet"
        print(f"  [{'ok' if ok else 'FAIL'}] scan: {label} ({expectation})")

    # An unscheduled gate must still be caught by the scan-plus-main logic,
    # and an empty workflow directory must never read as parity.
    with tempfile.TemporaryDirectory() as tmp:
        errors, _ = parity.check_workflows({registry_name: "fast"}, Path(tmp))
    ok = bool(errors)
    failures += 0 if ok else 1
    print(f"  [{'ok' if ok else 'FAIL'}] scan: an empty workflow directory is refused")

    # The YAML 1.1 `on:` -> True quirk: if this regressed, every workflow would
    # look trigger-less and the reachability rules would fire on everything.
    parsed = yaml.safe_load("on:\n  push:\n    branches: [dev]\njobs: {}\n")
    ok = parity.workflow_triggers(parsed) == {"push"}
    failures += 0 if ok else 1
    print(f"  [{'ok' if ok else 'FAIL'}] scan: bare `on:` parses as a trigger map, not a bool key")

    return failures
