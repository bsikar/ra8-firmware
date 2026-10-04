#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Audit the K3s architecture refusal boundary without contacting hosts."""

from __future__ import annotations

import argparse
import importlib
import re
import sys
from collections.abc import Iterator
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "infra/ansible/playbooks/k3s-arm64-preflight.yml"
FULL_PLAY = ROOT / "infra/ansible/playbooks/k3s-node.yml"
CONTROL_KEYS = {
    "when",
    "failed_when",
    "changed_when",
    "check_mode",
    "run_once",
    "delegate_to",
    "delegate_facts",
    "local_action",
    "connection",
    "until",
    "retries",
    "delay",
    "async",
    "poll",
    "ignore_errors",
    "ignore_unreachable",
    "throttle",
    "loop",
    "loop_control",
    "become",
    "become_user",
    "become_method",
    "environment",
    "no_log",
    "diff",
    "notify",
    "listen",
    "vars",
}
ALLOWED_REFUSAL_TASK_KEYS = {"name", "ansible.builtin.assert"}
MUTATING_MODULES = {
    "ansible.builtin.apt",
    "ansible.builtin.command",
    "ansible.builtin.copy",
    "ansible.builtin.file",
    "ansible.builtin.get_url",
    "ansible.builtin.pip",
    "ansible.builtin.shell",
    "ansible.builtin.systemd_service",
    "ansible.builtin.tempfile",
    "ansible.builtin.unarchive",
}
REQUIRED = {
    PREFLIGHT: "Refuse unsupported K3s preflight OS or architecture",
    FULL_PLAY: "Refuse OpenBao outside amd64",
}


def _tasks(value: object) -> Iterator[tuple[dict[str, object], tuple[str, ...]]]:
    """Walk tasks while retaining block/rescue/always placement."""
    if not isinstance(value, list):
        return
    for entry in value:
        if not isinstance(entry, dict):
            continue
        yield entry, ()
        for section in ("block", "rescue", "always"):
            nested = entry.get(section)
            if isinstance(nested, list):
                for child, _ in _tasks(nested):
                    yield child, (section,)


def _plays(path: Path) -> tuple[list[object], list[str]]:
    try:
        content = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        return [], [f"cannot read {path.relative_to(ROOT)}: {exc}"]
    if (
        not isinstance(content, list)
        or not content
        or not all(isinstance(play, dict) for play in content)
    ):
        return [], [f"{path.relative_to(ROOT)} must contain a non-empty play list"]
    return content, []


def _named_refusal(
    play: dict[str, object], name: str, path: Path, findings: list[str]
) -> dict[str, object] | None:
    all_matches: list[tuple[dict[str, object], tuple[str, ...]]] = []
    for section in ("pre_tasks", "tasks", "post_tasks"):
        all_matches.extend(
            (task, placement)
            for task, placement in _tasks(play.get(section))
            if task.get("name") == name
        )
    if len(all_matches) != 1:
        findings.append(f"{path.name}: expected one mandatory refusal task {name!r}")
        return None
    task, placement = all_matches[0]
    pre_tasks = play.get("pre_tasks")
    if (
        placement
        or not isinstance(pre_tasks, list)
        or not any(item is task for item in pre_tasks if isinstance(item, dict))
    ):
        findings.append(f"{path.name}: refusal {name!r} must be a direct pre_task")
    if any(key in task for key in CONTROL_KEYS):
        keys = sorted(key for key in CONTROL_KEYS if key in task)
        findings.append(f"{path.name}: refusal {name!r} has execution controls {keys}")
    unexpected = sorted(set(task) - ALLOWED_REFUSAL_TASK_KEYS)
    if unexpected:
        findings.append(f"{path.name}: refusal {name!r} has unapproved task keys {unexpected}")
    if "tags" in task:
        findings.append(f"{path.name}: refusal {name!r} must not have tags")
    if any(key in task for key in ("block", "rescue", "always")):
        findings.append(f"{path.name}: refusal {name!r} cannot wrap task sections")
    if "ansible.builtin.assert" not in task or not isinstance(
        task.get("ansible.builtin.assert"), dict
    ):
        findings.append(f"{path.name}: refusal {name!r} must use FQCN assert")
    return task


def _normal_expression(value: object) -> str:
    text = " ".join(str(value).split())
    return re.sub(r"\s*([()\[\],])\s*", r"\1", text)


def _exact_assertions(
    task: dict[str, object] | None, expected: tuple[str, ...], path: Path
) -> list[str]:
    if task is None:
        return []
    module = task.get("ansible.builtin.assert")
    actual = module.get("that") if isinstance(module, dict) else None
    if not isinstance(actual, list) or tuple(map(_normal_expression, actual)) != tuple(
        map(_normal_expression, expected)
    ):
        return [f"{path.name}: refusal assertions do not match the closed OS/architecture policy"]
    return []


