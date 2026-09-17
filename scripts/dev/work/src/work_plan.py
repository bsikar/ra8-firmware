# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Notes-to-plan parser: a strict Markdown schema, validated and topologically ordered.

The input is a plain Markdown file a person wrote while thinking, and it is
treated as UNTRUSTED DATA throughout. Nothing here evaluates anything, expands
anything, or interprets HTML; a heading is matched by a regular expression and
its text is carried as an opaque string all the way to the emitted commands,
where :func:`shlex.quote` puts it inside one shell word.

The schema is deliberately strict and every violation is collected rather than
raised at the first one, because a person fixing notes wants the whole list.
An unrecognised metadata bullet is a hard error naming its line: a silently
ignored bullet is how a plan comes to claim a priority nobody ever set.

Ordering is Kahn topological sort with a lexicographic tie-break, so the same
notes always produce byte-identical output. That determinism is the property
that lets the JSON be diffed, and a cycle is reported as one concrete path
rather than as a set of unsatisfied keys.

Nothing here runs a command, touches the network, or writes outside a path the
caller named.
"""

from __future__ import annotations

import heapq
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

from work_git import WorkError
from work_tracker import tracker_schema
from work_workspace import KEY_RE

#: Bumped whenever the emitted JSON shape changes incompatibly.
PLAN_SCHEMA_VERSION = 1

#: Metadata bullets recognised under an epic or issue heading.
ITEM_BULLETS = ("labels", "priority", "track", "status", "depends-on", "estimate")

#: Metadata bullets recognised under ``## Config``.
CONFIG_BULLETS = ("statuses", "tracks")

HEADING_RE = re.compile(r"^(?P<hashes>#{1,6})\s+(?P<rest>.*?)\s*$")
PLAN_HEADING_RE = re.compile(r"^Plan:\s*(?P<title>.*)$")
CONFIG_HEADING_RE = re.compile(r"^Config$")
#: The separator is EXACTLY one space, two hyphens, one space -- the form the
#: schema documents. A looser ``\s+--\s+`` accepted ``key   --   title`` and
#: silently produced a different title from the one the author typed, which is
#: the worst outcome available: not an error, just a quietly wrong plan.
EPIC_HEADING_RE = re.compile(r"^Epic: (?P<key>[^\s]+) -- (?P<title>.*)$")
ISSUE_HEADING_RE = re.compile(r"^Issue: (?P<key>[^\s]+) -- (?P<title>.*)$")
BULLET_RE = re.compile(r"^-\s+(?P<name>[A-Za-z][A-Za-z0-9-]*):\s*(?P<value>.*)$")
PRIORITY_RE = re.compile(r"^P[0-3]$")

#: Unicode ranges that can alter a terminal without being printable plan data.
C0_LIMIT = 0x20
DEL = 0x7F
C1_LIMIT = 0xA0

#: Unicode's Bidi_Control property from PropList.txt. These characters can
#: reorder or relabel terminal text without changing the visible data bytes.
BIDI_FORMAT_CONTROLS = frozenset(
    {
        0x061C,
        0x200E,
        0x200F,
        *range(0x202A, 0x202F),
        *range(0x2066, 0x206A),
    }
)

#: ``str.splitlines`` treats these as line boundaries, so they must be refused
#: before the Markdown parser can mistake data for document structure.
UNICODE_LINE_SEPARATORS = frozenset({0x2028, 0x2029})

#: Heading depth each section kind is written at.
LEVEL_PLAN = 1
LEVEL_SECTION = 2
LEVEL_ISSUE = 3

KIND_PLAN = "plan"
KIND_CONFIG = "config"
KIND_EPIC = "epic"
KIND_ISSUE = "issue"


class PlanError(WorkError):
    """One or more notes-schema violations, collected together."""

    def __init__(self, problems: list[str]) -> None:
        """Store the collected problems and build a single-line summary.

        Args:
            problems: Human-readable violations, already carrying line numbers
                where a line is meaningful.
        """
        self.problems = list(problems)
        super().__init__(f"{len(self.problems)} problem(s) in the notes file")


@dataclass
class _Section:
    """One heading and everything that followed it, before validation."""

    kind: str
    key: str
    title: str
    line: int
    body: list[str] = field(default_factory=list)
    meta: list[tuple[int, str, str]] = field(default_factory=list)


@dataclass(frozen=True)
class Node:
    """One validated epic or issue."""

    key: str
    kind: str
    title: str
    epic: str | None
    line: int
    body: str
    labels: tuple[str, ...]
    priority: str | None
    track: str | None
    status: str | None
    depends_on: tuple[str, ...]
    estimate: str | None

    def to_dict(self) -> dict[str, object]:
        """Return the JSON-serialisable form of this node.

        Returns:
            A mapping with every field, using null for absent optional values.
        """
        return {
            "key": self.key,
            "kind": self.kind,
            "title": self.title,
            "epic": self.epic,
            "body": self.body,
            "labels": list(self.labels),
            "priority": self.priority,
            "track": self.track,
            "status": self.status,
            "depends_on": list(self.depends_on),
            "estimate": self.estimate,
        }


@dataclass(frozen=True)
class Plan:
    """A validated, ordered plan."""

    title: str
    github_host: str
    repository: str
    project_owner: str
    project_number: int
    statuses: tuple[str, ...]
    tracks: tuple[str, ...]
    nodes: tuple[Node, ...]
    order: tuple[str, ...]

    def by_key(self) -> dict[str, Node]:
        """Return the nodes indexed by key.

        Returns:
            A mapping from key to node.
        """
        return {node.key: node for node in self.nodes}


def _is_disallowed_control(value: str, *, allow_newline: bool) -> bool:
    """Return whether one character can alter parsing or terminal display."""
    codepoint = ord(value)
    if value == "\n" and allow_newline:
        return False
    return (
        codepoint < C0_LIMIT
        or DEL <= codepoint < C1_LIMIT
        or codepoint in UNICODE_LINE_SEPARATORS
        or codepoint in BIDI_FORMAT_CONTROLS
    )


def _notes_control_problems(text: str) -> list[str]:
    """Locate controls before line-oriented parsing can reinterpret them."""
    problems: list[str] = []
    line = 1
    for value in text:
        if _is_disallowed_control(value, allow_newline=True):
            problems.append(f"line {line}: disallowed control U+{ord(value):04X} in notes")
        if value == "\n":
            line += 1
    return problems


def _plan_control_problems(plan: Plan) -> list[str]:
    """Validate every scalar even when a caller constructed a Plan directly."""
    fields: list[tuple[str, str, bool]] = [
        ("plan title", plan.title, False),
        *(("status", value, False) for value in plan.statuses),
        *(("track", value, False) for value in plan.tracks),
        *(("order key", value, False) for value in plan.order),
    ]
    for node in plan.nodes:
        fields.extend(
            (
                ("node key", node.key, False),
                ("node title", node.title, False),
                ("node body", node.body, True),
                *(("label", value, False) for value in node.labels),
                *(("dependency", value, False) for value in node.depends_on),
            )
        )
        fields.extend(
            (name, value, False)
            for name, value in (
                ("epic", node.epic),
                ("priority", node.priority),
                ("track", node.track),
                ("status", node.status),
                ("estimate", node.estimate),
            )
            if value is not None
        )
    return [
        f"{name} contains disallowed control U+{ord(value):04X}"
        for name, text, allow_newline in fields
        for value in text
        if _is_disallowed_control(value, allow_newline=allow_newline)
    ]


def _require_safe_plan(plan: Plan) -> None:
    """Refuse renderer input that bypassed the notes parser."""
    if problems := _plan_control_problems(plan):
        raise PlanError(problems)


def _make_section(match: re.Match[str], number: int, problems: list[str]) -> _Section | None:
    """Turn one heading line into a section, or record why it is not one.

    Args:
        match: A successful :data:`HEADING_RE` match.
        number: One-based line number, for diagnostics.
        problems: Collector appended to on a malformed heading.

    Returns:
        The new section, or None when the heading was rejected.
    """
    level = len(match.group("hashes"))
    rest = match.group("rest")
    if level == LEVEL_PLAN and (plan := PLAN_HEADING_RE.match(rest)):
        return _Section(KIND_PLAN, "", plan.group("title").strip(), number)
    if level == LEVEL_SECTION and CONFIG_HEADING_RE.match(rest):
        return _Section(KIND_CONFIG, "", "", number)
    if level == LEVEL_SECTION and (epic := EPIC_HEADING_RE.match(rest)):
        return _Section(KIND_EPIC, epic.group("key"), epic.group("title").strip(), number)
    if level == LEVEL_ISSUE and (issue := ISSUE_HEADING_RE.match(rest)):
        return _Section(KIND_ISSUE, issue.group("key"), issue.group("title").strip(), number)
    problems.append(
        f"line {number}: unrecognised heading. Expected one of "
        "'# Plan: <title>', '## Config', '## Epic: <key> -- <title>', "
        "'### Issue: <key> -- <title>'"
    )
    return None


def _absorb(section: _Section, number: int, line: str, problems: list[str]) -> None:
    """Add one non-heading line to the section it belongs to.

    Args:
        section: The section currently being filled.
        number: One-based line number, for diagnostics.
        line: The raw line, newline already stripped.
        problems: Collector appended to when body text follows the metadata.
    """
    if not line.strip():
        if not section.meta:
            section.body.append("")
        return
    bullet = BULLET_RE.match(line)
    if bullet is not None:
        section.meta.append((number, bullet.group("name").lower(), bullet.group("value").strip()))
        return
    if section.meta:
        problems.append(
            f"line {number}: body text is not allowed after the metadata bullets; "
            "move it above the first bullet"
        )
        return
    section.body.append(line.rstrip())


def _scan(text: str) -> tuple[list[_Section], list[str]]:
    """Split the notes into sections without validating any of their contents.

    Args:
        text: The whole notes file.

    Returns:
        The sections in document order, and any structural problems found.
    """
    sections: list[_Section] = []
    problems: list[str] = []
    current: _Section | None = None
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip()
        heading = HEADING_RE.match(line)
        if heading is not None:
            current = _make_section(heading, number, problems)
            if current is not None:
                sections.append(current)
            continue
        if current is None:
            if line.strip():
                problems.append(f"line {number}: text appears before the '# Plan:' header")
            continue
        _absorb(current, number, line, problems)
    return sections, problems


def _split_list(value: str) -> tuple[str, ...]:
    """Split a comma-separated bullet value into stripped, non-empty parts.

    Args:
        value: The raw text after the bullet colon.

    Returns:
        The parts, in order.
    """
    return tuple(part.strip() for part in value.split(",") if part.strip())


def _collect_meta(
    section: _Section, allowed: tuple[str, ...], problems: list[str]
) -> dict[str, str]:
    """Validate a section's bullets against ``allowed`` and return them by name.

    Args:
        section: The section whose metadata block is being read.
        allowed: Bullet names recognised in this kind of section.
        problems: Collector appended to for unknown or duplicated bullets.

    Returns:
        A mapping from bullet name to its raw value.
    """
    found: dict[str, str] = {}
    for number, name, value in section.meta:
        if name not in allowed:
            joined = ", ".join(allowed)
            problems.append(f"line {number}: unknown metadata bullet '{name}'. Allowed: {joined}")
            continue
        if name in found:
            problems.append(f"line {number}: metadata bullet '{name}' is repeated")
            continue
        found[name] = value
    return found


def _config_from(
    sections: list[_Section], problems: list[str]
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Read the optional ``## Config`` section, falling back to the defaults.

    Args:
        sections: Every scanned section.
        problems: Collector appended to when more than one Config appears.

    Returns:
        A two-element tuple of the accepted statuses and the accepted tracks.
    """
    configs = [item for item in sections if item.kind == KIND_CONFIG]
    if len(configs) > 1:
        problems.append(f"line {configs[1].line}: a second '## Config' section is not allowed")
    authority = tracker_schema()
    default_statuses = authority.statuses
    default_tracks = authority.tracks
    if not configs:
        return default_statuses, default_tracks
    meta = _collect_meta(configs[0], CONFIG_BULLETS, problems)
    statuses = _split_list(meta["statuses"]) if "statuses" in meta else default_statuses
    tracks = _split_list(meta["tracks"]) if "tracks" in meta else default_tracks
    return statuses, tracks


