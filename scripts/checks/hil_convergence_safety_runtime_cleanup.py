# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Failure-direction cleanup proofs for image supervisor runtime mutations."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
from contextlib import suppress
from pathlib import Path
from unittest.mock import patch

import hil_convergence_safety_runtime_loader as runtime_loader
import hil_convergence_safety_runtime_mutations as runtime_mutations
from hil_convergence_safety_runtime_mutations import (
    CANONICAL_TMP,
    INTEGRITY_REFUSAL_STATUS,
    ROOT_PREFIX,
    USAGE_STATUS,
    RuntimeMutationError,
    SourceBundle,
    SupervisorStart,
    _close_owned_descriptors,
    _close_runner_streams,
    _collect_runner,
    _create_replacement_root,
    _create_root,
    _dispose_runner,
    _finalize_root_swap,
    _identity,
    _identity_text,
    _new_root_path,
    _no_residue,
    _open_supervisor_sources,
    _owned_root_scope,
    _paths_have_no_live_references,
    _prepare_root_gate,
    _process_group_members,
    _reap_terminal_runner,
    _receipt_groups,
    _release_owned_descriptor,
    _remove_link,
    _remove_root,
    _replace_root_after_gate,
    _run_supervisor,
    _start_supervisor,
    _wait_runner,
    _write_sources,
)

GATE_DESCRIPTOR_COUNT = 4
SOURCE_DESCRIPTOR_COUNT = 3


class _CloseProbe:
    """Record one stream close and optionally inject its failure."""

    def __init__(self, *, fail: bool = False) -> None:
        self.closed = False
        self.fail = fail

    def close(self) -> None:
        """Mark the attempt before raising the configured failure."""
        self.closed = True
        if self.fail:
            message = "injected stream close failure"
            raise OSError(message)


class _StreamProcessProbe:
    """Expose the three stream fields consumed by the cleanup helper."""

    def __init__(self) -> None:
        self.stdin = _CloseProbe(fail=True)
        self.stdout = _CloseProbe()
        self.stderr = _CloseProbe()


def _terminal_is_retained(process: subprocess.Popen[bytes]) -> bool:
    """Report whether one terminal direct child remains waitable and unreaped."""
    try:
        result = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
    except ChildProcessError:
        return False
    return result is not None


