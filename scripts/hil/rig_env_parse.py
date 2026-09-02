#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Parse HIL rig literals and delegate their values to the Bash authority.

Both the interactive loader and Ansible use this parser. It recognizes a small
literal-only subset of Bash assignments for the declared rig fields, ignores
unrelated keys, and never executes or expands the input. Every resulting value
is validated by fixed argv through ``lib/rig_contract.sh``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

CONTRACT = Path(__file__).resolve().parent / "lib" / "rig_contract.sh"
SCHEMA_COLUMNS = 4
PROTECTED_MODE = 0o600
ACTOR_FIELDS = 3
ACTOR_QUOTE_DELIMITERS = 2
ASSIGNMENT_RE = re.compile(r"(?:export )?([A-Za-z_][A-Za-z0-9_]*)=(.*)", re.DOTALL)
LITERAL_RE = re.compile(
    r"(?:([A-Za-z0-9_@./-]*)|'([A-Za-z0-9_@./-]*)'|\"([A-Za-z0-9_@./-]*)\")"
    r"(?:[ \t]+(?:#.*)?)?"
)
INTERACTIVE_FIELDS = frozenset(
    {
        "C6_CONSOLE_TTY",
        "RA8_BENCH_ACTORS",
        "RA8_BENCH_WAIT",
        "RA8_BENCH_WAIT_S",
        "RA8_CONSOLE_TTY",
    }
)
WAIT_RE = re.compile(r"[0-9]+(?:[smh])?")
TTY_RE = re.compile(r"/dev/[A-Za-z0-9_.:/-]+")
ACTOR_NAME_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*")
TRANSPORT_WORD_RE = re.compile(r"[A-Za-z0-9_./:@=+-]+")


class RigConfigError(ValueError):
    """A protected rig file or its typed value violates the contract."""


@dataclass(frozen=True)
class FieldSpec:
    """One row reported by the single Bash contract authority."""

    name: str
    kind: str
    required: bool
    default: str


@dataclass(frozen=True)
class CommandResult:
    """Captured status and streams from one fixed-argv child process."""

    returncode: int
    stdout: str
    stderr: str


def _fail(message: str) -> NoReturn:
    raise RigConfigError(message)


def run_fixed_command(arguments: tuple[str, ...], environment: dict[str, str]) -> CommandResult:
    """Run fixed argv through posix_spawn, capturing bytes without a shell."""
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        actions = (
            (os.POSIX_SPAWN_DUP2, stdout_file.fileno(), 1),
            (os.POSIX_SPAWN_DUP2, stderr_file.fileno(), 2),
        )
        try:
            process = os.posix_spawn(arguments[0], arguments, environment, file_actions=actions)
            _, status = os.waitpid(process, 0)
        except OSError as error:
            _fail(f"fixed command could not run: {error}")
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
    return CommandResult(os.waitstatus_to_exitcode(status), stdout, stderr)


def _run_contract(*arguments: str) -> CommandResult:
    """Invoke the fixed adjacent authority without shell startup channels."""
    environment = os.environ.copy()
    environment.pop("BASH_ENV", None)
    environment.pop("ENV", None)
    return run_fixed_command(("/bin/bash", "-p", str(CONTRACT), *arguments), environment)


