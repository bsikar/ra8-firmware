#!/usr/bin/env python3
"""
check_new_compound_has_mcdc.py -- Reject staged commits that introduce a new
compound boolean decision (`&&` / `||`) without an accompanying MC/DC test.

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B Qualification" and
docs/MCDC.md, every compound boolean decision in production code under
`libs/`, `src/`, `port/` must have a matching MC/DC test vector set in
`tests/test_<module>.c`. The MC/DC test function (named `test_mcdc_*`)
declares its vector pattern in a Doxygen `@par MC/DC:` block that cites
the source `file:line` of the decision.

This pre-commit gate is a *static* check: it never builds or runs the
test suite (so it adds no perceptible latency to `git commit`). It works
by diffing the staged version of each production file against HEAD and
flagging compound boolean decisions that are present in the staged
version but were NOT present in the HEAD version on the same source
line. For each such NEW decision, the script searches every staged-or-
already-committed `tests/test_*.c` file for a `@par MC/DC:` block whose
text contains a citation of the source path and a nearby line number
(see `LINE_TOLERANCE` below for drift tolerance, since unrelated edits
can shift the cited line by a handful of rows).

If a new compound decision is staged WITHOUT a matching MC/DC test in
the same commit OR already committed in HEAD, the commit is REJECTED.

This complements `check_mcdc_block.py`, which enforces that *test* files
declare `@par MC/DC:` blocks; this script enforces that the production
side actually has a test counterpart.

The check intentionally does NOT cover:
  * `libs/third_party/`  -- SOUP exempted per docs/MCDC.md.
  * `tests/`             -- only production code.
  * `examples/`          -- application code, not yet under MC/DC gate.
  * Single-condition `if (x)` statements -- MC/DC only applies to
    compound decisions with `&&` or `||`.

Exit code:
  0  no new compound decision is missing a matching MC/DC test.
  1  one or more new decisions lack an MC/DC test (commit rejected).
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Production directories that are subject to the MC/DC pre-commit gate.
PROD_PREFIXES: tuple[str, ...] = ("libs/", "src/", "port/")

# Excluded subtrees (SOUP, generated, etc.).
EXCLUDED_SUBSTRINGS: tuple[str, ...] = (
    "/third_party/",
    "/_deps/",
    "/build/",
)

# Line-number drift tolerance when matching a citation against the actual
# line of a new compound decision. Tests cite a specific `file:line` but
# unrelated edits above the decision can shift the cited line by a small
# number of rows; we accept any cited line within +/- LINE_TOLERANCE of
# the actual decision line. Citations far outside this window are
# treated as stale (and the new decision is reported as untested).
LINE_TOLERANCE = 25

# Regex matching a compound boolean decision on a non-comment line.
# We require the operator to be surrounded by whitespace or paren so we
# do not collide with `&` / `|` (bitwise) or `&&` inside string literals
# (we additionally strip string contents below).
COMPOUND_OP_RE = re.compile(r"(?:\|\||&&)")

# Regex stripping line and block comment contents so we do not flag
# `&&` that appears in a comment. This is intentionally simple and may
# leave fragments inside multi-line block comments; the line-by-line
# diff approach further mitigates this (a NEW comment also cannot
# usually contain `&&` on its own).
LINE_COMMENT_RE = re.compile(r"//.*$")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/")
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')
CHAR_LITERAL_RE = re.compile(r"'(?:\\.|[^'\\])*'")

# Regex matching one citation token inside a `@par MC/DC:` block.
# We only need to capture the source path and the optional line number;
# the surrounding text varies (see tests/test_ra_sci.c for examples).
CITATION_RE = re.compile(
    r"(?P<path>(?:libs|src|port)/[A-Za-z0-9_./-]+\.c):(?P<line>\d+)"
)

# Regex isolating each `@par MC/DC:` block in a test file. The block
# starts at `@par MC/DC:` and runs until the next `@par`, the next
# `*/`, or the next blank Doxygen line (` *` followed by EOL).
MCDC_BLOCK_RE = re.compile(
    r"@par\s+MC/DC\s*:.*?(?=(?:\*/|@par\s+\w|\n\s*\*\s*\n))",
    re.IGNORECASE | re.DOTALL,
)


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------


def _git(*args: str) -> str:
    """Run `git <args...>` and return stdout (text)."""
    return subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def staged_files(*, suffix: str | None = None, prefixes: tuple[str, ...] | None = None) -> list[str]:
    """Return staged file paths (added/copied/modified/renamed) optionally
    filtered by suffix and/or path prefix."""
    out = _git("diff", "--cached", "--name-only", "--diff-filter=ACMR")
    paths: list[str] = []
    for p in out.splitlines():
        if suffix is not None and not p.endswith(suffix):
            continue
        if prefixes is not None and not any(p.startswith(pre) for pre in prefixes):
            continue
        if any(sub in p for sub in EXCLUDED_SUBSTRINGS):
            continue
        paths.append(p)
    return paths


def staged_blob(path: str) -> str:
    """Return the staged (index) version of `path`, or empty string if
    the file is not in the index."""
    try:
        return _git("show", f":0:{path}")
    except subprocess.CalledProcessError:
        return ""


def head_blob(path: str) -> str:
    """Return the HEAD version of `path`, or empty string if the file
    does not exist in HEAD (newly added)."""
    try:
        return _git("show", f"HEAD:{path}")
    except subprocess.CalledProcessError:
        return ""


# ---------------------------------------------------------------------------
# Decision detection
# ---------------------------------------------------------------------------


def _scrub(line: str) -> str:
    """Strip comment / string / char-literal contents from a single line
    so that downstream regex matches do not fire on tokens inside them."""
    line = STRING_LITERAL_RE.sub('""', line)
    line = CHAR_LITERAL_RE.sub("''", line)
    line = BLOCK_COMMENT_RE.sub("", line)
    line = LINE_COMMENT_RE.sub("", line)
    return line


def compound_decision_lines(text: str) -> set[tuple[int, str]]:
    """Return the set of (line_no, scrubbed_line) tuples in `text` that
    contain at least one compound boolean operator outside of
    comments/strings. Line numbers are 1-based."""
    found: set[tuple[int, str]] = set()
    for idx, raw in enumerate(text.splitlines(), start=1):
        scrubbed = _scrub(raw)
        if COMPOUND_OP_RE.search(scrubbed):
            # Normalize whitespace so cosmetic reformatting in the staged
            # file does not cause false-positive "new" decisions.
            normalized = re.sub(r"\s+", " ", scrubbed.strip())
            found.add((idx, normalized))
    return found


def new_decisions(staged_text: str, head_text: str) -> list[tuple[int, str]]:
    """Return the list of (line_no, normalized_line) in staged_text that
    represent NEW compound boolean decisions not present in head_text.

    A decision is considered "not new" if the SAME normalized scrubbed
    line text appears anywhere in head_text (regardless of line number),
    so that pure additions/insertions above an existing decision do not
    trip the gate.
    """
    head_norms = {norm for _, norm in compound_decision_lines(head_text)}
    staged = compound_decision_lines(staged_text)
    return sorted(
        [(ln, norm) for (ln, norm) in staged if norm not in head_norms],
        key=lambda t: t[0],
    )


# ---------------------------------------------------------------------------
# Test-side citation index
# ---------------------------------------------------------------------------


def collect_test_citations() -> list[tuple[str, int]]:
    """Walk every `tests/test_*.c` file in the working tree (which
    includes both staged additions and already-committed tests) and
    return the list of (source_path, source_line) citations found
    inside `@par MC/DC:` Doxygen blocks."""
    cites: list[tuple[str, int]] = []
    tests_dir = Path("tests")
    if not tests_dir.is_dir():
        return cites
    for tf in sorted(tests_dir.glob("test_*.c")):
        try:
            text = tf.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for block in MCDC_BLOCK_RE.findall(text):
            for m in CITATION_RE.finditer(block):
                cites.append((m.group("path"), int(m.group("line"))))
    return cites


def has_matching_citation(
    src_path: str,
    src_line: int,
    citations: list[tuple[str, int]],
) -> bool:
    """Return True if any (path, line) in `citations` matches the
    given source decision (same path, line within +/- LINE_TOLERANCE)."""
    for path, line in citations:
        if path == src_path and abs(line - src_line) <= LINE_TOLERANCE:
            return True
    return False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    prod_files = staged_files(suffix=".c", prefixes=PROD_PREFIXES)
    if not prod_files:
        return 0

    citations = collect_test_citations()

    findings: list[tuple[str, int, str]] = []
    for path in prod_files:
        staged_text = staged_blob(path)
        head_text = head_blob(path)
        if not staged_text:
            continue
        for line_no, normalized in new_decisions(staged_text, head_text):
            if not has_matching_citation(path, line_no, citations):
                findings.append((path, line_no, normalized))

    if findings:
        print("[FAIL] check_new_compound_has_mcdc.py: new compound boolean")
        print("       decisions are staged without an accompanying MC/DC")
        print("       test vector set in tests/test_*.c.")
        print("")
        print("       Per docs/MCDC.md, every `&&` / `||` decision under")
        print("       libs/, src/, port/ must have a `test_mcdc_*` function")
        print("       in the matching tests/test_<module>.c whose")
        print("       `@par MC/DC:` block cites the source file:line.")
        print("")
        print(f"       Citation drift tolerance: +/- {LINE_TOLERANCE} lines.")
        print("")
        print("       Offending decisions:")
        for path, line_no, normalized in findings[:50]:
            snippet = normalized if len(normalized) <= 80 else normalized[:77] + "..."
            print(f"         {path}:{line_no}: {snippet}")
        if len(findings) > 50:
            print(f"         ... and {len(findings) - 50} more")
        print("")
        print("       Fix: add a `test_mcdc_<decision>` function in the")
        print("       matching tests/test_<module>.c with N+1 vectors and")
        print("       a `@par MC/DC:` block citing the source file:line,")
        print("       then re-stage and commit. See docs/MCDC.md for the")
        print("       worked example.")
        return 1

    print("check_new_compound_has_mcdc.py: 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
