# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Strict contracts for the read-only terminal board explorer."""

from __future__ import annotations

import io
import json
import os
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

SRC = Path(__file__).resolve().parents[1] / "src"
FIXTURES = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(SRC))

import work_board as board  # noqa: E402 -- import needs source path above
import work_tracker as tracker_module  # noqa: E402 -- same local import setup
from work_git import Completed, WorkError  # noqa: E402 -- same local import setup
from work_tracker import TrackerSchema  # noqa: E402 -- same local import setup

block = vars(board)["_block"]
effort = vars(board)["_effort"]
emit = vars(board)["_emit"]
epic = vars(board)["_epic"]
explorer_command = vars(board)["_explorer_command"]
explorer_main = vars(board)["_explorer_main"]
fetch_snapshot = vars(board)["_fetch_snapshot"]
issue_command = vars(board)["_issue_command"]
parse_detail = vars(board)["_parse_detail"]
parse_snapshot = vars(board)["_parse_snapshot"]
priority = vars(board)["_priority"]
render_detail = vars(board)["_render_detail"]
render_filter = vars(board)["_render_filter"]
render_focus = vars(board)["_render_focus"]
render_quick_wins = vars(board)["_render_quick_wins"]
run_gh_readonly = vars(board)["_run_gh_readonly"]
sort_key = vars(board)["_sort_key"]
item_status = vars(board)["_status"]
strict_json = vars(board)["_strict_json"]
item_track = vars(board)["_track"]
truncate = vars(board)["_truncate"]
terminal_width = vars(board)["_width"]
string_list = vars(tracker_module)["_string_list"]


def schema() -> TrackerSchema:
    """Return the repository tracker shape without reading global state."""
    return TrackerSchema(
        github_host="github.com",
        repository="bsikar/ra8-firmware",
        project_owner="bsikar",
        project_number=5,
        statuses=("Needs you", "Ready", "In flight", "Bench-blocked", "Landed"),
        tracks=("C6 wireless", "Bench + infra", "CI health", "Codebase", "Product"),
    )


def fixture(name: str) -> str:
    """Load an ASCII JSON fixture exactly as GitHub would emit it."""
    return (FIXTURES / name).read_text(encoding="ascii")


def snapshot() -> tuple[board.BoardItem, ...]:
    """Parse the canonical explorer fixture."""
    return parse_snapshot(fixture("board_snapshot.json"), schema())


