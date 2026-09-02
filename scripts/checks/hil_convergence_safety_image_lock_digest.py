# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Bind privileged image-lock shell surfaces to exact reviewed raw bytes."""

from __future__ import annotations

import ast
import hashlib
import os
import re
import stat
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

import hil_convergence_safety_raw_digest_controls as raw_digest_controls

# 2026-08-28: These privileged runtime surfaces and their implementation-control
# policy own cross-user image serialization and its failure cleanup. Structural
# enumeration cannot prove an unaudited byte harmless, so each surface is
# review-bound; intentional edits must update these pins separately.
DEVCONTAINER_IMAGE_PATH = "scripts/ci/devcontainer_image.sh"
DEVCONTAINER_IMAGE_RAW_SHA256 = "c0c4496a1c1d890eb9f7b7e5af6826921f4b4c5ffbf4f6616edced88bb7c1f21"
DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH = "scripts/ci/devcontainer_image_lock_receipts.bash"
DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256 = (
    "854cfd1163d3d49eda0b05d8a14c5b32385b4b96e7d08de20b03da5cbd1ee727"
)
DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH = "scripts/ci/devcontainer_image_lock_selftest.bash"
DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256 = (
    "560f0d73cc37317d38ef7cd1d3b32a82a78dacde3baa4769a7bc0ef1d42189c6"
)
DEVCONTAINER_IMAGE_SELFTEST_PATH = "scripts/ci/devcontainer_image_selftest.bash"
DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256 = (
    "1d84ebe964ea5085a4145db610cb1d37ad61c02ea6d1e3220f8befdb2de1fcd7"
)
DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH = (
    "scripts/ci/devcontainer_image_bound_exit_selftest.bash"
)
DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256 = (
    "79a1a39638b961ecb2daededdd363f04a7f765991b860a629cfbdc589df8de4d"
)
DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH = "scripts/ci/devcontainer_image_selftest_cases.bash"
DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256 = (
    "82f83d5717186669852c92e6af4ee426b35527053787586f9130029755d9cb96"
)
DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH = "scripts/ci/devcontainer_image_signal_selftest.bash"
DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256 = (
    "37890007bdfe343848b8d42f2f58367e6018b0503b3117df6011ad599754ea21"
)
DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH = "scripts/ci/devcontainer_image_selftest_process.py"
DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256 = (
    "0d6735a43532e39ebcda7d876ba8223062656a2fe944c00b611a6c85f3dd730c"
)
DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH = "scripts/ci/devcontainer_image_selftest_supervisor.py"
DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256 = (
    "27b73473be5078f5e7f894aba36a6b3bd91fbce2338510b3fc5a0a1cb8b55396"
)
DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH = (
    "scripts/ci/devcontainer_image_selftest_supervisor_cases.py"
)
DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256 = (
    "897a5be60eec486f9f9615fead84db22f8526dba189df305f561bc1c7b5e49e7"
)
RAW_DIGEST_CONTROLS_PATH = "scripts/checks/hil_convergence_safety_raw_digest_controls.py"
RAW_DIGEST_CONTROLS_RAW_SHA256 = "62787d69cb8d4069facfcfb6b97aa1a0d7268d7609ec929f8f38d46fb2057a8b"

_DIRECTORY_MODE = 0o755
# Checkout roots may be private or read-only. Owner read/search is mandatory;
# group/other write and every special mode bit are forbidden. Shared setgid
# worktrees therefore fail closed and need a separately reviewed policy.
_ROOT_MODE_REQUIRED = 0o500
_ROOT_MODE_ALLOWED = 0o755
_EXECUTABLE_MODE = 0o755
_SOURCE_MODE = 0o644
_MAX_AUTHORITY_BYTES = 1_048_576
_READ_STEP_BYTES = 65_536
_FORBIDDEN_RELATIVE_PARTS = frozenset({"", ".", ".."})
_FIXED_PATH_BY_PIN = {
    "DEVCONTAINER_IMAGE_RAW_SHA256": DEVCONTAINER_IMAGE_PATH,
    "DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256": DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH,
    "DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256": DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH,
    "DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256": DEVCONTAINER_IMAGE_SELFTEST_PATH,
    "DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256": (
        DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH
    ),
    "DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256": DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH,
    "DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256": DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH,
    "DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256": DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH,
    "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256": (
        DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH
    ),
    "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256": (
        DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH
    ),
    "RAW_DIGEST_CONTROLS_RAW_SHA256": RAW_DIGEST_CONTROLS_PATH,
}

