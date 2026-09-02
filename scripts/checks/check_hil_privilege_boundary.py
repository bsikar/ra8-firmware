#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the root-owned HIL privilege boundary from structural facts."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import cast

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
HELPER_REL = "infra/ansible/roles/dev_box/files/ra8-hil-privileged.py"
POLICY_TEMPLATE_REL = "infra/ansible/roles/dev_box/templates/ra8-hil-privileged-policy.json.j2"
ROLE_ENTRY_REL = "infra/ansible/roles/dev_box/tasks/hil_runner.yml"
ROLE_REL = "infra/ansible/roles/dev_box/tasks/hil_runner_transaction.yml"
MANIFEST_REL = "scripts/hil/lib/ra8-hil-privileged.sha256"
CALLER_PATHS = (
    "scripts/hil/ppps.sh",
    "scripts/hil/flash_retry.sh",
    "scripts/hil/exit_low_power.sh",
    "scripts/hil/eth_tcp.sh",
)
WORKFLOW_PATHS = (
    HELPER_REL,
    POLICY_TEMPLATE_REL,
    ROLE_ENTRY_REL,
    "infra/fleet.yml",
    "scripts/dev/fleet_hil.py",
    "infra/ansible/roles/hil_bench/tasks/main.yml",
)
SPAWN_ARGC = 2
EXACT_POLICY_TEMPLATE = """{
  "board_iface": {{ dev_box_hil_runner_bench_iface | to_json }},
  "declaration_sha256": {{ dev_box_hil_runner_bench_policy_sha256 | to_json }},
  "mac": {{ dev_box_hil_runner_bench_mac | to_json }},
  "phc_index": {{ dev_box_hil_runner_bench_phc_index | int }},
  "sysfs_device": {{ dev_box_hil_runner_bench_sysfs_device | to_json }},
  "version": 1
}
"""
LOAD_POLICY_PREFIX = (
    "def _load_policy(path: Path = POLICY_PATH) -> dict[str, object]:\n"
    '    """Open the fixed root-owned policy without following a final symlink."""\n'
    "    try:\n"
)
LOAD_POLICY_NOFOLLOW = (
    LOAD_POLICY_PREFIX
    + "        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)"
)
LOAD_POLICY_DEAD_NOFOLLOW = (
    LOAD_POLICY_PREFIX
    + "        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC)  # dead os.O_NOFOLLOW"
)


def _copy_tasks(source: str) -> tuple[list[dict[str, object]], list[str]]:
    """Parse an Ansible task list or return one attributed error."""
    try:
        tasks = yaml.safe_load(source)
    except yaml.YAMLError:
        return [], ["hil_runner.yml: YAML is malformed"]
    if not isinstance(tasks, list) or any(not isinstance(task, dict) for task in tasks):
        return [], ["hil_runner.yml: role task file is not a task list"]
    return cast(list[dict[str, object]], tasks), []


def _module_tasks(tasks: list[dict[str, object]], module: str) -> list[dict[str, object]]:
    """Return exact mappings for one Ansible module."""
    return [
        cast(dict[str, object], task[module])
        for task in tasks
        if isinstance(task.get(module), dict)
    ]


def _exact_task(
    tasks: list[dict[str, object]],
    module: str,
    destination: str,
    expected: dict[str, object],
) -> bool:
    """Return whether exactly one destination task has every required field."""
    matches = [task for task in _module_tasks(tasks, module) if task.get("dest") == destination]
    return len(matches) == 1 and all(
        matches[0].get(key) == value for key, value in expected.items()
    )


def _role_errors(source: str, template: str) -> list[str]:
    """Return structural Ansible install/sudo/policy errors."""
    tasks, errors = _copy_tasks(source)
    if errors:
        return errors
    helper = {
        "src": "ra8-hil-privileged.py",
        "dest": "/usr/local/libexec/ra8-hil-privileged",
        "owner": "root",
        "group": "root",
        "mode": "0755",
    }
    if not _exact_task(tasks, "ansible.builtin.copy", str(helper["dest"]), helper):
        errors.append("hil_runner.yml: root helper copy boundary is not exact")
    policy = {
        "src": "ra8-hil-privileged-policy.json.j2",
        "dest": "/etc/ra8-hil-privileged-policy.json",
        "owner": "root",
        "group": "root",
        "mode": "0644",
        "validate": "/usr/bin/python3 -m json.tool %s",
    }
    if not _exact_task(tasks, "ansible.builtin.template", str(policy["dest"]), policy):
        errors.append("hil_runner.yml: root policy template boundary is not exact")
    errors.extend(_sudoers_errors(tasks))
    if template != EXACT_POLICY_TEMPLATE:
        errors.append("HIL helper policy template is not the exact fleet-derived document")
    return errors


