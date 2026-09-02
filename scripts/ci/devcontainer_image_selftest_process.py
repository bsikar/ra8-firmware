# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticated Linux process primitives for the image selftest supervisor."""

from __future__ import annotations

import ctypes
import fcntl
import os
import signal
import subprocess
import sys
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

import __main__

PROCESS_LOAD_VERSION = 1
PROCESS_GROUP_FIELD = 2
SESSION_FIELD = 3
START_TIME_FIELD = 19
PR_SET_CHILD_SUBREAPER = 36
PR_GET_CHILD_SUBREAPER = 37
CHILD_LIST_MAX_BYTES = 4096
ENTRY_EXEC_DESCRIPTOR_MINIMUM = 64
if globals().get("_RA8_SUPERVISOR_PROCESS_VERSION") != PROCESS_LOAD_VERSION:
    message = "supervisor process module is source-only"
    raise RuntimeError(message)
MAIN_API = vars(__main__)
DEADLINE_SECONDS = MAIN_API["DEADLINE_SECONDS"]
MANAGED_SIGNALS = MAIN_API["MANAGED_SIGNALS"]
POLL_SECONDS = MAIN_API["POLL_SECONDS"]
ControllerLaunch = MAIN_API["ControllerLaunch"]
_anchored_root_descriptor = MAIN_API["_anchored_root_descriptor"]
_entry_digest = MAIN_API["_entry_digest"]
_entry_metadata_is_safe = MAIN_API["_entry_metadata_is_safe"]
_open_entry_authority = MAIN_API["_open_entry_authority"]
SUPERVISOR_PROGRAM = __main__.__file__


@dataclass(frozen=True)
class ProcessIdentity:
    """Bind one live process against numeric PID reuse."""

    pid: int
    group: int
    session: int
    start_time: bytes


def _stat_identity(raw: bytes) -> tuple[int, int, bytes] | None:
    """Extract process authority without decoding the arbitrary comm field."""
    closing = raw.rfind(b")")
    if closing < 0:
        return None
    fields = raw[closing + 2 :].split()
    required = (PROCESS_GROUP_FIELD, SESSION_FIELD, START_TIME_FIELD)
    if len(fields) <= START_TIME_FIELD or any(not fields[index].isdigit() for index in required):
        return None
    return (
        int(fields[PROCESS_GROUP_FIELD]),
        int(fields[SESSION_FIELD]),
        fields[START_TIME_FIELD],
    )


def _stat_group(raw: bytes) -> int | None:
    """Extract one process group without decoding the arbitrary comm field."""
    identity = _stat_identity(raw)
    return None if identity is None else identity[0]


def _stat_parent(raw: bytes) -> int | None:
    """Extract one parent PID without decoding the arbitrary comm field."""
    closing = raw.rfind(b")")
    if closing < 0:
        return None
    fields = raw[closing + 2 :].split()
    return int(fields[1]) if len(fields) > 1 and fields[1].isdigit() else None


def _bind_process(pid: int) -> ProcessIdentity | None:
    """Bind one process identity while its numeric PID cannot be reused."""
    try:
        identity = _stat_identity(Path(f"/proc/{pid}/stat").read_bytes())
    except OSError:
        return None
    if identity is None:
        return None
    return ProcessIdentity(pid, identity[0], identity[1], identity[2])


def _reserve_entry_descriptor(descriptor: int) -> int:
    """Move an entry FD above the Bash helper descriptor namespace."""
    try:
        reserved = fcntl.fcntl(
            descriptor,
            fcntl.F_DUPFD_CLOEXEC,
            ENTRY_EXEC_DESCRIPTOR_MINIMUM,
        )
    finally:
        os.close(descriptor)
    try:
        descriptor_flags = fcntl.fcntl(reserved, fcntl.F_GETFD)
    except BaseException:
        os.close(reserved)
        raise
    if reserved < ENTRY_EXEC_DESCRIPTOR_MINIMUM or not (descriptor_flags & fcntl.FD_CLOEXEC):
        os.close(reserved)
        message = "entry descriptor reservation returned an unsafe descriptor"
        raise RuntimeError(message)
    return reserved