Target = tuple[str, str, str, int]
Identity = tuple[int, int, int, int, int, int, int, int, int, int]


@dataclass(frozen=True)
class RawReadAuthority:
    """Portable numeric ownership derived from the checked repository root.

    Hosted CI, containers, and worktrees use different IDs, so no numeric ID is
    pinned in source. The retained root descriptor establishes both IDs and all
    audited directories and files must match. Explicit fields exist so the
    production owner/group branches have unprivileged two-sided tests.
    """

    root_uid: int
    root_gid: int
    file_uid: int
    file_gid: int


@dataclass(frozen=True)
class RawReadTestHooks:
    """Deterministic race and failure hooks used by the two-sided selftest."""

    missing_capability: str | None = None
    allow_alternate_fixed_paths: bool = False
    after_root_open: Callable[[Path, int], None] | None = None
    after_component_open: Callable[[str, int, int], None] | None = None
    after_file_open: Callable[[str, int], None] | None = None
    after_read: Callable[[str, int], None] | None = None
    before_post_rewalk: Callable[[str], None] | None = None
    before_root_postcheck: Callable[[Path, int], None] | None = None
    after_close_fd: Callable[[int], None] | None = None


@dataclass(frozen=True)
class _AuditContext:
    """Shared authority, flags, and test hooks for one retained-root audit."""

    authority: RawReadAuthority
    flags: Mapping[str, int]
    hooks: RawReadTestHooks | None


@dataclass(frozen=True)
class _MetadataPolicy:
    """Exact metadata policy for one directory or regular file."""

    kind: str
    uid: int
    gid: int
    mode: int
    single_link: bool
    label: str


@dataclass(frozen=True)
class _DirectoryRequest:
    """One component-wise openat request."""

    parent_fd: int
    component: str
    relative: str
    index: int


@dataclass
class _OpenedTarget:
    """Descriptor-backed state retained across one bounded file read."""

    fd: int
    before: os.stat_result
    directory_identities: tuple[Identity, ...]
    ledger: _DescriptorLedger


def _shell_targets(authority: Mapping[str, object]) -> tuple[Target, ...]:
    """Return each raw-bound privileged shell surface."""
    return (
        (
            str(authority.get("DEVCONTAINER_IMAGE_PATH", "")),
            "DEVCONTAINER_IMAGE_RAW_SHA256",
            "devcontainer image authority",
            _EXECUTABLE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH", "")),
            "DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256",
            "devcontainer image-lock receipt authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH", "")),
            "DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256",
            "devcontainer image-lock selftest authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_SELFTEST_PATH", "")),
            "DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256",
            "devcontainer image selftest lifecycle authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH", "")),
            "DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256",
            "devcontainer image bound-exit selftest authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH", "")),
            "DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256",
            "devcontainer image selftest cases authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH", "")),
            "DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256",
            "devcontainer image signal selftest authority",
            _SOURCE_MODE,
        ),
    )


def _python_targets(authority: Mapping[str, object]) -> tuple[Target, ...]:
    """Return each raw-bound Python process and control surface."""
    return (
        (
            str(authority.get("DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH", "")),
            "DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256",
            "devcontainer image selftest process authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH", "")),
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256",
            "devcontainer image selftest supervisor authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH", "")),
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256",
            "devcontainer image selftest supervisor cases authority",
            _SOURCE_MODE,
        ),
        (
            str(authority.get("RAW_DIGEST_CONTROLS_PATH", "")),
            "RAW_DIGEST_CONTROLS_RAW_SHA256",
            "raw digest implementation-control authority",
            _SOURCE_MODE,
        ),
    )


def _targets(values: Mapping[str, object] | None = None) -> tuple[Target, ...]:
    """Return path, pin, label, and exact file mode for each bound surface."""
    authority = globals() if values is None else values
    return _shell_targets(authority) + _python_targets(authority)


def pin_names() -> tuple[str, ...]:
    """Return the exact mutable names used by the production wrong-pin tests."""
    return tuple(pin_name for _path, pin_name, _label, _mode in _targets())


