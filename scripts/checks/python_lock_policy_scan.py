# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""First-party dependency-authority, installer, and CLI-consumer scanners."""

from __future__ import annotations

import ast
import copy
import os
import re
import shlex
import subprocess
import sys
from collections.abc import Callable, Mapping
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))

from git_environment import isolated_git_environment, trusted_git_executable
from hil_convergence_safety_policy import load_bench_transaction
from python_lock_policy_process import (
    forbidden_argv,
    is_process_call,
    literal_bindings,
    literal_command_words,
    literal_string,
    process_aliases,
    process_command_argument,
    propagate_member_aliases,
    shell_installer_label,
)

GALAXY_MANIFEST = Path("infra/ansible/requirements.yml")
DERIVED_EXPORTS = {
    Path("infra/ansible/roles/k3s_node/files/requirements.lock"),
    Path("infra/ansible/roles/hil_bench/files/requirements.lock"),
}
VENDOR_BOUNDARIES = (
    Path("libs/third_party/mbedtls"),
    Path("libs/third_party/nimble"),
)
SECONDARY_AUTHORITY_NAMES = {
    "Pipfile",
    "Pipfile.lock",
    "poetry.lock",
    "setup.cfg",
    "setup.py",
}
IGNORED_SCAN_PARTS = {
    ".ansible",
    ".git",
    ".tools",
    ".venv",
    "__pycache__",
    "_deps",
}
BUILD_BOUNDARIES = (
    Path("build"),
    Path("docs/build"),
    Path("tests/build"),
    Path("tests/build-cov"),
    Path("tests/build-fuzz"),
    Path("tests/build-ubsan"),
)
MIN_UV_ARGV_SIZE = 2
HIL_UV_AUTH_ARGV = (
    "/usr/bin/python3",
    "{{ hil_bench_python_context }}/bootstrap_uv.py",
    "--verify-cache",
    "--manifest",
    "{{ hil_bench_python_context }}/uv_release.json",
    "--cache-root",
    "{{ hil_bench_uv_cache }}",
)
HIL_UV_PROBE_ARGV = (
    "/usr/bin/python3",
    "{{ hil_bench_python_context }}/bootstrap_uv.py",
    "--manifest",
    "{{ hil_bench_python_context }}/uv_release.json",
    "--cache-root",
    "{{ hil_bench_uv_cache }}",
    "--run",
    "--no-config",
    "pip",
    "check",
    "--python",
    "{{ hil_bench_python_venv }}/bin/python3",
)
HIL_UV_SYNC_ARGV = (
    "/usr/bin/python3",
    "{{ hil_bench_python_context }}/bootstrap_uv.py",
    "--manifest",
    "{{ hil_bench_python_context }}/uv_release.json",
    "--cache-root",
    "{{ hil_bench_uv_cache }}",
    "--ensure-and-run",
    "--directory",
    "{{ hil_bench_python_context }}",
    "--no-config",
    "sync",
    "--locked",
    "--only-group",
    "hil",
    "--no-install-project",
    "--python",
    "/usr/bin/python3",
)
CLI_DISTRIBUTIONS = {
    "ansible-playbook": "ansible-core",
    "cmake-format": "cmakelang",
    "cmake-lint": "cmakelang",
    "gcovr": "gcovr",
    "ruff": "ruff",
    "vela": "ethos-u-vela",
    "yamllint": "yamllint",
}


def git_executable() -> str:
    """Return the absolute Git authority required for index enumeration."""
    return trusted_git_executable()


def _ansible_tasks(document: object) -> list[Mapping[str, object]]:
    """Flatten task records, including block, rescue, and always sections."""
    if not isinstance(document, list):
        return []
    tasks: list[Mapping[str, object]] = []
    for item in document:
        if not isinstance(item, Mapping):
            continue
        tasks.append(item)
        for section in ("block", "rescue", "always"):
            tasks.extend(_ansible_tasks(item.get(section)))
    return tasks


def _task_argv(task: Mapping[str, object]) -> tuple[str, ...]:
    """Return one Ansible command task's literal argv, or an empty tuple."""
    command = task.get("ansible.builtin.command")
    if not isinstance(command, Mapping):
        return ()
    argv = command.get("argv")
    if not isinstance(argv, list) or any(not isinstance(word, str) for word in argv):
        return ()
    return tuple(argv)