def _check_membership(
    node_meta: dict[str, str], line: int, plan_bits: dict[str, tuple[str, ...]], problems: list[str]
) -> None:
    """Validate the priority, status and track values of one item.

    Args:
        node_meta: The item's collected bullets.
        line: The heading line of the item, for diagnostics.
        plan_bits: Mapping with ``statuses`` and ``tracks`` allow-lists.
        problems: Collector appended to for each rejected value.
    """
    priority = node_meta.get("priority")
    if priority is not None and not PRIORITY_RE.match(priority):
        problems.append(f"line {line}: priority '{priority}' must be one of P0, P1, P2, P3")
    status = node_meta.get("status")
    if status is not None and status not in plan_bits["statuses"]:
        allowed = ", ".join(plan_bits["statuses"])
        problems.append(f"line {line}: status '{status}' is not one of: {allowed}")
    track = node_meta.get("track")
    if track is not None and track not in plan_bits["tracks"]:
        allowed = ", ".join(plan_bits["tracks"])
        problems.append(f"line {line}: track '{track}' is not one of: {allowed}")


def _check_required_metadata(
    section: _Section,
    epic: str | None,
    meta: dict[str, str],
    problems: list[str],
) -> None:
    """Require the board fields and mirrored priority/epic labels CLAUDE.md mandates."""
    required = ("status", "track", "priority", "labels")
    problems.extend(
        f"line {section.line}: {section.kind} '{section.key}' requires {name}"
        for name in required
        if not meta.get(name)
    )
    labels = _split_list(meta.get("labels", ""))
    priority_labels = [label for label in labels if label.startswith("priority:")]
    expected_priority = f"priority:{meta.get('priority', '')}"
    if priority_labels != [expected_priority]:
        problems.append(
            f"line {section.line}: labels must contain exactly one "
            f"{expected_priority} matching priority"
        )
    expected_epic = f"epic:{section.key if section.kind == KIND_EPIC else epic}"
    epic_labels = [label for label in labels if label.startswith("epic:")]
    if epic_labels != [expected_epic]:
        problems.append(
            f"line {section.line}: labels must contain exactly one "
            f"{expected_epic} matching ownership"
        )


