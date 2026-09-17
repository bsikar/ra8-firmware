#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""world_tag_scope.py -- measure the SCOPE of the TrustZone world-tag requirement.

``check_world_tags.py`` requires ``[Ring N / NAME]`` and ``{World: ...}`` on
every Ring 3+ file, and decides which files those are in
``file_is_in_ring3_plus()``: a hand-written set of path shapes
(``libs/ra8_hal/``, ``libs/ra8_*_pal/``, ``libs/ra8_nsc/``,
``libs/ra8_secure_app/``, ``tests/``, per-app ``src/main.c``). Its own
docstring said the only trees deliberately left out are host tooling under
``tools/`` and vendored code -- "a documented scope decision ... not an
accident of a tuple".

The tree says otherwise. The requirement reaches 7 of 51 first-party roots.
``apps/``, ``port/`` and 38 first-party ``libs/`` roots sit outside it, none of
them documented, and most of them carry Ring/World tags the gate never reads:
delete the tags under ``libs/ra8_io/`` and the gate stays green. Three roots
(``libs/ra8_fs/``, ``libs/ra8_gfx/``, ``libs/ra8_mpu/``) carry no tag at all,
and nothing in the gate can say so.

That is the #842 shape one level up from the legacy inventory: the exemption
LIST is finite and shrinking, while the SCOPE it sits inside was never
measured, so a whole library root is a quieter exemption than any row in it.

This module makes the scope declare itself in
``.github/world-tag-scope-declaration.txt``, one row per first-party root:

    * ``required``  -- every file under the root is inside the requirement
    * ``partial``   -- some are, with a reason saying which
    * ``exempt``    -- none are, deliberately, with the policy reason
    * ``unscoped``  -- none are, and that is debt, with a note

and checks the declaration against the tree on every invocation: a root the
tree has and the file does not is a finding (a new library cannot enter
unjudged), a row the tree no longer has is a finding (the list only shrinks),
and a declared state that disagrees with what ``file_is_in_ring3_plus()``
actually classifies is a finding in EITHER direction, so the classifier and
the declaration cannot drift apart. Declared counts, total and per state, make
a root changing state show up as a changed number rather than a moved line.

Nothing here widens or narrows the requirement itself: which files must carry
tags today is exactly what it was before. The scope is now written down and
measured.
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
from collections.abc import Callable, Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from selftest_assert import expect

DECLARATION_REL = ".github/world-tag-scope-declaration.txt"

DIRECTIVE = "#!"

STATE_REQUIRED = "required"
STATE_PARTIAL = "partial"
STATE_EXEMPT = "exempt"
STATE_UNSCOPED = "unscoped"

STATES = (STATE_REQUIRED, STATE_PARTIAL, STATE_EXEMPT, STATE_UNSCOPED)

#: A root outside the requirement has to say why. An undocumented omission is
#: the defect this module exists to end, so a missing reason is a finding.
STATES_NEEDING_REASON = (STATE_PARTIAL, STATE_EXEMPT, STATE_UNSCOPED)

#: ``exempt`` and ``unscoped`` measure identically (nothing inside the
#: requirement) and differ only in the reason, which no measurement can tell
#: apart, so both satisfy a measured ``unscoped``.
STATES_OUTSIDE = (STATE_EXEMPT, STATE_UNSCOPED)

ROOTS_KEY = "roots"

#: Declared counts: the total plus one per state, so a root moving from
#: ``exempt`` to ``unscoped`` cannot net out to an unchanged total.
COUNT_KEYS = (ROOTS_KEY, *STATES)

TOP_LEVEL_ROOT = "./"

LIBS_ROOT = "libs"

#: ``libs/<name>/<file>``: the shortest path that names a library root.
LIBS_DEPTH = 3


class ScopeDeclarationError(ValueError):
    """A malformed, duplicated or unknown line in the scope declaration."""


def _malformed(problem: str, detail: str) -> ScopeDeclarationError:
    """Build the error for one bad declaration line.

    The message is assembled here rather than at each ``raise`` so every parse
    failure reads the same way and the raise sites stay one line each.
    """
    message = f"{problem}: {detail!r}"
    return ScopeDeclarationError(message)