def _sudoers_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require one exact sudo executable and no wildcarded privileged tool."""
    copies = _module_tasks(tasks, "ansible.builtin.copy")
    matches = [copy for copy in copies if copy.get("dest") == "/etc/sudoers.d/ra8-hil"]
    if len(matches) != 1:
        return ["hil_runner.yml: exact HIL sudoers file is missing or duplicated"]
    copy = matches[0]
    expected = {
        "owner": "root",
        "group": "root",
        "mode": "0440",
        "validate": "/usr/sbin/visudo -cf %s",
    }
    errors = []
    if any(copy.get(key) != value for key, value in expected.items()):
        errors.append("hil_runner.yml: sudoers ownership/mode/validation is not exact")
    content = copy.get("content")
    line = (
        "{{ dev_box_hil_runner_bench_user }} ALL=(root) NOPASSWD: "
        "/usr/local/libexec/ra8-hil-privileged"
    )
    lines = (
        [item.strip() for item in content.splitlines() if item.strip()]
        if isinstance(content, str)
        else []
    )
    if lines != [line]:
        errors.append("hil_runner.yml: sudoers must grant only the fixed helper executable")
    return errors


def _call_name(call: ast.Call) -> str:
    """Return one static call target, including a one-level receiver."""
    if isinstance(call.func, ast.Name):
        return call.func.id
    if isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name):
        return f"{call.func.value.id}.{call.func.attr}"
    return ""


def _calls(node: ast.AST) -> set[str]:
    """Return every statically named call below an AST node."""
    return {_call_name(item) for item in ast.walk(node) if isinstance(item, ast.Call)}


def _definitions(tree: ast.Module) -> tuple[dict[str, ast.FunctionDef], list[str]]:
    """Return unique synchronous top-level definitions and definition errors."""
    sync: dict[str, ast.FunctionDef] = {}
    errors = []
    names: list[str] = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names.append(node.name)
            if isinstance(node, ast.AsyncFunctionDef):
                errors.append(f"privileged helper: {node.name} must be synchronous")
            else:
                sync[node.name] = node
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        errors.append(f"privileged helper: duplicate definitions: {', '.join(duplicates)}")
    return sync, errors


def _argv_zero(node: ast.AST) -> bool:
    """Return whether a node is exactly argv[0]."""
    return (
        isinstance(node, ast.Subscript)
        and isinstance(node.value, ast.Name)
        and node.value.id == "argv"
        and isinstance(node.slice, ast.Constant)
        and node.slice.value == 0
    )


def _allow_guard(node: ast.AST) -> bool:
    """Return whether a test is exactly argv[0] not in allowed."""
    if isinstance(node, ast.BoolOp) and isinstance(node.op, ast.Or):
        return any(_allow_guard(value) for value in node.values)
    return (
        isinstance(node, ast.Compare)
        and _argv_zero(node.left)
        and len(node.ops) == 1
        and isinstance(node.ops[0], ast.NotIn)
        and len(node.comparators) == 1
        and isinstance(node.comparators[0], ast.Name)
        and node.comparators[0].id == "allowed"
    )


def _direct_fail(node: ast.If) -> bool:
    """Return whether the guard directly and unconditionally rejects."""
    if len(node.body) != 1 or node.orelse:
        return False
    statement = node.body[0]
    return (
        isinstance(statement, ast.Expr)
        and isinstance(statement.value, ast.Call)
        and _call_name(statement.value) == "_fail"
    )


def _run_boundary_errors(function: ast.FunctionDef) -> list[str]:
    """Prove the executable allowlist dominates the one spawn call."""
    assignments = [
        node
        for node in function.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "allowed" for target in node.targets)
    ]
    exact_sets = [
        {item.value for item in node.value.elts}
        for node in assignments
        if isinstance(node.value, ast.Set)
    ]
    guards = [
        node for node in function.body if isinstance(node, ast.If) and _allow_guard(node.test)
    ]
    spawns = [
        node
        for node in ast.walk(function)
        if isinstance(node, ast.Call) and _call_name(node) == "os.posix_spawn"
    ]
    errors = []
    if exact_sets != [{"/usr/sbin/ip", "/usr/sbin/uhubctl"}]:
        errors.append("privileged helper: executable allowlist assignment is not exact")
    if len(guards) != 1 or not _direct_fail(guards[0]):
        errors.append("privileged helper: executable rejection guard is not active")
    if len(spawns) != 1 or len(spawns[0].args) < SPAWN_ARGC or not _argv_zero(spawns[0].args[0]):
        errors.append("privileged helper: spawn executable is not guarded argv[0]")
    elif not isinstance(spawns[0].args[1], ast.Name) or spawns[0].args[1].id != "argv":
        errors.append("privileged helper: spawn does not receive the validated argv")
    if guards and spawns and cast(int, guards[0].end_lineno) >= spawns[0].lineno:
        errors.append("privileged helper: executable guard does not dominate spawn")
    return errors


def _call_requirements() -> dict[str, set[str]]:
    """Return the live call graph required at each privileged boundary."""
    return {
        "_mutate": {
            "fcntl.flock",
            "_recover_restore",
            "_perform_cycle",
            "_network_prepare",
            "_network_cleanup",
        },
        "_perform_cycle": {
            "ops.save",
            "ops.apply",
            "ops.pause",
            "ops.clear",
            "_restore_action",
        },
        "_recover_restore": {
            "_load_restore",
            "ops.apply",
            "ops.clear",
            "_restore_action",
        },
        "_network_prepare": {
            "_validate_live_iface",
            "_save_state",
            "_run",
            "_address_present",
            "_link_is_up",
            "_route_present",
        },
        "_network_cleanup": {
            "_load_state",
            "_authenticate_cleanup_iface",
            "_cleanup_route",
            "_cleanup_link",
            "_cleanup_address",
        },
        "_network_neigh_flush": {
            "_load_state",
            "_authenticate_cleanup_iface",
            "_run",
        },
        "_cleanup_route": {"_run", "_route_present", "_checkpoint_absent"},
        "_cleanup_link": {"_run", "_link_is_up", "_checkpoint_absent"},
        "_cleanup_address": {"_run", "_address_present", "_checkpoint_absent"},
        "_validate_live_iface": {"_authenticate_live_iface"},
        "_authenticate_cleanup_iface": {"_authenticate_live_iface"},
        "_authenticate_live_iface": {
            "_live_physical_identity",
            "_iface_facts",
            "_validate_iface_facts",
        },
        "_usb_authorize": {"_resolve_usb_device", "os.open", "os.fstat"},
    }


def _required_calls(functions: dict[str, ast.FunctionDef]) -> list[str]:
    """Prove privileged transactions remain reachable and checkpointed."""
    errors = []
    for name, expected in _call_requirements().items():
        missing = expected - _calls(functions[name])
        if missing:
            errors.append(f"privileged helper: {name} omits live calls {sorted(missing)}")
    errors.extend(_live_power_dispatch_errors(functions["_mutate"]))
    return errors


def _live_power_dispatch_errors(function: ast.FunctionDef) -> list[str]:
    """Prove the fixed argv builder is nested in the live execution call."""
    live_power = any(
        isinstance(call, ast.Call)
        and _call_name(call) == "_run"
        and any(
            isinstance(argument, ast.Call) and _call_name(argument) == "_usb_power_command"
            for argument in call.args
        )
        for call in ast.walk(function)
    )
    if not live_power:
        return ["privileged helper: persistent USB power dispatch is a no-op"]
    return []


def _live_cycle_errors(tree: ast.Module) -> list[str]:
    """Prove the production cycle backend reaches fixed live boundaries."""
    methods = [
        node
        for parent in tree.body
        if isinstance(parent, ast.ClassDef) and parent.name == "_LiveCycleOps"
        for node in parent.body
        if isinstance(node, ast.FunctionDef) and node.name == "apply"
    ]
    required = {"_usb_power_command", "_run", "_usb_authorize"}
    if len(methods) != 1 or not required.issubset(_calls(methods[0])):
        return ["privileged helper: live cycle backend does not reach fixed boundaries"]
    return []


def _has_nofollow_open(function: ast.FunctionDef) -> bool:
    """Return whether one real os.open call receives O_NOFOLLOW."""
    nofollow_names = {
        target.id
        for assignment in function.body
        if isinstance(assignment, ast.Assign)
        for target in assignment.targets
        if isinstance(target, ast.Name)
        and any(
            isinstance(item, ast.Attribute)
            and isinstance(item.value, ast.Name)
            and item.value.id == "os"
            and item.attr == "O_NOFOLLOW"
            for item in ast.walk(assignment.value)
        )
    }
    for call in ast.walk(function):
        if not isinstance(call, ast.Call) or _call_name(call) != "os.open":
            continue
        attributes = {
            item.attr
            for arg in call.args
            for item in ast.walk(arg)
            if isinstance(item, ast.Attribute)
            and isinstance(item.value, ast.Name)
            and item.value.id == "os"
        }
        named_flags = {arg.id for arg in call.args if isinstance(arg, ast.Name)}
        if "O_NOFOLLOW" in attributes or nofollow_names & named_flags:
            return True
    return False


def _fixed_power_argv(function: ast.FunctionDef, kind: str) -> tuple[str, ...] | None:
    """Return the direct argv for one live kind-guarded builder branch."""
    matches: list[ast.If] = []
    for statement in function.body:
        if not isinstance(statement, ast.If):
            continue
        constants = {
            node.value
            for node in ast.walk(statement.test)
            if isinstance(node, ast.Constant) and isinstance(node.value, str)
        }
        if kind in constants:
            matches.append(statement)
    if len(matches) != 1:
        return None
    returns = [statement for statement in matches[0].body if isinstance(statement, ast.Return)]
    if len(returns) != 1 or not isinstance(returns[0].value, ast.List):
        return None
    values = []
    for element in returns[0].value.elts:
        if isinstance(element, ast.Constant) and isinstance(element.value, str):
            values.append(element.value)
        elif isinstance(element, ast.Name) and element.id in {"port", "action"}:
            values.append(f"${element.id}")
        else:
            return None
    return tuple(values)


def _topology_errors(functions: dict[str, ast.FunctionDef]) -> list[str]:
    """Prove topology/policy operands occur in live enforcing functions."""
    errors = []
    expected_power = {
        "usb-port-power": (
            "/usr/sbin/uhubctl",
            "-S",
            "-l",
            "2-1.3",
            "-p",
            "$port",
            "-a",
            "$action",
        ),
        "usb-root-power": (
            "/usr/sbin/uhubctl",
            "-S",
            "-l",
            "2-1",
            "-a",
            "$action",
        ),
    }
    actual_power = {
        kind: _fixed_power_argv(functions["_usb_power_command"], kind) for kind in expected_power
    }
    if actual_power != expected_power:
        errors.append("privileged helper: live USB argv builder lost fixed topology")
    iface_strings = {
        node.value
        for node in ast.walk(functions["_iface_facts"])
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    }
    if not {"-4", "-6", "table", "all", "default"}.issubset(iface_strings):
        errors.append("privileged helper: all-table IPv4/IPv6 uplink census is incomplete")
    nofollow = (
        "_load_policy",
        "_load_state",
        "_load_restore",
        "_save_state",
        "_save_restore",
        "_usb_authorize",
    )
    missing = [name for name in nofollow if not _has_nofollow_open(functions[name])]
    if missing:
        errors.append(f"privileged helper: live no-follow open missing in {missing}")
    if not {"_canonical_policy", "secrets.compare_digest"}.issubset(
        _calls(functions["_strict_policy"])
    ):
        errors.append("privileged helper: installed policy is not bound to its declaration digest")
    return errors


def _helper_errors(source: str) -> list[str]:
    """Return definition, execution, call-graph, and topology errors."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return ["privileged helper: Python syntax is malformed"]
    functions, errors = _definitions(tree)
    required = {
        "_run",
        "_mutate",
        "_perform_cycle",
        "_recover_restore",
        "_usb_power_command",
        "_usb_authorize",
        "_resolve_usb_device",
        "_strict_policy",
        "_load_policy",
        "_validate_iface_facts",
        "_iface_facts",
        "_authenticate_live_iface",
        "_validate_live_iface",
        "_authenticate_cleanup_iface",
        "_strict_state",
        "_save_state",
        "_load_state",
        "_network_prepare",
        "_network_cleanup",
        "_network_neigh_flush",
        "_cleanup_route",
        "_cleanup_link",
        "_cleanup_address",
        "_save_restore",
        "_load_restore",
    }
    missing = sorted(required - functions.keys())
    if missing:
        message = f"privileged helper: synchronous functions missing: {', '.join(missing)}"
        return [*errors, message]
    forbidden = {"eval", "exec", "os.system", "subprocess.run", "subprocess.Popen"}
    bad = sorted(forbidden & _calls(tree))
    if bad:
        errors.append(f"privileged helper: forbidden calls: {', '.join(bad)}")
    return (
        errors
        + _run_boundary_errors(functions["_run"])
        + _required_calls(functions)
        + _topology_errors(functions)
        + _live_cycle_errors(tree)
    )


