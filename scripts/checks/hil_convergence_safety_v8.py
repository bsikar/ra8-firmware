# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural checks for the HIL convergence front-door and transport edges."""

from __future__ import annotations

import ast
from dataclasses import dataclass

import yaml


@dataclass(frozen=True)
class FrontDoor:
    """One playbook's authenticated dynamic-role contract."""

    label: str
    guard_role: str
    guard_file: str
    fact: str
    roles: tuple[str, ...]


def _function(tree: ast.Module, name: str) -> ast.FunctionDef | None:
    """Return one unique top-level function."""
    found = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == name]
    return found[0] if len(found) == 1 else None


def _same(node: ast.AST, source: str) -> bool:
    """Compare executable syntax while ignoring locations and comments."""
    expected = ast.parse(source).body[0]
    return ast.dump(node, include_attributes=False) == ast.dump(expected, include_attributes=False)


def _index(function: ast.FunctionDef | None, source: str) -> int:
    """Find one unique exact top-level statement."""
    if function is None:
        return -1
    found = [index for index, node in enumerate(function.body) if _same(node, source)]
    return found[0] if len(found) == 1 else -1


def _assigned(function: ast.FunctionDef | None, name: str) -> ast.expr | None:
    """Return one unique top-level assignment value."""
    if function is None:
        return None
    found = [
        node.value
        for node in function.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id == name
    ]
    return found[0] if len(found) == 1 else None


def _exact_call_argv(
    function: ast.FunctionDef | None,
    module: str,
    name: str,
    expected_source: str,
) -> bool:
    """Return whether one module call has the exact first positional argv."""
    if function is None:
        return False
    found = [
        node.args[0]
        for node in ast.walk(function)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and isinstance(node.func.value, ast.Name)
        and node.func.value.id == module
        and node.func.attr == name
        and node.args
    ]
    expected = ast.parse(expected_source, mode="eval").body
    return len(found) == 1 and ast.dump(found[0], include_attributes=False) == ast.dump(
        expected, include_attributes=False
    )


def _dispatch_order_errors(fleet: ast.Module) -> list[str]:
    """Require selector refusal before the lock wrapper and inventory preview."""
    errors: list[str] = []
    converge = _function(fleet, "cmd_converge")
    order = (
        "refusal = _converge_refusal(args, host, plays)",
        "if refusal:\n    return _fail(refusal)",
        "guard = _bench_guard_argv(host, plays, args)",
        "if guard:\n    return _run(guard, cwd=fm.REPO_ROOT)",
        "rc = cmd_inventory(data, argparse.Namespace(stdout=False))",
    )
    indices = [_index(converge, statement) for statement in order]
    if -1 in indices or indices != sorted(indices):
        errors.append("fleet.py: selector/extra-var refusal is not before lock and inventory")
    refusal = _function(fleet, "_converge_refusal")
    boundary = (
        'boundary = fb.control_flow_refusal(fb.FlowRequest(str(host["class"]), plays, '
        'args.mode, args.tags, args.extra_var, bool(getattr(args, "trusted_tags", False))))'
    )
    if _index(refusal, boundary) < 0 or _index(refusal, "if boundary:\n    return boundary") < 0:
        errors.append("fleet.py: bench control-flow refusal is not executable and exact")
    return errors


def _dispatch_errors(fleet: ast.Module, bench: ast.Module) -> list[str]:
    """Require authentication before wrapper, inventory, or preview."""
    errors = _dispatch_order_errors(fleet)
    guard = _function(bench, "guarded_argv")
    auth = (
        "if lock_id:\n"
        "    capability = _capability(request.environment)\n"
        "    if not authenticate(request.repo_root, capability):\n"
        "        raise BenchGuardError(INHERITED_LOCK_ERROR)\n"
        "    return []"
    )
    if _index(guard, "lock_id = _lock_id(request.environment)") < 0 or _index(guard, auth) < 0:
        errors.append("fleet_bench.py: inherited lock is trusted without live authentication")
    wrapper = (
        'return ["/bin/bash", "-p", '
        'str(request.repo_root / "scripts/hil/bench.sh"), '
        '"run", "--intent", "Ansible bench-affecting converge", '
        '"--for", "2h", "--wait", "2h", "--", '
        "str(request.fleet_script), *request.original_argv]"
    )
    if _index(guard, wrapper) < 0:
        errors.append("fleet_bench.py: bench wrapper is not the exact privileged Bash transport")
    live_auth = _function(bench, "_live_lock_matches")
    live_auth_argv = (
        '["/bin/bash", "--noprofile", "--norc", "-p", "-c", script, '
        '"ra8-lock", str(client), capability.lock_id, digest, broker_digest]'
    )
    if not _exact_call_argv(live_auth, "subprocess", "run", live_auth_argv):
        errors.append(
            "fleet_bench.py: live-lock verifier is not the exact privileged Bash transport"
        )
    bash_probe = _function(bench, "_privileged_bash_selftest")
    probe_argv = '["/bin/bash", "--noprofile", "--norc", "-p", "-c", probe]'
    if not _exact_call_argv(bash_probe, "subprocess", "run", probe_argv):
        errors.append("fleet_bench.py: privileged Bash execution probe argv is not exact")
    runtime = _function(bench, "run_selftest")
    if _index(runtime, "failures = _privileged_bash_selftest()") < 0:
        errors.append("fleet_bench.py: privileged Bash execution probe is not load-bearing")
    controls = _function(bench, "control_flow_refusal")
    required = (
        "if request.tags and not request.trusted_tags:\n"
        '    return "bench-affecting applies do not accept --tags"',
        "if request.extra_vars:\n"
        '    return "bench-affecting applies do not accept raw --extra-var"',
    )
    if any(_index(controls, statement) < 0 for statement in required):
        errors.append("fleet_bench.py: arbitrary tags or extra vars remain accepted")
    return errors


