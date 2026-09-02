# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Runtime mutation proofs for the descriptor-bound image supervisor."""

from __future__ import annotations

import os
import secrets
import shutil
import signal
import stat
import subprocess
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager, suppress
from pathlib import Path

import hil_convergence_safety_runtime_launcher as runtime_launcher
import hil_convergence_safety_runtime_root_swap as runtime_root_swap
import hil_convergence_safety_runtime_sources as runtime_sources

Mutation = tuple[str, str, str, str]
Census = Callable[[int], set[int] | None]
PipeFactory = Callable[[int], tuple[int, int]]
SourceWriter = Callable[[Path, str, str, str], tuple[Path, Path, Path]]
SourceBundle = tuple[str, str, str]
RUNTIME_TIMEOUT_SECONDS, RESIDUE_TIMEOUT_SECONDS = 25.0, 5.0
POLL_SECONDS = 0.01
PRIVATE_MODE = 0o700
ROOT_PREFIX = "ra8-devcontainer-image-selftest."
ROOT_SUFFIX_LENGTH, PROCESS_GROUP_FIELD = 32, 2
PROCESS_UID_FIELD_COUNT = 4
INTEGRITY_REFUSAL_STATUS = 126
PUBLIC_REFUSAL_STATUS = 125
USAGE_STATUS = 64
DESCENDANT_STATUS = 23
ONE_SHOT_MUTATION_STATUS = 5
CANONICAL_TMP = Path(os.path.sep, "tmp").resolve(strict=True)
RuntimeMutationError = runtime_sources.RuntimeSourceError
_write_sources = runtime_sources.publish
SupervisorStart = runtime_launcher.SupervisorStart
_close_owned_descriptors = runtime_launcher.close_owned_descriptors
_open_supervisor_sources = runtime_launcher.open_supervisor_sources


GatePreparer = Callable[
    [SourceBundle, str, Path],
    tuple[Path, Path, Path, dict[str, str], tuple[int, int, int, int], set[int]],
]
RootSwapHooks = tuple[GatePreparer, Callable[[], Path]]


def _identity(path: Path) -> tuple[int, int]:
    """Return one filesystem identity without following a final link."""
    metadata = path.lstat()
    return metadata.st_dev, metadata.st_ino


def _identity_text(path: Path) -> str:
    """Return the supervisor's stable device/inode spelling."""
    return ":".join(str(value) for value in _identity(path))


def _register_owned_root(root: Path, identity: tuple[int, int]) -> None:
    """Bind one created root to the currently active selftest run."""
    registry = getattr(_create_root, "_owned_root_registry", None)
    if registry is not None:
        registry.append((root, identity))


def _forget_owned_root(root: Path, identity: tuple[int, int]) -> None:
    """Forget one root only after its exact bound inode was removed."""
    registry = getattr(_create_root, "_owned_root_registry", None)
    if registry is None:
        return
    for index in range(len(registry) - 1, -1, -1):
        if registry[index] == (root, identity):
            del registry[index]
            return


def _created_root_cleanup(root: Path, identity: tuple[int, int]) -> RuntimeMutationError | None:
    """Remove a just-created root using independent containment and identity checks."""
    try:
        metadata = root.lstat()
        resolved = root.resolve(strict=True)
    except OSError:
        return RuntimeMutationError("created supervisor runtime root became unobservable")
    name = root.name
    suffix = name.removeprefix(ROOT_PREFIX)
    safe = (
        root.is_absolute()
        and root.parent == CANONICAL_TMP
        and resolved == root
        and name.startswith(ROOT_PREFIX)
        and len(suffix) == ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
        and (metadata.st_dev, metadata.st_ino) == identity
    )
    if not safe:
        return RuntimeMutationError("refusing unbound supervisor runtime root cleanup")
    if not _no_residue((root,)):
        return RuntimeMutationError("refusing live supervisor runtime cleanup")
    try:
        shutil.rmtree(root)
    except OSError:
        return RuntimeMutationError("created supervisor runtime root cleanup failed")
    if root.exists() or root.is_symlink():
        return RuntimeMutationError("supervisor runtime root survived cleanup")
    return None


def _drain_owned_roots(registry: list[tuple[Path, tuple[int, int]]]) -> RuntimeMutationError | None:
    """Clean only roots recorded by this run, preserving the first refusal."""
    first_error: RuntimeMutationError | None = None
    for root, identity in reversed(tuple(registry)):
        if not root.exists() and not root.is_symlink():
            _forget_owned_root(root, identity)
            continue
        try:
            _remove_root(root, identity)
        except RuntimeMutationError as error:
            if first_error is None:
                first_error = error
    return first_error


