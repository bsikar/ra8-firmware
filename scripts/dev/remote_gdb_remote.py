# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Supervise exactly one remote J-Link GDB server process."""

from __future__ import annotations

import ctypes
import os
import re
import select
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

PORT_MIN = 1024
PORT_MAX = 65535
IDENTIFIER_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}")
LISTEN_STATE = "0A"
START_TIMEOUT_SECONDS = 10.0
STOP_TIMEOUT_SECONDS = 5.0
POLL_SECONDS = 0.05
PR_SET_PDEATHSIG = 1
PROC_FIELDS_MIN = 10
REMOTE_ARG_COUNT = 4
_STOP_REQUESTED = [False]


class RemoteError(RuntimeError):
    """The remote server could not be started or supervised safely."""


def _fail(message: str) -> NoReturn:
    raise RemoteError(message)


@dataclass(frozen=True)
class SupervisorHooks:
    """Injectable boundaries keep the selftest offline and non-signalling."""

    spawn: Callable[[list[str]], object]
    port_busy: Callable[[int], bool]
    listener_owned: Callable[[int, int], bool]
    channel_open: Callable[[], bool]
    getppid: Callable[[], int]
    set_parent_death: Callable[[int], None]
    stop_requested: Callable[[], bool]
    monotonic: Callable[[], float]
    sleep: Callable[[float], None]


def _identifier(value: str, label: str) -> str:
    if IDENTIFIER_RE.fullmatch(value) is None:
        message = f"{label} is not a bounded SEGGER identifier"
        raise RemoteError(message)
    return value


def _port(value: str) -> int:
    if not value.isascii() or not value.isdecimal():
        _fail("port must be decimal")
    port = int(value, 10)
    if not PORT_MIN <= port <= PORT_MAX:
        _fail("port is outside the unprivileged TCP range")
    return port


def _closed_arguments(arguments: list[str]) -> tuple[str, str, int]:
    """Parse only the exact argv produced by the local transport authority."""
    if len(arguments) != REMOTE_ARG_COUNT or arguments[0] != "--":
        _fail("expected -- DEVICE SERIAL PORT")
    return (
        _identifier(arguments[1], "device"),
        _identifier(arguments[2], "serial"),
        _port(arguments[3]),
    )


