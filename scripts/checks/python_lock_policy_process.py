# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Conservative AST and process-launcher parsing for Python lock policy."""

from __future__ import annotations

import ast
import re
from collections.abc import Mapping
from dataclasses import dataclass


@dataclass(frozen=True)
class ProcessAliases:
    """Record imported process launchers and Python-interpreter aliases."""

    modules: Mapping[str, str]
    functions: Mapping[str, str]
    sys_modules: frozenset[str]
    sys_executables: frozenset[str]


def propagate_member_aliases(
    bindings: Mapping[str, ast.AST],
    modules: set[str],
    functions: set[str],
    member: str,
) -> None:
    """Extend one imported module/member pair through unique assignments."""
    unresolved = dict(bindings)
    while unresolved:
        resolved: list[str] = []
        for name, value in unresolved.items():
            if isinstance(value, ast.Name) and value.id in modules:
                modules.add(name)
            elif (isinstance(value, ast.Name) and value.id in functions) or (
                isinstance(value, ast.Attribute)
                and isinstance(value.value, ast.Name)
                and value.value.id in modules
                and value.attr == member
            ):
                functions.add(name)
            else:
                continue
            resolved.append(name)
        if not resolved:
            return
        for name in resolved:
            del unresolved[name]


def assigned_process_alias(
    value: ast.AST,
    modules: Mapping[str, str],
    functions: Mapping[str, str],
    allowed: Mapping[str, set[str]],
) -> tuple[str, str] | None:
    """Resolve one assignment to an imported process module or launcher."""
    if isinstance(value, ast.Name) and value.id in modules:
        return "module", modules[value.id]
    if isinstance(value, ast.Name) and value.id in functions:
        return "function", functions[value.id]
    if not isinstance(value, ast.Attribute) or not isinstance(value.value, ast.Name):
        return None
    module = modules.get(value.value.id)
    if module is None or value.attr not in allowed[module]:
        return None
    return "function", module


def propagate_process_aliases(
    bindings: Mapping[str, ast.AST],
    modules: dict[str, str],
    functions: dict[str, str],
    allowed: Mapping[str, set[str]],
) -> None:
    """Extend imported process launchers through unique assignments."""
    unresolved = dict(bindings)
    while unresolved:
        resolved: list[str] = []
        for name, value in unresolved.items():
            alias = assigned_process_alias(value, modules, functions, allowed)
            if alias is None:
                continue
            kind, module = alias
            target = modules if kind == "module" else functions
            target[name] = module
            resolved.append(name)
        if not resolved:
            return
        for name in resolved:
            del unresolved[name]


