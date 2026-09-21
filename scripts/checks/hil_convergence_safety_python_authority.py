# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate locked Python deployment across HIL and CI execution boundaries."""

from __future__ import annotations

from typing import cast

import yaml


class FixtureError(ValueError):
    """A structural policy lookup no longer has one exact task target."""


def _tasks(source: str, label: str) -> tuple[list[dict[str, object]], list[str]]:
    """Parse one role task list with attribution."""
    try:
        value = yaml.safe_load(source)
    except yaml.YAMLError:
        return [], [f"{label}: malformed YAML"]
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        return [], [f"{label}: expected a task list"]
    return cast(list[dict[str, object]], value), []


def _named(tasks: list[dict[str, object]], name: str) -> tuple[int, dict[str, object]]:
    """Return one uniquely named top-level task."""
    matches = [(index, task) for index, task in enumerate(tasks) if task.get("name") == name]
    if len(matches) != 1:
        message = f"task {name!r} is missing or duplicated"
        raise FixtureError(message)
    return matches[0]


def _named_loop(source: str, name: str, label: str) -> tuple[list[object], list[str]]:
    """Return one exact Ansible task loop for deployment-policy checks."""
    tasks, errors = _tasks(source, label)
    if errors:
        return [], errors
    try:
        _, task = _named(tasks, name)
    except FixtureError as exc:
        return [], [f"{label}: {exc}"]
    loop = task.get("loop")
    if not isinstance(loop, list):
        return [], [f"{label}: task {name!r} has no literal loop"]
    return loop, []


def _authority(source: str, destination: str, mode: str) -> dict[str, str]:
    """Build one exact source-to-destination authority manifest row."""
    return {"src": source, "dest": destination, "mode": mode}


def _expected_hil_manifest() -> list[dict[str, str]]:
    """Return the canonical HIL Python execution-authority manifest."""
    root = "{{ role_path }}/../../../../"
    return [
        _authority(f"{root}pyproject.toml", "pyproject.toml", "0644"),
        _authority(f"{root}uv.lock", "uv.lock", "0644"),
        _authority(f"{root}scripts/dev/bootstrap_uv.py", "bootstrap_uv.py", "0755"),
        _authority(f"{root}scripts/dev/bootstrap_uv_exec.py", "bootstrap_uv_exec.py", "0644"),
        _authority(f"{root}scripts/dev/uv_release.json", "uv_release.json", "0644"),
        _authority(
            f"{root}scripts/dev/verify_locked_environment.py",
            "verify_locked_environment.py",
            "0755",
        ),
        _authority("{{ role_path }}/files/requirements.lock", "requirements.lock", "0644"),
    ]


def _context_deployment_errors(inputs: dict[str, str], helper: str) -> list[str]:
    """Require the helper in the devcontainer context and image policy."""
    errors: list[str] = []
    if inputs["dockerignore"].splitlines().count(f"!{helper}") != 1:
        errors.append("devcontainer context: bootstrap execution helper is not allowlisted")
    docker_copy = "COPY scripts/dev/bootstrap_uv.py \\\n     scripts/dev/bootstrap_uv_exec.py \\"
    if inputs["dockerfile"].count(docker_copy) != 1:
        errors.append("devcontainer Dockerfile: bootstrap execution helper is not copied")
    image = inputs["devcontainer_image"]
    if (
        image.splitlines().count(f"!{helper}") != 1
        or image.splitlines().count(f"644 {helper}") != 1
    ):
        errors.append("devcontainer image policy: bootstrap execution helper is not canonical")
    return errors


def _hil_manifest_errors(source: str) -> list[str]:
    """Require the one exact HIL Python execution-authority manifest."""
    tasks, errors = _tasks(source, "HIL")
    if errors:
        return errors
    try:
        _, manifest_task = _named(tasks, "Define the one HIL Python execution-authority manifest")
    except FixtureError as exc:
        return [f"HIL: {exc}"]
    facts = manifest_task.get("ansible.builtin.set_fact")
    actual = facts.get("hil_bench_python_authorities") if isinstance(facts, dict) else None
    if actual != _expected_hil_manifest():
        return ["HIL: shared Python execution-authority manifest is not exact"]
    return []


