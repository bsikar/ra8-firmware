#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Reject .gitignore directory patterns that match at arbitrary depth.

WHY THIS EXISTS
---------------
`.gitignore` line 2 was `build/` for the life of the tree. A pattern with a
trailing slash and no leading slash matches at EVERY depth, so any directory
named `build` -- anywhere -- was silently untrackable. #359's reorganisation
created `scripts/build/` (PATHREF-OK: #359 renamed it), and its six
files were tracked only because `git mv` moves files that were already
tracked. A seventh, newly created, would never have been added, and
`git add` would have reported nothing wrong.

That is the whole failure mode: it produces no error, in either direction. Git
declines to add the file and says nothing, and the thirteen checkers that
excluded the substring `/build/` skipped the directory while reporting a clean
tree. It survived for years because nothing it did was ever visible.

Anchoring the patterns fixed the instance. This gate fixes the CLASS: an
unanchored directory pattern cannot be added again without a decision.

WHAT COUNTS AS ANCHORED
-----------------------
A directory pattern (one ending in `/`) is anchored when it carries a leading
slash, or contains a slash anywhere but the end -- either form binds it to a
known place in the tree:

    /build/                 anchored to the repo root
    /examples/**/build/     anchored under examples/, any depth beneath it
    tests/Unity/            anchored (an interior slash roots the pattern)
    build/                  NOT anchored -- matches at every depth
    Debug/                  NOT anchored

TWO WAYS TO BE UNANCHORED AND LEGAL
-----------------------------------
1. RESERVED_DIR_NAMES -- names a tool owns. CMake writes `CMakeFiles/`, CPython
   writes `__pycache__/`, npm writes `node_modules/`. Nobody can legitimately
   author a source directory with one of those names, so matching them at any
   depth cannot swallow source. This is a judgement about the NAME, not a
   convenience list, and that is why `build`, `output`, `ra`, `Debug` and
   `Release` are not on it: every one of them is a plausible source directory,
   and `ra/` had in fact already swallowed the vendored FSP blob trees.

2. An explicit `gitignore-scope-ok: <reason>` marker in the comment block
   directly above the pattern. `downloads/` is the real case: tools/media_dl
   writes it relative to whatever directory it runs from, so there is no single
   root to anchor it to. The marker records that this was decided rather than
   overlooked, and an empty reason is not accepted.

Run with --selftest to prove the rule fires on an unanchored pattern and stays
quiet on every legal form.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(
    subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],  # noqa: S607 -- trusted: fixed git argv
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
)

# Directory names owned by a tool. See the module docstring: this is a claim
# that the NAME is reserved, not that the directory is uninteresting.
RESERVED_DIR_NAMES = frozenset(
    {
        ".git",
        ".idea",
        ".metadata",
        ".settings",
        ".venv",
        ".vscode",
        "CMakeFiles",
        "__pycache__",
        "node_modules",
        "venv",
    }
)

MARKER = "gitignore-scope-ok:"

# A .gitignore this size cannot legitimately collapse to a handful of lines.
PATTERN_FLOOR = 20


class Finding:
    """One unanchored directory pattern."""

    def __init__(self, lineno: int, pattern: str) -> None:
        """Record one unanchored .gitignore pattern and where it was written."""
        self.lineno = lineno
        self.pattern = pattern

    def __str__(self) -> str:
        """Render the finding together with its remediation.

        The message carries the fix rather than just the diagnosis, because
        the failure mode is silent -- an unanchored pattern matches at every
        depth and hides files nobody meant to ignore.
        """
        return (
            f".gitignore:{self.lineno}: [GI001] '{self.pattern}' has no leading slash "
            "and no interior slash, so git matches it at EVERY depth.\n"
            f"    Anchor it (/{self.pattern}), scope it to the roots where it is "
            "actually produced\n"
            f"    (/examples/**/{self.pattern}), or record the decision with a "
            f"'{MARKER} <reason>'\n"
            "    comment directly above it."
        )


def is_anchored(pattern: str) -> bool:
    """True when `pattern` is bound to a known place in the tree."""
    if pattern.startswith("/"):
        return True
    return "/" in pattern.rstrip("/")


def scan(text: str) -> list[Finding]:
    """Every unanchored directory pattern in `text`, with its line number."""
    findings: list[Finding] = []
    pending_marker = False
    for lineno, raw in enumerate(text.split("\n"), start=1):
        line = raw.strip()
        if not line:
            pending_marker = False
            continue
        if line.startswith("#"):
            # A marker stays live until the comment block ends, so it may sit
            # above a short explanation rather than immediately on the pattern.
            if MARKER in line and line.split(MARKER, 1)[1].strip():
                pending_marker = True
            continue
        if line.startswith("!"):  # a negation re-includes; it cannot swallow
            pending_marker = False
            continue
        if not line.endswith("/"):  # file patterns are a different question
            pending_marker = False
            continue
        name = line.rstrip("/")
        if not is_anchored(line) and name not in RESERVED_DIR_NAMES and not pending_marker:
            findings.append(Finding(lineno, line))
        pending_marker = False
    return findings


