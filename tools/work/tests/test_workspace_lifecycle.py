#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Destructive-boundary selftest for the canonical workspace lifecycle."""

from __future__ import annotations

import importlib.util
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tools/work/tests/fixtures"))

from work_testlib import fixture_git_environment, git_binary  # noqa: E402 -- fixture path above

LIFECYCLE = REPO_ROOT / "scripts/dev/agent_workspace.sh"
GUARD_PATH = REPO_ROOT / "scripts/dev/workspace_guard.py"
GIT_ENVIRONMENT_SHELL = REPO_ROOT / "scripts/dev/git_environment.sh"
WS_JUST = REPO_ROOT / "just/ws.just"
REGISTERED_GATE_ENV = "RA8_WORK_HARNESS_REGISTERED_GATE"
REAL_JUST = next(
    (
        str(path)
        for path in (Path("/usr/local/bin/just"), Path("/usr/bin/just"))
        if path.is_file() and os.access(path, os.X_OK)
    ),
    None,
)
GIT_ENVIRONMENT = REPO_ROOT / "scripts/dev/git_environment.py"
REAL_GIT = git_binary()
REAL_BASH = "/bin/bash"
REAL_SLEEP = "/usr/bin/sleep"


def _extract_shell_function(path: Path, name: str) -> str:
    """Extract one uniquely named function at its declaration indentation."""
    text = path.read_text(encoding="utf-8")
    declarations = list(
        re.finditer(
            rf"^(?P<indent>[ \t]*){re.escape(name)}\(\)[ \t]*\{{[ \t]*$",
            text,
            flags=re.MULTILINE,
        )
    )
    if len(declarations) != 1:
        message = f"expected exactly one shell function named {name!r}"
        raise ValueError(message)
    declaration = declarations[0]
    indent = declaration.group("indent")
    closing = re.search(
        rf"^{re.escape(indent)}\}}[ \t]*$",
        text[declaration.end() :],
        flags=re.MULTILINE,
    )
    if closing is None:
        message = f"shell function {name!r} has no indentation-matched closing brace"
        raise ValueError(message)
    end = declaration.end() + closing.end()
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[declaration.start() : end]


class ShellFunctionExtraction(unittest.TestCase):
    """The isolated contract helper follows function structure, not column zero."""

    def test_indented_wrapper_and_nested_body_are_preserved(self) -> None:
        """An outer startup guard may indent a function without hiding its end."""
        with tempfile.TemporaryDirectory(prefix="ra8-shell-extract-") as temporary:
            fixture = Path(temporary) / "fixture.sh"
            fixture.write_text(
                "if true; then\n"
                "  wanted() {\n"
                "    if true; then\n"
                "      printf '%s\\n' value\n"
                "    fi\n"
                "  }\n"
                "else\n"
                "  false\n"
                "fi\n",
                encoding="ascii",
            )
            self.assertEqual(
                _extract_shell_function(fixture, "wanted"),
                "  wanted() {\n    if true; then\n      printf '%s\\n' value\n    fi\n  }\n",
            )


