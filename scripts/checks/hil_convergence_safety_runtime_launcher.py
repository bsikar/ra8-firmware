# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Transactional three-source launcher for image-supervisor runtime proofs."""

from __future__ import annotations

import os
import subprocess
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

DescriptorCloser = Callable[[int], None]
PYTHON_INTERPRETER = "/usr/bin/python3"


@dataclass(frozen=True)
class SupervisorStart:
    """Optional launch authorities kept together at the Popen boundary."""

    extra_descriptors: tuple[int, ...] = ()
    environment: dict[str, str] | None = None
    source_opener: Callable[[Path, int], int] = os.open
    descriptor_closer: DescriptorCloser = os.close


@dataclass(frozen=True)
class LaunchResult:
    """Retain a started runner and both errors for caller-owned resolution."""

    process: subprocess.Popen[bytes] | None
    start_error: OSError | ValueError | subprocess.SubprocessError | None
    close_error: OSError | None


def close_owned_descriptors(descriptors: set[int], closer: DescriptorCloser = os.close) -> None:
    """Attempt each close once after irrevocably releasing numeric authority."""
    first_error: OSError | None = None
    for descriptor in sorted(descriptors):
        descriptors.remove(descriptor)
        try:
            closer(descriptor)
        except OSError as error:
            if first_error is None:
                first_error = error
    if first_error is not None:
        raise first_error


def open_supervisor_sources(
    main_path: Path,
    process_path: Path,
    cases_path: Path,
    opener: Callable[[Path, int], int] = os.open,
    closer: DescriptorCloser = os.close,
) -> tuple[int, int, int, set[int]]:
    """Acquire all source descriptors or exhaustively release predecessors."""
    owned: set[int] = set()
    try:
        main_descriptor = opener(main_path, os.O_RDONLY | os.O_NOFOLLOW)
        owned.add(main_descriptor)
        process_descriptor = opener(process_path, os.O_RDONLY | os.O_NOFOLLOW)
        owned.add(process_descriptor)
        cases_descriptor = opener(cases_path, os.O_RDONLY | os.O_NOFOLLOW)
        owned.add(cases_descriptor)
    except OSError as open_error:
        try:
            close_owned_descriptors(owned, closer)
        except OSError as cleanup_error:
            raise open_error from cleanup_error
        raise
    return main_descriptor, process_descriptor, cases_descriptor, owned


def _spawn(
    descriptors: tuple[int, ...],
    arguments: tuple[str, ...],
    environment: dict[str, str] | None,
) -> subprocess.Popen[bytes]:
    """Spawn the fixed interpreter with three bound source descriptors."""
    main_descriptor, process_descriptor, cases_descriptor = descriptors[:3]
    return subprocess.Popen(  # noqa: S603 -- current interpreter and bound source FDs
        (
            PYTHON_INTERPRETER,
            "-B",
            "-I",
            "-S",
            f"/proc/self/fd/{main_descriptor}",
            "--process-fd",
            str(process_descriptor),
            "--cases-fd",
            str(cases_descriptor),
            *arguments,
        ),
        pass_fds=descriptors,
        start_new_session=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )


def launch(
    paths: tuple[Path, Path, Path],
    arguments: tuple[str, ...],
    config: SupervisorStart,
) -> LaunchResult:
    """Open, spawn, and release every authenticated source exactly once."""
    main_path, process_path, cases_path = paths
    main, process_source, cases, owned = open_supervisor_sources(
        main_path,
        process_path,
        cases_path,
        config.source_opener,
        config.descriptor_closer,
    )
    descriptors = main, process_source, cases, *config.extra_descriptors
    process: subprocess.Popen[bytes] | None = None
    start_error: OSError | ValueError | subprocess.SubprocessError | None = None
    close_error: OSError | None = None
    try:
        process = _spawn(descriptors, arguments, config.environment)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        start_error = error
    finally:
        try:
            close_owned_descriptors(owned, config.descriptor_closer)
        except OSError as error:
            close_error = error
    return LaunchResult(process, start_error, close_error)
