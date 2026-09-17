#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Serialize fleet mutations through an independent dev-host lock guardian."""

from __future__ import annotations

import argparse
import fcntl
import os
import pwd
import select
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator, Sequence
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path
from threading import Lock
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fleet_model as fm
import fleet_reach as fr

LOCK_BUSY_STATUS = 75
DIRECTORY_MODE = 0o700
LOCK_MODE = 0o600
REGISTER_FIELD_COUNT = 2
CANCELLED_STATUS = 125
CHILD_WAIT_TIMEOUT = 2
GUARDIAN_ERROR_STATUS = 2
LOCK_READY = b"RA8-FLEET-MUTATION-LOCKED\n"
GUARDIAN_FD_ENV = "RA8_FLEET_MUTATION_GUARDIAN_FD"
HOLDER_CONNECT_TIMEOUT = 15
HOLDER_ALIVE_INTERVAL = 5
HOLDER_ALIVE_COUNT = 3
HOLDER_READY_TIMEOUT = 20
GATED_EXEC = (
    "import os,sys;"
    "fd=int(sys.argv[1]);token=os.read(fd,1);os.close(fd);"
    "sys.exit(125) if token != b'1' else os.execvp(sys.argv[2],sys.argv[2:])"
)
GUARDIAN_REPLY_TIMEOUT = 5
REMOTE_HOLDER = (
    "/bin/bash --noprofile --norc -p -c '"
    "set -e; umask 077; "
    'd="$HOME/.local/state/ra8-fleet-mutation"; lock="$d/mutation.lock"; '
    '/usr/bin/mkdir -p -- "$d"; '
    '[ ! -L "$d" ] && [ -d "$d" ]; '
    'uid="$(/usr/bin/id -u)"; '
    '[ "$(/usr/bin/stat -c %u -- "$d")" = "$uid" ]; '
    '[ "$(/usr/bin/stat -c %a -- "$d")" = 700 ]; '
    '[ ! -e "$lock" ] || { [ ! -L "$lock" ] && [ -f "$lock" ]; }; '
    ': >>"$lock"; /usr/bin/chmod 600 -- "$lock"; '
    '[ "$(/usr/bin/stat -c %u -- "$lock")" = "$uid" ]; '
    '[ "$(/usr/bin/stat -c %a -- "$lock")" = 600 ]; '
    'exec 9>>"$lock"; '
    'path_meta="$(/usr/bin/stat -c %d:%i:%u:%a:%F -- "$lock")"; '
    'fd_meta="$(/usr/bin/stat -Lc %d:%i:%u:%a:%F -- /proc/$$/fd/9)"; '
    '[ "$path_meta" = "$fd_meta" ]; '
    '/usr/bin/flock -n -E 75 9 || { rc=$?; [ "$rc" -eq 75 ] && exit 75; exit "$rc"; }; '
    f'printf "{LOCK_READY.decode().rstrip()}\\n"; '
    "/bin/cat >/dev/null'"
)


class MutationLockBusyError(RuntimeError):
    """Another controller owns the dev-host fleet mutation lock."""


class MutationLockError(RuntimeError):
    """The dev-host fleet mutation authority could not be reached safely."""


@dataclass
class _CapabilityState:
    """Mutable process-local capability state without module rebinding."""

    active: socket.socket | None = None


_CAPABILITY_STATE = _CapabilityState()
_CAPABILITY_LOCK = Lock()


def authority_host(data: dict[str, Any]) -> str:
    """Return the unique declared dev control host."""
    names = [name for name, host in data["hosts"].items() if host.get("class") == "dev_box"]
    if len(names) != 1:
        message = f"expected one dev_box mutation authority, found {len(names)}"
        raise MutationLockError(message)
    return names[0]