def load_schema() -> dict[str, FieldSpec]:
    """Read the field list, kinds, required state, and defaults from Bash."""
    result = _run_contract("--describe")
    if result.returncode != 0:
        _fail("rig contract description failed")
    schema: dict[str, FieldSpec] = {}
    for number, line in enumerate(result.stdout.splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != SCHEMA_COLUMNS:
            _fail(f"rig contract description line {number} is malformed")
        name, kind, presence, default = parts
        if name in schema:
            _fail(f"rig contract description duplicates {name}")
        if presence not in {"required", "optional"}:
            _fail(f"rig contract description has invalid presence for {name}")
        schema[name] = FieldSpec(name, kind, presence == "required", default)
    if set(schema) != {"PI_HOST", "JLINK_SN", "JLINK_DEVICE", "PI_REPO"}:
        _fail("rig contract description does not declare the exact field set")
    return schema


def validate_value(name: str, value: str) -> None:
    """Require the Bash authority to accept one parsed value."""
    if "\0" in value:
        _fail(f"{name} contains a NUL byte")
    result = _run_contract("--validate", name, value)
    if result.returncode != 0:
        reason = result.stderr.strip().splitlines()
        detail = reason[-1] if reason else "typed validation failed"
        _fail(f"{name}: {detail.removeprefix('error: ')}")


def _malformed_allowlisted_key(line: str, allowed: frozenset[str]) -> str | None:
    """Return a declared key when a row resembles but is not our grammar."""
    names = "|".join(re.escape(name) for name in sorted(allowed))
    match = re.match(rf"^[ \t]*(?:export[ \t]+)?({names})\b", line)
    return match.group(1) if match is not None else None


def _parse_literal(encoded: str, key: str, number: int) -> str:
    """Decode one complete, expansion-free Bash literal subset."""
    match = LITERAL_RE.fullmatch(encoded)
    if match is None:
        _fail(
            f"{key} at line {number} must be one literal value without "
            "expansion, command syntax, or an attached comment"
        )
    for value in match.groups():
        if value is not None:
            return value
    message = "literal grammar matched without a value"
    raise AssertionError(message)


def _parse_actor_literal(encoded: str, number: int) -> str:
    """Decode the documented multiline single-quoted actor roster."""
    match = re.fullmatch(r"'([^']*)'(?:[ \t]+(?:#.*)?)?", encoded, re.DOTALL)
    if match is None:
        _fail(f"RA8_BENCH_ACTORS at line {number} must be one single-quoted literal")
    return match.group(1)


def _validate_actor_roster(value: str, number: int) -> None:
    """Reject shell syntax before bench_contention consumes transport words."""
    for offset, raw in enumerate(value.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split("|")]
        if len(parts) != ACTOR_FIELDS:
            _fail(f"RA8_BENCH_ACTORS line {number + offset} must contain two pipes")
        name, host, transport = parts
        if ACTOR_NAME_RE.fullmatch(name) is None:
            _fail(f"RA8_BENCH_ACTORS line {number + offset} has an invalid name")
        validate_value("PI_HOST", host)
        words = transport.split()
        if not words or any(TRANSPORT_WORD_RE.fullmatch(word) is None for word in words):
            _fail(f"RA8_BENCH_ACTORS line {number + offset} has unsafe transport words")


def _parse_interactive_value(encoded: str, key: str, number: int) -> str:
    """Decode and validate one explicitly supported non-secret workstation field."""
    if key == "RA8_BENCH_ACTORS":
        value = _parse_actor_literal(encoded, number)
        _validate_actor_roster(value, number)
        return value
    value = _parse_literal(encoded, key, number)
    if key == "RA8_BENCH_WAIT" and value and WAIT_RE.fullmatch(value) is None:
        _fail(f"{key} at line {number} must be seconds or a duration ending in s/m/h")
    if key == "RA8_BENCH_WAIT_S" and value and not value.isdecimal():
        _fail(f"{key} at line {number} must be decimal seconds")
    if key.endswith("_CONSOLE_TTY") and value and TTY_RE.fullmatch(value) is None:
        _fail(f"{key} at line {number} must be one absolute /dev path")
    return value


def _logical_rows(source: str, include_interactive: bool) -> list[tuple[int, str]]:
    """Join only the documented multiline actor literal; preserve other rows."""
    lines = source.splitlines()
    rows: list[tuple[int, str]] = []
    index = 0
    while index < len(lines):
        number = index + 1
        line = lines[index]
        if include_interactive and re.match(r"^RA8_BENCH_ACTORS='", line):
            parts = [line]
            while sum(part.count("'") for part in parts) < ACTOR_QUOTE_DELIMITERS:
                index += 1
                if index >= len(lines):
                    _fail(f"RA8_BENCH_ACTORS at line {number} has no closing quote")
                parts.append(lines[index])
            line = "\n".join(parts)
        rows.append((number, line))
        index += 1
    return rows


def _read_source(path: Path) -> str:
    """Read one owner-owned mode-0600 regular source without following links."""
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    if hasattr(os, "O_NONBLOCK"):
        flags |= os.O_NONBLOCK
    try:
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            _fail("rig environment input must be a regular non-symlink file")
        if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != PROTECTED_MODE:
            _fail("rig environment input must be owner-owned mode 0600")
        with os.fdopen(descriptor, encoding="utf-8") as stream:
            descriptor = -1
            return stream.read()
    except (OSError, UnicodeError) as error:
        _fail(f"cannot read rig environment as UTF-8: {error}")
    finally:
        if "descriptor" in locals() and descriptor >= 0:
            os.close(descriptor)


def _parse_allowlisted_line(
    line: str,
    number: int,
    schema: dict[str, FieldSpec],
    include_interactive: bool,
) -> tuple[str, str] | None:
    """Return one parsed allowlisted assignment or None for unrelated input."""
    allowed = frozenset(schema) | (INTERACTIVE_FIELDS if include_interactive else frozenset())
    assignment = ASSIGNMENT_RE.fullmatch(line)
    if assignment is None:
        malformed = _malformed_allowlisted_key(line, allowed)
        if malformed is not None:
            _fail(f"{malformed} at line {number} has unsupported assignment syntax")
        return None
    key, encoded = assignment.groups()
    if key not in allowed:
        return None
    if key in INTERACTIVE_FIELDS:
        value = _parse_interactive_value(encoded, key, number)
        return key, value
    value = _parse_literal(encoded, key, number)
    if not value:
        value = schema[key].default
    if value:
        validate_value(key, value)
    return key, value


def _apply_defaults(found: dict[str, str], schema: dict[str, FieldSpec]) -> None:
    """Fill and validate optional declared defaults in-place."""
    for name, spec in schema.items():
        if name not in found and spec.default:
            validate_value(name, spec.default)
            found[name] = spec.default


def parse_rig_environment(
    path: Path, required: frozenset[str], *, include_interactive: bool = False
) -> dict[str, str]:
    """Parse the allowlist, reject duplicates/malformed rows, and apply defaults."""
    schema = load_schema()
    unknown_required = required - schema.keys()
    if unknown_required:
        _fail("unknown required rig fields: " + ", ".join(sorted(unknown_required)))
    found: dict[str, str] = {}
    source = _read_source(path)

    for number, raw in _logical_rows(source, include_interactive):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        parsed = _parse_allowlisted_line(raw, number, schema, include_interactive)
        if parsed is None:
            continue
        key, value = parsed
        if key in found:
            _fail(f"duplicate {key} at line {number}")
        found[key] = value

    missing = {name for name in required if not found.get(name)}
    if missing:
        _fail("missing required HIL keys: " + ", ".join(sorted(missing)))
    _apply_defaults(found, schema)
    return found


def write_result(path: Path, values: dict[str, str], output_format: str = "json") -> None:
    """Write JSON through the preallocated protected regular-file descriptor."""
    flags = os.O_WRONLY | os.O_TRUNC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            _fail("rig result must be a regular file")
        if info.st_uid != os.getuid() or info.st_mode & 0o077:
            _fail("rig result must be owner-owned mode 0600")
        if output_format == "nul":
            with os.fdopen(descriptor, "wb") as stream:
                descriptor = -1
                for name in sorted(values):
                    stream.write(name.encode("ascii") + b"\0")
                    stream.write(values[name].encode("utf-8") + b"\0")
                stream.flush()
                os.fsync(stream.fileno())
            return
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            if output_format == "json":
                json.dump(values, stream, sort_keys=True)
                stream.write("\n")
            elif output_format == "tsv":
                for name in sorted(values):
                    stream.write(f"{name}\t{values[name]}\n")
            else:
                message = f"unknown protected output format: {output_format}"
                raise AssertionError(message)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        _fail(f"cannot write protected rig result: {error}")
    finally:
        if "descriptor" in locals() and descriptor >= 0:
            os.close(descriptor)


def _expect_failure(
    text: str,
    expected: str,
    required: frozenset[str],
    *,
    include_interactive: bool = False,
) -> None:
    with tempfile.TemporaryDirectory(prefix="ra8-rig-parser-") as temporary:
        source = Path(temporary) / "rig.env"
        source.write_text(text, encoding="utf-8")
        source.chmod(0o600)
        try:
            parse_rig_environment(source, required, include_interactive=include_interactive)
        except RigConfigError as error:
            if expected not in str(error):
                _fail(f"selftest expected {expected!r}, got {error!s}")
        else:
            _fail(f"selftest unsafe fixture passed; expected {expected!r}")


def _expect_path_failure(source: Path, expected: str, required: frozenset[str]) -> None:
    try:
        parse_rig_environment(source, required)
    except RigConfigError as error:
        if expected not in str(error):
            _fail(f"selftest expected {expected!r}, got {error!s}")
    else:
        _fail(f"selftest unsafe path passed; expected {expected!r}")


def _valid_parse_selftest(required: frozenset[str]) -> None:
    """Exercise one complete valid parse and protected output round trip."""
    with tempfile.TemporaryDirectory(prefix="ra8-rig-parser-") as temporary:
        root = Path(temporary)
        source = root / "rig.env"
        output = root / "result.json"
        source.write_text(
            "# unrelated secrets remain unreachable\n"
            "TAPO_PASS='not copied'\n"
            "export PI_HOST='sikar@10.0.40.103' # bench route\n"
            "JLINK_SN=123456789\n"
            "JLINK_DEVICE=\n"
            "PI_REPO=/home/ra8-hil/ra8-firmware\n",
            encoding="utf-8",
        )
        source.chmod(0o600)
        output.write_text("", encoding="utf-8")
        output.chmod(0o600)
        values = parse_rig_environment(source, required)
        expected = {
            "PI_HOST": "sikar@10.0.40.103",
            "JLINK_SN": "123456789",
            "JLINK_DEVICE": "R7KA8D2KF_CPU0",
            "PI_REPO": "/home/ra8-hil/ra8-firmware",
        }
        if values != expected:
            _fail(f"selftest valid parse differed: {values!r}")
        write_result(output, values)
        if json.loads(output.read_text(encoding="utf-8")) != expected:
            _fail("selftest protected result did not round-trip")
        write_result(output, values, "tsv")
        expected_rows = "".join(f"{name}\t{expected[name]}\n" for name in sorted(expected))
        if output.read_text(encoding="utf-8") != expected_rows:
            _fail("selftest protected TSV result did not round-trip")


def _interactive_parse_selftest(required: frozenset[str]) -> None:
    """Preserve documented non-secret workstation controls without execution."""
    with tempfile.TemporaryDirectory(prefix="ra8-rig-parser-") as temporary:
        root = Path(temporary)
        source = root / "rig.env"
        output = root / "result.nul"
        source.write_text(
            "PI_HOST=host\nJLINK_SN=1\nRA8_BENCH_WAIT=10m\n"
            "RA8_CONSOLE_TTY=/dev/cu.usbmodem1\n"
            "RA8_BENCH_ACTORS='\n"
            "local | host | /bin/bash -p -s\n"
            "dev | user@dev.local | /usr/bin/ssh dev /bin/bash -p -s\n"
            "'\nTAPO_PASS='not exported'\n",
            encoding="utf-8",
        )
        source.chmod(0o600)
        output.write_bytes(b"")
        output.chmod(0o600)
        values = parse_rig_environment(source, required, include_interactive=True)
        if values.get("RA8_BENCH_WAIT") != "10m" or "TAPO_PASS" in values:
            _fail("selftest interactive allowlist leaked or lost a field")
        if "dev | user@dev.local" not in values.get("RA8_BENCH_ACTORS", ""):
            _fail("selftest multiline actor roster did not round-trip")
        write_result(output, values, "nul")
        fields = output.read_bytes().split(b"\0")
        if fields[-1] or b"RA8_BENCH_ACTORS" not in fields:
            _fail("selftest protected NUL result did not round-trip")


def _protected_path_selftest(required: frozenset[str]) -> None:
    """Reject permissive and symlinked protected input paths."""
    with tempfile.TemporaryDirectory(prefix="ra8-rig-parser-") as temporary:
        root = Path(temporary)
        source = root / "rig.env"
        source.write_text("PI_HOST=host\nJLINK_SN=1\n", encoding="utf-8")
        source.chmod(0o644)
        _expect_path_failure(source, "owner-owned mode 0600", required)
        target = root / "target.env"
        target.write_text("PI_HOST=host\nJLINK_SN=1\n", encoding="utf-8")
        target.chmod(0o600)
        source.unlink()
        source.symlink_to(target)
        _expect_path_failure(source, "cannot read rig environment", required)
        source.unlink()
        os.mkfifo(source, mode=0o600)
        _expect_path_failure(source, "regular non-symlink", required)


def selftest() -> None:
    """Exercise parser and delegated value validation in both directions."""
    required = frozenset({"PI_HOST", "JLINK_SN"})
    _valid_parse_selftest(required)
    _interactive_parse_selftest(required)
    failures = (
        ("PI_HOST=host\nPI_HOST=other\nJLINK_SN=1\n", "duplicate PI_HOST"),
        ("export PI_HOST\nJLINK_SN=1\n", "unsupported assignment syntax"),
        ("PI_HOST = host\nJLINK_SN=1\n", "unsupported assignment syntax"),
        (" PI_HOST=host\nJLINK_SN=1\n", "unsupported assignment syntax"),
        ("export  PI_HOST=host\nJLINK_SN=1\n", "unsupported assignment syntax"),
        ("PI_HOST='unterminated\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=host words\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=host#not-a-comment\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=$(hostname)\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=`hostname`\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=$HOSTNAME\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=host; id\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=-oProxyCommand=bad\nJLINK_SN=1\n", "one literal value"),
        ("PI_HOST=user@@host\nJLINK_SN=1\n", "one user separator"),
        ("PI_HOST=1.2.3.999\nJLINK_SN=1\n", "malformed IPv4"),
        ("PI_HOST=host\nJLINK_SN=-1\n", "cannot start with punctuation"),
        ("PI_HOST=host\nJLINK_SN=1\nPI_REPO=../repo\n", "unsafe path segment"),
        ("PI_HOST=host\nJLINK_SN=1\nPI_REPO=repo'bad\n", "one literal value"),
        ("JLINK_SN=1\n", "missing required HIL keys"),
    )
    for text, expected in failures:
        _expect_failure(text, expected, required)
    _protected_path_selftest(required)
    _expect_failure(
        "PI_HOST=host\nJLINK_SN=1\nRA8_BENCH_WAIT=$(id)\n",
        "one literal value",
        required,
        include_interactive=True,
    )
    count = len(failures) + 4
    print(f"rig_env_parse.py --selftest: PASS (2 must-pass, {count} must-fire)")


def main() -> int:
    """Run parser selftests or validate one protected input/output pair."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--format", choices=("json", "nul", "tsv"), default="json")
    parser.add_argument("--include-interactive", action="store_true")
    parser.add_argument("--require", action="append", default=[])
    arguments = parser.parse_args()
    try:
        if arguments.selftest:
            if arguments.input is not None or arguments.output is not None:
                parser.error("--selftest does not accept input/output")
            selftest()
            return 0
        if arguments.input is None or arguments.output is None:
            parser.error("--input and --output are required")
        values = parse_rig_environment(
            arguments.input,
            frozenset(arguments.require),
            include_interactive=arguments.include_interactive,
        )
        write_result(arguments.output, values, arguments.format)
    except RigConfigError as error:
        print(f"rig_env_parse: {error}", file=sys.stderr)
        return 2
    print("validated HIL keys: " + ", ".join(sorted(values)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
