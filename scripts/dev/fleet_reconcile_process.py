# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Supervise one reconciler child process group and administrative stops."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import time
from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager, suppress
from dataclasses import dataclass

import fleet_model as fm
import fleet_mutation_lock as fml

TIMEOUT_STATUS = 124


@dataclass
class StopState:
    """Mutable signal state shared by the handler and foreground transaction."""

    process: subprocess.Popen[str] | None = None
    process_signal: int | None = None


STOP_STATE = StopState()


@dataclass(frozen=True)
class CommandResult:
    """Captured command status and output."""

    status: int
    stdout: str
    stderr: str


def _timeout_output(value: str | bytes | None) -> str:
    """Normalize output captured by a timed-out text subprocess."""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def _signal_process_group(process: subprocess.Popen[str], process_signal: int) -> None:
    """Signal the entire fleet-command process group if it still exists."""
    with suppress(ProcessLookupError):
        os.killpg(process.pid, process_signal)


def _stop_requested(process_signal: int, _frame: object) -> None:
    """Record an administrative stop and terminate only the owned child group."""
    if STOP_STATE.process_signal is None:
        STOP_STATE.process_signal = process_signal
    if STOP_STATE.process is not None:
        _signal_process_group(STOP_STATE.process, signal.SIGTERM)


@contextmanager
def stop_handlers() -> Iterator[None]:
    """Install TERM/HUP/INT handlers for one complete locked transaction."""
    STOP_STATE.process_signal = None
    previous = {
        process_signal: signal.signal(process_signal, _stop_requested)
        for process_signal in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)
    }
    try:
        yield
    finally:
        for process_signal, handler in previous.items():
            signal.signal(process_signal, handler)


def interrupted_status() -> int:
    """Return the shell status for the first pending stop, or zero."""
    if STOP_STATE.process_signal is None:
        return 0
    return 128 + STOP_STATE.process_signal


def _wait_process_group(process: subprocess.Popen[str], timeout_seconds: float) -> bool:
    """Wait until no process remains in the owned process group."""
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.01)
    return False


def _stop_child_group(process: subprocess.Popen[str]) -> tuple[str, str]:
    """Terminate a fleet command and wait for its complete owned process group."""
    _signal_process_group(process, signal.SIGTERM)
    try:
        return process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        _signal_process_group(process, signal.SIGKILL)
        return process.communicate()


def command_runner(
    argv: Sequence[str],
    *,
    timeout_seconds: float = 4 * 60 * 60,
    guardian: bool = False,
    before_publication: Callable[[subprocess.Popen[str]], None] | None = None,
) -> CommandResult:
    """Run one exact fleet command and capture its evidence."""
    signal_before = STOP_STATE.process_signal
    if signal_before is not None:
        return CommandResult(
            128 + signal_before,
            "",
            f"fleet-reconcile: interrupted by signal {signal_before}\n",
        )
    guardian_kwargs = fml.guardian_subprocess_kwargs() if guardian else {}
    process = subprocess.Popen(  # noqa: S603 -- executable and verbs are fixed below
        list(argv),
        cwd=fm.REPO_ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        **guardian_kwargs,
    )
    if before_publication is not None:
        before_publication(process)
    STOP_STATE.process = process
    if STOP_STATE.process_signal is not None:
        _signal_process_group(process, signal.SIGTERM)
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        stdout, stderr = _stop_child_group(process)
        return CommandResult(
            TIMEOUT_STATUS,
            _timeout_output(stdout or error.stdout),
            _timeout_output(stderr or error.stderr) + "fleet-reconcile: command timed out\n",
        )
    finally:
        STOP_STATE.process = None
    if STOP_STATE.process_signal is not None and STOP_STATE.process_signal != signal_before:
        if not _wait_process_group(process, 2):
            _signal_process_group(process, signal.SIGKILL)
            _wait_process_group(process, 2)
        return CommandResult(
            128 + STOP_STATE.process_signal,
            stdout,
            stderr + f"fleet-reconcile: interrupted by signal {STOP_STATE.process_signal}\n",
        )
    return CommandResult(process.returncode, stdout, stderr)


def run_selftest() -> list[str]:
    """Prove a stop in the child-publication gap is forwarded immediately."""
    failures: list[str] = []

    def request_stop(_process: subprocess.Popen[str]) -> None:
        os.kill(os.getpid(), signal.SIGTERM)

    with stop_handlers():
        result = command_runner(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            before_publication=request_stop,
        )
    if result.status != 128 + signal.SIGTERM:
        failures.append("signal pending during child publication was not forwarded")
    STOP_STATE.process_signal = None
    return failures