def _group_members(group: int) -> set[int] | None:
    """Return process IDs in one Linux group without spawning another process."""
    members = set()
    try:
        entries = tuple(Path("/proc").iterdir())
    except OSError:
        return None
    for entry in entries:
        if not entry.name.isdigit():
            continue
        try:
            process_group = _stat_group((entry / "stat").read_bytes())
        except FileNotFoundError:
            continue
        except (OSError, RuntimeError):
            return None
        if process_group is None:
            return None
        if process_group == group:
            members.add(int(entry.name))
    return members


def _direct_children() -> dict[int, ProcessIdentity] | None:
    """Bind every kernel-parented direct child, failing closed on unknown state."""
    try:
        value = Path(f"/proc/self/task/{os.getpid()}/children").read_bytes()
    except OSError:
        return None
    if len(value) > CHILD_LIST_MAX_BYTES:
        return None
    children = {}
    for token in value.split():
        if not token.isdigit():
            return None
        pid = int(token)
        try:
            raw = Path(f"/proc/{pid}/stat").read_bytes()
        except OSError:
            return None
        parent = _stat_parent(raw)
        identity = _stat_identity(raw)
        if parent != os.getpid() or identity is None:
            return None
        children[pid] = ProcessIdentity(pid, identity[0], identity[1], identity[2])
    return children


def _child_table_is_empty() -> bool:
    """Require both procfs and waitid to prove that this process has no children."""
    if _direct_children() != {}:
        return False
    try:
        os.waitid(os.P_ALL, 0, os.WEXITED | os.WNOHANG | os.WNOWAIT)
    except ChildProcessError:
        return True
    return False


def _enable_child_subreaper() -> bool:
    """Enable and verify the Linux child-subreaper boundary before any spawn."""
    if sys.platform != "linux":
        return False
    libc = ctypes.CDLL(None, use_errno=True)
    prctl = libc.prctl
    prctl.argtypes = (ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong)
    prctl.restype = ctypes.c_int
    if prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        return False
    state = ctypes.c_int()
    result = prctl(PR_GET_CHILD_SUBREAPER, ctypes.addressof(state), 0, 0, 0)
    return result == 0 and state.value == 1


