#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: the GitHub workflows and ``scripts/ci.sh`` cannot describe different CI.

``scripts/ci.sh`` owns the *definition* of every gate; the workflows own only
the *scheduling*. Each gate-bearing workflow step is therefore a thin
``just quality::local::gate <name>`` driver, and this checker enforces that the
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
   gate-ish legacy ``make`` or current ``just`` target -- otherwise "infra"
   becomes a smuggling route for
   the very checks this gate exists to centralise.

2. **Every registered gate** has to be scheduled somewhere. A gate added to
   ``ci.sh`` and forgotten in the YAML runs locally, passes, and then never
   runs in CI -- silent under-testing, the failure mode that motivated all of
   this.

3. **"Scheduled" has to mean "can actually run."** This is the half that was
   missing. The check above was satisfied by the gate name appearing as a
   substring of some YAML, which is a far weaker property than it reads as: a
   workflow whose triggers are all commented out, a step wrapped in
   ``continue-on-error: true``, and a job behind ``if: false`` were all
   indistinguishable from a gate running on every push. ``hil-all`` sat in
   exactly that state -- registered, listed by ``just quality::gate::list``, parity-clean,
   and unable to fire on any automatic trigger since its ``push:`` and
   ``pull_request:`` keys were commented out. So a ``fast``/``slow`` gate now
   has to reach at least one binding that is genuinely reachable, and a
   ``manual`` gate -- which is exempt from the automatic-trigger rule by
   definition -- still has to live in a workflow that can be dispatched or
   scheduled, so "manual" names a real invocation route rather than a dead one.

4. **Only disposable runners bootstrap Just.** Ansible-managed `ra8-ci` and
   `self-hosted` jobs consume the Just binary pinned into their runner image;
   installing it again in each workflow is redundant and can hide image drift.
   A non-managed job may use `setup-just`, but its exact `just-version` must
   match `.devcontainer/Dockerfile` because the action default floats.

5. **Managed runners never provision their own toolchain.** Jobs targeting
   `ra8-ci` or a `self-hosted` runner consume the environment built by Ansible.
   Package-manager commands and runtime setup actions in those jobs are
   rejected. The GitHub-hosted fork-PR workflow may still provision its clean
   `ubuntu-latest` VM.

None of the halves can be done alone: registering a gate without scheduling it
fails here, scheduling an unregistered gate fails here too, and scheduling one
somewhere it cannot run fails here as well.

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
import subprocess
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"
CI_SH = REPO_ROOT / "scripts" / "ci.sh"
DOCKERFILE = REPO_ROOT / ".devcontainer" / "Dockerfile"
SETUP_JUST_PREFIX = "extractions/setup-just@"
MANAGED_RUNNER_LABELS = frozenset({"ra8-ci", "self-hosted"})

# The runner image and native HIL listener are Ansible-owned. A workflow that
# heals either one with a package manager hides infrastructure drift and is not
# portable to the next managed runner. Keep the GitHub-hosted fork path free to
# provision its disposable ubuntu-latest VM.
FORBIDDEN_MANAGED_PROVISIONING = (
    re.compile(r"\b(?:sudo\s+)?(?:apt(?:-get)?|dnf|yum|zypper|pacman)\s+[^\n]*\binstall\b"),
    re.compile(r"\b(?:sudo\s+)?apk\s+add\b"),
    re.compile(r"\b(?:(?:python|python3)\s+-m\s+)?pip(?:3)?\s+install\b"),
    re.compile(r"\buv\s+pip\s+install\b"),
    re.compile(r"\bcargo\s+install\b"),
    re.compile(r"\bnpm\s+install\s+(?:--global|-g)\b"),
    re.compile(r"\bgo\s+install\b"),
)
FORBIDDEN_MANAGED_SETUP_ACTIONS = (
    "actions/setup-python@",
    "actions/setup-node@",
    "actions/setup-java@",
    "actions/setup-go@",
    "ruby/setup-ruby@",
)

