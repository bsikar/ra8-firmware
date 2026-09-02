# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Syntax-aware inventory for test, workflow, and Ansible controls."""

from __future__ import annotations

import ast
import re
from dataclasses import dataclass
from pathlib import Path

import yaml
from suppression_catalog import ownership
from suppression_hash_lex import HashLexLine, hash_lines
from suppression_model import Finding, Suppression

CMAKE_CONTROL_HINT = re.compile(
    r"\b(?:DISABLED|WILL_FAIL|SKIP_RETURN_CODE|SKIP_REGULAR_EXPRESSION|"
    r"PASS_REGULAR_EXPRESSION|FAIL_REGULAR_EXPRESSION)\b"
)
# Only workflow and Ansible trees carry the YAML result/log controls.
YAML_CONTROL_ROOTS = (".github/workflows/", "infra/ansible/")
YAML_CONTROL_HINT = re.compile(
    r"\b(?:continue-on-error|if-no-files-found|ignore_errors|failed_when|"
    r"changed_when|no_log)\s*:"
)
CTEST_PROPERTIES = frozenset(
    {
        "DISABLED",
        "WILL_FAIL",
        "SKIP_RETURN_CODE",
        "SKIP_REGULAR_EXPRESSION",
        "PASS_REGULAR_EXPRESSION",
        "FAIL_REGULAR_EXPRESSION",
    }
)
PYTHON_CALLS = {
    "pytest.mark.skip": ("pytest", "skip", 0),
    "pytest.mark.skipif": ("pytest", "skipif", None),
    "pytest.mark.xfail": ("pytest", "xfail", None),
    "pytest.skip": ("pytest", "skip", 0),
    "pytest.xfail": ("pytest", "xfail", 0),
    "unittest.skip": ("unittest", "skip", 0),
    "unittest.skipIf": ("unittest", "skipIf", 1),
    "unittest.skipUnless": ("unittest", "skipUnless", 1),
    "unittest.expectedFailure": ("unittest", "expectedFailure", None),
    "unittest.SkipTest": ("unittest", "SkipTest", 0),
    "self.skipTest": ("unittest", "skipTest", 0),
}
BARE_PYTHON_CONTROLS = {
    "pytest.mark.skip": ("pytest", "skip"),
    "pytest.mark.xfail": ("pytest", "xfail"),
    "unittest.expectedFailure": ("unittest", "expectedFailure"),
}


@dataclass(frozen=True)
class CMakeCall:
    """One active CMake command invocation."""

    name: str
    body: str
    line: int
    column: int


def _concerns(reason: str, *extra: str) -> tuple[str, ...]:
    """Return review concerns without treating inventory as approval."""
    concerns = list(extra)
    if not reason.strip():
        concerns.append("blank-reason")
    return tuple(dict.fromkeys(concerns))


def _python_aliases(tree: ast.AST) -> dict[str, str]:
    """Resolve explicit pytest/unittest import aliases used by controls."""
    aliases: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for item in node.names:
                if item.name in {"pytest", "unittest"}:
                    aliases[item.asname or item.name] = item.name
        elif isinstance(node, ast.ImportFrom) and node.module in {"pytest", "unittest"}:
            for item in node.names:
                if item.name != "*":
                    aliases[item.asname or item.name] = f"{node.module}.{item.name}"
    return aliases


def _dotted_name(node: ast.AST, aliases: dict[str, str]) -> str:
    """Return a dotted name for one Python name/attribute expression."""
    parts: list[str] = []
    cursor: ast.AST = node
    while isinstance(cursor, ast.Attribute):
        parts.append(cursor.attr)
        cursor = cursor.value
    if isinstance(cursor, ast.Name):
        parts.append(cursor.id)
        resolved = list(reversed(parts))
        resolved[0] = aliases.get(resolved[0], resolved[0])
        return ".".join(resolved)
    return ""


def _string_value(node: ast.AST | None) -> tuple[str, bool]:
    """Return a literal rationale and whether its value is dynamic."""
    if node is None:
        return "", False
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value.strip(), False
    return "dynamic reason expression", True