def _local_lock_path() -> Path:
    """Return a validated caller-owned local authority path."""
    home = Path(pwd.getpwuid(os.getuid()).pw_dir)
    directory = home / ".local/state/ra8-fleet-mutation"
    try:
        directory.mkdir(mode=DIRECTORY_MODE, parents=True, exist_ok=True)
    except OSError as error:
        message = "cannot create local mutation lock directory"
        raise MutationLockError(message) from error
    metadata = directory.lstat()
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.getuid()
        or stat.S_IMODE(metadata.st_mode) != DIRECTORY_MODE
    ):
        message = "local mutation lock directory must be real, caller-owned, mode 0700"
        raise MutationLockError(message)
    return directory / "mutation.lock"


def _exclusive(path: Path) -> object:
    """Open and acquire a validated nonblocking local lock."""
    flags = os.O_RDWR | os.O_CREAT | os.O_APPEND | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags, LOCK_MODE)
    stream = os.fdopen(descriptor, "a+", encoding="ascii")
    acquired = False
    try:
        metadata = os.fstat(descriptor)
        path_metadata = path.lstat()
        if (
            not stat.S_ISREG(metadata.st_mode)
            or stat.S_ISLNK(path_metadata.st_mode)
            or metadata.st_uid != os.getuid()
            or stat.S_IMODE(metadata.st_mode) != LOCK_MODE
            or (metadata.st_dev, metadata.st_ino) != (path_metadata.st_dev, path_metadata.st_ino)
        ):
            message = "local mutation lock must be one caller-owned mode-0600 inode"
            raise MutationLockError(message)
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        acquired = True
        return stream
    finally:
        if not acquired:
            stream.close()


def _holder_argv(data: dict[str, Any]) -> list[str]:
    """Build the holder-only SSH transport with bounded liveness."""
    target = fr.ssh_target(data, authority_host(data))
    return [
        target[0],
        "-o",
        f"ConnectTimeout={HOLDER_CONNECT_TIMEOUT}",
        "-o",
        f"ServerAliveInterval={HOLDER_ALIVE_INTERVAL}",
        "-o",
        f"ServerAliveCountMax={HOLDER_ALIVE_COUNT}",
        *target[1:],
        REMOTE_HOLDER,
    ]


def _terminate_and_reap(process: subprocess.Popen[bytes]) -> None:
    """Bound termination and always reap a holder transport."""
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=CHILD_WAIT_TIMEOUT)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _read_holder_ready(process: subprocess.Popen[bytes], timeout: float) -> bytes:
    """Read the holder token without permitting a silent transport to hang."""
    if process.stdout is None:
        message = "dev-host lock transport has no output pipe"
        raise MutationLockError(message)
    readable, _, _ = select.select([process.stdout], [], [], timeout)
    if not readable:
        message = "dev-host mutation lock READY handshake timed out"
        raise MutationLockError(message)
    return process.stdout.readline()


def _finish_holder_handshake(process: subprocess.Popen[bytes]) -> None:
    """Validate READY or classify the exact pre-token holder failure."""
    token = _read_holder_ready(process, HOLDER_READY_TIMEOUT)
    if token == LOCK_READY:
        return
    try:
        status = process.wait(timeout=CHILD_WAIT_TIMEOUT)
    except subprocess.TimeoutExpired as error:
        message = "dev-host lock transport was silent before READY"
        raise MutationLockError(message) from error
    if status == LOCK_BUSY_STATUS:
        message = "another controller owns the dev-host mutation lock"
        raise MutationLockBusyError(message)
    message = f"dev-host mutation lock setup failed before READY (rc={status})"
    raise MutationLockError(message)


def _open_remote_holder(data: dict[str, Any]) -> subprocess.Popen[bytes]:
    """Acquire the remote flock through a bounded token handshake."""
    process = subprocess.Popen(  # noqa: S603 -- fixed SSH executable and remote program
        _holder_argv(data), stdin=subprocess.PIPE, stdout=subprocess.PIPE
    )
    try:
        _finish_holder_handshake(process)
    except (MutationLockError, OSError, subprocess.SubprocessError):
        _terminate_and_reap(process)
        raise
    return process