def _spawn_descendant_runner() -> subprocess.Popen[bytes]:
    """Start a terminal leader whose live descendant retains its process group."""
    source = (
        "import os, signal, time\n"
        "child = os.fork()\n"
        "if child == 0:\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "    os.execl('/bin/sleep', '/bin/sleep', '30')\n"
        "time.sleep(0.1)\n"
        f"os._exit({runtime_mutations.DESCENDANT_STATUS})\n"
    )
    return subprocess.Popen(  # noqa: S603 -- current interpreter and fixed fixture source
        (sys.executable, "-B", "-I", "-S", "-c", source),
        start_new_session=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _runner_ownership_case() -> tuple[str, bool]:
    """Prove descendant cleanup and census failure retain leader authority."""
    base = _spawn_descendant_runner()
    base_status, base_clean, _stderr = _collect_runner(base)
    mutant = _spawn_descendant_runner()
    resolved = False
    refused = False
    try:
        try:
            _wait_runner(mutant, lambda _group: None)
        except RuntimeMutationError:
            refused = True
        retained = _terminal_is_retained(mutant)
        mutant_status, mutant_clean = _reap_terminal_runner(mutant)
        _close_runner_streams(mutant)
        resolved = True
    finally:
        if not resolved:
            _dispose_runner(mutant)
    return "runner census failure retains leader through descendant cleanup", (
        base_status == mutant_status == runtime_mutations.DESCENDANT_STATUS
        and base_clean
        and refused
        and retained
        and mutant_clean
    )


def _invalid_source_descriptor(sources: SourceBundle, invalid: str) -> tuple[str, bool]:
    """Prove one absent authenticated helper FD refuses before root effects."""
    supervisor, process_source, cases_source = sources
    root, identity = _create_root()
    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        baseline = {path.name for path in root.iterdir()}
        main_descriptor = os.open(main_path, os.O_RDONLY | os.O_NOFOLLOW)
        helper_path = cases_path if invalid == "process" else process_path
        helper_descriptor = os.open(helper_path, os.O_RDONLY | os.O_NOFOLLOW)
        process_descriptor = "2147483647" if invalid == "process" else str(helper_descriptor)
        cases_descriptor = "2147483647" if invalid == "cases" else str(helper_descriptor)
        try:
            runner = subprocess.Popen(  # noqa: S603 -- current interpreter and bound source FDs
                (
                    sys.executable,
                    "-B",
                    "-I",
                    "-S",
                    f"/proc/self/fd/{main_descriptor}",
                    "--process-fd",
                    process_descriptor,
                    "--cases-fd",
                    cases_descriptor,
                    "--selftest-missing-entry",
                    str(root),
                    _identity_text(root),
                ),
                pass_fds=(main_descriptor, helper_descriptor),
                start_new_session=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        finally:
            os.close(main_descriptor)
            os.close(helper_descriptor)
        status, clean, _stderr = _collect_runner(runner)
        passed = (
            status == runtime_mutations.PUBLIC_REFUSAL_STATUS
            and clean
            and {path.name for path in root.iterdir()} == baseline
            and _no_residue((root,))
        )
        return f"invalid {invalid} descriptor refuses before root effects", passed
    finally:
        _remove_root(root, identity)


def _descriptor_is_closed(descriptor: int) -> bool:
    """Return whether one exact descriptor no longer names an open file."""
    try:
        os.fstat(descriptor)
    except OSError:
        return True
    return False


def _descriptor_close_failure_case() -> tuple[str, bool]:
    """Prove ambiguous close failure cannot retain stale numeric authority."""
    failed, failed_peer = os.pipe2(os.O_CLOEXEC)
    later, later_peer = os.pipe2(os.O_CLOEXEC)
    owned = {failed, later}
    replacement = -1
    raised = False

    def close_reuse(descriptor: int) -> None:
        nonlocal replacement
        os.close(descriptor)
        if descriptor == failed:
            replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
            if replacement != descriptor:
                message = "released descriptor number was not reused"
                raise RuntimeMutationError(message)
            message = "injected ambiguous close failure"
            raise OSError(message)

    try:
        _close_owned_descriptors(owned, close_reuse)
    except OSError:
        raised = True
    finally:
        passed = (
            raised
            and not owned
            and replacement == failed
            and os.fstat(replacement) is not None
            and _descriptor_is_closed(later)
        )
        for descriptor in (replacement, failed_peer, later_peer):
            with suppress(OSError):
                os.close(descriptor)
    return "ambiguous descriptor close never regains reused numeric authority", passed


def _stream_close_failure_case() -> tuple[str, bool]:
    """Prove one stream-close error cannot skip the remaining streams."""
    process = _StreamProcessProbe()
    raised = False
    try:
        _close_runner_streams(process)
    except OSError:
        raised = True
    closed = process.stdin.closed and process.stdout.closed and process.stderr.closed
    return "stream close failure still attempts every owned stream", raised and closed


def _launcher_argv_case(sources: SourceBundle) -> tuple[str, bool]:
    """Bind the exact interpreter argv and three inherited source descriptors."""
    supervisor, process_source, cases_source = sources
    root, identity = _create_root()
    sentinel = object()
    try:
        paths = _write_sources(root, supervisor, process_source, cases_source)
        with patch.object(
            runtime_mutations.runtime_launcher.subprocess,
            "Popen",
            return_value=sentinel,
        ) as popen:
            result = runtime_mutations.runtime_launcher.launch(
                paths,
                ("request",),
                SupervisorStart(),
            )
        argv = popen.call_args.args[0]
        options = popen.call_args.kwargs
        descriptors = options["pass_fds"]
        exact = (
            result.process is sentinel
            and result.start_error is result.close_error is None
            and argv[:5]
            == (
                runtime_mutations.runtime_launcher.PYTHON_INTERPRETER,
                "-B",
                "-I",
                "-S",
                f"/proc/self/fd/{descriptors[0]}",
            )
            and argv[5:9]
            == ("--process-fd", str(descriptors[1]), "--cases-fd", str(descriptors[2]))
            and argv[9:] == ("request",)
            and len(descriptors) == SOURCE_DESCRIPTOR_COUNT
            and options["start_new_session"] is True
            and options["stdout"] is subprocess.PIPE
            and options["stderr"] is subprocess.PIPE
            and all(_descriptor_is_closed(descriptor) for descriptor in descriptors)
        )
        return "launcher binds exact isolated three-source interpreter argv", exact
    finally:
        _remove_root(root, identity)


def _source_open_failure_case(
    sources: SourceBundle, failure_index: int, source_name: str
) -> tuple[str, bool]:
    """Prove later source-open failure closes every predecessor FD."""
    supervisor, process_source, cases_source = sources
    root, identity = _create_root()
    opened: list[int] = []

    def fail_later(path: Path, flags: int) -> int:
        if len(opened) == failure_index:
            message = f"injected {source_name}-source open failure"
            raise OSError(message)
        descriptor = os.open(path, flags)
        opened.append(descriptor)
        return descriptor

    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        try:
            _open_supervisor_sources(main_path, process_path, cases_path, fail_later)
        except OSError:
            passed = len(opened) == failure_index and all(
                _descriptor_is_closed(descriptor) for descriptor in opened
            )
        else:
            passed = False
        return f"{source_name}-source open failure closes every predecessor FD", passed
    finally:
        _remove_root(root, identity)


def _first_source_open_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove first-source refusal has no descriptor cleanup authority."""
    root, identity = _create_root()
    supervisor, process_source, cases_source = sources
    close_attempts: list[int] = []

    def fail_first(_path: Path, _flags: int) -> int:
        message = "injected main-source open failure"
        raise OSError(message)

    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        try:
            _open_supervisor_sources(
                main_path,
                process_path,
                cases_path,
                fail_first,
                close_attempts.append,
            )
        except OSError as error:
            passed = str(error) == "injected main-source open failure" and not close_attempts
        else:
            passed = False
        return "main-source open failure owns no descriptor to close", passed
    finally:
        _remove_root(root, identity)


def _source_close_ambiguity_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove failed source cleanup never retries one reused descriptor number."""
    root, identity = _create_root()
    supervisor, process_source, cases_source = sources
    opened: list[int] = []
    replacement = -1

    def fail_second(path: Path, flags: int) -> int:
        if opened:
            message = "injected process-source open failure"
            raise OSError(message)
        descriptor = os.open(path, flags)
        opened.append(descriptor)
        return descriptor

    def close_reuse(descriptor: int) -> None:
        nonlocal replacement
        os.close(descriptor)
        replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
        if replacement != descriptor:
            message = "source descriptor number was not reused"
            raise RuntimeMutationError(message)
        message = "injected ambiguous source close failure"
        raise OSError(message)

    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        try:
            _open_supervisor_sources(main_path, process_path, cases_path, fail_second, close_reuse)
        except OSError as error:
            passed = (
                str(error) == "injected process-source open failure"
                and replacement == opened[0]
                and os.fstat(replacement) is not None
            )
        else:
            passed = False
        return "source-open failure preserves primary error without stale close retry", passed
    finally:
        with suppress(OSError):
            os.close(replacement)
        _remove_root(root, identity)


def _popen_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove every documented Popen failure closes all authenticated sources."""
    root, identity = _create_root()
    supervisor, process_source, cases_source = sources
    descriptors = Path("/proc/self/fd")
    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        failures = (
            OSError("injected Popen OS failure"),
            ValueError("injected Popen value failure"),
            subprocess.SubprocessError("injected Popen subprocess failure"),
        )
        outcomes = []
        for failure in failures:
            before = {entry.name for entry in descriptors.iterdir()}
            try:
                with patch.object(
                    runtime_mutations.runtime_launcher.subprocess,
                    "Popen",
                    side_effect=failure,
                ):
                    _start_supervisor(main_path, process_path, cases_path, ())
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                outcomes.append(type(error) is type(failure))
                outcomes.append({entry.name for entry in descriptors.iterdir()} == before)
            else:
                outcomes.append(False)
        return "every Popen failure closes all source FDs", all(outcomes)
    finally:
        _remove_root(root, identity)


def _popen_close_ambiguity_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove Popen failure remains primary when one source close is ambiguous."""
    root, identity = _create_root()
    supervisor, process_source, cases_source = sources
    replacement = -1
    first = True

    def close_reuse(descriptor: int) -> None:
        nonlocal first, replacement
        os.close(descriptor)
        if first:
            first = False
            replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
            if replacement != descriptor:
                message = "Popen source descriptor number was not reused"
                raise RuntimeMutationError(message)
            message = "injected ambiguous Popen-source close failure"
            raise OSError(message)

    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        message = "injected supervisor Popen failure"
        try:
            with patch.object(
                runtime_mutations.runtime_launcher.subprocess,
                "Popen",
                side_effect=OSError(message),
            ):
                _start_supervisor(
                    main_path,
                    process_path,
                    cases_path,
                    (),
                    SupervisorStart(descriptor_closer=close_reuse),
                )
        except OSError as error:
            passed = (
                str(error) == message and replacement >= 0 and os.fstat(replacement) is not None
            )
        else:
            passed = False
        return "Popen failure preserves primary error without stale close retry", passed
    finally:
        with suppress(OSError):
            os.close(replacement)
        _remove_root(root, identity)


def _descriptor_reuse_case() -> tuple[str, bool]:
    """Prove final cleanup cannot close one released and reused FD number."""
    root: Path | None = None
    identity: tuple[int, int] | None = None
    owned: set[int] = set()
    replacement = -1
    raised = False

    def close_reuse(descriptor: int) -> None:
        nonlocal replacement
        os.close(descriptor)
        replacement = os.open(os.devnull, os.O_RDONLY | os.O_CLOEXEC)
        if replacement != descriptor:
            message = "released descriptor number was not reused"
            raise RuntimeMutationError(message)
        message = "injected ambiguous release failure"
        raise OSError(message)

    try:
        root, identity = _create_root()
        retained, released = os.pipe2(os.O_CLOEXEC)
        owned.update((retained, released))
        saved = _new_root_path()
        try:
            _release_owned_descriptor(owned, released, close_reuse)
        except OSError:
            raised = True
        _finalize_root_swap(None, owned, (root, saved), (identity, None), (True, False, set()))
        passed = raised and replacement == released and os.fstat(replacement) is not None
        return "released descriptor reuse remains outside cleanup authority", passed
    finally:
        for descriptor in (*owned, replacement):
            with suppress(OSError):
                os.close(descriptor)
        if root is not None and identity is not None and root.exists():
            _remove_root(root, identity)


def _pipe_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove a second-pipe failure closes the already acquired pair."""
    root, identity = _create_root()
    opened: list[int] = []

    def fail_second(flags: int) -> tuple[int, int]:
        if opened:
            message = "injected second pipe failure"
            raise OSError(message)
        pair = os.pipe2(flags)
        opened.extend(pair)
        return pair

    raised = False
    try:
        _prepare_root_gate(sources, "pre-open", root, hooks=(fail_second, _write_sources))
    except OSError:
        raised = True
    finally:
        closed = bool(opened) and all(_descriptor_is_closed(item) for item in opened)
        _remove_root(root, identity)
    return "second pipe failure closes the first owned pair", raised and closed


def _first_pipe_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove first-pipe refusal leaves no descriptor or root residue."""
    root, identity = _create_root()
    descriptors = Path("/proc/self/fd")
    before = {entry.name for entry in descriptors.iterdir()}

    def fail_first(_flags: int) -> tuple[int, int]:
        message = "injected first pipe failure"
        raise OSError(message)

    try:
        try:
            _prepare_root_gate(sources, "pre-open", root, hooks=(fail_first, _write_sources))
        except OSError as error:
            passed = (
                str(error) == "injected first pipe failure"
                and {entry.name for entry in descriptors.iterdir()} == before
            )
        else:
            passed = False
        return "first pipe failure owns no descriptor to close", passed
    finally:
        _remove_root(root, identity)


def _source_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove source preparation failure closes all four gate descriptors."""
    root, identity = _create_root()
    opened: list[int] = []

    def tracked_pipe(flags: int) -> tuple[int, int]:
        pair = os.pipe2(flags)
        opened.extend(pair)
        return pair

    def fail_source(
        _root: Path, _supervisor: str, _process: str, _cases: str
    ) -> tuple[Path, Path, Path]:
        message = "injected source preparation failure"
        raise RuntimeMutationError(message)

    raised = False
    try:
        _prepare_root_gate(
            sources,
            "pre-open",
            root,
            hooks=(tracked_pipe, fail_source),
        )
    except RuntimeMutationError:
        raised = True
    finally:
        closed = len(opened) == GATE_DESCRIPTOR_COUNT and all(
            _descriptor_is_closed(item) for item in opened
        )
        _remove_root(root, identity)
    return "source preparation failure closes every gate descriptor", raised and closed


def _root_acquisition_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove gate preparation failure cannot leak its newly allocated root."""
    observed: list[Path] = []

    def fail_gate(
        _sources: SourceBundle, _phase: str, root: Path
    ) -> tuple[Path, Path, Path, dict[str, str], tuple[int, int, int, int], set[int]]:
        observed.append(root)
        message = "injected gate preparation failure"
        raise RuntimeMutationError(message)

    raised = False
    try:
        _replace_root_after_gate(
            sources,
            phase="pre-open",
            hooks=(fail_gate, _new_root_path),
        )
    except RuntimeMutationError:
        raised = True
    return "gate preparation failure removes its allocated root", (
        raised and len(observed) == 1 and not observed[0].exists()
    )


def _root_validation_exception_case() -> tuple[str, bool]:
    before = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}
    validation_raised = False
    try:
        with patch.object(
            runtime_mutations,
            "_root_path_is_safe",
            side_effect=RuntimeMutationError("injected root validation exception"),
        ):
            _create_root()
    except RuntimeMutationError:
        validation_raised = True
    after_validation = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}
    return "root validation exception removes its just-created root", (
        validation_raised and after_validation == before
    )


