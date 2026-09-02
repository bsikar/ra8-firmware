# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Exact executable-AST helpers for HIL convergence structural checks."""

from __future__ import annotations

import ast


def same_statement(node: ast.stmt, source: str) -> bool:
    """Compare executable AST rather than source tokens or comments."""
    expected = ast.parse(source).body[0]
    return ast.dump(node, include_attributes=False) == ast.dump(expected, include_attributes=False)


def function(tree: ast.Module, name: str) -> ast.FunctionDef | None:
    """Return one uniquely named top-level function."""
    matches = [
        node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    return matches[0] if len(matches) == 1 else None


def statement_index(function_node: ast.FunctionDef, source: str) -> int:
    """Find one exact executable top-level statement in a function."""
    matches = [
        index for index, node in enumerate(function_node.body) if same_statement(node, source)
    ]
    return matches[0] if len(matches) == 1 else -1


def assignment(function_node: ast.FunctionDef, name: str) -> ast.expr | None:
    """Return one exact top-level simple-assignment value."""
    matches = [
        node.value
        for node in function_node.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id == name
    ]
    return matches[0] if len(matches) == 1 else None


def nested_assignment(function_node: ast.FunctionDef, name: str) -> ast.expr | None:
    """Return one uniquely named simple assignment anywhere in a function."""
    matches = []
    for node in ast.walk(function_node):
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name:
            matches.append(node.value)
    return matches[0] if len(matches) == 1 else None


def return_strings(function_node: ast.FunctionDef | None) -> set[str]:
    """Return executable string literals from one unique return expression."""
    if function_node is None:
        return set()
    returns = [node.value for node in function_node.body if isinstance(node, ast.Return)]
    if len(returns) != 1:
        return set()
    return {
        node.value
        for node in ast.walk(returns[0])
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
    }


def module_assignment(tree: ast.Module, name: str) -> ast.expr | None:
    """Return one exact top-level module assignment value."""
    matches = [
        node.value
        for node in tree.body
        if isinstance(node, ast.Assign)
        and len(node.targets) == 1
        and isinstance(node.targets[0], ast.Name)
        and node.targets[0].id == name
    ]
    return matches[0] if len(matches) == 1 else None
