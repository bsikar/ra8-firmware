# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Semantic uv-cache bootstrap, provisioner, and Ansible policy checks."""

from __future__ import annotations

import ast
import copy
import hashlib
from pathlib import Path

import yaml
from python_lock_policy_uv_cache_contracts import (
    bootstrap_expected_bodies,
    bootstrap_module_mutations,
    mode_execution_references,
    mode_module_mutations,
    mutate_named_function_once,
    portable_execution_references,
)
from python_lock_policy_uv_cache_release import (
    provisioner_findings,
    provisioner_mutation_failures,
)

BOOTSTRAP_MODULE_SHA256 = "8f15ae1b817238a570b674c6023cc566ef4e43c3ac8e22da985711d592fab255"
MODE_TEST_MODULE_SHA256 = "fc33d269a68f0c6e5ed244d801be5f4de4ebdc0c19e307b79014304238d7db46"
PORTABLE_REGISTRY_DIGEST = "f4716c7f8a12aa6b22c092304d68978b98af975220dbe4099b64f6e8facd3cfe"
MODE_EXECUTION_REGISTRY_DIGEST = "18069a88c9c7ef89fc67ae1610cd95a7fa8459ff2e913c058cf90cec60f9ad92"


def _function(tree: ast.AST, name: str) -> ast.FunctionDef | None:
    """Return one top-level function definition, rejecting ambiguity."""
    matches = [
        node
        for node in getattr(tree, "body", [])
        if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    return matches[0] if len(matches) == 1 else None


def _without_docstring(function: ast.FunctionDef) -> ast.Module:
    """Return a comparable body without its optional documentation literal."""
    body = list(function.body)
    if (
        body
        and isinstance(body[0], ast.Expr)
        and isinstance(body[0].value, ast.Constant)
        and isinstance(body[0].value.value, str)
    ):
        body = body[1:]
    return ast.Module(body=body, type_ignores=[])


def _body_dump(function: ast.FunctionDef) -> str:
    """Return one formatting-independent function-body identity."""
    return ast.dump(_without_docstring(function), include_attributes=False)


def _expected_body(source: str, name: str) -> str:
    """Return the semantic body identity from a canonical function fixture."""
    function = _function(ast.parse(source), name)
    if function is None:
        message = f"canonical function fixture is missing {name}"
        raise ValueError(message)
    return _body_dump(function)


def _direct_call_count(function: ast.FunctionDef, name: str) -> int:
    """Count direct top-level expression calls within one function body."""
    return sum(
        isinstance(statement, ast.Expr)
        and isinstance(statement.value, ast.Call)
        and isinstance(statement.value.func, ast.Name)
        and statement.value.func.id == name
        for statement in function.body
    )


def _all_call_count(function: ast.FunctionDef, name: str) -> int:
    """Count calls within one function while excluding nested definitions."""
    count = 0
    pending = list(function.body)
    while pending:
        node = pending.pop()
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            continue
        count += int(
            isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == name
        )
        pending.extend(ast.iter_child_nodes(node))
    return count


def _attribute_path(node: ast.AST) -> str:
    """Return one dotted-name path or an empty string for a dynamic owner."""
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        parent = _attribute_path(node.value)
        return f"{parent}.{node.attr}" if parent else ""
    return ""


def _deep_qualified_reference_count(function: ast.FunctionDef, owner: str, name: str) -> int:
    """Count exact module attributes throughout one selftest function."""
    return sum(
        isinstance(node, ast.Attribute)
        and _attribute_path(node.value) == owner
        and node.attr == name
        for node in ast.walk(function)
    )


def _bootstrap_function_findings(tree: ast.Module) -> list[str]:
    """Bind security-critical bootstrap functions to their reviewed semantics."""
    findings: list[str] = []
    for name, source in bootstrap_expected_bodies().items():
        function = _function(tree, name)
        if function is None or _body_dump(function) != _expected_body(source, name):
            findings.append(f"uv bootstrap {name} semantic contract drifted")
    run_selftest = _function(tree, "run_selftest")
    if run_selftest is None or not (
        _direct_call_count(run_selftest, "cache_mode_selftest") == 1
        and _all_call_count(run_selftest, "cache_mode_selftest") == 1
    ):
        findings.append("bootstrap run_selftest does not directly execute cache_mode_selftest once")
    return findings


def _bootstrap_main_guard_findings(tree: ast.Module) -> list[str]:
    """Bind typed apply-required status to the production script entrypoint."""
    expected = ast.dump(
        ast.parse(
            """
if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CacheApplyRequiredError as error:
        print(f"APPLY REQUIRED: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    except BootstrapError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1) from error
"""
        ).body[0],
        include_attributes=False,
    )
    matches = [
        statement
        for statement in tree.body
        if ast.dump(statement, include_attributes=False) == expected
    ]
    classes = [
        node
        for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "CacheApplyRequiredError"
    ]
    findings = [] if len(matches) == 1 else ["bootstrap apply-required exit contract drifted"]
    if len(classes) != 1:
        findings.append("bootstrap CacheApplyRequiredError type is missing or ambiguous")
    return findings


def _bootstrap_verify_dispatch_findings(tree: ast.Module) -> list[str]:
    """Bind verification to bounded FDs and an authenticated-byte probe."""
    function = _function(tree, "verify_cached_uv")
    payload = _function(tree, "validated_cached_payload")
    required = {
        "validated_cached_payload": 1,
        "authenticated_cache_fds": 1,
        "verify_fd_mode": 2,
        "probe_authenticated_uv": 1,
        "verify_fd_unchanged": 2,
        "verify_fd_path": 2,
    }
    if function is None or payload is None:
        return ["bootstrap verify_cached_uv is missing or ambiguous"]
    findings = [
        f"bootstrap verify_cached_uv {name} call chain drifted"
        for name, expected in required.items()
        if _all_call_count(function, name) != expected
    ]
    findings.extend(
        f"bootstrap cached payload {name} call chain drifted"
        for name in ("open_cache_fd", "read_stable_fd")
        if _all_call_count(payload, name) != 1
    )
    return findings


def _mode_runner_findings(tree: ast.Module) -> list[str]:
    """Bind the public runner to every critical adversarial test leg."""
    expected = """
def run_mode_selftest():
    if os.name == "posix":
        darwin_root_alias_acceptance_selftest()
        darwin_root_alias_rejection_selftest()
        cache_mode_convergence_selftest()
        cache_mode_authentication_selftest()
        cache_open_flags_selftest()
        cache_parent_symlink_selftest()
        cache_parent_swap_selftest()
        cache_atomic_parent_swap_selftest()
        cache_nonregular_selftest()
        cache_stable_read_selftest()
        cache_verification_fifo_race_selftest()
        cache_exact_fd_execution_selftest()
        cache_same_inode_execution_selftest()
        cache_run_exit_status_selftest()
        cache_run_signal_status_selftest()
        portable_readonly_fd_selftest()
        portable_snapshot_flags_selftest()
        for attack in ("replace", "symlink", "hardlink", "overwrite", "descriptor"):
            portable_snapshot_path_attack_selftest(attack)
        portable_snapshot_unlink_failure_selftest()
        for target in ("archive", "binary"):
            cache_mode_path_attack_selftest(target, "symlink", preexisting=True)
            for replacement in ("symlink", "regular"):
                cache_mode_path_attack_selftest(target, replacement, preexisting=False)
        cache_status_contract_selftest()
    cache_mode_readonly_and_windows_selftest()
"""
    runner = _function(tree, "run_mode_selftest")
    if runner is None or _body_dump(runner) != _expected_body(expected, "run_mode_selftest"):
        return ["uv mode selftest runner no longer executes every critical leg"]
    return []


def _mode_cache_references() -> dict[str, tuple[tuple[str, str, int], ...]]:
    """Return critical cache-open/read selftest references."""
    return {
        "darwin_root_alias_acceptance_selftest": (
            ("", "open_simulated_darwin_parent", 1),
            ("b.bootstrap_uv_exec", "open_parent_components", 1),
            ("b", "fail", 1),
        ),
        "darwin_root_alias_rejection_selftest": (("", "expect_exec_failure", 6),),
        "cache_mode_convergence_selftest": (("b", "ensure_uv", 1),),
        "cache_mode_authentication_selftest": (("b", "expect_bootstrap_error", 1),),
        "cache_open_flags_selftest": (("b", "open_cache_fd", 2),),
        "cache_parent_symlink_selftest": (
            ("link", "symlink_to", 1),
            ("b", "open_cache_fd", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_parent_swap_selftest": (
            ("original_parent", "rename", 1),
            ("original_parent", "symlink_to", 1),
            ("b", "normalize_cached_modes", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_atomic_parent_swap_selftest": (
            ("os", "replace", 1),
            ("original_parent", "rename", 1),
            ("original_parent", "symlink_to", 1),
            ("b", "ensure_uv", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_nonregular_selftest": (
            ("os", "mkfifo", 1),
            ("socket", "socket", 1),
            ("b", "open_cache_fd", 1),
        ),
        "cache_stable_read_selftest": (
            ("b", "open_cache_fd", 2),
            ("b", "read_stable_fd", 2),
            ("b", "expect_bootstrap_error", 1),
            ("", "expect_concurrent_read_rejected", 2),
        ),
        "expect_concurrent_read_rejected": (
            ("os", "utime", 1),
            ("b", "open_cache_fd", 1),
            ("b", "read_stable_fd", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
        "cache_verification_fifo_race_selftest": (
            ("os", "mkfifo", 1),
            ("b", "verify_cached_uv", 1),
            ("b", "expect_bootstrap_error", 1),
        ),
    }


def _mode_test_body_findings(tree: ast.Module) -> list[str]:
    """Require each adversarial mode test to retain its critical call chain."""
    portable_references = portable_execution_references()
    mode_references = mode_execution_references()
    required = {
        **_mode_cache_references(),
        **mode_references,
        **portable_references,
    }
    findings = [
        *_registry_findings(portable_references, PORTABLE_REGISTRY_DIGEST, "portable uv execution"),
        *_registry_findings(mode_references, MODE_EXECUTION_REGISTRY_DIGEST, "uv mode execution"),
    ]
    for function_name, references in required.items():
        function = _function(tree, function_name)
        if function is None:
            findings.append(f"uv mode selftest critical function is missing: {function_name}")
            continue
        for owner, name, expected in references:
            actual = (
                _deep_qualified_reference_count(function, owner, name)
                if owner
                else _all_call_count(function, name)
            )
            if actual != expected:
                findings.append(
                    f"uv mode selftest {function_name} {owner}.{name} call chain drifted"
                )
    return findings


def _registry_findings(
    references: dict[str, tuple[tuple[str, str, int], ...]],
    expected_digest: str,
    label: str,
) -> list[str]:
    """Bind one split selftest registry against silent collapse or drift."""
    digest = hashlib.sha256(repr(references).encode()).hexdigest()
    if digest != expected_digest:
        return [f"{label} selftest registry drifted"]
    return []


def bootstrap_uv_findings(bootstrap: str, mode_test: str) -> list[str]:
    """Return semantic bootstrap/selftest binding findings."""
    identity_findings = (
        []
        if hashlib.sha256(bootstrap.encode()).hexdigest() == BOOTSTRAP_MODULE_SHA256
        else ["uv bootstrap module byte identity drifted"]
    )
    if hashlib.sha256(mode_test.encode()).hexdigest() != MODE_TEST_MODULE_SHA256:
        identity_findings.append("uv mode selftest module byte identity drifted")
    try:
        bootstrap_tree = ast.parse(bootstrap)
        mode_tree = ast.parse(mode_test)
    except SyntaxError as exc:
        return [f"uv bootstrap policy input is invalid Python: {exc}"]
    return [
        *identity_findings,
        *_bootstrap_function_findings(bootstrap_tree),
        *_bootstrap_main_guard_findings(bootstrap_tree),
        *_bootstrap_verify_dispatch_findings(bootstrap_tree),
        *_mode_runner_findings(mode_tree),
        *_mode_test_body_findings(mode_tree),
    ]


def gate_selftest_findings(source: str) -> list[str]:
    """Require the real pre-commit gate to execute the provisioner selftest."""
    start = "_pcc_python_authority() (\n"
    if source.count(start) != 1:
        return ["Python-authority gate body is missing or ambiguous"]
    body, separator, _rest = source.split(start, 1)[1].partition("\n)\n")
    if not separator:
        return ["Python-authority gate body is unterminated"]
    lines = [
        " ".join(line.strip().split())
        for line in body.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    expected = (
        "/bin/bash -p scripts/dev/provision_dev_box_toolchain.sh --selftest-uv-cache-contract"
    )
    lock_selftest = "python3 scripts/checks/check_python_lock_policy.py --selftest"
    if lines.count(expected) != 1 or lines.count(lock_selftest) != 1:
        return ["Python-authority gate does not run both uv-cache contract selftests"]
    if lines.index(expected) >= lines.index(lock_selftest):
        return ["provisioner contract selftest does not precede its policy check"]
    return []


def _tasks(document: object) -> list[dict[str, object]]:
    """Return one typed Ansible task list."""
    if not isinstance(document, list):
        return []
    return [task for task in document if isinstance(task, dict)]


def _task_by_register(tasks: list[dict[str, object]], name: str) -> dict[str, object] | None:
    """Return one task owning an exact Ansible register."""
    matches = [task for task in tasks if task.get("register") == name]
    return matches[0] if len(matches) == 1 else None


def _task_by_name(tasks: list[dict[str, object]], name: str) -> dict[str, object] | None:
    """Return one task carrying an exact display name."""
    matches = [task for task in tasks if task.get("name") == name]
    return matches[0] if len(matches) == 1 else None


def _check_task_findings(check: dict[str, object] | None) -> list[str]:
    """Validate the forced read-only Ansible check task."""
    findings: list[str] = []
    check_argv = [
        "/bin/bash",
        "-p",
        "scripts/dev/provision_dev_box_toolchain.sh",
        "--check-only",
    ]
    if check is None:
        return ["dev-box Ansible check-mode provisioner task is missing or ambiguous"]
    command = check.get("ansible.builtin.command")
    argv = command.get("argv") if isinstance(command, dict) else None
    if argv != check_argv:
        findings.append("dev-box Ansible check mode does not use exact read-only argv")
    if check.get("when") != "ansible_check_mode" or check.get("check_mode") is not False:
        findings.append("dev-box Ansible check task can skip or escape check mode")
    if check.get("become") is True:
        findings.append("dev-box Ansible read-only check unexpectedly escalates privilege")
    if check.get("changed_when") != "dev_box_provision_check.rc == 2":
        findings.append("dev-box Ansible drift status is not reported changed")
    if check.get("failed_when") != "dev_box_provision_check.rc not in [0, 2]":
        findings.append("dev-box Ansible check task does not fail real errors")
    return findings


def _postcheck_task_findings(task: dict[str, object] | None) -> list[str]:
    """Bind the post-provision import check to successful cache audit state."""
    expected_when = "not ansible_check_mode or (dev_box_provision_check.rc | default(0)) == 0"
    if task is None:
        return ["dev-box post-provision import task is missing or ambiguous"]
    if task.get("when") != expected_when:
        return ["dev-box post-provision import condition drifted"]
    return []


def _apply_task_findings(apply: dict[str, object] | None) -> list[str]:
    """Validate the disjoint mutating Ansible apply task."""
    findings: list[str] = []
    apply_argv = ["/bin/bash", "-p", "scripts/dev/provision_dev_box_toolchain.sh"]
    if apply is None:
        return ["dev-box Ansible apply provisioner task is missing or ambiguous"]
    command = apply.get("ansible.builtin.command")
    argv = command.get("argv") if isinstance(command, dict) else None
    if argv != apply_argv:
        findings.append("dev-box Ansible apply mode does not use exact mutating argv")
    if apply.get("when") != "not ansible_check_mode":
        findings.append("dev-box Ansible apply task can execute during check mode")
    if apply.get("changed_when") != "'->' in dev_box_provision.stdout":
        findings.append("dev-box Ansible apply change marker drifted")
    if "failed_when" in apply:
        findings.append("dev-box Ansible apply task overrides command failure")
    return findings


def _ansible_invocation_findings(tasks: list[dict[str, object]]) -> list[str]:
    """Reject any third or missing provisioner command boundary."""
    check_argv = [
        "/bin/bash",
        "-p",
        "scripts/dev/provision_dev_box_toolchain.sh",
        "--check-only",
    ]
    apply_argv = ["/bin/bash", "-p", "scripts/dev/provision_dev_box_toolchain.sh"]
    invocations = []
    for task in tasks:
        command = task.get("ansible.builtin.command")
        argv = command.get("argv") if isinstance(command, dict) else None
        if isinstance(argv, list) and "scripts/dev/provision_dev_box_toolchain.sh" in argv:
            invocations.append(argv)
    if sorted(invocations) != sorted([check_argv, apply_argv]):
        return ["dev-box Ansible has an extra or missing provisioner invocation"]
    return []


def dev_box_uv_task_findings(document: object) -> list[str]:
    """Bind Ansible check/apply modes to exact disjoint provisioner argv."""
    tasks = _tasks(document)
    postcheck = _task_by_name(tasks, "Assert the provisioned libclang binding actually imports")
    return [
        *_check_task_findings(_task_by_register(tasks, "dev_box_provision_check")),
        *_apply_task_findings(_task_by_register(tasks, "dev_box_provision")),
        *_postcheck_task_findings(postcheck),
        *_ansible_invocation_findings(tasks),
    ]


def uv_cache_policy_findings(root: Path) -> list[str]:
    """Read and validate every uv-cache control-plane source."""
    try:
        bootstrap = (root / "scripts/dev/bootstrap_uv.py").read_text(encoding="utf-8")
        mode_test = (root / "scripts/dev/bootstrap_uv_mode_selftest.py").read_text(encoding="utf-8")
        provisioner = (root / "scripts/dev/provision_dev_box_toolchain.sh").read_text(
            encoding="utf-8"
        )
        provisioner_selftest = (
            root / "scripts/dev/provision_dev_box_toolchain_selftest.bash"
        ).read_text(encoding="utf-8")
        gate = (root / "scripts/ci/gates/checks.sh").read_text(encoding="utf-8")
        transaction = yaml.safe_load(
            (root / "infra/ansible/roles/dev_box/tasks/transaction.yml").read_text(encoding="utf-8")
        )
    except (OSError, UnicodeError, yaml.YAMLError) as exc:
        return [f"uv-cache policy input is unreadable: {exc}"]
    return [
        *bootstrap_uv_findings(bootstrap, mode_test),
        *provisioner_findings(provisioner, provisioner_selftest),
        *gate_selftest_findings(gate),
        *dev_box_uv_task_findings(transaction),
    ]


def _mutate_once(source: str, old: str, new: str) -> str:
    """Apply one exact mutation and fail if the fixture authority drifted."""
    if source.count(old) != 1:
        message = f"selftest mutation anchor count changed: {old!r}"
        raise ValueError(message)
    return source.replace(old, new, 1)


def _hollow_python_function(source: str, name: str) -> str:
    """Replace one top-level Python function body with a no-op."""
    tree = ast.parse(source)
    function = _function(tree, name)
    if function is None:
        message = f"selftest function is missing: {name}"
        raise ValueError(message)
    function.body = [ast.Pass()]
    ast.fix_missing_locations(tree)
    return ast.unparse(tree)


def _hollow_shell_function(source: str, name: str) -> str:
    """Replace one two-space-indented shell function body with a no-op."""
    lines = source.splitlines()
    starts = [index for index, line in enumerate(lines) if line == f"  {name}() {{"]
    if len(starts) != 1:
        message = f"selftest shell function is missing: {name}"
        raise ValueError(message)
    end = next(index for index in range(starts[0] + 1, len(lines)) if lines[index] == "  }")
    replacement = [lines[starts[0]], "    :", lines[end]]
    return "\n".join([*lines[: starts[0]], *replacement, *lines[end + 1 :]]) + "\n"


def _bootstrap_source_mutation_failures(bootstrap: str, mode_test: str) -> list[str]:
    """Prove each bootstrap production safeguard is independently bound."""
    failures: list[str] = []
    bootstrap_mutations = (
        *bootstrap_module_mutations(),
        (
            "        return bootstrap_uv_exec.open_regular_nofollow(path)\n",
            "        return os.open(path, os.O_RDONLY)\n",
        ),
        (
            "        bootstrap_uv_exec.write_atomic_nofollow(path, payload, mode)\n",
            "        path.write_bytes(payload)\n",
        ),
        (
            "        reopened = open_cache_fd(path)\n",
            "        path_state = os.lstat(path)\n",
        ),
        (
            "if any(getattr(before, field) != getattr(after, field) for field in stable):",
            "if False:",
        ),
        ("if len(payload) > maximum:", "if False:"),
        ("    cache_mode_selftest()\n", "    if False:\n        cache_mode_selftest()\n"),
        ('"--check-cache-modes",', '"--disabled-cache-modes",'),
        (
            "        verify_cached_modes(destination.parent / asset_name, destination)\n",
            "        pass\n",
        ),
        ('        "--run",\n', '        "--disabled-run",\n'),
        (
            "            completed = bootstrap_uv_exec.run_uv_snapshot(binary, arguments)\n",
            "            completed = None\n",
        ),
        (
            "    return propagate_child_status(completed.returncode)\n",
            "    return completed.returncode\n",
        ),
        (
            "        probe_authenticated_uv(binary, version)\n"
            "        verify_fd_unchanged(archive_path, archive_fd, archive_state)\n"
            "        verify_fd_unchanged(destination, destination_fd, installed_state)\n"
            "        verify_fd_path(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)\n"
            "        verify_fd_path(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)\n",
            "        probe_authenticated_uv(binary, version)\n"
            "        verify_fd_unchanged(archive_path, archive_fd, archive_state)\n"
            "        verify_fd_unchanged(destination, destination_fd, installed_state)\n",
        ),
        ("raise SystemExit(2) from error", "raise SystemExit(1) from error"),
    )
    failures.extend(
        f"uv bootstrap mutation passed: {old}"
        for old, new in bootstrap_mutations
        if not bootstrap_uv_findings(_mutate_once(bootstrap, old, new), mode_test)
    )
    return failures


def _mode_mutation_failures(bootstrap: str, mode_test: str) -> list[str]:
    """Prove every mode runner leg and critical test body is load-bearing."""
    failures = [
        f"uv mode module mutation passed: {old}"
        for old, new in mode_module_mutations()
        if not bootstrap_uv_findings(bootstrap, _mutate_once(mode_test, old, new))
    ]
    runner_calls = (
        "        darwin_root_alias_acceptance_selftest()\n",
        "        darwin_root_alias_rejection_selftest()\n",
        "        cache_mode_convergence_selftest()\n",
        "        cache_mode_authentication_selftest()\n",
        "        cache_open_flags_selftest()\n",
        "        cache_parent_symlink_selftest()\n",
        "        cache_parent_swap_selftest()\n",
        "        cache_atomic_parent_swap_selftest()\n",
        "        cache_nonregular_selftest()\n",
        "        cache_stable_read_selftest()\n",
        "        cache_verification_fifo_race_selftest()\n",
        "        cache_exact_fd_execution_selftest()\n",
        "        cache_same_inode_execution_selftest()\n",
        "        cache_run_exit_status_selftest()\n",
        "        cache_run_signal_status_selftest()\n",
        "        portable_readonly_fd_selftest()\n",
        "        portable_snapshot_flags_selftest()\n",
        "        portable_snapshot_unlink_failure_selftest()\n",
        '            cache_mode_path_attack_selftest(target, "symlink", preexisting=True)\n',
        "        cache_status_contract_selftest()\n",
        "    cache_mode_readonly_and_windows_selftest()\n",
    )
    failures.extend(
        f"uv mode runner mutation passed: {call.strip()}"
        for call in runner_calls
        if not bootstrap_uv_findings(bootstrap, _mutate_once(mode_test, call, ""))
    )
    return [*failures, *_mode_body_mutation_failures(bootstrap, mode_test)]


def _mode_body_mutation_failures(bootstrap: str, mode_test: str) -> list[str]:
    """Prove supported-Python binding and each critical mode-test body."""
    interpreter_mutations = (
        "cache_run_exit_status_selftest",
        "cache_run_signal_status_selftest",
        "bootstrap_run_status",
    )
    failures = [
        "unsupported hardcoded Python passed the uv mode policy"
        for name in interpreter_mutations
        if not bootstrap_uv_findings(
            bootstrap,
            mutate_named_function_once(
                mode_test,
                name,
                "sys.executable",
                '"/usr/bin/python3"',
            ),
        )
    ]
    critical_tests = (
        "darwin_root_alias_acceptance_selftest",
        "darwin_root_alias_rejection_selftest",
        "cache_mode_convergence_selftest",
        "cache_mode_authentication_selftest",
        "cache_open_flags_selftest",
        "cache_parent_symlink_selftest",
        "cache_parent_swap_selftest",
        "cache_atomic_parent_swap_selftest",
        "cache_nonregular_selftest",
        "expect_concurrent_read_rejected",
        "cache_stable_read_selftest",
        "cache_verification_fifo_race_selftest",
        "cache_exact_fd_execution_selftest",
        "cache_same_inode_execution_selftest",
        "cache_run_exit_status_selftest",
        "cache_run_signal_status_selftest",
        "portable_readonly_fd_selftest",
        "run_portable_snapshot",
        "portable_snapshot_flags_selftest",
        "portable_snapshot_path_attack_selftest",
        "portable_snapshot_unlink_failure_selftest",
        "cache_mode_path_attack_selftest",
        "bootstrap_run_status",
        "cache_status_contract_selftest",
        "cache_mode_readonly_and_windows_selftest",
    )
    failures.extend(
        f"hollow uv mode selftest passed policy: {name}"
        for name in critical_tests
        if not bootstrap_uv_findings(bootstrap, _hollow_python_function(mode_test, name))
    )
    return failures


def _bootstrap_mutation_failures(bootstrap: str, mode_test: str) -> list[str]:
    """Prove the live bootstrap and every mutation-sensitive seam."""
    failures = []
    if bootstrap_uv_findings(bootstrap, mode_test):
        failures.append("live uv bootstrap semantic contract failed")
    if not _registry_findings({}, PORTABLE_REGISTRY_DIGEST, "portable uv execution"):
        failures.append("collapsed portable uv execution registry passed")
    if not _registry_findings({}, MODE_EXECUTION_REGISTRY_DIGEST, "uv mode execution"):
        failures.append("collapsed uv mode execution registry passed")
    return [
        *failures,
        *_bootstrap_source_mutation_failures(bootstrap, mode_test),
        *_mode_mutation_failures(bootstrap, mode_test),
    ]


def _gate_mutation_failures(gate: str) -> list[str]:
    """Prove the runtime provisioner selftest cannot leave its real gate."""
    failures = []
    if gate_selftest_findings(gate):
        failures.append("live Python-authority uv selftest binding failed")
    line = (
        "  /bin/bash -p scripts/dev/provision_dev_box_toolchain.sh --selftest-uv-cache-contract\n"
    )
    mutated = _mutate_once(gate, line, "")
    if not gate_selftest_findings(mutated):
        failures.append("uv gate selftest mutation passed")
    return failures


def _task_copy_with_value(
    tasks: list[dict[str, object]], register: str, key: str, *, value: object
) -> list[dict[str, object]]:
    """Return a deep copy with one registered task field changed."""
    mutated = copy.deepcopy(tasks)
    task = _task_by_register(mutated, register)
    if task is None:
        message = f"selftest task is missing: {register}"
        raise ValueError(message)
    task[key] = value
    return mutated


def _task_copy_with_argv(
    tasks: list[dict[str, object]], register: str, argv: list[str]
) -> list[dict[str, object]]:
    """Return a deep copy with one registered command argv changed."""
    mutated = copy.deepcopy(tasks)
    task = _task_by_register(mutated, register)
    command = task.get("ansible.builtin.command") if task is not None else None
    if not isinstance(command, dict):
        message = f"selftest command is missing: {register}"
        raise TypeError(message)
    command["argv"] = argv
    return mutated


def _task_copy_named_with_value(
    tasks: list[dict[str, object]], name: str, key: str, *, value: object
) -> list[dict[str, object]]:
    """Return a deep copy with one display-named task field changed."""
    mutated = copy.deepcopy(tasks)
    task = _task_by_name(mutated, name)
    if task is None:
        message = f"selftest task is missing: {name}"
        raise ValueError(message)
    task[key] = value
    return mutated


def _ansible_mutation_documents(
    tasks: list[dict[str, object]],
) -> tuple[tuple[str, list[dict[str, object]]], ...]:
    """Return independent Ansible policy mutations."""
    apply_argv = ["/bin/bash", "-p", "scripts/dev/provision_dev_box_toolchain.sh"]
    return (
        (
            "drop-check",
            [
                task
                for task in copy.deepcopy(tasks)
                if task.get("register") != "dev_box_provision_check"
            ],
        ),
        (
            "check-ensure",
            _task_copy_with_argv(tasks, "dev_box_provision_check", apply_argv),
        ),
        (
            "check-skip",
            _task_copy_with_value(tasks, "dev_box_provision_check", "check_mode", value=None),
        ),
        (
            "check-privileged",
            _task_copy_with_value(tasks, "dev_box_provision_check", "become", value=True),
        ),
        (
            "changed-always",
            _task_copy_with_value(tasks, "dev_box_provision_check", "changed_when", value=True),
        ),
        (
            "failed-never",
            _task_copy_with_value(tasks, "dev_box_provision_check", "failed_when", value=False),
        ),
        (
            "apply-during-check",
            _task_copy_with_value(tasks, "dev_box_provision", "when", value="ansible_check_mode"),
        ),
        (
            "postcheck-unconditional",
            _task_copy_named_with_value(
                tasks,
                "Assert the provisioned libclang binding actually imports",
                "when",
                value=True,
            ),
        ),
        (
            "postcheck-inverted",
            _task_copy_named_with_value(
                tasks,
                "Assert the provisioned libclang binding actually imports",
                "when",
                value="ansible_check_mode",
            ),
        ),
    )


def _ansible_mutation_failures(document: object) -> list[str]:
    """Prove skip, argv, status, failure, and apply-boundary mutations fire."""
    failures = []
    if dev_box_uv_task_findings(document):
        failures.append("live dev-box uv Ansible contract failed")
    documents = _ansible_mutation_documents(_tasks(document))
    failures.extend(
        f"uv Ansible mutation passed: {label}"
        for label, mutated in documents
        if not dev_box_uv_task_findings(mutated)
    )
    return failures


def uv_cache_policy_selftest(root: Path) -> list[str]:
    """Prove each security, status, shell, and Ansible binding leg must fire."""
    bootstrap = (root / "scripts/dev/bootstrap_uv.py").read_text(encoding="utf-8")
    mode_test = (root / "scripts/dev/bootstrap_uv_mode_selftest.py").read_text(encoding="utf-8")
    provisioner = (root / "scripts/dev/provision_dev_box_toolchain.sh").read_text(encoding="utf-8")
    provisioner_selftest = (
        root / "scripts/dev/provision_dev_box_toolchain_selftest.bash"
    ).read_text(encoding="utf-8")
    gate = (root / "scripts/ci/gates/checks.sh").read_text(encoding="utf-8")
    transaction = yaml.safe_load(
        (root / "infra/ansible/roles/dev_box/tasks/transaction.yml").read_text(encoding="utf-8")
    )
    return [
        *_bootstrap_mutation_failures(bootstrap, mode_test),
        *provisioner_mutation_failures(provisioner, provisioner_selftest),
        *_gate_mutation_failures(gate),
        *_ansible_mutation_failures(transaction),
    ]
