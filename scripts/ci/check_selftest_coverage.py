#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Enforce the ``--selftest`` requirement that nothing used to enforce.

The repo's stated remedy for its dominant defect class -- a detector that has
quietly stopped matching -- is a ``--selftest`` asserting BOTH directions.
``scripts/ci.sh`` says so and ``CLAUDE.md`` says so.  Nothing checked it, so a
new checker with no selftest, or one whose gate never ran it, landed clean
(#531).  Two of the checkers that turned out to have no selftest were the two
that had silently stopped seeing their subject -- ``check_obsolete_standards``
scanning 0 files and ``audit_init_order`` reaching 11 of 217 apps.  That is not
a coincidence, and it is why this gate exists.

WHAT THE RULE IS, AND WHY IT IS NARROWED
----------------------------------------

An unenforceable rule in ``CLAUDE.md`` is itself the defect class, so the rule
is scoped to where it can be both meaningful and true:

  **Rule A (universal).**  ANY first-party script a gate body invokes that
  *has* a selftest must have it RUN by that gate.  A selftest nobody executes
  is documentation.  This has no exceptions and no baseline.

  **Rule B (detectors).**  Every script under ``scripts/checks/`` -- plus the
  ``scripts/ci/check_*.py`` meta-checkers -- that a gate body invokes must
  ACCEPT a selftest.

Rule B is keyed on the taxonomy ``CLAUDE.md`` already documents, in which
``scripts/`` is organised by the QUESTION a script answers: ``checks/`` is
"Is the tree OK?" -- read-only, exits non-zero when it is not.  Those are the
detectors, and a detector is exactly the thing that can stop detecting.
``builders/`` produce a build output, ``report/`` "never fails on content",
and ``hil/`` drives the bench; demanding a both-directions selftest of
``scripts/builders/docs.sh`` would be ceremony, and a gate that demands
ceremony gets disabled.  Scoping by the repo's own stated organisation is a
principle, not an allowlist.

THE BACKLOG IS RETIRED, NOT WAIVED
----------------------------------

Turning Rule B on found a real backlog, and issue #790 closed every row. The
former ``.github/selftest-baseline.txt`` must remain absent: a NEW gate-wired
detector with no selftest fails immediately, and recreating even an empty
baseline fails too. There is no update mode because a detector regression is
fixed by restoring its genuine both-direction selftest, never by freezing it.

Run::

    check_selftest_coverage.py             # the gate
    check_selftest_coverage.py --list      # what is scanned, and its status
    check_selftest_coverage.py --selftest  # prove both directions

Exit 0 if clean, 1 on a violation, 2 when the scan itself collapsed.
"""

from __future__ import annotations

import argparse
import ast
import re
import shlex
import sys
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_DIR = REPO_ROOT / "scripts" / "ci" / "gates"
BASELINE_FILE = REPO_ROOT / ".github" / "selftest-baseline.txt"

# A first-party script path as one shell argv token. An optional prefix covers
# the normal ``$REPO_ROOT/scripts/...`` spelling without treating prose in a
# quoted argument as an invocation.
SCRIPT_TOKEN_RE = re.compile(r"(?:^|.*/)(scripts/[\w./-]+\.(?:py|sh))$")
SHELL_CONTROL = frozenset({";", "&&", "||", "|", "&", "(", ")"})
SELFTEST_ARGS = frozenset({"--selftest", "selftest"})

# Directories whose scripts are DETECTORS and therefore owe a selftest under
# Rule B. Derived from the scripts/ taxonomy documented in CLAUDE.md.
DETECTOR_DIRS = ("scripts/checks/",)
# The sibling meta-checkers, which live one directory up from checks/.
DETECTOR_META_RE = re.compile(r"^scripts/ci/check_[\w.]+\.py$")  # PATHREF-OK: a regex, not a path

# A tree with no gate-invoked scripts means the parser stopped matching, not
# that the gates stopped calling checkers. Refuse to report clean against
# nothing. Measured 94 gate-invoked first-party scripts on 2026-07-28.
MIN_INVOKED = 40

EXIT_OK = 0
EXIT_VIOLATION = 1
EXIT_VACUOUS = 2


def is_detector(rel: str) -> bool:
    """Report whether `rel` is a detector owing a selftest under Rule B.

    Args:
        rel: Repo-relative script path.

    Returns:
        True for ``scripts/checks/*`` and the ``scripts/ci/check_*.py`` meta
        checkers, False for builders, reports, generators and bench drivers.
    """
    return rel.startswith(DETECTOR_DIRS) or bool(DETECTOR_META_RE.match(rel))


def _shell_segments(text: str) -> list[list[str]]:
    """Tokenize shell source into simple-command segments.

    Quoting and comments are handled by :mod:`shlex`; control operators end a
    segment so a selftest argument on a neighboring command cannot confer
    credit on a detector that did not receive it.
    """
    logical = text.replace("\\\n", " ")
    segments: list[list[str]] = []
    for line in logical.splitlines():
        lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|()")
        lexer.commenters = "#"
        lexer.whitespace_split = True
        current: list[str] = []
        try:
            tokens = list(lexer)
        except ValueError:
            continue
        for token in tokens:
            if token in SHELL_CONTROL or (token and set(token) <= set(";&|()")):
                if current:
                    segments.append(current)
                    current = []
            else:
                current.append(token)
        if current:
            segments.append(current)
    return segments


def _segment_invocations(tokens: list[str]) -> list[tuple[str, bool]]:
    """Return scripts and exact selftest argv association for one command."""
    scripts: list[tuple[int, str]] = []
    for index, token in enumerate(tokens):
        match = SCRIPT_TOKEN_RE.match(token)
        if match:
            scripts.append((index, match.group(1)))
    found: list[tuple[str, bool]] = []
    for position, (index, rel) in enumerate(scripts):
        end = scripts[position + 1][0] if position + 1 < len(scripts) else len(tokens)
        found.append((rel, any(token in SELFTEST_ARGS for token in tokens[index + 1 : end])))
    return found


def scan_gate_invocations(text: str) -> dict[str, bool]:
    """Map every first-party script invoked in `text` to whether a selftest ran.

    Args:
        text: A gate fragment's source.

    Returns:
        ``script path -> True`` when at least one invocation of that script in
        this text carried a selftest.
    """
    found: dict[str, bool] = {}
    for segment in _shell_segments(text):
        for rel, ran in _segment_invocations(segment):
            found[rel] = found.get(rel, False) or ran
    return found


def _python_has_selftest(text: str) -> bool:
    """Recognize an actual Python argv branch or argparse declaration."""
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return False
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            function = node.func
            is_add_argument = (
                isinstance(function, ast.Attribute) and function.attr == "add_argument"
            )
            if is_add_argument and any(
                isinstance(arg, ast.Constant) and arg.value == "--selftest" for arg in node.args
            ):
                return True
        if isinstance(node, ast.Compare) and any(
            isinstance(child, ast.Constant) and child.value == "--selftest"
            for child in ast.walk(node)
        ):
            return True
    return False


def _shell_has_selftest(text: str) -> bool:
    """Recognize a shell case arm or argument-test dispatch for selftest."""
    for line in text.replace("\\\n", " ").splitlines():
        lexer = shlex.shlex(line, posix=True, punctuation_chars="()")
        lexer.commenters = "#"
        lexer.whitespace_split = True
        try:
            tokens = list(lexer)
        except ValueError:
            continue
        if not tokens:
            continue
        if tokens[0] in SELFTEST_ARGS and ")" in tokens[1:]:
            return True
        if tokens[0] in {"if", "[", "[["} and "--selftest" in tokens[1:]:
            return True
    return False


def source_has_selftest(rel: str, text: str) -> bool:
    """Recognize an implemented selftest from source syntax, not token prose."""
    if rel.endswith(".py"):
        return _python_has_selftest(text)
    if rel.endswith(".sh"):
        return _shell_has_selftest(text)
    return False


def collect() -> dict[str, bool]:
    """Gather every gate-invoked script and whether any gate runs its selftest.

    Gate bodies delegate. ``gate_format`` invokes ``format_code.sh``, and
    *that* is what drives ``check_comment_format.py`` -- a detector every bit as
    gate-wired as one named in the fragment itself. Reading only the fragments
    made such a script invisible: it was neither credited as invoked nor asked
    for its selftest, so the checker reported clean over a detector no gate ever
    proved. The walk therefore follows first-party shell helpers to a fixed
    point, `seen` guarding against a helper cycle.

    Returns:
        ``script path -> selftest is invoked somewhere along the gate's reach``.
    """
    direct: dict[str, bool] = {}
    for fragment in sorted(GATE_DIR.glob("*.sh")):
        for rel, ran in scan_gate_invocations(fragment.read_text(encoding="utf-8")).items():
            direct[rel] = direct.get(rel, False) or ran

    def read(rel: str) -> str | None:
        path = REPO_ROOT / rel
        return path.read_text(encoding="utf-8") if path.is_file() else None

    return expand_helpers(direct, read)


def expand_helpers(
    direct: dict[str, bool], read_text: Callable[[str], str | None]
) -> dict[str, bool]:
    """Extend `direct` with the scripts its shell helpers invoke, transitively.

    Args:
        direct: ``script path -> a selftest ran`` as read from the gate bodies.
        read_text: Returns a script's source, or None when it cannot be read.

    Returns:
        The same mapping, plus every script reachable through a ``.sh`` helper.
        `seen` makes a helper cycle terminate rather than spin.
    """
    out = dict(direct)
    queue = list(direct)
    seen: set[str] = set()
    while queue:
        rel = queue.pop()
        if rel in seen or not rel.endswith(".sh"):
            continue
        seen.add(rel)
        text = read_text(rel)
        if text is None:
            continue
        for sub, ran in scan_gate_invocations(text).items():
            out[sub] = out.get(sub, False) or ran
            queue.append(sub)
    return out


def has_selftest(rel: str) -> bool:
    """Report whether the script at `rel` implements a selftest.

    Args:
        rel: Repo-relative script path.

    Returns:
        True when either selftest spelling appears in the file; False when the
        file cannot be read (a stale invocation is caught separately).
    """
    path = REPO_ROOT / rel
    if not path.is_file():
        return False
    return source_has_selftest(rel, path.read_text(encoding="utf-8", errors="replace"))


def evaluate(invoked: dict[str, bool]) -> tuple[list[str], list[str]]:
    """Split the gate-invoked scripts into Rule A and Rule B offenders.

    Args:
        invoked: Output of `collect`.

    Returns:
        ``(rule_a, rule_b)`` -- scripts with an unrun selftest, and detectors
        with no selftest at all. Both sorted.
    """
    rule_a: list[str] = []
    rule_b: list[str] = []
    for rel, ran in sorted(invoked.items()):
        if not (REPO_ROOT / rel).is_file():
            continue
        if has_selftest(rel):
            if not ran:
                rule_a.append(rel)
        elif is_detector(rel):
            rule_b.append(rel)
    return rule_a, rule_b


def _report(rule_a: list[str], rule_b: list[str]) -> None:
    """Print every violation with the fix spelled out.

    Args:
        rule_a: Scripts whose selftest no gate runs.
        rule_b: Gate-wired detectors missing a selftest.
    """
    for rel in rule_a:
        sys.stderr.write(
            f"  {rel}: implements a selftest that NO gate body runs.\n"
            "      A selftest nobody executes is documentation. Add it to the gate,\n"
            f"      before the scan:   python3 {rel} --selftest\n\n"
        )
    for rel in rule_b:
        sys.stderr.write(
            f"  {rel}: a gate-wired detector with NO --selftest.\n"
            "      A detector that has quietly stopped matching is indistinguishable\n"
            "      from a clean tree. Add a --selftest asserting BOTH directions (a\n"
            "      must-fire case and a must-stay-quiet case) and run it in the gate.\n"
            "      Do NOT recreate .github/selftest-baseline.txt; that debt authority\n"
            "      is retired and must remain absent.\n\n"
        )


def run_check() -> int:
    """Apply Rule A and Rule B with no remaining baseline authority.

    Returns:
        0 clean, 1 on a violation, 2 when the scan collapsed.
    """
    invoked = collect()
    if len(invoked) < MIN_INVOKED:
        sys.stderr.write(
            f"check_selftest_coverage.py: FATAL -- only {len(invoked)} gate-invoked "
            f"script(s) found, floor is {MIN_INVOKED}.\n"
            "  A collapsed scan reports full selftest coverage because it saw nothing.\n"
        )
        return EXIT_VACUOUS

    rule_a, rule_b = evaluate(invoked)
    retired_baseline_present = BASELINE_FILE.is_file()

    if rule_a or rule_b or retired_baseline_present:
        sys.stderr.write("check_selftest_coverage.py: selftest requirement violated\n\n")
        _report(rule_a, rule_b)
        if retired_baseline_present:
            sys.stderr.write(
                "  .github/selftest-baseline.txt: the debt reached zero, so the "
                "retired baseline must be deleted.\n\n"
            )
        total = len(rule_a) + len(rule_b) + int(retired_baseline_present)
        sys.stderr.write(f"{total} violation(s).\n")
        return EXIT_VIOLATION

    print(
        f"check_selftest_coverage.py: clean -- {len(invoked)} gate-invoked script(s); "
        "every selftest present is run; zero detector selftest debt; "
        "retirement baseline absent."
    )
    return EXIT_OK


def run_list() -> int:
    """Print every gate-invoked script with its selftest status.

    Returns:
        Always 0.
    """
    invoked = collect()
    for rel, ran in sorted(invoked.items()):
        impl = has_selftest(rel)
        kind = "detector" if is_detector(rel) else "other   "
        print(f"{kind}  impl={'Y' if impl else 'n'}  run={'Y' if ran else 'n'}  {rel}")
    print(f"total: {len(invoked)}")
    return EXIT_OK


def _selftest_cases() -> list[tuple[str, str, bool]]:
    """Return ``(label, gate fragment text, must_fire)`` fixtures.

    Both directions are covered deliberately: a meta-checker that only ever
    sees compliant gate bodies cannot tell "compliant" from "stopped matching".

    Returns:
        The fixture list.
    """
    return [
        (
            "a detector whose selftest the gate runs (flag form)",
            "gate_x() (\n  set -e\n  python3 scripts/checks/check_asm.py --selftest\n"
            "  python3 scripts/checks/check_asm.py\n)\n",
            False,
        ),
        (
            "a detector whose selftest the gate SKIPS",
            "gate_x() (\n  set -e\n  python3 scripts/checks/check_asm.py\n)\n",
            True,
        ),
        (
            "the subcommand selftest spelling counts as running one",
            "gate_x() (\n  set -e\n  bash scripts/ci/monitor.sh selftest\n)\n",
            False,
        ),
        (
            "a commented-out invocation is not an invocation",
            "gate_x() (\n  set -e\n  # python3 scripts/checks/check_asm.py\n  true\n)\n",
            False,
        ),
        (
            "a selftest word in a later command does not confer credit",
            "gate_x() (\n  python3 scripts/checks/check_asm.py; echo --selftest\n)\n",
            True,
        ),
        (
            "a neighboring detector's selftest does not confer credit",
            "gate_x() (\n  python3 scripts/checks/check_asm.py && "
            "python3 scripts/checks/check_c23_headers.py --selftest\n)\n",
            True,
        ),
        (
            "quoted prose naming a detector is not an invocation",
            'gate_x() (\n  echo "scripts/checks/check_asm.py --selftest"\n)\n',
            False,
        ),
    ]


def _rule_a_cases() -> list[tuple[str, bool]]:
    """Rule A over the fixture gate bodies: it must fire on an unrun selftest.

    Returns:
        ``(label, held)`` per fixture, the label carrying which direction the
        fixture asserts so a failure names it.
    """
    out: list[tuple[str, bool]] = []
    for label, text, must_fire in _selftest_cases():
        invoked = scan_gate_invocations(text)
        fired = any(
            has_selftest(rel) and not ran
            for rel, ran in invoked.items()
            if (REPO_ROOT / rel).is_file()
        )
        expectation = "must fire" if must_fire else "must stay quiet"
        out.append((f"{label} ({expectation})", fired == must_fire))
    return out


def _helper_walk_cases() -> list[tuple[str, bool]]:
    """The helper walk, driven off a fixture filesystem.

    Without the walk a detector invoked through a gate's shell helper is
    invisible in both directions: never credited as gate-wired, never asked for
    its selftest.

    Returns:
        ``(label, held)`` per case.
    """
    helper = "scripts/checks/helper.sh"  # PATHREF-OK: selftest fixture, not a real script
    detector = "scripts/checks/check_thing.py"  # PATHREF-OK: selftest fixture, not a real script
    other = "scripts/checks/other.sh"  # PATHREF-OK: selftest fixture, not a real script
    quiet_helper = {helper: f"python3 {detector}\n"}
    loud_helper = {helper: f"python3 {detector} --selftest\npython3 {detector}\n"}
    cyclic = {helper: f"bash {other}\n", other: f"bash {helper}\n"}

    reached_quiet = expand_helpers({helper: False}, quiet_helper.get)
    reached_loud = expand_helpers({helper: False}, loud_helper.get)
    reached_cycle = expand_helpers({helper: False}, cyclic.get)

    return [
        (
            "a detector reached through a gate helper is seen",
            detector in reached_quiet,
        ),
        (
            "its selftest going unrun through that helper is reported",
            reached_quiet.get(detector) is False,
        ),
        (
            "its selftest being run through that helper counts",
            reached_loud.get(detector) is True,
        ),
        ("a helper cycle terminates", other in reached_cycle),
    ]


def _taxonomy_cases() -> list[tuple[str, bool]]:
    """Which directories count as detectors, and both selftest spellings.

    Returns:
        ``(label, held)`` per case.
    """
    return [
        ("scripts/checks/ is classified as a detector", is_detector("scripts/checks/check_asm.py")),
        (
            "scripts/ci/check_*.py is classified as a detector",
            is_detector("scripts/ci/check_ci_parity.py"),
        ),
        ("scripts/builders/ is NOT a detector", not is_detector("scripts/builders/docs.sh")),
        ("scripts/report/ is NOT a detector", not is_detector("scripts/report/roadmap_stats.py")),
        ("the flag selftest spelling is detected", has_selftest("scripts/checks/check_asm.py")),
        ("the subcommand selftest spelling is detected", has_selftest("scripts/ci/monitor.sh")),
    ]


def _implementation_cases() -> list[tuple[str, bool]]:
    """Prove implementation credit requires executable dispatch syntax."""
    python_probe = "scripts/checks/probe.py"  # PATHREF-OK: nonexistent selftest fixture
    shell_probe = "scripts/checks/probe.sh"  # PATHREF-OK: nonexistent selftest fixture
    return [
        (
            "Python argparse selftest declaration is implemented",
            source_has_selftest(
                python_probe,
                'parser.add_argument("--selftest", action="store_true")\n',
            ),
        ),
        (
            "Python argv comparison is implemented",
            source_has_selftest(
                python_probe,
                'if "--selftest" in argv[1:]:\n    run_selftest()\n',
            ),
        ),
        (
            "Python docstring token alone is not implemented",
            not source_has_selftest(
                python_probe,
                '"""Run this checker with --selftest."""\n',
            ),
        ),
        (
            "shell case arm is implemented",
            source_has_selftest(
                shell_probe,
                'case "$1" in\n  --selftest) run_selftest ;;\nesac\n',
            ),
        ),
        (
            "shell echo token alone is not implemented",
            not source_has_selftest(
                shell_probe,
                'echo "probe.sh --selftest: PASS"\n',
            ),
        ),
        (
            "shell comment token alone is not implemented",
            not source_has_selftest(
                shell_probe,
                "# --selftest) run_selftest ;;\n",
            ),
        ),
    ]


def _live_scan_cases() -> list[tuple[str, bool]]:
    """The two properties that can only be asserted against the real tree.

    Returns:
        ``(label, held)`` for the non-vacuity floor, zero debt, and retired
        baseline.
    """
    live = collect()
    _, rule_b = evaluate(live)
    return [
        (
            f"live scan sees {len(live)} gate-invoked script(s) (floor {MIN_INVOKED})",
            len(live) >= MIN_INVOKED,
        ),
        (
            "the retired baseline is absent instead of being recreated",
            not BASELINE_FILE.is_file(),
        ),
        ("the live tree carries zero detector selftest debt", not rule_b),
    ]


def _report_cases(cases: list[tuple[str, bool]]) -> int:
    """Print one line per case; return how many did not hold.

    Args:
        cases: ``(label, held)`` pairs.

    Returns:
        The number of cases that failed.
    """
    failures = 0
    for label, ok in cases:
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")
    return failures


def selftest() -> int:
    """Prove Rule A fires on an unrun selftest and spares a run one.

    Returns:
        0 when every case holds, 1 otherwise.
    """
    families = (
        _rule_a_cases,
        _helper_walk_cases,
        _taxonomy_cases,
        _implementation_cases,
        _live_scan_cases,
    )
    failures = sum(_report_cases(family()) for family in families)
    if failures:
        sys.stderr.write(f"check_selftest_coverage.py --selftest: {failures} case(s) failed.\n")
        return EXIT_VIOLATION
    print("check_selftest_coverage.py --selftest: all cases pass (both directions).")
    return EXIT_OK


def main() -> int:
    """Parse arguments and dispatch.

    Returns:
        A process exit status.
    """
    parser = argparse.ArgumentParser(description="enforce the --selftest requirement")
    parser.add_argument("--check", action="store_true", help="apply the rules (the gate mode)")
    parser.add_argument("--list", action="store_true", help="print the scanned set and status")
    parser.add_argument("--selftest", action="store_true", help="prove both directions, then exit")
    args = parser.parse_args()

    if not GATE_DIR.is_dir():
        sys.stderr.write(
            f"check_selftest_coverage.py: FATAL -- {GATE_DIR} does not exist; the gate "
            "bodies moved and this checker is scanning nothing.\n"
        )
        return EXIT_VACUOUS
    if args.selftest:
        return selftest()
    if args.list:
        return run_list()
    if not args.check:
        parser.error("one of --check / --list / --selftest is required")
    return run_check()


if __name__ == "__main__":
    raise SystemExit(main())
