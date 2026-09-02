# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Mutation tests for the HIL convergence safety checker."""

from __future__ import annotations

import json
from collections.abc import Callable
from typing import cast

import hil_convergence_safety_fixtures as fixtures
import hil_convergence_safety_policy as policy
import hil_convergence_safety_semantic_mutations as semantic_mutations
import hil_convergence_safety_v9 as v9
import yaml

Scan = Callable[[dict[str, str]], list[str]]


class SelftestFixtureError(ValueError):
    """A structural selftest no longer has one exact mutation target."""


def _mutate(inputs: dict[str, str], key: str, old: str, new: str) -> dict[str, str]:
    """Apply one unique must-fire mutation."""
    if inputs[key].count(old) != 1:
        message = f"non-unique selftest fixture in {key}: {old!r}"
        raise SelftestFixtureError(message)
    changed = dict(inputs)
    changed[key] = inputs[key].replace(old, new)
    return changed


def _replace_first(inputs: dict[str, str], key: str, old: str, new: str) -> dict[str, str]:
    """Replace one selected occurrence where repetition is the safety policy."""
    if old not in inputs[key]:
        message = f"missing selftest fixture in {key}: {old!r}"
        raise SelftestFixtureError(message)
    changed = dict(inputs)
    changed[key] = inputs[key].replace(old, new, 1)
    return changed


