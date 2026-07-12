#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""
check_new_compound_has_mcdc.py -- Reject staged commits that introduce a new
compound boolean decision (`&&` / `||`) without an accompanying MC/DC test.

Per CLAUDE.md "IEC 61508 SIL 3 / DO-178C Level B Qualification" and
docs/MCDC.md, every compound boolean decision in production code under
`libs/`, `src/`, `port/` must have a matching MC/DC test vector set in
`tests/test_<module>.c`. The MC/DC test function (named `test_mcdc_*`)
declares its vector pattern in a Doxygen `@par MC/DC:` block that cites
the decision as `path@function` -- the source path and the *enclosing
function* of the decision.

This pre-commit gate is a *static* check: it never builds or runs the
test suite (so it adds no perceptible latency to `git commit`). It works
by diffing the staged version of each production file against HEAD and
flagging compound boolean decisions that are present in the staged
version but were NOT present in the HEAD version on the same source
line. For each such NEW decision, the script resolves the decision's
enclosing function and searches every staged-or-already-committed
`tests/test_*.c` file for a `@par MC/DC:` block citing
`path@that_function`. Citing by function (not line number) means
unrelated edits that shift lines never invalidate a citation.

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

# Display limits.
MAX_DISPLAYED_FINDINGS = 50  # Max number of findings to print before summarizing the rest.
SNIPPET_MAX_LEN = 80  # Max characters of a decision snippet before truncation.
SNIPPET_TRUNCATE_LEN = 77  # Length of truncated snippet body (leaves room for "...").

# Number of tab-separated fields in a `git diff --name-status -M` rename row.
RENAME_ROW_FIELD_COUNT = 3  # <status>\t<old>\t<new>

# Excluded subtrees (SOUP, generated, etc.).
EXCLUDED_SUBSTRINGS: tuple[str, ...] = (
    "/third_party/",
    "/_deps/",
    "/build/",
)

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

