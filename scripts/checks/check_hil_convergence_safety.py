# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce idle-runner and whole-converge HIL exclusion boundaries."""

from __future__ import annotations

import argparse
import ast
import re
import sys
from typing import cast

import hil_convergence_check_mode as check_mode
import hil_convergence_safety_image_harness_policy as image_harness_policy
import hil_convergence_safety_image_lock_digest as image_lock_digest
import hil_convergence_safety_policy as policy
import hil_convergence_safety_python_authority as python_authority
import hil_convergence_safety_roles as roles
import hil_convergence_safety_selftest as selftest
import hil_convergence_safety_v8 as v8
import hil_convergence_safety_v9 as v9
import hil_convergence_safety_wsl as wsl
import yaml
from hil_convergence_safety_ast import (
    assignment as _assignment,
)
from hil_convergence_safety_ast import (
    function as _function,
)
from hil_convergence_safety_ast import (
    same_statement as _same_statement,
)
from hil_convergence_safety_ast import (
    statement_index as _statement_index,
)

REQUIRED_THAW_CALLS = 2
EXPECTED_APT_INDEX = 6
CONTROLLER_AUTH_ARGC = 10
REMOTE_VERIFY_ARGC = 9


class FixtureError(ValueError):
    """A structural selftest no longer has one exact mutation target."""


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


def _listener_state_errors(
    load: dict[str, object],
    activity: dict[str, object],
    refusal: dict[str, object],
) -> list[str]:
    """Require exact read-only service state evidence."""
    errors = []
    load_command = load.get("ansible.builtin.command")
    load_argv = load_command.get("argv") if isinstance(load_command, dict) else None
    activity_command = activity.get("ansible.builtin.command")
    activity_argv = activity_command.get("argv") if isinstance(activity_command, dict) else None
    if load_argv != [
        "/usr/bin/systemctl",
        "show",
        "--property=LoadState",
        "--value",
        "{{ dev_box_hil_runner_service }}",
    ]:
        errors.append("dev_box/tasks/main.yml: listener load-state proof is not exact")
    expected_activity = [
        "/usr/bin/systemctl",
        "show",
        "--property=ActiveState",
        "--value",
        "{{ dev_box_hil_runner_service }}",
    ]
    if activity_argv != expected_activity:
        errors.append("dev_box/tasks/main.yml: listener active-state proof is not exact")
    proof_keys = ("changed_when", "failed_when", "check_mode")
    if any(any(task.get(key) is not False for key in proof_keys) for task in (load, activity)):
        errors.append("dev_box/tasks/main.yml: listener state proof is not read-only")
    expected = _normalized(
        """ansible_check_mode or
        (dev_box_hil_runner_initial_load.rc == 0 and
         dev_box_hil_runner_initial_activity.rc == 0 and
         ((dev_box_hil_runner_initial_load.stdout | trim == 'not-found' and
           dev_box_hil_runner_initial_activity.stdout | trim == 'inactive') or
          (dev_box_hil_runner_initial_load.stdout | trim == 'loaded' and
           dev_box_hil_runner_initial_activity.stdout | trim in ['inactive', 'failed'])))"""
    )
    if _conditions(refusal) != (expected,):
        errors.append("dev_box/tasks/main.yml: listener state decision is not fail closed")
    return errors


def _dev_binding_errors(block: list[object]) -> list[str]:
    """Require both controller payload and canonical kernel-lock proofs."""
    expected_names = (
        "Authenticate the controller and immutable fleet dev-box payload",
        "Authenticate the canonical delegated kernel-held bench lock",
    )
    if tuple(task.get("name") for task in block if isinstance(task, dict)) != expected_names:
        return ["dev_box/tasks/main.yml: delegated live-capability sequence is not exact"]
    by_name = {task.get("name"): task for task in block if isinstance(task, dict)}
    local = by_name[expected_names[0]]
    remote = by_name[expected_names[1]]
    errors = _transaction_command_errors(local, "dev_box", "localhost")
    errors.extend(
        _kernel_command_errors(remote, "{{ dev_box_hil_runner_bench_alias }}", "delegated")
    )
    return errors


def _command_argv(task: object) -> list[object]:
    """Return one command argv or an empty list on structural drift."""
    command = task.get("ansible.builtin.command") if isinstance(task, dict) else None
    argv = command.get("argv") if isinstance(command, dict) else None
    return argv if isinstance(argv, list) else []


