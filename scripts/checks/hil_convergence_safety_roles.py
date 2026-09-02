# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate the authenticated bench-role entry points."""

from __future__ import annotations

import re
from typing import cast

import yaml

ROLE_PREFIX_LENGTH = 2
LOCAL_AUTH_ARGC = 10
FLEET_PAYLOAD_KEYS = 18
REMOTE_VERIFY_ARGC = 9
BENCH_HOLDER_DECISION = (
    "hil_bench_maintenance_record.resource == 'bench'",
    "hil_bench_maintenance_record.lock_id == hil_bench_maintenance_lock_id",
    "hil_bench_maintenance_record.hold_kind == 'wrapped'",
)
BENCH_HOLDER_FILE_PROOF = (
    "hil_bench_maintenance_record_stat.stat.exists",
    "hil_bench_maintenance_record_stat.stat.isreg | default(false)",
    "not hil_bench_maintenance_record_stat.stat.islnk | default(false)",
)


class RoleError(ValueError):
    """A required uniquely named task is missing or duplicated."""


def _service_installer_errors(source: str) -> list[str]:
    """Require fixed privileged Bash argv for both generated user services."""
    try:
        tasks = yaml.safe_load(source)
    except yaml.YAMLError:
        return ["dev_box transaction: malformed YAML"]
    if not isinstance(tasks, list):
        return ["dev_box transaction: task list is malformed"]
    expected = {
        "Install the shared CI status poller": [
            "/bin/bash",
            "-p",
            "scripts/ci/monitor.sh",
            "install-service",
        ],
        "Install the workspace reaper": [
            "/bin/bash",
            "-p",
            "scripts/dev/agent_workspace.sh",
            "install-timer",
        ],
    }
    found: dict[str, object] = {}
    for task in tasks:
        if isinstance(task, dict) and task.get("name") in expected:
            command = task.get("ansible.builtin.command")
            found[str(task["name"])] = command.get("argv") if isinstance(command, dict) else None
    return [] if found == expected else ["dev_box transaction: service installer argv is not exact"]


def _dev_shell_command_errors(source: str) -> list[str]:
    """Reject PATH/startup-sensitive Bash commands in the dev-box transaction."""
    try:
        tasks = yaml.safe_load(source)
    except yaml.YAMLError:
        return ["dev_box transaction: malformed YAML"]
    failures: list[str] = []
    for task in tasks if isinstance(tasks, list) else []:
        if not isinstance(task, dict):
            continue
        command = task.get("ansible.builtin.command")
        if isinstance(command, dict):
            raw = command.get("cmd")
            argv = command.get("argv")
            if isinstance(raw, str) and re.search(r"(^|\s)bash\s", raw):
                failures.append(str(task.get("name", "unnamed command")))
            if (
                isinstance(argv, list)
                and any(
                    isinstance(item, str) and item.startswith("scripts/") and item.endswith(".sh")
                    for item in argv
                )
                and argv[:2] != ["/bin/bash", "-p"]
            ):
                failures.append(str(task.get("name", "unnamed command")))
    guard = next(
        (
            task
            for task in tasks
            if isinstance(task, dict)
            if task.get("name") == "Keep the system Bash startup guard safe under nounset"
        ),
        {},
    )
    replace = guard.get("ansible.builtin.replace") if isinstance(guard, dict) else None
    if not isinstance(replace, dict) or replace.get("validate") != "/bin/bash -p -n %s":
        failures.append("Bash startup guard validator")
    return [f"dev_box transaction: unsafe Bash boundary: {name}" for name in failures]