# Regex matching one citation token inside a `@par MC/DC:` block. The
# only accepted form is `path@function_name`: it pins the decision to its
# enclosing function, so unrelated edits that shift lines never invalidate
# it, and -- having no `:line` -- it is not flagged by
# check_line_citations.py. (The legacy `path:line` form is gone; the gate
# does not accept brittle line-number anchors.)
SYMBOL_CITATION_RE = re.compile(
    r"(?P<path>(?:libs|src|port)/[A-Za-z0-9_./-]+\.c)@(?P<sym>[A-Za-z_]\w*)"
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
    return subprocess.run(  # noqa: S603  # trusted: fixed git argv
        ["git", *args],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def staged_files(
    *, suffix: str | None = None, prefixes: tuple[str, ...] | None = None
) -> list[str]:
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


def rename_map() -> dict[str, str]:
    """Map each staged rename's new path -> its pre-rename old path.

    A `git mv` followed by interior edits would otherwise make every
    compound decision in the moved file look brand-new (HEAD has no blob
    at the new path), forcing re-citation of decisions that already exist
    unchanged and are already covered by MC/DC vectors. Diffing the staged
    file against its pre-rename HEAD content restores correct "new vs
    existing" detection -- genuinely new decisions in a renamed file are
    still caught."""
    # 40% similarity: a rename that also renames many interior symbols
    # (e.g. ra8_iic_b_* -> internal_i3c_i2c_*) scores well below git's
    # default 50% threshold, so use a lower bar to still pair it with its
    # pre-rename blob. Mispairing only ever suppresses a "new" finding, so
    # a generous threshold is safe here.
    out = _git("diff", "--cached", "--name-status", "-M40%", "--diff-filter=R")
    mapping: dict[str, str] = {}
    for row in out.splitlines():
        parts = row.split("\t")
        if len(parts) == RENAME_ROW_FIELD_COUNT and parts[0].startswith("R"):
            _status, old, new = parts
            mapping[new] = old
    return mapping


# ---------------------------------------------------------------------------
# Decision detection
# ---------------------------------------------------------------------------


def _scrub(line: str) -> str:
    """Strip comment / string / char-literal contents from a single line
    so that downstream regex matches do not fire on tokens inside them."""
    line = STRING_LITERAL_RE.sub('""', line)
    line = CHAR_LITERAL_RE.sub("''", line)
    line = BLOCK_COMMENT_RE.sub("", line)
    return LINE_COMMENT_RE.sub("", line)


def compound_decision_lines(text: str) -> set[tuple[int, str]]:
    """Return the set of (line_no, scrubbed_line) tuples in `text` that
    contain at least one compound boolean operator outside of
    comments/strings. Line numbers are 1-based."""
    found: set[tuple[int, str]] = set()
    for idx, raw in enumerate(text.splitlines(), start=1):
        # Preprocessor directives (`#if` / `#elif` / `#define` ...) are
        # compile-time conditional compilation, not runtime boolean
        # decisions, so MC/DC -- a runtime coverage criterion -- does not
        # apply to them. The canonical case is the fail-closed stub-crypto
        # guard `#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_SIMULATOR_MODE)`
        # that check_stub_crypto_guarded.py mandates: its `||` selects a
        # translation unit, it is never evaluated at run time. Skip any
        # preprocessor line so it is not mistaken for a coverable decision.
        if raw.lstrip().startswith("#"):
            continue
        scrubbed = _scrub(raw)
        if COMPOUND_OP_RE.search(scrubbed):
            # Normalize whitespace + the NULL <-> nullptr swap so cosmetic
            # reformatting (or the C23 nullptr migration) in the staged
            # file does not cause false-positive "new" decisions.
            normalized = re.sub(r"\s+", " ", scrubbed.strip())
            normalized = re.sub(r"\bNULL\b", "nullptr", normalized)
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


def collect_test_citations() -> list[tuple[str, str]]:
    """Walk every `tests/test_*.c` file in the working tree (which
    includes both staged additions and already-committed tests) and
    return the ``(source_path, function_name)`` `path@function` citations
    found inside `@par MC/DC:` Doxygen blocks."""
    symbol_cites: list[tuple[str, str]] = []
    tests_dir = Path("tests")
    if not tests_dir.is_dir():
        return symbol_cites
    for tf in sorted(tests_dir.glob("test_*.c")):
        try:
            text = tf.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for block in MCDC_BLOCK_RE.findall(text):
            symbol_cites.extend(
                (m.group("path"), m.group("sym")) for m in SYMBOL_CITATION_RE.finditer(block)
            )
    return symbol_cites


def enclosing_function(src_text: str, decision_line: int) -> str | None:
    """Return the name of the function that encloses a 1-based source
    line, or None if it cannot be determined.

    Relies on the repo's clang-format style: a function-definition body
    opens with ``{`` alone on its own line at column 0, whereas control
    blocks keep their brace at end-of-line (``if (...) {``) and nested
    scopes are indented. So the nearest preceding line that is exactly
    ``{`` is the enclosing function's opening brace; the function name is
    the last identifier before the ``(`` in the signature above it."""
    lines = src_text.splitlines()
    idx = decision_line - 1
    if idx < 0 or idx >= len(lines):
        return None
    # Walk up to the function body's opening brace (a bare "{" at col 0).
    brace = idx
    while brace >= 0 and lines[brace] != "{":
        brace -= 1
    if brace < 0:
        return None
    # Assemble the signature lines just above the brace.
    sig_parts: list[str] = []
    j = brace - 1
    while j >= 0 and lines[j].strip() not in ("", "}", "};", "*/", "/*"):
        sig_parts.insert(0, lines[j])
        if "(" in lines[j]:
            break
        j -= 1
    sig = " ".join(part.strip() for part in sig_parts)
    # The function name is the identifier immediately before the first
    # parameter-list "(" in the signature.
    m = re.search(r"([A-Za-z_]\w*)\s*\(", sig)
    return m.group(1) if m else None


def has_matching_citation(
    src_path: str,
    src_line: int,
    src_text: str,
    symbol_cites: list[tuple[str, str]],
) -> bool:
    """Return True if the decision at ``src_path:src_line`` is cited by a
    ``path@function`` citation naming its enclosing function."""
    fn = enclosing_function(src_text, src_line)
    if fn is None:
        return False
    return any(path == src_path and sym == fn for path, sym in symbol_cites)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    prod_files = staged_files(suffix=".c", prefixes=PROD_PREFIXES)
    if not prod_files:
        return 0

    symbol_cites = collect_test_citations()
    renamed = rename_map()

    findings: list[tuple[str, int, str]] = []
    for path in prod_files:
        staged_text = staged_blob(path)
        head_text = head_blob(renamed.get(path, path))
        if not staged_text:
            continue
        for line_no, normalized in new_decisions(staged_text, head_text):
            if not has_matching_citation(path, line_no, staged_text, symbol_cites):
                findings.append((path, line_no, normalized))

    if findings:
        print("[FAIL] check_new_compound_has_mcdc.py: new compound boolean")
        print("       decisions are staged without an accompanying MC/DC")
        print("       test vector set in tests/test_*.c.")
        print()
        print("       Per docs/MCDC.md, every `&&` / `||` decision under")
        print("       libs/, src/, port/ must have a `test_mcdc_*` function")
        print("       in the matching tests/test_<module>.c whose")
        print("       `@par MC/DC:` block cites the decision as")
        print("       `path@function` (the enclosing function of the")
        print("       decision -- a drift-proof anchor, no line numbers).")
        print()
        print("       Offending decisions (path:line is informational):")
        for path, line_no, normalized in findings[:MAX_DISPLAYED_FINDINGS]:
            snippet = (
                normalized
                if len(normalized) <= SNIPPET_MAX_LEN
                else normalized[:SNIPPET_TRUNCATE_LEN] + "..."
            )
            print(f"         {path}:{line_no}: {snippet}")
        if len(findings) > MAX_DISPLAYED_FINDINGS:
            print(f"         ... and {len(findings) - MAX_DISPLAYED_FINDINGS} more")
        print()
        print("       Fix: add a `test_mcdc_<decision>` function in the")
        print("       matching tests/test_<module>.c with N+1 vectors and")
        print("       a `@par MC/DC:` block citing `path@function`,")
        print("       then re-stage and commit. See docs/MCDC.md for the")
        print("       worked example.")
        return 1

    print("check_new_compound_has_mcdc.py: 0 findings.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
