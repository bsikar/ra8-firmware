# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

"""Read-only terminal views over the repository's GitHub project board."""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import textwrap
from pathlib import Path
from typing import NoReturn

sys.path.insert(0, str(Path(__file__).resolve().parent))
from work_board_models import BoardItem, IssueDetail, Relation, RelationConnection
from work_git import Completed, ToolMissingError, WorkError, printable, run_process
from work_tracker import TrackerSchema, tracker_schema

GH_TIMEOUT_S = 30
NO_STATUS = "No Status"
BOARD_QUERY = "is:issue is:open repo:{repository}"
ISSUE_FIELDS = "number,title,url,state,updatedAt,labels,parent,subIssues,blockedBy,blocking"
ISSUE_ID_RE = re.compile(r"[1-9][0-9]*")
PRIORITY_LABEL_RE = re.compile(r"priority:(P[0-3])", re.IGNORECASE)
PRIORITY_FIELD_RE = re.compile(r"^(P[0-3])(?:$|[ \t])", re.IGNORECASE)
EFFORT_LABEL_RE = re.compile(r"effort:(.+)", re.IGNORECASE)
EPIC_LABEL_RE = re.compile(r"epic:(.+)", re.IGNORECASE)
BLOCK_LABELS = (("bench", "needs-bench"), ("purchase", "needs-purchase"))
MIN_WIDTH = 60
MAX_WIDTH = 160
WIDE_WIDTH = 100
ELLIPSIS_WIDTH = 3
MIN_TITLE_WIDTH = 12
SELECTOR_ARGC = 2
MAX_SELECTOR_DISPLAY = 48


class BoardDataError(ValueError):
    """GitHub returned data outside the explorer's strict contract."""


class DuplicateJsonKeyError(BoardDataError):
    """A JSON object repeated a key."""


def _fail_data(message: str) -> NoReturn:
    """Raise one consistently constructed board-data error."""
    raise BoardDataError(message)


def _fail_duplicate(message: str) -> NoReturn:
    """Raise one duplicate-key error for the JSON decoder."""
    raise DuplicateJsonKeyError(message)


def _load_schema() -> TrackerSchema | None:
    """Return the tracker authority, reporting a load failure."""
    try:
        return tracker_schema()
    except WorkError as exc:
        _error(f"error loading tracker schema: {exc}")
        return None


def _gh_environment() -> dict[str, str]:
    """Pin GitHub reads to the configured public host and explicit repositories."""
    env = dict(os.environ)
    env["GH_HOST"] = "github.com"
    env.pop("GH_REPO", None)
    return env


def _explorer_command(schema: TrackerSchema) -> list[str]:
    return [
        "gh",
        "project",
        "item-list",
        str(schema.project_number),
        "--owner",
        schema.project_owner,
        "--limit",
        "2500",
        "--format",
        "json",
        "--query",
        BOARD_QUERY.format(repository=schema.repository),
    ]


def _issue_command(schema: TrackerSchema, number: int) -> list[str]:
    return [
        "gh",
        "issue",
        "view",
        str(number),
        "--repo",
        schema.repository,
        "--json",
        ISSUE_FIELDS,
    ]


def _run_gh_readonly(
    schema: TrackerSchema, argv: list[str], *, issue_number: int | None = None
) -> Completed:
    """Run only one exact argv shape used by this read-only module."""
    allowed = {_tuple(_explorer_command(schema))}
    if issue_number is not None:
        allowed.add(_tuple(_issue_command(schema, issue_number)))
    if _tuple(argv) not in allowed:
        message = "refusing unsupported GitHub command"
        raise WorkError(message)
    return run_process(argv, timeout=GH_TIMEOUT_S, env=_gh_environment())


def _tuple(argv: list[str]) -> tuple[str, ...]:
    return tuple(argv)