def _move_dev_task_before(
    inputs: dict[str, str], moving_name: str, before_name: str
) -> dict[str, str]:
    """Move one uniquely named dev-box transaction task before another."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_main"]))
    moving_matches = [index for index, task in enumerate(tasks) if task.get("name") == moving_name]
    before_matches = [index for index, task in enumerate(tasks) if task.get("name") == before_name]
    if len(moving_matches) != 1 or len(before_matches) != 1:
        message = f"non-unique task reorder fixture: {moving_name!r} before {before_name!r}"
        raise SelftestFixtureError(message)
    moving = tasks.pop(moving_matches[0])
    before_at = next(index for index, task in enumerate(tasks) if task.get("name") == before_name)
    tasks.insert(before_at, moving)
    changed = dict(inputs)
    changed["dev_main"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _move_apt_before_idle_proof(inputs: dict[str, str]) -> dict[str, str]:
    """Return a role with a representative job-affecting mutator too early."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_main"]))
    name = "Install the substrate every later step needs"
    apt_at = next(index for index, task in enumerate(tasks) if task.get("name") == name)
    apt = tasks.pop(apt_at)
    tasks.insert(0, apt)
    changed = dict(inputs)
    changed["dev_main"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _weaken_listener_state(inputs: dict[str, str]) -> dict[str, str]:
    """Return a role that accepts transitional and unreadable service states."""
    return _mutate(
        inputs,
        "dev_guard",
        "dev_box_hil_runner_initial_activity.stdout | trim in ['inactive', 'failed']",
        "dev_box_hil_runner_initial_activity.stdout | trim != 'active'",
    )


def _weaken_service_installer(inputs: dict[str, str]) -> dict[str, str]:
    """Return a transaction that runs one installer through caller PATH."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_main"]))
    task = next(item for item in tasks if item.get("name") == "Install the workspace reaper")
    command = cast(dict[str, object], task["ansible.builtin.command"])
    argv = cast(list[str], command["argv"])
    argv[0] = "bash"
    changed = dict(inputs)
    changed["dev_main"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _weaken_hil_recipe_shell(inputs: dict[str, str]) -> dict[str, str]:
    """Return one HIL recipe that re-enables caller startup processing."""
    return _replace_first(inputs, "hil_just", "#!/bin/bash -p", "#!/usr/bin/env bash")


def _weaken_hil_script_shell(inputs: dict[str, str]) -> dict[str, str]:
    """Return one HIL script that resolves its interpreter through PATH."""
    sources = cast(dict[str, str], json.loads(inputs["hil_shells"]))
    path = sorted(sources)[0]
    sources[path] = sources[path].replace("#!/bin/bash -p", "#!/usr/bin/env bash", 1)
    changed = dict(inputs)
    changed["hil_shells"] = json.dumps(sources, sort_keys=True)
    return changed


def _weaken_monitor_service_shell(inputs: dict[str, str]) -> dict[str, str]:
    """Return a monitor generator that resolves service Bash through env."""
    sources = cast(dict[str, str], json.loads(inputs["hil_shells"]))
    path = "scripts/ci/monitor.sh"
    sources[path] = sources[path].replace(
        "ExecStart=/bin/bash -p $self daemon",
        "ExecStart=/usr/bin/env bash $self daemon",
        1,
    )
    changed = dict(inputs)
    changed["hil_shells"] = json.dumps(sources, sort_keys=True)
    return changed


def _bypass_infra_boundary_recipe(inputs: dict[str, str]) -> dict[str, str]:
    """Replace the public sanitation probe with an inert success command."""
    return _mutate(
        inputs,
        "infra_just",
        "{{ infra }} --selftest-boundary",
        "/bin/true",
    )


def _bypass_infra_boundary_endpoint(inputs: dict[str, str]) -> dict[str, str]:
    """Make the dependency-free infra endpoint unconditional."""
    return _mutate(
        inputs,
        "infra_sh",
        'if [[ "${1:-}" == --selftest-boundary ]]; then',
        "if true; then",
    )


def _move_boundary_after_consumer(
    inputs: dict[str, str], boundary_name: str, consumer_name: str
) -> dict[str, str]:
    """Move one check-mode boundary after the bytes it must protect."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_role"]))
    boundary_at = next(
        index for index, task in enumerate(tasks) if task.get("name") == boundary_name
    )
    boundary = tasks.pop(boundary_at)
    consumer_at = next(
        index for index, task in enumerate(tasks) if task.get("name") == consumer_name
    )
    tasks.insert(consumer_at + 1, boundary)
    changed = dict(inputs)
    changed["dev_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _task_control(
    inputs: dict[str, str], task_name: str, key: str, *, value: object
) -> dict[str, str]:
    """Set one top-level control on one governed listener task."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_role"]))
    task = next(item for item in tasks if item.get("name") == task_name)
    task[key] = value
    changed = dict(inputs)
    changed["dev_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _stat_field(
    inputs: dict[str, str], task_name: str, key: str, *, value: object
) -> dict[str, str]:
    """Replace one field in a governed no-follow stat."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_role"]))
    task = next(item for item in tasks if item.get("name") == task_name)
    stat = cast(dict[str, object], task["ansible.builtin.stat"])
    stat[key] = value
    changed = dict(inputs)
    changed["dev_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _task_module(inputs: dict[str, str], task_name: str, old: str, new: str) -> dict[str, str]:
    """Replace one governed task module without changing its arguments."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_role"]))
    task = next(item for item in tasks if item.get("name") == task_name)
    task[new] = task.pop(old)
    changed = dict(inputs)
    changed["dev_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _remove_loop_member(
    inputs: dict[str, str], key: str, task_name: str, member: str
) -> dict[str, str]:
    """Remove one exact string or destination-mapped task-loop member."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs[key]))
    matches = [task for task in tasks if task.get("name") == task_name]
    if len(matches) != 1 or not isinstance(matches[0].get("loop"), list):
        message = f"non-unique loop task in {key}: {task_name!r}"
        raise SelftestFixtureError(message)
    loop = cast(list[object], matches[0]["loop"])
    selected = [
        item
        for item in loop
        if item == member or (isinstance(item, dict) and item.get("dest") == member)
    ]
    if len(selected) != 1:
        message = f"non-unique loop member in {key}: {member!r}"
        raise SelftestFixtureError(message)
    loop.remove(selected[0])
    changed = dict(inputs)
    changed[key] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _remove_manifest_member(inputs: dict[str, str], member: str) -> dict[str, str]:
    """Remove one destination from the shared HIL Python authority manifest."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["bench_role"]))
    task = next(
        item
        for item in tasks
        if item.get("name") == "Define the one HIL Python execution-authority manifest"
    )
    facts = cast(dict[str, object], task["ansible.builtin.set_fact"])
    authorities = cast(list[object], facts["hil_bench_python_authorities"])
    selected = [
        item for item in authorities if isinstance(item, dict) and item.get("dest") == member
    ]
    if len(selected) != 1:
        message = f"non-unique HIL authority: {member!r}"
        raise SelftestFixtureError(message)
    authorities.remove(selected[0])
    changed = dict(inputs)
    changed["bench_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _remove_bench_task(inputs: dict[str, str], task_name: str) -> dict[str, str]:
    """Remove one uniquely named HIL transaction task."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["bench_role"]))
    selected = [task for task in tasks if task.get("name") == task_name]
    if len(selected) != 1:
        message = f"non-unique HIL task: {task_name!r}"
        raise SelftestFixtureError(message)
    tasks.remove(selected[0])
    changed = dict(inputs)
    changed["bench_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _weaken_hil_authority_mode(inputs: dict[str, str]) -> dict[str, str]:
    """Drift one executable-authority mode in the shared manifest."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["bench_role"]))
    task = next(
        item
        for item in tasks
        if item.get("name") == "Define the one HIL Python execution-authority manifest"
    )
    facts = cast(dict[str, object], task["ansible.builtin.set_fact"])
    authorities = cast(list[dict[str, object]], facts["hil_bench_python_authorities"])
    helper = next(item for item in authorities if item.get("dest") == "bootstrap_uv_exec.py")
    helper["mode"] = "0755"
    changed = dict(inputs)
    changed["bench_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _assert_condition(
    inputs: dict[str, str], task_name: str, index: int, condition: str
) -> dict[str, str]:
    """Replace one independently enforced identity predicate."""
    tasks = cast(list[dict[str, object]], yaml.safe_load(inputs["dev_role"]))
    task = next(item for item in tasks if item.get("name") == task_name)
    assertion = cast(dict[str, object], task["ansible.builtin.assert"])
    conditions = cast(list[str], assertion["that"])
    conditions[index] = condition
    changed = dict(inputs)
    changed["dev_role"] = yaml.safe_dump(tasks, sort_keys=False)
    return changed


def _reports(inputs: dict[str, str], scan: Scan, expected: str) -> bool:
    """Require the targeted defect class, not an unrelated scan failure."""
    return expected in scan(inputs)


def _registration_preflight_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return registration preflight identity mutations."""
    registration = "Check whether this runner is already registered"
    registration_id = "Refuse a linked or non-regular runner registration identity"
    registration_error = "hil_runner.yml: first-registration identity/token preflight is not exact"
    return [
        (
            "registration preflight path drift fires its own class",
            _reports(
                _stat_field(
                    inputs,
                    registration,
                    "path",
                    value="{{ dev_box_hil_runner_root }}/.credentials",
                ),
                scan,
                registration_error,
            ),
        ),
        (
            "registration preflight module drift fires its own class",
            _reports(
                _task_module(inputs, registration, "ansible.builtin.stat", "ansible.builtin.file"),
                scan,
                registration_error,
            ),
        ),
        (
            "registration link-following fires its own class",
            _reports(
                _stat_field(inputs, registration, "follow", value=True),
                scan,
                registration_error,
            ),
        ),
        (
            "registration non-regular acceptance fires its own class",
            _reports(
                _assert_condition(inputs, registration_id, 0, "true"),
                scan,
                registration_error,
            ),
        ),
        (
            "registration link acceptance fires its own class",
            _reports(
                _assert_condition(inputs, registration_id, 1, "true"),
                scan,
                registration_error,
            ),
        ),
    ]


def _runner_python_authority_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return CI and HIL runner Python-authority mutations."""
    return (
        [
            (
                f"CI runner {member} {task_name} removal fires",
                bool(scan(_remove_loop_member(inputs, "ci_runner", task_name, member))),
            )
            for member in (
                "scripts/dev/managed_python_env.py",
                "scripts/dev/managed_python_env_checks.py",
            )
            for task_name in (
                "Stage the root-context Python lock and bootstrap inputs",
                "Read back every staged root-context authority byte-for-byte",
                "Assert both Dockerfiles and every locked Python input arrived",
            )
        ]
        + [
            (
                f"HIL authority proof removal fires: {task_name}",
                bool(scan(_remove_bench_task(inputs, task_name))),
            )
            for task_name in (
                "Inspect every deployed HIL Python execution authority without following links",
                "Refuse check mode when any HIL Python authority needs apply",
                "Prove every deployed HIL Python authority is regular and mode-exact",
                "Read back every deployed HIL Python execution authority",
                "Prove every deployed HIL Python authority is byte-exact",
            )
        ]
        + [
            (
                "HIL shared authority mode drift fires",
                bool(scan(_weaken_hil_authority_mode(inputs))),
            )
        ]
    )


def _wsl_python_authority_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return WSL Python-authority mutations."""
    return [
        (
            "WSL explicit uv directory removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl",
                        'f"--no-config --directory {shlex.quote(stage)} sync --locked '
                        '--only-group infra "',
                        'f"--no-config sync --locked --only-group infra "',
                    )
                )
            ),
        ),
        (
            "WSL authenticated uv status masking fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl",
                        'f"{sync_flags}",',
                        'f"{sync_flags} || true",',
                    )
                )
            ),
        ),
        (
            "WSL helper mode proof removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl",
                        'f"644 {helper_digest}",',
                        'f"755 {helper_digest}",',
                    )
                )
            ),
        ),
    ]


