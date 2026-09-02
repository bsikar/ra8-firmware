# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Authenticated entry and descriptor regressions for the image supervisor."""

from __future__ import annotations

import errno
import os
import select
import signal
import stat
import subprocess
import sys
import time
from contextlib import suppress
from pathlib import Path

import __main__

HIDDEN_ARG_COUNT = 5
ROOT_ARG_COUNT = 4
CASES_LOAD_VERSION = 1
CLOSED_DESCRIPTOR = 99
PRIVATE_MODE = 0o700
SUITE_ROOT_SUFFIX_LENGTH = 32
STAT_SELFTEST_ARG_COUNT = 2
TEST_PROCESS_GROUP = 42
USAGE_STATUS = 64
CANONICAL_TMP = Path(
    "/tmp"  # noqa: S108 -- fixed physical parent; random mode-0700 inode-bound direct child
)
if globals().get("_RA8_SUPERVISOR_CASES_VERSION") != CASES_LOAD_VERSION:
    message = "supervisor cases module is source-only"
    raise RuntimeError(message)
MAIN_API = vars(__main__)
DEADLINE_SECONDS = MAIN_API["DEADLINE_SECONDS"]
ENTRY_MAX_BYTES = MAIN_API["ENTRY_MAX_BYTES"]
HARDLINK_COUNT = MAIN_API["HARDLINK_COUNT"]
INTEGRITY_REFUSAL_STATUS = MAIN_API["INTEGRITY_REFUSAL_STATUS"]
POLL_SECONDS = MAIN_API["POLL_SECONDS"]
PUBLIC_REFUSAL_STATUS = MAIN_API["PUBLIC_REFUSAL_STATUS"]
RECEIPT_MODE = MAIN_API["RECEIPT_MODE"]
RECEIPT_MAX_BYTES = MAIN_API["RECEIPT_MAX_BYTES"]
SELFTEST_WATCHDOG_TIMEOUT_SECONDS = MAIN_API["SELFTEST_WATCHDOG_TIMEOUT_SECONDS"]
STALL_STATUS = MAIN_API["STALL_STATUS"]
BoundGroup = MAIN_API["BoundGroup"]
ControllerLaunch = MAIN_API["ControllerLaunch"]
ProcessIdentity = MAIN_API["ProcessIdentity"]
SupervisionControls = MAIN_API["SupervisionControls"]
SupervisorRequest = MAIN_API["SupervisorRequest"]
_bind_process = MAIN_API["_bind_process"]
_close_suite_root_authority = MAIN_API["_close_suite_root_authority"]
_controller = MAIN_API["_controller"]
_group_members = MAIN_API["_group_members"]
_stat_group = MAIN_API["_stat_group"]
_wait_group_populated = MAIN_API["_wait_group_populated"]
_wait_group_gone = MAIN_API["_wait_group_gone"]
_census_cleanup_retry_selftest = MAIN_API["_census_cleanup_retry_selftest"]
_open_entry_authority = MAIN_API["_open_entry_authority"]
_open_suite_root_authority = MAIN_API["_open_suite_root_authority"]
_supervise = MAIN_API["_supervise"]
_write_exclusive = MAIN_API["_write_exclusive"]
_write_exact = MAIN_API["_write_exact"]
_anchored_root_path = MAIN_API["_anchored_root_path"]
SUPERVISOR_PROGRAM = __main__.__file__


def _bind_group_leader(pid: int) -> ProcessIdentity | None:
    """Bind one live session/group leader before an emergency fallback."""
    authority = _bind_process(pid)
    if authority is None or authority.group != pid or authority.session != pid:
        return None
    return authority


