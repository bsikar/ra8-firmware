# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Parse the read-only preflight that gates native-listener maintenance."""

from __future__ import annotations

import json
import os
import pwd
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import fleet_model as fm
import fleet_path_authority as fpa


class MaintenanceError(ValueError):
    """The preflight did not provide an unambiguous safe decision."""


@dataclass(frozen=True)
class MaintenanceRequest:
    """One read-only preflight and its fail-closed idle-stop transport."""

    preflight_argv: Sequence[str]
    ansible_cwd: Path
    environment: Mapping[str, str]
    host: str
    idle_stop_argv: Sequence[str]
    helper_source: str


@dataclass(frozen=True)
class MaintenanceDecision:
    """Whether the already-preflighted real converge should continue."""

    proceed: bool
    status: int


def playbook_executable(python: str) -> str:
    """Return the executable beside one managed Python, failing closed."""
    candidate = Path(python).parent / "ansible-playbook"
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        message = f"locked Ansible executable is absent beside {python}"
        raise MaintenanceError(message)
    return str(candidate)


def playbook_argv(
    data: dict[str, Any],
    name: str,
    host: dict[str, Any],
    play: str,
    extra: list[str],
) -> list[str]:
    """Build one inventory-backed playbook argv from locked authorities."""
    variables = fm.role_vars(data, name, host)
    argv = [
        playbook_executable(sys.executable),
        "-i",
        str(fm.INVENTORY),
        f"playbooks/{fm.PLAYS[play].playbook}",
        "--limit",
        name,
        "-e",
        json.dumps(variables),
    ]
    if fm.CLASSES[str(host["class"])].transport == "wsl":
        argv += ["-e", f"wsl_ci_host_id={name}"]
    return argv + extra


def applies(host_class: str, plays: Sequence[str], mode: str) -> bool:
    """Return whether this is the native HIL listener's mutating play."""
    return mode == "apply" and host_class == "dev_box" and list(plays) == ["dev-box"]


def _require_real_directory(path: Path, label: str) -> Path:
    """Require one existing directory with no lexical/resolved path drift."""
    lexical = path.absolute()
    try:
        resolved = lexical.resolve(strict=True)
    except OSError as exc:
        message = f"{label} is unavailable: {exc}"
        raise MaintenanceError(message) from exc
    if lexical != resolved or lexical.is_symlink() or not lexical.is_dir():
        message = f"{label} is not a real repository directory"
        raise MaintenanceError(message)
    return resolved


def _require_real_file(path: Path, label: str) -> Path:
    """Require one existing regular file with no lexical/resolved path drift."""
    lexical = path.absolute()
    try:
        resolved = lexical.resolve(strict=True)
    except OSError as exc:
        message = f"{label} is unavailable: {exc}"
        raise MaintenanceError(message) from exc
    if lexical != resolved or lexical.is_symlink() or not lexical.is_file():
        message = f"{label} is not a real repository file"
        raise MaintenanceError(message)
    return resolved


def ansible_environment(environment: Mapping[str, str], ansible_cwd: Path) -> dict[str, str]:
    """Bind Ansible to the repository config without inherited control paths."""
    resolved_cwd = _require_real_directory(ansible_cwd, "Ansible working directory")
    if resolved_cwd.name != "ansible" or resolved_cwd.parent.name != "infra":
        message = "Ansible working directory is not the repository infra/ansible root"
        raise MaintenanceError(message)
    repo_root = _require_real_directory(resolved_cwd.parents[1], "repository root")
    config = _require_real_file(resolved_cwd / "ansible.cfg", "repository Ansible config")
    collection_parent = _require_real_directory(repo_root / ".ansible", "collection parent")
    collections = _require_real_directory(collection_parent / "collections", "collection root")
    if config.parent != resolved_cwd or collections.parent != collection_parent:
        message = "repository Ansible authorities escaped their exact owner"
        raise MaintenanceError(message)
    link_errors = fpa.confined_link_errors(collections)
    if link_errors:
        raise MaintenanceError("; ".join(link_errors))
    del environment
    clean = {
        "HOME": pwd.getpwuid(os.getuid()).pw_dir,
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": "/usr/bin:/bin",
    }
    clean["ANSIBLE_CONFIG"] = str(config)
    clean["ANSIBLE_COLLECTIONS_PATH"] = str(collections)
    clean["ANSIBLE_COLLECTIONS_SCAN_SYS_PATH"] = "false"
    clean["PYTHONNOUSERSITE"] = "1"
    return clean


def callback_environment(environment: Mapping[str, str], ansible_cwd: Path) -> dict[str, str]:
    """Select the exact JSON callback on top of the bound repository config."""
    return {
        **ansible_environment(environment, ansible_cwd),
        "ANSIBLE_LOAD_CALLBACK_PLUGINS": "1",
        "ANSIBLE_STDOUT_CALLBACK": "ansible.posix.json",
    }


