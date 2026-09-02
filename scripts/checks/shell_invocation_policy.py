# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural caller validation for protected and sourced-only shell files.

The typed entry-point table owns which shell files require privileged startup
and which files may only be sourced.  This module owns how executable and
configuration text may refer to those files.  It deliberately parses argv
structure instead of looking for a few weak Bash spellings.
"""

from __future__ import annotations

import ast
import json
import re
import shlex
from collections.abc import Iterable, Mapping
from dataclasses import dataclass

import shell_invocation_selftest_cases as fixtures
from shell_entrypoint_policy import (
    ShellDialect,
    ShellPolicy,
    ShellSecurity,
    ShellUsage,
)
from shell_invocation_references import (
    DOCKER_COMMANDS,
    EXACT_REFERENCES,
    JSON_FENCE_LANGUAGES,
    PROCESS_CALLS,
    SHELL_FENCE_LANGUAGES,
    YAML_COMMAND_KEYS,
)

SHELL_CONTROL = frozenset({";", "&&", "||", "|", "&", "(", ")"})
SHELL_PREFIX = frozenset({"if", "elif", "while", "until", "then", "do", "!", "{"})
DECLARATION_PREFIX = frozenset({"declare", "export", "local", "readonly", "typeset"})
MARKDOWN_FENCE = re.compile(r"^\s*(```+|~~~+)\s*([A-Za-z0-9_+-]*)\s*$")
PRIVILEGED_TARGET_INDEX = 2
JUST_RECIPE = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*(?:\s+[^:]*)?:\s*$")
NONCOMMAND_SHELL_PREFIX = frozenset({"[[", "case", "for", "function", "select"})
RELEASE_LOADER_NAME = "source_release_selftest_helper_from"
RELEASE_LOADER_MAIN_REL = "scripts/dev/provision_dev_box_toolchain.sh"
RELEASE_LOADER_MAIN_LOGICAL = (
    'source_release_selftest_helper_from "$main" "$helper" '
    '"$expected_dir" "$expected_digest" || return 1'
)
RELEASE_LOADER_FIXTURE_REL = "scripts/dev/provision_dev_box_toolchain_selftest.bash"
RELEASE_LOADER_FIXTURE_LOGICAL = (
    'source_release_selftest_helper_from "$main" "$helper" "$directory" "$digest" || status=$?'
)
RELEASE_LOADER_GRAMMARS = {
    RELEASE_LOADER_MAIN_REL: (RELEASE_LOADER_MAIN_LOGICAL, ShellUsage.ENTRY, True, False),
    RELEASE_LOADER_FIXTURE_REL: (
        RELEASE_LOADER_FIXTURE_LOGICAL,
        ShellUsage.SOURCED_ONLY,
        False,
        True,
    ),
}


@dataclass(frozen=True)
class CallerFinding:
    """One structural caller-policy violation."""

    line: int
    message: str


@dataclass(frozen=True)
class ShellSegmentContext:
    """Immutable caller and source context for one logical shell command."""

    variables: Mapping[str, str]
    policies: Mapping[str, ShellPolicy]
    caller_policy: ShellPolicy | None
    rel: str | None
    logical: str
    stripped: str
    number: int


def guarded_paths(policies: Mapping[str, ShellPolicy]) -> frozenset[str]:
    """Return all privileged or source-only paths requiring caller review."""
    return frozenset(
        path
        for path, policy in policies.items()
        if policy.security is ShellSecurity.PRIVILEGED or policy.usage is ShellUsage.SOURCED_ONLY
    )


def _target_for_token(token: str, policies: Mapping[str, ShellPolicy]) -> str | None:
    """Resolve an exact or rooted token to one governed repository path."""
    normalized = token.replace("\\", "/")
    if normalized.startswith("file://"):
        normalized = normalized.removeprefix("file://")
    for target in guarded_paths(policies):
        if normalized == target or normalized.endswith(f"/{target}"):
            return target
    return None


def _expand_token(token: str, variables: Mapping[str, str]) -> str:
    """Resolve the deliberately small scalar shell-dataflow vocabulary."""
    if token.startswith("${") and token.endswith("}"):
        return variables.get(token[2:-1], token)
    if token.startswith("$") and token[1:].isidentifier():
        return variables.get(token[1:], token)
    return token


def _normalize_shell(logical: str) -> str:
    """Return stable whitespace for exact reviewed shell references."""
    return " ".join(logical.split())


def _is_exact_reference(rel: str | None, logical: str, target: str | None = None) -> bool:
    """Return whether an exact governed occurrence has a written non-launch reason."""
    if rel is None:
        return False
    normalized = _normalize_shell(logical)
    return any(
        entry.rel == rel
        and entry.logical == normalized
        and (target is None or entry.target == target)
        for entry in EXACT_REFERENCES
    )


def validate_argv(
    argv: list[str],
    policies: Mapping[str, ShellPolicy],
    caller_policy: ShellPolicy | None,
) -> list[str]:
    """Validate every governed target in one resolved argument vector."""
    targets = [(_target_for_token(token, policies), index) for index, token in enumerate(argv)]
    governed = [(target, index) for target, index in targets if target is not None]
    findings: list[str] = []
    for target, index in governed:
        if target is None:
            continue
        policy = policies[target]
        source_call = bool(argv) and argv[0] in {".", "source"} and index == 1
        if source_call:
            if policy.usage not in {ShellUsage.SOURCED_ONLY, ShellUsage.DUAL_USE}:
                findings.append(f"entry-only target is sourced: {target}")
            elif policy.source_requires_privileged_parent and (
                caller_policy is None or caller_policy.security is not ShellSecurity.PRIVILEGED
            ):
                findings.append(f"privileged source has no privileged parent: {target}")
            continue
        direct = (
            index == 0
            and policy.executable
            and policy.usage
            in {
                ShellUsage.ENTRY,
                ShellUsage.DUAL_USE,
            }
        )
        privileged_argv = (
            index == PRIVILEGED_TARGET_INDEX
            and argv[:2] == ["/bin/bash", "-p"]
            and policy.security is ShellSecurity.PRIVILEGED
            and policy.usage is not ShellUsage.SOURCED_ONLY
        )
        if not direct and not privileged_argv:
            findings.append(
                "governed target lacks direct verified shebang or exact "
                f"/bin/bash -p argv: {target}"
            )
    return findings


def _logical_shell_lines(text: str) -> list[tuple[int, str]]:
    """Join shell continuations and omit quoted heredoc payloads."""
    logicals: list[tuple[int, str]] = []
    lines = text.splitlines()
    index = 0
    heredoc_end: str | None = None
    while index < len(lines):
        number = index + 1
        line = lines[index]
        index += 1
        if heredoc_end is not None:
            if line.strip() == heredoc_end:
                heredoc_end = None
            continue
        logical = line
        while index < len(lines) and (
            logical.rstrip().endswith("\\") or logical.rstrip().endswith(("|", "||", "&&"))
        ):
            stripped = logical.rstrip()
            logical = (stripped.removesuffix("\\")) + lines[index]
            index += 1
        heredoc = re.search(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1", logical)
        if heredoc is not None:
            heredoc_end = heredoc.group(2)
        logicals.append((number, logical))
    return logicals


def _shell_tokens(logical: str) -> list[str] | None:
    """Decode one shell logical line, preserving command separators."""
    lexer = shlex.shlex(logical, posix=True, punctuation_chars=";&|()")
    lexer.commenters = "#"
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return None


def _command_segments(tokens: list[str]) -> Iterable[list[str]]:
    """Yield argv-like shell command segments."""
    segment: list[str] = []
    for token in tokens:
        if token in SHELL_CONTROL:
            if segment:
                yield segment
                segment = []
        else:
            segment.append(token)
    if segment:
        yield segment


def _env_command_index(arguments: list[str]) -> int:
    """Return the nested executable index for fixed env-style argv."""
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument in {"-u", "--unset"}:
            index += 2
            continue
        if argument.startswith("-") or re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", argument):
            index += 1
            continue
        break
    return index


def _xargs_command_index(arguments: list[str]) -> int:
    """Return the nested executable index for the supported xargs option grammar."""
    index = 1
    options_with_value = {"-a", "-E", "-I", "-L", "-n", "-P", "-s"}
    while index < len(arguments):
        argument = arguments[index]
        if argument == "-" * 2:
            return index + 1
        if argument in options_with_value:
            index += 2
        elif argument.startswith(("-I", "-E", "-L", "-n", "-P", "-s", "-")):
            index += 1
        else:
            break
    return index


def _nested_command_index(arguments: list[str]) -> int:
    """Return the actual executable index through reviewed command wrappers."""
    if arguments and arguments[0] in {"env", "/usr/bin/env"}:
        return _env_command_index(arguments)
    if arguments and arguments[0] in {"xargs", "/usr/bin/xargs"}:
        return _xargs_command_index(arguments)
    return 0


def _resolved_shell_argv(
    segment: list[str],
    variables: dict[str, str],
    policies: Mapping[str, ShellPolicy],
) -> tuple[list[str], bool]:
    """Resolve leading declarations and scalar command indirection."""
    tokens = list(segment)
    while tokens and tokens[0] in SHELL_PREFIX:
        tokens.pop(0)
    if tokens and tokens[0].startswith("@"):
        tokens[0] = tokens[0][1:]
    if not tokens or tokens[0] in NONCOMMAND_SHELL_PREFIX or tokens[0] in {"}", "]]"}:
        return [], False
    declaration = bool(tokens and tokens[0] in DECLARATION_PREFIX)
    if declaration:
        tokens.pop(0)
    while tokens and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", tokens[0]):
        name, value = tokens.pop(0).split("=", 1)
        variables[name] = _expand_token(value, variables)
    if declaration:
        return [], False
    active = [token for token in tokens if token not in {"then", "do", "}"}]
    if active and active[0] == "exec":
        active.pop(0)
    command_index = _nested_command_index(active)
    expanded = [_expand_token(token, variables) for token in active]
    indirect_interpreter = (
        command_index < len(active)
        and active[command_index].startswith("$")
        and expanded[command_index] == "/bin/bash"
    )
    indirect_target = any(
        token.startswith("$")
        and "/" not in token
        and _target_for_token(value, policies) is not None
        for index, (token, value) in enumerate(zip(active, expanded, strict=True))
        if index != command_index
    )
    return expanded[command_index:], indirect_interpreter or indirect_target


def _is_authenticated_release_loader(
    rel: str | None,
    caller_policy: ShellPolicy | None,
    logical: str,
    segment: list[str],
) -> bool:
    """Recognize either exact policy-bound release-helper source call."""
    grammar = RELEASE_LOADER_GRAMMARS.get(rel or "")
    if grammar is None:
        return False
    expected, usage, executable, privileged_parent = grammar
    expected_argv = tuple(shlex.split(expected.split(" || ", 1)[0]))
    return (
        caller_policy is not None
        and caller_policy.security is ShellSecurity.PRIVILEGED
        and caller_policy.usage is usage
        and caller_policy.dialect is ShellDialect.BASH
        and caller_policy.executable is executable
        and caller_policy.source_requires_privileged_parent is privileged_parent
        and logical.strip() == expected
        and tuple(segment) == expected_argv
    )


def _scan_shell_segment(
    segment: list[str],
    context: ShellSegmentContext,
) -> tuple[list[CallerFinding], int]:
    """Validate one shell command segment and count an accepted loader call."""
    is_loader_call = segment and segment[0] == RELEASE_LOADER_NAME
    is_loader_definition = context.stripped == f"{RELEASE_LOADER_NAME}() {{"
    if is_loader_call and not is_loader_definition:
        accepted = _is_authenticated_release_loader(
            context.rel,
            context.caller_policy,
            context.logical,
            segment,
        )
        findings = (
            []
            if accepted
            else [CallerFinding(context.number, "authenticated release loader drifted")]
        )
        return findings, int(accepted)
    argv, indirect = _resolved_shell_argv(segment, context.variables, context.policies)
    if indirect:
        if _is_exact_reference(context.rel, context.logical):
            return [], 0
        return [
            CallerFinding(context.number, "variable-indirect protected interpreter or target")
        ], 0
    messages = validate_argv(argv, context.policies, context.caller_policy)
    targets = {target for token in argv if (target := _target_for_token(token, context.policies))}
    if (
        messages
        and targets
        and all(_is_exact_reference(context.rel, context.logical, target) for target in targets)
    ):
        return [], 0
    return [CallerFinding(context.number, message) for message in messages], 0


def scan_shell_text(
    text: str,
    policies: Mapping[str, ShellPolicy],
    caller_policy: ShellPolicy | None,
    rel: str | None = None,
) -> list[CallerFinding]:
    """Structurally validate shell logical commands."""
    findings: list[CallerFinding] = []
    variables: dict[str, str] = {}
    governed = guarded_paths(policies)
    array_depth = 0
    release_loader_calls = 0
    for number, logical in _logical_shell_lines(text):
        stripped = logical.strip()
        if array_depth:
            array_depth += stripped.count("(") - stripped.count(")")
            continue
        if re.match(
            r"^(?:declare\s+-a\s+|local\s+-a\s+)?[A-Za-z_][A-Za-z0-9_]*=\($",
            stripped,
        ):
            array_depth = 1
            continue
        tokens = _shell_tokens(logical)
        if tokens is None:
            targets = [target for target in governed if target in logical]
            if targets and not all(_is_exact_reference(rel, logical, target) for target in targets):
                findings.append(CallerFinding(number, "cannot parse governed shell occurrence"))
            continue
        if tokens and tokens[0] in NONCOMMAND_SHELL_PREFIX:
            continue
        context = ShellSegmentContext(
            variables,
            policies,
            caller_policy,
            rel,
            logical,
            stripped,
            number,
        )
        for segment in _command_segments(tokens):
            segment_findings, accepted_calls = _scan_shell_segment(segment, context)
            findings.extend(segment_findings)
            release_loader_calls += accepted_calls
    findings.extend(
        ()
        if rel not in RELEASE_LOADER_GRAMMARS or release_loader_calls == 1
        else (CallerFinding(0, "authenticated release loader count drifted"),)
    )
    return findings


def _call_name(call: ast.Call) -> str:
    """Return a dotted static call name."""
    parts: list[str] = []
    node: ast.expr = call.func
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
    return ".".join(reversed(parts))


def _string_expr(node: ast.AST, values: Mapping[str, object]) -> str | None:
    """Resolve a bounded static/path-like Python string expression."""
    result: str | None = None
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        result = node.value
    elif isinstance(node, ast.Name):
        value = values.get(node.id)
        result = value if isinstance(value, str) else None
    elif isinstance(node, ast.JoinedStr):
        pieces: list[str] = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                pieces.append(value.value)
            else:
                pieces.append("*")
        result = "".join(pieces)
    elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Div)):
        left = _string_expr(node.left, values)
        right = _string_expr(node.right, values)
        if left is None:
            left = "*"
        if right is not None:
            separator = "/" if isinstance(node.op, ast.Div) else ""
            result = f"{left.rstrip('/')}{separator}{right.lstrip('/')}"
    elif (
        isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id in {"Path", "PurePath", "str"}
        and node.args
    ):
        result = _string_expr(node.args[0], values)
    return result


def _argv_expr(node: ast.AST, values: Mapping[str, object]) -> list[str] | None:
    """Resolve a bounded literal/list Python argv expression."""
    if isinstance(node, ast.Name):
        value = values.get(node.id)
        return list(value) if isinstance(value, tuple) else None
    if isinstance(node, (ast.List, ast.Tuple)):
        argv: list[str] = []
        for element in node.elts:
            if isinstance(element, ast.Starred):
                nested = _argv_expr(element.value, values)
                if nested is None:
                    return None
                argv.extend(nested)
                continue
            value = _string_expr(element, values)
            if value is None:
                return None
            argv.append(value)
        return argv
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = _argv_expr(node.left, values)
        right = _argv_expr(node.right, values)
        return None if left is None or right is None else left + right
    return None


class _PythonCallerVisitor(ast.NodeVisitor):
    """Track bounded argv dataflow into process-creation APIs."""

    def __init__(self, policies: Mapping[str, ShellPolicy]) -> None:
        self.policies = policies
        self.values: dict[str, object] = {}
        self.findings: list[CallerFinding] = []

    def visit_Assign(self, node: ast.Assign) -> None:
        """Remember simple string and argv assignments."""
        string = _string_expr(node.value, self.values)
        argv = _argv_expr(node.value, self.values)
        value: object | None = tuple(argv) if argv is not None else string
        for target in node.targets:
            if isinstance(target, ast.Name) and value is not None:
                self.values[target.id] = value
        self.generic_visit(node)

    def visit_AnnAssign(self, node: ast.AnnAssign) -> None:
        """Remember simple annotated assignments."""
        if isinstance(node.target, ast.Name) and node.value is not None:
            string = _string_expr(node.value, self.values)
            argv = _argv_expr(node.value, self.values)
            value: object | None = tuple(argv) if argv is not None else string
            if value is not None:
                self.values[node.target.id] = value
        self.generic_visit(node)

    def visit_Call(self, node: ast.Call) -> None:
        """Validate bounded argv passed to process-creation APIs."""
        name = _call_name(node)
        if name in PROCESS_CALLS and node.args:
            argv = _argv_expr(node.args[0], self.values)
            if argv is None and name.endswith("create_subprocess_exec"):
                argv = [
                    value
                    for argument in node.args
                    if (value := _string_expr(argument, self.values)) is not None
                ]
                if len(argv) != len(node.args):
                    argv = None
            governed_literals = {
                target
                for child in ast.walk(node)
                if isinstance(child, ast.Constant) and isinstance(child.value, str)
                for target in guarded_paths(self.policies)
                if target in child.value
            }
            if argv is None:
                if governed_literals:
                    self.findings.append(
                        CallerFinding(node.lineno, "governed Python process argv is unresolved")
                    )
            else:
                for message in validate_argv(argv, self.policies, None):
                    self.findings.append(CallerFinding(node.lineno, message))
        self.generic_visit(node)


def scan_python_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Parse Python and validate process argv with bounded dataflow."""
    try:
        tree = ast.parse(text)
    except SyntaxError as exc:
        if any(target in text for target in guarded_paths(policies)):
            return [CallerFinding(exc.lineno or 1, "cannot parse governed Python occurrence")]
        return []
    visitor = _PythonCallerVisitor(policies)
    visitor.visit(tree)
    return visitor.findings


