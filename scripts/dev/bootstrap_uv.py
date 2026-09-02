#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Install the repository-pinned uv binary from a verified release asset."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import platform
import re
import runpy
import signal
import stat
import struct
import sys
import sysconfig
import tarfile
import tempfile
import urllib.request
import zipfile
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
from typing import NoReturn, Protocol, Self

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootstrap_uv_exec

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path(__file__).with_name("uv_release.json")
DEFAULT_CACHE = ROOT / ".tools" / "uv"
MAX_ASSET_BYTES = 128 * 1024 * 1024
MAX_UV_BINARY_BYTES = 96 * 1024 * 1024
SHA256_HEX_LENGTH = hashlib.sha256().digest_size * 2
ZIP_UNIX_CREATE_SYSTEM = 3
PRIVATE_ARCHIVE_MODE = stat.S_IRUSR | stat.S_IWUSR
PRIVATE_EXECUTABLE_MODE = PRIVATE_ARCHIVE_MODE | stat.S_IXUSR
PUBLIC_ARCHIVE_MODE = PRIVATE_ARCHIVE_MODE | stat.S_IRGRP | stat.S_IROTH
PUBLIC_EXECUTABLE_MODE = PUBLIC_ARCHIVE_MODE | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
EXPECTED_PUBLIC_MODES = (0o644, 0o755)


class BootstrapError(RuntimeError):
    """Represent a fail-closed uv bootstrap error."""


class CacheApplyRequiredError(BootstrapError):
    """Report authenticated cache drift that a supported apply can repair."""


class DownloadHeaders(Protocol):
    """Describe the response header operation used by the downloader."""

    def get(self, name: str, default: str | None = None) -> str | None:
        """Return one response header."""


class DownloadResponse(Protocol):
    """Describe the bounded subset of an HTTP response used here."""

    headers: DownloadHeaders

    def read(self, size: int = -1) -> bytes:
        """Read at most size response bytes."""

    def __enter__(self) -> Self:
        """Enter the response context."""

    def __exit__(self, *_args: object) -> None:
        """Leave the response context."""


DownloadOpener = Callable[..., DownloadResponse]


def fail(message: str) -> NoReturn:
    """Stop bootstrap processing with one user-facing policy error."""
    raise BootstrapError(message)


def load_manifest(path: Path) -> dict[str, object]:
    """Load and validate the single uv release manifest."""
    try:
        document = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(f"cannot read uv manifest {path}: {exc}")
    if not isinstance(document, dict):
        fail("uv manifest root must be an object")
    if document.get("schema") != 1:
        fail("uv manifest schema must be 1")
    if document.get("repository") != "astral-sh/uv":
        fail("uv manifest repository must be astral-sh/uv")
    version = document.get("version")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        fail(f"uv manifest has an invalid release version: {version!r}")
    assets = document.get("assets")
    if not isinstance(assets, dict) or not assets:
        fail("uv manifest has no asset table")
    return document


def normalized_machine(machine: str) -> str:
    """Map operating-system architecture spellings to release triples."""
    value = machine.lower()
    if value in {"amd64", "x64", "x86_64"}:
        return "x86_64"
    if value in {"aarch64", "arm64"}:
        return "aarch64"
    fail(f"unsupported architecture: {machine}")


def has_musl_loader(directories: tuple[Path, ...]) -> bool:
    """Return whether a known musl loader name resolves to a regular file."""
    return any(
        path.is_file() for directory in directories for path in directory.glob("ld-musl-*.so.1")
    )


def detect_linux_libc() -> str:
    """Distinguish the two supported Linux release ABIs without executing host tools."""
    libc_name = platform.libc_ver()[0].lower()
    if "musl" in libc_name:
        return "musl"
    if libc_name in {"glibc", "gnu libc"}:
        return "gnu"
    abi_values = " ".join(
        str(sysconfig.get_config_var(name) or "").lower() for name in ("MULTIARCH", "HOST_GNU_TYPE")
    )
    if "musl" in abi_values:
        return "musl"
    if "linux-gnu" in abi_values:
        return "gnu"
    if has_musl_loader((Path("/lib"), Path("/usr/lib"))):
        return "musl"
    fail("unsupported or unidentified Linux libc")


def asset_key(system: str, machine: str, libc_name: str | None = None) -> str:
    """Return the manifest key for a supported host."""
    architecture = normalized_machine(machine)
    if system == "Linux":
        selected_libc = libc_name or detect_linux_libc()
        if selected_libc not in {"gnu", "musl"}:
            fail(f"unsupported Linux libc: {selected_libc}")
        return f"Linux|{architecture}|{selected_libc}"
    if system in {"Darwin", "Windows"}:
        return f"{system}|{architecture}"
    fail(f"unsupported operating system: {system}")


