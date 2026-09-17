# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Transactional suite-root swap proofs for the image supervisor."""

from __future__ import annotations

import os
import select
import signal
import subprocess
from collections.abc import Callable
from contextlib import suppress
from pathlib import Path

import hil_convergence_safety_runtime_mutations as runtime_mutations

PipeFactory = Callable[[int], tuple[int, int]]
SourceWriter = Callable[[Path, str, str, str], tuple[Path, Path, Path]]
RootSwapHooks = tuple[Callable[..., object], Callable[[], Path]]


def _runtime_api(name: str) -> Callable[..., object]:
    """Resolve one mutation helper dynamically to preserve test patching."""
    return runtime_mutations.__dict__[name]


def _root_gate_sources(
    sources: runtime_mutations.SourceBundle, phase: str, handshake: str
) -> runtime_mutations.SourceBundle:
    """Inject one descriptor handshake at an exact root-authority boundary."""
    supervisor, process_source, cases = sources
    if phase == "pre-open":
        anchor = "    before = root.lstat()\n"
        if supervisor.count(anchor) != 1:
            message = "pre-open suite-root mutation authority is not unique"
            raise runtime_mutations.RuntimeMutationError(message)
        supervisor = supervisor.replace(anchor, anchor + handshake, 1)
    elif phase == "post-open":
        anchor = "    anchored_root = _anchored_root_path(descriptor)\n"
        if cases.count(anchor) != 1:
            message = "post-open suite-root mutation authority is not unique"
            raise runtime_mutations.RuntimeMutationError(message)
        cases = cases.replace(anchor, anchor + handshake, 1)
    else:
        message = f"unknown root replacement phase: {phase}"
        raise runtime_mutations.RuntimeMutationError(message)
    return supervisor, process_source, cases


def _retained_receipts_match(saved: Path, baseline: set[str], phase: str) -> bool:
    """Require exact retained-inode effects and their bound receipt contents."""
    added = {path.name for path in saved.iterdir()} - baseline
    if phase == "pre-open":
        return added == set()
    expected = {
        "supervisor-missing-entry.bound",
        "supervisor-missing-entry.outer",
        "supervisor-missing-entry.status",
    }
    if added != expected:
        return False
    try:
        bound = (saved / "supervisor-missing-entry.bound").read_bytes().strip()
        outer = (saved / "supervisor-missing-entry.outer").read_bytes().strip()
        status = (saved / "supervisor-missing-entry.status").read_bytes()
    except OSError:
        return False
    return bound.isdigit() and outer == bound and status == b"127\n"


def _complete_root_swap(
    process: subprocess.Popen[bytes],
    descriptors: tuple[int, int],
    paths: tuple[Path, Path],
    fixture: tuple[set[str], str, bool],
) -> tuple[int | None, bool, bool, bool, tuple[int, int] | None]:
    """Perform one synchronized swap and report effects on both root inodes."""
    ready_read, go_write = descriptors
    original, saved = paths
    baseline, phase, inject_after_rename = fixture
    readable, _, _ = select.select((ready_read,), (), (), runtime_mutations.RESIDUE_TIMEOUT_SECONDS)
    if not readable or os.read(ready_read, 1) != b"R":
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        status, clean, _stderr = _runtime_api("_collect_runner")(process)
        return status, clean, False, False, None
    original.rename(saved)
    _runtime_api("_move_owned_root")(original, saved)
    if inject_after_rename:
        message = "injected root-swap failure after rename"
        raise runtime_mutations.RuntimeMutationError(message)
    replacement_identity = _runtime_api("_create_replacement_root")(original)
    os.write(go_write, b"G")
    status, clean, _stderr = _runtime_api("_collect_runner")(process)
    return (
        status,
        clean and _runtime_api("_no_residue")((original, saved)),
        not tuple(original.iterdir()),
        _retained_receipts_match(saved, baseline, phase),
        replacement_identity,
    )


def _path_is_present(path: Path) -> bool:
    """Report path presence without treating a broken symlink as absent."""
    return path.exists() or path.is_symlink()