def _json_argv_findings(
    value: object,
    policies: Mapping[str, ShellPolicy],
    line: int = 1,
) -> list[CallerFinding]:
    """Validate command/args objects and argv arrays recursively."""
    findings: list[CallerFinding] = []
    if isinstance(value, dict):
        command = value.get("command")
        args = value.get("args")
        grouped = isinstance(command, str) and isinstance(args, list)
        if grouped:
            argv = [command, *(item for item in args if isinstance(item, str))]
            if len(argv) != len(args) + 1:
                findings.append(CallerFinding(line, "JSON command argv contains non-string data"))
            else:
                findings.extend(
                    CallerFinding(line, message) for message in validate_argv(argv, policies, None)
                )
        for key, nested in value.items():
            if grouped and key in {"command", "args"}:
                continue
            findings.extend(_json_argv_findings(nested, policies, line))
    elif isinstance(value, list):
        if all(isinstance(item, str) for item in value):
            argv = list(value)
            if any(_target_for_token(item, policies) is not None for item in argv):
                findings.extend(
                    CallerFinding(line, message) for message in validate_argv(argv, policies, None)
                )
        for nested in value:
            findings.extend(_json_argv_findings(nested, policies, line))
    return findings


def scan_json_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Parse JSON command structures and argv arrays."""
    try:
        value = json.loads(text)
    except json.JSONDecodeError as exc:
        if any(target in text for target in guarded_paths(policies)):
            return [CallerFinding(exc.lineno, "cannot parse governed JSON occurrence")]
        return []
    return _json_argv_findings(value, policies)


def scan_markdown_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Validate executable shell and JSON fenced blocks; prose stays prose."""
    findings: list[CallerFinding] = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        match = MARKDOWN_FENCE.match(lines[index])
        if match is None:
            index += 1
            continue
        fence, language = match.groups()
        start = index + 2
        index += 1
        payload: list[str] = []
        while index < len(lines) and not lines[index].lstrip().startswith(fence):
            payload.append(lines[index])
            index += 1
        block = "\n".join(payload) + "\n"
        if language in SHELL_FENCE_LANGUAGES:
            normalized = "\n".join(line.removeprefix("$ ").removeprefix("> ") for line in payload)
            block_findings = scan_shell_text(normalized, policies, None)
        elif language in JSON_FENCE_LANGUAGES:
            block_findings = scan_json_text(block, policies)
        else:
            block_findings = []
        findings.extend(
            CallerFinding(start + finding.line - 1, finding.message) for finding in block_findings
        )
        index += 1
    return findings