def _source_names() -> frozenset[str]:
    """Return every direct source authority name parsed without execution."""
    return frozenset(
        {
            "DEVCONTAINER_IMAGE_PATH",
            "DEVCONTAINER_IMAGE_RAW_SHA256",
            "DEVCONTAINER_IMAGE_LOCK_RECEIPTS_PATH",
            "DEVCONTAINER_IMAGE_LOCK_RECEIPTS_RAW_SHA256",
            "DEVCONTAINER_IMAGE_LOCK_SELFTEST_PATH",
            "DEVCONTAINER_IMAGE_LOCK_SELFTEST_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SELFTEST_PATH",
            "DEVCONTAINER_IMAGE_SELFTEST_RAW_SHA256",
            "DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_PATH",
            "DEVCONTAINER_IMAGE_BOUND_EXIT_SELFTEST_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SELFTEST_CASES_PATH",
            "DEVCONTAINER_IMAGE_SELFTEST_CASES_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_PATH",
            "DEVCONTAINER_IMAGE_SIGNAL_SELFTEST_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SELFTEST_PROCESS_PATH",
            "DEVCONTAINER_IMAGE_SELFTEST_PROCESS_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_PATH",
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_RAW_SHA256",
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_PATH",
            "DEVCONTAINER_IMAGE_SELFTEST_SUPERVISOR_CASES_RAW_SHA256",
            "RAW_DIGEST_CONTROLS_PATH",
            "RAW_DIGEST_CONTROLS_RAW_SHA256",
        }
    )


def authority_values() -> dict[str, object]:
    """Return a detached copy of the direct path and digest authorities."""
    return {name: globals()[name] for name in _source_names()}


def target_specs(values: Mapping[str, object] | None = None) -> tuple[Target, ...]:
    """Return immutable target specifications for the runtime selftest."""
    return _targets(values)


def maximum_authority_bytes() -> int:
    """Return the exact bounded-read ceiling exercised by the selftest."""
    return _MAX_AUTHORITY_BYTES


def _expected_digest(
    pin_name: str, label: str, values: Mapping[str, object] | None = None
) -> tuple[str | None, str | None]:
    """Return one exact pin or one diagnostic for that authority."""
    authority = globals() if values is None else values
    expected = authority.get(pin_name)
    if not isinstance(expected, str) or re.fullmatch(r"[0-9a-f]{64}", expected) is None:
        return None, f"{label}: exact raw SHA-256 pin is missing or malformed"
    return expected, None


def _digest_error(payload: bytes, expected: str, label: str) -> str | None:
    """Return one raw-byte mismatch diagnostic, if any."""
    if hashlib.sha256(payload).hexdigest() != expected:
        return f"{label}: raw bytes differ from exact audited digest"
    return None


def _raw_target_error(
    target: Target,
    files: Mapping[str, bytes],
    values: Mapping[str, object] | None,
) -> str | None:
    """Return one trusted local path, pin, presence, or digest error."""
    path, pin_name, label, _mode = target
    path_error = _target_path_error(path, pin_name, label)
    if path_error is not None:
        return path_error
    expected, error = _expected_digest(pin_name, label, values)
    if error is not None:
        return error
    payload = files.get(path)
    if payload is None:
        return f"{label}: bound file is missing"
    return _digest_error(payload, expected, label)


def raw_errors(files: Mapping[str, bytes], values: Mapping[str, object] | None = None) -> list[str]:
    """Return errors for missing pins/files or raw-byte digest mismatches."""
    return [
        error
        for target in _targets(values)
        if (error := _raw_target_error(target, files, values)) is not None
    ]


def _control_target(values: Mapping[str, object]) -> Target:
    """Return the one raw-bound implementation-control policy target."""
    matches = [
        target for target in _targets(values) if target[1] == "RAW_DIGEST_CONTROLS_RAW_SHA256"
    ]
    if len(matches) != 1:
        return "", "RAW_DIGEST_CONTROLS_RAW_SHA256", "raw digest control target", _SOURCE_MODE
    return matches[0]