def _cleanup_swap_roots(
    paths: tuple[Path, Path],
    identities: tuple[tuple[int, int], tuple[int, int] | None],
) -> runtime_mutations.RuntimeMutationError | None:
    """Attempt every bound root removal while preserving the first refusal."""
    original, saved = paths
    original_identity, replacement_identity = identities
    first_error: runtime_mutations.RuntimeMutationError | None = None
    candidates: list[tuple[Path, tuple[int, int]]] = []
    if replacement_identity is not None and _path_is_present(original):
        candidates.append((original, replacement_identity))
    if _path_is_present(saved):
        candidates.append((saved, original_identity))
    elif _path_is_present(original):
        candidates.append((original, original_identity))
    for path, identity in candidates:
        error = _root_removal_error(path, identity)
        if first_error is None:
            first_error = error
    return first_error


def _root_removal_error(
    path: Path, identity: tuple[int, int]
) -> runtime_mutations.RuntimeMutationError | None:
    """Attempt one bound root removal and return its refusal."""
    try:
        _runtime_api("_remove_root")(path, identity)
    except runtime_mutations.RuntimeMutationError as error:
        return error
    return None


def _finalize_root_swap(
    process: subprocess.Popen[bytes] | None,
    descriptors: set[int],
    paths: tuple[Path, Path],
    identities: tuple[tuple[int, int], tuple[int, int] | None],
    proof: tuple[bool, bool, set[str]],
) -> tuple[bool, bool]:
    """Dispose the runner, prove descriptor closure, then remove bound roots."""
    cleanup_clean, injected, baseline = proof
    first_error: (
        OSError | runtime_mutations.RuntimeMutationError | subprocess.TimeoutExpired | None
    ) = None
    process_resolved = process is None
    if process is not None:
        try:
            _status, cleanup_clean = _runtime_api("_dispose_runner")(process)
            process_resolved = cleanup_clean
        except (
            OSError,
            runtime_mutations.RuntimeMutationError,
            subprocess.TimeoutExpired,
        ) as error:
            first_error = error
    try:
        _runtime_api("_close_owned_descriptors")(descriptors)
    except OSError as error:
        if first_error is None:
            first_error = error
    if not process_resolved and first_error is None:
        first_error = runtime_mutations.RuntimeMutationError(
            "root-swap runner authority remained unresolved"
        )
    original, saved = paths
    injected_state = (
        injected
        and not _path_is_present(original)
        and _path_is_present(saved)
        and {path.name for path in saved.iterdir()} == baseline
        and _runtime_api("_no_residue")(paths)
    )
    if process_resolved:
        root_error = _cleanup_swap_roots(paths, identities)
        if first_error is None:
            first_error = root_error
    if first_error is not None:
        raise first_error
    return cleanup_clean, injected_state


def _prepare_root_gate(
    sources: runtime_mutations.SourceBundle,
    phase: str,
    root: Path,
    hooks: tuple[PipeFactory, SourceWriter] | None = None,
) -> tuple[Path, Path, Path, dict[str, str], tuple[int, int, int, int], set[int]]:
    """Create one handshake whose descriptors remain in an exact owned set."""
    owned: set[int] = set()
    pipe_factory, source_writer = hooks or (os.pipe2, _runtime_api("_write_sources"))
    try:
        ready_read, ready_write = pipe_factory(os.O_CLOEXEC)
        owned.update((ready_read, ready_write))
        go_read, go_write = pipe_factory(os.O_CLOEXEC)
        owned.update((go_read, go_write))
        descriptors = (ready_read, ready_write, go_read, go_write)
        environment = dict(os.environ)
        environment["RA8_RUNTIME_READY_FD"] = str(ready_write)
        environment["RA8_RUNTIME_GO_FD"] = str(go_read)
        handshake = (
            '    os.write(int(os.environ["RA8_RUNTIME_READY_FD"]), b"R")\n'
            '    os.read(int(os.environ["RA8_RUNTIME_GO_FD"]), 1)\n'
        )
        supervisor, process_source, cases = _root_gate_sources(sources, phase, handshake)
        main_path, process_path, cases_path = source_writer(root, supervisor, process_source, cases)
    except (OSError, runtime_mutations.RuntimeMutationError) as primary:
        try:
            _runtime_api("_close_owned_descriptors")(owned)
        except OSError as cleanup_error:
            raise primary from cleanup_error
        raise
    else:
        return main_path, process_path, cases_path, environment, descriptors, owned


