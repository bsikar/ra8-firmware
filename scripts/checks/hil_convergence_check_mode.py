# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate fresh-listener dry-run and post-apply service boundaries."""

from __future__ import annotations

LISTENER_ACTIVE_RETRIES = 15
LISTENER_ACTIVE_DELAY_S = 2
ASSERT_TASK_KEYS = frozenset({"name", "ansible.builtin.assert"})
ASSERT_ARGUMENT_KEYS = frozenset({"that", "fail_msg"})
STAT_TASK_KEYS = frozenset({"name", "become", "ansible.builtin.stat", "register", "changed_when"})


class CheckModeFixtureError(ValueError):
    """A governed listener task is missing or duplicated."""


def _named(tasks: list[dict[str, object]], name: str) -> tuple[int, dict[str, object]]:
    """Return one uniquely named top-level task."""
    matches = [(index, task) for index, task in enumerate(tasks) if task.get("name") == name]
    if len(matches) != 1:
        message = f"task {name!r} is missing or duplicated"
        raise CheckModeFixtureError(message)
    return matches[0]


def _normalized(value: object) -> str:
    """Collapse presentation whitespace without weakening expression bytes."""
    return " ".join(str(value).split())


def _conditions(task: dict[str, object]) -> tuple[str, ...]:
    """Return normalized assert conditions, or an empty tuple."""
    assertion = task.get("ansible.builtin.assert")
    values = assertion.get("that") if isinstance(assertion, dict) else None
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        return ()
    return tuple(_normalized(value) for value in values)


def _exact_assert(task: dict[str, object], expected: tuple[str, ...]) -> bool:
    """Return whether one fail-closed assertion has only its governed controls."""
    assertion = task.get("ansible.builtin.assert")
    return (
        set(task) == ASSERT_TASK_KEYS
        and isinstance(assertion, dict)
        and set(assertion) == ASSERT_ARGUMENT_KEYS
        and isinstance(assertion.get("fail_msg"), str)
        and _conditions(task) == expected
    )


def _exact_stat(task: dict[str, object], path: str, result: str) -> bool:
    """Return whether one identity stat is exact and does not follow links."""
    stat = task.get("ansible.builtin.stat")
    return (
        set(task) == STAT_TASK_KEYS
        and task.get("become") is True
        and isinstance(stat, dict)
        and stat == {"path": path, "follow": False}
        and task.get("register") == result
        and task.get("changed_when") is False
    )


def _registration_errors(
    registration: dict[str, object], identity: dict[str, object], token: dict[str, object]
) -> list[str]:
    """Require an exact no-follow registration marker and token decision."""
    identity_expected = (
        _normalized(
            "not dev_box_hil_runner_registration.stat.exists or "
            "dev_box_hil_runner_registration.stat.isreg"
        ),
        _normalized(
            "not dev_box_hil_runner_registration.stat.exists or "
            "not dev_box_hil_runner_registration.stat.islnk"
        ),
    )
    token_expected = _normalized(
        "dev_box_hil_runner_registration.stat.exists or "
        "(dev_box_hil_runner_registration_token | default('') | length > 0)"
    )
    if (
        not _exact_stat(
            registration,
            "{{ dev_box_hil_runner_root }}/.runner",
            "dev_box_hil_runner_registration",
        )
        or not _exact_assert(identity, identity_expected)
        or not _exact_assert(token, (token_expected,))
    ):
        return ["hil_runner.yml: first-registration identity/token preflight is not exact"]
    return []


def _public_key_errors(key_stat: dict[str, object], identity: dict[str, object]) -> list[str]:
    """Require an exact real, regular, no-follow public-key decision."""
    identity_expected = (
        _normalized(
            "not dev_box_hil_runner_public_key_stat.stat.exists or "
            "dev_box_hil_runner_public_key_stat.stat.isreg"
        ),
        _normalized(
            "not dev_box_hil_runner_public_key_stat.stat.exists or "
            "not dev_box_hil_runner_public_key_stat.stat.islnk"
        ),
    )
    if not _exact_stat(
        key_stat,
        "{{ dev_box_hil_runner_home }}/.ssh/id_ed25519.pub",
        "dev_box_hil_runner_public_key_stat",
    ) or not _exact_assert(identity, identity_expected):
        return ["hil_runner.yml: fresh-account public-key identity preflight is not exact"]
    return []


def _account_errors(user: dict[str, object], home: dict[str, object]) -> list[str]:
    """Require ordinary check-mode planning for account and home creation."""
    user_expected = {
        "name": "{{ dev_box_hil_runner_user }}",
        "group": "{{ dev_box_hil_runner_group }}",
        "home": "{{ dev_box_hil_runner_home }}",
        "shell": "/usr/sbin/nologin",
        "system": True,
        "create_home": True,
        "generate_ssh_key": True,
        "ssh_key_type": "ed25519",
        "ssh_key_file": ".ssh/id_ed25519",
        "ssh_key_comment": "ra8-hil@dev",
    }
    home_expected = {
        "path": "{{ dev_box_hil_runner_home }}",
        "state": "directory",
        "owner": "{{ dev_box_hil_runner_user }}",
        "group": "{{ dev_box_hil_runner_group }}",
        "mode": "0700",
    }
    if (
        set(user) != {"name", "become", "ansible.builtin.user"}
        or user.get("become") is not True
        or user.get("ansible.builtin.user") != user_expected
        or set(home) != {"name", "become", "ansible.builtin.file"}
        or home.get("become") is not True
        or home.get("ansible.builtin.file") != home_expected
    ):
        return ["hil_runner.yml: fresh-account user/home planning is not exact"]
    return []