def _registration_identity_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return registration and Python-authority identity mutations."""
    preflight_cases, runner_cases, wsl_cases = (
        _registration_preflight_cases(inputs, scan),
        _runner_python_authority_cases(inputs, scan),
        _wsl_python_authority_cases(inputs, scan),
    )
    if not all((preflight_cases, runner_cases, wsl_cases)):
        message = "registration case helper returned no mutation cases"
        raise SelftestFixtureError(message)
    return preflight_cases + runner_cases + wsl_cases


def _public_key_identity_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return public-key path, follow, type, and link mutations."""
    public_key = "Inspect the dedicated HIL public key before account planning"
    identity = "Refuse a linked or non-regular dedicated HIL public key"
    expected = "hil_runner.yml: fresh-account public-key identity preflight is not exact"
    mutations = (
        (
            "path drift",
            _stat_field(
                inputs,
                public_key,
                "path",
                value="{{ dev_box_hil_runner_home }}/.ssh/id_rsa.pub",
            ),
        ),
        ("link-following", _stat_field(inputs, public_key, "follow", value=True)),
        ("non-regular acceptance", _assert_condition(inputs, identity, 0, "true")),
        ("link acceptance", _assert_condition(inputs, identity, 1, "true")),
    )
    return [
        (f"public-key preflight {label} fires its own class", _reports(case, scan, expected))
        for label, case in mutations
    ]