def expected_asset_name(key: str) -> str:
    """Derive the only acceptable official asset name for a host key."""
    system, architecture, *libc_value = key.split("|")
    if system == "Darwin" and not libc_value:
        return f"uv-{architecture}-apple-darwin.tar.gz"
    if system == "Windows" and not libc_value:
        return f"uv-{architecture}-pc-windows-msvc.zip"
    if system == "Linux" and len(libc_value) == 1 and libc_value[0] in {"gnu", "musl"}:
        return f"uv-{architecture}-unknown-linux-{libc_value[0]}.tar.gz"
    fail(f"invalid uv platform key: {key}")


def select_asset(
    manifest: dict[str, object],
    system: str | None = None,
    machine: str | None = None,
    libc_name: str | None = None,
) -> tuple[str, str, str]:
    """Select one release URL, name, and digest from the manifest."""
    key = asset_key(system or platform.system(), machine or platform.machine(), libc_name)
    assets = manifest["assets"]
    if not isinstance(assets, dict) or key not in assets:
        fail(f"uv manifest has no asset for {key}")
    record = assets[key]
    if not isinstance(record, dict):
        fail(f"uv asset record for {key} must be an object")
    name = record.get("name")
    digest = record.get("sha256")
    if not isinstance(name, str) or not isinstance(digest, str):
        fail(f"uv asset record for {key} is incomplete")
    if len(digest) != SHA256_HEX_LENGTH or any(char not in "0123456789abcdef" for char in digest):
        fail(f"uv asset record for {key} has an invalid SHA-256")
    expected_name = expected_asset_name(key)
    if name != expected_name:
        fail(f"uv asset record for {key} must name {expected_name}, got {name}")
    version = manifest["version"]
    repository = manifest["repository"]
    if not isinstance(version, str) or not isinstance(repository, str):
        fail("uv manifest release identity is malformed")
    release_url = f"https://github.com/{repository}/releases/download/{version}"
    return f"{release_url}/{name}", name, digest


def verify_payload(payload: bytes, expected: str) -> None:
    """Reject release bytes whose SHA-256 does not match the manifest."""
    actual = hashlib.sha256(payload).hexdigest()
    if actual != expected:
        fail(f"uv asset SHA-256 mismatch: expected {expected}, got {actual}")


def require_bounded_executable(stream: object, declared_size: int) -> bytes:
    """Read one executable member without allowing archive inflation."""
    if declared_size < 1 or declared_size > MAX_UV_BINARY_BYTES:
        fail(f"uv executable size is outside policy: {declared_size} bytes")
    if not hasattr(stream, "read"):
        fail("uv executable archive member is not readable")
    payload = stream.read(MAX_UV_BINARY_BYTES + 1)
    if not isinstance(payload, bytes) or len(payload) != declared_size:
        fail("uv executable size does not match archive metadata")
    return payload


def executable_bytes(payload: bytes, asset_name: str) -> bytes:
    """Read the one exact uv member from a verified release archive."""
    archive_stem = asset_name.removesuffix(".tar.gz").removesuffix(".zip")
    executable = "uv.exe" if asset_name.endswith(".zip") else "uv"
    expected_member = f"{archive_stem}/{executable}"
    if asset_name.endswith(".zip"):
        try:
            with zipfile.ZipFile(io.BytesIO(payload)) as archive:
                names = archive.namelist()
                executable_members = [
                    name for name in names if PurePosixPath(name).name == executable
                ]
                if names.count(expected_member) != 1 or executable_members != [expected_member]:
                    fail(f"uv archive lacks exact member {expected_member}")
                info = archive.getinfo(expected_member)
                if info.is_dir():
                    fail(f"uv archive member is not a file: {expected_member}")
                unix_type = stat.S_IFMT(info.external_attr >> 16)
                if info.create_system == ZIP_UNIX_CREATE_SYSTEM and unix_type not in {
                    0,
                    stat.S_IFREG,
                }:
                    fail(f"uv archive member is not a regular file: {expected_member}")
                with archive.open(info) as source:
                    return require_bounded_executable(source, info.file_size)
        except zipfile.BadZipFile as exc:
            fail(f"invalid uv release archive: {exc}")
    try:
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
            members = archive.getmembers()
            executable_members = [
                member for member in members if PurePosixPath(member.name).name == executable
            ]
            matches = [member for member in members if member.name == expected_member]
            if len(matches) != 1 or executable_members != matches or not matches[0].isfile():
                fail(f"uv archive lacks exact regular member {expected_member}")
            source = archive.extractfile(matches[0])
            if source is None:
                fail("uv executable could not be read from archive")
            with source:
                return require_bounded_executable(source, matches[0].size)
    except tarfile.TarError as exc:
        fail(f"invalid uv release archive: {exc}")


