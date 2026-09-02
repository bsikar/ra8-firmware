# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Two-sided escaped-descendant proofs for the image supervisor."""

from __future__ import annotations

import os
import signal
import stat
import subprocess
import time
from pathlib import Path

from hil_convergence_safety_runtime_mutations import (
    POLL_SECONDS,
    PRIVATE_MODE,
    RESIDUE_TIMEOUT_SECONDS,
    RuntimeMutationError,
    _create_root,
    _identity_text,
    _no_residue,
    _owned_root_scope,
    _remove_root,
    _run_supervisor,
    _write_sources,
)

IDENTITY_FIELD_COUNT = 2
PROCESS_GROUP_FIELD, SESSION_FIELD, START_TIME_FIELD = 2, 3, 19


def _write_escape_fixture(root: Path) -> Path:
    """Write a payload whose grandchild escapes its inherited session."""
    payload = root / "escape-payload.py"
    payload.write_text(
        "import os, pathlib, sys, time\n"
        "root = pathlib.Path(sys.argv[1])\n"
        "child = os.fork()\n"
        "if child == 0:\n"
        "    os.setsid()\n"
        "    os.chdir(root)\n"
        "    held = os.open(root, os.O_RDONLY | os.O_DIRECTORY)\n"
        "    os.close(1)\n"
        "    os.close(2)\n"
        "    process = os.getpid()\n"
        "    raw = pathlib.Path('/proc/self/stat').read_bytes()\n"
        "    fields = raw[raw.rfind(b')') + 2:].split()\n"
        "    if os.getpgrp() != process or os.getsid(0) != process:\n"
        "        os._exit(70)\n"
        "    identity = f'{process}:{int(fields[19])}\\n'.encode('ascii')\n"
        "    identity_fd = os.open(root / 'escape-child.identity', "
        "os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)\n"
        "    os.write(identity_fd, identity)\n"
        "    os.close(identity_fd)\n"
        "    group_fd = os.open(root / 'escape-child.bound', "
        "os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)\n"
        "    os.write(group_fd, f'{process}\\n'.encode('ascii'))\n"
        "    os.close(group_fd)\n"
        "    time.sleep(30)\n"
        "    os.close(held)\n"
        "    os._exit(0)\n"
        "deadline = time.monotonic() + 5.0\n"
        "while not (root / 'escape-child.bound').exists():\n"
        "    if time.monotonic() >= deadline:\n"
        "        os._exit(71)\n"
        "    time.sleep(0.01)\n"
        "os._exit(1)\n",
        encoding="ascii",
    )
    payload.chmod(stat.S_IRUSR | stat.S_IWUSR)
    entry = root / "escape-entry.sh"
    entry.write_text(
        "#!/bin/bash\n"
        '[[ "$1" == "--selftest" ]] || exit 64\n'
        f'exec /usr/bin/python3 -B -I -S "{payload}" "{root}"\n',
        encoding="ascii",
    )
    entry.chmod(PRIVATE_MODE)
    return entry


def _read_escape_identity(root: Path) -> tuple[int, int]:
    """Read the exact PID/start-time identity published by the fixture."""
    raw = (root / "escape-child.identity").read_bytes().strip().split(b":")
    group = (root / "escape-child.bound").read_bytes().strip()
    if (
        len(raw) != IDENTITY_FIELD_COUNT
        or not all(value.isdigit() for value in raw)
        or not group.isdigit()
    ):
        message = "escaped-descendant identity receipt is malformed"
        raise RuntimeMutationError(message)
    identity = int(raw[0]), int(raw[1])
    if identity[0] <= 0 or int(group) != identity[0]:
        message = "escaped-descendant group receipt is inconsistent"
        raise RuntimeMutationError(message)
    return identity


def _live_process_identity(process: int) -> tuple[int, int, int] | None:
    """Return one live process's group, session, and start time."""
    try:
        raw = Path(f"/proc/{process}/stat").read_bytes()
    except FileNotFoundError:
        return None
    except OSError as error:
        message = "escaped-descendant identity is unreadable"
        raise RuntimeMutationError(message) from error
    closing = raw.rfind(b")")
    fields = raw[closing + 2 :].split() if closing >= 0 else []
    identity_fields = (PROCESS_GROUP_FIELD, SESSION_FIELD, START_TIME_FIELD)
    if len(fields) <= START_TIME_FIELD or not all(
        fields[index].isdigit() for index in identity_fields
    ):
        message = "escaped-descendant process identity is malformed"
        raise RuntimeMutationError(message)
    return tuple(int(fields[index]) for index in identity_fields)


def _escape_is_live(identity: tuple[int, int]) -> bool:
    """Prove the receipt still names its isolated escaped process."""
    process, start_time = identity
    return _live_process_identity(process) == (process, process, start_time)


