#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Runtime fixtures for immutable hook ownership and candidate dispatch."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.dev.git_environment import sanitized_git_environment, trusted_git_executable

REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALLER = REPO_ROOT / "scripts/git/install-hooks.sh"
LAUNCHER = REPO_ROOT / "scripts/git/hook-launcher"
PRE_COMMIT = REPO_ROOT / "scripts/git/pre-commit"
HOOKS_JUST = REPO_ROOT / "just/hooks.just"
CI_SCRIPT = REPO_ROOT / "scripts/ci.sh"
PROOF_WRITER = REPO_ROOT / "scripts/git/write-proof.py"
HOOK_NAMES = (
    "commit-msg",
    "post-checkout",
    "post-commit",
    "post-merge",
    "pre-commit",
    "pre-push",
)
ABORTED = 3
EXPECTED_GATES = (
    "ascii",
    "copyright",
    "since",
    "format",
    "pre-commit-checks",
    "shebangs",
    "entry-points",
    "annotations",
    "doc-attachment",
    "toolchain-parity",
    "lint-py-shell",
    "lint-just",
    "cite-check",
    "hil-eil-parity",
    "roadmap-stats",
    "sbom",
    "soup-upstream",
    "tidy",
    "cppcheck",
)


class RuntimeSelftestError(RuntimeError):
    """One runtime hook invariant failed."""


def _fail(message: str) -> None:
    raise RuntimeSelftestError(message)


def default_signal_test_command(*command: str) -> tuple[str, ...]:
    """Wrap one test child so an asynchronous parent cannot mask its signals."""
    if not command:
        _fail("default-signal fixture command is empty")
    program = """\
import os
import signal
import sys
for item in (signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM):
    signal.signal(item, signal.SIG_DFL)
os.execv(sys.argv[1], sys.argv[1:])
"""
    return (sys.executable, "-I", "-c", program, *command)


@dataclass(frozen=True)
class PrivateRun:
    """One fully specified invocation of a private selftest script."""

    command: tuple[str, ...]
    cwd: Path
    environment: dict[str, str]
    pass_fds: tuple[int, ...] = ()
    umask: int = -1
    preexec_fn: Callable[[], None] | None = None


def _run_private(spec: PrivateRun) -> subprocess.CompletedProcess[str]:
    """Run one repository-owned or generated private fixture."""
    return subprocess.run(  # noqa: S603 -- private fixture command and paths
        spec.command,
        cwd=spec.cwd,
        env=spec.environment,
        pass_fds=spec.pass_fds,
        umask=spec.umask,
        preexec_fn=spec.preexec_fn,
        capture_output=True,
        text=True,
        check=False,
        timeout=15,
    )


def _git_result(
    root: Path, *args: str, input_data: bytes | None = None
) -> subprocess.CompletedProcess[bytes]:
    """Run trusted Git in one private fixture without assuming its status."""
    return subprocess.run(  # noqa: S603 -- private fixture Git argv
        [trusted_git_executable(), "-C", str(root), *args],
        env=sanitized_git_environment(),
        input=input_data,
        capture_output=True,
        check=False,
    )


def _git(root: Path, *args: str, input_data: bytes | None = None) -> bytes:
    proc = _git_result(root, *args, input_data=input_data)
    if proc.returncode:
        _fail(proc.stderr.decode(errors="replace").strip())
    return proc.stdout


