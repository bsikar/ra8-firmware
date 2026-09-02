# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Supervise injected image-selftest failures without reusable PID authority."""

from __future__ import annotations

import errno
import hashlib
import os
import select
import signal
import stat
import sys
import time
import types
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn

MANAGED_SIGNALS = (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)
DEADLINE_SECONDS = 10.0
POLL_SECONDS = 0.01
CONTROLLER_ARG_COUNT = 8
SUPERVISOR_ARG_COUNT = 7
HIDDEN_SELFTEST_ARG_COUNT = 4
ROOT_SELFTEST_ARG_COUNT = 3
WATCHDOG_TIMEOUT_SECONDS = 30.0
SELFTEST_WATCHDOG_TIMEOUT_SECONDS = 1.0
RECEIPT_MODE = 0o600
RECEIPT_MAX_BYTES = 64
WRITE_ATTEMPT_MULTIPLIER = 2
STATUS_SIZE = 2
INJECTED_FAILURE_STATUS = 1
PUBLIC_REFUSAL_STATUS = 125
INTEGRITY_REFUSAL_STATUS = 126
HARDLINK_COUNT = 2
STALL_STATUS = 124
MIN_POPULATED_GROUP_MEMBERS = 2
ENTRY_MAX_BYTES = 1024 * 1024
ENTRY_READ_STEPS = 257
ENTRY_MODES = (0o700, 0o755)
CASES_MODE = 0o644
CASES_MAX_BYTES = 128 * 1024
CASES_READ_STEPS = 33
CASES_RAW_SHA256 = "897a5be60eec486f9f9615fead84db22f8526dba189df305f561bc1c7b5e49e7"
CASES_ARG = "--cases-fd"
PROCESS_MODE = 0o644
PROCESS_MAX_BYTES = 64 * 1024
PROCESS_READ_STEPS = 17
PROCESS_RAW_SHA256 = "0d6735a43532e39ebcda7d876ba8223062656a2fe944c00b611a6c85f3dd730c"
PROCESS_ARG = "--process-fd"
PRIVATE_MODE = 0o700
SUITE_ROOT_PREFIX = "ra8-devcontainer-image-selftest."
SUITE_ROOT_SUFFIX_LENGTH = 32
CANONICAL_TMP = Path(
    "/tmp"  # noqa: S108 -- fixed physical parent; random mode-0700 inode-bound direct child
)


@dataclass(frozen=True)
class SupervisorRequest:
    """Describe one immutable public supervision request."""

    entry: str
    bound: Path
    outer: Path
    status: Path
    mode: str


@dataclass(frozen=True)
class ControllerLaunch:
    """Bind one controller's entry, status, deadline, and test-only controls."""

    entry: str
    status: Path
    watchdog_timeout: float
    missing_entry_selftest: bool = False
    entry_mutator: Callable[[], None] | None = None
    pre_open_mutator: Callable[[], None] | None = None


@dataclass(frozen=True)
class SupervisionControls:
    """Describe optional private observations without widening the public CLI."""

    test_descriptor: int | None = None
    pause_before_bound: bool = False
    watchdog_timeout: float = WATCHDOG_TIMEOUT_SECONDS
    missing_entry_selftest: bool = False
    entry_mutator: Callable[[], None] | None = None
    pre_open_mutator: Callable[[], None] | None = None


def _refuse_entry(message: str) -> NoReturn:
    """Raise one uniform entry-authority refusal."""
    raise RuntimeError(message)


def _entry_digest(descriptor: int) -> str:
    """Hash one bounded entry descriptor without reopening its pathname."""
    digest = hashlib.sha256()
    total = 0
    for _step in range(ENTRY_READ_STEPS):
        chunk = os.pread(descriptor, 4096, total)
        if not chunk:
            break
        total += len(chunk)
        if total > ENTRY_MAX_BYTES:
            message = "entry exceeds its byte bound"
            _refuse_entry(message)
        digest.update(chunk)
    else:
        message = "entry read exceeded its step bound"
        _refuse_entry(message)
    return digest.hexdigest()


