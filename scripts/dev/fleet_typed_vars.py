# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Validate and privately snapshot typed Ansible variables for fleet operations."""

from __future__ import annotations

import os
import stat
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager, suppress
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import NoReturn
from unittest.mock import patch

import fleet_model as fm
import yaml

RUNNER_REGISTRATION = "runner registration"
HIL_REGISTRATION = "HIL registration"
RUNNER_REMOVAL = "runner removal"

RUNNER_REGISTRATION_KEYS = frozenset({"ci_runner_docker_registration_token"})
HIL_REGISTRATION_KEYS = frozenset({"dev_box_hil_runner_registration_token"})
RUNNER_REMOVAL_KEYS = frozenset(
    {"ci_runner_docker_removal_token", "ci_runner_docker_destroy_dataset"}
)
TYPED_VAR_KEYS = MappingProxyType(
    {
        RUNNER_REGISTRATION: RUNNER_REGISTRATION_KEYS,
        HIL_REGISTRATION: HIL_REGISTRATION_KEYS,
        RUNNER_REMOVAL: RUNNER_REMOVAL_KEYS,
    }
)
EXACT_KEY_OPERATIONS = frozenset({RUNNER_REGISTRATION, HIL_REGISTRATION})
TOKEN_KEYS = frozenset(
    {
        "ci_runner_docker_registration_token",
        "ci_runner_docker_removal_token",
        "dev_box_hil_runner_registration_token",
    }
)
CONTAINER_RUNNER_CLASSES = frozenset({"docker_linux", "docker_wsl"})
CONTAINER_RUNNER_PLAYS = frozenset({"ci-runner-docker", "wsl-ci-host"})
MAX_TYPED_VARS_BYTES = 64 * 1024
PRIVATE_FILE_MODE = stat.S_IRUSR | stat.S_IWUSR


@dataclass(frozen=True)
class TypedVars:
    """Validated, caller-owned Ansible variables captured before side effects."""

    source: Path
    content: bytes


def _raise_fleet_error(message: str, cause: Exception | None = None) -> NoReturn:
    """Raise one fleet precondition error, preserving an optional OS cause."""
    if cause is not None:
        raise fm.FleetError(message) from cause
    raise fm.FleetError(message)


def _read_owned_vars_content(candidate: Path, operation: str) -> bytes:
    """Read one bounded, owned, mode-0600 regular file without following it."""
    if candidate.is_symlink():
        _raise_fleet_error(f"{operation} vars file must not be a symlink: {candidate}")
    flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOFOLLOW", 0)
        | getattr(os, "O_NONBLOCK", 0)
    )
    try:
        fd = os.open(candidate, flags)
    except OSError as exc:
        _raise_fleet_error(
            f"{operation} vars file is not a readable non-symlink: {candidate}: {exc}", exc
        )
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            _raise_fleet_error(f"{operation} vars file is not a regular file: {candidate}")
        if info.st_size > MAX_TYPED_VARS_BYTES:
            _raise_fleet_error(
                f"{operation} vars file exceeds {MAX_TYPED_VARS_BYTES} bytes: {candidate}"
            )
        if info.st_uid != os.geteuid():
            _raise_fleet_error(
                f"{operation} vars file must be owned by uid {os.geteuid()}: {candidate}"
            )
        if stat.S_IMODE(info.st_mode) != PRIVATE_FILE_MODE:
            _raise_fleet_error(f"{operation} vars file must be mode 0600: {candidate}")
        with os.fdopen(fd, "rb", closefd=True) as stream:
            content = stream.read(MAX_TYPED_VARS_BYTES + 1)
        fd = -1
    finally:
        if fd >= 0:
            os.close(fd)
    if len(content) > MAX_TYPED_VARS_BYTES:
        _raise_fleet_error(f"{operation} vars file grew beyond the size limit: {candidate}")
    return content


def _validate_mapping(content: bytes, operation: str, candidate: Path) -> None:
    """Require a small typed YAML mapping for exactly one infra operation."""
    allowed_keys = TYPED_VAR_KEYS[operation]
    try:
        values = yaml.safe_load(content.decode("utf-8"))
    except (UnicodeDecodeError, yaml.YAMLError) as exc:
        _raise_fleet_error(f"{operation} vars file is not valid UTF-8 YAML: {candidate}", exc)
    if not isinstance(values, dict) or not values:
        _raise_fleet_error(f"{operation} vars file must contain a non-empty YAML mapping")
    if any(not isinstance(key, str) for key in values):
        _raise_fleet_error(f"{operation} vars file keys must all be strings")
    keys = set(values)
    unexpected = keys - allowed_keys
    if unexpected:
        _raise_fleet_error(
            f"{operation} vars file contains unsupported key(s): {', '.join(sorted(unexpected))}"
        )
    if operation in EXACT_KEY_OPERATIONS and keys != allowed_keys:
        missing = allowed_keys - keys
        _raise_fleet_error(
            f"{operation} vars file is missing required key(s): {', '.join(sorted(missing))}"
        )
    for key in keys & TOKEN_KEYS:
        if not isinstance(values[key], str) or not values[key].strip():
            _raise_fleet_error(f"{operation} key {key} must be a non-empty string")
    if "ci_runner_docker_destroy_dataset" in values and not isinstance(
        values["ci_runner_docker_destroy_dataset"], bool
    ):
        _raise_fleet_error(
            f"{operation} key ci_runner_docker_destroy_dataset must be a YAML boolean"
        )