def _transaction_command_errors(task: dict[str, object], role: str, delegate: str) -> list[str]:
    """Require exact local transaction-auth executable and capability arguments."""
    argv = _command_argv(task)
    expected_tail = [
        "{{ hil_bench_maintenance_lock_id }}",
        "{{ hil_bench_maintenance_holder_pid | string }}",
        "{{ hil_bench_maintenance_holder_start_ticks | string }}",
        "{{ hil_bench_maintenance_holder_target }}",
    ]
    fixed = ["{{ playbook_dir }}/../../../.venv/bin/python3", "-I"]
    errors = []
    if (
        task.get("delegate_to") != delegate
        or task.get("become") is not False
        or task.get("changed_when") is not False
        or len(argv) != CONTROLLER_AUTH_ARGC
        or argv[:2] != fixed
        or argv[2] != "{{ playbook_dir }}/../../../scripts/dev/fleet_transaction_auth.py"
        or argv[3] != "{{ inventory_hostname }}"
        or argv[4] != role
        or argv[6:] != expected_tail
    ):
        errors.append(f"{role} role: controller transaction authentication command is not exact")
    payload = str(argv[5]) if len(argv) == CONTROLLER_AUTH_ARGC else ""
    keys = re.findall(r"'([a-z0-9_]+)'\s*:", payload)
    prefix = "dev_box_hil_runner_" if role == "dev_box" else "hil_bench_"
    expected_count = 24 if role == "dev_box" else 18
    if (
        len(keys) != expected_count
        or len(keys) != len(set(keys))
        or any(not key.startswith(prefix) for key in keys)
    ):
        errors.append(f"{role} role: immutable fleet payload key set is not exact")
    return errors


def _kernel_command_errors(task: dict[str, object], delegate: str | None, label: str) -> list[str]:
    """Require the by-value kernel verifier and reviewed-holder digest binding."""
    argv = _command_argv(task)
    errors = []
    if (
        task.get("delegate_to") != delegate
        or task.get("changed_when") is not False
        or argv[:4] != ["/usr/bin/python3", "-I", "-S", "-c"]
        or len(argv) != REMOTE_VERIFY_ARGC
        or argv[5:7] != ["{{ hil_bench_maintenance_lock_id }}", "wrapped"]
    ):
        errors.append(f"{label} role: canonical kernel-lock verifier command is not exact")
        return errors
    source_lookup = _normalized(argv[4])
    digest_lookup = _normalized(argv[7])
    broker_digest_lookup = _normalized(argv[8])
    expected_source = _normalized(
        "{{ lookup('ansible.builtin.file', "
        "playbook_dir ~ '/../../../scripts/hil/lib/bench_lock_verify.py', rstrip=false) }}"
    )
    expected_digest = _normalized(
        "{{ lookup('ansible.builtin.file', "
        "playbook_dir ~ '/../../../scripts/hil/lib/bench_host.sh', "
        "rstrip=false) | hash('sha256') }}"
    )
    expected_broker_digest = _normalized(
        "{{ lookup('ansible.builtin.file', "
        "playbook_dir ~ '/../../../scripts/hil/lib/bench_lock_broker.py', rstrip=false) "
        "| hash('sha256') }}"
    )
    if source_lookup != expected_source:
        errors.append(f"{label} role: verifier bytes are not sourced from the reviewed tree")
    if digest_lookup != expected_digest:
        errors.append(f"{label} role: holder digest is not sourced from the reviewed tree")
    if broker_digest_lookup != expected_broker_digest:
        errors.append(f"{label} role: broker digest is not sourced from the reviewed tree")
    return errors


def _bench_hold_errors(hold: dict[str, object], binding: dict[str, object]) -> list[str]:
    """Require the capability and exact remote holder binding."""
    hold_expected = _normalized(
        """ansible_check_mode or
        ((hil_bench_maintenance_lock_id | default(''))
         is match('^[0-9a-f]{16}$') and
         dev_box_hil_runner_bench_alias | length > 0)"""
    )
    errors = []
    if _conditions(hold) != (hold_expected,):
        errors.append("dev_box/tasks/main.yml: bench hold requirement is not exact")
    block = binding.get("block")
    if binding.get("when") != "not ansible_check_mode" or not isinstance(block, list):
        return [*errors, "dev_box/tasks/main.yml: delegated holder block is not exact"]
    return errors + _dev_binding_errors(block)