def changed(stdout: str, host: str) -> bool:
    """Return the exact host changed verdict from one successful preflight."""
    try:
        document = json.loads(stdout)
    except json.JSONDecodeError as exc:
        message = "native-runner preflight did not emit JSON"
        raise MaintenanceError(message) from exc
    stats = document.get("stats") if isinstance(document, dict) else None
    if not isinstance(stats, dict) or set(stats) != {host}:
        message = "native-runner preflight stats do not name exactly one host"
        raise MaintenanceError(message)
    result = stats[host]
    required = ("changed", "failures", "ignored", "rescued", "unreachable")
    if not isinstance(result, dict) or any(
        type(result.get(key)) is not int or result[key] < 0 for key in required
    ):
        message = "native-runner preflight stats are incomplete"
        raise MaintenanceError(message)
    if any(result[key] for key in required if key != "changed"):
        message = "native-runner preflight reported a non-clean failure outcome"
        raise MaintenanceError(message)
    return result["changed"] > 0


def prepare(request: MaintenanceRequest) -> MaintenanceDecision:
    """Run the read-only preflight, then idle-stop only for real drift."""
    # Exact Ansible argv is derived from the fleet declaration.
    preflight = subprocess.run(  # noqa: S603 -- fleet model supplies exact Ansible argv
        list(request.preflight_argv),
        cwd=request.ansible_cwd,
        env=callback_environment(request.environment, request.ansible_cwd),
        check=False,
        capture_output=True,
        text=True,
        timeout=7200,
    )
    sys.stdout.write(preflight.stdout)
    sys.stderr.write(preflight.stderr)
    if preflight.returncode:
        return MaintenanceDecision(proceed=False, status=preflight.returncode)
    try:
        has_changes = changed(preflight.stdout, request.host)
    except MaintenanceError as exc:
        print(f"fleet: error: {exc}", file=sys.stderr)
        return MaintenanceDecision(proceed=False, status=2)
    if not has_changes:
        print("fleet: native HIL listener is already converged; leaving it running")
        return MaintenanceDecision(proceed=False, status=0)
    # The ssh transport and root-helper argv are fixed, with no shell-derived input.
    stop = subprocess.run(  # noqa: S603 -- fleet model supplies exact SSH helper argv
        list(request.idle_stop_argv),
        input=request.helper_source,
        text=True,
        check=False,
        timeout=60,
    )
    return MaintenanceDecision(
        proceed=stop.returncode == 0,
        status=stop.returncode,
    )


def _environment_fixture(root: Path) -> tuple[Path, Path, dict[str, str]]:
    """Create one isolated repository-shaped Ansible authority."""
    ansible_cwd = root / "infra" / "ansible"
    ansible_cwd.mkdir(parents=True)
    collections = root / ".ansible" / "collections"
    collections.mkdir(parents=True)
    (ansible_cwd / "ansible.cfg").write_text("[defaults]\n", encoding="ascii")
    hostile = {
        "PATH": "/usr/bin:/bin",
        "ANSIBLE_CONFIG": str(ansible_cwd / "hostile.cfg"),
        "ANSIBLE_ROLES_PATH": str(ansible_cwd / "hostile-roles"),
        "ANSIBLE_CALLBACK_PLUGINS": str(ansible_cwd / "hostile-callbacks"),
        "PYTHONHOME": str(ansible_cwd / "hostile-python-home"),
        "PYTHONPATH": str(ansible_cwd / "hostile-python-path"),
    }
    return ansible_cwd, collections, hostile


def _sanitizer_selftest(
    root: Path, ansible_cwd: Path, collections: Path, hostile: dict[str, str]
) -> list[str]:
    """Prove exact configuration and managed executable selection."""
    failures: list[str] = []
    clean = callback_environment(hostile, ansible_cwd)
    expected = {
        "PATH": "/usr/bin:/bin",
        "ANSIBLE_CONFIG": str((ansible_cwd / "ansible.cfg").resolve()),
        "ANSIBLE_COLLECTIONS_PATH": str(collections.resolve()),
        "ANSIBLE_COLLECTIONS_SCAN_SYS_PATH": "false",
        "PYTHONNOUSERSITE": "1",
    }
    if any(clean.get(key) != value for key, value in expected.items()):
        failures.append("repository Ansible environment did not replace hostile controls")
    forbidden = (
        "ANSIBLE_ROLES_PATH",
        "ANSIBLE_CALLBACK_PLUGINS",
        "PYTHONHOME",
        "PYTHONPATH",
    )
    if any(key in clean for key in forbidden):
        failures.append("hostile Ansible or Python import root survived sanitization")
    managed = root / "managed/bin"
    hostile_bin = root / "hostile/bin"
    managed.mkdir(parents=True)
    hostile_bin.mkdir(parents=True)
    python = managed / "python3"
    playbook = managed / "ansible-playbook"
    for path in (python, playbook, hostile_bin / "ansible-playbook"):
        path.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
        path.chmod(0o755)
    hostile["PATH"] = str(hostile_bin)
    if playbook_executable(str(python)) != str(playbook):
        failures.append("hostile PATH replaced locked ansible-playbook")
    return failures