# A step body that calls a gate. Tolerates the line continuations and leading
# whitespace a block scalar carries.
GATE_CALL_RE = re.compile(r"^\s*just\s+quality::local::gate\s+\s*([A-Za-z0-9._-]+)\s*$")

# The infra escape hatch. The trailing reason is mandatory: an unexplained
# exemption is how an exemption list rots into a dumping ground.
INFRA_MARKER_RE = re.compile(r"^\s*#\s*ci-parity:\s*infra\s*--\s*(\S.*)$")

# An infra step may not run a project check. These are the shapes a smuggled
# check takes: an in-repo script, a host-test driver, or a legacy/current task
# runner target that can execute project checks.
FORBIDDEN_IN_INFRA = (
    (re.compile(r"(?<!ci\.sh)\bscripts/(?!ci\.sh)\S+"), "invokes an in-repo script under scripts/"),
    (re.compile(r"\btests/\S+\.sh\b"), "invokes a host-test driver under tests/"),
    (
        re.compile(
            r"\bmake\s+(?:-\S+\s+)*"
            r"(test|tidy|cppcheck|coverage|mcdc|ubsan|docs|misra|check|ascii"
            r"|version|format|bench-cache|fuzz|ci|ci-fast)\b"
        ),
        "invokes a gate-ish legacy task-runner target",
    ),
    (
        re.compile(
            r"\bjust\s+(?!(?:quality::local::gate)\b)"
            r"(?:quality|checks|tests|apps|docs|tools|hil)(?:::[A-Za-z0-9_.-]+)*\b"
        ),
        "invokes a project-checking `just` recipe",
    ),
)

# Minimum reason length -- "infra -- x" teaches a reader nothing.
MIN_REASON_CHARS = 12

# `--list-gates` emits "name<TAB>speed<TAB>description"; name and speed are the
# two fields this checker needs.
REGISTRY_MIN_FIELDS = 2

# Triggers that fire without a human pressing anything. A gate reachable only
# through `workflow_dispatch` is not part of CI; it is a button.
AUTOMATIC_TRIGGERS = frozenset(
    {"push", "pull_request", "pull_request_target", "schedule", "merge_group"}
)

# Triggers that can still invoke a workflow, just not on their own. A `manual`
# speed-class gate must reach at least one of these or it cannot run at all.
INVOCABLE_TRIGGERS = AUTOMATIC_TRIGGERS | {
    "workflow_dispatch",
    "repository_dispatch",
    "workflow_call",
}

# `if:` expressions that evaluate to a constant false, disabling the job
# outright. Matched literally: anything else is a real condition whose value
# this checker cannot and should not try to predict. Note that an unquoted
# `if: false` reaches us as the BOOLEAN False, not this string -- see
# _job_disabled().
ALWAYS_FALSE_IF = frozenset({"false", "${{ false }}", "${{false}}"})