def download_payload(url: str, opener: DownloadOpener = urllib.request.urlopen) -> bytes:
    """Download one bounded release asset and report transport failures."""
    if not url.startswith("https://github.com/astral-sh/uv/releases/download/"):
        fail(f"refusing non-official uv release URL: {url}")
    request = urllib.request.Request(  # noqa: S310 -- URL is constrained above.
        url, headers={"User-Agent": "ra8-firmware-uv-bootstrap"}
    )
    try:
        with opener(request, timeout=60) as response:
            length = response.headers.get("Content-Length")
            if length is not None and int(length) > MAX_ASSET_BYTES:
                fail(f"uv asset exceeds {MAX_ASSET_BYTES} bytes")
            payload = response.read(MAX_ASSET_BYTES + 1)
    except BootstrapError:
        raise
    except (OSError, ValueError) as exc:
        fail(f"cannot download pinned uv asset {url}: {exc}")
    if len(payload) > MAX_ASSET_BYTES:
        fail(f"uv asset exceeds {MAX_ASSET_BYTES} bytes")
    return payload


def reject_symlink_components(
    cache_root: Path, destination: Path, anchor: Path | None = None
) -> None:
    """Reject symlinks from a trusted anchor through the cache destination."""
    cache_root = cache_root.absolute()
    destination = destination.absolute()
    if anchor is None:
        try:
            cache_root.relative_to(ROOT)
        except ValueError:
            anchor = cache_root.parent
        else:
            anchor = ROOT
    anchor = anchor.absolute()
    try:
        relative = destination.relative_to(cache_root)
    except ValueError:
        fail(f"uv destination escapes cache root: {destination}")
    try:
        cache_relative = cache_root.relative_to(anchor)
    except ValueError:
        fail(f"uv cache root escapes trusted anchor: {cache_root}")
    current = anchor
    if current.is_symlink():
        fail(f"uv cache anchor is a symlink: {current}")
    for component in (*cache_relative.parts, *relative.parts):
        current /= component
        if current.is_symlink():
            fail(f"symlink in uv cache path: {current}")


def cache_destination(cache_root: Path, version: str, asset_name: str) -> Path:
    """Return the platform-specific executable cache path."""
    archive_stem = asset_name.removesuffix(".tar.gz").removesuffix(".zip")
    executable = "uv.exe" if asset_name.endswith(".zip") else "uv"
    return cache_root / version / archive_stem / executable


def validated_cached_payload(archive_path: Path, digest: str) -> bytes:
    """Read and authenticate a retained release archive on every reuse."""
    if os.name == "posix":
        descriptor = open_cache_fd(archive_path)
        try:
            payload, _ = read_stable_fd(descriptor, archive_path, MAX_ASSET_BYTES)
        finally:
            os.close(descriptor)
        verify_payload(payload, digest)
        return payload
    if archive_path.is_symlink() or not archive_path.is_file():
        fail(f"verified uv archive is missing: {archive_path}")
    try:
        payload = archive_path.read_bytes()
    except OSError as exc:
        fail(f"cannot read cached uv archive {archive_path}: {exc}")
    verify_payload(payload, digest)
    return payload


def validated_cached_binary(
    archive_path: Path, destination: Path, asset_name: str, digest: str
) -> bytes:
    """Authenticate the retained archive and compare its executable byte-for-byte."""
    if os.name == "posix":
        with authenticated_cache_fds(
            archive_path, destination, asset_name, digest
        ) as authenticated:
            return authenticated[2]
    payload = validated_cached_payload(archive_path, digest)
    binary = executable_bytes(payload, asset_name)
    if destination.is_symlink() or not destination.is_file():
        fail(f"cached uv executable is missing: {destination}")
    try:
        installed = destination.read_bytes()
    except OSError as exc:
        fail(f"cannot read cached uv executable {destination}: {exc}")
    if not binary or installed != binary:
        fail(f"cached uv executable differs from verified archive: {destination}")
    return binary


def write_atomic(path: Path, payload: bytes, executable: bool = False) -> None:
    """Write one cache artifact through a held no-follow parent."""
    if os.name != "posix":
        fail("authenticated uv cache writes require POSIX; use WSL on Windows")
    mode = PRIVATE_EXECUTABLE_MODE if executable else PRIVATE_ARCHIVE_MODE
    try:
        bootstrap_uv_exec.write_atomic_nofollow(path, payload, mode)
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))