def _dev_invocation_errors(tasks: list[dict[str, object]]) -> list[str]:
    """Require exact safe service state and live bench hold before mutation."""
    names = (
        "Read native HIL listener load state before any dev-box mutation",
        "Read native HIL listener activity before any dev-box mutation",
        "Refuse every dev-box mutation without an exact safe listener state",
        "Require a live wrapped bench hold before every dev-box mutation",
        "Bind this dev-box apply to the exact live wrapped bench holder",
    )
    try:
        found = [_named(tasks, name) for name in names]
    except ValueError as exc:
        return [f"dev_box/tasks/main.yml: {exc}"]
    indices = [index for index, _ in found]
    load, activity, refusal, hold, binding = (task for _, task in found)
    errors = _listener_state_errors(load, activity, refusal)
    errors.extend(_bench_hold_errors(hold, binding))
    if indices != list(range(5)):
        errors.append("dev_box/tasks/main.yml: service/hold proof is not the role prefix")
    return errors


def _include_guard_errors(
    tasks: list[dict[str, object]],
    name: str,
    module: str,
    expected: dict[str, object],
    label: str,
) -> list[str]:
    """Require one exact always-tagged authenticated include at task zero."""
    if not tasks or tasks[0].get("name") != name:
        return [f"{label}: authenticated guard is not the role/task prefix"]
    task = tasks[0]
    if task.get(module) != expected or task.get("tags") != ["always"]:
        return [f"{label}: authenticated guard include is not exact"]
    return []


def _dev_order_errors(tasks: list[dict[str, object]], source: str, handler: str) -> list[str]:
    """Require the HIL subrole to start with identity, not a mutation."""
    errors = []
    try:
        apt_at, _ = _named(tasks, "Install the official runner's Debian runtime dependencies")
        _, start = _named(tasks, "Enable and start the dedicated HIL listener")
    except ValueError as exc:
        return [f"hil_runner.yml: {exc}"]
    include_errors = _include_guard_errors(
        tasks,
        "Re-authenticate the mutation boundary for direct HIL task inclusion",
        "ansible.builtin.include_tasks",
        {"file": "hil_mutation_guard.yml", "apply": {"tags": ["always"]}},
        "hil_runner.yml",
    )
    if include_errors:
        errors.extend(include_errors)
    if (
        tasks[2].get("name") != "Require the fleet-derived native HIL declaration"
        or apt_at != EXPECTED_APT_INDEX
    ):
        errors.append("hil_runner.yml: identity proof does not precede its first mutator")
    service = start.get("ansible.builtin.systemd_service")
    if not isinstance(service, dict) or service.get("state") != "started":
        errors.append("hil_runner.yml: final listener start is missing")
    if (
        "Restart the HIL Actions runner" in source
        or "state: restarted" in source
        or "Restart the HIL Actions runner" in handler
        or "state: restarted" in handler
    ):
        errors.append("dev_box: an unguarded listener restart path remains")
    errors.extend(check_mode.errors(tasks))
    return errors


def _dev_role_errors(main_source: str, source: str, handler: str, guard_source: str) -> list[str]:
    """Require idle proof before all dev-box and native-listener mutations."""
    main_tasks, errors = _tasks(main_source, "dev_box/tasks/main.yml")
    if errors:
        return errors
    tasks, errors = _tasks(source, "hil_runner.yml")
    if errors:
        return errors
    guard_tasks, errors = _tasks(guard_source, "hil_mutation_guard.yml")
    if errors:
        return errors
    main_errors = _include_guard_errors(
        main_tasks,
        "Authenticate the HIL and bench mutation boundary before this role",
        "ansible.builtin.include_tasks",
        {"file": "hil_mutation_guard.yml", "apply": {"tags": ["always"]}},
        "dev_box/tasks/main.yml",
    )
    return (
        main_errors
        + _dev_invocation_errors(guard_tasks)
        + _dev_order_errors(tasks, source, handler)
    )


