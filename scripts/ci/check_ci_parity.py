#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the GitHub workflows and ``scripts/ci.sh`` cannot describe different CI.

``scripts/ci.sh`` owns the *definition* of every gate; the workflows own only
the *scheduling*. Each gate-bearing workflow step is therefore a thin
``bash scripts/ci.sh --gate <name>`` driver, and this checker enforces that the
two sides stay welded together in both directions:

1. **Every ``run:`` step in every workflow** must either invoke a registered
   gate, or be explicitly tagged as infrastructure with a written reason::

       - name: Install Unicorn + Capstone
         run: |
           # ci-parity: infra -- runner provisioning, runs no project check
           sudo apt-get install -y libunicorn-dev

   An untagged raw ``run:`` step is exactly how check logic grows a second
   home, so it is rejected. An infra-tagged step may not reference anything
   under ``scripts/`` (other than ``ci.sh`` itself), ``tests/*.sh``, or a
   gate-ish ``make`` target -- otherwise "infra" becomes a smuggling route for
   the very checks this gate exists to centralise.

2. **Every registered gate** (except the ``manual`` speed class, which still
   must be bound to some workflow step) has to be scheduled somewhere. A gate
   added to ``ci.sh`` and forgotten in the YAML runs locally, passes, and then
   never runs in CI -- silent under-testing, the failure mode that motivated
   all of this.

Neither half can be done alone: registering a gate without scheduling it fails
here, and scheduling an unregistered gate fails here too.

Why this exists: ``ci.sh`` drifted from the workflows four separate times -- a
missing annotation gate plus a missing MISRA ratchet turned a green local run
into a red push and got dev reverted; agents hand-copied gate bodies into
throwaway ``/tmp`` scripts that silently stopped mirroring CI the moment a gate
was added; an audit found 21 checks in ``firmware.yml``'s pre-commit job alone
that were absent locally; and a hand re-sync landed to close them, which is
evidence for this gate rather than against it. Measured across *every*
workflow just before this checker landed, 26 distinct check invocations ran in
CI with no local equivalent. Moving the bodies into ``ci.sh`` removes the
duplication; this checker removes the ability to re-create it.

Run::

    check_ci_parity.py            # scan every workflow
    check_ci_parity.py --selftest # prove the checker still detects violations

Exit 0 when the workflows and the registry agree, 1 (listing every mismatch)
otherwise.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"
CI_SH = REPO_ROOT / "scripts" / "ci.sh"

# A step body that calls a gate. Tolerates the line continuations and leading
# whitespace a block scalar carries.
GATE_CALL_RE = re.compile(r"^\s*bash\s+scripts/ci\.sh\s+--gate[= ]\s*([A-Za-z0-9._-]+)\s*$")

# The infra escape hatch. The trailing reason is mandatory: an unexplained
# exemption is how an exemption list rots into a dumping ground.
INFRA_MARKER_RE = re.compile(r"^\s*#\s*ci-parity:\s*infra\s*--\s*(\S.*)$")

# An infra step may not run a project check. These are the shapes a smuggled
# check takes: an in-repo script, a host-test driver, or a `make` gate target.
FORBIDDEN_IN_INFRA = (
    (re.compile(r"(?<!ci\.sh)\bscripts/(?!ci\.sh)\S+"), "invokes an in-repo script under scripts/"),
    (re.compile(r"\btests/\S+\.sh\b"), "invokes a host-test driver under tests/"),
    (
        re.compile(
            r"\bmake\s+(?:-\S+\s+)*"
            r"(test|tidy|cppcheck|coverage|mcdc|ubsan|docs|misra|check|ascii"
            r"|version|format|bench-cache|fuzz|ci|ci-fast)\b"
        ),
        "invokes a gate-ish `make` target",
    ),
)

# Minimum reason length -- "infra -- x" teaches a reader nothing.
MIN_REASON_CHARS = 12

# `--list-gates` emits "name<TAB>speed<TAB>description"; name and speed are the
# two fields this checker needs.
REGISTRY_MIN_FIELDS = 2


def load_registry() -> dict[str, str]:
    """Return ``{gate_name: speed}`` by asking ci.sh itself.

    Executing ``--list-gates`` rather than parsing the bash array keeps this
    checker honest: the registry it validates against is the one the runner
    will actually execute, including ci.sh's own self-check that every listed
    name has a function behind it.
    """
    bash = shutil.which("bash")
    if bash is None:
        sys.stderr.write(
            "check_ci_parity.py: no `bash` on PATH -- the gate registry lives in "
            "scripts/ci.sh and cannot be read without it.\n"
        )
        raise SystemExit(1)

    proc = subprocess.run(  # noqa: S603  # fixed argv, no shell; bash resolved via shutil.which
        [bash, str(CI_SH), "--list-gates"],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
        check=False,
    )
    if proc.returncode != 0:
        sys.stderr.write(
            "check_ci_parity.py: `ci.sh --list-gates` failed -- the gate registry "
            "is unreadable, so parity cannot be established.\n"
        )
        sys.stderr.write(proc.stderr)
        raise SystemExit(1)

    registry: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < REGISTRY_MIN_FIELDS:
            sys.stderr.write(f"check_ci_parity.py: malformed registry row: {line!r}\n")
            raise SystemExit(1)
        registry[parts[0]] = parts[1]
    if not registry:
        sys.stderr.write(
            "check_ci_parity.py: the gate registry is EMPTY. Refusing to report "
            "parity against nothing.\n"
        )
        raise SystemExit(1)
    return registry


def iter_run_steps(workflow: Path) -> Iterator[tuple[str, str, str]]:
    """Yield ``(job_name, step_label, run_body)`` for every ``run:`` step."""
    with workflow.open(encoding="utf-8") as handle:
        doc = yaml.safe_load(handle)
    if not isinstance(doc, dict):
        return
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return
    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        steps = job.get("steps")
        if not isinstance(steps, list):
            continue
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                continue
            body = step.get("run")
            if body is None:
                continue
            label = step.get("name") or f"step #{index + 1}"
            yield job_name, str(label), str(body)


def classify_step(body: str) -> tuple[str, list[str], str | None]:
    """Classify one ``run:`` body.

    Returns ``(kind, gate_names, reason)`` where kind is ``"gate"``,
    ``"infra"`` or ``"raw"``.
    """
    gates: list[str] = []
    reason: str | None = None
    other_lines: list[str] = []

    for raw_line in body.splitlines():
        line = raw_line.rstrip()
        if not line.strip():
            continue
        infra = INFRA_MARKER_RE.match(line)
        if infra:
            reason = infra.group(1).strip()
            continue
        if line.lstrip().startswith("#"):
            continue
        call = GATE_CALL_RE.match(line)
        if call:
            gates.append(call.group(1))
            continue
        other_lines.append(line)

    if gates and not other_lines:
        return "gate", gates, reason
    if reason is not None and not gates:
        return "infra", [], reason
    return "raw", gates, reason


def _check_gate_step(
    where: str, gates: list[str], registry: dict[str, str], scheduled: set[str]
) -> list[str]:
    """Check one `--gate` step, recording the gates it schedules.

    A workflow naming a gate the registry does not define is a typo or a
    missing function: the step would fail at run time, having checked nothing.
    """
    errors: list[str] = []
    for gate in gates:
        if gate not in registry:
            errors.append(
                f"{where}\n"
                f"    runs unregistered gate '{gate}'.\n"
                f"    Add a row to RA8_GATE_REGISTRY in scripts/ci.sh and\n"
                f"    write the matching gate_{gate.replace('-', '_')}() function."
            )
        else:
            scheduled.add(gate)
    return errors


def _check_infra_step(where: str, body: str, reason: str | None) -> list[str]:
    """Check one step that claims to be infrastructure rather than a check.

    The claim has to be earned twice: the reason must actually say what the
    step provisions, and the body must not invoke anything gate-shaped. A
    check does not become infrastructure by being labelled one.
    """
    errors: list[str] = []
    if reason is None or len(reason) < MIN_REASON_CHARS:
        errors.append(
            f"{where}\n"
            f"    is tagged `# ci-parity: infra` but the reason is missing or\n"
            f"    too terse. Write what the step provisions and why it runs no\n"
            f"    project check."
        )
    for pattern, why in FORBIDDEN_IN_INFRA:
        hit = pattern.search(body)
        if hit:
            errors.append(
                f"{where}\n"
                f"    is tagged `# ci-parity: infra` but {why}: {hit.group(0)!r}.\n"
                f"    A check does not become infrastructure by being labelled one.\n"
                f"    Move it into a gate function in scripts/ci.sh and call it\n"
                f"    with `bash scripts/ci.sh --gate <name>`."
            )
    return errors


def check_workflows(registry: dict[str, str]) -> tuple[list[str], set[str]]:
    """Return ``(errors, scheduled_gate_names)``."""
    errors: list[str] = []
    scheduled: set[str] = set()

    workflows = sorted(list(WORKFLOW_DIR.glob("*.yml")) + list(WORKFLOW_DIR.glob("*.yaml")))
    if not workflows:
        errors.append(
            f"no workflow files found under {WORKFLOW_DIR.relative_to(REPO_ROOT)} -- "
            "refusing to report parity against nothing"
        )
        return errors, scheduled

    for workflow in workflows:
        rel = workflow.relative_to(REPO_ROOT)
        for job_name, label, body in iter_run_steps(workflow):
            where = f"{rel}: job '{job_name}', step '{label}'"
            kind, gates, reason = classify_step(body)

            if kind == "gate":
                errors.extend(_check_gate_step(where, gates, registry, scheduled))
                continue

            if kind == "infra":
                errors.extend(_check_infra_step(where, body, reason))
                continue

            errors.append(
                f"{where}\n"
                f"    is a raw `run:` step. Every workflow step must either invoke a\n"
                f"    registered gate:\n"
                f"        run: bash scripts/ci.sh --gate <name>\n"
                f"    or declare itself infrastructure with a reason:\n"
                f"        run: |\n"
                f"          # ci-parity: infra -- <why this runs no project check>\n"
                f"          ...\n"
                f"    Inline check bodies in YAML are the drift this gate exists to stop."
            )

    return errors, scheduled


def main() -> int:
    """Verify the gate registry and the workflows describe the same set of gates.

    Catches both halves of the drift, which fail in opposite directions: a
    gate registered but never scheduled passes locally and never runs in CI,
    while a workflow naming an unregistered gate is a typo or a missing
    function. Either way the tree looks greener than it is.

    Also rejects raw check bodies written inline in a workflow, since that is
    how a second, drifting home for check logic gets created. A step that only
    provisions the runner must declare itself as infrastructure.

    Returns 0 when registry and workflows agree, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the checker still rejects the violations it is meant to catch",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    registry = load_registry()
    errors, scheduled = check_workflows(registry)

    unscheduled = sorted(set(registry) - scheduled)
    for gate in unscheduled:
        errors.append(
            f"gate '{gate}' is registered in scripts/ci.sh but no workflow step\n"
            f"    ever runs it. It would pass locally and never run in CI.\n"
            f"    Add `run: bash scripts/ci.sh --gate {gate}` to a workflow job,\n"
            f"    or delete the gate."
        )

    if errors:
        sys.stderr.write(
            "check_ci_parity.py: the workflows and the ci.sh gate registry disagree.\n\n"
        )
        for error in errors:
            sys.stderr.write(f"  {error}\n\n")
        sys.stderr.write(f"{len(errors)} parity violation(s).\n")
        return 1

    print(
        f"check_ci_parity.py: clean -- {len(registry)} registered gates, "
        f"all scheduled, no raw check steps in any workflow."
    )
    return 0


def selftest() -> int:
    """Verify the classifier still rejects each violation shape.

    A parity guard nobody has watched fail is worth nothing, so the shapes it
    must reject are asserted here rather than trusted.
    """
    cases = [
        (
            "raw check step",
            "python3 scripts/checks/check_magic_numbers.py",
            "raw",
        ),
        (
            "raw multi-line step",
            "set -e\npython3 scripts/checks/doxy_audit.py --check",
            "raw",
        ),
        (
            "gate call",
            "bash scripts/ci.sh --gate ascii",
            "gate",
        ),
        (
            "gate call with trailing smuggled command",
            "bash scripts/ci.sh --gate ascii\npython3 scripts/checks/cite_check.py --strict",
            "raw",
        ),
        (
            "infra step",
            "# ci-parity: infra -- installs runner packages, runs no project check\n"
            "sudo apt-get install -y libunicorn-dev",
            "infra",
        ),
    ]
    failures = 0
    for label, body, expected in cases:
        kind, _, _ = classify_step(body)
        status = "ok" if kind == expected else "FAIL"
        if kind != expected:
            failures += 1
        print(f"  [{status}] {label}: classified '{kind}', expected '{expected}'")

    # An infra-tagged step that smuggles a checker must be caught by the
    # forbidden-pattern sweep, not merely classified as infra.
    smuggled = (
        "# ci-parity: infra -- pretends to be provisioning\n"
        "python3 scripts/checks/check_file_size.py"
    )
    caught = any(pattern.search(smuggled) for pattern, _ in FORBIDDEN_IN_INFRA)
    print(f"  [{'ok' if caught else 'FAIL'}] infra step smuggling a checker is rejected")
    if not caught:
        failures += 1

    if failures:
        sys.stderr.write(f"check_ci_parity.py --selftest: {failures} case(s) failed.\n")
        return 1
    print("check_ci_parity.py --selftest: all cases pass.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