def _python_reason(call: ast.Call, position: int | None) -> tuple[str, bool]:
    """Read a Python skip reason from its tool-specific argument slot."""
    for keyword in call.keywords:
        if keyword.arg == "reason":
            return _string_value(keyword.value)
    if position is not None and len(call.args) > position:
        return _string_value(call.args[position])
    return "", False


def _strict_xfail(call: ast.Call) -> bool:
    """Return whether an xfail call makes an unexpected pass fail."""
    for keyword in call.keywords:
        if keyword.arg == "strict":
            return isinstance(keyword.value, ast.Constant) and keyword.value.value is True
    return False


def _python_record(path: str, node: ast.AST, dotted: str, call: ast.Call | None) -> Suppression:
    """Build one Python test-control inventory row."""
    if call is None:
        tool, rule = BARE_PYTHON_CONTROLS[dotted]
        reason, dynamic = "", False
    else:
        tool, rule, position = PYTHON_CALLS[dotted]
        reason, dynamic = _python_reason(call, position)
    extras: list[str] = []
    if dynamic:
        extras.append("dynamic-reason")
    if dotted == "pytest.mark.xfail" and (call is None or not _strict_xfail(call)):
        extras.append("non-strict-xfail")
    return Suppression(
        path,
        getattr(node, "lineno", 1),
        getattr(node, "col_offset", 0) + 1,
        "test-control",
        tool,
        rule,
        dotted,
        "test",
        reason,
        "python-ast",
        ownership(path),
        _concerns(reason, *extras),
    )


def _bare_python_nodes(tree: ast.AST) -> list[ast.AST]:
    """Return bare marker/decorator nodes that are not calls."""
    nodes: list[ast.AST] = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            nodes.extend(item for item in node.decorator_list if not isinstance(item, ast.Call))
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            value = node.value
            if value is not None and not isinstance(value, ast.Call):
                nodes.append(value)
    return nodes


