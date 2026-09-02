# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Linux process-identity proofs for the remote-GDB state broker."""

from __future__ import annotations

import contextlib
import os
import select
import signal
import stat
from dataclasses import dataclass
from pathlib import Path

MAX_AUTHORITY_BYTES = 256 * 1024
RUN_ARGS_MIN = 3
RUN_ARGS_MAX = 6
RUN_TAIL_MAX = 3
PORT_ARG_COUNT = 2
SCRIPT_ARG = 2


class ProcessError(ValueError):
    """A Linux process claim could not be bound to one live identity."""


@dataclass(frozen=True)
class ProcessProof:
    """Authenticated identity of the live remote-GDB Bash process."""

    start_ticks: int
    argv: tuple[str, ...]


def pid_alive(pid: int) -> bool:
    """Return process existence without sending a state-changing signal."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def pidfd_live(descriptor: int) -> bool:
    """Require the retained Linux pidfd target to remain alive."""
    poller = select.poll()
    poller.register(descriptor, select.POLLIN | select.POLLHUP | select.POLLERR)
    return not poller.poll(0)


def signal_authority(parent_pid: int, platform: str) -> tuple[object, int | None]:
    """Acquire the strongest stdlib parent-signal capability for the platform."""
    if platform.startswith("linux"):
        try:
            descriptor = os.pidfd_open(parent_pid, 0)
        except (AttributeError, OSError) as exc:
            message = "Linux pidfd authority is unavailable"
            raise ProcessError(message) from exc

        def signal_pidfd(_pid: int) -> None:
            if not pidfd_live(descriptor):
                message = "remote-GDB parent exited before stop"
                raise ProcessError(message)
            signal.pidfd_send_signal(descriptor, signal.SIGTERM, None, 0)

        return signal_pidfd, descriptor

    def signal_parent(pid: int) -> None:
        os.kill(pid, signal.SIGTERM)

    return signal_parent, None


def start_ticks(proc_root: Path, pid: int) -> int:
    """Read Linux start ticks without splitting the comm field."""
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
        return int(raw[raw.rindex(")") + 2 :].split()[19])
    except (OSError, ValueError, IndexError) as exc:
        message = "cannot authenticate process start time"
        raise ProcessError(message) from exc


def process_uid(proc_root: Path, pid: int) -> int:
    """Read the effective process owner from procfs."""
    try:
        lines = (proc_root / str(pid) / "status").read_text(encoding="ascii").splitlines()
        uid_line = next(line for line in lines if line.startswith("Uid:"))
        return int(uid_line.split()[1])
    except (OSError, ValueError, IndexError, StopIteration) as exc:
        message = "cannot authenticate process owner"
        raise ProcessError(message) from exc


def process_argv(proc_root: Path, pid: int) -> tuple[str, ...]:
    """Read one strict NUL-delimited procfs argv."""
    try:
        fields = (proc_root / str(pid) / "cmdline").read_bytes().split(b"\0")
        if fields and not fields[-1]:
            fields.pop()
        return tuple(field.decode("utf-8", "strict") for field in fields)
    except (OSError, UnicodeError) as exc:
        message = "cannot authenticate process argv"
        raise ProcessError(message) from exc


def _regular_identity(path: Path) -> os.stat_result:
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        before = os.fstat(descriptor)
        raw = os.read(descriptor, MAX_AUTHORITY_BYTES + 1)
        after = os.fstat(descriptor)
        current = path.lstat()
    except OSError as exc:
        message = "cannot authenticate canonical remote-GDB script"
        raise ProcessError(message) from exc
    finally:
        if "descriptor" in locals():
            os.close(descriptor)
    if (
        len(raw) > MAX_AUTHORITY_BYTES
        or not stat.S_ISREG(before.st_mode)
        or (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino)
        or (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino)
    ):
        message = "canonical remote-GDB script is linked, replaced, or special"
        raise ProcessError(message)
    return before


def _script_open(proc_root: Path, pid: int, identity: os.stat_result) -> bool:
    try:
        entries = tuple((proc_root / str(pid) / "fd").iterdir())
    except OSError as exc:
        message = "cannot authenticate process script descriptor"
        raise ProcessError(message) from exc
    for entry in entries:
        with contextlib.suppress(OSError):
            observed = entry.stat()
            if (observed.st_dev, observed.st_ino) == (identity.st_dev, identity.st_ino):
                return True
    return False


def _parent_paths(pid: int, root: Path, proc_root: Path) -> None:
    if process_uid(proc_root, pid) != os.getuid():
        message = "remote-GDB parent owner is invalid"
        raise ProcessError(message)
    try:
        executable = (proc_root / str(pid) / "exe").resolve(strict=True)
        cwd = (proc_root / str(pid) / "cwd").resolve(strict=True)
    except OSError as exc:
        message = "cannot authenticate parent executable or cwd"
        raise ProcessError(message) from exc
    if executable != Path("/bin/bash").resolve(strict=True) or cwd != root:
        message = "remote-GDB parent executable or workspace is wrong"
        raise ProcessError(message)


def _parent_argv(pid: int, claim: tuple[Path, Path, str, str], proc_root: Path) -> tuple[str, ...]:
    root, script, port, app_arg = claim
    argv = process_argv(proc_root, pid)
    if not RUN_ARGS_MIN <= len(argv) <= RUN_ARGS_MAX + 1 or argv[1] != "-p":
        message = "remote-GDB parent argv is not privileged Bash"
        raise ProcessError(message)
    if argv[SCRIPT_ARG] == "--" and len(argv) == RUN_ARGS_MIN:
        message = "remote-GDB parent argv omits its script"
        raise ProcessError(message)
    script_arg = SCRIPT_ARG + 1 if argv[SCRIPT_ARG] == "--" else SCRIPT_ARG
    invoked = (
        Path(argv[script_arg]) if Path(argv[script_arg]).is_absolute() else root / argv[script_arg]
    )
    if invoked.resolve(strict=True) != script:
        message = "remote-GDB parent argv names another script"
        raise ProcessError(message)
    tail = argv[script_arg + 1 :]
    if (tail and tail[0] != "run") or len(tail) > RUN_TAIL_MAX:
        message = "remote-GDB parent action or argv count is invalid"
        raise ProcessError(message)
    actual_port = tail[1] if len(tail) >= PORT_ARG_COUNT else "2331"
    actual_app = tail[2] if len(tail) == RUN_TAIL_MAX else ""
    if actual_port != port or actual_app != app_arg:
        message = "remote-GDB parent argv does not match requested state"
        raise ProcessError(message)
    if not _script_open(proc_root, pid, _regular_identity(script)):
        message = "remote-GDB parent has no canonical script descriptor"
        raise ProcessError(message)
    return argv


def parent_proof(
    pid: int,
    claim: tuple[Path, Path, str, str],
    proc_root: Path,
) -> ProcessProof:
    """Bind Bash, cwd, argv, open script, and start time after pidfd acquisition."""
    _parent_paths(pid, claim[0], proc_root)
    return ProcessProof(start_ticks(proc_root, pid), _parent_argv(pid, claim, proc_root))
