#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate: no first-party shell function is called with its errexit masked.

The defect class
----------------
Under ``set -e``, calling a function on the left of ``||`` puts it into bash's
inherited "ignoring errors" state::

    run_suite "$fast" || rc=$?

Two things then go wrong.  A failure part-way through the function body no
longer aborts it, so every remaining statement still runs and the caller sees
only the *last* command's status.  Worse, that state propagates into nested
subshells where a plain ``set -e`` cannot clear it -- ``$-`` reports ``e`` set
while a failing command still does not abort.  This tree shipped exactly that:
the gate suite silently degraded to "did each gate's LAST command succeed", so
a gate failing part-way -- including ``require_cmd`` reporting an absent tool
-- reported PASS.

The remedy, already documented at the ``run_suite`` call site in
``scripts/ci.sh``, is to disable errexit around the CALL only::

    set +e
    run_suite "$fast"
    rc=$?
    set -e

The callee then runs in a normal errexit context, so its own ``( set -e; ... )``
subshells re-arm and a mid-body failure is not swallowed.

Why this check and not ShellCheck's SC2310
------------------------------------------
``check-set-e-suppressed`` covers this defect but is unsatisfiable here: it
fires on any function in a condition, including a bare one-line predicate and
the rewrite its own help text recommends.  Measured on this tree it produces 90
findings, and the only two source forms it accepts are worse than what it
rejects -- ``set +e; fn; rc=$?; set -e`` (which it passes) and a bare subshell
(which aborts the parent).  Adopting it would mean ~90 inline disables rather
than ~90 fixes.  See #363 for the full form-by-form evidence.

This check keeps the signal and drops the noise: it fires only where a
first-party function whose body has more than one command is invoked with its
status masked.  A one-command predicate is exempt by construction -- there is
no statement after the failure for errexit to have protected.

Run::

    check_errexit_masking.py             # gate (fail on any finding)
    check_errexit_masking.py --selftest  # prove it fires AND stays quiet

Exit 0 if clean, exit 1 on findings, exit 2 on an internal error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

# A function body with a single command cannot hide a mid-body failure: the
# status the caller captures IS that command's status. Two or more is where
# the defect becomes possible, so that is where the check starts firing.
MIN_BODY_COMMANDS = 2

# The bad fixture masks exactly three calls to the multi-command `work`.
# Pinned as a constant so the selftest asserts a number rather than a
# literal that could be edited to match whatever the checker happens to do.
SELFTEST_EXPECTED_HITS = 3

_DEF_RE = re.compile(r"^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(\)\s*[({]")

# `fn ... || true`, `|| :`, `|| rc=$?` -- the forms that swallow the status.
# The callee must sit at the start of a command position: line start, or after
# a separator, a `case` pattern's `)`, or a `then`/`do`/`else` keyword.
_MASK_RE = re.compile(
    r"(?:^|[;&|()]|\bthen\b|\bdo\b|\belse\b)\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\s+[^|;&]*?)?"
    r"\|\|\s*(?:true\b|:\s|:$|[A-Za-z_][A-Za-z0-9_]*=\$\?)"
)


def _strip(line: str) -> str:
    """Drop comments and neutralise quoted spans, preserving column count."""
    out: list[str] = []
    quote: str | None = None
    for index, char in enumerate(line):
        if quote is not None:
            out.append(char)
            if char == quote:
                quote = None
        elif char in "\"'":
            quote = char
            out.append(char)
        elif char == "#" and (index == 0 or line[index - 1].isspace()):
            break
        else:
            out.append(char)
    return "".join(out)


# Control keywords. A compound block counts as its WORST branch, never the sum,
# and a condition counts zero: a non-zero status in a condition is control flow,
# not a failure errexit was ever going to catch.
_BLOCK_OPEN = frozenset({"if", "while", "until", "for", "case"})
_BODY_START = frozenset({"then", "do", "in"})
_BLOCK_CLOSE = frozenset({"fi", "done", "esac"})
_KEYWORD_RE = re.compile(r"^(if|elif|while|until|for|case|then|do|else|fi|done|esac|\{|\})\b\s*")