def _exact_argv(actual: tuple[str, ...], expected: object) -> bool:
    """Match only immutable argv authorities with identical elements and order."""
    return isinstance(expected, tuple) and actual == expected


def _has_sequence(words: tuple[str, ...], sequence: tuple[str, ...]) -> bool:
    """Return whether one exact contiguous argv sequence occurs."""
    return any(words[index : index + len(sequence)] == sequence for index in range(len(words)))


def _registered_task(
    tasks: list[Mapping[str, object]], register: str
) -> tuple[Mapping[str, object] | None, list[str]]:
    """Return the unique task owning a register, reporting missing or duplicates."""
    matches = [task for task in tasks if task.get("register") == register]
    if len(matches) != 1:
        return None, [f"HIL Python policy needs one {register} task; found {len(matches)}"]
    return matches[0], []


def _named_task(
    tasks: list[Mapping[str, object]], name: str
) -> tuple[Mapping[str, object] | None, list[str]]:
    """Return the unique task carrying one exact display name."""
    matches = [task for task in tasks if task.get("name") == name]
    if len(matches) != 1:
        return None, [f"HIL Python policy needs one {name!r} task; found {len(matches)}"]
    return matches[0], []


def _uv_auth_task_findings(task: Mapping[str, object]) -> list[str]:
    """Validate the cached-uv authentication preflight task."""
    findings: list[str] = []
    argv = _task_argv(task)
    if not _exact_argv(argv, HIL_UV_AUTH_ARGV):
        findings.append("HIL uv preflight does not authenticate the pinned cached release")
    if (
        task.get("changed_when") is not False
        or task.get("failed_when") is not False
        or task.get("check_mode") is not False
        or "ignore_errors" in task
    ):
        findings.append("HIL uv authentication preflight is not read-only/fail-observable")
    return findings


def _uv_probe_task_findings(task: Mapping[str, object]) -> list[str]:
    """Validate the dependency probe executes only through authenticated bytes."""
    findings: list[str] = []
    argv = _task_argv(task)
    if not _exact_argv(argv, HIL_UV_PROBE_ARGV):
        findings.append("HIL dependency preflight bypasses authenticated uv execution")
    environment = task.get("environment")
    if environment != {"UV_PYTHON_DOWNLOADS": "never"}:
        findings.append("HIL dependency preflight permits uv Python downloads")
    if (
        task.get("changed_when") is not False
        or task.get("failed_when") is not False
        or task.get("check_mode") is not False
        or "ignore_errors" in task
    ):
        findings.append("HIL dependency preflight is not read-only/fail-observable")
    return findings


def _uv_apply_task_findings(tasks: list[Mapping[str, object]]) -> list[str]:
    """Require HIL sync and final pip check to stay behind the bootstrap runner."""
    findings: list[str] = []
    specifications = (
        (
            "Synchronize the exact uv-locked HIL dependency group",
            HIL_UV_SYNC_ARGV,
            {
                "UV_PROJECT_ENVIRONMENT": "{{ hil_bench_python_venv }}",
                "UV_PYTHON_DOWNLOADS": "never",
            },
        ),
        (
            "Check the HIL Python dependency graph",
            HIL_UV_PROBE_ARGV,
            {"UV_PYTHON_DOWNLOADS": "never"},
        ),
    )
    for name, expected_argv, expected_environment in specifications:
        task, errors = _named_task(tasks, name)
        findings.extend(errors)
        if task is None:
            continue
        argv = _task_argv(task)
        if not _exact_argv(argv, expected_argv):
            findings.append(f"HIL task {name!r} bypasses authenticated uv execution")
        environment = task.get("environment")
        if environment != expected_environment:
            findings.append(f"HIL task {name!r} permits uv Python downloads")
        if name.startswith("Synchronize"):
            if task.get("changed_when") != "hil_bench_python_sync.rc == 0":
                findings.append(f"HIL task {name!r} masks authenticated uv status")
        elif task.get("changed_when") is not False:
            findings.append(f"HIL task {name!r} masks authenticated uv status")
        if "failed_when" in task or "ignore_errors" in task:
            findings.append(f"HIL task {name!r} masks authenticated uv status")
    return findings


