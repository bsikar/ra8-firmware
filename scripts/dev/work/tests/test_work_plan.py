# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Notes-schema tests: what the parser accepts, what it refuses, and how it quotes.

Every rejection case asserts on the MESSAGE as well as the failure, because a
parser that refuses everything for the same unhelpful reason passes a test that
only checks the exit path. The quoting tests go further and round-trip the
emitted script back through :mod:`shlex`, which is the only assertion that
actually proves a hostile title stayed inside one shell word.
"""

from __future__ import annotations

import contextlib
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "fixtures"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from work_emit import issue_commands, render_commands  # needs the sys.path lines above
from work_git import printable  # needs the sys.path lines above
from work_plan import (  # needs the sys.path lines above
    Plan,
    PlanError,
    find_cycle,
    load_plan,
    parse_plan,
    plan_to_json,
    render_summary,
)
from work_testlib import (  # needs the sys.path lines above
    FIXTURES_DIR,
    portable_prerequisite,
)

#: Long enough to blow the default recursion limit if the walk were recursive.
DEEP_CHAIN = 2000
WORK_CLI = Path(__file__).resolve().parents[1] / "src/work.py"
TEST_C0_LIMIT = 0x20
TEST_DEL = 0x7F
TEST_C1_LIMIT = 0xA0
TEST_BIDI_FORMAT_CONTROLS = frozenset(
    {
        0x061C,
        0x200E,
        0x200F,
        *range(0x202A, 0x202F),
        *range(0x2066, 0x206A),
    }
)
TEST_UNICODE_LINE_SEPARATORS = frozenset({0x2028, 0x2029})


def notes(name: str) -> str:
    """Read one notes fixture.

    Args:
        name: File name inside the fixtures directory.

    Returns:
        The file contents.
    """
    return (FIXTURES_DIR / name).read_text(encoding="ascii")


def problems_of(name: str) -> list[str]:
    """Parse a fixture that must fail, and return the collected problems.

    Args:
        name: File name inside the fixtures directory.

    Returns:
        The problem strings.

    Raises:
        AssertionError: The fixture parsed cleanly, so it is not testing what
            it claims to test.
    """
    try:
        parse_plan(notes(name))
    except PlanError as exc:
        return exc.problems
    message = f"{name} was expected to fail validation and did not"
    raise AssertionError(message)


class ParseValidNotes(unittest.TestCase):
    """The accepted direction: a well-formed file parses to the expected plan."""

    def setUp(self) -> None:
        """Parse the shared valid fixture once per test."""
        self.plan: Plan = parse_plan(notes("valid_notes.md"))

    def test_title_and_counts(self) -> None:
        """The plan header, the epic, and every issue are all recovered."""
        self.assertEqual(self.plan.title, "Local workflow harness prototype")
        kinds = [node.kind for node in self.plan.nodes]
        self.assertEqual(kinds.count("epic"), 1)
        self.assertEqual(kinds.count("issue"), 3)

    def test_metadata_is_attached_to_the_right_node(self) -> None:
        """Labels, priority and dependencies land on the node they followed."""
        index = self.plan.by_key()
        self.assertEqual(
            index["harness-core"].labels,
            ("priority:P2", "epic:harness-core", "area:scripts"),
        )
        self.assertEqual(index["harness-start"].priority, "P1")
        self.assertEqual(index["harness-doc"].depends_on, ("harness-parser", "harness-start"))
        self.assertEqual(index["harness-start"].epic, "harness-core")

    def test_body_text_is_kept_and_bullets_are_not(self) -> None:
        """Freeform text above the bullets becomes the body; the bullets do not."""
        body = self.plan.by_key()["harness-core"].body
        self.assertIn("umbrella for the prototype", body)
        self.assertNotIn("- priority:", body)

    def test_topological_order_breaks_ties_lexicographically(self) -> None:
        """Among ready keys the smallest is always taken, so order is stable."""
        self.assertEqual(
            self.plan.order,
            ("harness-core", "harness-parser", "harness-start", "harness-doc"),
        )

    def test_summary_lists_every_ordered_key(self) -> None:
        """The human summary names each key and its dependencies."""
        text = render_summary(self.plan)
        for key in self.plan.order:
            self.assertIn(key, text)
        self.assertIn("depends-on: harness-parser, harness-start", text)


class DeterministicOutput(unittest.TestCase):
    """The same notes must always produce the same bytes."""

    def test_json_is_byte_identical_across_runs(self) -> None:
        """Two independent parses of one file serialise identically."""
        first = plan_to_json(parse_plan(notes("valid_notes.md")))
        second = plan_to_json(parse_plan(notes("valid_notes.md")))
        self.assertEqual(first, second)

    def test_json_carries_no_timestamp_or_host_detail(self) -> None:
        """Determinism is a property of the payload, not of luck in timing."""
        payload = json.loads(plan_to_json(parse_plan(notes("valid_notes.md"))))
        self.assertEqual(set(payload), {"schema_version", "title", "config", "nodes", "order"})
        self.assertNotIn("created", json.dumps(payload))

    def test_config_section_overrides_the_defaults(self) -> None:
        """A board is renamed by its owners, so the allowed sets are overridable."""
        plan = parse_plan(notes("custom_config.md"))
        self.assertEqual(plan.statuses, ("Sketch", "Building", "Done"))
        self.assertEqual(plan.tracks, ("Alpha", "Beta"))
        self.assertEqual(plan.by_key()["cfg-epic"].status, "Building")


class ControlCharacterSafety(unittest.TestCase):
    """Untrusted plan data cannot carry terminal controls into any artifact."""

    def setUp(self) -> None:
        """Parse one clean plan used by positive and direct-construction cases."""
        self.plan = parse_plan(notes("valid_notes.md"))

    def test_c0_del_and_c1_controls_are_rejected_before_parsing(self) -> None:
        """Every terminal-control range fires while structural LF remains legal."""
        for value in ("\x00", "\x09", "\x1b", "\x1f", "\x7f", "\x85", "\x9f"):
            with self.subTest(codepoint=f"U+{ord(value):04X}"):
                text = notes("valid_notes.md").replace(
                    "Local workflow harness prototype",
                    f"safe{value}spoof",
                    1,
                )
                with self.assertRaises(PlanError) as caught:
                    parse_plan(text)
                self.assertIn(f"U+{ord(value):04X}", " ".join(caught.exception.problems))

    def test_clean_unicode_and_structural_newlines_reach_all_artifacts(self) -> None:
        """The guard is not an ASCII ban and does not remove document structure."""
        safe_unicode = "caf\u00e9 \u05e9\u05dc\u05d5\u05dd \u0627\u0644\u0633\u0644\u0627\u0645"
        plan = parse_plan(notes("valid_notes.md").replace("prototype", safe_unicode, 1))
        artifacts = (plan_to_json(plan), render_summary(plan), render_commands(plan))
        for artifact in artifacts:
            self.assertIn("\n", artifact)
            controls = [
                value
                for value in artifact
                if value != "\n"
                and (
                    ord(value) < TEST_C0_LIMIT
                    or TEST_DEL <= ord(value) < TEST_C1_LIMIT
                    or ord(value) in TEST_BIDI_FORMAT_CONTROLS
                    or ord(value) in TEST_UNICODE_LINE_SEPARATORS
                )
            ]
            self.assertEqual(controls, [])

    def test_unicode_separators_and_bidi_controls_are_rejected_in_raw_notes(self) -> None:
        """Splitlines and terminal directionality cannot reinterpret notes data."""
        codepoints = TEST_UNICODE_LINE_SEPARATORS | TEST_BIDI_FORMAT_CONTROLS
        for codepoint in sorted(codepoints):
            with self.subTest(codepoint=f"U+{codepoint:04X}"):
                text = notes("valid_notes.md").replace(
                    "Local workflow harness prototype",
                    f"safe{chr(codepoint)}spoof",
                    1,
                )
                with self.assertRaises(PlanError) as caught:
                    parse_plan(text)
                self.assertIn(f"U+{codepoint:04X}", " ".join(caught.exception.problems))

    def test_renderers_reject_directly_constructed_unsafe_plans(self) -> None:
        """Constructing the frozen dataclasses directly cannot bypass validation."""
        unsafe_node = replace(
            self.plan.nodes[0],
            labels=(*self.plan.nodes[0].labels, "unsafe\x9blabel"),
        )
        cases = (
            (render_summary, replace(self.plan, title="unsafe\x1btitle")),
            (plan_to_json, replace(self.plan, statuses=("unsafe\x00status",))),
            (render_commands, replace(self.plan, nodes=(unsafe_node, *self.plan.nodes[1:]))),
        )
        for renderer, plan in cases:
            with self.subTest(renderer=renderer.__name__):
                with self.assertRaises(PlanError) as caught:
                    renderer(plan)
                self.assertIn("disallowed control", " ".join(caught.exception.problems))

    def test_renderers_reject_direct_unicode_display_controls(self) -> None:
        """Direct dataclass construction cannot bypass Unicode display safety."""
        unsafe_node = replace(
            self.plan.nodes[0],
            labels=(*self.plan.nodes[0].labels, "unsafe\u2066label"),
        )
        cases = (
            (render_summary, replace(self.plan, title="unsafe\u2028title")),
            (plan_to_json, replace(self.plan, statuses=("unsafe\u202estatus",))),
            (render_commands, replace(self.plan, nodes=(unsafe_node, *self.plan.nodes[1:]))),
        )
        for renderer, plan in cases:
            with self.subTest(renderer=renderer.__name__):
                with self.assertRaises(PlanError) as caught:
                    renderer(plan)
                self.assertIn("disallowed control", " ".join(caught.exception.problems))


class PlanOutputModes(unittest.TestCase):
    """One invocation writes at most one stdout artifact."""

    def _run(self, *flags: str) -> subprocess.CompletedProcess[str]:
        """Invoke the offline plan CLI with the shared valid notes fixture."""
        return subprocess.run(  # noqa: S603 -- fixed interpreter and repository script
            [
                "/usr/bin/python3",
                "-I",
                str(WORK_CLI),
                "plan",
                str(FIXTURES_DIR / "valid_notes.md"),
                *flags,
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )

    def test_conflicting_stdout_modes_are_exact_refusals(self) -> None:
        """Summary, command script, and stdout JSON are pairwise exclusive."""
        for flags in (
            ("--summary", "--emit-commands"),
            ("--json", "-", "--summary"),
            ("--json", "-", "--emit-commands"),
        ):
            with self.subTest(flags=flags):
                done = self._run(*flags)
                self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
                self.assertEqual(done.stdout, "")

    def test_json_file_may_accompany_exactly_one_stdout_mode(self) -> None:
        """File JSON is orthogonal, and its notice remains on stderr."""
        with tempfile.TemporaryDirectory() as scratch:
            cases = (
                ((), ""),
                (("--summary",), "Plan:"),
                (("--emit-commands",), "#!/bin/sh"),
            )
            for stdout_mode, prefix in cases:
                with self.subTest(stdout_mode=stdout_mode):
                    target = Path(scratch) / f"plan-{len(stdout_mode)}-{prefix[:1] or 'none'}.json"
                    done = self._run("--json", str(target), *stdout_mode)
                    self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
                    if prefix:
                        self.assertTrue(done.stdout.startswith(prefix), done.stdout)
                    else:
                        self.assertEqual(done.stdout, "")
                    self.assertNotIn("wrote ", done.stdout)
                    self.assertIn("wrote ", done.stderr)
                    payload = json.loads(target.read_text(encoding="ascii"))
                    self.assertEqual(payload["schema_version"], 1)
        stdout_json = self._run("--json", "-")
        self.assertEqual(stdout_json.returncode, 0, stdout_json.stderr)
        self.assertEqual(json.loads(stdout_json.stdout)["schema_version"], 1)

    def test_json_file_replaces_only_regular_destinations(self) -> None:
        """Atomic output preserves nonregular targets without opening them."""
        if not portable_prerequisite(
            available=hasattr(os, "mkfifo"), reason="mkfifo is unavailable on this platform"
        ):
            self.skipTest("mkfifo is unavailable on this platform")
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            fifo = root / "output.fifo"
            os.mkfifo(fifo)
            fifo_result = self._run("--json", str(fifo))
            self.assertEqual(fifo_result.returncode, 2, fifo_result.stdout + fifo_result.stderr)
            self.assertIn("not a regular file", fifo_result.stderr)
            self.assertTrue(stat.S_ISFIFO(fifo.lstat().st_mode))

            directory = root / "output-dir"
            directory.mkdir()
            directory_result = self._run("--json", str(directory))
            self.assertEqual(
                directory_result.returncode,
                2,
                directory_result.stdout + directory_result.stderr,
            )
            self.assertIn("not a regular file", directory_result.stderr)
            self.assertTrue(directory.is_dir())

            referent = root / "referent.json"
            referent.write_text("unchanged", encoding="ascii")
            symlink = root / "output-link.json"
            symlink.symlink_to(referent)
            symlink_result = self._run("--json", str(symlink))
            self.assertEqual(
                symlink_result.returncode,
                2,
                symlink_result.stdout + symlink_result.stderr,
            )
            self.assertTrue(symlink.is_symlink())
            self.assertEqual(referent.read_text(encoding="ascii"), "unchanged")

            regular = root / "output.json"
            regular.write_text("old", encoding="ascii")
            regular_result = self._run("--json", str(regular))
            self.assertEqual(
                regular_result.returncode,
                0,
                regular_result.stdout + regular_result.stderr,
            )
            self.assertTrue(stat.S_ISREG(regular.lstat().st_mode))
            self.assertEqual(json.loads(regular.read_text(encoding="ascii"))["schema_version"], 1)


class RejectMalformedNotes(unittest.TestCase):
    """The refused direction, one fixture per defect class."""

    def test_missing_plan_header(self) -> None:
        """A file with no plan header is refused by name."""
        found = problems_of("missing_header.md")
        self.assertIn("missing required '# Plan: <title>' header", found)

    def test_unknown_metadata_bullet_names_its_line(self) -> None:
        """An unrecognised bullet is a hard error carrying its line number."""
        found = problems_of("unknown_bullet.md")
        self.assertTrue(any("unknown metadata bullet 'owner'" in item for item in found), found)
        self.assertTrue(any(item.startswith("line 5:") for item in found), found)

    def test_bad_key_charset_is_refused(self) -> None:
        """Upper case, underscores and path-shaped keys are all rejected."""
        found = " ".join(problems_of("bad_key.md"))
        self.assertIn("key 'Bad_Key'", found)
        self.assertIn("key '../escape'", found)

    def test_duplicate_keys_point_at_the_first_use(self) -> None:
        """A repeated key names the line it was already used on."""
        found = " ".join(problems_of("duplicate_keys.md"))
        self.assertIn("key 'same-key' is already used at line 3", found)

    def test_body_after_metadata_is_refused(self) -> None:
        """Prose below the bullets is a structure error, not silently dropped."""
        found = " ".join(problems_of("body_after_meta.md"))
        self.assertIn("body text is not allowed after the metadata bullets", found)

    def test_membership_violations_are_all_reported_together(self) -> None:
        """Priority, status and track are checked in one pass, not one at a time."""
        found = " ".join(problems_of("bad_membership.md"))
        self.assertIn("priority 'P9'", found)
        self.assertIn("status 'Not A Status'", found)
        self.assertIn("track 'Not A Track'", found)

    def test_empty_plan_is_refused(self) -> None:
        """A header-only document cannot make the compiler and gate vacuously pass."""
        with self.assertRaises(PlanError) as caught:
            parse_plan("# Plan: Empty\n")
        self.assertIn("must contain at least one", " ".join(caught.exception.problems))

    def test_required_board_metadata_and_mirrored_labels(self) -> None:
        """Every emitted issue carries the board fields and matching labels."""
        text = "# Plan: Missing\n\n## Epic: e -- E\n- priority: P1\n"
        with self.assertRaises(PlanError) as caught:
            parse_plan(text)
        found = " ".join(caught.exception.problems)
        self.assertIn("requires status", found)
        self.assertIn("requires track", found)
        self.assertIn("priority:P1", found)
        self.assertIn("epic:e", found)


class DependencyValidation(unittest.TestCase):
    """Dependency edges, and the three ways they go wrong."""

    def test_unknown_dependency_target(self) -> None:
        """A dependency on a key nobody declared is refused."""
        found = " ".join(problems_of("missing_dependency.md"))
        self.assertIn("depends on unknown key 'nowhere-at-all'", found)

    def test_self_dependency(self) -> None:
        """A key cannot depend on itself."""
        found = " ".join(problems_of("self_dependency.md"))
        self.assertIn("'self-issue' depends on itself", found)

    def test_three_node_cycle_reports_a_concrete_path(self) -> None:
        """A cycle is reported as a walkable path, not as a set of stuck keys."""
        found = [item for item in problems_of("cycle_notes.md") if "cycle" in item]
        self.assertEqual(len(found), 1, found)
        path = found[0].split("cycle: ", 1)[1].split(" -> ")
        self.assertEqual(path[0], path[-1])
        self.assertEqual(set(path), {"cyc-a", "cyc-b", "cyc-c"})

    def test_enclosing_epic_is_an_implicit_ordering_edge(self) -> None:
        """A lexically earlier child is never emitted before its epic."""
        text = (
            "# Plan: Ownership\n\n## Epic: z-epic -- Epic\n"
            "- labels: priority:P1, epic:z-epic\n- priority: P1\n"
            "- track: Codebase\n- status: Ready\n\n"
            "### Issue: a-child -- Child\n"
            "- labels: priority:P1, epic:z-epic\n- priority: P1\n"
            "- track: Codebase\n- status: Ready\n"
        )
        self.assertEqual(parse_plan(text).order, ("z-epic", "a-child"))


class HeadingGrammar(unittest.TestCase):
    """The heading separator is exactly one form, and near-misses are errors."""

    def test_multiple_spaces_around_the_separator_are_rejected(self) -> None:
        """``key   --   title`` used to parse and yield a different title silently."""
        text = "# Plan: Spacing\n\n## Epic: spaced   --   Title with padding\n- priority: P1\n"
        with self.assertRaises(PlanError) as caught:
            parse_plan(text)
        problems = getattr(caught.exception, "problems", [])
        self.assertTrue(any("line 3:" in item for item in problems), problems)
        self.assertTrue(any("unrecognised heading" in item for item in problems), problems)

    def test_multiple_spaces_are_rejected_for_issues_too(self) -> None:
        """Both heading forms carry the same rule."""
        text = (
            "# Plan: Spacing\n\n## Epic: ok -- Fine\n- priority: P1\n\n"
            "### Issue: spaced   --   Padded\n- priority: P1\n"
        )
        with self.assertRaises(PlanError) as caught:
            parse_plan(text)
        problems = getattr(caught.exception, "problems", [])
        self.assertTrue(any("line 6:" in item for item in problems), problems)

    def test_the_documented_single_space_form_still_parses(self) -> None:
        """The quiet direction: the form the schema documents is accepted."""
        text = (
            "# Plan: Spacing\n\n## Epic: fine -- One space either side\n"
            "- labels: priority:P1, epic:fine\n- priority: P1\n"
            "- track: Codebase\n- status: Ready\n"
        )
        plan = parse_plan(text)
        self.assertEqual(plan.by_key()["fine"].title, "One space either side")


class DeepGraphs(unittest.TestCase):
    """Graph depth comes from the notes, so it may not be bounded by the C stack."""

    def test_a_long_acyclic_chain_does_not_exhaust_the_stack(self) -> None:
        """A 2000-key chain is an ordinary plan; a recursive walk crashed on it."""
        graph = {f"k{index:04d}": (f"k{index + 1:04d}",) for index in range(DEEP_CHAIN - 1)}
        graph[f"k{DEEP_CHAIN - 1:04d}"] = ()
        self.assertEqual(find_cycle(graph), None)

    def test_a_long_cycle_is_still_reported_as_a_path(self) -> None:
        """Depth must not cost the diagnostic either."""
        graph = {
            f"k{index:04d}": (f"k{(index + 1) % DEEP_CHAIN:04d}",) for index in range(DEEP_CHAIN)
        }
        found = find_cycle(graph)
        self.assertTrue(found is not None)
        self.assertEqual(found[0], found[-1])

    def test_the_smallest_ready_branch_is_explored_first(self) -> None:
        """Determinism survived the switch from recursion to an explicit stack."""
        graph = {"a": ("c", "b"), "b": ("a",), "c": ("a",)}
        self.assertEqual(find_cycle(graph), ["a", "b", "a"])


class OutputHygiene(unittest.TestCase):
    """The report filter keeps content and drops only what can move a cursor."""

    def test_escapes_and_carriage_returns_are_replaced(self) -> None:
        """Everything outside printable ASCII goes, so no byte can repaint a line."""
        cleaned = printable("head\x1b[2K\rSPOOFED\x07")
        self.assertNotIn("\x1b", cleaned)
        self.assertNotIn("\r", cleaned)
        self.assertNotIn("\x07", cleaned)
        self.assertIn("SPOOFED", cleaned)

    def test_newlines_cannot_inject_a_second_report_row(self) -> None:
        """Each report call is one line, including metadata-controlled values."""
        self.assertEqual(printable("one\ntwo"), "one?two")

    def test_ordinary_report_text_is_untouched(self) -> None:
        """A filter that mangled normal output would be its own defect."""
        text = "  READY        keeper   work/keeper   /home/user/ra8-ws/work-keeper"
        self.assertEqual(printable(text), text)


class UndecodableNotes(unittest.TestCase):
    """A notes file that is not text fails like every other bad file."""

    def test_a_non_utf8_byte_is_a_normal_diagnostic(self) -> None:
        """It used to escape as a raw traceback, bypassing the batched report."""
        with tempfile.TemporaryDirectory() as scratch:
            notes = Path(scratch) / "broken.md"
            notes.write_bytes(b"# Plan: Fine\n\n## Epic: e -- T\n- priority: \xffP1\n")
            with self.assertRaises(PlanError) as caught:
                load_plan(notes)
        problems = getattr(caught.exception, "problems", [])
        self.assertEqual(len(problems), 1)
        self.assertIn("is not valid UTF-8 text", problems[0])
        self.assertIn("offset", problems[0])


class EmittedCommandSafety(unittest.TestCase):
    """The emitted script is data for a human, and it must quote like one."""

    def setUp(self) -> None:
        """Parse and render the hostile fixture once per test."""
        self.plan = parse_plan(notes("injection_notes.md"))
        self.script = render_commands(self.plan)

    def _stage_run_root(
        self, root: Path, jq: str, workdir: str, home: str | None
    ) -> tuple[Path, Path, dict[str, str]]:
        """Materialize the offline fake gh, the script, and the child environment."""
        (root / "home").mkdir(exist_ok=True)
        cwd = root / workdir if workdir else root
        cwd.mkdir(parents=True, exist_ok=True)
        fake = root / "gh"
        fake.write_text(
            f'#!/bin/sh\nexec /usr/bin/python3 -I "{FIXTURES_DIR / "fake_board_gh.py"}" "$@"\n',
            encoding="ascii",
        )
        fake.chmod(0o755)
        (root / "plan.sh").write_text(self.script, encoding="ascii")
        log = root / "gh.jsonl"
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{root}:{Path(jq).parent}:/usr/bin:/bin",
                "HOME": str(root / "home") if home is None else home,
                "FAKE_GH_LOG": str(log),
                "FAKE_GH_STATE": str(root / "counter"),
            }
        )
        return cwd, log, environment

    def run_script(
        self,
        overrides: dict[str, str] | None = None,
        *,
        shared: Path | None = None,
        workdir: str = "",
        home: str | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], list[list[str]]]:
        """Run the script against an offline fake gh and capture any mutations.

        Args:
            overrides: Extra environment for the fake gh fixture.
            shared: A run root reused across calls, so a second call sees the
                first call's HOME, ledger and issue counter. None gives this
                call a private one.
            workdir: Subdirectory of that root to execute from, so a caller can
                prove the rerun guard does not depend on the working directory.
            home: The exact HOME to hand the script, so a caller can drive a
                relative or absent one. None uses the run root's own HOME.

        Returns:
            The completed process and every captured issue-create argv.
        """
        jq = shutil.which("jq")
        if not portable_prerequisite(
            available=jq is not None,
            reason="jq is required to execute the emitted-script fixture",
        ):
            self.skipTest("jq is required to execute the emitted-script fixture")
        with contextlib.ExitStack() as stack:
            if shared is None:
                root = Path(stack.enter_context(tempfile.TemporaryDirectory()))
            else:
                root = shared
            cwd, log, environment = self._stage_run_root(root, jq, workdir, home)
            environment.update(overrides or {})
            shell = shutil.which("sh")
            if not portable_prerequisite(
                available=shell is not None,
                reason="sh is required to execute the emitted-script fixture",
            ):
                self.skipTest("sh is required to execute the emitted-script fixture")
            done = subprocess.run(  # noqa: S603 -- generated script, isolated fake gh
                [shell, str(root / "plan.sh")],
                cwd=cwd,
                capture_output=True,
                text=True,
                env=environment,
                timeout=20,
                check=False,
            )
            created = []
            if log.exists():
                created = [
                    json.loads(line) for line in log.read_text(encoding="ascii").splitlines()
                ]
            return done, created

    def execute_script(self) -> list[list[str]]:
        """Run the happy path and return exact issue-create argument vectors."""
        done, created = self.run_script()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        return created

    def test_argv_round_trips_through_shlex(self) -> None:
        """Every emitted word survives quoting and splitting unchanged."""
        for command in issue_commands(self.plan):
            quoted = " ".join(shlex.quote(word) for word in command.argv)
            self.assertEqual(shlex.split(quoted), list(command.argv))

    def test_rendered_script_splits_back_to_the_original_titles(self) -> None:
        """A POSIX shell would hand gh exactly the titles the notes contained."""
        recovered = self.execute_script()
        self.assertEqual(len(recovered), len(self.plan.order))
        titles = [words[words.index("--title") + 1] for words in recovered]
        self.assertIn(
            "Title with $(rm -rf /) and `backquotes`; and a semicolon",
            titles,
        )
        self.assertIn(
            "Another $(rm -rf /) title with 'single' and \"double\" quotes",
            titles,
        )

    def test_multi_line_body_stays_one_argument(self) -> None:
        """A body spanning lines is one word, newline and all."""
        recovered = self.execute_script()
        bodies = [words[words.index("--body") + 1] for words in recovered]
        spanning = [body for body in bodies if "\n" in body]
        self.assertTrue(spanning, bodies)
        self.assertTrue(any("A body that spans\ntwo lines" in body for body in spanning))

    def test_labels_with_metacharacters_stay_single_words(self) -> None:
        """A label carrying a substitution and a semicolon is still one word."""
        recovered = self.execute_script()
        labels = [
            words[position + 1]
            for words in recovered
            for position, word in enumerate(words)
            if word == "--label"
        ]
        self.assertIn("area:$(rm -rf /)", labels)
        self.assertIn("needs;review", labels)

    def test_cross_reference_lines_are_embedded_in_the_body(self) -> None:
        """Dependencies appear textually so a reader can follow them."""
        recovered = self.execute_script()
        bodies = "\n".join(words[words.index("--body") + 1] for words in recovered)
        self.assertIn("Depends-on: #1", bodies)
        self.assertIn("Epic: #1", bodies)
        self.assertIn("Plan-key: inj-epic", bodies)

    def test_all_discovery_precedes_the_first_mutation(self) -> None:
        """Repository, project, fields, options and labels are preflighted first."""
        first_create = self.script.index("gh issue create")
        for token in (
            "REPO_META",
            "viewerCanUpdate",
            "require_field",
            "require_option",
            "repos/$TARGET_REPO/labels?per_page=100",
        ):
            self.assertLess(self.script.index(token), first_create)

    def test_host_repository_and_project_are_exact_pinned_data(self) -> None:
        """Neither environment nor gh defaults can redirect the mutation target."""
        self.assertIn("GH_HOST=github.com", self.script)
        self.assertIn("TARGET_REPO=bsikar/ra8-firmware", self.script)
        self.assertIn("PROJECT_OWNER=bsikar", self.script)
        self.assertIn("PROJECT_NUMBER=5", self.script)
        self.assertIn('--hostname "$GH_HOST"', self.script)

    def test_discovery_queries_are_paginated_and_unique(self) -> None:
        """Paginated JSON streams cannot hide a duplicate or break preflight."""
        self.assertGreaterEqual(self.script.count("--paginate"), 2)
        self.assertIn("must resolve exactly once", self.script)
        self.assertIn("| jq -s --arg n ", self.script)
        done, created = self.run_script()
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(created), len(self.plan.order))

    def test_ambiguous_or_missing_discovery_refuses_every_mutation(self) -> None:
        """Every discovery family fails closed before the first issue is created."""
        cases = (
            {"FAKE_GH_REPOSITORY": "someone/else"},
            {"FAKE_GH_ISSUES": "0"},
            {"FAKE_GH_REPO_WRITE": "0"},
            {"FAKE_GH_MISSING_PROJECT": "1"},
            {"FAKE_GH_PROJECT_NUMBER": "6"},
            {"FAKE_GH_PROJECT_UPDATE": "0"},
            {"FAKE_GH_DUP_FIELD": "Status"},
            {"FAKE_GH_DUP_OPTION": "Ready"},
            {"FAKE_GH_DUP_LABEL": "needs;review"},
            {"FAKE_GH_MISSING_LABEL": "needs;review"},
        )
        for environment in cases:
            with self.subTest(environment=environment):
                done, created = self.run_script(environment)
                self.assertNotEqual(done.returncode, 0, done.stdout + done.stderr)
                self.assertEqual(created, [])

    def test_board_ids_are_discovered_by_name_not_hardcoded(self) -> None:
        """No node id is baked in; every one is looked up from a name."""
        self.assertNotIn("PVT_", self.script)
        self.assertNotIn("PVTSSF_", self.script)
        self.assertIn("projectV2(number:", self.script)
        self.assertIn("field_id", self.script)
        self.assertIn("option_id", self.script)

    def test_script_says_loudly_that_it_was_not_run(self) -> None:
        """The reader is told what this is before they see a single command."""
        header = self.script.split("set -eu", 1)[0]
        self.assertIn("REVIEW BEFORE RUNNING", header)

    def test_partial_failure_prints_a_deterministic_recovery_ledger(self) -> None:
        """Every completed and uncertain mutation remains visible without deletion."""
        done, created = self.run_script({"FAKE_GH_FAIL_MUTATION": "2"})
        self.assertNotEqual(done.returncode, 0)
        self.assertEqual(len(created), 1)
        self.assertIn("GitHub mutation recovery ledger:", done.stderr)
        self.assertIn("BEGIN\tissue\tinj-epic", done.stderr)
        self.assertIn(
            "issue\tinj-epic\thttps://github.com/bsikar/ra8-firmware/issues/1",
            done.stderr,
        )
        self.assertIn("BEGIN\titem\tinj-epic", done.stderr)
        self.assertIn("Do not rerun blindly and do not auto-delete", done.stderr)

    def test_a_first_run_is_not_mistaken_for_a_rerun(self) -> None:
        """Must stay quiet: a plan with no ledger yet creates every issue once.

        Also pins the modes. The ledger is the only thing standing between an
        operator and a duplicated board, so a world-writable one is a defect,
        not a detail -- and nothing else in the suite would notice.
        """
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            done, created = self.run_script(shared=root, workdir="first")
            ledger_root = root / "home" / ".ra8-work-recovery"
            plan_dir = next(ledger_root.iterdir())
            self.assertEqual(ledger_root.stat().st_mode & 0o777, 0o700)
            self.assertEqual(plan_dir.stat().st_mode & 0o777, 0o700)
            self.assertEqual((plan_dir / "ledger.tsv").stat().st_mode & 0o777, 0o600)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(created), len(self.plan.order))

    def test_an_existing_ledger_root_is_reused_rather_than_refused(self) -> None:
        """Must stay quiet: only the per-plan directory is the rerun signal.

        The mode of a root the script did not create belongs to the operator,
        so this also proves the script never silently re-modes one.
        """
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            ledger_root = root / "home" / ".ra8-work-recovery"
            ledger_root.mkdir(parents=True)
            ledger_root.chmod(0o755)
            done, created = self.run_script(shared=root, workdir="first")
            self.assertEqual(ledger_root.stat().st_mode & 0o777, 0o755)
            plan_dir = next(ledger_root.iterdir())
            self.assertEqual(plan_dir.stat().st_mode & 0o777, 0o700)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertEqual(len(created), len(self.plan.order))

    def test_an_empty_home_is_refused_with_the_same_code_as_a_bad_one(self) -> None:
        """Must fire: an untrustworthy anchor always exits 2, in every shell."""
        with tempfile.TemporaryDirectory() as scratch:
            done, created = self.run_script(shared=Path(scratch), workdir="first", home="")
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("HOME must be set", done.stderr)
        self.assertEqual(created, [])

    def test_a_ledger_root_that_is_not_a_directory_is_refused_by_name(self) -> None:
        """Must fire: the refusal is the script's own, not a raw mkdir error."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / "home").mkdir()
            (root / "home" / ".ra8-work-recovery").write_text("", encoding="ascii")
            done, created = self.run_script(shared=root, workdir="first")
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("root exists and is not a directory", done.stderr)
        self.assertEqual(created, [])

    def test_a_rerun_from_any_directory_refuses_to_duplicate_issues(self) -> None:
        """Must fire: the ledger anchors to the operator, never to the cwd.

        A $PWD-relative ledger passed the same-directory case and silently
        created a second complete set of issues from anywhere else, so both
        directories have to be asserted rather than just the obvious one.
        """
        with tempfile.TemporaryDirectory() as scratch:
            shared = Path(scratch)
            first, created = self.run_script(shared=shared, workdir="first")
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertEqual(len(created), len(self.plan.order))
            for workdir in ("first", "elsewhere/deeper"):
                with self.subTest(workdir=workdir):
                    again, after = self.run_script(shared=shared, workdir=workdir)
                    self.assertNotEqual(again.returncode, 0, again.stdout + again.stderr)
                    self.assertIn("already has a recovery ledger", again.stderr)
                    self.assertEqual(len(after), len(self.plan.order))

    def test_a_relative_home_is_refused_before_any_mutation(self) -> None:
        """Must fire: a relative HOME would re-anchor the ledger to the cwd."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / "first" / "relative-home").mkdir(parents=True)
            done, created = self.run_script(shared=root, workdir="first", home="relative-home")
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("HOME must be an absolute path", done.stderr)
        self.assertEqual(created, [])

    def test_a_home_that_does_not_exist_is_refused_not_fabricated(self) -> None:
        """Must fire: a mistyped HOME must not mint a fresh empty ledger root."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            absent = root / "absent" / "deeper"
            done, created = self.run_script(shared=root, workdir="first", home=str(absent))
            self.assertFalse((root / "absent").exists())
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("does not name an existing directory", done.stderr)
        self.assertEqual(created, [])

    def test_a_symlinked_ledger_root_is_refused_and_never_followed(self) -> None:
        """Must fire: the root is never chmodded or written through a symlink."""
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch)
            (root / "home").mkdir()
            victim = root / "victim"
            victim.mkdir(mode=0o755)
            (root / "home" / ".ra8-work-recovery").symlink_to(victim)
            before = victim.stat().st_mode
            done, created = self.run_script(shared=root, workdir="first")
            self.assertEqual(victim.stat().st_mode, before)
            self.assertEqual(list(victim.iterdir()), [])
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("recovery ledger root is a symlink", done.stderr)
        self.assertEqual(created, [])

    def test_the_ledger_path_is_independent_of_the_working_directory(self) -> None:
        """The emitted anchor names HOME, and the script never mentions $PWD."""
        self.assertIn('RECOVERY_ROOT="$HOME/.ra8-work-recovery"', self.script)
        self.assertIn('RECOVERY_DIR="$RECOVERY_ROOT/$PLAN_ID"', self.script)
        self.assertNotIn("$PWD", self.script)

    def test_the_root_is_rechecked_after_it_is_created(self) -> None:
        """The race-closing re-check and both modes are asserted structurally.

        Deleting the post-create re-check, or widening either mkdir mode, is
        invisible to every behavioural test here: the good path still works and
        no reachable input distinguishes them. Only an assertion on the emitted
        text notices, so this makes those lines load-bearing.
        """
        lines = self.script.splitlines()
        create = lines.index('[ -d "$RECOVERY_ROOT" ] || mkdir -m 700 "$RECOVERY_ROOT"')
        recheck = next(
            position
            for position, line in enumerate(lines)
            if position > create
            and line.startswith('[ ! -L "$RECOVERY_ROOT" ] && [ -d "$RECOVERY_ROOT" ]')
        )
        self.assertGreater(recheck, create)
        self.assertIn('mkdir -m 700 "$RECOVERY_DIR"', lines)
        self.assertNotIn("chmod", self.script)


if __name__ == "__main__":
    unittest.main()
