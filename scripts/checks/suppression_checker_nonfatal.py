# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""AST declarations and active-call cross-check for checker nonfatal modes."""

from __future__ import annotations

import ast
import re
from dataclasses import dataclass
from pathlib import Path

from suppression_build_controls import _shell_tokens
from suppression_catalog import ownership
from suppression_hash_lex import hash_lines
from suppression_model import Finding, Suppression

EXPECTED_DECLARATIONS = 10
EXPECTED_INFORMATIONAL_RULES = 3
DECLARED_OPTIONS = {
    "scripts/checks/stack_usage_check.py": {
        "--allow-empty": "fresh-build enumeration exception",
        "--warn-only": "soft stack-budget reporting mode",
    },
    "scripts/checks/audit_init_order.py": {"--no-strict": "advisory init-order mode"},
    "scripts/checks/check_world_tags.py": {"--warn": "advisory World-tag mode"},
    "scripts/checks/cite_check.py": {"--warn": "advisory citation mode"},
}
DEFAULT_NONFATAL_OPTIONS = {
    "scripts/checks/check_world_tags.py": ("--warn", "--strict"),
    "scripts/checks/cite_check.py": ("--warn", "--strict"),
}
DORMANT_CONSTANTS = {
    "scripts/checks/check_line_citations.py": "WARN_ONLY_MODE",
    "scripts/checks/check_inclusive_terminology.py": "WARN_ONLY_MODE",
}
INFORMATIONAL_AUTHORITY = ("scripts/checks/annot_rulekeys.py", "INFORMATIONAL_RULES")
CALL_SURFACES = (
    ".github/workflows/",
    "just/",
    "scripts/ci/",
    "scripts/git/",
)
ROOT_CALL_SURFACES = frozenset({"justfile"})
_SCALAR_ASSIGNMENT_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.+)$")
_ARRAY_ASSIGNMENT_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=$")
_ARRAY_APPEND_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\+=$")
_VARIABLE_RE = re.compile(
    r"^\$(?:{([A-Za-z_][A-Za-z0-9_]*)(?:\[(?:@|\*)\])?}|"
    r"([A-Za-z_][A-Za-z0-9_]*))$"
)
_COMMAND_SEPARATORS = frozenset({";", "&&", "||", "|"})
_MIN_ARRAY_ASSIGNMENT_TOKENS = 3
_DECLARATION_BUILTINS = frozenset({"declare", "export", "local", "readonly", "typeset"})
_STATIC_SHELLS = frozenset({"bash", "sh", "zsh"})
_NON_ANALYSIS_MODES = frozenset({"--help", "--selftest", "--version", "-h"})


def _literal_strings(node: ast.AST) -> list[str]:
    """Return literal string leaves from a closed AST container."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return [node.value]
    if isinstance(node, (ast.Set, ast.Tuple, ast.List)):
        return [item for child in node.elts for item in _literal_strings(child)]
    return []


def _help(call: ast.Call) -> str:
    """Return one argparse declaration's literal help text."""
    for keyword in call.keywords:
        if keyword.arg == "help":
            bits = _literal_strings(keyword.value)
            if bits:
                return " ".join(bits).strip()
            if isinstance(keyword.value, ast.BinOp):
                try:
                    value = ast.literal_eval(keyword.value)
                except (ValueError, TypeError):
                    return ""
                return value if isinstance(value, str) else ""
    return ""


@dataclass(frozen=True)
class NonfatalSpec:
    """Normalized fields for one declaration or invocation."""

    rule: str
    scope: str
    reason: str
    directive: str = "nonfatal-declaration"


def _row(path: str, line: int, spec: NonfatalSpec) -> Suppression:
    """Build a source-located nonfatal availability/activation row."""
    return Suppression(
        path,
        line,
        1,
        "checker-nonfatal-control",
        "repository-checker",
        spec.rule,
        spec.directive,
        spec.scope,
        spec.reason,
        "checker-control-plane",
        ownership(path),
        () if spec.reason else ("blank-reason",),
    )


def _option_declarations(path: str, tree: ast.Module) -> tuple[list[Suppression], list[Finding]]:
    """Parse only the five audited argparse nonfatal switches."""
    expected = DECLARED_OPTIONS.get(path)
    if expected is None:
        return [], []
    found: dict[str, Suppression] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr != "add_argument":
            continue
        flags = [
            argument.value
            for argument in node.args
            if isinstance(argument, ast.Constant) and isinstance(argument.value, str)
        ]
        boolean_optional = any(
            keyword.arg == "action"
            and isinstance(keyword.value, ast.Attribute)
            and keyword.value.attr == "BooleanOptionalAction"
            for keyword in node.keywords
        )
        if boolean_optional:
            flags.extend(f"--no-{flag[2:]}" for flag in tuple(flags) if flag.startswith("--"))
        for flag in flags:
            if flag in expected:
                found[flag] = _row(
                    path,
                    node.lineno,
                    NonfatalSpec(flag, "available-inactive", _help(node) or expected[flag]),
                )
    findings = [
        Finding("missing-nonfatal-declaration", flag, path)
        for flag in expected
        if flag not in found
    ]
    return [found[flag] for flag in expected if flag in found], findings