def _write(path: Path, text: str, *, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if executable:
        path.chmod(0o755)


def _init_repo(root: Path) -> None:
    _git(root, "init", "--quiet")
    _git(root, "config", "user.email", "selftest@invalid")
    _git(root, "config", "user.name", "selftest")


def _owner_text(hook: str, label: str = "") -> str:
    return f"#!/usr/bin/env bash\nset -euo pipefail\nprintf '%s\\0' '{label}{hook}' \"$@\"\n"


def _seed_launcher_repo(root: Path, *, include_launcher: bool = True) -> None:
    _init_repo(root)
    if include_launcher:
        (root / "scripts/git").mkdir(parents=True, exist_ok=True)
        shutil.copy2(LAUNCHER, root / "scripts/git/hook-launcher")
    for hook in HOOK_NAMES:
        _write(root / f"scripts/git/{hook}", _owner_text(hook), executable=True)
    _git(root, "add", ".")
    _git(root, "commit", "--quiet", "-m", "fixture")


def _run_installer(
    root: Path,
    environment: dict[str, str] | None = None,
    installer: Path = INSTALLER,
) -> subprocess.CompletedProcess[str]:
    return _run_private(
        PrivateRun(
            default_signal_test_command("/bin/bash", "-p", str(installer)),
            root,
            sanitized_git_environment() if environment is None else environment,
        )
    )


def _managed_dir(root: Path) -> Path:
    common = os.fsdecode(_git(root, "rev-parse", "--path-format=absolute", "--git-common-dir"))
    return Path(common.strip()) / "ra8-hooks"


def _run_launcher(
    root: Path, hook: str, args: tuple[str, ...], environment: dict[str, str] | None = None
) -> bytes:
    proc = subprocess.run(  # noqa: S603 -- private installed launcher
        [str(_managed_dir(root) / hook), *args],
        cwd=root,
        env=sanitized_git_environment() if environment is None else environment,
        capture_output=True,
        check=False,
        timeout=15,
    )
    if proc.returncode:
        _fail(f"{hook} launcher returned {proc.returncode}: {proc.stderr!r}")
    return proc.stdout


def _assert_argv_forwarding(root: Path) -> None:
    args = ("plain", "path with spaces", "line one\nline two")
    for hook in HOOK_NAMES:
        expected = b"\0".join(item.encode() for item in (hook, *args)) + b"\0"
        actual = _run_launcher(root, hook, args)
        if actual != expected:
            _fail(f"{hook} launcher corrupted argv: {actual!r}")


def _launcher_path_case(root: Path) -> None:
    fake_bin = root / ".venv/bin"
    git_marker = root / ".git/fake-git"
    core_markers = tuple(
        root / f".git/fake-{name}"
        for name in ("chmod", "env", "ln", "mkdir", "mktemp", "readlink", "rm")
    )
    _write(
        fake_bin / "git",
        f"#!/bin/bash\nprintf 'ran\\n' >{git_marker!s}\nexit 99\n",
        executable=True,
    )
    for marker in core_markers:
        name = marker.name.removeprefix("fake-")
        _write(
            fake_bin / name,
            f"#!/bin/bash -p\nprintf 'ran\\n' >{marker!s}\nexit 99\n",
            executable=True,
        )
    environment = sanitized_git_environment()
    environment["PATH"] = f"{fake_bin}:{environment.get('PATH', os.defpath)}"
    actual = _run_launcher(root, "pre-commit", (), environment)
    if actual != b"pre-commit\0":
        _fail("launcher PATH hardening changed hook output")
    if git_marker.exists() or any(marker.exists() for marker in core_markers):
        _fail("launcher executed a source-tree Git or core-utility shim")


def _installer_path_case(base: Path) -> None:
    """Prove installation never resolves mutable core tools through PATH."""
    root, fake_bin = base / "installer-path", base / "installer-path-bin"
    marker_dir = base / "installer-path-markers"
    root.mkdir()
    fake_bin.mkdir()
    marker_dir.mkdir()
    _seed_launcher_repo(root)
    for name in ("chmod", "cp", "grep", "mkdir", "mktemp", "mv", "rm", "rmdir"):
        _write(
            fake_bin / name,
            f"#!/bin/bash -p\nprintf 'ran\\n' >{marker_dir!s}/${{0##*/}}\nexit 99\n",
            executable=True,
        )
    environment = sanitized_git_environment()
    environment["PATH"] = f"{fake_bin}:{environment.get('PATH', os.defpath)}"
    result = _run_installer(root, environment)
    if result.returncode:
        _fail(f"installer PATH isolation failed: {result.stderr}")
    if tuple(marker_dir.iterdir()):
        _fail("installer executed an arbitrary-PATH core utility")


def _launcher_immutability_case(base: Path) -> None:
    root = base / "launcher"
    linked = base / "linked worktree"
    root.mkdir()
    _seed_launcher_repo(root)
    _git(root, "branch", "linked-branch")
    _git(root, "worktree", "add", "--quiet", str(linked), "linked-branch")
    _write(linked / "scripts/git/pre-commit", _owner_text("pre-commit", "linked:"), executable=True)
    _git(linked, "add", "scripts/git/pre-commit")
    _git(linked, "commit", "--quiet", "-m", "linked owner")
    result = _run_installer(root)
    if result.returncode:
        _fail(f"launcher install failed: {result.stderr}")
    managed = _managed_dir(root)
    configured = os.fsdecode(_git(root, "config", "--local", "--get", "core.hooksPath")).strip()
    if configured != str(managed) or _managed_dir(linked) != managed:
        _fail("linked worktrees did not share the managed common-dir hook path")
    _assert_argv_forwarding(root)
    if not _run_launcher(linked, "pre-commit", ()).startswith(b"linked:pre-commit\0"):
        _fail("shared launcher did not select the linked worktree HEAD")
    _write(root / "scripts/git/pre-commit", _owner_text("pre-commit", "mutable:"), executable=True)
    _launcher_path_case(root)
    _git(root, "add", "scripts/git/pre-commit")
    if _run_launcher(root, "pre-commit", ()).startswith(b"mutable:"):
        _fail("staged worktree hook ran instead of immutable HEAD")
    _write(root / "scripts/git/pre-commit", _owner_text("pre-commit"), executable=True)
    _git(root, "add", "scripts/git/pre-commit")
    _unmanaged_install_cases(root)


def _unmanaged_install_cases(root: Path) -> None:
    managed = _managed_dir(root)
    unmanaged = root / "unmanaged-hooks"
    _git(root, "config", "--local", "core.hooksPath", str(unmanaged))
    result = _run_installer(root)
    if result.returncode == 0:
        _fail("installer replaced an unmanaged core.hooksPath")
    current = os.fsdecode(_git(root, "config", "--local", "--get", "core.hooksPath")).strip()
    if current != str(unmanaged):
        _fail("failed unmanaged install changed core.hooksPath")
    _git(root, "config", "--local", "core.hooksPath", str(managed))
    intruder = managed / "unmanaged"
    _write(intruder, "unmanaged\n")
    result = _run_installer(root)
    if result.returncode == 0:
        _fail("installer replaced an unknown file in its managed directory")
    intruder.unlink()
    hidden = managed / ".pre-commit.new.interrupted"
    _write(hidden, "interrupted\n")
    result = _run_installer(root)
    if result.returncode == 0:
        _fail("installer ignored an interrupted hidden candidate")
    hidden.unlink()


def _same_commit_bootstrap_case(base: Path) -> None:
    root = base / "bootstrap"
    root.mkdir()
    _seed_launcher_repo(root, include_launcher=False)
    shutil.copy2(LAUNCHER, root / "scripts/git/hook-launcher")
    result = _run_installer(root)
    if result.returncode == 0 or "HEAD does not own" not in result.stderr:
        _fail("same-commit launcher bootstrap did not fail closed before commit")
    _git(root, "add", "scripts/git/hook-launcher")
    _git(root, "commit", "--quiet", "-m", "add launcher")
    result = _run_installer(root)
    if result.returncode:
        _fail(f"committed launcher did not install: {result.stderr}")


def _wait_for_staging(common: Path, process: subprocess.Popen[str]) -> None:
    """Stop only after the install transaction owns a populated staging dir."""
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if tuple(common.glob("ra8-hooks.stage.*")):
            return
        if process.poll() is not None:
            _fail(f"installer exited before staging: {process.returncode}")
        time.sleep(0.001)
    _fail("installer did not expose its staging transaction")


def _installer_transaction_case(base: Path) -> None:
    root = base / "installer-transaction"
    root.mkdir()
    _seed_launcher_repo(root)
    result = _run_installer(root)
    if result.returncode:
        _fail(f"initial transactional install failed: {result.stderr}")
    launcher = root / "scripts/git/hook-launcher"
    with launcher.open("ab") as stream:
        stream.write(b"\n# padding keeps the transaction observable\n")
        stream.write(os.urandom(8 * 1024 * 1024))
    _git(root, "add", "scripts/git/hook-launcher")
    _git(root, "commit", "--quiet", "-m", "large launcher transaction fixture")
    environment = sanitized_git_environment()
    common = _managed_dir(root).parent
    process = subprocess.Popen(  # noqa: S603 -- audited private-repo installer
        default_signal_test_command("/bin/bash", "-p", str(INSTALLER)),
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    try:
        _wait_for_staging(common, process)
        os.kill(process.pid, signal.SIGSTOP)
        contender = _run_installer(root)
        if contender.returncode == 0 or "another hook installer" not in contender.stderr:
            _fail("concurrent installer did not fail on the common-dir lock")
        os.kill(process.pid, signal.SIGCONT)
        os.killpg(process.pid, signal.SIGTERM)
        process.communicate(timeout=15)
    finally:
        if process.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
    residue = tuple(common.glob("ra8-hooks.*"))
    hooks = tuple(sorted(path.name for path in _managed_dir(root).iterdir()))
    if residue or hooks != tuple(sorted(HOOK_NAMES)):
        _fail("interrupted installer left residue or a partial hook generation")


ManagedState = tuple[int, tuple[tuple[str, int, bytes], ...]] | None
ConfigState = tuple[bool, str]


def _managed_state(root: Path) -> ManagedState:
    """Return the installed generation's exact names, modes, and bytes."""
    managed = _managed_dir(root)
    if not managed.exists():
        return None
    entries = tuple(
        (path.name, path.stat().st_mode & 0o777, path.read_bytes())
        for path in sorted(managed.iterdir())
    )
    return managed.stat().st_mode & 0o777, entries


def _committed_state(root: Path) -> ManagedState:
    """Return the only generation a successful transaction may install."""
    launcher = _git(root, "show", "HEAD:scripts/git/hook-launcher")
    return 0o700, tuple((name, 0o500, launcher) for name in HOOK_NAMES)


def _hooks_config_state(root: Path) -> ConfigState:
    """Distinguish an absent key from a present empty or nonempty value."""
    result = _git_result(root, "config", "--local", "--get", "core.hooksPath")
    if result.returncode == 0:
        return True, result.stdout.decode(encoding="utf-8").removesuffix("\n")
    if result.returncode == 1:
        return False, ""
    _fail(f"fatal hooksPath read in fixture: {result.stderr!r}")
    return False, ""


def _configure_fixture(root: Path, state: str) -> tuple[ConfigState, ManagedState]:
    """Prepare one exact pre-transaction config/directory state."""
    if state == "managed":
        result = _run_installer(root)
        if result.returncode:
            _fail(f"managed fixture install failed: {result.stderr}")
    elif state == "empty":
        _git(root, "config", "--local", "core.hooksPath", "")
    elif state == "legacy":
        _git(root, "config", "--local", "core.hooksPath", "scripts/git")
    elif state != "absent":
        _fail(f"unknown hook config fixture: {state}")
    return _hooks_config_state(root), _managed_state(root)


def _signal_injected_installer(
    base: Path,
    label: str,
    insertions: tuple[tuple[str, str], ...],
) -> Path:
    """Materialize the real installer with deterministic signal injections."""
    text = INSTALLER.read_text(encoding="ascii")
    for needle, injected in insertions:
        if text.count(needle) != 1:
            _fail(f"installer signal boundary {label} is not unique: {needle!r}")
        text = text.replace(needle, f"{needle}\n{injected}", 1)
    path = base / f"install-hooks-{label}.sh"
    _write(path, text, executable=True)
    return path


def _assert_transaction_state(
    root: Path,
    label: str,
    config: ConfigState,
    generation: ManagedState,
) -> None:
    """Require exact configuration presence/value and launcher bytes/modes."""
    if _hooks_config_state(root) != config:
        _fail(f"{label}: transaction changed exact core.hooksPath state")
    if _managed_state(root) != generation:
        _fail(f"{label}: transaction produced incorrect launcher bytes or modes")


@dataclass(frozen=True)
class _BoundaryCase:
    """One exact signal/configuration transaction interruption."""

    label: str
    boundary: str
    committed: bool
    config_state: str = "managed"
    signal_name: str = "TERM"
    second_cleanup_signal: bool = False


def _installer_boundary_signal_case(base: Path, case: _BoundaryCase) -> None:
    """Signal one exact transaction boundary and verify the resulting generation."""
    root = base / f"installer-boundary-{case.label}"
    root.mkdir()
    _seed_launcher_repo(root)
    original_config, original_state = _configure_fixture(root, case.config_state)
    launcher = root / "scripts/git/hook-launcher"
    with launcher.open("a", encoding="ascii") as stream:
        stream.write("\n# boundary signal generation\n")
    _git(root, "add", "scripts/git/hook-launcher")
    _git(root, "commit", "--quiet", "-m", "new launcher generation")

    committed_state = _committed_state(root)
    insertions = [(case.boundary, f'  kill -{case.signal_name} "$$"')]
    if case.second_cleanup_signal:
        insertions.append(("    trap '' HUP INT QUIT TERM", '    kill -TERM "$$"'))
    installer = _signal_injected_installer(base, case.label, tuple(insertions))
    result = _run_installer(root, installer=installer)
    if result.returncode != ABORTED:
        _fail(f"{case.label}: injected termination returned {result.returncode}, not 3")
    if case.committed:
        expected_config = (True, str(_managed_dir(root)))
        _assert_transaction_state(root, case.label, expected_config, committed_state)
    else:
        _assert_transaction_state(root, case.label, original_config, original_state)
    residue = tuple(_managed_dir(root).parent.glob("ra8-hooks.*"))
    if residue:
        _fail(f"{case.label}: interrupted transaction left residue: {residue!r}")


def _installer_boundary_signal_cases(base: Path) -> None:
    """Inject termination after every directory/configuration commit boundary."""
    cases = (
        ("before-first-move", "  transaction_started=1", False, False),
        ("after-backup-move", '    mv -- "$managed" "$backup"', False, False),
        ("after-install-move", '  mv -- "$staging" "$managed"', False, False),
        (
            "after-config",
            '  "$TRUSTED_GIT" -C "$root" config --local core.hooksPath "$managed"',
            False,
            False,
        ),
        ("after-commit-record", "  installed=1", True, False),
        ("rollback-second-signal", '    mv -- "$managed" "$backup"', False, True),
    )
    for label, boundary, committed, second_cleanup_signal in cases:
        _installer_boundary_signal_case(
            base,
            _BoundaryCase(label, boundary, committed, second_cleanup_signal=second_cleanup_signal),
        )
    for config_state in ("absent", "empty", "legacy", "managed"):
        for signal_name in ("HUP", "INT", "QUIT", "TERM"):
            label = f"matrix-{config_state}-{signal_name.lower()}"
            _installer_boundary_signal_case(
                base,
                _BoundaryCase(
                    label,
                    '  "$TRUSTED_GIT" -C "$root" config --local core.hooksPath "$managed"',
                    committed=False,
                    config_state=config_state,
                    signal_name=signal_name,
                ),
            )
    _installer_restore_failure_case(base)
    _installer_config_read_failure_case(base)
    _installer_multiline_config_case(base)
    _installer_exact_generation_case(base)
    _installer_lock_acquisition_signal_case(base)


def _installer_lock_acquisition_signal_case(base: Path) -> None:
    """Defer termination until an acquired installer lock can be released."""
    root = base / "installer-lock-acquisition-signal"
    root.mkdir()
    _seed_launcher_repo(root)
    installer = _signal_injected_installer(
        base,
        "lock-acquisition-signal",
        (('  if mkdir -- "$lock" 2>/dev/null; then', '    kill -TERM "$$"'),),
    )
    result = _run_installer(root, installer=installer)
    if result.returncode != ABORTED:
        _fail(f"lock acquisition termination returned {result.returncode}, not 3")
    _assert_transaction_state(root, "lock-acquisition-signal", (False, ""), None)
    residue = tuple((root / ".git").glob("ra8-hooks.*"))
    if residue:
        _fail(f"lock acquisition termination left residue: {residue!r}")
    retry = _run_installer(root)
    if retry.returncode:
        _fail(f"lock acquisition termination blocked retry: {retry.stderr}")
    expected_config = (True, str(_managed_dir(root)))
    _assert_transaction_state(
        root,
        "lock-acquisition-retry",
        expected_config,
        _committed_state(root),
    )


def _installer_restore_failure_case(base: Path) -> None:
    """Retain an exact recovery backup when directory restoration fails."""
    root = base / "installer-restore-failure"
    root.mkdir()
    _seed_launcher_repo(root)
    original_config, original_state = _configure_fixture(root, "managed")
    launcher = root / "scripts/git/hook-launcher"
    with launcher.open("a", encoding="ascii") as stream:
        stream.write("\n# restore failure generation\n")
    _git(root, "add", "scripts/git/hook-launcher")
    _git(root, "commit", "--quiet", "-m", "new launcher generation")
    text = INSTALLER.read_text(encoding="ascii")
    move = '    mv -- "$managed" "$backup"'
    restore = '          if mv -- "$backup" "$managed"; then'
    if text.count(move) != 1 or text.count(restore) != 1:
        _fail("restore-failure fixture did not bind both production moves")
    text = text.replace(move, f'{move}\n    kill -TERM "$$"', 1)
    text = text.replace(restore, "          if false; then", 1)
    installer = base / "install-hooks-restore-failure.sh"
    _write(installer, text, executable=True)
    result = _run_installer(root, installer=installer)
    if result.returncode != 1 or "recovery backup retained" not in result.stderr:
        _fail("restore failure did not return 1 and disclose retained recovery state")
    if _hooks_config_state(root) != original_config or _managed_state(root) is not None:
        _fail("restore failure changed configuration or fabricated a generation")
    common = _managed_dir(root).parent
    backups = tuple(common.glob("ra8-hooks.backup.*"))
    if len(backups) != 1:
        _fail("restore failure did not retain exactly one recovery backup")
    backup = backups[0]
    backup_state = (
        backup.stat().st_mode & 0o777,
        tuple(
            (path.name, path.stat().st_mode & 0o777, path.read_bytes())
            for path in sorted(backup.iterdir())
        ),
    )
    if backup_state != original_state:
        _fail("retained recovery backup changed original bytes or modes")


def _installer_config_read_failure_case(base: Path) -> None:
    """Require a fatal hooksPath read to abort before any transaction."""
    root = base / "installer-config-read-failure"
    root.mkdir()
    _seed_launcher_repo(root)
    text = INSTALLER.read_text(encoding="ascii")
    needle = '  read_hooks_path() {\n    local count=0 row status=""'
    if text.count(needle) != 1:
        _fail("fatal config-read fixture did not bind the production helper")
    text = text.replace(
        needle,
        '  read_hooks_path() {\n    return 5\n    local count=0 row status=""',
        1,
    )
    installer = base / "install-hooks-config-read-failure.sh"
    _write(installer, text, executable=True)
    result = _run_installer(root, installer=installer)
    if result.returncode != 1 or "cannot read local core.hooksPath" not in result.stderr:
        _fail("fatal hooksPath read did not fail closed")
    if _managed_state(root) is not None or tuple((root / ".git").glob("ra8-hooks.*")):
        _fail("fatal hooksPath read started a transaction")


def _installer_multiline_config_case(base: Path) -> None:
    """Refuse an unmanaged trailing-newline value without normalizing it."""
    root = base / "installer-multiline-config"
    root.mkdir()
    _seed_launcher_repo(root)
    value = "scripts/git\n"
    _git(root, "config", "--local", "core.hooksPath", value)
    before = _hooks_config_state(root)
    result = _run_installer(root)
    if result.returncode != 1 or "refusing to replace unmanaged" not in result.stderr:
        _fail("multiline hooksPath was normalized into an allowed value")
    if before != (True, value) or _hooks_config_state(root) != before:
        _fail("multiline hooksPath did not preserve its exact value")
    if _managed_state(root) is not None:
        _fail("multiline hooksPath refusal installed a managed generation")


def _installer_exact_generation_case(base: Path) -> None:
    """Repair unauthorized installed bytes to the exact committed generation."""
    root = base / "installer-exact-generation"
    root.mkdir()
    _seed_launcher_repo(root)
    first = _run_installer(root)
    if first.returncode:
        _fail(f"exact generation fixture failed to install: {first.stderr}")
    managed = _managed_dir(root)
    mutated = managed / "pre-commit"
    mutated.chmod(0o700)
    with mutated.open("ab") as stream:
        stream.write(b"# unauthorized installed bytes\n")
    (managed / "pre-push").chmod(0o700)
    second = _run_installer(root)
    if second.returncode:
        _fail(f"installer did not repair unauthorized installed generation: {second.stderr}")
    expected_config = (True, str(managed))
    _assert_transaction_state(root, "exact-generation", expected_config, _committed_state(root))


def _launcher_signal_case(base: Path) -> None:
    root = base / "launcher-signal"
    ready = base / "launcher.ready"
    continued = base / "launcher.continued"
    root.mkdir()
    _seed_launcher_repo(root)
    script = """#!/usr/bin/env bash
set -euo pipefail
trap 'exit 3' HUP INT QUIT TERM
printf 'ready\\n' >"${RA8_SELFTEST_READY:?}"
while :; do :; done
printf 'continued\\n' >"${RA8_SELFTEST_CONTINUED:?}"
"""
    _write(root / "scripts/git/pre-commit", script, executable=True)
    _git(root, "add", "scripts/git/pre-commit")
    _git(root, "commit", "--quiet", "-m", "signal owner")
    result = _run_installer(root)
    if result.returncode:
        _fail(f"signal fixture install failed: {result.stderr}")
    environment = sanitized_git_environment()
    environment.update(RA8_SELFTEST_READY=str(ready), RA8_SELFTEST_CONTINUED=str(continued))
    proc = subprocess.Popen(  # noqa: S603 -- private installed launcher
        default_signal_test_command(str(_managed_dir(root) / "pre-commit")),
        cwd=root,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        _wait_path(ready, proc)
        os.kill(proc.pid, signal.SIGTERM)
        proc.communicate(timeout=15)
    finally:
        if proc.poll() is None:
            with suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=5)
    residue = tuple(_managed_dir(root).parent.glob("ra8-hook-run.*"))
    if proc.returncode != ABORTED or continued.exists() or residue:
        _fail("launcher did not preserve owner semantics after parent-only SIGTERM")


def _wait_path(path: Path, proc: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.exists():
            return
        if proc.poll() is not None:
            _fail(f"fixture exited before ready: {proc.returncode}")
        time.sleep(0.02)
    _fail("fixture did not report readiness")


def _extract_supervisor() -> str:
    text = PRE_COMMIT.read_text(encoding="utf-8")
    start = text.index("<<'PY' || true\n") + len("<<'PY' || true\n")
    end = text.index("\nPY\n", start)
    return text[start:end]


def _supervisor_failure_case(base: Path, mode: str) -> None:
    ready = base / f"{mode}.ready"
    child_pid = base / f"{mode}.pid"
    child = base / f"{mode}.sh"
    _write(child, f"#!/usr/bin/env bash\necho $$ >{child_pid!s}\nsleep 60\n", executable=True)
    if mode == "collision":
        _write(ready, "occupied\n")
    environment = sanitized_git_environment()
    if mode == "write-failure":
        ready = base / "missing-ready-parent" / "ready"
    command = [
        "/usr/bin/python3",
        "-I",
        "-c",
        _extract_supervisor(),
        str(ready),
        "/bin/bash",
        "-p",
        str(child),
    ]
    result = subprocess.run(  # noqa: S603 -- extracted audited supervisor
        command, env=environment, capture_output=True, check=False, timeout=15
    )
    if result.returncode == 0:
        _fail(f"supervisor {mode} returned success")
    if child_pid.exists():
        pgid = int(child_pid.read_text(encoding="ascii").strip())
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            pass
        else:
            with suppress(ProcessLookupError):
                os.killpg(pgid, signal.SIGKILL)
            _fail(f"supervisor {mode} left its child process group alive")


def _supervisor_failure_cases(base: Path) -> None:
    _supervisor_failure_case(base, "collision")
    _supervisor_failure_case(base, "write-failure")
    result = subprocess.run(  # noqa: S603 -- extracted audited supervisor
        [
            "/usr/bin/python3",
            "-I",
            "-c",
            _extract_supervisor(),
            str(base / "missing.ready"),
            "/absent",
        ],
        env=sanitized_git_environment(),
        capture_output=True,
        check=False,
        timeout=10,
    )
    if result.returncode == 0:
        _fail("supervisor interpreter failure returned success")


def _stub_ci_support(root: Path) -> None:
    for relative in ("git_environment.sh",):
        _write(root / f"scripts/dev/{relative}", "#!/usr/bin/env bash\n")
    _write(root / "scripts/ci/lib/parallelism.sh", "#!/usr/bin/env bash\n")
    _write(root / "scripts/ci/lib/arm_toolchain.sh", "#!/usr/bin/env bash\n")
    _write(root / "scripts/ci/lib/snapshot.sh", "#!/usr/bin/env bash\n")
    _write(root / "scripts/ci/lib/tool_env.sh", "use_pinned_tool_path() { :; }\n")
    _write(
        root / "scripts/ci/lib/abort.sh",
        "RA8_CI_EXIT_ABORTED=3\nci_require_tree_intact() { :; }\nci_install_abort_traps() { :; }\n",
    )
    gate_source = """for row in "${RA8_GATE_REGISTRY[@]}"; do
  name="${row%%|*}"
  fn="gate_${name//-/_}"
  eval "$fn() { \"$RA8_SELFTEST_IGNORED_PROBE\"; \\
    printf '%s\\n' '$name' >>\"$RA8_SELFTEST_GATE_LOG\"; }"
done
"""
    _write(root / "scripts/ci/gates/fixture.sh", gate_source)  # PATHREF-OK: private fixture


def _policy_environment(root: Path) -> tuple[Path, dict[str, str]]:
    gate_log = root / ".git/gates.log"
    probe = root / ".venv/bin/probe"
    _write(
        probe,
        "#!/usr/bin/env bash\n"
        "if env | grep -Eq '^RA8_STAGED_(HOOK|GATE)_PROOF'; then exit 91; fi\n",
        executable=True,
    )
    environment = sanitized_git_environment()
    tools = root / ".git/policy-tools"
    tools.mkdir()
    (tools / "git").symlink_to(trusted_git_executable())
    (tools / "bash").symlink_to("/bin/bash")
    environment.update(
        PATH=f"{tools}:{environment.get('PATH', os.defpath)}",
        RA8_STAGED_HOOK_SNAPSHOT="1",
        RA8_SELFTEST_GATE_LOG=str(gate_log),
        RA8_SELFTEST_IGNORED_PROBE=str(probe),
        RA8_TOOLS_CACHE=str(root / ".git/tool-cache"),
    )
    return gate_log, environment


def _policy_fixture(root: Path, hooks_text: str) -> tuple[Path, dict[str, str]]:
    _init_repo(root)
    _write(root / "just/hooks.just", hooks_text)
    _write(
        root / "justfile",
        (
            'set shell := ["/bin/bash", "-puc"]\n'
            'export BASH_ENV := "/dev/null"\n'
            'export ENV := "/dev/null"\n'
            'export PYTHONHOME := ""\n'
            'export PYTHONPATH := ""\n'
            'mod git_hooks "just/hooks.just"\n'
            'mod quality "quality.just"\n'
        ),
    )
    _write(root / "quality.just", 'set working-directory := "."\nmod local "quality_local.just"\n')
    _write(
        root / "quality_local.just",
        'set working-directory := "."\ngate name:\n'
        '    /bin/bash -p scripts/ci.sh --gate "{{ name }}"\n',
    )
    (root / "scripts/git").mkdir(parents=True)
    shutil.copy2(CI_SCRIPT, root / "scripts/ci.sh")
    shutil.copy2(PROOF_WRITER, root / "scripts/git/write-proof.py")
    _stub_ci_support(root)
    for name in (
        "check_hook_parity.py",
        "check_mcdc_block.py",
        "check_new_compound_has_mcdc.py",
        "check_obsolete_standards.py",
    ):
        _write(
            root / f"scripts/checks/{name}",
            "#!/usr/bin/env python3\nraise SystemExit(0)\n",
            executable=True,
        )
    _write(root / "sample.c", "int value;\n")
    _write(root / ".gitignore", ".venv/\n")
    _git(root, "add", ".")
    _git(root, "commit", "--quiet", "-m", "fixture")
    _write(root / "sample.c", "int value = 1;\n")
    _git(root, "add", "sample.c")
    return _policy_environment(root)


def _policy_case(base: Path, name: str, hooks_text: str) -> bool:
    root = base / name
    root.mkdir()
    gate_log, environment = _policy_fixture(root, hooks_text)
    just = shutil.which("just")
    if just is None:
        _fail("Just is required for the real recipe runtime selftest")
    result = subprocess.run(  # noqa: S603 -- private fixture Just and paths
        [
            just,
            "--shell",
            "/bin/bash",
            "--clear-shell-args",
            "--shell-arg",
            "-puc",
            "--justfile",
            str(root / "justfile"),
            "--working-directory",
            str(root),
            "git_hooks::pre-commit",
        ],
        cwd=root,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    gates = tuple(gate_log.read_text(encoding="utf-8").splitlines()) if gate_log.exists() else ()
    return result.returncode == 0 and gates == EXPECTED_GATES


def _real_policy_cases(base: Path) -> None:
    live = HOOKS_JUST.read_text(encoding="utf-8")
    if not _policy_case(base, "policy-live", live):
        _fail("real Just pre-commit recipe did not run every gate")
    early = live.replace("    gates=(", "    exit 0\n    gates=(", 1)
    dead = live.replace(
        '    for gate in "${gates[@]}"; do\n        run_gate "$gate"\n    done',
        (
            '    if false; then\n        for gate in "${gates[@]}"; do\n'
            '            run_gate "$gate"\n        done\n    fi'
        ),
        1,
    )
    reordered = live.replace(
        "        ascii\n        copyright", "        copyright\n        ascii", 1
    )
    for name, mutated in (
        ("policy-early", early),
        ("policy-dead", dead),
        ("policy-order", reordered),
    ):
        if _policy_case(base, name, mutated):
            _fail(f"real Just runtime accepted {name} mutation")


def _proof_writer_case(base: Path) -> None:
    proof = base / "atomic.proof"
    command = ["/usr/bin/python3", "-I", str(PROOF_WRITER), str(proof)]
    first = subprocess.run(  # noqa: S603 -- audited helper and private path
        command, input=b"token\n", capture_output=True, check=False
    )
    second = subprocess.run(  # noqa: S603 -- audited helper and private path
        command, input=b"token\n", capture_output=True, check=False
    )
    target = base / "target"
    target.write_text("unchanged\n", encoding="ascii")
    linked = base / "linked.proof"
    linked.symlink_to(target)
    linked_result = subprocess.run(  # noqa: S603 -- audited helper and private path
        ["/usr/bin/python3", "-I", str(PROOF_WRITER), str(linked)],
        input=b"changed\n",
        capture_output=True,
        check=False,
    )
    if first.returncode or second.returncode == 0 or linked_result.returncode == 0:
        _fail("atomic proof writer did not enforce exclusive no-follow creation")
    if target.read_text(encoding="ascii") != "unchanged\n":
        _fail("atomic proof writer followed a final-component symlink")


def run_runtime_selftests() -> None:
    """Run all hook ownership, supervisor, real-policy, and proof fixtures."""
    with tempfile.TemporaryDirectory(prefix="ra8-hook-runtime-") as temporary:
        base = Path(temporary)
        _launcher_immutability_case(base)
        _installer_path_case(base)
        _same_commit_bootstrap_case(base)
        _launcher_signal_case(base)
        _supervisor_failure_cases(base)
        _installer_transaction_case(base)
        _installer_boundary_signal_cases(base)
        _real_policy_cases(base)
        _proof_writer_case(base)