def _assert_failure_control_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return waiver and token-bypass mutations for fail-closed assertions."""
    registration = "Refuse a linked or non-regular runner registration identity"
    requirement_task = "Require a short-lived token only for first registration"
    public_key = "Refuse a linked or non-regular dedicated HIL public key"
    registration_error = "hil_runner.yml: first-registration identity/token preflight is not exact"
    public_key_error = "hil_runner.yml: fresh-account public-key identity preflight is not exact"
    cases = []
    for task, error, label in (
        (registration, registration_error, "registration identity"),
        (requirement_task, registration_error, "first-registration token"),
        (public_key, public_key_error, "public-key identity"),
    ):
        for control, value in (("ignore_errors", True), ("failed_when", False)):
            changed = _task_control(inputs, task, control, value=value)
            cases.append(
                (f"{label} {control} waiver fires its own class", _reports(changed, scan, error))
            )
    bypass = _task_control(inputs, requirement_task, "when", value=False)
    cases.append(
        ("token decision bypass fires its own class", _reports(bypass, scan, registration_error))
    )
    removed = _assert_condition(inputs, requirement_task, 0, "true")
    cases.append(
        (
            "token requirement removal fires its own class",
            _reports(removed, scan, registration_error),
        )
    )
    return cases


def _check_mode_control_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return account, service-plan, and liveness failure-control mutations."""
    user = "Create the isolated HIL runner account and its dedicated SSH key"
    home = "Keep the isolated runner home private"
    start = "Enable and start the dedicated HIL listener"
    verify = "Verify the dedicated HIL listener is active"
    account_error = "hil_runner.yml: fresh-account user/home planning is not exact"
    start_error = "hil_runner.yml: listener start check-mode planning is not exact"
    verify_error = "hil_runner.yml: post-apply listener liveness proof is not exact"
    return [
        (
            "forced check-mode key generation fires its own class",
            _reports(_task_control(inputs, user, "check_mode", value=False), scan, account_error),
        ),
        (
            "forced check-mode home mutation fires its own class",
            _reports(_task_control(inputs, home, "check_mode", value=False), scan, account_error),
        ),
        (
            "forced check-mode listener start fires its own class",
            _reports(_task_control(inputs, start, "check_mode", value=False), scan, start_error),
        ),
        (
            "hidden check-mode listener start fires its own class",
            _reports(
                _task_control(inputs, start, "when", value="not ansible_check_mode"),
                scan,
                start_error,
            ),
        ),
        (
            "ignored liveness failure fires its own class",
            _reports(
                _task_control(inputs, verify, "ignore_errors", value=True), scan, verify_error
            ),
        ),
        (
            "disabled liveness failure fires its own class",
            _reports(_task_control(inputs, verify, "failed_when", value=False), scan, verify_error),
        ),
        (
            "check-mode liveness probe fires its own class",
            _reports(
                _task_control(inputs, verify, "when", value="ansible_check_mode"),
                scan,
                verify_error,
            ),
        ),
    ]


