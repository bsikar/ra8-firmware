# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate remote-GDB transport fields and broker its local state."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import hmac
import json
import os
import pwd
import re
import runpy
import secrets
import socket
import stat
import struct
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

RECORD_VERSION = 1
PROTOCOL_VERSION = 1
STATE_NAME = "state.json"
SOCKET_NAME = "control.sock"
MAX_RECORD_BYTES = 16 * 1024
MAX_AUTHORITY_BYTES = 256 * 1024
MAX_REQUEST_BYTES = 1024
MAX_APP_ARG_BYTES = 256
ASCII_MIN = 0x21
ASCII_MAX = 0x7E
PORT_MIN = 1024
PORT_MAX = 65535
DIR_MODE = 0o700
FILE_MODE = 0o600
HEX_RE = re.compile(r"[0-9a-f]{64}")
ACCEPT_TIMEOUT = socket.timeout


class GuardError(ValueError):
    """A transport, broker, or process-state claim failed closed."""


@dataclass(frozen=True)
class RecordProof:
    """One descriptor-bound state record and its content identity."""

    value: dict[str, object]
    stat: os.stat_result
    digest: str


@dataclass(frozen=True)
class BrokerRequest:
    """Canonical authorities supplied by the direct parent Bash process."""

    root: Path
    script: Path
    port: str
    app_arg: str
    parent_pid: int
    runtime_base: Path | None = None
    proc_root: Path = Path("/proc")
    platform: str = sys.platform


@dataclass(frozen=True)
class BrokerHooks:
    """Injectable process hooks keep offline tests away from real signals."""

    getppid: Callable[[], int] = os.getppid
    signal_parent: Callable[[int], None] | None = None
    pid_alive: Callable[[int], bool] | None = None


def _load_process_authority() -> dict[str, object]:
    path = Path(__file__).resolve(strict=True).with_name("remote_gdb_process.py")
    try:
        observed = path.lstat()
    except OSError as exc:
        msg = "remote-GDB process authority is unavailable"
        raise RuntimeError(msg) from exc
    if not stat.S_ISREG(observed.st_mode) or path.is_symlink():
        msg = "remote-GDB process authority is linked or special"
        raise RuntimeError(msg)
    return runpy.run_path(str(path))


_PROCESS = _load_process_authority()
ProcessError = _PROCESS["ProcessError"]
ProcessProof = _PROCESS["ProcessProof"]


def _validate_port(value: str) -> int:
    if not value.isascii() or not value.isdecimal():
        msg = "port must be decimal"
        raise GuardError(msg)
    port = int(value, 10)
    if not PORT_MIN <= port <= PORT_MAX:
        msg = "port must be between 1024 and 65535"
        raise GuardError(msg)
    return port


def _canonical_authorities(root_arg: str) -> tuple[Path, Path]:
    root = Path(root_arg)
    try:
        resolved_root = root.resolve(strict=True)
        script = resolved_root / "scripts/dev/remote_gdb_server.sh"
        resolved_script = script.resolve(strict=True)
        root_stat = root.lstat()
        script_stat = script.lstat()
    except OSError as exc:
        msg = "workspace or script authority is unavailable"
        raise GuardError(msg) from exc
    expected = resolved_root / "scripts/dev/remote_gdb_server.sh"
    if root != resolved_root or script != resolved_script or resolved_script != expected:
        msg = "workspace or script authority is not canonical"
        raise GuardError(msg)
    if not stat.S_ISDIR(root_stat.st_mode) or not stat.S_ISREG(script_stat.st_mode):
        msg = "workspace or script authority has the wrong type"
        raise GuardError(msg)
    return resolved_root, resolved_script


def _regular_identity(path: Path) -> tuple[os.stat_result, str]:
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        before = os.fstat(descriptor)
        raw = os.read(descriptor, MAX_AUTHORITY_BYTES + 1)
        after = os.fstat(descriptor)
        current = path.lstat()
    except OSError as exc:
        msg = f"cannot authenticate regular authority {path}"
        raise GuardError(msg) from exc
    finally:
        if "descriptor" in locals():
            os.close(descriptor)
    if (
        len(raw) > MAX_AUTHORITY_BYTES
        or not stat.S_ISREG(before.st_mode)
        or (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino)
        or (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino)
    ):
        msg = f"regular authority {path} is linked, replaced, special, or oversized"
        raise GuardError(msg)
    return before, hashlib.sha256(raw).hexdigest()


