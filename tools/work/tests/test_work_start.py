# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Canonical workspace delegation, locking, metadata, and binding tests."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "fixtures"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import work
import work_workspace
from work_git import branch_exists, discover_repo
from work_testlib import (
    HarnessCase,
    RunResult,
    fixture_git_environment,
    hostile_filter_environment,
    make_repo,
    portable_prerequisite,
    prove_hostile_filter_executes,
    run_git,
    run_work,
)
from work_workspace import (
    FOREIGN,
    FORGED,
    READY,
    ClaimError,
    classify,
    list_claims,
    load_claim,
    metadata_path,
    recovery_command,
)

SOURCE_SCRIPT = Path(__file__).resolve().parents[3] / "scripts/dev/agent_workspace.sh"
SOURCE_GUARD = Path(__file__).resolve().parents[3] / "scripts/dev/workspace_guard.py"
SOURCE_GIT_ENVIRONMENT = Path(__file__).resolve().parents[3] / "scripts/dev/git_environment.py"
SOURCE_GIT_ENVIRONMENT_SHELL = (
    Path(__file__).resolve().parents[3] / "scripts/dev/git_environment.sh"
)
LINUX_LIFECYCLE_REASON = "canonical lifecycle is Linux-only"
LINUX_LIFECYCLE_AVAILABLE = sys.platform.startswith("linux")


