# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticate the controller-side process capability for a bench hold."""

from __future__ import annotations

import base64
import binascii
import os
import re
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path

SSH_PREFIX = (
    "-T",
    "-o",
    "BatchMode=yes",
    "-o",
    "ConnectTimeout=8",
    "-o",
    "ServerAliveInterval=15",
    "-o",
    "ServerAliveCountMax=4",
)
LOCK_ID_RE = re.compile(r"[0-9a-f]{16}")
PID_RE = re.compile(r"[1-9][0-9]*")
MAX_ANCESTORS = 64


@dataclass(frozen=True)
class Capability:
    """Inherited facts which must bind to the live wrapper process tree."""

    lock_id: str
    holder_pid: int
    holder_start_ticks: int
    target: str


@dataclass(frozen=True)
class _SyntheticProcess:
    """One synthetic proc identity used by the offline attacks."""

    pid: int
    parent: int
    ticks: int
    executable: str
    argv: list[str]
    cwd: Path | None = None


@dataclass(frozen=True)
class _Fixture:
    """Controller process-tree authority for the offline attacks."""

    repo: Path
    proc: Path
    capability: Capability
    current_pid: int


class CapabilityError(ValueError):
    """Inherited capability facts did not match the trusted wrapper."""


def _stat_fields(proc_root: Path, pid: int) -> tuple[int, int]:
    """Return parent PID and start ticks from one proc stat record."""
    try:
        raw = (proc_root / str(pid) / "stat").read_text(encoding="ascii")
        tail = raw[raw.rindex(")") + 2 :].split()
        return int(tail[1]), int(tail[19])
    except (OSError, ValueError, IndexError) as exc:
        msg = f"cannot authenticate controller PID {pid}"
        raise CapabilityError(msg) from exc


def _argv(proc_root: Path, pid: int) -> list[str]:
    """Read one process argv as strict UTF-8 fields."""
    try:
        raw = (proc_root / str(pid) / "cmdline").read_bytes()
        return [field.decode("utf-8", "strict") for field in raw.split(b"\0") if field]
    except (OSError, UnicodeError) as exc:
        msg = f"cannot authenticate controller argv for PID {pid}"
        raise CapabilityError(msg) from exc


def _executable(proc_root: Path, pid: int) -> Path:
    """Resolve one process executable or fail closed."""
    try:
        return (proc_root / str(pid) / "exe").resolve(strict=True)
    except OSError as exc:
        msg = f"cannot authenticate controller executable for PID {pid}"
        raise CapabilityError(msg) from exc


def _ancestors(proc_root: Path, pid: int) -> list[int]:
    """Return the bounded live ancestry for one process."""
    result: list[int] = []
    seen: set[int] = set()
    while pid > 1 and pid not in seen and len(result) < MAX_ANCESTORS:
        seen.add(pid)
        result.append(pid)
        pid, _ticks = _stat_fields(proc_root, pid)
    return result


def _decoded_fields(remote: str) -> list[str]:
    """Decode bounded base64 arguments embedded in the fixed remote command."""
    decoded: list[str] = []
    for candidate in re.findall(r"'([A-Za-z0-9+/=]{16,})'", remote):
        value = _decode_field(candidate)
        if value is not None:
            decoded.append(value)
    return decoded


def _decode_field(candidate: str) -> str | None:
    """Decode one bounded field without placing exception handling in the scan loop."""
    try:
        raw = base64.b64decode(candidate, validate=True)
        return raw.decode("utf-8", "strict")
    except (binascii.Error, UnicodeError):
        return None