def _hil_just_errors(source: str) -> list[str]:
    """Require every HIL recipe shell boundary to enter fixed privileged Bash."""
    errors: list[str] = []
    expected_environment = {
        'export BASH_ENV := "/dev/null"',
        'export ENV := "/dev/null"',
        'export PYTHONHOME := ""',
        'export PYTHONPATH := ""',
        'export PYTHONNOUSERSITE := "1"',
        'export RA8_TOOL_VENV := ""',
        'export PATH := ".venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"',
    }
    if expected_environment - set(source.splitlines()):
        errors.append("just/hil.just: public environment sanitizer is incomplete")
    shebangs = [line.strip() for line in source.splitlines() if line.lstrip().startswith("#!")]
    if any(line != "#!/bin/bash -p" for line in shebangs):
        errors.append("just/hil.just: recipe shebang is not fixed privileged Bash")
    executable = [line for line in source.splitlines() if not line.lstrip().startswith("#")]
    if any(re.search(r"(?<!/)\bbash\b", line) for line in executable):
        errors.append("just/hil.just: recipe invokes Bash through caller PATH")
    return errors


def dev_box_shell_boundary_errors(transaction_source: str, hil_just: str) -> list[str]:
    """Return service, transaction-shell, and HIL-Just boundary findings."""
    return (
        _service_installer_errors(transaction_source)
        + _dev_shell_command_errors(transaction_source)
        + _hil_just_errors(hil_just)
    )


def _pin_authority_errors(transaction_source: str, dockerfile_source: str) -> list[str]:
    """Require every role-consumed pin to exist in the Dockerfile authority."""
    try:
        tasks = yaml.safe_load(transaction_source)
    except yaml.YAMLError:
        return ["dev box pins: malformed transaction"]
    if not isinstance(tasks, list):
        return ["dev box pins: transaction is not a task list"]
    matches = [
        task
        for task in tasks
        if isinstance(task, dict)
        and task.get("name") == "Assert every pin this role consumes is actually declared there"
    ]
    consumed = matches[0].get("loop") if len(matches) == 1 else None
    if not isinstance(consumed, list) or any(not isinstance(pin, str) for pin in consumed):
        return ["dev box pins: consumed-pin census is missing or malformed"]
    declared = set(re.findall(r"(?m)^ARG ([A-Z0-9_]+)=", dockerfile_source))
    missing = sorted(pin for pin in consumed if pin not in declared)
    if missing:
        return ["dev box pins: consumed names absent from Dockerfile: " + ", ".join(missing)]
    return []


def _shell_authority_errors(root_justfile: str) -> list[str]:
    """Require Just to enter every recipe with fixed privileged Bash."""
    definitions = [
        line.strip() for line in root_justfile.splitlines() if line.startswith("set shell")
    ]
    expected = ['set shell := ["/bin/bash", "-puc"]']
    return [] if definitions == expected else ["justfile: public shell authority is not exact"]


def pin_and_shell_authority_errors(
    transaction_source: str, dockerfile_source: str, root_justfile: str
) -> list[str]:
    """Return pin-declaration and public Just-shell authority findings."""
    return _pin_authority_errors(transaction_source, dockerfile_source) + _shell_authority_errors(
        root_justfile
    )


def startup_authority_selftest(prefix: tuple[str, ...]) -> list[str]:
    """Prove presentation indentation does not change wrapper semantics."""
    indented = tuple(f"  {line}" for line in prefix)
    return (
        []
        if [line.strip() for line in indented] == [line.strip() for line in prefix]
        else ["indented canonical startup authority changed semantics"]
    )


def _tasks(source: str, label: str) -> tuple[list[dict[str, object]], list[str]]:
    """Parse one role task list with attribution."""
    try:
        value = yaml.safe_load(source)
    except yaml.YAMLError:
        return [], [f"{label}: malformed YAML"]
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        return [], [f"{label}: expected a task list"]
    return cast(list[dict[str, object]], value), []


def _normalized(value: object) -> str:
    """Collapse presentation whitespace without weakening expression bytes."""
    return " ".join(str(value).split())


def _conditions(task: dict[str, object]) -> tuple[str, ...]:
    """Return normalized Ansible assert conditions, or an empty tuple."""
    assertion = task.get("ansible.builtin.assert")
    values = assertion.get("that") if isinstance(assertion, dict) else None
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        return ()
    return tuple(_normalized(value) for value in values)