def _group_exists(process_group: int) -> bool:
    """Return whether a protected group retains any live member."""
    try:
        entries = tuple(Path("/proc").iterdir())
    except OSError:
        return True
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            raw = (entry / "stat").read_bytes()
        except FileNotFoundError:
            continue
        except OSError:
            return True
        closing = raw.rfind(b")")
        fields = raw[closing + 2 :].split() if closing >= 0 else []
        try:
            state, _parent, group, *_remaining = fields
        except ValueError:
            return True
        if not group.isdigit():
            return True
        if int(group) == process_group and state != b"Z":
            return True
    return False


def _signal_groups(groups: set[int], process_signal: int) -> None:
    """Signal every process group registered with this guardian."""
    for process_group in groups:
        with suppress(ProcessLookupError):
            os.killpg(process_group, process_signal)


def _guardian_loop(capability: socket.socket, holder: object) -> int:
    """Own the holder until capability closure and every mutation group exit."""
    groups: set[int] = set()
    released = False
    remote = holder if isinstance(holder, subprocess.Popen) else None
    capability.settimeout(0.0)
    while True:
        groups = {group for group in groups if _group_exists(group)}
        if remote is not None and remote.poll() is not None:
            _signal_groups(groups, signal.SIGTERM)
            time.sleep(0.2)
            _signal_groups(groups, signal.SIGKILL)
            with suppress(OSError):
                capability.send(b"LOST")
            return 2
        if released and not groups:
            return 0
        readable, _, _ = select.select([capability], [], [], 0.2)
        if not readable:
            continue
        try:
            request = capability.recv(128)
        except BlockingIOError:
            continue
        if not request:
            released = True
            continue
        if request == b"PING":
            capability.send(b"ACK")
            continue
        fields = request.decode("ascii", errors="strict").split()
        if len(fields) != REGISTER_FIELD_COUNT or fields[0] != "REGISTER":
            capability.send(b"DENY")
            continue
        try:
            process_group = int(fields[1])
        except ValueError:
            capability.send(b"DENY")
            continue
        if process_group <= 0:
            capability.send(b"DENY")
            continue
        groups.add(process_group)
        capability.send(b"ACK")


def _guardian_main(
    data: dict[str, Any], installed_local: bool, capability: socket.socket, status_fd: int
) -> None:
    """Acquire authority, publish READY, and supervise independently."""
    holder: object | None = None
    status = os.fdopen(status_fd, "wb", buffering=0)
    try:
        holder = _exclusive(_local_lock_path()) if installed_local else _open_remote_holder(data)
        status.write(b"READY\n")
        result = _guardian_loop(capability, holder)
        status.write(f"EXIT {result}\n".encode("ascii"))
    except MutationLockBusyError:
        status.write(b"BUSY\n")
        result = LOCK_BUSY_STATUS
    except (
        MutationLockError,
        OSError,
        ValueError,
        UnicodeError,
        subprocess.SubprocessError,
    ) as error:
        status.write(f"ERROR {type(error).__name__}: {error}\n".encode("utf-8", errors="replace"))
        result = GUARDIAN_ERROR_STATUS
    finally:
        if isinstance(holder, subprocess.Popen):
            if holder.stdin is not None:
                with suppress(BrokenPipeError):
                    holder.stdin.close()
            _terminate_and_reap(holder)
        elif holder is not None:
            holder.close()
        capability.close()
        status.close()
    os._exit(result)


def _read_status_line(descriptor: int, timeout: float) -> bytes:
    """Read one bounded guardian status record."""
    readable, _, _ = select.select([descriptor], [], [], timeout)
    if not readable:
        message = "mutation guardian startup timed out"
        raise MutationLockError(message)
    output = bytearray()
    while not output.endswith(b"\n"):
        chunk = os.read(descriptor, 1)
        if not chunk:
            break
        output += chunk
    return bytes(output)