def _mkdir_race_case() -> tuple[str, bool]:
    mkdir_target = _new_root_path()
    mkdir = Path.mkdir

    def create_then_fail(*_args: object, **kwargs: object) -> None:
        mkdir(mkdir_target, **kwargs)
        message = "injected post-mkdir exception"
        raise RuntimeMutationError(message)

    try:
        with (
            patch.object(runtime_mutations, "_new_root_path", return_value=mkdir_target),
            patch.object(Path, "mkdir", side_effect=create_then_fail),
        ):
            _create_root()
    except RuntimeMutationError:
        pass
    mkdir_survives = mkdir_target.exists() and not mkdir_target.is_symlink()
    if mkdir_target.exists():
        mkdir_target.rmdir()
    return "mkdir exception preserves a same-owner competing root", mkdir_survives


def _identity_replacement_case() -> tuple[str, bool]:
    identity_target = _new_root_path()
    identity_backup = identity_target.with_name(f"identity-original-{identity_target.name}")
    identity_raised = False

    def replace_then_fail(path: Path) -> None:
        path.rename(identity_backup)
        path.mkdir(mode=runtime_mutations.PRIVATE_MODE)
        message = "injected root identity replacement exception"
        raise RuntimeMutationError(message)

    try:
        with (
            patch.object(runtime_mutations, "_new_root_path", return_value=identity_target),
            patch.object(runtime_mutations, "_identity", side_effect=replace_then_fail),
        ):
            _create_root()
    except RuntimeMutationError:
        identity_raised = True
    identity_survives = identity_target.exists() and not identity_target.is_symlink()
    if identity_target.exists():
        identity_target.rmdir()
    if identity_backup.exists():
        identity_backup.rmdir()
    return "root identity replacement survives without captured identity", (
        identity_raised and identity_survives
    )


