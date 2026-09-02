#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Secure lock and metadata boundary for the workspace lifecycle shell."""

from __future__ import annotations

import fcntl
import os
import signal
import stat
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

SAFE_FILE_MODE_MASK = stat.S_IWGRP | stat.S_IWOTH
EXPECTED_METADATA_FIELDS = {
    "schema",
    "name",
    "created",
    "by",
    "ref",
    "base_commit",
    "path",
    "branch",
    "owner",
}
LOCK_ARGC_MIN = 5
METADATA_ARGC = 5
INHERIT_DESCRIPTOR = True


class GuardError(Exception):
    """One fail-closed workspace boundary violation."""


def fail(message: str, cause: BaseException | None = None) -> NoReturn:
    """Raise one consistently constructed boundary error."""
    error = GuardError(message)
    if cause is not None:
        raise error from cause
    raise error


def lexical(path: str) -> Path:
    """Return an absolute normalized path without following symlinks."""
    if not path or not path.isascii() or not path.isprintable():
        fail("path must be printable single-line ASCII")
    expanded = Path(path).expanduser()
    return Path(os.path.abspath(expanded))  # noqa: PTH100 -- resolve would follow symlinks


def _components(path: Path) -> list[Path]:
    """Return every absolute component from slash through ``path``."""
    parts = path.parts
    current = Path(parts[0])
    result = [current]
    for part in parts[1:]:
        current /= part
        result.append(current)
    return result


def reject_symlink_components(path: Path) -> None:
    """Reject every existing symlink component in an absolute path."""
    for component in _components(path):
        try:
            info = component.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(info.st_mode):
            fail(f"symlink path component is forbidden: {component}")


def _contains(parent: Path, child: Path) -> bool:
    """Return whether ``parent`` is equal to or lexically contains ``child``."""
    try:
        child.relative_to(parent)
    except ValueError:
        return False
    return True


def validate_root(raw_root: str, raw_upstream: str) -> tuple[Path, Path]:
    """Validate or safely create the narrowly scoped workspace root."""
    root = lexical(raw_root)
    upstream = lexical(raw_upstream)
    home = lexical(str(Path.home()))
    forbidden = (Path("/"), home, upstream)
    if any(root == item for item in forbidden):
        fail(f"workspace root is a forbidden broad path: {root}")
    if _contains(root, home) or _contains(root, upstream):
        fail(f"workspace root may not be an ancestor of HOME or the repository: {root}")
    reject_symlink_components(root)
    reject_symlink_components(upstream)
    if not upstream.is_dir():
        fail(f"upstream repository is not a directory: {upstream}")
    if not root.exists():
        parent = root.parent
        if not parent.is_dir():
            fail(f"workspace root parent does not exist: {parent}")
        root.mkdir(mode=0o700)
    info = root.lstat()
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        fail(f"workspace root is not a real directory: {root}")
    if info.st_uid != os.geteuid():
        fail(f"workspace root has foreign ownership: {root}")
    if info.st_mode & SAFE_FILE_MODE_MASK:
        fail(f"workspace root is group/world writable: {root}")
    return root, upstream


def open_lock(root: Path) -> int:
    """Open and validate the non-following, non-truncating lifecycle lock."""
    flags = os.O_CREAT | os.O_RDWR | os.O_CLOEXEC
    flags |= getattr(os, "O_NOFOLLOW", 0)
    path = root / ".workspace.lock"
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as exc:
        fail(f"workspace lock could not be opened safely: {exc}", exc)
    try:
        info = os.fstat(descriptor)
        checks = (
            stat.S_ISREG(info.st_mode),
            info.st_uid == os.geteuid(),
            info.st_nlink == 1,
            not bool(info.st_mode & SAFE_FILE_MODE_MASK),
        )
        if not all(checks):
            fail("workspace lock must be a private, singly linked regular file")
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        os.set_inheritable(descriptor, INHERIT_DESCRIPTOR)
    except BaseException:
        os.close(descriptor)
        raise
    else:
        return descriptor


def run_locked(root: str, upstream: str, script: str, argv: list[str]) -> int:
    """Hold the validated lock while one shell lifecycle transaction runs."""
    validated_root, validated_upstream = validate_root(root, upstream)
    descriptor = open_lock(validated_root)
    environment = os.environ.copy()
    environment.update(
        {
            "RA8_WS_ROOT": str(validated_root),
            "RA8_WS_UPSTREAM": str(validated_upstream),
            "RA8_WS_LOCKED": "1",
            "RA8_WS_LOCK_FD": str(descriptor),
        }
    )
    bash = Path("/bin/bash")
    if not bash.is_file() or not os.access(bash, os.X_OK):
        fail("/bin/bash is required for workspace lifecycle transactions")
    child = subprocess.Popen(  # noqa: S603 -- resolved Bash, fixed script argv, no shell
        [str(bash), "-p", script, *argv],
        env=environment,
        pass_fds=(descriptor,),
    )

    def forward(signum: int, _frame: object) -> None:
        """Forward termination to the transaction that owns the lock."""
        if child.poll() is None:
            child.send_signal(signum)

    signals = (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)
    previous = {sig: signal.signal(sig, forward) for sig in signals}
    try:
        return child.wait()
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)
        os.close(descriptor)


def _metadata_fields(path: Path) -> dict[str, str]:
    """Read one private, regular schema-2 metadata record."""
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode):
        fail("metadata is not a regular file")
    if info.st_uid != os.geteuid() or info.st_nlink != 1 or info.st_mode & SAFE_FILE_MODE_MASK:
        fail("metadata ownership, links, or mode are unsafe")
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in fields or not value or not value.isprintable():
            fail("metadata contains an invalid or duplicate field")
        fields[key] = value
    if set(fields) != EXPECTED_METADATA_FIELDS or fields.get("schema") != "2":
        fail("metadata schema is unsupported")
    return fields


def validate_metadata(raw_path: str, raw_root: str, name: str, owner: str) -> int:
    """Validate deletion authority for one exact metadata record."""
    path = lexical(raw_path)
    root = lexical(raw_root)
    expected = root / ".meta" / name
    if path != expected:
        fail("metadata path is outside its exact reserved slot")
    fields = _metadata_fields(path)
    if fields["name"] != name or fields["path"] != str(root / name):
        fail("metadata name/path binding is inconsistent")
    actual_owner = fields["owner"]
    if owner not in ("any", actual_owner):
        fail(f"metadata owner is {actual_owner}, expected {owner}")
    if actual_owner not in {"agent", "work"}:
        fail("metadata owner is unsupported")
    identifier = name.removeprefix("work-")
    if actual_owner == "work":
        if not name.startswith("work-") or fields["branch"] != f"work/{identifier}":
            fail("work metadata violates reserved name/branch binding")
    elif name.startswith("work-"):
        fail("agent metadata may not occupy a reserved work-* name")
    return 0


def main(argv: list[str]) -> int:
    """Dispatch the private lock wrapper or metadata validator."""
    try:
        if len(argv) >= LOCK_ARGC_MIN and argv[0] == "lock" and argv[4] == "--":
            return run_locked(argv[1], argv[2], argv[3], argv[5:])
        if len(argv) == METADATA_ARGC and argv[0] == "metadata":
            return validate_metadata(argv[1], argv[2], argv[3], argv[4])
        fail("invalid workspace guard invocation")
    except (GuardError, OSError, UnicodeError) as exc:
        print(f"workspace guard: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