def _start_guardian(data: dict[str, Any], installed_local: bool) -> tuple[int, socket.socket, int]:
    """Fork a session-independent guardian and await authoritative readiness."""
    parent_socket, guardian_socket = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    status_read, status_write = os.pipe()
    pid = os.fork()
    if pid == 0:
        parent_socket.close()
        os.close(status_read)
        with suppress(OSError):
            os.setsid()
        for process_signal in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGQUIT):
            signal.signal(process_signal, signal.SIG_IGN)
        _guardian_main(data, installed_local, guardian_socket, status_write)
    guardian_socket.close()
    os.close(status_write)
    try:
        record = _read_status_line(status_read, HOLDER_READY_TIMEOUT + 5)
    except (MutationLockError, OSError):
        parent_socket.close()
        with suppress(ProcessLookupError):
            os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        os.close(status_read)
        raise
    if record == b"READY\n":
        return pid, parent_socket, status_read
    parent_socket.close()
    os.waitpid(pid, 0)
    os.close(status_read)
    if record == b"BUSY\n":
        message = "another controller owns the dev-host mutation lock"
        raise MutationLockBusyError(message)
    message = record.decode("utf-8", errors="replace").strip()
    if not message:
        message = "guardian exited before READY"
    raise MutationLockError(message)


def _socket_from_descriptor(descriptor: int) -> socket.socket:
    """Validate and adopt one inherited AF_UNIX capability descriptor."""
    candidate = socket.socket(fileno=descriptor)
    if candidate.family != socket.AF_UNIX:
        candidate.close()
        message = "guardian descriptor is not an AF_UNIX socket"
        raise OSError(message)
    return candidate


def _capability_socket() -> socket.socket:
    """Resolve the active inherited guardian capability, never an environment claim alone."""
    if _CAPABILITY_STATE.active is not None:
        return _CAPABILITY_STATE.active
    raw = os.environ.get(GUARDIAN_FD_ENV)
    if raw is None:
        message = "mutating fleet command requires a live guardian capability"
        raise MutationLockError(message)
    try:
        descriptor = int(raw)
        _CAPABILITY_STATE.active = _socket_from_descriptor(descriptor)
    except (ValueError, OSError) as error:
        message = "inherited guardian capability is invalid"
        raise MutationLockError(message) from error
    return _CAPABILITY_STATE.active


def require_guardian_capability() -> None:
    """Validate live authority and register this mutation process group."""
    capability = _capability_socket()
    with _CAPABILITY_LOCK:
        prior_timeout = capability.gettimeout()
        try:
            capability.settimeout(GUARDIAN_REPLY_TIMEOUT)
            capability.send(f"REGISTER {os.getpgrp()}".encode("ascii"))
            if capability.recv(16) != b"ACK":
                message = "mutation guardian denied the process group"
                raise MutationLockError(message)
        except (OSError, TimeoutError) as error:
            message = "mutation guardian lease is lost"
            raise MutationLockError(message) from error
        finally:
            with suppress(OSError):
                capability.settimeout(prior_timeout)


def guardian_subprocess_kwargs() -> dict[str, object]:
    """Prove live authority, then return the FD inheritance for a guarded spawn."""
    capability = _capability_socket()
    with _CAPABILITY_LOCK:
        prior_timeout = capability.gettimeout()
        try:
            capability.settimeout(GUARDIAN_REPLY_TIMEOUT)
            capability.send(b"PING")
            if capability.recv(16) != b"ACK":
                message = "mutation guardian denied child preparation"
                raise MutationLockError(message)
        except (OSError, TimeoutError) as error:
            message = "mutation guardian was lost before child spawn"
            raise MutationLockError(message) from error
        finally:
            with suppress(OSError):
                capability.settimeout(prior_timeout)
    descriptor = capability.fileno()
    environment = os.environ.copy()
    environment[GUARDIAN_FD_ENV] = str(descriptor)
    return {"env": environment, "pass_fds": (descriptor,)}