def _expect(cond: bool, label: str, failures: list[str]) -> None:
    """Record one selftest assertion and print its pass/fail line.

    Accumulates into ``failures`` instead of raising, so one failing
    assertion does not hide the ones after it -- the value of a
    both-direction selftest is the whole picture, not the first breakage.
    """
    print(f"  [{'ok' if cond else 'FAIL'}] {label}")
    if not cond:
        failures.append(label)


def selftest() -> int:
    """Assert the rule fires on the real defect and stays quiet on legal forms."""
    print("check_gitignore_scope.py --selftest")
    failures: list[str] = []

    # --- MUST-FIRE: the exact shape that caused #377 ----------------------
    got = scan("build/\n")
    _expect(
        [f.pattern for f in got] == ["build/"],
        "a bare 'build/' fires (the #377 landmine)",
        failures,
    )
    _expect(
        [f.pattern for f in scan("output/\nra/\nDebug/\n")] == ["output/", "ra/", "Debug/"],
        "every plausible source name fires (output/, ra/, Debug/)",
        failures,
    )
    _expect(
        [f.lineno for f in scan("# a comment\n\nbuild/\n")] == [3],
        "the reported line number is the pattern's own",
        failures,
    )

    # --- QUIET: every legal form ------------------------------------------
    _expect(not scan("/build/\n"), "a root-anchored '/build/' stays quiet", failures)
    _expect(
        not scan("/examples/**/build/\n"),
        "a root-scoped '/examples/**/build/' stays quiet",
        failures,
    )
    _expect(not scan("tests/Unity/\n"), "an interior slash anchors the pattern", failures)
    _expect(
        not scan("CMakeFiles/\n__pycache__/\nnode_modules/\n"),
        "tool-reserved names stay quiet unanchored",
        failures,
    )
    _expect(not scan("*.o\n*.elf\n"), "file patterns are out of scope", failures)
    _expect(not scan("!libs/third_party/**/ra/\n"), "a negation stays quiet", failures)
    _expect(
        not scan(f"# {MARKER} media_dl writes it relative to its own cwd\ndownloads/\n"),
        "an explicit marker with a reason stays quiet",
        failures,
    )

    # --- the marker must carry a reason, and must not leak ----------------
    _expect(
        bool(scan(f"# {MARKER}\ndownloads/\n")),
        "an EMPTY marker reason does NOT waive the rule",
        failures,
    )
    _expect(
        [f.pattern for f in scan(f"# {MARKER} ok\ndownloads/\n\nbuild/\n")] == ["build/"],
        "a marker does not leak past its own comment block",
        failures,
    )

    # --- the real file must be clean --------------------------------------
    real = (REPO_ROOT / ".gitignore").read_text()
    _expect(
        len([ln for ln in real.split("\n") if ln.strip()]) >= PATTERN_FLOOR,
        "the real .gitignore was actually read (floor check)",
        failures,
    )

    if failures:
        print(f"\nSELFTEST FAILED: {len(failures)} assertion(s)", file=sys.stderr)
        for item in failures:
            print(f"  {item}", file=sys.stderr)
        return 1
    print("selftest: all assertions held (both directions).")
    return 0


def main(argv: list[str]) -> int:
    """Reject .gitignore patterns that match at every directory depth.

    A slashless pattern such as ``build`` matches ANY directory of that name
    anywhere in the tree, which is how a first-party source directory under
    ``scripts/build/`` -- PATHREF-OK: #359 has since renamed it away -- became
    invisible to git and to every gate at once (#377).  Anchoring makes the
    intended scope explicit.

    Returns 0 when every pattern is anchored or justified, 1 otherwise.
    """
    ap = argparse.ArgumentParser(description="Reject unanchored .gitignore directory patterns")
    ap.add_argument("--selftest", action="store_true", help="assert both directions")
    args = ap.parse_args(argv[1:])
    if args.selftest:
        return selftest()

    path = REPO_ROOT / ".gitignore"
    if not path.is_file():
        print("check_gitignore_scope.py: FATAL -- .gitignore not found", file=sys.stderr)
        return 2
    text = path.read_text()
    patterns = [ln for ln in text.split("\n") if ln.strip() and not ln.strip().startswith("#")]
    if len(patterns) < PATTERN_FLOOR:
        print(
            f"check_gitignore_scope.py: FATAL -- only {len(patterns)} pattern(s) read, "
            f"floor is {PATTERN_FLOOR}. A collapsed read reports success because it "
            "saw nothing.",
            file=sys.stderr,
        )
        return 2

    findings = scan(text)
    if findings:
        print(
            f"\n{len(findings)} unanchored .gitignore directory pattern(s) -- each one "
            "silently\nhides any directory of that name, at any depth, from git AND "
            "from every checker:\n",
            file=sys.stderr,
        )
        for finding in findings:
            print(finding, file=sys.stderr)
        return 1
    print(f"check_gitignore_scope.py: {len(patterns)} pattern(s), all anchored or justified.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
