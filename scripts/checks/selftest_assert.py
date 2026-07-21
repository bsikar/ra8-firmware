# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Shared both-direction selftest scaffolding for the ``check_*.py`` gates.

Every gate in this tree is required to prove itself in both directions -- it
must FIRE on a broken input and stay QUIET on a good one -- because a checker
that has quietly stopped matching is this repository's dominant defect class
and reports success forever.

Four gates had grown a byte-identical copy of that scaffolding:
``check_asm``, ``check_devcontainer``, ``check_gitignore_scope`` and
``check_lint_coverage`` each carried the same ``_expect`` and the same
report-and-exit tail, down to the docstring. Four copies of an assertion
helper is four places for the reporting convention to drift, and drift in a
selftest is invisible by construction. They share this module instead.

This is deliberately not a unittest/pytest layer. The gates run as standalone
scripts from ``scripts/git/pre-commit`` and from ``scripts/ci.sh`` with no test
runner on PATH, and their selftest output is read by humans in a CI log.
"""

from __future__ import annotations

import sys


def expect(cond: bool, label: str, failures: list[str]) -> None:
    """Record one selftest assertion and print its pass/fail line.

    Accumulates into ``failures`` instead of raising, so one failing
    assertion does not hide the ones after it -- the value of a
    both-direction selftest is the whole picture, not the first breakage.
    """
    print(f"  [{'ok' if cond else 'FAIL'}] {label}")
    if not cond:
        failures.append(label)


def report(failures: list[str]) -> int:
    """Print the closing verdict for a selftest run and return its exit code.

    Returns 1 when anything failed (listing each failed assertion on stderr),
    0 when every assertion held in both directions.
    """
    if failures:
        print(f"\nSELFTEST FAILED: {len(failures)} assertion(s)", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    print("selftest: all assertions held (both directions).")
    return 0