@contextmanager
def mutation_lock(
    data: dict[str, Any], *, installed_local: bool = False, on_loss: object | None = None
) -> Iterator[None]:
    """Expose one guardian capability while it owns the canonical flock."""
    del on_loss
    if _CAPABILITY_STATE.active is not None:
        message = "nested mutation guardians are forbidden"
        raise MutationLockError(message)
    pid, capability, status_fd = _start_guardian(data, installed_local)
    _CAPABILITY_STATE.active = capability
    body_completed = False
    try:
        yield
        body_completed = True
    finally:
        _CAPABILITY_STATE.active = None
        capability.close()
        _, wait_status = os.waitpid(pid, 0)
        ready = select.select([status_fd], [], [], 0)[0]
        final = _read_status_line(status_fd, 0) if ready else b""
        os.close(status_fd)
        failed = not os.WIFEXITED(wait_status) or os.WEXITSTATUS(wait_status) != 0
        if body_completed and failed:
            detail = final.decode("utf-8", errors="replace").strip()
            message = detail or "mutation guardian lost authority"
            raise MutationLockError(message)


def _gated_argv(descriptor: int, argv: Sequence[str]) -> list[str]:
    """Build an inert bootstrap that execs command code only after one token."""
    return [sys.executable, "-I", "-c", GATED_EXEC, str(descriptor), *argv]


def run_locked(data: dict[str, Any], argv: Sequence[str]) -> int:
    """Run one complete process group under an independent guardian."""
    child: subprocess.Popen[bytes] | None = None
    pending_signal = 0

    def forward(process_signal: int, _frame: object) -> None:
        nonlocal pending_signal
        pending_signal = process_signal
        if child is not None:
            with suppress(ProcessLookupError):
                os.killpg(child.pid, process_signal)

    handled = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT, signal.SIGQUIT)
    previous = {item: signal.signal(item, forward) for item in handled}
    try:
        with mutation_lock(data):
            if pending_signal:
                return 128 + pending_signal
            gate_read, gate_write = os.pipe()
            blocked = signal.pthread_sigmask(signal.SIG_BLOCK, handled)
            try:
                kwargs = guardian_subprocess_kwargs()
                pass_fds = (*kwargs["pass_fds"], gate_read)
                child = subprocess.Popen(  # noqa: S603 -- caller supplies exact argv
                    _gated_argv(gate_read, argv),
                    start_new_session=True,
                    env=kwargs["env"],
                    pass_fds=pass_fds,
                )
                os.close(gate_read)
                gate_read = -1
                require_guardian_capability_for_group(child.pid)
                queued = set(signal.sigpending()).intersection(handled)
                cancellation = pending_signal or (min(queued) if queued else 0)
                if cancellation:
                    with suppress(ProcessLookupError):
                        os.killpg(child.pid, cancellation)
                else:
                    os.write(gate_write, b"1")
            except BaseException:
                if child is not None:
                    with suppress(ProcessLookupError):
                        os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                raise
            finally:
                if gate_read >= 0:
                    os.close(gate_read)
                os.close(gate_write)
                signal.pthread_sigmask(signal.SIG_SETMASK, blocked)
            return child.wait()
    finally:
        for process_signal, handler in previous.items():
            signal.signal(process_signal, handler)


def require_guardian_capability_for_group(process_group: int) -> None:
    """Register a just-created, still-inert protected process group."""
    capability = _capability_socket()
    with _CAPABILITY_LOCK:
        prior_timeout = capability.gettimeout()
        try:
            capability.settimeout(GUARDIAN_REPLY_TIMEOUT)
            capability.send(f"REGISTER {process_group}".encode("ascii"))
            if capability.recv(16) != b"ACK":
                message = "mutation guardian refused child publication"
                raise MutationLockError(message)
        except (OSError, TimeoutError) as error:
            message = "mutation guardian was lost before child publication"
            raise MutationLockError(message) from error
        finally:
            with suppress(OSError):
                capability.settimeout(prior_timeout)


