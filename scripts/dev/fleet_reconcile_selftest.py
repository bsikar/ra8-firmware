#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Mutation-sensitive source contracts for fleet reconciliation deployment."""

from __future__ import annotations

import grp
import os
import subprocess
import tempfile
from pathlib import Path

CAPACITY_LOCK_MODE = 0o660
UNSAFE_MOVE_COUNT = 2


def _named_task_block(role_text: str, task_name: str) -> str:
    """Return one Ansible task block at either top-level or block depth."""
    lines = role_text.splitlines(keepends=True)
    matches = [
        index for index, line in enumerate(lines) if line.lstrip() == f"- name: {task_name}\n"
    ]
    if len(matches) != 1:
        msg = f"deployment role has no unique task named {task_name!r}"
        raise ValueError(msg)
    start = matches[0]
    indentation = len(lines[start]) - len(lines[start].lstrip())
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        same_depth = len(line) - len(line.lstrip()) == indentation
        if same_depth and line.lstrip().startswith("- name: "):
            end = index
            break
    prefix = " " * indentation
    return "".join(line.removeprefix(prefix) for line in lines[start + 1 : end])


def _deployment_uv_contract_errors(role_text: str) -> list[str]:
    """Require candidate uv repair followed by a strictly read-only proof."""
    errors: list[str] = []
    try:
        check = _named_task_block(role_text, "Prove the synchronized candidate Python environment")
        sync = _named_task_block(role_text, "Synchronize the candidate locked Python environment")
    except ValueError as error:
        return [str(error)]
    if "--run" not in check or "--ensure-and-run" in check:
        errors.append("candidate uv proof is not strictly read-only")
    if "--ensure-and-run" not in sync:
        errors.append("candidate uv repair cannot populate its authenticated cache")
    return errors


def _task(role: str, name: str, errors: list[str]) -> str:
    """Return one required task and record its absence."""
    try:
        return _named_task_block(role, name)
    except ValueError:
        errors.append(f"deployment task is missing: {name}")
        return ""


def _runtime_contract_errors(role: str) -> list[str]:
    """Check independently converged private runtime paths."""
    errors: list[str] = []
    name = "Converge private reconciliation runtime paths on every apply"
    if role.splitlines().count(f"- name: {name}") != 1:
        errors.append("runtime convergence is missing or nested under source refresh")
    converged = _task(role, name, errors)
    inspected = _task(role, "Inspect reconciliation runtime paths", errors)
    paths = ("ansible-local", "dev_box_reconcile_state }}/control", "ra8-fleet-mutation")
    metadata = ('owner: "{{ dev_box_user }}"', 'group: "{{ dev_box_user }}"', 'mode: "0700"')
    if any(value not in inspected for value in paths) or any(
        value not in converged for value in metadata
    ):
        errors.append("runtime paths are not independently private and exact")
    return errors


def _source_contract_errors(role: str) -> list[str]:
    """Check installed source metadata and interrupted-swap recovery."""
    errors: list[str] = []
    auth = _task(role, "Authenticate the installed reconciliation source metadata", errors)
    required = (
        "source_status.stat.isdir",
        "source_status.stat.islnk",
        "source_status.stat.uid",
        "source_status.stat.gid",
        "source_status.stat.mode",
        "source_realpath.rc",
        "source_realpath.stdout == dev_box_reconcile_source",
        "marker.stat.isreg",
        "marker.stat.islnk",
        "marker.stat.uid",
        "marker.stat.gid",
        "marker.stat.mode",
        "marker_realpath.rc",
        "marker_realpath.stdout",
        "== dev_box_reconcile_source ~ '/.ra8-source-sha256'",
    )
    if any(value not in auth for value in required):
        errors.append("source-stale decision does not authenticate all root/marker metadata")
    recovery = _task(role, "Recover the sole last-good source before inspecting freshness", errors)
    proof = (
        '[[ ! -e "$current" && ! -L "$current"',
        '[[ -d "$previous" && ! -L "$previous" ]]',
        'readlink -f -- "$previous"',
        "== 0:0:755",
        "== 0:0:444",
        'mv -- "$previous" "$current"',
    )
    if any(value not in recovery for value in proof):
        errors.append("interrupted source swap recovery is incomplete")
    return errors


