# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Execute authenticated uv bytes without reopening a mutable cache path."""

from __future__ import annotations

import importlib
import os
import secrets
import stat
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager, suppress
from pathlib import Path
from typing import NoReturn

PROBE_EXECUTABLE_MODE = 0o500
PRIVATE_TEMPORARY_DIRECTORY_MODE = 0o700
PRIVATE_TEMPORARY_FILE_MODE = 0o600
MAX_CACHE_PATH_COMPONENTS = 64
MAX_TEMPORARY_NAME_ATTEMPTS = 16
CACHE_DIRECTORY_MODE = 0o755
DARWIN_ROOT_ALIASES = {
    "tmp": ("private", "tmp"),
    "var": ("private", "var"),
}
DARWIN_ROOT_ALIAS_POSITIONS = frozenset(
    (0, "darwin", component) for component in DARWIN_ROOT_ALIASES
)


class UvExecError(RuntimeError):
    """Report a fail-closed anonymous-execution boundary failure."""


def fail(message: str) -> NoReturn:
    """Raise one execution-boundary error."""
    raise UvExecError(message)


def _stat_identity(state: os.stat_result) -> tuple[int, int]:
    """Return the filesystem identity fields used by alias binding."""
    return state.st_dev, state.st_ino


def _link_fingerprint(state: os.stat_result) -> tuple[int, ...]:
    """Return metadata that must remain stable across one alias proof."""
    return (
        state.st_dev,
        state.st_ino,
        state.st_mode,
        state.st_uid,
        state.st_gid,
        state.st_size,
        state.st_mtime_ns,
        state.st_ctime_ns,
    )


def _require_trusted_system_root(descriptor: int) -> None:
    """Require a held descriptor to name the immutable system root."""
    held = os.fstat(descriptor)
    named = Path("/").stat(follow_symlinks=False)
    unsafe_mode = stat.S_IMODE(held.st_mode) & 0o022
    if _stat_identity(held) != _stat_identity(named):
        fail("Darwin uv cache alias root changed identity")
    if not stat.S_ISDIR(held.st_mode):
        fail("Darwin uv cache alias root is not a directory")
    if held.st_uid != 0:
        fail("Darwin uv cache alias root is not root-owned")
    if unsafe_mode:
        fail("Darwin uv cache alias requires the trusted system root")


def _open_physical_alias_target(
    root_descriptor: int, components: tuple[str, ...], flags: int
) -> int:
    """Open an allowlisted alias target without following any component."""
    descriptor = -1
    current = root_descriptor
    try:
        for component in components:
            next_descriptor = os.open(component, flags, dir_fd=current)
            if descriptor >= 0:
                os.close(descriptor)
            descriptor = next_descriptor
            current = descriptor
    except OSError:
        if descriptor >= 0:
            os.close(descriptor)
        raise
    return descriptor