def _fresh_boundary_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return position-sensitive fresh-output boundary mutations."""
    cases = [
        (
            "fresh-account boundary position fires its own class",
            _reports(
                _move_boundary_after_consumer(
                    inputs,
                    "End a fresh-account check before consuming the planned public key",
                    "Read the dedicated HIL public key",
                ),
                scan,
                "hil_runner.yml: fresh-account boundary follows its key consumer",
            ),
        ),
        (
            "fresh-runner boundary position fires its own class",
            _reports(
                _move_boundary_after_consumer(
                    inputs,
                    "End a fresh-runner check before consuming planned package bytes",
                    "Install the official service launcher beside the runner",
                ),
                scan,
                "hil_runner.yml: fresh-runner package boundary follows a byte consumer",
            ),
        ),
    ]
    return (
        cases
        + _registration_identity_cases(inputs, scan)
        + _public_key_identity_cases(inputs, scan)
        + _assert_failure_control_cases(inputs, scan)
        + _check_mode_control_cases(inputs, scan)
    )


def _live_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return live semantic and whole-role ordering cases."""
    return [
        ("complete convergence boundary stays quiet", not scan(inputs)),
        (
            "indented canonical startup authority stays quiet",
            not v9.startup_authority_selftest(),
        ),
        ("v9 live capability attacks stay quiet", not v9.semantic_errors()),
        (
            "public hostile startup stays inert",
            not v9.public_boundary_selftest(policy.REPO_ROOT),
        ),
        (
            "infra public-boundary recipe bypass fires",
            bool(scan(_bypass_infra_boundary_recipe(inputs))),
        ),
        (
            "infra boundary endpoint bypass fires",
            bool(scan(_bypass_infra_boundary_endpoint(inputs))),
        ),
        (
            "recursive workflow closure selftest",
            not policy.workflow_dependency_selftest(),
        ),
        (
            "package mutation before idle proof fires",
            bool(scan(_move_apt_before_idle_proof(inputs))),
        ),
        (
            "transitional listener state fires",
            bool(scan(_weaken_listener_state(inputs))),
        ),
        (
            "caller-PATH workspace installer fires",
            bool(scan(_weaken_service_installer(inputs))),
        ),
        (
            "HIL recipe startup poisoning fires",
            bool(scan(_weaken_hil_recipe_shell(inputs))),
        ),
        (
            "HIL script startup poisoning fires",
            bool(scan(_weaken_hil_script_shell(inputs))),
        ),
        (
            "generated monitor startup poisoning fires",
            bool(scan(_weaken_monitor_service_shell(inputs))),
        ),
        *_fresh_boundary_cases(inputs, scan),
    ]