def _candidate_contract_errors(role: str) -> list[str]:
    """Check candidate, systemd, activation, and retention order."""
    names = (
        "Create the reconciliation candidate root",
        "Synchronize the candidate locked Python environment",
        "Prove the synchronized candidate Python environment",
        "Synchronize the candidate Galaxy collection set",
        "Read the synchronized candidate Galaxy collection set",
        "Prove the synchronized candidate Galaxy collection set",
        "Prove the candidate reconciliation controller",
        "Render the candidate reconciliation unit set",
        "Validate the staged reconciliation unit set",
        "Activate the fully proven reconciliation generation",
        "Start reconciliation from the activated generation",
        "Verify the activated reconciliation generation is scheduled",
        "Retire recovery artifacts only after the timer is proven",
    )
    positions = [role.find(f"- name: {name}\n") for name in names]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        return ["candidate proof, activation, or retention ordering is incomplete"]
    return []


def _rollback_contract_errors(role: str) -> list[str]:
    """Check isolated, authenticated, fail-closed last-good rollback."""
    errors: list[str] = []
    preserve = _task(role, "Preserve each safe last-good unit", errors)
    if "unit-previous" not in preserve or "remote_src: true" not in preserve:
        errors.append("safe last-good units are not retained before replacement")
    switch = _task(role, "Switch source while retaining exactly one recovery generation", errors)
    switch_proof = (
        "dev_box_reconcile_installed_safe",
        'mv -- "$current" "$previous"',
        'mv -- "$current" "$failed"',
    )
    if any(value not in switch for value in switch_proof):
        errors.append("unsafe current source is not isolated from last-good recovery")
    restore = _task(role, "Restore the exact last-good source after post-switch failure", errors)
    restore_proof = (
        '[[ -d "$previous" && ! -L "$previous" ]]',
        'readlink -f -- "$previous"',
        "== 0:0:755",
        '[[ -f "$marker" && ! -L "$marker" ]]',
        'readlink -f -- "$marker"',
        "== 0:0:444",
        'cat -- "$marker"',
        'mv -- "$previous" "$current"',
    )
    if any(value not in restore for value in restore_proof):
        errors.append("last-good source rescue lacks complete authenticated metadata proof")
    restart = _task(role, "Restart the exact last-good reconciliation timer", errors)
    if any(
        value not in restart
        for value in ("'restored' in", "enabled: true", "state: started", "daemon_reload: true")
    ):
        errors.append("last-good timer is not restarted only after authenticated rescue")
    fail_closed = _task(role, "Fail closed without an authenticated last-good generation", errors)
    if "timer remains stopped" not in fail_closed:
        errors.append("missing/tampered last-good does not leave the timer stopped")
    return errors


def _deployment_runtime_contract_errors(role: str) -> list[str]:
    """Return all deployment authority errors."""
    return (
        _runtime_contract_errors(role)
        + _source_contract_errors(role)
        + _candidate_contract_errors(role)
        + _rollback_contract_errors(role)
    )


def _check_mutations(
    role: str, mutations: tuple[tuple[str, str, str], ...], failures: list[str]
) -> None:
    """Require each source mutation to trigger the contract checker."""
    for old, new, label in mutations:
        if role.count(old) != 1:
            failures.append(f"deployment authority fixture is not unique: {label}")
        elif not _deployment_runtime_contract_errors(role.replace(old, new, 1)):
            failures.append(f"deployment authority mutation stayed invisible: {label}")


def _authority_mutations() -> tuple[tuple[str, str, str], ...]:
    """Return runtime and installed-source mutations."""
    return (
        (
            "- name: Converge private reconciliation runtime paths on every apply\n",
            "  - name: Converge private reconciliation runtime paths on every apply\n",
            "runtime nesting",
        ),
        ("dev_box_reconcile_source_status.stat.uid", "source_uid_removed", "source uid"),
        ("dev_box_reconcile_source_status.stat.mode", "source_mode_removed", "source mode"),
        ("dev_box_reconcile_source_realpath.rc", "source_realpath_removed", "source realpath"),
        ("dev_box_reconcile_marker.stat.uid", "marker_uid_removed", "marker uid"),
        ("dev_box_reconcile_marker.stat.mode", "marker_mode_removed", "marker mode"),
        ("dev_box_reconcile_marker_realpath.rc", "marker_realpath_removed", "marker realpath"),
        (
            "- name: Recover the sole last-good source before inspecting freshness\n",
            "- name: Recovery removed\n",
            "interrupted recovery",
        ),
    )