def scan_just_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Validate recipe bodies and preserve their exact interpreter context."""
    findings: list[CallerFinding] = []
    lines = text.splitlines()
    clean_prefixes = {
        match.group(1): "/usr/bin/env -i"
        for line in lines
        if (
            match := re.match(
                r'^([A-Za-z_][A-Za-z0-9_]*)\s*:=\s*"/usr/bin/env -i(?:\s|\")',
                line,
            )
        )
    }
    index = 0
    while index < len(lines):
        if not JUST_RECIPE.match(lines[index]):
            index += 1
            continue
        index += 1
        body_start = index + 1
        body: list[str] = []
        while index < len(lines) and (not lines[index] or lines[index][0].isspace()):
            body_line = lines[index].lstrip()
            for name, prefix in clean_prefixes.items():
                body_line = body_line.replace(f"{{{{ {name} }}}}", prefix)
            body.append(body_line)
            index += 1
        active = [
            line for line in body if line and (line == "#!/bin/bash -p" or not line.startswith("#"))
        ]
        caller_policy = None
        if active and active[0] == "#!/bin/bash -p":
            caller_policy = ShellPolicy(
                ShellSecurity.PRIVILEGED,
                ShellUsage.ENTRY,
                ShellDialect.BASH,
                executable=False,
                source_requires_privileged_parent=False,
            )
        findings.extend(
            CallerFinding(body_start + finding.line - 1, finding.message)
            for finding in scan_shell_text("\n".join(body) + "\n", policies, caller_policy)
        )
    return findings


def scan_yaml_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Validate YAML command scalars and block bodies without accepting data keys."""
    findings: list[CallerFinding] = []
    lines = text.splitlines()
    block_indent: int | None = None
    block_folded = False
    block: list[str] = []
    block_start = 1

    def normalize_templates(command: str) -> str:
        """Resolve only quoted literal governed paths in Ansible expressions."""
        normalized = command
        for target in guarded_paths(policies):
            pattern = (
                r"\{\{\s*\([A-Za-z_][A-Za-z0-9_]*\s*~\s*"
                rf"(['\"])/?{re.escape(target)}\1\)\s*\|\s*quote\s*\}}\}}"
            )
            normalized = re.sub(pattern, target, normalized)
        return normalized

    def flush() -> None:
        if not block:
            return
        command = " ".join(line.strip() for line in block) if block_folded else "\n".join(block)
        command = normalize_templates(command)
        findings.extend(
            CallerFinding(block_start + finding.line - 1, finding.message)
            for finding in scan_shell_text(command + "\n", policies, None)
        )
        block.clear()

    for number, line in enumerate(lines, start=1):
        indent = len(line) - len(line.lstrip())
        if block_indent is not None:
            if line.strip() and indent <= block_indent:
                flush()
                block_indent = None
            else:
                block.append(line[block_indent + 1 :] if len(line) > block_indent else "")
                continue
        match = re.match(r"^\s*(?:-\s*)?([A-Za-z_][A-Za-z0-9_-]*):\s*(.*)$", line)
        if match is None or match.group(1) not in YAML_COMMAND_KEYS:
            continue
        value = match.group(2).strip()
        if value in {"|", ">", "|-", ">-"}:
            block_indent = indent
            block_folded = value.startswith(">")
            block_start = number + 1
        elif value:
            findings.extend(
                CallerFinding(number, finding.message)
                for finding in scan_shell_text(normalize_templates(value), policies, None)
            )
    flush()
    return findings