@contextmanager
def _owned_root_scope() -> Iterator[None]:
    """Own every root allocated in one run until its outer cleanup completes."""
    state = vars(_create_root)
    previous = state.get("_owned_root_registry")
    registry: list[tuple[Path, tuple[int, int]]] = []
    state["_owned_root_registry"] = registry
    try:
        yield
    except BaseException as primary:
        cleanup_error = _drain_owned_roots(registry)
        if cleanup_error is not None:
            raise primary from cleanup_error
        raise
    else:
        cleanup_error = _drain_owned_roots(registry)
        if cleanup_error is not None:
            raise cleanup_error
    finally:
        if previous is None:
            del state["_owned_root_registry"]
        else:
            state["_owned_root_registry"] = previous


def _root_path_is_safe(path: Path, identity: tuple[int, int]) -> bool:
    """Prove one exact private direct child of the physical temporary root."""
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError:
        return False
    name = path.name
    suffix = name.removeprefix(ROOT_PREFIX)
    return (
        path.is_absolute()
        and path.parent == CANONICAL_TMP
        and resolved == path
        and name.startswith(ROOT_PREFIX)
        and len(suffix) == ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and stat.S_ISDIR(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and stat.S_IMODE(metadata.st_mode) == PRIVATE_MODE
        and (metadata.st_dev, metadata.st_ino) == identity
    )


def _new_root_path() -> Path:
    """Choose one absent canonical root path with 128 random bits."""
    for _attempt in range(20):
        candidate = CANONICAL_TMP / f"{ROOT_PREFIX}{secrets.token_hex(16)}"
        if not candidate.exists() and not candidate.is_symlink():
            return candidate
    message = "could not select a fresh supervisor runtime root"
    raise RuntimeMutationError(message)


def _create_root() -> tuple[Path, tuple[int, int]]:
    """Create and bind one private canonical suite root."""
    root = _new_root_path()
    root.mkdir(mode=PRIVATE_MODE)
    return root, _bind_created_root(root)


def _bind_created_root(root: Path) -> tuple[int, int]:
    """Validate and register one already-created private canonical root."""
    identity: tuple[int, int] | None = None
    try:
        identity = _identity(root)
        if not _root_path_is_safe(root, identity):
            message = "created supervisor runtime root is unsafe"
            raise RuntimeMutationError(message)  # noqa: TRY301 -- validation is the transaction boundary.
    except BaseException as primary:
        cleanup_identity = identity
        if cleanup_identity is not None:
            cleanup_error = _created_root_cleanup(root, cleanup_identity)
            if cleanup_error is not None:
                raise primary from cleanup_error
        raise
    _register_owned_root(root, identity)
    return identity


def _create_replacement_root(root: Path) -> tuple[int, int]:
    """Create one root-swap replacement with the same transactional binding."""
    root.mkdir(mode=PRIVATE_MODE)
    return _bind_created_root(root)


def _remove_root(root: Path, identity: tuple[int, int]) -> None:
    """Remove only one still-bound private runtime root."""
    if not _root_path_is_safe(root, identity):
        message = f"refusing unbound supervisor runtime cleanup: {root}"
        raise RuntimeMutationError(message)
    if not _no_residue((root,)):
        message = f"refusing live supervisor runtime cleanup: {root}"
        raise RuntimeMutationError(message)
    shutil.rmtree(root)
    if root.exists() or root.is_symlink():
        message = f"supervisor runtime root survived cleanup: {root}"
        raise RuntimeMutationError(message)
    _forget_owned_root(root, identity)


def _move_owned_root(source: Path, destination: Path) -> None:
    """Move one registry binding across an identity-preserving rename."""
    registry = getattr(_create_root, "_owned_root_registry", None)
    if registry is None:
        return
    for index, (path, identity) in enumerate(registry):
        if path == source:
            registry[index] = destination, identity
            return


def _remove_link(link: Path, identity: tuple[int, int]) -> None:
    """Remove one exact test-created canonical symlink."""
    metadata = link.lstat()
    name = link.name
    suffix = name.removeprefix(ROOT_PREFIX)
    safe = (
        link.is_absolute()
        and link.parent == CANONICAL_TMP
        and name.startswith(ROOT_PREFIX)
        and stat.S_ISLNK(metadata.st_mode)
        and metadata.st_uid == os.getuid()
        and metadata.st_gid == os.getgid()
        and len(suffix) == ROOT_SUFFIX_LENGTH
        and all(character in "0123456789abcdef" for character in suffix)
        and (metadata.st_dev, metadata.st_ino) == identity
    )
    if not safe:
        message = "refusing unbound supervisor runtime symlink cleanup"
        raise RuntimeMutationError(message)
    link.unlink()


def _write_stall_entry(root: Path) -> Path:
    """Write one payload whose live process retains an observable root marker."""
    entry = root / "stall-entry.sh"
    entry.write_text(
        "#!/bin/bash\n"
        '[[ "$1" == "--selftest" ]] || exit 64\n'
        "trap '' HUP INT TERM\n"
        'exec -a "$0" /bin/sleep 30\n',
        encoding="ascii",
    )
    entry.chmod(PRIVATE_MODE)
    return entry


def _process_group_members(group: int) -> set[int] | None:
    """Return Linux process IDs in one exact process group."""
    members: set[int] = set()
    try:
        processes = tuple(Path("/proc").iterdir())
    except OSError:
        return None
    for process in processes:
        if not process.name.isdigit():
            continue
        try:
            raw = (process / "stat").read_bytes()
        except FileNotFoundError:
            continue
        except OSError:
            return None
        closing = raw.rfind(b")")
        fields = raw[closing + 2 :].split() if closing >= 0 else []
        if len(fields) <= PROCESS_GROUP_FIELD or not fields[PROCESS_GROUP_FIELD].isdigit():
            return None
        if int(fields[PROCESS_GROUP_FIELD]) == group:
            members.add(int(process.name))
    return members


def _wait_group_members(
    group: int, allowed: set[int], census: Census = _process_group_members
) -> bool:
    """Wait until a group contains only the explicitly allowed process IDs."""
    deadline = time.monotonic() + RESIDUE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        members = census(group)
        if members is not None and members <= allowed:
            return True
        time.sleep(POLL_SECONDS)
    return False


def _reap_terminal_runner(
    process: subprocess.Popen[bytes], census: Census = _process_group_members
) -> tuple[int, bool]:
    """Remove descendants before reaping one already-terminal group leader."""
    members = census(process.pid)
    group_bound = members is not None and members <= {process.pid}
    if not group_bound:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        group_bound = _wait_group_members(process.pid, {process.pid}, census)
    if not group_bound:
        message = "supervisor runtime runner group survived before leader reap"
        raise RuntimeMutationError(message)
    status = process.wait(timeout=RESIDUE_TIMEOUT_SECONDS)
    return status, _wait_group_members(process.pid, set(), census)


def _kill_and_reap_runner(
    process: subprocess.Popen[bytes], census: Census = _process_group_members
) -> tuple[int | None, bool]:
    """Immediately close one owned runner without signaling after leader reap."""
    try:
        terminal = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
    except ChildProcessError:
        if process.returncode is None:
            message = "supervisor runtime runner was reaped outside its owner"
            raise RuntimeMutationError(message) from None
        return process.returncode, _wait_group_members(process.pid, set(), census)
    if terminal is None:
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        deadline = time.monotonic() + RESIDUE_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            terminal = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
            if terminal is not None:
                break
            time.sleep(POLL_SECONDS)
    if terminal is None:
        message = "supervisor runtime runner survived its bounded SIGKILL deadline"
        raise RuntimeMutationError(message)
    return _reap_terminal_runner(process, census)


def _wait_runner(
    process: subprocess.Popen[bytes], census: Census = _process_group_members
) -> tuple[int | None, bool]:
    """Retain the direct leader with WNOWAIT until its group is empty."""
    deadline = time.monotonic() + RUNTIME_TIMEOUT_SECONDS
    terminal = False
    while time.monotonic() < deadline:
        result = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        if result is not None:
            terminal = True
            break
        time.sleep(POLL_SECONDS)
    if not terminal:
        return _kill_and_reap_runner(process, census)
    return _reap_terminal_runner(process, census)


def _close_runner_streams(process: subprocess.Popen[bytes]) -> None:
    """Close every parent-side pipe owned by one runner."""
    first_error: OSError | None = None
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None:
            try:
                stream.close()
            except OSError as error:
                if first_error is None:
                    first_error = error
    if first_error is not None:
        raise first_error


def _dispose_runner(process: subprocess.Popen[bytes]) -> tuple[int | None, bool]:
    """Terminate, group-clean, reap, and close one runner on a failure path."""
    result: tuple[int | None, bool] | None = None
    primary: OSError | RuntimeMutationError | subprocess.TimeoutExpired | None = None
    try:
        result = _kill_and_reap_runner(process)
    except (OSError, RuntimeMutationError, subprocess.TimeoutExpired) as error:
        primary = error
    try:
        _close_runner_streams(process)
    except OSError as cleanup_error:
        if primary is not None:
            raise primary from cleanup_error
        raise
    if primary is not None:
        raise primary
    if result is None:
        message = "supervisor runtime runner disposal produced no terminal result"
        raise RuntimeMutationError(message)
    return result


def _release_owned_descriptor(
    descriptors: set[int], descriptor: int, closer: runtime_launcher.DescriptorCloser = os.close
) -> None:
    """Release numeric authority before the one ambiguous close attempt."""
    descriptors.remove(descriptor)
    closer(descriptor)


def _start_supervisor(
    main_path: Path,
    process_path: Path,
    cases_path: Path,
    arguments: tuple[str, ...],
    launch: SupervisorStart | None = None,
) -> subprocess.Popen[bytes]:
    """Start one descriptor-bound supervisor in a retained runner group."""
    result = runtime_launcher.launch(
        (main_path, process_path, cases_path), arguments, launch or SupervisorStart()
    )
    if result.close_error is not None:
        cleanup_error: OSError | RuntimeMutationError | subprocess.TimeoutExpired | None = None
        if result.process is not None:
            try:
                _dispose_runner(result.process)
            except (OSError, RuntimeMutationError, subprocess.TimeoutExpired) as error:
                cleanup_error = error
        if result.start_error is not None:
            raise result.start_error from result.close_error
        if cleanup_error is not None:
            raise result.close_error from cleanup_error
        raise result.close_error
    if result.start_error is not None:
        raise result.start_error
    if result.process is None:
        message = "supervisor launcher returned no process or error"
        raise RuntimeMutationError(message)
    return result.process


def _collect_runner(process: subprocess.Popen[bytes]) -> tuple[int | None, bool, bytes]:
    """Collect one started runner through a finally-owned cleanup funnel."""
    try:
        status, clean = _wait_runner(process)
        _stdout, stderr = process.communicate(timeout=RESIDUE_TIMEOUT_SECONDS)
    except (OSError, RuntimeMutationError, subprocess.TimeoutExpired, ValueError) as primary:
        cleanup_error: (
            OSError | RuntimeMutationError | subprocess.TimeoutExpired | ValueError | None
        )
        try:
            _status, cleanup_clean = _dispose_runner(process)
        except (OSError, RuntimeMutationError, subprocess.TimeoutExpired, ValueError) as error:
            cleanup_error = error
        else:
            cleanup_error = None
            if not cleanup_clean:
                message = "supervisor runtime runner cleanup did not converge"
                cleanup_error = RuntimeMutationError(message)
        if cleanup_error is not None:
            raise primary from cleanup_error
        raise
    else:
        _close_runner_streams(process)
        return status, clean, stderr


def _run_supervisor(
    main_path: Path,
    process_path: Path,
    cases_path: Path,
    arguments: tuple[str, ...],
) -> tuple[int | None, bool, bytes]:
    """Run and reap one supervisor while preserving its diagnostic bytes."""
    return _collect_runner(_start_supervisor(main_path, process_path, cases_path, arguments))


def _receipt_groups(paths: tuple[Path, ...]) -> set[int] | None:
    """Return numeric group receipts, or unknown on any unsafe observation."""
    groups: set[int] = set()
    for root in paths:
        try:
            receipts = tuple(root.iterdir())
        except FileNotFoundError:
            continue
        except OSError:
            return None
        for receipt in receipts:
            if receipt.suffix not in (".bound", ".outer"):
                continue
            try:
                value = receipt.read_bytes().strip()
            except FileNotFoundError:
                continue
            except OSError:
                return None
            if not value.isdigit() or int(value) <= 0:
                return None
            groups.add(int(value))
    return groups


def _descriptor_path_reference(
    descriptors: tuple[Path, ...], needles: tuple[bytes, ...]
) -> bool | None:
    """Return one descriptor table's reference state, or unknown."""
    for descriptor in descriptors:
        if not descriptor.name.isdigit():
            return None
        try:
            target = os.fsencode(descriptor.readlink())
        except FileNotFoundError:
            continue
        except OSError:
            return None
        if not target:
            return None
        if any(needle in target for needle in needles):
            return True
    return False


def _process_path_reference(process: Path, needles: tuple[bytes, ...]) -> bool | None:
    """Return one process's reference state, or unknown on malformed observation."""
    try:
        command = (process / "cmdline").read_bytes()
        descriptors = tuple((process / "fd").iterdir())
    except FileNotFoundError:
        return False
    except OSError:
        return None
    if command and not command.endswith(b"\0"):
        return None
    if any(needle in command for needle in needles):
        return True
    return _descriptor_path_reference(descriptors, needles)


def _process_has_suite_owner(process: Path) -> bool | None:
    """Return whether one process has any UID matching this selftest owner."""
    try:
        lines = (process / "status").read_text(encoding="ascii").splitlines()
    except FileNotFoundError:
        return False
    except (OSError, UnicodeError):
        return None
    uid_lines = [line for line in lines if line.startswith("Uid:")]
    if len(uid_lines) != 1:
        return None
    fields = uid_lines[0].split()[1:]
    if len(fields) != PROCESS_UID_FIELD_COUNT or not all(field.isdigit() for field in fields):
        return None
    return str(os.getuid()) in fields


def _paths_have_no_live_references(paths: tuple[Path, ...]) -> bool:
    """Prove no process command or descriptor retains an owned runtime path."""
    needles = tuple(os.fsencode(path) for path in paths)
    deadline = time.monotonic() + RESIDUE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        referenced = False
        try:
            processes = tuple(Path("/proc").iterdir())
        except OSError:
            return False
        for process in processes:
            if not process.name.isdigit():
                continue
            owned = _process_has_suite_owner(process)
            if owned is None:
                return False
            if not owned:
                continue
            state = _process_path_reference(process, needles)
            if state is None:
                return False
            if state:
                referenced = True
                break
        if not referenced:
            return True
        time.sleep(POLL_SECONDS)
    return False


def _receipt_group_is_absent(group: int) -> bool:
    """Use the kernel's exact negative-PGID authority to prove group absence."""
    try:
        os.killpg(group, 0)
    except ProcessLookupError:
        return True
    except OSError:
        return False
    return False


def _no_residue(paths: tuple[Path, ...]) -> bool:
    """Prove every process group named by an owned receipt is absent."""
    groups = _receipt_groups(paths)
    if groups is None:
        return False
    deadline = time.monotonic() + RESIDUE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if all(_receipt_group_is_absent(group) for group in groups):
            return True
        time.sleep(POLL_SECONDS)
    return False


def _watchdog_case(supervisor: str, process_source: str, cases: str) -> tuple[str, bool]:
    """Require the one-second base to pass and the production-deadline mutant to fail."""
    assignment = 'SELFTEST_WATCHDOG_TIMEOUT_SECONDS = MAIN_API["SELFTEST_WATCHDOG_TIMEOUT_SECONDS"]'
    replacement = 'SELFTEST_WATCHDOG_TIMEOUT_SECONDS = MAIN_API["WATCHDOG_TIMEOUT_SECONDS"]'
    if cases.count(assignment) != 1:
        message = "watchdog runtime mutation authority is not unique"
        raise RuntimeMutationError(message)
    outcomes: list[tuple[str, int | None, bool, bool]] = []
    roots: list[tuple[Path, tuple[int, int]]] = []
    try:
        for label, source in (("base", cases), ("mutant", cases.replace(assignment, replacement))):
            root, identity = _create_root()
            roots.append((root, identity))
            main_path, process_path, cases_path = _write_sources(
                root, supervisor, process_source, source
            )
            entry = _write_stall_entry(root)
            status, clean, _stderr = _run_supervisor(
                main_path,
                process_path,
                cases_path,
                ("--selftest-watchdog-expiry", str(entry), str(root), _identity_text(root)),
            )
            outcomes.append((label, status, clean, _no_residue((root,))))
        passed = outcomes == [("base", 0, True, True), ("mutant", 1, True, True)]
        return "watchdog runtime deadline mutation fires", passed
    finally:
        for root, identity in reversed(roots):
            _remove_root(root, identity)


def _observation_case(supervisor: str, process_source: str, cases: str) -> tuple[str, bool]:
    """Require the injected observation error to remain load-bearing."""
    call = "            _inject_observation_failure()"
    replacement = "            None  # runtime mutation: required observation omitted"
    if cases.count(call) != 1:
        message = "observation runtime mutation authority is not unique"
        raise RuntimeMutationError(message)
    outcomes: list[tuple[str, int | None, bool, bool]] = []
    roots: list[tuple[Path, tuple[int, int]]] = []
    try:
        for label, source in (("base", cases), ("mutant", cases.replace(call, replacement))):
            root, identity = _create_root()
            roots.append((root, identity))
            main_path, process_path, cases_path = _write_sources(
                root, supervisor, process_source, source
            )
            entry = _write_stall_entry(root)
            status, clean, _stderr = _run_supervisor(
                main_path,
                process_path,
                cases_path,
                ("--selftest-observation-failure", str(entry), str(root), _identity_text(root)),
            )
            outcomes.append((label, status, clean, _no_residue((root,))))
        passed = outcomes == [("base", 0, True, True), ("mutant", 1, True, True)]
        return "observation failure runtime mutation fires", passed
    finally:
        for root, identity in reversed(roots):
            _remove_root(root, identity)


def _closed_death_group_race_case(
    supervisor: str, process_source: str, cases: str
) -> tuple[str, bool]:
    """Prove a pre-cleanup process-group census cannot return as success."""
    observation = (
        "        if result is not None:\n"
        "            return result.si_code == os.CLD_KILLED and "
        "result.si_status == signal.SIGKILL\n"
    )
    premature = (
        "        if result is not None:\n"
        "            members = _group_members(child.pid)\n"
        "            return (\n"
        "                result.si_code == os.CLD_KILLED\n"
        "                and result.si_status == signal.SIGKILL\n"
        "                and members is not None\n"
        "                and members <= {child.pid}\n"
        "            )\n"
    )
    if cases.count(observation) != 1:
        message = "closed-death group-census mutation authority is not unique"
        raise RuntimeMutationError(message)
    outcomes: list[tuple[str, int | None, int | None, bool, bool]] = []
    roots: list[tuple[Path, tuple[int, int]]] = []
    try:
        for label, source in (("base", cases), ("mutant", cases.replace(observation, premature))):
            root, identity = _create_root()
            roots.append((root, identity))
            main_path, process_path, cases_path = _write_sources(
                root, supervisor, process_source, source
            )
            entry = _write_stall_entry(root)
            watchdog, watchdog_clean, _stderr = _run_supervisor(
                main_path,
                process_path,
                cases_path,
                ("--selftest-watchdog-expiry", str(entry), str(root), _identity_text(root)),
            )
            closed, closed_clean, _stderr = _run_supervisor(
                main_path,
                process_path,
                cases_path,
                ("--selftest-closed-death-fd", str(entry), str(root), _identity_text(root)),
            )
            outcomes.append(
                (label, watchdog, closed, watchdog_clean and closed_clean, _no_residue((root,)))
            )
        expected = [("base", 0, 0, True, True), ("mutant", 0, 1, True, True)]
        return "closed-death premature group census mutation fires", outcomes == expected
    finally:
        for root, identity in reversed(roots):
            _remove_root(root, identity)


def _prepare_root_gate(*args: object, **kwargs: object) -> object:
    """Delegate root-gate preparation to the focused swap module."""
    return runtime_root_swap.__dict__["_prepare_root_gate"](*args, **kwargs)


def _finalize_root_swap(*args: object, **kwargs: object) -> object:
    """Delegate root-swap finalization to the focused swap module."""
    return runtime_root_swap.__dict__["_finalize_root_swap"](*args, **kwargs)


def _replace_root_after_gate(
    sources: SourceBundle,
    *,
    phase: str,
    inject_after_rename: bool = False,
    hooks: RootSwapHooks | None = None,
) -> tuple[int | None, bool, bool, bool]:
    """Replace a suite root at one deterministic retained-authority boundary."""
    return runtime_root_swap.__dict__["_replace_root_after_gate"](
        sources, phase=phase, inject_after_rename=inject_after_rename, hooks=hooks
    )


def runtime_cases(inputs: dict[str, str]) -> list[tuple[str, bool]]:
    """Run every Linux-only descriptor, root, and cleanup mutation proof."""
    if not Path("/proc/self/stat").is_file():
        return [("image supervisor runtime mutations are Linux-only", True)]
    supervisor = inputs["devcontainer_image_selftest_supervisor"]
    process_source = inputs["devcontainer_image_selftest_process"]
    cases = inputs["devcontainer_image_selftest_supervisor_cases"]
    with _owned_root_scope():
        return [
            _watchdog_case(supervisor, process_source, cases),
            _observation_case(supervisor, process_source, cases),
            _closed_death_group_race_case(supervisor, process_source, cases),
        ]
