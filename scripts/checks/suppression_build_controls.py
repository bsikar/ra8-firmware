# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Inventory active warning controls in shell, CMake, Make, and YAML syntax."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from suppression_catalog import (
    BLANKET_WARNING_RE,
    WARNING_FLAG_RE,
    is_build_control,
    is_shell_control,
    ownership,
)
from suppression_hash_lex import hash_lines
from suppression_model import Suppression

_COMPILER_NAME = r"(?:[A-Za-z0-9_.+-]*-)?(?:cc|c\+\+|gcc|g\+\+|clang|clang\+\+)"
_COMPILER_CONTEXT_RE = re.compile(
    r"\b[A-Za-z0-9_]*(?:CFLAGS|CXXFLAGS|CPPFLAGS|COMPILE_OPTIONS)\b"
    r"|--extra-arg(?:-before)?=|(?:^|[\s(])-(?:D|I|f|std=)",
    re.IGNORECASE,
)
_COMPILER_OWNER_RE = re.compile(
    r"\b[A-Za-z0-9_]*(?:CFLAGS|CXXFLAGS|CPPFLAGS|COMPILE_OPTIONS)\b"
    r"|--extra-arg(?:-before)?=",
    re.IGNORECASE,
)
_COMPILER_OPTION_RE = re.compile(r"(?:^|[\s(])-(?:D|I|f|std=)")
_CMAKE_VARIABLES = frozenset(("$CMAKE", "$" + "{CMAKE}", "$" + "{CMAKE_COMMAND}"))
_CMAKE_WARNING_SUPPRESSIONS = frozenset(
    {
        "-Wno-deprecated",
        "-Wno-dev",
        "-Wno-error=deprecated",
        "-Wno-error=dev",
    }
)
_COMMAND_PREFIXES = frozenset(
    {"!", "COMMAND", "command", "do", "elif", "env", "exec", "if", "then"}
)
_SHELL_OPERATORS = frozenset({"&", "&&", "(", ")", ";", ";;", "|", "||"})
_YAML_KEY_RE = re.compile(r"^\s*(?:-\s*)?(?P<key>[A-Za-z0-9_.-]+)\s*:\s*(?P<value>.*)$")
_YAML_BLOCK_RE = re.compile(
    r"^(?P<indent>\s*)(?:-\s*)?(?P<key>[A-Za-z0-9_.-]+)\s*:\s*"
    r"(?P<indicator>[>|])(?P<modifiers>(?:[+-][1-9]?|[1-9][+-]?))?"
    r"\s*(?:#.*)?$"
)
_CMAKE_COMPILER_RE = re.compile(
    r"\b(?:add_compile_options|target_compile_options)\s*\("
    r"|\b(?:CMAKE_[A-Za-z0-9_]*(?:C|CXX|ASM)[A-Za-z0-9_]*FLAGS|COMPILE_OPTIONS)\b"
    r"|\b(?:set|list)\s*\([^\n)]*(?:WNO[A-Za-z0-9_]*|C_FLAGS|CXX_FLAGS|WARNING_FLAGS)\b",
    re.IGNORECASE,
)
_CMAKE_NONCONFIGURE_RE = re.compile(
    r"(?:^|\s)(?:--build|--install|--open|--find-package|--help(?:-[A-Za-z-]+)?|"
    r"--version|--system-information|-E|-P)(?=$|\s)"
)


@dataclass(frozen=True)
class ActiveBuildCode:
    """One flag-bearing source line and the context that owns it."""

    path: str
    line: int
    code: str
    source_offset: int
    blanket_is_compiler: bool
    context: str
    reason: str = ""


@dataclass(frozen=True)
class YamlCommandLine:
    """One source-mapped line from a YAML-owned shell command."""

    line: int
    code: str
    source_offset: int
    context: str = ""
    reason: str = ""


@dataclass(frozen=True)
class ShellToken:
    """One quote-decoded shell word or command-separating operator."""

    value: str
    start: int
    end: int
    operator: bool = False


