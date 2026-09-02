# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Adversarial permission and race selftests for the pinned uv bootstrap."""

from __future__ import annotations

import hashlib
import importlib
import json
import os
import platform
import signal
import socket
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable
from functools import partial
from pathlib import Path
from typing import Any, cast
from unittest import mock

b = cast(Any, globals()["bootstrap"])
ACTUAL_UV_RUN_INVOCATION = 2
EXPECTED_CHILD_STATUS = 37
AUTH_ERROR_STATUS = 1
APPLY_REQUIRED_STATUS = 2
EXPECTED_PORTABLE_READER_OPENS = 3


def mode_fixture(
    root: Path, populate: bool = True, binary: bytes = b"verified-uv"
) -> tuple[Path, Path, Path, str, bytes]:
    """Create one release-shaped mode fixture and return its exact authorities."""
    key = b.asset_key(platform.system(), platform.machine())
    asset_name = b.expected_asset_name(key)
    payload = b.synthetic_archive(asset_name, binary)
    manifest = {
        "schema": 1,
        "repository": "astral-sh/uv",
        "version": "0.0.0",
        "assets": {key: {"name": asset_name, "sha256": hashlib.sha256(payload).hexdigest()}},
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="ascii")
    destination = b.cache_destination(root, "0.0.0", asset_name)
    archive = destination.parent / asset_name
    if populate:
        destination.parent.mkdir(parents=True)
        archive.write_bytes(payload)
        destination.write_bytes(binary)
        archive.chmod(b.PRIVATE_ARCHIVE_MODE)
        destination.chmod(b.PRIVATE_EXECUTABLE_MODE)
    return manifest_path, archive, destination, asset_name, payload


def expect_modes(archive: Path, destination: Path, archive_mode: int, binary_mode: int) -> None:
    """Require exact fixture modes after one positive or negative action."""
    if stat.S_IMODE(archive.stat().st_mode) != archive_mode:
        b.fail(f"selftest: cached archive mode is not {archive_mode:04o}")
    if stat.S_IMODE(destination.stat().st_mode) != binary_mode:
        b.fail(f"selftest: cached executable mode is not {binary_mode:04o}")


def expect_exec_failure(action: Callable[[], object], label: str) -> None:
    """Require one execution-boundary action to fail closed."""
    try:
        result = action()
    except (OSError, b.bootstrap_uv_exec.UvExecError):
        return
    if isinstance(result, int):
        os.close(result)
    b.fail(f"selftest: uv execution boundary {label} passed unexpectedly")


def owned_stat_wrapper(
    real_stat: Callable[..., os.stat_result], alias: str, root_descriptor: int, owner_uid: int
) -> Callable[..., os.stat_result]:
    """Give a Darwin alias an explicit owner independent of the test process."""

    def wrapped_stat(
        candidate: str | Path,
        *,
        dir_fd: int | None = None,
        follow_symlinks: bool = True,
    ) -> os.stat_result:
        state = real_stat(candidate, dir_fd=dir_fd, follow_symlinks=follow_symlinks)
        if str(candidate) == alias and dir_fd == root_descriptor and not follow_symlinks:
            fields = list(state)
            fields[4] = owner_uid
            return os.stat_result(fields)
        return state

    return wrapped_stat