def _read_cases_source(descriptor: int) -> bytes:
    """Read one bounded authenticated cases module without using its pathname."""
    metadata = os.fstat(descriptor)
    safe = (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == CASES_MODE
        and 0 < metadata.st_size <= CASES_MAX_BYTES
    )
    if not safe:
        message = "supervisor cases metadata is unsafe"
        _refuse_entry(message)
    parts = []
    offset = 0
    for _step in range(CASES_READ_STEPS):
        chunk = os.pread(descriptor, 4096, offset)
        if not chunk:
            break
        parts.append(chunk)
        offset += len(chunk)
    source = b"".join(parts)
    if len(source) != metadata.st_size or len(source) > CASES_MAX_BYTES:
        message = "supervisor cases read is incomplete"
        _refuse_entry(message)
    return source


def _read_process_source(descriptor: int) -> bytes:
    """Read the bounded authenticated process module without its pathname."""
    metadata = os.fstat(descriptor)
    safe = (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PROCESS_MODE
        and 0 < metadata.st_size <= PROCESS_MAX_BYTES
    )
    if not safe:
        message = "supervisor process metadata is unsafe"
        _refuse_entry(message)
    parts = []
    offset = 0
    for _step in range(PROCESS_READ_STEPS):
        chunk = os.pread(descriptor, 4096, offset)
        if not chunk:
            break
        parts.append(chunk)
        offset += len(chunk)
    source = b"".join(parts)
    if len(source) != metadata.st_size or len(source) > PROCESS_MAX_BYTES:
        message = "supervisor process read is incomplete"
        _refuse_entry(message)
    return source


def _entry_metadata_is_safe(metadata: os.stat_result) -> bool:
    """Apply the complete reusable entry metadata predicate."""
    return (
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_nlink == 1
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) in ENTRY_MODES
        and 0 < metadata.st_size <= ENTRY_MAX_BYTES
    )


def _suite_root_metadata_is_safe(metadata: os.stat_result) -> bool:
    """Apply the complete private suite-root metadata predicate."""
    return (
        stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
    )


def _suite_root_path_is_safe(root: Path, metadata: os.stat_result) -> bool:
    """Require one direct canonical-/tmp random suite-root pathname."""
    try:
        canonical = CANONICAL_TMP.resolve(strict=True)
        resolved = root.resolve(strict=True)
    except OSError:
        return False
    suffix = root.name.removeprefix(SUITE_ROOT_PREFIX)
    return (
        root.is_absolute()
        and root.parent == canonical
        and resolved == root
        and len(suffix) == SUITE_ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and _suite_root_metadata_is_safe(metadata)
    )


def _open_suite_root_authority(root: Path) -> tuple[int, tuple[int, int]]:
    """Bind a suite root before any receipt path is derived from it."""
    before = root.lstat()
    if not _suite_root_path_is_safe(root, before):
        message = "suite root path is unsafe"
        _refuse_entry(message)
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        after = os.fstat(descriptor)
        identity = (after.st_dev, after.st_ino)
        if not _suite_root_metadata_is_safe(after) or identity != (
            before.st_dev,
            before.st_ino,
        ):
            message = "suite root identity changed while opening"
            _refuse_entry(message)
    except BaseException:
        os.close(descriptor)
        raise
    else:
        return descriptor, identity


def _close_suite_root_authority(descriptor: int, root: Path, identity: tuple[int, int]) -> bool:
    """Revalidate and close one retained root only after process cleanup."""
    safe = False
    try:
        opened = os.fstat(descriptor)
        current = root.lstat()
        safe = (
            _suite_root_metadata_is_safe(opened)
            and _suite_root_path_is_safe(root, current)
            and (opened.st_dev, opened.st_ino) == identity
            and (current.st_dev, current.st_ino) == identity
        )
    except OSError:
        safe = False
    finally:
        os.close(descriptor)
    return safe