def read_typed_vars_file(raw_path: str, operation: str) -> TypedVars:
    """Capture a canonical typed vars file before any converge side effect."""
    if operation not in TYPED_VAR_KEYS:
        _raise_fleet_error(f"unsupported typed vars operation: {operation}")
    candidate = Path(raw_path).expanduser()
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    content = _read_owned_vars_content(candidate, operation)
    _validate_mapping(content, operation, candidate)
    resolved = candidate.resolve(strict=True)
    if resolved.is_relative_to(fm.REPO_ROOT):
        _raise_fleet_error(f"{operation} vars file must live outside the checkout: {resolved}")
    return TypedVars(resolved, content)


@contextmanager
def local_vars_snapshot(typed_vars: TypedVars) -> Iterator[Path]:
    """Yield a canonical mode-0600 snapshot and remove it on every exit path."""
    fd, raw_path = tempfile.mkstemp(prefix="ra8-ansible-vars-", suffix=".yml")
    path = Path(raw_path).resolve(strict=True)
    try:
        os.fchmod(fd, PRIVATE_FILE_MODE)
        with os.fdopen(fd, "wb", closefd=True) as stream:
            stream.write(typed_vars.content)
        fd = -1
        yield path
    finally:
        if fd >= 0:
            os.close(fd)
        path.unlink(missing_ok=True)


def _expect_refusal(path: Path, operation: str) -> bool:
    """Return whether a typed vars fixture is rejected without escaping."""
    try:
        read_typed_vars_file(str(path), operation)
    except fm.FleetError:
        return True
    return False


def _fixture(root: Path, name: str, content: bytes, mode: int = 0o600) -> Path:
    """Create one isolated typed-vars fixture with an explicit mode."""
    path = root / name
    path.write_bytes(content)
    path.chmod(mode)
    return path


def _rejection_selftest(root: Path, good: Path) -> list[str]:
    """Exercise every typed-file refusal without any converge side effect."""
    failures: list[str] = []
    cases = (
        (_fixture(root, "wrong-mode.yml", good.read_bytes(), 0o644), "non-0600"),
        (_fixture(root, "wrong-key.yml", b"arbitrary_ansible_override: true\n"), "out-of-schema"),
        (_fixture(root, "non-string-key.yml", b"1: token\n"), "non-string key"),
        (_fixture(root, "oversized.yml", b"x" * (MAX_TYPED_VARS_BYTES + 1)), "oversized"),
    )
    for path, label in cases:
        if not _expect_refusal(path, RUNNER_REGISTRATION):
            failures.append(f"{label} typed vars file was accepted")
    if not _expect_refusal(root, RUNNER_REGISTRATION):
        failures.append("non-regular typed vars path was accepted")
    with patch.object(os, "geteuid", return_value=os.geteuid() + 1):
        if not _expect_refusal(good, RUNNER_REGISTRATION):
            failures.append("wrong-owner typed file was accepted")
    link = root / "link.yml"
    link.symlink_to(good)
    if not _expect_refusal(link, RUNNER_REGISTRATION):
        failures.append("symlinked typed file was accepted")
    return failures


def _fail_inside_snapshot(typed: TypedVars, observed: list[tuple[Path, bool]]) -> NoReturn:
    """Raise from inside a snapshot context and expose only its former path."""
    with local_vars_snapshot(typed) as snapshot:
        private = (
            snapshot.is_absolute() and stat.S_IMODE(snapshot.stat().st_mode) == PRIVATE_FILE_MODE
        )
        observed.append((snapshot, private))
        message = "exercise cleanup"
        raise RuntimeError(message)


def _snapshot_cleanup_selftest(typed: TypedVars) -> list[str]:
    """Prove a local snapshot is canonical, private, and failure-cleaned."""
    observed: list[tuple[Path, bool]] = []
    with suppress(RuntimeError):
        _fail_inside_snapshot(typed, observed)
    failures = [] if observed and observed[0][1] else ["local snapshot was not absolute/mode-0600"]
    snapshot_path = observed[0][0] if observed else None
    if snapshot_path is None or snapshot_path.exists():
        failures.append("local snapshot survived a failing converge")
    return failures


def run_selftest() -> list[str]:
    """Exercise typed vars acceptance and refusal in both directions."""
    with tempfile.TemporaryDirectory(prefix="ra8-fleet-vars-") as scratch:
        root = Path(scratch)
        checkout = root / "checkout"
        checkout.mkdir()
        with patch.object(fm, "REPO_ROOT", checkout):
            good = _fixture(
                root, "registration.yml", b"ci_runner_docker_registration_token: test-token\n"
            )
            typed = read_typed_vars_file(str(good), RUNNER_REGISTRATION)
            failures = _rejection_selftest(root, good)
            if typed.source != good.resolve() or typed.content != good.read_bytes():
                failures.append("valid typed file was not captured canonically")
            if not _expect_refusal(good, "arbitrary operation"):
                failures.append("unlisted typed operation was accepted")
            removal = _fixture(root, "removal.yml", b"ci_runner_docker_destroy_dataset: false\n")
            if read_typed_vars_file(str(removal), RUNNER_REMOVAL).source != removal.resolve():
                failures.append("valid removal typed file was rejected")
            return failures + _snapshot_cleanup_selftest(typed)