def _ci_stage_spec(helper: str) -> list[dict[str, str]]:
    """Return the exact CI root-context staging manifest."""
    return [
        _authority(".dockerignore", ".dockerignore", "0644"),
        _authority("pyproject.toml", "pyproject.toml", "0644"),
        _authority("uv.lock", "uv.lock", "0644"),
        _authority("scripts/dev/bootstrap_uv.py", "scripts/dev/bootstrap_uv.py", "0755"),
        _authority(helper, helper, "0644"),
        _authority(
            "scripts/dev/managed_python_env.py", "scripts/dev/managed_python_env.py", "0755"
        ),
        _authority(
            "scripts/dev/managed_python_env_checks.py",
            "scripts/dev/managed_python_env_checks.py",
            "0755",
        ),
        _authority("scripts/dev/uv_release.json", "scripts/dev/uv_release.json", "0644"),
    ]


def _ci_readback_spec(helper: str) -> list[str]:
    """Return the exact CI root-context readback manifest."""
    return [
        ".dockerignore",
        "pyproject.toml",
        "scripts/dev/bootstrap_uv.py",
        helper,
        "scripts/dev/managed_python_env.py",
        "scripts/dev/managed_python_env_checks.py",
        "scripts/dev/uv_release.json",
        "uv.lock",
    ]


def _ci_presence_spec(helper: str) -> list[str]:
    """Return every CI root-context input that must be present."""
    return [
        ".dockerignore",
        ".devcontainer/Dockerfile",
        "pyproject.toml",
        "runner/Dockerfile",
        "scripts/dev/bootstrap_uv.py",
        helper,
        "scripts/dev/managed_python_env.py",
        "scripts/dev/managed_python_env_checks.py",
        "scripts/dev/uv_release.json",
        "uv.lock",
    ]


def _ci_deployment_errors(source: str, helper: str) -> list[str]:
    """Require all CI root-context authority loops to remain exact."""
    specs = (
        ("Stage the root-context Python lock and bootstrap inputs", _ci_stage_spec(helper)),
        ("Read back every staged root-context authority byte-for-byte", _ci_readback_spec(helper)),
        (
            "Assert both Dockerfiles and every locked Python input arrived",
            _ci_presence_spec(helper),
        ),
    )
    errors: list[str] = []
    for task_name, wanted in specs:
        loop, loop_errors = _named_loop(source, task_name, "CI runner")
        errors.extend(loop_errors)
        if loop != wanted:
            errors.append(f"CI runner: {task_name} authority list is not exact")
    return errors


def _wsl_deployment_errors(stage: str, wsl: str, helper: str) -> list[str]:
    """Require WSL staging and path proof to cover the execution helper."""
    errors: list[str] = []
    if stage.count(f'    "{helper}",') != 1:
        errors.append("fleet WSL stage: bootstrap execution helper is not archived")
    proof = '        f"{stage}/scripts/dev/bootstrap_uv_exec.py",'
    if wsl.count(proof) != 1:
        errors.append("fleet WSL: bootstrap execution helper is not path-proven")
    return errors


def uv_helper_deployment_errors(inputs: dict[str, str]) -> list[str]:
    """Require every deployed bootstrap to carry its adjacent execution helper."""
    helper = "scripts/dev/bootstrap_uv_exec.py"
    errors = _context_deployment_errors(inputs, helper)
    errors.extend(_hil_manifest_errors(inputs["bench_role"]))
    errors.extend(_ci_deployment_errors(inputs["ci_runner"], helper))
    errors.extend(_wsl_deployment_errors(inputs["fleet_wsl_stage"], inputs["fleet_wsl"], helper))
    return errors


def _selected_hil_proof_tasks(
    tasks: list[dict[str, object]],
) -> tuple[list[tuple[int, dict[str, object]]], list[str]]:
    """Select all uniquely named HIL Python authority proof tasks."""
    names = (
        "Stage the exact HIL Python project and bootstrap inputs",
        "Inspect every deployed HIL Python execution authority without following links",
        "Refuse check mode when any HIL Python authority needs apply",
        "Prove every deployed HIL Python authority is regular and mode-exact",
        "Read back every deployed HIL Python execution authority",
        "Prove every deployed HIL Python authority is byte-exact",
    )
    selected: list[tuple[int, dict[str, object]]] = []
    errors: list[str] = []
    for name in names:
        try:
            selected.append(_named(tasks, name))
        except FixtureError as exc:
            errors.append(f"HIL: {exc}")
    return selected, errors