def _idle_transaction_errors(idle: ast.FunctionDef) -> list[str]:
    """Require the exact executable freeze/inspect/stop/thaw transaction."""
    transaction = [node for node in idle.body if isinstance(node, ast.Try)]
    if len(transaction) != 1:
        return ["idle-stop helper: transaction try/finally is missing or duplicated"]
    body = transaction[0].body
    expected = (
        "freeze_may_have_landed = True",
        'control.command("freeze", service)',
        'freezer = control.command("show", "--property=FreezerState", "--value", service)',
        'if freezer != "frozen":\n'
        '    message = f"{service} did not enter the frozen state"\n'
        "    raise IdleStopError(message)",
        'group = control.command("show", "--property=ControlGroup", "--value", service)',
        "if any(_is_worker(proc_root, pid) for pid in "
        "_cgroup_pids(_cgroup_path(cgroup_root, group))):\n"
        '    message = "Runner.Worker is active; refusing to stop the listener"\n'
        "    raise IdleStopError(message)",
        'control.command("stop", "--no-block", service)',
        "_wait_stop_committed(control, service, stop_commit_timeout_s)",
        'control.command("thaw", service, accept=(0, 1))',
        "freeze_may_have_landed = False",
        "_wait_inactive(control, service, 30)",
        "return True",
    )
    errors = []
    if len(body) != len(expected) or any(
        not _same_statement(node, statement)
        for node, statement in zip(body, expected, strict=False)
    ):
        errors.append("idle-stop helper: executable transaction shape is not exact")
    final = transaction[0].finalbody
    final_source = 'if freeze_may_have_landed:\n    control.command("thaw", service, accept=(0, 1))'
    if len(final) != 1 or not _same_statement(final[0], final_source):
        errors.append("idle-stop helper: fail-safe thaw is not exact")
    return errors


def _helper_entry_errors(tree: ast.Module, gate_source: str) -> list[str]:
    """Require signal unwinding and an executable semantic helper selftest."""
    main = _function(tree, "main")
    if main is None:
        return ["idle-stop helper: main is missing or duplicated"]
    required = (
        "signal.signal(signal.SIGTERM, _interrupt)",
        "signal.signal(signal.SIGHUP, _interrupt)",
    )
    errors = []
    if any(_statement_index(main, statement) < 0 for statement in required):
        errors.append("idle-stop helper: signal unwinding is not executable and exact")
    invocation = (
        "python3 infra/ansible/roles/dev_box/files/"
        "ra8-hil-runner-idle-stop.py --selftest ignored.service"
    )
    executable = [line.strip() for line in gate_source.splitlines() if line.strip()]
    if executable.count(invocation) != 1:
        errors.append("checks.sh: exact idle-stop semantic selftest is not executable")
    return errors


