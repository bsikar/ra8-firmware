# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate the WSL managed Ansible and Python environment boundary."""

from __future__ import annotations

import ast

from hil_convergence_safety_ast import assignment as _assignment
from hil_convergence_safety_ast import function as _function
from hil_convergence_safety_ast import module_assignment as _module_assignment
from hil_convergence_safety_ast import nested_assignment as _nested_assignment
from hil_convergence_safety_ast import return_strings as _return_strings


def _binary_errors(tree: ast.Module, render: ast.FunctionDef | None) -> list[str]:
    """Require the exact managed WSL playbook binary and rendered argv."""
    constants = {
        "WSL_MANAGED_ROOT": "/opt/ra8-python-tools",
        "WSL_MANAGED_CACHE": "/opt/ra8-python-tools-cache",
        "WSL_ANSIBLE_PLAYBOOK": "/opt/ra8-python-tools/bin/ansible-playbook",
        "WSL_SYSTEM_PYTHON": "/usr/bin/python3",
    }
    errors = []
    for name, wanted in constants.items():
        value = _module_assignment(tree, name)
        if not isinstance(value, ast.Constant) or value.value != wanted:
            errors.append(f"fleet WSL: {name} authority is not exact")
    specs = [
        node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "ConvergeSpec"
    ]
    field = None
    if len(specs) == 1:
        field = next(
            (
                node.value
                for node in specs[0].body
                if isinstance(node, ast.AnnAssign)
                and isinstance(node.target, ast.Name)
                and node.target.id == "ansible_playbook"
            ),
            None,
        )
    wanted_field = ast.parse("WSL_ANSIBLE_PLAYBOOK", mode="eval").body
    argv = _nested_assignment(render, "argv") if render is not None else None
    first = argv.elts[0] if isinstance(argv, ast.List) and argv.elts else None
    wanted_first = ast.parse("ansible_playbook", mode="eval").body
    if (
        field is None
        or ast.dump(field, include_attributes=False)
        != ast.dump(wanted_field, include_attributes=False)
        or first is None
        or ast.dump(first, include_attributes=False)
        != ast.dump(wanted_first, include_attributes=False)
    ):
        errors.append("fleet WSL: managed playbook executable binding is not exact")
    return errors


def _isolation_requirements() -> set[str]:
    """Return required WSL environment-isolation commands."""
    return {
        '  case "$name" in ANSIBLE_*) unset "$name" ;; esac',
        '  case "$name" in PYTHONHOME|PYTHONPATH|PYTHONNOUSERSITE) unset "$name" ;; esac',
        '  case "$name" in UV_*) unset "$name" ;; esac',
        "export PYTHONNOUSERSITE=1",
    }


def _path_proof_requirements() -> set[str]:
    """Return required managed-path identity and publication commands."""
    return {
        '  [ -d "$1" ] && [ ! -L "$1" ] && [ "$(readlink -f -- "$1")" = "$1" ] || {',
        '  [ -f "$1" ] && [ ! -L "$1" ] && [ "$(readlink -f -- "$1")" = "$1" ] || {',
        'require_real_dir "$(dirname "$managed_root")"',
        'require_real_dir "$(dirname "$managed_cache")"',
        '  ! /usr/bin/mountpoint -q -- "$1" || {',
        "require_exact_file() {",
        '  [ "$(stat -c %a -- "$1")" = "$2" ] || {',
        '  file_digest="$(sha256sum -- "$1")"',
        '  [ "${file_digest%% *}" = "$3" ] || {',
        "/scripts/dev/bootstrap_uv.py",
        "/scripts/dev/bootstrap_uv_exec.py",
        " 755 ",
        " 644 ",
        "sync_file() {",
        "sync_dir() {",
    }


def _sync_requirements() -> set[str]:
    """Return required locked uv synchronization commands."""
    return {
        "uv_run() {",
        ' --run "$@"',
        'if [ "$mode" = apply ]; then',
        '    install -d -m 0755 -- "$managed_root"',
        '    install -d -m 0755 -- "$managed_cache"',
        '    refuse_mount "$managed_root"',
        '    refuse_mount "$managed_cache"',
        '  UV_PROJECT_ENVIRONMENT="$managed_root" UV_PYTHON_DOWNLOADS=never '
        'UV_CACHE_DIR="$managed_cache" uv_run ',
        '  UV_PROJECT_ENVIRONMENT="$managed_root" UV_PYTHON_DOWNLOADS=never '
        'UV_CACHE_DIR="$managed_cache" uv_run --offline --no-cache ',
        " --check",
    }