def _wait_direct_child_status(child: int) -> int | None:
    """Wait boundedly and reap one exact direct child without releasing its PID early."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while time.monotonic() < deadline:
        result = os.waitid(os.P_PID, child, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if result is not None:
            _, raw_status = os.waitpid(child, 0)
            return os.waitstatus_to_exitcode(raw_status)
        time.sleep(POLL_SECONDS)
    return None


def _identity_is_current(authority: ProcessIdentity) -> bool:
    """Revalidate one unreaped leader immediately before emergency cleanup."""
    return _bind_process(authority.pid) == authority


def _emergency_group_cleanup(authority: ProcessIdentity) -> bool:
    """Remove a failed watchdog group only while its exact leader stays bound."""
    if _wait_group_gone(authority.group):
        return True
    if not _identity_is_current(authority):
        return False
    with suppress(ProcessLookupError):
        os.killpg(authority.group, signal.SIGKILL)
    return _wait_group_gone(authority.group)


def _receive_controller_identity(descriptor: int) -> ProcessIdentity | None:
    """Bind the controller named by one private runner pipe."""
    ready, _, _ = select.select((descriptor,), (), (), 2)
    if not ready:
        return None
    value = os.read(descriptor, 32).strip()
    if not value.isdigit():
        return None
    return _bind_group_leader(int(value))


def _inject_observation_failure() -> None:
    """Raise the controlled observation error used by the cleanup direction."""
    message = "injected controller observation failure"
    raise OSError(message)


def _suite_root_is_safe(root: Path, expected_identity: str) -> bool:
    """Bind one direct canonical-/tmp suite root before any case side effect."""
    try:
        canonical = CANONICAL_TMP.resolve(strict=True)
        metadata = root.lstat()
        suffix = root.name.removeprefix("ra8-devcontainer-image-selftest.")
        identity = f"{metadata.st_dev}:{metadata.st_ino}"
        resolved = root.resolve(strict=True)
    except OSError:
        return False
    return (
        root.is_absolute()
        and root.parent == canonical
        and resolved == root
        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
        and identity == expected_identity
    )


def _entry_belongs_to_root(entry: Path, root: Path) -> bool:
    """Require one private mode-0700 regular payload directly under its suite."""
    try:
        metadata = entry.lstat()
    except OSError:
        return False
    return (
        entry.is_absolute()
        and entry.parent == root
        and stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
    )


def _bound_phase_matches(bound: Path, authority: ProcessIdentity, phase: str) -> bool:
    """Prove the requested publication boundary before killing the runner."""
    deadline = time.monotonic() + 2
    expected = b"" if phase == "pre-bound" else f"{authority.pid}\n".encode("ascii")
    while time.monotonic() < deadline:
        try:
            value = bound.read_bytes()
        except OSError:
            return False
        if value == expected:
            return True
        if phase == "pre-bound" or value:
            return False
        time.sleep(POLL_SECONDS)
    return False


def _supervisor_sigkill_case(entry: str, root: Path, phase: str) -> bool:
    """Prove pipe EOF removes a controller group after supervisor SIGKILL."""
    bound = root / f"supervisor-sigkill-{phase}.bound"
    outer = root / f"supervisor-sigkill-{phase}.outer"
    status = root / f"supervisor-sigkill-{phase}.status"
    descriptor = os.open(
        bound,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        RECEIPT_MODE,
    )
    os.close(descriptor)
    read_descriptor, write_descriptor = os.pipe2(os.O_CLOEXEC)
    runner = os.fork()
    if runner == 0:
        os.close(read_descriptor)
        request = SupervisorRequest(entry, bound, outer, status, "normal")
        controls = SupervisionControls(
            test_descriptor=write_descriptor,
            pause_before_bound=phase == "pre-bound",
        )
        os._exit(_supervise(request, controls))
    os.close(write_descriptor)
    authority: ProcessIdentity | None = None
    runner_reaped = False
    watchdog_succeeded = False
    try:
        authority = _receive_controller_identity(read_descriptor)
        if authority is not None and _bound_phase_matches(bound, authority, phase):
            os.kill(runner, signal.SIGKILL)
            os.waitpid(runner, 0)
            runner_reaped = True
            watchdog_succeeded = _wait_group_gone(authority.group)
    finally:
        os.close(read_descriptor)
        if not runner_reaped:
            with suppress(ProcessLookupError):
                os.kill(runner, signal.SIGKILL)
            with suppress(ChildProcessError):
                os.waitpid(runner, 0)
        if authority is not None and not watchdog_succeeded:
            _emergency_group_cleanup(authority)
    return watchdog_succeeded


def _supervisor_sigkill_selftest(entry: str, root: Path) -> int:
    """Exercise the parent-death watchdog before and after public binding."""
    phases = ("pre-bound", "post-bound")
    return 0 if all(_supervisor_sigkill_case(entry, root, phase) for phase in phases) else 1


def _hardlink_bound_selftest(entry: str, root: Path) -> int:
    """Prove hardlink refusal preserves data and leaves no controller group."""
    victim = root / "supervisor-hardlink-bound.victim"
    bound = root / "supervisor-hardlink-bound.bound"
    outer = root / "supervisor-hardlink-bound.outer"
    status = root / "supervisor-hardlink-bound.status"
    _write_exclusive(victim, "preserve\n")
    os.link(victim, bound, follow_symlinks=False)
    read_descriptor, write_descriptor = os.pipe2(os.O_CLOEXEC)
    runner = os.fork()
    if runner == 0:
        os.close(read_descriptor)
        request = SupervisorRequest(entry, bound, outer, status, "normal")
        controls = SupervisionControls(test_descriptor=write_descriptor)
        os._exit(_supervise(request, controls))
    os.close(write_descriptor)
    authority: ProcessIdentity | None = None
    group_absent = False
    runner_status: int | None = None
    hardlink_runner_reaped = False
    try:
        authority = _receive_controller_identity(read_descriptor)
        if authority is not None:
            group_absent = _wait_group_gone(authority.group)
        runner_status = _wait_direct_child_status(runner)
        hardlink_runner_reaped = runner_status is not None
    finally:
        os.close(read_descriptor)
        if not hardlink_runner_reaped:
            with suppress(ProcessLookupError):
                os.kill(runner, signal.SIGKILL)
            with suppress(ChildProcessError):
                os.waitpid(runner, 0)
        if authority is not None and not group_absent:
            _emergency_group_cleanup(authority)
    preserved = victim.read_bytes() == b"preserve\n" and victim.stat().st_nlink == HARDLINK_COUNT
    return 0 if group_absent and runner_status == PUBLIC_REFUSAL_STATUS and preserved else 1


def _watchdog_expiry_runner(
    entry: str,
    status: Path,
    identity_descriptor: int,
    proof_descriptor: int,
    release_descriptor: int,
) -> int:
    """Hold supervisor ownership until the observing parent releases this runner."""
    supervisor = BoundGroup()
    source_descriptor = int(Path(SUPERVISOR_PROGRAM).name)
    try:
        if not supervisor.enable_subreaper():
            return INTEGRITY_REFUSAL_STATUS
        launch = ControllerLaunch(entry, status, SELFTEST_WATCHDOG_TIMEOUT_SECONDS)
        supervisor.spawn(source_descriptor, launch)
        if supervisor.pid is None:
            return INTEGRITY_REFUSAL_STATUS
        _write_exact(
            identity_descriptor,
            f"{supervisor.pid}\n".encode("ascii"),
            RECEIPT_MAX_BYTES,
        )
        deadline = time.monotonic() + DEADLINE_SECONDS
        killed = False
        while time.monotonic() < deadline:
            result = os.waitid(os.P_PID, supervisor.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if result is not None:
                killed = result.si_code == os.CLD_KILLED and result.si_status == signal.SIGKILL
                break
            time.sleep(POLL_SECONDS)
        contained = supervisor.contain()
        _write_exact(
            proof_descriptor,
            b"K\n" if killed and contained else b"F\n",
            RECEIPT_MAX_BYTES,
        )
        ready, _, _ = select.select((release_descriptor,), (), (), DEADLINE_SECONDS * 2)
        return STALL_STATUS if ready else INTEGRITY_REFUSAL_STATUS
    finally:
        os.close(identity_descriptor)
        os.close(proof_descriptor)
        os.close(release_descriptor)
        supervisor.contain()


def _watchdog_expiry_selftest(entry: str, root: Path) -> int:
    """Prove a live but stalled supervisor cannot retain its controller group."""
    status = root / "supervisor-watchdog-expiry.status"
    read_descriptor, write_descriptor = os.pipe2(os.O_CLOEXEC)
    proof_read, proof_write = os.pipe2(os.O_CLOEXEC)
    release_read, release_write = os.pipe2(os.O_CLOEXEC)
    runner = os.fork()
    if runner == 0:
        os.close(read_descriptor)
        os.close(proof_read)
        os.close(release_write)
        result = _watchdog_expiry_runner(entry, status, write_descriptor, proof_write, release_read)
        os._exit(result)
    os.close(write_descriptor)
    os.close(proof_write)
    os.close(release_read)
    authority: ProcessIdentity | None = None
    watchdog_succeeded = False
    runner_reaped = False
    try:
        authority = _receive_controller_identity(read_descriptor)
        if authority is not None:
            ready, _, _ = select.select((proof_read,), (), (), DEADLINE_SECONDS)
            killed_receipt = ready and os.read(proof_read, 2) == b"K\n"
            members = _group_members(authority.group)
            runner_is_live = (
                os.waitid(os.P_PID, runner, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None
            )
            pre_release_proven = killed_receipt and members is not None
            pre_release_proven = pre_release_proven and members <= {authority.pid}
            if pre_release_proven and runner_is_live:
                os.close(release_write)
                release_write = -1
                runner_status = _wait_direct_child_status(runner)
                runner_reaped = runner_status is not None
                expected = runner_status == STALL_STATUS
                watchdog_succeeded = expected and _wait_group_gone(authority.group)
    finally:
        os.close(read_descriptor)
        os.close(proof_read)
        if release_write >= 0:
            os.close(release_write)
        if not runner_reaped:
            with suppress(ProcessLookupError):
                os.kill(runner, signal.SIGKILL)
            with suppress(ChildProcessError):
                os.waitpid(runner, 0)
        if authority is not None and not watchdog_succeeded:
            _emergency_group_cleanup(authority)
    return 0 if watchdog_succeeded else 1


def _missing_entry_selftest(root: Path) -> int:
    """Prove payload exec failure is published and its group is removed."""
    bound = root / "supervisor-missing-entry.bound"
    outer = root / "supervisor-missing-entry.outer"
    status = root / "supervisor-missing-entry.status"
    descriptor = os.open(bound, os.O_WRONLY | os.O_CREAT | os.O_EXCL, RECEIPT_MODE)
    os.close(descriptor)
    request = SupervisorRequest(str(root / "absent-entry"), bound, outer, status, "normal")
    controls = SupervisionControls(missing_entry_selftest=True)
    result = _supervise(request, controls)
    bound_value = bound.read_bytes().strip()
    if not bound_value.isdigit():
        return 1
    group = int(bound_value)
    exec_failure_proven = result == PUBLIC_REFUSAL_STATUS and status.read_bytes() == b"127\n"
    return 0 if exec_failure_proven and _wait_group_gone(group) else 1


def _write_entry_fixture(path: Path, body: str) -> None:
    """Publish one private executable used only by entry-authority tests."""
    _write_exclusive(path, body)
    path.chmod(0o700)


def _entry_mutation_case(root: Path, mode: str) -> bool:
    """Prove replacement uses the opened inode and in-place change is detected."""
    entry = root / f"entry-{mode}.sh"
    saved = root / f"entry-{mode}.saved"
    marker = root / f"entry-{mode}.forged"
    bound = root / f"entry-{mode}.bound"
    outer = root / f"entry-{mode}.outer"
    status = root / f"entry-{mode}.status"
    _write_entry_fixture(entry, "#!/bin/bash\nexit 1\n")
    _write_exclusive(bound, "")
    marker_path = marker.resolve(strict=False)
    forged = f"#!/bin/bash\nprintf forged >{marker_path}\nexit 0\n"

    def mutate() -> None:
        if mode in ("replace", "pre-open-replace"):
            entry.rename(saved)
            _write_entry_fixture(entry, forged)
        elif mode == "grow-in-place":
            os.truncate(entry, ENTRY_MAX_BYTES + 1)
        else:
            entry.write_text(forged, encoding="ascii")
            entry.chmod(0o700)

    request = SupervisorRequest(str(entry), bound, outer, status, "normal")
    controls = (
        SupervisionControls(pre_open_mutator=mutate)
        if mode == "pre-open-replace"
        else SupervisionControls(entry_mutator=mutate)
    )
    result = _supervise(request, controls)
    if mode == "pre-open-replace":
        return result == PUBLIC_REFUSAL_STATUS and not marker.exists()
    if mode in ("replace", "grow-in-place"):
        return result == INTEGRITY_REFUSAL_STATUS and not marker.exists()
    return result == INTEGRITY_REFUSAL_STATUS and marker.read_bytes() == b"forged"


def _entry_refusal_case(root: Path, mode: str) -> bool:
    """Prove unsafe path and metadata shapes fail before controller spawn."""
    entry = root / f"entry-refusal-{mode}.sh"
    target = root / f"entry-refusal-{mode}.target"
    bound = root / f"entry-refusal-{mode}.bound"
    _write_entry_fixture(target, "#!/bin/bash\nexit 1\n")
    if mode == "symlink":
        entry.symlink_to(target.name)
    elif mode == "hardlink":
        os.link(target, entry, follow_symlinks=False)
    else:
        target.rename(entry)
        if mode == "mode":
            entry.chmod(0o600)
        elif mode == "owner":
            os.chown(entry, 1, os.getgid())
        elif mode == "group":
            os.chown(entry, os.getuid(), 1)
        elif mode == "zero":
            entry.write_bytes(b"")
        elif mode == "oversize":
            os.truncate(entry, ENTRY_MAX_BYTES + 1)
    _write_exclusive(bound, "")
    request = SupervisorRequest(
        str(entry),
        bound,
        root / f"entry-refusal-{mode}.outer",
        root / f"entry-refusal-{mode}.status",
        "normal",
    )
    return _supervise(request) == PUBLIC_REFUSAL_STATUS and bound.read_bytes() == b""


def _root_argument_directions(root: Path, identity: str) -> bool:
    """Exercise safe root refusal directions without changing root metadata."""
    return (
        _suite_root_is_safe(root, identity)
        and not _suite_root_is_safe(Path("/"), identity)
        and not _suite_root_is_safe(Path(root.name), identity)
        and not _suite_root_is_safe(root, "0:0")
    )


def _entry_binding_selftest(root: Path, original_root: Path, identity: str) -> int:
    """Exercise both live entry mutations and every rejected metadata shape."""
    mutation_modes = ("pre-open-replace", "replace", "in-place", "grow-in-place")
    mutations = all(_entry_mutation_case(root, mode) for mode in mutation_modes)
    refusal_modes = ["symlink", "hardlink", "mode", "zero", "oversize"]
    if os.getuid() == 0:
        refusal_modes.extend(("owner", "group"))
    refusals = all(_entry_refusal_case(root, mode) for mode in refusal_modes)
    roots = _root_argument_directions(original_root, identity)
    return 0 if roots and mutations and refusals else 1


def _controller_kill_observed(child: subprocess.Popen[bytes]) -> bool:
    """Observe one controller terminal while its leader remains unreaped."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while time.monotonic() < deadline:
        result = os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if result is not None:
            return result.si_code == os.CLD_KILLED and result.si_status == signal.SIGKILL
        time.sleep(POLL_SECONDS)
    return False


