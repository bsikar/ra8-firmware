# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Structural macOS recursive-cleanup policy and adversarial fixtures."""

from __future__ import annotations

import re
import shlex

from check_shebangs import PRIVILEGED_BODY_PREFIX

MAC_SETUP = "scripts/emu/setup_macos.sh"
MAC_CANONICAL_CLEANUP = r""" {
  if [[ "$#" -ne 2 ]]; then
    echo "ERROR: cleanup validation requires one target and one allowed root." >&2
    return 1
  fi
  local target="$1" allowed_root="$2"
  local root_name root_parent root_physical target_name target_parent canonical
  if [[ "$target" != /* ]] || [[ "$allowed_root" != /* ]] || [[ "$allowed_root" == "/" ]]; then
    echo "ERROR: refusing relative target or unsafe cleanup root." >&2
    return 1
  fi
  root_name="$(basename -- "$allowed_root")"
  target_name="$(basename -- "$target")"
  if [[ -z "$root_name" ]] || [[ "$root_name" == "." ]] || [[ "$root_name" == ".." ]] ||
    [[ -z "$target_name" ]] || [[ "$target_name" == "." ]] || [[ "$target_name" == ".." ]]; then
    echo "ERROR: refusing unsafe cleanup path component." >&2
    return 1
  fi
  root_parent="$(cd "$(dirname -- "$allowed_root")" && pwd -P)" || return 1
  root_physical="${root_parent}/${root_name}"
  if [[ -e "$allowed_root" ]] || [[ -L "$allowed_root" ]]; then
    if [[ ! -d "$allowed_root" ]] || [[ -L "$allowed_root" ]] ||
      [[ "$(cd "$allowed_root" && pwd -P)" != "$root_physical" ]]; then
      echo "ERROR: cleanup root is not a physical directory: $allowed_root" >&2
      return 1
    fi
  fi
  if [[ -d "$(dirname -- "$target")" ]]; then
    target_parent="$(cd "$(dirname -- "$target")" && pwd -P)" || return 1
  elif [[ "$(dirname -- "$target")" == "$root_physical" ]] && [[ ! -e "$allowed_root" ]]; then
    target_parent="$root_physical"
  else
    echo "ERROR: cleanup target parent cannot be canonicalized: $target" >&2
    return 1
  fi
  canonical="${target_parent}/${target_name}"
  if [[ "$target_parent" != "$root_physical" ]] || [[ -L "$canonical" ]] ||
    { [[ -e "$canonical" ]] && [[ ! -d "$canonical" ]]; }; then
    echo "ERROR: cleanup target escapes its allowed root: $target" >&2
    return 1
  fi
  printf '%s\n' "$canonical"
}"""
MAC_SAFE_REMOVE_TREE = r""" {
  if [[ "$#" -ne 2 ]]; then
    echo "ERROR: safe removal requires one target and one allowed root." >&2
    return 1
  fi
  local target="$1" allowed_root="$2" canonical
  canonical="$(canonical_cleanup_target "$target" "$allowed_root")" || return 1
  if [[ "$canonical" != "$target" ]]; then
    echo "ERROR: cleanup target changed after validation: $target" >&2
    return 1
  fi
  command rm -rf -- "$canonical"
}"""

SENSITIVE_HELPER_DEFINITIONS = (
    ("dockerfile_arg", "dockerfile_arg() {"),
    ("require_arm_hash_pins", "require_arm_hash_pins() {"),
    ("canonical_cleanup_target", "canonical_cleanup_target() {"),
    ("safe_remove_tree", "safe_remove_tree() {"),
    ("install_homebrew", "install_homebrew() ("),
)
SENSITIVE_HELPER_NAMES = tuple(name for name, _header in SENSITIVE_HELPER_DEFINITIONS)
_REEXEC_START = next(
    index for index, line in enumerate(PRIVILEGED_BODY_PREFIX) if "exec /usr/bin/env" in line
)
_REEXEC_LINES = PRIVILEGED_BODY_PREFIX[_REEXEC_START : _REEXEC_START + 3]
_REEXEC_LITERAL = " ".join(line.removesuffix("\\").strip() for line in _REEXEC_LINES)
_REEXEC_SOURCE = _REEXEC_LITERAL.removeprefix("if ! ").removesuffix("; then")
_REEXEC_WORDS = tuple(shlex.split(_REEXEC_SOURCE))
_REEXEC_RECORD = "if ! " + " ".join(_REEXEC_WORDS)
ALLOWED_INDIRECT_EXECUTION = (
    (
        _REEXEC_RECORD,
        (),
        _REEXEC_WORDS[0],
        _REEXEC_WORDS[1:],
    ),
)