@dataclass(frozen=True)
class ToolCommand:
    """One compiler or CMake executable occupying a command-word position."""

    kind: str
    start: int
    end: int


def _shell_tokens(source: str) -> list[ShellToken]:
    """Tokenize shell command words with quote and backslash semantics."""
    tokens: list[ShellToken] = []
    index = 0
    while index < len(source):
        if source[index].isspace():
            index += 1
            continue
        pair = source[index : index + 2]
        if pair in _SHELL_OPERATORS:
            tokens.append(ShellToken(pair, index, index + 2, operator=True))
            index += 2
            continue
        if source[index] in _SHELL_OPERATORS:
            tokens.append(ShellToken(source[index], index, index + 1, operator=True))
            index += 1
            continue
        start = index
        value: list[str] = []
        while index < len(source):
            char = source[index]
            if char.isspace() or char in _SHELL_OPERATORS:
                break
            if char in {"'", '"'}:
                quote = char
                index += 1
                while index < len(source) and source[index] != quote:
                    char = source[index]
                    if char == "\\" and quote == '"' and index + 1 < len(source):
                        escaped = source[index + 1]
                        if escaped in {'"', "$", chr(96), "\\", "\n"}:
                            value.append(escaped)
                            index += 2
                            continue
                    value.append(char)
                    index += 1
                if index >= len(source):
                    return tokens
                index += 1
                continue
            if char == "\\" and index + 1 < len(source):
                value.append(source[index + 1])
                index += 2
                continue
            value.append(char)
            index += 1
        tokens.append(ShellToken("".join(value), start, index))
    return tokens


def _tool_kind(value: str) -> str | None:
    """Classify one decoded command word by executable basename."""
    if value in _CMAKE_VARIABLES:
        return "cmake"
    basename = re.split(r"[/\\]", value)[-1]
    if basename == "cmake":
        return "cmake"
    if re.fullmatch(_COMPILER_NAME, basename, re.IGNORECASE) is not None:
        return "compiler"
    return None


def _is_assignment(value: str) -> bool:
    """Return whether a shell word is an environment assignment."""
    return re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=.*", value, re.DOTALL) is not None


def _tool_commands(source: str) -> list[ToolCommand]:
    """Return tools in executable positions, excluding data arguments."""
    commands: list[ToolCommand] = []
    expect_command = True
    wrapper = ""
    active_kind: str | None = None
    cmake_dash_e = False
    for token in _shell_tokens(source):
        if token.operator:
            expect_command = True
            wrapper = ""
            active_kind = None
            cmake_dash_e = False
            continue
        basename = re.split(r"[/\\]", token.value)[-1]
        if expect_command:
            if _is_assignment(token.value):
                continue
            if basename in _COMMAND_PREFIXES:
                wrapper = basename
                continue
            if wrapper and token.value.startswith("-"):
                continue
            kind = _tool_kind(token.value)
            if kind is not None:
                commands.append(ToolCommand(kind, token.start, token.end))
            active_kind = kind
            expect_command = False
            wrapper = ""
            cmake_dash_e = False
            continue
        if active_kind == "cmake" and token.value == "-E":
            cmake_dash_e = True
        elif active_kind == "cmake" and cmake_dash_e and basename == "env":
            expect_command = True
            wrapper = "env"
            active_kind = None
            cmake_dash_e = False
    return commands


def _has_compiler_context(source: str) -> bool:
    """Return whether source owns a compiler command or option context."""
    return _COMPILER_CONTEXT_RE.search(source) is not None or any(
        command.kind == "compiler" for command in _tool_commands(source)
    )


def _cmake_commands(source: str) -> list[ToolCommand]:
    """Return source-mapped CMake executables in command positions."""
    return [command for command in _tool_commands(source) if command.kind == "cmake"]


def _has_cmake_context(source: str) -> bool:
    """Return whether source contains an active CMake command."""
    return bool(_cmake_commands(source))