def root_of(rel_path: str) -> str:
    """The declaration root a repo-relative path belongs to.

    ``libs/`` splits one level deeper than the other trees because each
    library is its own ring and world decision; everything else is judged at
    its top level. A file sitting at the repository root maps to ``./`` so it
    cannot escape a row by having no directory at all.
    """
    parts = rel_path.split("/")
    if len(parts) == 1:
        return TOP_LEVEL_ROOT
    if parts[0] == LIBS_ROOT and len(parts) >= LIBS_DEPTH:
        return f"{LIBS_ROOT}/{parts[1]}/"
    return f"{parts[0]}/"


def scope_census(
    paths: Iterable[str],
    is_ring3_plus: Callable[[str], bool],
) -> dict[str, tuple[int, int]]:
    """Per-root ``(file count, files inside the requirement)`` measured off the tree.

    ``is_ring3_plus`` is a parameter rather than an import so the selftest can
    measure a deliberately different classifier, and so this module does not
    import the checker that imports it.
    """
    tally: dict[str, list[int]] = {}
    for rel in paths:
        row = tally.setdefault(root_of(rel), [0, 0])
        row[0] += 1
        if is_ring3_plus(rel):
            row[1] += 1
    return {root: (counts[0], counts[1]) for root, counts in sorted(tally.items())}


def measured_state(files: int, inside: int) -> str:
    """The state the tree itself supports for a root.

    ``unscoped`` is returned for a root with nothing inside the requirement;
    an ``exempt`` row measures the same way, which is why the reason, not the
    measurement, is what separates those two.
    """
    if inside == 0:
        return STATE_UNSCOPED
    if inside == files:
        return STATE_REQUIRED
    return STATE_PARTIAL


def _parse_directive(line: str) -> tuple[str, int]:
    """One ``#! key: count`` line, or raise."""
    key, sep, value = line[len(DIRECTIVE) :].partition(":")
    if not sep:
        error = _malformed("directive carries no count", line)
        raise error
    key = key.strip()
    if key not in COUNT_KEYS:
        error = _malformed(f"unknown directive key {key!r}", line)
        raise error
    try:
        count = int(value.strip())
    except ValueError as exc:
        error = _malformed("non-integer count", line)
        raise error from exc
    if count < 0:
        error = _malformed("negative count", line)
        raise error
    return key, count


def _parse_row(line: str) -> tuple[str, str, str]:
    """One ``state root[: reason]`` row, or raise."""
    state, sep, tail = line.partition(" ")
    if not sep:
        error = _malformed("row carries no root", line)
        raise error
    if state not in STATES:
        error = _malformed(f"unknown state {state!r}", line)
        raise error
    root, _, reason = tail.partition(":")
    root = root.strip()
    if not root.endswith("/"):
        error = _malformed("root must end in a slash", line)
        raise error
    return state, root, reason.strip()


def parse_declaration(text: str) -> tuple[dict[str, int], dict[str, tuple[str, str]]]:
    """Parse the declaration into declared counts and rows keyed by root.

    Raises instead of skipping on a malformed directive, a duplicate key or
    root, or an unknown state: a declaration that parses to less than it says
    bounds less than it claims.
    """
    counts: dict[str, int] = {}
    rows: dict[str, tuple[str, str]] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(DIRECTIVE):
            key, count = _parse_directive(line)
            if key in counts:
                error = _malformed("duplicate directive key", key)
                raise error
            counts[key] = count
            continue
        if not line or line.startswith("#"):
            continue
        state, root, reason = _parse_row(line)
        if root in rows:
            error = _malformed("duplicate root", root)
            raise error
        rows[root] = (state, reason)
    return counts, rows


def state_tally(rows: dict[str, tuple[str, str]]) -> dict[str, int]:
    """Row counts by state, plus the total under :data:`ROOTS_KEY`."""
    tally = dict.fromkeys(STATES, 0)
    for state, _reason in rows.values():
        tally[state] += 1
    tally[ROOTS_KEY] = len(rows)
    return tally


def format_declaration_counts(rows: dict[str, tuple[str, str]]) -> str:
    """The ``#!`` count block a given row set requires."""
    tally = state_tally(rows)
    return "\n".join(f"{DIRECTIVE} {key}: {tally[key]}" for key in COUNT_KEYS)


def _count_failures(declared: dict[str, int], rows: dict[str, tuple[str, str]]) -> list[str]:
    """Declared counts that are absent or disagree with the rows present."""
    failures: list[str] = []
    tally = state_tally(rows)
    for key in COUNT_KEYS:
        if key not in declared:
            failures.append(f"{DECLARATION_REL}: no '{DIRECTIVE} {key}:' count declared")
        elif declared[key] != tally[key]:
            failures.append(
                f"{DECLARATION_REL}: declares {declared[key]} '{key}' row(s), carries {tally[key]}"
            )
    return failures


