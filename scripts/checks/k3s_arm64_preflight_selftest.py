# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Static mutations and local Ansible refusal fixtures for the K3s preflight."""

from __future__ import annotations

import copy
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import check_k3s_arm64_preflight as checker
import yaml


class FixtureError(Exception):
    """Raised when a checked-in fixture has the wrong shape."""

    def __init__(self) -> None:
        """Initialize a fixture shape error with its diagnostic."""
        super().__init__("invalid K3s Ansible selftest fixture")


@dataclass(frozen=True)
class AnsibleCase:
    """One synthetic localhost run of a tracked refusal task."""

    source: Path
    values: dict[str, str]
    expect_refusal: bool
    label: str


def _read(path: Path) -> list[object]:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, list):
        raise FixtureError
    return value


def _refusal(plays: list[object], name: str) -> dict[str, object]:
    play = plays[0]
    if not isinstance(play, dict):
        raise FixtureError
    tasks = play.get("pre_tasks")
    if not isinstance(tasks, list):
        raise FixtureError
    return next(task for task in tasks if isinstance(task, dict) and task.get("name") == name)


def _mutate_controls(path: Path, name: str) -> tuple[list[str], int]:
    failures: list[str] = []
    fields = sorted(checker.CONTROL_KEYS | {"tags"})
    mutations = 0
    base = _read(path)
    for field in fields:
        changed = copy.deepcopy(base)
        _refusal(changed, name)[field] = False if field != "tags" else ["never"]
        if not checker.audit_plays(path, changed):
            failures.append(f"execution control {field!r} was accepted in {path.name}")
        mutations += 1
    if path == checker.PREFLIGHT:
        for tag_value in (["preflight"], ["never"], ["always"]):
            changed = copy.deepcopy(base)
            _refusal(changed, name)["tags"] = tag_value
            if not checker.audit_plays(path, changed):
                failures.append(f"refusal tag selector {tag_value!r} was accepted")
            mutations += 1
    return failures, mutations


def _mutate_placement(path: Path, name: str) -> tuple[list[str], int]:
    failures: list[str] = []
    base = _read(path)
    for section in ("block", "rescue", "always"):
        changed = copy.deepcopy(base)
        play = changed[0]
        if not isinstance(play, dict):
            raise FixtureError
        task = _refusal(changed, name)
        play["pre_tasks"].remove(task)
        play["pre_tasks"].insert(0, {section: [task]})
        if not checker.audit_plays(path, changed):
            failures.append(f"refusal placement in {section!r} was accepted")
    changed = copy.deepcopy(base)
    play = changed[0]
    if not isinstance(play, dict):
        raise FixtureError
    task = _refusal(changed, name)
    play["pre_tasks"].remove(task)
    play.setdefault("tasks", []).append(task)
    if not checker.audit_plays(path, changed):
        failures.append("refusal moved out of pre_tasks was accepted")
    changed = copy.deepcopy(base)
    play = changed[0]
    if not isinstance(play, dict):
        raise FixtureError
    play["pre_tasks"].insert(
        0,
        {
            "name": "mutating sentinel before refusal",
            "ansible.builtin.copy": {
                "content": "bad",
                "dest": "{{ playbook_dir }}/unsafe-marker",
            },
        },
    )
    if not checker.audit_plays(path, changed):
        failures.append("mutation before refusal was accepted")
    return failures, 5


def _controls_and_placement() -> tuple[list[str], int]:
    failures: list[str] = []
    count = 0
    for path, name in checker.REQUIRED.items():
        control_failures, control_count = _mutate_controls(path, name)
        placement_failures, placement_count = _mutate_placement(path, name)
        failures.extend(control_failures + placement_failures)
        count += control_count + placement_count
    return failures, count


def _policy_mutations(pre_base: list[object], full_base: list[object]) -> tuple[list[str], int]:
    failures: list[str] = []
    mutations = 0
    policy_mutations: list[tuple[str, list[object], list[object]]] = []
    changed = copy.deepcopy(pre_base)
    pre_guard = _refusal(changed, checker.REQUIRED[checker.PREFLIGHT])
    pre_guard["ansible.builtin.assert"]["that"][1] = (
        "ansible_architecture != 'aarch64' or ansible_distribution == 'Debian'"
    )
    policy_mutations.append(("Debian 12/Ubuntu 24.04 guard widened", changed, full_base))
    changed = copy.deepcopy(pre_base)
    pre_guard = _refusal(changed, checker.REQUIRED[checker.PREFLIGHT])
    pre_guard["ansible.builtin.assert"]["that"].pop(1)
    policy_mutations.append(("OS release guard removed", changed, full_base))
    changed = copy.deepcopy(full_base)
    full_guard = _refusal(changed, checker.REQUIRED[checker.FULL_PLAY])
    full_guard["ansible.builtin.assert"]["that"][0] = "ansible_architecture != 'x86_64'"
    policy_mutations.append(("amd64-only OpenBao guard inverted", pre_base, changed))
    changed = copy.deepcopy(pre_base)
    changed[0]["roles"] = ["openbao"]
    policy_mutations.append(("OpenBao added to Arm64 structural preflight", changed, full_base))
    changed = copy.deepcopy(pre_base)
    changed[0]["tasks"].append(
        {
            "name": "mutation",
            "ansible.builtin.command": {"cmd": "touch {{ playbook_dir }}/unsafe-marker"},
        }
    )
    policy_mutations.append(("mutating task added to structural preflight", changed, full_base))
    for label, pre_mutation, full_mutation in policy_mutations:
        if not checker.audit_documents(pre_mutation, full_mutation):
            failures.append(f"policy mutation was accepted: {label}")
        mutations += 1

    benign = copy.deepcopy(pre_base)
    report = benign[0]["tasks"][0]
    report["ansible.builtin.debug"]["msg"] = "reworded read-only status"
    if checker.audit_documents(benign, full_base):
        failures.append("benign read-only report wording was rejected")
    mutations += 1
    return failures, mutations