class BoundGroup:
    """Own one unreaped process-group leader from spawn through cleanup."""

    def __init__(self) -> None:
        """Create an empty authority that cannot signal before a successful fork."""
        self.pid: int | None = None
        self.child: subprocess.Popen[bytes] | None = None
        self.reaped = False
        self.leader_terminal = False
        self.leader_identity: ProcessIdentity | None = None
        self.authority_lost = False
        self.cleaning = False
        self.death_read: int | None = None
        self.death_write: int | None = None
        self.entry_descriptor: int | None = None
        self.entry_path: str | None = None
        self.entry_identity: tuple[int, int] | None = None
        self.entry_digest: str | None = None
        self.entry_integrity = True
        self.subreaper = False
        self.adopted: dict[int, ProcessIdentity] = {}
        self.children_contained = True

    def enable_subreaper(self) -> bool:
        """Establish an empty Linux subreaper boundary before the first spawn."""
        self.subreaper = _enable_child_subreaper()
        return self.subreaper and _child_table_is_empty()

    def spawn(self, source_descriptor: int, launch: ControllerLaunch) -> None:
        """Fork one isolated controller while managed signals remain blocked."""
        if not self.subreaper:
            message = "child subreaper boundary is unavailable or not empty"
            raise RuntimeError(message)
        self.death_read, self.death_write = os.pipe2(os.O_CLOEXEC)
        root_descriptor = _anchored_root_descriptor(launch.status)
        root_metadata = os.fstat(root_descriptor)
        root_identity = f"{root_metadata.st_dev}:{root_metadata.st_ino}"
        entry_authority = "missing-entry-selftest"
        if not launch.missing_entry_selftest:
            descriptor, identity, digest = _open_entry_authority(
                launch.entry, launch.pre_open_mutator
            )
            descriptor = _reserve_entry_descriptor(descriptor)
            self.entry_descriptor = descriptor
            self.entry_path = launch.entry
            self.entry_identity = identity
            self.entry_digest = digest
            entry_authority = str(descriptor)
            if launch.entry_mutator is not None:
                launch.entry_mutator()
        argv = (
            sys.executable,
            "-B",
            "-I",
            "-S",
            SUPERVISOR_PROGRAM,
            "--controller",
            entry_authority,
            str(launch.status),
            str(self.death_read),
            str(root_descriptor),
            root_identity,
            str(launch.watchdog_timeout),
        )
        inherited = [source_descriptor, self.death_read, root_descriptor]
        if self.entry_descriptor is not None:
            inherited.append(self.entry_descriptor)
        child = subprocess.Popen(  # noqa: S603 -- current pinned interpreter and helper
            argv,
            pass_fds=tuple(inherited),
            start_new_session=True,
        )
        self.bind_spawned_child(child)
        os.close(self.death_read)
        self.death_read = None

    def bind_spawned_child(self, child: subprocess.Popen[bytes]) -> None:
        """Bind a manually launched isolated child to the subreaper authority."""
        if not self.subreaper or self.child is not None or self.pid is not None:
            message = "manual child cannot be bound to this subreaper authority"
            raise RuntimeError(message)
        self.child = child
        self.pid = child.pid
        self.children_contained = False
        try:
            terminal = os.waitid(
                os.P_PID,
                child.pid,
                os.WEXITED | os.WNOHANG | os.WNOWAIT,
            )
        except ChildProcessError:
            self.reaped = True
            self.leader_terminal = True
            self.authority_lost = True
            message = "manual child was reaped before identity binding"
            raise RuntimeError(message) from None
        self.leader_terminal = terminal is not None
        identity = _bind_process(child.pid)
        self.leader_identity = identity
        children = _direct_children()
        safe = (
            identity is not None
            and identity.pid == identity.group == identity.session
            and children is not None
            and children.get(child.pid) == identity
        )
        if not safe:
            message = "manual child is not the exact isolated direct child"
            raise RuntimeError(message)

    def _reap(self) -> bool:
        """Reap the exact leader once, after its process group is empty."""
        if self.pid is None or self.child is None or self.reaped:
            return True
        try:
            self.child.wait(timeout=DEADLINE_SECONDS)
        except subprocess.TimeoutExpired:
            return False
        self.reaped = True
        return True

    def _close_death_pipe(self) -> None:
        """Close each still-owned parent-death descriptor at most once."""
        if self.death_read is not None:
            os.close(self.death_read)
            self.death_read = None
        if self.death_write is not None:
            os.close(self.death_write)
            self.death_write = None

    def _close_entry_authority(self) -> bool:
        """Recheck and close the exact entry only after its controller is reaped."""
        if self.entry_descriptor is None:
            return self.entry_integrity
        try:
            metadata = os.fstat(self.entry_descriptor)
            current_identity = (metadata.st_dev, metadata.st_ino)
            current_digest = _entry_digest(self.entry_descriptor)
            path_metadata = os.lstat(self.entry_path) if self.entry_path is not None else None
            self.entry_integrity = (
                _entry_metadata_is_safe(metadata)
                and path_metadata is not None
                and _entry_metadata_is_safe(path_metadata)
                and (path_metadata.st_dev, path_metadata.st_ino) == self.entry_identity
                and current_identity == self.entry_identity
                and current_digest == self.entry_digest
            )
        except (OSError, RuntimeError):
            self.entry_integrity = False
        finally:
            os.close(self.entry_descriptor)
            self.entry_descriptor = None
            self.entry_path = None
        return self.entry_integrity

    def _kill_and_reap_adopted(self, authority: ProcessIdentity) -> bool:
        """Kill and reap one exact direct child without releasing PID authority early."""
        if _bind_process(authority.pid) != authority:
            return False
        with suppress(ProcessLookupError):
            os.kill(authority.pid, signal.SIGKILL)
        result = os.waitid(
            os.P_PID,
            authority.pid,
            os.WEXITED | os.WNOHANG | os.WNOWAIT,
        )
        if result is None:
            return False
        if _bind_process(authority.pid) != authority:
            return False
        waited, _status = os.waitpid(authority.pid, 0)
        return waited == authority.pid

    def _bound_direct_children(self, excluded_pid: int | None) -> dict[int, ProcessIdentity] | None:
        """Bind direct children while retaining one separately owned leader."""
        children = _direct_children()
        if children is None:
            return None
        if excluded_pid is not None:
            leader = children.pop(excluded_pid, None)
            if leader != self.leader_identity:
                return None
        return children

    def _cleanup_adopted_children(self, excluded_pid: int | None = None) -> bool:
        """Drain adopted descendants without reaping a separately bound leader."""
        if not self.subreaper:
            return False
        deadline = time.monotonic() + DEADLINE_SECONDS
        while time.monotonic() < deadline:
            children = self._bound_direct_children(excluded_pid)
            if children is None:
                time.sleep(POLL_SECONDS)
                continue
            for pid, authority in children.items():
                retained = self.adopted.setdefault(pid, authority)
                if retained != authority:
                    return False
            pending = tuple(self.adopted.values())
            for authority in pending:
                if self._kill_and_reap_adopted(authority):
                    self.adopted.pop(authority.pid, None)
            if not self.adopted and not children:
                if excluded_pid is not None:
                    return True
                if _child_table_is_empty():
                    self.children_contained = True
                    return True
            time.sleep(POLL_SECONDS)
        return False

    def _wait_leader_terminal(self) -> bool:
        """Retain exact leader authority through a WNOWAIT terminal proof."""
        if self.pid is None or self.leader_identity is None:
            return False
        deadline = time.monotonic() + DEADLINE_SECONDS
        while time.monotonic() < deadline:
            try:
                result = os.waitid(
                    os.P_PID,
                    self.pid,
                    os.WEXITED | os.WNOHANG | os.WNOWAIT,
                )
            except ChildProcessError:
                self.authority_lost = True
                return False
            except OSError:
                time.sleep(POLL_SECONDS)
                continue
            if result is not None:
                self.leader_terminal = _bind_process(self.pid) == self.leader_identity
                return self.leader_terminal
            time.sleep(POLL_SECONDS)
        return False

    def leader_is_running(self) -> bool:
        """Prove the exact bound controller remains live and non-zombie."""
        if self.pid is None or self.reaped:
            return False
        try:
            raw = Path(f"/proc/{self.pid}/stat").read_bytes()
        except OSError:
            return False
        closing = raw.rfind(b")")
        return closing >= 0 and raw[closing + 2 :].split()[0] != b"Z"

    def require_running(self, context: str) -> None:
        """Fail the supervision transaction when its exact leader is not live."""
        if not self.leader_is_running():
            message = f"controller lost group authority {context}"
            raise RuntimeError(message)

    def _finish_terminal_leader(self) -> bool:
        """Reap a WNOWAIT-bound terminal leader without signaling its numeric group."""
        if not self._reap():
            self.cleaning = False
            return False
        descendants_clean = self._cleanup_adopted_children()
        return descendants_clean and self._close_entry_authority()

    def cleanup(self) -> bool:
        """Kill the bound group before reaping its non-reusable leader."""
        signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)
        if self.authority_lost:
            return False
        if self.pid is None or self.reaped:
            self._close_death_pipe()
            descendants_clean = self._cleanup_adopted_children()
            return descendants_clean and self._close_entry_authority()
        if self.cleaning:
            return False
        self.cleaning = True
        for managed in MANAGED_SIGNALS:
            signal.signal(managed, signal.SIG_IGN)
        leader = self.pid
        self._close_death_pipe()
        if self.leader_terminal:
            return self._finish_terminal_leader()
        with suppress(ProcessLookupError):
            os.killpg(leader, signal.SIGKILL)
        leader_terminal = self._wait_leader_terminal()
        descendants_drained = leader_terminal and self._cleanup_adopted_children(leader)
        members = _group_members(leader) if descendants_drained else None
        if members is not None and members <= {leader} and self._reap():
            descendants_clean = self._cleanup_adopted_children()
            return descendants_clean and self._close_entry_authority()
        self.cleaning = False
        return False

    def contain(self) -> bool:
        """Remain the subreaper until every effected child is safely contained."""
        cleaned = self.cleanup()
        while not cleaned and not self.children_contained:
            time.sleep(POLL_SECONDS)
            cleaned = self.cleanup()
        return cleaned