def open_cache_fd(path: Path) -> int:
    """Open one POSIX cache artifact through a no-follow parent walk."""
    try:
        return bootstrap_uv_exec.open_regular_nofollow(path)
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))


def read_stable_fd(descriptor: int, path: Path, maximum: int) -> tuple[bytes, os.stat_result]:
    """Read one bounded regular FD and reject concurrent inode mutation."""
    before = os.fstat(descriptor)
    try:
        with os.fdopen(os.dup(descriptor), "rb") as source:
            payload = source.read(maximum + 1)
        after = os.fstat(descriptor)
    except OSError as exc:
        fail(f"cannot read cached uv artifact {path}: {exc}")
    stable = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_nlink")
    if any(getattr(before, field) != getattr(after, field) for field in stable):
        fail(f"cached uv artifact changed while authenticating: {path}")
    if len(payload) > maximum:
        fail(f"cached uv artifact exceeds policy: {path}")
    return payload, after


@contextmanager
def authenticated_cache_fds(
    archive_path: Path, destination: Path, asset_name: str, digest: str
) -> Iterator[tuple[int, int, bytes, os.stat_result, os.stat_result]]:
    """Hold exact authenticated archive and executable descriptors."""
    archive_fd = open_cache_fd(archive_path)
    destination_fd = -1
    try:
        destination_fd = open_cache_fd(destination)
        payload, archive_state = read_stable_fd(archive_fd, archive_path, MAX_ASSET_BYTES)
        verify_payload(payload, digest)
        binary = executable_bytes(payload, asset_name)
        installed, installed_state = read_stable_fd(
            destination_fd, destination, MAX_UV_BINARY_BYTES
        )
        if not binary or installed != binary:
            fail(f"cached uv executable differs from verified archive: {destination}")
        yield archive_fd, destination_fd, binary, archive_state, installed_state
    finally:
        if destination_fd >= 0:
            os.close(destination_fd)
        os.close(archive_fd)


def verify_fd_mode(path: Path, descriptor: int, mode: int) -> None:
    """Require an exact authenticated descriptor to carry one shared mode."""
    state = os.fstat(descriptor)
    if stat.S_IMODE(state.st_mode) != mode:
        message = f"cached uv permissions require an apply: {path}"
        raise CacheApplyRequiredError(message)


def probe_authenticated_uv(binary: bytes, version: str) -> None:
    """Probe an immutable authenticated uv snapshot for its exact version."""
    try:
        completed = bootstrap_uv_exec.run_uv_snapshot(
            binary, ["--version"], capture_output=True, timeout=10
        )
    except bootstrap_uv_exec.UvExecError as exc:
        fail(str(exc))
    if completed.returncode != 0 or completed.stdout.split()[:2] != ["uv", version]:
        fail(f"installed uv failed its version probe: {completed.stderr.strip()}")


def propagate_child_status(status: int) -> int:
    """Return an exit code or terminate this wrapper through the child's signal."""
    if status >= 0:
        return status
    signum = -status
    unmask = getattr(signal, "pthread_sigmask", None)
    if signum >= signal.NSIG or unmask is None:
        fail("authenticated uv child returned an unsupported signal status")
    uncatchable = (signal.SIGKILL, signal.SIGSTOP)
    try:
        if signum not in uncatchable:
            signal.signal(signum, signal.SIG_DFL)
        unmask(signal.SIG_UNBLOCK, {signum})
        os.kill(os.getpid(), signum)
    except (OSError, ValueError) as exc:
        fail(f"cannot propagate authenticated uv child signal: {exc}")
    fail("authenticated uv child signal did not terminate the wrapper")


def verify_fd_unchanged(path: Path, descriptor: int, expected: os.stat_result) -> None:
    """Require cache identity and content metadata to remain authenticated."""
    current = os.fstat(descriptor)
    stable = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns", "st_nlink")
    if any(getattr(expected, field) != getattr(current, field) for field in stable):
        fail(f"cached uv artifact changed after authenticating: {path}")


def verify_fd_path(path: Path, descriptor: int, mode: int) -> None:
    """Require one chmod target to remain the exact regular path opened."""
    descriptor_state = os.fstat(descriptor)
    reopened = -1
    try:
        reopened = open_cache_fd(path)
        path_state = os.fstat(reopened)
    except (BootstrapError, OSError) as exc:
        fail(f"cached uv path moved during permission repair: {path}: {exc}")
    finally:
        if reopened >= 0:
            os.close(reopened)
    same_file = (descriptor_state.st_dev, descriptor_state.st_ino) == (
        path_state.st_dev,
        path_state.st_ino,
    )
    if not stat.S_ISREG(path_state.st_mode) or path_state.st_nlink != 1 or not same_file:
        fail(f"cached uv path moved during permission repair: {path}")
    if stat.S_IMODE(descriptor_state.st_mode) != mode:
        fail(f"cached uv permissions did not converge: {path}")