def open_simulated_darwin_parent(
    root: Path,
    alias: str,
    *,
    scenario: str = "valid",
) -> int:
    """Run the production parent walker against one synthetic system root."""
    platform_name = "linux" if scenario == "linux" else "darwin"
    root_owned = scenario != "owner"
    redirect_alias_to = "attacker/var" if scenario == "inode" else None
    reject_root = scenario == "root"
    directory_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_DIRECTORY
    real_open = os.open
    real_stat = os.stat
    root_descriptor = real_open(root, directory_flags)

    def wrapped_open(
        candidate: str | Path,
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        is_alias_follow = (
            str(candidate) == alias and dir_fd == root_descriptor and flags & os.O_NOFOLLOW == 0
        )
        if is_alias_follow and redirect_alias_to is not None:
            return real_open(redirect_alias_to, flags, mode, dir_fd=root_descriptor)
        return real_open(candidate, flags, mode, dir_fd=dir_fd)

    stat_hook = owned_stat_wrapper(
        real_stat,
        alias,
        root_descriptor,
        0 if root_owned else 1,
    )
    root_error = b.bootstrap_uv_exec.UvExecError("simulated untrusted system root")
    trust_hook = mock.Mock(side_effect=root_error) if reject_root else mock.Mock()
    with (
        mock.patch.object(os, "open", side_effect=wrapped_open),
        mock.patch.object(os, "stat", side_effect=stat_hook),
        mock.patch.object(b.bootstrap_uv_exec, "_require_trusted_system_root", trust_hook),
    ):
        return b.bootstrap_uv_exec.open_parent_components(
            root_descriptor,
            (alias, "folders"),
            directory_flags,
            create=False,
            platform_name=platform_name,
        )


def darwin_root_alias_acceptance_selftest() -> None:
    """Accept only the two fixed Darwin aliases and the physical spelling."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-darwin-alias-ok-") as raw:
        root = Path(raw)
        for alias in ("var", "tmp"):
            physical = root / "private" / alias / "folders"
            physical.mkdir(parents=True)
            (root / alias).symlink_to(Path("private") / alias, target_is_directory=True)
            descriptor = open_simulated_darwin_parent(root, alias)
            try:
                state = os.fstat(descriptor)
                if (state.st_dev, state.st_ino) != (
                    physical.stat().st_dev,
                    physical.stat().st_ino,
                ):
                    b.fail(f"selftest: Darwin /{alias} alias opened the wrong directory")
            finally:
                os.close(descriptor)
        flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_DIRECTORY
        root_descriptor = os.open(root, flags)
        descriptor = b.bootstrap_uv_exec.open_parent_components(
            root_descriptor,
            ("private", "var", "folders"),
            flags,
            create=False,
            platform_name="darwin",
        )
        os.close(descriptor)


def darwin_root_alias_rejection_selftest() -> None:
    """Reject hostile targets, owners, identities, roots, and later symlinks."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-darwin-alias-bad-") as raw:
        root = Path(raw)
        (root / "private" / "var" / "folders").mkdir(parents=True)
        (root / "attacker" / "var" / "folders").mkdir(parents=True)
        (root / "var").symlink_to("attacker/var", target_is_directory=True)
        expect_exec_failure(partial(open_simulated_darwin_parent, root, "var"), "alias target")
        (root / "var").unlink()
        (root / "var").symlink_to("private/var", target_is_directory=True)
        expect_exec_failure(
            partial(open_simulated_darwin_parent, root, "var", scenario="owner"),
            "owner",
        )
        expect_exec_failure(
            partial(open_simulated_darwin_parent, root, "var", scenario="inode"),
            "inode mismatch",
        )
        expect_exec_failure(
            partial(open_simulated_darwin_parent, root, "var", scenario="root"),
            "untrusted root",
        )
        expect_exec_failure(
            partial(open_simulated_darwin_parent, root, "var", scenario="linux"),
            "accepted outside Darwin",
        )
    with tempfile.TemporaryDirectory(prefix="ra8-uv-darwin-later-link-") as raw:
        root = Path(raw)
        physical = root / "private" / "tmp"
        (physical / "real-folders").mkdir(parents=True)
        (physical / "folders").symlink_to("real-folders", target_is_directory=True)
        (root / "tmp").symlink_to("private/tmp", target_is_directory=True)
        expect_exec_failure(partial(open_simulated_darwin_parent, root, "tmp"), "alias later link")


def write_metadata(path: Path) -> tuple[int, int, int, int]:
    """Return file metadata that can change only through a write-like operation."""
    state = path.stat()
    return stat.S_IMODE(state.st_mode), state.st_size, state.st_mtime_ns, state.st_ctime_ns


def cache_mode_convergence_selftest() -> None:
    """Prove fresh and retained POSIX caches converge after exact authentication."""
    probe = subprocess.CompletedProcess(["uv", "--version"], 0, "uv 0.0.0\n", "")
    for populate in (False, True):
        with tempfile.TemporaryDirectory(prefix="ra8-uv-mode-test-") as raw:
            root = Path(raw)
            manifest, archive, destination, _, payload = mode_fixture(root, populate)
            with (
                mock.patch.object(subprocess, "run", return_value=probe),
                mock.patch.object(b, "download_payload", return_value=payload),
            ):
                b.ensure_uv(manifest, root)
            expect_modes(archive, destination, *b.EXPECTED_PUBLIC_MODES)


def cache_mode_authentication_selftest() -> None:
    """Prove digest and byte mutations cannot reach either fchmod call."""
    for target in ("archive", "binary"):
        with tempfile.TemporaryDirectory(prefix="ra8-uv-auth-mode-") as raw:
            root = Path(raw)
            manifest, archive, destination, _, payload = mode_fixture(root)
            if target == "archive":
                mutated = bytearray(payload)
                mutated[4] ^= 1
                archive.write_bytes(mutated)
            else:
                destination.write_bytes(b"other-uv")
            b.expect_bootstrap_error(
                lambda manifest=manifest, root=root: b.ensure_uv(manifest, root),
                f"tampered cached {target} permission repair",
            )
            expect_modes(
                archive,
                destination,
                b.PRIVATE_ARCHIVE_MODE,
                b.PRIVATE_EXECUTABLE_MODE,
            )


