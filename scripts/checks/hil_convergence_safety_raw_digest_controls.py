# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact load-bearing controls for the privileged raw-digest reader."""

from __future__ import annotations

import ast
from dataclasses import dataclass


@dataclass(frozen=True)
class ImplementationControl:
    """One exact control and its independently detectable mutation."""

    label: str
    function: str
    token: str
    replacement: str


def _flag_controls(
    function: str, scope: str, prefix: str, names: tuple[str, ...]
) -> tuple[ImplementationControl, ...]:
    """Return exact flag-use controls for one open boundary."""
    return tuple(
        ImplementationControl(f"{scope} {name}", function, f'| {prefix}["{name}"]', "| 0")
        for name in names
    )


def _metadata_read_controls() -> tuple[ImplementationControl, ...]:
    """Return exact identity, metadata, and bounded-read controls."""
    return (
        ImplementationControl(
            "ctime identity", "_identity", "value.st_ctime_ns,", "value.st_mtime_ns,"
        ),
        ImplementationControl(
            "intermediate directory mode",
            "_open_directory",
            "mode=_DIRECTORY_MODE,",
            "mode=stat.S_IMODE(before.st_mode),",
        ),
        ImplementationControl(
            "fixed read step",
            "_read_exact_payload",
            "requested = min(_READ_STEP_BYTES, before.st_size - offset)",
            "requested = before.st_size - offset",
        ),
        ImplementationControl(
            "root owner access",
            "_root_metadata_error",
            "(mode & _ROOT_MODE_REQUIRED) != _ROOT_MODE_REQUIRED",
            "False",
        ),
        ImplementationControl(
            "root forbidden mode bits",
            "_root_metadata_error",
            "mode & ~_ROOT_MODE_ALLOWED",
            "False",
        ),
        ImplementationControl(
            "single-link final file",
            "_metadata_error",
            "if not kind_ok or (policy.single_link and value.st_nlink != 1):",
            "if not kind_ok:",
        ),
        ImplementationControl(
            "exact pre-size EOF",
            "_read_exact_payload",
            'os.pread(fd, 1, before.st_size) != b""',
            "False",
        ),
    )


def _rewalk_dispatch_controls() -> tuple[ImplementationControl, ...]:
    """Return exact rewalk and production-dispatch controls."""
    return (
        ImplementationControl(
            "component post-rewalk identity",
            "_post_rewalk",
            "if identity != expected_directories[index]:",
            "if False:",
        ),
        ImplementationControl(
            "final post-rewalk identity",
            "_post_rewalk",
            "if _identity(after_path) != expected_file:",
            "if False:",
        ),
        ImplementationControl(
            "root post-rewalk identity",
            "audit_live_errors",
            "or _identity(before) != _identity(\n                after_path\n            )",
            "or False",
        ),
        ImplementationControl(
            "live target audit dispatch",
            "audit_live_errors",
            "errors.extend(_audit_all_targets(root_fd, values, context))",
            "errors.extend(())",
        ),
        ImplementationControl(
            "implementation audit dispatch",
            "source_errors",
            "errors = implementation_errors(authority_source)",
            "errors: list[str] = []",
        ),
        ImplementationControl(
            "control policy raw-pin dispatch",
            "source_errors",
            "control_error = _raw_target_error(_control_target(values), files, values)",
            "control_error = None",
        ),
        ImplementationControl(
            "control policy verification guard",
            "source_errors",
            "if control_error is not None:\n        return [control_error]",
            "if False:\n        return [control_error]",
        ),
        ImplementationControl(
            "control policy import",
            "<module>",
            "import hil_convergence_safety_raw_digest_controls as raw_digest_controls",
            "",
        ),
        ImplementationControl(
            "control policy error dispatch",
            "implementation_errors",
            "return raw_digest_controls.implementation_errors(source)",
            "return []",
        ),
    )