def _server_path() -> str:
    selected = shutil.which("JLinkGDBServerCLExe") or shutil.which("JLinkGDBServer")
    if selected is None:
        _fail("J-Link GDB server is not installed on the rig")
    try:
        path = Path(selected).resolve(strict=True)
        observed = path.stat()
    except OSError as exc:
        message = "J-Link GDB server path is unavailable"
        raise RemoteError(message) from exc
    if (
        not path.is_absolute()
        or not stat.S_ISREG(observed.st_mode)
        or not os.access(path, os.X_OK)
        or observed.st_uid not in {0, os.getuid()}
        or observed.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        _fail("J-Link GDB server path is not a protected executable")
    return str(path)


def _listener_inodes(port: int, proc_root: Path = Path("/proc")) -> set[str]:
    """Return Linux TCP-listener socket inodes for one local port."""
    found: set[str] = set()
    for name in ("tcp", "tcp6"):
        path = proc_root / "net" / name
        try:
            lines = path.read_text(encoding="ascii").splitlines()[1:]
        except OSError as exc:
            message = f"cannot inspect {path}"
            raise RemoteError(message) from exc
        for line in lines:
            fields = line.split()
            if len(fields) < PROC_FIELDS_MIN or fields[3] != LISTEN_STATE:
                continue
            try:
                observed_port = int(fields[1].rsplit(":", 1)[1], 16)
            except (IndexError, ValueError) as exc:
                message = f"malformed listener table {path}"
                raise RemoteError(message) from exc
            if observed_port == port:
                found.add(fields[9])
    return found


def _process_socket_inodes(pid: int, proc_root: Path = Path("/proc")) -> set[str]:
    """Return socket inodes retained by one exact, unreaped process."""
    descriptors = proc_root / str(pid) / "fd"
    try:
        entries = tuple(descriptors.iterdir())
    except OSError as exc:
        message = "cannot inspect J-Link server descriptors"
        raise RemoteError(message) from exc
    found: set[str] = set()
    for entry in entries:
        try:
            target = str(entry.readlink())
        except OSError:
            continue
        if target.startswith("socket:[") and target.endswith("]"):
            found.add(target[8:-1])
    return found


def _port_busy(port: int) -> bool:
    return bool(_listener_inodes(port))


def _listener_owned(pid: int, port: int) -> bool:
    listeners = _listener_inodes(port)
    return bool(listeners and listeners & _process_socket_inodes(pid))


def _channel_open() -> bool:
    """Keep the server only while the owning SSH stdout channel is live."""
    poller = select.poll()
    poller.register(sys.stdout.fileno(), select.POLLOUT | select.POLLERR | select.POLLHUP)
    return not any(
        events & (select.POLLERR | select.POLLHUP | select.POLLNVAL)
        for _descriptor, events in poller.poll(0)
    )


def _set_parent_death(expected_parent: int) -> None:
    """Make loss of the ssh-owned command shell terminate this supervisor."""
    if not sys.platform.startswith("linux") or expected_parent <= 1:
        _fail("remote parent-death authority requires Linux")
    library = ctypes.CDLL(None, use_errno=True)
    if library.prctl(PR_SET_PDEATHSIG, signal.SIGTERM, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        _fail(f"cannot install parent-death signal: errno {error}")
    if os.getppid() != expected_parent:
        _fail("remote command parent changed during startup")


def _request_stop(_signal_number: int, _frame: object) -> None:
    _STOP_REQUESTED[0] = True


def _install_signal_handlers() -> None:
    for selected in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(selected, _request_stop)


def _stop_requested() -> bool:
    return _STOP_REQUESTED[0]


def _spawn(arguments: list[str]) -> subprocess.Popen[bytes]:
    return subprocess.Popen(  # noqa: S603 - executable is protected and argv is closed.
        arguments,
        stdin=subprocess.DEVNULL,
        stdout=None,
        stderr=None,
        close_fds=True,
    )


def _default_hooks() -> SupervisorHooks:
    return SupervisorHooks(
        _spawn,
        _port_busy,
        _listener_owned,
        _channel_open,
        os.getppid,
        _set_parent_death,
        _stop_requested,
        time.monotonic,
        time.sleep,
    )


def _stop_child(process: object) -> None:
    """Signal only the retained, direct child while its PID cannot be reused."""
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=STOP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=STOP_TIMEOUT_SECONDS)


def _ready_line(line: str) -> None:
    print(line, flush=True)


def supervise(
    arguments: list[str],
    port: int,
    hooks: SupervisorHooks | None = None,
    ready: Callable[[str], None] = _ready_line,
) -> int:
    """Run one direct child until it exits or either owner boundary disappears."""
    selected = _default_hooks() if hooks is None else hooks
    parent = selected.getppid()
    selected.set_parent_death(parent)
    if selected.port_busy(port):
        _fail(f"TCP port {port} already has a listener")
    process = selected.spawn(arguments)
    stopping = True
    try:
        deadline = selected.monotonic() + START_TIMEOUT_SECONDS
        while selected.monotonic() < deadline:
            result = process.poll()
            if result is not None:
                stopping = False
                _fail(f"J-Link server exited before listening ({result})")
            if selected.getppid() != parent or not selected.channel_open():
                _fail("owning SSH channel closed before server readiness")
            if selected.stop_requested():
                _fail("remote server startup was interrupted")
            if selected.listener_owned(process.pid, port):
                ready(f"RA8_REMOTE_GDB_READY port={port}")
                break
            selected.sleep(POLL_SECONDS)
        else:
            _fail(f"timed out waiting for owned listener on port {port}")

        while process.poll() is None:
            if (
                selected.stop_requested()
                or selected.getppid() != parent
                or not selected.channel_open()
            ):
                return 0
            selected.sleep(POLL_SECONDS)
        stopping = False
        return int(process.returncode)
    finally:
        if stopping:
            _stop_child(process)


class _FakeProcess:
    """Minimal retained-child model for offline lifecycle tests."""

    def __init__(self, exit_after: int | None = None, *, ignore_terminate: bool = False) -> None:
        self.pid = 4242
        self.returncode: int | None = None
        self.polls = 0
        self.exit_after = exit_after
        self.ignore_terminate = ignore_terminate
        self.terminated = 0
        self.killed = 0

    def poll(self) -> int | None:
        self.polls += 1
        if self.exit_after is not None and self.polls >= self.exit_after:
            self.returncode = 0
        return self.returncode

    def terminate(self) -> None:
        self.terminated += 1
        if not self.ignore_terminate:
            self.returncode = -signal.SIGTERM

    def kill(self) -> None:
        self.killed += 1
        self.returncode = -signal.SIGKILL

    def wait(self, timeout: float) -> int:
        del timeout
        if self.returncode is None:
            command = "fake"
            raise subprocess.TimeoutExpired(command, STOP_TIMEOUT_SECONDS)
        return self.returncode


def _fake_hooks(
    process: _FakeProcess,
    *,
    busy: bool = False,
    channel: Callable[[], bool] = lambda: True,
    parent: Callable[[], int] = lambda: 77,
    stop: Callable[[], bool] = lambda: False,
) -> SupervisorHooks:
    clock = [0.0]

    def sleep(interval: float) -> None:
        clock[0] += interval

    return SupervisorHooks(
        lambda _arguments: process,
        lambda _port: busy,
        lambda _pid, _port: True,
        channel,
        parent,
        lambda _parent: None,
        stop,
        lambda: clock[0],
        sleep,
    )


def _proc_fixture(root: Path, pid: int, port: int) -> None:
    """Create one listener table and one process descriptor for identity tests."""
    (root / "net").mkdir(parents=True)
    (root / str(pid) / "fd").mkdir(parents=True)
    header = (
        "sl local_address rem_address st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
    )
    row = f"0: 0100007F:{port:04X} 00000000:0000 0A 0:0 0:0 0 0 0 98765\n"
    for name in ("tcp", "tcp6"):
        (root / "net" / name).write_text(header + (row if name == "tcp" else ""), encoding="ascii")
    (root / str(pid) / "fd" / "4").symlink_to("socket:[98765]")


def _lifecycle_cases(failures: list[str]) -> None:
    """Prove natural exit, channel loss, busy port, and pre-ready exit."""
    ready: list[str] = []
    process = _FakeProcess(exit_after=5)
    try:
        result = supervise(["/protected/server"], 2331, _fake_hooks(process), ready.append)
        if result != 0 or ready != ["RA8_REMOTE_GDB_READY port=2331"] or process.terminated:
            failures.append("natural direct-child lifecycle was not preserved")
    except RemoteError as exc:
        failures.append(f"valid lifecycle failed: {exc}")

    process = _FakeProcess()
    channels = iter((True, False))
    try:
        result = supervise(
            ["/protected/server"],
            2331,
            _fake_hooks(process, channel=lambda: next(channels, False)),
            lambda _line: None,
        )
        if result != 0 or process.terminated != 1 or process.killed:
            failures.append("channel loss did not terminate exactly one retained child")
    except RemoteError as exc:
        failures.append(f"channel-loss lifecycle failed: {exc}")

    process = _FakeProcess()
    try:
        supervise(["/protected/server"], 2331, _fake_hooks(process, busy=True))
        failures.append("pre-existing listener was accepted")
    except RemoteError:
        if process.terminated or process.polls:
            failures.append("busy-port refusal touched an unspawned process")

    process = _FakeProcess(exit_after=1)
    try:
        supervise(["/protected/server"], 2331, _fake_hooks(process))
        failures.append("pre-readiness server exit was accepted")
    except RemoteError:
        if process.terminated:
            failures.append("already-reaped PID was signalled during cleanup")


def _identity_cases(failures: list[str]) -> None:
    """Prove listener ownership is bound to the direct process descriptor."""
    with tempfile.TemporaryDirectory(prefix="ra8-remote-gdb-proc-", dir="/tmp") as directory:
        proc = Path(directory)
        _proc_fixture(proc, 4242, 2331)
        if not (_listener_inodes(2331, proc) & _process_socket_inodes(4242, proc)):
            failures.append("owned listener identity was not recognized")
        if _listener_inodes(2332, proc):
            failures.append("unowned listener identity was accepted")
        try:
            _process_socket_inodes(4243, proc)
            failures.append("absent process identity was accepted")
        except RemoteError:
            pass


def _owner_loss_cases(failures: list[str]) -> None:
    """Prove every liveness boundary cleans the same retained child."""
    process = _FakeProcess()
    parents = iter((77, 77, 78))
    result = supervise(
        ["/protected/server"],
        2331,
        _fake_hooks(process, parent=lambda: next(parents, 78)),
        lambda _line: None,
    )
    if result != 0 or process.terminated != 1:
        failures.append("remote parent loss did not stop the retained child")

    process = _FakeProcess()
    stops = iter((False, True))
    result = supervise(
        ["/protected/server"],
        2331,
        _fake_hooks(process, stop=lambda: next(stops, True)),
        lambda _line: None,
    )
    if result != 0 or process.terminated != 1:
        failures.append("remote signal request did not stop the retained child")

    process = _FakeProcess(ignore_terminate=True)
    _stop_child(process)
    if process.terminated != 1 or process.killed != 1:
        failures.append("unresponsive retained child did not receive bounded escalation")


def _input_cases(failures: list[str]) -> None:
    """Prove unsafe remote fields remain rejected in both input classes."""
    for value in ("", "-1", "1023", "65536", "23 31"):
        try:
            _port(value)
            failures.append(f"unsafe port passed: {value!r}")
        except RemoteError:
            pass
    for value in ("bad value", "-device", "bad;value"):
        try:
            _identifier(value, "device")
            failures.append(f"unsafe identifier passed: {value!r}")
        except RemoteError:
            pass
    try:
        parsed = _closed_arguments(["--", "R7KA8D2KF_CPU0", "123456789", "2331"])
        if parsed != ("R7KA8D2KF_CPU0", "123456789", 2331):
            failures.append("valid closed remote argv changed during parsing")
    except RemoteError as exc:
        failures.append(f"valid closed remote argv failed: {exc}")
    for arguments in (
        ["R7KA8D2KF_CPU0", "123456789", "2331"],
        ["--", "123456789", "2331", "R7KA8D2KF_CPU0"],
        ["--", "R7KA8D2KF_CPU0", "123456789", "2331", "extra"],
    ):
        try:
            _closed_arguments(arguments)
            failures.append(f"invalid closed remote argv passed: {arguments!r}")
        except RemoteError:
            pass


def selftest() -> int:
    """Exercise both lifecycle directions without opening a socket or signalling."""
    failures: list[str] = []
    _lifecycle_cases(failures)
    _identity_cases(failures)
    _owner_loss_cases(failures)
    _input_cases(failures)
    if failures:
        for failure in failures:
            print(f"  [FAIL] {failure}", file=sys.stderr)
        return 1
    print("remote_gdb_remote.py: PASS (direct-child/readiness/channel/PID-reuse)")
    return 0


def main() -> int:
    """Validate the closed remote argv and supervise the selected server."""
    arguments = sys.argv[1:]
    if arguments == ["--selftest"]:
        return selftest()
    try:
        device, serial, port = _closed_arguments(arguments)
        server = _server_path()
        _install_signal_handlers()
        argv = [
            server,
            "-device",
            device,
            "-if",
            "SWD",
            "-speed",
            "1000",
            "-port",
            str(port),
            "-nogui",
            "-select",
            f"USB={serial}",
        ]
        return supervise(argv, port)
    except RemoteError as exc:
        print(f"remote_gdb_remote: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