def _load_guard() -> object:
    """Import the guard from its exact path for synthetic owner checks."""
    spec = importlib.util.spec_from_file_location("workspace_guard_selftest", GUARD_PATH)
    if spec is None or spec.loader is None:
        message = "workspace guard import spec is absent"
        raise AssertionError(message)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WorkspaceLifecycleSafety(unittest.TestCase):
    """Every unsafe filesystem or stale-remote condition retains data."""

    def setUp(self) -> None:
        """Create one private local repository and pushed dev reference."""
        if REAL_GIT is None:
            self.fail("git is required")
        self.temporary = tempfile.TemporaryDirectory(prefix="ra8-workspace-selftest-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "home"
        self.home.mkdir(mode=0o700)
        self.upstream = self.root / "upstream"
        self.origin = self.root / "origin.git"
        self.ws_root = self.root / "workspaces"
        self._git("init", "--bare", "--quiet", str(self.origin), cwd=self.root)
        self._git("init", "--quiet", str(self.upstream), cwd=self.root)
        self._git("config", "user.name", "Workspace Selftest", cwd=self.upstream)
        self._git("config", "user.email", "workspace@example.invalid", cwd=self.upstream)
        (self.upstream / "README.md").write_text("fixture\n", encoding="ascii")
        fixture_sources = {
            LIFECYCLE: "scripts/dev/agent_workspace.sh",
            GUARD_PATH: "scripts/dev/workspace_guard.py",
            GIT_ENVIRONMENT: "scripts/dev/git_environment.py",
            GIT_ENVIRONMENT_SHELL: "scripts/dev/git_environment.sh",
            WS_JUST: "just/ws.just",
        }
        for source, relative in fixture_sources.items():
            destination = self.upstream / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        (self.upstream / "justfile").write_text(
            'set shell := ["/bin/bash", "-puc"]\n'
            'export BASH_ENV := "/dev/null"\n'
            'export ENV := "/dev/null"\n'
            'export PATH := env("RA8_SELFTEST_RECIPE_PATH")\n'
            'mod workspace "just/ws.just"\n',
            encoding="ascii",
        )
        self._git("add", "--all", cwd=self.upstream)
        self._git("commit", "--quiet", "-m", "fixture", cwd=self.upstream)
        self._git("branch", "-M", "dev", cwd=self.upstream)
        self._git("remote", "add", "origin", str(self.origin), cwd=self.upstream)
        self._git("push", "--quiet", "-u", "origin", "dev", cwd=self.upstream)

    def tearDown(self) -> None:
        """Remove only the selftest's uniquely named temporary root."""
        self.temporary.cleanup()

    def _git(self, *argv: str, cwd: Path) -> subprocess.CompletedProcess[str]:
        """Run the real Git binary against the isolated fixture."""
        return subprocess.run(  # noqa: S603 -- resolved Git and controlled fixture argv
            [str(REAL_GIT), *argv],
            cwd=cwd,
            text=True,
            capture_output=True,
            env=fixture_git_environment(),
            check=True,
        )

    def env(self, **updates: str) -> dict[str, str]:
        """Return a lifecycle environment confined to this fixture."""
        environment = fixture_git_environment()
        environment.update(
            {
                "HOME": str(self.home),
                "USER": "workspace-selftest",
                "RA8_WS_ROOT": str(self.ws_root),
                "RA8_WS_UPSTREAM": str(self.upstream),
                "RA8_WS_TTL_HOURS": "0",
                "RA8_WS_TTL_HOURS_FULL": "0",
            }
        )
        environment.update(updates)
        return environment

    def run_lifecycle(
        self, *argv: str, environment: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        """Run one real lifecycle command and capture its result."""
        if REAL_BASH is None:
            self.fail("bash is required")
        return subprocess.run(  # noqa: S603 -- resolved Bash and controlled fixture argv
            [REAL_BASH, "-p", str(LIFECYCLE), *argv],
            cwd=self.upstream,
            env=environment or self.env(),
            text=True,
            capture_output=True,
            check=False,
            timeout=30,
        )

    def hostile_shell_environment(self, label: str) -> tuple[dict[str, str], dict[str, Path]]:
        """Build live PATH, startup, and exported-function attacks."""
        source_bin = self.upstream / ".venv/bin"
        arbitrary_bin = self.root / f"{label}-arbitrary-bin"
        source_bin.mkdir(parents=True, exist_ok=True)
        arbitrary_bin.mkdir(parents=True, exist_ok=True)
        markers = {
            name: self.root / f"{label}-{name}.ran"
            for name in (
                "source-bash",
                "arbitrary-bash",
                "startup",
                "function",
                "utility",
            )
        }
        for directory, key in (
            (source_bin, "source-bash"),
            (arbitrary_bin, "arbitrary-bash"),
        ):
            wrapper = directory / "bash"
            wrapper.write_text(
                "#!/bin/bash -p\n"
                f"/usr/bin/printf x >{shlex.quote(str(markers[key]))}\n"
                'exec /bin/bash "$@"\n',
                encoding="ascii",
            )
            wrapper.chmod(0o755)
        utility = source_bin / "mkdir"
        utility.write_text(
            f"#!/bin/bash -p\n/usr/bin/printf x >{shlex.quote(str(markers['utility']))}\nexit 73\n",
            encoding="ascii",
        )
        utility.chmod(0o755)
        startup = self.root / f"{label}-bash-env"
        startup.write_text(
            f"/usr/bin/printf x >{shlex.quote(str(markers['startup']))}\n",
            encoding="ascii",
        )
        path = f"{source_bin}:{arbitrary_bin}:/usr/local/bin:/usr/bin:/bin"
        environment = self.env(
            BASH_ENV=str(startup),
            PATH=path,
            RA8_SELFTEST_RECIPE_PATH=path,
        )
        environment.pop("SSH_CLIENT", None)
        environment["BASH_FUNC_mkdir%%"] = (
            f"() {{ /usr/bin/printf x >{shlex.quote(str(markers['function']))}; return 73; }}"
        )
        return environment, markers

    def prove_shell_attacks(self, environment: dict[str, str], markers: dict[str, Path]) -> None:
        """Prove every hostile control executes before testing the boundary."""
        source = subprocess.run(
            ["/usr/bin/env", "bash", "-c", "true"], env=environment, check=False
        )
        arbitrary_env = dict(environment)
        path_parts = arbitrary_env["PATH"].split(":")
        arbitrary_env["PATH"] = ":".join((path_parts[1], path_parts[0], *path_parts[2:]))
        arbitrary = subprocess.run(
            ["/usr/bin/env", "bash", "-c", "true"], env=arbitrary_env, check=False
        )
        function = subprocess.run(  # noqa: S603 -- must-fire exported-function control
            [REAL_BASH, "-c", "PATH=/usr/bin:/bin; mkdir"], env=environment, check=False
        )
        utility = subprocess.run(  # noqa: S603 -- must-fire core utility control
            [REAL_BASH, "-p", "-c", "mkdir"], env=environment, check=False
        )
        self.assertEqual((source.returncode, arbitrary.returncode), (0, 0))
        self.assertEqual((function.returncode, utility.returncode), (73, 73))
        for marker in markers.values():
            self.assertTrue(marker.is_file(), marker)
            marker.unlink()

    def assert_shell_attacks_quiet(self, markers: dict[str, Path]) -> None:
        """Assert the production boundary executed none of the live attacks."""
        for marker in markers.values():
            self.assertFalse(marker.exists(), marker)

    def _root_with_lock(self, kind: str) -> tuple[Path, Path]:
        """Create one private root and a hostile lock object."""
        root = self.root / f"lock-{kind}"
        root.mkdir(mode=0o700)
        lock = root / ".workspace.lock"
        victim = self.root / f"victim-{kind}"
        if kind == "symlink":
            victim.write_text("DO NOT TRUNCATE\n", encoding="ascii")
            lock.symlink_to(victim)
        elif kind == "hardlink":
            victim.write_text("DO NOT TOUCH\n", encoding="ascii")
            os.link(victim, lock)
        elif kind == "fifo":
            os.mkfifo(lock, 0o600)
        elif kind == "directory":
            lock.mkdir(mode=0o700)
        elif kind == "mode":
            lock.write_text("LOCK\n", encoding="ascii")
            lock.chmod(0o666)
        return root, victim

    def test_lock_objects_are_refused_without_truncation(self) -> None:
        """Symlink, hardlink, FIFO, directory, and unsafe mode all fail closed."""
        for kind in ("symlink", "hardlink", "fifo", "directory", "mode"):
            with self.subTest(kind=kind):
                root, victim = self._root_with_lock(kind)
                before = victim.read_bytes() if victim.exists() and victim.is_file() else b""
                result = self.run_lifecycle(
                    "reap", "--quiet", environment=self.env(RA8_WS_ROOT=str(root))
                )
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                if before:
                    self.assertEqual(victim.read_bytes(), before)

    def test_foreign_lock_owner_is_refused_without_truncation(self) -> None:
        """The post-open fstat owner check rejects a foreign lock inode."""
        root = self.root / "foreign-lock-owner"
        root.mkdir(mode=0o700)
        lock = root / ".workspace.lock"
        lock.write_text("DO NOT TRUNCATE\n", encoding="ascii")
        guard = _load_guard()
        with (
            mock.patch.object(guard.os, "geteuid", return_value=os.geteuid() + 1),
            self.assertRaises(guard.GuardError),
        ):
            guard.open_lock(root)
        self.assertEqual(lock.read_text(encoding="ascii"), "DO NOT TRUNCATE\n")

    def test_broad_and_symlinked_roots_are_refused(self) -> None:
        """Slash, HOME, repository, ancestors, and symlink components never run."""
        linked_parent = self.root / "linked-parent"
        linked_parent.symlink_to(self.root, target_is_directory=True)
        candidates = (
            Path("/"),
            self.home,
            self.upstream,
            self.root,
            linked_parent / "workspaces",
        )
        for candidate in candidates:
            with self.subTest(root=candidate):
                result = self.run_lifecycle(
                    "reap", "--quiet", environment=self.env(RA8_WS_ROOT=str(candidate))
                )
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_foreign_root_owner_is_refused_by_the_real_validator(self) -> None:
        """The owner check has a direct negative test without requiring root."""
        self.ws_root.mkdir(mode=0o700)
        guard = _load_guard()
        with (
            mock.patch.object(guard.os, "geteuid", return_value=os.geteuid() + 1),
            self.assertRaises(guard.GuardError),
        ):
            guard.validate_root(str(self.ws_root), str(self.upstream))

    def test_failed_fetch_retains_a_reapable_workspace(self) -> None:
        """Cached refs never authorize deletion after refresh failure."""
        created = self.run_lifecycle("create", "safe-agent", "origin/dev")
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        workspace = self.ws_root / "safe-agent"
        self._git(
            "remote", "set-url", "origin", str(self.root / "missing-origin"), cwd=self.upstream
        )
        result = self.run_lifecycle("reap", "--force")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(workspace.is_dir())

    def test_direct_entry_rejects_source_and_arbitrary_path_utilities(self) -> None:
        """The destructive lifecycle never resolves core tools through caller PATH."""
        fake_bin = self.upstream / ".venv/bin"
        marker = self.root / "workspace-path.ran"
        fake_bin.mkdir(parents=True)
        for name in ("dirname", "mkdir", "mktemp", "readlink", "rm"):
            wrapper = fake_bin / name
            wrapper.write_text(
                f"#!/bin/bash -p\nprintf 'ran\\n' >{marker!s}\nexit 73\n",
                encoding="ascii",
            )
            wrapper.chmod(0o755)
        control = subprocess.run(  # noqa: S603 -- must-fire private PATH fixture
            [REAL_BASH, "-p", "-c", "mkdir"],
            env={"PATH": f"{fake_bin}:/usr/bin:/bin"},
            capture_output=True,
            check=False,
        )
        self.assertEqual(control.returncode, 73)
        self.assertTrue(marker.is_file())
        marker.unlink()
        environment = self.env(PATH=f"{fake_bin}:/arbitrary/path:/usr/bin:/bin")
        result = self.run_lifecycle("create", "path-proof", "origin/dev", environment=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(marker.exists())

    def test_documented_privileged_entry_ignores_live_shell_attacks(self) -> None:
        """The documented direct argv owns Bash despite hostile caller state."""
        environment, markers = self.hostile_shell_environment("direct-entry")
        self.prove_shell_attacks(environment, markers)
        result = self.run_lifecycle(
            "create", "direct-entry-proof", "origin/dev", environment=environment
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("done:   /bin/bash -p", result.stdout)
        self.assert_shell_attacks_quiet(markers)

    def test_unprivileged_entry_refuses_without_reaching_the_body(self) -> None:
        """An unsupported ordinary-Bash caller cannot reach lifecycle work."""
        environment, markers = self.hostile_shell_environment("weak-entry")
        weak_entry = self.root / "weak-entry-fixture.sh"
        shutil.copy2(LIFECYCLE, weak_entry)
        result = subprocess.run(  # noqa: S603 -- intentional ordinary-Bash control path
            [REAL_BASH, str(weak_entry), "create", "weak-proof", "origin/dev"],
            cwd=self.upstream,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
            timeout=30,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(markers["startup"].is_file())
        self.assertFalse(self.ws_root.exists())
        self.assertNotIn("done:", result.stdout)

    def test_real_just_facade_ignores_live_shell_attacks(self) -> None:
        """The supported Just facade owns its interpreter and lifecycle argv."""
        if REAL_JUST is None:
            if os.environ.get(REGISTERED_GATE_ENV) == "1":
                self.fail("registered work-harness gate requires an absolute Just executable")
            self.skipTest("Just is unavailable for the portable direct selftest")
        environment, markers = self.hostile_shell_environment("just-facade")
        self.prove_shell_attacks(environment, markers)
        result = subprocess.run(  # noqa: S603 -- fixed Just and isolated fixture argv
            [
                REAL_JUST,
                "--justfile",
                str(self.upstream / "justfile"),
                "workspace::new",
                "just-facade-proof",
                "origin/dev",
            ],
            cwd=self.upstream,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_shell_attacks_quiet(markers)

    def test_reaper_unit_executes_only_the_stable_privileged_script(self) -> None:
        """Generated systemd text bypasses PATH Bash and keeps exact argv."""
        unitdir = self.root / "units"
        stable = self.home / ".local/bin/ra8-agent-workspace"
        capture = self.root / "reaper.argv"
        stable.parent.mkdir(parents=True)
        stable.write_text(
            f"#!/bin/bash -p\nprintf '%s\\n' \"$@\" >{capture!s}\n",
            encoding="ascii",
        )
        stable.chmod(0o755)
        function = _extract_shell_function(LIFECYCLE, "write_reap_units")
        script = (
            "set -euo pipefail\n"
            "die() { printf '%s\\n' \"$*\" >&2; return 1; }\n"
            "RA8_WS_ROOT=/tmp/ws RA8_WS_UPSTREAM=/tmp/upstream\n"
            "RA8_WS_TTL_HOURS=24 RA8_WS_DISK_PCT=80 RA8_WS_TTL_HOURS_FULL=4\n"
            f'{function}\nwrite_reap_units "$1" "$2"\n'
        )
        result = subprocess.run(  # noqa: S603 -- exact Bash and extracted audited function
            [REAL_BASH, "-p", "-c", script, "unit-selftest", str(unitdir), str(stable)],
            env=self.env(PATH=f"{self.root / 'absent'}:/usr/bin:/bin"),
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        service = (unitdir / "ra8-workspace-reap.service").read_text(encoding="ascii")
        self.assertIn(f"ExecStart={stable} reap\n", service)
        self.assertNotIn("/usr/bin/env", service)
        invoked = subprocess.run(  # noqa: S603 -- private exact stable executable
            [str(stable), "reap"], env=self.env(PATH="/arbitrary/path"), check=False
        )
        self.assertEqual(invoked.returncode, 0)
        self.assertEqual(capture.read_text(encoding="ascii"), "reap\n")
        unsafe = subprocess.run(  # noqa: S603 -- negative unit-path contract
            [
                REAL_BASH,
                "-p",
                "-c",
                script,
                "unit-selftest",
                str(self.root / "unsafe-units"),
                f"{stable} with-space",
            ],
            env=self.env(),
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(unsafe.returncode, 0)

    def test_clean_remote_reachable_workspace_is_reaped(self) -> None:
        """The safe deletion direction remains live for a clean published HEAD."""
        created = self.run_lifecycle("create", "clean-reap", "origin/dev")
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        workspace = self.ws_root / "clean-reap"
        metadata = self.ws_root / ".meta/clean-reap"
        result = self.run_lifecycle("reap", "--force")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(workspace.exists())
        self.assertFalse(metadata.exists())

    def test_symlink_child_never_traverses_or_deletes_outside(self) -> None:
        """A child symlink cannot turn reaping into an outside-tree traversal."""
        self.ws_root.mkdir(mode=0o700)
        outside = self.root / "outside"
        build = outside / "build"
        build.mkdir(parents=True)
        (build / "sentinel").write_text("keep\n", encoding="ascii")
        (self.ws_root / "linked").symlink_to(outside, target_is_directory=True)
        foreign = self.ws_root / "foreign-repository"
        foreign.mkdir()
        self._git("init", "--quiet", cwd=foreign)
        result = self.run_lifecycle("reap", "--force")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((build / "sentinel").is_file())
        self.assertTrue(foreign.is_dir())

    def test_failed_status_retains_workspace_and_metadata(self) -> None:
        """A genuinely unreadable worktree identity is retained, never raw-deleted."""
        name = "status-proof"
        created = self.run_lifecycle("create", name, "origin/dev")
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        (self.ws_root / name / ".git").write_text(
            "gitdir: /absent/status-proof\n", encoding="ascii"
        )
        result = self.run_lifecycle("reap", "--force")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.ws_root / name).is_dir())
        self.assertTrue((self.ws_root / ".meta" / name).is_file())

    def test_failed_remove_retains_workspace_and_metadata(self) -> None:
        """Git's real locked-worktree refusal preserves workspace metadata."""
        name = "remove-proof"
        created = self.run_lifecycle("create", name, "origin/dev")
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        workspace = self.ws_root / name
        self._git("worktree", "lock", str(workspace), cwd=self.upstream)
        result = self.run_lifecycle("reap", "--force")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(workspace.is_dir())
        self.assertTrue((self.ws_root / ".meta" / name).is_file())

    def test_dirty_untracked_busy_and_local_only_work_are_retained(self) -> None:
        """Every class of potentially valuable local state survives force reaping."""
        dirty = self.run_lifecycle("create", "dirty-proof", "origin/dev")
        self.assertEqual(dirty.returncode, 0, dirty.stdout + dirty.stderr)
        (self.ws_root / "dirty-proof/README.md").write_text("dirty\n", encoding="ascii")

        untracked = self.run_lifecycle("create", "untracked-proof", "origin/dev")
        self.assertEqual(untracked.returncode, 0, untracked.stdout + untracked.stderr)
        (self.ws_root / "untracked-proof/LOCAL.txt").write_text("local\n", encoding="ascii")

        local = self.run_lifecycle("create", "local-commit-proof", "origin/dev")
        self.assertEqual(local.returncode, 0, local.stdout + local.stderr)
        local_ws = self.ws_root / "local-commit-proof"
        self._git("config", "user.name", "Workspace Selftest", cwd=local_ws)
        self._git("config", "user.email", "workspace@example.invalid", cwd=local_ws)
        (local_ws / "README.md").write_text("local commit\n", encoding="ascii")
        self._git("add", "README.md", cwd=local_ws)
        self._git("commit", "--quiet", "-m", "local only", cwd=local_ws)

        busy = self.run_lifecycle("create", "busy-proof", "origin/dev")
        self.assertEqual(busy.returncode, 0, busy.stdout + busy.stderr)
        if REAL_SLEEP is None:
            self.fail("sleep is required")
        sleeper = subprocess.Popen(  # noqa: S603 -- resolved sleep with fixed argv
            [REAL_SLEEP, "30"], cwd=self.ws_root / "busy-proof"
        )
        try:
            result = self.run_lifecycle("reap", "--force")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in ("dirty-proof", "untracked-proof", "local-commit-proof", "busy-proof"):
                self.assertTrue((self.ws_root / name).is_dir(), name)
        finally:
            sleeper.terminate()
            sleeper.wait(timeout=5)

    def test_two_callers_block_and_signal_releases_the_lock(self) -> None:
        """A second transaction waits and proceeds after the first is terminated."""
        entered = self.root / "entered"
        release = self.root / "release"
        wrapper = self.root / "ssh-transport"
        wrapper.write_text(
            "#!/bin/bash -p\n"
            "set -euo pipefail\n"
            'if [[ "${RA8_HOLD_FETCH:-}" == 1 ]]; then\n'
            '  : >"${RA8_ENTERED:?}"\n'
            '  while [[ ! -e "${RA8_RELEASE:?}" ]]; do /usr/bin/sleep 0.01; done\n'
            "fi\n"
            "exit 77\n",
            encoding="ascii",
        )
        wrapper.chmod(0o755)
        self._git("remote", "set-url", "origin", "ssh://fixture.invalid/repo", cwd=self.upstream)
        common = self.env(
            RA8_ENTERED=str(entered),
            RA8_RELEASE=str(release),
            GIT_SSH_COMMAND=str(wrapper),
        )
        first_env = dict(common, RA8_HOLD_FETCH="1")
        if REAL_BASH is None:
            self.fail("bash is required")
        first = subprocess.Popen(  # noqa: S603 -- resolved Bash and fixed lifecycle argv
            [REAL_BASH, "-p", str(LIFECYCLE), "reap", "--quiet"],
            cwd=self.upstream,
            env=first_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        for _attempt in range(500):
            if entered.exists():
                break
            time.sleep(0.01)
        self.assertTrue(entered.exists())
        second = subprocess.Popen(  # noqa: S603 -- resolved Bash and fixed lifecycle argv
            [REAL_BASH, "-p", str(LIFECYCLE), "reap", "--quiet"],
            cwd=self.upstream,
            env=common,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        time.sleep(0.2)
        self.assertIsNone(second.poll(), "second lifecycle transaction did not block")
        first.send_signal(signal.SIGTERM)
        release.touch()
        first.communicate(timeout=10)
        second_stdout, second_stderr = second.communicate(timeout=10)
        self.assertEqual(second.returncode, 0, second_stdout + second_stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