def _replacement_identity_case() -> tuple[str, bool]:
    replacement_path = _new_root_path()
    replacement_raised = False
    try:
        with patch.object(
            runtime_mutations,
            "_identity",
            side_effect=RuntimeMutationError("injected replacement identity exception"),
        ):
            _create_replacement_root(replacement_path)
    except RuntimeMutationError:
        replacement_raised = True
    replacement_survives = replacement_path.exists() and not replacement_path.is_symlink()
    if replacement_path.exists():
        replacement_path.rmdir()
    return "replacement identity exception preserves its root", (
        replacement_raised and replacement_survives
    )


def _outer_root_exception_case() -> tuple[str, bool]:
    before = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}
    outer_raised = False
    try:
        with _owned_root_scope():
            _create_root()
            message = "injected outer runtime exception"
            raise RuntimeMutationError(message)  # noqa: TRY301 -- injected outer-boundary failure.
    except RuntimeMutationError:
        outer_raised = True
    after_outer = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}
    return "outer runtime exception drains only owned roots", outer_raised and after_outer == before


def _root_allocation_exception_cases() -> list[tuple[str, bool]]:
    """Prove every post-mkdir exception leaves the owned-root set unchanged."""
    return [
        _root_validation_exception_case(),
        _mkdir_race_case(),
        _identity_replacement_case(),
        _replacement_identity_case(),
        _outer_root_exception_case(),
    ]


