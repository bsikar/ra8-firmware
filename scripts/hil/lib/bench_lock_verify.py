# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticate the canonical bench holder against Linux kernel state."""

from __future__ import annotations

import argparse
import base64
import hashlib
import itertools
import json
import os
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, replace
from pathlib import Path
from unittest import mock

CANONICAL_ROOT = Path("/var/lib/ra8-bench")
LOCK_ID_RE = re.compile(r"[0-9a-f]{16}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
MAX_RECORD_BYTES = 8192
MAX_SCRIPT_BYTES = 128 * 1024
BROKER_ARGC = 11
SECOND_LOCK_OBSERVATION = 2


class LockProofError(ValueError):
    """The advertised holder did not match the live canonical flock."""


class _ProofDeadlineError(RuntimeError):
    """A selftest descriptor proof exceeded its hard liveness deadline."""


@dataclass(frozen=True)
class ProofRequest:
    """Expected identities and local authorities for one live proof."""

    lock_id: str
    kind: str
    expected_script_sha256: str
    expected_broker_sha256: str
    root: Path = CANONICAL_ROOT
    proc_root: Path = Path("/proc")


@dataclass(frozen=True)
class _ProofState:
    """Descriptor-bound identities retained across the two proof phases."""

    record: dict[str, object]
    record_identity: os.stat_result
    lock_identity: os.stat_result
    holder_pid: int


@dataclass(frozen=True)
class _Fixture:
    """Paths and identities for one synthetic proc/lock authority."""

    root: Path
    proc: Path
    record: Path
    lock: Path
    script: Path
    broker: Path
    pid: int
    ticks: int
    host_pid: int
    host_ticks: int
    lock_id: str

    @property
    def digest(self) -> str:
        """Return the reviewed holder fixture digest."""
        return hashlib.sha256(self.script.read_bytes()).hexdigest()

    @property
    def broker_digest(self) -> str:
        """Return the reviewed broker fixture digest."""
        return hashlib.sha256(self.broker.read_bytes()).hexdigest()

    @property
    def request(self) -> ProofRequest:
        """Return the valid proof request bound to this fixture."""
        return ProofRequest(
            self.lock_id,
            "wrapped",
            self.digest,
            self.broker_digest,
            self.root,
            self.proc,
        )


def _read_regular(
    path: Path,
    limit: int,
    post_read: Callable[[], None] | None = None,
) -> tuple[bytes, os.stat_result]:
    """Read one non-linked regular file through one stable descriptor."""
    flags = os.O_RDONLY | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        msg = f"cannot open {path}: {exc}"
        raise LockProofError(msg) from exc
    try:
        before = os.fstat(descriptor)
        if not stat.S_ISREG(before.st_mode):
            msg = f"{path} is not a regular file"
            raise LockProofError(msg)
        raw = os.read(descriptor, limit + 1)
        if post_read is not None:
            post_read()
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if len(raw) > limit or (before.st_dev, before.st_ino) != (
        after.st_dev,
        after.st_ino,
    ):
        msg = f"{path} changed or exceeded its size bound"
        raise LockProofError(msg)
    try:
        current = path.stat(follow_symlinks=False)
    except OSError as exc:
        msg = f"cannot restat {path}: {exc}"
        raise LockProofError(msg) from exc
    if (current.st_dev, current.st_ino) != (before.st_dev, before.st_ino):
        msg = f"{path} was replaced during authentication"
        raise LockProofError(msg)
    return raw, before


def _record(path: Path, lock_id: str, kind: str) -> tuple[dict[str, object], os.stat_result]:
    """Load and validate the holder record's capability-binding fields."""
    raw, identity = _read_regular(path, MAX_RECORD_BYTES)
    try:
        value = json.loads(raw.decode("utf-8", "strict"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        msg = "holder record is malformed"
        raise LockProofError(msg) from exc
    if not isinstance(value, dict):
        msg = "holder record is not a mapping"
        raise LockProofError(msg)
    required = {
        "resource",
        "lock_id",
        "hold_kind",
        "pid",
        "pid_start_ticks",
        "host_pid",
        "host_start_ticks",
        "boot_id",
    }
    if not required <= value.keys():
        msg = "holder record omits live-capability fields"
        raise LockProofError(msg)
    if value["resource"] != "bench" or value["lock_id"] != lock_id:
        msg = "holder record does not name this bench transaction"
        raise LockProofError(msg)
    if value["hold_kind"] != kind:
        msg = "holder record has the wrong hold kind"
        raise LockProofError(msg)
    if type(value["pid"]) is not int or int(value["pid"]) <= 1:
        msg = "holder record PID is invalid"
        raise LockProofError(msg)
    for field in ("pid_start_ticks", "host_pid", "host_start_ticks"):
        if type(value[field]) is not int or int(value[field]) <= 0:
            msg = f"holder record {field} is invalid"
            raise LockProofError(msg)
    return value, identity


def _start_ticks(proc_root: Path, pid: int) -> int:
    """Read one process start time without confusing spaces in comm."""
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
        tail = raw[raw.rindex(")") + 2 :].split()
        return int(tail[19])
    except (OSError, ValueError, IndexError) as exc:
        msg = "cannot authenticate holder PID start time"
        raise LockProofError(msg) from exc


def _lock_owner_pids(proc_root: Path, lock_stat: os.stat_result) -> set[int]:
    """Return active kernel FLOCK owners for one exact device and inode."""
    try:
        lines = (proc_root / "locks").read_text(encoding="ascii").splitlines()
    except OSError as exc:
        msg = "cannot read kernel lock authority"
        raise LockProofError(msg) from exc
    owners: set[int] = set()
    for line in lines:
        fields = line.split()
        if "FLOCK" not in fields or "->" in fields:
            continue
        for index, field in enumerate(fields):
            match = re.fullmatch(r"([0-9a-fA-F]+):([0-9a-fA-F]+):(\d+)", field)
            if match is None or index == 0 or not fields[index - 1].isdigit():
                continue
            major = int(match.group(1), 16)
            minor = int(match.group(2), 16)
            inode = int(match.group(3))
            if (major, minor, inode) == (
                os.major(lock_stat.st_dev),
                os.minor(lock_stat.st_dev),
                lock_stat.st_ino,
            ):
                owners.add(int(fields[index - 1]))
    return owners


def _holder_fds(proc_root: Path, pid: int) -> list[Path]:
    """Return a stable snapshot of the holder's visible file descriptors."""
    try:
        return sorted((proc_root / str(pid) / "fd").iterdir(), key=lambda path: path.name)
    except OSError as exc:
        msg = "cannot enumerate holder file descriptors"
        raise LockProofError(msg) from exc


def _has_locked_inode(fds: list[Path], lock_stat: os.stat_result) -> bool:
    """Return whether one holder descriptor references the canonical inode."""
    for path in fds:
        try:
            observed = path.stat()
        except OSError:
            continue
        if (observed.st_dev, observed.st_ino) == (lock_stat.st_dev, lock_stat.st_ino):
            return True
    return False


def _has_script_digest(fds: list[Path], expected: str) -> bool:
    """Bind the holder to an open descriptor containing reviewed host bytes."""
    if not hasattr(os, "O_PATH"):
        return False
    for path in fds:
        try:
            anchor = os.open(path, os.O_PATH | os.O_CLOEXEC)
            try:
                before = os.fstat(anchor)
                if not stat.S_ISREG(before.st_mode):
                    continue
                descriptor = os.open(
                    Path("/proc/self/fd") / str(anchor),
                    os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK,
                )
                try:
                    descriptor_before = os.fstat(descriptor)
                    raw = os.read(descriptor, MAX_SCRIPT_BYTES + 1)
                    descriptor_after = os.fstat(descriptor)
                finally:
                    os.close(descriptor)
                anchored_after = os.fstat(anchor)
            finally:
                os.close(anchor)
        except OSError:
            continue
        identities = {
            (value.st_dev, value.st_ino)
            for value in (before, descriptor_before, descriptor_after, anchored_after)
        }
        stable = len(identities) == 1 and all(
            stat.S_ISREG(value.st_mode)
            for value in (before, descriptor_before, descriptor_after, anchored_after)
        )
        if stable and len(raw) <= MAX_SCRIPT_BYTES and hashlib.sha256(raw).hexdigest() == expected:
            return True
    return False


def _process_argv(proc_root: Path, pid: int) -> tuple[Path, list[str]]:
    """Return one strict executable/argv pair from proc."""
    try:
        executable = (proc_root / str(pid) / "exe").resolve(strict=True)
        argv = (proc_root / str(pid) / "cmdline").read_bytes().split(b"\0")
    except OSError as exc:
        msg = "cannot authenticate holder executable"
        raise LockProofError(msg) from exc
    return executable, [item.decode("utf-8", "strict") for item in argv if item]


def _broker_shape(
    proc_root: Path,
    root: Path,
    record: dict[str, object],
    expected_broker_sha256: str,
) -> None:
    """Bind the kernel owner to exact broker code and the reviewed Bash parent."""
    pid = int(record["pid"])
    host_pid = int(record["host_pid"])
    executable, argv = _process_argv(proc_root, pid)
    expected_python = Path("/usr/bin/python3").resolve(strict=True)
    if executable != expected_python or len(argv) != BROKER_ARGC or argv[1:4] != ["-I", "-S", "-c"]:
        msg = "kernel owner is not the fixed isolated broker"
        raise LockProofError(msg)
    if hashlib.sha256(argv[4].encode()).hexdigest() != expected_broker_sha256:
        msg = "kernel owner is not running the reviewed broker bytes"
        raise LockProofError(msg)
    if argv[5:7] != [str(root / "board.lock"), str(root / "holder.json")]:
        msg = "broker command is not bound to the canonical lock and record"
        raise LockProofError(msg)
    if argv[-2:] != [str(host_pid), str(record["host_start_ticks"])]:
        msg = "broker command is not bound to its recorded Bash parent"
        raise LockProofError(msg)
    parent, _ticks = _stat_fields(proc_root, pid)
    if parent != host_pid:
        msg = "broker is not the direct child of the reviewed Bash host"
        raise LockProofError(msg)


def _host_shape(proc_root: Path, record: dict[str, object], kind: str) -> None:
    """Require the broker parent to be reviewed Bash host bytes without lock fd."""
    host_pid = int(record["host_pid"])
    executable, argv = _process_argv(proc_root, host_pid)
    if executable.name != "bash" or not any(
        left == "hold" and right == kind for left, right in itertools.pairwise(argv)
    ):
        msg = "broker parent is not the reviewed host hold process"
        raise LockProofError(msg)


def _stat_fields(proc_root: Path, pid: int) -> tuple[int, int]:
    """Return one process parent and start time from proc stat."""
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
        tail = raw[raw.rindex(")") + 2 :].split()
        return int(tail[1]), int(tail[19])
    except (OSError, ValueError, IndexError) as exc:
        msg = "cannot authenticate process identity"
        raise LockProofError(msg) from exc


def _validate_request(request: ProofRequest) -> None:
    """Reject malformed or noncanonical proof authorities."""
    if LOCK_ID_RE.fullmatch(request.lock_id) is None or request.kind not in {"wrapped", "detached"}:
        msg = "requested lock identity is malformed"
        raise LockProofError(msg)
    if any(
        SHA256_RE.fullmatch(digest) is None
        for digest in (request.expected_script_sha256, request.expected_broker_sha256)
    ):
        msg = "reviewed holder digest is malformed"
        raise LockProofError(msg)
    if request.root != CANONICAL_ROOT and os.environ.get("RA8_LOCK_VERIFY_SELFTEST") != "1":
        msg = "production verification is bound to the canonical lock root"
        raise LockProofError(msg)


def _initial_proof(request: ProofRequest) -> _ProofState:
    """Bind the record, live PIDs, canonical flock, and reviewed source bytes."""
    record, record_identity = _record(request.root / "holder.json", request.lock_id, request.kind)
    pid = int(record["pid"])
    if _start_ticks(request.proc_root, pid) != int(record["pid_start_ticks"]):
        msg = "holder PID was reused or the record is stale"
        raise LockProofError(msg)
    try:
        boot = (request.proc_root / "sys/kernel/random/boot_id").read_text(encoding="ascii").strip()
        lock_stat = (request.root / "board.lock").stat(follow_symlinks=False)
    except OSError as exc:
        msg = "canonical lock or boot identity is unavailable"
        raise LockProofError(msg) from exc
    if stat.S_ISLNK(lock_stat.st_mode) or not stat.S_ISREG(lock_stat.st_mode):
        msg = "canonical board lock is not a regular inode"
        raise LockProofError(msg)
    if record["boot_id"] != boot or _lock_owner_pids(request.proc_root, lock_stat) != {pid}:
        msg = "record does not match one unambiguous live kernel lock owner"
        raise LockProofError(msg)
    broker_fds = _holder_fds(request.proc_root, pid)
    host_fds = _holder_fds(request.proc_root, int(record["host_pid"]))
    if not _has_locked_inode(broker_fds, lock_stat):
        msg = "broker lacks the canonical lock descriptor"
        raise LockProofError(msg)
    if _has_script_digest(broker_fds, request.expected_script_sha256):
        msg = "broker inherited the reviewed host descriptor"
        raise LockProofError(msg)
    if _has_locked_inode(host_fds, lock_stat) or not _has_script_digest(
        host_fds, request.expected_script_sha256
    ):
        msg = "reviewed host is missing or inherited the lock descriptor"
        raise LockProofError(msg)
    _broker_shape(request.proc_root, request.root, record, request.expected_broker_sha256)
    _host_shape(request.proc_root, record, request.kind)
    return _ProofState(record, record_identity, lock_stat, pid)


def _final_proof(request: ProofRequest, proof: _ProofState) -> None:
    """Recheck every replaceable identity before returning a held verdict."""
    final_record = (request.root / "holder.json").stat(follow_symlinks=False)
    if (final_record.st_dev, final_record.st_ino) != (
        proof.record_identity.st_dev,
        proof.record_identity.st_ino,
    ):
        msg = "holder record changed before proof completion"
        raise LockProofError(msg)
    if _start_ticks(request.proc_root, proof.holder_pid) != int(proof.record["pid_start_ticks"]):
        msg = "holder exited before proof completion"
        raise LockProofError(msg)
    if _start_ticks(request.proc_root, int(proof.record["host_pid"])) != int(
        proof.record["host_start_ticks"]
    ):
        msg = "reviewed host exited before proof completion"
        raise LockProofError(msg)
    if _lock_owner_pids(request.proc_root, proof.lock_identity) != {proof.holder_pid}:
        msg = "kernel lock changed before proof completion"
        raise LockProofError(msg)
    final_lock = (request.root / "board.lock").stat(follow_symlinks=False)
    if (final_lock.st_dev, final_lock.st_ino) != (
        proof.lock_identity.st_dev,
        proof.lock_identity.st_ino,
    ):
        msg = "canonical lock inode changed before proof completion"
        raise LockProofError(msg)


def verify(request: ProofRequest) -> None:
    """Prove record, process, script, and canonical flock are one live holder."""
    _validate_request(request)
    _final_proof(request, _initial_proof(request))


def _replacement_selftest() -> list[str]:
    """Prove descriptor reads reject a path replacement after the read."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-lock-proof-") as raw:
        path = Path(raw) / "holder.json"
        replacement = path.with_suffix(".new")
        path.write_bytes(b"old")
        replacement.write_bytes(b"new")

        def replace() -> None:
            replacement.replace(path)

        try:
            _read_regular(path, 16, replace)
        except LockProofError:
            pass
        else:
            failures.append("holder record replacement escaped descriptor binding")
    return failures


def _stat_line(pid: int, parent: int, ticks: int) -> str:
    """Render the proc stat fields consumed by this verifier."""
    return f"{pid} (holder) S {parent} " + " ".join(["0"] * 17 + [str(ticks)]) + "\n"


def _write_lock_line(fixture: _Fixture, owners: tuple[int, ...]) -> None:
    """Render active FLOCK records for the canonical fixture inode."""
    observed = fixture.lock.stat()
    device = f"{os.major(observed.st_dev):x}:{os.minor(observed.st_dev):x}:{observed.st_ino}"
    lines = [
        f"{index}: FLOCK ADVISORY WRITE {pid} {device} 0 EOF" for index, pid in enumerate(owners, 1)
    ]
    (fixture.proc / "locks").write_text("\n".join(lines) + "\n", encoding="ascii")


def _write_broker_process(fixture: _Fixture) -> None:
    """Create the synthetic isolated broker process and its locked fd."""
    argv = [
        "/usr/bin/python3",
        "-I",
        "-S",
        "-c",
        fixture.broker.read_text(encoding="utf-8"),
        str(fixture.lock),
        str(fixture.record),
        "0",
        "fields",
        str(fixture.host_pid),
        str(fixture.host_ticks),
    ]
    process = fixture.proc / str(fixture.pid)
    (process / "stat").write_text(
        _stat_line(fixture.pid, fixture.host_pid, fixture.ticks), encoding="ascii"
    )
    (process / "cmdline").write_bytes(b"\0".join(item.encode() for item in argv) + b"\0")
    (process / "exe").symlink_to("/usr/bin/python3")
    (process / "fd/3").symlink_to(fixture.lock)


def _write_host_process(proc: Path, script: Path, pid: int, ticks: int) -> None:
    """Create the reviewed Bash parent without a lock descriptor."""
    (proc / str(pid) / "stat").write_text(_stat_line(pid, 12, ticks), encoding="ascii")
    (proc / str(pid) / "cmdline").write_bytes(b"bash\0bench_host.sh\0hold\0wrapped\0")
    (proc / str(pid) / "exe").symlink_to("/bin/bash")
    (proc / str(pid) / "fd/4").symlink_to(script)


def _make_fixture(base: Path) -> _Fixture:
    """Create a complete synthetic Linux proc and canonical lock authority."""
    root = base / "bench"
    proc = base / "proc"
    pid = 321
    ticks = 98765
    host_pid = 320
    host_ticks = 87654
    lock_id = "0123456789abcdef"
    (proc / str(pid) / "fd").mkdir(parents=True)
    (proc / str(host_pid) / "fd").mkdir(parents=True)
    (proc / "sys/kernel/random").mkdir(parents=True)
    root.mkdir()
    lock = root / "board.lock"
    script = root / "bench_host.sh"
    broker = root / "bench_lock_broker.py"
    record = root / "holder.json"
    lock.write_bytes(b"")
    script.write_bytes(b"#!/bin/bash\n")
    broker.write_bytes(b"# broker fixture\n")
    boot_id = "12345678-1234-1234-1234-123456789abc"
    (proc / "sys/kernel/random/boot_id").write_text(boot_id + "\n", encoding="ascii")
    record.write_text(
        json.dumps(
            {
                "resource": "bench",
                "lock_id": lock_id,
                "hold_kind": "wrapped",
                "pid": pid,
                "pid_start_ticks": ticks,
                "host_pid": host_pid,
                "host_start_ticks": host_ticks,
                "boot_id": boot_id,
            }
        ),
        encoding="utf-8",
    )
    fixture = _Fixture(
        root,
        proc,
        record,
        lock,
        script,
        broker,
        pid,
        ticks,
        host_pid,
        host_ticks,
        lock_id,
    )
    _write_broker_process(fixture)
    _write_host_process(proc, script, host_pid, host_ticks)
    _write_lock_line(fixture, (pid,))
    return fixture


def _expect_refusal(fixture: _Fixture, label: str, failures: list[str]) -> None:
    """Require the current fixture state to fail authentication."""
    try:
        verify(fixture.request)
    except LockProofError:
        return
    failures.append(f"{label} escaped live lock authentication")


def _authority_selftest(fixture: _Fixture) -> list[str]:
    """Attack kernel ownership, record identity, PID reuse, and script binding."""
    failures: list[str] = []
    verify(fixture.request)
    _write_lock_line(fixture, ())
    _expect_refusal(fixture, "forged record without flock", failures)
    _write_lock_line(fixture, (fixture.pid + 1,))
    _expect_refusal(fixture, "unrelated flock", failures)
    _write_lock_line(fixture, (fixture.pid, fixture.pid + 1))
    _expect_refusal(fixture, "ambiguous flock ownership", failures)
    _write_lock_line(fixture, (fixture.pid,))
    stat_path = fixture.proc / str(fixture.pid) / "stat"
    stat_path.write_text(_stat_line(fixture.pid, 12, fixture.ticks + 1), encoding="ascii")
    _expect_refusal(fixture, "reused holder PID", failures)
    stat_path.write_text(_stat_line(fixture.pid, 12, fixture.ticks), encoding="ascii")
    try:
        verify(replace(fixture.request, expected_script_sha256="f" * 64))
    except LockProofError:
        pass
    else:
        failures.append("tampered reviewed-holder digest was accepted")
    try:
        verify(replace(fixture.request, expected_broker_sha256="e" * 64))
    except LockProofError:
        pass
    else:
        failures.append("tampered reviewed-broker digest was accepted")
    leaked = fixture.proc / str(fixture.host_pid) / "fd/9"
    leaked.symlink_to(fixture.lock)
    _expect_refusal(fixture, "lock descriptor leaked to Bash host", failures)
    leaked.unlink()
    broker_leak = fixture.proc / str(fixture.pid) / "fd/4"
    broker_leak.symlink_to(fixture.script)
    _expect_refusal(fixture, "reviewed host descriptor leaked to broker", failures)
    broker_leak.unlink()
    return failures


def _race_selftest(fixture: _Fixture) -> list[str]:
    """Attack record/lock replacement and holder exit between proof phases."""
    failures: list[str] = []
    real_ticks = _start_ticks

    def replace_record(proc_root: Path, pid: int) -> int:
        if fixture.record.exists():
            replacement = fixture.record.with_suffix(".replacement")
            replacement.write_bytes(fixture.record.read_bytes())
            replacement.replace(fixture.record)
        return real_ticks(proc_root, pid)

    with mock.patch.object(sys.modules[__name__], "_start_ticks", side_effect=replace_record):
        _expect_refusal(fixture, "holder record inode replacement", failures)
    # Restore the record to a stable inode before later race attacks.
    fixture.record.write_bytes(fixture.record.read_bytes())
    with mock.patch.object(
        sys.modules[__name__],
        "_start_ticks",
        side_effect=[fixture.ticks, LockProofError("holder exited")],
    ):
        _expect_refusal(fixture, "holder exit during proof", failures)
    real_owners = _lock_owner_pids
    calls = 0

    def replace_lock(proc_root: Path, observed: os.stat_result) -> set[int]:
        nonlocal calls
        calls += 1
        result = real_owners(proc_root, observed)
        if calls == SECOND_LOCK_OBSERVATION:
            replacement = fixture.lock.with_suffix(".replacement")
            replacement.write_bytes(b"")
            replacement.replace(fixture.lock)
        return result

    with mock.patch.object(sys.modules[__name__], "_lock_owner_pids", side_effect=replace_lock):
        _expect_refusal(fixture, "canonical lock inode replacement", failures)
    return failures


def _live_fields(lock_id: str) -> str:
    """Return one valid wrapped request for the real-process descriptor test."""
    fields = {
        "resource": "bench",
        "lock_id": lock_id,
        "holder_class": "agent",
        "holder_name": "selftest",
        "intent": "offline live descriptor selftest",
        "max_hold_s": "30",
        "hold_kind": "wrapped",
        "break_glass": "false",
        "origin": "selftest",
        "git_ref": "dev",
    }
    raw = "".join(f"{key}={value}\n" for key, value in fields.items()).encode()
    return base64.b64encode(raw).decode("ascii")


def _start_live_host(base: Path, lock_id: str) -> tuple[subprocess.Popen[str], ProofRequest]:
    """Start the actual Bash host and Python broker against a throwaway lock."""
    source_dir = Path(__file__).resolve().parent
    host_source = (source_dir / "bench_host.sh").read_bytes()
    broker_source = (source_dir / "bench_lock_broker.py").read_bytes()
    host = base / "bench_host.sh"
    broker = base / "bench_lock_broker.py"
    root = base / "bench"
    host.write_bytes(host_source)
    broker.write_bytes(broker_source)
    environment = {
        "PATH": "/usr/bin:/bin",
        "RA8_BENCH_DIR": str(root),
        "RA8_BENCH_BROKER_SRC": str(broker),
    }
    process = subprocess.Popen(  # noqa: S603 -- exact reviewed selftest host
        ["/bin/bash", str(host), "hold", "wrapped", "0", _live_fields(lock_id)],
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    request = ProofRequest(
        lock_id,
        "wrapped",
        hashlib.sha256(host_source).hexdigest(),
        hashlib.sha256(broker_source).hexdigest(),
        root,
    )
    return process, request


def _proof_timeout(_signum: int, _frame: object) -> None:
    """Interrupt a verifier that reads the wrapped liveness pipe."""
    message = "live descriptor proof blocked"
    raise _ProofDeadlineError(message)


def _verify_bounded(request: ProofRequest) -> None:
    """Run one real-process proof with a hard nonblocking deadline."""
    previous_handler = signal.signal(signal.SIGALRM, _proof_timeout)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, 5.0)
    try:
        verify(request)
    finally:
        signal.setitimer(signal.ITIMER_REAL, *previous_timer)
        signal.signal(signal.SIGALRM, previous_handler)


def _stop_live_host(process: subprocess.Popen[str]) -> None:
    """Close the liveness pipe and reap only this throwaway holder."""
    if process.stdin is not None:
        process.stdin.close()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def _digest_scan_bounded(fds: list[Path], expected: str) -> bool:
    """Run one descriptor digest scan with a hard liveness deadline."""
    previous_handler = signal.signal(signal.SIGALRM, _proof_timeout)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, 2.0)
    try:
        return _has_script_digest(fds, expected)
    finally:
        signal.setitimer(signal.ITIMER_REAL, *previous_timer)
        signal.signal(signal.SIGALRM, previous_handler)


def _prefilled_pipe_scan(script: Path, expected: str, payload: bytes) -> tuple[bool, bytes]:
    """Scan a pipe before a regular file and return its unconsumed payload."""
    read_fd, write_fd = os.pipe()
    try:
        os.write(write_fd, payload)
        matched = _has_script_digest([Path(f"/proc/self/fd/{read_fd}"), script], expected)
        os.set_blocking(read_fd, False)
        try:
            unread = os.read(read_fd, len(payload))
        except BlockingIOError:
            unread = b""
    finally:
        os.close(read_fd)
        os.close(write_fd)
    return matched, unread


def _pipe_descriptor_selftest() -> list[str]:
    """Prove descriptor classification never consumes a pipe before a file."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-lock-pipe-fd-") as raw:
        root = Path(raw)
        script = root / "bench_host.sh"
        script.write_bytes(b"#!/bin/bash\n# reviewed host\n")
        expected = hashlib.sha256(script.read_bytes()).hexdigest()
        payload = b"pipe bytes are not reviewed host source"
        pipe_digest = hashlib.sha256(payload).hexdigest()
        false_match, false_unread = _prefilled_pipe_scan(script, pipe_digest, payload)
        if false_match or false_unread != payload:
            failures.append("non-regular fd supplied or consumed a matching digest")
        true_match, true_unread = _prefilled_pipe_scan(script, expected, payload)
        if not true_match or true_unread != payload:
            failures.append("non-regular fd was consumed before the reviewed regular fd")
        fifo = root / "blocking.fifo"
        os.mkfifo(fifo)
        try:
            fifo_match = _digest_scan_bounded([fifo, script], expected)
        except _ProofDeadlineError:
            failures.append("opening a non-regular fd blocked before the reviewed regular fd")
        else:
            if not fifo_match:
                failures.append("a skipped FIFO prevented the reviewed regular fd from matching")
    return failures


def _live_digest_controls(request: ProofRequest, host_fds: list[Path], base: Path) -> list[str]:
    """Attack a real holder's digest and its deleted script pathname."""
    failures: list[str] = []
    try:
        _verify_bounded(replace(request, expected_script_sha256="f" * 64))
    except LockProofError:
        pass
    else:
        failures.append("real Bash host accepted the wrong reviewed script digest")
    replacement_payload = b"#!/bin/bash\n# path replacement\n"
    replacement_path = base / "bench_host.sh"
    replacement_path.write_bytes(replacement_payload)
    try:
        _verify_bounded(request)
    except (LockProofError, OSError, _ProofDeadlineError) as exc:
        failures.append(f"script path replacement displaced the open host inode: {exc}")
    replacement_digest = hashlib.sha256(replacement_payload).hexdigest()
    if _has_script_digest(host_fds, replacement_digest):
        failures.append("script path replacement redirected the open host descriptor")
    return failures


def _live_descriptor_selftest() -> list[str]:
    """Prove the verifier never reads the real host's liveness pipe."""
    if not Path("/proc/self/fd").is_dir():
        return []
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-lock-live-") as raw:
        process, request = _start_live_host(Path(raw), "fedcba9876543210")
        try:
            if process.stdout is None or not select.select([process.stdout], [], [], 5)[0]:
                return ["real Bash host did not acknowledge its throwaway lock"]
            if process.stdout.readline() != "bench: ACQUIRED fedcba9876543210\n":
                return ["real Bash host returned the wrong acquisition identity"]
            try:
                _verify_bounded(request)
            except (LockProofError, OSError, _ProofDeadlineError) as exc:
                failures.append(f"real Bash host proof failed: {exc}")
                return failures
            record, _identity = _record(request.root / "holder.json", request.lock_id, request.kind)
            lock_stat = (request.root / "board.lock").stat(follow_symlinks=False)
            broker_fds = _holder_fds(request.proc_root, int(record["pid"]))
            host_fds = _holder_fds(request.proc_root, int(record["host_pid"]))
            if not _has_locked_inode(broker_fds, lock_stat):
                failures.append("real broker lost its lock descriptor")
            if _has_locked_inode(host_fds, lock_stat):
                failures.append("real Bash host inherited the lock descriptor")
            if not _has_script_digest(host_fds, request.expected_script_sha256):
                failures.append("real Bash host lost the reviewed script descriptor")
            if _has_script_digest(broker_fds, request.expected_script_sha256):
                failures.append("real broker inherited the reviewed script descriptor")
            failures.extend(_live_digest_controls(request, host_fds, Path(raw)))
        finally:
            _stop_live_host(process)
    return failures


def run_selftest() -> list[str]:
    """Run deterministic offline attacks against every lock proof boundary."""
    previous = os.environ.get("RA8_LOCK_VERIFY_SELFTEST")
    os.environ["RA8_LOCK_VERIFY_SELFTEST"] = "1"
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-lock-authority-") as raw:
            fixture = _make_fixture(Path(raw))
            return (
                _replacement_selftest()
                + _authority_selftest(fixture)
                + _race_selftest(fixture)
                + _pipe_descriptor_selftest()
                + _live_descriptor_selftest()
            )
    finally:
        if previous is None:
            os.environ.pop("RA8_LOCK_VERIFY_SELFTEST", None)
        else:
            os.environ["RA8_LOCK_VERIFY_SELFTEST"] = previous


def main() -> int:
    """Parse one fixed verification request and report a fail-closed verdict."""
    if sys.argv[1:] == ["--selftest"]:
        failures = run_selftest()
        for failure in failures:
            print(f"bench-lock-verify selftest: {failure}", file=sys.stderr)
        return 1 if failures else 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lock_id")
    parser.add_argument("hold_kind", choices=("wrapped", "detached"))
    parser.add_argument("expected_script_sha256")
    parser.add_argument("expected_broker_sha256")
    args = parser.parse_args()
    try:
        verify(
            ProofRequest(
                args.lock_id,
                args.hold_kind,
                args.expected_script_sha256,
                args.expected_broker_sha256,
            )
        )
    except (LockProofError, OSError, UnicodeError) as exc:
        print(f"bench-lock-verify: {exc}", file=sys.stderr)
        return 3
    print("bench-lock-verify: HELD")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