def _native_environment_errors(runner: ast.Module, reach: ast.Module) -> list[str]:
    """Require minimal child environment, absolute SSH, and link census."""
    errors: list[str] = []
    environment = _function(runner, "ansible_environment")
    expected_clean = ast.parse(
        '{"HOME": pwd.getpwuid(os.getuid()).pw_dir, "LANG": "C.UTF-8", '
        '"LC_ALL": "C.UTF-8", "PATH": "/usr/bin:/bin"}',
        mode="eval",
    ).body
    clean = _assigned(environment, "clean")
    if clean is None or ast.dump(clean, include_attributes=False) != ast.dump(
        expected_clean, include_attributes=False
    ):
        errors.append("fleet runner: native Ansible child environment is not minimal")
    link_call = "link_errors = fpa.confined_link_errors(collections)"
    if _index(environment, link_call) < 0:
        errors.append("fleet runner: collection link census is not before callback use")
    ssh_target = _function(reach, "ssh_target")
    expected_ssh = ast.parse('["/usr/bin/ssh", *SSH_OPTIONS]', mode="eval").body
    ssh_argv = _assigned(ssh_target, "argv")
    if ssh_argv is None or ast.dump(ssh_argv, include_attributes=False) != ast.dump(
        expected_ssh, include_attributes=False
    ):
        errors.append("fleet reach: SSH executable is not an absolute trusted authority")
    return errors


def _wsl_boundary_errors(model: ast.Module, stage: ast.Module) -> list[str]:
    """Require env-empty WSL Bash and absolute local archive creation."""
    errors: list[str] = []
    remote = _function(model, "remote_shell")
    strings = set(_return_strings(remote))
    required = {
        "wsl -d ",
        " -u root -e /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin /bin/bash -s",
        "/bin/bash -s",
    }
    if not required <= strings:
        errors.append("fleet model: actual WSL boundary is not env-empty before Bash")
    archive = _function(stage, "_stage_archive")
    tar_tool = _assigned(archive, "tar_tool")
    expected_tar = ast.parse('Path("/usr/bin/tar")', mode="eval").body
    if tar_tool is None or ast.dump(tar_tool, include_attributes=False) != ast.dump(
        expected_tar, include_attributes=False
    ):
        errors.append("fleet WSL stage: archive tool is not fixed /usr/bin/tar")
    return errors


def _return_strings(function: ast.FunctionDef | None) -> list[str]:
    """Return list-literal strings which are executed by rendered shell."""
    if function is None:
        return []
    return [
        node.value
        for node in ast.walk(function)
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    ]


def _return_lines(function: ast.FunctionDef | None) -> set[str]:
    """Return exact nonempty shell lines from executable return literals."""
    return {
        line.strip()
        for value in _return_strings(function)
        for line in value.splitlines()
        if line.strip()
    }