def _active_shell(source: str) -> str:
    """Return executable-looking shell lines with comments removed."""
    return "\n".join(
        line.strip()
        for line in source.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def _caller_errors(callers: dict[str, str]) -> list[str]:
    """Require live identity invocations, transactions, and no sudo bypass."""
    active = {path: _active_shell(source) for path, source in callers.items()}
    required = {
        "scripts/hil/ppps.sh": (
            r'^ra8_hil_privileged_verify_remote "\$PI_HOST" \|\| exit \$\?$',
            r"usb-port-cycle",
            r"usb-authorize-cycle",
        ),
        "scripts/hil/flash_retry.sh": (
            r'^ra8_hil_privileged_verify_remote "\$PI_HOST" \|\| exit \$\?$',
            r"usb-root-cycle",
        ),
        "scripts/hil/exit_low_power.sh": (
            r"^ra8_hil_privileged_verify_local \|\| exit \$\?$",
            r"usb-root-cycle",
        ),
        "scripts/hil/eth_tcp.sh": (
            r"actual_identity=.*--identity",
            r"net-prepare \"\$BOARD_IP\"",
            r"--policy-interface",
        ),
    }
    errors = []
    for path, patterns in required.items():
        errors.extend(
            f"{path}: active exact helper/identity invocation missing"
            for pattern in patterns
            if re.search(pattern, active[path], re.MULTILINE) is None
        )
    bypass = re.compile(
        r"sudo\s+-n(?:\s+--)?\s+(?:/usr/sbin/)?(?:ip|uhubctl|tcpdump|timeout)\b"
        r"|sudo\s+-n(?:\s+--)?\s+(?:/usr/bin/)?tee\b"
    )
    for path, source in active.items():
        if bypass.search(source):
            errors.append(f"{path}: direct privileged executable bypasses the fixed helper")
    if "usb-root-power off" in active["scripts/hil/flash_retry.sh"]:
        errors.append("flash retry must use one transactional root-cycle operation")
    if "usb-root-power off" in active["scripts/hil/exit_low_power.sh"]:
        errors.append("low-power recovery must use one transactional root-cycle operation")
    return errors


def _fleet_policy(fleet_source: str) -> tuple[dict[str, object] | None, list[str]]:
    """Read the sole fleet-owned board-interface declaration."""
    try:
        fleet = yaml.safe_load(fleet_source)
        interface = fleet["hosts"]["star"]["board_interface"]
    except (yaml.YAMLError, KeyError, TypeError):
        return None, ["infra/fleet.yml: star board_interface declaration is missing"]
    keys = {"name", "mac", "sysfs_device", "phc_index"}
    if not isinstance(interface, dict) or set(interface) != keys:
        return None, ["infra/fleet.yml: star board_interface schema is not exact"]
    policy = {
        "board_iface": interface["name"],
        "mac": interface["mac"],
        "phc_index": interface["phc_index"],
        "sysfs_device": interface["sysfs_device"],
        "version": 1,
    }
    return policy, []


def _identity_errors(helper: bytes, manifest: str, fleet: str) -> list[str]:
    """Bind caller identity independently to helper bytes and fleet policy."""
    policy, errors = _fleet_policy(fleet)
    if errors or policy is None:
        return errors
    payload = (json.dumps(policy, sort_keys=True, separators=(",", ":")) + "\n").encode()
    expected = f"{hashlib.sha256(helper).hexdigest()}:{hashlib.sha256(payload).hexdigest()}"
    if re.fullmatch(r"[0-9a-f]{64}:[0-9a-f]{64}\n?", manifest) is None:
        return ["privileged helper identity manifest is malformed"]
    return [] if manifest.strip() == expected else ["privileged helper or policy identity is stale"]


def _strings(value: object) -> list[str]:
    """Flatten scalar strings in parsed YAML."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [item for child in value for item in _strings(child)]
    if isinstance(value, dict):
        return [item for child in value.values() for item in _strings(child)]
    return []


def _workflow_errors(source: str) -> list[str]:
    """Require trigger coverage while forbidding provisioning mutation."""
    try:
        document = yaml.safe_load(source)
    except yaml.YAMLError:
        return ["hil.yml: workflow YAML is malformed"]
    triggers = document.get("on", document.get(True)) if isinstance(document, dict) else None
    if not isinstance(document, dict) or not isinstance(triggers, dict):
        return ["hil.yml: workflow triggers are malformed"]
    errors = []
    for event in ("push", "pull_request"):
        config = triggers.get(event)
        paths = config.get("paths") if isinstance(config, dict) else None
        if not isinstance(paths, list) or any(path not in paths for path in WORKFLOW_PATHS):
            errors.append(f"hil.yml: trusted policy paths missing from {event}")
    forbidden = ("ansible-playbook", "just infra::apply", "infra::apply")
    values = _strings(document.get("jobs", {}))
    errors.extend(
        f"hil.yml: workflow must not auto-apply trusted provisioning: {token}"
        for token in forbidden
        if any(token in value for value in values)
    )
    return errors


def _scan(inputs: dict[str, object]) -> list[str]:
    """Return every privilege-boundary structural error."""
    return (
        _role_errors(cast(str, inputs["role"]), cast(str, inputs["template"]))
        + _helper_errors(cast(str, inputs["helper"]))
        + _identity_errors(
            cast(bytes, inputs["helper_bytes"]),
            cast(str, inputs["manifest"]),
            cast(str, inputs["fleet"]),
        )
        + _caller_errors(cast(dict[str, str], inputs["callers"]))
        + _workflow_errors(cast(str, inputs["workflow"]))
    )


def _repo_inputs(root: Path) -> dict[str, object]:
    """Load the exact governed repository files."""
    helper = root / HELPER_REL
    return {
        "role": (root / ROLE_REL).read_text(encoding="utf-8"),
        "template": (root / POLICY_TEMPLATE_REL).read_text(encoding="utf-8"),
        "helper": helper.read_text(encoding="utf-8"),
        "helper_bytes": helper.read_bytes(),
        "manifest": (root / MANIFEST_REL).read_text(encoding="ascii"),
        "fleet": (root / "infra/fleet.yml").read_text(encoding="utf-8"),
        "callers": {path: (root / path).read_text(encoding="utf-8") for path in CALLER_PATHS},
        "workflow": (root / ".github/workflows/hil.yml").read_text(encoding="utf-8"),
    }


def _refresh_helper_identity(inputs: dict[str, object]) -> None:
    """Refresh only the helper half after an in-memory helper mutation."""
    manifest = cast(str, inputs["manifest"]).strip().split(":")
    inputs["helper_bytes"] = cast(str, inputs["helper"]).encode()
    helper_sha = hashlib.sha256(cast(bytes, inputs["helper_bytes"])).hexdigest()
    inputs["manifest"] = f"{helper_sha}:{manifest[1]}\n"


def _mutated(inputs: dict[str, object], key: str, old: str, new: str) -> dict[str, object]:
    """Return one exact single-replacement mutation."""
    result = dict(inputs)
    source = cast(str, inputs[key])
    if source.count(old) != 1:
        message = f"selftest fixture for {key} is not unique: {old!r}"
        raise RuntimeError(message)
    result[key] = source.replace(old, new)
    if key == "helper":
        _refresh_helper_identity(result)
    return result


def _helper_mutations() -> list[tuple[str, str, str, str]]:
    """Return executable helper mutations found by adversarial review."""
    return [
        (
            "async execution boundary",
            "helper",
            "def _run(argv: list[str]",
            "async def _run(argv: list[str]",
        ),
        (
            "dead allowlist guard",
            "helper",
            "if (\n        not argv",
            "if False and (\n        not argv",
        ),
        (
            "dead nested rejection",
            "helper",
            '        _fail("child command is outside the fixed executable boundary")',
            (
                "        if False:\n"
                '            _fail("child command is outside the fixed executable boundary")'
            ),
        ),
        (
            "no-op mutation dispatch",
            "helper",
            "_run(_usb_power_command(command, args))",
            "_usb_power_command(command, args)",
        ),
    ]


def _helper_policy_mutations() -> list[tuple[str, str, str, str]]:
    """Return network-authentication and topology mutations."""
    return [
        (
            "cleanup physical-auth bypass",
            "helper",
            (
                "    _authenticate_cleanup_iface(policy, state)\n"
                "    _cleanup_route(path, state, policy)"
            ),
            "    _cleanup_route(path, state, policy)",
        ),
        (
            "neighbour physical-auth bypass",
            "helper",
            (
                "    _authenticate_cleanup_iface(policy, state)\n"
                '    _run(["/usr/sbin/ip", "neigh", "flush", "dev", str(state["iface"])])'
            ),
            '    _run(["/usr/sbin/ip", "neigh", "flush", "dev", str(state["iface"])])',
        ),
        (
            "dead topology constant",
            "helper",
            '        return ["/usr/sbin/uhubctl", "-S", "-l", "2-1.3", "-p", port, "-a", action]',
            (
                '        dead_topology = "2-1.3"\n'
                '        return ["/usr/sbin/uhubctl", "-S", "-l", "9-9", '
                '"-p", port, "-a", action]'
            ),
        ),
        (
            "dead no-follow token",
            "helper",
            LOAD_POLICY_NOFOLLOW,
            LOAD_POLICY_DEAD_NOFOLLOW,
        ),
    ]


def _configuration_mutations() -> list[tuple[str, str, str, str]]:
    """Return trusted provisioning and workflow mutations."""
    return [
        ("policy template drift", "template", '  "version": 1', '  "version": 2'),
        (
            "workflow auto-apply",
            "workflow",
            "just quality::local::gate hil-all",
            "just quality::local::gate hil-all\n          ansible-playbook live.yml",
        ),
    ]


def _selftest_cases(inputs: dict[str, object]) -> list[tuple[str, bool]]:
    """Apply independent reviewer mutations that every checker must catch."""
    cases = [("complete boundary stays quiet", not _scan(inputs))]
    mutations = _helper_mutations() + _helper_policy_mutations() + _configuration_mutations()
    for label, key, old, new in mutations:
        cases.append((f"{label} fires", bool(_scan(_mutated(inputs, key, old, new)))))
    changed = dict(inputs)
    callers = dict(cast(dict[str, str], inputs["callers"]))
    line = 'ra8_hil_privileged_verify_remote "$PI_HOST" || exit $?'
    callers["scripts/hil/flash_retry.sh"] = callers["scripts/hil/flash_retry.sh"].replace(
        line, f"# {line}"
    )
    changed["callers"] = callers
    cases.append(("commented identity invocation fires", bool(_scan(changed))))
    cases.extend(_stale_identity_cases(inputs))
    return cases


def _stale_identity_cases(inputs: dict[str, object]) -> list[tuple[str, bool]]:
    """Prove helper and fleet-policy drift fail independently."""
    first, second = cast(str, inputs["manifest"]).strip().split(":")
    stale_helper = dict(inputs)
    stale_helper["manifest"] = f"{'0' * 64}:{second}\n"
    stale_policy = dict(inputs)
    stale_policy["manifest"] = f"{first}:{'0' * 64}\n"
    return [
        ("stale helper identity fires independently", bool(_scan(stale_helper))),
        ("stale policy identity fires independently", bool(_scan(stale_policy))),
    ]


def run_selftest() -> int:
    """Run quiet and must-fire mutations against the live governed shape."""
    cases = _selftest_cases(_repo_inputs(REPO_ROOT))
    for label, passed in cases:
        print(f"  [{'PASS' if passed else 'FAIL'}] {label}")
    ok = all(passed for _, passed in cases)
    print(f"check_hil_privilege_boundary.py --selftest: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def _parser() -> argparse.ArgumentParser:
    """Build the strict CLI."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the checker or its mutation selftest."""
    args = _parser().parse_args(argv)
    if args.selftest:
        return run_selftest()
    errors = _scan(_repo_inputs(REPO_ROOT))
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"check_hil_privilege_boundary.py: {len(errors)} error(s)", file=sys.stderr)
        return 1
    print("check_hil_privilege_boundary.py: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