def _command_failure(prefix: str, proc: Completed) -> int:
    message = proc.stderr.strip() or proc.stdout.strip() or f"gh exited {proc.returncode}"
    _error(f"{prefix}: {message}")
    return 1


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            _fail_duplicate(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> NoReturn:
    """Reject the non-standard NaN and infinity tokens accepted by stdlib JSON."""
    _fail_data(f"invalid JSON numeric constant: {value}")


def _strict_json(text: str) -> object:
    try:
        return json.loads(
            text,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_json_constant,
        )
    except BoardDataError:
        raise
    except (RecursionError, ValueError) as exc:
        _fail_data(f"invalid GitHub JSON: {exc}")


def _strict_object(value: object, context: str) -> dict[str, object]:
    if not isinstance(value, dict):
        _fail_data(f"{context} must be an object")
    return value


def _strict_string(value: object, context: str, *, blank: bool = False) -> str:
    if not isinstance(value, str):
        _fail_data(f"{context} must be a string")
    result = value.strip(" \t")
    if not blank and not result:
        _fail_data(f"{context} must not be blank")
    return result


def _strict_positive_int(value: object, context: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        _fail_data(f"{context} must be a positive integer")
    return value


def _optional_scalar(item: dict[str, object], name: str) -> str | None:
    value = item.get(name)
    if value is None:
        return None
    return _strict_string(value, f"project item {name}", blank=True) or None


def _labels(value: object, context: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list):
        _fail_data(f"{context} must be a list or null")
    kept: dict[str, str] = {}
    for raw in value:
        label = _strict_string(raw, f"{context} entry")
        kept.setdefault(label.casefold(), label)
    return tuple(
        sorted(
            kept.values(),
            key=lambda label: (printable(label).casefold(), printable(label)),
        )
    )


def _detail_labels(value: object) -> tuple[str, ...]:
    """Read the label objects emitted by ``gh issue view --json labels``."""
    if not isinstance(value, list):
        _fail_data("issue labels must be a list")
    names: list[str] = []
    for raw in value:
        label = _strict_object(raw, "issue label")
        names.append(_strict_string(label.get("name"), "issue label name"))
    return _labels(names, "issue label names")


def _casefold_collision(item: dict[str, object]) -> None:
    seen: dict[str, str] = {}
    for key in item:
        folded = key.casefold()
        if folded in seen and seen[folded] != key:
            _fail_data(f"project item keys collide case-insensitively: {seen[folded]} and {key}")
        seen[folded] = key


def _repository_name(value: object, schema: TrackerSchema, number: int) -> str:
    """Return the configured identity for one exact GitHub repository spelling."""
    raw = _strict_string(value, "project item repository")
    allowed = (schema.repository, f"https://{schema.github_host}/{schema.repository}")
    if raw != value or raw not in allowed:
        _fail_data(f"project item #{number} belongs to unexpected repository {raw}")
    return schema.repository


def _parse_item(raw: object, schema: TrackerSchema) -> BoardItem:
    item = _strict_object(raw, "project item")
    _casefold_collision(item)
    content = _strict_object(item.get("content"), "project item content")
    _casefold_collision(content)
    type_fields = tuple(
        (context, obj["type"])
        for context, obj in (("project item type", item), ("project item content type", content))
        if "type" in obj
    )
    if not type_fields:
        _fail_data("project item type must be Issue")
    for context, value in type_fields:
        if _strict_string(value, context).casefold() != "issue":
            _fail_data("project item type must be Issue")
    number = _strict_positive_int(content.get("number"), "project item number")
    repository = _repository_name(content.get("repository"), schema, number)
    if "repository" in item:
        _repository_name(item["repository"], schema, number)
    title_value = item.get("title")
    if title_value is None or (isinstance(title_value, str) and not title_value.strip(" \t")):
        title_value = content.get("title")
    title = _strict_string(title_value, f"project item #{number} title")
    url = _strict_string(content.get("url", item.get("url")), f"project item #{number} url")
    labels = _labels(item.get("labels"), f"project item #{number} labels")
    priority_labels = tuple(
        match.group(1).upper()
        for label in labels
        if (match := PRIORITY_LABEL_RE.fullmatch(label)) is not None
    )
    effort_labels = tuple(
        match.group(1).strip(" \t")
        for label in labels
        if (match := EFFORT_LABEL_RE.fullmatch(label)) is not None
    )
    epic_labels = tuple(
        match.group(1).strip(" \t")
        for label in labels
        if (match := EPIC_LABEL_RE.fullmatch(label)) is not None
    )
    if any(not value for value in (*effort_labels, *epic_labels)):
        _fail_data(f"project item #{number} has an empty derived label")
    return BoardItem(
        number=number,
        title=title,
        url=url,
        repository=repository,
        content_type="Issue",
        status=_optional_scalar(item, "status"),
        track=_optional_scalar(item, "track"),
        horizon=_optional_scalar(item, "horizon"),
        project_priority=_optional_scalar(item, "priority"),
        priority_labels=priority_labels,
        effort_labels=effort_labels,
        epic_labels=epic_labels,
        labels=labels,
    )


def _parse_snapshot(text: str, schema: TrackerSchema) -> tuple[BoardItem, ...]:
    root = _strict_object(_strict_json(text), "GitHub project response")
    if "items" not in root or "totalCount" not in root:
        _fail_data("GitHub project response must contain items and totalCount")
    raw_items = root["items"]
    if not isinstance(raw_items, list):
        _fail_data("GitHub project response items must be a list")
    total = root["totalCount"]
    if not isinstance(total, int) or isinstance(total, bool) or total != len(raw_items):
        _fail_data("GitHub project response totalCount must equal items length")
    items = tuple(_parse_item(raw, schema) for raw in raw_items)
    numbers = [item.number for item in items]
    if len(numbers) != len(set(numbers)):
        _fail_data("GitHub project response contains duplicate issue numbers")
    return items


def _fetch_snapshot(schema: TrackerSchema) -> tuple[tuple[BoardItem, ...] | None, int]:
    try:
        proc = _run_gh_readonly(schema, _explorer_command(schema))
    except ToolMissingError:
        _error("error: 'gh' CLI is not installed")
        return None, 1
    except WorkError as exc:
        _error(f"error fetching board snapshot: {exc}")
        return None, 1
    if not proc.ok:
        return None, _command_failure("error fetching board snapshot", proc)
    try:
        return _parse_snapshot(proc.stdout, schema), 0
    except BoardDataError as exc:
        _error(f"error parsing GitHub project data: {exc}")
        return None, 1


def _canonical(value: str | None, choices: tuple[str, ...]) -> str | None:
    if value is None:
        return None
    lookup = {choice.casefold(): choice for choice in choices}
    return lookup.get(value.casefold(), value)


def _status(item: BoardItem, schema: TrackerSchema) -> str | None:
    return _canonical(item.status, schema.statuses)


def _track(item: BoardItem, schema: TrackerSchema) -> str | None:
    return _canonical(item.track, schema.tracks)


def _priority_values(item: BoardItem) -> tuple[str, ...]:
    values = list(item.priority_labels)
    if item.project_priority is not None:
        match = PRIORITY_FIELD_RE.match(item.project_priority)
        if match is not None:
            values.append(match.group(1).upper())
    return tuple(dict.fromkeys(values))


def _evidence(values: tuple[str, ...], *, upper: bool = False) -> str:
    unique = list(dict.fromkeys(value.upper() if upper else value for value in values))
    if not unique:
        return "-"
    return "/".join(unique) + ("!" if len(unique) > 1 else "")


def _priority(item: BoardItem) -> str:
    return _evidence(_priority_values(item), upper=True)


def _effort(item: BoardItem) -> str:
    return _evidence(item.effort_labels)


def _epic(item: BoardItem) -> str:
    if not item.epic_labels:
        return "-"
    return ", ".join(item.epic_labels)


def _block(item: BoardItem) -> str:
    folded = {label.casefold() for label in item.labels}
    markers = ["owner"] if (item.status or "").casefold() == "needs you" else []
    markers.extend(marker for marker, label in BLOCK_LABELS if label in folded)
    return _evidence(tuple(markers))


def _sort_key(item: BoardItem) -> tuple[int, int, int]:
    values = _priority_values(item)
    ranks = [int(value[1]) for value in values]
    return (min(ranks, default=4), 1 if len(values) > 1 else 0, item.number)


def _width() -> int:
    columns = shutil.get_terminal_size(fallback=(120, 24)).columns
    return max(MIN_WIDTH, min(MAX_WIDTH, columns))


def _safe(value: object) -> str:
    return printable(str(value))


def _truncate(value: str, width: int) -> str:
    safe = _safe(value)
    if len(safe) <= width:
        return safe
    if width <= ELLIPSIS_WIDTH:
        return safe[:width]
    return safe[: width - ELLIPSIS_WIDTH] + "..."


def _wrapped(text: str, width: int, *, initial: str = "", subsequent: str = "") -> list[str]:
    safe = _safe(text)
    return textwrap.wrap(
        safe,
        width=width,
        initial_indent=initial,
        subsequent_indent=subsequent,
        break_long_words=True,
        break_on_hyphens=False,
        replace_whitespace=False,
        drop_whitespace=True,
    ) or [initial.rstrip()]


def _emit(lines: list[str], *, stderr: bool = False) -> None:
    stream = sys.stderr if stderr else sys.stdout
    width = _width()
    for logical in lines:
        for physical in _wrapped(logical, width):
            print(physical, file=stream)


def _error(message: str) -> None:
    _emit([message], stderr=True)


def _wide_rows(items: tuple[BoardItem, ...], width: int) -> list[str]:
    id_width = max(4, *(len(f"#{item.number}") for item in items))
    title_width = width - 2 - id_width - 9 - 7 - 14 - 8 - 14 - 18 - 7
    if title_width < MIN_TITLE_WIDTH:
        return _narrow_rows(items, width)
    lines = [
        f"  {'ID':<{id_width}} {'PRI':<9} {'EFF':<7} {'TRACK':<14} "
        f"{'HORIZON':<8} {'BLOCK':<14} {'EPIC':<18} TITLE"
    ]
    for item in items:
        values = (
            f"#{item.number}",
            _priority(item),
            _effort(item),
            item.track or "-",
            item.horizon or "-",
            _block(item),
            _epic(item),
            item.title,
        )
        fields = (
            _truncate(values[0], id_width),
            _truncate(values[1], 9),
            _truncate(values[2], 7),
            _truncate(values[3], 14),
            _truncate(values[4], 8),
            _truncate(values[5], 14),
            _truncate(values[6], 18),
            _truncate(values[7], title_width),
        )
        lines.append(
            f"  {fields[0]:<{id_width}} {fields[1]:<9} {fields[2]:<7} "
            f"{fields[3]:<14} {fields[4]:<8} {fields[5]:<14} "
            f"{fields[6]:<18} {fields[7]}"
        )
    return lines


def _narrow_rows(items: tuple[BoardItem, ...], width: int) -> list[str]:
    lines: list[str] = []
    for item in items:
        lines.extend(
            _wrapped(
                f"#{item.number} {item.title}",
                width,
                initial="  ",
                subsequent="    ",
            )
        )
        metadata = (
            f"PRI={_priority(item)} EFF={_effort(item)} TRACK={item.track or '-'} "
            f"HORIZON={item.horizon or '-'} BLOCK={_block(item)} EPIC={_epic(item)}"
        )
        lines.extend(_wrapped(metadata, width, initial="    ", subsequent="    "))
    return lines


def _section(label: str, items: tuple[BoardItem, ...]) -> list[str]:
    lines = ["", f"[ {_safe(label).upper()} ] ({len(items)})"]
    if not items:
        lines.append("  (empty)")
        return lines
    ordered = tuple(sorted(items, key=_sort_key))
    if _width() >= WIDE_WIDTH:
        lines.extend(_wide_rows(ordered, _width()))
    else:
        lines.extend(_narrow_rows(ordered, _width()))
    return lines


def _status_sections(items: tuple[BoardItem, ...], schema: TrackerSchema) -> list[str]:
    lines: list[str] = []
    consumed: set[int] = set()
    for configured in schema.statuses:
        group = tuple(item for item in items if _status(item, schema) == configured)
        consumed.update(item.number for item in group)
        lines.extend(_section(configured, group))
    no_status = tuple(item for item in items if _status(item, schema) is None)
    if no_status:
        consumed.update(item.number for item in no_status)
        lines.extend(_section(NO_STATUS, no_status))
    unexpected = sorted(
        {
            _status(item, schema)
            for item in items
            if item.number not in consumed and _status(item, schema) is not None
        },
        key=lambda value: (_safe(value).casefold(), _safe(value)),
    )
    for status in unexpected:
        group = tuple(item for item in items if _status(item, schema) == status)
        lines.extend(_section(status, group))
    return lines


def _render_focus(schema: TrackerSchema, items: tuple[BoardItem, ...]) -> list[str]:
    def status(item: BoardItem) -> str:
        return (_status(item, schema) or "").casefold()

    has_horizon = any(item.horizon is not None for item in items)
    if has_horizon:
        ready = tuple(
            item
            for item in items
            if status(item) == "ready" and (item.horizon or "").casefold() == "now"
        )
        rule = "Ready candidate rule: Status=Ready and Horizon=Now."
    else:
        ready = tuple(
            item for item in items if status(item) == "ready" and "P0" in _priority_values(item)
        )
        rule = (
            "Notice: Horizon is unavailable; Ready candidates use explicit P0 field/label evidence."
        )
    owner = tuple(item for item in items if status(item) == "needs you")
    in_flight = tuple(item for item in items if status(item) == "in flight")
    bench = tuple(item for item in items if status(item) == "bench-blocked")
    lines = [
        f"=== Board Focus: {schema.project_owner}/{schema.project_number} ===",
        f"Open project issues scanned: {len(items)}",
        rule,
    ]
    for label, group in (
        ("Needs you", owner),
        ("In flight", in_flight),
        ("Ready candidates", ready),
        ("Bench-blocked", bench),
    ):
        lines.extend(_section(label, group))
    lines.append("")
    return lines


def _render_quick_wins(schema: TrackerSchema, items: tuple[BoardItem, ...]) -> list[str]:
    candidates = tuple(
        item
        for item in items
        if (_status(item, schema) or "").casefold() == "ready"
        and any(value.casefold() == "s" for value in item.effort_labels)
    )
    blocked = tuple(item for item in candidates if _block(item) != "-")
    clear = tuple(item for item in candidates if _block(item) == "-")
    lines = [
        f"=== Board Quick-win Candidates: {schema.project_owner}/{schema.project_number} ===",
        f"Candidates: {len(candidates)}",
        "Rule: Status=Ready and explicit effort:S; verify acceptance and "
        "dependencies before closing.",
    ]
    lines.extend(_section("No Explicit Board Blocker", clear))
    lines.extend(_section("Explicit Board Blocker", blocked))
    lines.append("")
    return lines


def _resolve_selector(selector: str, choices: tuple[str, ...]) -> str | None:
    folded = selector.casefold()
    return next((choice for choice in choices if choice.casefold() == folded), None)


def _render_filter(
    schema: TrackerSchema,
    items: tuple[BoardItem, ...],
    *,
    kind: str,
    selector: str,
) -> tuple[list[str] | None, list[str]]:
    display_selector = _truncate(selector, MAX_SELECTOR_DISPLAY)
    if kind == "Track":
        selected = tuple(item for item in items if _track(item, schema) == selector)
    else:
        selected = tuple(
            item
            for item in items
            if any(value.casefold() == selector.casefold() for value in item.epic_labels)
        )
    if kind == "Epic" and not selected:
        labels = sorted(
            {value for item in items for value in item.epic_labels},
            key=lambda value: (_safe(value).casefold(), _safe(value)),
        )
        available = ", ".join(f"epic:{value}" for value in labels) or "(none)"
        return None, [
            f"error: no open project issues matched epic:{display_selector}",
            f"available epic labels: {available}",
        ]
    lines = [
        f"=== Board {kind}: {display_selector} "
        f"({schema.project_owner}/{schema.project_number}) ===",
        f"Open project issues matched: {len(selected)}",
    ]
    if selected:
        lines.extend(_status_sections(selected, schema))
    else:
        lines.append("(no open issues matched)")
    lines.append("")
    return lines, []


def _parse_relation(value: object, context: str) -> Relation:
    obj = _strict_object(value, context)
    number = _strict_positive_int(obj.get("number"), f"{context} number")
    return Relation(
        number=number,
        title=_strict_string(obj.get("title"), f"{context} title"),
        url=_strict_string(obj.get("url"), f"{context} url"),
        state=_strict_string(obj.get("state"), f"{context} state"),
    )


def _parse_connection(value: object, context: str) -> RelationConnection:
    obj = _strict_object(value, context)
    total = obj.get("totalCount")
    if not isinstance(total, int) or isinstance(total, bool) or total < 0:
        _fail_data(f"{context} totalCount must be a non-negative integer")
    nodes = obj.get("nodes")
    if not isinstance(nodes, list):
        _fail_data(f"{context} nodes must be a list")
    parsed = tuple(_parse_relation(node, f"{context} node") for node in nodes)
    if total < len(parsed):
        _fail_data(f"{context} totalCount must be at least nodes length")
    numbers = [relation.number for relation in parsed]
    if len(numbers) != len(set(numbers)):
        _fail_data(f"{context} contains duplicate issue numbers")
    return RelationConnection(total_count=total, nodes=parsed)


def _parse_detail(text: str) -> IssueDetail:
    payload = _strict_object(_strict_json(text), "GitHub issue response")
    required = {
        "number",
        "title",
        "url",
        "state",
        "updatedAt",
        "labels",
        "parent",
        "subIssues",
        "blockedBy",
        "blocking",
    }
    if not required.issubset(payload):
        _fail_data("GitHub issue response is missing required fields")
    parent_raw = payload["parent"]
    return IssueDetail(
        number=_strict_positive_int(payload["number"], "issue number"),
        title=_strict_string(payload["title"], "issue title"),
        url=_strict_string(payload["url"], "issue url"),
        state=_strict_string(payload["state"], "issue state"),
        updated_at=_strict_string(payload["updatedAt"], "issue updatedAt"),
        labels=_detail_labels(payload["labels"]),
        parent=None if parent_raw is None else _parse_relation(parent_raw, "issue parent"),
        sub_issues=_parse_connection(payload["subIssues"], "issue subIssues"),
        blocked_by=_parse_connection(payload["blockedBy"], "issue blockedBy"),
        blocking=_parse_connection(payload["blocking"], "issue blocking"),
    )


def _fetch_detail(schema: TrackerSchema, number: int) -> tuple[IssueDetail | None, int]:
    command = _issue_command(schema, number)
    try:
        proc = _run_gh_readonly(schema, command, issue_number=number)
    except ToolMissingError:
        _error("error: 'gh' CLI is not installed")
        return None, 1
    except WorkError as exc:
        _error(f"error fetching issue detail: {exc}")
        return None, 1
    if not proc.ok:
        return None, _command_failure("error fetching issue detail", proc)
    try:
        return _parse_detail(proc.stdout), 0
    except BoardDataError as exc:
        _error(f"error parsing GitHub issue data: {exc}")
        return None, 1


def _relation_lines(relation: Relation, width: int) -> list[str]:
    line = f"#{relation.number} [{relation.state}] {relation.title} ({relation.url})"
    return _wrapped(line, width, initial="  ", subsequent="    ")


def _detail_section(label: str, connection: RelationConnection) -> list[str]:
    lines = ["", f"[ {label} ]"]
    if not connection.nodes and connection.total_count == 0:
        lines.append("  (none)")
        return lines
    for relation in connection.nodes:
        lines.extend(_relation_lines(relation, _width()))
    hidden = connection.total_count - len(connection.nodes)
    if hidden:
        lines.append(f"  ({hidden} additional relation(s) omitted by GitHub)")
    return lines


def _render_detail(
    schema: TrackerSchema, snapshot: tuple[BoardItem, ...], detail: IssueDetail
) -> list[str]:
    board_item = next(item for item in snapshot if item.number == detail.number)
    lines = [
        f"=== Board Issue: #{detail.number} ({schema.repository}) ===",
        f"Title: {detail.title}",
        f"State: {detail.state}",
        f"URL: {detail.url}",
        f"Updated: {detail.updated_at}",
        f"Labels: {', '.join(detail.labels) if detail.labels else '-'}",
        f"Project Status: {board_item.status or '-'}",
        f"Track: {board_item.track or '-'}",
        f"Horizon: {board_item.horizon or '-'}",
        f"Priority: {_priority(board_item)}",
        f"Effort: {_effort(board_item)}",
        f"Epic: {_epic(board_item)}",
        f"Board Blocker: {_block(board_item)}",
    ]
    lines.extend(
        _detail_section(
            "PARENT",
            RelationConnection(
                total_count=0 if detail.parent is None else 1,
                nodes=() if detail.parent is None else (detail.parent,),
            ),
        )
    )
    lines.extend(_detail_section("SUB-ISSUES", detail.sub_issues))
    lines.extend(_detail_section("BLOCKED BY", detail.blocked_by))
    lines.extend(_detail_section("BLOCKING", detail.blocking))
    if detail.state.casefold() != "open":
        lines.append("")
        lines.append("Notice: issue state changed after the open-project snapshot was read.")
    if detail.title != board_item.title or detail.url != board_item.url:
        lines.append("")
        lines.append("Notice: issue title or URL changed after the project snapshot was read.")
    detail_labels = {label.casefold() for label in detail.labels}
    snapshot_labels = {label.casefold() for label in board_item.labels}
    if detail_labels != snapshot_labels:
        lines.append("")
        lines.append("Notice: issue labels changed after the project snapshot was read.")
    lines.append("")
    return lines


def _run_issue_view(schema: TrackerSchema, items: tuple[BoardItem, ...], selector: str) -> int:
    """Fetch and render one issue already proven present in the snapshot."""
    board_item = next((item for item in items if str(item.number) == selector), None)
    if board_item is None:
        display_selector = _truncate(selector, MAX_SELECTOR_DISPLAY)
        _error(f"error: issue #{display_selector} is not in the open project snapshot")
        return 1
    number = board_item.number
    detail, code = _fetch_detail(schema, number)
    if detail is None:
        return code
    if detail.number != number:
        _error(f"error: GitHub returned issue #{detail.number} for requested issue #{number}")
        return 1
    _emit(_render_detail(schema, items, detail))
    return 0


def _explorer_main(schema: TrackerSchema, view: str, selector: str | None) -> int:
    resolved_track: str | None = None
    if view == "track":
        track_selector = selector or ""
        resolved_track = _resolve_selector(track_selector, schema.tracks)
        if resolved_track is None:
            choices = ", ".join(f"'{value}'" for value in schema.tracks)
            display_selector = _truncate(track_selector, MAX_SELECTOR_DISPLAY)
            _error(f"error: unknown track '{display_selector}'; configured tracks: {choices}")
            return 2
    items, code = _fetch_snapshot(schema)
    if items is None:
        return code
    if view == "focus":
        lines, errors = _render_focus(schema, items), []
    elif view == "quick-wins":
        lines, errors = _render_quick_wins(schema, items), []
    elif view == "track":
        lines, errors = _render_filter(schema, items, kind="Track", selector=resolved_track or "")
    elif view == "epic":
        lines, errors = _render_filter(schema, items, kind="Epic", selector=selector or "")
    else:
        return _run_issue_view(schema, items, selector or "0")
    if lines is None:
        for error in errors:
            _error(error)
        return 2
    _emit(lines)
    return 0


def _epic_selector(raw: str) -> str | None:
    """Strip space/tab and at most one case-insensitive ``epic:`` prefix."""
    selector = raw.strip(" \t")
    if selector.casefold().startswith("epic:"):
        selector = selector[5:].strip(" \t")
    if not selector:
        _error("error: epic selector must not be empty")
        return None
    return selector


def _parse_view_args(args: list[str]) -> tuple[str, str | None] | None:
    """Validate and normalize one explorer argv shape before loading config."""
    if args in (["focus"], ["quick-wins"]):
        return args[0], None
    if len(args) != SELECTOR_ARGC or args[0] not in {"track", "epic", "issue"}:
        _error("usage: work_board.py [focus|quick-wins|track TRACK|epic EPIC|issue ID]")
        return None
    view, selector = args
    if view == "track" and not selector.strip(" \t"):
        _error("error: track selector must not be empty")
        return None
    if view == "epic":
        selector = _epic_selector(selector)
        if selector is None:
            return None
    if view == "issue" and ISSUE_ID_RE.fullmatch(selector) is None:
        _error("error: issue id must be an ASCII positive integer")
        return None
    return view, selector


def main(argv: list[str] | None = None) -> int:
    """Dispatch one strict read-only board view."""
    args = sys.argv[1:] if argv is None else argv
    parsed = _parse_view_args(args)
    if parsed is None:
        return 2
    schema = _load_schema()
    if schema is None:
        return 2
    view, selector = parsed
    return _explorer_main(schema, view, selector)


if __name__ == "__main__":
    sys.exit(main())