def _build_node(
    section: _Section, epic: str | None, plan_bits: dict[str, tuple[str, ...]], problems: list[str]
) -> Node:
    """Validate one epic or issue section and build its node.

    Args:
        section: The scanned section.
        epic: The enclosing epic key, or None for an epic itself.
        plan_bits: Mapping with the ``statuses`` and ``tracks`` allow-lists.
        problems: Collector appended to for every violation found.

    Returns:
        The node, built even when problems were recorded so that later checks
        (dependencies, cycles) can still report everything in one pass.
    """
    if not KEY_RE.match(section.key):
        problems.append(
            f"line {section.line}: key '{section.key}' must match "
            "a lowercase slug of 1 to 63 characters, letters, digits and dashes"
        )
    if not section.title:
        problems.append(f"line {section.line}: {section.kind} '{section.key}' has an empty title")
    meta = _collect_meta(section, ITEM_BULLETS, problems)
    _check_membership(meta, section.line, plan_bits, problems)
    _check_required_metadata(section, epic, meta, problems)
    return Node(
        key=section.key,
        kind=section.kind,
        title=section.title,
        epic=epic,
        line=section.line,
        body="\n".join(section.body).strip(),
        labels=_split_list(meta.get("labels", "")),
        priority=meta.get("priority"),
        track=meta.get("track"),
        status=meta.get("status"),
        depends_on=_split_list(meta.get("depends-on", "")),
        estimate=meta.get("estimate"),
    )


