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
selftest failure.
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
    "libs/fonts/",
    "port/threadx/",
)

# Scan-state for the per-line C/C++ tokeniser.
_ST_NORMAL = 0
_ST_BLOCK = 1  # inside a multi-line /* ... */ block comment
_ST_RAW = 2  # inside a C++ raw string R"delim( ... )delim"

# Must match .clang-format ColumnLimit. End-alignment never pads a comment up to
# this column: a run splits instead, because a trailing comment that reaches the
# limit makes clang-format collapse its (clang-owned) leading alignment to fit,
# which would fight this pass. It preserves alignment only strictly under it.
_COLUMN_LIMIT = 100


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

    # Per line: the trailing comment (or None), the verbatim text in front of it
    # (code + clang's leading alignment), and the column the comment opens at.
    trailing: list[_Comment | None] = [None] * len(lines)
    prefix: list[str] = [""] * len(lines)
    start_col: list[int] = [-1] * len(lines)

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
            out[i] = before + _render(last.opener, last.content, 1)  # standalone
        else:
            if not before[-1].isspace():
                before += " "  # rule 3: >=1 space before a trailing comment
            trailing[i] = last
            prefix[i] = before  # preserve code + clang's start-column alignment
            start_col[i] = len(before)

    # End-align consecutive trailing comments clang put in the same start column.
    i = 0
    while i < len(lines):
        if trailing[i] is None:
            i += 1
            continue
        j = i
        body_hi = _body_len(trailing[i])
        while (
            j + 1 < len(lines) and trailing[j + 1] is not None and start_col[j + 1] == start_col[i]
        ):
            nbody = max(body_hi, _body_len(trailing[j + 1]))
            if start_col[i] + nbody >= _COLUMN_LIMIT:  # padding would reach the limit
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
    args = argv[1:]
    if "--selftest" in args:
        return _selftest()
    do_fix = "--fix" in args
    paths = [a for a in args if not a.startswith("--")]

    targets = _enumerate_targets(paths)
    if not targets:
        print("check_comment_format.py: no files to scan")
        return 0

    bad: list[Path] = []
    fixed = 0
    for path in sorted(targets):
        try:
            original = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        updated = fix_text(original)
        if updated == original:
            continue
        if do_fix:
            path.write_text(updated, encoding="utf-8")
            fixed += 1
        else:
            bad.append(path)

    if do_fix:
        print(f"check_comment_format.py: reformatted {fixed} file(s).")
        return 0

    if not bad:
        print(f"check_comment_format.py: {len(targets)} file(s) scanned, comments well-formed.")
        return 0

    sys.stderr.write("check_comment_format.py: comment-format finding(s):\n")
    for path in bad:
        rel = _rel(path)
        updated = fix_text(path.read_text(encoding="utf-8"))
        for lineno, old, new in _first_diff_lines(path.read_text(encoding="utf-8"), updated):
            sys.stderr.write(
                f"  {rel}:{lineno}\n      have: {old.rstrip()}\n      want: {new.rstrip()}\n"
            )
    sys.stderr.write("\nRun: make format  (or check_comment_format.py --fix)\n")
    return 1


