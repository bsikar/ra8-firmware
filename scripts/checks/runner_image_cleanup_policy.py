# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate scoped Buildah and CRI cleanup for deployed runner images."""

from __future__ import annotations

import yaml

MANAGED_IMAGE_LABEL = "com.ra8-firmware.managed=runner-fleet"
MANAGED_IMAGE_KIND = "com.ra8-firmware.image-kind"
NamedTasks = dict[str, list[tuple[int, dict[str, object]]]]


def _named_tasks(source: str) -> tuple[NamedTasks, list[str]]:
    """Parse the deployed producer into uniquely addressable task candidates."""
    try:
        value = yaml.safe_load(source)
    except yaml.YAMLError:
        return {}, ["ci_runner image cleanup: task file is not valid YAML"]
    if not isinstance(value, list):
        return {}, ["ci_runner image cleanup: task file is not a list"]
    named: NamedTasks = {}
    for index, item in enumerate(value):
        if isinstance(item, dict) and isinstance(item.get("name"), str):
            named.setdefault(item["name"], []).append((index, item))
    return named, []


def _one(named: NamedTasks, name: str, errors: list[str]) -> tuple[int, dict[str, object]] | None:
    """Return one named task while making absence or duplication a finding."""
    matches = named.get(name, [])
    if len(matches) != 1:
        errors.append(f"ci_runner image cleanup: expected one task named {name!r}")
        return None
    return matches[0]


def _argv(task: dict[str, object]) -> object:
    """Return an Ansible command task's argv value."""
    command = task.get("ansible.builtin.command")
    return command.get("argv") if isinstance(command, dict) else None


def capacity_field_manager_errors(source: str) -> list[str]:
    """Require temporary ARC capacity changes to share Helm's field manager."""
    executable = "\n".join(
        line for line in source.splitlines() if not line.lstrip().startswith("#")
    )
    normalized = " ".join(executable.replace("\\\n", " ").split())
    patches = normalized.count("kc patch autoscalingrunnerset")
    managed = normalized.count("--field-manager=helm --type=merge")
    if patches > 0 and managed == patches:
        return []
    return ["fleet capacity: ARC patch does not share Helm's field manager"]


def _build_label_errors(named: NamedTasks) -> tuple[list[str], int]:
    """Require both producer images to carry ownership and kind labels."""
    errors: list[str] = []
    positions: list[int] = []
    specs = (
        ("Build the devcontainer toolchain image (single source of truth)", "devcontainer"),
        ("Build the runner image (devcontainer + actions-runner)", "runner"),
    )
    for name, kind in specs:
        match = _one(named, name, errors)
        if match is None:
            continue
        index, task = match
        positions.append(index)
        command = task.get("ansible.builtin.command")
        cmd = command.get("cmd", "") if isinstance(command, dict) else ""
        labels = (MANAGED_IMAGE_LABEL, f"{MANAGED_IMAGE_KIND}={kind}")
        errors.extend(
            f"ci_runner image cleanup: {name!r} lacks label {label!r}"
            for label in labels
            if f"--label {label}" not in str(cmd)
        )
    return errors, max(positions, default=-1)


def _buildah_kind_errors(named: NamedTasks, kind: str, previous: int) -> tuple[list[str], int]:
    """Require one dangling-only removal pair after the prior managed step."""
    errors: list[str] = []
    find_name = f"Find superseded managed {kind} images"
    remove_name = f"Remove superseded managed {kind} images"
    register = f"ci_runner_dangling_{kind}_images"
    find_match = _one(named, find_name, errors)
    remove_match = _one(named, remove_name, errors)
    if find_match is None or remove_match is None:
        return errors, previous
    find_index, find_task = find_match
    remove_index, remove_task = remove_match
    expected_find = [
        "buildah", "images", "--filter", f"label={MANAGED_IMAGE_LABEL}",
        "--filter", f"label={MANAGED_IMAGE_KIND}={kind}", "--filter",
        "dangling=true", "--quiet", "--no-trunc",
    ]
    if _argv(find_task) != expected_find:
        errors.append(f"ci_runner image cleanup: {find_name!r} selector is not exact")
    if find_task.get("register") != register or find_task.get("changed_when") is not False:
        errors.append(f"ci_runner image cleanup: {find_name!r} receipt is not exact")
    if _argv(remove_task) != ["buildah", "rmi", "{{ item }}"]:
        errors.append(f"ci_runner image cleanup: {remove_name!r} argv is not exact")
    if (
        remove_task.get("loop") != f"{{{{ {register}.stdout_lines }}}}"
        or remove_task.get("when") != "item | length > 0"
        or remove_task.get("changed_when") is not True
    ):
        errors.append(f"ci_runner image cleanup: {remove_name!r} loop is not exact")
    if not previous < find_index < remove_index:
        errors.append(f"ci_runner image cleanup: {kind} cleanup order is not exact")
    return errors, remove_index