def _holder_command(
    proc_root: Path,
    capability: Capability,
    bench_host_source: bytes,
) -> int:
    """Require exact ssh transport, target, reviewed source, and lock fields."""
    argv = _argv(proc_root, capability.holder_pid)
    if _executable(proc_root, capability.holder_pid) != Path("/usr/bin/ssh"):
        msg = "bench holder is not the fixed ssh executable"
        raise CapabilityError(msg)
    prefix_end = 1 + len(SSH_PREFIX)
    if tuple(argv[1:prefix_end]) != SSH_PREFIX or len(argv) != prefix_end + 2:
        msg = "bench holder ssh options are not exact"
        raise CapabilityError(msg)
    if argv[prefix_end] != capability.target:
        msg = "bench holder ssh target does not match the declared target"
        raise CapabilityError(msg)
    remote = argv[prefix_end + 1]
    encoded_source = base64.b64encode(bench_host_source).decode("ascii")
    if encoded_source not in remote or "'hold' 'wrapped'" not in remote:
        msg = "bench holder remote command is not the reviewed wrapped hold"
        raise CapabilityError(msg)
    fields = _decoded_fields(remote)
    expected = f"lock_id={capability.lock_id}\n"
    if not any(expected in value and "hold_kind=wrapped\n" in value for value in fields):
        msg = "bench holder remote command does not bind this lock ID"
        raise CapabilityError(msg)
    parent, ticks = _stat_fields(proc_root, capability.holder_pid)
    if ticks != capability.holder_start_ticks:
        msg = "bench holder PID was reused"
        raise CapabilityError(msg)
    return parent


def _wrapper_parent(proc_root: Path, parent: int, repo_root: Path, current_pid: int) -> None:
    """Require the holder and current command beneath one reviewed bench.sh."""
    if parent not in _ancestors(proc_root, current_pid):
        msg = "bench holder is not a sibling in this wrapper transaction"
        raise CapabilityError(msg)
    if _executable(proc_root, parent).name != "bash":
        msg = "bench wrapper parent is not Bash"
        raise CapabilityError(msg)
    argv = _argv(proc_root, parent)
    try:
        cwd = (proc_root / str(parent) / "cwd").resolve(strict=True)
    except OSError as exc:
        msg = "cannot authenticate bench wrapper working directory"
        raise CapabilityError(msg) from exc
    expected = (repo_root / "scripts/hil/bench.sh").resolve(strict=True)
    candidates = [
        (cwd / argument).resolve() for argument in argv[1:] if argument.endswith("bench.sh")
    ]
    if candidates != [expected] or "run" not in argv:
        msg = "controller ancestry does not contain the reviewed bench wrapper"
        raise CapabilityError(msg)


def authenticate(
    capability: Capability,
    repo_root: Path,
    proc_root: Path = Path("/proc"),
    current_pid: int | None = None,
) -> None:
    """Authenticate one inherited hold against the exact local process tree."""
    if LOCK_ID_RE.fullmatch(capability.lock_id) is None or capability.holder_pid <= 1:
        msg = "bench capability identity is malformed"
        raise CapabilityError(msg)
    if (
        capability.holder_start_ticks <= 0
        or not capability.target
        or any(character.isspace() for character in capability.target)
    ):
        msg = "bench capability transport is malformed"
        raise CapabilityError(msg)
    source = (repo_root / "scripts/hil/lib/bench_host.sh").read_bytes()
    if not source:
        msg = "bench holder source is empty"
        raise CapabilityError(msg)
    parent = _holder_command(proc_root, capability, source)
    _wrapper_parent(proc_root, parent, repo_root, current_pid or os.getpid())


def from_environment(environment: Mapping[str, str]) -> Capability:
    """Parse the exact inherited capability fields without accepting aliases."""
    pid = environment.get("RA8_BENCH_HOLDER_PID", "")
    ticks = environment.get("RA8_BENCH_HOLDER_START_TICKS", "")
    if PID_RE.fullmatch(pid) is None or PID_RE.fullmatch(ticks) is None:
        msg = "bench holder process identity is absent or malformed"
        raise CapabilityError(msg)
    return Capability(
        environment.get("RA8_BENCH_LOCK_ID", ""),
        int(pid),
        int(ticks),
        environment.get("RA8_BENCH_HOLDER_TARGET", ""),
    )


def _write_process(
    proc_root: Path,
    spec: _SyntheticProcess,
) -> None:
    """Create one synthetic proc entry for the offline capability attacks."""
    process = proc_root / str(spec.pid)
    process.mkdir(parents=True)
    tail = ["S", str(spec.parent), *(["0"] * 17), str(spec.ticks)]
    (process / "stat").write_text(
        f"{spec.pid} (fixture) " + " ".join(tail) + "\n", encoding="ascii"
    )
    (process / "cmdline").write_bytes(b"\0".join(item.encode() for item in spec.argv) + b"\0")
    (process / "exe").symlink_to(spec.executable)
    if spec.cwd is not None:
        (process / "cwd").symlink_to(spec.cwd)