def _authority_condition_controls() -> tuple[ImplementationControl, ...]:
    """Return exact owner and mode conditions with fail-open mutations."""
    specs = (
        "root owner|_root_metadata_error|"
        "if authority is not None and value.st_uid != authority.root_uid:\n"
        "root group|_root_metadata_error|"
        "if authority is not None and value.st_gid != authority.root_gid:\n"
        "final owner|_metadata_error|if value.st_uid != policy.uid:\n"
        "final group|_metadata_error|if value.st_gid != policy.gid:\n"
        "final mode|_metadata_error|if stat.S_IMODE(value.st_mode) != policy.mode:"
    )
    return tuple(
        ImplementationControl(*line.split("|", 2), "if False:") for line in specs.splitlines()
    )


def _authority_cleanup_controls() -> tuple[ImplementationControl, ...]:
    """Return exact validation dispatch and descriptor controls."""
    return (
        *_authority_condition_controls(),
        ImplementationControl(
            "target path validation dispatch",
            "_raw_target_error",
            "path_error = _target_path_error(path, pin_name, label)",
            "path_error = None",
        ),
        ImplementationControl(
            "required capabilities dispatch",
            "audit_live_errors",
            "flags, error = _required_flags(hooks)",
            "flags, error = {}, None",
        ),
        ImplementationControl(
            "descriptor close operation",
            "_close_released_descriptor",
            "os.close(fd)",
            "None",
        ),
        ImplementationControl(
            "ledger release before close",
            "close_all",
            "fd = self._fds.pop()\n"
            "            errors.extend(_close_released_descriptor(fd, label, self._hooks))",
            "fd = self._fds[-1]\n"
            "            errors.extend(_close_released_descriptor(fd, label, self._hooks))",
        ),
    )


def controls() -> tuple[ImplementationControl, ...]:
    """Return every control whose removal must fire without relying on pins."""
    directory_flags = ("O_DIRECTORY", "O_NOFOLLOW", "O_CLOEXEC", "O_NONBLOCK")
    file_flags = ("O_NOFOLLOW", "O_CLOEXEC", "O_NONBLOCK")
    return (
        *_flag_controls("audit_live_errors", "root open", "flags", directory_flags),
        *_flag_controls("_open_directory", "component open", "context.flags", directory_flags),
        *_flag_controls("_open_final", "final open", "context.flags", file_flags),
        *_metadata_read_controls(),
        *_rewalk_dispatch_controls(),
        *_authority_cleanup_controls(),
    )


def _function_nodes(source: str) -> tuple[ast.Module | None, dict[str, ast.FunctionDef]]:
    """Parse uniquely named functions and methods without executing source."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return None, {}
    candidates: dict[str, list[ast.FunctionDef]] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef):
            candidates.setdefault(node.name, []).append(node)
    unique = {name: values[0] for name, values in candidates.items() if len(values) == 1}
    return tree, unique


def _diagnostic(control: ImplementationControl) -> str:
    """Return the stable exact diagnostic for one missing live control."""
    return f"raw digest implementation: required control is not unique: {control.label}"


def _segment(
    source: str,
    function: str,
    nodes: dict[str, ast.FunctionDef],
) -> str | None:
    """Return the exact function or module segment named by one control."""
    if function == "<module>":
        return source
    node = nodes.get(function)
    return None if node is None else ast.get_source_segment(source, node)


def implementation_errors(source: str) -> list[str]:
    """Reject removal or duplication of every load-bearing reader control."""
    tree, nodes = _function_nodes(source)
    if tree is None:
        return ["raw digest implementation: source is not valid Python"]
    return [
        _diagnostic(control)
        for control in controls()
        if (segment := _segment(source, control.function, nodes)) is None
        or segment.count(control.token) != 1
    ]


def implementation_mutations(source: str) -> tuple[tuple[str, str, str], ...]:
    """Return exact one-control mutants and their required diagnostics."""
    _tree, nodes = _function_nodes(source)
    mutations = []
    for control in controls():
        segment = _segment(source, control.function, nodes)
        if segment is None or segment.count(control.token) != 1:
            continue
        changed = source.replace(segment, segment.replace(control.token, control.replacement, 1), 1)
        mutations.append((control.label, changed, _diagnostic(control)))
    return tuple(mutations)
