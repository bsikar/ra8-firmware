#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""cite_check.py -- validate Hardware User's Manual citations in source.

This script scans C / header files for in-line annotations of the form

    /* HUM Ch X.Y "section name", p NNNN */

and runs one or both of two complementary passes:

    Cite-VALIDATION (always on) -- for every cite that already exists,
    verify that:
        1. The chapter number X exists in docs/reference/CHAPTER_MAP.md.
        2. The page number NNNN falls inside chapter X's page range.
        3. The comment is well-formed (a `HUM Ch` comment that does not
           parse is reported as malformed).

    Cite-COVERAGE (--require-cites) -- the complementary headline rule
    from CLAUDE.md: every direct MMIO register read/write must be
    immediately preceded by a `/* HUM ... */` cite. Flags accesses that
    have none. Modelled block-aware (one cite covers the contiguous block
    of accesses beneath it).

Modes:

    --warn (default) -- exit 0, print findings to stderr.
    --strict (onward) -- exit 1 on any finding.
    --require-cites -- also run the cite-COVERAGE pass (advisory unless
        combined with --strict).

The script also accepts a list of explicit file arguments. With no
arguments it scans every first-party C file, derived from git ls-files
via lint_targets (#358) -- so tools/ra8_emulator (which models RA8
registers and cites the RA8 HUM) and port/usbx were previously omitted
and their citations went unvalidated. Vendored C under port/threadx and both
canonical third-party roots is dropped automatically.

The chapter map is parsed from CHAPTER_MAP.md so the page-range
truth lives in exactly one place. The pre-commit hook invokes this
script after build_chapter_map.sh has been run; nothing here calls
pdftotext.

Citation format details:

    /* HUM Ch X.Y "..." p NNNN */ single-page form
    /* HUM Ch X.Y "..." p NNNN-MMMM */ page range form

The X chapter number is required; subsection Y is optional in the
parser (some module-stop-only writes only carry the chapter). The
section-name string is enforced as present so a future linter pass
can grep for human-readable context.

The script is intentionally conservative: an unparseable comment
that *looks* like a HUM cite (starts with `HUM Ch`) is reported as
malformed even if it would otherwise pass the page-in-range check.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tempfile
from collections.abc import Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from lint_targets import first_party_paths
from selftest_assert import expect, report

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHAPTER_MAP_PATH = REPO_ROOT / "docs" / "reference" / "CHAPTER_MAP.md"

SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp")

CITE_RE = re.compile(
    r"""
    /\*\s*HUM\s+Ch\s+
    (?P<chapter>\d{1,2}) # chapter number
    (?:\.(?P<sub>\d{1,3}(?:\.\d{1,3})*))? # optional subsection X.Y[.Z...]
    \s*
    "(?P<section>[^"]*)" # quoted section name
    \s*,?\s*
    (?:(?:Table|Figure)\s+\d{1,3}(?:\.\d{1,3})*\s*)? # optional Table/Figure ref
    p\s+
    (?P<start>\d{1,5}) # start page
    (?:\s*-\s*(?P<end>\d{1,5}))? # optional end page
    (?:\s*--[^*]*)? # optional trailing "-- note" clause before the close
    \s*\*/
    """,
    re.VERBOSE,
)

LOOSE_HUM_RE = re.compile(r"/\*\s*HUM\s+Ch\b[^*]*\*/")

# HUM chapter numbers are 1-based; the manual has fewer than 100 chapters.
MAX_HUM_CHAPTER = 99