def _fact_value(task: object, key: str) -> str:
    """Return one normalized set_fact value, empty on shape drift."""
    fact = task.get("ansible.builtin.set_fact") if isinstance(task, dict) else None
    value = fact.get(key) if isinstance(fact, dict) else None
    return _normalized(value)


def _named(tasks: list[dict[str, object]], name: str) -> dict[str, object]:
    """Return one uniquely named top-level task."""
    matches = [task for task in tasks if task.get("name") == name]
    if len(matches) != 1:
        message = f"task {name!r} is missing or duplicated"
        raise RoleError(message)
    return matches[0]


def _include_guard_errors(
    tasks: list[dict[str, object]],
    name: str,
    module: str,
    expected: dict[str, object],
    label: str,
) -> list[str]:
    """Require an always-selected guard and its authenticated fact assertion."""
    if len(tasks) < ROLE_PREFIX_LENGTH:
        return [f"{label}: authenticated role prefix is incomplete"]
    include = tasks[0]
    assertion = tasks[1]
    fact = (
        "hil_bench_transaction_authenticated | default(false) | bool"
        if "whole-bench" in name
        else "dev_box_hil_mutation_authenticated | default(false) | bool"
    )
    if (
        include.get("name") != name
        or include.get(module) != expected
        or include.get("tags") != ["always"]
        or _conditions(assertion) != (fact,)
    ):
        return [f"{label}: independently selected role bypasses authentication"]
    return []


def _health_errors(tasks: list[dict[str, object]], defaults: str) -> list[str]:
    """Require the final health check to reuse the outer hold."""
    name = "Health check -- the EK-RA8D2 must be reachable over J-Link (EIL==HIL ground truth)"
    try:
        health = _named(tasks, name)
    except RoleError as exc:
        return [f"hil_bench role: {exc}"]
    shell = health.get("ansible.builtin.shell")
    command = shell.get("cmd") if isinstance(shell, dict) else None
    expected = (
        "device=$(/bin/bash -p {{ (hil_bench_repo_dir ~ "
        "'/scripts/hil/lib/rig_contract.sh') | quote }} --default JLINK_DEVICE) && "
        r"printf 'si 1\nspeed {{ hil_bench_jlink_speed }}\nr\nh\nq\n' "
        '> /tmp/hil_bench_ping.jlink && JLinkExe -device "$device" -if SWD '
        "-speed {{ hil_bench_jlink_speed }} "
        "-autoconnect 1 -CommandFile /tmp/hil_bench_ping.jlink"
    )
    errors = []
    if (
        _normalized(command) != expected
        or health.get("register") != "hil_bench_jlink_probe"
        or health.get("changed_when") is not False
        or health.get("failed_when") != "'Cortex-M85' not in hil_bench_jlink_probe.stdout"
    ):
        errors.append("hil_bench role: final health check can deadlock or escaped the outer hold")
    document = yaml.safe_load(defaults)
    if not isinstance(document, dict) or document.get("hil_bench_maintenance_lock_id") != "":
        errors.append("hil_bench defaults: direct apply does not fail closed")
    if isinstance(document, dict) and "hil_bench_jlink_device" in document:
        errors.append("hil_bench defaults: duplicates the rig-contract J-Link device")
    return errors