def _terminate_escape(identity: tuple[int, int]) -> bool:
    """Kill only the exact still-live escaped group and wait for disappearance."""
    process, _start_time = identity
    if not _escape_is_live(identity):
        return True
    os.killpg(process, signal.SIGKILL)
    deadline = time.monotonic() + RESIDUE_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        current = _live_process_identity(process)
        if current is None:
            return True
        if current != (process, process, identity[1]):
            message = "escaped-descendant PID identity changed during cleanup"
            raise RuntimeMutationError(message)
        time.sleep(POLL_SECONDS)
    return False


def _require_empty_diagnostics(stderr: bytes) -> None:
    """Reject any unexpected diagnostic from the public supervisor."""
    if stderr:
        message = "escaped-descendant supervisor emitted diagnostics"
        raise RuntimeMutationError(message)


def _run_escape(
    supervisor: str, process_source: str, cases_source: str
) -> tuple[Path, tuple[int, int], tuple[int, int], int | None, bool]:
    """Run the public supervisor against one isolated escaping payload."""
    root, root_identity = _create_root()
    escape_identity: tuple[int, int] | None = None
    try:
        main_path, process_path, cases_path = _write_sources(
            root, supervisor, process_source, cases_source
        )
        entry = _write_escape_fixture(root)
        bound = root / "escape-supervisor.bound"
        bound.write_bytes(b"")
        bound.chmod(stat.S_IRUSR | stat.S_IWUSR)
        status, clean, stderr = _run_supervisor(
            main_path,
            process_path,
            cases_path,
            (
                str(entry),
                str(bound),
                str(root / "escape-supervisor.outer"),
                str(root / "escape-supervisor.status"),
                "normal",
                _identity_text(root),
            ),
        )
        escape_identity = _read_escape_identity(root)
        _require_empty_diagnostics(stderr)
    except (OSError, RuntimeMutationError, subprocess.TimeoutExpired, ValueError):
        identity_receipt = root / "escape-child.identity"
        if escape_identity is None and identity_receipt.exists():
            escape_identity = _read_escape_identity(root)
        if (
            escape_identity is not None
            and _escape_is_live(escape_identity)
            and not _terminate_escape(escape_identity)
        ):
            message = "escaped-descendant failure cleanup did not converge"
            raise RuntimeMutationError(message) from None
        _remove_root(root, root_identity)
        raise
    else:
        return root, root_identity, escape_identity, status, clean


def cases(inputs: dict[str, str]) -> list[tuple[str, bool]]:
    """Prove subreaper cleanup and detect deleting its enablement."""
    if not Path("/proc/self/stat").is_file():
        return [("image supervisor escaped-descendant proof is Linux-only", True)]
    supervisor = inputs["devcontainer_image_selftest_supervisor"]
    process_source = inputs["devcontainer_image_selftest_process"]
    supervisor_cases = inputs["devcontainer_image_selftest_supervisor_cases"]
    enable = "        self.subreaper = _enable_child_subreaper()\n"
    if process_source.count(enable) != 1:
        message = "subreaper runtime mutation authority is not unique"
        raise RuntimeMutationError(message)
    mutant_process = process_source.replace(
        enable,
        "        self.subreaper = True  # runtime mutation: kernel subreaper disabled\n",
        1,
    )
    with _owned_root_scope():
        roots: list[tuple[Path, tuple[int, int], tuple[int, int] | None]] = []
        try:
            base_root, base_root_identity, base_escape, base_status, base_clean = _run_escape(
                supervisor, process_source, supervisor_cases
            )
            roots.append((base_root, base_root_identity, base_escape))
            mutant_root, mutant_root_identity, mutant_escape, mutant_status, mutant_clean = (
                _run_escape(supervisor, mutant_process, supervisor_cases)
            )
            roots.append((mutant_root, mutant_root_identity, mutant_escape))
            base_absent = not _escape_is_live(base_escape) and _no_residue((base_root,))
            mutant_live = _escape_is_live(mutant_escape) and not _no_residue((mutant_root,))
            deletion_refused = False
            try:
                _remove_root(mutant_root, mutant_root_identity)
            except RuntimeMutationError:
                deletion_refused = mutant_root.is_dir()
            mutant_recovered = _terminate_escape(mutant_escape) and _no_residue((mutant_root,))
            return [
                (
                    "supervisor subreaper removes a setsid root-owning descendant",
                    base_status == 1 and base_clean and base_absent,
                ),
                (
                    "subreaper deletion mutation preserves live root and then recovers exactly",
                    mutant_status == 1
                    and mutant_clean
                    and mutant_live
                    and deletion_refused
                    and mutant_recovered,
                ),
            ]
        finally:
            for root, root_identity, escape_identity in reversed(roots):
                if (
                    escape_identity is not None
                    and _escape_is_live(escape_identity)
                    and not _terminate_escape(escape_identity)
                ):
                    message = "escaped-descendant recovery did not converge"
                    raise RuntimeMutationError(message)
                _remove_root(root, root_identity)