CLEANUP_NAMES = (
    "home_root",
    "arm_root",
    "arm_prefix",
    "work_candidate",
    "work_root",
    "work",
    "arm_work_candidate",
    "arm_work_root",
    "arm_work",
    "emu_root",
    "emu_build",
)
CLEANUP_STATIC_READONLY = ('readonly arm_root="${home_root}/opt"',)
CLEANUP_ASSIGNMENT_PAIRS = (
    (
        'home_root="$(cd "$HOME" && pwd -P)" || exit 1',
        "readonly home_root",
    ),
    (
        'arm_prefix="$(canonical_cleanup_target '
        '"${arm_root}/arm-gnu-toolchain-${arm_version}" "$arm_root")" || exit 1',
        "readonly arm_prefix",
    ),
    (
        'work_candidate="$(mktemp -d)" || return 1',
        "readonly work_candidate",
    ),
    (
        'work_root="$(cd "$(dirname -- "$work_candidate")" && pwd -P)" || return 1',
        "readonly work_root",
    ),
    (
        'work="$(canonical_cleanup_target "$work_candidate" "$work_root")" || return 1',
        "readonly work",
    ),
    (
        'arm_work_candidate="$(mktemp -d)" || exit 1',
        "readonly arm_work_candidate",
    ),
    (
        'arm_work_root="$(cd "$(dirname -- "$arm_work_candidate")" && pwd -P)" || exit 1',
        "readonly arm_work_root",
    ),
    (
        'arm_work="$(canonical_cleanup_target "$arm_work_candidate" "$arm_work_root")" || exit 1',
        "readonly arm_work",
    ),
    (
        'emu_root="$(cd "$root/tools/ra8_emulator" && pwd -P)" || exit 1',
        "readonly emu_root",
    ),
    (
        'emu_build="$(canonical_cleanup_target "${emu_root}/build" "$emu_root")" || exit 1',
        "readonly emu_build",
    ),
)
CLEANUP_DEFINITION_LINES = CLEANUP_STATIC_READONLY + tuple(
    line for pair in CLEANUP_ASSIGNMENT_PAIRS for line in pair
)
CLEANUP_DEFINITION_ORDER = (
    (
        *CLEANUP_ASSIGNMENT_PAIRS[0],
        'if [[ -z "$home_root" ]]; then',
        CLEANUP_STATIC_READONLY[0],
        *CLEANUP_ASSIGNMENT_PAIRS[1],
        'if [[ -z "$arm_prefix" ]]; then',
        'safe_remove_tree "$arm_prefix" "$arm_root"',
    ),
    (
        *CLEANUP_ASSIGNMENT_PAIRS[2],
        'if [[ -z "$work_candidate" ]]; then',
        *CLEANUP_ASSIGNMENT_PAIRS[3],
        'if [[ -z "$work_root" ]]; then',
        *CLEANUP_ASSIGNMENT_PAIRS[4],
        '[[ -n "$work" ]] || return 1',
        'trap \'safe_remove_tree "${work}" "${work_root}"\' EXIT',
    ),
    (
        *CLEANUP_ASSIGNMENT_PAIRS[5],
        'if [[ -z "$arm_work_candidate" ]]; then',
        *CLEANUP_ASSIGNMENT_PAIRS[6],
        'if [[ -z "$arm_work_root" ]]; then',
        *CLEANUP_ASSIGNMENT_PAIRS[7],
        '[[ -n "$arm_work" ]] || exit 1',
        'trap \'safe_remove_tree "$arm_work" "$arm_work_root"\' EXIT',
    ),
    (
        *CLEANUP_ASSIGNMENT_PAIRS[8],
        'if [[ -z "$emu_root" ]]; then',
        *CLEANUP_ASSIGNMENT_PAIRS[9],
        '[[ -n "$emu_build" ]] || exit 1',
        'safe_remove_tree "$emu_build" "$emu_root"',
    ),
)


def _active(text: str) -> str:
    """Discard comment-only lines and join shell continuations."""
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    return re.sub(r"\\\n\s*", " ", code)