def _fresh_account_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require token preflight, then stop before a planned key is consumed."""
    names = (
        "Require the fleet-derived native HIL declaration",
        "Check whether this runner is already registered",
        "Refuse a linked or non-regular runner registration identity",
        "Require a short-lived token only for first registration",
        "Install the official runner's Debian runtime dependencies",
        "Inspect the dedicated HIL public key before account planning",
        "Refuse a linked or non-regular dedicated HIL public key",
        "Create the isolated HIL runner account and its dedicated SSH key",
        "Keep the isolated runner home private",
        "Explain the fresh-account check-mode boundary",
        "End a fresh-account check before consuming the planned public key",
        "Read the dedicated HIL public key",
    )
    found = [_named(tasks, name) for name in names]
    indices = [index for index, _ in found]
    declaration, registration, registration_id, token, apt = found[:5]
    key_stat, key_id, user, home, explain, end, slurp = found[5:]
    boundary_when = ["ansible_check_mode", "not dev_box_hil_runner_public_key_stat.stat.exists"]
    errors = []
    if registration[0] != declaration[0] + 1 or token[0] >= apt[0]:
        errors.append("hil_runner.yml: registration/token preflight follows a mutator")
    if indices != sorted(indices) or indices[6:11] != list(range(indices[5] + 1, indices[5] + 6)):
        errors.append("hil_runner.yml: fresh-account token/key boundary order is not exact")
    errors.extend(_registration_errors(registration[1], registration_id[1], token[1]))
    errors.extend(_public_key_errors(key_stat[1], key_id[1]))
    errors.extend(_account_errors(user[1], home[1]))
    if explain[1].get("when") != boundary_when or not isinstance(
        explain[1].get("ansible.builtin.debug"), dict
    ):
        errors.append("hil_runner.yml: fresh-account boundary explanation is not exact")
    if end[1].get("when") != boundary_when or end[1].get("ansible.builtin.meta") != "end_host":
        errors.append("hil_runner.yml: fresh-account check does not end before key consumption")
    if end[0] >= slurp[0]:
        errors.append("hil_runner.yml: fresh-account boundary follows its key consumer")
    return errors


def _fresh_package_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require a fresh-package dry run to stop before package byte consumers."""
    names = (
        "Decide whether the pinned runner package must be installed",
        "Explain the fresh-runner check-mode boundary",
        "End a fresh-runner check before consuming planned package bytes",
        "Install the official service launcher beside the runner",
        "Read back the installed runner version",
    )
    found = [_named(tasks, name) for name in names]
    decide, explain, end, launcher, version = found
    boundary_when = ["ansible_check_mode", "dev_box_hil_runner_install_needed | bool"]
    errors = []
    if explain[0] != decide[0] + 1 or end[0] != explain[0] + 1:
        errors.append("hil_runner.yml: fresh-runner package boundary order is not exact")
    if end[0] >= launcher[0] or end[0] >= version[0]:
        errors.append("hil_runner.yml: fresh-runner package boundary follows a byte consumer")
    if explain[1].get("when") != boundary_when or not isinstance(
        explain[1].get("ansible.builtin.debug"), dict
    ):
        errors.append("hil_runner.yml: fresh-runner boundary explanation is not exact")
    if end[1].get("when") != boundary_when or end[1].get("ansible.builtin.meta") != "end_host":
        errors.append("hil_runner.yml: fresh-runner check does not end before byte consumers")
    return errors


def _listener_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require planned start in check mode and live acceptance only after apply."""
    start_at, start = _named(tasks, "Enable and start the dedicated HIL listener")
    verify_at, verify = _named(tasks, "Verify the dedicated HIL listener is active")
    service = start.get("ansible.builtin.systemd_service")
    start_expected = {
        "name": "{{ dev_box_hil_runner_service }}",
        "enabled": True,
        "state": "started",
        "daemon_reload": True,
    }
    errors = []
    if (
        set(start) != {"name", "become", "ansible.builtin.systemd_service"}
        or start.get("become") is not True
        or service != start_expected
    ):
        errors.append("hil_runner.yml: listener start check-mode planning is not exact")
    command = verify.get("ansible.builtin.command")
    argv = command.get("argv") if isinstance(command, dict) else None
    allowed = {
        "name",
        "when",
        "become",
        "ansible.builtin.command",
        "register",
        "changed_when",
        "check_mode",
        "retries",
        "delay",
        "until",
    }
    if (
        verify_at != start_at + 1
        or set(verify) != allowed
        or verify.get("when") != "not ansible_check_mode"
        or verify.get("become") is not True
        or verify.get("register") != "dev_box_hil_runner_active"
        or verify.get("changed_when") is not False
        or verify.get("check_mode") is not False
        or verify.get("retries") != LISTENER_ACTIVE_RETRIES
        or verify.get("delay") != LISTENER_ACTIVE_DELAY_S
        or _normalized(verify.get("until"))
        != _normalized("dev_box_hil_runner_active.stdout | trim == 'active'")
        or argv != ["systemctl", "is-active", "{{ dev_box_hil_runner_service }}"]
    ):
        errors.append("hil_runner.yml: post-apply listener liveness proof is not exact")
    return errors


def errors(tasks: list[dict[str, object]]) -> list[str]:
    """Return every fresh-listener check-mode boundary defect."""
    try:
        return _fresh_account_errors(tasks) + _fresh_package_errors(tasks) + _listener_errors(tasks)
    except CheckModeFixtureError as error:
        return [f"hil_runner.yml: {error}"]