def _helper_errors(source: str, gate_source: str) -> list[str]:
    """Require exact executable helper control flow, not source tokens."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return ["idle-stop helper: invalid Python"]
    idle = _function(tree, "idle_stop")
    if idle is None:
        return ["idle-stop helper: idle_stop is missing or duplicated"]
    prefix = (
        'load_state = control.command("show", "--property=LoadState", "--value", service)',
        'if load_state == "not-found":\n    return False',
        'if load_state != "loaded":\n'
        '    message = f"{service} has unsafe load state {load_state!r}"\n'
        "    raise IdleStopError(message)",
    )
    errors = _idle_transaction_errors(idle) + _helper_entry_errors(tree, gate_source)
    if any(_statement_index(idle, statement) < 0 for statement in prefix):
        errors.append("idle-stop helper: absent and unsafe unit decisions are not exact")
    worker = _function(tree, "_is_worker")
    worker_return = 'return executable == "Runner.Worker" or comm == "Runner.Worker"'
    if worker is None or _statement_index(worker, worker_return) < 0:
        errors.append("idle-stop helper: exact Runner.Worker identity proof is missing")
    return errors


def _converge_ast_errors(tree: ast.Module) -> list[str]:
    """Require the live wrapper and preview in executable statement order."""
    function = _function(tree, "cmd_converge")
    if function is None:
        return ["fleet.py: cmd_converge is missing or duplicated"]
    expected = (
        "guard = _bench_guard_argv(host, plays, args)",
        "if guard:\n    return _run(guard, cwd=fm.REPO_ROOT)",
        "rc = cmd_inventory(data, argparse.Namespace(stdout=False))",
        "maintenance = _prepare_native_runner(request)",
        "if not maintenance.proceed:\n    return maintenance.status",
        "rc = _run_converge_transport(request)",
    )
    indices = [_statement_index(function, statement) for statement in expected]
    if -1 in indices or indices != sorted(indices):
        return ["fleet.py: executable lock/preflight/idle-stop/transport order is not exact"]
    return []


def _fleet_selftest_errors(tree: ast.Module) -> list[str]:
    """Require the semantic lock, WSL, and maintenance selftests to execute."""
    function = _function(tree, "cmd_selftest")
    statement = (
        "failures = (ftv.run_selftest() + fw.run_selftest(data) + fb.run_selftest() + "
        "frm.run_selftest() + fb.parser_selftest(_parser))"
    )
    if function is None or _statement_index(function, statement) < 0:
        return ["fleet.py: executable HIL transaction selftests are not exact"]
    return []


def _runner_environment_errors(tree: ast.Module) -> list[str]:
    """Require inherited Ansible controls to be discarded structurally."""
    function = _function(tree, "ansible_environment")
    expected = (
        'resolved_cwd = _require_real_directory(ansible_cwd, "Ansible working directory")',
        'repo_root = _require_real_directory(resolved_cwd.parents[1], "repository root")',
        'config = _require_real_file(resolved_cwd / "ansible.cfg", "repository Ansible config")',
        'collection_parent = _require_real_directory(repo_root / ".ansible", "collection parent")',
        "collections = _require_real_directory(collection_parent / "
        "'collections', 'collection root')",
        "link_errors = fpa.confined_link_errors(collections)",
        "del environment",
        'clean = {"HOME": pwd.getpwuid(os.getuid()).pw_dir, "LANG": "C.UTF-8", '
        '"LC_ALL": "C.UTF-8", "PATH": "/usr/bin:/bin"}',
        'clean["ANSIBLE_CONFIG"] = str(config)',
        'clean["ANSIBLE_COLLECTIONS_PATH"] = str(collections)',
        'clean["ANSIBLE_COLLECTIONS_SCAN_SYS_PATH"] = "false"',
        'clean["PYTHONNOUSERSITE"] = "1"',
        "return clean",
    )
    if function is None or any(_statement_index(function, item) < 0 for item in expected):
        return ["fleet runner maintenance: Ansible environment sanitizer is not exact"]
    return []


def _playbook_environment_errors(tree: ast.Module) -> list[str]:
    """Require the playbook executable beside the active locked Python."""
    function = _function(tree, "playbook_argv")
    if function is None:
        return ["fleet.py: locked Ansible executable decision is not exact"]
    argv = _assignment(function, "argv")
    first = argv.elts[0] if isinstance(argv, ast.List) and argv.elts else None
    wanted = ast.parse("playbook_executable(sys.executable)", mode="eval").body
    if first is None or ast.dump(first, include_attributes=False) != ast.dump(
        wanted, include_attributes=False
    ):
        return ["fleet.py: playbook argv does not use the locked executable"]
    return []


def _runner_prepare_errors(tree: ast.Module) -> list[str]:
    """Require executable fail-closed no-op and idle-stop decisions."""
    function = _function(tree, "prepare")
    if function is None:
        return ["fleet runner maintenance: prepare is missing or duplicated"]
    no_op = (
        "if not has_changes:\n"
        '    print("fleet: native HIL listener is already converged; leaving it running")\n'
        "    return MaintenanceDecision(proceed=False, status=0)"
    )
    decision = "return MaintenanceDecision(proceed=stop.returncode == 0, status=stop.returncode)"
    if _statement_index(function, no_op) < 0 or _statement_index(function, decision) < 0:
        return ["fleet runner maintenance: executable no-op/idle-stop decision is not exact"]
    return []


def _converge_env_errors(tree: ast.Module) -> list[str]:
    """Require every native play to use the exact sanitized environment."""
    function = _function(tree, "_converge_ssh")
    if function is None:
        return ["fleet.py: native converge transport is missing or duplicated"]
    calls = [
        node
        for node in ast.walk(function)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "_run"
    ]
    if len(calls) != 1:
        return ["fleet.py: native converge runner call is missing or duplicated"]
    env = next((keyword.value for keyword in calls[0].keywords if keyword.arg == "env"), None)
    expected = ast.parse("frm.ansible_environment(os.environ, fm.ANSIBLE_DIR)", mode="eval").body
    if env is None or ast.dump(env, include_attributes=False) != ast.dump(
        expected, include_attributes=False
    ):
        return ["fleet.py: actual Ansible environment is not exact"]
    return []


def _wsl_mode_errors(tree: ast.Module) -> list[str]:
    """Require the validated operation mode to reach the WSL toolchain."""
    function = _function(tree, "_run_converge_transport")
    calls = (
        [
            node
            for node in ast.walk(function)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "fw"
            and node.func.attr == "ConvergeSpec"
        ]
        if function is not None
        else []
    )
    wanted = ast.parse("request.args.mode", mode="eval").body
    mode_arg = 5
    if len(calls) != 1 or len(calls[0].args) <= mode_arg:
        return ["fleet.py: WSL convergence mode binding is absent"]
    if ast.dump(calls[0].args[mode_arg], include_attributes=False) != ast.dump(
        wanted, include_attributes=False
    ):
        return ["fleet.py: WSL convergence mode binding is not exact"]
    return []


def _infra_uv_execution_errors(source: str) -> list[str]:
    """Require infra lock verification to execute only authenticated uv bytes."""
    expected = """verify_managed_python_environment() {
  (
    cd "$ROOT"
    UV_PROJECT_ENVIRONMENT="$MANAGED_VENV" UV_PYTHON_DOWNLOADS=never \\
      UV_CACHE_DIR="$ROOT/.tools/uv" \\
      /usr/bin/python3 -I -S "$ROOT/scripts/dev/bootstrap_uv.py" \\
      --run --no-config sync --locked --all-groups --no-install-project \\
      --python /usr/bin/python3 --check
  ) >/dev/null
}