def _root_removal_mutation_case() -> tuple[str, bool]:
    """Prove root cleanup refuses a wrong identity and a final symlink."""
    root, identity = _create_root()
    saved = _new_root_path()
    wrong_identity = identity[0], identity[1] + 1
    wrong_refused = False
    symlink_refused = False
    try:
        try:
            _remove_root(root, wrong_identity)
        except RuntimeMutationError:
            wrong_refused = root.is_dir() and _identity(root) == identity
        root.rename(saved)
        root.symlink_to(saved)
        try:
            _remove_root(root, identity)
        except RuntimeMutationError:
            symlink_refused = root.is_symlink() and _identity(saved) == identity
        root.unlink()
        _remove_root(saved, identity)
        return "root removal refuses wrong identity and symlink mutations", (
            wrong_refused and symlink_refused
        )
    finally:
        if root.is_symlink():
            root.unlink()
        if saved.exists():
            _remove_root(saved, identity)


def _saved_path_failure_case(sources: SourceBundle) -> tuple[str, bool]:
    """Prove second-path acquisition failure removes the first suite root."""
    before = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}

    def fail_saved() -> Path:
        message = "injected saved-path acquisition failure"
        raise RuntimeMutationError(message)

    raised = False
    try:
        _replace_root_after_gate(
            sources,
            phase="pre-open",
            hooks=(_prepare_root_gate, fail_saved),
        )
    except RuntimeMutationError:
        raised = True
    after = {path.name for path in CANONICAL_TMP.glob(f"{ROOT_PREFIX}*")}
    return "saved-path acquisition failure removes the first suite root", raised and before == after


