# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Offline test fixtures for the repository workflow client."""

from __future__ import annotations

import contextlib
import functools
import io
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parents[1]
FIXTURES_DIR = Path(__file__).resolve().parent
SRC_DIR = TESTS_DIR.parent / "src"
sys.path.insert(0, str(SRC_DIR))

import work  # noqa: E402 -- import needs the source path above

REGISTERED_GATE_ENV = "RA8_WORK_HARNESS_REGISTERED_GATE"
REPO_ROOT = TESTS_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts/dev"))

from git_environment import (  # noqa: E402 -- repository scripts path is inserted above
    LOCAL_GIT_ENVIRONMENT,
    sanitized_git_environment,
    trusted_git_executable,
)


def portable_prerequisite(*, available: bool, reason: str) -> bool:
    """Skip only for direct portable runs; a registered gate must fail closed."""
    if available:
        return True
    if os.environ.get(REGISTERED_GATE_ENV) == "1":
        message = f"registered work-harness gate lacks prerequisite: {reason}"
        raise RuntimeError(message)
    return False


@dataclass(frozen=True)
class RunResult:
    """One captured in-process invocation."""

    status: int
    stdout: str
    stderr: str

    @property
    def output(self) -> str:
        """Return both captured streams."""
        return self.stdout + self.stderr


def git_binary() -> str:
    """Return the fixed Git authority for workflow fixtures."""
    return trusted_git_executable()


@functools.lru_cache(maxsize=1)
def git_local_environment_names() -> frozenset[str]:
    """Return Git's current repository/worktree-routing environment names."""
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    done = subprocess.run(  # noqa: S603 -- resolved git and fixed discovery argv
        [git_binary(), "rev-parse", "--local-env-vars"],
        capture_output=True,
        text=True,
        env=clean,
        check=False,
    )
    names = frozenset(done.stdout.splitlines())
    malformed = sorted(name for name in names if not name.startswith("GIT_") or not name.isupper())
    missing = sorted(names - set(LOCAL_GIT_ENVIRONMENT))
    if done.returncode != 0 or done.stderr or malformed or missing or "GIT_INDEX_FILE" not in names:
        detail = done.stderr.strip() or f"malformed={malformed}, missing-from-authority={missing}"
        message = f"cannot establish Git local-environment boundary: {detail}"
        raise RuntimeError(message)
    return names


def fixture_git_environment() -> dict[str, str]:
    """Return the shared hardened environment for fixture Git children."""
    return sanitized_git_environment()


def hostile_filter_environment(root: Path) -> tuple[dict[str, str], Path]:
    """Return a live global/system config attack and its execution marker."""
    attack = root / "hostile-git-config"
    attack.mkdir()
    marker = attack / "filter-ran"
    helper = attack / "filter-helper.sh"
    helper.write_text(
        f"#!/bin/sh\nprintf x >> {shlex.quote(str(marker))}\ncat\n",
        encoding="ascii",
    )
    helper.chmod(0o755)
    attributes = attack / "attributes"
    attributes.write_text("* filter=ra8-hostile\n", encoding="ascii")
    system_config = attack / "system.config"
    system_config.write_text(
        f"[core]\n\tattributesFile = {attributes}\n",
        encoding="ascii",
    )
    global_config = attack / "global.config"
    global_config.write_text(
        f'[filter "ra8-hostile"]\n\tclean = {helper}\n\tsmudge = {helper}\n\trequired = true\n',
        encoding="ascii",
    )
    hostile = {
        "GIT_ATTR_NOSYSTEM": "0",
        "GIT_CONFIG_GLOBAL": str(global_config),
        "GIT_CONFIG_NOSYSTEM": "0",
        "GIT_CONFIG_SYSTEM": str(system_config),
    }
    return hostile, marker