def _build_nodes(
    sections: list[_Section], plan_bits: dict[str, tuple[str, ...]], problems: list[str]
) -> list[Node]:
    """Walk the sections in order, attaching issues to the epic above them.

    Args:
        sections: Every scanned section.
        plan_bits: Mapping with the ``statuses`` and ``tracks`` allow-lists.
        problems: Collector appended to for epic-less issues and duplicate keys.

    Returns:
        The nodes in document order.
    """
    nodes: list[Node] = []
    seen: dict[str, int] = {}
    epic: str | None = None
    for section in sections:
        if section.kind not in (KIND_EPIC, KIND_ISSUE):
            continue
        if section.kind == KIND_EPIC:
            epic = section.key
        elif epic is None:
            problems.append(
                f"line {section.line}: issue '{section.key}' has no enclosing "
                "'## Epic:' section above it"
            )
        if section.key in seen:
            problems.append(
                f"line {section.line}: key '{section.key}' is already used at line "
                f"{seen[section.key]}"
            )
        else:
            seen[section.key] = section.line
        nodes.append(
            _build_node(section, None if section.kind == KIND_EPIC else epic, plan_bits, problems)
        )
    return nodes


def _check_dependencies(nodes: list[Node], problems: list[str]) -> None:
    """Validate that every dependency names a real, different key.

    Args:
        nodes: Every built node.
        problems: Collector appended to for unknown and self dependencies.
    """
    known = {node.key for node in nodes}
    for node in nodes:
        for target in node.depends_on:
            if target == node.key:
                problems.append(f"line {node.line}: '{node.key}' depends on itself")
            elif target not in known:
                problems.append(f"line {node.line}: '{node.key}' depends on unknown key '{target}'")