def _open_verified_darwin_alias(root_descriptor: int, component: str, flags: int) -> int:
    """Open one fixed Darwin root alias and bind it to its physical target."""
    target = DARWIN_ROOT_ALIASES.get(component)
    if target is None:
        fail(f"unsupported Darwin uv cache root alias: {component}")
    expected = "/".join(target)
    before = os.stat(component, dir_fd=root_descriptor, follow_symlinks=False)
    before_target = os.readlink(component, dir_fd=root_descriptor)
    if not stat.S_ISLNK(before.st_mode):
        fail(f"Darwin uv cache root alias is not a symlink: /{component}")
    if before.st_uid != 0:
        fail(f"Darwin uv cache root alias is not root-owned: /{component}")
    if before_target != expected:
        fail(f"untrusted Darwin uv cache root alias: /{component}")
    alias_descriptor = -1
    physical_descriptor = -1
    succeeded = False
    try:
        alias_descriptor = os.open(
            component,
            flags & ~os.O_NOFOLLOW,
            dir_fd=root_descriptor,
        )
        physical_descriptor = _open_physical_alias_target(root_descriptor, target, flags)
        alias_state = os.fstat(alias_descriptor)
        physical_state = os.fstat(physical_descriptor)
        after = os.stat(component, dir_fd=root_descriptor, follow_symlinks=False)
        after_target = os.readlink(component, dir_fd=root_descriptor)
        if not stat.S_ISDIR(alias_state.st_mode):
            fail(f"Darwin uv cache alias target is not a directory: /{component}")
        if not stat.S_ISDIR(physical_state.st_mode):
            fail(f"Darwin uv cache physical target is not a directory: /{component}")
        if _stat_identity(alias_state) != _stat_identity(physical_state):
            fail(f"Darwin uv cache root alias target mismatched: /{component}")
        if _link_fingerprint(before) != _link_fingerprint(after):
            fail(f"Darwin uv cache root alias changed identity: /{component}")
        if after_target != expected:
            fail(f"Darwin uv cache root alias changed target: /{component}")
        succeeded = True
    finally:
        if alias_descriptor >= 0:
            os.close(alias_descriptor)
        if not succeeded and physical_descriptor >= 0:
            os.close(physical_descriptor)
    return physical_descriptor


def open_parent_components(
    descriptor: int,
    components: tuple[str, ...],
    flags: int,
    *,
    create: bool,
    platform_name: str,
) -> int:
    """Walk parent components, allowing only the two proven Darwin aliases."""
    try:
        for index, component in enumerate(components):
            try:
                next_descriptor = -1
                alias_key = index, platform_name, component
                if alias_key in DARWIN_ROOT_ALIAS_POSITIONS:
                    _require_trusted_system_root(descriptor)
                    next_descriptor = _open_verified_darwin_alias(descriptor, component, flags)
                if next_descriptor < 0:
                    next_descriptor = os.open(component, flags, dir_fd=descriptor)
            except FileNotFoundError:
                if not create:
                    raise
                with suppress(FileExistsError):
                    os.mkdir(component, CACHE_DIRECTORY_MODE, dir_fd=descriptor)
                next_descriptor = os.open(component, flags, dir_fd=descriptor)
            previous = descriptor
            descriptor = next_descriptor
            os.close(previous)
    except Exception:
        os.close(descriptor)
        raise
    return descriptor


def _open_parent_fd(path: Path, *, create: bool = False) -> int:
    """Open or create an absolute parent without following path components."""
    nofollow = getattr(os, "O_NOFOLLOW", None)
    cloexec = getattr(os, "O_CLOEXEC", None)
    directory = getattr(os, "O_DIRECTORY", None)
    if nofollow is None or cloexec is None or directory is None:
        fail("POSIX uv cache access requires O_NOFOLLOW, O_CLOEXEC, and O_DIRECTORY")
    if not path.is_absolute() or path.name in ("", ".", ".."):
        fail(f"uv cache artifact path is not an absolute file path: {path}")
    components = path.parent.parts[1:]
    if len(components) > MAX_CACHE_PATH_COMPONENTS:
        fail(f"uv cache artifact path has too many components: {path}")
    flags = os.O_RDONLY | nofollow | cloexec | directory
    descriptor = -1
    try:
        descriptor = os.open(path.anchor, flags)
        root_descriptor = descriptor
        descriptor = -1
        descriptor = open_parent_components(
            root_descriptor,
            components,
            flags,
            create=create,
            platform_name=sys.platform,
        )
    except (OSError, NotImplementedError, TypeError) as exc:
        if descriptor >= 0:
            os.close(descriptor)
        fail(f"cannot open cached uv parent {path.parent}: {exc}")
    return descriptor