def _fixture_remote(source: bytes, lock_id: str) -> str:
    """Render the bounded fields consumed from the reviewed holder command."""
    encoded_source = base64.b64encode(source).decode("ascii")
    fields = base64.b64encode(
        f"resource=bench\nlock_id={lock_id}\nhold_kind=wrapped\n".encode()
    ).decode("ascii")
    return f"printf %s '{encoded_source}' | base64 -d; 'hold' 'wrapped' '{fields}'"


def _make_fixture(base: Path) -> _Fixture:
    """Create one wrapper, exact holder, foreign sibling, and current child."""
    repo = base / "repo"
    proc = base / "proc"
    host = repo / "scripts/hil/lib/bench_host.sh"
    wrapper = repo / "scripts/hil/bench.sh"
    host.parent.mkdir(parents=True)
    host.write_bytes(b"#!/bin/bash\n# reviewed\n")
    wrapper.write_bytes(b"#!/bin/bash\n")
    wrapper_pid, holder_pid, foreign_pid, current_pid = 100, 101, 103, 102
    _write_process(
        proc,
        _SyntheticProcess(wrapper_pid, 1, 500, "/bin/bash", ["bash", str(wrapper), "run"], repo),
    )
    capability = Capability("0123456789abcdef", holder_pid, 600, "star.local")
    holder_argv = [
        "/usr/bin/ssh",
        *SSH_PREFIX,
        capability.target,
        _fixture_remote(host.read_bytes(), capability.lock_id),
    ]
    _write_process(
        proc, _SyntheticProcess(holder_pid, wrapper_pid, 600, "/usr/bin/ssh", holder_argv)
    )
    foreign_argv = ["/usr/bin/ssh", *SSH_PREFIX, "foreign.local", "unreviewed"]
    _write_process(
        proc, _SyntheticProcess(foreign_pid, wrapper_pid, 601, "/usr/bin/ssh", foreign_argv)
    )
    _write_process(
        proc, _SyntheticProcess(current_pid, wrapper_pid, 700, "/usr/bin/python3", ["python3"])
    )
    return _Fixture(repo, proc, capability, current_pid)


def _expect_refusal(
    fixture: _Fixture,
    capability: Capability,
    label: str,
    failures: list[str],
) -> None:
    """Require a synthetic controller capability attack to fail closed."""
    try:
        authenticate(capability, fixture.repo, fixture.proc, fixture.current_pid)
    except CapabilityError:
        return
    failures.append(f"{label} escaped controller capability authentication")


def run_selftest() -> list[str]:
    """Exercise PID, ancestry, target, source, and sibling binding offline."""
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="ra8-capability-") as raw:
        fixture = _make_fixture(Path(raw))
        capability = fixture.capability
        authenticate(capability, fixture.repo, fixture.proc, fixture.current_pid)
        reused = Capability(capability.lock_id, capability.holder_pid, 601, capability.target)
        _expect_refusal(fixture, reused, "PID reuse", failures)
        wrong_target = Capability(capability.lock_id, capability.holder_pid, 600, "other.local")
        _expect_refusal(fixture, wrong_target, "remote target drift", failures)
        foreign = Capability(capability.lock_id, 103, 601, "foreign.local")
        _expect_refusal(fixture, foreign, "foreign same-parent ssh", failures)
        (fixture.repo / "scripts/hil/lib/bench_host.sh").write_bytes(b"tampered\n")
        _expect_refusal(fixture, capability, "reviewed holder tamper", failures)
        (fixture.repo / "scripts/hil/lib/bench_host.sh").write_bytes(b"#!/bin/bash\n# reviewed\n")
        detached_pid = 104
        _write_process(
            fixture.proc,
            _SyntheticProcess(detached_pid, 1, 800, "/usr/bin/python3", ["python3"]),
        )
        detached = _Fixture(fixture.repo, fixture.proc, capability, detached_pid)
        _expect_refusal(detached, capability, "foreign process tree", failures)
    return failures