def _binding_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require controller-payload and kernel-lock authentication at the prefix."""
    errors = []
    hold_expected = _normalized(
        "ansible_check_mode or hil_bench_maintenance_lock_id is match('^[0-9a-f]{16}$')"
    )
    if _conditions(tasks[0]) != (hold_expected,):
        errors.append("hil_bench role: maintenance hold assertion is not exact")
    binding = tasks[1].get("block")
    if tasks[1].get("when") != "not ansible_check_mode" or not isinstance(binding, list):
        return [*errors, "hil_bench role: live holder binding block is not exact"]
    expected_names = (
        "Authenticate the controller and immutable fleet bench payload",
        "Authenticate the canonical kernel-held bench lock",
    )
    if tuple(task.get("name") for task in binding if isinstance(task, dict)) != expected_names:
        return [*errors, "hil_bench role: live capability task sequence is not exact"]
    by_name = {task.get("name"): task for task in binding if isinstance(task, dict)}
    return [
        *errors,
        *_local_binding_errors(by_name[expected_names[0]]),
        *_remote_binding_errors(by_name[expected_names[1]]),
    ]


def _local_binding_errors(local: object) -> list[str]:
    """Require the exact controller transaction and fleet payload key set."""
    local_command = local.get("ansible.builtin.command") if isinstance(local, dict) else None
    local_argv = local_command.get("argv") if isinstance(local_command, dict) else None
    if (
        not isinstance(local, dict)
        or local.get("delegate_to") != "localhost"
        or local.get("become") is not False
        or local.get("changed_when") is not False
        or not isinstance(local_argv, list)
        or len(local_argv) != LOCAL_AUTH_ARGC
        or local_argv[:5]
        != [
            "{{ playbook_dir }}/../../../.venv/bin/python3",
            "-I",
            "{{ playbook_dir }}/../../../scripts/dev/fleet_transaction_auth.py",
            "{{ inventory_hostname }}",
            "hil_bench",
        ]
    ):
        return ["hil_bench role: fleet transaction command is not exact"]
    payload = (
        str(local_argv[5])
        if isinstance(local_argv, list) and len(local_argv) == LOCAL_AUTH_ARGC
        else ""
    )
    keys = re.findall(r"'(hil_bench_[a-z0-9_]+)'\s*:", payload)
    expected_tail = [
        "{{ hil_bench_maintenance_lock_id }}",
        "{{ hil_bench_maintenance_holder_pid | string }}",
        "{{ hil_bench_maintenance_holder_start_ticks | string }}",
        "{{ hil_bench_maintenance_holder_target }}",
    ]
    if (
        len(keys) != FLEET_PAYLOAD_KEYS
        or len(set(keys)) != FLEET_PAYLOAD_KEYS
        or not isinstance(local_argv, list)
        or local_argv[6:] != expected_tail
    ):
        return ["hil_bench role: immutable fleet payload key set is not exact"]
    return []


def _remote_binding_errors(remote: object) -> list[str]:
    """Require the exact by-value verifier and both reviewed digests."""
    remote_command = remote.get("ansible.builtin.command") if isinstance(remote, dict) else None
    remote_argv = remote_command.get("argv") if isinstance(remote_command, dict) else None
    if (
        not isinstance(remote, dict)
        or remote.get("delegate_to") is not None
        or remote.get("changed_when") is not False
        or not isinstance(remote_argv, list)
        or len(remote_argv) != REMOTE_VERIFY_ARGC
        or remote_argv[:4] != ["/usr/bin/python3", "-I", "-S", "-c"]
        or remote_argv[5:7] != ["{{ hil_bench_maintenance_lock_id }}", "wrapped"]
    ):
        return ["hil_bench role: kernel lock verifier command is not exact"]
    if isinstance(remote_argv, list) and len(remote_argv) == REMOTE_VERIFY_ARGC:
        source = _normalized(remote_argv[4])
        digest = _normalized(remote_argv[7])
        broker_digest = _normalized(remote_argv[8])
        expected_source = _normalized(
            "{{ lookup('ansible.builtin.file', "
            "playbook_dir ~ '/../../../scripts/hil/lib/bench_lock_verify.py', rstrip=false) }}"
        )
        expected_digest = _normalized(
            "{{ lookup('ansible.builtin.file', "
            "playbook_dir ~ '/../../../scripts/hil/lib/bench_host.sh', rstrip=false) "
            "| hash('sha256') }}"
        )
        expected_broker_digest = _normalized(
            "{{ lookup('ansible.builtin.file', "
            "playbook_dir ~ '/../../../scripts/hil/lib/bench_lock_broker.py', rstrip=false) "
            "| hash('sha256') }}"
        )
        if (
            source != expected_source
            or digest != expected_digest
            or broker_digest != expected_broker_digest
        ):
            return ["hil_bench role: reviewed verifier/holder bytes are not exact"]
    return []


def _role_prefix_errors(source: str, label: str, role: str) -> list[str]:
    """Require each independently includable bench role to authenticate first."""
    tasks, errors = _tasks(source, label)
    if errors:
        return errors
    if role == "hil_bench":
        return _include_guard_errors(
            tasks,
            "Authenticate the whole-bench transaction before this role",
            "ansible.builtin.include_tasks",
            {"file": "transaction_guard.yml", "apply": {"tags": ["always"]}},
            label,
        )
    role_label = "C6" if role == "c6_toolchain" else "AD2"
    return _include_guard_errors(
        tasks,
        f"Authenticate the whole-bench transaction before the {role_label} role",
        "ansible.builtin.include_role",
        {
            "name": "hil_bench",
            "tasks_from": "transaction_guard.yml",
            "apply": {"tags": ["always"]},
        },
        label,
    )


def _entry_errors(source: str, label: str, name: str, file: str) -> list[str]:
    """Require a single dynamic, always-selected transaction entry point."""
    tasks, errors = _tasks(source, label)
    expected = {"file": file}
    if errors:
        return errors
    if (
        len(tasks) != 1
        or tasks[0].get("name") != name
        or tasks[0].get("ansible.builtin.include_tasks") != expected
        or tasks[0].get("tags") != ["always"]
    ):
        return [f"{label}: transaction entry can expose internal mutators to selectors"]
    return []


def _entry_point_errors(inputs: dict[str, str]) -> list[str]:
    """Validate each public dynamic transaction entry."""
    entries = (
        (
            "dev_main_entry",
            "dev_box/tasks/main.yml",
            "Enter the authenticated dev-box transaction",
            "transaction.yml",
        ),
        (
            "dev_entry",
            "dev_box/tasks/hil_runner.yml",
            "Enter the authenticated HIL-listener transaction",
            "hil_runner_transaction.yml",
        ),
        (
            "bench_entry",
            "hil_bench/tasks/main.yml",
            "Enter the authenticated bench transaction",
            "transaction.yml",
        ),
        (
            "c6_entry",
            "c6_toolchain/tasks/main.yml",
            "Enter the authenticated C6 transaction",
            "transaction.yml",
        ),
        (
            "ad2_entry",
            "ad2_tools/tasks/main.yml",
            "Enter the authenticated AD2 transaction",
            "transaction.yml",
        ),
    )
    findings: list[str] = []
    for key, label, name, file in entries:
        findings.extend(_entry_errors(inputs[key], label, name, file))
    return findings


def errors(inputs: dict[str, str]) -> list[str]:
    """Require live-holder binding before task one and no nested health lock."""
    tasks, findings = _tasks(inputs["bench_role"], "hil_bench/tasks/main.yml")
    if findings:
        return findings
    guard_tasks, guard_errors = _tasks(
        inputs["bench_guard"], "hil_bench/tasks/transaction_guard.yml"
    )
    if guard_errors:
        return guard_errors
    if len(guard_tasks) < ROLE_PREFIX_LENGTH:
        return ["hil_bench role: live holder prefix is incomplete"]
    expected = [
        "Require the fleet-owned maintenance transaction for a mutating converge",
        "Bind this apply to the exact live wrapped bench holder",
    ]
    if [task.get("name") for task in guard_tasks[:2]] != expected:
        findings.append("hil_bench role: lock assertion/binding is not before every mutator")
    findings.extend(_binding_errors(guard_tasks))
    findings.extend(_health_errors(tasks, inputs["bench_defaults"]))
    findings.extend(
        _role_prefix_errors(inputs["bench_role"], "hil_bench/tasks/main.yml", "hil_bench")
    )
    findings.extend(
        _role_prefix_errors(inputs["c6_role"], "c6_toolchain/tasks/main.yml", "c6_toolchain")
    )
    findings.extend(
        _role_prefix_errors(inputs["ad2_role"], "ad2_tools/tasks/main.yml", "ad2_tools")
    )
    return [*findings, *_entry_point_errors(inputs)]
