#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: a gate body shall be structurally capable of failing.

``run_gate_capture`` in ``scripts/ci.sh`` disables ERREXIT around the call so
that a gate's own ``set -e`` decides its verdict rather than the caller's::

    run_gate_capture() {
      local name="$1"
      set +e
      run_one_gate "$name"
      RA8_GATE_RC=$?
      set -e
      return 0
    }

That ``set +e`` is live inside any gate whose body is a ``{ }`` BLOCK, because
a block runs in the calling shell. A block reports only its LAST command's
status, so every command before the last one can fail with no effect on the
verdict::

    gate_thing() {
      checker --selftest      # may fail; status discarded
      checker --strict        # only this one decides
    }

A ``( )`` SUBSHELL body is immune: the subshell re-enables ERREXIT for itself
(``set -e``) or guards each step (``... || return 1``), and neither is undone
by the caller.

This is not a hypothetical. ``gate_cite_check`` and ``gate_no_ai_attribution``
each ran a ``--selftest`` first "so a detector that stopped matching cannot
pass as a clean tree" -- and then discarded that selftest's status, because
both were ``{ }`` bodies. The protection the comment described was inert in
``just ci`` while working under ``just quality::local::gate <name>``, i.e. the
exact local-green / CI-red divergence ``scripts/ci.sh`` claims is structurally
impossible.

``suite_errexit_selftest`` (in ``scripts/ci/gates/hygiene.sh``) already proves
the RUNNER propagates a mid-body failure, but it can only prove it for the
shape it probes with -- a ``( set -e )`` subshell. It is therefore blind to a
gate that is not that shape, which is why the defect survived alongside it.
This checker covers the other half: the runner is honest, and now so is every
body it dispatches to.

A THIRD RULE: A GATE MEASURES THE TREE UNDER TEST
-------------------------------------------------

``run_suite_on_snapshot`` cds into a clean snapshot of HEAD and dispatches
every gate from there, which is what "``just ci`` gates committed HEAD, exactly
like CI" means.  ``$REPO_ROOT`` is something else entirely: the HOST checkout
the runner was invoked from.  A gate body that reaches for it measures a
different tree than the one the suite claims to be gating.

``tools-build`` did, and both consequences were real (#546).  It configured and
compiled the WORKING TREE's ``tools/`` -- whatever happened to be dirty in it --
and left its build output there, while the snapshot beside it went unbuilt.
And on the containerised path it could not run at all: the host repo is
bind-mounted READ-ONLY at ``/workspace``, so the first ``cmake -B`` under it
died with ``CMake Error: Unable to (re)create the private pkgRedirects
directory``.  That took win-ci -- the fleet's second verification host, where
``ci-gate-container`` is the normal path -- out of ever reporting a full green.

There is no legitimate use, so there is no exception list: a gate needing the
host repository's *history* calls ``ci_history_repo`` (which ``ci.sh`` points
at the real repo deliberately, because a snapshot cannot carry commit
messages), and a gate needing a path uses ``$PWD``.

Three rules, all purely structural:

1. every ``gate_*()`` body is a ``( ... )`` subshell, never a ``{ ... }`` block;
2. that subshell establishes its own failure discipline -- it enables ERREXIT
   (``set -e`` in any bundling) or it returns explicitly (``|| return 1``);
3. no body references ``$REPO_ROOT``; a gate reads the tree it is standing in.

Run::

    check_gate_bodies.py            # scan scripts/ci/gates/*.sh
    check_gate_bodies.py --selftest # prove the checker fires on every defect

Exit 0 when every gate body can fail and stays inside the tree under test, 1
(listing each offender) otherwise, and 2 when no gate body could be found at
all -- a parser that has stopped seeing its subject must not report a clean
tree.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_DIR = REPO_ROOT / "scripts" / "ci" / "gates"

# `gate_<name>() (` or `gate_<name>() {` on a line of its own -- the only two
# body forms bash offers, and the shape every gate in this tree is written in.
GATE_OPEN_RE = re.compile(r"^gate_([a-z0-9_]+)\(\)\s*([({])\s*$")

# `set -e`, `set -eu`, `set -euo pipefail`, `set -o errexit`, ... Any bundling
# that turns ERREXIT on counts; the flag letters may arrive in any order.
ERREXIT_RE = re.compile(r"^\s*set\s+(?:-[a-df-zA-Z]*e[a-zA-Z]*|-o\s+errexit)\b", re.MULTILINE)

# An explicit `return` anywhere in the body -- the discipline used by the
# commit-range gates, which run `set -uo pipefail` and guard every step with
# `|| return 1` instead of relying on ERREXIT.
RETURN_RE = re.compile(r"\breturn\b")