def cache_open_flags_selftest() -> None:
    """Prove parent and final FD opens require every POSIX safety flag."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-open-flags-") as raw:
        path = Path(raw) / "artifact"
        path.write_bytes(b"verified")
        real_open = os.open
        seen: list[tuple[str, int, int | None, bool]] = []

        def record_open(
            candidate: str | Path,
            flags: int,
            mode: int = 0o777,
            *,
            dir_fd: int | None = None,
        ) -> int:
            parent_is_root = False
            if dir_fd is not None:
                parent_state = os.fstat(dir_fd)
                root_state = Path("/").stat(follow_symlinks=False)
                parent_is_root = (parent_state.st_dev, parent_state.st_ino) == (
                    root_state.st_dev,
                    root_state.st_ino,
                )
            seen.append((str(candidate), flags, dir_fd, parent_is_root))
            return real_open(candidate, flags, mode, dir_fd=dir_fd)

        with mock.patch.object(os, "open", side_effect=record_open):
            descriptor = b.open_cache_fd(path)
            os.close(descriptor)
        final = [item for item in seen if item[0] == path.name and item[2] is not None]
        final_required = os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK
        parent_required = os.O_NOFOLLOW | os.O_CLOEXEC | os.O_DIRECTORY
        parents = [item for item in seen if item not in final]
        if len(final) != 1 or not parents:
            b.fail("selftest: cache parent/final FD open structure changed")
        if final[0][1] & final_required != final_required:
            b.fail("selftest: exact cache FD open omitted a POSIX safety flag")
        unsafe_parents = [item for item in parents if item[1] & parent_required != parent_required]
        expected_alias_follows = int(
            (sys.platform, path.parts[1]) in {("darwin", "tmp"), ("darwin", "var")}
        )
        if len(unsafe_parents) != expected_alias_follows:
            b.fail("selftest: cache parent FD open omitted a POSIX safety flag")
        alias_required = os.O_CLOEXEC | os.O_DIRECTORY
        for name, flags, _dir_fd, parent_is_root in unsafe_parents:
            if (name, parent_is_root) not in {("tmp", True), ("var", True)}:
                b.fail("selftest: non-root cache parent followed a symlink")
            if flags & alias_required != alias_required or flags & os.O_NOFOLLOW:
                b.fail("selftest: Darwin root alias open used unsafe flags")
        for name in ("O_NOFOLLOW", "O_CLOEXEC", "O_NONBLOCK", "O_DIRECTORY"):
            with mock.patch.object(os, name, None):
                b.expect_bootstrap_error(
                    partial(b.open_cache_fd, path),
                    f"missing {name} support",
                )


def cache_parent_symlink_selftest() -> None:
    """Prove a symlink in a cache parent cannot redirect the final open."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-parent-symlink-") as raw:
        root = Path(raw).resolve()
        real = root / "real"
        real.mkdir()
        artifact = real / "artifact"
        artifact.write_bytes(b"verified")
        link = root / "link"
        link.symlink_to(real, target_is_directory=True)
        b.expect_bootstrap_error(
            partial(b.open_cache_fd, link / artifact.name),
            "cache parent symlink",
            "cannot open cached uv parent",
        )