def _candidate_rollback_mutations() -> tuple[tuple[str, str, str], ...]:
    """Return candidate, systemd, retention, and fail-closed mutations."""
    return (
        (
            "- name: Synchronize the candidate locked Python environment\n",
            "- name: Candidate uv removed\n",
            "candidate uv",
        ),
        (
            "- name: Prove the synchronized candidate Galaxy collection set\n",
            "- name: Candidate Galaxy proof removed\n",
            "candidate Galaxy",
        ),
        (
            "- name: Prove the candidate reconciliation controller\n",
            "- name: Candidate controller proof removed\n",
            "candidate controller",
        ),
        (
            "- name: Validate the staged reconciliation unit set\n",
            "- name: Candidate unit proof removed\n",
            "candidate units",
        ),
        (
            "- name: Preserve each safe last-good unit\n",
            "- name: Old units discarded\n",
            "old unit retention",
        ),
        (
            "- name: Retire recovery artifacts only after the timer is proven\n",
            "- name: Recovery artifacts retired early\n",
            "old source retention",
        ),
        (
            "- name: Fail closed without an authenticated last-good generation\n",
            "- name: Unsafe rollback allowed\n",
            "no authenticated previous",
        ),
        (
            "- name: Restart the exact last-good reconciliation timer\n",
            "- name: Rescue restart removed\n",
            "rescue restart",
        ),
    )


def _check_unsafe_current(role: str, failures: list[str]) -> None:
    """Require unsafe current source to route to failed, never previous."""
    move = '              mv -- "$current" "$failed"\n'
    if role.count(move) != UNSAFE_MOVE_COUNT:
        failures.append("unsafe-current isolation fixture count drifted")
        return
    weakened = role.replace(move, '              mv -- "$current" "$previous"\n', 1)
    if not _deployment_runtime_contract_errors(weakened):
        failures.append("unsafe current without previous stayed rollback-eligible")


def _selftest_deployment_runtime_contract(repo_root: Path, failures: list[str]) -> None:
    """Prove deployment authority and every mutation direction."""
    path = repo_root / "infra/ansible/roles/dev_box/tasks/fleet_reconcile.yml"
    role = path.read_text(encoding="ascii")
    failures.extend(_deployment_runtime_contract_errors(role))
    _check_mutations(role, _authority_mutations(), failures)
    _check_mutations(role, _candidate_rollback_mutations(), failures)
    _check_unsafe_current(role, failures)


def _selftest_deployment_uv_contract(repo_root: Path, failures: list[str]) -> None:
    """Prove the candidate uv bootstrap works in both directions."""
    role_path = repo_root / "infra/ansible/roles/dev_box/tasks/fleet_reconcile.yml"
    role_text = role_path.read_text(encoding="ascii")
    failures.extend(_deployment_uv_contract_errors(role_text))
    repair_weakened = role_text.replace("--ensure-and-run", "--run", 1)
    if not _deployment_uv_contract_errors(repair_weakened):
        failures.append("candidate uv repair mutation stayed invisible")
    check_start = role_text.index("- name: Prove the synchronized candidate Python environment")
    check_weakened = role_text[:check_start] + role_text[check_start:].replace(
        "--run", "--ensure-and-run", 1
    )
    if not _deployment_uv_contract_errors(check_weakened):
        failures.append("candidate uv check mutation stayed invisible")


def _write_capacity_fixture(source: str, root: Path, state: Path) -> Path:
    """Write one isolated capacity script and fake Docker implementation."""
    script = root / "capacity.sh"
    old_state = 'RA8_FLEET_STATE_DIR="${RA8_FLEET_STATE_DIR:-/var/lib/ra8-fleet}"'
    script.write_text(
        source.replace(old_state, f'RA8_FLEET_STATE_DIR="{state}"'),
        encoding="ascii",
    )
    script.chmod(0o755)
    docker = root / "docker"
    docker.write_text(
        "#!/bin/bash\n"
        "set -eu\n"
        'printf \'%s\\n\' "$*" >>"$RA8_TEST_DOCKER_LOG"\n'
        'case "$*" in\n'
        "  *\"ps -a\"*) printf 'runner\\n' ;;\n"
        "  *\"{{.State.Status}}\"*) printf 'exited\\n' ;;\n"
        "  *\"{{.Image}}\"*) printf 'sha256:test\\n' ;;\n"
        '  *"run --rm"*) [ "${RA8_TEST_FAIL_ADMIT:-0}" = 0 ] ;;\n'
        "esac\n",
        encoding="ascii",
    )
    docker.chmod(0o755)
    return script