def _wait_forked_group_terminal(process: int) -> bool:
    """Retain one forked group leader unreaped through its absence proof."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while time.monotonic() < deadline:
        result = os.waitid(os.P_PID, process, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if result is not None:
            members = _group_members(process)
            return members is not None and members <= {process}
        time.sleep(POLL_SECONDS)
    return False


def _controller_isolation_selftest(root: Path, identity: str) -> int:
    """Prove a raw non-session-leader controller refuses before any effect."""
    status = root / "supervisor-controller-isolation.status"
    identity_read, identity_write = os.pipe2(os.O_CLOEXEC)
    result_read, result_write = os.pipe2(os.O_CLOEXEC)
    broker = os.fork()
    if broker == 0:
        os.close(identity_read)
        os.close(result_read)
        os.setsid()
        _write_exact(
            identity_write,
            f"{os.getpid()}\n".encode("ascii"),
            RECEIPT_MAX_BYTES,
        )
        contender = os.fork()
        if contender == 0:
            root_authority = (int(root.name), identity)
            os._exit(_controller("99", status, CLOSED_DESCRIPTOR, root_authority, 1.0))
        _, raw_status = os.waitpid(contender, 0)
        _write_exact(
            result_write,
            f"{os.waitstatus_to_exitcode(raw_status)}\n".encode("ascii"),
            RECEIPT_MAX_BYTES,
        )
        os._exit(0)
    os.close(identity_write)
    os.close(result_write)
    authority: ProcessIdentity | None = None
    broker_reaped = False
    terminal = False
    refusal = b""
    try:
        authority = _receive_controller_identity(identity_read)
        ready, _, _ = select.select((result_read,), (), (), DEADLINE_SECONDS)
        if ready:
            refusal = os.read(result_read, 16).strip()
        terminal = _wait_forked_group_terminal(broker)
        if terminal:
            os.waitpid(broker, 0)
            broker_reaped = True
        safe = (
            authority is not None
            and refusal == b"64"
            and terminal
            and broker_reaped
            and not status.exists()
        )
        return 0 if safe else 1
    finally:
        os.close(identity_read)
        os.close(result_read)
        if not broker_reaped:
            if authority is not None and _identity_is_current(authority):
                with suppress(ProcessLookupError):
                    os.killpg(authority.group, signal.SIGKILL)
                _wait_forked_group_terminal(broker)
            with suppress(ChildProcessError):
                os.waitpid(broker, 0)


def _refused_controller_launch(root_descriptor: int, identity: str, status: Path) -> bool:
    """Run one isolated raw controller expected to refuse before forking."""
    source_descriptor = int(Path(SUPERVISOR_PROGRAM).name)
    child = subprocess.Popen(  # noqa: S603 -- pinned interpreter and supervisor FD
        (
            sys.executable,
            "-B",
            "-I",
            "-S",
            SUPERVISOR_PROGRAM,
            "--controller",
            "99",
            str(status),
            str(CLOSED_DESCRIPTOR),
            str(root_descriptor),
            identity,
            str(SELFTEST_WATCHDOG_TIMEOUT_SECONDS),
        ),
        pass_fds=(source_descriptor, root_descriptor),
        start_new_session=True,
    )
    try:
        result = child.wait(timeout=DEADLINE_SECONDS)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=DEADLINE_SECONDS)
        return False
    return result == USAGE_STATUS and not status.exists()


def _controller_root_refusal_selftest(root: Path) -> bool:
    """Refuse wrong identity and non-suite directory FDs before effects."""
    root_descriptor = int(root.name)
    wrong = _refused_controller_launch(
        root_descriptor, "0:0", root / "controller-wrong-identity.status"
    )
    sibling = root / "controller-sibling"
    sibling.mkdir(mode=PRIVATE_MODE)
    descriptor = os.open(sibling, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(descriptor)
        sibling_identity = f"{metadata.st_dev}:{metadata.st_ino}"
        sibling_root = _anchored_root_path(descriptor)
        refused = _refused_controller_launch(
            descriptor, sibling_identity, sibling_root / "controller-sibling.status"
        )
    finally:
        os.close(descriptor)
        sibling.rmdir()
    return wrong and refused


def _reap_cleanup_retry_selftest(entry: str, root: Path) -> bool:
    """Retain a terminal leader when its first exact reap fails, then retry."""
    supervisor = BoundGroup()
    original_reap = vars(BoundGroup)["_reap"]
    retained = False
    cleaned = False

    def fail_reap(_authority: BoundGroup) -> bool:
        return False

    try:
        if not supervisor.enable_subreaper():
            return False
        launch = ControllerLaunch(
            entry, root / "supervisor-reap-retry.status", SELFTEST_WATCHDOG_TIMEOUT_SECONDS
        )
        supervisor.spawn(int(Path(SUPERVISOR_PROGRAM).name), launch)
        if supervisor.pid is not None and _wait_group_populated(supervisor.pid):
            type.__setattr__(BoundGroup, "_reap", fail_reap)
            first_cleanup = supervisor.cleanup()
            retained = (
                not first_cleanup
                and not supervisor.reaped
                and not supervisor.cleaning
                and supervisor.entry_descriptor is not None
            )
    finally:
        type.__setattr__(BoundGroup, "_reap", original_reap)
        cleaned = supervisor.contain()
    group_absent = supervisor.pid is None or _wait_group_gone(supervisor.pid)
    return retained and cleaned and supervisor.entry_descriptor is None and group_absent


def _cleanup_retry_selftest(entry: str, root: Path) -> int:
    """Exercise independent census and reap retry directions."""
    census = _census_cleanup_retry_selftest(entry, root)
    reaping = _reap_cleanup_retry_selftest(entry, root)
    writes = _receipt_write_selftest()
    permanent = _permanent_containment_failure_selftest()
    return 0 if census and reaping and writes and permanent else 1


def _closed_controller_command(
    entry_authority: str,
    status: Path,
    death_descriptor: int,
    root_descriptor: int,
    root_identity: str,
) -> tuple[str, ...]:
    """Build the fixed controller command used by closed-descriptor cases."""
    return (
        sys.executable,
        "-B",
        "-I",
        "-S",
        SUPERVISOR_PROGRAM,
        "--controller",
        entry_authority,
        str(status),
        str(death_descriptor),
        str(root_descriptor),
        root_identity,
        str(SELFTEST_WATCHDOG_TIMEOUT_SECONDS),
    )


def _closed_controller_descriptor_selftest(entry: str, root: Path, mode: str) -> int:
    """Prove invalid entry/death descriptors cannot bypass group cleanup."""
    status = root / f"supervisor-closed-{mode}.status"
    source_descriptor = int(Path(SUPERVISOR_PROGRAM).name)
    root_descriptor = int(root.name)
    root_metadata = os.fstat(root_descriptor)
    root_identity = f"{root_metadata.st_dev}:{root_metadata.st_ino}"
    inherited = [source_descriptor, root_descriptor]
    supervisor = BoundGroup()
    death_descriptor: int | None = CLOSED_DESCRIPTOR
    death_write: int | None = None
    entry_authority = "98"
    observation_injected = False
    try:
        if not supervisor.enable_subreaper():
            return 1
        if mode in ("death", "observation"):
            descriptor, identity, digest = _open_entry_authority(entry)
            supervisor.entry_descriptor = descriptor
            supervisor.entry_path = entry
            supervisor.entry_identity = identity
            supervisor.entry_digest = digest
            inherited.append(descriptor)
            entry_authority = str(descriptor)
        if mode != "death":
            death_descriptor, death_write = os.pipe2(os.O_CLOEXEC)
            inherited.append(death_descriptor)
        command = _closed_controller_command(
            entry_authority, status, death_descriptor, root_descriptor, root_identity
        )
        child = subprocess.Popen(  # noqa: S603 -- pinned interpreter and supervisor FD
            command,
            pass_fds=tuple(inherited),
            start_new_session=True,
        )
        supervisor.bind_spawned_child(child)
        if mode == "observation":
            observation_injected = True
            _inject_observation_failure()
        if death_write is not None:
            os.close(death_write)
            os.close(death_descriptor)
            death_write, death_descriptor = None, None
        observed = _controller_kill_observed(child)
        cleaned = supervisor.cleanup()
    except OSError:
        succeeded = observation_injected and supervisor.pid is not None
        return 0 if succeeded and supervisor.contain() else 1
    else:
        if mode == "observation":
            return 1
        return 0 if observed and cleaned and child.returncode == -signal.SIGKILL else 1
    finally:
        for descriptor in (death_write, death_descriptor):
            if descriptor is not None and descriptor != CLOSED_DESCRIPTOR:
                with suppress(OSError):
                    os.close(descriptor)
        supervisor.contain()


def _validated_case_paths(
    argv: list[str], entry_modes: set[str], root_modes: set[str]
) -> tuple[Path | None, Path | None, int | None]:
    """Validate one case root and optional entry before any side effect."""
    mode = argv[1] if len(argv) > 1 else ""
    entry: Path | None = None
    root: Path | None = None
    error_status: int | None = None
    if mode in entry_modes:
        if len(argv) != HIDDEN_ARG_COUNT:
            error_status = 64
        else:
            entry, root = Path(argv[2]), Path(argv[3])
            if not _suite_root_is_safe(root, argv[4]) or not _entry_belongs_to_root(entry, root):
                error_status = 64
    elif mode in root_modes:
        if len(argv) != ROOT_ARG_COUNT:
            error_status = 64
        else:
            root = Path(argv[2])
            if not _suite_root_is_safe(root, argv[3]):
                error_status = 64
    return entry, root, error_status


def _run_supervisor_case(
    mode: str,
    entry: Path | None,
    root: Path,
    original_root: Path,
    root_identity: str,
) -> int | None:
    """Run one already validated supervisor regression."""
    if (
        mode
        in {
            "--selftest-parent-death",
            "--selftest-watchdog-expiry",
            "--selftest-hardlink-bound",
            "--selftest-closed-death-fd",
            "--selftest-closed-entry-fd",
            "--selftest-observation-failure",
            "--selftest-cleanup-retry",
        }
        and entry is None
    ):
        return 64
    status = None
    if mode == "--selftest-parent-death":
        status = _supervisor_sigkill_selftest(str(entry), root)
    elif mode == "--selftest-watchdog-expiry":
        status = _watchdog_expiry_selftest(str(entry), root)
    elif mode == "--selftest-hardlink-bound":
        status = _hardlink_bound_selftest(str(entry), root)
    elif mode == "--selftest-closed-death-fd":
        status = _closed_controller_descriptor_selftest(str(entry), root, "death")
    elif mode == "--selftest-closed-entry-fd":
        status = _closed_controller_descriptor_selftest(str(entry), root, "entry")
    elif mode == "--selftest-observation-failure":
        status = _closed_controller_descriptor_selftest(str(entry), root, "observation")
    elif mode == "--selftest-cleanup-retry":
        status = _cleanup_retry_selftest(str(entry), root)
    elif mode == "--selftest-missing-entry":
        status = _missing_entry_selftest(root)
    elif mode == "--selftest-entry-binding":
        status = _entry_binding_selftest(root, original_root, root_identity)
    elif mode == "--selftest-controller-isolation":
        isolated = _controller_isolation_selftest(root, root_identity) == 0
        root_refused = _controller_root_refusal_selftest(root)
        status = 0 if isolated and root_refused else 1
    return status


def _open_validated_root(root: Path, expected_identity: str) -> tuple[int, tuple[int, int]] | None:
    """Open the exact caller-bound root or refuse before case effects."""
    try:
        descriptor, opened_identity = _open_suite_root_authority(root)
    except (OSError, RuntimeError):
        return None
    if f"{opened_identity[0]}:{opened_identity[1]}" != expected_identity:
        _close_suite_root_authority(descriptor, root, opened_identity)
        return None
    return descriptor, opened_identity


def _nonroot_case_status(argv: list[str], mode: str) -> int | None:
    """Run the authenticated stat-parser case that requires no suite root."""
    if len(argv) != STAT_SELFTEST_ARG_COUNT or mode != "--selftest-stat-parser":
        return None
    raw = b"1 (non-ascii-\xff) S 0 42 42 " + (b"0 " * 15) + b"99"
    return 0 if _stat_group(raw) == TEST_PROCESS_GROUP else 1


def dispatch_supervisor_cases(argv: list[str]) -> int | None:
    """Dispatch one authenticated entry or descriptor regression."""
    entry_modes = {
        "--selftest-parent-death",
        "--selftest-watchdog-expiry",
        "--selftest-hardlink-bound",
        "--selftest-closed-death-fd",
        "--selftest-closed-entry-fd",
        "--selftest-observation-failure",
        "--selftest-cleanup-retry",
    }
    root_modes = {
        "--selftest-missing-entry",
        "--selftest-entry-binding",
        "--selftest-controller-isolation",
    }
    mode = argv[1] if len(argv) > 1 else ""
    entry, root, error_status = _validated_case_paths(argv, entry_modes, root_modes)
    if error_status is not None:
        return error_status
    if mode in entry_modes and (entry is None or root is None):
        return 64
    if mode in root_modes and root is None:
        return 64
    if root is None:
        return _nonroot_case_status(argv, mode)
    expected_identity = argv[4] if mode in entry_modes else argv[3]
    root_authority = _open_validated_root(root, expected_identity)
    if root_authority is None:
        return 64
    descriptor, opened_identity = root_authority
    anchored_root = _anchored_root_path(descriptor)
    anchored_entry = anchored_root / entry.name if entry is not None else None
    case_failed = False
    try:
        try:
            status = _run_supervisor_case(
                mode, anchored_entry, anchored_root, root, expected_identity
            )
        except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError):
            case_failed = True
            status = PUBLIC_REFUSAL_STATUS
    finally:
        root_integrity = _close_suite_root_authority(descriptor, root, opened_identity)
    if not root_integrity:
        status = INTEGRITY_REFUSAL_STATUS
    elif case_failed:
        status = PUBLIC_REFUSAL_STATUS
    return status


def _receipt_write_selftest() -> bool:
    """Prove exact receipt writes complete and reject impossible progress."""
    original_write = os.write

    def scripted(actions: tuple[str, ...], payload: bytes, expect_success: bool) -> bool:
        read_descriptor, write_descriptor = os.pipe2(os.O_CLOEXEC)
        action_index = 0

        def fake_write(descriptor: int, value: bytes) -> int:
            nonlocal action_index
            action = actions[min(action_index, len(actions) - 1)]
            action_index += 1
            if action == "eintr":
                raise InterruptedError(errno.EINTR, "injected interruption")
            if action == "zero":
                return 0
            if action == "partial":
                return original_write(descriptor, value[:1])
            return original_write(descriptor, value)

        os.write = fake_write
        succeeded = False
        try:
            try:
                _write_exact(write_descriptor, payload, RECEIPT_MAX_BYTES)
                succeeded = True
            except (OSError, ValueError):
                succeeded = False
            if succeeded != expect_success:
                return False
            if succeeded:
                return os.read(read_descriptor, len(payload)) == payload
            return True
        finally:
            os.write = original_write
            os.close(read_descriptor)
            os.close(write_descriptor)

    payload = b"1234\n"
    partial = scripted(("partial",), payload, expect_success=True)
    interrupted = scripted(("eintr", "partial", "eintr", "partial"), payload, expect_success=True)
    zero = scripted(("zero",), payload, expect_success=False)
    excess_eintr = scripted(("eintr",), payload, expect_success=False)
    return partial and interrupted and zero and excess_eintr


def _permanent_containment_observer(mode: str, terminate_early: bool = False) -> bool:
    """Prove contain remains fail-stopped under permanent census or reap failure."""
    process_globals = BoundGroup.cleanup.__globals__
    observer = os.fork()
    if observer == 0:
        if terminate_early:
            time.sleep(POLL_SECONDS * 10)
            os._exit(0)
        supervisor = BoundGroup()
        supervisor.children_contained = False
        if mode == "census":
            supervisor.subreaper = True
            process_globals["_direct_children"] = lambda: None
        else:
            supervisor.pid = 999999
            supervisor.child = object()
            supervisor.leader_terminal = True
            type.__setattr__(BoundGroup, "_reap", lambda _authority: False)
        supervisor.contain()
        os._exit(1)
    deadline = time.monotonic() + 0.25
    stayed_live = True
    try:
        while time.monotonic() < deadline:
            waited, _status = os.waitpid(observer, os.WNOHANG)
            if waited != 0:
                stayed_live = False
                break
            time.sleep(POLL_SECONDS)
    finally:
        if stayed_live:
            with suppress(ProcessLookupError):
                os.kill(observer, signal.SIGKILL)
        with suppress(ChildProcessError):
            os.waitpid(observer, 0)
    return stayed_live


def _permanent_containment_failure_selftest() -> bool:
    """Keep permanent cleanup uncertainty fail-stopped until external containment."""
    census = _permanent_containment_observer("census")
    reaping = _permanent_containment_observer("reap")
    early_terminal = not _permanent_containment_observer("census", terminate_early=True)
    return census and reaping and early_terminal