def _public_root_swap_result(
    injected: bool,
    result: tuple[int | None, bool, bool, bool, tuple[int, int] | None] | None,
    cleanup_clean: bool,
    injected_state: bool,
) -> tuple[int | None, bool, bool, bool]:
    """Return one public result only after the cleanup proof completed."""
    if injected:
        return None, cleanup_clean, injected_state, injected_state
    if result is None:
        message = "root-swap runtime produced no result"
        raise runtime_mutations.RuntimeMutationError(message)
    return result[:4]


def _create_swap_paths(
    saved_selector: Callable[[], Path],
) -> tuple[Path, Path, tuple[int, int]]:
    """Create the first root only when the absent saved path is also acquired."""
    original, identity = _runtime_api("_create_root")()
    try:
        saved = saved_selector()
    except runtime_mutations.RuntimeMutationError as primary:
        try:
            _runtime_api("_remove_root")(original, identity)
        except runtime_mutations.RuntimeMutationError as cleanup_error:
            raise primary from cleanup_error
        raise
    if saved == original:
        _runtime_api("_remove_root")(original, identity)
        message = "saved supervisor runtime root aliases its live original"
        raise runtime_mutations.RuntimeMutationError(message)
    return original, saved, identity


def _start_root_swap_runner(
    paths: tuple[Path, Path, Path],
    environment: dict[str, str],
    descriptors: tuple[int, int, int, int],
    owned: set[int],
    root: Path,
) -> subprocess.Popen[bytes]:
    """Launch a synchronized swap runner and release its inherited gate ends."""
    main_path, process_path, cases_path = paths
    _ready_read, ready_write, go_read, _go_write = descriptors
    process = _runtime_api("_start_supervisor")(
        main_path,
        process_path,
        cases_path,
        ("--selftest-missing-entry", str(root), _runtime_api("_identity_text")(root)),
        runtime_mutations.SupervisorStart(
            extra_descriptors=(ready_write, go_read), environment=environment
        ),
    )
    _runtime_api("_release_owned_descriptor")(owned, ready_write)
    _runtime_api("_release_owned_descriptor")(owned, go_read)
    return process


def _replace_root_after_gate(
    sources: runtime_mutations.SourceBundle,
    *,
    phase: str,
    inject_after_rename: bool = False,
    hooks: RootSwapHooks | None = None,
) -> tuple[int | None, bool, bool, bool]:
    """Replace a suite root at one deterministic retained-authority boundary."""
    gate_preparer, saved_selector = hooks or (
        _runtime_api("_prepare_root_gate"),
        _runtime_api("_new_root_path"),
    )
    original, saved, original_identity = _create_swap_paths(saved_selector)
    replacement_identity: tuple[int, int] | None = None
    process: subprocess.Popen[bytes] | None = None
    cleanup_clean = True
    owned_descriptors: set[int] = set()
    baseline: set[str] = set()
    result: tuple[int | None, bool, bool, bool, tuple[int, int] | None] | None = None
    injected = False
    injected_state = False
    try:
        (
            main_path,
            process_path,
            cases_path,
            environment,
            descriptors,
            owned_descriptors,
        ) = gate_preparer(sources, phase, original)
        ready_read, _ready_write, _go_read, go_write = descriptors
        baseline = {path.name for path in original.iterdir()}
        process = _start_root_swap_runner(
            (main_path, process_path, cases_path),
            environment,
            descriptors,
            owned_descriptors,
            original,
        )
        result = _complete_root_swap(
            process,
            (ready_read, go_write),
            (original, saved),
            (baseline, phase, inject_after_rename),
        )
        _status, cleanup_clean, _replacement_empty, _retained_bound, replacement_identity = result
        process = None
        _runtime_api("_release_owned_descriptor")(owned_descriptors, go_write)
    except runtime_mutations.RuntimeMutationError as error:
        if not inject_after_rename or str(error) != "injected root-swap failure after rename":
            raise
        injected = True
    finally:
        cleanup_clean, injected_state = _finalize_root_swap(
            process,
            owned_descriptors,
            (original, saved),
            (original_identity, replacement_identity),
            (cleanup_clean, injected, baseline),
        )
    return _public_root_swap_result(inject_after_rename, result, cleanup_clean, injected_state)