def _python_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Parse Python test-control calls and decorators through the AST."""
    if Path(path).suffix not in {".py", ".pyi"} or ownership(path) != "first-party":
        return [], []
    try:
        tree = ast.parse(text, filename=path)
    except SyntaxError as exc:
        message = exc.msg or "Python parser rejected test-control source"
        return [], [Finding("malformed-test-control", message, path, exc.lineno or 0)]
    aliases = _python_aliases(tree)
    records: list[Suppression] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        dotted = _dotted_name(node.func, aliases)
        if dotted in PYTHON_CALLS:
            records.append(_python_record(path, node, dotted, node))
    for node in _bare_python_nodes(tree):
        dotted = _dotted_name(node, aliases)
        if dotted in BARE_PYTHON_CONTROLS:
            records.append(_python_record(path, node, dotted, None))
    return records, []


def _cmake_source(path: str, text: str) -> tuple[str, list[HashLexLine]]:
    """Mask CMake comments/bracket payloads while retaining line positions."""
    lines, _ = hash_lines(path, text)
    return "\n".join(line.code for line in lines), lines


def _cmake_call_end(source: str, opening: int) -> int | None:
    """Find the matching close parenthesis outside quoted arguments."""
    depth = 1
    quote = False
    escaped = False
    for index in range(opening + 1, len(source)):
        char = source[index]
        if escaped:
            escaped = False
        elif quote and char == "\\":
            escaped = True
        elif char == '"':
            quote = not quote
        elif not quote and char == "(":
            depth += 1
        elif not quote and char == ")":
            depth -= 1
            if depth == 0:
                return index
    return None


def _quoted_cmake_end(source: str, opening: int) -> int | None:
    """Return the end of a top-level quoted token, honoring escapes."""
    escaped = False
    for index in range(opening + 1, len(source)):
        char = source[index]
        if escaped:
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == '"':
            return index
    return None


def _cmake_calls(source: str) -> tuple[list[CMakeCall], bool]:
    """Parse top-level CMake calls without searching argument strings."""
    calls: list[CMakeCall] = []
    position = 0
    while position < len(source):
        if source[position] == '"':
            end = _quoted_cmake_end(source, position)
            if end is None:
                return calls, True
            position = end + 1
            continue
        if not (source[position].isalpha() or source[position] == "_"):
            position += 1
            continue
        start = position
        while position < len(source) and (source[position].isalnum() or source[position] == "_"):
            position += 1
        name = source[start:position]
        while position < len(source) and source[position].isspace():
            position += 1
        if position >= len(source) or source[position] != "(":
            continue
        opening = position
        end = _cmake_call_end(source, opening)
        if end is None:
            return calls, True
        line = source.count("\n", 0, start) + 1
        prior = source.rfind("\n", 0, start)
        calls.append(CMakeCall(name.lower(), source[opening + 1 : end], line, start - prior))
        position = end + 1
    return calls, False


def _cmake_tokens(body: str) -> list[str]:
    """Tokenize CMake command arguments closely enough for property pairs."""
    pattern = re.compile(r'"(?:\\.|[^"\\])*"|[^\s()]+')
    tokens: list[str] = []
    for match in pattern.finditer(body):
        token = match.group(0)
        if token.startswith('"') and token.endswith('"'):
            token = token[1:-1]
        tokens.append(token)
    return tokens


def _nearby_comments(lines: list[HashLexLine], line_no: int) -> str:
    """Collect one contiguous rationale immediately before a CMake call."""
    notes: list[str] = []
    index = line_no - 2
    while index >= 0:
        line = lines[index]
        if line.code.strip():
            break
        note = line.comment.strip()
        if not note:
            break
        notes.append(note)
        index -= 1
    return " ".join(reversed(notes))


def _property_pairs(call: CMakeCall) -> tuple[list[str], list[tuple[str, str]]]:
    """Return test targets and property/value pairs from one setter call."""
    tokens = _cmake_tokens(call.body)
    upper = [token.upper() for token in tokens]
    if call.name == "set_tests_properties":
        if "PROPERTIES" not in upper:
            return [], []
        split = upper.index("PROPERTIES")
        targets = tokens[:split]
    elif call.name == "set_property" and upper and upper[0] == "TEST":
        if "PROPERTY" not in upper:
            return [], []
        split = upper.index("PROPERTY")
        targets = [
            item for item in tokens[1:split] if item.upper() not in {"APPEND", "APPEND_STRING"}
        ]
    else:
        return [], []
    values = tokens[split + 1 :]
    return targets, list(zip(values[::2], values[1::2], strict=False))


def _cmake_property_active(name: str, value: str) -> bool:
    """Return whether a recognized CTest property actually softens selection."""
    if name not in {"DISABLED", "WILL_FAIL"}:
        return True
    return value.strip().upper() not in {"0", "FALSE", "OFF", "NO", "N"}


def _ctest_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Inventory active CTest result/skip properties, not getter mentions."""
    item = Path(path)
    if item.name != "CMakeLists.txt" and item.suffix != ".cmake":
        return [], []
    if not CMAKE_CONTROL_HINT.search(text):
        return [], []
    source, lines = _cmake_source(path, text)
    calls, malformed = _cmake_calls(source)
    findings = (
        [Finding("malformed-ctest-control", "unterminated CMake call", path)] if malformed else []
    )
    records: list[Suppression] = []
    for call in calls:
        targets, pairs = _property_pairs(call)
        for raw_name, value in pairs:
            name = raw_name.upper()
            if name not in CTEST_PROPERTIES or not _cmake_property_active(name, value):
                continue
            reason = _nearby_comments(lines, call.line)
            records.append(
                Suppression(
                    path,
                    call.line,
                    call.column,
                    "ctest",
                    "ctest",
                    name,
                    f"{name} {value}",
                    ",".join(targets) or "test",
                    reason,
                    "cmake-ast-lite",
                    ownership(path),
                    _concerns(reason),
                )
            )
    return records, findings