# `$REPO_ROOT` / `${REPO_ROOT}` reached from a gate body -- rule 3. Matched
# against comment-stripped text, so the fragments may still EXPLAIN the rule.
REPO_ROOT_RE = re.compile(r"\$\{?REPO_ROOT\b")

# A tree with no gate bodies means the parser stopped matching, not that the
# gates became compliant. Refuse to report a clean scan against nothing.
MIN_GATE_BODIES = 1


def strip_comments(text: str) -> str:
    """Drop ``#`` comments so a rule fires on code and not on prose.

    A ``#`` opens a comment at line start or after whitespace, the convention
    ``check_errexit_masking.py`` already uses.  Quoting is not modelled: no
    gate body in this tree carries a ``#`` inside a string, and a rule that
    over-fires on a comment is a rule that gets disabled.

    Args:
        text: raw shell text.

    Returns:
        The same text with comment tails removed, line count preserved.
    """
    out: list[str] = []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            out.append("")
            continue
        cut = re.search(r"(?:^|\s)#", line)
        out.append(line[: cut.start()] if cut else line)
    return "\n".join(out)


class GateBody:
    """One parsed ``gate_*()`` definition and the properties this gate checks.

    Holds the source location, the opening delimiter (which decides whether the
    caller's ``set +e`` leaks in) and the body text, so both rules can be
    evaluated without re-reading the file.
    """

    def __init__(self, name: str, path: Path, line: int, opener: str, body: str) -> None:
        """Record one gate definition.

        Args:
            name: gate function suffix, e.g. ``cite_check`` for ``gate_cite_check``.
            path: the ``scripts/ci/gates/*.sh`` fragment defining it.
            line: 1-based line number of the ``gate_*()`` opener, for the report.
            opener: ``(`` for a subshell body, ``{`` for a block body.
            body: the raw text between the opener and its closing delimiter.
        """
        self.name = name
        self.path = path
        self.line = line
        self.opener = opener
        self.body = body

    @property
    def is_subshell(self) -> bool:
        """Return True when the body is a ``( )`` subshell rather than a block."""
        return self.opener == "("

    @property
    def has_failure_discipline(self) -> bool:
        """Return True when the body decides its own verdict.

        Either ERREXIT is enabled (so any failing command aborts the subshell)
        or the body returns explicitly (the ``|| return 1`` idiom). A body with
        neither reports only its last command's status even as a subshell.
        """
        return bool(ERREXIT_RE.search(self.body)) or bool(RETURN_RE.search(self.body))

    @property
    def where(self) -> str:
        """Return a ``path:line`` location string relative to the repo root."""
        return f"{self.path.relative_to(REPO_ROOT)}:{self.line}"


def parse_gate_bodies(text: str, path: Path) -> list[GateBody]:
    """Extract every ``gate_*()`` definition from one shell fragment.

    The body runs from the opener line to the first line that is exactly the
    matching closing delimiter at column zero. Every gate in this tree is
    written that way (the fragments are formatted by shfmt), so no brace
    counting is needed and a nested ``)`` inside a command cannot end a body
    early.

    Args:
        text: full contents of the shell fragment.
        path: its path, recorded on each returned body for reporting.

    Returns:
        One ``GateBody`` per definition found, in source order.
    """
    lines = text.splitlines()
    bodies: list[GateBody] = []
    index = 0
    while index < len(lines):
        match = GATE_OPEN_RE.match(lines[index])
        if match is None:
            index += 1
            continue
        name, opener = match.group(1), match.group(2)
        closer = ")" if opener == "(" else "}"
        cursor = index + 1
        collected: list[str] = []
        while cursor < len(lines) and lines[cursor].rstrip() != closer:
            collected.append(lines[cursor])
            cursor += 1
        bodies.append(GateBody(name, path, index + 1, opener, "\n".join(collected)))
        index = cursor + 1
    return bodies


def check_bodies(bodies: list[GateBody]) -> list[str]:
    """Apply every structural rule and return one message per violation."""
    errors: list[str] = []
    for gate in bodies:
        if not gate.is_subshell:
            errors.append(
                f"{gate.where}: gate_{gate.name}() has a `{{ }}` BLOCK body.\n"
                f"    run_gate_capture runs gates under `set +e`, and that suppression\n"
                f"    is live inside a block -- so only the LAST command decides the\n"
                f"    verdict and every command before it can fail unnoticed.\n"
                f"    Write it as a subshell instead:\n"
                f"        gate_{gate.name}() (\n"
                f"          set -e\n"
                f"          ...\n"
                f"        )"
            )
            continue
        if not gate.has_failure_discipline:
            errors.append(
                f"{gate.where}: gate_{gate.name}() is a subshell that never enables\n"
                f"    ERREXIT and never returns explicitly, so it reports only its\n"
                f"    last command's status. Add `set -e` as the first line, or guard\n"
                f"    each step with `|| return 1`."
            )
    return errors


