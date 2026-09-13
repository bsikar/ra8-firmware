# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Real-Just and fixed-adapter argv preservation tests."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SRC = Path(__file__).resolve().parents[1] / "src"
REPO_ROOT = Path(__file__).resolve().parents[4]
FIXTURE = Path(__file__).resolve().parent / "fixtures/valid_notes.md"
BOARD_GH_FIXTURE = Path(__file__).resolve().parent / "fixtures/fake_board_read_gh.py"
WORK_JUST = REPO_ROOT / "just/work.just"
sys.path.insert(0, str(SRC))
sys.path.insert(0, str(Path(__file__).resolve().parent / "fixtures"))

from work_just import build_argv  # noqa: E402 -- import needs the source path above
from work_testlib import (  # noqa: E402 -- import needs the fixture path above
    REGISTERED_GATE_ENV,
    portable_prerequisite,
)


class PortablePrerequisiteContract(unittest.TestCase):
    """Direct runs may skip, but the registered gate must fail on the same absence."""

    def test_direct_run_reports_an_unavailable_prerequisite(self) -> None:
        """A portable direct caller receives false and may issue its named skip."""
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop(REGISTERED_GATE_ENV, None)
            self.assertFalse(portable_prerequisite(available=False, reason="fixture dependency"))
            self.assertTrue(portable_prerequisite(available=True, reason="fixture dependency"))

    def test_registered_gate_fails_closed_or_runs_the_passing_direction(self) -> None:
        """The registered mode converts absence to failure and preserves success."""
        with patch.dict(os.environ, {REGISTERED_GATE_ENV: "1"}, clear=False):
            with self.assertRaisesRegex(RuntimeError, "registered work-harness gate"):
                portable_prerequisite(available=False, reason="fixture dependency")
            self.assertTrue(portable_prerequisite(available=True, reason="fixture dependency"))


class FixedAdapter(unittest.TestCase):
    """The adapter never joins or reparses caller data."""

    def test_every_hostile_character_stays_in_one_argument(self) -> None:
        """Spaces, metacharacters, slashes, backslashes and newlines survive."""
        hostile = "space ; $(command) \\ slash/part\nnext"
        self.assertEqual(
            build_argv(["plan", hostile, "json", hostile]),
            ["plan", hostile, "--json", hostile],
        )

    def test_unknown_or_variadic_shapes_are_refused(self) -> None:
        """There is no generic flags tail to reinterpret later."""
        with self.assertRaises(ValueError):
            build_argv(["plan", "notes", "default", "", "--extra"])

    def test_board_explorer_actions_map_to_fixed_subcommands(self) -> None:
        """Every public read-only recipe has one non-variadic CLI shape."""
        cases = {
            ("board_focus",): ["board", "focus"],
            ("board_quick_wins",): ["board", "quick-wins"],
            ("board_track", "Codebase"): ["board", "track", "Codebase"],
            ("board_epic", "epic:Harness"): ["board", "epic", "epic:Harness"],
            ("board_issue", "13"): ["board", "issue", "13"],
        }
        for raw, expected in cases.items():
            with self.subTest(raw=raw):
                self.assertEqual(build_argv(list(raw)), expected)

    def test_board_explorer_rejects_extra_arguments(self) -> None:
        """No explorer recipe exposes a generic flags tail."""
        with self.assertRaises(ValueError):
            build_argv(["board_track", "Codebase", "--format=json"])

    def test_bare_board_action_is_refused(self) -> None:
        """Verify the deleted status-column command has no adapter path."""
        with self.assertRaisesRegex(ValueError, "invalid Just action: board"):
            build_argv(["board"])

    def test_public_test_uses_the_supported_host_aware_gate_boundary(self) -> None:
        """Verify macOS reaches Linux through the existing gate selector."""
        recipe = WORK_JUST.read_text(encoding="ascii")
        expected = (
            'test:\n    "{{ just_executable() }}" '
            "quality::gate::run work-harness\n"
        )
        self.assertIn(expected, recipe)


class RealJustFacade(unittest.TestCase):
    """Invoke the actual recipe rather than testing only Python helpers."""

    def test_hostile_notes_path_is_exact_and_has_no_side_effect(self) -> None:
        """A raw-shell interpolation would create the marker; this recipe cannot."""
        just = shutil.which("just")
        self.assertIsNotNone(just, "the registered harness gate requires just")
        with tempfile.TemporaryDirectory(prefix="ra8-work-just-") as raw:
            root = Path(raw)
            marker = root / "JUST_INJECTION_MARKER"
            name = "notes with space;touch${IFS}$JUST_MARKER;$(touch${IFS}$JUST_MARKER)\\x\nline.md"
            notes = root / name
            notes.write_bytes(FIXTURE.read_bytes())
            environment = os.environ.copy()
            environment["JUST_MARKER"] = str(marker)
            result = subprocess.run(  # noqa: S603 -- resolved Just with controlled recipe argv
                [str(just), "work::plan", str(notes)],
                cwd=REPO_ROOT,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Plan: Local workflow harness prototype", result.stdout)
            self.assertFalse(marker.exists(), "Just recipe executed metacharacter data")

    def test_board_explorer_recipes_run_through_real_just_without_mutation(self) -> None:
        """The public facade reaches every read view using only fake gh reads."""
        just = shutil.which("just")
        self.assertIsNotNone(just, "the registered harness gate requires just")
        with tempfile.TemporaryDirectory(prefix="ra8-board-just-") as raw:
            root = Path(raw)
            fake_gh = root / "gh"
            fake_gh.write_text(
                "#!/usr/bin/env python3\n"
                + BOARD_GH_FIXTURE.read_text(encoding="ascii"),
                encoding="ascii",
            )
            fake_gh.chmod(0o755)
            log = root / "gh.jsonl"
            environment = os.environ.copy()
            environment["PATH"] = f"{root}{os.pathsep}{environment['PATH']}"
            environment["FAKE_BOARD_FIXTURES"] = str(FIXTURE.parent)
            environment["FAKE_BOARD_LOG"] = str(log)
            cases = (
                (("work::board_focus",), "=== Board Focus:"),
                (("work::board_quick_wins",), "=== Board Quick-win Candidates:"),
                (("work::board_track", "Codebase"), "=== Board Track: Codebase"),
                (("work::board_epic", "Release"), "=== Board Epic: Release"),
                (("work::board_issue", "13"), "=== Board Issue: #13"),
            )
            for arguments, expected in cases:
                with self.subTest(arguments=arguments):
                    result = subprocess.run(  # noqa: S603 -- resolved Just and fixed recipes
                        [str(just), *arguments],
                        cwd=REPO_ROOT,
                        env=environment,
                        text=True,
                        capture_output=True,
                        check=False,
                        timeout=30,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn(expected, result.stdout)
            calls = [json.loads(line) for line in log.read_text(encoding="ascii").splitlines()]
            self.assertEqual(len(calls), 6)
            self.assertTrue(all(call[0] in {"project", "issue"} for call in calls))


if __name__ == "__main__":
    unittest.main(verbosity=2)