def load_registry() -> dict[str, str]:
    """Return ``{gate_name: speed}`` by asking ci.sh itself.

    Executing ``--list-gates`` rather than parsing the bash array keeps this
    checker honest: the registry it validates against is the one the runner
    will actually execute, including ci.sh's own self-check that every listed
    name has a function behind it.
    """
    proc = subprocess.run(  # noqa: S603  # fixed absolute protected-Bash argv, no shell
        ["/bin/bash", "-p", str(CI_SH), "--list-gates"],
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


def workflow_triggers(doc: dict) -> set[str]:
    """Return the set of trigger names declared by one workflow document.

    The ``on:`` key needs care. YAML 1.1 -- which PyYAML implements -- resolves
    a bare ``on`` to the boolean ``True``, so an unquoted ``on:`` in a GitHub
    workflow parses as the key ``True`` rather than the string ``"on"``. Both
    spellings are accepted here; missing that is how a checker concludes a
    workflow has no triggers (or, worse, stops looking).

    Args:
        doc: the parsed workflow mapping.

    Returns:
        Trigger names as strings. Empty when the workflow declares none --
        i.e. it can never run, which callers must treat as an error rather
        than as "no constraints".
    """
    raw = doc.get("on", doc.get(True))
    if isinstance(raw, str):
        return {raw}
    if isinstance(raw, list):
        return {str(item) for item in raw}
    if isinstance(raw, dict):
        return {str(key) for key in raw}
    return set()


def dockerfile_just_version() -> str:
    """Return the canonical Just release pinned by the devcontainer."""
    text = DOCKERFILE.read_text(encoding="utf-8")
    match = re.search(r"^ARG JUST_VERSION=(\S+)$", text, re.MULTILINE)
    if match is None:
        msg = f"{DOCKERFILE} does not declare ARG JUST_VERSION"
        raise ValueError(msg)
    return match.group(1)


def check_setup_just_policy(workflow_dir: Path, expected: str) -> list[str]:
    """Allow pinned setup-just steps only on non-managed runners."""
    errors: list[str] = []
    hosted_action_count = 0
    workflows = sorted(list(workflow_dir.glob("*.yml")) + list(workflow_dir.glob("*.yaml")))
    for workflow in workflows:
        with workflow.open(encoding="utf-8") as handle:
            doc = yaml.safe_load(handle) or {}
        jobs = doc.get("jobs", {}) if isinstance(doc, dict) else {}
        if not isinstance(jobs, dict):
            continue
        for job_name, job in jobs.items():
            if not isinstance(job, dict):
                continue
            managed = bool(_runner_labels(job.get("runs-on")) & MANAGED_RUNNER_LABELS)
            steps = job.get("steps", [])
            if not isinstance(steps, list):
                continue
            for index, step in enumerate(steps, 1):
                if not isinstance(step, dict):
                    continue
                uses = str(step.get("uses", ""))
                if not uses.startswith(SETUP_JUST_PREFIX):
                    continue
                if managed:
                    errors.append(
                        f"{workflow.name}:{job_name}:step {index} uses {uses} on an "
                        "Ansible-managed runner; use the Just binary pinned into the "
                        "runner image."
                    )
                    continue
                hosted_action_count += 1
                inputs = step.get("with", {})
                actual = inputs.get("just-version") if isinstance(inputs, dict) else None
                if str(actual) != expected:
                    errors.append(
                        f"{workflow.name}:{job_name}:step {index} uses {uses} with "
                        f"just-version={actual!r}; expected Dockerfile pin {expected!r}."
                    )
    if hosted_action_count == 0:
        errors.append(f"no non-managed {SETUP_JUST_PREFIX} action found under {workflow_dir}")
    return errors


def _runner_labels(runs_on: object) -> set[str]:
    """Normalise a job's ``runs-on`` value to a set of literal labels."""
    if isinstance(runs_on, str):
        return {runs_on}
    if isinstance(runs_on, list):
        return {str(label) for label in runs_on}
    return set()


def _check_managed_runner_dependencies(where: str, job: dict) -> list[str]:
    """Reject dependency provisioning in an Ansible-managed runner job."""
    if not (_runner_labels(job.get("runs-on")) & MANAGED_RUNNER_LABELS):
        return []

    errors: list[str] = []
    steps = job.get("steps", [])
    if not isinstance(steps, list):
        return errors
    for index, step in enumerate(steps, 1):
        if not isinstance(step, dict):
            continue
        label = str(step.get("name") or f"step #{index}")
        uses = str(step.get("uses", ""))
        if uses.startswith(FORBIDDEN_MANAGED_SETUP_ACTIONS):
            errors.append(
                f"{where}, step '{label}' uses {uses}.\n"
                f"    Managed runners consume the Ansible-provisioned toolchain; they\n"
                f"    must not install a runtime inside the workflow. Put the dependency\n"
                f"    in the runner image/role and let its verification gate fail loudly."
            )
        body = str(step.get("run", ""))
        for pattern in FORBIDDEN_MANAGED_PROVISIONING:
            hit = pattern.search(body)
            if hit:
                errors.append(
                    f"{where}, step '{label}' provisions dependencies with\n"
                    f"    {hit.group(0)!r}. Managed runners are Ansible-owned; move the\n"
                    f"    dependency into the runner image/role and verify it there."
                )
                break
    return errors


@dataclass(frozen=True)
class RunStep:
    """One ``run:`` step, with everything needed to judge whether it can fail.

    A step's body says what it would do; the last three fields say whether that
    body's verdict reaches the outside world. All three were previously
    ignored, which is what let "the name appears in some YAML" masquerade as
    "the gate runs in CI".

    Attributes:
        job_name: the job key owning the step.
        label: the step's ``name:``, or a positional fallback.
        body: the step's ``run:`` script.
        triggers: the owning workflow's declared trigger names.
        job_disabled: True when the job carries a constant-false ``if:``.
        soft: True when the step or its job cannot fail the run
            (``continue-on-error: true``).
    """

    job_name: str
    label: str
    body: str
    triggers: frozenset[str]
    job_disabled: bool
    soft: bool

    @property
    def runs_automatically(self) -> bool:
        """Return True when this step executes without anyone pressing a button.

        Requires an automatic trigger, an enabled job, and a step whose failure
        actually fails the run. A ``continue-on-error`` step executes but
        cannot enforce anything, so it does not count as a gate running.
        """
        return bool(self.triggers & AUTOMATIC_TRIGGERS) and not self.job_disabled and not self.soft

    @property
    def invocable(self) -> bool:
        """Return True when this step can be reached by any route at all."""
        return bool(self.triggers & INVOCABLE_TRIGGERS) and not self.job_disabled


def _is_true(value: object) -> bool:
    """Return True for a YAML value meaning boolean true, string or bool."""
    return value is True or (isinstance(value, str) and value.strip().lower() == "true")


def _job_disabled(job_if: object) -> bool:
    """Return True when a job's ``if:`` is a constant false.

    PyYAML resolves an unquoted ``if: false`` to the boolean ``False``, while
    ``if: ${{ false }}`` stays a string, so both spellings have to be handled.
    Checking only the string form would have let the plainest way of disabling
    a job go unnoticed -- which is how this rule would have grown its own
    blind spot.
    """
    if job_if is False:
        return True
    return isinstance(job_if, str) and job_if.strip() in ALWAYS_FALSE_IF


def iter_run_steps(workflow: Path) -> Iterator[RunStep]:
    """Yield one ``RunStep`` per ``run:`` step in a workflow file."""
    with workflow.open(encoding="utf-8") as handle:
        doc = yaml.safe_load(handle)
    if not isinstance(doc, dict):
        return
    triggers = workflow_triggers(doc)
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return
    for job_name, job in jobs.items():
        if not isinstance(job, dict):
            continue
        steps = job.get("steps")
        if not isinstance(steps, list):
            continue
        job_disabled = _job_disabled(job.get("if"))
        job_soft = _is_true(job.get("continue-on-error"))
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                continue
            body = step.get("run")
            if body is None:
                continue
            label = step.get("name") or f"step #{index + 1}"
            yield RunStep(
                job_name=str(job_name),
                label=str(label),
                body=str(body),
                triggers=frozenset(triggers),
                job_disabled=job_disabled,
                soft=job_soft or _is_true(step.get("continue-on-error")),
            )


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


class Bindings:
    """How each registered gate is bound to the workflows.

    Three sets rather than one, because "named in YAML", "reachable at all"
    and "runs on its own" are three different claims and only the last one
    means the gate is enforcing anything on the normal path.
    """

    def __init__(self) -> None:
        """Start with every set empty."""
        self.named: set[str] = set()
        self.invocable: set[str] = set()
        self.automatic: set[str] = set()

    def record(self, gate: str, step: RunStep) -> None:
        """Record one binding of ``gate`` at ``step``, keeping the best route."""
        self.named.add(gate)
        if step.invocable:
            self.invocable.add(gate)
        if step.runs_automatically:
            self.automatic.add(gate)


def _check_gate_step(
    where: str, gates: list[str], registry: dict[str, str], step: RunStep, bindings: Bindings
) -> list[str]:
    """Check one `--gate` step, recording how the gates it names are bound.

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
            bindings.record(gate, step)
    return errors


def reachability_errors(registry: dict[str, str], bindings: Bindings) -> list[str]:
    """Report every gate that is named in the YAML but cannot actually enforce.

    Split by speed class, because the classes make different promises:

    * ``fast`` / ``slow`` claim to run in CI, so they must reach a binding on
      an automatic trigger, in an enabled job, on a step whose failure fails
      the run;
    * ``manual`` claims only to be runnable on demand, so it must reach a
      binding that something can invoke -- a dispatch or a schedule.

    Args:
        registry: ``{gate_name: speed}`` as ci.sh reports it.
        bindings: the routes discovered while scanning the workflows.

    Returns:
        One message per gate whose binding does not back its claim.
    """
    errors: list[str] = []
    for gate, speed in sorted(registry.items()):
        if gate not in bindings.named:
            continue  # the unscheduled case is reported separately
        if speed == "manual":
            if gate not in bindings.invocable:
                errors.append(
                    f"gate '{gate}' is speed=manual and is named in a workflow, but that\n"
                    f"    workflow declares no trigger that can invoke it -- not even\n"
                    f"    workflow_dispatch. It cannot be run by any route.\n"
                    f"    Give the workflow a trigger, or delete the gate."
                )
            continue
        if gate not in bindings.automatic:
            errors.append(
                f"gate '{gate}' is speed={speed} but no binding of it can actually run.\n"
                f"    Every step naming it is in a workflow with no automatic trigger\n"
                f"    (push / pull_request / schedule / merge_group), or in a job\n"
                f"    disabled by `if: false`, or on a step marked\n"
                f"    `continue-on-error: true` -- which executes but cannot fail\n"
                f"    anything.\n"
                f"    A gate that cannot fail CI is not scheduled, however it reads in\n"
                f"    the YAML. Restore the trigger, drop the continue-on-error, or\n"
                f"    reclassify the gate as speed=manual in RA8_GATE_REGISTRY."
            )
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
                f"    with `just quality::local::gate <name>`."
            )
    return errors


def _check_workflow(workflow: Path, registry: dict[str, str], bindings: Bindings) -> list[str]:
    """Check one workflow's triggers, managed dependencies, and run steps."""
    errors: list[str] = []
    try:
        rel: object = workflow.relative_to(REPO_ROOT)
    except ValueError:
        rel = workflow.name
    with workflow.open(encoding="utf-8") as handle:
        doc = yaml.safe_load(handle)
    if isinstance(doc, dict) and not workflow_triggers(doc):
        errors.append(
            f"{rel}\n"
            f"    declares no `on:` triggers at all, so nothing in it can ever run.\n"
            f"    A workflow whose triggers were commented out looks identical to\n"
            f"    one that runs on every push -- which is exactly how a registered\n"
            f"    gate goes dormant unnoticed. Give it a trigger or delete it."
        )
    jobs = doc.get("jobs", {}) if isinstance(doc, dict) else {}
    if isinstance(jobs, dict):
        for job_name, job in jobs.items():
            if isinstance(job, dict):
                errors.extend(
                    _check_managed_runner_dependencies(
                        f"{rel}: job '{job_name}'",
                        job,
                    )
                )
    for step in iter_run_steps(workflow):
        where = f"{rel}: job '{step.job_name}', step '{step.label}'"
        kind, gates, reason = classify_step(step.body)

        if kind == "gate":
            errors.extend(_check_gate_step(where, gates, registry, step, bindings))
            continue

        if kind == "infra":
            errors.extend(_check_infra_step(where, step.body, reason))
            continue

        errors.append(
            f"{where}\n"
            f"    is a raw `run:` step. Every workflow step must either invoke a\n"
            f"    registered gate:\n"
            f"        run: just quality::local::gate <name>\n"
            f"    or declare itself infrastructure with a reason:\n"
            f"        run: |\n"
            f"          # ci-parity: infra -- <why this runs no project check>\n"
            f"          ...\n"
            f"    Inline check bodies in YAML are the drift this gate exists to stop."
        )
    return errors


def check_workflows(
    registry: dict[str, str], workflow_dir: Path = WORKFLOW_DIR
) -> tuple[list[str], Bindings]:
    """Scan every workflow and return ``(errors, bindings)``.

    ``workflow_dir`` remains injectable so selftests exercise the exact scan
    path that CI uses against synthetic workflow trees.
    """
    errors: list[str] = []
    bindings = Bindings()
    workflows = sorted(list(workflow_dir.glob("*.yml")) + list(workflow_dir.glob("*.yaml")))
    if not workflows:
        errors.append(
            f"no workflow files found under {workflow_dir} -- "
            "refusing to report parity against nothing"
        )
        return errors, bindings

    for workflow in workflows:
        errors.extend(_check_workflow(workflow, registry, bindings))

    return errors, bindings


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
    errors, bindings = check_workflows(registry)
    try:
        errors.extend(check_setup_just_policy(WORKFLOW_DIR, dockerfile_just_version()))
    except (OSError, ValueError) as exc:
        errors.append(f"cannot verify setup-just pins: {exc}")

    unscheduled = sorted(set(registry) - bindings.named)
    for gate in unscheduled:
        errors.append(
            f"gate '{gate}' is registered in scripts/ci.sh but no workflow step\n"
            f"    ever runs it. It would pass locally and never run in CI.\n"
            f"    Add `run: just quality::local::gate {gate}` to a workflow job,\n"
            f"    or delete the gate."
        )
    errors.extend(reachability_errors(registry, bindings))

    if errors:
        sys.stderr.write(
            "check_ci_parity.py: the workflows and the ci.sh gate registry disagree.\n\n"
        )
        for error in errors:
            sys.stderr.write(f"  {error}\n\n")
        sys.stderr.write(f"{len(errors)} parity violation(s).\n")
        return 1

    auto = len(bindings.automatic)
    print(
        f"check_ci_parity.py: clean -- {len(registry)} registered gates, all scheduled "
        f"({auto} on an automatic trigger, {len(registry) - auto} manual), "
        f"no raw check steps in any workflow."
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
            "just quality::local::gate ascii",
            "gate",
        ),
        (
            "gate call with trailing smuggled command",
            "just quality::local::gate ascii\npython3 scripts/checks/cite_check.py --strict",
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

    failures += _infra_smuggling_selftest()
    failures += _setup_just_pin_selftest()
    failures += _managed_runner_dependencies_selftest()
    # Imported only in the proof mode so production parity scans do not load
    # temporary-fixture machinery.  Keeping the end-to-end scan fixture in a
    # companion module also keeps this checker focused on policy enforcement.
    from ci_parity_scan_selftest import scan_selftest  # noqa: PLC0415 -- selftest-only import

    failures += scan_selftest()

    if failures:
        sys.stderr.write(f"check_ci_parity.py --selftest: {failures} case(s) failed.\n")
        return 1
    print("check_ci_parity.py --selftest: all cases pass.")
    return 0


def _infra_smuggling_selftest() -> int:
    """Prove an infra marker cannot hide either legacy or current checks."""
    prefix = "# ci-parity: infra -- pretends to be provisioning\n"
    cases = (
        ("a checker", prefix + "python3 scripts/checks/check_file_size.py"),
        ("a Just check", prefix + "just checks::local"),
    )
    failures = 0
    for label, body in cases:
        caught = any(pattern.search(body) for pattern, _ in FORBIDDEN_IN_INFRA)
        print(f"  [{'ok' if caught else 'FAIL'}] infra step smuggling {label} is rejected")
        if not caught:
            failures += 1
    return failures


def _setup_just_pin_selftest() -> int:
    """Prove setup-just is hosted-only, exactly pinned, and non-vacuous."""
    import tempfile  # noqa: PLC0415  # selftest-only temporary fixtures

    expected = "1.40.0"
    fixtures = (
        ("hosted exact pin", "ubuntu-latest", "with:\n          just-version: 1.40.0\n", False),
        ("hosted missing pin", "ubuntu-latest", "", True),
        (
            "hosted mismatched pin",
            "ubuntu-latest",
            "with:\n          just-version: 1.58.0\n",
            True,
        ),
    )
    failures = 0
    for label, runs_on, with_block, must_fire in fixtures:
        text = (
            "name: probe\n"
            "on: push\n"
            "jobs:\n"
            "  probe:\n"
            f"    runs-on: {runs_on}\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            f"        {with_block}"
        )
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "probe.yml").write_text(text, encoding="utf-8")
            fired = bool(check_setup_just_policy(directory, expected))
        ok = fired == must_fire
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] setup-just: {label}")

    failures += _setup_just_managed_selftest(expected)

    with tempfile.TemporaryDirectory() as tmp:
        fired = bool(check_setup_just_policy(Path(tmp), expected))
    ok = fired
    failures += 0 if ok else 1
    print(f"  [{'ok' if ok else 'FAIL'}] setup-just: empty action census is rejected")
    return failures