def _hil_stage_proof_is_exact(
    stage: dict[str, object],
    stat: dict[str, object],
    refusal: dict[str, object],
    identity: dict[str, object],
) -> bool:
    """Return whether HIL staging, no-follow stat, and mode proof are exact."""
    return (
        stage.get("loop") == "{{ hil_bench_python_authorities }}"
        and stage.get("register") == "hil_bench_python_stage"
        and stage.get("ansible.builtin.copy")
        == {
            "src": "{{ item.src }}",
            "dest": "{{ hil_bench_python_context }}/{{ item.dest }}",
            "owner": "root",
            "group": "root",
            "mode": "{{ item.mode }}",
        }
        and stat.get("loop") == "{{ hil_bench_python_authorities }}"
        and stat.get("register") == "hil_bench_python_authority_stats"
        and stat.get("ansible.builtin.stat")
        == {"path": "{{ hil_bench_python_context }}/{{ item.dest }}", "follow": False}
        and stat.get("changed_when") is False
        and stat.get("check_mode") is False
        and refusal.get("ansible.builtin.assert", {}).get("that")
        == [
            "not ansible_check_mode or (hil_bench_python_stage.results | "
            "selectattr('changed') | list | length == 0)"
        ]
        and identity.get("ansible.builtin.assert", {}).get("that")
        == [
            "item.stat.exists",
            "item.stat.isreg",
            "not item.stat.islnk",
            "item.stat.mode == item.item.mode",
        ]
        and identity.get("loop") == "{{ hil_bench_python_authority_stats.results }}"
    )


def _hil_readback_proof_is_exact(
    readback: dict[str, object], byte_proof: dict[str, object]
) -> bool:
    """Return whether HIL authority readback and byte proof are exact."""
    return (
        readback.get("loop") == "{{ hil_bench_python_authorities }}"
        and readback.get("register") == "hil_bench_python_authority_bytes"
        and readback.get("ansible.builtin.slurp")
        == {"src": "{{ hil_bench_python_context }}/{{ item.dest }}"}
        and readback.get("changed_when") is False
        and readback.get("check_mode") is False
        and byte_proof.get("loop") == "{{ hil_bench_python_authority_bytes.results }}"
        and byte_proof.get("ansible.builtin.assert", {}).get("that")
        == [
            "(item.content | b64decode | hash('sha256')) == "
            "(lookup('file', item.item.src, rstrip=false) | hash('sha256'))"
        ]
    )


def _hil_proof_order_errors(
    tasks: list[dict[str, object]], selected: list[tuple[int, dict[str, object]]]
) -> list[str]:
    """Require every exact authority proof before Python execution."""
    proof_indices = [index for index, _ in selected]
    execution_indices = [
        index
        for index, task in enumerate(tasks)
        if "hil_bench_python_context" in str(task.get("ansible.builtin.command", {}))
    ]
    if proof_indices != sorted(proof_indices) or (
        execution_indices and proof_indices[-1] >= min(execution_indices)
    ):
        return ["HIL: Python execution can precede exact deployed-authority proof"]
    return []


def hil_python_authority_errors(source: str) -> list[str]:
    """Require one fail-closed HIL authority proof before Python execution."""
    tasks, errors = _tasks(source, "HIL")
    if errors:
        return errors
    selected, errors = _selected_hil_proof_tasks(tasks)
    if errors:
        return errors
    stage, stat, refusal, identity, readback, byte_proof = [task for _, task in selected]
    exact = _hil_stage_proof_is_exact(stage, stat, refusal, identity)
    exact = exact and _hil_readback_proof_is_exact(readback, byte_proof)
    if not exact:
        errors.append("HIL: Python authority copy/readback/mode proof is not exact")
    errors.extend(_hil_proof_order_errors(tasks, selected))
    return errors