def parse_chapter_map(path: pathlib.Path) -> dict[int, tuple[int, int, str]]:
    """Parse CHAPTER_MAP.md into {chapter: (start_page, end_page, title)}.

    The map's table rows look like:

        | 1 | Overview | 69 | 110 |

    Lines that don't match the row pattern are ignored.
    """
    if not path.exists():
        msg = f"chapter map missing: {path}"
        raise FileNotFoundError(msg)

    row_re = re.compile(
        r"^\|\s*(\d{1,2})\s*\|\s*([^|]+?)\s*\|\s*(\d{1,5})\s*\|\s*(\d{1,5})\s*\|\s*$"
    )

    chapters: dict[int, tuple[int, int, str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = row_re.match(line)
        if m is None:
            continue
        num = int(m.group(1))
        title = m.group(2).strip()
        start = int(m.group(3))
        end = int(m.group(4))
        if not (1 <= num <= MAX_HUM_CHAPTER):
            continue
        chapters[num] = (start, end, title)
    return chapters


def iter_source_files(targets: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    """Yield every C/H source file under any of the given targets."""
    for t in targets:
        if not t.exists():
            continue
        if t.is_file():
            if t.suffix.lower() in SOURCE_SUFFIXES:
                yield t
            continue
        for sub in t.rglob("*"):
            if not sub.is_file():
                continue
            if sub.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            # Skip vendored / build trees: nothing under either canonical
            # third-party root or any build directory is linted for HUM cites.
            parts = set(sub.parts)
            if "third_party" in parts or "build" in parts:
                continue
            if "_deps" in parts:
                continue
            yield sub


# ---------------------------------------------------------------------------
# Register-access coverage pass ("does every MMIO access HAVE a cite?").
#
# cite_check's original job is to validate cites that already EXIST. The
# coverage pass answers the complementary, headline question from CLAUDE.md's
# HUM-citation policy: every direct register read/write must be immediately
# preceded by a `/* HUM ... */` cite. It is enabled with --require-cites
# (advisory unless combined with --strict) and models the tree's real
# convention -- one cite comment covers the contiguous block of accesses
# beneath it until the next non-access statement.
# ---------------------------------------------------------------------------

# An MMIO field access: `<ptr>->FIELD` or `accessor()->FIELD`, where FIELD is
# the project's ALL-CAPS register-field spelling (>= 2 chars). First-party C
# structs in this tree use lower_snake_case members, so an upper-case member
# after `->` is a reliable register-access signal with a low false-positive
# rate.
#
# The trailing `\b` is load-bearing: without it the ALL-CAPS run is allowed to
# stop in the middle of a CamelCase identifier, so a C++ member call whose first
# two characters happen to be upper-case matched as a register. `text->CData()`
# in an XML shim was read as `text->CD` and reported as an uncited MMIO
# access -- a class that grows with every C++ shim added, since `\b` is the only
# thing that distinguishes `->CFDGSTS` (a real CAN-FD register) from `->CData`.
ACCESS_RE = re.compile(r"(?:\)|[A-Za-z_]\w*)\s*->\s*[A-Z][A-Z0-9_]+\b")
# Raw volatile-pointer dereference, e.g. `*(volatile uint32_t *)addr = ...`.
RAW_DEREF_RE = re.compile(r"\*\s*\(\s*volatile\b")
# A HUM cite token anywhere on a line (any recognised form: "HUM Ch",
# "HUM N.N", "HUM Table", ...). Used only to decide whether an access is
# covered, so a permissive match is deliberate: it can only SUPPRESS a finding.
HUM_ON_LINE_RE = re.compile(r"\bHUM\b")

# How far up we scan for a covering cite before giving up.
MAX_CITE_LOOKUP_LINES = 40


class _Blanker:
    """Character state machine blanking comment and literal interiors.

    One method per state. The machine is the whole algorithm, so it is
    expressed as states rather than as one loop with a state variable and
    four branches -- each method's contract is "consume from i, return the
    next state and index", which is checkable a state at a time.
    """

    def __init__(self, text: str) -> None:
        """Prepare a mutable blanking buffer over ``text``.

        ``out`` starts as a character-for-character copy so blanking preserves
        every offset -- line and column numbers reported against the blanked
        view stay valid for the original.
        """
        self.text = text
        self.out = list(text)
        self.n = len(text)

    def _peek(self, i: int) -> str:
        """The character after ``i``, or "" at end of text.

        Returning "" rather than raising lets every two-character token test
        (``//``, ``/*``, ``*/``) run unguarded at the last position.
        """
        return self.text[i + 1] if i + 1 < self.n else ""

    def _blank(self, i: int, count: int = 1) -> None:
        """Overwrite ``count`` characters with spaces, clamped to the end."""
        for k in range(i, min(i + count, self.n)):
            self.out[k] = " "

    def _code(self, i: int, c: str) -> tuple[str, int]:
        """Step the scanner through ordinary code, returning ``(next_state, index)``.

        Code itself is left intact; this only recognises where a comment or
        literal begins and hands control to the matching state.
        """
        nxt = self._peek(i)
        if c == "/" and nxt == "/":
            self._blank(i, 2)
            return "line_comment", i + 2
        if c == "/" and nxt == "*":
            self._blank(i, 2)
            return "block_comment", i + 2
        if c == '"':
            return "string", i + 1
        if c == "'":
            return "char", i + 1
        return "code", i + 1

    def _line_comment(self, i: int, c: str) -> tuple[str, int]:
        """Blank a ``//`` comment, ending at the newline, which is preserved."""
        if c == "\n":
            return "code", i + 1
        self._blank(i)
        return "line_comment", i + 1

    def _block_comment(self, i: int, c: str) -> tuple[str, int]:
        """Blank a ``/* */`` comment, preserving its interior newlines.

        Keeping the newlines is what holds line numbers stable across a
        multi-line comment.
        """
        if c == "*" and self._peek(i) == "/":
            self._blank(i, 2)
            return "code", i + 2
        if c != "\n":
            self._blank(i)
        return "block_comment", i + 1

    def _literal(self, i: int, c: str, state: str) -> tuple[str, int]:
        """Blank a string or char literal, honouring backslash escapes.

        An escaped closer must not end the literal, and a line continuation
        inside one must not be mistaken for its end -- both are why this
        consumes the escaped character rather than only the backslash.
        """
        closer = '"' if state == "string" else "'"
        if c == "\\":
            self._blank(i)
            if i + 1 < self.n and self.text[i + 1] != "\n":
                self._blank(i + 1)
            return state, i + 2
        if c == closer:
            return "code", i + 1
        if c != "\n":
            self._blank(i)
        return state, i + 1

    def run(self) -> str:
        """Drive the machine over the whole text and return the blanked copy."""
        state, i = "code", 0
        while i < self.n:
            c = self.text[i]
            if state == "code":
                state, i = self._code(i, c)
            elif state == "line_comment":
                state, i = self._line_comment(i, c)
            elif state == "block_comment":
                state, i = self._block_comment(i, c)
            else:  # string or char literal
                state, i = self._literal(i, c, state)
        return "".join(self.out)


def blank_comments_and_strings(text: str) -> str:
    """Return `text` with comment and string interiors replaced by spaces.

    Newlines and total length are preserved so per-line indexing still lines
    up with the original. Blanking prose keeps a comment like
    ``channel-index -> MSTP id`` or a log string like ``"SCR->CTL"`` from
    being misread as a register access.
    """
    return _Blanker(text).run()


def is_access_line(blanked_line: str) -> bool:
    """True if a code (comment/string-blanked) line performs an MMIO access."""
    return bool(ACCESS_RE.search(blanked_line)) or bool(RAW_DEREF_RE.search(blanked_line))


def scan_access_coverage(path: pathlib.Path, text: str) -> tuple[list[str], int]:
    """Return uncited-access findings AND the total access-line count, in one pass.

    `cite_ratchet.py` needs both numbers for every file: the findings are the
    debt it ratchets, and the total is its detector-health floor -- the uncited
    count alone cannot distinguish "the tree got cited" from "ACCESS_RE stopped
    matching", and the second reads as a burn-down. Blanking is the expensive
    step (a per-character state machine over every source file), so both come
    out of one blanking pass rather than two.

    Args:
        path: File the findings are reported against.
        text: Raw source text.

    Returns:
        A ``(findings, access_line_count)`` pair. The count includes CITED
        accesses; only the findings are uncited.
    """
    orig = text.splitlines()
    blanked = blank_comments_and_strings(text).splitlines()
    return (
        _uncited_in_lines(path, orig, blanked),
        sum(1 for line in blanked if is_access_line(line)),
    )


def _is_block_continuation(blanked_line: str) -> bool:
    """True if a line is part of the same register-access block while walking up.

    Blank lines, and statement fragments split across lines (a trailing binary
    operator, or a leading continuation token), are transparent: one cite above
    a block still covers accesses whose value expression wraps onto its own
    line.
    """
    s = blanked_line.strip()
    if s == "":
        return True
    if s.endswith(("=", "|", "&", "+", "-", "*", ",", "(", "<<", ">>", "?", ":")):
        return True
    return s[0] in ".|&)+*?:"


def _uncited_in_lines(path: pathlib.Path, orig: list[str], blanked: list[str]) -> list[str]:
    """Findings for the already-blanked view of one file.

    The single definition of "this access lacks a citation". Both
    `find_uncited_accesses` and `scan_access_coverage` delegate here so a
    second, drifting copy of the block-walk cannot appear.

    Args:
        path: File the findings are reported against.
        orig: Original source lines -- citations are read from these, since
            blanking erases comment interiors.
        blanked: The comment/string-blanked view, line for line with `orig`.

    Returns:
        One finding string per uncited access, in line order.
    """
    findings: list[str] = []
    for i, bline in enumerate(blanked):
        if not is_access_line(bline):
            continue
        # Same-line trailing cite, e.g. `reg->X = 1; /* HUM Ch .. */`.
        if HUM_ON_LINE_RE.search(orig[i]):
            continue
        covered = False
        j = i - 1
        steps = 0
        while j >= 0 and steps < MAX_CITE_LOOKUP_LINES:
            # A HUM token on a comment-blanked (i.e. non-code) line is a cite.
            if blanked[j].strip() == "" and HUM_ON_LINE_RE.search(orig[j]):
                covered = True
                break
            if is_access_line(blanked[j]) or _is_block_continuation(blanked[j]):
                j -= 1
                steps += 1
                continue
            break  # a real statement with no covering cite above the block
        if not covered:
            findings.append(
                f"{path}:{i + 1}: MMIO access without preceding HUM cite: {orig[i].strip()[:70]}"
            )
    return findings


def find_uncited_accesses(path: pathlib.Path, text: str) -> list[str]:
    """Return findings for MMIO accesses lacking a preceding HUM cite.

    Models the tree convention that one `/* HUM ... */` comment covers the
    contiguous block of register accesses directly beneath it: an access is
    covered when walking upward -- past blank lines, comment-only lines, other
    accesses in the same block, and statement continuations -- reaches a HUM
    cite before any other statement.

    Args:
        path: File the findings are reported against.
        text: Raw source text.

    Returns:
        One finding string per uncited access, in line order.
    """
    return _uncited_in_lines(path, text.splitlines(), blank_comments_and_strings(text).splitlines())


def check_file(
    path: pathlib.Path,
    chapters: dict[int, tuple[int, int, str]],
    *,
    require_cites: bool = False,
) -> list[str]:
    """Return a list of finding strings for one file."""
    findings: list[str] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"{path}: read error: {exc}"]

    parsed_spans: list[tuple[int, int]] = []
    for m in CITE_RE.finditer(text):
        parsed_spans.append(m.span())
        chapter = int(m.group("chapter"))
        start = int(m.group("start"))
        end_raw = m.group("end")
        end = int(end_raw) if end_raw else start
        section = m.group("section")

        line_no = text.count("\n", 0, m.start()) + 1

        if chapter not in chapters:
            findings.append(f"{path}:{line_no}: HUM Ch {chapter} not in chapter map")
            continue
        ch_start, ch_end, ch_title = chapters[chapter]

        if start < ch_start or end > ch_end:
            findings.append(
                f'{path}:{line_no}: HUM Ch {chapter} "{section}" '
                f"page range {start}-{end} outside chapter range "
                f"{ch_start}-{ch_end} ({ch_title})"
            )
            continue

        if end < start:
            findings.append(f"{path}:{line_no}: HUM Ch {chapter} reversed page range {start}-{end}")
            continue

    # Catch malformed HUM cites that did NOT match CITE_RE. Anything
    # under LOOSE_HUM_RE that wasn't already covered by a parsed span
    # is reported as malformed.
    for lm in LOOSE_HUM_RE.finditer(text):
        if any(s <= lm.start() < e for s, e in parsed_spans):
            continue
        line_no = text.count("\n", 0, lm.start()) + 1
        findings.append(
            f"{path}:{line_no}: malformed HUM cite "
            f'{lm.group(0)!r} -- expected /* HUM Ch X.Y "..." p NNNN */'
        )

    if require_cites:
        findings.extend(find_uncited_accesses(path, text))

    return findings


def _build_parser() -> argparse.ArgumentParser:
    """Build the command-line parser for this gate."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="files or directories to scan (default: every tracked first-party C file)",
    )
    parser.add_argument(
        "--warn",
        action="store_true",
        help="warn-only mode: print findings, exit 0 (default)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="strict mode: exit 1 on any finding (onward)",
    )
    parser.add_argument(
        "--require-cites",
        action="store_true",
        help=(
            "also flag MMIO register accesses that lack a preceding HUM cite "
            "(the citation-COVERAGE pass, complementary to the default "
            "cite-VALIDATION pass). Advisory unless combined with --strict."
        ),
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="prove the validator fires on a malformed cite and the scope holds",
    )
    parser.add_argument(
        "--chapter-map",
        default=str(CHAPTER_MAP_PATH),
        help=f"path to CHAPTER_MAP.md (default: {CHAPTER_MAP_PATH})",
    )
    return parser


def _report_findings(
    findings: list[str], file_count: int, *, strict: bool, require_cites: bool
) -> None:
    """Print every finding and a one-line summary naming the mode it ran in.

    The summary splits the two passes apart when --require-cites is on: an
    uncited MMIO access and a malformed citation are different defects, and a
    single total hides which one a reader has to go and fix.
    """
    for line in findings:
        print(line, file=sys.stderr)
    verdict = "strict" if strict else "warn"
    total = len(findings)
    head = f"cite_check.py: {total} finding(s) across {file_count} file(s) [{verdict}]"
    if require_cites:
        uncited = sum(1 for line in findings if "MMIO access without preceding HUM cite" in line)
        head += f" ({uncited} uncited-access, {total - uncited} cite-validation)"
    print(head, file=sys.stderr)


# ---------------------------------------------------------------------------
# Selftest -- both directions, plus a scope assertion under tools/, silently
# omitted until #358.
# ---------------------------------------------------------------------------

# An uncited MMIO write. The cite-COVERAGE pass must FIRE on this.
_ST_UNCITED_C = """\
void drv_init(void)
{
  volatile r_canfd_regs_t* reg = canfd();
  reg->CFDGSTS = 0U;
}
"""

# The same write, cited. The pass must stay QUIET.
_ST_CITED_C = """\
void drv_init(void)
{
  volatile r_canfd_regs_t* reg = canfd();
  /* HUM Ch 25 "Ultra-Low-Power Timer" p 1190 */
  reg->CFDGSTS = 0U;
}
"""

# Address-of a register field handed to a register-agnostic poll helper. The
# load happens INSIDE the helper, so this call site is the only place the
# register is named -- it must FIRE. Asserted so a future "precision" narrowing
# that drops address-of has to argue with a failing selftest rather than with a
# silently shrinking backlog.
_ST_ADDR_OF_C = """\
ra8_err_t drv_wait(void)
{
  volatile r_canfd_regs_t* reg = canfd();
  return ra8_hw_wait_flag_set32(&reg->CFDGSTS, mask, spin);
}
"""

# A C++ member call whose first two characters are upper-case. NOT a register;
# the pass must stay QUIET (it read this as `text->CD` before the trailing \\b).
_ST_CAMEL_CPP = """\
bool shim_is_cdata(const XMLText* text)
{
  if (text->CData()) {
    return true;
  }
  return false;
}
"""

# A genuinely two-character register name, so the \\b fix cannot be satisfied by
# simply demanding a longer ALL-CAPS run. Must FIRE.
_ST_SHORT_REG_C = """\
void drv_poke(void)
{
  volatile r_i3c_regs_t* reg = i3c();
  reg->CD = 0U;
}
"""


def _selftest_coverage(chapters: dict[int, tuple[int, int, str]], failures: list[str]) -> None:
    """Assert the cite-COVERAGE pass fires and stays quiet on the right inputs.

    Both directions, because a coverage pass that quietly stopped matching
    reports a shrinking backlog -- which reads as progress.

    Args:
        chapters: Parsed chapter map, as returned by ``parse_chapter_map``.
        failures: Accumulator that ``expect`` appends failed labels to.
    """
    cases: tuple[tuple[str, str, bool, str], ...] = (
        ("uncited.c", _ST_UNCITED_C, True, "an uncited MMIO write fires under --require-cites"),
        ("cited.c", _ST_CITED_C, False, "the same write with a HUM cite above stays quiet"),
        ("addrof.c", _ST_ADDR_OF_C, True, "&reg->FIELD passed to a poll helper fires"),
        ("shim.cpp", _ST_CAMEL_CPP, False, "a C++ `->CData()` member call stays quiet"),
        ("short.c", _ST_SHORT_REG_C, True, "a two-character register name still fires"),
    )
    with tempfile.TemporaryDirectory() as tmp:
        for name, body, want_finding, label in cases:
            path = pathlib.Path(tmp) / name
            path.write_text(body, encoding="utf-8")
            got = bool(check_file(path, chapters, require_cites=True))
            expect(got == want_finding, label, failures)

        # The coverage pass is opt-in: without --require-cites the very input
        # that fires above must produce nothing, or the default mode would have
        # been silently reporting a backlog it never promised to.
        path = pathlib.Path(tmp) / "uncited.c"
        expect(
            not check_file(path, chapters),
            "the coverage pass stays off unless --require-cites is given",
            failures,
        )


def selftest() -> int:
    """Prove a malformed cite fires, a well-formed one is quiet, and scope holds."""
    print("cite_check.py --selftest")
    failures: list[str] = []
    chapters = parse_chapter_map(CHAPTER_MAP_PATH)
    with tempfile.TemporaryDirectory() as tmp:
        bad = pathlib.Path(tmp) / "bad.c"
        bad.write_text("/* HUM Ch 25 a malformed cite with no page */\n", encoding="utf-8")
        good = pathlib.Path(tmp) / "good.c"
        good.write_text('/* HUM Ch 25 "Ultra-Low-Power Timer" p 1190 */\n', encoding="utf-8")
        expect(bool(check_file(bad, chapters)), "a malformed HUM cite fires", failures)
        expect(
            not check_file(good, chapters),
            "a well-formed, in-range cite stays quiet",
            failures,
        )
    _selftest_coverage(chapters, failures)
    scope = set(first_party_paths(SOURCE_SUFFIXES))
    expect(
        any(s.startswith("tools/") for s in scope),
        "tools/ is in scope (the scan-dir list omitted it before #358)",
        failures,
    )
    expect(
        not any(
            s.startswith(("libs/third_party/", "apps/shared_libs/third_party/")) for s in scope
        ),
        "vendored SOUP stays out of scope",
        failures,
    )
    return report(failures)


def main(argv: list[str]) -> int:
    """Verify every register access carries a Hardware User's Manual citation.

    The rule exists because a register write with no citation cannot be
    reviewed: the reader has no way to confirm the bit pattern against the
    manual, and a wrong one produces hardware that misbehaves rather than
    code that fails to build.

    Citations are looked for only in COMMENTS, which is why the source is
    scanned through the blanking pass above -- a manual section number
    appearing in a string literal is not a citation.

    Returns 0 when every access is cited, 1 otherwise.
    """
    args = _build_parser().parse_args(argv)

    if args.selftest:
        return selftest()

    if args.warn and args.strict:
        print("cite_check.py: --warn and --strict are mutually exclusive", file=sys.stderr)
        return 2

    # Default to warn unless explicitly strict.
    strict = args.strict and not args.warn

    try:
        chapters = parse_chapter_map(pathlib.Path(args.chapter_map))
    except FileNotFoundError as exc:
        print(f"cite_check.py: {exc}", file=sys.stderr)
        return 2

    if args.paths:
        targets = [pathlib.Path(p) for p in args.paths]
    else:
        # Derived from git ls-files (#358): every first-party C file, not just
        # a short fixed root list. tools/ra8_emulator models RA8
        # registers and cites the RA8 HUM, and port/usbx holds first-party RA8
        # USB glue -- both were silently omitted, so their cites went
        # unvalidated. Vendored C under port/threadx and both canonical
        # third-party roots is already dropped by first_party_paths.
        targets = [REPO_ROOT / rel for rel in first_party_paths(SOURCE_SUFFIXES)]

    findings: list[str] = []
    file_count = 0
    for f in iter_source_files(targets):
        file_count += 1
        findings.extend(check_file(f, chapters, require_cites=args.require_cites))

    if findings:
        _report_findings(findings, file_count, strict=strict, require_cites=args.require_cites)
        return 1 if strict else 0

    print(
        f"cite_check.py: 0 findings across {file_count} file(s)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