def _new_temporary_fd(parent: int, mode: int = PRIVATE_TEMPORARY_FILE_MODE) -> tuple[int, str]:
    """Create one unpredictable private file relative to a held parent FD."""
    cloexec = getattr(os, "O_CLOEXEC", None)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if cloexec is None or nofollow is None:
        fail("POSIX uv cache writes require O_CLOEXEC and O_NOFOLLOW")
    if mode not in (PROBE_EXECUTABLE_MODE, PRIVATE_TEMPORARY_FILE_MODE):
        fail("POSIX uv cache temporary mode is outside policy")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | cloexec | nofollow
    for _attempt in range(MAX_TEMPORARY_NAME_ATTEMPTS):
        name = f".ra8-uv-{secrets.token_hex(16)}.tmp"
        try:
            return os.open(name, flags, mode, dir_fd=parent), name
        except FileExistsError:
            continue
    fail("cannot allocate a private uv cache temporary")


def write_atomic_nofollow(path: Path, payload: bytes, mode: int) -> None:
    """Atomically write bytes within one held no-follow parent directory."""
    if not payload or mode not in (0o600, 0o700):
        fail("uv cache write requires nonempty bytes and one private mode")
    parent = _open_parent_fd(path, create=True)
    descriptor = -1
    temporary = ""
    try:
        descriptor, temporary = _new_temporary_fd(parent)
        write_exact_fd(descriptor, payload)
        os.fsync(descriptor)
        os.fchmod(descriptor, mode)
        os.replace(temporary, path.name, src_dir_fd=parent, dst_dir_fd=parent)
    except (OSError, NotImplementedError, TypeError) as exc:
        fail(f"cannot write cached uv artifact {path}: {exc}")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary:
            with suppress(FileNotFoundError):
                os.unlink(temporary, dir_fd=parent)
        os.close(parent)


def open_regular_nofollow(path: Path) -> int:
    """Open one single-link regular file relative to a held safe parent."""
    nonblock = getattr(os, "O_NONBLOCK", None)
    cloexec = getattr(os, "O_CLOEXEC", None)
    nofollow = getattr(os, "O_NOFOLLOW", None)
    if nonblock is None or cloexec is None or nofollow is None:
        fail("POSIX uv cache access requires O_NONBLOCK, O_CLOEXEC, and O_NOFOLLOW")
    parent = _open_parent_fd(path)
    descriptor = -1
    try:
        descriptor = os.open(
            path.name,
            os.O_RDONLY | nonblock | cloexec | nofollow,
            dir_fd=parent,
        )
        state = os.fstat(descriptor)
    except OSError as exc:
        if descriptor >= 0:
            os.close(descriptor)
        fail(f"cannot open cached uv artifact {path}: {exc}")
    finally:
        os.close(parent)
    if not stat.S_ISREG(state.st_mode) or state.st_nlink != 1:
        os.close(descriptor)
        fail(f"cached uv artifact is not one single-link regular file: {path}")
    return descriptor


def executable_fd_path(descriptor: int) -> str:
    """Return a system descriptor path that cannot reopen a cache pathname."""
    for root in (Path("/proc/self/fd"), Path("/dev/fd")):
        if root.is_dir():
            return str(root / str(descriptor))
    fail("POSIX uv execution requires /proc/self/fd or /dev/fd")


def write_exact_fd(descriptor: int, binary: bytes) -> None:
    """Write every authenticated byte to one private executable descriptor."""
    with os.fdopen(os.dup(descriptor), "wb") as target:
        written = target.write(binary)
        target.flush()
    if written != len(binary):
        fail("cannot stage the authenticated uv executable")
    os.lseek(descriptor, 0, os.SEEK_SET)


