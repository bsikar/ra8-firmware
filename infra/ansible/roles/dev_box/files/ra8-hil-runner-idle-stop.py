# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Stop one native Actions listener only after freezing and proving it idle."""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Protocol

SERVICE_RE = re.compile(r"[A-Za-z0-9_.@-]+[.]service")


class IdleStopError(RuntimeError):
    """The listener could not be proven idle and safely stopped."""


class Control(Protocol):
    """The systemd operations used by the idle-stop transaction."""

    def command(self, *args: str, accept: tuple[int, ...] = (0,)) -> str:
        """Run systemctl and return stripped stdout."""
        ...


class Systemctl:
    """Deadline-bound real systemctl transport."""

    def command(self, *args: str, accept: tuple[int, ...] = (0,)) -> str:
        """Run one fixed systemctl argv without a shell."""
        # The executable is fixed; args are internal literals plus a validated unit.
        result = subprocess.run(  # noqa: S603 -- fixed systemctl and validated unit argv
            ["/usr/bin/systemctl", *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
        if result.returncode not in accept:
            detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
            message = f"systemctl {' '.join(args)} failed: {detail}"
            raise IdleStopError(message)
        return result.stdout.strip()


def _cgroup_path(cgroup_root: Path, control_group: str) -> Path:
    """Resolve one absolute systemd cgroup beneath the fixed cgroup root."""
    if not control_group.startswith("/") or ".." in Path(control_group).parts:
        message = "systemd returned a non-canonical ControlGroup"
        raise IdleStopError(message)
    root = cgroup_root.resolve(strict=True)
    candidate = (root / control_group.lstrip("/")).resolve(strict=True)
    if candidate != root and root not in candidate.parents:
        message = "systemd ControlGroup escaped the cgroup root"
        raise IdleStopError(message)
    return candidate


def _cgroup_pids(cgroup: Path) -> set[int]:
    """Read every process in a frozen cgroup and its descendants."""
    pids: set[int] = set()
    for directory, dirnames, filenames in os.walk(cgroup, followlinks=False):
        dirnames[:] = [name for name in dirnames if not Path(directory, name).is_symlink()]
        if "cgroup.procs" not in filenames:
            continue
        for word in Path(directory, "cgroup.procs").read_text(encoding="ascii").split():
            if not word.isdecimal() or int(word) <= 0:
                message = "cgroup.procs contained a malformed PID"
                raise IdleStopError(message)
            pids.add(int(word))
    return pids


def _is_worker(proc_root: Path, pid: int) -> bool:
    """Recognize the exact Actions job process from authenticated proc data."""
    process = proc_root / str(pid)
    try:
        executable = (process / "exe").readlink().name
        comm = (process / "comm").read_text(encoding="utf-8").strip()
    except OSError as exc:
        message = f"cannot authenticate frozen cgroup PID {pid}: {exc}"
        raise IdleStopError(message) from exc
    return executable == "Runner.Worker" or comm == "Runner.Worker"


def _wait_inactive(control: Control, service: str, timeout_s: float) -> None:
    """Wait for systemd's already-enqueued stop to finish."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = control.command("show", "--property=ActiveState", "--value", service)
        if state in {"inactive", "failed"}:
            return
        time.sleep(0.1)
    message = f"{service} did not stop within {timeout_s:g}s"
    raise IdleStopError(message)


def _wait_stop_committed(control: Control, service: str, timeout_s: float) -> None:
    """Keep the cgroup frozen until systemd commits the stop transition."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = control.command("show", "--property=ActiveState", "--value", service)
        if state in {"deactivating", "inactive", "failed"}:
            return
        if state != "active":
            message = f"{service} entered unsafe stop state {state!r}"
            raise IdleStopError(message)
        time.sleep(0.1)
    message = f"{service} stop was not committed within {timeout_s:g}s"
    raise IdleStopError(message)


def idle_stop(
    service: str,
    control: Control,
    cgroup_root: Path = Path("/sys/fs/cgroup"),
    proc_root: Path = Path("/proc"),
    stop_commit_timeout_s: float = 30,
) -> bool:
    """Freeze, prove idle, enqueue stop, thaw, and wait inactive."""
    if SERVICE_RE.fullmatch(service) is None:
        message = "service name is not canonical"
        raise IdleStopError(message)
    load_state = control.command("show", "--property=LoadState", "--value", service)
    if load_state == "not-found":
        return False
    if load_state != "loaded":
        message = f"{service} has unsafe load state {load_state!r}"
        raise IdleStopError(message)
    state = control.command("show", "--property=ActiveState", "--value", service)
    if state in {"inactive", "failed"}:
        return False
    if state != "active":
        message = f"{service} has unsafe transitional state {state!r}"
        raise IdleStopError(message)
    freeze_may_have_landed = False
    try:
        freeze_may_have_landed = True
        control.command("freeze", service)
        freezer = control.command("show", "--property=FreezerState", "--value", service)
        if freezer != "frozen":
            message = f"{service} did not enter the frozen state"
            raise IdleStopError(message)
        group = control.command("show", "--property=ControlGroup", "--value", service)
        if any(
            _is_worker(proc_root, pid) for pid in _cgroup_pids(_cgroup_path(cgroup_root, group))
        ):
            message = "Runner.Worker is active; refusing to stop the listener"
            raise IdleStopError(message)
        control.command("stop", "--no-block", service)
        _wait_stop_committed(control, service, stop_commit_timeout_s)
        control.command("thaw", service, accept=(0, 1))
        freeze_may_have_landed = False
        _wait_inactive(control, service, 30)
        return True
    finally:
        if freeze_may_have_landed:
            control.command("thaw", service, accept=(0, 1))


class _FakeControl:
    """Small deterministic systemd model for the offline selftest."""

    def __init__(
        self,
        active: str = "active",
        load_state: str = "loaded",
        load_error: bool = False,
        freeze_error: bool = False,
        stop_commit_reads: int | None = 0,
    ) -> None:
        self.active = active
        self.load_state = load_state
        self.load_error = load_error
        self.freeze_error = freeze_error
        self.stop_commit_reads = stop_commit_reads
        self.stop_pending = False
        self.early_thaw = False
        self.frozen = False
        self.calls: list[tuple[str, ...]] = []

    def _show(self, args: tuple[str, ...]) -> str | None:
        """Model the exact systemd properties consumed by idle_stop."""
        if len(args) > 1 and args[1] == "--property=LoadState":
            if self.load_error:
                message = "systemctl load-state transport failed"
                raise IdleStopError(message)
            return self.load_state
        if len(args) <= 1:
            return None
        if args[1] == "--property=FreezerState":
            return "frozen" if self.frozen else "running"
        if args[1] == "--property=ControlGroup":
            return "/system.slice/ra8-hil-runner.service"
        if (
            args[1] == "--property=ActiveState"
            and self.stop_pending
            and self.stop_commit_reads is not None
            and self.stop_commit_reads > 0
        ):
            self.stop_commit_reads -= 1
            if self.stop_commit_reads == 0:
                self.active = "deactivating"
        return self.active

    def _change(self, args: tuple[str, ...]) -> str:
        """Model the freeze, stop, and thaw state transitions."""
        if args[0] == "freeze":
            self.frozen = True
            if self.freeze_error:
                raise KeyboardInterrupt
        elif args[0] == "thaw":
            if self.stop_pending and self.active == "active":
                self.early_thaw = True
            self.frozen = False
            if self.stop_pending and self.active == "deactivating":
                self.active = "inactive"
        elif args[0] == "stop":
            if not self.frozen:
                message = "stop was not enqueued while frozen"
                raise IdleStopError(message)
            self.stop_pending = True
            if self.stop_commit_reads == 0:
                self.active = "deactivating"
        return self.active

    def command(self, *args: str, accept: tuple[int, ...] = (0,)) -> str:
        """Model only the exact operations idle_stop owns."""
        del accept
        self.calls.append(args)
        shown = self._show(args) if args[0] == "show" else None
        return shown if shown is not None else self._change(args)


def _fixture(*, worker: bool) -> tuple[tempfile.TemporaryDirectory[str], Path, Path]:
    """Create one synthetic cgroup/proc tree."""
    temp = tempfile.TemporaryDirectory()
    root = Path(temp.name)
    group = root / "cgroup/system.slice/ra8-hil-runner.service"
    process = root / "proc/41"
    group.mkdir(parents=True)
    process.mkdir(parents=True)
    (group / "cgroup.procs").write_text("41\n", encoding="ascii")
    name = "Runner.Worker" if worker else "Runner.Listener"
    (process / "comm").write_text(f"{name}\n", encoding="utf-8")
    (process / "exe").symlink_to(f"/opt/runner/bin/{name}")
    return temp, root / "cgroup", root / "proc"


def _selftest_idle_transaction(failures: list[str]) -> None:
    """Prove the active-idle and active-busy transaction directions."""
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl()
        if not idle_stop("ra8-hil-runner.service", control, cgroup, proc):
            failures.append("idle active listener was not stopped")
        stop_at = control.calls.index(("stop", "--no-block", "ra8-hil-runner.service"))
        freeze_at = control.calls.index(("freeze", "ra8-hil-runner.service"))
        if stop_at < freeze_at:
            failures.append("stop preceded the cgroup freeze")
        thaw_at = control.calls.index(("thaw", "ra8-hil-runner.service"))
        committed_at = control.calls.index(
            ("show", "--property=ActiveState", "--value", "ra8-hil-runner.service"),
            stop_at + 1,
        )
        if thaw_at < committed_at or control.early_thaw:
            failures.append("listener thawed before the stop transition was committed")
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl(stop_commit_reads=2)
        if not idle_stop("ra8-hil-runner.service", control, cgroup, proc):
            failures.append("delayed asynchronous stop did not complete")
        if control.early_thaw:
            failures.append("delayed asynchronous stop thawed while still active")
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl(stop_commit_reads=None)
        try:
            idle_stop(
                "ra8-hil-runner.service",
                control,
                cgroup,
                proc,
                stop_commit_timeout_s=0.01,
            )
        except IdleStopError as exc:
            if "not committed" not in str(exc):
                failures.append("uncommitted stop failure lost attribution")
        else:
            failures.append("uncommitted asynchronous stop was accepted")
    temp, cgroup, proc = _fixture(worker=True)
    with temp:
        control = _FakeControl()
        try:
            idle_stop("ra8-hil-runner.service", control, cgroup, proc)
        except IdleStopError as exc:
            if "Runner.Worker" not in str(exc) or control.frozen:
                failures.append("busy refusal did not thaw with an attributed error")
        else:
            failures.append("busy Runner.Worker was stopped")


def _selftest_unit_states(failures: list[str]) -> None:
    """Prove absent is safe while unreadable or unsafe load states fail."""
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl("inactive")
        if idle_stop("ra8-hil-runner.service", control, cgroup, proc):
            failures.append("inactive listener was reported changed")
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl(load_state="not-found")
        if idle_stop("ra8-hil-runner.service", control, cgroup, proc):
            failures.append("absent unit was reported changed")
        if any(call[0] in {"freeze", "stop"} for call in control.calls):
            failures.append("absent unit reached the freeze/stop transaction")
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        for control in (_FakeControl(load_state="bad-setting"), _FakeControl(load_error=True)):
            try:
                idle_stop("ra8-hil-runner.service", control, cgroup, proc)
            except IdleStopError:
                continue
            failures.append("unsafe or unreadable load state was accepted as absent")


def _selftest_proc_identity(failures: list[str]) -> None:
    """Prove a vanished frozen PID identity cannot become an idle verdict."""
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        (proc / "41/comm").unlink()
        try:
            idle_stop("ra8-hil-runner.service", _FakeControl(), cgroup, proc)
        except IdleStopError as exc:
            if "cannot authenticate" not in str(exc):
                failures.append("vanished proc identity failure lost attribution")
        else:
            failures.append("vanished proc identity was accepted as idle")


def _selftest_freeze_transition(failures: list[str]) -> None:
    """Prove cleanup is armed before a freeze command can raise or interrupt."""
    temp, cgroup, proc = _fixture(worker=False)
    with temp:
        control = _FakeControl(freeze_error=True)
        try:
            idle_stop("ra8-hil-runner.service", control, cgroup, proc)
        except KeyboardInterrupt:
            pass
        else:
            failures.append("freeze-transition interrupt was not propagated")
        if control.frozen or ("thaw", "ra8-hil-runner.service") not in control.calls:
            failures.append("freeze-transition interrupt stranded the listener frozen")


def run_selftest() -> int:
    """Prove idle, busy, absent, failure, and freeze-order behavior offline."""
    failures: list[str] = []
    _selftest_idle_transaction(failures)
    _selftest_unit_states(failures)
    _selftest_proc_identity(failures)
    _selftest_freeze_transition(failures)
    for failure in failures:
        print(f"ra8-hil-runner-idle-stop.py --selftest: FAIL: {failure}")
    if failures:
        return 1
    print("ra8-hil-runner-idle-stop.py --selftest: PASS")
    return 0


def _interrupt(_signum: int, _frame: object) -> None:
    """Turn SIGTERM into unwinding so idle_stop's thaw finally clause runs."""
    raise KeyboardInterrupt


def main() -> int:
    """Parse the fixed service identity and execute the transaction."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("service")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()
    signal.signal(signal.SIGTERM, _interrupt)
    signal.signal(signal.SIGHUP, _interrupt)
    changed = idle_stop(args.service, Systemctl())
    print("stopped" if changed else "already inactive")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