def _anchored_root_path(descriptor: int) -> Path:
    """Name one retained root descriptor without reopening its pathname."""
    return Path(f"/proc/self/fd/{descriptor}")


def _anchored_root_descriptor(path: Path) -> int:
    """Recover the retained root descriptor from one anchored child path."""
    parent = str(path.parent)
    prefix = "/proc/self/fd/"
    suffix = parent.removeprefix(prefix)
    if parent == f"{prefix}{suffix}" and suffix.isdigit() and path.name not in ("", ".", ".."):
        return int(suffix)
    message = "receipt path is not rooted in a retained descriptor"
    _refuse_entry(message)


def _open_entry_authority(
    path: str, pre_open_mutator: Callable[[], None] | None = None
) -> tuple[int, tuple[int, int], str]:
    """Bind one trusted regular entry by metadata, inode, and initial digest."""
    before = os.lstat(path)
    if pre_open_mutator is not None:
        pre_open_mutator()
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        after = os.fstat(descriptor)
        identity = (after.st_dev, after.st_ino)
        safe = _entry_metadata_is_safe(before) and _entry_metadata_is_safe(after)
        safe = safe and (before.st_dev, before.st_ino) == identity
        if not safe:
            message = "entry metadata is unsafe"
            _refuse_entry(message)
        return descriptor, identity, _entry_digest(descriptor)
    except BaseException:
        os.close(descriptor)
        raise


def _write_exact(descriptor: int, payload: bytes, declared_max: int) -> None:
    """Write every payload byte with a finite progress and interruption bound."""
    if declared_max < 0 or len(payload) > declared_max:
        message = "receipt payload exceeds its declared bound"
        raise ValueError(message)
    if not payload:
        return
    attempts = 0
    max_attempts = max(1, min(len(payload), declared_max)) * WRITE_ATTEMPT_MULTIPLIER
    offset = 0
    while offset < len(payload):
        attempts += 1
        if attempts > max_attempts:
            raise OSError(errno.EIO, "receipt write made no bounded progress")
        try:
            accepted = os.write(descriptor, payload[offset:])
        except OSError as error:
            if error.errno == errno.EINTR:
                continue
            raise
        remaining = len(payload) - offset
        if accepted <= 0 or accepted > remaining:
            raise OSError(errno.EIO, "receipt write returned invalid progress")
        offset += accepted