def _helper_digest() -> str:
    helper = Path(__file__).resolve(strict=True)
    _guard_stat, guard_digest = _regular_identity(helper)
    _process_stat, process_digest = _regular_identity(helper.with_name("remote_gdb_process.py"))
    return hashlib.sha256(f"{guard_digest}:{process_digest}".encode("ascii")).hexdigest()


def _open_private_directory(path: Path, uid: int) -> int:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
        observed = os.fstat(descriptor)
        current = path.lstat()
    except OSError as exc:
        msg = f"unsafe runtime directory {path}"
        raise GuardError(msg) from exc
    if (
        not stat.S_ISDIR(observed.st_mode)
        or observed.st_uid != uid
        or stat.S_IMODE(observed.st_mode) != DIR_MODE
        or (observed.st_dev, observed.st_ino) != (current.st_dev, current.st_ino)
    ):
        os.close(descriptor)
        msg = f"runtime directory {path} is not owned mode 0700"
        raise GuardError(msg)
    return descriptor


def _secure_child(parent: Path, name: str, uid: int) -> Path:
    parent_fd = _open_private_directory(parent, uid)
    try:
        try:
            os.mkdir(name, DIR_MODE, dir_fd=parent_fd)
            os.fsync(parent_fd)
        except FileExistsError:
            pass
    finally:
        os.close(parent_fd)
    child = parent / name
    descriptor = _open_private_directory(child, uid)
    os.close(descriptor)
    return child