# ---------------------------------------------------------------------------
# Built-in test battery (run via --selftest; mirrors the user's examples).
# ---------------------------------------------------------------------------
def _selftest() -> int:
    cases: list[tuple[str, str, str]] = []

    # Rule 1: space after the opener.
    cases.append(("space after /*", "int x; /*hi */\n", "int x; /* hi */\n"))
    # Rule 2: space before */.
    cases.append(("space before */", "int x; /* hi*/\n", "int x; /* hi */\n"))
    cases.append(
        (
            "doxy member . */",
            "bool e; /**< DSISETR.EOTPEN.*/\n",
            "bool e; /**< DSISETR.EOTPEN. */\n",
        )
    )
    # Rule 3: >=1 space before a trailing comment.
    cases.append(("space before comment", "int x;/* hi */\n", "int x; /* hi */\n"))

    # Rule 4: align a run of trailing comments to the longest (which gets 1 space).
    run_in = (
        "enum {\n"
        "  a = 0x01U, /**< Sync event: VSYNC start. */\n"
        "  b = 0x08U, /**< End of Transmission packet. */\n"
        "};\n"
    )
    run_out = (
        "enum {\n"
        "  a = 0x01U, /**< Sync event: VSYNC start.    */\n"
        "  b = 0x08U, /**< End of Transmission packet. */\n"
        "};\n"
    )
    cases.append(("align run to longest", run_in, run_out))

    # Over-padded run gets tightened so the longest has exactly one space.
    over_in = "enum {\n  a = 1, /**< short.        */\n  b = 2, /**< longer text.  */\n};\n"
    over_out = "enum {\n  a = 1, /**< short.       */\n  b = 2, /**< longer text. */\n};\n"
    cases.append(("tighten over-padded run", over_in, over_out))

    # The leading spaces (clang-format's start-column alignment) are preserved
    # verbatim, and comments at different start columns are not cross-aligned --
    # only the comment interior and the */ end column are this pass's business.
    sep_in = "  k_a = 1,   /**< x.*/\n  k_bb = 2, /**< yy. */\n"
    sep_out = "  k_a = 1,   /**< x. */\n  k_bb = 2, /**< yy. */\n"
    cases.append(("leading preserved, columns separate", sep_in, sep_out))

    # A code-only line breaks the run; the standalone doc line collapses to one
    # space; the trailing comments keep their (clang-owned) leading spaces.
    break_in = (
        "enum {\n"
        "  a = 1U,  /**< one.   */\n"
        "  b = 2U,  /**< two.   */\n"
        "  c = (1U << 9),\n"
        "  /**< composite.                    */\n"
        "};\n"
    )
    break_out = (
        "enum {\n"
        "  a = 1U,  /**< one. */\n"
        "  b = 2U,  /**< two. */\n"
        "  c = (1U << 9),\n"
        "  /**< composite. */\n"
        "};\n"
    )
    cases.append(("code line breaks run", break_in, break_out))

    # Column-limit guard: end-alignment that would reach the limit splits the run
    # instead, so a short comment is never padded out to a far column (which would
    # make clang-format collapse the leading alignment). `a` is tightened to one
    # space rather than aligned to the long sibling's */.
    long_c = "x" * 84
    cap_in = f"  a = 1, /**< short.        */\n  b = 2, /**< {long_c}. */\n"
    cap_out = f"  a = 1, /**< short. */\n  b = 2, /**< {long_c}. */\n"
    cases.append(("column-limit splits run", cap_in, cap_out))

    # Safety: never touch text inside string literals.
    cases.append(("string with /* */", 'const char* s = "/*x*/";\n', 'const char* s = "/*x*/";\n'))
    cases.append(("string with */", 'puts("a*/b");\n', 'puts("a*/b");\n'))
    # Safety: never touch // line comments.
    cases.append(("line comment left alone", "int x; // a*/b\n", "int x; // a*/b\n"))
    # Safety: multi-line block comment interior untouched.
    multi = "/**\n * @brief foo.*/bar\n * body\n */\nint x;\n"
    cases.append(("multiline block untouched", multi, multi))
    # Safety: banners untouched.
    cases.append(
        ("banner untouched", "/******** section ********/\n", "/******** section ********/\n")
    )
    cases.append(("empty comment untouched", "x; /**/\n", "x; /**/\n"))
    # Safety: C++ raw string with */ inside is not a comment.
    cases.append(("raw string untouched", 'auto s = R"(a*/b/*c)";\n', 'auto s = R"(a*/b/*c)";\n'))
    # Internal double-space (manual sub-column alignment) is preserved.
    cases.append(
        (
            "internal spacing kept",
            "x = 1; /**< VBTBPSR  Ch 12.2 p 509.*/\n",
            "x = 1; /**< VBTBPSR  Ch 12.2 p 509. */\n",
        )
    )
    # Inline mid-code comments (code follows the */) are left byte-for-byte
    # alone -- clang-format owns the spacing around them; rewriting them fights
    # it (e.g. clang then wants a space between */ and the next token).
    cases.append(("inline arg-label spaced", "f(a, /* tag= */ b);\n", "f(a, /* tag= */ b);\n"))
    cases.append(("inline arg-label tight", "f(a, /*tag=*/b);\n", "f(a, /*tag=*/b);\n"))
    cases.append(("inline then trailing", "f(/*a*/x); /*hi*/\n", "f(/*a*/x); /* hi */\n"))

    failures = 0
    for name, src, want in cases:
        got = fix_text(src)
        if got != want:
            failures += 1
            sys.stderr.write(f"[FAIL] {name}\n   want: {want!r}\n   got:  {got!r}\n")
            continue
        # Idempotency: a second pass must change nothing.
        if fix_text(got) != got:
            failures += 1
            sys.stderr.write(f"[FAIL] {name}: not idempotent\n")

    if failures:
        sys.stderr.write(f"check_comment_format.py: selftest FAILED ({failures} case(s)).\n")
        return 2
    print(f"check_comment_format.py: selftest passed ({len(cases)} cases).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