def _setup_just_managed_selftest(expected: str) -> int:
    """Prove Ansible-managed runners reject the hosted setup action."""
    import tempfile  # noqa: PLC0415  # selftest-only temporary fixtures

    failures = 0
    for label, runs_on in (
        ("managed ra8-ci action", "ra8-ci"),
        ("managed self-hosted action", "[self-hosted, hil, ra8d2]"),
    ):
        text = (
            "name: probe\n"
            "on: push\n"
            "jobs:\n"
            "  hosted:\n"
            "    runs-on: ubuntu-latest\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            "        with:\n"
            "          just-version: 1.40.0\n"
            "  managed:\n"
            f"    runs-on: {runs_on}\n"
            "    steps:\n"
            "      - uses: extractions/setup-just@v3\n"
            "        with:\n"
            "          just-version: 1.40.0\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "probe.yml").write_text(text, encoding="utf-8")
            errors = check_setup_just_policy(directory, expected)
        fired = any("Ansible-managed runner" in error for error in errors)
        ok = fired and not any("no non-managed" in error for error in errors)
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] setup-just: {label} is rejected")

    return failures


def _managed_runner_dependencies_selftest() -> int:
    """Prove only Ansible-managed jobs reject workflow-time provisioning."""
    cases = (
        (
            "ra8-ci apt install",
            {"runs-on": "ra8-ci", "steps": [{"run": "sudo apt-get install -y graphviz"}]},
            True,
        ),
        (
            "self-hosted setup-python",
            {
                "runs-on": ["self-hosted", "hil", "ra8d2"],
                "steps": [{"uses": "actions/setup-python@v5"}],
            },
            True,
        ),
        (
            "managed gate invocation",
            {
                "runs-on": "ra8-ci",
                "steps": [{"run": "just quality::local::gate lint-yaml"}],
            },
            False,
        ),
        (
            "hosted fork provisioning",
            {
                "runs-on": "ubuntu-latest",
                "steps": [{"run": "sudo apt-get install -y clang-format-22"}],
            },
            False,
        ),
    )
    failures = 0
    for label, job, must_fire in cases:
        fired = bool(_check_managed_runner_dependencies("probe", job))
        ok = fired == must_fire
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] managed runner: {label}")
    return failures


if __name__ == "__main__":
    raise SystemExit(main())