def _capacity_environment(root: Path, commands: Path) -> dict[str, str]:
    """Return a quiet-hours environment for the isolated capacity script."""
    return {
        **os.environ,
        "PATH": f"{root}:/usr/bin:/bin",
        "RA8_TEST_DOCKER_LOG": str(commands),
        "RA8_FLEET_DOCKER": "docker",
        "RA8_FLEET_STATE_GROUP": grp.getgrgid(os.getgid()).gr_name,
        "RA8_FLEET_FULL_INSTANCES": "1",
        "RA8_FLEET_QUIET_INSTANCES": "0",
        "RA8_FLEET_QUIET_START": "00:00",
        "RA8_FLEET_QUIET_END": "23:59",
        "RA8_FLEET_QUIET_DAYS": "Mon,Tue,Wed,Thu,Fri,Sat,Sun",
    }


def _run_capacity(
    argv: list[str], environment: dict[str, str]
) -> subprocess.CompletedProcess[bytes]:
    """Run only the exact isolated capacity fixture assembled by this selftest."""
    # The fixture executable and complete argv are assembled above, never
    # caller supplied.
    return subprocess.run(argv, env=environment, check=False)  # noqa: S603 -- Exact fixture executable and argv.


def _capacity_runtime_selftest(repo_root: Path, failures: list[str]) -> None:
    """Exercise maintenance, timer exclusion, quiet restore, and retention."""
    source_path = repo_root / "scripts/ci/fleet_capacity.sh"
    source = source_path.read_text(encoding="ascii")
    with tempfile.TemporaryDirectory(prefix="ra8-capacity-selftest-") as raw:
        root = Path(raw)
        state = root / "state"
        state.mkdir(mode=0o770)
        state.chmod(0o770)
        commands = root / "docker.log"
        script = _write_capacity_fixture(source, root, state)
        environment = _capacity_environment(root, commands)
        common = [str(script), "--kind", "docker", "--container", "runner"]
        enter = _run_capacity([*common, "maintenance-enter"], environment)
        marker = state / "maintenance"
        lock = state / "capacity.lock"
        if enter.returncode or not marker.is_file():
            failures.append("maintenance entry did not durably park the host")
            return
        if not lock.is_file() or lock.stat().st_mode & 0o777 != CAPACITY_LOCK_MODE:
            failures.append("first controller did not create the shared capacity lock safely")
        before = commands.read_text(encoding="ascii")
        window = _run_capacity([*common, "window"], environment)
        after = commands.read_text(encoding="ascii")
        if window.returncode or not marker.is_file() or " start " in after[len(before) :]:
            failures.append("capacity timer admitted a host during maintenance")
        restore = _run_capacity([*common, "restore"], environment)
        if restore.returncode or marker.exists():
            failures.append("quiet-hours restore did not clear maintenance")
        quarantine = _run_capacity([*common, "quarantine"], environment)
        failed_environment = {
            **environment,
            "RA8_FLEET_QUIET_DAYS": "",
            "RA8_TEST_FAIL_ADMIT": "1",
        }
        failed = _run_capacity([*common, "restore"], failed_environment)
        if quarantine.returncode or failed.returncode == 0 or not marker.is_file():
            failures.append("failed restore did not retain durable quarantine")
        bypass = _run_capacity([*common, "scale", "1"], environment)
        if bypass.returncode == 0:
            failures.append("caller-controlled scale bypassed durable maintenance")