def _is_cmake_configure_context(source: str) -> bool:
    """Return whether source invokes CMake's configure mode."""
    commands = _cmake_commands(source)
    if not commands:
        return False
    invocation = source[commands[-1].end :]
    return _CMAKE_NONCONFIGURE_RE.search(invocation) is None


def _control_kind_at(source: str, flag_position: int) -> str | None:
    """Return the nearest active compiler/CMake command owning one flag."""
    prefix = source[:flag_position]
    compiler_positions = [match.start() for match in _COMPILER_OWNER_RE.finditer(prefix)]
    compiler_positions.extend(
        command.start for command in _tool_commands(prefix) if command.kind == "compiler"
    )
    compiler_positions.extend(match.start() for match in _CMAKE_COMPILER_RE.finditer(prefix))
    cmake_matches = _cmake_commands(prefix)
    compiler_position = max(compiler_positions, default=-1)
    cmake_match = cmake_matches[-1] if cmake_matches else None
    if cmake_match is not None and cmake_match.start > compiler_position:
        invocation = prefix[cmake_match.end :]
        if _CMAKE_NONCONFIGURE_RE.search(invocation) is None:
            return "cmake"
        return None
    if compiler_position >= 0:
        return "compiler"
    if _COMPILER_OPTION_RE.search(prefix) is not None:
        return "compiler"
    return None


def _is_cmake_warning_control(source: str, flag_position: int, flag: str) -> bool:
    """Return whether one exact CMake diagnostic option owns the match."""
    if flag not in _CMAKE_WARNING_SUPPRESSIONS:
        return False
    return any(
        not token.operator and token.value == flag and token.start <= flag_position < token.end
        for token in _shell_tokens(source)
    )


def _folded_yaml_commands(
    block: list[tuple[int, str, int]], content_indent: int
) -> list[YamlCommandLine]:
    """Join folded YAML paragraphs while retaining source and rationale."""
    commands: list[YamlCommandLine] = []
    group: list[tuple[int, str, int]] = []
    pending_reason = ""

    def flush() -> None:
        nonlocal pending_reason
        if not group:
            return
        pieces: list[str] = []
        spans: list[tuple[int, int, int]] = []
        cursor = 0
        for line_no, content, _indent in group:
            if pieces:
                cursor += 1
            start = cursor
            pieces.append(content)
            cursor += len(content)
            spans.append((line_no, start, cursor))
        joined = " ".join(pieces)
        lexical_lines, _ = hash_lines("yaml-command.sh", joined + "\n")
        lexical = lexical_lines[0] if lexical_lines else None
        active = lexical.code if lexical is not None else ""
        comment = lexical.comment.strip() if lexical is not None else ""
        if not active.strip():
            pending_reason = (
                _inline_reason(comment) if comment.startswith("Suppression rationale:") else ""
            )
            group.clear()
            return
        reason = _inline_reason(comment) or pending_reason
        for line_no, start, end in spans:
            active_end = min(end, len(active))
            code = active[start:active_end] if start < active_end else ""
            commands.append(
                YamlCommandLine(
                    line_no,
                    code,
                    content_indent,
                    active[:active_end],
                    reason,
                )
            )
        pending_reason = ""
        group.clear()

    for source in block:
        _line_no, content, indent = source
        if not content.strip() or indent > content_indent:
            flush()
            if content.strip():
                group.append(source)
                flush()
            continue
        group.append(source)
    flush()
    return commands