def _expect_link_refusal(hostile: dict[str, str], ansible_cwd: Path, label: str) -> list[str]:
    """Require one linked authority fixture to fail closed."""
    try:
        callback_environment(hostile, ansible_cwd)
    except MaintenanceError:
        return []
    return [f"{label} passed environment binding"]


def _link_selftest(
    root: Path, ansible_cwd: Path, collections: Path, hostile: dict[str, str]
) -> list[str]:
    """Prove config, collection root, and collection parent links are rejected."""
    failures: list[str] = []
    real_collections = root / "real-collections"
    real_collections.mkdir()
    collections.rmdir()
    collections.symlink_to(real_collections, target_is_directory=True)
    failures.extend(_expect_link_refusal(hostile, ansible_cwd, "symlinked collection root"))
    collections.unlink()
    collections.mkdir()

    real_parent = root / "real-parent"
    (real_parent / "collections").mkdir(parents=True)
    (root / ".ansible").rename(root / ".ansible.saved")
    (root / ".ansible").symlink_to(real_parent, target_is_directory=True)
    failures.extend(_expect_link_refusal(hostile, ansible_cwd, "symlinked collection parent"))
    (root / ".ansible").unlink()
    (root / ".ansible.saved").rename(root / ".ansible")

    config = ansible_cwd / "ansible.cfg"
    real_config = ansible_cwd / "real.cfg"
    config.rename(real_config)
    config.symlink_to(real_config)
    failures.extend(_expect_link_refusal(hostile, ansible_cwd, "symlinked Ansible config"))
    return failures


def _collection_tree_selftest(root: Path) -> list[str]:
    """Prove contained links pass while absolute, broken, and escaping fail."""
    failures: list[str] = []
    tree = root / "collection-links"
    tree.mkdir()
    (tree / "target").write_text("owned\n", encoding="ascii")
    (tree / "inside").symlink_to("target")
    if fpa.confined_link_errors(tree):
        failures.append("contained collection link was refused")
    outside = root / "outside"
    outside.write_text("external\n", encoding="ascii")
    hostile_links = (
        ("absolute", outside),
        ("escape", Path("../outside")),
        ("broken", Path("missing")),
    )
    for name, target in hostile_links:
        link = tree / name
        link.symlink_to(target)
        if not fpa.confined_link_errors(tree):
            failures.append(f"{name} collection link was accepted")
        link.unlink()
    return failures


def _environment_selftest() -> list[str]:
    """Prove exact Ansible/Python environment and no-link isolation."""
    with tempfile.TemporaryDirectory(prefix="ra8-ansible-env-") as scratch:
        first = Path(scratch) / "sanitizer"
        ansible_cwd, collections, hostile = _environment_fixture(first)
        failures = _sanitizer_selftest(first, ansible_cwd, collections, hostile)
        second = Path(scratch) / "links"
        ansible_cwd, collections, hostile = _environment_fixture(second)
        failures.extend(_link_selftest(second, ansible_cwd, collections, hostile))
        failures.extend(_collection_tree_selftest(Path(scratch)))
        return failures


def run_selftest() -> list[str]:
    """Prove changed, malformed, and environment decisions."""
    clean = {"changed": 0, "failures": 0, "ignored": 0, "rescued": 0, "unreachable": 0}
    failures = _environment_selftest()
    quiet = json.dumps({"stats": {"dev": clean}})
    dirty = json.dumps({"stats": {"dev": {**clean, "changed": 2}}})
    if changed(quiet, "dev") or not changed(dirty, "dev"):
        failures.append("changed/no-op preflight decision drifted")
    attacks = ["not json", json.dumps({"stats": {}})]
    attacks.append(json.dumps({"stats": {"dev": clean, "star": clean}}))
    outcomes = tuple(clean)
    non_success = tuple(key for key in outcomes if key != "changed")
    attacks.extend(
        json.dumps({"stats": {"dev": {**clean, key: value}}})
        for key, value in ((key, 1) for key in non_success)
    )
    attacks.extend(json.dumps({"stats": {"dev": {**clean, key: -1}}}) for key in outcomes)
    attacks.extend(json.dumps({"stats": {"dev": {**clean, key: "0"}}}) for key in outcomes)
    attacks.extend(
        json.dumps(
            {"stats": {"dev": {name: value for name, value in clean.items() if name != key}}}
        )
        for key in outcomes
    )
    for attack in attacks:
        try:
            changed(attack, "dev")
        except MaintenanceError:
            continue
        failures.append("ambiguous preflight result was accepted")
    return failures