def _rebuild_decision_findings(tasks: list[Mapping[str, object]]) -> list[str]:
    """Require cached-uv and dependency-probe results to drive HIL rebuilding."""
    rebuilds = []
    for task in tasks:
        facts = task.get("ansible.builtin.set_fact")
        if isinstance(facts, Mapping) and "hil_bench_python_rebuild" in facts:
            rebuilds.append(facts)
    if len(rebuilds) != 1:
        return [f"HIL Python policy needs one rebuild decision; found {len(rebuilds)}"]
    expression = str(rebuilds[0]["hil_bench_python_rebuild"])
    return [
        f"HIL rebuild decision ignores {register}"
        for register in ("hil_bench_uv_preflight.rc", "hil_bench_uv_pip_probe.rc")
        if register not in expression
    ]


def hil_preflight_findings(document: object) -> list[str]:
    """Require the HIL idempotency preflight to use authenticated pinned uv."""
    tasks = _ansible_tasks(document)
    findings = [] if tasks else ["HIL bench task document has no tasks"]
    for task in tasks:
        argv = _task_argv(task)
        if (
            argv
            and "hil_bench_python_venv" in argv[0]
            and _has_sequence(argv, ("-m", "pip", "check"))
        ):
            findings.append("HIL preflight calls python -m pip in a uv-created environment")
    auth, errors = _registered_task(tasks, "hil_bench_uv_preflight")
    findings.extend(errors)
    if auth is not None:
        findings.extend(_uv_auth_task_findings(auth))
    probe, errors = _registered_task(tasks, "hil_bench_uv_pip_probe")
    findings.extend(errors)
    if probe is not None:
        findings.extend(_uv_probe_task_findings(probe))
    findings.extend(_uv_apply_task_findings(tasks))
    findings.extend(_rebuild_decision_findings(tasks))
    return findings


def load_hil_tasks(root: Path) -> object:
    """Follow the exact public role entry to its authoritative transaction."""
    return load_bench_transaction(root)


def _runner_removal_findings(document: object) -> list[str]:
    """Prove preflight and convergence cannot omit bootstrap runner modes."""
    failures: list[str] = []
    cases = (
        (
            "hil_bench_uv_pip_probe",
            "",
            "--run",
            "HIL preflight without bootstrap --run passed",
        ),
        (
            "",
            "Synchronize the exact uv-locked HIL dependency group",
            "--ensure-and-run",
            "HIL apply sync without bootstrap runner passed",
        ),
    )
    for register, name, token, message in cases:
        mutated = copy.deepcopy(document)
        tasks = _ansible_tasks(mutated)
        task, _ = _registered_task(tasks, register) if register else _named_task(tasks, name)
        command = task.get("ansible.builtin.command") if isinstance(task, dict) else None
        argv = command.get("argv") if isinstance(command, dict) else None
        if not isinstance(argv, list) or token not in argv:
            failures.append(f"could not mutate {message.lower()}")
            continue
        argv.remove(token)
        if not any("bypasses authenticated" in item for item in hil_preflight_findings(mutated)):
            failures.append(message)
    return failures


def _runner_shape_findings(document: object) -> list[str]:
    """Prove exact argv ordering and child-status propagation are load-bearing."""
    failures: list[str] = []
    selectors = (
        ("hil_bench_uv_preflight", ""),
        ("hil_bench_uv_pip_probe", ""),
        ("", "Synchronize the exact uv-locked HIL dependency group"),
        ("", "Check the HIL Python dependency graph"),
    )
    for register, name in selectors:
        for attack in ("raw", "reorder", "mask"):
            mutated = copy.deepcopy(document)
            tasks = _ansible_tasks(mutated)
            task, _ = _registered_task(tasks, register) if register else _named_task(tasks, name)
            if not isinstance(task, dict):
                failures.append(f"could not select HIL uv task {register or name!r}")
                continue
            command = task.get("ansible.builtin.command")
            argv = command.get("argv") if isinstance(command, dict) else None
            if not isinstance(argv, list) or len(argv) < MIN_UV_ARGV_SIZE:
                failures.append(f"could not mutate HIL uv task {register or name!r}")
                continue
            if attack == "raw":
                argv[0] = "/opt/ra8-uv-cache/uv"
            elif attack == "reorder":
                argv[-2], argv[-1] = argv[-1], argv[-2]
            else:
                task["ignore_errors"] = True
            if not hil_preflight_findings(mutated):
                failures.append(f"HIL uv {attack} mutation passed: {register or name}")
    return failures