def _constant_declaration(path: str, tree: ast.Module) -> tuple[list[Suppression], list[Finding]]:
    """Inventory the two explicitly false warn-only module switches."""
    expected = DORMANT_CONSTANTS.get(path)
    if expected is None:
        return [], []
    for node in tree.body:
        targets: list[ast.expr] = []
        value: ast.AST | None = None
        if isinstance(node, ast.Assign):
            targets, value = node.targets, node.value
        elif isinstance(node, ast.AnnAssign):
            targets, value = [node.target], node.value
        if any(isinstance(target, ast.Name) and target.id == expected for target in targets):
            if isinstance(value, ast.Constant) and value.value is False:
                reason = (
                    "Dormant compatibility mode is explicitly false; the checker remains fatal."
                )
                spec = NonfatalSpec(expected, "available-inactive", reason)
                return [_row(path, node.lineno, spec)], []
            finding = Finding(
                "active-nonfatal-constant",
                f"{expected} is not literal False",
                path,
                node.lineno,
            )
            return [], [finding]
    return [], [Finding("missing-nonfatal-declaration", expected, path)]


def _informational_declarations(
    path: str, tree: ast.Module
) -> tuple[list[Suppression], list[Finding]]:
    """Split the annotation informational-rule authority into three rows."""
    if (path, INFORMATIONAL_AUTHORITY[1]) != INFORMATIONAL_AUTHORITY:
        return [], []
    for node in tree.body:
        target = None
        value = None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target, value = node.targets[0], node.value
        elif isinstance(node, ast.AnnAssign):
            target, value = node.target, node.value
        if not isinstance(target, ast.Name) or target.id != INFORMATIONAL_AUTHORITY[1]:
            continue
        values = _literal_strings(value) if value is not None else []
        if len(values) != EXPECTED_INFORMATIONAL_RULES:
            message = f"found {len(values)}; expected {EXPECTED_INFORMATIONAL_RULES}"
            return [], [Finding("nonfatal-informational-count", message, path, node.lineno)]
        reason = (
            "Annotation records information rather than asserting a developer-fixable property."
        )
        return [
            _row(path, node.lineno, NonfatalSpec(value, "informational-rule", reason))
            for value in sorted(values)
        ], []
    return [], [Finding("missing-nonfatal-declaration", INFORMATIONAL_AUTHORITY[1], path)]


def _logical_commands(path: str, text: str) -> list[tuple[int, str]]:
    """Return comment-free command lines with backslash continuations joined."""
    lines, _findings = hash_lines(path, text)
    result: list[tuple[int, str]] = []
    start = 0
    buffer = ""
    for item in lines:
        code = item.code.strip()
        if not code:
            continue
        if not buffer:
            start = item.line
        continued = code.endswith("\\")
        buffer += code[:-1].rstrip() + " " if continued else code
        if not continued:
            result.append((start, buffer.strip()))
            buffer = ""
    if buffer:
        result.append((start, buffer.strip()))
    return result


def _command_statements(command: str) -> list[list[tuple[str, bool]]]:
    """Split one comment-free shell line at real command operators."""
    statements: list[list[tuple[str, bool]]] = []
    current: list[tuple[str, bool]] = []
    for token in _shell_tokens(command):
        if token.operator and token.value in _COMMAND_SEPARATORS:
            if current:
                statements.append(current)
                current = []
            continue
        current.append((token.value, token.operator))
    if current:
        statements.append(current)
    return statements


def _expanded(value: str, variables: dict[str, tuple[str, ...]]) -> tuple[str, ...]:
    """Expand only an exact, previously observed scalar/array reference."""
    match = _VARIABLE_RE.fullmatch(value)
    if match is None:
        return (value,)
    name = match.group(1) or match.group(2)
    return variables.get(name, (value,))


def _remember_assignment(
    statement: list[tuple[str, bool]], variables: dict[str, tuple[str, ...]]
) -> bool:
    """Record closed-form scalar and argv-array assignments, without evaluation."""
    core = _assignment_core(statement)
    if len(core) == 1 and not core[0][1]:
        match = _SCALAR_ASSIGNMENT_RE.fullmatch(core[0][0])
        if match is not None:
            variables[match.group(1)] = (match.group(2),)
            return True
    if len(core) < _MIN_ARRAY_ASSIGNMENT_TOKENS or core[0][1]:
        return False
    match = _ARRAY_ASSIGNMENT_RE.fullmatch(core[0][0])
    append = _ARRAY_APPEND_RE.fullmatch(core[0][0])
    if (match is None and append is None) or core[1] != ("(", True) or core[-1] != (")", True):
        return False
    if any(operator for _value, operator in core[2:-1]):
        return False
    selected = match if match is not None else append
    if selected is None:
        return False
    name = selected.group(1)
    values = tuple(part for value, _operator in core[2:-1] for part in _expanded(value, variables))
    variables[name] = variables.get(name, ()) + values if append is not None else values
    return True