def _cri_inventory_errors(inventory: dict[str, object], reset: dict[str, object]) -> list[str]:
    """Require an immutable CRI inventory and empty selection receipt."""
    errors: list[str] = []
    if (
        _argv(inventory) != ["k3s", "crictl", "images", "-o", "json"]
        or inventory.get("register") != "ci_runner_cri_images"
        or inventory.get("changed_when") is not False
        or inventory.get("when") != "not ansible_check_mode"
    ):
        errors.append("ci_runner image cleanup: CRI inventory receipt is not exact")
    facts = reset.get("ansible.builtin.set_fact")
    expected_repository = "{{ ci_runner_image | regex_replace(':[^/:]+$', '') }}"
    if not isinstance(facts, dict) or (
        facts.get("ci_runner_stale_cri_images") != []
        or facts.get("ci_runner_image_repository") != expected_repository
        or reset.get("changed_when") is not False
        or reset.get("when") != "not ansible_check_mode"
    ):
        errors.append("ci_runner image cleanup: CRI selection reset is not exact")
    return errors


def _normalized_conditions(task: dict[str, object]) -> set[str]:
    """Return whitespace-stable conditions from one Ansible task."""
    conditions = task.get("when")
    if not isinstance(conditions, list):
        return set()
    return {" ".join(str(condition).split()) for condition in conditions}


def _cri_selection_errors(select: dict[str, object], remove: dict[str, object]) -> list[str]:
    """Require exact untagged repository selection and bounded CRI removal."""
    errors: list[str] = []
    facts = select.get("ansible.builtin.set_fact")
    selection = facts.get("ci_runner_stale_cri_images") if isinstance(facts, dict) else None
    expected_conditions = {
        "not ansible_check_mode",
        "item.repoTags | default([]) | length == 0",
        "item.repoDigests | default([]) | select( 'match', '^' ~ "
        "(ci_runner_image_repository | regex_escape) ~ "
        "'@sha256:[0-9a-f]{64}$' ) | list | length > 0",
    }
    if (
        selection != "{{ ci_runner_stale_cri_images + [item.id] }}"
        or select.get("loop") != "{{ (ci_runner_cri_images.stdout | from_json).images }}"
        or _normalized_conditions(select) != expected_conditions
        or select.get("changed_when") is not False
    ):
        errors.append("ci_runner image cleanup: CRI ownership selection is not exact")
    if (
        _argv(remove) != ["k3s", "crictl", "rmi", "{{ item }}"]
        or remove.get("loop") != "{{ ci_runner_stale_cri_images }}"
        or remove.get("when") != "not ansible_check_mode"
        or remove.get("changed_when") is not True
    ):
        errors.append("ci_runner image cleanup: CRI removal loop is not exact")
    return errors


def _cri_errors(named: NamedTasks, previous: int) -> list[str]:
    """Require a repository-scoped CRI sweep after current-image publication."""
    errors: list[str] = []
    names = (
        "Inventory CRI images after publishing the current runner",
        "Reset the superseded runner image selection",
        "Select superseded untagged runner images",
        "Remove superseded untagged runner images",
    )
    matches = [_one(named, name, errors) for name in names]
    if any(match is None for match in matches):
        return errors
    inventory, reset, select, remove = matches
    positions = [inventory[0], reset[0], select[0], remove[0]]
    if positions != sorted(positions) or previous >= positions[0]:
        errors.append("ci_runner image cleanup: CRI cleanup order is not exact")
    errors.extend(_cri_inventory_errors(inventory[1], reset[1]))
    errors.extend(_cri_selection_errors(select[1], remove[1]))
    return errors


def _helm_ownership_errors(named: NamedTasks) -> list[str]:
    """Require Helm to reclaim declared runner fields from transient managers."""
    errors: list[str] = []
    match = _one(named, "Install the ra8-ci runner scale set", errors)
    if match is None:
        return errors
    module = match[1].get("kubernetes.core.helm")
    if not isinstance(module, dict) or module.get("force_conflicts") is not True:
        errors.append("ci_runner image cleanup: Helm does not reclaim declared field ownership")
    return errors


def errors(source: str) -> list[str]:
    """Require the deployed producer to delete only its own stale images."""
    named, findings = _named_tasks(source)
    if findings:
        return findings
    build_findings, previous = _build_label_errors(named)
    findings.extend(build_findings)
    for kind in ("runner", "devcontainer"):
        kind_findings, previous = _buildah_kind_errors(named, kind, previous)
        findings.extend(kind_findings)
    findings.extend(_cri_errors(named, previous))
    findings.extend(_helm_ownership_errors(named))
    return findings