def _replacement_cases(sources: SourceBundle) -> list[tuple[str, bool]]:
    """Prove pre-open refusal and retained post-open root authority."""
    return [
        (
            "pre-open suite-root replacement refuses with zero effects",
            _replace_root_after_gate(sources, phase="pre-open") == (USAGE_STATUS, True, True, True),
        ),
        (
            "post-open suite-root replacement stays descriptor-bound",
            _replace_root_after_gate(sources, phase="post-open")
            == (INTEGRITY_REFUSAL_STATUS, True, True, True),
        ),
        (
            "root-swap exception closes runner, descriptors, and paths",
            _replace_root_after_gate(sources, phase="pre-open", inject_after_rename=True)
            == (None, True, True, True),
        ),
    ]


def _invalid_root_cases(sources: SourceBundle) -> list[tuple[str, bool]]:
    """Prove wrong-identity and symlink roots have zero effects."""
    supervisor, process_source, cases_source = sources
    target: Path | None = None
    target_identity: tuple[int, int] | None = None
    link: Path | None = None
    link_identity: tuple[int, int] | None = None
    try:
        target, target_identity = _create_root()
        link = _new_root_path()
        main_path, process_path, cases_path = _write_sources(
            target, supervisor, process_source, cases_source
        )
        baseline = {path.name for path in target.iterdir()}
        wrong_status, wrong_clean, _stderr = _run_supervisor(
            main_path,
            process_path,
            cases_path,
            ("--selftest-missing-entry", str(target), "0:0"),
        )
        wrong_passed = (
            wrong_status == USAGE_STATUS
            and wrong_clean
            and {path.name for path in target.iterdir()} == baseline
        )
        link.symlink_to(target)
        link_identity = _identity(link)
        link_status, link_clean, _stderr = _run_supervisor(
            main_path,
            process_path,
            cases_path,
            ("--selftest-missing-entry", str(link), _identity_text(link)),
        )
        link_passed = (
            link_status == USAGE_STATUS
            and link_clean
            and {path.name for path in target.iterdir()} == baseline
        )
        return [
            ("wrong suite-root identity refuses before effects", wrong_passed),
            ("symlink suite root refuses before effects", link_passed),
            (
                "invalid-root runtime leaves no process or descriptor residue",
                _no_residue((target, link)),
            ),
        ]
    finally:
        if link is not None and link_identity is not None and link.is_symlink():
            _remove_link(link, link_identity)
        if target is not None and target_identity is not None and target.exists():
            _remove_root(target, target_identity)