_WHITE = 0
_GREY = 1
_BLACK = 2


def _next_targets(graph: dict[str, tuple[str, ...]], node: str) -> list[str]:
    """Return ``node``'s dependencies ordered so that ``pop()`` yields the smallest.

    Args:
        graph: Mapping from key to the keys it depends on.
        node: The key whose edges are wanted.

    Returns:
        The targets in reverse lexicographic order, to be consumed from the end.
    """
    return sorted(graph.get(node, ()), reverse=True)


def _descend(
    start: str, graph: dict[str, tuple[str, ...]], state: dict[str, int]
) -> list[str] | None:
    """Iteratively depth-first search from ``start`` for the first cycle it closes.

    The traversal carries its own stack rather than using the interpreter's.
    The graph comes from a notes file, so its depth is chosen by whoever wrote
    that file: a 2000-key dependency chain is a perfectly ordinary plan and a
    recursive walk would have raised ``RecursionError`` on it, turning a valid
    input into a crash.

    Args:
        start: The key to descend from.
        graph: Mapping from key to the keys it depends on.
        state: Shared colouring, mutated in place across calls.

    Returns:
        The closing cycle path, or None when this subtree has none.
    """
    path: list[str] = []
    pending: list[tuple[str, list[str]]] = [(start, _next_targets(graph, start))]
    state[start] = _GREY
    path.append(start)
    while pending:
        node, remaining = pending[-1]
        if not remaining:
            state[node] = _BLACK
            pending.pop()
            path.pop()
            continue
        target = remaining.pop()
        if target not in graph:
            continue
        colour = state.get(target, _WHITE)
        if colour == _GREY:
            return [*path[path.index(target) :], target]
        if colour == _WHITE:
            state[target] = _GREY
            path.append(target)
            pending.append((target, _next_targets(graph, target)))
    return None


def find_cycle(graph: dict[str, tuple[str, ...]]) -> list[str] | None:
    """Return one concrete dependency cycle, or None when the graph is acyclic.

    Args:
        graph: Mapping from key to the keys it depends on.

    Returns:
        A path whose first and last element are the same key, or None.
    """
    state: dict[str, int] = {}
    for key in sorted(graph):
        if state.get(key, _WHITE) == _WHITE:
            found = _descend(key, graph, state)
            if found is not None:
                return found
    return None


def topological_order(nodes: list[Node], problems: list[str]) -> tuple[str, ...]:
    """Order the nodes so every dependency precedes its dependants.

    Kahn with a min-heap: among the keys whose dependencies are all satisfied,
    the lexicographically smallest is always taken next, so the order is a
    function of the notes alone and not of dictionary iteration.

    Args:
        nodes: Every built node.
        problems: Collector appended to with one concrete cycle path when the
            graph does not fully drain.

    Returns:
        The ordered keys, empty when a cycle was reported.
    """
    known = {node.key for node in nodes}
    graph = {
        node.key: tuple(
            dict.fromkeys(
                [
                    *(target for target in node.depends_on if target in known),
                    *([node.epic] if node.epic in known else []),
                ]
            )
        )
        for node in nodes
    }
    remaining = {key: len(set(deps)) for key, deps in graph.items()}
    dependants: dict[str, list[str]] = {key: [] for key in graph}
    for key, deps in graph.items():
        for target in set(deps):
            dependants[target].append(key)
    ready = [key for key, count in remaining.items() if count == 0]
    heapq.heapify(ready)
    order: list[str] = []
    while ready:
        key = heapq.heappop(ready)
        order.append(key)
        for follower in sorted(dependants[key]):
            remaining[follower] -= 1
            if remaining[follower] == 0:
                heapq.heappush(ready, follower)
    if len(order) != len(graph):
        cycle = find_cycle({k: v for k, v in graph.items() if remaining[k] > 0})
        path = " -> ".join(cycle) if cycle else "unresolved"
        problems.append(f"dependency cycle: {path}")
        return ()
    return tuple(order)