def _reason_failures(rows: dict[str, tuple[str, str]]) -> list[str]:
    """Rows outside the requirement that give no reason."""
    return [
        f"{DECLARATION_REL}: {state} row '{root}' gives no reason after ':'"
        for root, (state, reason) in sorted(rows.items())
        if state in STATES_NEEDING_REASON and not reason
    ]


def _coverage_failures(
    rows: dict[str, tuple[str, str]],
    census: dict[str, tuple[int, int]],
) -> list[str]:
    """Roots the tree has and the file does not, and rows the tree no longer has."""
    failures = [
        f"{DECLARATION_REL}: tree root '{root}' is not declared -- "
        f"a new root cannot enter the tree unjudged"
        for root in census
        if root not in rows
    ]
    failures.extend(
        f"{DECLARATION_REL}: stale row '{root}': no first-party file sits under it"
        for root in sorted(rows)
        if root not in census
    )
    return failures


def _state_failures(
    rows: dict[str, tuple[str, str]],
    census: dict[str, tuple[int, int]],
) -> list[str]:
    """Declared states that disagree with what the classifier actually covers."""
    failures: list[str] = []
    for root, (state, _reason) in sorted(rows.items()):
        if root not in census:
            continue
        files, inside = census[root]
        measured = measured_state(files, inside)
        if state == measured or (state in STATES_OUTSIDE and measured == STATE_UNSCOPED):
            continue
        failures.append(
            f"{DECLARATION_REL}: '{root}' declared {state}, tree measures {measured} "
            f"({inside} of {files} file(s) inside the requirement)"
        )
    return failures


def declaration_text_failures(text: str, census: dict[str, tuple[int, int]]) -> list[str]:
    """Every finding for a declaration body measured against a census."""
    try:
        declared, rows = parse_declaration(text)
    except ScopeDeclarationError as exc:
        return [f"{DECLARATION_REL}: {exc}"]
    if not rows:
        return [f"{DECLARATION_REL}: declares no roots at all"]
    if not any(state == STATE_REQUIRED for state, _reason in rows.values()):
        return [
            f"{DECLARATION_REL}: no root is declared {STATE_REQUIRED} -- "
            f"the World-tag requirement would reach nothing"
        ]
    failures = _coverage_failures(rows, census)
    failures.extend(_state_failures(rows, census))
    failures.extend(_reason_failures(rows))
    failures.extend(_count_failures(declared, rows))
    return failures


def census_summary(
    rows: dict[str, tuple[str, str]],
    census: dict[str, tuple[int, int]],
) -> str:
    """One line naming how far the requirement reaches, printed on every sweep."""
    files = sum(count for count, _inside in census.values())
    inside = sum(inside for _count, inside in census.values())
    tally = state_tally(rows)
    states = ", ".join(f"{tally[state]} {state}" for state in STATES)
    return (
        f"world-tag scope: {len(census)} root(s) [{states}]; "
        f"{inside} of {files} first-party file(s) inside the requirement"
    )


def evaluate(
    repo_root: pathlib.Path,
    paths: Iterable[str],
    is_ring3_plus: Callable[[str], bool],
) -> tuple[list[str], str]:
    """Findings for the committed declaration, plus its one-line census.

    A missing or unreadable declaration is a FINDING, not an empty bound: with
    no declaration nothing holds the scope to what it claims, which is the
    state this module was written to end.
    """
    census = scope_census(paths, is_ring3_plus)
    path = repo_root / DECLARATION_REL
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        return ([f"{DECLARATION_REL}: missing or unreadable: {exc}"], "world-tag scope: unread")
    if not census:
        return (
            [f"{DECLARATION_REL}: no first-party file was enumerated -- scope unmeasurable"],
            "world-tag scope: nothing enumerated",
        )
    failures = declaration_text_failures(text, census)
    try:
        _declared, rows = parse_declaration(text)
    except ScopeDeclarationError:
        rows = {}
    return failures, census_summary(rows, census)


# ---------------------------------------------------------------------------
# Selftest -- both directions. A scope bound that has quietly stopped matching
# reports "documented" forever, which is the defect class this module is about,
# so every rule is proved to FIRE on a broken declaration and stay QUIET on the
# committed one, against the live tree.
# ---------------------------------------------------------------------------