def _yaml_scalar(node: yaml.Node | None) -> str:
    """Return a scalar's source spelling, or an empty marker for structures."""
    return node.value if isinstance(node, yaml.ScalarNode) else ""


def _yaml_true(value: str) -> bool:
    """Return whether one YAML spelling is an unconditional true value."""
    return value.strip().lower() in {"true", "yes", "on", "1"}


def _yaml_false(value: str) -> bool:
    """Return whether one YAML spelling is an unconditional false value."""
    return value.strip().lower() in {"false", "no", "off", "0"}


def _mapping_name(node: yaml.MappingNode, inherited: str) -> str:
    """Return a task/step name from the closest containing mapping."""
    for key, value in node.value:
        if _yaml_scalar(key) == "name" and isinstance(value, yaml.ScalarNode):
            return value.value.strip()
    return inherited


def _yaml_record(
    path: str, key: yaml.ScalarNode, value: yaml.Node, name: str
) -> Suppression | None:
    """Normalize one active workflow or Ansible suppression mapping."""
    control = key.value
    raw = _yaml_scalar(value).strip()
    family = "workflow" if path.startswith(".github/workflows/") else "ansible"
    active = False
    extras: tuple[str, ...] = ()
    if family == "workflow" and control == "continue-on-error":
        active = not _yaml_false(raw)
    elif family == "workflow" and control == "if-no-files-found":
        active = raw.lower() in {"ignore", "warn"}
    elif family == "ansible" and control in {"ignore_errors", "no_log"}:
        active = _yaml_true(raw) or (bool(raw) and not _yaml_false(raw))
        extras = ("broad-result-mask",) if control == "ignore_errors" else ("broad-output-mask",)
    elif family == "ansible" and control in {"failed_when", "changed_when"}:
        active = _yaml_false(raw)
        if control == "failed_when":
            extras = ("broad-result-mask",)
    if not active:
        return None
    return Suppression(
        path,
        key.start_mark.line + 1,
        key.start_mark.column + 1,
        family,
        "github-actions" if family == "workflow" else "ansible",
        control if control != "if-no-files-found" else raw.lower(),
        f"{control}: {raw}",
        "step" if family == "workflow" else "task",
        name,
        "yaml-compose",
        ownership(path),
        _concerns(name, *extras),
    )


def _walk_yaml(node: yaml.Node, path: str, inherited: str, seen: set[int]) -> list[Suppression]:
    """Walk composed YAML nodes while retaining source locations and names."""
    if id(node) in seen:
        return []
    seen.add(id(node))
    records: list[Suppression] = []
    if isinstance(node, yaml.MappingNode):
        name = _mapping_name(node, inherited)
        for key, value in node.value:
            if isinstance(key, yaml.ScalarNode):
                record = _yaml_record(path, key, value, name)
                if record is not None:
                    records.append(record)
            records.extend(_walk_yaml(value, path, name, seen))
    elif isinstance(node, yaml.SequenceNode):
        for value in node.value:
            records.extend(_walk_yaml(value, path, inherited, seen))
    return records


def _yaml_controls(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Compose workflow/Ansible YAML and inventory only active mapping keys."""
    relevant = path.startswith(YAML_CONTROL_ROOTS)
    if not relevant or Path(path).suffix not in {".yml", ".yaml"}:
        return [], []
    if not YAML_CONTROL_HINT.search(text):
        return [], []
    try:
        documents = list(yaml.compose_all(text))
    except yaml.YAMLError as exc:
        return [], [Finding("malformed-yaml-control", str(exc), path)]
    records: list[Suppression] = []
    for document in documents:
        if document is not None:
            records.extend(_walk_yaml(document, path, "", set()))
    return records, []


def scan_control_file(path: str, text: str) -> tuple[list[Suppression], list[Finding]]:
    """Run every non-overlapping test/infrastructure control recognizer."""
    records: list[Suppression] = []
    findings: list[Finding] = []
    for scanner in (_python_controls, _ctest_controls, _yaml_controls):
        scanned, errors = scanner(path, text)
        records.extend(scanned)
        findings.extend(errors)
    return records, findings