def scan_dockerfile_text(text: str, policies: Mapping[str, ShellPolicy]) -> list[CallerFinding]:
    """Validate Docker RUN shell commands and JSON-form CMD/ENTRYPOINT argv."""
    findings: list[CallerFinding] = []
    for number, logical in _logical_shell_lines(text):
        match = re.match(r"^\s*([A-Za-z]+)\s+(.+)$", logical)
        if match is None or match.group(1).upper() not in DOCKER_COMMANDS:
            continue
        command, value = match.group(1).upper(), match.group(2).strip()
        if command in {"CMD", "ENTRYPOINT"} and value.startswith("["):
            findings.extend(
                CallerFinding(number, finding.message)
                for finding in scan_json_text(value, policies)
            )
        else:
            findings.extend(
                CallerFinding(number, finding.message)
                for finding in scan_shell_text(value, policies, None)
            )
    return findings


def scan_caller_text(
    rel: str,
    text: str,
    policies: Mapping[str, ShellPolicy],
) -> list[CallerFinding]:
    """Dispatch one first-party executable/configuration text surface."""
    findings: list[CallerFinding]
    caller_policy = policies.get(rel)
    if rel.endswith(".just") or rel == "justfile":
        findings = scan_just_text(text, policies)
    elif caller_policy is not None:
        findings = scan_shell_text(text, policies, caller_policy, rel)
    elif rel.endswith(".py"):
        findings = scan_python_text(text, policies)
    elif rel.endswith(".json"):
        findings = scan_json_text(text, policies)
    elif rel.endswith(".md"):
        findings = scan_markdown_text(text, policies)
    elif rel.endswith((".yml", ".yaml")):
        findings = scan_yaml_text(text, policies)
    elif rel.endswith("Dockerfile") or rel.rsplit("/", 1)[-1] == "Dockerfile":
        findings = scan_dockerfile_text(text, policies)
    elif rel == ".env.example":
        findings = scan_shell_text(text, policies, None)
    else:
        findings = []
    return findings