def _capacity_k8s_missing_selftest(repo_root: Path, failures: list[str]) -> None:
    """Prove only exact missing-ARS maintenance entry is accepted as parked."""
    source = (repo_root / "scripts/ci/fleet_capacity.sh").read_text(encoding="ascii")
    with tempfile.TemporaryDirectory(prefix="ra8-capacity-k8s-") as raw:
        root = Path(raw)
        state = root / "state"
        state.mkdir(mode=0o770)
        state.chmod(0o770)
        script = _write_capacity_fixture(source, root, state)
        kubectl = root / "kubectl"
        kubectl.write_text(
            "#!/bin/bash\n"
            'if [ "${RA8_TEST_K8S_ERROR:-}" = notfound ]; then\n'
            "  printf '%s\n' 'Error from server (NotFound): "
            'autoscalingrunnersets.actions.github.com "ra8-ci" not found\' >&2\n'
            "  exit 1\n"
            "fi\n"
            "printf '%s\n' 'Unable to connect to the server: refused' >&2\n"
            "exit 1\n",
            encoding="ascii",
        )
        kubectl.chmod(0o755)
        environment = {
            **os.environ,
            "PATH": f"{root}:/usr/bin:/bin",
            "RA8_FLEET_STATE_GROUP": grp.getgrgid(os.getgid()).gr_name,
            "RA8_FLEET_KUBECTL": str(kubectl),
            "RA8_FLEET_FULL_INSTANCES": "1",
            "RA8_TEST_K8S_ERROR": "notfound",
        }
        common = [str(script), "--kind", "k8s", "--scale-set", "ra8-ci"]
        enter = _run_capacity([*common, "maintenance-enter"], environment)
        if enter.returncode or not (state / "maintenance").is_file():
            failures.append("exact missing ARC scale set did not enter maintenance")
        quarantine = _run_capacity([*common, "quarantine"], environment)
        if quarantine.returncode == 0:
            failures.append("missing ARC scale set was accepted outside maintenance entry")
        other = _run_capacity(
            [*common, "maintenance-enter"],
            {**environment, "RA8_TEST_K8S_ERROR": "transport"},
        )
        if other.returncode == 0:
            failures.append("non-NotFound kubectl failure was accepted as zero admission")


def _source_contract_selftest(repo_root: Path, failures: list[str]) -> None:
    """Require parked roles, candidate rollback, and read-only WSL checks."""
    paths = {
        "fleet": repo_root / "scripts/dev/fleet.py",
        "wsl": repo_root / "scripts/dev/fleet_wsl.py",
        "stage": repo_root / "scripts/dev/fleet_wsl_stage.py",
        "docker": repo_root / "infra/ansible/roles/ci_runner_docker/tasks/deploy.yml",
        "arc": repo_root / "infra/ansible/roles/ci_runner/tasks/main.yml",
        "role": repo_root / "infra/ansible/roles/dev_box/tasks/fleet_reconcile.yml",
        "capacity-role": repo_root / "infra/ansible/roles/fleet_capacity/tasks/main.yml",
        "arc-play": repo_root / "infra/ansible/playbooks/ci-runner.yml",
    }
    texts = {name: path.read_text(encoding="ascii") for name, path in paths.items()}
    required = (
        ("fleet", '["-e", "fleet_reconcile_parked=true"]', 1),
        ("fleet", '["maintenance-enter"]', 1),
        ("docker", "if fleet_reconcile_parked | default(false) | bool", 1),
        ("arc", "{{ 0 if fleet_reconcile_parked", 2),
        ("fleet", "fleet_reconcile_activation_hold=true", 1),
        ("arc", "post_renderer: >-", 1),
        ("arc", 'spec["maxRunners"] = 0', 1),
        ("arc", "if matches != 1:", 1),
        ("stage", 'mode == "apply" and not installed else "--verify-cache"', 1),
        ("wsl", 'if spec.mode == "check":', 3),
        ("wsl", 'fws.transaction_lock_lines(spec.mode != "check")', 1),
        ("fleet", 'sync_image=request.args.mode == "apply"', 1),
        ("capacity-role", 'path: "{{ fleet_capacity_state_dir }}/capacity.lock"', 1),
        ("capacity-role", 'group: "{{ fleet_capacity_state_group }}"', 2),
        ("capacity-role", 'mode: "0660"', 1),
        ("arc-play", "    - fleet_capacity\n", 1),
    )
    for name, value, expected in required:
        if texts[name].count(value) != expected:
            failures.append("parked role or WSL read-only contract count drifted")
        weakened = texts[name].replace(value, "", 1)
        if weakened.count(value) == expected:
            failures.append("source contract mutation unexpectedly stayed invisible")


def run(repo_root: Path) -> list[str]:
    """Return deployment contract failures for the injected repository root."""
    failures: list[str] = []
    _selftest_deployment_runtime_contract(repo_root, failures)
    _selftest_deployment_uv_contract(repo_root, failures)
    _capacity_runtime_selftest(repo_root, failures)
    _capacity_k8s_missing_selftest(repo_root, failures)
    _source_contract_selftest(repo_root, failures)
    return failures