def _assignment_core(statement: list[tuple[str, bool]]) -> list[tuple[str, bool]]:
    """Remove a bounded shell declaration prefix before one assignment."""
    if not statement or statement[0][1] or statement[0][0] not in _DECLARATION_BUILTINS:
        return statement
    index = 1
    while (
        index < len(statement) and not statement[index][1] and statement[index][0].startswith("-")
    ):
        index += 1
    return statement[index:]


def _expanded_argv(
    statement: list[tuple[str, bool]], variables: dict[str, tuple[str, ...]]
) -> list[str]:
    """Expand exact shell references after an assignment-only statement."""
    return [
        part
        for value, operator in statement
        if not operator
        for part in _expanded(value, variables)
    ]


def _invokes_checker(argv: list[str], checker_name: str) -> bool:
    """Require the checker to be the command or a Python script argument."""
    positions = [index for index, value in enumerate(argv) if value.endswith(checker_name)]
    if not positions:
        return False
    if 0 in positions:
        return True
    python_positions = [
        index for index, value in enumerate(argv) if Path(value).name in {"python", "python3"}
    ]
    return any(python < checker for python in python_positions for checker in positions)


def _active_rows(rel: str, line: int, argv: list[str]) -> list[Suppression]:
    """Return every audited nonfatal flag on one resolved checker invocation."""
    rows: list[Suppression] = []
    for checker, options in DECLARED_OPTIONS.items():
        if not _invokes_checker(argv, Path(checker).name):
            continue
        default = DEFAULT_NONFATAL_OPTIONS.get(checker)
        for flag, reason in options.items():
            active_by_default = (
                default == (flag, "--strict")
                and "--strict" not in argv
                and not _NON_ANALYSIS_MODES.intersection(argv)
            )
            if flag in argv or active_by_default:
                spec = NonfatalSpec(flag, f"active-caller:{checker}", reason, "nonfatal-invocation")
                rows.append(_row(rel, line, spec))
    return rows


def _resolved_invocations(
    statement: list[tuple[str, bool]], variables: dict[str, tuple[str, ...]]
) -> list[list[str]]:
    """Resolve one direct argv and bounded static ``shell -c`` payloads."""
    argv = _expanded_argv(statement, variables)
    invocations = [argv]
    if (
        len(argv) >= _MIN_ARRAY_ASSIGNMENT_TOKENS
        and Path(argv[0]).name in _STATIC_SHELLS
        and argv[1] == "-c"
        and not any(marker in argv[2] for marker in ("$", "`", "\\n", "\\r"))
    ):
        nested_variables: dict[str, tuple[str, ...]] = {}
        invocations.extend(
            _expanded_argv(nested, nested_variables)
            for nested in _command_statements(argv[2])
            if not _remember_assignment(nested, nested_variables)
        )
    return invocations


def _active_usages(root: Path, paths: list[str]) -> list[Suppression]:
    """Find nonfatal flags passed by gate/Just/hook/workflow command surfaces."""
    rows: list[Suppression] = []
    for rel in paths:
        if rel not in ROOT_CALL_SURFACES and not rel.startswith(CALL_SURFACES):
            continue
        path = root / rel
        if not path.is_file():
            continue
        variables: dict[str, tuple[str, ...]] = {}
        for line, command in _logical_commands(rel, path.read_text(errors="replace")):
            for statement in _command_statements(command):
                if _remember_assignment(statement, variables):
                    continue
                for argv in _resolved_invocations(statement, variables):
                    rows.extend(_active_rows(rel, line, argv))
    return rows


def scan_checker_nonfatal_controls(
    root: Path, paths: list[str]
) -> tuple[list[Suppression], list[Finding]]:
    """Inventory declarations separately from active invocations and lock counts."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    interesting = set(DECLARED_OPTIONS) | set(DORMANT_CONSTANTS) | {INFORMATIONAL_AUTHORITY[0]}
    for rel in sorted(interesting):
        if rel not in paths or not (root / rel).is_file():
            findings.append(Finding("missing-nonfatal-authority", rel))
            continue
        try:
            tree = ast.parse((root / rel).read_text(encoding="utf-8"), filename=rel)
        except (OSError, SyntaxError) as exc:
            findings.append(Finding("checker-nonfatal-ast", str(exc), rel))
            continue
        for parser in (_option_declarations, _constant_declaration, _informational_declarations):
            rows, problems = parser(rel, tree)
            records.extend(rows)
            findings.extend(problems)
    active = _active_usages(root, paths)
    records.extend(active)
    declarations = [item for item in records if item.directive == "nonfatal-declaration"]
    if len(declarations) != EXPECTED_DECLARATIONS:
        findings.append(
            Finding(
                "checker-nonfatal-declaration-count",
                f"found {len(declarations)}; audited contract is {EXPECTED_DECLARATIONS}",
            )
        )
    if active:
        findings.append(
            Finding(
                "active-checker-nonfatal-invocation",
                f"found {len(active)} active gate/Just/hook/workflow invocation(s)",
            )
        )
    return records, findings