def cache_parent_swap_selftest() -> None:
    """Prove a parent rename plus same-inode symlink cannot pass revalidation."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-parent-swap-") as raw:
        root = Path(raw).resolve()
        _manifest, archive, destination, asset_name, payload = mode_fixture(root)
        original_parent = destination.parent
        displaced = root / "displaced-authenticated-parent"
        real_open = os.open
        swapped = False

        def swap_parent_then_open(
            candidate: str | Path,
            flags: int,
            mode: int = 0o777,
            *,
            dir_fd: int | None = None,
        ) -> int:
            nonlocal swapped
            is_final = str(candidate) == destination.name and dir_fd is not None
            if is_final and flags & os.O_NONBLOCK and not swapped:
                swapped = True
                original_parent.rename(displaced)
                original_parent.symlink_to(displaced, target_is_directory=True)
            return real_open(candidate, flags, mode, dir_fd=dir_fd)

        digest = hashlib.sha256(payload).hexdigest()
        with mock.patch.object(os, "open", side_effect=swap_parent_then_open):
            b.expect_bootstrap_error(
                partial(
                    b.normalize_cached_modes,
                    archive,
                    destination,
                    asset_name,
                    digest,
                ),
                "cache parent rename/symlink swap",
                "moved during permission repair",
            )
        expect_modes(
            displaced / archive.name,
            displaced / destination.name,
            b.PUBLIC_ARCHIVE_MODE,
            b.PUBLIC_EXECUTABLE_MODE,
        )


def cache_atomic_parent_swap_selftest() -> None:
    """Prove apply writes stay in a held parent when its path is redirected."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-write-swap-") as raw:
        root = Path(raw).resolve()
        manifest, archive, destination, _asset_name, payload = mode_fixture(root, populate=False)
        external = root / "external-parent"
        external.mkdir()
        original_parent = destination.parent
        displaced = root / "displaced-write-parent"
        real_replace = os.replace
        swapped = False

        def swap_parent_then_replace(
            source: str,
            target: str,
            *,
            src_dir_fd: int | None = None,
            dst_dir_fd: int | None = None,
        ) -> None:
            nonlocal swapped
            readiness = (swapped, src_dir_fd is None, dst_dir_fd is None)
            if readiness == (False, False, False):
                swapped = True
                original_parent.rename(displaced)
                original_parent.symlink_to(external, target_is_directory=True)
            real_replace(
                source,
                target,
                src_dir_fd=src_dir_fd,
                dst_dir_fd=dst_dir_fd,
            )

        with (
            mock.patch.object(b, "download_payload", return_value=payload),
            mock.patch.object(os, "replace", side_effect=swap_parent_then_replace),
        ):
            b.expect_bootstrap_error(
                partial(b.ensure_uv, manifest, root),
                "atomic cache write parent swap",
                "cannot open cached uv parent",
            )
        if list(external.iterdir()):
            b.fail("selftest: redirected parent received a privileged uv cache write")
        if (displaced / archive.name).read_bytes() != payload:
            b.fail("selftest: held cache parent did not receive exact archive bytes")


def cache_nonregular_selftest() -> None:
    """Prove every non-regular cache node is rejected without blocking."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-node-mode-") as raw:
        root = Path(raw)
        regular = root / "regular"
        regular.write_bytes(b"verified")
        hardlink = root / "hardlink"
        hardlink.hardlink_to(regular)
        directory = root / "directory"
        directory.mkdir()
        fifo = root / "fifo"
        os.mkfifo(fifo, 0o600)
        socket_path = root / "socket"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(socket_path))

        def timeout(_signum: int, _frame: object) -> None:
            message = "non-regular cache open blocked"
            raise RuntimeError(message)

        previous = signal.signal(signal.SIGALRM, timeout)
        try:
            for path in (regular, hardlink, directory, fifo, socket_path):
                signal.alarm(1)
                b.expect_bootstrap_error(
                    partial(b.open_cache_fd, path),
                    f"non-regular cache node {path.name}",
                )
                signal.alarm(0)
        finally:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, previous)
            listener.close()


def expect_concurrent_read_rejected(path: Path, replacement: bytes, label: str) -> None:
    """Require one concurrent inode rewrite to invalidate a bounded read."""
    descriptor = b.open_cache_fd(path)
    real_fdopen = os.fdopen

    def rewrite_then_open(duplicate: int, *args: object, **kwargs: object) -> object:
        before = path.stat()
        path.write_bytes(replacement)
        if len(replacement) == before.st_size:
            os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns + 1))
        return real_fdopen(duplicate, *args, **kwargs)

    try:
        with mock.patch.object(os, "fdopen", side_effect=rewrite_then_open):
            b.expect_bootstrap_error(
                partial(b.read_stable_fd, descriptor, path, 8),
                label,
                "changed while authenticating",
            )
    finally:
        os.close(descriptor)


def cache_stable_read_selftest() -> None:
    """Prove bounded reads reject excess size and two concurrent rewrites."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-stable-read-") as raw:
        path = Path(raw) / "artifact"
        path.write_bytes(b"12345678")
        descriptor = b.open_cache_fd(path)
        try:
            payload, _ = b.read_stable_fd(descriptor, path, 8)
            if payload != b"12345678":
                b.fail("selftest: maximum-sized cache read changed bytes")
        finally:
            os.close(descriptor)
        path.write_bytes(b"123456789")
        descriptor = b.open_cache_fd(path)
        try:
            b.expect_bootstrap_error(
                partial(b.read_stable_fd, descriptor, path, 8),
                "oversized cached artifact",
                "exceeds policy",
            )
        finally:
            os.close(descriptor)
        path.write_bytes(b"12345678")
        expect_concurrent_read_rejected(path, b"x", "concurrently truncated cached artifact")
        path.write_bytes(b"12345678")
        expect_concurrent_read_rejected(
            path, b"ABCDEFGH", "concurrent same-size cached artifact rewrite"
        )