def _write_exclusive(path: Path, value: str) -> None:
    """Write one non-link receipt without replacing an existing object."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        payload = value.encode("ascii")
        _write_exact(descriptor, payload, ENTRY_MAX_BYTES)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_bound_receipt(path: Path, value: str) -> None:
    """Write the child PID through one pre-created regular receipt."""
    descriptor = os.open(path, os.O_WRONLY | os.O_NOFOLLOW)
    try:
        metadata = os.fstat(descriptor)
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_nlink != 1
            or metadata.st_uid != os.getuid()
            or stat.S_IMODE(metadata.st_mode) != RECEIPT_MODE
        ):
            message = "bound receipt metadata is unsafe"
            raise RuntimeError(message)
        os.ftruncate(descriptor, 0)
        payload = value.encode("ascii")
        _write_exact(descriptor, payload, RECEIPT_MAX_BYTES)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_status_atomic(path: Path, value: str) -> None:
    """Publish a complete status without replacing a pre-existing final path."""
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    _write_exclusive(temporary, value)
    try:
        os.link(temporary, path, follow_symlinks=False)
    finally:
        temporary.unlink(missing_ok=True)


def _reset_managed_signals() -> None:
    """Give the nested Bash fresh dispositions after controller isolation."""
    for managed in MANAGED_SIGNALS:
        signal.signal(managed, signal.SIG_DFL)


def _spawn_payload(entry_authority: str, private_descriptors: tuple[int, ...]) -> int:
    """Fork the fixed Bash payload from a bound descriptor or explicit negative."""
    if entry_authority == "missing-entry-selftest":
        entry = "/proc/self/fd/2147483647"
    else:
        descriptor = int(entry_authority)
        metadata = os.fstat(descriptor)
        if not _entry_metadata_is_safe(metadata):
            message = "controller entry descriptor is unsafe"
            _refuse_entry(message)
        entry = f"/proc/self/fd/{descriptor}"
    child = os.fork()
    if child == 0:
        for private_descriptor in private_descriptors:
            with suppress(OSError):
                os.close(private_descriptor)
        _reset_managed_signals()
        try:
            os.execl(  # noqa: S606 -- fixed Bash and descriptor-bound entry
                "/bin/bash",
                "bash",
                "-p",
                "--",
                entry,
                "--selftest",
            )
        except OSError:
            os._exit(127)
    return child


def _poll_payload(child: int) -> int | None:
    """Return and reap one terminal payload, or report that it is still live."""
    waited, raw_status = os.waitpid(child, os.WNOHANG)
    return None if waited == 0 else os.waitstatus_to_exitcode(raw_status)


def _controller_status_root_is_safe(
    status_receipt: Path, root_descriptor: int, expected_identity: str
) -> bool:
    """Bind the private controller to its inherited suite-root descriptor."""
    try:
        metadata = os.fstat(root_descriptor)
        resolved = _anchored_root_path(root_descriptor).resolve(strict=True)
    except OSError:
        return False
    identity = f"{metadata.st_dev}:{metadata.st_ino}"
    return (
        status_receipt.parent == _anchored_root_path(root_descriptor)
        and _suite_root_metadata_is_safe(metadata)
        and _suite_root_path_is_safe(resolved, metadata)
        and identity == expected_identity
    )


def _controller(
    entry_authority: str,
    status_receipt: Path,
    death_descriptor: int,
    root_authority: tuple[int, str],
    watchdog_timeout: float,
) -> int:
    """Publish nested status while enforcing parent death and a fixed deadline."""
    process = os.getpid()
    root_descriptor, root_identity = root_authority
    if (
        process != os.getpgrp()
        or process != os.getsid(0)
        or not _controller_status_root_is_safe(status_receipt, root_descriptor, root_identity)
    ):
        return 64
    for managed in MANAGED_SIGNALS:
        signal.signal(managed, signal.SIG_IGN)
    signal.pthread_sigmask(signal.SIG_UNBLOCK, MANAGED_SIGNALS)
    published = False
    steps = int(watchdog_timeout / POLL_SECONDS) + 1
    try:
        child = _spawn_payload(entry_authority, (death_descriptor, root_descriptor))
        for _step in range(steps):
            ready, _, _ = select.select((death_descriptor,), (), (), POLL_SECONDS)
            if ready:
                os.read(death_descriptor, 1)
                break
            if not published:
                child_status = _poll_payload(child)
                if child_status is not None:
                    _write_status_atomic(status_receipt, f"{child_status}\n")
                    published = True
    finally:
        for private_descriptor in (death_descriptor, root_descriptor):
            with suppress(OSError):
                os.close(private_descriptor)
        os.killpg(os.getpgrp(), signal.SIGKILL)
    return 126


def _wait_status(path: Path) -> int:
    """Return a bounded controller status while its leader remains live."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while time.monotonic() < deadline:
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        except FileNotFoundError:
            time.sleep(POLL_SECONDS)
            continue
        try:
            metadata = os.fstat(descriptor)
            if metadata.st_nlink != 1:
                time.sleep(POLL_SECONDS)
                continue
            safe = (
                stat.S_ISREG(metadata.st_mode)
                and metadata.st_uid == os.getuid()
                and stat.S_IMODE(metadata.st_mode) == RECEIPT_MODE
                and metadata.st_size == STATUS_SIZE
            )
            value = os.read(descriptor, 3)
        finally:
            os.close(descriptor)
        if safe and value == b"1\n":
            return 1
        message = "controller status receipt is malformed"
        raise RuntimeError(message)
    message = "controller status receipt timed out"
    raise TimeoutError(message)