if ! verify_managed_python_environment; then
  echo "error: repository .venv does not exactly match pyproject.toml and uv.lock" >&2
  exit 1
fi"""
    function = re.search(r"(?ms)^verify_managed_python_environment\(\) \{.*?^fi$", source)
    if function is None or function.group(0) != expected:
        return ["infra.sh: dependency verification bypasses authenticated uv execution"]
    return []


def _wsl_cache_errors(wsl_tree: ast.Module, stage_tree: ast.Module) -> list[str]:
    """Require cache ownership before reuse and a no-follow receiver."""
    errors: list[str] = []
    sync_image = _function(wsl_tree, "_sync_runner_image")
    cache_prepare = (
        "rc = run([*target_ssh, fm.remote_shell(target)], stdin=fws.cache_prepare_script())"
    )
    prepare_at = _statement_index(sync_image, cache_prepare) if sync_image is not None else -1
    cache_probe_at = -1
    if sync_image is not None:
        cache_probe_at = next(
            (
                index
                for index, node in enumerate(sync_image.body)
                if isinstance(node, ast.Assign)
                and isinstance(node.value, ast.Call)
                and isinstance(node.value.func, ast.Name)
                and node.value.func.id == "_command_output"
                and any(
                    isinstance(target, ast.Tuple)
                    and any(
                        isinstance(name, ast.Name) and name.id == "cache_rc" for name in target.elts
                    )
                    for target in node.targets
                )
            ),
            -1,
        )
    if prepare_at < 0 or cache_probe_at < 0 or prepare_at >= cache_probe_at:
        errors.append("fleet WSL: runner cache ownership is not proven before reuse")
    receiver = _function(stage_tree, "cache_receive_command")
    code = _assignment(receiver, "code") if receiver is not None else None
    expected_code = (
        "import os,shutil,sys;"
        "fd=os.open(sys.argv[1],os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600);"
        "out=os.fdopen(fd,'wb');shutil.copyfileobj(sys.stdin.buffer,out);"
        "out.flush();os.fsync(out.fileno());out.close()"
    )
    if not isinstance(code, ast.Constant) or code.value != expected_code:
        errors.append("fleet WSL stage: runner-cache receiver is not no-follow")
    return errors


def _wsl_stage_errors(wsl_tree: ast.Module, stage_tree: ast.Module) -> list[str]:
    """Require executable owned-stage/cache selftests and publication order."""
    errors = _wsl_cache_errors(wsl_tree, stage_tree)
    wsl_selftest = _function(wsl_tree, "run_selftest")
    stage_selftest_call = "stage_failures = fws.run_selftest()"
    if wsl_selftest is None or _statement_index(wsl_selftest, stage_selftest_call) < 0:
        errors.append("fleet WSL: owned stage/cache semantic selftest is not executable")
    converge = _function(wsl_tree, "converge")
    push_call = "rc = fws.push(data, name, spec.mode, run)"
    if converge is None or _statement_index(converge, push_call) < 0:
        errors.append("fleet WSL: owned stage publication is not executable")
    stage_selftest = _function(stage_tree, "run_selftest")
    expected_return = "return _stage_selftest(root) + _cache_selftest(root) + _link_selftest(root)"
    returns = (
        [node for node in ast.walk(stage_selftest) if isinstance(node, ast.Return)]
        if stage_selftest is not None
        else []
    )
    if len(returns) != 1 or not _same_statement(returns[0], expected_return):
        errors.append("fleet WSL stage: ownership semantic selftests are not exact")
    push = _function(stage_tree, "push")
    required = (
        "tar_rc, archive = _stage_archive(mode)",
        "if tar_rc:\n    return tar_rc",
    )
    if push is None or any(_statement_index(push, statement) < 0 for statement in required):
        errors.append("fleet WSL stage: local archive is not proven before remote mutation")
    return errors


def _fleet_errors(inputs: dict[str, str]) -> list[str]:
    """Require structural lock and exact Ansible environment boundaries."""
    try:
        tree = ast.parse(inputs["fleet"])
        ast.parse(inputs["fleet_bench"])
        runner_tree = ast.parse(inputs["fleet_runner"])
        wsl_tree = ast.parse(inputs["fleet_wsl"])
        wsl_stage_tree = ast.parse(inputs["fleet_wsl_stage"])
    except SyntaxError:
        return ["fleet bench guard: invalid Python"]
    errors = _converge_ast_errors(tree) + _fleet_selftest_errors(tree)
    errors.extend(_playbook_environment_errors(runner_tree))
    errors.extend(_runner_environment_errors(runner_tree))
    errors.extend(_runner_prepare_errors(runner_tree))
    errors.extend(_converge_env_errors(tree))
    errors.extend(_wsl_mode_errors(tree))
    errors.extend(wsl.environment_errors(wsl_tree))
    errors.extend(_wsl_stage_errors(wsl_tree, wsl_stage_tree))
    gate_lines = [line.strip() for line in inputs["gate"].splitlines() if line.strip()]
    if gate_lines.count("python3 scripts/dev/fleet.py selftest") != 1:
        errors.append("checks.sh: exact fleet transaction semantic selftest is not executable")
    return errors


def _scan(inputs: dict[str, str]) -> list[str]:
    """Return every convergence-safety defect."""
    return (
        image_lock_digest.source_errors(
            (
                inputs["devcontainer_image"],
                inputs["devcontainer_image_lock_receipts"],
                inputs["devcontainer_image_lock_selftest"],
                inputs["devcontainer_image_selftest"],
                inputs["devcontainer_image_bound_exit_selftest"],
                inputs["devcontainer_image_selftest_cases"],
                inputs["devcontainer_image_signal_selftest"],
                inputs["devcontainer_image_selftest_process"],
                inputs["devcontainer_image_selftest_supervisor"],
                inputs["devcontainer_image_selftest_supervisor_cases"],
                inputs["raw_digest_controls"],
            ),
            inputs["image_lock_digest"],
        )
        + _dev_role_errors(
            inputs["dev_main"],
            inputs["dev_role"],
            inputs["dev_handler"],
            inputs["dev_guard"],
        )
        + _helper_errors(inputs["idle_helper"], inputs["gate"])
        + _fleet_errors(inputs)
        + image_harness_policy.errors(inputs)
        + _infra_uv_execution_errors(inputs["infra_sh"])
        + python_authority.uv_helper_deployment_errors(inputs)
        + python_authority.hil_python_authority_errors(inputs["bench_role"])
        + roles.errors(inputs)
        + policy.workflow_errors(inputs["workflow"], inputs["declaration"])
        + v8.errors(inputs)
        + v9.errors(inputs)
    )


def main(argv: list[str] | None = None) -> int:
    """Run the live scan or the mutation selftest."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest.run(_scan)
    raw_errors = image_lock_digest.live_errors(policy.REPO_ROOT)
    if raw_errors:
        for error in raw_errors:
            print(error, file=sys.stderr)
        print(
            f"check_hil_convergence_safety.py: {len(raw_errors)} raw-byte error(s)",
            file=sys.stderr,
        )
        return 1
    errors = _scan(policy.load_inputs(policy.REPO_ROOT)) + v9.semantic_errors()
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"check_hil_convergence_safety.py: {len(errors)} error(s)", file=sys.stderr)
        return 1
    print("check_hil_convergence_safety.py: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