def linux_sealed_exec_fd(binary: bytes) -> int:
    """Return a sealed Linux memory descriptor containing authenticated uv."""
    cloexec = getattr(os, "MFD_CLOEXEC", None)
    allow_sealing = getattr(os, "MFD_ALLOW_SEALING", None)
    if cloexec is None or allow_sealing is None or not hasattr(os, "memfd_create"):
        fail("Linux authenticated uv execution requires sealed memfd support")
    seals = importlib.import_module("fcntl")
    seal_names = ("F_SEAL_WRITE", "F_SEAL_GROW", "F_SEAL_SHRINK", "F_SEAL_SEAL")
    if any(not hasattr(seals, name) for name in (*seal_names, "F_ADD_SEALS", "F_GET_SEALS")):
        fail("Linux authenticated uv execution requires file-seal support")
    descriptor = os.memfd_create("ra8-authenticated-uv", cloexec | allow_sealing)
    succeeded = False
    try:
        write_exact_fd(descriptor, binary)
        os.fchmod(descriptor, PROBE_EXECUTABLE_MODE)
        mask = sum(getattr(seals, name) for name in seal_names)
        seals.fcntl(descriptor, seals.F_ADD_SEALS, mask)
        if seals.fcntl(descriptor, seals.F_GET_SEALS) & mask != mask:
            fail("authenticated uv memory descriptor did not seal")
        succeeded = True
    finally:
        if not succeeded:
            os.close(descriptor)
    return descriptor


def _verify_portable_exec_fd(
    descriptor: int,
    binary: bytes,
    identity: tuple[int, int],
    *,
    linked: bool,
) -> None:
    """Authenticate one portable execution FD before and after unlinking."""
    controls = importlib.import_module("fcntl")
    access_mode = controls.fcntl(descriptor, controls.F_GETFL) & os.O_ACCMODE
    descriptor_flags = controls.fcntl(descriptor, controls.F_GETFD)
    state = os.fstat(descriptor)
    expected_links = 1 if linked else 0
    if access_mode != os.O_RDONLY:
        fail("authenticated uv descriptor did not reopen read-only")
    if descriptor_flags & controls.FD_CLOEXEC == 0:
        fail("authenticated uv descriptor is not close-on-exec")
    if _stat_identity(state) != identity:
        fail("authenticated uv descriptor changed identity")
    if not stat.S_ISREG(state.st_mode) or state.st_nlink != expected_links:
        fail("authenticated uv descriptor is not one private regular file")
    if stat.S_IMODE(state.st_mode) != PROBE_EXECUTABLE_MODE:
        fail("authenticated uv descriptor has the wrong executable mode")
    if state.st_uid != os.geteuid() or state.st_size != len(binary):
        fail("authenticated uv descriptor has untrusted ownership or size")
    try:
        payload = os.pread(descriptor, len(binary) + 1, 0)
    except (AttributeError, OSError) as exc:
        fail(f"cannot read authenticated uv descriptor: {exc}")
    if not secrets.compare_digest(payload, binary):
        fail("authenticated uv descriptor bytes changed")


def _unlink_matching_temporary(
    parent: int,
    name: str,
    identity: tuple[int, int],
    *,
    required: bool,
) -> None:
    """Unlink a temporary name only while it retains the authenticated inode."""
    try:
        state = os.stat(name, dir_fd=parent, follow_symlinks=False)
    except FileNotFoundError:
        if required:
            fail("authenticated uv temporary vanished before unlink")
        return
    if _stat_identity(state) != identity:
        if required:
            fail("authenticated uv temporary changed identity before unlink")
        return
    os.unlink(name, dir_fd=parent)


def _require_private_temporary_parent(descriptor: int) -> None:
    """Require the held portable-snapshot directory to be caller-private."""
    state = os.fstat(descriptor)
    if not stat.S_ISDIR(state.st_mode):
        fail("authenticated uv temporary parent is not a directory")
    if state.st_uid != os.geteuid():
        fail("authenticated uv temporary parent has the wrong owner")
    if stat.S_IMODE(state.st_mode) != PRIVATE_TEMPORARY_DIRECTORY_MODE:
        fail("authenticated uv temporary parent is not private")


