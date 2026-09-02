# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Own the bench flock without exposing its descriptor to Bash children."""

from __future__ import annotations

import argparse
import base64
import binascii
import fcntl
import json
import os
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

DENIED = 11
FAILED = 13
MAX_FIELDS_BYTES = 8192
FD_PROBE_SOURCE = (
    'import sys\nsys.stdout.write("READY\\n")\nsys.stdout.flush()\n'
    "raise SystemExit(0 if sys.stdin.buffer.read(1) == b'x' else 1)"
)


@dataclass(frozen=True)
class HoldRequest:
    """One authenticated broker request parsed from the fixed CLI."""

    lock: Path
    record: Path
    wait_s: int
    fields_b64: str
    host_pid: int
    host_ticks: int


class BrokerError(ValueError):
    """The lock request or its parent holder identity was unsafe."""


def _start_ticks(pid: int) -> int:
    """Read one Linux process start time without parsing its comm as fields."""
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        return int(raw[raw.rindex(")") + 2 :].split()[19])
    except (OSError, ValueError, IndexError) as exc:
        msg = f"cannot authenticate process {pid}"
        raise BrokerError(msg) from exc


def _fields(encoded: str) -> dict[str, str]:
    """Decode the existing bounded key=value hold request."""
    try:
        raw = base64.b64decode(encoded, validate=True)
        text = raw.decode("utf-8", "strict")
    except (binascii.Error, UnicodeError) as exc:
        msg = "hold fields are not canonical base64 UTF-8"
        raise BrokerError(msg) from exc
    if len(raw) > MAX_FIELDS_BYTES:
        msg = "hold fields exceed their size bound"
        raise BrokerError(msg)
    result: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in result:
            msg = "hold fields are malformed or duplicated"
            raise BrokerError(msg)
        result[key] = value
    required = {
        "break_glass",
        "git_ref",
        "hold_kind",
        "holder_class",
        "holder_name",
        "intent",
        "lock_id",
        "max_hold_s",
        "origin",
        "resource",
    }
    if not required <= result.keys() or result["resource"] != "bench":
        msg = "hold fields omit the bench identity"
        raise BrokerError(msg)
    return result


def _close_error(descriptor: int, closer: Callable[[int], None]) -> OSError | None:
    """Attempt one numeric close exactly once and return its error."""
    try:
        closer(descriptor)
    except OSError as error:
        return error
    return None


def _open_lock(
    path: Path,
    *,
    opener: Callable[[Path, int, int], int] = os.open,
    fd_stat: Callable[[int], os.stat_result] = os.fstat,
    path_stat: Callable[[Path], os.stat_result] | None = None,
    closer: Callable[[int], None] = os.close,
) -> tuple[int, os.stat_result]:
    """Open one stable non-linked regular lock inode with CLOEXEC."""
    flags = os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    descriptor: int | None = None
    try:
        descriptor = opener(path, flags, 0o666)
        observed = fd_stat(descriptor)
        current = path.stat(follow_symlinks=False) if path_stat is None else path_stat(path)
        inheritable = os.get_inheritable(descriptor)
    except OSError as exc:
        close_failure = None if descriptor is None else _close_error(descriptor, closer)
        msg = f"cannot open canonical lock: {exc}"
        error = BrokerError(msg)
        if close_failure is not None:
            error.add_note(f"lock descriptor close also failed: {close_failure}")
        raise error from exc
    if (
        not stat.S_ISREG(observed.st_mode)
        or observed.st_nlink != 1
        or (observed.st_dev, observed.st_ino) != (current.st_dev, current.st_ino)
        or inheritable
    ):
        close_failure = _close_error(descriptor, closer)
        msg = "canonical lock is linked, replaced, non-regular, or inheritable"
        error = BrokerError(msg)
        if close_failure is not None:
            error.add_note(f"lock descriptor close also failed: {close_failure}")
        raise error
    return descriptor, observed


def _take(descriptor: int, wait_s: int) -> None:
    """Take the flock within a monotonic bounded wait."""
    deadline = time.monotonic() + wait_s
    while not _try_take(descriptor):
        if time.monotonic() >= deadline:
            raise BlockingIOError
        time.sleep(0.1)


def _try_take(descriptor: int) -> bool:
    """Attempt one nonblocking flock acquisition."""
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return False
    return True