def prove_hostile_filter_executes(root: Path, hostile: dict[str, str], marker: Path) -> None:
    """Prove the attack is live with an intentionally unsanitized Git add."""
    probe = root / "hostile-filter-probe"
    probe.mkdir()
    run_git(["init", "-b", "main"], probe)
    (probe / "probe.txt").write_text("probe\n", encoding="ascii")
    environment = {name: value for name, value in os.environ.items() if not name.startswith("GIT_")}
    environment.update(hostile)
    done = subprocess.run(  # noqa: S603 -- resolved Git and fixed fixture argv
        [git_binary(), "add", "probe.txt"],
        cwd=probe,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0 or not marker.is_file():
        message = f"hostile filter anti-vacuity probe failed: {done.stderr}"
        raise AssertionError(message)
    marker.unlink()


def run_git(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    """Run one git command inside an isolated fixture repository."""
    done = subprocess.run(  # noqa: S603 -- resolved git, controlled fixture argv
        [git_binary(), *args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        env=fixture_git_environment(),
        check=False,
    )
    if done.returncode != 0:
        message = f"fixture git {' '.join(args)} failed: {done.stderr}"
        raise AssertionError(message)
    return done


def make_repo(path: Path, hooks_dir: Path) -> Path:
    """Create one local throwaway repository with no active hooks."""
    path.mkdir(parents=True, exist_ok=True)
    run_git(["init", "-b", "main"], path)
    run_git(["config", "core.hooksPath", str(hooks_dir)], path)
    run_git(["config", "user.email", "fixture@example.invalid"], path)
    run_git(["config", "user.name", "Fixture Author"], path)
    (path / "README.md").write_text("fixture\n", encoding="ascii")
    run_git(["add", "README.md"], path)
    run_git(["commit", "-m", "seed"], path)
    return path


def make_bin_dir(path: Path, *, with_gh: bool) -> Path:
    """Build an isolated PATH with git and an optional fake gh."""
    path.mkdir(parents=True, exist_ok=True)
    (path / "git").symlink_to(git_binary())
    if with_gh:
        wrapper = path / "gh"
        wrapper.write_text(
            f'#!/bin/sh\nexec /usr/bin/python3 -I "{FIXTURES_DIR / "fake_gh.py"}" "$@"\n',
            encoding="ascii",
        )
        wrapper.chmod(0o755)
    return path


def run_work(args: list[str], cwd: Path) -> RunResult:
    """Run ``work.main`` in process with captured output."""
    out, err = io.StringIO(), io.StringIO()
    with (
        contextlib.chdir(cwd),
        contextlib.redirect_stdout(out),
        contextlib.redirect_stderr(err),
    ):
        status = work.main(args)
    return RunResult(status, out.getvalue(), err.getvalue())


class HarnessCase(unittest.TestCase):
    """Test case with throwaway HOME, hooks, repository, and workspace root."""

    def setUp(self) -> None:
        """Build the isolated fixture."""
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name).resolve()
        self._saved_env = dict(os.environ)
        self.addCleanup(self._restore_env)
        self.home = self.root / "home"
        self.home.mkdir()
        self.hooks = self.root / "nohooks"
        self.hooks.mkdir()
        os.environ["HOME"] = str(self.home)
        os.environ["GIT_CONFIG_GLOBAL"] = str(self.home / "absent-gitconfig")
        os.environ["GIT_CONFIG_SYSTEM"] = str(self.home / "absent-gitconfig")
        os.environ.pop("RA8_WS_ROOT", None)
        self.repo = make_repo(self.root / "repo", self.hooks)
        self.ws = self.root / "workspaces"

    def _restore_env(self) -> None:
        """Restore the process environment exactly."""
        os.environ.clear()
        os.environ.update(self._saved_env)

    def fixture_text(self, name: str) -> str:
        """Read one notes fixture."""
        return (FIXTURES_DIR / name).read_text(encoding="ascii")

    def fixture_path(self, name: str) -> Path:
        """Return one notes fixture path."""
        return FIXTURES_DIR / name
