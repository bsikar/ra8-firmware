# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Two-way trust boundary between native HIL and shared compiler caches."""

from __future__ import annotations

from pathlib import Path

HIL_RUNNER_TASKS = "infra/ansible/roles/dev_box/tasks/hil_runner_transaction.yml"
HIL_RUNNER_SERVICE = "infra/ansible/roles/dev_box/templates/ra8-hil-runner.service.j2"
SHARED_CACHE_TASKS = "infra/ansible/roles/dev_box/tasks/transaction.yml"
SHARED_CACHE_ROOT = "/var/cache/ccache-ra8"
HIL_CACHE_ROOT = "/var/cache/ccache-ra8-hil"


def check_texts(hil_tasks: str, hil_service: str, shared_tasks: str) -> list[str]:
    """Keep the private HIL and shared interactive cache domains disjoint."""
    problems = []
    for relative, text in ((HIL_RUNNER_TASKS, hil_tasks), (HIL_RUNNER_SERVICE, hil_service)):
        if SHARED_CACHE_ROOT in text or "dev_box_ccache_dir" in text:
            problems.append(f"{relative}: HIL surface references the shared compiler cache")
    if HIL_CACHE_ROOT in shared_tasks or "dev_box_hil_ccache_dir" in shared_tasks:
        problems.append(f"{SHARED_CACHE_TASKS}: shared cache surface references the HIL cache")

    expected_service = (
        'Environment="CCACHE_DIR={{ dev_box_hil_ccache_dir }}"',
        'Environment="RA8_CCACHE_DIR={{ dev_box_hil_ccache_dir }}"',
    )
    actual_service = tuple(line for line in hil_service.splitlines() if "CCACHE_DIR=" in line)
    if actual_service != expected_service:
        problems.append(f"{HIL_RUNNER_SERVICE}: cache environment is not exactly HIL-private")

    required_hil_fragments = (
        'path: "{{ dev_box_hil_ccache_dir }}"',
        'dest: "{{ dev_box_hil_ccache_dir }}/ccache.conf"',
        'owner: "{{ dev_box_hil_runner_user }}"',
        'group: "{{ dev_box_hil_runner_group }}"',
        'mode: "0700"',
        "when: not ansible_check_mode",
        "ansible.builtin.tempfile:",
    )
    if any(fragment not in hil_tasks for fragment in required_hil_fragments):
        problems.append(f"{HIL_RUNNER_TASKS}: private cache converge/probe contract is incomplete")
    if "ansible.posix.acl" in hil_tasks:
        problems.append(f"{HIL_RUNNER_TASKS}: HIL cache must not use shared-access ACLs")
    return problems


def check(repo_root: Path) -> list[str]:
    """Read and validate the two disjoint full-role cache surfaces."""
    try:
        return check_texts(
            (repo_root / HIL_RUNNER_TASKS).read_text(encoding="utf-8"),
            (repo_root / HIL_RUNNER_SERVICE).read_text(encoding="utf-8"),
            (repo_root / SHARED_CACHE_TASKS).read_text(encoding="utf-8"),
        )
    except (OSError, UnicodeError) as exc:
        return [f"HIL/shared compiler-cache isolation cannot be read: {exc}"]


def selftest() -> list[str]:
    """Prove shared and HIL cache references cannot cross either way."""
    hil_tasks = """path: "{{ dev_box_hil_ccache_dir }}"
dest: "{{ dev_box_hil_ccache_dir }}/ccache.conf"
owner: "{{ dev_box_hil_runner_user }}"
group: "{{ dev_box_hil_runner_group }}"
mode: "0700"
when: not ansible_check_mode
ansible.builtin.tempfile:
"""
    hil_service = """Environment="CCACHE_DIR={{ dev_box_hil_ccache_dir }}"
Environment="RA8_CCACHE_DIR={{ dev_box_hil_ccache_dir }}"
"""
    shared_tasks = 'path: "{{ dev_box_ccache_dir }}"\n'
    failures = []
    if check_texts(hil_tasks, hil_service, shared_tasks):
        failures.append("  approved private/shared cache split was rejected")
    mutations = {
        "HIL task shared variable": (hil_tasks + "\ndev_box_ccache_dir", hil_service, shared_tasks),
        "HIL task shared literal": (
            hil_tasks + f"\n{SHARED_CACHE_ROOT}",
            hil_service,
            shared_tasks,
        ),
        "HIL service shared variable": (
            hil_tasks,
            hil_service.replace("dev_box_hil_ccache_dir", "dev_box_ccache_dir"),
            shared_tasks,
        ),
        "shared task HIL variable": (
            hil_tasks,
            hil_service,
            shared_tasks + "\ndev_box_hil_ccache_dir",
        ),
        "shared task HIL literal": (hil_tasks, hil_service, shared_tasks + f"\n{HIL_CACHE_ROOT}"),
        "HIL ACL restored": (hil_tasks + "\nansible.posix.acl:", hil_service, shared_tasks),
    }
    failures.extend(
        f"  cache trust-boundary crossing was accepted: {name}"
        for name, texts in mutations.items()
        if not check_texts(*texts)
    )
    return failures