def _yaml_command_lines(text: str) -> list[YamlCommandLine]:
    """Return de-indented shell owned only by active run/shell block keys."""
    raw_lines = text.splitlines()
    commands: list[YamlCommandLine] = []
    index = 0
    while index < len(raw_lines):
        header = _YAML_BLOCK_RE.match(raw_lines[index])
        if header is None:
            index += 1
            continue
        header_indent = len(header.group("indent"))
        key = header.group("key").lower()
        block: list[tuple[int, str, int]] = []
        index += 1
        content_indent: int | None = None
        while index < len(raw_lines):
            raw = raw_lines[index]
            indent = len(raw) - len(raw.lstrip(" "))
            if raw.strip():
                if content_indent is None:
                    if indent <= header_indent:
                        break
                    content_indent = indent
                elif indent < content_indent:
                    break
            if content_indent is not None:
                block.append((index + 1, raw[content_indent:], indent))
            elif not raw.strip():
                block.append((index + 1, "", 0))
            index += 1
        if key not in {"run", "shell"} or content_indent is None:
            continue
        if header.group("indicator") == ">":
            commands.extend(_folded_yaml_commands(block, content_indent))
            continue
        # Preserve a final blank payload line: str.splitlines(), used by the
        # shell lexer, otherwise drops it and breaks source-line ownership.
        shell_text = "\n".join(line for _, line, _ in block) + "\n"
        shell_lines, _ = hash_lines("yaml-command.sh", shell_text)
        for line_index, (source, lexical) in enumerate(zip(block, shell_lines, strict=True)):
            reason = _inline_reason(lexical.comment) or _structured_reason_above(
                shell_lines, line_index
            )
            commands.append(
                YamlCommandLine(
                    source[0],
                    lexical.code,
                    content_indent,
                    reason=reason,
                )
            )
    return commands


def _active_build_code(path: str, code: str, inherited_context: str = "") -> tuple[str, bool, int]:
    """Return flag-bearing code, blanket -w status, and its source offset."""
    item = Path(path)
    if item.name == "CMakeLists.txt" or item.suffix == ".cmake":
        source = f"{inherited_context} {code}"
        active = _CMAKE_COMPILER_RE.search(source) is not None or _is_cmake_configure_context(
            source
        )
        return (code, True, 0) if active else ("", False, 0)
    if item.suffix in {".yaml", ".yml"}:
        match = _YAML_KEY_RE.match(code)
        if match is None:
            return "", False, 0
        key = match.group("key").lower()
        value = match.group("value")
        flag_key = key.endswith(("cflags", "cxxflags", "cppflags", "compile_options"))
        command_key = key in {"command", "run", "shell"}
        command_context = _has_compiler_context(value) or _has_cmake_context(value)
        if flag_key or (command_key and command_context):
            return value, True, match.start("value")
        return "", False, 0
    active_source = f"{inherited_context} {code}"
    active = _has_compiler_context(active_source) or _has_cmake_context(active_source)
    return (code, True, 0) if active else ("", False, 0)


def _inline_reason(comment: str) -> str:
    """Return an attached reason, normalizing the structured prefix."""
    prefix = "Suppression rationale:"
    reason = comment.strip()
    if not reason:
        return ""
    if reason.startswith(prefix):
        return reason.removeprefix(prefix).strip()
    return reason


def _structured_reason_above(lines: list[object], index: int) -> str:
    """Return one explicit rationale block immediately above a command."""
    prefix = "Suppression rationale:"
    notes: list[str] = []
    for candidate in reversed(lines[:index]):
        code = getattr(candidate, "code", "")
        comment = getattr(candidate, "comment", "").strip()
        if code.strip() or not comment:
            break
        notes.append(comment)
    notes.reverse()
    for note_index, note in enumerate(notes):
        if not note.startswith(prefix):
            continue
        first = note.removeprefix(prefix).strip()
        if not first:
            return ""
        return " ".join([first, *notes[note_index + 1 :]]).strip()
    return ""


def _reason_concerns(flag: str, reason: str) -> tuple[str, ...]:
    """Report missing reasons and retain the blanket-warning concern."""
    concerns = [] if reason else ["blank-reason"]
    if flag == "-w":
        concerns.append("broad-rule")
    return tuple(concerns)