def _source_values(source: str) -> dict[str, object]:
    """Parse the direct module authorities without executing candidate code."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return {}
    values: dict[str, object] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id in _source_names():
            try:
                values[target.id] = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                continue
    return values


def source_errors(
    sources: tuple[str, ...],
    authority_source: str,
) -> list[str]:
    """Apply the production digest path to mutation-selftest source bytes."""
    values = _source_values(authority_source)
    files = {
        path: source.encode("utf-8")
        for (path, _pin, _label, _mode), source in zip(_targets(values), sources, strict=True)
    }
    control_error = _raw_target_error(_control_target(values), files, values)
    if control_error is not None:
        return [control_error]
    errors = implementation_errors(authority_source)
    return errors + raw_errors(files, values)


def implementation_errors(source: str) -> list[str]:
    """Reject removal or duplication of every load-bearing reader control."""
    return raw_digest_controls.implementation_errors(source)


def implementation_controls() -> tuple[raw_digest_controls.ImplementationControl, ...]:
    """Return every exact reader control from its focused policy module."""
    return raw_digest_controls.controls()


def implementation_mutations(source: str) -> tuple[tuple[str, str, str], ...]:
    """Return exact one-control mutants and their required diagnostics."""
    return raw_digest_controls.implementation_mutations(source)


def _identity(value: os.stat_result) -> Identity:
    """Return every security-relevant field required to remain exact."""
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_uid,
        value.st_gid,
        value.st_nlink,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
        stat.S_IFMT(value.st_mode),
    )


def _fixed_relative_parts(path: str) -> tuple[str, ...] | None:
    """Return canonical POSIX components, rejecting absolute or parent paths."""
    if re.fullmatch(r"[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)*", path) is None:
        return None
    pure = PurePosixPath(path)
    if pure.is_absolute() or any(part in _FORBIDDEN_RELATIVE_PARTS for part in pure.parts):
        return None
    if pure.as_posix() != path:
        return None
    return pure.parts


def _target_path_error(path: str, pin_name: str, label: str) -> str | None:
    """Require one normalized path and its immutable repository location."""
    if _fixed_relative_parts(path) is None:
        return f"{label}: bound path must be a fixed normalized relative path"
    if path != _FIXED_PATH_BY_PIN[pin_name]:
        return f"{label}: bound path differs from fixed repository authority"
    return None


def _required_flags(hooks: RawReadTestHooks | None) -> tuple[dict[str, int] | None, str | None]:
    """Resolve every required no-follow/nonblocking platform capability."""
    required = ("O_DIRECTORY", "O_NOFOLLOW", "O_CLOEXEC", "O_NONBLOCK")
    flags: dict[str, int] = {}
    for name in required:
        value = getattr(os, name, None)
        if hooks is not None and hooks.missing_capability == name:
            value = None
        if not isinstance(value, int) or value == 0:
            return None, (
                f"raw digest authority: required platform capability is unavailable: {name}"
            )
        flags[name] = value
    predicates = (
        ("open_dir_fd", os.open in os.supports_dir_fd),
        ("stat_dir_fd", os.stat in os.supports_dir_fd),
        ("stat_follow_symlinks", os.stat in os.supports_follow_symlinks),
        ("pread", callable(getattr(os, "pread", None))),
    )
    for name, available in predicates:
        is_available = available and not (hooks is not None and hooks.missing_capability == name)
        if not is_available:
            return None, (
                f"raw digest authority: required platform capability is unavailable: {name}"
            )
    return flags, None


def _metadata_error(value: os.stat_result, policy: _MetadataPolicy) -> str | None:
    """Return an exact type, owner, group, mode, or link diagnostic."""
    kind_ok = (
        stat.S_ISDIR(value.st_mode) if policy.kind == "directory" else stat.S_ISREG(value.st_mode)
    )
    if not kind_ok or (policy.single_link and value.st_nlink != 1):
        return f"{policy.label}: bound path is absent, linked, or non-regular"
    if value.st_uid != policy.uid:
        return f"{policy.label}: bound path owner differs from repository authority"
    if value.st_gid != policy.gid:
        return f"{policy.label}: bound path group differs from repository authority"
    if stat.S_IMODE(value.st_mode) != policy.mode:
        return f"{policy.label}: bound path mode differs from exact audited mode"
    return None


class _DescriptorLedger:
    """Own descriptors until their one exhaustive reverse-order close."""

    def __init__(self, hooks: RawReadTestHooks | None) -> None:
        self._fds: list[int] = []
        self._hooks = hooks

    def retain(self, fd: int) -> int:
        """Record and return one newly opened descriptor."""
        self._fds.append(fd)
        return fd

    def close_all(self, label: str) -> list[str]:
        """Release ownership, close once, then invoke an after-close hook."""
        errors = []
        while self._fds:
            fd = self._fds.pop()
            errors.extend(_close_released_descriptor(fd, label, self._hooks))
        return errors


def _close_released_descriptor(
    fd: int,
    label: str,
    hooks: RawReadTestHooks | None,
) -> list[str]:
    """Close one released number once, then notify an untrusted test hook."""
    try:
        os.close(fd)
    except OSError as exc:
        return [f"{label}: cannot close audited descriptor safely: {exc.errno}"]
    hook = None if hooks is None else hooks.after_close_fd
    if hook is not None:
        try:
            hook(fd)
        except (OSError, RuntimeError):
            return [f"{label}: descriptor-close hook failed"]
    return []


def _open_directory(
    request: _DirectoryRequest,
    context: _AuditContext,
    ledger: _DescriptorLedger,
) -> tuple[int | None, Identity | None, str | None]:
    """No-follow open and bind one exact path component."""
    label = f"raw digest authority directory: {request.relative}"
    try:
        before = os.stat(request.component, dir_fd=request.parent_fd, follow_symlinks=False)
        error = _metadata_error(
            before,
            _MetadataPolicy(
                kind="directory",
                uid=context.authority.root_uid,
                gid=context.authority.root_gid,
                mode=_DIRECTORY_MODE,
                single_link=False,
                label=label,
            ),
        )
        if error is not None:
            return None, None, error
        descriptor = ledger.retain(
            os.open(
                request.component,
                os.O_RDONLY
                | context.flags["O_DIRECTORY"]
                | context.flags["O_NOFOLLOW"]
                | context.flags["O_CLOEXEC"]
                | context.flags["O_NONBLOCK"],
                dir_fd=request.parent_fd,
            )
        )
        opened = os.fstat(descriptor)
        if _identity(before) != _identity(opened):
            return None, None, f"{label}: component changed before its no-follow open"
        hooks = context.hooks
        if hooks is not None and hooks.after_component_open is not None:
            hooks.after_component_open(request.relative, request.index, descriptor)
    except OSError as exc:
        return None, None, f"{label}: cannot inspect or open component safely: {exc.errno}"
    return descriptor, _identity(opened), None


def _read_exact_payload(
    fd: int, before: os.stat_result, relative: str, hooks: RawReadTestHooks | None
) -> tuple[bytes | None, str | None]:
    """Use bounded positional reads and prove exact EOF at the pre-open size."""
    label = f"raw digest authority file: {relative}"
    if before.st_size > _MAX_AUTHORITY_BYTES:
        return None, f"{label}: bound file exceeds maximum audited size"
    chunks = []
    offset = 0
    try:
        while offset < before.st_size:
            requested = min(_READ_STEP_BYTES, before.st_size - offset)
            chunk = os.pread(fd, requested, offset)
            if len(chunk) != requested:
                return None, f"{label}: bound file shrank during bounded raw-byte read"
            chunks.append(chunk)
            offset += requested
        if os.pread(fd, 1, before.st_size) != b"" or os.pread(fd, 1, before.st_size + 1) != b"":
            return None, f"{label}: bound file has bytes beyond its audited pre-read size"
        if hooks is not None and hooks.after_read is not None:
            hooks.after_read(relative, fd)
    except OSError as exc:
        return None, f"{label}: cannot read bound bytes safely: {exc.errno}"
    return b"".join(chunks), None


def _post_rewalk(
    root_fd: int,
    parts: tuple[str, ...],
    expected_directories: tuple[Identity, ...],
    expected_file: Identity,
    context: _AuditContext,
) -> str | None:
    """Rewalk every component from the retained root and compare identities."""
    relative = "/".join(parts)
    ledger = _DescriptorLedger(context.hooks)
    parent_fd = root_fd
    error = None
    try:
        for index, component in enumerate(parts[:-1]):
            request = _DirectoryRequest(parent_fd, component, relative, index)
            descriptor, identity, error = _open_directory(
                request,
                _AuditContext(context.authority, context.flags, None),
                ledger,
            )
            if error is not None:
                break
            if identity != expected_directories[index]:
                error = (
                    f"raw digest authority file: {relative}: parent changed during raw-byte read"
                )
                break
            parent_fd = descriptor
        if error is None:
            after_path = os.stat(parts[-1], dir_fd=parent_fd, follow_symlinks=False)
            if _identity(after_path) != expected_file:
                error = f"raw digest authority file: {relative}: path changed during raw-byte read"
    except OSError as exc:
        error = (
            f"raw digest authority file: {relative}: cannot rewalk bound path safely: {exc.errno}"
        )
    close_errors = ledger.close_all(f"raw digest authority file: {relative}")
    return error if error is not None else (close_errors[0] if close_errors else None)


def _open_target(
    root_fd: int,
    target: Target,
    context: _AuditContext,
) -> tuple[_OpenedTarget | None, tuple[str, ...] | None, str | None]:
    """Open and bind one target and every directory leading to it."""
    path, _pin, label, _expected_mode = target
    parts = _fixed_relative_parts(path)
    if parts is None:
        return None, None, f"{label}: bound path must be a fixed normalized relative path"
    hooks = context.hooks
    if path != _FIXED_PATH_BY_PIN[target[1]] and (
        hooks is None or not hooks.allow_alternate_fixed_paths
    ):
        return None, None, f"{label}: bound path differs from fixed repository authority"
    ledger = _DescriptorLedger(context.hooks)
    parent_fd = root_fd
    directory_identities: list[Identity] = []
    error = None
    completed = False
    close_errors: list[str] = []
    try:
        for index, component in enumerate(parts[:-1]):
            request = _DirectoryRequest(parent_fd, component, path, index)
            parent_fd, identity, error = _open_directory(
                request,
                context,
                ledger,
            )
            if error is not None:
                break
            directory_identities.append(identity)
        if error is None:
            fd, before, error = _open_final(parent_fd, parts, target, context, ledger)
        completed = error is None
    except (OSError, RuntimeError) as exc:
        detail = exc.errno if isinstance(exc, OSError) else "hook"
        error = f"{label}: cannot inspect or open bound bytes safely: {detail}"
    finally:
        if not completed:
            close_errors = ledger.close_all(label)
    if error is not None:
        return None, None, error if not close_errors else close_errors[0]
    state = _OpenedTarget(fd, before, tuple(directory_identities), ledger)
    return state, parts, None


def _open_final(
    parent_fd: int,
    parts: tuple[str, ...],
    target: Target,
    context: _AuditContext,
    ledger: _DescriptorLedger,
) -> tuple[int | None, os.stat_result | None, str | None]:
    """No-follow open and bind the single-link regular final component."""
    path, _pin, label, expected_mode = target
    before = os.stat(parts[-1], dir_fd=parent_fd, follow_symlinks=False)
    error = _metadata_error(
        before,
        _MetadataPolicy(
            kind="file",
            uid=context.authority.file_uid,
            gid=context.authority.file_gid,
            mode=expected_mode,
            single_link=True,
            label=label,
        ),
    )
    if error is not None:
        return None, None, error
    fd = ledger.retain(
        os.open(
            parts[-1],
            os.O_RDONLY
            | context.flags["O_NOFOLLOW"]
            | context.flags["O_CLOEXEC"]
            | context.flags["O_NONBLOCK"],
            dir_fd=parent_fd,
        )
    )
    opened = os.fstat(fd)
    if _identity(before) != _identity(opened):
        return None, None, f"{label}: bound path changed before its no-follow open"
    hooks = context.hooks
    if hooks is not None and hooks.after_file_open is not None:
        hooks.after_file_open(path, fd)
    return fd, before, None


def _audit_target(
    root_fd: int,
    target: Target,
    expected: str,
    context: _AuditContext,
) -> list[str]:
    """Read, bind, and digest one fixed target below the retained root."""
    path, _pin, label, _expected_mode = target
    state, parts, error = _open_target(root_fd, target, context)
    if error is not None:
        return [error]
    payload = None
    errors: list[str] = []
    try:
        hooks = context.hooks
        if error is None:
            payload, error = _read_exact_payload(state.fd, state.before, path, hooks)
        if error is None:
            after_fd = os.fstat(state.fd)
            if _identity(after_fd) != _identity(state.before):
                error = f"{label}: bound file metadata changed during bounded raw-byte read"
        if error is None and hooks is not None and hooks.before_post_rewalk is not None:
            hooks.before_post_rewalk(path)
        if error is None:
            error = _post_rewalk(
                root_fd,
                parts,
                state.directory_identities,
                _identity(state.before),
                context,
            )
        if error is None:
            error = _digest_error(payload, expected, label)
    except (OSError, RuntimeError) as exc:
        detail = exc.errno if isinstance(exc, OSError) else "hook"
        error = f"{label}: cannot inspect or open bound bytes safely: {detail}"
    finally:
        errors.extend(state.ledger.close_all(label))
    if error is not None:
        errors.insert(0, error)
    return errors


def _root_metadata_error(value: os.stat_result, authority: RawReadAuthority | None) -> str | None:
    """Validate repository-root type, ownership, and safe portable mode."""
    label = "raw digest authority root"
    if not stat.S_ISDIR(value.st_mode):
        return f"{label}: repository root is not a directory"
    mode = stat.S_IMODE(value.st_mode)
    if (mode & _ROOT_MODE_REQUIRED) != _ROOT_MODE_REQUIRED or mode & ~_ROOT_MODE_ALLOWED:
        return f"{label}: repository root mode is not safe for the audited authority"
    if authority is not None and value.st_uid != authority.root_uid:
        return f"{label}: repository root owner differs from configured authority"
    if authority is not None and value.st_gid != authority.root_gid:
        return f"{label}: repository root group differs from configured authority"
    return None


def _audit_all_targets(
    root_fd: int,
    values: Mapping[str, object],
    context: _AuditContext,
) -> list[str]:
    """Audit all fixed targets below one retained repository root."""
    errors = []
    for target in _targets(values):
        expected, error = _expected_digest(target[1], target[2], values)
        if error is None:
            errors.extend(_audit_target(root_fd, target, expected, context))
        else:
            errors.append(error)
    return errors


def audit_live_errors(
    root: Path,
    values: Mapping[str, object],
    *,
    authority: RawReadAuthority | None = None,
    hooks: RawReadTestHooks | None = None,
) -> list[str]:
    """Audit exact live bytes through one retained no-follow root descriptor."""
    flags, error = _required_flags(hooks)
    if error is not None:
        return [error]
    ledger = _DescriptorLedger(hooks)
    root_path = Path(root)
    errors = []
    try:
        before = os.lstat(root_path)
        error = _root_metadata_error(before, authority)
        if error is None:
            root_fd = ledger.retain(
                os.open(
                    root_path,
                    os.O_RDONLY
                    | flags["O_DIRECTORY"]
                    | flags["O_NOFOLLOW"]
                    | flags["O_CLOEXEC"]
                    | flags["O_NONBLOCK"],
                )
            )
            opened = os.fstat(root_fd)
            if _identity(before) != _identity(opened):
                error = "raw digest authority root: path changed before its no-follow open"
        if error is None:
            configured = authority or RawReadAuthority(
                root_uid=opened.st_uid,
                root_gid=opened.st_gid,
                file_uid=opened.st_uid,
                file_gid=opened.st_gid,
            )
            context = _AuditContext(configured, flags, hooks)
            if hooks is not None and hooks.after_root_open is not None:
                hooks.after_root_open(root_path, root_fd)
            errors.extend(_audit_all_targets(root_fd, values, context))
            if hooks is not None and hooks.before_root_postcheck is not None:
                hooks.before_root_postcheck(root_path, root_fd)
            after_fd = os.fstat(root_fd)
            after_path = os.lstat(root_path)
            if _identity(before) != _identity(after_fd) or _identity(before) != _identity(
                after_path
            ):
                errors.append("raw digest authority root: path changed during retained-root audit")
        else:
            errors.append(error)
    except (OSError, RuntimeError) as exc:
        detail = exc.errno if isinstance(exc, OSError) else "hook"
        errors.append(f"raw digest authority root: cannot inspect or open safely: {detail}")
    finally:
        errors.extend(ledger.close_all("raw digest authority root"))
    return errors


def live_errors(root: Path) -> list[str]:
    """Read and validate each exact authority with fail-closed diagnostics."""
    return audit_live_errors(root, authority_values())