def _normalise_shell(text: str) -> str:
    """Collapse insignificant shell whitespace for exact statement checks."""
    return " ".join(_active(text).split())


def _section(text: str, start: str, end: str) -> str:
    """Return a required text section, or an empty string when anchors drift."""
    _before, marker, rest = text.partition(start)
    if not marker:
        return ""
    body, marker, _after = rest.partition(end)
    return body if marker else ""


def _without_heredocs(text: str) -> str:
    """Remove literal heredoc bodies before structural command decoding."""
    kept: list[str] = []
    delimiter: str | None = None
    for line in text.splitlines():
        if delimiter is not None:
            if line.strip() == delimiter:
                delimiter = None
            continue
        kept.append(line)
        match = re.search(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1", line)
        if match:
            delimiter = match.group(2)
    return "\n".join(kept)


def _protect_parameter_lengths(text: str) -> str:
    """Keep Bash ``${#name}`` expansions from becoming shlex comments."""
    return re.sub(r"\$\{#([A-Za-z_][A-Za-z0-9_]*)(\[@\])?\}", r"${RA8_LENGTH_\1\2}", text)


def _shell_words(text: str) -> tuple[str, ...]:
    """Decode shell quoting/escaping into conservative lexical words."""
    code = _protect_parameter_lengths(_active(_without_heredocs(text)))
    lexer = shlex.shlex(code, posix=True, punctuation_chars=";&|()<>")
    lexer.commenters = "#"
    lexer.whitespace_split = True
    try:
        return tuple(word for word in lexer if word.strip(";&|()<>"))
    except ValueError:
        return ()


def _shell_segments(text: str) -> tuple[tuple[str, tuple[str, ...]], ...] | None:
    """Split decoded shell commands while respecting multiline quoting."""
    code = _protect_parameter_lengths(_active(_without_heredocs(text)))
    lexer = shlex.shlex(code, posix=True, punctuation_chars=";&|\n")
    lexer.commenters = "#"
    lexer.whitespace = " \t\r"
    lexer.whitespace_split = True
    try:
        words = tuple(lexer)
    except ValueError:
        return None
    segments: list[tuple[str, tuple[str, ...]]] = []
    start = 0
    for index, word in enumerate((*words, ";")):
        if set(word) <= set(";&|\n"):
            if index > start:
                command = words[start:index]
                segments.append((" ".join(command), command))
            start = index + 1
    return tuple(segments)


def _consume_env_options(pending: list[str]) -> tuple[str, ...] | None:
    """Consume ordinary env options, returning unsafe split-string arguments."""
    while pending:
        option = pending[0]
        if option.startswith(("-S", "--split-string")):
            return tuple(pending)
        if option in {"-u", "--unset", "-C", "--chdir"}:
            del pending[:2]
            continue
        if option == "--":
            pending.pop(0)
            break
        if option.startswith("-") or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=.*", option):
            pending.pop(0)
            continue
        break
    return None


def _normalise_command(
    words: tuple[str, ...],
) -> tuple[tuple[str, ...], str, tuple[str, ...]] | None:
    """Return wrapper prefixes, decoded command, and arguments."""
    pending = list(words)
    while pending and pending[0] in {"!", "{", "}", "if", "elif", "then", "while", "until", "do"}:
        pending.pop(0)
    while pending and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*=.*", pending[0]):
        pending.pop(0)
    prefixes: list[str] = []
    while pending:
        command = pending.pop(0).rsplit("/", 1)[-1]
        if command == "command":
            prefixes.append(command)
            if pending and pending[0] in {"-v", "-V"}:
                return tuple(prefixes), "command-v", tuple(pending[1:])
            while pending and pending[0] in {"--", "-p"}:
                pending.pop(0)
            continue
        if command == "builtin":
            prefixes.append(command)
            if pending and pending[0] == "--":
                pending.pop(0)
            continue
        if command == "env":
            prefixes.append(command)
            dynamic = _consume_env_options(pending)
            if dynamic is not None:
                return tuple(prefixes), "env-dynamic", dynamic
            continue
        return tuple(prefixes), command, tuple(pending)
    return None


def _command_records(
    text: str,
) -> tuple[tuple[str, tuple[str, ...], str, tuple[str, ...]], ...] | None:
    """Return decoded command records after normalizing execution wrappers."""
    segments = _shell_segments(text)
    if segments is None:
        return None
    records = []
    for line, words in segments:
        command = _normalise_command(words)
        if command is not None:
            prefixes, name, arguments = command
            records.append((line, prefixes, name, arguments))
    return tuple(records)


def _recursive_option(word: str) -> bool:
    """Return whether one decoded word enables recursive removal."""
    return bool(re.fullmatch(r"--recursive(?:=.*)?|-[A-Za-z]*[rR][A-Za-z]*", word))


def _dynamic_command(command: str) -> bool:
    """Return whether a decoded command is selected through expansion."""
    return command.startswith("$") or "$(" in command or "`" in command


def _shell_interpreter(command: str) -> bool:
    """Return whether a normalized basename can execute shell source with -c."""
    return command.endswith("sh") or command in {"fish", "nu"}


def _sensitive_function_definitions(text: str) -> tuple[tuple[str, str], ...]:
    """Return active definitions of helpers that anchor the safety policy."""
    names = "|".join(re.escape(name) for name in SENSITIVE_HELPER_NAMES)
    pattern = re.compile(
        rf"(?m)^[ \t]*(?:"
        rf"function[ \t]+({names})(?:[ \t]*\(\))?|"
        rf"({names})[ \t]*\(\)"
        r")[ \t\r\n]*(?:\{|\()"
    )
    active = _active(_without_heredocs(text))
    return tuple(
        (match.group(1) or match.group(2), match.group(0).strip())
        for match in pattern.finditer(active)
    )


def _function_flag(argument: str) -> bool:
    """Return whether one builtin option selects function attributes."""
    return argument in {"--function", "--functions"} or bool(
        re.fullmatch(r"-[A-Za-z]*[fF][A-Za-z]*", argument)
    )


def _check_sensitive_helpers(text: str) -> list[str]:
    """Require one canonical definition and forbid function-attribute mutation."""
    findings: list[str] = []
    if _sensitive_function_definitions(text) != SENSITIVE_HELPER_DEFINITIONS:
        findings.append(f"{MAC_SETUP}: sensitive helper definition inventory is not exact")
    records = _command_records(text)
    function_builtins = {"unset", "readonly", "declare", "typeset", "local", "export"}
    if records is not None and any(
        command in function_builtins
        and any(_function_flag(argument) for argument in arguments)
        and any(
            argument in SENSITIVE_HELPER_NAMES or argument.startswith("$") for argument in arguments
        )
        for _line, _prefixes, command, arguments in records
    ):
        findings.append(f"{MAC_SETUP}: sensitive helper function mutation is forbidden")
    return findings


def _check_cleanup_tokens(text: str) -> list[str]:
    """Reject direct, prefixed, or dynamically assembled execution surfaces."""
    findings: list[str] = []
    words = _shell_words(text)
    direct_rm = tuple(word for word in words if word.rsplit("/", 1)[-1] == "rm")
    if direct_rm != ("rm",):
        findings.append(f"{MAC_SETUP}: raw recursive removal inventory is not exact")
    records = _command_records(text)
    if records is None:
        return [*findings, f"{MAC_SETUP}: shell command structure cannot be decoded"]
    evals = tuple((line, prefixes) for line, prefixes, cmd, _args in records if cmd == "eval")
    if evals != (("eval $($brew shellenv)", ()),):
        findings.append(f"{MAC_SETUP}: eval inventory is not exact")
    dynamic = tuple(
        (line, prefixes, cmd) for line, prefixes, cmd, _args in records if _dynamic_command(cmd)
    )
    if dynamic != (("$brew install ${brew_missing[@]}", (), "$brew"),):
        findings.append(f"{MAC_SETUP}: dynamic command inventory is not exact")
    if any(
        _shell_interpreter(cmd) and any(re.fullmatch(r"-[A-Za-z]*c[A-Za-z]*", arg) for arg in args)
        for _line, _prefixes, cmd, args in records
    ):
        findings.append(f"{MAC_SETUP}: dynamic shell wrapper is forbidden")
    indirect = tuple(
        record
        for record in records
        if record[2] in {"env-dynamic", "exec", "source", ".", "xargs", "parallel"}
    )
    if indirect != ALLOWED_INDIRECT_EXECUTION:
        findings.append(f"{MAC_SETUP}: indirect execution inventory is not exact")
    if any(words[index : index + 2] == ("set", "--") for index in range(len(words) - 1)):
        findings.append(f"{MAC_SETUP}: positional-command indirection is forbidden")
    assignments = (
        word.split("=", 1)[1] for word in words if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", word)
    )
    if any(value.rsplit("/", 1)[-1] == "rm" or _recursive_option(value) for value in assignments):
        findings.append(f"{MAC_SETUP}: command/flag assignment indirection is forbidden")
    if re.search(r"\$\{[^}\n]*(?::?[-+=?])(?:[^}\n]*/)?rm(?:[^A-Za-z]|})", text):
        findings.append(f"{MAC_SETUP}: parameter-default removal command is forbidden")
    return findings


def _tokens_in_order(text: str, tokens: tuple[str, ...]) -> bool:
    """Return whether unique tokens occur once in strict source order."""
    offsets = tuple(text.find(token) for token in tokens)
    return all(
        offset >= 0 and text.count(token) == 1
        for offset, token in zip(offsets, tokens, strict=True)
    ) and offsets == tuple(sorted(offsets))


def _lines_in_order(lines: tuple[str, ...], tokens: tuple[str, ...]) -> bool:
    """Return whether exact logical lines occur once in strict order."""
    positions: list[int] = []
    for token in tokens:
        matches = tuple(index for index, line in enumerate(lines) if line == token)
        if len(matches) != 1:
            return False
        positions.append(matches[0])
    return positions == sorted(positions)


def _check_cleanup_calls(text: str) -> list[str]:
    """Require the exact safe helper and deferred-trap call inventory."""
    active_lines = tuple(line.strip() for line in _active(text).splitlines())
    call_lines = tuple(line for line in active_lines if "safe_remove_tree" in line)
    expected_calls = (
        "safe_remove_tree() {",
        'trap \'safe_remove_tree "${work}" "${work_root}"\' EXIT',
        'trap \'safe_remove_tree "$arm_work" "$arm_work_root"\' EXIT',
        'safe_remove_tree "$arm_prefix" "$arm_root"',
        'safe_remove_tree "$emu_build" "$emu_root"',
    )
    trap_lines = tuple(line for line in active_lines if line.startswith("trap "))
    if call_lines == expected_calls and trap_lines == expected_calls[1:3]:
        return []
    return [f"{MAC_SETUP}: safe removal call/trap inventory is not exact"]


def _mentions_cleanup_name(word: str) -> bool:
    """Return whether a decoded mutator operand names a cleanup variable."""
    return any(
        re.search(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", word)
        for name in CLEANUP_NAMES
    )


def _direct_cleanup_mutation(line: str) -> bool:
    """Return whether a logical line directly assigns a cleanup variable."""
    names = "|".join(re.escape(name) for name in CLEANUP_NAMES)
    direct = rf"^(?:{names})(?:\[[^]]*\])?\s*(?:\+?=)"
    arithmetic = rf"^\(\(.*(?:{names})\s*(?:\+\+|--|[+*/%&|^-]?=)"
    return bool(re.search(direct, line) or re.search(arithmetic, line))


def _builtin_cleanup_mutation(command: str, arguments: tuple[str, ...]) -> bool:
    """Return whether one assignment builtin can mutate a cleanup variable."""
    allowed_readonly = {
        tuple(shlex.split(definition))[1:]
        for definition in CLEANUP_DEFINITION_LINES
        if definition.startswith("readonly ")
    }
    if command == "readonly" and arguments in allowed_readonly:
        return False
    if command in {"declare", "typeset", "local"} and "-n" in arguments:
        return True
    if command == "printf":
        if "-v" not in arguments:
            return False
        target = arguments[arguments.index("-v") + 1 : arguments.index("-v") + 2]
        return bool(target and (_mentions_cleanup_name(target[0]) or target[0].startswith("$")))
    mutators = {
        "declare",
        "typeset",
        "local",
        "readonly",
        "export",
        "unset",
        "read",
        "mapfile",
        "readarray",
        "let",
    }
    return command in mutators and any(_mentions_cleanup_name(arg) for arg in arguments)


def _check_cleanup_mutations(text: str) -> list[str]:
    """Reject every non-authoritative way to mutate a cleanup variable."""
    logical_lines = tuple(line.strip() for line in _active(text).splitlines())
    if any(
        line not in CLEANUP_DEFINITION_LINES and _direct_cleanup_mutation(line)
        for line in logical_lines
    ):
        return [f"{MAC_SETUP}: cleanup variable has a direct or compound override"]
    records = _command_records(text)
    if records is None:
        return [f"{MAC_SETUP}: cleanup variable command structure cannot be decoded"]
    if any(_builtin_cleanup_mutation(command, args) for _line, _p, command, args in records):
        return [f"{MAC_SETUP}: cleanup variable uses a forbidden assignment builtin"]
    return []


def _check_cleanup_variables(text: str) -> list[str]:
    """Require status-preserving assignments, adjacent readonly, and use order."""
    findings: list[str] = []
    logical_lines = tuple(line.strip() for line in _active(text).splitlines())
    if any(logical_lines.count(definition) != 1 for definition in CLEANUP_DEFINITION_LINES):
        findings.append(f"{MAC_SETUP}: cleanup assignment/readonly inventory is not exact")
    if any(
        not any(
            tuple(logical_lines[index : index + 2]) == pair for index in range(len(logical_lines))
        )
        for pair in CLEANUP_ASSIGNMENT_PAIRS
    ):
        findings.append(f"{MAC_SETUP}: cleanup assignment and readonly are not adjacent")
    if any(not _lines_in_order(logical_lines, tokens) for tokens in CLEANUP_DEFINITION_ORDER):
        findings.append(f"{MAC_SETUP}: cleanup definition/guard/use order is not exact")
    return [*findings, *_check_cleanup_mutations(text)]


def check_macos_cleanup(text: str) -> list[str]:
    """Require canonical readonly targets and one safe deletion primitive."""
    findings = _check_cleanup_tokens(text)
    canonical = _section(text, "canonical_cleanup_target()", "safe_remove_tree()")
    safe_remove = _section(text, "safe_remove_tree()", 'arm_release="')
    if _normalise_shell(canonical) != _normalise_shell(MAC_CANONICAL_CLEANUP):
        findings.append(f"{MAC_SETUP}: cleanup canonicalizer active body is not exact")
    if _normalise_shell(safe_remove) != _normalise_shell(MAC_SAFE_REMOVE_TREE):
        findings.append(f"{MAC_SETUP}: safe removal active body is not exact")
    return [
        *findings,
        *_check_sensitive_helpers(text),
        *_check_cleanup_calls(text),
        *_check_cleanup_variables(text),
    ]


def _macos_direct_delete_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return direct, additive, multi-target, and raw-trap removals."""
    arm_call = '  safe_remove_tree "$arm_prefix" "$arm_root"'
    return (
        (macos.replace(f"{arm_call}\n", "", 1), "removed Arm safe removal"),
        (
            macos.replace(arm_call, '  safe_remove_tree "$arm_prefix" "$home_root"', 1),
            "wrong allowed root",
        ),
        (
            macos.replace(arm_call, '  command rm -rf -- "$arm_prefix"', 1),
            "direct Arm removal",
        ),
        (macos + '\nrm -rf -- "$HOME"\n', "appended HOME removal"),
        (macos + '\nrm -rf -- "$arm_prefix/.."\n', "appended Arm parent removal"),
        (macos + '\nrm -r -f -- "$HOME"\n', "split recursive flags"),
        (macos + '\nrm --recursive -- "$HOME"\n', "long recursive flag"),
        (macos + '\nrm -rf -- "$arm_prefix" "$HOME"\n', "multiple removal targets"),
        (
            macos.replace(arm_call, f'{arm_call}\n  rm -rf -- "$HOME"', 1),
            "unsafe removal after safe helper",
        ),
        (macos + '\nsafe_remove_tree "$emu_build" "$emu_root"\n', "additive safe helper call"),
        (
            macos.replace(
                'trap \'safe_remove_tree "${work}" "${work_root}"\' EXIT',
                "trap 'rm -rf -- \"${work}\"' EXIT",
                1,
            ),
            "deferred raw trap removal",
        ),
    )


def _macos_indirect_delete_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return command, flag, positional, wrapper, and immutability indirections."""
    work_pair = "    " + "\n    ".join(CLEANUP_ASSIGNMENT_PAIRS[4])
    emu_pair = "  " + "\n  ".join(CLEANUP_ASSIGNMENT_PAIRS[9])
    return (
        (macos + '\nrm_flags=-rf\nrm "$rm_flags" -- "$HOME"\n', "recursive flag variable"),
        (macos + '\ndelete_cmd=rm\n"$delete_cmd" -rf -- "$HOME"\n', "removal command variable"),
        (macos + '\nset -- rm -rf -- "$HOME"\n"$@"\n', "positional command indirection"),
        (macos + '\nr""m -rf -- "$HOME"\n', "quoted removal command"),
        (macos + '\n\\rm -rf -- "$HOME"\n', "escaped removal command"),
        (
            macos + '\nwipe_tree() { command rm -rf -- "$HOME"; }\nwipe_tree\n',
            "recursive removal wrapper",
        ),
        (macos + '\n"${delete_cmd:-rm}" -rf -- "$HOME"\n', "parameter-default command"),
        (macos + "\neval 'rm -rf -- \"$HOME\"'\n", "eval removal"),
        (macos + "\nsh -c 'rm -rf -- \"$HOME\"'\n", "dynamic shell wrapper"),
        (macos + '\n"$DELETE_CMD" -rf -- "$HOME"\n', "external dynamic command"),
        (
            macos + '\n"$DELETE_CMD" "$RM_FLAGS" -- "$HOME"\n',
            "external dynamic command and flags",
        ),
        (
            macos.replace(
                work_pair,
                work_pair.replace("\n    readonly work", '\n    work="$HOME"\n    readonly work'),
                1,
            ),
            "intervening work override before readonly",
        ),
        (
            macos.replace(
                work_pair,
                work_pair.replace("\n    readonly work", "\n    echo wait\n    readonly work"),
                1,
            ),
            "intervening command before readonly",
        ),
        (
            macos.replace(emu_pair, CLEANUP_ASSIGNMENT_PAIRS[9][0], 1),
            "missing emulator readonly",
        ),
    )


def _macos_variable_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return alternate Bash assignment forms targeting cleanup variables."""
    arm_static = CLEANUP_STATIC_READONLY[0]
    return (
        (macos + "\ndeclare arm_root=/tmp\n", "declare cleanup override"),
        (macos + "\ntypeset arm_prefix=/tmp\n", "typeset cleanup override"),
        (macos + "\nlocal emu_build=/tmp\n", "local cleanup shadow"),
        (macos + "\nprintf -v work %s /tmp\n", "printf-v cleanup override"),
        (macos + "\nread arm_work_root </dev/null\n", "read cleanup override"),
        (macos + "\nmapfile work_root </dev/null\n", "mapfile cleanup override"),
        (macos + "\nunset arm_prefix\n", "unset cleanup target"),
        (macos + "\nemu_build+=(/tmp)\n", "compound cleanup assignment"),
        (macos + "\narm_root[0]=/tmp\n", "array cleanup assignment"),
        (
            macos + "\ndeclare -n cleanup_ref=arm_prefix\ncleanup_ref=/tmp\n",
            "indirect cleanup assignment",
        ),
        (macos + "\n((work_root=0))\n", "arithmetic cleanup assignment"),
        (
            macos.replace(arm_static, arm_static + '\narm_root="/tmp"\n', 1),
            "intervening cleanup-root override",
        ),
    )


def _macos_prefixed_execution_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return prefixed and indirect execution forms that must fail closed."""
    return (
        (macos + "\ncommand eval 'true'\n", "command-prefixed eval"),
        (macos + "\nbuiltin eval 'true'\n", "builtin-prefixed eval"),
        (macos + "\nenv eval 'true'\n", "env-prefixed eval"),
        (macos + "\ncommand sh -c 'true'\n", "command-prefixed shell-c"),
        (macos + "\nenv bash -c 'true'\n", "env-prefixed shell-c"),
        (macos + "\nbuiltin sh -c 'true'\n", "builtin-prefixed shell-c"),
        (macos + '\ncommand "$DELETE_CMD" "$HOME"\n', "command-prefixed dynamic command"),
        (macos + '\nbuiltin "$DELETE_CMD" "$HOME"\n', "builtin-prefixed dynamic command"),
        (macos + '\nenv "$DELETE_CMD" "$HOME"\n', "env-prefixed dynamic command"),
        (macos + '\nexec "$DELETE_CMD" "$HOME"\n', "exec dynamic command"),
        (macos + '\nsource "$DELETE_SCRIPT"\n', "source dynamic command"),
        (macos + '\nprintf x | xargs "$DELETE_CMD"\n', "xargs dynamic command"),
        (macos + "\ncommand -- eval 'true'\n", "optioned command-prefixed eval"),
        (macos + "\nbuiltin -- eval 'true'\n", "optioned builtin-prefixed eval"),
        (macos + "\nenv -i command eval 'true'\n", "nested env-command eval"),
        (macos + "\nenv -C /tmp sh -c 'true'\n", "optioned env shell-c"),
        (macos + "\nenv -S 'eval true'\n", "env split-string execution"),
        (macos + "\nenv --split-string='eval true'\n", "long env split-string execution"),
        (macos + "\nenv -S'eval true'\n", "attached env split-string execution"),
        (macos + "\n/bin/zsh -c 'true'\n", "absolute zsh shell-c"),
        (macos + "\nzsh -c 'true'\n", "zsh shell-c"),
        (macos + "\ndash -c 'true'\n", "dash shell-c"),
        (macos + "\n/usr/bin/ksh -c 'true'\n", "absolute ksh shell-c"),
        (macos + "\nenv /bin/ash -c 'true'\n", "env-prefixed ash shell-c"),
        (macos + "\ncommand /usr/local/bin/fish -c 'true'\n", "command-prefixed fish shell-c"),
        (macos + "\nenv nu -c 'true'\n", "env-prefixed nu shell-c"),
    )


def _macos_helper_definition_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return duplicate and alternate definitions of policy-sensitive helpers."""
    return (
        (macos + "\ndockerfile_arg() { :; }\n", "later Docker parser definition"),
        ("safe_remove_tree() { :; }\n" + macos, "earlier removal-helper definition"),
        (
            macos + "\nfunction canonical_cleanup_target { :; }\n",
            "later function-keyword canonicalizer",
        ),
        (
            macos + "\nfunction require_arm_hash_pins() { :; }\n",
            "later function-keyword pin validator",
        ),
        (macos + "\ninstall_homebrew() ( : )\n", "later Homebrew helper definition"),
        (
            macos + "\nfunction safe_remove_tree\n{\n  :\n}\n",
            "multiline function-keyword removal helper",
        ),
        (
            "dockerfile_arg ()\n{\n  :\n}\n" + macos,
            "earlier multiline Docker parser",
        ),
    )


def _macos_helper_mutation_commands(macos: str) -> tuple[tuple[str, str], ...]:
    """Return function removal and attribute mutation commands."""
    return (
        (macos + "\nunset -f safe_remove_tree\n", "unset removal helper"),
        (macos + "\ncommand unset -f dockerfile_arg\n", "prefixed unset Docker parser"),
        (
            macos + "\nbuiltin readonly -f canonical_cleanup_target\n",
            "readonly canonicalizer mutation",
        ),
        (macos + "\ndeclare -fx require_arm_hash_pins\n", "declare pin-validator mutation"),
        (macos + "\nexport -f install_homebrew\n", "export Homebrew helper"),
        (macos + "\ntypeset -f dockerfile_arg\n", "typeset Docker parser"),
        (macos + '\nunset -f "$HELPER"\n', "dynamic function removal"),
        (macos + "\nlocal -f safe_remove_tree\n", "local removal helper attribute"),
    )


def _macos_delete_safety_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return every hostile recursive-deletion or execution mutation."""
    return (
        *_macos_direct_delete_mutations(macos),
        *_macos_indirect_delete_mutations(macos),
        *_macos_variable_mutations(macos),
        *_macos_prefixed_execution_mutations(macos),
        *_macos_helper_definition_mutations(macos),
        *_macos_helper_mutation_commands(macos),
    )


def _macos_guard_safety_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return hostile physical-boundary canonicalizer mutations."""
    return (
        (
            macos.replace(
                '[[ "$target_parent" != "$root_physical" ]]',
                '[[ "$target_parent" == "$root_physical" ]]',
                1,
            ),
            "inverted cleanup-root boundary",
        ),
        (
            macos.replace('[[ -L "$canonical" ]]', '[[ ! -L "$canonical" ]]', 1),
            "symlink cleanup target allowed",
        ),
    )


def macos_cleanup_mutations(macos: str) -> tuple[tuple[str, str], ...]:
    """Return all hostile macOS recursive-cleanup mutations."""
    return (*_macos_delete_safety_mutations(macos), *_macos_guard_safety_mutations(macos))