def _install_interruption_handlers(supervisor: BoundGroup) -> None:
    """Install managed-signal handlers that retry bound cleanup before exit."""

    def interrupted(signum: int, _frame: object) -> None:
        signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)
        for managed in MANAGED_SIGNALS:
            signal.signal(managed, signal.SIG_IGN)
        cleaned = supervisor.contain()
        os._exit(128 + signum if cleaned else 1)

    for managed in MANAGED_SIGNALS:
        signal.signal(managed, interrupted)


def _supervise(
    request: SupervisorRequest,
    controls: SupervisionControls | None = None,
) -> int:
    """Run one injected failure with pre-spawn signal deferral and exact cleanup."""
    supervisor = BoundGroup()
    active = controls if controls is not None else SupervisionControls()
    owned_test_descriptor = active.test_descriptor
    if not supervisor.enable_subreaper():
        return INTEGRITY_REFUSAL_STATUS
    old_mask = signal.pthread_sigmask(signal.SIG_BLOCK, MANAGED_SIGNALS)
    _install_interruption_handlers(supervisor)
    try:
        source_descriptor = int(Path(__file__).name)
        launch = ControllerLaunch(
            request.entry,
            request.status,
            active.watchdog_timeout,
            active.missing_entry_selftest,
            active.entry_mutator,
            active.pre_open_mutator,
        )
        supervisor.spawn(source_descriptor, launch)
        if supervisor.pid is None:
            return 126
        if owned_test_descriptor is not None:
            _write_exact(
                owned_test_descriptor,
                f"{supervisor.pid}\n".encode("ascii"),
                RECEIPT_MAX_BYTES,
            )
            os.close(owned_test_descriptor)
            owned_test_descriptor = None
        if active.pause_before_bound:
            time.sleep(min(DEADLINE_SECONDS * 2, active.watchdog_timeout * 2))
            return 124
        _write_bound_receipt(request.bound, f"{supervisor.pid}\n")
        if request.mode == "signal-pre-bind":
            os.kill(os.getpid(), signal.SIGTERM)
        signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)
        _write_exclusive(request.outer, f"{supervisor.pid}\n")
        child_status = _wait_status(request.status)
        supervisor.require_running("after publishing status")
        time.sleep(POLL_SECONDS * 2)
        supervisor.require_running("through the observation interval")
    except (OSError, RuntimeError, TimeoutError):
        cleaned = supervisor.cleanup()
        return PUBLIC_REFUSAL_STATUS if cleaned else INTEGRITY_REFUSAL_STATUS
    else:
        cleaned = supervisor.cleanup()
        return child_status if cleaned else INTEGRITY_REFUSAL_STATUS
    finally:
        if owned_test_descriptor is not None:
            with suppress(OSError):
                os.close(owned_test_descriptor)
        supervisor.contain()
        signal.pthread_sigmask(signal.SIG_SETMASK, old_mask)


def _dispatch_controller(argv: list[str]) -> int | None:
    """Dispatch the private controller and stat-parser modes."""
    if len(argv) == CONTROLLER_ARG_COUNT and argv[1] == "--controller":
        watchdog_timeout = float(argv[7])
        if watchdog_timeout not in (WATCHDOG_TIMEOUT_SECONDS, SELFTEST_WATCHDOG_TIMEOUT_SECONDS):
            return 64
        return _controller(
            argv[2],
            Path(argv[3]),
            int(argv[4]),
            (int(argv[5]), argv[6]),
            watchdog_timeout,
        )
    return None


