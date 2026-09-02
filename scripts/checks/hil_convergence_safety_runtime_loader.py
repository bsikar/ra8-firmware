# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Source-only loader proofs for image-supervisor runtime mutations."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import hil_convergence_safety_runtime_loader_harness as loader_harness
import hil_convergence_safety_runtime_mutations as runtime_mutations
from hil_convergence_safety_runtime_mutations import (
    RuntimeMutationError,
    SourceBundle,
    _create_root,
    _identity_text,
    _no_residue,
    _owned_root_scope,
    _remove_root,
    _run_supervisor,
    _write_sources,
)


def _one_shot_result(
    main_path: Path, process_path: Path, cases_path: Path
) -> subprocess.CompletedProcess[bytes]:
    """Load each private API, then require the cases globals to reject re-exec."""
    return loader_harness.run(
        main_path, process_path, cases_path, runtime_mutations.RESIDUE_TIMEOUT_SECONDS
    )


def _one_shot_case(root: Path, sources: SourceBundle) -> tuple[bool, frozenset[str]]:
    """Require base refusal and prove deleting consumption makes the negative fire."""
    supervisor, process_source, cases = sources
    main_path, process_path, cases_path = _write_sources(root, supervisor, process_source, cases)
    baseline = frozenset(path.name for path in root.iterdir())
    base = _one_shot_result(main_path, process_path, cases_path)
    source_guard = 'globals().get("_RA8_SUPERVISOR_CASES_VERSION")'
    if cases.count(source_guard) != 1:
        message = "supervisor cases one-shot source guard is not unique"
        raise RuntimeMutationError(message)
    mutant_main, mutant_process, mutant_cases = _write_sources(
        root,
        supervisor,
        process_source,
        cases.replace(source_guard, "CASES_LOAD_VERSION"),
    )
    mutation = _one_shot_result(mutant_main, mutant_process, mutant_cases)
    exact = (
        base.returncode == 0
        and base.stdout == base.stderr == b""
        and mutation.returncode == runtime_mutations.ONE_SHOT_MUTATION_STATUS
        and mutation.stdout == mutation.stderr == b""
    )
    return exact, baseline


def _source_only_cases(sources: SourceBundle) -> list[tuple[str, bool]]:
    """Prove the exact source-only diagnostic and loader sentinel."""
    supervisor, process_source, cases = sources
    root, identity = _create_root()
    try:
        _main_path, _process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases
        )
        descriptor = os.open(cases_path, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            direct = subprocess.run(  # noqa: S603 -- current interpreter and bound source FD
                (sys.executable, "-B", "-I", "-S", f"/proc/self/fd/{descriptor}"),
                pass_fds=(descriptor,),
                capture_output=True,
                timeout=runtime_mutations.RESIDUE_TIMEOUT_SECONDS,
                check=False,
            )
        finally:
            os.close(descriptor)
        one_shot, baseline = _one_shot_case(root, sources)
        source_exact = (
            direct.returncode == 1
            and direct.stderr.count(b"RuntimeError: supervisor cases module is source-only") == 1
        )
        sentinel = '            "_RA8_SUPERVISOR_CASES_VERSION": 1,\n'
        if supervisor.count(sentinel) != 1:
            message = "supervisor cases load sentinel is not unique"
            raise RuntimeMutationError(message)
        mutant_main, mutant_process, mutant_cases = _write_sources(
            root, supervisor.replace(sentinel, ""), process_source, cases
        )
        names = {path.name for path in root.iterdir()}
        status, clean, _stderr = _run_supervisor(
            mutant_main,
            mutant_process,
            mutant_cases,
            ("--selftest-missing-entry", str(root), _identity_text(root)),
        )
        sentinel_refused = (
            status == runtime_mutations.PUBLIC_REFUSAL_STATUS
            and clean
            and {path.name for path in root.iterdir()} == names
        )
        return [
            ("supervisor cases source-only diagnostic is exact", source_exact),
            (
                "supervisor cases grant is consumed before exact re-exec refusal",
                one_shot and frozenset(path.name for path in root.iterdir()) == baseline,
            ),
            ("supervisor cases load sentinel removal refuses before effects", sentinel_refused),
            ("source-only runtime leaves no process or descriptor residue", _no_residue((root,))),
        ]
    finally:
        _remove_root(root, identity)


def cases(inputs: dict[str, str]) -> list[tuple[str, bool]]:
    """Run source-only, loader collision, removal, and repeat-load proofs."""
    sources = (
        inputs["devcontainer_image_selftest_supervisor"],
        inputs["devcontainer_image_selftest_process"],
        inputs["devcontainer_image_selftest_supervisor_cases"],
    )
    with _owned_root_scope():
        return _source_only_cases(sources)