def _boundary_contract_errors(infra_text: str) -> list[str]:
    """Require every direct wrapper mutation to enter the guardian."""
    required = (
        'MUTATION_LOCK="${ROOT}/scripts/dev/fleet_mutation_lock.py"\n',
        '  "$PYTHON" -I "$MUTATION_LOCK" -- "$PYTHON" -I "$FLEET" "$@"\n',
        '  fleet_mutation apply "$@"\n',
        '  fleet_mutation register-runner "$@"\n',
        '  fleet_mutation register-hil "$@"\n',
        '  fleet_mutation remove "$@"\n',
        '  fleet_mutation scale "$1" "$2"\n',
    )
    return (
        []
        if all(infra_text.count(item) == 1 for item in required)
        else ["a supported direct mutation bypasses the independent guardian"]
    )


def _capability_selftest() -> list[str]:
    """Prove an environment claim cannot manufacture mutation authority."""
    failures: list[str] = []
    prior = os.environ.get(GUARDIAN_FD_ENV)
    os.environ[GUARDIAN_FD_ENV] = "999999"
    try:
        require_guardian_capability()
        failures.append("an environment-only guardian claim was accepted")
    except MutationLockError:
        pass
    finally:
        if prior is None:
            os.environ.pop(GUARDIAN_FD_ENV, None)
        else:
            os.environ[GUARDIAN_FD_ENV] = prior
    return failures


def _bench_reentry_selftest() -> list[str]:
    """Prove validated authority remains inheritable by a nested mutation entry."""
    parent, guardian = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    guardian_pid = os.fork()
    if guardian_pid == 0:
        parent.close()
        for expected in (b"PING", b"REGISTER"):
            request = guardian.recv(128)
            if not request.startswith(expected):
                os._exit(2)
            guardian.send(b"ACK")
        os._exit(0)
    guardian.close()
    descriptor = parent.fileno()
    previous = os.environ.get(GUARDIAN_FD_ENV)
    os.environ[GUARDIAN_FD_ENV] = str(descriptor)
    _CAPABILITY_STATE.active = None
    kwargs = guardian_subprocess_kwargs()
    nested = os.fork()
    if nested == 0:
        _CAPABILITY_STATE.active.detach()
        _CAPABILITY_STATE.active = None
        os.environ.clear()
        os.environ.update(kwargs["env"])
        os.setsid()
        require_guardian_capability()
        os._exit(0)
    _, nested_status = os.waitpid(nested, 0)
    parent.close()
    _CAPABILITY_STATE.active = None
    _, guardian_status = os.waitpid(guardian_pid, 0)
    if previous is None:
        os.environ.pop(GUARDIAN_FD_ENV, None)
    else:
        os.environ[GUARDIAN_FD_ENV] = previous
    inherited = kwargs["pass_fds"] == (descriptor,)
    if not inherited or nested_status != 0 or guardian_status != 0:
        return ["nested bench mutation did not inherit and register the live guardian"]
    return []


def _metadata_selftest() -> list[str]:
    """Prove remote setup and holder transport retain their fail-closed clauses."""
    failures: list[str] = []
    clauses = ("set -e", "[ ! -L", "stat -c %u", "stat -c %a", "%d:%i", "exit 75")
    if any(clause not in REMOTE_HOLDER for clause in clauses):
        failures.append("remote holder setup metadata checks are incomplete")
    ready_clause = f'printf "{LOCK_READY.decode().rstrip()}\\n"'
    if ready_clause not in REMOTE_HOLDER:
        failures.append("remote holder READY record is not newline-delimited")
    fake = {"hosts": {"dev": {"class": "dev_box", "connect": {"address": "127.0.0.1"}}}}
    argv = _holder_argv(fake)
    options = ("ConnectTimeout=15", "ServerAliveInterval=5", "ServerAliveCountMax=3")
    failures.extend(f"holder transport omits {option}" for option in options if option not in argv)
    return failures