def _load_cases_dispatch(descriptor: int) -> Callable[[list[str]], int | None]:
    """Load the authenticated source-only cases dispatcher from its bound FD."""
    try:
        source = _read_cases_source(descriptor)
        digest = hashlib.sha256(source).hexdigest()
        if digest != CASES_RAW_SHA256:
            message = "supervisor cases digest drifted"
            _refuse_entry(message)
        namespace = {
            "__name__": "_ra8_supervisor_cases",
            "__file__": f"/proc/self/fd/{descriptor}",
            "_RA8_SUPERVISOR_CASES_VERSION": 1,
        }
        exec(  # noqa: S102 -- exact digest-bound source-only FD
            compile(source, namespace["__file__"], "exec"), namespace
        )
        grant = namespace.pop("_RA8_SUPERVISOR_CASES_VERSION", None)
        if grant != 1 or "_RA8_SUPERVISOR_CASES_VERSION" in namespace:
            message = "supervisor cases load grant was not consumed"
            _refuse_entry(message)
        if hashlib.sha256(_read_cases_source(descriptor)).hexdigest() != digest:
            message = "supervisor cases changed while loading"
            _refuse_entry(message)
        dispatch = namespace.get("dispatch_supervisor_cases")
        if not callable(dispatch):
            message = "supervisor cases dispatcher is absent"
            _refuse_entry(message)
        if dispatch.__globals__ is not namespace:
            message = "supervisor cases dispatcher escaped its private namespace"
            _refuse_entry(message)
        return dispatch
    finally:
        os.close(descriptor)


def _load_process_api(descriptor: int) -> tuple[object, ...]:
    """Load the authenticated source-only process primitives from a bound FD."""
    module_name = "_ra8_supervisor_process"
    module = None
    try:
        source = _read_process_source(descriptor)
        digest = hashlib.sha256(source).hexdigest()
        if digest != PROCESS_RAW_SHA256:
            message = "supervisor process digest drifted"
            _refuse_entry(message)
        if module_name in sys.modules:
            message = "supervisor process module name is already occupied"
            _refuse_entry(message)
        module = types.ModuleType(module_name)
        namespace = module.__dict__
        namespace["__file__"] = f"/proc/self/fd/{descriptor}"
        namespace["_RA8_SUPERVISOR_PROCESS_VERSION"] = 1
        sys.modules[module_name] = module
        exec(  # noqa: S102 -- exact digest-bound source-only FD
            compile(source, namespace["__file__"], "exec"), namespace
        )
        grant = namespace.pop("_RA8_SUPERVISOR_PROCESS_VERSION", None)
        if grant != 1 or "_RA8_SUPERVISOR_PROCESS_VERSION" in namespace:
            message = "supervisor process load grant was not consumed"
            _refuse_entry(message)
        if hashlib.sha256(_read_process_source(descriptor)).hexdigest() != digest:
            message = "supervisor process changed while loading"
            _refuse_entry(message)
        return _validate_process_api(module_name, namespace, module)
    finally:
        if module is not None and sys.modules.get(module_name) is module:
            del sys.modules[module_name]
        os.close(descriptor)


def _validate_process_api(
    module_name: str, namespace: dict[str, object], module: types.ModuleType
) -> tuple[object, ...]:
    """Validate the exact process API and its digest-bound namespace."""
    names = (
        "ProcessIdentity",
        "BoundGroup",
        "_stat_group",
        "_bind_process",
        "_group_members",
        "_direct_children",
        "_child_table_is_empty",
        "_enable_child_subreaper",
    )
    api = tuple(namespace.get(name) for name in names)
    classes, functions = api[:2], api[2:]
    if not all(isinstance(value, type) for value in classes) or not all(
        isinstance(value, types.FunctionType) for value in functions
    ):
        _refuse_entry("supervisor process API is incomplete")
    methods = tuple(
        value for value in vars(api[1]).values() if isinstance(value, types.FunctionType)
    )
    escaped = any(value.__module__ != namespace["__name__"] for value in classes)
    escaped = escaped or any(value.__globals__ is not namespace for value in functions)
    escaped = escaped or any(value.__globals__ is not namespace for value in methods)
    escaped = escaped or sys.modules.get(module_name) is not module
    if escaped:
        _refuse_entry("supervisor process API escaped its private namespace")
    return api


