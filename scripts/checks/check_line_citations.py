#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""check_line_citations.py -- Reject in-tree source citations with line numbers.

Per CLAUDE.md "Code Style / Comment citations":

  Comments must NOT reference files in this repo by line number
  (e.g. `libs/foo.c:776`). Line numbers go stale on the next reformat.
  Reference the function / symbol name instead.

  External / vendor citations (HUM, FSP, RFC, datasheet) remain
  MANDATORY for any HAL register access, ISR, or driver path.

Scope (derived, not a hardcoded root list -- #358):
  Scans C / C++ source comments in every first-party C file (via
  lint_targets, so tools/ -- which the old SCAN_ROOTS tuple
  silently omitted -- is covered). Flags tokens matching
  `<file>.<ext>:<line>` inside `// ...` or `/* ... */` comments.

  Also scans every first-party Markdown (`.md`) / plain-text (`.txt`)
  doc, wherever it lives (tools/mcp, examples/**/README.md, .claude/ agent
  prompts, the repo root), not just docs/. The same regex applies; in docs
  the whole line is treated as the "comment" (no comment-span extraction).

Exemptions:
  * docs/reference/* paths (HUM PDFs etc).
  * libs/third_party/* (SOUP -- not our citations to manage).
  * Any line containing `CITES-OK: <reason>` (reason text required).
  * CHANGELOG-style "moved from <file>:NNN to ..." historical notes.
  * `// SPDX-License-Identifier:` headers and `#include` directives.
  * In docs: lines inside fenced code blocks whose info-string mentions
    a tool name (`cppcheck`, `clang`, `clang-tidy`, `llvm-cov`, `gdb`,
    `objdump`, `readelf`) -- these are tool transcripts, not citations.
  * In docs: inline-code spans (backticked) whose first token is one
    of the same tool names.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from line_citation_lex import (
    CITATION_RE,
    CITES_OK_RE,
    DOCS_REFERENCE_RE,
    MOVED_FROM_RE,
    THIRD_PARTY_RE,
    all_tracked_files,
    find_comment_spans,
    find_mcdc_reason_spans,
    is_exempt,
    line_of_offset,
)
from line_citation_lex import is_in_scope as _lex_in_scope
from lint_targets import is_build_output_path
from selftest_assert import expect, report

# Strict: cleanup wave landed; gate now blocks any new line-citation
# violation. See docs/CITATION_POLICY.md for the rule and the
# `// CITES-OK: <reason>` per-line opt-out.
WARN_ONLY_MODE = False

# Snippet display limit for violation output lines.
MAX_SNIPPET_LEN = 120
SNIPPET_TRUNCATE_LEN = 117

EXCLUDE_PREFIXES = ("libs/third_party/", "docs/reference/")
# Vendored / generated doc trees. Not first-party prose, so out of scope --
# named and reasoned rather than left to a positive root allowlist (#358).
DOC_EXCLUDE_PREFIXES = ("docs/doxygen_theme/", "libs/ra8_fonts/")
DOC_EXTS = (".md", ".txt")
TOOL_OUTPUT_TOKENS = (
    "cppcheck",
    "clang-tidy",
    "clang",
    "llvm-cov",
    "gdb",
    "objdump",
    "readelf",
)


def staged_files() -> list[str]:
    """Paths added, copied, modified or renamed in the index.

    Deletions are filtered out: a removed file has no citation left to check,
    and reading its blob would fail.
    """
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],  # noqa: S607  # trusted: fixed git argv
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def is_in_scope(path: str) -> bool:
    """Whether a source path is subject to the ban, with this tool's exclusions."""
    return _lex_in_scope(path, EXCLUDE_PREFIXES)


def is_doc_in_scope(path: str) -> bool:
    """Any first-party Markdown / plain-text doc, derived from the tree.

    Widened from the old "docs/ or repo-root only" rule (#358): tools/mcp,
    examples/**/README.md, .claude/ agent prompts and every other tracked doc
    are now scanned, so a stale ``file.c:99`` citation cannot hide in one.
    Vendored SOUP, generated doc trees and build output are the only
    subtractions.
    """
    if not path.endswith(DOC_EXTS):
        return False
    if path.startswith(EXCLUDE_PREFIXES) or path.startswith(DOC_EXCLUDE_PREFIXES):
        return False
    return not is_build_output_path(path)


def _line_is_tool_exempt(line: str) -> bool:
    """Heuristic: line looks like tool transcript output."""
    line.lstrip()
    # Inline-code span starting with a known tool token.
    # Examples:  `clang foo.c:12: warning: ...`
    #            "  `cppcheck libs/x.c:99 ...`"
    for tok in TOOL_OUTPUT_TOKENS:
        if tok in line:
            # Require the token to appear *before* the first file:line
            # match on the line (i.e. it owns/produced that citation).
            tok_pos = line.find(tok)
            m = CITATION_RE.search(line)
            if m and tok_pos < m.start():
                return True
    return False


def scan_doc_file(path: Path) -> list[tuple[int, str, str]]:
    """Scan a markdown or plain-text doc for stale-prone line citations.

    Returns a list of ``(line_no, matched_text, snippet)`` violations.

    Docs need far more exemptions than source does, because a doc legitimately
    QUOTES tool output that contains ``file:line`` -- which is a transcript,
    not a citation the reader is meant to follow.

    Exemptions:
      * Fenced code blocks whose info-string contains a tool name.
      * Lines where a tool token precedes the citation (inline tool
        transcript).
      * `CITES-OK: <reason>` opt-out.
      * `moved from <file>:NN to ...` historical notes.
      * Anchor links / heading IDs of the form `(file:NN)` are still
        considered violations -- that is exactly what we want to ban.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    violations: list[tuple[int, str, str]] = []
    in_fence = False
    fence_is_tool = False
    fence_marker = ""
    for line_no, raw in enumerate(text.splitlines(), start=1):
        stripped = raw.lstrip()
        # Fence open/close tracking (``` or ~~~).
        if not in_fence and (stripped.startswith(("```", "~~~"))):
            in_fence = True
            fence_marker = stripped[:3]
            info = stripped[3:].strip().lower()
            fence_is_tool = any(tok in info for tok in TOOL_OUTPUT_TOKENS)
            continue
        if in_fence and stripped.startswith(fence_marker):
            in_fence = False
            fence_is_tool = False
            fence_marker = ""
            continue
        if in_fence and fence_is_tool:
            continue
        for m in CITATION_RE.finditer(raw):
            matched = m.group(0)
            if DOCS_REFERENCE_RE.search(matched) or DOCS_REFERENCE_RE.search(raw):
                continue
            if THIRD_PARTY_RE.search(matched) or THIRD_PARTY_RE.search(raw):
                continue
            cok = CITES_OK_RE.search(raw)
            if cok and cok.group(1).strip():
                continue
            if MOVED_FROM_RE.search(raw):
                continue
            if _line_is_tool_exempt(raw):
                continue
            snippet = raw.strip()
            if len(snippet) > MAX_SNIPPET_LEN:
                snippet = snippet[:SNIPPET_TRUNCATE_LEN] + "..."
            violations.append((line_no, matched, snippet))
    return violations


def line_text(text: str, line_no: int) -> str:
    """The text of a 1-based line, or "" when the number is out of range.

    Returns empty rather than raising so a finding reported at a line past
    EOF (possible on a truncated read) still prints instead of aborting the
    sweep.
    """
    lines = text.splitlines()
    if 1 <= line_no <= len(lines):
        return lines[line_no - 1]
    return ""


def scan_file(path: Path) -> list[tuple[int, str, str]]:
    """Return list of (line_no, matched_text, comment_snippet) violations."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    violations: list[tuple[int, str, str]] = []
    seen: set[tuple[int, str]] = set()
    # Two citation-bearing regions: C/C++ comments, and the string reason of an
    # RA8_MCDC_DEACTIVATED(...) annotation. The reason is a string literal that
    # find_comment_spans skips, but docs/ANNOTATIONS.md promises this gate scans
    # it -- a deactivation reason anchored to a line number is DO-178C evidence
    # that rots silently (#547). Both regions share one exemption cascade, so a
    # CITES-OK on the line excuses either; dedup keeps a reason that happens to
    # sit inside a doc comment from being reported twice.
    spans = find_comment_spans(text) + find_mcdc_reason_spans(text)
    for start, end in spans:
        region = text[start:end]
        # Per-line check: walk each match, validate against the
        # specific physical line it sits on (so CITES-OK: on the same
        # line excuses it).
        for m in CITATION_RE.finditer(region):
            abs_off = start + m.start()
            line_no = line_of_offset(text, abs_off)
            matched = m.group(0)
            line = line_text(text, line_no)
            if is_exempt(matched, line):
                continue
            if (line_no, matched) in seen:
                continue
            seen.add((line_no, matched))
            snippet = line.strip()
            if len(snippet) > MAX_SNIPPET_LEN:
                snippet = snippet[:SNIPPET_TRUNCATE_LEN] + "..."
            violations.append((line_no, matched, snippet))
    return violations


def _files_to_scan() -> list[str]:
    """The file set for this run: staged paths, else the whole tree.

    Falling back to the whole tree when nothing is staged is deliberate -- a
    hook invocation with an empty index must not report a clean tree having
    looked at no files.
    """
    if "--all" in sys.argv:
        return all_tracked_files()
    staged = staged_files()
    return staged or all_tracked_files()


def _report_violations(
    repo_root: Path,
    paths: list[str],
    scan: Callable[[Path], list[tuple[int, str, str]]],
    per_file_counts: dict[str, int],
) -> int:
    """Print every violation ``scan`` finds under ``paths``; return the count.

    Takes the scanner as a parameter because source files and documentation
    are lexed differently but reported identically -- two copies of the
    reporting loop is how the two report formats drift apart.
    """
    total = 0
    for f in paths:
        path = repo_root / f
        if not path.is_file():
            continue
        viols = scan(path)
        if not viols:
            continue
        per_file_counts[f] = len(viols)
        for line_no, matched, snippet in viols:
            print(f"{f}:{line_no}: line-citation found ('{matched}'): {snippet}")
            print("       fix: replace with function/symbol name, or add `// CITES-OK: <reason>`")
            total += 1
    return total


# ---------------------------------------------------------------------------
# Selftest -- both directions, for source AND docs, plus scope assertions under
# tools/ (source and docs), silently omitted until #358.
# ---------------------------------------------------------------------------
def selftest() -> int:
    """Prove a file:line citation fires, legal forms stay quiet, and scope holds."""
    print("check_line_citations.py --selftest")
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        bad_src = Path(tmp) / "bad.c"
        bad_src.write_text("/* see libs/foo.c:123 for the layout */\n", encoding="utf-8")
        expect(bool(scan_file(bad_src)), "a file:line citation in a C comment fires", failures)
        good_src = Path(tmp) / "good.c"
        good_src.write_text(
            "/* see ra8_foo(); moved from x.c:12 to here */\n"
            "// libs/y.c:9  CITES-OK: illustrative example\n",
            encoding="utf-8",
        )
        expect(
            not scan_file(good_src),
            "symbol ref / moved-from / CITES-OK stays quiet (source)",
            failures,
        )
        bad_mcdc = Path(tmp) / "bad_mcdc.c"
        bad_mcdc.write_text(
            'RA8_MCDC_DEACTIVATED("guard justified in libs/foo.c:123")\n'
            "static inline bool internal_guard(const void* p);\n",
            encoding="utf-8",
        )
        expect(
            bool(scan_file(bad_mcdc)),
            "a file:line inside an RA8_MCDC_DEACTIVATED reason fires (#547)",
            failures,
        )
        good_mcdc = Path(tmp) / "good_mcdc.c"
        good_mcdc.write_text(
            'RA8_MCDC_DEACTIVATED("guard: ra8_pin_validator_check asserts non-null")\n'
            'RA8_MCDC_DEACTIVATED("legacy libs/foo.c:1 CITES-OK: historical note")\n',
            encoding="utf-8",
        )
        expect(
            not scan_file(good_mcdc),
            "a symbol-only reason and a CITES-OK reason stay quiet (source)",
            failures,
        )
        bad_doc = Path(tmp) / "bad.md"
        bad_doc.write_text("See `ra8_ipc_regs.h:267` for the bit.\n", encoding="utf-8")
        expect(bool(scan_doc_file(bad_doc)), "a file:line citation in a doc fires", failures)
        good_doc = Path(tmp) / "good.md"
        good_doc.write_text(
            "See ra8_ipc_regs.h SAIPCIR2. libs/z.c:3 <!-- CITES-OK: illustrative -->\n",
            encoding="utf-8",
        )
        expect(not scan_doc_file(good_doc), "doc CITES-OK stays quiet", failures)

    expect(
        is_in_scope("tools/ra8_emulator/src/main.c"),
        "tools/ C is in scope (SCAN_ROOTS omitted it before #358)",
        failures,
    )
    expect(is_doc_in_scope("tools/mcp/README.md"), "tools/ docs are in scope", failures)
    expect(
        not is_in_scope("libs/third_party/miniz/miniz.c"),
        "vendored SOUP stays out of scope",
        failures,
    )
    return report(failures)


def main() -> int:
    """Reject in-tree citations that name a file by line number.

    The rule exists because ``libs/foo.c:123`` goes stale the moment anything
    above line 123 changes, and nothing detects that it has: the reference
    still parses, still looks precise, and now points at the wrong line.
    Function and symbol names survive edits, so they are what must be cited.

    External HUM citations are unaffected -- the manual has stable page
    numbers, and citing them is mandatory elsewhere in the tree.

    Returns 1 listing each stale-prone citation, 0 when the scanned set is
    clean.
    """
    if "--selftest" in sys.argv[1:]:
        return selftest()

    repo_root = Path(
        subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],  # noqa: S607  # trusted: fixed git argv
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    )

    files = _files_to_scan()
    total_violations = 0
    per_file_counts: dict[str, int] = {}
    for scan, paths in (
        (scan_file, [f for f in files if is_in_scope(f)]),
        (scan_doc_file, [f for f in files if is_doc_in_scope(f)]),
    ):
        total_violations += _report_violations(repo_root, paths, scan, per_file_counts)

    if total_violations == 0:
        return 0

    print(file=sys.stderr)
    print(
        f"check_line_citations: {total_violations} violation(s) across "
        f"{len(per_file_counts)} file(s).",
        file=sys.stderr,
    )
    if WARN_ONLY_MODE:
        print(
            "check_line_citations: WAVE 0 -- warn-only, not blocking commit.",
            file=sys.stderr,
        )
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
