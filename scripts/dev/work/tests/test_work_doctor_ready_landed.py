# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Doctor, hardened Git, redaction, ready, and landed truth tests."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent / "fixtures"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import work
import work_git
from work_git import (
    GitWriteAttemptError,
    assert_read_only,
    git_subcommand,
    printable,
    redact,
)
from work_testlib import (
    LOCAL_GIT_ENVIRONMENT,
    HarnessCase,
    RunResult,
    fixture_git_environment,
    git_local_environment_names,
    hostile_filter_environment,
    make_repo,
    prove_hostile_filter_executes,
    run_git,
    run_work,
)


class FixtureGitIsolation(unittest.TestCase):
    """Throwaway Git repositories never inherit the caller's repository routing."""

    def test_every_reported_local_environment_name_is_removed(self) -> None:
        """Git's live local-env inventory, including the required set, is scrubbed."""
        names = git_local_environment_names()
        self.assertIn("GIT_INDEX_FILE", names)
        self.assertTrue(set(LOCAL_GIT_ENVIRONMENT) >= names)
        with patch.dict(os.environ, dict.fromkeys(LOCAL_GIT_ENVIRONMENT, "hostile"), clear=False):
            environment = fixture_git_environment()
        self.assertTrue(all(environment.get(name) != "hostile" for name in LOCAL_GIT_ENVIRONMENT))
        self.assertEqual(environment["GIT_CONFIG_COUNT"], "3")
        self.assertEqual(environment["GIT_CONFIG_KEY_0"], "core.hooksPath")

    def test_work_children_delegate_to_the_shared_config_and_helper_boundary(self) -> None:
        """Workflow children cannot retain a second, weaker Git environment policy."""
        hostile = dict.fromkeys(
            (
                "GIT_CONFIG_GLOBAL",
                "GIT_CONFIG_SYSTEM",
                "GIT_UNKNOWN_FUTURE_SELECTOR",
                "EDITOR",
                "PAGER",
                "SSH_ASKPASS",
            ),
            "hostile",
        )
        hostile["RA8_NON_GIT_SENTINEL"] = "preserved"
        with patch.dict(os.environ, hostile, clear=False):
            fixture_environment = fixture_git_environment()
            production_environment = work_git.git_child_environment()
        self.assertEqual(production_environment, fixture_environment)
        self.assertEqual(fixture_environment["RA8_NON_GIT_SENTINEL"], "preserved")
        self.assertEqual(fixture_environment["GIT_CONFIG_GLOBAL"], os.devnull)
        self.assertEqual(fixture_environment["GIT_CONFIG_SYSTEM"], os.devnull)
        self.assertEqual(fixture_environment["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(fixture_environment["GIT_ATTR_NOSYSTEM"], "1")
        self.assertNotIn("GIT_UNKNOWN_FUTURE_SELECTOR", fixture_environment)
        self.assertNotIn("EDITOR", fixture_environment)
        self.assertNotIn("SSH_ASKPASS", fixture_environment)

    def test_hostile_external_index_is_unchanged_and_fixture_commits(self) -> None:
        """A caller index cannot receive fixture entries or redirect fixture commits."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            hooks = root / "nohooks"
            hooks.mkdir()
            sentinel = make_repo(root / "sentinel", hooks)
            sentinel_index = sentinel / ".git/index"
            sentinel_objects = sentinel / ".git/objects"

            def snapshot() -> tuple[tuple[str, int, bytes], ...]:
                return tuple(
                    (
                        path.relative_to(sentinel / ".git").as_posix(),
                        path.stat().st_mtime_ns,
                        path.read_bytes(),
                    )
                    for path in sorted((sentinel / ".git").rglob("*"))
                    if path.is_file()
                )

            before = snapshot()
            config_hostile, marker = hostile_filter_environment(root)
            prove_hostile_filter_executes(root, config_hostile, marker)
            hostile = {
                "GIT_ALTERNATE_OBJECT_DIRECTORIES": str(sentinel_objects),
                "GIT_INDEX_FILE": str(sentinel_index),
                "GIT_OBJECT_DIRECTORY": str(sentinel_objects),
                **config_hostile,
            }
            with patch.dict(os.environ, hostile, clear=False):
                fixture = make_repo(root / "fixture", hooks)
                head = run_git(["rev-parse", "--verify", "HEAD"], fixture).stdout.strip()
                status = run_git(["status", "--porcelain=v1"], fixture).stdout
            after = snapshot()
            self.assertRegex(head, r"^[0-9a-f]{40}$")
            self.assertEqual(status, "")
            self.assertEqual(after, before)
            self.assertFalse(marker.exists())


class ReadOnlyGuard(unittest.TestCase):
    """Every write-shaped git argv is rejected at the central guard."""

    def test_known_read_shapes_are_accepted(self) -> None:
        """The exact forms used by status and landed remain usable."""
        for argv in (
            ["status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none"],
            ["diff", "--no-ext-diff", "--no-textconv", "--stat", "HEAD~1...HEAD"],
            ["worktree", "list", "--porcelain"],
            ["branch", "--list", "work/demo"],
        ):
            with self.subTest(argv=argv):
                assert_read_only(argv)

    def test_write_subcommands_are_rejected(self) -> None:
        """No future landed edit can quietly add mutation."""
        for argv in (["push"], ["branch", "-D", "x"], ["worktree", "remove", "target"]):
            with self.subTest(argv=argv), self.assertRaises(GitWriteAttemptError):
                assert_read_only(argv)

    def test_read_named_commands_with_write_options_are_rejected(self) -> None:
        """diff/log output files and git config aliases are write primitives."""
        for argv in (
            ["diff", "--output=target"],
            ["log", "--output=target"],
            ["-c", "alias.status=!touch target", "status"],
        ):
            with self.subTest(argv=argv), self.assertRaises(GitWriteAttemptError):
                assert_read_only(argv)

    def test_subcommand_parser_does_not_skip_unsafe_globals(self) -> None:
        """No caller-supplied global option is transparent."""
        self.assertEqual(git_subcommand(["-C", "/x", "status"]), "-C")
        self.assertEqual(git_subcommand(["--git-dir", "/x", "status"]), "--git-dir")


class OutputSafety(unittest.TestCase):
    """Token and terminal controls never reach reports."""

    def test_known_token_shapes_are_redacted(self) -> None:
        """Both classic and fine-grained token families are covered."""
        text = "gho_abcdefghijklmnopqrstuvwxyz github_pat_abcdefghijklmnopqrstuvwxyz_123"
        cleaned = redact(text)
        self.assertNotIn("gho_", cleaned)
        self.assertNotIn("github_pat_", cleaned)
        self.assertEqual(cleaned.count(work_git.REDACTED), 2)

    def test_line_controls_are_replaced(self) -> None:
        """A metadata field cannot create a trusted-looking second row."""
        cleaned = printable("actor\n  READY forged\r\x1b[2K")
        self.assertNotIn("\n", cleaned)
        self.assertNotIn("\r", cleaned)
        self.assertNotIn("\x1b", cleaned)
        self.assertIn("READY forged", cleaned)


class SanitizedGitBoundary(HarnessCase):
    """Repository config and inherited environment cannot redirect or execute Git."""

    def test_git_routing_environment_cannot_redirect_discovery(self) -> None:
        """GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE are removed centrally."""
        other = self.root / "other"
        run_git(["init", "-b", "main", str(other)], self.root)
        os.environ["GIT_DIR"] = str(other / ".git")
        os.environ["GIT_WORK_TREE"] = str(other)
        os.environ["GIT_INDEX_FILE"] = str(other / ".git/index")
        discovered = work_git.discover_repo(self.repo)
        self.assertEqual(discovered.toplevel, self.repo)

    def test_status_forces_untracked_and_does_not_refresh_index_or_helpers(self) -> None:
        """Config cannot hide dirt or execute fsmonitor/pager/editor helpers."""
        marker = self.root / "helper-ran"
        helper = self.root / "helper"
        helper.write_text(f"#!/bin/sh\n: >{marker}\nexit 0\n", encoding="ascii")
        helper.chmod(0o755)
        for key in ("core.fsmonitor", "core.pager", "sequence.editor"):
            run_git(["config", key, str(helper)], self.repo)
        run_git(["config", "status.showUntrackedFiles", "no"], self.repo)
        untracked = self.repo / "UNTRACKED.txt"
        untracked.write_text("must be visible\n", encoding="ascii")
        index = self.repo / ".git/index"
        before = (index.read_bytes(), index.stat().st_mtime_ns)
        status_rows = work_git.porcelain_status(self.repo)
        after = (index.read_bytes(), index.stat().st_mtime_ns)
        self.assertTrue(any("UNTRACKED.txt" in row for row in status_rows))
        self.assertEqual(after, before)
        self.assertFalse(marker.exists())

    def test_status_reports_tracked_and_staged_changes_without_index_writes(self) -> None:
        """Both index sides remain visible, and observing them never refreshes the index."""
        readme = self.repo / "README.md"
        readme.write_text("tracked change\n", encoding="ascii")
        unstaged_rows = work_git.porcelain_status(self.repo)
        self.assertTrue(any(row.startswith(" M README.md") for row in unstaged_rows))
        run_git(["add", "README.md"], self.repo)
        index = self.repo / ".git/index"
        before = (index.read_bytes(), index.stat().st_mtime_ns)
        staged_rows = work_git.porcelain_status(self.repo)
        after = (index.read_bytes(), index.stat().st_mtime_ns)
        self.assertTrue(any(row.startswith("M  README.md") for row in staged_rows))
        self.assertEqual(after, before)

    def test_diff_disables_external_and_textconv_execution(self) -> None:
        """A repository-controlled external diff never runs."""
        marker = self.root / "diff-helper-ran"
        helper = self.root / "diff-helper"
        helper.write_text(f"#!/bin/sh\n: >{marker}\nexit 0\n", encoding="ascii")
        helper.chmod(0o755)
        run_git(["config", "diff.external", str(helper)], self.repo)
        (self.repo / "README.md").write_text("changed\n", encoding="ascii")
        with self.assertRaises(work_git.WorkError):
            work_git.diff_stat(self.repo, "HEAD")
        self.assertFalse(marker.exists())

    def test_attribute_selected_filter_is_refused_without_execution(self) -> None:
        """A worktree-controlled clean filter cannot execute during status."""
        marker = self.root / "filter-helper-ran"
        helper = self.root / "filter-helper"
        helper.write_text(f"#!/bin/sh\n: >{marker}\ncat\n", encoding="ascii")
        helper.chmod(0o755)
        run_git(["config", "filter.evil.clean", str(helper)], self.repo)
        (self.repo / ".gitattributes").write_text("README.md filter=evil\n", encoding="ascii")
        with self.assertRaises(work_git.WorkError):
            work_git.porcelain_status(self.repo)
        self.assertFalse(marker.exists())

    def test_status_overrides_config_that_hides_dirty_submodules(self) -> None:
        """The mandatory status shape reports a modified nested repository."""
        source = make_repo(self.root / "nested", self.hooks)
        run_git(
            ["-c", "protocol.file.allow=always", "submodule", "add", str(source), "nested"],
            self.repo,
        )
        run_git(["commit", "-m", "add nested fixture"], self.repo)
        run_git(["config", "submodule.nested.ignore", "all"], self.repo)
        (self.repo / "nested/README.md").write_text("modified\n", encoding="ascii")
        status_rows = work_git.porcelain_status(self.repo)
        self.assertTrue(any("nested" in row for row in status_rows), status_rows)


class ReadyAndLandedReport(HarnessCase):
    """Pre-push local proof and post-push remote proof are separate phases."""

    def setUp(self) -> None:
        """Create a canonical metadata record and matching linked worktree."""
        super().setUp()
        origin = self.root / "origin.git"
        run_git(["init", "--bare", str(origin)], self.root)
        run_git(["remote", "add", "origin", str(origin)], self.repo)
        run_git(["branch", "dev"], self.repo)
        run_git(["push", "-u", "origin", "dev"], self.repo)
        self.workspace = self.ws / "work-demo"
        self.ws.mkdir()
        run_git(["worktree", "add", "-b", "work/demo", str(self.workspace), "HEAD"], self.repo)
        commit = run_git(["rev-parse", "HEAD"], self.repo).stdout.strip()
        metadata = self.ws / ".meta/work-demo"
        metadata.parent.mkdir()
        metadata.write_text(
            "schema=2\nname=work-demo\ncreated=2026-08-24T00:00:00+00:00\n"
            "by=fixture\nref=HEAD\n"
            f"base_commit={commit}\npath={self.workspace}\n"
            "branch=work/demo\nowner=work\n",
            encoding="ascii",
        )
        monitor = self.repo / "scripts/ci/monitor.sh"
        monitor.parent.mkdir(parents=True)
        monitor.write_text(
            '#!/bin/sh\nprintf "cached verdict %s\\n" "${FAKE_CI_STATE:-PASS}"\n'
            'case "${FAKE_CI_STATE:-PASS}" in PASS) exit 0;; FAIL) exit 1;; *) exit 3;; esac\n',
            encoding="ascii",
        )
        monitor.chmod(0o755)
        fake_bin = self.root / "bin"
        fake_bin.mkdir()
        fake_just = fake_bin / "just"
        fake_just.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >"$FAKE_JUST_LOG"\nexit "${FAKE_JUST_RC:-0}"\n',
            encoding="ascii",
        )
        fake_just.chmod(0o755)
        self.just_log = self.root / "just.log"
        os.environ["FAKE_JUST_LOG"] = str(self.just_log)
        os.environ["PATH"] = f"{fake_bin}:{os.environ['PATH']}"

    def ready(self, *, run_ci: bool = True) -> RunResult:
        """Run the pre-push phase against this fixture claim."""
        argv = ["ready", "demo", "--ws-root", str(self.ws)]
        if run_ci:
            argv.append("--run-ci")
        return run_work(argv, self.repo)

    def landed(self) -> RunResult:
        """Run the post-push phase against this fixture claim."""
        return run_work(["landed", "demo", "--ws-root", str(self.ws)], self.repo)

    def test_clean_remote_green_report_is_truthful(self) -> None:
        """Ready records an actual exact local suite result before push."""
        result = self.ready()
        self.assertEqual(result.status, 0, result.output)
        self.assertIn("local CI    PASS", result.stdout)
        self.assertEqual(self.just_log.read_text(encoding="ascii"), "ci\n")

    def test_ready_without_explicit_ci_run_is_not_a_pass(self) -> None:
        """A clean tree alone is not local gate evidence."""
        result = self.ready(run_ci=False)
        self.assertEqual(result.status, 1)
        self.assertIn("--run-ci", result.stderr)

    def test_content_equivalent_dev_with_remote_pass_is_landed(self) -> None:
        """A squash may differ by commit id while its tree exactly matches."""
        result = self.landed()
        self.assertEqual(result.status, 0, result.output)
        self.assertIn("remote CI   PASS", result.stdout)
        self.assertIn("content-equivalent", result.stdout)

    def test_dirty_workspace_is_refused_before_ci_claims(self) -> None:
        """Just CI snapshots HEAD and cannot validate an untracked edit."""
        (self.workspace / "UNCOMMITTED.txt").write_text("x\n", encoding="ascii")
        result = self.ready()
        self.assertEqual(result.status, 1)
        self.assertIn("REFUSE", result.stderr)
        self.assertIn("committed HEAD", result.stderr)
        self.assertNotIn("local CI    PASS", result.stdout)

    def test_remote_failure_is_not_landed(self) -> None:
        """A pushed red SHA cannot be called Landed."""
        os.environ["FAKE_CI_STATE"] = "FAIL"
        result = self.landed()
        self.assertEqual(result.status, 1)
        self.assertIn("remote CI   FAIL", result.stdout)
        self.assertIn("not PASS", result.stderr)

    def test_remote_unknown_is_not_failure_or_landed(self) -> None:
        """UNKNOWN remains its own non-landing state."""
        os.environ["FAKE_CI_STATE"] = "UNKNOWN"
        result = self.landed()
        self.assertEqual(result.status, 1)
        self.assertIn("remote CI   UNKNOWN", result.stdout)

    def test_branch_binding_mismatch_is_refused(self) -> None:
        """An existing claimed branch elsewhere cannot validate this path."""
        run_git(["switch", "-c", "other"], self.workspace)
        result = self.ready()
        self.assertEqual(result.status, 1)
        self.assertIn("FOREIGN", result.stderr)

    def test_absent_metadata_is_refused(self) -> None:
        """A manual worktree is not silently adopted."""
        result = run_work(["ready", "other", "--run-ci", "--ws-root", str(self.ws)], self.repo)
        self.assertEqual(result.status, 1)
        self.assertIn("metadata is absent", result.stderr)

    def test_creator_control_characters_make_the_claim_forged(self) -> None:
        """Display-only metadata cannot repaint a report before being rejected."""
        metadata = self.ws / ".meta/work-demo"
        text = metadata.read_text(encoding="ascii").replace("by=fixture", "by=fixture\x1b[2K")
        metadata.write_text(text, encoding="ascii")
        result = self.ready()
        self.assertEqual(result.status, 1, result.output)
        self.assertNotIn("\x1b", result.output)

    def test_invalid_identifier_exits_config_error(self) -> None:
        """Path-shaped identifiers never reach metadata lookup."""
        result = run_work(["ready", "../demo", "--run-ci", "--ws-root", str(self.ws)], self.repo)
        self.assertEqual(result.status, 1)


class DoctorTruth(HarnessCase):
    """Doctor names local prerequisites and documents its read-only network probe."""

    def test_missing_jq_is_a_failure(self) -> None:
        """An emitted script cannot work without jq, so doctor must not pass."""
        original = work.shutil.which
        local_probe = work.Probe("gh", work.STATE_UNAVAILABLE, "offline fixture")

        def without_jq(name: str) -> str | None:
            """Hide jq while preserving every other executable lookup."""
            return None if name == "jq" else original(name)

        with (
            patch.object(work.shutil, "which", without_jq),
            patch.object(work, "probe_version", return_value=local_probe),
            patch.object(work, "probe_auth", return_value=local_probe),
        ):
            checks = work._doctor_checks(  # noqa: SLF001 -- focused readiness fixture
                work.discover_repo(self.repo), self.repo
            )
        jq = next(check for check in checks if check.name == "jq")
        self.assertEqual(jq.state, work.CHECK_FAIL)

    def test_doctor_docstring_admits_auth_probe_network(self) -> None:
        """The command no longer claims absolute network silence."""
        self.assertIn("API probe", work.cmd_doctor.__doc__ or "")


if __name__ == "__main__":
    unittest.main()