class CanonicalStart(HarnessCase):
    """The work client creates only through the canonical lifecycle."""

    def setUp(self) -> None:
        """Install the canonical script into the fixture repository."""
        super().setUp()
        destination = self.repo / "scripts/dev/agent_workspace.sh"
        destination.parent.mkdir(parents=True)
        shutil.copy2(SOURCE_SCRIPT, destination)
        destination.chmod(0o755)
        shutil.copy2(SOURCE_GUARD, destination.with_name("workspace_guard.py"))
        shutil.copy2(SOURCE_GIT_ENVIRONMENT, destination.with_name("git_environment.py"))
        shutil.copy2(SOURCE_GIT_ENVIRONMENT_SHELL, destination.with_name("git_environment.sh"))
        self.script = destination
        run_git(["remote", "add", "origin", str(self.repo)], self.repo)
        os.environ["RA8_WS_ROOT"] = str(self.ws)
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        self.podman_log = self.root / "podman.log"
        self.fake_podman = fake_bin / "podman"
        self.fake_podman.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >>"$FAKE_PODMAN_LOG"\n', encoding="ascii"
        )
        self.fake_podman.chmod(0o755)
        os.environ["FAKE_PODMAN_LOG"] = str(self.podman_log)
        os.environ["PATH"] = f"{fake_bin}:{os.environ['PATH']}"

    def start(self, identifier: str, *flags: str) -> RunResult:
        """Invoke one work start."""
        return run_work(["start", identifier, "--ws-root", str(self.ws), *flags], self.repo)

    def test_default_is_a_pure_preview(self) -> None:
        """No workspace, branch, metadata, or lock appears in dry-run mode."""
        result = self.start("demo")
        self.assertEqual(result.status, 0, result.output)
        self.assertIn("DRY RUN", result.stdout)
        self.assertIn("canonical", result.stdout)
        self.assertFalse(self.ws.exists())
        self.assertFalse(branch_exists("work/demo", cwd=self.repo))

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_execute_creates_canonical_metadata_and_bound_branch(self) -> None:
        """The canonical tool owns creation and records its schema-2 claim."""
        result = self.start("demo", "--execute")
        self.assertEqual(result.status, 0, result.output)
        path = metadata_path(self.ws, "demo")
        claim = load_claim(path, self.ws)
        self.assertEqual(claim.branch, "work/demo")
        self.assertEqual(claim.worktree, (self.ws / "work-demo").resolve())
        self.assertEqual(classify(claim, self.repo), READY)
        text = path.read_text(encoding="ascii")
        self.assertIn("owner=work\n", text)
        self.assertIn("schema=2\n", text)
        self.assertFalse((self.repo / ".git/ra8-work").exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_execute_cannot_route_through_an_external_git_environment(self) -> None:
        """Discovery and the lifecycle child both ignore hostile caller Git routing."""
        sentinel = make_repo(self.root / "sentinel", self.hooks)
        sentinel_git = sentinel / ".git"

        def snapshot() -> tuple[tuple[str, int, bytes], ...]:
            return tuple(
                (
                    path.relative_to(sentinel_git).as_posix(),
                    path.stat().st_mtime_ns,
                    path.read_bytes(),
                )
                for path in sorted(sentinel_git.rglob("*"))
                if path.is_file()
            )

        before = snapshot()
        config_hostile, marker = hostile_filter_environment(self.root)
        prove_hostile_filter_executes(self.root, config_hostile, marker)
        hostile = {
            "GIT_ALTERNATE_OBJECT_DIRECTORIES": str(sentinel_git / "objects"),
            "GIT_DIR": str(sentinel_git),
            "GIT_INDEX_FILE": str(sentinel_git / "index"),
            "GIT_OBJECT_DIRECTORY": str(sentinel_git / "objects"),
            "GIT_WORK_TREE": str(sentinel),
            **config_hostile,
        }
        with patch.dict(os.environ, hostile, clear=False):
            result = self.start("hostile-env", "--execute")
        self.assertEqual(result.status, 0, result.output)
        claim = load_claim(metadata_path(self.ws, "hostile-env"), self.ws)
        self.assertEqual(classify(claim, self.repo), READY)
        self.assertEqual(snapshot(), before)
        self.assertFalse(marker.exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_execute_neutralizes_local_checkout_hooks(self) -> None:
        """A trusted repo's local hooks stay installed but cannot run during creation."""
        marker = self.root / "post-checkout-ran"
        hook = self.hooks / "post-checkout"
        hook.write_text(f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\n", encoding="ascii")
        hook.chmod(0o755)
        result = self.start("no-checkout-hook", "--execute")
        self.assertEqual(result.status, 0, result.output)
        claim = load_claim(metadata_path(self.ws, "no-checkout-hook"), self.ws)
        self.assertEqual(classify(claim, self.repo), READY)
        self.assertFalse(marker.exists())
        configured = run_git(["config", "--local", "core.hooksPath"], self.repo).stdout.strip()
        self.assertEqual(configured, str(self.hooks))

    def test_start_refuses_unexpected_local_filter_config_without_execution(self) -> None:
        """An external attributes file cannot activate a novel local filter."""
        filter_marker = self.root / "local-filter-ran"
        filter_helper = self.root / "local-filter"
        filter_helper.write_text(
            f"#!/bin/sh\nprintf x >>{shlex.quote(str(filter_marker))}\ncat\n",
            encoding="ascii",
        )
        filter_helper.chmod(0o755)
        local_attributes = self.root / "local-attributes"
        local_attributes.write_text("* filter=local-attack\n", encoding="ascii")
        run_git(["config", "core.attributesFile", str(local_attributes)], self.repo)
        run_git(["config", "filter.local-attack.smudge", str(filter_helper)], self.repo)
        result = self.start("local-filter", "--execute")
        self.assertEqual(result.status, work.EXIT_CONFIG, result.output)
        self.assertIn("refusing untrusted local Git driver config", result.output)
        self.assertFalse(filter_marker.exists())
        configured_attributes = run_git(
            ["config", "--local", "core.attributesFile"], self.repo
        ).stdout.strip()
        self.assertEqual(configured_attributes, str(local_attributes))

    def test_start_refuses_a_novel_tracked_filter_without_execution(self) -> None:
        """Only the repository's declared LFS driver may reach workspace checkout."""
        marker = self.root / "novel-filter-ran"
        helper = self.root / "novel-filter"
        helper.write_text(
            f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\ncat\n", encoding="ascii"
        )
        helper.chmod(0o755)
        run_git(["config", "filter.novel.smudge", str(helper)], self.repo)
        attributes = self.repo / ".gitattributes"
        attributes.write_text("README.md filter=novel\n", encoding="ascii")
        run_git(["add", ".gitattributes"], self.repo)
        run_git(["commit", "-m", "hostile attribute fixture"], self.repo)
        result = self.start("novel-filter", "--execute")
        self.assertEqual(result.status, work.EXIT_CONFIG, result.output)
        self.assertIn("refusing untrusted Git attribute", result.output)
        self.assertFalse(marker.exists())
        self.assertFalse(metadata_path(self.ws, "novel-filter").exists())

    def test_start_validates_attributes_from_the_exact_target_commit(self) -> None:
        """A clean current tree cannot hide a hostile .gitattributes in --ref."""
        marker = self.root / "target-filter-ran"
        helper = self.root / "target-filter"
        helper.write_text(
            f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\ncat\n", encoding="ascii"
        )
        helper.chmod(0o755)
        run_git(["switch", "-c", "hostile-target"], self.repo)
        (self.repo / ".gitattributes").write_text(
            "README.md filter=target-evil\n", encoding="ascii"
        )
        run_git(["add", ".gitattributes"], self.repo)
        run_git(["commit", "-m", "target-only hostile attribute"], self.repo)
        target = run_git(["rev-parse", "HEAD"], self.repo).stdout.strip()
        run_git(["switch", "main"], self.repo)
        run_git(["config", "filter.target-evil.smudge", str(helper)], self.repo)
        result = self.start("target-filter", "--ref", target, "--execute")
        self.assertEqual(result.status, work.EXIT_CONFIG, result.output)
        self.assertIn(target, result.output)
        self.assertIn("filter=target-evil", result.output)
        self.assertFalse(marker.exists())
        self.assertFalse(metadata_path(self.ws, "target-filter").exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_committed_lfs_attribute_policy_remains_usable(self) -> None:
        """Exact LFS policy checks out pointers without smudge/helper execution."""
        attributes = self.repo / ".gitattributes"
        attributes.write_text(
            "*.c diff=c\n*.cpp diff=cpp\n*.epub filter=lfs diff=lfs merge=lfs -text\n",
            encoding="ascii",
        )
        pointer = f"version https://git-lfs.github.com/spec/v1\noid sha256:{'0' * 64}\nsize 0\n"
        source = self.repo / "fixture.epub"
        source.write_text(pointer, encoding="ascii")
        run_git(["add", ".gitattributes", "fixture.epub"], self.repo)
        run_git(["commit", "-m", "declared attribute policy"], self.repo)
        marker = self.root / "git-lfs-ran"
        fake_lfs = self.fake_podman.with_name("git-lfs")
        fake_lfs.write_text(
            f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\nexit 9\n", encoding="ascii"
        )
        fake_lfs.chmod(0o755)
        for key, value in (
            ("filter.lfs.clean", "git-lfs clean -- %f"),
            ("filter.lfs.smudge", "git-lfs smudge -- %f"),
            ("filter.lfs.process", "git-lfs filter-process"),
            ("filter.lfs.required", "true"),
        ):
            run_git(["config", key, value], self.repo)
        result = self.start("declared-attributes", "--execute")
        self.assertEqual(result.status, 0, result.output)
        claim = load_claim(metadata_path(self.ws, "declared-attributes"), self.ws)
        self.assertEqual(classify(claim, self.repo), READY)
        checked_out = self.ws / "work-declared-attributes/fixture.epub"
        self.assertEqual(checked_out.read_text(encoding="ascii"), pointer)
        self.assertFalse(marker.exists())

    def test_start_refuses_duplicate_lfs_driver_values(self) -> None:
        """A canonical value plus one extra value is config drift, not canonical."""
        marker = self.root / "duplicate-lfs-ran"
        helper = self.root / "duplicate-lfs"
        helper.write_text(
            f"#!/bin/sh\nprintf x >>{shlex.quote(str(marker))}\ncat\n", encoding="ascii"
        )
        helper.chmod(0o755)
        for key, value in (
            ("filter.lfs.clean", "git-lfs clean -- %f"),
            ("filter.lfs.smudge", "git-lfs smudge -- %f"),
            ("filter.lfs.process", "git-lfs filter-process"),
            ("filter.lfs.required", "true"),
        ):
            run_git(["config", key, value], self.repo)
        run_git(["config", "--add", "filter.lfs.smudge", str(helper)], self.repo)
        result = self.start("duplicate-lfs", "--execute")
        self.assertEqual(result.status, work.EXIT_CONFIG, result.output)
        self.assertIn("drifted partial filter.lfs", result.output)
        self.assertFalse(marker.exists())
        self.assertFalse(metadata_path(self.ws, "duplicate-lfs").exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_original_create_api_remains_detached_and_agent_owned(self) -> None:
        """Direct shell entry stays detached and ignores hostile Git routing."""
        commit = run_git(["rev-parse", "HEAD"], self.repo).stdout.strip()
        sentinel = make_repo(self.root / "direct-sentinel", self.hooks)
        sentinel_git = sentinel / ".git"

        def snapshot() -> tuple[tuple[str, int, bytes], ...]:
            return tuple(
                (
                    path.relative_to(sentinel_git).as_posix(),
                    path.stat().st_mtime_ns,
                    path.read_bytes(),
                )
                for path in sorted(sentinel_git.rglob("*"))
                if path.is_file()
            )

        before = snapshot()
        hostile_config, marker = hostile_filter_environment(self.root)
        prove_hostile_filter_executes(self.root, hostile_config, marker)
        environment = fixture_git_environment()
        environment.update(
            {
                "GIT_DIR": str(sentinel_git),
                "GIT_INDEX_FILE": str(sentinel_git / "index"),
                "GIT_OBJECT_DIRECTORY": str(sentinel_git / "objects"),
                "GIT_WORK_TREE": str(sentinel),
                "RA8_WS_UPSTREAM": str(self.repo),
                **hostile_config,
            }
        )
        shell = "/bin/bash" if Path("/bin/bash").is_file() else None
        if not portable_prerequisite(
            available=shell is not None,
            reason="bash is required for the canonical lifecycle fixture",
        ):
            self.skipTest("bash is required for the canonical lifecycle fixture")
        done = subprocess.run(  # noqa: S603 -- fixture script and controlled argv
            [shell, "-p", str(self.script), "create", "legacy", commit],
            cwd=self.repo,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(snapshot(), before)
        self.assertFalse(marker.exists())
        metadata = (self.ws / ".meta/legacy").read_text(encoding="ascii")
        self.assertIn("owner=agent\n", metadata)
        self.assertIn("branch=DETACHED\n", metadata)
        branch = run_git(["rev-parse", "--abbrev-ref", "HEAD"], self.ws / "legacy").stdout.strip()
        self.assertEqual(branch, "HEAD")

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_create_and_reap_never_invoke_unscoped_podman(self) -> None:
        """The recorder fires directly but stays quiet through both lifecycle paths."""
        direct = subprocess.run(  # noqa: S603 -- isolated recorder fixture
            [str(self.fake_podman), "sentinel"],
            env=os.environ.copy(),
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(direct.returncode, 0, direct.stdout + direct.stderr)
        self.assertEqual(self.podman_log.read_text(encoding="ascii"), "sentinel\n")
        self.podman_log.unlink()

        self.assertEqual(self.start("no-podman", "--execute").status, 0)
        environment = fixture_git_environment()
        environment["RA8_WS_UPSTREAM"] = str(self.repo)
        shell = "/bin/bash" if Path("/bin/bash").is_file() else None
        if not portable_prerequisite(
            available=shell is not None,
            reason="bash is required for the canonical lifecycle fixture",
        ):
            self.skipTest("bash is required for the canonical lifecycle fixture")
        reaped = subprocess.run(  # noqa: S603 -- isolated canonical fixture
            [shell, "-p", str(self.script), "reap"],
            cwd=self.repo,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(reaped.returncode, 0, reaped.stdout + reaped.stderr)
        self.assertFalse(self.podman_log.exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_canonical_lock_serializes_two_service_callers(self) -> None:
        """Exactly one simultaneous creation wins; healthy metadata is not downgraded."""
        commit = run_git(["rev-parse", "HEAD"], self.repo).stdout.strip()
        script = self.repo / "scripts/dev/agent_workspace.sh"
        environment = fixture_git_environment()
        environment["RA8_WS_UPSTREAM"] = str(self.repo)
        argv = [
            "/bin/bash",
            "-p",
            str(script),
            "create",
            "work-race",
            commit,
            "--branch",
            "work/race",
            "--owner",
            "work",
        ]
        barrier = threading.Barrier(3)
        results: list[int] = []

        def caller() -> None:
            barrier.wait()
            done = subprocess.run(  # noqa: S603 -- fixture script and argv
                argv,
                cwd=self.repo,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            results.append(done.returncode)

        workers = [threading.Thread(target=caller) for _ in range(2)]
        for worker in workers:
            worker.start()
        barrier.wait()
        for worker in workers:
            worker.join()
        self.assertEqual(sorted(results), [0, 1])
        claim = load_claim(metadata_path(self.ws, "race"), self.ws)
        self.assertEqual(classify(claim, self.repo), READY)

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_canonical_create_rechecks_branch_after_client_preview(self) -> None:
        """A collision introduced after preview is rejected under the canonical lock."""
        paths = discover_repo(self.repo)
        options = argparse.Namespace(identifier="late", ref="HEAD", ws_root=str(self.ws))
        preview = work._build_start_plan(  # noqa: SLF001 -- exercise the locked race seam
            paths, self.repo, options
        )
        self.assertFalse(preview.refusals)
        run_git(["branch", "work/late"], self.repo)
        result = work._execute_start(  # noqa: SLF001 -- exercise the locked race seam
            paths, preview
        )
        self.assertEqual(result, 1)
        self.assertFalse(metadata_path(self.ws, "late").exists())

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_branch_mismatch_is_foreign_not_ready(self) -> None:
        """A registered path on another branch cannot satisfy a claim by name alone."""
        self.assertEqual(self.start("binding", "--execute").status, 0)
        claim = load_claim(metadata_path(self.ws, "binding"), self.ws)
        run_git(["switch", "-c", "other"], claim.worktree)
        self.assertEqual(classify(claim, self.repo), FOREIGN)

    @unittest.skipUnless(
        portable_prerequisite(available=LINUX_LIFECYCLE_AVAILABLE, reason=LINUX_LIFECYCLE_REASON),
        "canonical lifecycle is Linux-only",
    )
    def test_registered_head_and_base_object_mismatches_are_forged(self) -> None:
        """Neither a stale inventory HEAD nor an absent immutable base can validate."""
        self.assertEqual(self.start("objects", "--execute").status, 0)
        claim_path = metadata_path(self.ws, "objects")
        claim = load_claim(claim_path, self.ws)
        fake_binding = {claim.worktree: (claim.branch, "0" * 40)}
        # Inject contradictory Git porcelain evidence at the classifier seam.
        with patch.object(work_workspace, "_worktree_bindings", return_value=fake_binding):
            self.assertEqual(classify(claim, self.repo), FORGED)
        text = claim_path.read_text(encoding="ascii")
        claim_path.write_text(
            text.replace(f"base_commit={claim.base_commit}", f"base_commit={'0' * 40}"),
            encoding="ascii",
        )
        self.assertEqual(classify(load_claim(claim_path, self.ws), self.repo), FORGED)

    def test_status_ignores_legacy_agent_metadata(self) -> None:
        """Only schema-2 owner=work records belong to this client."""
        directory = self.ws / ".meta"
        directory.mkdir(parents=True)
        (directory / "agent-job").write_text(
            "created=now\nby=agent\nref=HEAD\npath=/tmp/agent-job\n",
            encoding="ascii",
        )
        self.assertEqual(list_claims(self.ws), [])

    def test_reserved_agent_metadata_is_loudly_forged(self) -> None:
        """An agent-owned record may not disappear under the work-* namespace."""
        self.assertEqual(self.start("reserved", "--execute").status, 0)
        path = metadata_path(self.ws, "reserved")
        path.write_text(
            path.read_text(encoding="ascii").replace("owner=work", "owner=agent"),
            encoding="ascii",
        )
        records = list_claims(self.ws)
        self.assertEqual(len(records), 1)
        self.assertIsInstance(records[0][1], ClaimError)
        result = run_work(["status", "--ws-root", str(self.ws)], self.repo)
        self.assertEqual(result.status, 1)
        self.assertIn("FORGED", result.stdout)

    def test_reserved_wrong_schema_is_loudly_forged(self) -> None:
        """A reserved record with an unsupported schema makes status nonzero."""
        directory = self.ws / ".meta"
        directory.mkdir(parents=True)
        (directory / "work-schema").write_text("schema=99\n", encoding="ascii")
        records = list_claims(self.ws)
        self.assertEqual(len(records), 1)
        self.assertIsInstance(records[0][1], ClaimError)
        result = run_work(["status", "--ws-root", str(self.ws)], self.repo)
        self.assertEqual(result.status, 1)
        self.assertIn("FORGED", result.stdout)

    def test_symlinked_canonical_child_is_never_ready(self) -> None:
        """Resolving a child outside the canonical root cannot satisfy a claim."""
        self.assertEqual(self.start("link", "--execute").status, 0)
        claim_path = metadata_path(self.ws, "link")
        claim = load_claim(claim_path, self.ws)
        outside = self.root / "outside-worktree"
        run_git(["worktree", "move", str(claim.worktree), str(outside)], self.repo)
        claim.worktree.symlink_to(outside, target_is_directory=True)
        text = claim_path.read_text(encoding="ascii").replace(
            f"path={claim.worktree}", f"path={outside}"
        )
        claim_path.write_text(text, encoding="ascii")
        with self.assertRaises(ClaimError):
            load_claim(claim_path, self.ws)

    def test_recovery_command_is_shell_quoted(self) -> None:
        """A workspace root containing spaces stays one argv element."""
        root = self.root / "root with spaces"
        path = root / ".meta/work-safe"
        path.parent.mkdir(parents=True)
        commit = run_git(["rev-parse", "HEAD"], self.repo).stdout.strip()
        path.write_text(
            "schema=2\nname=work-safe\ncreated=now\nby=user\nref=HEAD\n"
            f"base_commit={commit}\npath={root / 'work-safe'}\n"
            "branch=work/safe\nowner=work\n",
            encoding="ascii",
        )
        claim = load_claim(path, root)
        command = recovery_command(claim, self.repo, stale=True)
        words = shlex.split(command)
        self.assertEqual(words[-2:], ["forget", "work-safe"])
        self.assertEqual(words[:2], ["/bin/bash", "-p"])
        self.assertEqual(words[2], str(self.repo / "scripts/dev/agent_workspace.sh"))

    def test_invalid_identifiers_are_rejected_before_paths(self) -> None:
        """Traversal, spaces and shell syntax cannot become workspace names."""
        for value in ("../x", "with space", "x;touch", ""):
            with self.subTest(value=value):
                result = self.start(value)
                self.assertEqual(result.status, 2)

    def test_existing_path_branch_and_metadata_are_refusals(self) -> None:
        """Every collision is visible during preview."""
        (self.ws / "work-path").mkdir(parents=True)
        self.assertIn("path already exists", self.start("path").stderr)
        run_git(["branch", "work/branch"], self.repo)
        self.assertIn("branch already exists", self.start("branch").stderr)

    def test_unresolved_ref_is_a_refusal(self) -> None:
        """Creation never guesses or fetches a missing start point."""
        result = run_work(
            ["start", "badref", "--ref", "refs/heads/absent", "--ws-root", str(self.ws)],
            self.repo,
        )
        self.assertEqual(result.status, 1)
        self.assertIn("does not resolve locally", result.stderr)

    def test_single_line_root_filter_blocks_report_injection(self) -> None:
        """A newline-bearing workspace override is refused before reporting it."""
        injected = f"{self.root / 'one'}\nFORGED"
        result = run_work(["start", "demo", "--ws-root", injected], self.repo)
        self.assertEqual(result.status, 2)
        self.assertNotIn("\nFORGED", result.output)

    def test_non_ascii_or_control_root_is_rejected_before_reporting(self) -> None:
        """Canonical ASCII metadata cannot be requested through an unsafe path."""
        for suffix in ("\tcontrol", "-nonascii-\N{SNOWMAN}"):
            with self.subTest(suffix=suffix):
                result = run_work(["start", "demo", "--ws-root", f"{self.root}{suffix}"], self.repo)
                self.assertEqual(result.status, 2)


class ClaimParser(HarnessCase):
    """Canonical metadata is untrusted input even though its writer is shared."""

    def test_symlink_metadata_is_refused(self) -> None:
        """A planted symlink cannot redirect the client into another file."""
        target = self.root / "outside"
        target.write_text("schema=2\n", encoding="ascii")
        path = self.ws / ".meta/work-demo"
        path.parent.mkdir(parents=True)
        path.symlink_to(target)
        with self.assertRaises(ClaimError):
            load_claim(path, self.ws)

    def test_duplicate_field_is_refused(self) -> None:
        """Last-one-wins metadata parsing is not permitted."""
        path = self.ws / ".meta/work-demo"
        path.parent.mkdir(parents=True)
        path.write_text("schema=2\nschema=2\n", encoding="ascii")
        with self.assertRaises(ClaimError):
            load_claim(path, self.ws)

    def test_newline_metadata_cannot_form_a_valid_claim(self) -> None:
        """Injected rows are parsed as extra fields and fail the exact schema."""
        path = self.ws / ".meta/work-demo"
        path.parent.mkdir(parents=True)
        path.write_text("schema=2\nname=work-demo\nby=x\nFORGED\n", encoding="ascii")
        with self.assertRaises(ClaimError):
            load_claim(path, self.ws)


if __name__ == "__main__":
    unittest.main()