def _argv_type_contract_findings(document: object) -> list[str]:
    """Bind immutable expected argv authorities to the real HIL task parser."""
    tasks = _ansible_tasks(document)
    auth, _ = _registered_task(tasks, "hil_bench_uv_preflight")
    probe, _ = _registered_task(tasks, "hil_bench_uv_pip_probe")
    sync, _ = _named_task(tasks, "Synchronize the exact uv-locked HIL dependency group")
    final, _ = _named_task(tasks, "Check the HIL Python dependency graph")
    cases = (
        ("authentication", auth, HIL_UV_AUTH_ARGV),
        ("dependency probe", probe, HIL_UV_PROBE_ARGV),
        ("synchronization", sync, HIL_UV_SYNC_ARGV),
        ("final dependency check", final, HIL_UV_PROBE_ARGV),
    )
    failures: list[str] = []
    for label, task, expected in cases:
        if task is None or not _exact_argv(_task_argv(task), expected):
            failures.append(f"real HIL {label} argv violates the immutable type contract")
        if _exact_argv(_task_argv(task or {}), list(expected)):
            failures.append(f"mutable-list HIL {label} argv authority passed")
    return failures


def hil_preflight_selftest(root: Path) -> list[str]:
    """Prove valid HIL tasks pass and unauthenticated or pip-based probes fail."""
    document = load_hil_tasks(root)
    failures = ["live HIL uv preflight policy failed"] if hil_preflight_findings(document) else []
    failures.extend(_runner_removal_findings(document))
    failures.extend(_runner_shape_findings(document))
    failures.extend(_argv_type_contract_findings(document))
    raw_pip = copy.deepcopy(document)
    probe, _ = _registered_task(_ansible_tasks(raw_pip), "hil_bench_uv_pip_probe")
    if not isinstance(probe, dict):
        failures.append("could not mutate HIL dependency probe fixture")
    else:
        command = probe.get("ansible.builtin.command")
        if isinstance(command, dict):
            command["argv"] = ["{{ hil_bench_python_venv }}/bin/python3", "-m", "pip", "check"]
        if not any("python -m pip" in item for item in hil_preflight_findings(raw_pip)):
            failures.append("python -m pip HIL preflight passed")
    unverified = copy.deepcopy(document)
    auth, _ = _registered_task(_ansible_tasks(unverified), "hil_bench_uv_preflight")
    if not isinstance(auth, dict):
        failures.append("could not mutate HIL uv authentication fixture")
    else:
        command = auth.get("ansible.builtin.command")
        argv = command.get("argv") if isinstance(command, dict) else None
        if isinstance(argv, list):
            argv.remove("--verify-cache")
        if not any("authenticate" in item for item in hil_preflight_findings(unverified)):
            failures.append("unauthenticated cached uv preflight passed")
    unbound = copy.deepcopy(document)
    for task in _ansible_tasks(unbound):
        facts = task.get("ansible.builtin.set_fact")
        if isinstance(facts, dict) and "hil_bench_python_rebuild" in facts:
            facts["hil_bench_python_rebuild"] = "{{ false }}"
    if not any("rebuild decision ignores" in item for item in hil_preflight_findings(unbound)):
        failures.append("unbound HIL preflight result passed")
    return failures