def _record(fields: dict[str, str], host_pid: int, host_ticks: int) -> dict[str, object]:
    """Construct telemetry only after the broker owns the live kernel lock."""
    now = int(time.time())
    try:
        boot = Path("/proc/sys/kernel/random/boot_id").read_text(encoding="ascii").strip()
        broker_ticks = _start_ticks(os.getpid())
        budget = int(fields["max_hold_s"])
    except (OSError, ValueError) as exc:
        msg = "cannot construct the live holder record"
        raise BrokerError(msg) from exc
    return {
        "resource": "bench",
        "lock_id": fields["lock_id"],
        "holder_class": fields["holder_class"],
        "holder_name": fields["holder_name"],
        "pid": os.getpid(),
        "pid_start_ticks": broker_ticks,
        "host_pid": host_pid,
        "host_start_ticks": host_ticks,
        "boot_id": boot,
        "origin": fields["origin"],
        "intent": fields["intent"],
        "git_ref": fields["git_ref"],
        "acquired_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "acquired_epoch": now,
        "max_hold_s": budget,
        "hold_kind": fields["hold_kind"],
        "last_activity": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "break_glass": fields["break_glass"] == "true",
    }


def _publish(path: Path, record: dict[str, object]) -> os.stat_result:
    """Atomically publish and durably bind one holder-record inode."""
    payload = (json.dumps(record, indent=2, separators=(",", ": ")) + "\n").encode()
    temp = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temp, flags, 0o666)
    try:
        os.fchmod(descriptor, 0o666)
        remaining = memoryview(payload)
        while remaining:
            remaining = remaining[os.write(descriptor, remaining) :]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    temp.replace(path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return path.stat(follow_symlinks=False)


def _cleanup(path: Path, identity: os.stat_result | None) -> None:
    """Remove only the exact record inode this broker published."""
    if identity is None:
        return
    try:
        current = path.stat(follow_symlinks=False)
        if (current.st_dev, current.st_ino) != (identity.st_dev, identity.st_ino):
            return
        path.unlink()
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except FileNotFoundError:
        return


@dataclass(frozen=True)
class ReleaseActions:
    """Exact record, lock, and descriptor operations for one finalizer."""

    cleanup: Callable[[Path, os.stat_result | None], None] = _cleanup
    unlock: Callable[[int, int], None] = fcntl.flock
    closer: Callable[[int], None] = os.close


def _interrupt(_signum: int, _frame: object) -> None:
    """Turn transport-loss signals into finally-based record/lock cleanup."""
    raise InterruptedError


def _release_hold(
    record: tuple[Path, os.stat_result | None],
    descriptor: int,
    actions: ReleaseActions | None = None,
) -> OSError | BrokerError | None:
    """Attempt record cleanup, unlock, and close while retaining the first error."""
    path, identity = record
    active = actions or ReleaseActions()
    first_error: OSError | BrokerError | None = None
    try:
        active.cleanup(path, identity)
    except (OSError, BrokerError) as error:
        first_error = error
    try:
        active.unlock(descriptor, fcntl.LOCK_UN)
    except OSError as error:
        if first_error is None:
            first_error = error
    close_error = _close_error(descriptor, active.closer)
    if first_error is None:
        first_error = close_error
    return first_error


def hold(request: HoldRequest) -> int:
    """Acquire, publish the exact process pair, and hold until parent EOF."""
    if os.getppid() != request.host_pid or _start_ticks(request.host_pid) != request.host_ticks:
        msg = "broker parent is not the authenticated host process"
        raise BrokerError(msg)
    fields = _fields(request.fields_b64)
    descriptor, _identity = _open_lock(request.lock)
    published: os.stat_result | None = None
    result = 0
    primary: OSError | BrokerError | InterruptedError | None = None
    try:
        try:
            _take(descriptor, request.wait_s)
        except BlockingIOError:
            result = DENIED
        else:
            published = _publish(
                request.record,
                _record(fields, request.host_pid, request.host_ticks),
            )
            print(f"ACQUIRED {os.getpid()} {_start_ticks(os.getpid())}", flush=True)
            while os.read(0, 4096):
                pass
    except (OSError, BrokerError, InterruptedError) as error:
        primary = error
    cleanup_error = _release_hold((request.record, published), descriptor)
    if primary is not None:
        if cleanup_error is not None:
            primary.add_note(f"broker finalizer also failed: {cleanup_error}")
        raise primary
    if cleanup_error is not None:
        raise cleanup_error
    return result


def _selftest_fields() -> str:
    """Return one valid wrapped-hold request for the broker selftest."""
    fields = {
        "resource": "bench",
        "lock_id": "0123456789abcdef",
        "holder_class": "agent",
        "holder_name": "selftest",
        "intent": "offline broker selftest",
        "max_hold_s": "30",
        "hold_kind": "wrapped",
        "break_glass": "false",
        "origin": "selftest",
        "git_ref": "dev",
    }
    text = "".join(f"{key}={value}\n" for key, value in fields.items())
    return base64.b64encode(text.encode()).decode("ascii")


def _start_broker(lock: Path, record: Path, host_ticks: int) -> subprocess.Popen[bytes]:
    """Start these exact source bytes as the synthetic broker child."""
    source = Path(__file__).read_text(encoding="utf-8")
    return subprocess.Popen(  # noqa: S603 -- fixed interpreter and this exact in-memory source
        [
            "/usr/bin/python3",
            "-B",
            "-I",
            "-S",
            "-c",
            source,
            str(lock),
            str(record),
            "0",
            _selftest_fields(),
            str(os.getpid()),
            str(host_ticks),
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
    )


def _lock_owners(lock: Path) -> set[int]:
    """Return kernel FLOCK owner PIDs for one exact inode."""
    observed = lock.stat()
    expected = (os.major(observed.st_dev), os.minor(observed.st_dev), observed.st_ino)
    owners: set[int] = set()
    for line in Path("/proc/locks").read_text(encoding="ascii").splitlines():
        fields = line.split()
        for index, field in enumerate(fields):
            match = re.fullmatch(r"([0-9a-fA-F]+):([0-9a-fA-F]+):(\d+)", field)
            if match is None or "FLOCK" not in fields or index == 0:
                continue
            identity = (int(match.group(1), 16), int(match.group(2), 16), int(match.group(3)))
            if identity == expected and fields[index - 1].isdigit():
                owners.add(int(fields[index - 1]))
    return owners


def _wait_ack(process: subprocess.Popen[bytes]) -> str:
    """Read the one flushed acquisition line with a fixed deadline."""
    if process.stdout is None:
        return ""
    readable, _writable, _exceptional = select.select([process.stdout], [], [], 5)
    return process.stdout.readline().decode("utf-8", "replace").strip() if readable else ""


def _process_argv(pid: int) -> tuple[bytes, ...] | None:
    """Return one live Linux argv only when its NUL framing is canonical."""
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        return None
    if not raw or not raw.endswith(b"\0"):
        return None
    fields = tuple(raw[:-1].split(b"\0"))
    return fields if fields and all(field for field in fields) else None


def _start_fd_probe() -> subprocess.Popen[bytes]:
    """Start one bounded child that stays live until its parent releases it."""
    return subprocess.Popen(  # noqa: S603 -- fixed interpreter and in-tree probe source
        ["/usr/bin/python3", "-B", "-I", "-S", "-c", FD_PROBE_SOURCE],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        close_fds=True,
    )


def _close_process_streams(process: subprocess.Popen[bytes]) -> OSError | None:
    """Attempt every parent-side stream close and retain the first failure."""
    first_error: OSError | None = None
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is None:
            continue
        try:
            stream.close()
        except OSError as error:
            if first_error is None:
                first_error = error
    return first_error


def _finish_child(
    process: subprocess.Popen[bytes], *, close_input: bool
) -> tuple[int, OSError | None]:
    """Bound, reap, and close one test child while retaining the first I/O error."""
    first_error: OSError | None = None
    if close_input and process.stdin is not None:
        try:
            process.stdin.close()
        except OSError as error:
            first_error = error
    try:
        try:
            status = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            status = process.wait(timeout=5)
    finally:
        close_error = _close_process_streams(process)
    if first_error is None:
        first_error = close_error
    return status, first_error


def _stop_fd_probe(process: subprocess.Popen[bytes]) -> int:
    """Release or forcibly reap the exact direct probe child within a deadline."""
    if process.stdin is not None:
        with suppress(BrokenPipeError):
            process.stdin.write(b"x")
    status, close_error = _finish_child(process, close_input=True)
    if close_error is not None:
        raise close_error
    return status


class _CloseProbe:
    """Record a parent-side stream close and optionally refuse it."""

    def __init__(self, *, fail: bool = False) -> None:
        self.closed = False
        self.fail = fail

    def close(self) -> None:
        """Record the exact attempt before raising the injected failure."""
        self.closed = True
        if self.fail:
            message = "injected broker stream-close failure"
            raise OSError(message)


class _ProcessProbe:
    """Expose the three stream fields consumed by exhaustive cleanup."""

    def __init__(self) -> None:
        self.stdin = _CloseProbe(fail=True)
        self.stdout = _CloseProbe()
        self.stderr = _CloseProbe()


def _linked_lock_refusal(root: Path, lock: Path) -> str | None:
    """Require a hard-linked lock refusal to close its opened descriptor."""
    linked = root / "open-cleanup-linked.lock"
    os.link(lock, linked)
    before = {path.name for path in Path("/proc/self/fd").iterdir()}
    try:
        _open_lock(lock)
    except BrokerError:
        if {path.name for path in Path("/proc/self/fd").iterdir()} != before:
            return "linked lock refusal leaked its opened descriptor"
    else:
        return "multiply-linked canonical lock inode was accepted"
    finally:
        linked.unlink()
    return None


def _open_lock_cleanup_selftest(root: Path) -> list[str]:
    """Prove every post-open refusal releases exactly its owned lock FD."""
    failures: list[str] = []
    lock = root / "open-cleanup.lock"
    lock.write_bytes(b"")
    opened = -1
    replacement = -1
    close_attempts: list[int] = []

    def opener(path: Path, flags: int, mode: int) -> int:
        nonlocal opened
        opened = os.open(path, flags, mode)
        return opened

    def deny_path(_path: Path) -> os.stat_result:
        message = "injected canonical-path stat failure"
        raise PermissionError(message)

    def close_reuse(descriptor: int) -> None:
        nonlocal replacement
        close_attempts.append(descriptor)
        os.close(descriptor)
        replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
        if replacement != descriptor:
            message = "lock descriptor number was not reused"
            raise BrokerError(message)
        message = "injected ambiguous lock close failure"
        raise OSError(message)

    try:
        _open_lock(lock, opener=opener, path_stat=deny_path, closer=close_reuse)
    except BrokerError as error:
        if (
            error.__cause__ is None
            or str(error.__cause__) != "injected canonical-path stat failure"
            or close_attempts != [opened]
            or replacement != opened
        ):
            failures.append("post-open stat refusal lost its primary error or retried close")
        else:
            try:
                os.fstat(replacement)
            except OSError:
                failures.append("post-open stat refusal closed a reused unrelated descriptor")
    else:
        failures.append("post-open canonical-path stat failure was accepted")
    finally:
        with suppress(OSError):
            os.close(replacement)

    linked_failure = _linked_lock_refusal(root, lock)
    if linked_failure is not None:
        failures.append(linked_failure)
    return failures


def _finalizer_cleanup_selftest(root: Path) -> list[str]:
    """Prove cleanup, unlock, close, and stream failures remain exhaustive."""
    failures: list[str] = []
    calls: list[str] = []

    def fail_cleanup(_path: Path, _identity: os.stat_result | None) -> None:
        calls.append("cleanup")
        message = "injected record cleanup failure"
        raise BrokerError(message)

    def fail_unlock(_descriptor: int, _operation: int) -> None:
        calls.append("unlock")
        message = "injected unlock failure"
        raise OSError(message)

    def fail_close(_descriptor: int) -> None:
        calls.append("close")
        message = "injected close failure"
        raise OSError(message)

    error = _release_hold(
        (root / "unused.json", None),
        12345,
        ReleaseActions(fail_cleanup, fail_unlock, fail_close),
    )
    if not isinstance(error, BrokerError) or calls != ["cleanup", "unlock", "close"]:
        failures.append("hold finalizer did not attempt every action or retain its first error")
    process = _ProcessProbe()
    stream_error = _close_process_streams(process)
    streams = (process.stdin, process.stdout, process.stderr)
    if stream_error is None or not all(stream.closed for stream in streams):
        failures.append("parent stream cleanup did not attempt every close after failure")
    return failures


def _fd_probe_selftest(lock: Path) -> list[str]:
    """Prove a ready, parent-held child has not inherited the broker lock."""
    failures = []
    child = _start_fd_probe()
    try:
        if _wait_ack(child) != "READY" or child.poll() is not None:
            failures.append("descriptor probe did not remain live after its readiness receipt")
        else:
            expected_argv = (
                b"/usr/bin/python3",
                b"-B",
                b"-I",
                b"-S",
                b"-c",
                FD_PROBE_SOURCE.encode("ascii"),
            )
            if _process_argv(child.pid) != expected_argv:
                failures.append("descriptor probe did not retain its fixed reviewed argv")
            lock_identity = lock.stat()
            try:
                descriptors = tuple(Path(f"/proc/{child.pid}/fd").iterdir())
                for fd in descriptors:
                    observed = fd.stat()
                    if (observed.st_dev, observed.st_ino) == (
                        lock_identity.st_dev,
                        lock_identity.st_ino,
                    ):
                        failures.append("broker lock descriptor leaked to an unrelated child")
                        break
            except OSError:
                failures.append("live descriptor probe disappeared during its inode proof")
    finally:
        if _stop_fd_probe(child) != 0:
            failures.append("descriptor probe did not exit cleanly after release")
    return failures


def _broker_lifecycle_selftest(root: Path) -> list[str]:
    """Prove kernel PID, competitor exclusion, cleanup, and fd non-inheritance."""
    failures: list[str] = []
    lock = root / "board.lock"
    record = root / "holder.json"
    lock.write_bytes(b"")
    host_ticks = _start_ticks(os.getpid())
    process = _start_broker(lock, record, host_ticks)
    try:
        ack = _wait_ack(process)
        source = Path(__file__).read_bytes()
        expected_argv = (
            b"/usr/bin/python3",
            b"-B",
            b"-I",
            b"-S",
            b"-c",
            source,
            os.fsencode(lock),
            os.fsencode(record),
            b"0",
            _selftest_fields().encode("ascii"),
            str(os.getpid()).encode("ascii"),
            str(host_ticks).encode("ascii"),
        )
        argv = _process_argv(process.pid)
        if argv != expected_argv:
            failures.append("broker child did not retain its fixed reviewed argv")
        if ack.split()[:2] != ["ACQUIRED", str(process.pid)] or _lock_owners(lock) != {process.pid}:
            failures.append("broker acquisition lacks one matching kernel FLOCK owner")
        competitor = os.open(lock, os.O_RDWR | os.O_CLOEXEC)
        try:
            try:
                fcntl.flock(competitor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                failures.append("competing client acquired the broker-held inode")
        finally:
            os.close(competitor)
        failures.extend(_fd_probe_selftest(lock))
    finally:
        status, close_error = _finish_child(process, close_input=True)
    if status != 0 or record.exists() or _lock_owners(lock):
        failures.append("stdin EOF did not release and clean the broker hold")
    if close_error is not None:
        failures.append("broker lifecycle did not close every parent-side stream")
    return failures


def _broker_failure_selftest(root: Path) -> list[str]:
    """Prove signal, stale-parent, and replaced-record cleanup fail closed."""
    failures: list[str] = []
    lock = root / "signal.lock"
    record = root / "signal.json"
    lock.write_bytes(b"")
    process = _start_broker(lock, record, _start_ticks(os.getpid()))
    try:
        if not _wait_ack(process):
            failures.append("signal fixture did not acquire")
        replacement = record.with_suffix(".replacement")
        replacement.write_text("unowned\n", encoding="utf-8")
        replacement.replace(record)
        process.send_signal(signal.SIGTERM)
    finally:
        _status, close_error = _finish_child(process, close_input=False)
    if record.read_text(encoding="utf-8") != "unowned\n":
        failures.append("broker cleanup removed a replaced holder record")
    if close_error is not None:
        failures.append("signaled broker did not close every parent-side stream")
    stale = _start_broker(root / "stale.lock", root / "stale.json", 1)
    stale_status, stale_close_error = _finish_child(stale, close_input=True)
    if stale_status == 0:
        failures.append("stale parent start time was accepted")
    if stale_close_error is not None:
        failures.append("stale-parent broker did not close every parent-side stream")
    return failures


def run_selftest() -> list[str]:
    """Exercise the real Linux broker process without a bench or network."""
    if not Path("/proc/locks").is_file():
        return ["bench lock broker requires Linux /proc/locks"]
    with tempfile.TemporaryDirectory(prefix="ra8-lock-broker-") as raw:
        root = Path(raw)
        return (
            _open_lock_cleanup_selftest(root)
            + _finalizer_cleanup_selftest(root)
            + _broker_lifecycle_selftest(root)
            + _broker_failure_selftest(root)
        )


def main() -> int:
    """Parse one fixed broker request and fail closed with stable exit codes."""
    if sys.argv[1:] == ["--selftest"]:
        failures = run_selftest()
        for failure in failures:
            print(f"bench-lock-broker selftest: {failure}", file=sys.stderr)
        return 1 if failures else 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lock", type=Path)
    parser.add_argument("record", type=Path)
    parser.add_argument("wait_s", type=int)
    parser.add_argument("fields_b64")
    parser.add_argument("host_pid", type=int)
    parser.add_argument("host_start_ticks", type=int)
    args = parser.parse_args()
    for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(signum, _interrupt)
    try:
        return hold(
            HoldRequest(
                args.lock,
                args.record,
                args.wait_s,
                args.fields_b64,
                args.host_pid,
                args.host_start_ticks,
            )
        )
    except InterruptedError:
        return FAILED
    except (BrokerError, OSError) as exc:
        print(f"bench-lock-broker: {exc}", file=sys.stderr)
        return FAILED


if __name__ == "__main__":
    raise SystemExit(main())