def parse_plan(text: str) -> Plan:
    """Parse and validate a notes file into an ordered plan.

    Args:
        text: The whole notes file, as read from disk.

    Returns:
        The validated plan.

    Raises:
        PlanError: One or more schema violations. Every problem found is
            carried, not just the first.
    """
    if control_problems := _notes_control_problems(text):
        raise PlanError(control_problems)
    sections, problems = _scan(text)
    plans = [item for item in sections if item.kind == KIND_PLAN]
    if not plans:
        problems.append("missing required '# Plan: <title>' header")
    elif len(plans) > 1:
        problems.append(f"line {plans[1].line}: a second '# Plan:' header is not allowed")
    if plans and not plans[0].title:
        problems.append(f"line {plans[0].line}: the plan header has an empty title")
    authority = tracker_schema()
    statuses, tracks = _config_from(sections, problems)
    plan_bits = {"statuses": statuses, "tracks": tracks}
    nodes = _build_nodes(sections, plan_bits, problems)
    if not nodes:
        problems.append("plan must contain at least one epic or issue")
    _check_dependencies(nodes, problems)
    order = topological_order(nodes, problems)
    if problems:
        raise PlanError(problems)
    return Plan(
        title=plans[0].title,
        github_host=authority.github_host,
        repository=authority.repository,
        project_owner=authority.project_owner,
        project_number=authority.project_number,
        statuses=statuses,
        tracks=tracks,
        nodes=tuple(nodes),
        order=order,
    )


def load_plan(path: Path) -> Plan:
    """Read a notes file from disk and parse it.

    Args:
        path: The notes file.

    Returns:
        The validated plan.

    Raises:
        PlanError: The file could not be read, or violates the schema.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        problems = [f"could not be read: {exc}"]
        raise PlanError(problems) from exc
    except UnicodeDecodeError as exc:
        problems = [
            f"is not valid UTF-8 text: byte {exc.object[exc.start]:#04x} at offset "
            f"{exc.start} could not be decoded ({exc.reason})"
        ]
        raise PlanError(problems) from exc
    return parse_plan(text)


def plan_to_json(plan: Plan) -> str:
    """Render the plan as deterministic JSON.

    No timestamp, no host name, no path: the same notes always produce the same
    bytes, which is what makes the output diffable and testable.

    Args:
        plan: The validated plan.

    Returns:
        JSON text with sorted object keys and a trailing newline.
    """
    _require_safe_plan(plan)
    payload = {
        "schema_version": PLAN_SCHEMA_VERSION,
        "title": plan.title,
        "config": {
            "github_host": plan.github_host,
            "repository": plan.repository,
            "project_owner": plan.project_owner,
            "project_number": plan.project_number,
            "statuses": list(plan.statuses),
            "tracks": list(plan.tracks),
        },
        "nodes": [node.to_dict() for node in plan.nodes],
        "order": list(plan.order),
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def render_summary(plan: Plan) -> str:
    """Render the human-readable summary of a plan.

    Args:
        plan: The validated plan.

    Returns:
        Multi-line text ending in a newline.
    """
    _require_safe_plan(plan)
    index = plan.by_key()
    epics = sum(1 for node in plan.nodes if node.kind == KIND_EPIC)
    issues = len(plan.nodes) - epics
    lines = [
        f"Plan: {plan.title}",
        f"  epics {epics}   issues {issues}   ordered {len(plan.order)}",
        f"  statuses: {', '.join(plan.statuses)}",
        f"  tracks:   {', '.join(plan.tracks)}",
        "",
        "Order:",
    ]
    for position, key in enumerate(plan.order, start=1):
        node = index[key]
        lines.append(f"  {position:3d}. [{node.kind}] {node.key} -- {node.title}")
        if node.depends_on:
            lines.append(f"       depends-on: {', '.join(node.depends_on)}")
    return "\n".join(lines) + "\n"