def _append_records(
    records: list[Suppression],
    source: ActiveBuildCode,
) -> None:
    """Append warning-control records with truthful CMake/compiler ownership."""
    matches = list(WARNING_FLAG_RE.finditer(source.code))
    if source.blanket_is_compiler:
        matches.extend(BLANKET_WARNING_RE.finditer(source.code))
    reason = _inline_reason(source.reason)
    for match in sorted(matches, key=lambda item: item.start()):
        flag = match.group("flag")
        code_position = source.context.rfind(source.code)
        if code_position < 0:
            continue
        control_kind = _control_kind_at(source.context, code_position + match.start())
        if control_kind is None:
            continue
        flag_position = code_position + match.start()
        cmake_control = control_kind == "cmake" and _is_cmake_warning_control(
            source.context, flag_position, flag
        )
        family = "cmake" if cmake_control else "compiler"
        records.append(
            Suppression(
                source.path,
                source.line,
                source.source_offset + match.start() + 1,
                family,
                family,
                flag,
                flag,
                "configure-command" if cmake_control else "build-target",
                reason,
                "build-config",
                ownership(source.path),
                _reason_concerns(flag, reason),
            )
        )


def _continued_build_context(
    line: object,
    current: str,
    *,
    shell_control: bool,
    cmake_control: bool,
) -> str:
    """Return the active command context carried to the next source line."""
    code = getattr(line, "code", "")
    active_source = f"{current} {code}".strip()
    if shell_control and code.rstrip().endswith("\\"):
        return f"{current} {code.rstrip()[:-1]}".strip()
    if (
        cmake_control
        and (current or code.count("(") > code.count(")"))
        and active_source.count("(") > active_source.count(")")
    ):
        return active_source
    return ""


def _append_line_build_records(
    records: list[Suppression], path: str, text: str, first_line: str
) -> None:
    """Append controls found in ordinary build-control source lines."""
    lines, _ = hash_lines(path, text)
    shell_control = is_shell_control(path, first_line)
    cmake_control = Path(path).name == "CMakeLists.txt" or Path(path).suffix == ".cmake"
    continued_context = ""
    continued_reason = ""
    for index, line in enumerate(lines):
        structured_reason = _structured_reason_above(lines, index)
        line_reason = _inline_reason(line.comment) or continued_reason or structured_reason
        code, blanket_is_compiler, source_offset = _active_build_code(
            path, line.code, continued_context
        )
        active_source = f"{continued_context} {line.code}"
        _append_records(
            records,
            ActiveBuildCode(
                path=path,
                line=line.line,
                code=code,
                source_offset=source_offset,
                blanket_is_compiler=blanket_is_compiler,
                context=active_source,
                reason=line_reason,
            ),
        )
        next_context = _continued_build_context(
            line,
            continued_context,
            shell_control=shell_control,
            cmake_control=cmake_control,
        )
        if next_context and not continued_context:
            continued_reason = _inline_reason(line.comment) or structured_reason
        elif not next_context:
            continued_reason = ""
        continued_context = next_context


def _append_yaml_build_records(records: list[Suppression], path: str, text: str) -> None:
    """Append controls from YAML-owned shell blocks with source-line identity."""
    block_context = ""
    for command in _yaml_command_lines(text):
        active_source = command.context or f"{block_context} {command.code}"
        active = _has_compiler_context(active_source) or _is_cmake_configure_context(active_source)
        if active:
            _append_records(
                records,
                ActiveBuildCode(
                    path=path,
                    line=command.line,
                    code=command.code,
                    source_offset=command.source_offset,
                    blanket_is_compiler=True,
                    context=active_source,
                    reason=command.reason,
                ),
            )
        if command.context:
            block_context = ""
        elif command.code.rstrip().endswith("\\"):
            block_context = f"{block_context} {command.code.rstrip()[:-1]}".strip()
        else:
            block_context = ""


def compiler_records(path: str, text: str) -> list[Suppression]:
    """Inventory active warning-disable flags in build-control syntax."""
    first_line = text.partition("\n")[0]
    if not is_build_control(path, first_line):
        return []
    records: list[Suppression] = []
    _append_line_build_records(records, path, text, first_line)
    if Path(path).suffix in {".yaml", ".yml"}:
        _append_yaml_build_records(records, path, text)
    return records