def _durability_errors(stage: ast.Module, wsl: ast.Module) -> list[str]:
    """Require file/parent fsyncs on every stage/cache namespace publication."""
    owner = _return_lines(_function(stage, "_owned_shell"))
    required_owner = {
        "sync_file() {",
        "sync_dir() {",
        'sync_dir "$parent"',
    }
    if not required_owner <= set(owner):
        return ["fleet WSL stage: owned removal is not parent-fsynced"]
    required = {
        "stage_prepare_script": {
            'sync_dir "$(dirname -- "$stage")"': 1,
            'sync_dir "$incoming"': 1,
            'sync_dir "$(dirname -- "$incoming")"': 1,
        },
        "stage_publish_script": {'sync_dir "$(dirname -- "$stage")"': 3},
        "cache_prepare_script": {'sync_dir "$cache_root"': 2},
        "cache_cleanup_script": {'sync_dir "$cache_root"': 1},
        "cache_publish_script": {'sync_file "$part"': 1, 'sync_dir "$cache_root"': 1},
    }
    errors: list[str] = []
    for name, wanted in required.items():
        lines = [
            line.strip()
            for value in _return_strings(_function(stage, name))
            for line in value.splitlines()
            if line.strip()
        ]
        if any(lines.count(line) != count for line, count in wanted.items()):
            errors.append(f"fleet WSL stage: {name} lost crash-durable publication")
    verify_lines = _return_lines(_function(wsl, "_toolchain_verify_lines"))
    marker_lines = {
        'mv -f -- "$marker" "$managed_root/.ra8-infra-lock.sha256"',
        'sync_file "$managed_root/.ra8-infra-lock.sha256"',
        'sync_dir "$managed_root"',
    }
    if not marker_lines <= verify_lines:
        errors.append("fleet WSL: managed lock marker is not crash durable")
    return errors


def _one_play(source: str, label: str) -> tuple[dict[str, object] | None, list[str]]:
    """Parse one exactly shaped playbook."""
    try:
        document = yaml.safe_load(source)
    except yaml.YAMLError:
        return None, [f"{label}: malformed YAML"]
    if not isinstance(document, list) or len(document) != 1 or not isinstance(document[0], dict):
        return None, [f"{label}: expected one play"]
    return document[0], []


def _playbook_frontdoor_errors(source: str, contract: FrontDoor) -> list[str]:
    """Require dynamic role entry beneath one always-tagged guard."""
    play, errors = _one_play(source, contract.label)
    if play is None:
        return errors
    if play.get("roles"):
        errors.append(f"{contract.label}: static roles can bypass the authenticated front door")
    pre = play.get("pre_tasks")
    tasks = play.get("tasks")
    if not isinstance(pre, list) or len(pre) != 1 or not isinstance(tasks, list):
        errors.append(f"{contract.label}: guard/task front door is incomplete")
        return errors
    include = pre[0].get("ansible.builtin.include_role") if isinstance(pre[0], dict) else None
    expected_guard = {
        "name": contract.guard_role,
        "tasks_from": contract.guard_file,
        "apply": {"tags": ["always"]},
    }
    if include != expected_guard or pre[0].get("tags") != ["always"]:
        errors.append(f"{contract.label}: guard include is not exact and always-selected")
    seen: list[str] = []
    condition = f"{contract.fact} | default(false) | bool"
    for task in tasks:
        role = task.get("ansible.builtin.include_role") if isinstance(task, dict) else None
        if (
            not isinstance(role, dict)
            or set(role) != {"name"}
            or task.get("when") != condition
            or task.get("tags") != ["always"]
        ):
            errors.append(
                f"{contract.label}: a role is outside the authenticated dynamic front door"
            )
            break
        seen.append(str(role["name"]))
    if not errors and tuple(seen) != contract.roles:
        errors.append(f"{contract.label}: guarded role closure drifted")
    return errors


def errors(inputs: dict[str, str]) -> list[str]:
    """Return all v8 boundary findings, failing closed on syntax errors."""
    try:
        trees = {
            key: ast.parse(inputs[key])
            for key in (
                "fleet",
                "fleet_bench",
                "fleet_runner",
                "fleet_wsl_stage",
                "fleet_wsl",
                "fleet_model",
                "fleet_runner_model",
                "fleet_reach",
            )
        }
    except SyntaxError:
        return ["HIL convergence v8: invalid governed Python"]
    return (
        _dispatch_errors(trees["fleet"], trees["fleet_bench"])
        + _native_environment_errors(trees["fleet_runner"], trees["fleet_reach"])
        + _wsl_boundary_errors(trees["fleet_runner_model"], trees["fleet_wsl_stage"])
        + _durability_errors(trees["fleet_wsl_stage"], trees["fleet_wsl"])
        + _playbook_frontdoor_errors(
            inputs["dev_playbook"],
            FrontDoor(
                "dev-box.yml",
                "dev_box",
                "hil_mutation_guard.yml",
                "dev_box_hil_mutation_authenticated",
                ("dev_box",),
            ),
        )
        + _playbook_frontdoor_errors(
            inputs["bench_playbook"],
            FrontDoor(
                "hil-bench.yml",
                "hil_bench",
                "transaction_guard.yml",
                "hil_bench_transaction_authenticated",
                ("hil_bench", "c6_toolchain", "ad2_tools"),
            ),
        )
    )
