#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Gate + fixer: house style for single-line C/C++ block comments.

clang-format owns the *start* column of trailing comments (``AlignTrailing
Comments: true`` aligns each block's ``/**<`` to the widest code + one space)
but never touches the comment *interior* (``ReflowComments: false``).  This
pass runs *after* clang-format and owns the interior, normalising three things:

1. **Space after the opener.**  ``/*foo`` -> ``/* foo`` (the Doxygen forms
   ``/**`` / ``/**<`` likewise gain a single space before their text).
2. **Space before the closer.**  ``foo.*/`` -> ``foo. */`` -- one space before
   ``*/`` unless the comment is widened for end alignment (rule 3).
3. **Aligned ``*/`` end column.**  Across a run of consecutive trailing comments
   that clang-format placed in the *same start column*, the closers are padded
   to line up under the longest (which gets one space before ``*/``).  A run is
   broken by a blank line, a code-only line, a standalone comment, a change of
   start column, or where padding would reach the column limit (which would make
   clang-format collapse its leading alignment).  Standalone full-line comments
   are spacing-normalised but never end-aligned.
4. **One block, one alignment.**  Rule 3 *accommodates* a torn block; this rule
   forbids one.  ``AlignTrailingComments`` aligns a run to its widest code plus
   one space but abandons that column when the longest comment would cross the
   limit, leaving one struct with two ``/**<`` columns and two ``*/`` columns.
   That is canonical clang-format output, so neither tool objects to it, and
   ragged blocks accumulated in the tree unremarked.  A run must therefore align
   as a single unit.  This rule is REPORTED, never fixed: the remedy is to
   shorten the comment or move it to its own ``/** ... */`` block above the line
   it documents, and neither is a rewrite a formatter may make on its own.  A
   reported block IS exempted from rule 3, though -- end-padding is a grouping
   hint clang-format honours, so padding a torn block to its two sub-widths
   would keep it torn even after the author shortened the comment that tore it.

The text in front of each comment -- code plus clang-format's leading alignment
-- is preserved verbatim, so the two tools never fight: clang owns the start
column, this pass owns the interior + the ``*/`` column.

Only **single-line** block comments are touched (opener and matching ``*/`` on
the same physical line).  Multi-line ``/** ... */`` blocks, ``//`` line
comments, string / char / raw-string literals, decorative banners, and inline
mid-code comments (``f(/*tag=*/x)``) are left byte-for-byte alone.  The pass is
idempotent (re-running ``--fix`` is a no-op) and clang-format-stable (a
clang-format re-run leaves the result alone).

Run::

    check_comment_format.py                 # gate the whole tree (exit 1 on drift)
    check_comment_format.py path ...        # gate only the listed files/dirs
    check_comment_format.py --fix [path...] # rewrite in place
    check_comment_format.py --selftest      # run the built-in test battery

Exit 0 if clean (or fixed), exit 1 on findings in check mode, exit 2 on a
selftest failure or a whole-tree sweep that collapsed below FILE_FLOOR.
"""

from __future__ import annotations

import sys
from collections.abc import Iterable
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lint_targets import is_build_output_path

REPO_ROOT = Path(__file__).resolve().parents[2]

SOURCE_SUFFIXES = (
    ".c",
    ".h",
    ".cpp",
    ".hpp",
    ".cc",
    ".cxx",
    ".hh",
    ".hxx",
    ".m",
    ".mm",
    ".inl",
)
# Mirror scripts/checks/format_code.sh's scope: this pass runs *after* clang-format and
# only makes sense where clang-format already aligned the comment starts.
# format_code.sh formats libs/ src/ tests/ examples/ tools/ (not port/), so the
# no-argument scan here matches; format_code.sh also drives this tool with an
# explicit file list, which is the authoritative gate path.
SCAN_ROOTS = ("libs", "src", "port", "examples", "tools", "tests")
EXCLUDE_FRAGMENTS = (
    "libs/third_party/",
    "libs/ra8_fonts/",
    "port/threadx/",
)

# A tree this size cannot legitimately collapse to a handful of files. If the
# whole-tree sweep returns less than this, something broke (an unreachable repo
# root, a renamed SCAN_ROOTS entry) and reporting "comments well-formed" would
# be a lie -- the old `no files to scan` branch exited 0 on exactly that.
# Measured 2026-07-28: 2124 first-party C/C++ sources. Same trip-wire as
# check_ruff.py.
FILE_FLOOR = 1700

# Scan-state for the per-line C/C++ tokeniser.
_ST_NORMAL = 0
_ST_BLOCK = 1  # inside a multi-line /* ... */ block comment
_ST_RAW = 2  # inside a C++ raw string R"delim( ... )delim"

# Must match .clang-format ColumnLimit. End-alignment never pads a comment PAST
# this column: a run splits instead, because a trailing comment that overruns the
# limit makes clang-format collapse its (clang-owned) leading alignment to fit,
# which would fight this pass.
#
# The bound is inclusive. ColumnLimit permits a line of exactly this width --
# clang-format-22 leaves a 100-column trailing comment alone and only wraps the
# declaration at 101 -- so a guard that stopped one short of it tore blocks in
# two for no reason, which is how 47 of them ended up in the tree.
_COLUMN_LIMIT = 100

# How many offending lines a split-run finding names before eliding the rest.
_MAX_NAMED_LINES = 6

# A line opening with one of these is never part of clang-format's trailing
# comment alignment sequence: a scope-closing brace, or a preprocessor
# directive.
_OUT_OF_SEQUENCE = ("}", "#")

# One trailing comment is a run of one, and a run of one always aligns.
_MIN_RUN = 2


class _Comment(NamedTuple):
    """One single-line block comment found in code (NORMAL) context.

    `start`/`end` are character indices into the physical line such that
    ``line[start:end]`` is the full ``/* ... */`` text.  `opener` is the
    normalised opener token (``/*`` / ``/**`` / ``/**<`` / ``/*!`` / ``/*!<``)
    and `content` is the inner prose with leading/trailing whitespace stripped
    and internal whitespace preserved.  `processable` is False for banners /
    empties that must be left untouched.
    """

    start: int
    end: int
    opener: str
    content: str
    processable: bool


def _classify(text: str) -> tuple[str, str, bool]:
    """Split a ``/* ... */`` comment into (opener, content, processable).

    `text` is the full comment including the ``/*`` and ``*/`` delimiters.
    Returns the normalised opener token, the stripped inner content, and a
    processability flag that is False for empty comments and decorative
    banners (which are returned verbatim by the caller).
    """
    inner = text[2:-2]  # drop the leading /* and trailing */
    if not inner.strip():
        return ("/*", inner, False)
    if "\t" in inner:
        return ("/*", inner, False)  # tabs break column math -- leave alone

    if inner.startswith("*<"):
        opener, rest = "/**<", inner[2:]
    elif inner.startswith("!<"):
        opener, rest = "/*!<", inner[2:]
    elif inner.startswith("**"):
        # /***... banner -- not a Doxygen block; leave untouched.
        return ("/*", inner, False)
    elif inner.startswith("*"):
        opener, rest = "/**", inner[1:]
    elif inner.startswith("!"):
        opener, rest = "/*!", inner[1:]
    else:
        opener, rest = "/*", inner

    content = rest.strip()
    if not content:
        return (opener, content, False)
    # Decorative leading/trailing asterisks (banners, dividers): leave alone.
    if content.startswith("*") or content.endswith("*"):
        return (opener, content, False)
    return (opener, content, True)


def _scan_line(line: str, state: int, raw_delim: str) -> tuple[list[_Comment], int, str]:
    """Tokenise one line, returning (single-line comments, new_state, raw_delim).

    Tracks string/char/raw-string literals, ``//`` line comments, and multi-line
    ``/* */`` blocks so that ``/*`` and ``*/`` sequences inside literals or other
    comments are never mistaken for a comment delimiter.
    """
    comments: list[_Comment] = []
    i = 0
    n = len(line)

    if state == _ST_BLOCK:
        end = line.find("*/")
        if end == -1:
            return (comments, _ST_BLOCK, raw_delim)
        i = end + 2
    elif state == _ST_RAW:
        close = ")" + raw_delim + '"'
        end = line.find(close)
        if end == -1:
            return (comments, _ST_RAW, raw_delim)
        i = end + len(close)

    while i < n:
        c = line[i]
        nxt = line[i + 1] if i + 1 < n else ""

        if c == "/" and nxt == "/":
            break  # line comment: rest of the line is not code
        if c == "/" and nxt == "*":
            close = line.find("*/", i + 2)
            if close == -1:
                return (comments, _ST_BLOCK, raw_delim)  # opens a multi-line block
            text = line[i : close + 2]
            opener, content, processable = _classify(text)
            comments.append(_Comment(i, close + 2, opener, content, processable))
            i = close + 2
            continue
        if c == '"':
            delim = _raw_string_delim(line, i)
            if delim is not None:
                close = ")" + delim + '"'
                end = line.find(close, i + 2 + len(delim))
                if end == -1:
                    return (comments, _ST_RAW, delim)
                i = end + len(close)
                continue
            i = _skip_quoted(line, i, '"')
            continue
        if c == "'":
            i = _skip_quoted(line, i, "'")
            continue
        i += 1

    return (comments, _ST_NORMAL, "")


def _raw_string_delim(line: str, quote_idx: int) -> str | None:
    """If ``line[quote_idx]`` opens a C++ raw string, return its ``(``-delimiter.

    A raw string is ``R"delim(`` with an optional encoding prefix (``u8``, ``L``,
    ``u``, ``U``) on the ``R``.  Returns None when this quote is a normal string.
    """
    if quote_idx == 0 or line[quote_idx - 1] != "R":
        return None
    # Verify the R is a raw-string introducer, not an identifier character.
    j = quote_idx - 1
    prefix_start = j
    while prefix_start > 0 and line[prefix_start - 1] in "uUL8":
        prefix_start -= 1
    if prefix_start > 0 and (line[prefix_start - 1].isalnum() or line[prefix_start - 1] == "_"):
        return None
    open_paren = line.find("(", quote_idx + 1)
    if open_paren == -1:
        return None
    return line[quote_idx + 1 : open_paren]


def _skip_quoted(line: str, i: int, quote: str) -> int:
    """Return the index just past a ``quote``-delimited literal starting at `i`."""
    j = i + 1
    n = len(line)
    while j < n:
        if line[j] == "\\":
            j += 2
            continue
        if line[j] == quote:
            return j + 1
        j += 1
    return n  # unterminated on this line; treat the rest as consumed


def _render(opener: str, content: str, pad: int) -> str:
    """Build a normalised comment: opener, one space, content, `pad` spaces, ``*/``."""
    return f"{opener} {content}{' ' * pad}*/"


def _body_len(c: _Comment) -> int:
    """Width of the minimal (one-space) comment ``/**< content */`` for `c`."""
    return len(c.opener) + 1 + len(c.content) + 1 + 2


def _scan_trailing(
    lines: list[str],
) -> tuple[list[_Comment | None], list[str], list[int], list[tuple[int, str]]]:
    """Classify every line's last block comment as trailing, standalone, or neither.

    Returns four parallel results: per line, the trailing comment (``None`` when
    the line has none), the verbatim text in front of it (code plus
    clang-format's leading alignment), and the column that comment opens at
    (``-1`` when there is none); plus a list of ``(index, rendered)`` pairs for
    full-line standalone comments, which are spacing-normalised but never
    aligned.

    A line whose last comment is a banner or an empty ``/**/``, or which has
    code after the ``*/``, yields no trailing comment: neither this pass nor
    clang-format's ``AlignTrailingComments`` treats those as alignable, so both
    end an alignment run there.
    """
    trailing: list[_Comment | None] = [None] * len(lines)
    prefix: list[str] = [""] * len(lines)
    start_col: list[int] = [-1] * len(lines)
    standalone: list[tuple[int, str]] = []

    state, raw_delim = _ST_NORMAL, ""
    for i, line in enumerate(lines):
        comments, state, raw_delim = _scan_line(line, state, raw_delim)
        if not comments:
            continue
        last = comments[-1]
        if not last.processable or line[last.end :].strip() != "":
            continue  # banner/empty, or code follows -> leave the line untouched
        before = line[: last.start]
        if before.strip() == "":
            standalone.append((i, before + _render(last.opener, last.content, 1)))
        else:
            if not before[-1].isspace():
                before += " "  # rule 3: >=1 space before a trailing comment
            trailing[i] = last
            prefix[i] = before  # preserve code + clang's start-column alignment
            start_col[i] = len(before)
    return trailing, prefix, start_col, standalone


def _clang_format_off(lines: list[str]) -> set[int]:
    """Return the indices of lines clang-format has been told to leave alone.

    Everything from a ``clang-format off`` marker up to the matching ``on`` (or
    end of file) is emitted verbatim, so no alignment rule can be read off it:
    the columns there are whatever the author typed. Reporting a torn block
    inside such a region asks for a repair clang-format would never make -- and
    in one case for a comment whose budget was NEGATIVE, since the author had
    switched the formatter off precisely to keep an over-long marker on its
    line.
    """
    off: set[int] = set()
    active = False
    for i, line in enumerate(lines):
        marker = _fmt_toggle(line)
        if marker == "off":
            active = True
        elif marker == "on":
            active = False
            continue
        if active:
            off.add(i)
    return off


def _fmt_toggle(line: str) -> str | None:
    """Return ``"off"`` / ``"on"`` when `line` is a clang-format toggle comment.

    Both delimiters count, and so does the ``clang-format off: why`` form --
    clang-format honours a trailing explanation, and this tree uses it, so a
    matcher demanding the bare spelling would read a live marker as ordinary
    prose and police a region the formatter never touched.
    """
    stripped = line.strip()
    if stripped.startswith("//"):
        body = stripped[2:].strip()
    elif stripped.startswith("/*") and stripped.endswith("*/"):
        body = stripped[2:-2].strip()
    else:
        return None
    for word in ("off", "on"):
        if body == f"clang-format {word}" or body.startswith(f"clang-format {word}:"):
            return word
    return None


class _SplitRun(NamedTuple):
    """A block of trailing comments that cannot be aligned as a single unit.

    `first`/`last` are the 1-based line numbers bounding the run.  `cols` holds
    the distinct 1-based columns its ``/**<`` openers start at -- more than one
    means clang-format gave up and split the block.  `over` lists the 1-based
    lines whose minimal comment is too wide to sit at `col`, and `excess` is how
    many characters the widest of them must lose.
    """

    first: int
    last: int
    cols: tuple[int, ...]
    col: int
    over: tuple[int, ...]
    excess: int


def find_split_runs(text: str) -> list[_SplitRun]:
    """Report every trailing-comment block the column limit tore in two.

    A *run* is a maximal stretch of consecutive lines that each end in a
    single-line trailing comment.  A blank line, a code-only line, a standalone
    comment, or an inline mid-code comment ends one -- and ends clang-format's
    alignment sequence too, so a run is exactly the unit clang-format tries to
    align.

    ``AlignTrailingComments`` aligns a run to its widest code plus one space,
    but abandons that column and starts a fresh group when it would push the
    longest comment past ``ColumnLimit``.  The result reads as ragged: one
    struct, two ``/**<`` columns and two ``*/`` columns.  This pass cannot
    repair it -- shortening prose is the author's call -- so it reports.

    A run is reported when its comments open at more than one column, or when
    the widest one, placed at the run's rightmost column, would reach the limit
    (which is what splits the closing ``*/`` alignment on the next run of the
    formatter).  Both are the same defect measured before and after
    clang-format has reacted to it.
    """
    lines = text.removesuffix("\n").split("\n")
    trailing, _prefix, start_col, _standalone = _scan_trailing(lines)
    for i in _clang_format_off(lines):
        trailing[i] = None  # clang-format aligns nothing here, so neither do we

    found: list[_SplitRun] = []
    for run in _alignment_runs(lines, trailing):
        cols = sorted({start_col[k] for k in run})
        col = cols[-1]
        over = tuple(k + 1 for k in run if col + _body_len(trailing[k]) > _COLUMN_LIMIT)
        if len(cols) > 1 or over:
            widest = max(_body_len(trailing[k]) for k in run)
            found.append(
                _SplitRun(
                    first=run[0] + 1,
                    last=run[-1] + 1,
                    cols=tuple(c + 1 for c in cols),
                    col=col + 1,
                    over=over,
                    excess=max(0, col + widest - _COLUMN_LIMIT),
                )
            )
    return found


def _alignment_runs(lines: list[str], trailing: list[_Comment | None]) -> list[list[int]]:
    """Group line indices into the blocks clang-format aligns as one.

    A block is a maximal stretch of consecutive trailing-comment lines at ONE
    indentation, and it excludes two kinds of line that clang-format keeps out
    of an alignment sequence regardless of how wide anything is:

    * a line that CLOSES a scope (``}``, ``};``, ``} while (x);``) -- with
      ``int a; /* a */`` inside a block and ``} /* done */`` after it,
      clang-format leaves the two comments in unrelated columns;
    * a preprocessor directive.

    Modelling the unit matters more than it sounds. Judging a whole struct as
    one block, when clang-format was aligning its members and its anonymous
    union's closing ``};`` separately all along, reports a tear that is not
    there and asks for a repair no formatter would ever make. Blocks at
    different indentation are likewise judged apart: clang-format sometimes
    aligns across an indent change, so treating them as one unit could only
    manufacture findings.
    """
    runs: list[list[int]] = []
    cur: list[int] = []
    indent = -1
    for i, line in enumerate(lines):
        stripped = line.lstrip()
        breaks = (
            trailing[i] is None
            or stripped.startswith(_OUT_OF_SEQUENCE)
            or (cur and len(line) - len(stripped) != indent)
        )
        if breaks:
            if len(cur) >= _MIN_RUN:
                runs.append(cur)
            cur = []
            if trailing[i] is None or stripped.startswith(_OUT_OF_SEQUENCE):
                continue
        if not cur:
            indent = len(line) - len(stripped)
        cur.append(i)
    if len(cur) >= _MIN_RUN:
        runs.append(cur)
    return runs


def fix_text(text: str) -> str:
    """Return `text` with the comment-format rules applied. Pure; idempotent.

    Only the LAST block comment on a line is ever touched, and only when nothing
    but whitespace follows it.  That covers trailing comments (code before them)
    and full-line standalone comments (only whitespace before).  Inline mid-code
    comments (``f(/*tag=*/x)``) are left byte-for-byte alone.

    The text in front of a trailing comment (code + clang-format's start-column
    alignment) is preserved verbatim -- clang-format owns the comment start
    column.  This pass only rewrites each comment from ``/**<`` to ``*/``: one
    space after the opener, one space before the closer, and, across a run of
    consecutive trailing comments that clang-format put in the same start
    column, the closing ``*/`` padded so they line up under the longest (which
    gets one space).  A run is also split where end-alignment would reach the
    column limit, so the padding never makes clang-format reflow the line.
    Standalone full-line comments are spacing-normalised but never aligned.
    """
    had_trailing_nl = text.endswith("\n")
    body = text[:-1] if had_trailing_nl else text
    lines = body.split("\n")
    out = list(lines)

    trailing, prefix, start_col, standalone = _scan_trailing(lines)
    for i, rendered in standalone:
        out[i] = rendered

    # Never cement a block this pass is REPORTING as torn (rule 4). End-padding
    # is a grouping hint clang-format honours, so a block padded to its two
    # sub-widths stays in two groups even after the comment that split it has
    # been shortened -- the author does as the finding asks, runs `make format`,
    # and nothing moves. Rendering a reported block at one space instead lets
    # the next clang-format round see the minimal form and re-merge it.
    cemented = {k for run in find_split_runs(text) for k in range(run.first - 1, run.last)}

    # End-align consecutive trailing comments clang put in the same start column.
    i = 0
    while i < len(lines):
        if trailing[i] is None:
            i += 1
            continue
        if i in cemented:
            c = trailing[i]
            out[i] = prefix[i] + _render(c.opener, c.content, 1)
            i += 1
            continue
        j = i
        body_hi = _body_len(trailing[i])
        while (
            j + 1 < len(lines) and trailing[j + 1] is not None and start_col[j + 1] == start_col[i]
        ):
            nbody = max(body_hi, _body_len(trailing[j + 1]))
            if start_col[i] + nbody > _COLUMN_LIMIT:  # padding would overrun the limit
                break
            body_hi = nbody
            j += 1
        for k in range(i, j + 1):
            c = trailing[k]
            pad = body_hi - _body_len(c) + 1
            out[k] = prefix[k] + _render(c.opener, c.content, pad)
        i = j + 1

    result = "\n".join(out)
    return result + "\n" if had_trailing_nl else result


def _is_excluded(path: Path) -> bool:
    return is_build_output_path(path) or any(frag in str(path) for frag in EXCLUDE_FRAGMENTS)


def _is_source(path: Path) -> bool:
    return path.suffix in SOURCE_SUFFIXES


def _rel(path: Path) -> str:
    if path.is_relative_to(REPO_ROOT):
        return str(path.relative_to(REPO_ROOT))
    return str(path)


def _enumerate_targets(arg_paths: Iterable[str]) -> list[Path]:
    args = list(arg_paths)
    if args:
        out: list[Path] = []
        for raw in args:
            path = Path(raw)
            if not path.is_absolute():
                path = REPO_ROOT / path
            if path.is_dir():
                out.extend(c for c in path.rglob("*") if c.is_file() and _is_source(c))
            elif _is_source(path):
                out.append(path)
        return [p for p in out if not _is_excluded(p)]

    out = []
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if base.is_dir():
            out.extend(c for c in base.rglob("*") if c.is_file() and _is_source(c))
    return [p for p in out if not _is_excluded(p)]


def _first_diff_lines(old: str, new: str, limit: int = 4) -> list[tuple[int, str, str]]:
    """Return up to `limit` (1-based lineno, old, new) tuples that differ."""
    olines = old.split("\n")
    nlines = new.split("\n")
    diffs = []
    for idx, (o, n) in enumerate(zip(olines, nlines, strict=False), start=1):
        if o != n:
            diffs.append((idx, o, n))
            if len(diffs) >= limit:
                break
    return diffs


def main(argv: list[str]) -> int:
    """Report, or with ``--fix`` rewrite, comment blocks whose interior is misaligned.

    Three modes, checked in that order: ``--selftest`` runs the built-in
    battery and ignores everything else on the line, ``--fix`` rewrites in
    place and reports how many files changed, and the default reports offenders
    and fails. Only ``--fix`` writes; the default mode leaves the tree alone.

    A file that cannot be decoded as UTF-8 is skipped silently rather than
    failed -- comment alignment is not the gate that should adjudicate
    encoding, and check-encoding already owns that and would report it twice.

    FILE_FLOOR applies to the whole-tree sweep ONLY, and exits 2 below it. An
    explicit path list is how format_code.sh and the pre-commit hook drive this
    tool, and it legitimately filters to nothing when a commit touches no C
    source; an empty SWEEP is a broken enumeration reporting well-formed
    comments because it read nothing.

    Returns 0 when clean or when ``--fix`` rewrote everything it found, 1 when
    the default mode found a file needing a rewrite, 2 when the whole-tree
    sweep enumerated too few files to trust.
    """
    args = argv[1:]
    if "--selftest" in args:
        return _selftest()
    do_fix = "--fix" in args
    paths = [a for a in args if not a.startswith("--")]

    targets = _enumerate_targets(paths)
    if not paths and len(targets) < FILE_FLOOR:
        sys.stderr.write(
            f"check_comment_format.py: FATAL -- only {len(targets)} file(s) in scope, "
            f"floor is {FILE_FLOOR}.\n"
            "  A collapsed sweep reports well-formed comments because it read nothing.\n"
        )
        return 2
    if not targets:
        print("check_comment_format.py: no files to scan")
        return 0

    bad: list[Path] = []
    split: list[tuple[Path, _SplitRun]] = []
    fixed = 0
    for path in sorted(targets):
        try:
            original = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        updated = fix_text(original)
        if updated != original:
            if do_fix:
                path.write_text(updated, encoding="utf-8")
                fixed += 1
            else:
                bad.append(path)
        split.extend((path, run) for run in find_split_runs(updated))

    if do_fix:
        print(f"check_comment_format.py: reformatted {fixed} file(s).")
    elif bad:
        _report_rewrites(bad)

    if split:
        _report_split_runs(split)
    if bad or split:
        return 1
    print(f"check_comment_format.py: {len(targets)} file(s) scanned, comments well-formed.")
    return 0


def _report_rewrites(bad: list[Path]) -> None:
    """Write the first few lines this pass would rewrite in each file, to stderr."""
    sys.stderr.write("check_comment_format.py: comment-format finding(s):\n")
    for path in bad:
        rel = _rel(path)
        original = path.read_text(encoding="utf-8")
        for lineno, old, new in _first_diff_lines(original, fix_text(original)):
            sys.stderr.write(
                f"  {rel}:{lineno}\n      have: {old.rstrip()}\n      want: {new.rstrip()}\n"
            )
    sys.stderr.write("\nRun: make format  (or check_comment_format.py --fix)\n")


def _report_split_runs(split: list[tuple[Path, _SplitRun]]) -> None:
    """Write the split-run findings, and how to clear them, to stderr."""
    sys.stderr.write(
        f"check_comment_format.py: {len(split)} trailing-comment block(s) too wide to align:\n"
    )
    for path, run in split:
        rel = _rel(path)
        where = f"  {rel}:{run.first}-{run.last}"
        if len(run.cols) > 1:
            cols = ", ".join(str(c) for c in run.cols)
            sys.stderr.write(f"{where}: /**< opens at columns {cols} -- clang-format split it\n")
        else:
            sys.stderr.write(f"{where}: at column {run.col} the */ alignment splits\n")
        if run.over:
            lines = ", ".join(str(line) for line in run.over[:_MAX_NAMED_LINES])
            more = ", ..." if len(run.over) > _MAX_NAMED_LINES else ""
            sys.stderr.write(
                f"      too wide at column {run.col}: line(s) {lines}{more}"
                f" -- shed {run.excess} char(s)\n"
            )
    sys.stderr.write(
        "\nOne block of trailing comments must align as one: a single /**< column and\n"
        "a single */ column. clang-format abandons the alignment when the widest code\n"
        f"plus the longest comment would reach column {_COLUMN_LIMIT}, and this pass\n"
        "cannot shorten prose for you. Either tighten the comments named above, or\n"
        "move the long one to its own /** ... */ block above the line it documents.\n"
    )


# ---------------------------------------------------------------------------
# Built-in test battery (run via --selftest; mirrors the user's examples).
# ---------------------------------------------------------------------------
# --- selftest fixtures ------------------------------------------------------
# The table below is DATA. It lives at module scope because a function wrapping
# a 90-line literal is still a 90-line function -- the size rule measures the
# body either way -- and because chopping one cohesive spec into arbitrary
# sub-tables would cost a reader the ability to scan every rule at once.
# Each entry is (name, input, expected output).

# Rule 4: align a run of trailing comments to the longest (which gets 1 space).
_RUN_IN = (
    "enum {\n"
    "  a = 0x01U, /**< Sync event: VSYNC start. */\n"
    "  b = 0x08U, /**< End of Transmission packet. */\n"
    "};\n"
)
_RUN_OUT = (
    "enum {\n"
    "  a = 0x01U, /**< Sync event: VSYNC start.    */\n"
    "  b = 0x08U, /**< End of Transmission packet. */\n"
    "};\n"
)

# Over-padded run gets tightened so the longest has exactly one space.
_OVER_IN = "enum {\n  a = 1, /**< short.        */\n  b = 2, /**< longer text.  */\n};\n"
_OVER_OUT = "enum {\n  a = 1, /**< short.       */\n  b = 2, /**< longer text. */\n};\n"

# The leading spaces (clang-format's start-column alignment) are preserved
# verbatim, and comments at different start columns are not cross-aligned --
# only the comment interior and the */ end column are this pass's business.
_SEP_IN = "  k_a = 1,   /**< x.*/\n  k_bb = 2, /**< yy. */\n"
_SEP_OUT = "  k_a = 1,   /**< x. */\n  k_bb = 2, /**< yy. */\n"

# A code-only line breaks the run; the standalone doc line collapses to one
# space; the trailing comments keep their (clang-owned) leading spaces.
_BREAK_IN = (
    "enum {\n"
    "  a = 1U,  /**< one.   */\n"
    "  b = 2U,  /**< two.   */\n"
    "  c = (1U << 9),\n"
    "  /**< composite.                    */\n"
    "};\n"
)
_BREAK_OUT = (
    "enum {\n"
    "  a = 1U,  /**< one. */\n"
    "  b = 2U,  /**< two. */\n"
    "  c = (1U << 9),\n"
    "  /**< composite. */\n"
    "};\n"
)

# Column-limit guard: end-alignment that would reach the limit splits the run
# instead, so a short comment is never padded out to a far column (which would
# make clang-format collapse the leading alignment). `a` is tightened to one
# space rather than aligned to the long sibling's */.
_LONG_C = "x" * 84
_CAP_IN = f"  a = 1, /**< short.        */\n  b = 2, /**< {_LONG_C}. */\n"
_CAP_OUT = f"  a = 1, /**< short. */\n  b = 2, /**< {_LONG_C}. */\n"

# A block rule 4 reports is rendered at one space, never padded to its two
# sub-widths: padding is a grouping hint, and cementing the split would keep
# clang-format from re-merging the block once the long comment is shortened.
_CEMENT_IN = "  ra8_mount_t* aa; /**< short.       */\n  uint32_t bb;   /**< " + "x" * 78 + ". */\n"
_CEMENT_OUT = "  ra8_mount_t* aa; /**< short. */\n  uint32_t bb;   /**< " + "x" * 78 + ". */\n"

# Safety: multi-line block comment interior untouched.
_MULTI = "/**\n * @brief foo.*/bar\n * body\n */\nint x;\n"

_SELFTEST_CASES: tuple[tuple[str, str, str], ...] = (
    # Rule 1: space after the opener.
    ("space after /*", "int x; /*hi */\n", "int x; /* hi */\n"),
    # Rule 2: space before */.
    ("space before */", "int x; /* hi*/\n", "int x; /* hi */\n"),
    (
        "doxy member . */",
        "bool e; /**< DSISETR.EOTPEN.*/\n",
        "bool e; /**< DSISETR.EOTPEN. */\n",
    ),
    # Rule 3: >=1 space before a trailing comment.
    ("space before comment", "int x;/* hi */\n", "int x; /* hi */\n"),
    ("align run to longest", _RUN_IN, _RUN_OUT),
    ("tighten over-padded run", _OVER_IN, _OVER_OUT),
    ("leading preserved, columns separate", _SEP_IN, _SEP_OUT),
    ("code line breaks run", _BREAK_IN, _BREAK_OUT),
    ("column-limit splits run", _CAP_IN, _CAP_OUT),
    ("a reported block is left un-padded", _CEMENT_IN, _CEMENT_OUT),
    # Safety: never touch text inside string literals.
    ("string with /* */", 'const char* s = "/*x*/";\n', 'const char* s = "/*x*/";\n'),
    ("string with */", 'puts("a*/b");\n', 'puts("a*/b");\n'),
    # Safety: never touch // line comments.
    ("line comment left alone", "int x; // a*/b\n", "int x; // a*/b\n"),
    ("multiline block untouched", _MULTI, _MULTI),
    # Safety: banners untouched.
    ("banner untouched", "/******** section ********/\n", "/******** section ********/\n"),
    ("empty comment untouched", "x; /**/\n", "x; /**/\n"),
    # Safety: C++ raw string with */ inside is not a comment.
    ("raw string untouched", 'auto s = R"(a*/b/*c)";\n', 'auto s = R"(a*/b/*c)";\n'),
    # Internal double-space (manual sub-column alignment) is preserved.
    (
        "internal spacing kept",
        "x = 1; /**< VBTBPSR  Ch 12.2 p 509.*/\n",
        "x = 1; /**< VBTBPSR  Ch 12.2 p 509. */\n",
    ),
    # Inline mid-code comments (code follows the */) are left byte-for-byte
    # alone -- clang-format owns the spacing around them; rewriting them fights
    # it (e.g. clang then wants a space between */ and the next token).
    ("inline arg-label spaced", "f(a, /* tag= */ b);\n", "f(a, /* tag= */ b);\n"),
    ("inline arg-label tight", "f(a, /*tag=*/b);\n", "f(a, /*tag=*/b);\n"),
    ("inline then trailing", "f(/*a*/x); /*hi*/\n", "f(/*a*/x); /* hi */\n"),
)


# --- split-run detector fixtures -------------------------------------------
# Must-fire: one struct, two /**< columns, because `b`'s comment is too long to
# sit at the column the wider declaration above it would impose.
_SPLIT_TWO_COLS = (
    f"struct s {{\n  ra8_mount_t* aa; /**< short. */\n  uint32_t bb;   /**< {'x' * 78}. */\n}};\n"
)
# Must-fire on the column clause ALONE: two /**< columns, every comment short.
# This is `_SEP_IN` above, seen from the other side -- fix_text refuses to
# cross-align the two, and this rule is what says the tree should not contain
# them in the first place.
_SPLIT_COLS_ONLY = "  k_a = 1,   /**< x. */\n  k_bb = 2, /**< yy. */\n"
# Must-fire on the width clause ALONE: one column, but the widest comment
# reaches the limit, so the */ alignment (not the /**< alignment) is what splits.
_SPLIT_ONE_COL = f"  a = 1, /**< short. */\n  b = 2, /**< {'x' * 88}. */\n"

# Must stay quiet: a plain aligned block.
_QUIET_RUN = "struct s {\n  uint32_t a; /**< one. */\n  uint32_t b; /**< two. */\n};\n"
# Must stay quiet: the columns differ, but a code-only line sits between them,
# which ends clang-format's alignment sequence as well as this pass's run.
_QUIET_BREAK = (
    "struct s {\n"
    "  const paint_t* a; /**< one. */\n"
    "  void (*cb)(int x, int y);\n"
    "  uint32_t b; /**< two. */\n"
    "};\n"
)
# Must stay quiet: a blank line ends the run just as firmly.
_QUIET_BLANK = "struct s {\n  const paint_t* a; /**< one. */\n\n  uint32_t b; /**< two. */\n};\n"
# Must stay quiet: a lone trailing comment is a run of one and always aligns.
_QUIET_SINGLE = f"  uint32_t a; /**< {'x' * 60}. */\n"
# Must stay quiet: clang-format was told to leave the region alone, so its
# columns are the author's, not an alignment clang-format gave up on.
_QUIET_FMT_OFF = "// clang-format off\n" + _SPLIT_TWO_COLS + "// clang-format on\n"
# The `off: why` form is live in this tree and clang-format honours it.
_QUIET_FMT_OFF_WHY = (
    "// clang-format off: the marker must stay on the call line.\n" + _SPLIT_TWO_COLS
)
# ...but the exemption must END at the `on` marker, or one `off` anywhere in a
# file would silence the rest of it.
_SPLIT_AFTER_ON = "// clang-format off\nint x; /* a */\n// clang-format on\n" + _SPLIT_TWO_COLS

# Must stay quiet: clang-format keeps a scope-closing brace out of the alignment
# sequence, so its comment column says nothing about the members above it.
# Each of the three fixtures below isolates ONE reason a run ends, so that
# dropping that one reason is what makes it fire.
_QUIET_BRACE = "  } /* close */\n  return; /* ret */\n"
# Must stay quiet: a preprocessor directive is out of the sequence too. Same
# indent as its neighbour, so only the directive itself can end the run.
_QUIET_PREPROC = "int aaaaaa; /* one */\n#endif /* end */\n"
# Must stay quiet: a change of indentation is judged apart, because
# clang-format sometimes aligns across one and sometimes does not.
_QUIET_INDENT = "  int a; /* one */\n    int bb; /* two */\n"

_SPLIT_CASES: tuple[tuple[str, str, int], ...] = (
    ("two /**< columns reported", _SPLIT_TWO_COLS, 1),
    ("two columns alone are enough", _SPLIT_COLS_ONLY, 1),
    ("one column, */ alignment splits", _SPLIT_ONE_COL, 1),
    ("aligned block stays quiet", _QUIET_RUN, 0),
    ("code line ends the run", _QUIET_BREAK, 0),
    ("blank line ends the run", _QUIET_BLANK, 0),
    ("single comment stays quiet", _QUIET_SINGLE, 0),
    ("clang-format off exempts the region", _QUIET_FMT_OFF, 0),
    ("the off: why spelling exempts too", _QUIET_FMT_OFF_WHY, 0),
    ("a scope-closing brace is out of the run", _QUIET_BRACE, 0),
    ("a preprocessor directive is out of the run", _QUIET_PREPROC, 0),
    ("an indent change is judged apart", _QUIET_INDENT, 0),
    ("the exemption ends at clang-format on", _SPLIT_AFTER_ON, 1),
)


def _check_split_case(name: str, src: str, want: int) -> int:
    """Run one detector case; return the number of failures it produced (0 or 1).

    The detector is run on this pass's own output, which is how `main` drives
    it: a finding that only survives on unformatted input would be noise.
    """
    got = len(find_split_runs(fix_text(src)))
    if got != want:
        sys.stderr.write(f"[FAIL] {name}: want {want} finding(s), got {got}\n")
        return 1
    return 0


def _check_case(name: str, src: str, want: str) -> int:
    """Run one rewrite case; return the number of failures it produced (0 or 1).

    Idempotency is asserted alongside correctness because this pass runs in a
    formatter loop: a rule that keeps changing its own output would churn every
    file on every run.
    """
    got = fix_text(src)
    if got != want:
        sys.stderr.write(f"[FAIL] {name}\n   want: {want!r}\n   got:  {got!r}\n")
        return 1
    if fix_text(got) != got:
        sys.stderr.write(f"[FAIL] {name}: not idempotent\n")
        return 1
    return 0


def _selftest() -> int:
    failures = sum(_check_case(*case) for case in _SELFTEST_CASES)
    failures += sum(_check_split_case(*case) for case in _SPLIT_CASES)
    total = len(_SELFTEST_CASES) + len(_SPLIT_CASES)
    if failures:
        sys.stderr.write(f"check_comment_format.py: selftest FAILED ({failures} case(s)).\n")
        return 2
    print(f"check_comment_format.py: selftest passed ({total} cases).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