_FIXTURE_CENSUS = {
    "examples/": (4, 1),
    "libs/ra8_core/": (2, 0),
    "libs/ra8_fs/": (2, 0),
    "libs/ra8_hal/": (3, 3),
    "tests/": (2, 2),
}

#: Two required roots on purpose: moving ONE of them out still leaves the
#: requirement reaching something, so the state-drift rule is what fires
#: rather than the "no required root at all" floor.
_FIXTURE_ROWS = {
    "examples/": (STATE_PARTIAL, "per-app src/main.c only"),
    "libs/ra8_core/": (STATE_EXEMPT, "Rings 1-2, Secure by policy"),
    "libs/ra8_fs/": (STATE_UNSCOPED, "first-party, no tag required yet"),
    "libs/ra8_hal/": (STATE_REQUIRED, ""),
    "tests/": (STATE_REQUIRED, ""),
}


def render_declaration(rows: dict[str, tuple[str, str]]) -> str:
    """A declaration body for a row set, counts first, as the committed file is written."""
    lines = [format_declaration_counts(rows)]
    for root, (state, reason) in sorted(rows.items()):
        lines.append(f"{state} {root}: {reason}" if reason else f"{state} {root}")
    return "\n".join(lines) + "\n"


def _fires(rows: dict[str, tuple[str, str]], token: str) -> bool:
    """Whether a row set's rendered declaration reports a finding naming ``token``."""
    findings = declaration_text_failures(render_declaration(rows), _FIXTURE_CENSUS)
    return any(token in f for f in findings)


def _selftest_rows(expect_fn: Callable[[bool, str, list[str]], None], failures: list[str]) -> None:
    """Row-level rules: coverage, drift in both directions, and a missing reason."""
    expect_fn(
        not declaration_text_failures(render_declaration(_FIXTURE_ROWS), _FIXTURE_CENSUS),
        "a declaration matching the tree stays quiet",
        failures,
    )
    dropped = {k: v for k, v in _FIXTURE_ROWS.items() if k != "libs/ra8_fs/"}
    expect_fn(_fires(dropped, "is not declared"), "a tree root with no row fires", failures)
    extra = dict(_FIXTURE_ROWS, **{"libs/ra8_gone/": (STATE_UNSCOPED, "deleted library")})
    expect_fn(_fires(extra, "stale row"), "a row the tree no longer has fires", failures)
    widened = dict(_FIXTURE_ROWS, **{"examples/": (STATE_REQUIRED, "")})
    expect_fn(
        _fires(widened, "declared required, tree measures partial"),
        "a required row the classifier only partly covers fires",
        failures,
    )
    narrowed = dict(_FIXTURE_ROWS, **{"libs/ra8_hal/": (STATE_UNSCOPED, "debt")})
    expect_fn(
        _fires(narrowed, "declared unscoped, tree measures required"),
        "a root the classifier covers but the file calls unscoped fires",
        failures,
    )
    reasonless = dict(_FIXTURE_ROWS, **{"libs/ra8_fs/": (STATE_UNSCOPED, "")})
    expect_fn(
        _fires(reasonless, "gives no reason"),
        "a root left outside the requirement with no reason fires",
        failures,
    )


def _selftest_counts(
    expect_fn: Callable[[bool, str, list[str]], None],
    failures: list[str],
) -> None:
    """Declared counts: a wrong total, a state swap at an unchanged total, and omissions."""
    text = render_declaration(_FIXTURE_ROWS)
    total = state_tally(_FIXTURE_ROWS)[ROOTS_KEY]
    inflated = text.replace(
        f"{DIRECTIVE} {ROOTS_KEY}: {total}", f"{DIRECTIVE} {ROOTS_KEY}: {total + 1}"
    )
    expect_fn(
        any("roots" in f for f in declaration_text_failures(inflated, _FIXTURE_CENSUS)),
        "a total that disagrees with the rows present fires",
        failures,
    )
    swapped = text.replace(
        f"{STATE_EXEMPT} libs/ra8_core/", f"{STATE_UNSCOPED} libs/ra8_core/"
    )
    expect_fn(
        any(
            STATE_EXEMPT in f or STATE_UNSCOPED in f
            for f in declaration_text_failures(swapped, _FIXTURE_CENSUS)
        ),
        "a root moving between states fires even at an unchanged total",
        failures,
    )
    for key in COUNT_KEYS:
        dropped = "\n".join(
            line for line in text.splitlines() if not line.startswith(f"{DIRECTIVE} {key}:")
        )
        expect_fn(
            any(
                f"'{DIRECTIVE} {key}:'" in f
                for f in declaration_text_failures(dropped, _FIXTURE_CENSUS)
            ),
            f"an undeclared '{key}' count fires",
            failures,
        )