def _tokens(body: str) -> list[str]:
    r"""Split a function body into statements at separator positions.

    Newlines and `;` only separate at paren depth zero: a multi-line array
    literal (`local -a args=(\n  --hex ...\n)`) is ONE assignment, not one
    command per element.
    """
    joined = re.sub(r"\\\n", " ", body)
    depth = 0
    flat: list[str] = []
    for char in joined:
        if char == "(":
            depth += 1
        elif char == ")":
            depth = max(0, depth - 1)
        if char in "\n;" and depth > 0:
            flat.append(" ")
        else:
            flat.append(char)
    out: list[str] = []
    for raw in re.split(r"[\n;]", "".join(flat)):
        piece = raw.strip()
        while piece:
            match = _KEYWORD_RE.match(piece)
            if match is None:
                break
            out.append(match.group(1))
            piece = piece[match.end() :].strip()
        if piece:
            out.append(piece)
    return out


# An assignment with no command substitution in it cannot fail, so it is not a
# step errexit was ever protecting. Counting `local app="$1"` as a command is
# what made an early revision of this check fire on functions whose only
# failable statement was the last one -- the same false-positive problem that
# makes SC2310 unusable.
_ASSIGN_RE = re.compile(
    r"^(?:(?:local|declare|readonly|export|typeset)\s+(?:-\w+\s+)*)?"
    r"[A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?\+?=|"
    r"^(?:local|declare|readonly|export|typeset)\s+-?\w*\s*[A-Za-z_]"
)
_SUBST_RE = re.compile(r"\$\(|`")


def _is_failable(token: str) -> bool:
    """False for assignments that cannot fail -- no substitution, no exit status."""
    if _SUBST_RE.search(token):
        return True
    return _ASSIGN_RE.match(token) is None


def _max_sequential(body: str) -> int:
    """Max commands executed sequentially on any ONE path through `body`.

    This is the number that decides whether errexit had anything to protect:
    with two or more commands in a row, a failure in the first still lets the
    rest run and the caller sees only the last one's status.
    """
    stack: list[list[int]] = []
    run = 0
    skipping = False
    for word in _tokens(body):
        if word in _BLOCK_OPEN:
            stack.append([0, run])
            run = 0
            skipping = True
        elif word in _BODY_START:
            skipping = False
        elif word in ("elif", "else"):
            if stack:
                stack[-1][0] = max(stack[-1][0], run)
            run = 0
            skipping = word == "elif"
        elif word in _BLOCK_CLOSE:
            if stack:
                best, saved = stack.pop()
                run = saved + max(best, run)
            skipping = False
        elif word not in ("{", "}") and not skipping and _is_failable(word):
            run += 1
    while stack:
        best, saved = stack.pop()
        run = saved + max(best, run)
    return run


def _function_sizes(lines: list[str]) -> dict[str, int]:
    """Map every function defined in `lines` to its max sequential command count."""
    sizes: dict[str, int] = {}
    index = 0
    while index < len(lines):
        match = _DEF_RE.match(lines[index])
        if match is None:
            index += 1
            continue
        end, body = _body_span(lines, index)
        sizes[match.group(1)] = _max_sequential(body)
        index = end + 1
    return sizes


def _body_span(lines: list[str], start: int) -> tuple[int, str]:
    """Return (last line index, body text) for the function opening at `start`."""
    depth = 0
    opened = False
    collected: list[str] = []
    index = start
    while index < len(lines):
        stripped = _strip(lines[index])
        for char in stripped:
            if char in "{(":
                depth += 1
                opened = True
            elif char in "})":
                depth -= 1
        collected.append(stripped.split("{", 1)[-1] if index == start else stripped)
        if opened and depth <= 0:
            return index, "\n".join(collected)
        index += 1
    return index - 1, "\n".join(collected)