def _static() -> list[str]:
    """Prove the live checker accepts policy and rejects its mutations."""
    if checker.audit():
        return ["production baseline did not pass the static checker"]
    pre_base = _read(checker.PREFLIGHT)
    full_base = _read(checker.FULL_PLAY)
    if checker.audit_documents(pre_base, full_base):
        return ["static checker entrypoint rejected its unmodified baseline"]
    control_failures, control_count = _controls_and_placement()
    policy_failures, policy_count = _policy_mutations(pre_base, full_base)
    failures = control_failures + policy_failures
    if not failures:
        print(f"K3s preflight static mutations: PASS ({control_count + policy_count} cases)")
    return failures


def _ansible_fixture(executable: str, case: AnsibleCase, temp: Path) -> str | None:
    plays = _read(case.source)
    refusal = _refusal(plays, checker.REQUIRED[case.source])
    fixture: dict[str, object] = {
        "name": case.label,
        "hosts": "localhost",
        "gather_facts": False,
        "connection": "local",
        "any_errors_fatal": True,
        "vars": case.values,
        "pre_tasks": [copy.deepcopy(refusal)],
        "tasks": [],
    }
    marker = temp / f"{case.label}.mutated"
    if case.expect_refusal:
        fixture["tasks"] = [
            {
                "name": "mutating sentinel",
                "ansible.builtin.command": {"argv": ["/usr/bin/touch", str(marker)]},
            }
        ]
    else:
        fixture["tasks"] = [
            {
                "name": "successful read-only sentinel",
                "ansible.builtin.debug": {"msg": case.label},
            }
        ]
    playbook = temp / f"{case.label}.yml"
    playbook.write_text(yaml.safe_dump([fixture], sort_keys=False), encoding="utf-8")
    result = subprocess.run(  # noqa: S603 -- executable is resolved from PATH; args target a generated localhost-only fixture
        [executable, "-i", "localhost,", "-c", "local", str(playbook)],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    combined = result.stdout + result.stderr
    errors: list[str] = []
    if case.expect_refusal:
        if result.returncode == 0:
            errors.append(f"{case.label}: unsupported host was accepted")
        if marker.exists():
            errors.append(f"{case.label}: mutation sentinel ran after refusal")
        if "changed=0" not in combined:
            errors.append(f"{case.label}: Ansible output did not prove changed=0")
        if "mutating sentinel" in result.stdout:
            errors.append(f"{case.label}: mutating sentinel appeared in task output")
    else:
        if result.returncode != 0:
            errors.append(f"{case.label}: supported host was refused: {combined.strip()}")
        if marker.exists() or "successful read-only sentinel" not in result.stdout:
            errors.append(f"{case.label}: supported preflight did not complete read-only")
    return "; ".join(errors) or None


def _ansible() -> list[str]:
    executable = shutil.which("ansible-playbook")
    if executable is None:
        return ["ansible-playbook is required for the real fixture selftest"]
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-k3s-preflight-") as raw:
        temp = Path(raw)
        cases = [
            AnsibleCase(
                source=checker.PREFLIGHT,
                values={
                    "ansible_architecture": "aarch64",
                    "ansible_distribution": "Debian",
                    "ansible_distribution_major_version": "12",
                    "ansible_distribution_version": "12",
                },
                expect_refusal=False,
                label="debian12-arm64",
            ),
            AnsibleCase(
                source=checker.PREFLIGHT,
                values={
                    "ansible_architecture": "aarch64",
                    "ansible_distribution": "Ubuntu",
                    "ansible_distribution_major_version": "24",
                    "ansible_distribution_version": "24.04",
                },
                expect_refusal=False,
                label="ubuntu2404-arm64",
            ),
            AnsibleCase(
                source=checker.PREFLIGHT,
                values={
                    "ansible_architecture": "armv7l",
                    "ansible_distribution": "Debian",
                    "ansible_distribution_major_version": "12",
                    "ansible_distribution_version": "12",
                },
                expect_refusal=True,
                label="unsupported-architecture",
            ),
            AnsibleCase(
                source=checker.PREFLIGHT,
                values={
                    "ansible_architecture": "aarch64",
                    "ansible_distribution": "Debian",
                    "ansible_distribution_major_version": "11",
                    "ansible_distribution_version": "11",
                },
                expect_refusal=True,
                label="unsupported-debian-release",
            ),
            AnsibleCase(
                source=checker.FULL_PLAY,
                values={"ansible_architecture": "aarch64"},
                expect_refusal=True,
                label="openbao-arm64-refusal",
            ),
        ]
        for case in cases:
            error = _ansible_fixture(executable, case, temp)
            if error:
                failures.append(error)
        if failures:
            return failures
        print("K3s preflight ansible-playbook fixtures: PASS (5 cases, changed=0 on refusals)")
    return []


def run(*, static: bool, ansible: bool) -> int:
    """Run the selected checker selftest suites."""
    """Run selected static mutation and local Ansible fixture suites."""
    failures: list[str] = []
    if static:
        failures.extend(_static())
    if ansible:
        failures.extend(_ansible())
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    if static and ansible:
        print("K3s Arm64 preflight selftests: PASS")
    return 0