def _selftest_malformed(
    expect_fn: Callable[[bool, str, list[str]], None],
    failures: list[str],
) -> None:
    """Every malformed shape is a finding rather than a skipped line."""
    good = render_declaration(_FIXTURE_ROWS)
    cases = (
        ("duplicate root", good + f"{STATE_UNSCOPED} libs/ra8_fs/: again\n"),
        ("unknown state", good + "grandfathered libs/ra8_gfx/: nope\n"),
        ("root must end in a slash", good + f"{STATE_UNSCOPED} libs/ra8_gfx: x\n"),
        (
            "non-integer count",
            good.replace(f"{DIRECTIVE} {ROOTS_KEY}: ", f"{DIRECTIVE} {ROOTS_KEY}: n"),
        ),
        ("unknown directive key", good + f"{DIRECTIVE} {ROOTS_KEY}x: 4\n"),
        ("carries no count", good + f"{DIRECTIVE} roots 4\n"),
        ("duplicate directive", good + f"{DIRECTIVE} {ROOTS_KEY}: 4\n"),
    )
    for token, text in cases:
        expect_fn(
            any(token in f for f in declaration_text_failures(text, _FIXTURE_CENSUS)),
            f"a malformed declaration ({token}) fires",
            failures,
        )
    only_outside = {"libs/ra8_fs/": (STATE_UNSCOPED, "debt")}
    expect_fn(
        any(
            "would reach nothing" in f
            for f in declaration_text_failures(render_declaration(only_outside), _FIXTURE_CENSUS)
        ),
        "a declaration with no required root fires",
        failures,
    )
    expect_fn(
        any("no roots at all" in f for f in declaration_text_failures("", _FIXTURE_CENSUS)),
        "a declaration with no rows fires",
        failures,
    )


def _selftest_vacuity(
    expect_fn: Callable[[bool, str, list[str]], None],
    failures: list[str],
    paths: Iterable[str],
    is_ring3_plus: Callable[[str], bool],
) -> None:
    """A missing declaration and an empty tree are findings, not empty bounds."""
    path_list = list(paths)
    with tempfile.TemporaryDirectory() as tmp:
        findings, _line = evaluate(pathlib.Path(tmp), path_list, is_ring3_plus)
        expect_fn(
            any("missing or unreadable" in f for f in findings),
            "a missing scope declaration fires instead of bounding nothing",
            failures,
        )
    expect_fn(
        any("scope unmeasurable" in f for f in evaluate(pathlib.Path.cwd(), (), is_ring3_plus)[0]),
        "an empty first-party set fires (scope unmeasurable)",
        failures,
    )


def _selftest_classification(
    expect_fn: Callable[[bool, str, list[str]], None],
    failures: list[str],
) -> None:
    """Root mapping and the measured-state ladder."""
    cases = (
        (root_of("libs/ra8_hal/inc/a.h") == "libs/ra8_hal/", "libs/ splits per library"),
        (root_of("tests/hal/src/a.c") == "tests/", "tests/ is judged at its top level"),
        (root_of("libs/README.md") == "libs/", "a file directly in libs/ maps to libs/"),
        (root_of("stray.c") == TOP_LEVEL_ROOT, "a repo-root file maps to ./"),
        (measured_state(4, 4) == STATE_REQUIRED, "all files inside measures required"),
        (measured_state(4, 1) == STATE_PARTIAL, "some files inside measures partial"),
        (measured_state(4, 0) == STATE_UNSCOPED, "no files inside measures unscoped"),
    )
    for held, label in cases:
        expect_fn(held, label, failures)


def selftest_failures(
    repo_root: pathlib.Path,
    paths: Iterable[str],
    is_ring3_plus: Callable[[str], bool],
) -> list[str]:
    """Run every scope assertion, ending with the COMMITTED declaration on the live tree."""
    failures: list[str] = []
    path_list = list(paths)
    _selftest_classification(expect, failures)
    _selftest_rows(expect, failures)
    _selftest_counts(expect, failures)
    _selftest_malformed(expect, failures)
    _selftest_vacuity(expect, failures, path_list, is_ring3_plus)
    live, line = evaluate(repo_root, path_list, is_ring3_plus)
    expect(not live, f"committed scope declaration matches the tree ({line})", failures)
    return failures