def check_fragment_scope(text: str, path: Path) -> list[str]:
    """Rule 3: nothing in a gate fragment may reach for ``$REPO_ROOT``.

    Applied to the WHOLE fragment rather than to the ``gate_*()`` bodies alone,
    because a gate is its helpers too.  ``gate_tools_build`` delegated to
    ``_tb_mdl`` / ``_tb_rabook_viewer`` / ``_tb_other_tools``, and it was
    those helpers that held most of the ``$REPO_ROOT`` paths -- a body-scoped
    rule would have passed the file with the defect still in it.

    Every function in ``scripts/ci/gates/`` runs inside the snapshot the suite
    dispatches from, so the rule needs no exception anywhere in these files.

    Args:
        text: full contents of the fragment.
        path: its path, for the reported location.

    Returns:
        One message per offending line, in source order.
    """
    errors: list[str] = []
    for number, line in enumerate(strip_comments(text).splitlines(), start=1):
        if not REPO_ROOT_RE.search(line):
            continue
        errors.append(
            f"{path.relative_to(REPO_ROOT)}:{number}: reads $REPO_ROOT.\n"
            f"        {line.strip()}\n"
            f"    That is the HOST checkout. The suite runs every gate inside a\n"
            f"    clean snapshot of HEAD, so this measures a different tree than\n"
            f"    the run reports on -- and on the containerised path it cannot\n"
            f"    write under it at all, because the host repo is mounted\n"
            f"    read-only at /workspace (#546).\n"
            f"    Use $PWD, which is the tree under test on every path."
        )
    return errors


def scan(gate_dir: Path) -> tuple[list[str], int]:
    """Scan every shell fragment in ``gate_dir``.

    Args:
        gate_dir: directory holding the sourced ``gate_*`` body fragments.

    Returns:
        ``(errors, bodies_seen)``. A caller must treat ``bodies_seen`` below
        ``MIN_GATE_BODIES`` as a broken scan rather than a clean tree.
    """
    errors: list[str] = []
    seen = 0
    for fragment in sorted(gate_dir.glob("*.sh")):
        text = fragment.read_text(encoding="utf-8")
        bodies = parse_gate_bodies(text, fragment)
        seen += len(bodies)
        errors.extend(check_bodies(bodies))
        errors.extend(check_fragment_scope(text, fragment))
    return errors, seen


def main() -> int:
    """Verify every gate body can fail, and report each one that cannot.

    A gate whose body swallows a failing command reports PASS for work it did
    not do, which is the defect class this whole checker family exists to
    close. The scan is refused outright when it finds no gate bodies, because
    a parser that has stopped matching would otherwise report the cleanest
    tree it has ever seen.

    Returns:
        0 when every body is a failure-capable subshell, 1 when any is not,
        and 2 when the scan found nothing to check.
    """
    parser = argparse.ArgumentParser(description="check that every ci.sh gate body can fail")
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the checker still fires on both defect shapes, and stays quiet on neither",
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if not GATE_DIR.is_dir():
        sys.stderr.write(
            f"check_gate_bodies.py: {GATE_DIR} does not exist -- the gate bodies "
            "moved and this checker is scanning nothing.\n"
        )
        return 2

    errors, seen = scan(GATE_DIR)
    if seen < MIN_GATE_BODIES:
        sys.stderr.write(
            "check_gate_bodies.py: found NO gate bodies under "
            f"{GATE_DIR.relative_to(REPO_ROOT)}. Refusing to report a clean scan "
            "against nothing -- the parser has stopped matching its subject.\n"
        )
        return 2

    if errors:
        sys.stderr.write("check_gate_bodies.py: gate bodies that break the runner contract:\n\n")
        for error in errors:
            sys.stderr.write(f"  {error}\n\n")
        sys.stderr.write(
            f"{len(errors)} gate body/bodies swallow a failing command or leave "
            "the tree under test.\n"
        )
        return 1

    print(
        f"check_gate_bodies.py: clean -- all {seen} gate bodies can fail and stay "
        "inside the tree under test."
    )
    return 0