def _two_controller_selftest(path: Path) -> list[str]:
    """Prove one live kernel lock excludes a second controller."""
    failures: list[str] = []
    ready_read, ready_write = os.pipe()
    release_read, release_write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(ready_read)
        os.close(release_write)
        try:
            with _exclusive(path):
                os.write(ready_write, b"1")
                os.read(release_read, 1)
        finally:
            os._exit(0)
    os.close(ready_write)
    os.close(release_read)
    try:
        if os.read(ready_read, 1) != b"1":
            failures.append("first controller did not acquire the mutation lock")
        try:
            with _exclusive(path):
                failures.append("second controller acquired the live mutation lock")
        except BlockingIOError:
            pass
    finally:
        os.write(release_write, b"1")
        os.close(release_write)
        os.close(ready_read)
        os.waitpid(pid, 0)
    with _exclusive(path):
        pass
    return failures


def _silent_transport_selftest() -> list[str]:
    """Prove a connected transport that emits no token is bounded and reaped."""
    process = subprocess.Popen(["/bin/sleep", "60"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        _read_holder_ready(process, 0.05)
    except MutationLockError:
        _terminate_and_reap(process)
    else:
        _terminate_and_reap(process)
        return ["silent pre-token holder transport did not time out"]
    if process.poll() is None:
        return ["timed-out holder transport was not reaped"]
    return []


def _holder_loss_selftest() -> list[str]:
    """Prove post-token holder death kills an already-published mutation group."""
    parent_capability, guardian_capability = socket.socketpair(
        socket.AF_UNIX, socket.SOCK_SEQPACKET
    )
    guardian_pid = os.fork()
    if guardian_pid == 0:
        parent_capability.close()
        holder = subprocess.Popen(["/bin/sleep", "0.15"])
        result = _guardian_loop(guardian_capability, holder)
        _terminate_and_reap(holder)
        os._exit(result)
    guardian_capability.close()
    child = subprocess.Popen(["/bin/sleep", "60"], start_new_session=True)
    parent_capability.send(f"REGISTER {child.pid}".encode("ascii"))
    acknowledged = parent_capability.recv(16) == b"ACK"
    try:
        child.wait(timeout=CHILD_WAIT_TIMEOUT)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait()
        failure = "holder death did not terminate the published mutation group"
    else:
        failure = ""
    parent_capability.close()
    os.waitpid(guardian_pid, 0)
    failures = [] if acknowledged else ["guardian did not publish the mutation child"]
    if failure:
        failures.append(failure)
    return failures


def _cancelled_spawn_selftest(path: Path) -> list[str]:
    """Prove cancellation closes the execution gate before command code runs."""
    gate_read, gate_write = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(gate_write)
        token = os.read(gate_read, 1)
        os.close(gate_read)
        if token != b"1":
            os._exit(CANCELLED_STATUS)
        path.touch()
        os._exit(0)
    os.close(gate_read)
    os.close(gate_write)
    _, wait_status = os.waitpid(child, 0)
    status = os.waitstatus_to_exitcode(wait_status)
    if status != CANCELLED_STATUS or path.exists():
        return ["cancellation before publication allowed mutation code to execute"]
    return []


def _hard_parent_death_selftest(path: Path) -> list[str]:
    """Prove controller SIGKILL cannot release authority before its child group."""
    ready_read, ready_write = os.pipe()
    controller = os.fork()
    if controller == 0:
        os.close(ready_read)
        lock_read, lock_write = os.pipe()
        parent_capability, guardian_capability = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_SEQPACKET
        )
        guardian = os.fork()
        if guardian == 0:
            parent_capability.close()
            os.close(lock_read)
            lock = _exclusive(path)
            os.write(lock_write, b"L")
            os.close(lock_write)
            result = _guardian_loop(guardian_capability, lock)
            lock.close()
            os._exit(result)
        guardian_capability.close()
        os.close(lock_write)
        if os.read(lock_read, 1) != b"L":
            os._exit(2)
        os.close(lock_read)
        child = subprocess.Popen(
            ["/bin/sleep", "0.4"],
            start_new_session=True,
            pass_fds=(parent_capability.fileno(),),
        )
        parent_capability.send(f"REGISTER {child.pid}".encode("ascii"))
        if parent_capability.recv(16) != b"ACK":
            os._exit(3)
        os.write(ready_write, b"C")
        signal.pause()
        os._exit(4)
    os.close(ready_write)
    if os.read(ready_read, 1) != b"C":
        os.kill(controller, signal.SIGKILL)
        os.waitpid(controller, 0)
        return ["hard-parent-death fixture did not publish its child"]
    os.close(ready_read)
    os.kill(controller, signal.SIGKILL)
    os.waitpid(controller, 0)
    try:
        with _exclusive(path):
            return ["controller SIGKILL released the flock while its child survived"]
    except BlockingIOError:
        pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            with _exclusive(path):
                return []
        except BlockingIOError:
            time.sleep(0.05)
    return ["guardian did not release after the complete child group exited"]


def _isolated_import_selftest() -> list[str]:
    """Prove the exact isolated interpreter entry can load local fleet modules."""
    result = subprocess.run(  # noqa: S603 -- fixed Python and current module
        ["/usr/bin/python3", "-I", str(Path(__file__).resolve()), "--selftest-import"],
        capture_output=True,
        check=False,
        text=True,
    )
    if result.returncode == 0:
        return []
    detail = result.stderr.strip() or f"exit {result.returncode}"
    return [f"isolated mutation-lock entry failed: {detail}"]


def run_selftest() -> list[str]:
    """Run deterministic boundary, metadata, capability, and exclusion proofs."""
    root = Path(__file__).resolve().parents[2]
    infra_text = (root / "scripts/dev/infra.sh").read_text(encoding="ascii")
    failures = (
        _isolated_import_selftest()
        + _boundary_contract_errors(infra_text)
        + _capability_selftest()
        + _metadata_selftest()
        + _silent_transport_selftest()
        + _holder_loss_selftest()
        + _bench_reentry_selftest()
        + _cancelled_spawn_selftest(Path(tempfile.gettempdir()) / f"ra8-cancel-{os.getpid()}")
    )
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-mutation-lock-") as raw:
        directory = Path(raw)
        directory.chmod(DIRECTORY_MODE)
        failures += _two_controller_selftest(directory / "mutation.lock")
        failures += _hard_parent_death_selftest(directory / "parent-death.lock")
    return failures


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse the offline selftest or one protected command."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--selftest-import", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    """Enter the lock selftest or execute one serialized fleet mutation."""
    args = parse_args(argv)
    if args.selftest_import:
        return 0
    if args.selftest:
        failures = run_selftest()
        for failure in failures:
            print(f"fleet_mutation_lock.py --selftest: FAIL: {failure}", file=sys.stderr)
        if not failures:
            print("fleet_mutation_lock.py --selftest: PASS")
        return int(bool(failures))
    command = list(args.command)
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        print("fleet-mutation-lock: a command is required", file=sys.stderr)
        return 2
    try:
        return run_locked(fm.load(), command)
    except MutationLockBusyError as error:
        print(f"fleet-mutation-lock: {error}", file=sys.stderr)
        return LOCK_BUSY_STATUS
    except (MutationLockError, OSError, fm.FleetError) as error:
        print(f"fleet-mutation-lock: FATAL: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