def _install_process_api(api: tuple[object, ...]) -> None:
    """Install the exact authenticated process API before any case dispatch."""
    if "_ra8_supervisor_process" in sys.modules:
        message = "supervisor process module residue remained after loading"
        _refuse_entry(message)
    global ProcessIdentity
    global BoundGroup
    global _bind_process, _child_table_is_empty, _direct_children
    global _enable_child_subreaper, _group_members, _stat_group
    (
        ProcessIdentity,
        BoundGroup,
        _stat_group,
        _bind_process,
        _group_members,
        _direct_children,
        _child_table_is_empty,
        _enable_child_subreaper,
    ) = api


def _wait_group_populated(group: int) -> bool:
    """Wait for a controller and its payload to share one bound group."""
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        members = _group_members(group)
        if members is not None and len(members) >= MIN_POPULATED_GROUP_MEMBERS:
            return True
        time.sleep(POLL_SECONDS)
    return False


def _wait_group_gone(group: int) -> bool:
    """Wait a bounded interval for one controller group to disappear."""
    deadline = time.monotonic() + DEADLINE_SECONDS
    while time.monotonic() < deadline:
        if _group_members(group) == set():
            return True
        time.sleep(POLL_SECONDS)
    return False


def _cleanup_retry_authorities() -> tuple[dict[str, object], object, object, object, object] | None:
    """Bind process cleanup globals and the separate supervisor namespace."""
    cleanup_method = vars(BoundGroup).get("cleanup")
    if not isinstance(cleanup_method, types.FunctionType):
        return None
    cleanup_globals = cleanup_method.__globals__
    main_globals = globals()
    if cleanup_globals is main_globals:
        return None
    values = (
        cleanup_globals.get("_group_members"),
        cleanup_globals.get("DEADLINE_SECONDS"),
        main_globals.get("_group_members"),
        main_globals.get("DEADLINE_SECONDS"),
    )
    if values[0] is not _group_members or values[1] != DEADLINE_SECONDS:
        return None
    if values[2] is not _group_members or values[3] != DEADLINE_SECONDS:
        return None
    return (cleanup_globals, *values)


def _census_retry_probe(
    supervisor: BoundGroup,
    authorities: tuple[dict[str, object], object, object, object, object],
    main_globals: dict[str, object],
    original_killpg: Callable[[int, int], None],
) -> bool:
    """Inject one census failure and verify that cleanup retains authority."""
    cleanup_globals, original_members, original_deadline, _, _ = authorities
    skipped_signal = False

    def fail_bound_census(group: int) -> set[int] | None:
        return None if group == supervisor.pid else original_members(group)

    def skip_first_group_kill(group: int, sent_signal: int) -> None:
        nonlocal skipped_signal
        if group == supervisor.pid and sent_signal == signal.SIGKILL and not skipped_signal:
            skipped_signal = True
            return
        original_killpg(group, sent_signal)

    main_globals["_group_members"], main_globals["DEADLINE_SECONDS"] = (
        fail_bound_census,
        POLL_SECONDS * 2,
    )
    wrong_namespace_ignored = (
        cleanup_globals["_group_members"] is original_members
        and cleanup_globals["DEADLINE_SECONDS"] == original_deadline
    )
    cleanup_globals["_group_members"], cleanup_globals["DEADLINE_SECONDS"] = (
        fail_bound_census,
        POLL_SECONDS * 2,
    )
    os.killpg = skip_first_group_kill
    first_cleanup = supervisor.cleanup()
    retained_members = original_members(supervisor.pid)
    return (
        not first_cleanup
        and wrong_namespace_ignored
        and not supervisor.reaped
        and skipped_signal
        and supervisor.entry_descriptor is not None
        and retained_members is not None
        and len(retained_members) >= MIN_POPULATED_GROUP_MEMBERS
    )