def normalize_cached_modes(
    archive_path: Path, destination: Path, asset_name: str, digest: str
) -> None:
    """Authenticate exact POSIX FDs, then make those public release bytes shared."""
    if os.name != "posix":
        return
    try:
        with authenticated_cache_fds(archive_path, destination, asset_name, digest) as descriptors:
            archive_fd, destination_fd, _, _, _ = descriptors
            os.fchmod(archive_fd, PUBLIC_ARCHIVE_MODE)
            os.fchmod(destination_fd, PUBLIC_EXECUTABLE_MODE)
            verify_fd_path(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
            verify_fd_path(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
    except OSError as exc:
        fail(f"cannot normalize authenticated uv cache permissions: {exc}")


def verify_cached_modes(archive_path: Path, destination: Path) -> None:
    """Require exact shared POSIX modes without mutating either cache file."""
    if os.name != "posix":
        return
    for path, mode in (
        (archive_path, PUBLIC_ARCHIVE_MODE),
        (destination, PUBLIC_EXECUTABLE_MODE),
    ):
        try:
            state = os.lstat(path)
        except OSError as exc:
            fail(f"cannot inspect cached uv permissions {path}: {exc}")
        if not stat.S_ISREG(state.st_mode) or state.st_nlink != 1:
            fail(f"cached uv artifact is not one single-link regular file: {path}")
        if stat.S_IMODE(state.st_mode) != mode:
            message = f"cached uv permissions require an apply: {path}"
            raise CacheApplyRequiredError(message)


def verify_cached_uv(manifest_path: Path, cache_root: Path) -> Path:
    """Authenticate and probe an existing cache without network access or writes."""
    manifest = load_manifest(manifest_path)
    _, asset_name, digest = select_asset(manifest)
    version = manifest["version"]
    if not isinstance(version, str):
        fail("uv manifest version is not a string")
    cache_root = cache_root.absolute()
    destination = cache_destination(cache_root, version, asset_name)
    archive_path = destination.parent / asset_name
    reject_symlink_components(cache_root, archive_path)
    reject_symlink_components(cache_root, destination)
    if not archive_path.exists() and not destination.exists():
        message = f"authenticated uv cache requires an apply: {destination}"
        raise CacheApplyRequiredError(message)
    if not archive_path.exists():
        fail(f"cached uv has no authenticated archive: {destination}")
    if not destination.exists():
        validated_cached_payload(archive_path, digest)
        message = f"authenticated uv cache requires an apply: {destination}"
        raise CacheApplyRequiredError(message)
    if os.name != "posix":
        validated_cached_binary(archive_path, destination, asset_name, digest)
        return destination
    with authenticated_cache_fds(archive_path, destination, asset_name, digest) as descriptors:
        archive_fd, destination_fd, binary, archive_state, installed_state = descriptors
        verify_fd_mode(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_mode(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
        probe_authenticated_uv(binary, version)
        verify_fd_unchanged(archive_path, archive_fd, archive_state)
        verify_fd_unchanged(destination, destination_fd, installed_state)
        verify_fd_path(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_path(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
    return destination


def run_cached_uv(
    manifest_path: Path,
    cache_root: Path,
    arguments: list[str],
    *,
    ensure: bool,
) -> int:
    """Run uv only from bytes authenticated and snapshotted by this process."""
    if not arguments:
        fail("authenticated uv execution requires at least one uv argument")
    if os.name != "posix":
        fail("authenticated uv execution requires POSIX; use WSL on Windows")
    destination = (
        ensure_uv(manifest_path, cache_root)
        if ensure
        else verify_cached_uv(manifest_path, cache_root)
    )
    manifest = load_manifest(manifest_path)
    _, asset_name, digest = select_asset(manifest)
    archive_path = destination.parent / asset_name
    with authenticated_cache_fds(archive_path, destination, asset_name, digest) as descriptors:
        archive_fd, destination_fd, binary, archive_state, installed_state = descriptors
        verify_fd_mode(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_mode(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
        try:
            completed = bootstrap_uv_exec.run_uv_snapshot(binary, arguments)
        except bootstrap_uv_exec.UvExecError as exc:
            fail(str(exc))
        verify_fd_unchanged(archive_path, archive_fd, archive_state)
        verify_fd_unchanged(destination, destination_fd, installed_state)
        verify_fd_path(archive_path, archive_fd, PUBLIC_ARCHIVE_MODE)
        verify_fd_path(destination, destination_fd, PUBLIC_EXECUTABLE_MODE)
    return propagate_child_status(completed.returncode)


def ensure_uv(manifest_path: Path, cache_root: Path) -> Path:
    """Download, verify, and atomically install uv when it is not cached."""
    manifest = load_manifest(manifest_path)
    url, asset_name, digest = select_asset(manifest)
    version = manifest["version"]
    if not isinstance(version, str):
        fail("uv manifest version is not a string")
    cache_root = cache_root.absolute()
    destination = cache_destination(cache_root, version, asset_name)
    archive_path = destination.parent / asset_name
    reject_symlink_components(cache_root, archive_path)
    reject_symlink_components(cache_root, destination)

    if not (archive_path.exists() and destination.exists()):
        if destination.exists():
            fail(f"cached uv has no authenticated archive: {destination}")
        if archive_path.exists():
            payload = validated_cached_payload(archive_path, digest)
        else:
            payload = download_payload(url)
            verify_payload(payload, digest)
        binary = executable_bytes(payload, asset_name)
        if not binary:
            fail("uv release archive contained an empty executable")
        if not archive_path.exists():
            write_atomic(archive_path, payload)
        write_atomic(destination, binary, executable=True)
    normalize_cached_modes(archive_path, destination, asset_name, digest)
    return verify_cached_uv(manifest_path, cache_root)


def synthetic_archive(
    asset_name: str,
    binary: bytes,
    member_name: str | None = None,
    member_kind: str = "file",
) -> bytes:
    """Create a small release-shaped archive for offline negative tests."""
    stem = asset_name.removesuffix(".tar.gz").removesuffix(".zip")
    executable = "uv.exe" if asset_name.endswith(".zip") else "uv"
    name = member_name or f"{stem}/{executable}"
    output = io.BytesIO()
    if asset_name.endswith(".zip"):
        with zipfile.ZipFile(output, mode="w") as archive:
            if member_kind == "symlink":
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(info, b"elsewhere")
            else:
                archive.writestr(name, binary)
                if member_kind == "duplicate":
                    archive.writestr(f"other/{executable}", binary)
        result = bytearray(output.getvalue())
        if member_kind == "oversized":
            local_header = result.index(b"PK\x03\x04")
            central_header = result.index(b"PK\x01\x02")
            struct.pack_into("<I", result, local_header + 22, MAX_UV_BINARY_BYTES + 1)
            struct.pack_into("<I", result, central_header + 24, MAX_UV_BINARY_BYTES + 1)
        return bytes(result)
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        info = tarfile.TarInfo(name)
        if member_kind == "symlink":
            info.type = tarfile.SYMTYPE
            info.linkname = "elsewhere"
            archive.addfile(info)
        else:
            info.size = len(binary)
            archive.addfile(info, io.BytesIO(binary))
            if member_kind == "duplicate":
                duplicate = tarfile.TarInfo(f"other/{executable}")
                duplicate.size = len(binary)
                archive.addfile(duplicate, io.BytesIO(binary))
    return output.getvalue()


def expect_bootstrap_error(
    action: Callable[[], object], label: str, message: str | None = None
) -> None:
    """Require one negative selftest action to fail closed."""
    try:
        action()
    except BootstrapError as error:
        if message is not None and message not in str(error):
            fail(f"selftest: {label} returned unexpected error: {error}")
        return
    fail(f"selftest: {label} passed unexpectedly")


def archive_selftest() -> None:
    """Exercise exact-member and malformed-archive handling."""
    for asset_name in (
        "uv-x86_64-unknown-linux-gnu.tar.gz",
        "uv-x86_64-pc-windows-msvc.zip",
    ):
        payload = synthetic_archive(asset_name, b"verified-uv")
        if executable_bytes(payload, asset_name) != b"verified-uv":
            fail(f"selftest: valid archive failed for {asset_name}")
        expect_bootstrap_error(
            lambda asset=asset_name: executable_bytes(
                synthetic_archive(asset, b"verified-uv", "wrong/place/uv"), asset
            ),
            f"wrong member path for {asset_name}",
        )
        expect_bootstrap_error(
            lambda asset=asset_name: executable_bytes(synthetic_archive(asset, b""), asset),
            f"empty executable for {asset_name}",
        )
        expect_bootstrap_error(
            lambda asset=asset_name: executable_bytes(
                synthetic_archive(asset, b"verified-uv", member_kind="duplicate"), asset
            ),
            f"duplicate executable basename for {asset_name}",
        )
        expect_bootstrap_error(
            lambda asset=asset_name: executable_bytes(b"not an archive", asset),
            f"corrupt archive for {asset_name}",
        )
    for asset_name in (
        "uv-x86_64-unknown-linux-gnu.tar.gz",
        "uv-x86_64-pc-windows-msvc.zip",
    ):
        expect_bootstrap_error(
            lambda asset=asset_name: executable_bytes(
                synthetic_archive(asset, b"", member_kind="symlink"), asset
            ),
            f"symlink archive member for {asset_name}",
        )
    zip_name = "uv-x86_64-pc-windows-msvc.zip"
    expect_bootstrap_error(
        lambda: executable_bytes(
            synthetic_archive(zip_name, b"uv", member_kind="oversized"), zip_name
        ),
        "oversized executable metadata",
        "outside policy",
    )


def cache_selftest() -> None:
    """Exercise archive reauthentication and cache path hardening."""
    asset_name = "uv-x86_64-unknown-linux-gnu.tar.gz"
    archive = synthetic_archive(asset_name, b"verified-uv")
    digest = hashlib.sha256(archive).hexdigest()
    with tempfile.TemporaryDirectory(prefix="ra8-uv-cache-test-") as raw:
        root = Path(raw)
        directory = root / "0.0.0" / asset_name.removesuffix(".tar.gz")
        directory.mkdir(parents=True)
        archive_path = directory / asset_name
        destination = directory / "uv"
        archive_path.write_bytes(archive)
        destination.write_bytes(b"verified-uv")
        validated_cached_binary(archive_path, destination, asset_name, digest)

        destination.write_bytes(b"mutated")
        expect_bootstrap_error(
            lambda: validated_cached_binary(archive_path, destination, asset_name, digest),
            "mutated cached executable",
        )
        destination.write_bytes(b"verified-uv")
        archive_path.write_bytes(b"mutated")
        expect_bootstrap_error(
            lambda: validated_cached_binary(archive_path, destination, asset_name, digest),
            "mutated cached archive",
        )
        archive_path.unlink()
        expect_bootstrap_error(
            lambda: validated_cached_binary(archive_path, destination, asset_name, digest),
            "binary without authenticated archive",
        )
        destination.unlink()
        outside = root / "outside"
        outside.write_bytes(archive)
        archive_path.symlink_to(outside)
        expect_bootstrap_error(
            lambda: validated_cached_payload(archive_path, digest),
            "symlinked cached archive",
        )
        archive_path.unlink()
        real_parent = root / "real-parent"
        real_parent.mkdir()
        linked_parent = root / "linked-parent"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        expect_bootstrap_error(
            lambda: reject_symlink_components(root, linked_parent / "uv"),
            "symlinked cache parent",
        )
        workspace = root / "workspace"
        workspace.mkdir()
        external_tools = root / "external-tools"
        external_tools.mkdir()
        (workspace / ".tools").symlink_to(external_tools, target_is_directory=True)
        expect_bootstrap_error(
            lambda: reject_symlink_components(
                workspace / ".tools" / "uv",
                workspace / ".tools" / "uv" / "0.0.0" / "uv",
                anchor=workspace,
            ),
            "symlinked ancestor before cache root",
        )


def cache_mode_selftest() -> None:
    """Run the adjacent adversarial mode suite under the bootstrap module."""
    namespace = runpy.run_path(
        str(Path(__file__).with_name("bootstrap_uv_mode_selftest.py")),
        init_globals={"bootstrap": sys.modules[__name__]},
    )
    runner = namespace.get("run_mode_selftest")
    if not callable(runner):
        fail("uv mode selftest module has no runner")
    runner()


def manifest_matrix_selftest(manifest: dict[str, object]) -> None:
    """Prove every supported mapping and reject unsupported/swapped records."""
    expected_keys = {
        "Darwin|aarch64",
        "Darwin|x86_64",
        "Windows|aarch64",
        "Windows|x86_64",
        "Linux|aarch64|gnu",
        "Linux|aarch64|musl",
        "Linux|x86_64|gnu",
        "Linux|x86_64|musl",
    }
    assets = manifest["assets"]
    if not isinstance(assets, dict) or set(assets) != expected_keys:
        fail("selftest: manifest platform matrix is incomplete or over-broad")
    for key in expected_keys:
        system, machine, *libc_value = key.split("|")
        _, name, digest = select_asset(
            manifest, system, machine, libc_value[0] if libc_value else None
        )
        if name != expected_asset_name(key) or len(digest) != SHA256_HEX_LENGTH:
            fail(f"selftest: malformed selected asset for {key}")
    for system, machine, libc_name in (
        ("FreeBSD", "x86_64", None),
        ("Linux", "riscv64", "gnu"),
        ("Linux", "x86_64", "uclibc"),
    ):
        expect_bootstrap_error(
            lambda s=system, m=machine, libc=libc_name: select_asset(manifest, s, m, libc),
            f"unsupported host {system}/{machine}/{libc_name}",
        )
    mutated = json.loads(json.dumps(manifest))
    mutated["assets"]["Linux|x86_64|gnu"]["name"] = "uv-aarch64-unknown-linux-gnu.tar.gz"
    expect_bootstrap_error(
        lambda: select_asset(mutated, "Linux", "x86_64", "gnu"),
        "architecture-swapped asset",
    )


def transport_and_libc_selftest(manifest: dict[str, object]) -> None:
    """Exercise timeout handling and non-executing musl-loader detection."""

    def timeout_opener(*_args: object, **_kwargs: object) -> DownloadResponse:
        message = "timed out"
        raise TimeoutError(message)

    url, _, _ = select_asset(manifest, "Linux", "x86_64", "gnu")
    expect_bootstrap_error(
        lambda: download_payload(url, timeout_opener),
        "download timeout",
        "cannot download pinned uv asset",
    )
    with tempfile.TemporaryDirectory(prefix="ra8-musl-detect-") as raw:
        directory = Path(raw)
        loader_target = directory / "loader"
        loader_target.write_bytes(b"musl")
        (directory / "ld-musl-x86_64.so.1").symlink_to(loader_target)
        if not has_musl_loader((directory,)):
            fail("selftest: resolving musl loader symlink was rejected")


def run_selftest() -> None:
    """Exercise supported mappings and all important fail-closed paths."""
    manifest = load_manifest(DEFAULT_MANIFEST)
    manifest_matrix_selftest(manifest)
    expect_bootstrap_error(lambda: verify_payload(b"tampered", "0" * 64), "checksum mismatch")
    payload = b"uv-test-payload"
    verify_payload(payload, hashlib.sha256(payload).hexdigest())
    transport_and_libc_selftest(manifest)
    archive_selftest()
    cache_selftest()
    cache_mode_selftest()
    print("bootstrap_uv.py --selftest: PASS")


def parse_args() -> argparse.Namespace:
    """Parse the small bootstrap command-line interface."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--ensure", action="store_true", help="install and print pinned uv")
    mode.add_argument(
        "--verify-cache",
        action="store_true",
        help="authenticate an existing cache without downloads or writes",
    )
    mode.add_argument(
        "--check-cache-modes",
        action="store_true",
        help="check shared POSIX cache modes without authenticating or writing",
    )
    mode.add_argument("--print-path", action="store_true", help="print cache path without writes")
    mode.add_argument(
        "--run",
        nargs=argparse.REMAINDER,
        metavar="UV_ARG",
        help="run uv from an authenticated existing-cache snapshot",
    )
    mode.add_argument(
        "--ensure-and-run",
        nargs=argparse.REMAINDER,
        metavar="UV_ARG",
        help="ensure the cache, then run uv from an authenticated snapshot",
    )
    mode.add_argument("--selftest", action="store_true", help="run offline fail-closed tests")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cache-root", type=Path, default=DEFAULT_CACHE)
    return parser.parse_args()


def main() -> int:
    """Dispatch the requested bootstrap mode."""
    args = parse_args()
    if args.selftest:
        run_selftest()
        status = 0
    elif args.run is not None:
        status = run_cached_uv(args.manifest, args.cache_root, args.run, ensure=False)
    elif args.ensure_and_run is not None:
        status = run_cached_uv(args.manifest, args.cache_root, args.ensure_and_run, ensure=True)
    else:
        manifest = load_manifest(args.manifest)
        _, asset_name, _ = select_asset(manifest)
        version = manifest["version"]
        if not isinstance(version, str):
            fail("uv manifest version is not a string")
        destination = cache_destination(args.cache_root, version, asset_name)
        if args.print_path:
            print(destination)
        elif args.check_cache_modes:
            verify_cached_modes(destination.parent / asset_name, destination)
            print(destination)
        elif args.verify_cache:
            print(verify_cached_uv(args.manifest, args.cache_root))
        else:
            print(ensure_uv(args.manifest, args.cache_root))
        status = 0
    return status


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CacheApplyRequiredError as error:
        print(f"APPLY REQUIRED: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    except BootstrapError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1) from error