def _static_reference_selftest_failures(policies: dict[str, ShellPolicy]) -> list[str]:
    """Exercise privileged source edges and the exact syntax-only reference."""
    sourced = fixtures.SOURCED
    failures = [
        "privileged source parent rejected"
        for prefix in ("source ", ". $ROOT/")
        if scan_shell_text(f"{prefix}{sourced}\n", policies, fixtures.PRIVILEGED_CALLER)
    ]
    exact_static = "/bin/bash -p -n scripts/dev/agent_workspace.sh\n"
    static_policy = {
        **policies,
        "scripts/dev/agent_workspace.sh": ShellPolicy(
            ShellSecurity.PRIVILEGED,
            ShellUsage.ENTRY,
            ShellDialect.BASH,
            executable=True,
            source_requires_privileged_parent=False,
        ),
    }
    if scan_shell_text(exact_static, static_policy, None, "scripts/ci/gates/tests.sh"):
        failures.append("exact reasoned static syntax reference was rejected")
    if not scan_shell_text(
        exact_static.replace(" -n ", " -n -O extglob "),
        static_policy,
        None,
        "scripts/ci/gates/tests.sh",
    ):
        failures.append("mutated static syntax reference was accepted")
    return failures


def selftest_failures() -> tuple[list[str], int]:
    """Replay the complete caller-bypass class in inert fixtures."""
    policies = fixtures.POLICIES
    shell_cases = fixtures.SHELL_CASES
    failures = [
        label
        for text, expected, label in shell_cases
        if len(scan_shell_text(text, policies, None)) != expected
    ]
    failures.extend(_static_reference_selftest_failures(policies))
    loader_cases = fixtures.RELEASE_LOADER_CASES
    failures.extend(
        label
        for rel, caller, text, expect_finding, label in loader_cases
        if bool(scan_shell_text(f"{text}\n", policies, caller, rel)) != expect_finding
    )
    python_cases = fixtures.PYTHON_CASES
    failures.extend(
        label
        for text, expected, label in python_cases
        if len(scan_python_text(text, policies)) != expected
    )
    surface_scanners = {
        "json": scan_json_text,
        "yaml": scan_yaml_text,
        "docker": scan_dockerfile_text,
        "markdown": scan_markdown_text,
    }
    surface_cases = fixtures.SURFACE_CASES
    failures.extend(
        label
        for text, scanner, expected, label in surface_cases
        if len(surface_scanners[scanner](text, policies)) != expected
    )
    just_cases = fixtures.JUST_CASES
    failures.extend(
        label
        for text, expected, label in just_cases
        if len(scan_just_text(text, policies)) != expected
    )
    return (
        failures,
        len(shell_cases)
        + 4
        + len(loader_cases)
        + len(python_cases)
        + len(surface_cases)
        + len(just_cases),
    )