def _procfs_unknown_cases() -> list[tuple[str, bool]]:
    """Prove malformed or unreadable Linux observations retain authority."""
    group = os.getpgrp()
    with patch.object(Path, "read_bytes", side_effect=PermissionError("injected stat denial")):
        denied_stat = _process_group_members(group) is None
    with patch.object(Path, "read_bytes", return_value=b"malformed"):
        malformed_stat = _process_group_members(group) is None
        malformed_command = not _paths_have_no_live_references((CANONICAL_TMP,))
    with patch.object(Path, "readlink", side_effect=PermissionError("injected fd denial")):
        denied_descriptor = not _paths_have_no_live_references((CANONICAL_TMP,))
    return [
        ("unreadable process stat retains process-group authority", denied_stat),
        ("malformed process stat retains process-group authority", malformed_stat),
        ("malformed process command retains path authority", malformed_command),
        ("unreadable process descriptor retains path authority", denied_descriptor),
    ]


def _receipt_unknown_cases() -> list[tuple[str, bool]]:
    """Prove malformed and unreadable receipts cannot erase group authority."""
    root, identity = _create_root()
    receipt = root / "unknown.bound"
    try:
        receipt.write_bytes(b"not-a-process-group\n")
        malformed = _receipt_groups((root,)) is None
        with patch.object(
            Path, "read_bytes", side_effect=PermissionError("injected receipt denial")
        ):
            denied = _receipt_groups((root,)) is None
        receipt.unlink()
        missing = _receipt_groups((root / "absent",)) == set()
        return [
            ("malformed group receipt retains process authority", malformed),
            ("unreadable group receipt retains process authority", denied),
            ("vanished receipt root is the only skipped observation", missing),
        ]
    finally:
        if receipt.exists():
            receipt.unlink()
        _remove_root(root, identity)


def _receipt_scoped_cleanup_cases() -> list[tuple[str, bool]]:
    """Prove unrelated procfs denial cannot hide a receipt-bound live group."""
    root, identity = _create_root()
    receipt = root / "live.bound"
    child: subprocess.Popen[bytes] | None = None
    try:
        with patch.object(Path, "read_bytes", side_effect=PermissionError("injected denial")):
            strict_unknown = not _paths_have_no_live_references((root,))
            unrelated_does_not_wedge = _no_residue((root,))
        child = subprocess.Popen(
            ("/bin/sleep", "30"),
            cwd=root,
            start_new_session=True,
        )
        receipt.write_text(f"{child.pid}\n", encoding="ascii")
        live_refuses = not _no_residue((root,))
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=runtime_mutations.RESIDUE_TIMEOUT_SECONDS)
        gone_accepts = _no_residue((root,))
        return [
            (
                "unrelated unreadable procfs state does not gate receipt-scoped cleanup",
                strict_unknown and unrelated_does_not_wedge,
            ),
            (
                "receipt-bound descendant cwd reference prevents cleanup until group absence",
                live_refuses and gone_accepts,
            ),
        ]
    finally:
        if child is not None and child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=runtime_mutations.RESIDUE_TIMEOUT_SECONDS)
        if receipt.exists():
            receipt.unlink()
        _remove_root(root, identity)


def cases(inputs: dict[str, str]) -> list[tuple[str, bool]]:
    """Run every cleanup, acquisition, replacement, and invalid-root proof."""
    supervisor = inputs["devcontainer_image_selftest_supervisor"]
    process_source = inputs["devcontainer_image_selftest_process"]
    supervisor_cases = inputs["devcontainer_image_selftest_supervisor_cases"]
    sources = supervisor, process_source, supervisor_cases
    with _owned_root_scope():
        return [
            *runtime_loader.cases(inputs),
            _runner_ownership_case(),
            _invalid_source_descriptor(sources, "process"),
            _invalid_source_descriptor(sources, "cases"),
            _descriptor_close_failure_case(),
            _stream_close_failure_case(),
            _launcher_argv_case(sources),
            _first_source_open_failure_case(sources),
            _source_open_failure_case(sources, 1, "process"),
            _source_open_failure_case(sources, 2, "cases"),
            _source_close_ambiguity_case(sources),
            _popen_failure_case(sources),
            _popen_close_ambiguity_case(sources),
            _descriptor_reuse_case(),
            _first_pipe_failure_case(sources),
            _pipe_failure_case(sources),
            _source_failure_case(sources),
            _root_acquisition_failure_case(sources),
            *_root_allocation_exception_cases(),
            _root_removal_mutation_case(),
            _saved_path_failure_case(sources),
            *_replacement_cases(sources),
            *_invalid_root_cases(sources),
            *_procfs_unknown_cases(),
            *_receipt_unknown_cases(),
            *_receipt_scoped_cleanup_cases(),
        ]