def process_aliases(tree: ast.AST) -> ProcessAliases:
    """Resolve simple aliases for process launchers and sys.executable."""
    modules: dict[str, str] = {}
    functions: dict[str, str] = {}
    sys_modules: set[str] = set()
    sys_executables: set[str] = set()
    allowed = {
        "os": {"popen", "system"},
        "subprocess": {"call", "check_call", "check_output", "Popen", "run"},
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name in allowed:
                    modules[alias.asname or alias.name] = alias.name
                elif alias.name == "sys":
                    sys_modules.add(alias.asname or alias.name)
        elif isinstance(node, ast.ImportFrom) and node.module in allowed:
            for alias in node.names:
                if alias.name in allowed[node.module]:
                    functions[alias.asname or alias.name] = node.module
        elif isinstance(node, ast.ImportFrom) and node.module == "sys":
            for alias in node.names:
                if alias.name == "executable":
                    sys_executables.add(alias.asname or alias.name)
    bindings = literal_bindings(tree)
    propagate_process_aliases(bindings, modules, functions, allowed)
    propagate_member_aliases(bindings, sys_modules, sys_executables, "executable")
    return ProcessAliases(
        modules,
        functions,
        frozenset(sys_modules),
        frozenset(sys_executables),
    )


def literal_bindings(tree: ast.AST) -> dict[str, ast.AST]:
    """Return uniquely assigned literal candidates safe for conservative resolution."""
    store_counts: dict[str, int] = {}
    candidates: dict[str, list[ast.AST]] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store):
            store_counts[node.id] = store_counts.get(node.id, 0) + 1
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            arguments = [*node.args.posonlyargs, *node.args.args, *node.args.kwonlyargs]
            if node.args.vararg is not None:
                arguments.append(node.args.vararg)
            if node.args.kwarg is not None:
                arguments.append(node.args.kwarg)
            for argument in arguments:
                store_counts[argument.arg] = store_counts.get(argument.arg, 0) + 1
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name):
                candidates.setdefault(target.id, []).append(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            if node.value is not None:
                candidates.setdefault(node.target.id, []).append(node.value)
        elif isinstance(node, ast.NamedExpr) and isinstance(node.target, ast.Name):
            candidates.setdefault(node.target.id, []).append(node.value)
    return {
        name: values[0]
        for name, values in candidates.items()
        if store_counts.get(name) == 1 and len(values) == 1
    }


def literal_string(
    node: ast.AST,
    bindings: Mapping[str, ast.AST],
    seen: frozenset[str] = frozenset(),
) -> str | None:
    """Resolve one bounded literal string expression without executing it."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.Name) and node.id in bindings and node.id not in seen:
        return literal_string(bindings[node.id], bindings, seen | {node.id})
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = literal_string(node.left, bindings, seen)
        right = literal_string(node.right, bindings, seen)
        return None if left is None or right is None else left + right
    if isinstance(node, ast.JoinedStr):
        pieces: list[str] = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                pieces.append(value.value)
            else:
                # Keep dynamic holes from joining two safe fragments into a
                # different forbidden token during conservative evaluation.
                pieces.append(" ")
        return "".join(pieces)
    return None


def literal_command_words(
    node: ast.AST,
    aliases: ProcessAliases,
    bindings: Mapping[str, ast.AST],
) -> list[str] | None:
    """Return literal argv words, representing aliased sys.executable as Python."""
    if isinstance(node, ast.Name) and node.id in bindings:
        return literal_command_words(bindings[node.id], aliases, bindings)
    if not isinstance(node, (ast.List, ast.Tuple)):
        return None
    words: list[str] = []
    for element in node.elts:
        is_python = (
            isinstance(element, ast.Attribute)
            and isinstance(element.value, ast.Name)
            and element.value.id in aliases.sys_modules
            and element.attr == "executable"
        ) or (isinstance(element, ast.Name) and element.id in aliases.sys_executables)
        value = literal_string(element, bindings)
        if value is not None:
            words.append(value)
        elif is_python:
            words.append("python")
        else:
            words.append("")
    return words


def is_process_call(function: ast.AST, aliases: ProcessAliases) -> bool:
    """Return whether a call target resolves to a supported process launcher."""
    if isinstance(function, ast.Name):
        return function.id in aliases.functions
    if not isinstance(function, ast.Attribute) or not isinstance(function.value, ast.Name):
        return False
    module = aliases.modules.get(function.value.id)
    return (
        module == "subprocess"
        and function.attr in {"call", "check_call", "check_output", "Popen", "run"}
    ) or (module == "os" and function.attr in {"popen", "system"})


def process_command_argument(node: ast.Call) -> ast.AST | None:
    """Return a process call's positional or explicit args= command expression."""
    if node.args:
        return node.args[0]
    return next((item.value for item in node.keywords if item.arg == "args"), None)


def forbidden_argv(words: list[str]) -> str | None:
    """Classify forbidden installer argv without executing or resolving it."""
    if not words:
        return None
    command = words[0].replace("\\", "/").rsplit("/", maxsplit=1)[-1].lower()
    if command in {"uvx", "uvx.exe"}:
        return "uvx"
    if command in {"uv", "uv.exe"} and words[1:3] == ["pip", "install"]:
        return "uv pip install"
    if re.fullmatch(r"pip(?:3(?:\.[0-9]+)?)?(?:\.exe)?", command) and words[1:2] == ["install"]:
        return "raw pip install"
    if re.fullmatch(r"(?:py|python(?:3(?:\.[0-9]+)?)?)(?:\.exe)?", command) and words[1:4] == [
        "-m",
        "pip",
        "install",
    ]:
        return "raw pip install"
    return None


def shell_installer_label(text: str) -> str | None:
    """Classify a literal shell command containing an unlocked installer."""
    if re.search(r"\buvx\b", text):
        return "uvx"
    if re.search(r"\buv\s+pip\s+install\b", text):
        return "uv pip install"
    interpreter = r"(?:py|python(?:3(?:\.[0-9]+)?)?)(?:\.exe)?"
    pip = r"pip(?:3(?:\.[0-9]+)?)?(?:\.exe)?"
    if re.search(rf"\b(?:{interpreter}\s+-m\s+)?{pip}\s+install\b", text):
        return "raw pip install"
    return None