def _census_cleanup_retry_selftest(entry: str, root: Path) -> bool:
    """Retain authority after failed census, then retry cleanup to completion."""
    supervisor = BoundGroup()
    held_death = None
    authorities = _cleanup_retry_authorities()
    if authorities is None:
        return False
    cleanup_globals, original_members, original_deadline, main_members, main_deadline = authorities
    main_globals = globals()
    original_killpg = os.killpg
    try:
        if not supervisor.enable_subreaper():
            return False
        launch = ControllerLaunch(
            entry, root / "supervisor-cleanup-retry.status", SELFTEST_WATCHDOG_TIMEOUT_SECONDS
        )
        supervisor.spawn(int(Path(__file__).name), launch)
        if supervisor.pid is None or supervisor.death_write is None:
            return False
        held_death = os.dup(supervisor.death_write)
        if not _wait_group_populated(supervisor.pid):
            return False
        retained = _census_retry_probe(
            supervisor,
            authorities,
            main_globals,
            original_killpg,
        )
    finally:
        cleanup_globals["_group_members"], cleanup_globals["DEADLINE_SECONDS"] = (
            original_members,
            original_deadline,
        )
        main_globals["_group_members"], main_globals["DEADLINE_SECONDS"] = (
            main_members,
            main_deadline,
        )
        os.killpg = original_killpg
        if held_death is not None:
            os.close(held_death)
        cleaned = supervisor.contain()
        group_absent = supervisor.pid is None or _wait_group_gone(supervisor.pid)
    return retained and cleaned and supervisor.entry_descriptor is None and group_absent


def _public_supervision(request_argv: list[str]) -> int:
    """Validate and run one public request after cases authentication."""
    if len(request_argv) != SUPERVISOR_ARG_COUNT or request_argv[5] not in (
        "normal",
        "signal-pre-bind",
    ):
        return 64
    if not Path("/proc/self/stat").is_file():
        return 78
    request = SupervisorRequest(
        request_argv[1],
        Path(request_argv[2]),
        Path(request_argv[3]),
        Path(request_argv[4]),
        request_argv[5],
    )
    root = request.bound.parent
    receipts = (request.bound, request.outer, request.status)
    if any(receipt.parent != root or receipt.name in ("", ".", "..") for receipt in receipts):
        return PUBLIC_REFUSAL_STATUS
    try:
        descriptor, identity = _open_suite_root_authority(root)
    except (OSError, RuntimeError):
        return PUBLIC_REFUSAL_STATUS
    if f"{identity[0]}:{identity[1]}" != request_argv[6]:
        _close_suite_root_authority(descriptor, root, identity)
        return PUBLIC_REFUSAL_STATUS
    anchored = _anchored_root_path(descriptor)
    anchored_request = SupervisorRequest(
        request.entry,
        anchored / request.bound.name,
        anchored / request.outer.name,
        anchored / request.status.name,
        request.mode,
    )
    try:
        result = _supervise(anchored_request)
    finally:
        root_integrity = _close_suite_root_authority(descriptor, root, identity)
    return result if root_integrity else INTEGRITY_REFUSAL_STATUS


def _dispatch_authorized(
    process_descriptor: int, cases_descriptor: int, request_argv: list[str]
) -> int:
    """Dispatch one hidden or public mode after authenticating its cases FD."""
    try:
        process_api = _load_process_api(process_descriptor)
        _install_process_api(process_api)
        cases_dispatch = _load_cases_dispatch(cases_descriptor)
    except (OSError, RuntimeError, SyntaxError, UnicodeDecodeError):
        return PUBLIC_REFUSAL_STATUS
    hidden_status = _dispatch_controller(request_argv)
    cases_status = cases_dispatch(request_argv)
    if hidden_status is None:
        hidden_status = cases_status
    if hidden_status is not None:
        return hidden_status
    return _public_supervision(request_argv)


def main(argv: list[str]) -> int:
    """Dispatch private controllers or requests with an authenticated cases FD."""
    controller_status = _dispatch_controller(argv)
    if controller_status is not None:
        return controller_status
    valid = (
        len(argv) >= ROOT_SELFTEST_ARG_COUNT + 2
        and argv[1] == PROCESS_ARG
        and argv[2].isdigit()
        and argv[3] == CASES_ARG
        and argv[4].isdigit()
    )
    if not valid:
        return 64
    return _dispatch_authorized(int(argv[2]), int(argv[4]), [argv[0], *argv[5:]])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