def repository_policy_paths(root: Path) -> list[Path]:
    """Return tracked policy inputs, or all files for synthetic non-Git fixtures."""
    if not (root / ".git").exists():
        return sorted(path for path in root.rglob("*") if path.is_file())
    result = subprocess.run(  # noqa: S603 -- absolute executable, fixed argv, no shell
        [git_executable(), "ls-files", "--cached", "-z", "--", "."],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        message = f"cannot enumerate tracked policy inputs: {detail}"
        raise OSError(message)
    relatives = [os.fsdecode(item) for item in result.stdout.split(b"\0") if item]
    return [root / relative for relative in relatives if (root / relative).is_file()]


def read_authored_text(path: Path, subject: str) -> tuple[str | None, str | None]:
    """Read authored UTF-8 text or return one path-specific policy finding."""
    try:
        return path.read_text(encoding="utf-8"), None
    except UnicodeError as error:
        return None, f"{path}: {subject} is not valid UTF-8: {error}"
    except OSError as error:
        return None, f"{path}: cannot read {subject}: {error}"


def python_source_paths(root: Path) -> list[Path]:
    """Return tracked first-party Python sources outside generated/vendor trees."""
    paths: list[Path] = []
    for path in repository_policy_paths(root):
        if path.suffix != ".py" or ignored_policy_path(root, path):
            continue
        relative = path.relative_to(root)
        if "third_party" in relative.parts or relative.is_relative_to(Path("libs/ra8_fonts")):
            continue
        paths.append(path)
    return sorted(paths)


def first_party_import_closure(
    root: Path,
    initial: list[Path],
    imported_roots: Callable[[Path], tuple[set[str], list[str]]],
) -> tuple[list[Path], list[str]]:
    """Include adjacent authored modules and recursively inspect their imports."""
    sources = set(initial)
    pending = list(initial)
    errors: list[str] = []
    while pending:
        source = pending.pop()
        imported, import_errors = imported_roots(source)
        errors.extend(import_errors)
        for name in imported:
            candidates = (
                source.parent / f"{name}.py",
                source.parent / name / "__init__.py",
            )
            for candidate in candidates:
                if candidate in sources or not candidate.is_file() or candidate.is_symlink():
                    continue
                try:
                    relative = candidate.resolve(strict=True).relative_to(root.resolve(strict=True))
                except (OSError, ValueError):
                    continue
                if ignored_policy_path(root, root / relative):
                    continue
                text, error = read_authored_text(candidate, "adjacent Python module")
                if error is not None:
                    errors.append(error)
                    continue
                header = "\n".join((text or "").splitlines()[:5])
                if "SPDX-License-Identifier:" not in header or "Copyright" not in header:
                    continue
                sources.add(candidate)
                pending.append(candidate)
    return sorted(sources), errors


def adjacent_import_closure_selftest(
    root: Path,
    source: Path,
    imported_roots: Callable[[Path], tuple[set[str], list[str]]],
) -> list[str]:
    """Prove an authored adjacent module recursively enters the import census."""
    source.write_text(
        "# SPDX-License-Identifier: MIT\n# Copyright (c) 2026 Test\nimport adjacent\n",
        encoding="utf-8",
    )
    adjacent = root / "adjacent.py"
    adjacent.write_text(
        "# SPDX-License-Identifier: MIT\n# Copyright (c) 2026 Test\nimport rogue_external\n",
        encoding="utf-8",
    )
    closure, errors = first_party_import_closure(root, [source], imported_roots)
    discovered = set().union(*(imported_roots(path)[0] for path in closure))
    if errors or adjacent not in closure or "rogue_external" not in discovered:
        return ["adjacent authored-module import closure was not scanned recursively"]
    return []


def ignored_policy_path(root: Path, path: Path) -> bool:
    """Return whether a path is generated, cached, or outside first-party policy."""
    relative = path.relative_to(root)
    return any(part in IGNORED_SCAN_PARTS for part in relative.parts) or any(
        relative == boundary or relative.is_relative_to(boundary) for boundary in BUILD_BOUNDARIES
    )


def is_dependency_authority_candidate(root: Path, path: Path) -> bool:
    """Recognize dependency metadata without treating arbitrary text/data as a lock."""
    if path.name in SECONDARY_AUTHORITY_NAMES or path.name in {
        "pyproject.toml",
        "uv.lock",
    }:
        return True
    if re.fullmatch(
        r"(?:requirements|constraints)(?:[._-][^.]+)*\.(?:in|lock|txt)",
        path.name,
    ):
        return True
    relative = path.relative_to(root)
    authority_directories = {"constraints", "requirements"}
    return any(part in authority_directories for part in relative.parts[:-1]) and path.suffix in {
        ".in",
        ".lock",
        ".txt",
    }


def requirement_findings(root: Path) -> list[str]:
    """Reject secondary first-party dependency authorities while preserving vendors."""
    allowed = {
        root / GALAXY_MANIFEST,
        *(root / relative for relative in DERIVED_EXPORTS),
    }
    findings: list[str] = []
    candidates = [
        path
        for path in repository_policy_paths(root)
        if path.is_file()
        and not ignored_policy_path(root, path)
        and is_dependency_authority_candidate(root, path)
    ]
    for path in candidates:
        if path in allowed or path in {root / "pyproject.toml", root / "uv.lock"}:
            continue
        relative = path.relative_to(root)
        if any(relative.is_relative_to(boundary) for boundary in VENDOR_BOUNDARIES):
            continue
        findings.append(f"stale first-party dependency authority: {relative}")
    return sorted(findings)


def python_installer_findings(path: Path, root: Path) -> list[str]:
    """Inspect Python process-launch APIs for literal package installers."""
    source, error = read_authored_text(path, "Python installer policy input")
    if error is not None:
        return [error]
    try:
        tree = ast.parse(source or "", filename=str(path))
    except SyntaxError as error:
        return [f"{path.relative_to(root)}: cannot inspect process calls: {error}"]
    findings: list[str] = []
    aliases = process_aliases(tree)
    bindings = literal_bindings(tree)
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not is_process_call(node.func, aliases):
            continue
        argument = process_command_argument(node)
        if argument is None:
            continue
        words = literal_command_words(argument, aliases, bindings)
        label = forbidden_argv(words or [])
        shell_text = literal_string(argument, bindings)
        if label is None and shell_text is not None:
            label = shell_installer_label(shell_text)
        if label is None and words is not None:
            label = next((shell_installer_label(word) for word in words if word), None)
        if label is not None:
            findings.append(
                f"{path.relative_to(root)}:{node.lineno}: forbidden {label} process call"
            )
    return findings


def unsafe_install_findings(root: Path) -> list[str]:
    """Reject raw Python provisioning outside the locked uv/Ansible boundaries."""
    findings: list[str] = []
    suffixes = {".bash", ".bat", ".cmd", ".ps1", ".sh", ".yaml", ".yml", ".zsh"}
    for path in repository_policy_paths(root):
        if (
            not path.is_file()
            or ignored_policy_path(root, path)
            or "third_party" in path.relative_to(root).parts
        ):
            continue
        if path.suffix == ".py":
            findings.extend(python_installer_findings(path, root))
            continue
        if path.suffix not in suffixes and path.name not in {"Dockerfile", "justfile"}:
            continue
        source, error = read_authored_text(path, "installer policy input")
        if error is not None:
            findings.append(error)
            continue
        label = shell_installer_label(source or "")
        if label is not None:
            findings.append(f"{path.relative_to(root)}: forbidden {label}")
    return sorted(findings)


def command_distribution(token: str) -> str | None:
    """Map one literal executable token to its owning Python distribution."""
    command = token.strip("();|&").replace("\\", "/").rsplit("/", maxsplit=1)[-1]
    return CLI_DISTRIBUTIONS.get(command)


def shutil_which_aliases(
    tree: ast.AST, bindings: Mapping[str, ast.AST]
) -> tuple[set[str], set[str]]:
    """Return imported shutil module and which-function aliases."""
    modules: set[str] = set()
    functions: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(
                alias.asname or alias.name for alias in node.names if alias.name == "shutil"
            )
        elif isinstance(node, ast.ImportFrom) and node.module == "shutil":
            functions.update(
                alias.asname or alias.name for alias in node.names if alias.name == "which"
            )
    propagate_member_aliases(bindings, modules, functions, "which")
    return modules, functions


def python_cli_consumers(path: Path) -> tuple[set[str], list[str]]:
    """Discover Python CLI consumers and report malformed authored inputs."""
    source, error = read_authored_text(path, "Python CLI policy input")
    if error is not None:
        return set(), [error]
    try:
        tree = ast.parse(source or "", filename=str(path))
    except SyntaxError as error:
        return set(), [f"{path}: cannot inspect Python CLI calls: {error}"]
    aliases = process_aliases(tree)
    bindings = literal_bindings(tree)
    shutil_modules, which_functions = shutil_which_aliases(tree, bindings)

    consumers: set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        function = node.func
        is_which = (isinstance(function, ast.Name) and function.id in which_functions) or (
            isinstance(function, ast.Attribute)
            and isinstance(function.value, ast.Name)
            and function.value.id in shutil_modules
            and function.attr == "which"
        )
        if is_which and node.args:
            command = literal_string(node.args[0], bindings)
            if command is not None:
                package = command_distribution(command)
                if package is not None:
                    consumers.add(package)
        if is_process_call(function, aliases):
            argument = process_command_argument(node)
            words = (
                literal_command_words(argument, aliases, bindings) if argument is not None else None
            )
            if words:
                package = command_distribution(words[0])
                if package is not None:
                    consumers.add(package)
    return consumers, []


def discover_cli_consumers(root: Path) -> tuple[set[str], list[str]]:
    """Discover live CLI use independently from direct-pin and proof registries."""
    consumers: set[str] = set()
    findings: list[str] = []
    shell_suffixes = {".bash", ".cmd", ".just", ".ps1", ".sh", ".yaml", ".yml", ".zsh"}
    for path in repository_policy_paths(root):
        if (
            not path.is_file()
            or ignored_policy_path(root, path)
            or "third_party" in path.relative_to(root).parts
        ):
            continue
        if path.suffix == ".py":
            discovered, errors = python_cli_consumers(path)
            consumers.update(discovered)
            findings.extend(errors)
            continue
        if path.suffix not in shell_suffixes and path.name not in {
            "Dockerfile",
            "justfile",
        }:
            continue
        source, error = read_authored_text(path, "CLI policy input")
        if error is not None:
            findings.append(error)
            continue
        for line in (source or "").splitlines():
            try:
                tokens = shlex.split(line, comments=True, posix=True)
            except ValueError:
                continue
            consumers.update(
                package
                for package in (command_distribution(token) for token in tokens)
                if package is not None
            )
    return consumers, findings


def scanner_selection_selftest(root: Path) -> list[str]:
    """Prove tracked text is scanned while ignored artifacts stay out of scope."""
    with isolated_git_environment():
        return scanner_selection_cases(root)


def scanner_selection_cases(root: Path) -> list[str]:
    """Exercise Git-index selection and UTF-8 findings in an isolated fixture."""
    failures: list[str] = []
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(  # noqa: S603 -- absolute executable, fixed selftest argv, no shell
        [git_executable(), "init", "-q"], cwd=root, check=True
    )
    (root / ".gitignore").write_text("._*\n", encoding="ascii")
    (root / "valid.py").write_text("import subprocess\n", encoding="utf-8")
    (root / "invalid.py").write_bytes(b"# bad utf-8: \xa3\n")
    (root / "._ignored.py").write_bytes(b"# ignored artifact: \xa3\n")
    subprocess.run(  # noqa: S603 -- absolute executable, fixed selftest argv, no shell
        [git_executable(), "add", "--", ".gitignore", "valid.py", "invalid.py"],
        cwd=root,
        check=True,
    )
    names = {path.name for path in repository_policy_paths(root)}
    if names != {".gitignore", "invalid.py", "valid.py"}:
        failures.append(f"tracked policy enumeration mismatch: {sorted(names)}")
    valid, valid_error = read_authored_text(root / "valid.py", "fixture")
    if valid is None or valid_error is not None:
        failures.append("valid tracked UTF-8 input failed")
    invalid, invalid_error = read_authored_text(root / "invalid.py", "fixture")
    if invalid is not None or invalid_error is None or "not valid UTF-8" not in invalid_error:
        failures.append("tracked non-UTF-8 input did not fail clearly")
    if any("._ignored.py" in item for item in unsafe_install_findings(root)):
        failures.append("ignored AppleDouble artifact entered the installer scan")
    _, cli_errors = discover_cli_consumers(root)
    if not any("invalid.py" in item and "not valid UTF-8" in item for item in cli_errors):
        failures.append("tracked non-UTF-8 CLI input did not produce a finding")
    return failures


def cli_consumer_findings(root: Path, pins: Mapping[str, str]) -> list[str]:
    """Require every live locked CLI consumer to retain an exact direct pin."""
    discovered, findings = discover_cli_consumers(root)
    missing = set(CLI_DISTRIBUTIONS.values()) - discovered
    if missing:
        findings.append(f"CLI consumer census missed live package(s): {sorted(missing)}")
    findings.extend(
        f"CLI dependency {package} is invoked but not directly pinned"
        for package in sorted(discovered - set(pins))
    )
    return findings