class StrictSnapshotParser(unittest.TestCase):
    """Reject ambiguous or lossy project snapshots."""

    def test_valid_snapshot_builds_exact_model(self) -> None:
        """Verify valid snapshot builds exact model."""
        items = snapshot()
        self.assertEqual(len(items), 6)
        self.assertEqual(items[0].number, 11)
        self.assertEqual(items[0].content_type, "Issue")

    def test_duplicate_json_key_is_rejected(self) -> None:
        """Verify duplicate json key is rejected."""
        with self.assertRaises(board.DuplicateJsonKeyError) as raised:
            strict_json('{"items": [], "items": []}')
        self.assertEqual(str(raised.exception), "duplicate JSON key: items")

    def test_nonstandard_json_number_is_rejected(self) -> None:
        """Verify nonstandard json number is rejected."""
        with self.assertRaisesRegex(board.BoardDataError, "numeric constant"):
            strict_json('{"totalCount": NaN, "items": []}')

    def test_oversized_json_integer_is_normalized_as_board_data_error(self) -> None:
        """Verify oversized decoder integers do not leak a ValueError traceback."""
        payload = '{"totalCount": ' + ("9" * 5000) + ', "items": []}'
        with self.assertRaisesRegex(board.BoardDataError, "invalid GitHub JSON"):
            strict_json(payload)

    def test_deep_snapshot_json_is_normalized_as_board_data_error(self) -> None:
        """Verify recursive snapshot JSON does not leak a RecursionError traceback."""
        with (
            patch.object(board.json, "loads", side_effect=RecursionError("depth")),
            self.assertRaisesRegex(board.BoardDataError, "invalid GitHub JSON"),
        ):
            parse_snapshot("{}", schema())

    def test_root_must_be_object(self) -> None:
        """Verify root must be object."""
        with self.assertRaisesRegex(board.BoardDataError, "must be an object"):
            parse_snapshot("[]", schema())

    def test_items_must_be_list(self) -> None:
        """Verify items must be list."""
        with self.assertRaisesRegex(board.BoardDataError, "items must be a list"):
            parse_snapshot('{"totalCount": 0, "items": {}}', schema())

    def test_total_count_rejects_boolean(self) -> None:
        """Verify total count rejects boolean."""
        with self.assertRaisesRegex(board.BoardDataError, "totalCount"):
            parse_snapshot('{"totalCount": true, "items": []}', schema())

    def test_total_count_must_equal_length(self) -> None:
        """Verify total count must equal length."""
        with self.assertRaisesRegex(board.BoardDataError, "equal items length"):
            parse_snapshot('{"totalCount": 1, "items": []}', schema())

    def test_non_issue_item_is_rejected(self) -> None:
        """Verify non issue item is rejected."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["type"] = "PullRequest"
        with self.assertRaisesRegex(board.BoardDataError, "must be Issue"):
            parse_snapshot(json.dumps(payload), schema())

    def test_conflicting_nested_content_type_is_rejected(self) -> None:
        """Verify nested non-issue content cannot hide behind an Issue item type."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["content"]["type"] = "PullRequest"
        with self.assertRaisesRegex(board.BoardDataError, "must be Issue"):
            parse_snapshot(json.dumps(payload), schema())

    def test_null_nested_content_type_is_rejected(self) -> None:
        """Verify a present null nested content type is not treated as absent."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["content"]["type"] = None
        with self.assertRaisesRegex(board.BoardDataError, "type must be a string"):
            parse_snapshot(json.dumps(payload), schema())

    def test_null_top_level_item_type_is_rejected(self) -> None:
        """Verify a present null top-level item type is not treated as absent."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["type"] = None
        with self.assertRaisesRegex(board.BoardDataError, "type must be a string"):
            parse_snapshot(json.dumps(payload), schema())

    def test_nested_content_key_collision_is_rejected(self) -> None:
        """Verify nested content keys cannot collide case-insensitively."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["content"]["Type"] = "PullRequest"
        with self.assertRaisesRegex(board.BoardDataError, "collide case-insensitively"):
            parse_snapshot(json.dumps(payload), schema())

    def test_foreign_repository_is_rejected(self) -> None:
        """Verify foreign repository is rejected."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["content"]["repository"] = "someone/else"
        with self.assertRaisesRegex(board.BoardDataError, "unexpected repository"):
            parse_snapshot(json.dumps(payload), schema())

    def test_live_repository_url_is_canonical_identity(self) -> None:
        """Verify GitHub's live URL form names the configured repository."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["repository"] = "https://github.com/bsikar/ra8-firmware"
        item = parse_snapshot(json.dumps(payload), schema())[0]
        self.assertEqual(item.repository, "bsikar/ra8-firmware")

    def test_repository_identity_rejects_unsafe_or_ambiguous_forms(self) -> None:
        """Verify repository identity accepts no URL decoration or partial object."""
        unsafe: tuple[object, ...] = (
            " bsikar/ra8-firmware",
            "bsikar/ra8-firmware ",
            "https://github.com/bsikar/ra8-firmware/",
            "http://github.com/bsikar/ra8-firmware",
            "https://github.com/bsikar/ra8-firmware?redirect=1",
            "https://github.com/bsikar/ra8-firmware#fragment",
            "https://user@github.com/bsikar/ra8-firmware",
            "https://github.com.evil/bsikar/ra8-firmware",
            "https://github.com/bsikar/../ra8-firmware",
            {"name": "ra8-firmware"},
            {"nameWithOwner": "bsikar/ra8-firmware"},
        )
        for repository in unsafe:
            with self.subTest(repository=repository):
                payload = json.loads(fixture("board_snapshot.json"))
                payload["items"][0]["repository"] = repository
                with self.assertRaises(board.BoardDataError):
                    parse_snapshot(json.dumps(payload), schema())

    def test_conflicting_top_level_repository_is_rejected(self) -> None:
        """Verify top-level repository metadata cannot contradict content."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["repository"] = "someone/else"
        with self.assertRaisesRegex(board.BoardDataError, "unexpected repository"):
            parse_snapshot(json.dumps(payload), schema())

    def test_duplicate_issue_number_is_rejected(self) -> None:
        """Verify duplicate issue number is rejected."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][1]["content"]["number"] = 11
        with self.assertRaisesRegex(board.BoardDataError, "duplicate issue numbers"):
            parse_snapshot(json.dumps(payload), schema())

    def test_blank_top_level_title_falls_back_to_content(self) -> None:
        """Verify blank top level title falls back to content."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["title"] = " \t"
        items = parse_snapshot(json.dumps(payload), schema())
        self.assertEqual(items[0].title, "Owner decision")

    def test_non_string_top_level_title_is_rejected(self) -> None:
        """Verify non string top level title is rejected."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["title"] = 42
        with self.assertRaisesRegex(board.BoardDataError, "title must be a string"):
            parse_snapshot(json.dumps(payload), schema())

    def test_casefold_key_collision_is_rejected(self) -> None:
        """Verify casefold key collision is rejected."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["Status"] = "Ready"
        with self.assertRaisesRegex(board.BoardDataError, "collide case-insensitively"):
            parse_snapshot(json.dumps(payload), schema())

    def test_labels_are_casefold_deduplicated_and_sorted(self) -> None:
        """Verify labels are casefold deduplicated and sorted."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["labels"] = ["Zulu", "alpha", "ALPHA"]
        item = parse_snapshot(json.dumps(payload), schema())[0]
        self.assertEqual(item.labels, ("alpha", "Zulu"))

    def test_null_labels_are_empty(self) -> None:
        """Verify null labels are empty."""
        self.assertEqual(snapshot()[-1].labels, ())

    def test_nonmatching_derived_label_is_ordinary_label(self) -> None:
        """Verify nonmatching derived label is ordinary label."""
        payload = json.loads(fixture("board_snapshot.json"))
        payload["items"][0]["labels"] = ["effort:   "]
        item = parse_snapshot(json.dumps(payload), schema())[0]
        self.assertEqual(item.effort_labels, ())
        self.assertEqual(item.labels, ("effort:",))


class TrackerSchemaCasefold(unittest.TestCase):
    """Keep user-facing selector resolution unambiguous."""

    def test_status_casefold_duplicate_is_rejected(self) -> None:
        """Verify status casefold duplicate is rejected."""
        with self.assertRaisesRegex(WorkError, "case-insensitive duplicate"):
            string_list({"statuses": ["Ready", "ready"]}, "statuses")

    def test_track_casefold_duplicate_is_rejected(self) -> None:
        """Verify track casefold duplicate is rejected."""
        with self.assertRaisesRegex(WorkError, "case-insensitive duplicate"):
            string_list({"tracks": ["Codebase", "CODEBASE"]}, "tracks")


class EvidenceAndSelection(unittest.TestCase):
    """Render explicit metadata evidence without inventing policy."""

    def test_priority_field_is_recognized(self) -> None:
        """Verify priority field is recognized."""
        self.assertEqual(priority(snapshot()[0]), "P1")

    def test_priority_conflict_is_visible(self) -> None:
        """Verify priority conflict is visible."""
        self.assertEqual(priority(snapshot()[2]), "P0/P2!")

    def test_effort_label_is_visible(self) -> None:
        """Verify effort label is visible."""
        self.assertEqual(effort(snapshot()[0]), "S")

    def test_epic_labels_are_comma_separated(self) -> None:
        """Verify epic labels are comma separated."""
        item = replace(snapshot()[0], epic_labels=("One", "Two"))
        self.assertEqual(epic(item), "One, Two")

    def test_fixed_block_labels_are_visible(self) -> None:
        """Verify fixed block labels are visible."""
        item = replace(snapshot()[0], labels=("needs-purchase", "unrelated"))
        self.assertEqual(block(item), "owner/purchase!")

    def test_bench_block_label_is_visible(self) -> None:
        """Verify the configured bench blocker label is rendered explicitly."""
        item = replace(snapshot()[0], status="Ready", labels=("needs-bench",))
        self.assertEqual(block(item), "bench")

    def test_sort_uses_best_priority_then_conflict_then_number(self) -> None:
        """Verify sort uses best priority then conflict then number."""
        base = snapshot()[0]
        plain = replace(base, number=2, project_priority="P0", priority_labels=())
        conflict = replace(base, number=1, project_priority="P2", priority_labels=("P0",))
        low = replace(base, number=3, project_priority="P1", priority_labels=())
        self.assertEqual(
            [item.number for item in sorted((low, conflict, plain), key=sort_key)],
            [2, 1, 3],
        )

    def test_status_matches_config_case_insensitively(self) -> None:
        """Verify status matches config case insensitively."""
        item = replace(snapshot()[0], status="ready")
        self.assertEqual(item_status(item, schema()), "Ready")

    def test_track_matches_config_case_insensitively(self) -> None:
        """Verify track matches config case insensitively."""
        item = replace(snapshot()[0], track="codebase")
        self.assertEqual(item_track(item, schema()), "Codebase")


class RenderingContract(unittest.TestCase):
    """Keep explorer reports deterministic, safe, and width bounded."""

    def test_terminal_width_is_clamped_low(self) -> None:
        """Verify terminal width is clamped low."""
        with patch.object(
            board.shutil, "get_terminal_size", return_value=os.terminal_size((20, 24))
        ):
            self.assertEqual(terminal_width(), 60)

    def test_terminal_width_is_clamped_high(self) -> None:
        """Verify terminal width is clamped high."""
        with patch.object(
            board.shutil, "get_terminal_size", return_value=os.terminal_size((300, 24))
        ):
            self.assertEqual(terminal_width(), 160)

    def test_truncation_reserves_three_dots(self) -> None:
        """Verify truncation reserves three dots."""
        self.assertEqual(truncate("abcdefgh", 6), "abc...")

    def test_focus_uses_horizon_rule_and_four_sections(self) -> None:
        """Verify focus uses horizon rule and four sections."""
        rendered = "\n".join(render_focus(schema(), snapshot()))
        self.assertIn("Ready candidate rule: Status=Ready and Horizon=Now.", rendered)
        self.assertEqual(rendered.count("] ("), 4)
        self.assertIn("[ BENCH-BLOCKED ] (1)", rendered)

    def test_focus_falls_back_to_explicit_p0(self) -> None:
        """Verify focus falls back to explicit p0."""
        items = tuple(replace(item, horizon=None) for item in snapshot())
        rendered = "\n".join(render_focus(schema(), items))
        self.assertIn("Horizon is unavailable", rendered)
        self.assertIn("[ READY CANDIDATES ] (1)", rendered)

    def test_quick_wins_split_on_explicit_blocker(self) -> None:
        """Verify quick wins split on explicit blocker."""
        rendered = "\n".join(render_quick_wins(schema(), snapshot()))
        self.assertIn("Candidates: 2", rendered)
        self.assertIn("[ NO EXPLICIT BOARD BLOCKER ] (1)", rendered)
        self.assertIn("[ EXPLICIT BOARD BLOCKER ] (1)", rendered)

    def test_empty_track_filter_has_no_sections(self) -> None:
        """Verify empty track filter has no sections."""
        lines, errors = render_filter(schema(), snapshot(), kind="Track", selector="Product")
        self.assertEqual(errors, [])
        self.assertIn("(no open issues matched)", lines or [])
        self.assertFalse(any(line.startswith("[ ") for line in lines or []))

    def test_epic_no_match_reports_full_available_labels(self) -> None:
        """Verify epic no match reports full available labels."""
        lines, errors = render_filter(schema(), snapshot(), kind="Epic", selector="Missing")
        self.assertIsNone(lines)
        self.assertIn("epic:Harness, epic:Release", errors[1])

    def test_issue_detail_has_all_relation_sections(self) -> None:
        """Verify issue detail has all relation sections."""
        detail = parse_detail(fixture("issue_detail.json"))
        rendered = "\n".join(render_detail(schema(), snapshot(), detail))
        for heading in ("[ PARENT ]", "[ SUB-ISSUES ]", "[ BLOCKED BY ]", "[ BLOCKING ]"):
            self.assertIn(heading, rendered)
        self.assertIn("2 additional relation(s) omitted by GitHub", rendered)

    def test_issue_detail_reports_snapshot_races(self) -> None:
        """Verify issue detail reports snapshot races."""
        detail = replace(
            parse_detail(fixture("issue_detail.json")),
            title="Changed",
            state="CLOSED",
            labels=(),
        )
        rendered = "\n".join(render_detail(schema(), snapshot(), detail))
        self.assertEqual(rendered.count("Notice:"), 3)
        self.assertIn("issue labels changed", rendered)

    def test_hostile_text_is_ascii_and_width_bounded(self) -> None:
        """Verify hostile text is ascii and width bounded."""
        item = replace(snapshot()[0], title="bad\x1b[31m\n" + "x" * 100)
        with patch.object(
            board.shutil, "get_terminal_size", return_value=os.terminal_size((60, 24))
        ):
            output = io.StringIO()
            with patch("sys.stdout", output):
                emit(render_focus(schema(), (item,)))
        self.assertNotIn("\x1b", output.getvalue())
        self.assertTrue(
            all(len(line) <= board.MIN_WIDTH for line in output.getvalue().splitlines())
        )


class DetailParser(unittest.TestCase):
    """Validate native relationship data before display."""

    def test_deep_detail_json_is_normalized_as_board_data_error(self) -> None:
        """Verify recursive detail JSON does not leak a RecursionError traceback."""
        with (
            patch.object(board.json, "loads", side_effect=RecursionError("depth")),
            self.assertRaisesRegex(board.BoardDataError, "invalid GitHub JSON"),
        ):
            parse_detail("{}")

    def test_valid_detail_parses_parent(self) -> None:
        """Verify valid detail parses parent."""
        detail = parse_detail(fixture("issue_detail.json"))
        self.assertEqual(detail.parent.number if detail.parent else None, 3)

    def test_missing_parent_key_is_rejected(self) -> None:
        """Verify missing parent key is rejected."""
        payload = json.loads(fixture("issue_detail.json"))
        del payload["parent"]
        with self.assertRaisesRegex(board.BoardDataError, "missing required fields"):
            parse_detail(json.dumps(payload))

    def test_connection_count_cannot_be_less_than_nodes(self) -> None:
        """Verify connection count cannot be less than nodes."""
        payload = json.loads(fixture("issue_detail.json"))
        payload["subIssues"]["totalCount"] = 0
        with self.assertRaisesRegex(board.BoardDataError, "at least nodes length"):
            parse_detail(json.dumps(payload))

    def test_duplicate_connection_number_is_rejected(self) -> None:
        """Verify duplicate connection number is rejected."""
        payload = json.loads(fixture("issue_detail.json"))
        payload["subIssues"]["nodes"].append(payload["subIssues"]["nodes"][0])
        payload["subIssues"]["totalCount"] = 2
        with self.assertRaisesRegex(board.BoardDataError, "duplicate issue numbers"):
            parse_detail(json.dumps(payload))


class CommandAndCliContract(unittest.TestCase):
    """Allow only fixed read commands and normalize new-view failures."""

    def test_explorer_command_adds_exact_server_query(self) -> None:
        """Verify explorer command adds exact server query."""
        command = explorer_command(schema())
        self.assertEqual(command[-2:], ["--query", "is:issue is:open repo:bsikar/ra8-firmware"])

    def test_issue_command_requests_only_fixed_fields(self) -> None:
        """Verify issue command requests only fixed fields."""
        self.assertEqual(issue_command(schema(), 13)[-1], board.ISSUE_FIELDS)

    def test_readonly_guard_rejects_any_other_command(self) -> None:
        """Verify readonly guard rejects any other command."""
        with self.assertRaisesRegex(WorkError, "unsupported GitHub command"):
            run_gh_readonly(schema(), ["gh", "issue", "close", "13"])

    def test_readonly_guard_pins_host_and_removes_gh_repo(self) -> None:
        """Verify readonly guard pins host and removes gh repo."""
        result = Completed(argv=(), returncode=0, stdout="", stderr="")
        with (
            patch.dict(os.environ, {"GH_REPO": "attacker/repo"}, clear=False),
            patch.object(board, "run_process", return_value=result) as run,
        ):
            run_gh_readonly(schema(), explorer_command(schema()))
        environment = run.call_args.kwargs["env"]
        self.assertEqual(environment["GH_HOST"], "github.com")
        self.assertNotIn("GH_REPO", environment)
        self.assertEqual(run.call_args.kwargs["timeout"], 30)

    def test_missing_view_fails_before_schema_load(self) -> None:
        """Verify the removed legacy default cannot fetch project data."""
        stderr = io.StringIO()
        with patch.object(board, "_load_schema") as load, patch("sys.stderr", stderr):
            self.assertEqual(board.main([]), 2)
        load.assert_not_called()
        self.assertIn("usage: work_board.py", stderr.getvalue())

    def test_empty_track_fails_before_schema_load(self) -> None:
        """Verify empty track fails before schema load."""
        stderr = io.StringIO()
        with patch.object(board, "_load_schema") as load, patch("sys.stderr", stderr):
            code = board.main(["track", " \t"])
        self.assertEqual(code, 2)
        load.assert_not_called()
        self.assertEqual(stderr.getvalue(), "error: track selector must not be empty\n")

    def test_unknown_track_fails_after_schema_but_before_snapshot(self) -> None:
        """Verify unknown track does not make a project API request."""
        with patch.object(board, "_fetch_snapshot") as fetch:
            code = explorer_main(schema(), "track", "Not configured")
        self.assertEqual(code, 2)
        fetch.assert_not_called()

    def test_epic_prefix_is_stripped_once(self) -> None:
        """Verify epic prefix is stripped once."""
        with (
            patch.object(board, "_load_schema", return_value=schema()),
            patch.object(board, "_explorer_main", return_value=0) as run,
        ):
            self.assertEqual(board.main(["epic", " Epic:Release "]), 0)
        run.assert_called_once_with(schema(), "epic", "Release")

    def test_issue_id_rejects_whitespace_and_unicode_digits(self) -> None:
        """Verify issue id rejects whitespace and unicode digits."""
        for value in (" 13", "13 ", "0", "\u0661\u0663"):
            with self.subTest(value=value), patch.object(board, "_load_schema") as load:
                self.assertEqual(board.main(["issue", value]), 2)
                load.assert_not_called()

    def test_absent_issue_never_fetches_detail(self) -> None:
        """Verify absent issue never fetches detail."""
        with (
            patch.object(board, "_fetch_snapshot", return_value=(snapshot(), 0)),
            patch.object(board, "_fetch_detail") as detail,
        ):
            self.assertEqual(explorer_main(schema(), "issue", "99"), 1)
        detail.assert_not_called()

    def test_huge_issue_id_does_not_convert_or_fetch_detail(self) -> None:
        """Verify huge issue id does not convert or fetch detail."""
        selector = "9" * 5000
        stderr = io.StringIO()
        with (
            patch.object(board, "_fetch_snapshot", return_value=(snapshot(), 0)),
            patch.object(board, "_fetch_detail") as detail,
            patch("sys.stderr", stderr),
        ):
            self.assertEqual(explorer_main(schema(), "issue", selector), 1)
        detail.assert_not_called()
        self.assertLess(len(stderr.getvalue()), 200)

    def test_huge_filter_selectors_are_truncated_in_diagnostics(self) -> None:
        """Verify user selectors cannot amplify track or epic error output."""
        selector = "x" * 5000
        stderr = io.StringIO()
        with patch("sys.stderr", stderr):
            self.assertEqual(explorer_main(schema(), "track", selector), 2)
        self.assertLess(len(stderr.getvalue()), 300)

        lines, errors = render_filter(schema(), snapshot(), kind="Epic", selector=selector)
        self.assertIsNone(lines)
        self.assertLess(sum(len(error) for error in errors), 300)

    def test_transport_failure_uses_exit_one_and_empty_stdout(self) -> None:
        """Verify transport failure uses exit one and empty stdout."""
        failed = Completed(argv=(), returncode=7, stdout="", stderr="network failed")
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            patch.object(board, "_run_gh_readonly", return_value=failed),
            patch("sys.stdout", stdout),
            patch("sys.stderr", stderr),
        ):
            items, code = fetch_snapshot(schema())
        self.assertIsNone(items)
        self.assertEqual(code, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("network failed", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
