# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Both-direction selftest for the tree-coverage gate.

Proves the detectors in ``check_tree_coverage.py`` fire when they should and
stay quiet when they should not: the line and branch ratchets, measured vs
unmeasured kind changes, file moves, severity, scope, baseline round-tripping
and claim handling. Every case asserts both directions, so a detector that has
been silently disabled fails here.

Split out of ``check_tree_coverage.py`` (#2791), which sat over the 1000-line
cap ``scripts/checks/check_file_size.py`` enforces, and follows the pattern
``markdown_reference_selftest.py`` already sets for this repo. The gate imports
this module lazily from ``main()`` so the two files do not form an import cycle.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from check_tree_coverage import (
    BRANCH_FLOOR_PCT,
    HARD,
    LINE_FLOOR_PCT,
    Row,
    detect_moves,
    evaluate,
    format_baseline,
    measured,
    parse_baseline,
    unmeasured,
)
from tree_coverage_model import (
    REASON_COMPILED,
    REASON_FIRMWARE,
    REASON_HOSTED,
    REASON_PLATFORM,
    ceiling_selftest_failures,
    census_floor_failures,
    census_paths,
    in_census,
    requirement_claim_failures,
    srs_text,
    structural_reason,
    unclaimed_coverage_projects,
)

#: The committed state the ratchet cases are measured against: one unit at the
#: floor, one deep in debt, one firmware composition, one host tool with no
#: coverage build. Every reason class and both row kinds are represented, so a
#: rule that stopped covering either kind is caught by a case rather than by
#: nobody.
SELFTEST_BASELINE: dict[str, Row] = {
    "libs/ra8_demo/src/frozen.c": measured((90, 100, 80, 100)),
    "apps/shared_libs/mdl/src/debt.c": measured((41, 100, 30, 100)),
    "examples/ek_ra8d2/demo/main.c": unmeasured(REASON_FIRMWARE),
    "tools/demo/src/host.c": unmeasured(REASON_HOSTED),
}
Case = tuple[str, dict[str, Row], bool]


def _swap(rel: str, row: Row) -> dict[str, Row]:
    """The baseline with one row replaced -- the shape every case needs."""
    return {**SELFTEST_BASELINE, rel: row}


def _ratchet_cases() -> list[Case]:
    """Cases for the MEASURED ratchet: debt, ratio, floors, improvement."""
    frozen = "libs/ra8_demo/src/frozen.c"
    debt = "apps/shared_libs/mdl/src/debt.c"
    return [
        ("an unchanged tree stays quiet", dict(SELFTEST_BASELINE), False),
        ("uncovered line debt growth fires", _swap(frozen, measured((90, 101, 80, 100))), True),
        ("a line ratio drop fires", _swap(frozen, measured((89, 100, 80, 100))), True),
        ("a branch ratio drop fires", _swap(frozen, measured((90, 100, 79, 100))), True),
        ("an improvement stays quiet", _swap(frozen, measured((97, 100, 88, 100))), False),
        ("burning debt down stays quiet", _swap(debt, measured((60, 100, 45, 100))), False),
        ("a debt unit sliding further fires", _swap(debt, measured((40, 100, 30, 100))), True),
        # Deleting covered code lowers the ratio without making anything worse:
        # exempt. Shrinking while uncovered debt grows is still caught.
        ("moving covered code out stays quiet", _swap(debt, measured((31, 90, 25, 95))), False),
        ("a shrink that grew debt still fires", _swap(debt, measured((30, 90, 30, 100))), True),
    ]


def _kind_cases() -> list[Case]:
    """Cases for the row kinds: new units, one-way moves, reason classes."""
    frozen = "libs/ra8_demo/src/frozen.c"
    tool = "tools/demo/src/host.c"
    new = "libs/ra8_demo/src/new.c"
    return [
        (
            "a well-covered new unit still needs a row",
            {**SELFTEST_BASELINE, new: measured((9, 10, 8, 10))},
            True,
        ),
        (
            "a poorly-covered new unit fires",
            {**SELFTEST_BASELINE, new: measured((8, 10, 7, 10))},
            True,
        ),
        (
            "a new unmeasured unit fires",
            {**SELFTEST_BASELINE, new: unmeasured(REASON_PLATFORM)},
            True,
        ),
        ("losing measurement fires", _swap(frozen, unmeasured(REASON_PLATFORM)), True),
        ("gaining measurement fires", _swap(tool, measured((10, 10, 10, 10))), True),
        ("a changed reason class fires", _swap(tool, unmeasured(REASON_COMPILED)), True),
        (
            "a deleted unit's stale row fires",
            {k: v for k, v in SELFTEST_BASELINE.items() if k != tool},
            True,
        ),
    ]


#: The unit the move cases relocate, and where they relocate it to.
MOVE_FROM = "apps/shared_libs/mdl/src/debt.c"
MOVE_TO = "apps/host/mdl/src/debt.c"


def _moved(destination: str, row: Row) -> dict[str, Row]:
    """The baseline with ``MOVE_FROM`` relocated to ``destination``."""
    out = {k: v for k, v in SELFTEST_BASELINE.items() if k != MOVE_FROM}
    out[destination] = row
    return out


def _move_cases() -> list[Case]:
    """Cases for move detection: a real move, and every ambiguity it refuses."""
    carried = SELFTEST_BASELINE[MOVE_FROM]
    return [
        ("an unchanged tree pairs nothing", dict(SELFTEST_BASELINE), False),
        ("a move still reports its stale row", _moved(MOVE_TO, carried), True),
        ("a move that regressed fires", _moved(MOVE_TO, measured((40, 100, 30, 100))), True),
        (
            "a move that also renames the file is not paired",
            _moved("apps/host/mdl/src/renamed.c", carried),
            True,
        ),
        (
            "a split into two same-named units is not paired",
            {**_moved(MOVE_TO, carried), "tools/demo/src/debt.c": carried},
            True,
        ),
    ]


def _move_failures() -> list[str]:
    """Prove a move carries its debt, and that no ambiguous pairing does."""
    carried = SELFTEST_BASELINE[MOVE_FROM]
    out: list[str] = []
    if detect_moves(SELFTEST_BASELINE, SELFTEST_BASELINE):
        out.append("an unchanged tree must pair no move")
    if detect_moves(_moved(MOVE_TO, carried), SELFTEST_BASELINE) != {MOVE_TO: MOVE_FROM}:
        out.append("one vanished and one arrived unit of the same name must pair")
    if detect_moves(
        {**_moved(MOVE_TO, carried), "tools/demo/src/debt.c": carried}, SELFTEST_BASELINE
    ):
        out.append("an ambiguous same-basename arrival must not pair")
    # The load-bearing pair: identical below-floor counts are HARD when the
    # unit is new and quiet when the same unit merely moved.
    as_new = evaluate({**SELFTEST_BASELINE, MOVE_TO: carried}, SELFTEST_BASELINE)
    as_move = evaluate(_moved(MOVE_TO, carried), SELFTEST_BASELINE)
    regressed = evaluate(_moved(MOVE_TO, measured((40, 100, 30, 100))), SELFTEST_BASELINE)
    if not any(f.severity == HARD for f in as_new):
        out.append("a genuinely new below-floor unit must stay HARD")
    if any(f.severity == HARD for f in as_move):
        out.append("a moved unit must carry its debt rather than re-enter at the floor")
    if not any(f.severity == HARD for f in regressed):
        out.append("a move must not launder a coverage regression")
    return out


def _evaluate_failures() -> list[str]:
    """Run every evaluate() case and name the ones that answered wrongly."""
    return [
        name
        for name, fresh, should_fire in _ratchet_cases() + _kind_cases() + _move_cases()
        if bool(evaluate(fresh, SELFTEST_BASELINE)) != should_fire
    ]


def _severity_failures() -> list[str]:
    """Prove --update refuses a regression and accepts a pure staleness."""
    frozen = "libs/ra8_demo/src/frozen.c"
    tool = "tools/demo/src/host.c"
    debt = "apps/shared_libs/mdl/src/debt.c"
    regression = evaluate(_swap(frozen, measured((80, 100, 80, 100))), SELFTEST_BASELINE)
    staleness = evaluate(_swap(tool, measured((10, 10, 10, 10))), SELFTEST_BASELINE)
    shrink = evaluate(_swap(debt, measured((31, 90, 25, 95))), SELFTEST_BASELINE)
    out: list[str] = []
    if not any(f.severity == HARD for f in regression):
        out.append("a coverage regression must be HARD so --update refuses it")
    if any(f.severity == HARD for f in staleness):
        out.append("a unit that merely gained measurement must not be HARD")
    if shrink:
        out.append("deleting covered code must not be reported as a regression")
    # The ratio rule must still bite where it adds signal: a unit that did NOT
    # shrink. Without this the shrink exemption could widen into a no-op.
    if not any(
        "ratio regressed" in f.message
        for f in evaluate(_swap(frozen, measured((85, 100, 80, 100))), SELFTEST_BASELINE)
    ):
        out.append("a same-size unit whose ratio fell must still report the ratio")
    return out


def _scope_failures() -> list[str]:
    """Prove the census, the floors and the project-claim guard all still bite."""
    live = census_paths()
    out: list[str] = []
    # Carry the root and the numbers out. "a floor failed" with no figure in it
    # is what let #2638 sit red for 13 days: it could not say whether code had
    # left tools/ or the enumeration had broken.
    collapsed = census_floor_failures([rel for rel in live if not rel.startswith("tools/")])
    out += [
        f"the live census must clear every root floor: {m}" for m in census_floor_failures(live)
    ]
    if not collapsed:
        out.append("a census with tools/ removed must fail its root floor")
    elif not any("re-pin this floor" in m for m in collapsed):
        out.append("a census failure must tell the reader which way to resolve it")
    if unclaimed_coverage_projects(["tests", "apps/shared_libs/mdl"]):
        out.append("a claimed coverage project must not be reported unclaimed")
    if not unclaimed_coverage_projects(["tools/unwired"]):
        out.append("an unclaimed coverage project must be reported")
    if not in_census("libs/ra8_demo/src/a.c") or in_census("libs/ra8_demo/tests/a.c"):
        out.append("the census must take production units and reject test sources")
    if (
        structural_reason(
            "apps/shared_libs/reflow/v2/src/reflow_v2.cpp",
            compiled=False,
            firmware_dirs=(),
        )
        != REASON_PLATFORM
    ):
        out.append("the mutually exclusive reflow v2 adapter must remain platform-cross-only")
    if (
        structural_reason("apps/shared_libs/demo/src/host.c", compiled=False, firmware_dirs=())
        != REASON_HOSTED
    ):
        out.append("ordinary unmeasured app code must remain hosted debt")
    return out


def _format_failures() -> list[str]:
    """Prove the baseline round-trips and renders byte-identically twice."""
    text = format_baseline(SELFTEST_BASELINE)
    out: list[str] = []
    if parse_baseline(text) != SELFTEST_BASELINE:
        out.append("the baseline must round-trip through format/parse unchanged")
    if format_baseline(SELFTEST_BASELINE) != text:
        out.append("the baseline must render identically on every call")
    try:
        parse_baseline("libs/a.c\tMEASURED\t1\t2\n")
    except ValueError:
        pass
    else:
        out.append("a malformed baseline row must raise, not be skipped")
    return out


def _claim_failures() -> list[str]:
    """Prove the REQ-SAFE-017 tie holds on the committed doc and fires on drift.

    The quiet case is the LIVE requirement, so a floor edited here without the
    requirement (or the reverse) fails the selftest that runs in every CI leg,
    not only the coverage leg, which needs a measurement to reach a verdict at
    all.
    """
    live = srs_text()
    drifted = "| REQ-SAFE-017 | First-party coverage SHALL reach 90/90. | gate | CI |"
    silent = "| REQ-SAFE-016 | something else entirely | gate | CI |"
    out: list[str] = []
    if requirement_claim_failures(live, LINE_FLOOR_PCT, BRANCH_FLOOR_PCT):
        out.append("the committed REQ-SAFE-017 row must state the floors this gate enforces")
    if not requirement_claim_failures(live, LINE_FLOOR_PCT + 1, BRANCH_FLOOR_PCT):
        out.append("a line floor the requirement does not state must fire")
    if not requirement_claim_failures(live, LINE_FLOOR_PCT, BRANCH_FLOOR_PCT - 1):
        out.append("a branch floor the requirement does not state must fire")
    if not requirement_claim_failures(drifted, LINE_FLOOR_PCT, BRANCH_FLOOR_PCT):
        out.append("the pre-#844 universal 90/90 claim must fire")
    if not requirement_claim_failures(silent, LINE_FLOOR_PCT, BRANCH_FLOOR_PCT):
        out.append("an SRS that states REQ-SAFE-017 nowhere must fire")
    return out


def selftest() -> int:
    """Prove every rule fires and stays quiet, and that no scope collapsed."""
    cases = len(_ratchet_cases()) + len(_kind_cases()) + len(_move_cases())
    failures = (
        _evaluate_failures()
        + _severity_failures()
        + _move_failures()
        + _scope_failures()
        + _format_failures()
        + ceiling_selftest_failures()
        + _claim_failures()
    )
    if failures:
        for name in failures:
            print(f"check_tree_coverage.py --selftest: FAIL: {name}", file=sys.stderr)
        return 1
    print(
        f"check_tree_coverage.py --selftest: PASS "
        f"({cases} both-direction cases, 5 non-vacuity floors, "
        f"REQ-SAFE-017 tied to {LINE_FLOOR_PCT}/{BRANCH_FLOOR_PCT})"
    )
    return 0