def _environment_variable_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return environment-variable sanitizer mutations."""
    return [
        (
            "HIL caller-selected tool venv fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "hil_just",
                        'export RA8_TOOL_VENV := ""',
                        'export RA8_TOOL_VENV := env("RA8_TOOL_VENV", "")',
                    )
                )
            ),
        ),
        (
            "Ansible setup sanitizer removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "setup_ansible",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
                        ": # sanitizer removed",
                    )
                )
            ),
        ),
        (
            "toolchain provision TMPDIR sanitizer removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "provision_toolchain",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV TMPDIR",
                        "unset PYTHONHOME PYTHONPATH RA8_TOOL_VENV",
                    )
                )
            ),
        ),
    ]


def _environment_path_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return fixed-path interpreter and shell mutations."""
    return [
        (
            "toolchain provision PATH poisoning fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "provision_toolchain",
                        "PATH=/usr/local/bin:/usr/bin:/bin",
                        "PATH=${PATH}",
                    )
                )
            ),
        ),
        (
            "infra bootstrap PATH Bash fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "infra_bootstrap",
                        '/bin/bash -p "${ROOT}/scripts/dev/setup_ansible.sh"',
                        'bash "${ROOT}/scripts/dev/setup_ansible.sh"',
                    )
                )
            ),
        ),
    ]


def _environment_sanitizer_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return every public environment-sanitizer mutation."""
    return _environment_variable_cases(inputs, scan) + _environment_path_cases(inputs, scan)


def _authenticated_uv_runner_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return authenticated uv-runner mutations."""
    return [
        (
            "infra authenticated uv runner removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "infra_sh",
                        "--run --no-config sync",
                        "--no-config sync",
                    )
                )
            ),
        ),
        (
            "WSL authenticated uv runner removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl",
                        '--run "$@"',
                        '--verify-cache "$@"',
                    )
                )
            ),
        ),
    ]