def _verify_requirements() -> set[str]:
    """Return required managed-environment verification commands."""
    return {
        'require_real_file "$managed_root/bin/python3"',
        'require_real_file "$managed_root/bin/ansible-galaxy"',
        '  mv -f -- "$marker" "$managed_root/.ra8-infra-lock.sha256"',
        '  sync_file "$managed_root/.ra8-infra-lock.sha256"',
        '  sync_dir "$managed_root"',
        'export ANSIBLE_CONFIG="$PWD/ansible.cfg"',
        'export ANSIBLE_COLLECTIONS_PATH="$PWD/../../.ansible/collections"',
        "export ANSIBLE_COLLECTIONS_SCAN_SYS_PATH=false",
    }


def _required_strings(tree: ast.Module) -> bool:
    """Return whether executable render helpers contain every safety decision."""
    requirements = (
        (_function(tree, "_isolation_lines"), _isolation_requirements()),
        (_function(tree, "_path_proof_lines"), _path_proof_requirements()),
        (_function(tree, "_toolchain_sync_lines"), _sync_requirements()),
        (_function(tree, "_toolchain_verify_lines"), _verify_requirements()),
    )
    return all(wanted <= _return_strings(function) for function, wanted in requirements)


def environment_errors(tree: ast.Module) -> list[str]:
    """Require rendered WSL commands to scrub and bind environment controls."""
    render = _function(tree, "render_converge")
    combined = _function(tree, "_ansible_environment_lines")
    runner = _function(tree, "_run_script")
    environment = _assignment(runner, "env") if runner is not None else None
    env_keys = (
        {key.value for key in environment.keys if isinstance(key, ast.Constant)}
        if isinstance(environment, ast.Dict)
        else set()
    )
    hostile = {"ANSIBLE_CONFIG", "ANSIBLE_ROLES_PATH", "PYTHONHOME", "PYTHONPATH"}
    render_lines = _assignment(render, "lines") if render is not None else None
    expected = ast.parse("_ansible_environment_lines(spec)", mode="eval").body
    combined_return = (
        next((node.value for node in combined.body if isinstance(node, ast.Return)), None)
        if combined is not None
        else None
    )
    composition = ast.parse(
        "[*_isolation_lines(), "
        "*_path_proof_lines(spec.stage, spec.managed_root, spec.managed_cache), "
        "*_toolchain_sync_lines(spec.stage, spec.mode, spec.system_python), "
        "*_toolchain_verify_lines(spec.stage, spec.ansible_playbook)]",
        mode="eval",
    ).body
    sync = _function(tree, "_toolchain_sync_lines")
    sync_strings = _return_strings(sync)
    verify = _function(tree, "_toolchain_verify_lines")
    verify_strings = _return_strings(verify)
    sync_flags = _assignment(sync, "sync_flags") if sync is not None else None
    wanted_flags = ast.parse(
        'f"--no-config --directory {shlex.quote(stage)} sync --locked "'
        'f"--only-group infra --no-install-project --python {shlex.quote(system_python)}"',
        mode="eval",
    ).body
    errors = _binary_errors(tree, render)
    if (
        render_lines is None
        or ast.dump(render_lines, include_attributes=False)
        != ast.dump(expected, include_attributes=False)
        or combined_return is None
        or ast.dump(combined_return, include_attributes=False)
        != ast.dump(composition, include_attributes=False)
        or not _required_strings(tree)
        or any(
            "uv_bin" in value or "--verify-cache" in value or "|| true" in value
            for value in sync_strings | verify_strings
        )
        or not any("uv_run --no-config --directory " in value for value in verify_strings)
        or not any(
            " export --locked --offline --only-group infra " in value for value in verify_strings
        )
        or sync_flags is None
        or ast.dump(sync_flags, include_attributes=False)
        != ast.dump(wanted_flags, include_attributes=False)
        or not hostile <= env_keys
    ):
        errors.append("fleet WSL: rendered managed environment boundary is not exact")
    return errors