def audit_plays(path: Path, plays: list[object]) -> list[str]:
    """Check parsed play data against one playbook's refusal contract."""
    findings: list[str] = []
    expected_name = REQUIRED[path]
    if len(plays) != 1:
        return [f"{path.name}: expected exactly one play"]
    play = plays[0]
    if not isinstance(play, dict):
        return [f"{path.name}: play must be a mapping"]
    refusal = _named_refusal(play, expected_name, path, findings)
    if refusal is None:
        return findings
    if play.get("gather_facts") is False:
        findings.append(f"{path.name}: refusal preflight requires gathered host facts")
    if play.get("tags"):
        findings.append(f"{path.name}: play-level tags could bypass the refusal")
    if play.get("any_errors_fatal") is not True:
        findings.append(f"{path.name}: refusal must be fatal across the play")
    pre_tasks = play.get("pre_tasks", [])
    if not isinstance(pre_tasks, list) or not pre_tasks or pre_tasks[0] is not refusal:
        findings.append(f"{path.name}: refusal must be the first pre_task")
    for task, placement in _tasks(pre_tasks):
        if placement:
            continue
        if task is refusal:
            continue
        if any(key in task for key in ("block", "rescue", "always")):
            findings.append(f"{path.name}: execution-control block in pre_tasks")
    return findings


def audit_document(path: Path) -> list[str]:
    """Read and check a playbook without invoking Ansible or a host."""
    plays, findings = _plays(path)
    if findings:
        return findings
    findings.extend(audit_plays(path, plays))
    return findings


def audit_documents(pre_plays: list[object], full_plays: list[object]) -> list[str]:
    """Audit parsed inputs; used by both the repository check and mutations."""
    findings: list[str] = []
    findings.extend(audit_plays(PREFLIGHT, pre_plays))
    findings.extend(audit_plays(FULL_PLAY, full_plays))
    if pre_plays and isinstance(pre_plays[0], dict):
        task = _named_refusal(pre_plays[0], REQUIRED[PREFLIGHT], PREFLIGHT, findings)
        findings.extend(
            _exact_assertions(
                task,
                (
                    "ansible_architecture in ['aarch64', 'x86_64']",
                    "ansible_architecture != 'aarch64' or "
                    "(ansible_distribution == 'Debian' and "
                    "ansible_distribution_major_version == '12') or "
                    "(ansible_distribution == 'Ubuntu' and "
                    "ansible_distribution_version == '24.04')",
                ),
                PREFLIGHT,
            )
        )
    if full_plays and isinstance(full_plays[0], dict):
        task = _named_refusal(full_plays[0], REQUIRED[FULL_PLAY], FULL_PLAY, findings)
        findings.extend(_exact_assertions(task, ("ansible_architecture == 'x86_64'",), FULL_PLAY))
        roles = full_plays[0].get("roles")
        if not isinstance(roles, list) or "openbao" not in roles:
            findings.append("OpenBao amd64 structural scope is no longer explicit")
    if pre_plays and isinstance(pre_plays[0], dict):
        play = pre_plays[0]
        if play.get("roles"):
            findings.append("Arm64 preflight must remain structural and role-free")
        tasks = play.get("tasks")
        if not isinstance(tasks, list) or len(tasks) != 1:
            findings.append("Arm64 preflight may contain only its read-only report task")
        elif not isinstance(tasks[0], dict) or "ansible.builtin.debug" not in tasks[0]:
            findings.append("Arm64 preflight's post-guard task must be read-only debug")
    for path, plays in ((PREFLIGHT, pre_plays), (FULL_PLAY, full_plays)):
        if plays and isinstance(plays[0], dict):
            play = plays[0]
            pre_tasks = play.get("pre_tasks")
            if isinstance(pre_tasks, list):
                refusal = next(
                    (
                        task
                        for task in pre_tasks
                        if isinstance(task, dict) and task.get("name") == REQUIRED[path]
                    ),
                    None,
                )
                if refusal is not None:
                    before = pre_tasks[: pre_tasks.index(refusal)]
                    if any(
                        isinstance(task, dict)
                        and any(module in task for module in MUTATING_MODULES)
                        for task in before
                    ):
                        findings.append(f"{path.name}: mutation occurs before refusal task")
    return findings


def audit() -> list[str]:
    """Audit the tracked playbooks without invoking Ansible or a host."""
    pre_plays, pre_errors = _plays(PREFLIGHT)
    full_plays, full_errors = _plays(FULL_PLAY)
    return [*pre_errors, *full_errors, *audit_documents(pre_plays, full_plays)]


def main() -> int:
    """Run static policy checks and the requested local selftest modes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--ansible-selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest or args.ansible_selftest:
        runner = importlib.import_module("k3s_arm64_preflight_selftest").run
        return runner(static=args.selftest, ansible=args.ansible_selftest)
    findings = audit()
    if findings:
        print("K3s Arm64 preflight audit: FAIL")
        for finding in findings:
            print(f"- {finding}")
        return 1
    print("K3s Arm64 preflight audit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