def _container_helper_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return devcontainer uv-helper mutations."""
    return [
        (
            "devcontainer uv helper allowlist removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "dockerignore",
                        "!scripts/dev/bootstrap_uv_exec.py\n",
                        "",
                    )
                )
            ),
        ),
        (
            "devcontainer uv helper COPY removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "dockerfile",
                        "     scripts/dev/bootstrap_uv_exec.py \\\n",
                        "",
                    )
                )
            ),
        ),
        (
            "devcontainer uv helper canonical-input removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "devcontainer_image",
                        "644 scripts/dev/bootstrap_uv_exec.py\n",
                        "",
                    )
                )
            ),
        ),
    ]


def _runner_helper_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return HIL and CI runner uv-helper mutations."""
    return [
        (
            "HIL uv helper staging removal fires",
            bool(scan(_remove_manifest_member(inputs, "bootstrap_uv_exec.py"))),
        ),
        (
            "CI runner uv helper copy removal fires",
            bool(
                scan(
                    _remove_loop_member(
                        inputs,
                        "ci_runner",
                        "Stage the root-context Python lock and bootstrap inputs",
                        "scripts/dev/bootstrap_uv_exec.py",
                    )
                )
            ),
        ),
        (
            "CI runner uv helper readback removal fires",
            bool(
                scan(
                    _remove_loop_member(
                        inputs,
                        "ci_runner",
                        "Read back every staged root-context authority byte-for-byte",
                        "scripts/dev/bootstrap_uv_exec.py",
                    )
                )
            ),
        ),
        (
            "CI runner uv helper presence-proof removal fires",
            bool(
                scan(
                    _remove_loop_member(
                        inputs,
                        "ci_runner",
                        "Assert both Dockerfiles and every locked Python input arrived",
                        "scripts/dev/bootstrap_uv_exec.py",
                    )
                )
            ),
        ),
    ]


def _wsl_helper_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return WSL uv-helper archive and path-proof mutations."""
    return [
        (
            "WSL uv helper archive removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl_stage",
                        '    "scripts/dev/bootstrap_uv_exec.py",\n',
                        "",
                    )
                )
            ),
        ),
        (
            "WSL uv helper path-proof removal fires",
            bool(
                scan(
                    _mutate(
                        inputs,
                        "fleet_wsl",
                        '        f"{stage}/scripts/dev/bootstrap_uv_exec.py",\n',
                        "",
                    )
                )
            ),
        ),
    ]


def _environment_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return public environment and installer-boundary mutations."""
    sanitizer_cases, uv_runner_cases, container_cases, runner_cases, wsl_cases = (
        _environment_sanitizer_cases(inputs, scan),
        _authenticated_uv_runner_cases(inputs, scan),
        _container_helper_cases(inputs, scan),
        _runner_helper_cases(inputs, scan),
        _wsl_helper_cases(inputs, scan),
    )
    if not all((sanitizer_cases, uv_runner_cases, container_cases, runner_cases, wsl_cases)):
        message = "environment case helper returned no mutation cases"
        raise SelftestFixtureError(message)
    return sanitizer_cases + uv_runner_cases + container_cases + runner_cases + wsl_cases


def _base_cases(inputs: dict[str, str], scan: Scan) -> list[tuple[str, bool]]:
    """Return every hand-authored boundary case."""
    return (
        _live_cases(inputs, scan)
        + _environment_cases(inputs, scan)
        + semantic_mutations.aggregator_cases(inputs, scan)
        + semantic_mutations.digest_cases(inputs, scan)
    )


def run(scan: Scan) -> int:
    """Prove the complete boundary stays quiet and independent removals fire."""
    inputs = policy.load_inputs(policy.REPO_ROOT)
    cases = _base_cases(inputs, scan)
    for label, key, old, new in fixtures.mutations():
        changed = _mutate(inputs, key, old, new)
        expected = semantic_mutations.semantic_image_findings(label, key)
        if expected is not None:
            changed = semantic_mutations.rebind_helper_mutation(changed, key)
            findings = scan(changed)
            passed = len(findings) == len(expected) and set(findings) == set(expected)
        else:
            passed = bool(scan(changed))
        cases.append((f"{label} fires", passed))
    for label, moving_name, before_name in fixtures.reorders():
        changed = _move_dev_task_before(inputs, moving_name, before_name)
        cases.append((f"{label} fires", bool(scan(changed))))
    for event in ("push", "pull_request"):
        for path in policy.workflow_paths(policy.REPO_ROOT):
            changed = policy.remove_workflow_path(inputs, event, path)
            cases.append((f"{event} trigger removal fires: {path}", bool(scan(changed))))
    for label, passed in cases:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
    ok = all(passed for _, passed in cases)
    print(f"check_hil_convergence_safety.py --selftest: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1
