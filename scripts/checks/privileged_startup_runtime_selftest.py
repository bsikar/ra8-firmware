# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Focused hostile runtime cases for the privileged startup wrapper."""

from __future__ import annotations

import os
import signal
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class PrivateRun:
    """One fully specified invocation of a private selftest script."""

    command: tuple[str, ...]
    cwd: Path
    environment: dict[str, str]
    pass_fds: tuple[int, ...] = ()
    umask: int = -1
    preexec_fn: Callable[[], None] | None = None
    timeout: float = 15


OWNER_STATUS = 37


@dataclass(frozen=True)
class WrapperVariant:
    """One wrapper variant supplied by the single policy authority."""

    name: str
    prefix: tuple[str, ...]
    close: tuple[str, ...]


class PrivilegedRuntimeError(RuntimeError):
    """One privileged wrapper runtime invariant failed."""


def _fail(message: str) -> None:
    raise PrivilegedRuntimeError(message)


def run_private(spec: PrivateRun) -> subprocess.CompletedProcess[str]:
    """Run one fixed private startup fixture."""
    return subprocess.run(  # noqa: S603 -- fixed inert temporary fixture
        spec.command,
        cwd=spec.cwd,
        env=spec.environment,
        pass_fds=spec.pass_fds,
        umask=spec.umask,
        preexec_fn=spec.preexec_fn,
        capture_output=True,
        text=True,
        check=False,
        timeout=spec.timeout,
    )


def _write_wrapper(path: Path, variant: WrapperVariant, body: str) -> None:
    lines = ["#!/bin/bash -p", *variant.prefix, *body.splitlines(), *variant.close]
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    path.chmod(0o755)


def _hostile_environment() -> dict[str, str]:
    environment = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
    environment["BASH_FUNC_ra8_probe%%"] = "() { :; }"
    environment["BASH_FUNC_ra8_legacy()"] = "() { :; }"
    return environment


def _exec_failure_case(
    base: Path,
    variant: WrapperVariant,
    failure: str,
) -> None:
    body_marker = base / f"{variant.name}-{failure}.body"
    descendant_marker = base / f"{variant.name}-{failure}.descendant"
    script = base / f"{variant.name}-{failure}.sh"
    _write_wrapper(
        script,
        variant,
        f"printf 'body\\n' >{body_marker!s}\n"
        f"/bin/bash -c \"printf 'descendant\\\\n' >{descendant_marker!s}\"\n",
    )
    text = script.read_text(encoding="ascii")
    if failure == "e2big":
        injection = (
            'ra8_startup_e2big="$(/usr/bin/head -c 3000000 /dev/zero | '
            "/usr/bin/tr '\\000' x)\"\n"
            'ra8_startup_env_unset+=(-u "$ra8_startup_e2big")\n'
        )
        text = text.replace("if ! exec /usr/bin/env ", f"{injection}if ! exec /usr/bin/env ", 1)
    else:
        text = text.replace("exec /usr/bin/env ", "exec /ra8-absent-env ", 1)
    script.write_text(text, encoding="ascii")
    commands = (
        ("ordinary", (str(script),)),
        ("execfail", ("/bin/bash", "-O", "execfail", "-p", str(script))),
    )
    for mode, command in commands:
        result = run_private(PrivateRun(command, base, _hostile_environment()))
        if result.returncode == 0 or body_marker.exists() or descendant_marker.exists():
            _fail(f"{variant.name}/{failure}/{mode}: failed exec reached body")
        if mode == "execfail" and "could not enter sanitized process" not in result.stderr:
            _fail(f"{variant.name}/{failure}: explicit refusal did not run")


def _ignore_usr1() -> None:
    signal.signal(signal.SIGUSR1, signal.SIG_IGN)


def _preservation_findings(
    result: subprocess.CompletedProcess[str], expected: tuple[str, ...]
) -> list[str]:
    findings = [line for line in expected if f"{line}\n" not in result.stdout]
    if result.returncode != OWNER_STATUS:
        findings.append(f"status={OWNER_STATUS}")
    return findings


def _assert_preservation(
    result: subprocess.CompletedProcess[str], expected: tuple[str, ...], variant: str
) -> None:
    """Require every process-state observation and prove each check is live."""
    if findings := _preservation_findings(result, expected):
        _fail(f"{variant}: changed process state: {findings!r}")
    for line in expected:
        mutated = result.stdout.replace(f"{line}\n", "mutated\n", 1)
        control = subprocess.CompletedProcess(result.args, OWNER_STATUS, mutated, result.stderr)
        if not _preservation_findings(control, expected):
            _fail(f"{variant}: assertion missed {line!r}")
    control = subprocess.CompletedProcess(result.args, 0, result.stdout, result.stderr)
    if not _preservation_findings(control, expected):
        _fail(f"{variant}: assertion missed owner status")


def _preservation_case(base: Path, variant: WrapperVariant) -> None:
    script = base / f"{variant.name}-preserve.sh"
    body = """set -u
printf 'cwd=%s\\n' "$PWD"
printf 'umask=%s\\n' "$(umask)"
IFS= read -r ra8_fd_payload <&"${RA8_TEST_FD:?}"
printf 'fd=%s\\n' "$ra8_fd_payload"
kill -USR1 "$$"
printf 'signal=ignored\\n'
ra8_function_rows=0
while IFS= read -r -d '' ra8_env_row; do
  case "${ra8_env_row%%=*}" in
    BASH_FUNC_*) ra8_function_rows=$((ra8_function_rows + 1)) ;;
  esac
done < <(/usr/bin/env -0)
printf 'function-rows=%s\\n' "$ra8_function_rows"
printf 'argc=%s\\n' "$#"
ra8_arg_index=0
for ra8_arg in "$@"; do
  printf 'arg%s=%s\\n' "$ra8_arg_index" "$ra8_arg"
  ra8_arg_index=$((ra8_arg_index + 1))
done
exit 37"""
    _write_wrapper(script, variant, body)
    read_fd, write_fd = os.pipe()
    os.write(write_fd, b"open-descriptor\n")
    os.close(write_fd)
    environment = _hostile_environment()
    environment["RA8_TEST_FD"] = str(read_fd)
    args = ("plain", "path with spaces", "line-one_line-two")
    try:
        result = run_private(
            PrivateRun(
                (str(script), *args),
                base,
                environment,
                pass_fds=(read_fd,),
                umask=0o027,
                preexec_fn=_ignore_usr1,
            )
        )
    finally:
        os.close(read_fd)
    expected = (
        f"cwd={base}",
        "umask=0027",
        "fd=open-descriptor",
        "signal=ignored",
        "function-rows=0",
        "argc=3",
        "arg0=plain",
        "arg1=path with spaces",
        "arg2=line-one_line-two",
    )
    _assert_preservation(result, expected, variant.name)


def run_privileged_wrapper_runtime_cases(base: Path, variants: tuple[WrapperVariant, ...]) -> int:
    """Exercise all three live wrapper variants in both directions."""
    for variant in variants:
        for failure in ("missing", "e2big"):
            _exec_failure_case(base, variant, failure)
        _preservation_case(base, variant)
    return 5 * len(variants)
