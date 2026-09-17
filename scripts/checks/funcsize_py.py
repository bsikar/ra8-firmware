# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Python function-body measurement for ``check_function_size.py``.

Measures the same span the C rule measures, which needs one deliberate
adjustment rather than a different number -- see the "what is counted"
discussion in ``check_function_size.py``.

In C, a function's Doxygen contract sits *above* the signature and is therefore
outside the measured span.  In Python the docstring is the first statement of
the body and would be inside it, so a heavily documented function would count as
oversized purely for being documented -- in a repository whose stated policy is
maximum documentation.  The docstring span is therefore subtracted, which makes
the two languages measure the same thing: the code a reader has to hold in
their head, with the contract excluded in both.

Nothing else is subtracted.  Blank lines and comments are inside the C span, so
they stay inside the Python one.

Unlike the C side this is a real parse (``ast``), so nested functions,
decorators, and multi-line signatures are exact rather than heuristic.
"""

from __future__ import annotations

import ast
from pathlib import Path


def _docstring_lines(node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
    """Physical line count of `node`'s docstring, or 0 if it has none."""
    if not node.body:
        return 0
    first = node.body[0]
    if (
        isinstance(first, ast.Expr)
        and isinstance(first.value, ast.Constant)
        and isinstance(first.value.value, str)
        and first.end_lineno is not None
    ):
        return first.end_lineno - first.lineno + 1
    return 0


def _signature(source_lines: list[str], node: ast.AST) -> str:
    """The ``def`` line, for the diagnostic table."""
    idx = node.lineno - 1
    if 0 <= idx < len(source_lines):
        return source_lines[idx].strip()
    return "<unknown>"


def scan(path: Path, threshold: int) -> list[tuple[int, int, str]]:
    """Return (start_line, length, signature) for functions over `threshold`.

    A file that does not parse yields nothing here; ``check_ruff.py`` owns
    syntax errors and reporting them twice helps nobody.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return []

    source_lines = text.splitlines()
    out: list[tuple[int, int, str]] = []
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if node.end_lineno is None:
            continue
        length = node.end_lineno - node.lineno + 1 - _docstring_lines(node)
        if length > threshold:
            out.append((node.lineno, length, _signature(source_lines, node)))
    out.sort()
    return out