def _verify_portable_exec_name(
    parent: int,
    name: str,
    binary: bytes,
    identity: tuple[int, int],
) -> None:
    """Reopen and authenticate the private executable name relative to its parent."""
    flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW
    descriptor = -1
    try:
        descriptor = os.open(name, flags, dir_fd=parent)
        _verify_portable_exec_fd(descriptor, binary, identity, linked=True)
    finally:
        if descriptor >= 0:
            os.close(descriptor)


@contextmanager
def portable_named_exec_snapshot(binary: bytes) -> Iterator[tuple[int, str]]:
    """Yield a read-only executable under one private non-Linux POSIX name.

    The private-name window excludes other OS identities and all cache-path
    races. Like the surrounding checkout and caller, it is not an integrity
    boundary against a malicious peer process running under the same UID.
    """
    if not binary:
        fail("authenticated uv executable bytes are empty")
    reader = -1
    writer = -1
    parent = -1
    temporary = ""
    identity = (-1, -1)
    try:
        with tempfile.TemporaryDirectory(prefix="ra8-uv-probe-") as raw:
            probe_path = Path(raw) / "probe"
            parent = _open_parent_fd(probe_path)
            _require_private_temporary_parent(parent)
            writer, temporary = _new_temporary_fd(parent, PROBE_EXECUTABLE_MODE)
            write_exact_fd(writer, binary)
            os.fsync(writer)
            os.fchmod(writer, PROBE_EXECUTABLE_MODE)
            writer_state = os.fstat(writer)
            identity = _stat_identity(writer_state)
            flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW
            reader = os.open(temporary, flags, dir_fd=parent)
            if _stat_identity(os.fstat(reader)) != identity:
                fail("authenticated uv reader did not reopen the staged inode")
            os.close(writer)
            writer = -1
            _verify_portable_exec_fd(reader, binary, identity, linked=True)
            _verify_portable_exec_name(parent, temporary, binary, identity)
            try:
                yield reader, str(Path(raw) / temporary)
            finally:
                _verify_portable_exec_fd(reader, binary, identity, linked=True)
                _verify_portable_exec_name(parent, temporary, binary, identity)
                _unlink_matching_temporary(parent, temporary, identity, required=True)
                os.fsync(parent)
                temporary = ""
                _verify_portable_exec_fd(reader, binary, identity, linked=False)
    except (OSError, NotImplementedError, TypeError) as exc:
        fail(f"cannot stage portable authenticated uv executable: {exc}")
    finally:
        if writer >= 0:
            os.close(writer)
        if temporary and parent >= 0:
            with suppress(OSError):
                _unlink_matching_temporary(
                    parent,
                    temporary,
                    identity,
                    required=False,
                )
        if parent >= 0:
            os.close(parent)
        if reader >= 0:
            os.close(reader)


@contextmanager
def authenticated_executable_fd(binary: bytes) -> Iterator[tuple[int, str]]:
    """Yield one authenticated execution descriptor and its invocation path."""
    if os.name != "posix":
        fail("authenticated uv execution requires POSIX; use WSL on Windows")
    if sys.platform.startswith("linux"):
        descriptor = linux_sealed_exec_fd(binary)
        try:
            yield descriptor, executable_fd_path(descriptor)
        finally:
            os.close(descriptor)
    else:
        with portable_named_exec_snapshot(binary) as snapshot:
            yield snapshot


def run_uv_snapshot(
    binary: bytes,
    arguments: list[str],
    *,
    capture_output: bool = False,
    timeout: int | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run exact uv arguments from an authenticated immutable snapshot."""
    try:
        with authenticated_executable_fd(binary) as (descriptor, executable):
            return subprocess.run(  # noqa: S603 -- exact immutable FD.
                [executable, *arguments],
                check=False,
                capture_output=capture_output,
                text=True,
                timeout=timeout,
                pass_fds=(descriptor,),
            )
    except (OSError, subprocess.TimeoutExpired) as exc:
        fail(f"authenticated uv execution failed: {exc}")