def _home_runtime(uid: int, home: Path | None) -> Path:
    account_home = Path(pwd.getpwuid(uid).pw_dir) if home is None else home
    try:
        resolved = account_home.resolve(strict=True)
        observed = account_home.lstat()
    except OSError as exc:
        msg = "account home is unavailable"
        raise GuardError(msg) from exc
    if (
        account_home != resolved
        or not stat.S_ISDIR(observed.st_mode)
        or observed.st_uid != uid
        or stat.S_IMODE(observed.st_mode) & (stat.S_IWGRP | stat.S_IWOTH)
    ):
        msg = "account home is linked, foreign, special, or writable by others"
        raise GuardError(msg)
    name = ".ra8-runtime"
    try:
        (account_home / name).mkdir(mode=DIR_MODE)
        home_fd = os.open(account_home, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(home_fd)
        finally:
            os.close(home_fd)
    except FileExistsError:
        pass
    return _secure_child(account_home / name, "remote-gdb", uid)


def _runtime_directory(
    root: Path,
    *,
    uid: int | None = None,
    platform: str = sys.platform,
    base: Path | None = None,
) -> Path:
    owner = os.getuid() if uid is None else uid
    linux_base = Path("/run/user") / str(owner)
    if base is not None:
        descriptor = _open_private_directory(base, owner)
        os.close(descriptor)
        selected = _secure_child(base, "ra8-remote-gdb", owner)
    elif platform.startswith("linux") and linux_base.exists():
        descriptor = _open_private_directory(linux_base, owner)
        os.close(descriptor)
        selected = _secure_child(linux_base, "ra8-remote-gdb", owner)
    else:
        selected = _home_runtime(owner, None)
    key = hashlib.sha256(os.fsencode(root)).hexdigest()[:24]
    return _secure_child(selected, key, owner)


def _process_value(name: str, *arguments: object) -> object:
    try:
        return _PROCESS[name](*arguments)
    except ProcessError as exc:
        raise GuardError(str(exc)) from exc


def _proc_start_ticks(proc_root: Path, pid: int) -> int:
    return int(_process_value("start_ticks", proc_root, pid))


def _proc_uid(proc_root: Path, pid: int) -> int:
    return int(_process_value("process_uid", proc_root, pid))


def _proc_argv(proc_root: Path, pid: int) -> tuple[str, ...]:
    return tuple(_process_value("process_argv", proc_root, pid))


def _process_proof(pid: int, request: BrokerRequest) -> ProcessProof:
    return _process_value(
        "parent_proof",
        pid,
        (request.root, request.script, request.port, request.app_arg),
        request.proc_root,
    )


def _record_keys() -> set[str]:
    return {
        "version",
        "uid",
        "platform",
        "broker_pid",
        "broker_start_ticks",
        "parent_pid",
        "parent_start_ticks",
        "parent_argv",
        "root",
        "root_dev",
        "root_ino",
        "script",
        "script_dev",
        "script_ino",
        "script_sha256",
        "helper_sha256",
        "port",
        "app_arg",
        "socket_dev",
        "socket_ino",
        "nonce",
    }


def _no_duplicate_json(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            msg = f"duplicate state field {key}"
            raise GuardError(msg)
        result[key] = value
    return result


def _validate_record(value: dict[str, object]) -> None:
    if (
        set(value) != _record_keys()
        or type(value["version"]) is not int
        or value["version"] != RECORD_VERSION
    ):
        msg = "state schema or version is invalid"
        raise GuardError(msg)
    integers = (
        "uid",
        "broker_pid",
        "broker_start_ticks",
        "parent_pid",
        "parent_start_ticks",
        "root_dev",
        "root_ino",
        "script_dev",
        "script_ino",
        "socket_dev",
        "socket_ino",
    )
    if any(type(value[field]) is not int or int(value[field]) < 0 for field in integers):
        msg = "state integer field is invalid"
        raise GuardError(msg)
    strings = (
        "platform",
        "root",
        "script",
        "script_sha256",
        "helper_sha256",
        "port",
        "app_arg",
        "nonce",
    )
    if any(type(value[field]) is not str for field in strings):
        msg = "state string field is invalid"
        raise GuardError(msg)
    argv = value["parent_argv"]
    if not isinstance(argv, list) or any(type(field) is not str for field in argv):
        msg = "state parent argv is invalid"
        raise GuardError(msg)
    if any(
        HEX_RE.fullmatch(str(value[field])) is None
        for field in ("script_sha256", "helper_sha256", "nonce")
    ):
        msg = "state digest or nonce is invalid"
        raise GuardError(msg)
    _validate_port(str(value["port"]))


def _publish_record(state_dir: Path, value: dict[str, object]) -> RecordProof:
    payload = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    directory = _open_private_directory(state_dir, os.getuid())
    temporary = f".{STATE_NAME}.tmp.{os.getpid()}.{secrets.token_hex(8)}"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    descriptor = -1
    try:
        descriptor = os.open(temporary, flags, FILE_MODE, dir_fd=directory)
        os.fchmod(descriptor, FILE_MODE)
        remaining = memoryview(payload)
        while remaining:
            remaining = remaining[os.write(descriptor, remaining) :]
        os.fsync(descriptor)
        os.link(
            temporary, STATE_NAME, src_dir_fd=directory, dst_dir_fd=directory, follow_symlinks=False
        )
        os.unlink(temporary, dir_fd=directory)
        os.fsync(directory)
    except FileExistsError as exc:
        msg = "remote-GDB state already exists"
        raise GuardError(msg) from exc
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary, dir_fd=directory)
        os.close(directory)
    return _read_record(state_dir)


def _read_record(state_dir: Path) -> RecordProof:
    directory = _open_private_directory(state_dir, os.getuid())
    flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(STATE_NAME, flags, dir_fd=directory)
        before = os.fstat(descriptor)
        raw = os.read(descriptor, MAX_RECORD_BYTES + 1)
        after = os.fstat(descriptor)
        current = os.stat(STATE_NAME, dir_fd=directory, follow_symlinks=False)
    except FileNotFoundError as exc:
        msg = "remote-GDB state is absent"
        raise GuardError(msg) from exc
    except OSError as exc:
        msg = "remote-GDB state cannot be opened safely"
        raise GuardError(msg) from exc
    finally:
        if "descriptor" in locals():
            os.close(descriptor)
        os.close(directory)
    if (
        len(raw) > MAX_RECORD_BYTES
        or not stat.S_ISREG(before.st_mode)
        or before.st_uid != os.getuid()
        or stat.S_IMODE(before.st_mode) != FILE_MODE
        or (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino)
        or (before.st_dev, before.st_ino) != (current.st_dev, current.st_ino)
    ):
        msg = "remote-GDB state is linked, replaced, foreign, special, or oversized"
        raise GuardError(msg)
    try:
        value = json.loads(raw.decode("ascii", "strict"), object_pairs_hook=_no_duplicate_json)
    except (UnicodeError, json.JSONDecodeError) as exc:
        msg = "remote-GDB state JSON is malformed"
        raise GuardError(msg) from exc
    if not isinstance(value, dict):
        msg = "remote-GDB state is not an object"
        raise GuardError(msg)
    _validate_record(value)
    return RecordProof(value, before, hashlib.sha256(raw).hexdigest())


def _unlink_exact(state_dir: Path, expected: RecordProof) -> bool:
    try:
        current = _read_record(state_dir)
    except GuardError:
        return False
    if (current.stat.st_dev, current.stat.st_ino) != (
        expected.stat.st_dev,
        expected.stat.st_ino,
    ) or current.digest != expected.digest:
        return False
    directory = _open_private_directory(state_dir, os.getuid())
    try:
        os.unlink(STATE_NAME, dir_fd=directory)
        os.fsync(directory)
    finally:
        os.close(directory)
    return True


def _peer_uid(connection: socket.socket) -> int:
    if hasattr(connection, "getpeereid"):
        uid, _gid = connection.getpeereid()
        return int(uid)
    if hasattr(socket, "SO_PEERCRED"):
        _pid, uid, _gid = struct.unpack(
            "3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        )
        return int(uid)
    if hasattr(socket, "LOCAL_PEERCRED"):
        version, uid = struct.unpack(
            "II",
            connection.getsockopt(getattr(socket, "SOL_LOCAL", 0), socket.LOCAL_PEERCRED, 8),
        )
        if version != 0:
            msg = "Unix peer credential version is invalid"
            raise GuardError(msg)
        return int(uid)
    msg = "Unix peer credentials are unavailable"
    raise GuardError(msg)


def _socket_identity(path: Path) -> os.stat_result:
    try:
        observed = path.lstat()
    except OSError as exc:
        msg = "remote-GDB control socket is unavailable"
        raise GuardError(msg) from exc
    if (
        not stat.S_ISSOCK(observed.st_mode)
        or observed.st_uid != os.getuid()
        or stat.S_IMODE(observed.st_mode) != FILE_MODE
    ):
        msg = "remote-GDB control path is not the owned socket"
        raise GuardError(msg)
    return observed


def _request_bytes(action: str, nonce: str) -> bytes:
    if action not in {"status", "stop", "release"} or HEX_RE.fullmatch(nonce) is None:
        msg = "control request fields are invalid"
        raise GuardError(msg)
    value = {"action": action, "nonce": nonce, "version": PROTOCOL_VERSION}
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def _parse_request(raw: bytes, nonce: str) -> str:
    if not raw.endswith(b"\n") or len(raw) > MAX_REQUEST_BYTES:
        msg = "control request is incomplete or oversized"
        raise GuardError(msg)
    try:
        value = json.loads(raw.decode("ascii", "strict"), object_pairs_hook=_no_duplicate_json)
    except (UnicodeError, json.JSONDecodeError) as exc:
        msg = "control request JSON is malformed"
        raise GuardError(msg) from exc
    if not isinstance(value, dict) or set(value) != {"action", "nonce", "version"}:
        msg = "control request schema is invalid"
        raise GuardError(msg)
    if value["version"] != PROTOCOL_VERSION or type(value["action"]) is not str:
        msg = "control request version or action type is invalid"
        raise GuardError(msg)
    supplied = value["nonce"]
    if type(supplied) is not str or not hmac.compare_digest(supplied, nonce):
        msg = "control request nonce is invalid"
        raise GuardError(msg)
    if value["action"] not in {"status", "stop", "release"}:
        msg = "control request action is invalid"
        raise GuardError(msg)
    return str(value["action"])


def _static_record(request: BrokerRequest, record: RecordProof) -> None:
    value = record.value
    root_stat = request.root.lstat()
    script_stat, script_digest = _regular_identity(request.script)
    helper_digest = _helper_digest()
    expected = {
        "uid": os.getuid(),
        "platform": request.platform,
        "root": str(request.root),
        "root_dev": root_stat.st_dev,
        "root_ino": root_stat.st_ino,
        "script": str(request.script),
        "script_dev": script_stat.st_dev,
        "script_ino": script_stat.st_ino,
        "script_sha256": script_digest,
        "helper_sha256": helper_digest,
        "port": request.port,
    }
    if any(value[key] != wanted for key, wanted in expected.items()):
        msg = "remote-GDB state does not match current authorities"
        raise GuardError(msg)


def _broker_live(request: BrokerRequest, record: RecordProof) -> bool:
    pid = int(record.value["broker_pid"])
    if request.platform.startswith("linux"):
        try:
            return _proc_uid(request.proc_root, pid) == os.getuid() and _proc_start_ticks(
                request.proc_root, pid
            ) == int(record.value["broker_start_ticks"])
        except GuardError:
            return False
    return _pid_alive(pid)


def _live_broker_record(request: BrokerRequest, record: RecordProof) -> None:
    _static_record(request, record)
    if not _broker_live(request, record):
        msg = "remote-GDB broker identity is stale"
        raise GuardError(msg)
    if not request.platform.startswith("linux"):
        return
    pid = int(record.value["broker_pid"])
    try:
        executable = (request.proc_root / str(pid) / "exe").resolve(strict=True)
    except OSError as exc:
        msg = "remote-GDB broker executable is unavailable"
        raise GuardError(msg) from exc
    helper = str(Path(__file__).resolve(strict=True))
    expected = (
        "/usr/bin/python3",
        "-I",
        helper,
        "broker",
        "--root",
        str(request.root),
        "--port",
        request.port,
        "--parent-pid",
        str(record.value["parent_pid"]),
        "--app-arg",
        str(record.value["app_arg"]),
    )
    if (
        executable != Path("/usr/bin/python3").resolve(strict=True)
        or _proc_argv(request.proc_root, pid) != expected
    ):
        msg = "remote-GDB broker executable or argv is invalid"
        raise GuardError(msg)


def _request_broker(
    state_dir: Path,
    action: str,
    *,
    request: BrokerRequest | None = None,
    timeout: float = 2.0,
) -> RecordProof:
    record = _read_record(state_dir)
    if request is not None:
        _live_broker_record(request, record)
    socket_path = state_dir / SOCKET_NAME
    socket_stat = _socket_identity(socket_path)
    if (socket_stat.st_dev, socket_stat.st_ino) != (
        int(record.value["socket_dev"]),
        int(record.value["socket_ino"]),
    ):
        msg = "control socket identity does not match state"
        raise GuardError(msg)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    try:
        client.connect(str(socket_path))
        client.sendall(_request_bytes(action, str(record.value["nonce"])))
        response = client.recv(16)
    except OSError as exc:
        msg = "control broker is unavailable"
        raise GuardError(msg) from exc
    finally:
        client.close()
    if response != b"OK\n":
        msg = "control broker returned an invalid response"
        raise GuardError(msg)
    return record


def _pid_alive(pid: int) -> bool:
    return bool(_process_value("pid_alive", pid))


def _pidfd_live(descriptor: int) -> bool:
    return bool(_process_value("pidfd_live", descriptor))


def _signal_authority(request: BrokerRequest) -> tuple[Callable[[int], None], int | None]:
    raw_signal, descriptor = _process_value(
        "signal_authority", request.parent_pid, request.platform
    )

    def checked_signal(pid: int) -> None:
        try:
            raw_signal(pid)
        except ProcessError as exc:
            raise GuardError(str(exc)) from exc

    return checked_signal, descriptor


def _parent_authority(
    request: BrokerRequest, hooks: BrokerHooks
) -> tuple[Callable[[int], None], int | None, ProcessProof]:
    signal_parent, pidfd = (
        _signal_authority(request) if hooks.signal_parent is None else (hooks.signal_parent, None)
    )
    try:
        parent = (
            _process_proof(request.parent_pid, request)
            if request.platform.startswith("linux")
            else ProcessProof(0, ())
        )
    except (GuardError, OSError):
        if pidfd is not None:
            os.close(pidfd)
        raise
    if request.platform.startswith("linux") and (
        pidfd is None or not _pidfd_live(pidfd) or hooks.getppid() != request.parent_pid
    ):
        if pidfd is not None:
            os.close(pidfd)
        msg = "remote-GDB parent changed during pidfd authentication"
        raise GuardError(msg)
    return signal_parent, pidfd, parent


def _broker_record(
    request: BrokerRequest,
    socket_stat: os.stat_result,
    parent: ProcessProof,
) -> dict[str, object]:
    root_stat = request.root.stat()
    script_stat, script_digest = _regular_identity(request.script)
    helper_digest = _helper_digest()
    broker_ticks = (
        _proc_start_ticks(request.proc_root, os.getpid())
        if request.platform.startswith("linux")
        else 0
    )
    return {
        "version": RECORD_VERSION,
        "uid": os.getuid(),
        "platform": request.platform,
        "broker_pid": os.getpid(),
        "broker_start_ticks": broker_ticks,
        "parent_pid": request.parent_pid,
        "parent_start_ticks": parent.start_ticks,
        "parent_argv": list(parent.argv),
        "root": str(request.root),
        "root_dev": root_stat.st_dev,
        "root_ino": root_stat.st_ino,
        "script": str(request.script),
        "script_dev": script_stat.st_dev,
        "script_ino": script_stat.st_ino,
        "script_sha256": script_digest,
        "helper_sha256": helper_digest,
        "port": request.port,
        "app_arg": request.app_arg,
        "socket_dev": socket_stat.st_dev,
        "socket_ino": socket_stat.st_ino,
        "nonce": secrets.token_hex(32),
    }


def _cleanup_socket(state_dir: Path, socket_identity: os.stat_result) -> None:
    directory = _open_private_directory(state_dir, os.getuid())
    try:
        with contextlib.suppress(FileNotFoundError):
            current = os.stat(SOCKET_NAME, dir_fd=directory, follow_symlinks=False)
            if (current.st_dev, current.st_ino) == (
                socket_identity.st_dev,
                socket_identity.st_ino,
            ):
                os.unlink(SOCKET_NAME, dir_fd=directory)
                os.fsync(directory)
    finally:
        os.close(directory)


def _recv_request(connection: socket.socket) -> bytes:
    raw = bytearray()
    while b"\n" not in raw and len(raw) <= MAX_REQUEST_BYTES:
        chunk = connection.recv(min(256, MAX_REQUEST_BYTES + 1 - len(raw)))
        if not chunk:
            break
        raw.extend(chunk)
    if raw.count(b"\n") != 1 or not raw.endswith(b"\n"):
        msg = "control request has trailing, incomplete, or excessive data"
        raise GuardError(msg)
    return bytes(raw)


def _serve_connection(
    connection: socket.socket,
    nonce: str,
    request: BrokerRequest,
    signal_parent: Callable[[int], None],
    getppid: Callable[[], int],
) -> bool:
    connection.settimeout(2.0)
    if _peer_uid(connection) != os.getuid():
        msg = "control peer has the wrong UID"
        raise GuardError(msg)
    raw = _recv_request(connection)
    action = _parse_request(raw, nonce)
    if action == "status":
        connection.sendall(b"OK\n")
        return True
    if getppid() != request.parent_pid:
        msg = "broker parent died before control action"
        raise GuardError(msg)
    if action == "stop":
        signal_parent(request.parent_pid)
    connection.sendall(b"OK\n")
    return False


def _state_dir(request: BrokerRequest) -> Path:
    return _runtime_directory(
        request.root,
        platform=request.platform,
        base=request.runtime_base,
    )


def _prepare_state(request: BrokerRequest, hooks: BrokerHooks) -> Path:
    state_dir = _state_dir(request)
    socket_path = state_dir / SOCKET_NAME
    try:
        record = _read_record(state_dir)
    except GuardError as exc:
        if str(exc) != "remote-GDB state is absent":
            raise
        if socket_path.exists() or socket_path.is_symlink():
            msg = "control socket exists without authenticated state"
            raise GuardError(msg) from exc
        return state_dir
    _static_record(request, record)
    alive = (
        hooks.pid_alive(int(record.value["broker_pid"]))
        if hooks.pid_alive is not None and not request.platform.startswith("linux")
        else _broker_live(request, record)
    )
    if alive:
        msg = "an authenticated remote-GDB broker is already live"
        raise GuardError(msg)
    if socket_path.exists() or socket_path.is_symlink():
        socket_stat = _socket_identity(socket_path)
        if (socket_stat.st_dev, socket_stat.st_ino) != (
            int(record.value["socket_dev"]),
            int(record.value["socket_ino"]),
        ):
            msg = "stale control socket identity is ambiguous"
            raise GuardError(msg)
        _cleanup_socket(state_dir, socket_stat)
    if not _unlink_exact(state_dir, record):
        msg = "stale remote-GDB state changed during cleanup"
        raise GuardError(msg)
    return state_dir


def _run_broker(request: BrokerRequest, hooks: BrokerHooks | None = None) -> None:
    hooks = BrokerHooks() if hooks is None else hooks
    if hooks.getppid() != request.parent_pid:
        msg = "broker caller is not its direct parent"
        raise GuardError(msg)
    state_dir = _prepare_state(request, hooks)
    socket_path = state_dir / SOCKET_NAME
    record: RecordProof | None = None
    socket_stat: os.stat_result | None = None
    signal_parent, pidfd, parent = _parent_authority(request, hooks)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        server.bind(str(socket_path))
        socket_path.chmod(FILE_MODE)
        socket_stat = _socket_identity(socket_path)
        server.listen(1)
        server.settimeout(0.25)
        record = _publish_record(state_dir, _broker_record(request, socket_stat, parent))
        keep_running = True
        while keep_running and hooks.getppid() == request.parent_pid:
            try:
                connection, _address = server.accept()
            except ACCEPT_TIMEOUT:
                continue
            with connection:
                try:
                    keep_running = _serve_connection(
                        connection,
                        str(record.value["nonce"]),
                        request,
                        signal_parent,
                        hooks.getppid,
                    )
                except (GuardError, OSError):
                    with contextlib.suppress(OSError):
                        connection.sendall(b"ERR\n")
    finally:
        server.close()
        if record is not None:
            _unlink_exact(state_dir, record)
        if socket_stat is not None:
            _cleanup_socket(state_dir, socket_stat)
        if pidfd is not None:
            os.close(pidfd)


def _status_broker(state_dir: Path, request: BrokerRequest) -> RecordProof | None:
    try:
        return _request_broker(state_dir, "status", request=request, timeout=0.25)
    except GuardError:
        return None


def _await_broker(request: BrokerRequest, broker_pid: int, timeout: float = 5.0) -> None:
    state_dir = _state_dir(request)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        record = _status_broker(state_dir, request)
        if record is not None and int(record.value["broker_pid"]) == broker_pid:
            return
        time.sleep(0.05)
    msg = "remote-GDB broker did not become ready"
    raise GuardError(msg)


def _control_broker(request: BrokerRequest, action: str) -> None:
    state_dir = _state_dir(request)
    try:
        _request_broker(state_dir, action, request=request)
    except GuardError as exc:
        if str(exc) == "remote-GDB state is absent":
            return
        raise


def _request_from_args(args: argparse.Namespace) -> BrokerRequest:
    root, script = _canonical_authorities(args.root)
    _validate_port(args.port)
    app_arg = getattr(args, "app_arg", "")
    try:
        raw = app_arg.encode("ascii", "strict")
    except UnicodeEncodeError as exc:
        msg = "application argument must be ASCII"
        raise GuardError(msg) from exc
    if raw and (
        len(raw) > MAX_APP_ARG_BYTES or any(byte < ASCII_MIN or byte > ASCII_MAX for byte in raw)
    ):
        msg = "application argument is unsafe or oversized"
        raise GuardError(msg)
    parent_pid = getattr(args, "parent_pid", 0)
    if parent_pid < 0 or (args.command in {"broker", "await"} and parent_pid <= 1):
        msg = "parent PID is invalid"
        raise GuardError(msg)
    return BrokerRequest(root, script, args.port, app_arg, parent_pid)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("broker", "await", "stop", "release"):
        command = commands.add_parser(name)
        command.add_argument("--root", required=True)
        command.add_argument("--port", required=True)
        if name in {"broker", "await"}:
            command.add_argument("--parent-pid", required=True, type=int)
        if name == "broker":
            command.add_argument("--app-arg", default="")
        if name == "await":
            command.add_argument("--broker-pid", required=True, type=int)
    return parser


def main() -> int:
    """Dispatch one broker operation."""
    args = _parser().parse_args()
    try:
        request = _request_from_args(args)
        if args.command == "broker":
            _run_broker(request)
        elif args.command == "await":
            _await_broker(request, args.broker_pid)
        else:
            _control_broker(request, args.command)
    except GuardError as exc:
        print(f"remote_gdb_guard: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
