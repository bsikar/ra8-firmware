#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Hostile HOME Git-policy regressions for the immutable pre-commit owner."""

from __future__ import annotations

import subprocess
from collections.abc import Callable
from pathlib import Path
from typing import Any

from scripts.dev.git_environment import trusted_git_executable


class HostileGitPolicyError(RuntimeError):
    """The hook executed inherited Git policy or lost its refusal path."""


def _fail(message: str) -> None:
    raise HostileGitPolicyError(message)


def _write(path: Path, text: str, *, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


def _hostile_home_policy(
    base: Path, transport_environment: Callable[[Path, Path], dict[str, str]]
) -> tuple[dict[str, str], tuple[Path, ...]]:
    """Create ordinary HOME policy covering filters, templates, and fsmonitor."""
    home = base / "hostile-home"
    template = home / "template/hooks"
    template.mkdir(parents=True)
    markers = tuple(base / name for name in ("filter.ran", "template.ran", "fsmonitor.ran"))
    filter_helper = home / "filter.sh"
    fsmonitor_helper = home / "fsmonitor.sh"
    _write(filter_helper, f"#!/bin/sh\nprintf x >>{markers[0]}\ncat\n", executable=True)
    _write(
        template / "reference-transaction",
        f"#!/bin/sh\nprintf x >>{markers[1]}\ncat >/dev/null\n",
        executable=True,
    )
    _write(
        fsmonitor_helper,
        f"#!/bin/sh\nprintf x >>{markers[2]}\nprintf '\n'\n",
        executable=True,
    )
    attributes = home / "attributes"
    _write(attributes, "* filter=evil\n")
    _write(
        home / ".gitconfig",
        "[core]\n"
        f"\tattributesFile = {attributes}\n\tfsmonitor = {fsmonitor_helper}\n"
        f"[init]\n\ttemplateDir = {template.parent}\n"
        f'[filter "evil"]\n\tclean = {filter_helper}\n'
        f"\tsmudge = {filter_helper}\n\trequired = true\n",
    )
    environment = transport_environment(base, base)
    for name in tuple(environment):
        if name.startswith("GIT_CONFIG_") or name == "GIT_ATTR_NOSYSTEM":
            environment.pop(name)
    environment.update(HOME=str(home), XDG_CONFIG_HOME=str(home / "xdg"))
    return environment, markers


def _prove_hostile_home_is_live(
    base: Path, environment: dict[str, str], markers: tuple[Path, ...]
) -> None:
    """Prove every planted ordinary HOME helper executes without the repair."""
    root = base / "unprotected-home-probe"
    root.mkdir()
    _write(root / "probe.txt", "probe\n")
    commands = (
        ("init", "--quiet"),
        ("add", "probe.txt"),
        ("status", "--porcelain"),
        (
            "-c",
            "user.email=selftest@invalid",
            "-c",
            "user.name=selftest",
            "commit",
            "--quiet",
            "-m",
            "probe",
        ),
    )
    for args in commands:
        proc = subprocess.run(  # noqa: S603 -- fixed Git executable and hostile fixture argv
            [trusted_git_executable(), "-C", str(root), *args],
            env=environment,
            capture_output=True,
            check=False,
        )
        if proc.returncode != 0:
            _fail(f"hostile HOME probe did not execute {args[-1]}: {proc.stderr!r}")
    if any(not marker.exists() for marker in markers):
        _fail("hostile HOME probe did not activate filter, template, and fsmonitor")
    for marker in markers:
        marker.unlink()


def _install_core_wrappers(directory: Path) -> None:
    """Install source/PATH core-utility attacks used by the real owner test."""
    for name in ("cp", "mkdir", "mktemp", "ln", "readlink", "rm"):
        _write(
            directory / name,
            "#!/bin/bash -p\n"
            'printf "ran\\n" >"${RA8_PATH_MARKER_DIR:?}/${RA8_PATH_MARKER_PREFIX:?}.${0##*/}"\n'
            "exit 73\n",
            executable=True,
        )


def _prove_core_wrappers_live(directory: Path, marker_dir: Path, prefix: str) -> None:
    """Prove the hostile PATH would select every planted wrapper."""
    expected_return = 73
    for name in ("cp", "mkdir", "mktemp", "ln", "readlink", "rm"):
        environment = {
            "PATH": f"{directory}:/usr/bin:/bin",
            "RA8_PATH_MARKER_DIR": str(marker_dir),
            "RA8_PATH_MARKER_PREFIX": prefix,
        }
        result = subprocess.run(  # noqa: S603 -- must-fire private PATH fixture
            ["/bin/bash", "-p", "-c", name],
            env=environment,
            capture_output=True,
            check=False,
        )
        marker = marker_dir / f"{prefix}.{name}"
        if result.returncode != expected_return or not marker.is_file():
            _fail(f"hostile core-utility control did not execute {prefix}/{name}")
        marker.unlink()


def _hostile_owner_path_case(
    base: Path,
    callbacks: tuple[Callable[..., Any], ...],
) -> None:
    """Prove source-local and arbitrary PATH core utilities cannot become owner tools."""
    make_fixture, _git, source_state, run_owner, transport_environment = callbacks
    root, temp_root = base / "hostile-owner-path", base / "hostile-owner-path-tmp"
    arbitrary = base / "arbitrary-path"
    marker_dir = base / "path-markers"
    root.mkdir()
    temp_root.mkdir()
    arbitrary.mkdir()
    marker_dir.mkdir()
    make_fixture(root, "success", "failure")
    source_bin = root / ".venv/bin"
    source_bin.mkdir(parents=True, exist_ok=True)
    _install_core_wrappers(source_bin)
    _install_core_wrappers(arbitrary)
    _prove_core_wrappers_live(source_bin, marker_dir, "source")
    _prove_core_wrappers_live(arbitrary, marker_dir, "arbitrary")
    before = source_state(root)
    environment = transport_environment(base, root)
    environment.update(
        PATH=f"{source_bin}:{arbitrary}:{environment.get('PATH', '')}",
        RA8_PATH_MARKER_DIR=str(marker_dir),
        RA8_PATH_MARKER_PREFIX="attack",
    )
    result = run_owner(root, temp_root, environment)
    if result.returncode:
        _fail(f"owner core-utility isolation case failed: {result.stderr}")
    if tuple(marker_dir.iterdir()):
        _fail("immutable owner executed a source-local or arbitrary PATH core utility")
    if source_state(root) != before or tuple(temp_root.iterdir()):
        _fail("owner core-utility isolation changed source state or left residue")


def run_hostile_owner_cases(
    base: Path,
    callbacks: tuple[Callable[..., Any], ...],
) -> None:
    """Prove the real owner ignores HOME policy and rejects staged drivers."""
    make_fixture, git, source_state, run_owner, transport_environment = callbacks
    _hostile_owner_path_case(base, callbacks)
    environment, markers = _hostile_home_policy(base, transport_environment)
    _prove_hostile_home_is_live(base, environment, markers)
    for name, hostile_attributes, expected in (
        ("hostile-home-success", False, 0),
        ("hostile-candidate-attribute", True, 1),
    ):
        root, temp_root = base / name, base / f"{name}-tmp"
        root.mkdir()
        temp_root.mkdir()
        make_fixture(root, "success", "failure")
        (root / ".venv/bin").mkdir(parents=True)
        case_environment = {**environment, "RA8_SELFTEST_VENV_BIN": str(root / ".venv/bin")}
        if hostile_attributes:
            _write(root / ".gitattributes", "* filter=evil\n")
            git(root, "add", ".gitattributes")
        before = source_state(root)
        result = run_owner(root, temp_root, case_environment)
        if result.returncode != expected:
            detail = result.stderr or result.stdout
            _fail(f"{name}: expected {expected}, got {result.returncode}: {detail}")
        executed = tuple(marker.name for marker in markers if marker.exists())
        if executed:
            _fail(f"{name}: owner executed ordinary HOME Git policy: {executed}")
        if source_state(root) != before or tuple(temp_root.iterdir()):
            _fail(f"{name}: owner changed source state or left residue")