def _selftest_cases() -> list[tuple[str, str, bool]]:
    """Return ``(label, fragment_text, must_fire)`` selftest fixtures.

    Both directions are covered deliberately: a checker that only ever sees
    good input cannot tell "compliant" from "stopped matching".
    """
    return [
        (
            "block body with two commands (the gate_cite_check shape)",
            "gate_thing() {\n  checker --selftest\n  checker --strict\n}\n",
            True,
        ),
        (
            "block body with one command",
            "gate_thing() {\n  checker --strict\n}\n",
            True,
        ),
        (
            "subshell with neither errexit nor return",
            "gate_thing() (\n  checker --selftest\n  checker --strict\n)\n",
            True,
        ),
        (
            "subshell with set -e",
            "gate_thing() (\n  set -e\n  checker --selftest\n  checker --strict\n)\n",
            False,
        ),
        (
            "subshell with set -euo pipefail",
            "gate_thing() (\n  set -euo pipefail\n  checker --strict\n)\n",
            False,
        ),
        (
            "subshell guarding each step with || return 1",
            "gate_thing() (\n  set -uo pipefail\n  probe || return 1\n  checker --strict\n)\n",
            False,
        ),
        (
            "set -uo pipefail alone must NOT count as errexit",
            "gate_thing() (\n  set -uo pipefail\n  checker --selftest\n  checker --strict\n)\n",
            True,
        ),
    ]


def _scope_selftest_cases() -> list[tuple[str, str, bool]]:
    """Return ``(label, fragment_text, must_fire)`` fixtures for rule 3.

    Separate from the body fixtures because the rule is fragment-scoped: the
    defect it exists for lived in a gate's HELPER, not in the gate body, so a
    fixture set that only ever showed it bodies would prove the wrong thing.
    """
    return [
        (
            "gate body reaching for $REPO_ROOT",
            "gate_thing() (\n  set -e\n"
            '  cmake -S "$REPO_ROOT/tools/x" -B "$REPO_ROOT/build/x"\n)\n',
            True,
        ),
        (
            "gate HELPER reaching for ${REPO_ROOT} (the tools-build shape)",
            '_tb_x() (\n  set -e\n  bash "${REPO_ROOT}/tools/x/run.sh"\n)\n'
            "gate_thing() (\n  set -e\n  _tb_x\n)\n",
            True,
        ),
        (
            "$PWD -- the tree under test",
            'gate_thing() (\n  set -e\n  cmake -S "$PWD/tools/x" -B "$PWD/build/x"\n)\n',
            False,
        ),
        (
            "REPO_ROOT named only in a COMMENT",
            "# never reach for $REPO_ROOT from a gate\n"
            "gate_thing() (\n  set -e\n  checker --strict  # not $REPO_ROOT either\n)\n",
            False,
        ),
        (
            "a fragment naming no repo root at all",
            "gate_thing() (\n  set -e\n  checker --strict\n)\n",
            False,
        ),
    ]


def selftest() -> int:
    """Prove the checker fires on every defect shape and spares the good ones.

    Runs the fixtures through the same ``parse_gate_bodies`` + ``check_bodies``
    path the live scan uses, then asserts the empty-scan floor separately --
    the floor is the guard against this checker silently becoming a no-op, so
    it is the one property that must never be taken on trust.

    Returns:
        0 when every assertion held in both directions, 1 otherwise.
    """
    failures = 0
    for label, text, must_fire in _selftest_cases():
        bodies = parse_gate_bodies(text, GATE_DIR / "selftest.sh")
        fired = bool(check_bodies(bodies))
        ok = fired == must_fire
        if not ok:
            failures += 1
        expectation = "must fire" if must_fire else "must stay quiet"
        print(f"  [{'ok' if ok else 'FAIL'}] {label} ({expectation})")

    for label, text, must_fire in _scope_selftest_cases():
        fired = bool(check_fragment_scope(text, GATE_DIR / "selftest.sh"))
        ok = fired == must_fire
        if not ok:
            failures += 1
        expectation = "must fire" if must_fire else "must stay quiet"
        print(f"  [{'ok' if ok else 'FAIL'}] tree-under-test: {label} ({expectation})")

    # The parser must actually find the definition; a rule that never runs
    # cannot fire, and would read identically to a compliant tree.
    parsed = parse_gate_bodies("gate_alpha() (\n  set -e\n  x\n)\n", GATE_DIR / "selftest.sh")
    ok = len(parsed) == 1 and parsed[0].name == "alpha"
    failures += 0 if ok else 1
    print(f"  [{'ok' if ok else 'FAIL'}] parser extracts a gate name and body")

    # The empty-scan floor: a fragment with no gates must not look clean.
    _, seen = scan(GATE_DIR)
    ok = seen >= MIN_GATE_BODIES
    failures += 0 if ok else 1
    status = "ok" if ok else "FAIL"
    print(f"  [{status}] live scan sees {seen} gate bodies (floor {MIN_GATE_BODIES})")

    if failures:
        sys.stderr.write(f"check_gate_bodies.py --selftest: {failures} case(s) failed.\n")
        return 1
    print("check_gate_bodies.py --selftest: all cases pass (both directions).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