def _scan_file(rel: str) -> list[str]:
    """Return one message per errexit-masked first-party call in `rel`."""
    text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    sizes = _function_sizes(lines)
    findings: list[str] = []
    for number, line in enumerate(lines, 1):
        for match in _MASK_RE.finditer(_strip(line)):
            name = match.group(1)
            size = sizes.get(name)
            if size is None or size < MIN_BODY_COMMANDS:
                continue
            findings.append(
                f"{rel}:{number}: `{name}` (~{size} commands) is invoked with its "
                f"exit status masked; a failure part-way through its body is "
                f"silently swallowed.\n"
                f"    {line.strip()}\n"
                f"    Use:  set +e; {name} ...; rc=$?; set -e"
            )
    return findings


def _targets() -> list[str]:
    """First-party shell scripts -- the same scope `check_shell.py` gates.

    Imported rather than re-derived: a second copy of the scope list is how a
    file ends up covered by one shell gate and invisible to the other.
    """
    import check_shell  # noqa: PLC0415 -- needs the sys.path insert above

    return check_shell.first_party_scripts()


_SELFTEST_BAD = """#!/usr/bin/env bash
set -euo pipefail
work() {
  cp /a /b
  rm /c
}
predicate() { [ -e /tmp ]; }
rc=0
work || rc=$?
case "$1" in
  go) work "$@" || rc=$? ;;
esac
out="$(work || true)"
predicate || rc=$?
if predicate; then echo yes; fi
grep -q x /etc/hosts || rc=$?
"""

_SELFTEST_GOOD = """#!/usr/bin/env bash
set -euo pipefail
work() {
  cp /a /b
  rm /c
}
predicate() { [ -e /tmp ]; }
set +e
work
rc=$?
set -e
if predicate; then echo yes; fi
grep -q x /etc/hosts || rc=$?
external_tool --flag || true
"""


def _selftest() -> int:
    """Assert the check fires on the defect AND stays silent on the remedy."""
    import tempfile  # noqa: PLC0415 -- selftest-only dependency

    failures: list[str] = []
    with tempfile.TemporaryDirectory(dir=REPO_ROOT) as tmp:
        holder = Path(tmp)
        bad = holder / "bad.sh"
        good = holder / "good.sh"
        bad.write_text(_SELFTEST_BAD, encoding="utf-8")
        good.write_text(_SELFTEST_GOOD, encoding="utf-8")
        bad_hits = _scan_file(str(bad.relative_to(REPO_ROOT)))
        good_hits = _scan_file(str(good.relative_to(REPO_ROOT)))

    # MUST-FIRE: the three masked calls on the multi-command `work`.
    if len(bad_hits) != SELFTEST_EXPECTED_HITS:
        failures.append(
            f"must-fire: expected {SELFTEST_EXPECTED_HITS} findings on the bad "
            f"fixture, got {len(bad_hits)}"
        )
    if any("predicate" in hit for hit in bad_hits):
        failures.append("must-fire: flagged the one-command predicate, which is exempt")
    if any("grep" in hit for hit in bad_hits):
        failures.append("must-fire: flagged an external command, which is out of scope")

    # MUST-BE-SILENT: the documented `set +e` remedy and the exempt forms.
    if good_hits:
        failures.append(f"must-be-silent: {len(good_hits)} finding(s) on the good fixture")

    for line in failures:
        sys.stderr.write(f"check_errexit_masking.py: SELFTEST FAIL -- {line}\n")
    if failures:
        return 1
    sys.stdout.write(
        f"check_errexit_masking.py: selftest OK "
        f"(fires on {len(bad_hits)} masked calls, silent on the remedy)\n"
    )
    return 0


def main() -> int:
    """Entry point: `--selftest` proves non-vacuity, otherwise gate the tree."""
    if "--selftest" in sys.argv[1:]:
        return _selftest()
    findings: list[str] = []
    for rel in _targets():
        findings.extend(_scan_file(rel))
    if findings:
        sys.stderr.write("check_errexit_masking.py: errexit-masked first-party call(s):\n\n")
        for finding in findings:
            sys.stderr.write(f"  {finding}\n\n")
        sys.stderr.write(
            f"{len(findings)} finding(s). Disabling errexit around the CALL keeps the\n"
            "callee in a normal errexit context; `||` does not, and the state it sets\n"
            "propagates into nested subshells that `set -e` cannot rescue.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