def cache_verification_fifo_race_selftest() -> None:
    """Prove the read-only verifier cannot block on a raced FIFO."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-verify-fifo-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root)
        archive.chmod(b.PUBLIC_ARCHIVE_MODE)
        destination.chmod(b.PUBLIC_EXECUTABLE_MODE)
        real_open = b.open_cache_fd
        swapped = False

        def swap_then_open(path: Path) -> int:
            nonlocal swapped
            if path == archive and not swapped:
                swapped = True
                archive.unlink()
                os.mkfifo(archive, 0o600)
            return real_open(path)

        with mock.patch.object(b, "open_cache_fd", side_effect=swap_then_open):
            b.expect_bootstrap_error(
                partial(b.verify_cached_uv, manifest, root),
                "read-only verification FIFO race",
                "not one single-link regular file",
            )


def cache_exact_fd_execution_selftest() -> None:
    """Prove actual uv work executes authenticated bytes, not a replacement path."""
    good = b'#!/bin/sh\nif [ "${1:-}" = --version ]; then echo uv 0.0.0; fi\n'
    with tempfile.TemporaryDirectory(prefix="ra8-uv-exact-exec-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root, binary=good)
        archive.chmod(b.PUBLIC_ARCHIVE_MODE)
        destination.chmod(b.PUBLIC_EXECUTABLE_MODE)
        victim = root / "unauthenticated-executed"
        real_run = subprocess.run
        calls = 0

        def swap_then_run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            nonlocal calls
            calls += 1
            if calls == ACTUAL_UV_RUN_INVOCATION:
                replacement = destination.with_name("replacement")
                displaced = destination.with_name("displaced-authenticated")
                replacement.write_text(
                    f"#!/bin/sh\ntouch {victim}\necho uv 0.0.0\n", encoding="ascii"
                )
                replacement.chmod(b.PUBLIC_EXECUTABLE_MODE)
                destination.rename(displaced)
                replacement.replace(destination)
            return real_run(argv, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=swap_then_run):
            b.expect_bootstrap_error(
                partial(b.run_cached_uv, manifest, root, ["work"], ensure=False),
                "post-auth cache pathname replacement",
                "changed after authenticating",
            )
        if victim.exists():
            b.fail("selftest: version probe executed unauthenticated replacement bytes")


def cache_same_inode_execution_selftest() -> None:
    """Prove a post-auth cache-inode write cannot control actual uv work."""
    good = b'#!/bin/sh\nif [ "${1:-}" = --version ]; then echo uv 0.0.0; fi\n'
    with tempfile.TemporaryDirectory(prefix="ra8-uv-inode-exec-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root, binary=good)
        archive.chmod(b.PUBLIC_ARCHIVE_MODE)
        destination.chmod(b.PUBLIC_EXECUTABLE_MODE)
        victim = root / "unauthenticated-inode-executed"
        malicious = f"#!/bin/sh\ntouch {victim}\necho uv 0.0.0\n".encode("ascii")
        real_run = subprocess.run
        calls = 0

        def mutate_inode_then_run(
            argv: list[str], **kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            nonlocal calls
            calls += 1
            if calls == ACTUAL_UV_RUN_INVOCATION:
                with destination.open("r+b") as target:
                    target.write(malicious)
                    target.truncate()
                    target.flush()
                    os.fsync(target.fileno())
            return real_run(argv, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=mutate_inode_then_run):
            b.expect_bootstrap_error(
                partial(b.run_cached_uv, manifest, root, ["work"], ensure=False),
                "post-auth same-inode cache mutation",
                "changed after authenticating",
            )
        if victim.exists():
            b.fail("selftest: version probe executed post-auth cache inode bytes")


def cache_run_exit_status_selftest() -> None:
    """Prove the public --run mode returns the exact immutable child status."""
    binary = (
        b'#!/bin/sh\nif [ "${1:-}" = --version ]; then echo uv 0.0.0; exit 0; fi\n'
        b'if [ "${1:-}" = exit ]; then exit "$2"; fi\n'
    )
    with tempfile.TemporaryDirectory(prefix="ra8-uv-run-status-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root, binary=binary)
        archive.chmod(b.PUBLIC_ARCHIVE_MODE)
        destination.chmod(b.PUBLIC_EXECUTABLE_MODE)
        for execution_mode in ("--run", "--ensure-and-run"):
            argv = [
                sys.executable,
                "-I",
                "-S",
                str(Path(b.__file__)),
                "--manifest",
                str(manifest),
                "--cache-root",
                str(root),
                execution_mode,
                "exit",
                str(EXPECTED_CHILD_STATUS),
            ]
            process = os.posix_spawn(argv[0], argv, os.environ.copy())
            waited, status = os.waitpid(process, 0)
            if waited != process or os.waitstatus_to_exitcode(status) != EXPECTED_CHILD_STATUS:
                b.fail(f"selftest: {execution_mode} did not preserve the uv child status")


def cache_run_signal_status_selftest() -> None:
    """Prove public run modes reproduce an immutable child's terminating signal."""
    binary = (
        b'#!/bin/sh\nif [ "${1:-}" = --version ]; then echo uv 0.0.0; exit 0; fi\n'
        b'if [ "${1:-}" = signal ]; then kill -TERM $$; fi\n'
    )
    with tempfile.TemporaryDirectory(prefix="ra8-uv-run-signal-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root, binary=binary)
        archive.chmod(b.PUBLIC_ARCHIVE_MODE)
        destination.chmod(b.PUBLIC_EXECUTABLE_MODE)
        for execution_mode in ("--run", "--ensure-and-run"):
            argv = [
                sys.executable,
                "-I",
                "-S",
                str(Path(b.__file__)),
                "--manifest",
                str(manifest),
                "--cache-root",
                str(root),
                execution_mode,
                "signal",
            ]
            process = os.posix_spawn(argv[0], argv, os.environ.copy())
            waited, status = os.waitpid(process, 0)
            if waited != process or not os.WIFSIGNALED(status):
                b.fail(f"selftest: {execution_mode} did not propagate the uv child signal")
            if os.WTERMSIG(status) != signal.SIGTERM:
                b.fail(f"selftest: {execution_mode} changed the uv child signal")


def portable_readonly_fd_selftest() -> None:
    """Prove the non-Linux fallback yields one exact private executable."""
    binary = b"#!/bin/sh\nexit 0\n"
    controls = importlib.import_module("fcntl")
    executable = ""
    with b.bootstrap_uv_exec.portable_named_exec_snapshot(binary) as snapshot:
        descriptor, executable = snapshot
        access_mode = controls.fcntl(descriptor, controls.F_GETFL) & os.O_ACCMODE
        if access_mode != os.O_RDONLY:
            b.fail("selftest: portable uv execution descriptor retained write access")
        if os.fstat(descriptor).st_nlink != 1 or not Path(executable).is_file():
            b.fail("selftest: portable uv execution name lost its bound descriptor")
        if os.pread(descriptor, len(binary) + 1, 0) != binary:
            b.fail("selftest: portable uv execution descriptor changed bytes")
        try:
            os.write(descriptor, b"x")
        except OSError:
            pass
        else:
            b.fail("selftest: portable uv execution descriptor accepted a write")
        process = os.posix_spawn(executable, [executable], os.environ.copy())
        waited, status = os.waitpid(process, 0)
        if waited != process or os.waitstatus_to_exitcode(status) != 0:
            b.fail("selftest: portable uv execution path did not execute exact bytes")
    if Path(executable).exists():
        b.fail("selftest: portable uv execution name survived cleanup")


def run_portable_snapshot(binary: bytes) -> None:
    """Enter and leave one portable snapshot for negative attack tests."""
    with b.bootstrap_uv_exec.portable_named_exec_snapshot(binary):
        pass


def portable_snapshot_flags_selftest() -> None:
    """Prove the named portable snapshot is reopened with every safety flag."""
    real_open = os.open
    reader_flags: list[int] = []

    def record_open(
        candidate: str | Path,
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        is_reader = str(candidate).startswith(".ra8-uv-") and flags & os.O_CREAT == 0
        if is_reader:
            reader_flags.append(flags)
        return real_open(candidate, flags, mode, dir_fd=dir_fd)

    with mock.patch.object(os, "open", side_effect=record_open):
        run_portable_snapshot(b"verified-uv")
    required = os.O_NONBLOCK | os.O_CLOEXEC | os.O_NOFOLLOW
    if len(reader_flags) != EXPECTED_PORTABLE_READER_OPENS:
        b.fail("selftest: portable uv reader authentication count changed")
    if any(flags & required != required for flags in reader_flags):
        b.fail("selftest: portable uv reader omitted a safety flag")
    if any(flags & os.O_ACCMODE != os.O_RDONLY for flags in reader_flags):
        b.fail("selftest: portable uv reader was not opened read-only")


def portable_snapshot_path_attack_selftest(attack: str) -> None:
    """Reject one deterministic mutation between portable write and reopen."""
    binary = b"verified-uv"
    replacement = b"untrusted!!"
    real_open = os.open
    attacked = False

    def attack_open(
        candidate: str | Path,
        flags: int,
        mode: int = 0o777,
        *,
        dir_fd: int | None = None,
    ) -> int:
        nonlocal attacked
        is_reader = str(candidate).startswith(".ra8-uv-") and flags & os.O_CREAT == 0
        if not is_reader or attacked or dir_fd is None:
            return real_open(candidate, flags, mode, dir_fd=dir_fd)
        attacked = True
        if attack in {"replace", "symlink"}:
            os.unlink(candidate, dir_fd=dir_fd)
        if attack in {"replace", "descriptor", "symlink"}:
            victim = real_open(
                ".ra8-uv-victim", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o500, dir_fd=dir_fd
            )
            os.write(victim, replacement)
            os.close(victim)
        if attack == "replace":
            os.rename(".ra8-uv-victim", candidate, src_dir_fd=dir_fd, dst_dir_fd=dir_fd)
        elif attack == "symlink":
            os.symlink(".ra8-uv-victim", candidate, dir_fd=dir_fd)
        elif attack == "hardlink":
            os.link(candidate, ".ra8-uv-extra", src_dir_fd=dir_fd, dst_dir_fd=dir_fd)
        elif attack == "overwrite":
            os.chmod(candidate, 0o700, dir_fd=dir_fd, follow_symlinks=False)
            writer = real_open(candidate, os.O_WRONLY | os.O_NOFOLLOW, dir_fd=dir_fd)
            os.write(writer, replacement)
            os.close(writer)
            os.chmod(candidate, 0o500, dir_fd=dir_fd, follow_symlinks=False)
        elif attack == "descriptor":
            return real_open(".ra8-uv-victim", flags, dir_fd=dir_fd)
        return real_open(candidate, flags, mode, dir_fd=dir_fd)

    with mock.patch.object(os, "open", side_effect=attack_open):
        expect_exec_failure(
            partial(run_portable_snapshot, binary),
            f"portable {attack} attack",
        )
    if not attacked:
        b.fail(f"selftest: portable {attack} attack did not reach the reader open")


def portable_snapshot_unlink_failure_selftest() -> None:
    """Refuse to return a portable descriptor when its name cannot be removed."""
    real_unlink = os.unlink
    attacked = False

    def reject_unlink(candidate: str | Path, *, dir_fd: int | None = None) -> None:
        nonlocal attacked
        if str(candidate).startswith(".ra8-uv-") and not attacked:
            attacked = True
            message = "simulated unlink denial"
            raise PermissionError(message)
        real_unlink(candidate, dir_fd=dir_fd)

    with mock.patch.object(os, "unlink", side_effect=reject_unlink):
        expect_exec_failure(
            partial(run_portable_snapshot, b"verified-uv"),
            "portable unlink failure",
        )
    if not attacked:
        b.fail("selftest: portable unlink attack did not reach the unlink boundary")


def bootstrap_run_status(manifest: Path, root: Path) -> int:
    """Return the real public --run status for one cache fixture."""
    argv = [
        sys.executable,
        "-I",
        "-S",
        str(Path(b.__file__)),
        "--manifest",
        str(manifest),
        "--cache-root",
        str(root),
        "--run",
        "--version",
    ]
    process = os.posix_spawn(argv[0], argv, os.environ.copy())
    waited, status = os.waitpid(process, 0)
    if waited != process:
        b.fail("selftest: bootstrap status child identity changed")
    return os.waitstatus_to_exitcode(status)


def cache_status_contract_selftest() -> None:
    """Prove repairable drift and authenticated-content failure use distinct types."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-status-mode-") as raw:
        root = Path(raw)
        manifest, archive, _destination, _, _payload = mode_fixture(root)
        try:
            b.verify_cached_uv(manifest, root)
        except b.CacheApplyRequiredError:
            pass
        else:
            b.fail("selftest: repairable cache drift lacks its exact exception contract")
        if bootstrap_run_status(manifest, root) != APPLY_REQUIRED_STATUS:
            b.fail("selftest: repairable --run drift did not return status 2")
        archive.write_bytes(b"tampered")
        try:
            b.verify_cached_uv(manifest, root)
        except b.CacheApplyRequiredError:
            b.fail("selftest: cache authentication failure was classified as drift")
        except b.BootstrapError as error:
            if "SHA-256 mismatch" not in str(error):
                b.fail("selftest: cache authentication failure returned the wrong error")
        else:
            b.fail("selftest: cache authentication failure passed")
        if bootstrap_run_status(manifest, root) != AUTH_ERROR_STATUS:
            b.fail("selftest: authenticated --run failure did not return status 1")

    with tempfile.TemporaryDirectory(prefix="ra8-uv-missing-mode-") as raw:
        root = Path(raw)
        manifest, _archive, _destination, _, _ = mode_fixture(root, populate=False)
        try:
            b.verify_cached_uv(manifest, root)
        except b.CacheApplyRequiredError:
            pass
        else:
            b.fail("selftest: missing cache was not classified as repairable drift")
        if bootstrap_run_status(manifest, root) != APPLY_REQUIRED_STATUS:
            b.fail("selftest: missing-cache --run drift did not return status 2")


def cache_mode_path_attack_selftest(
    target_name: str, replacement_kind: str, preexisting: bool
) -> None:
    """Prove symlink and moved-path attacks cannot redirect authenticated chmod."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-path-mode-") as raw:
        root = Path(raw)
        _, archive, destination, asset_name, payload = mode_fixture(root)
        target = archive if target_name == "archive" else destination
        mode = b.PRIVATE_ARCHIVE_MODE if target_name == "archive" else b.PRIVATE_EXECUTABLE_MODE
        moved = root / f"opened-{target_name}"
        replacement = root / "replacement"
        replacement.write_bytes(target.read_bytes())
        replacement.chmod(mode)

        def replace_target() -> None:
            """Replace the selected cache path without changing replacement bytes."""
            target.rename(moved)
            if replacement_kind == "symlink":
                target.symlink_to(replacement)
            else:
                replacement.rename(target)

        real_fchmod = os.fchmod
        calls = 0

        def swap_then_fchmod(descriptor: int, requested_mode: int) -> None:
            nonlocal calls
            if calls == 0 and not preexisting:
                replace_target()
            calls += 1
            real_fchmod(descriptor, requested_mode)

        if preexisting:
            replace_target()
        expected = (
            "cannot open cached uv artifact" if preexisting else "moved during permission repair"
        )
        with (
            mock.patch.object(os, "fchmod", side_effect=swap_then_fchmod),
            mock.patch.object(os, "chmod", side_effect=AssertionError("path-based chmod")),
        ):
            b.expect_bootstrap_error(
                partial(
                    b.normalize_cached_modes,
                    archive,
                    destination,
                    asset_name,
                    hashlib.sha256(payload).hexdigest(),
                ),
                f"{target_name} {replacement_kind} path attack",
                expected,
            )
        if stat.S_IMODE(target.stat().st_mode) != mode:
            b.fail(f"selftest: {target_name} {replacement_kind} replacement received chmod")


def cache_mode_readonly_and_windows_selftest() -> None:
    """Prove audits never write and Windows never asserts POSIX permission bits."""
    with tempfile.TemporaryDirectory(prefix="ra8-uv-readonly-mode-") as raw:
        root = Path(raw)
        manifest, archive, destination, _, _ = mode_fixture(root)
        probe = subprocess.CompletedProcess(["uv", "--version"], 0, "uv 0.0.0\n", "")
        before = (write_metadata(archive), write_metadata(destination))
        with mock.patch.object(subprocess, "run", return_value=probe):
            if os.name == "posix":
                b.expect_bootstrap_error(
                    lambda: b.verify_cached_uv(manifest, root),
                    "read-only audit detects mode drift",
                    "permissions require an apply",
                )
            else:
                b.verify_cached_uv(manifest, root)
        if (write_metadata(archive), write_metadata(destination)) != before:
            b.fail("selftest: read-only cache audit changed metadata")
        with (
            mock.patch.object(os, "name", "nt"),
            mock.patch.object(
                os,
                "fchmod",
                side_effect=AssertionError("POSIX chmod on Windows"),
                create=True,
            ),
            mock.patch.object(
                subprocess,
                "run",
                side_effect=AssertionError("path-based uv probe on Windows"),
            ),
        ):
            b.ensure_uv(manifest, root)
        if (write_metadata(archive), write_metadata(destination)) != before:
            b.fail("selftest: Windows checksum path changed POSIX metadata")


def run_mode_selftest() -> None:
    """Exercise authenticated POSIX modes, races, read-only audit, and Windows."""
    if os.name == "posix":
        darwin_root_alias_acceptance_selftest()
        darwin_root_alias_rejection_selftest()
        cache_mode_convergence_selftest()
        cache_mode_authentication_selftest()
        cache_open_flags_selftest()
        cache_parent_symlink_selftest()
        cache_parent_swap_selftest()
        cache_atomic_parent_swap_selftest()
        cache_nonregular_selftest()
        cache_stable_read_selftest()
        cache_verification_fifo_race_selftest()
        cache_exact_fd_execution_selftest()
        cache_same_inode_execution_selftest()
        cache_run_exit_status_selftest()
        cache_run_signal_status_selftest()
        portable_readonly_fd_selftest()
        portable_snapshot_flags_selftest()
        for attack in ("replace", "symlink", "hardlink", "overwrite", "descriptor"):
            portable_snapshot_path_attack_selftest(attack)
        portable_snapshot_unlink_failure_selftest()
        for target in ("archive", "binary"):
            cache_mode_path_attack_selftest(target, "symlink", preexisting=True)
            for replacement in ("symlink", "regular"):
                cache_mode_path_attack_selftest(target, replacement, preexisting=False)
        cache_status_contract_selftest()
    cache_mode_readonly_and_windows_selftest()
