# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The register symbol table the Hardware User's Manual actually publishes.

``cite_check.py`` answers "is this citation well-formed, and does it name a
real chapter and a page inside that chapter?". It cannot answer "does the
cited text describe the register being touched?", and it cannot answer "does
this register exist at all?". Three landed defects lived in that gap, each
carrying a citation naming a REAL chapter and a REAL page:

* the whole ``ra8_rsip`` crypto family addressed registers HUM Ch 52 does not
  publish -- that chapter is six pages long and describes no registers at all;
* ``ra8_ptp_regs.h`` declared a thirteen-register window at ``0x403E_0100``,
  a reserved hole in the GPTP aperture, and the demo printed ``clock PASS``
  because a reserved aperture echoed back its own writes (#498);
* ``ra8_etha_regs.h`` declared ``EASCR`` at ``0x0580``, which appears nowhere
  in HUM Ch 32, and ~26 ETHA citations pointed at real pages describing other
  registers (#539).

Nothing mechanically connected a register SYMBOL in our headers to the symbol
table the manual publishes. This module is that connection: it parses the
per-register description subsections out of the committed manual PDF, which
have a rigidly regular shape --

    32.3.2.6      EATMFSCq : Transmission Maximum Frame Size Configuration ...
      Base address: ETHAm = 0x403C_A000 + 0x2000 x m (m = 0, 1)
     Offset address: 0x0040 + 0x4 x q

-- yielding ``(chapter, section, symbol, offset, page, name)`` for every
register in the manual. That is a strictly stronger source than the chapter
register-list tables (Table X.Y), because the subsection heading carries the
page number the register is actually described on, which is exactly what the
"plausible but false citation" defects got wrong.

Two properties this module is built around, both distilled by the #190 audit
and both easy to get wrong here:

* **No check may compare a constant to itself.** A committed snapshot of the
  extracted names, diffed against a committed header, proves nothing -- both
  sides are ours to edit. So the authority is always the PDF; the committed
  CSV is a reviewable convenience whose freshness is separately proven by
  regenerating.
* **Every scan needs a vacuity floor.** A PDF text extraction that silently
  yielded zero rows would report every register in the tree clean, forever.
  That is the single most likely failure mode here -- a poppler upgrade, a
  re-typeset manual revision, a changed heading style -- so
  :func:`assert_not_vacuous` is a hard, absolute floor that runs before any
  caller is allowed to use the result.
"""

from __future__ import annotations

import csv
import io
import re
import shutil
import subprocess
from dataclasses import dataclass, replace
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The committed Renesas RA8D2 Hardware User's Manual (R01UH1065EJ, Rev.1.30).
HUM_PDF = REPO_ROOT / "docs" / "reference" / "ra8d2-hardware-user-manual.pdf"

# The generated, committed symbol table.
HUM_CSV = REPO_ROOT / "docs" / "reference" / "HUM_REGISTERS.csv"

CSV_FIELDS = (
    "chapter",
    "section",
    "symbol",
    "offset",
    "offset_expr",
    "page",
    "page_end",
    "name",
)

# A register description subsection heading:
#     "32.3.2.6      EATMFSCq : Transmission Maximum Frame Size ... Register q"
# The chapter and section number are separated so a citation naming chapter X
# can be resolved against exactly chapter X's symbols.
#
# Three irregularities in this manual that a tighter pattern silently drops,
# each of which cost real registers before it was handled:
#   * one subsection can define several forms of one register under
#     SLASH-separated symbols -- "38.2.3  TDR/TDRLL/TDRLH : Transmit Data
#     Register", "48.2.4  CRCDOR/CRCDOR_HA/CRCDOR_BY : CRC Data Output
#     Register". Each alternate is a symbol our headers may legitimately
#     declare, so each becomes its own row.
#   * or under COMMA-separated symbols -- "60.2.13  CDAYR, CDAYR_x : Capture
#     Data Address Y Register (x = B, M)", "7.2.4  OFS1, OFS1_SEC : ...". 43
#     headings use this form, and rejecting it made the whole CEU capture-
#     address family read as invented.
#   * the space after the colon is not always typeset -- "19.2.3
#     ELSRn:Event Link Setting Register n". Requiring it lost the whole ELC
#     chapter's ELSRn.
HEADING_RE = re.compile(
    r"^\s*(?P<chapter>\d{1,2})\.(?P<section>\d{1,3}(?:\.\d{1,3})*)"
    r"\s{2,}(?P<symbol>[A-Za-z][A-Za-z0-9_]{1,23}"
    r"(?:\s*[/,]\s*[A-Za-z][A-Za-z0-9_]{1,23})*)"
    r"\s*:\s*(?P<name>[A-Za-z(]\S*(?:\s+\S+)*?)\s*$"
)

# Splits a heading's symbol group into its individual alternates.
ALTERNATE_SEP_RE = re.compile(r"\s*[/,]\s*")

# The leading hex literal of an offset expression. Taking everything up to the
# first "+" was not enough: the manual qualifies some offsets with a
# parenthetical -- "0x003C (CDAYR)", "0x014 (CFIFO/CFIFOL/CFIFOLL)" -- and that
# text has to be dropped before the value can be compared as a number.
BASE_OFFSET_RE = re.compile(r"^(0[xX][0-9A-Fa-f_]+)")

# "Offset address: 0x0040 + 0x4 x q". Only the offset form is taken: registers
# documented by absolute address instead carry no offset, and the offset check
# skips them rather than inventing one.
OFFSET_RE = re.compile(r"^\s*Offset address\s*:\s*(?P<expr>\S.*?)\s*$")

# Any numbered section heading, register-bearing or not ("32.4  ETHA Operation
# Modes"). These bound how far a register's description runs: the text for one
# register ends where the next heading begins. Without that, a citation naming
# the second page of a two-page register description would read as wrong, which
# is most of what a naive page check would shout about.
#
# The two-space minimum is load-bearing: ``-layout`` preserves the manual's
# wide heading gutter, so a real heading always has one, while a mid-paragraph
# cross-reference ("see 38.26 for details.") has a single space. Accepting the
# latter would plant a spurious boundary inside a register's description and
# shrink its page range, manufacturing exactly the false "wrong page" reports
# this range model exists to prevent.
SECTION_RE = re.compile(r"^\s*(?P<chapter>\d{1,2})\.(?P<section>\d{1,3}(?:\.\d{1,3})*)\s{2,}\S")

# A register abbreviation is uppercase letters and digits, optionally carrying
# lowercase array-index letters (EATMFSCq, BCNTnAER, PDCFCH00RCHn). Requiring
# uppercase to dominate rejects prose headings that happen to share the
# "<number> <Word> : <text>" shape -- an electrical-characteristics table
# yielded "1.012  Conditions : AVCC: 2.7 to 3.63 V" without this.
MIN_UPPERCASE_IN_SYMBOL = 2

# Highest code point the repository's 7-bit ASCII rule permits.
MAX_ASCII_CODEPOINT = 127


def _looks_like_register_symbol(symbol: str) -> bool:
    """True when `symbol` has the shape of a HUM register abbreviation."""
    upper = sum(1 for ch in symbol if ch.isupper())
    lower = sum(1 for ch in symbol if ch.islower())
    return upper >= MIN_UPPERCASE_IN_SYMBOL and upper > lower


# The manual's table of contents repeats every subsection heading with a dot
# leader and a page number ("... EATMFSCq : Transmission ....... 1635"). Those
# lines match HEADING_RE exactly, so they are rejected explicitly -- left in,
# they would triple the row count and attribute every register to the TOC page.
TOC_LEADER_RE = re.compile(r"\.{4,}\s*\d{1,5}\s*$")

# The printed page footer, e.g. "R01UH1065EJ0130 Rev.1.30   Page 1630 of 4291".
# In this revision the printed number equals the PDF page index, but the footer
# is read rather than assumed so a re-paginated revision cannot shift silently.
FOOTER_RE = re.compile(r"Page\s+(?P<page>\d{1,5})\s+of\s+\d{4,5}")

# How far below a heading the offset line may sit before the heading is taken
# to have none. The intervening lines are the base-address block.
OFFSET_LOOKAHEAD_LINES = 14

# Transliterations for the only two non-ASCII characters the manual's register
# headings and offset expressions contain: U+00D7 MULTIPLICATION SIGN (the
# array-stride operator, "0x0040 + 0x4 x q") and U+00B5 MICRO SIGN. Spelled as
# escapes because this file is itself held to the repository's 7-bit ASCII
# rule. Anything outside this set is a hard error rather than a silent
# mangling -- a dropped character would corrupt a symbol or an offset.
ASCII_FOLD = {"\u00d7": "*", "\u00b5": "u"}

# --- Vacuity floors --------------------------------------------------------
# Absolute, hand-verified lower bounds on what a working extraction produces
# from this manual. They are deliberately well below the true counts (2000+
# rows across 62 chapters at the time of writing) so a normal revision bump
# does not trip them, and deliberately far above zero so a broken extraction
# cannot pass. A floor of "whatever we got last time" would be the
# compare-a-constant-to-itself mistake these guard against.
MIN_TOTAL_ROWS = 1500
MIN_CHAPTERS_WITH_REGISTERS = 50
MIN_ROWS_WITH_OFFSET = 1200


class HumExtractionError(RuntimeError):
    """The manual could not be turned into a usable register symbol table."""


@dataclass(frozen=True)
class HumRegister:
    """One register as the Hardware User's Manual describes it.

    Attributes:
        chapter: HUM chapter number the register is described in.
        section: Full subsection number, e.g. ``3.2.6`` within that chapter.
        symbol: Register abbreviation exactly as printed, index letter and
            all (``EATMFSCq``, ``PTPTIVCt``, ``BCNTnAER``).
        offset: Base byte offset as printed, e.g. ``0x0040``; empty when the
            register is documented by absolute address rather than offset.
        offset_expr: The full offset expression including any array stride,
            e.g. ``0x0040 + 0x4 * q``; empty alongside an empty ``offset``.
        page: Printed page the register description begins on.
        page_end: Last page the description can still run onto -- the page of
            the next section heading. Bit-field tables routinely spill onto
            the following page, so a citation anywhere in ``page..page_end``
            is pointing at this register's description.
        name: The register's full name as printed.
    """

    chapter: int
    section: str
    symbol: str
    offset: str
    offset_expr: str
    page: int
    page_end: int
    name: str

    def covers(self, first: int, last: int) -> bool:
        """True when the cited page span overlaps this description's pages."""
        return first <= self.page_end and last >= self.page

    @property
    def canonical(self) -> str:
        """The symbol with index letters removed, for matching against C code."""
        return canonical_symbol(self.symbol)


def canonical_symbol(symbol: str) -> str:
    """Reduce a register abbreviation to the form a C header would spell.

    The manual writes array registers with a lowercase index letter wherever
    it reads best -- ``EATMFSCq``, ``IPCSEMn``, ``BCNTnAER``, ``PDCFCH00RCHn``
    -- while a C header names the array once (``EATMFSC[8]``). Since every
    genuine abbreviation is uppercase and digits, dropping every lowercase
    letter normalises both spellings onto one key. Across the whole manual
    this collides for exactly five symbols, and in each case the colliding
    pair is the same register written with two different index letters
    (``PVDmCR0`` / ``PVDnCR0``), so no distinct register is masked.

    The strip happens BEFORE the upper-case fold, not after: folding first
    would leave no lowercase letters to remove and quietly turn this into the
    identity function, so ``EATMFSCq`` and ``EATMFSC`` would stop matching.
    Callers holding a name in some other case (an offset enum suffix, say)
    must upper-case it themselves before calling.
    """
    return re.sub(r"[a-z]", "", symbol).upper()


def _fold_ascii(text: str, context: str) -> str:
    """Return `text` as 7-bit ASCII, or raise if it holds an unknown character."""
    out = "".join(ASCII_FOLD.get(ch, ch) for ch in text)
    bad = sorted({ch for ch in out if ord(ch) > MAX_ASCII_CODEPOINT})
    if bad:
        codes = ", ".join(f"U+{ord(ch):04X}" for ch in bad)
        msg = (
            f"non-ASCII character(s) {codes} in {context!r}; add a transliteration "
            f"to hum_regmap.ASCII_FOLD rather than dropping the character"
        )
        raise HumExtractionError(msg)
    return out


def _pdftotext(pdf: Path) -> str:
    """Run ``pdftotext -layout`` over the manual and return its text.

    Raises:
        HumExtractionError: pdftotext is missing, the PDF is absent, or the
            extraction failed. A gate that skipped here would report every
            register clean.
    """
    if shutil.which("pdftotext") is None:
        msg = "pdftotext not found on PATH (install poppler-utils)"
        raise HumExtractionError(msg)
    if not pdf.is_file():
        msg = f"manual PDF not found: {pdf}"
        raise HumExtractionError(msg)
    result = subprocess.run(  # noqa: S603 -- fixed argv, no shell
        ["pdftotext", "-layout", str(pdf), "-"],  # noqa: S607 -- poppler from PATH is intended
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        msg = f"pdftotext failed on {pdf.name}: {detail}"
        raise HumExtractionError(msg)
    return result.stdout.decode("utf-8", "replace")


def _find_offset(lines: list[str], start: int) -> str:
    """Return the offset expression printed below the heading at `start`."""
    limit = min(start + OFFSET_LOOKAHEAD_LINES, len(lines))
    for index in range(start + 1, limit):
        match = OFFSET_RE.match(lines[index])
        if match is not None:
            return match.group("expr")
        if HEADING_RE.match(lines[index]) is not None:
            break
    return ""


def _join_wrapped(lines: list[str], index: int) -> str:
    """Return the heading's register name, re-joining a typeset line wrap.

    A long heading wraps mid-name -- "... Configuration Register q (q = 0"
    then "to 7)" on the next line. An unbalanced parenthesis is the reliable
    tell, and pulling in the continuation keeps the committed name readable
    instead of truncated.
    """
    match = HEADING_RE.match(lines[index])
    name = "" if match is None else match.group("name")
    if name.count("(") <= name.count(")") or index + 1 >= len(lines):
        return name
    continuation = lines[index + 1].strip()
    if not continuation or HEADING_RE.match(lines[index + 1]) is not None:
        return name
    return f"{name} {continuation}"


def _heading_rows(lines: list[str], index: int, page_number: int) -> list[HumRegister]:
    """Build the register rows a single heading line declares, if any.

    A heading may name several access widths of one register at once
    (``TDR/TDRLL/TDRLH``); each alternate becomes its own row sharing the
    section, offset and page. ``page_end`` is filled in later, once the next
    section boundary is known.
    """
    match = HEADING_RE.match(lines[index])
    if match is None or TOC_LEADER_RE.search(match.group("name")):
        return []
    if int(match.group("chapter")) < 1:
        return []
    parts = ALTERNATE_SEP_RE.split(match.group("symbol"))
    alternates = [s for s in parts if _looks_like_register_symbol(s)]
    if not alternates:
        return []
    expr = _fold_ascii(_find_offset(lines, index), f"offset of {alternates[0]}")
    base_match = BASE_OFFSET_RE.match(expr)
    base = base_match.group(1) if base_match else ""
    name = _fold_ascii(_join_wrapped(lines, index), f"name of {alternates[0]}")
    return [
        HumRegister(
            chapter=int(match.group("chapter")),
            section=match.group("section"),
            symbol=symbol,
            offset=base,
            offset_expr=expr,
            page=page_number,
            page_end=page_number,
            name=name,
        )
        for symbol in alternates
    ]


def _scan_text(text: str) -> list[HumRegister]:
    """Walk the extracted manual and resolve each register's page range.

    Every numbered section heading -- register-bearing or not -- is recorded in
    document order. A register's description runs from its own heading page up
    to the page of the next heading, which is how a bit-field table spilling
    onto the following page stays inside the register it belongs to.
    """
    boundaries: list[tuple[int, list[HumRegister]]] = []
    for page_text in text.split("\f"):
        footer = FOOTER_RE.search(page_text)
        if footer is None:
            continue
        page_number = int(footer.group("page"))
        lines = page_text.split("\n")
        for index, line in enumerate(lines):
            if SECTION_RE.match(line) is None:
                continue
            boundaries.append((page_number, _heading_rows(lines, index, page_number)))

    rows: list[HumRegister] = []
    for position, (page_number, here) in enumerate(boundaries):
        following = boundaries[position + 1][0] if position + 1 < len(boundaries) else page_number
        end = max(page_number, following)
        rows.extend(replace(row, page_end=end) for row in here)
    rows.sort(key=lambda row: (row.chapter, row.page, row.section, row.symbol))
    return rows


def extract_from_pdf(pdf: Path = HUM_PDF) -> list[HumRegister]:
    """Parse every register description subsection out of the manual.

    Raises:
        HumExtractionError: the extraction failed or produced a vacuous result.
    """
    rows = _scan_text(_pdftotext(pdf))
    assert_not_vacuous(rows)
    return rows


def assert_not_vacuous(rows: list[HumRegister]) -> None:
    """Fail loudly when an extraction is too thin to have worked.

    An empty or near-empty symbol table makes every downstream check pass
    unconditionally, which reads as a clean tree. This is the guard that turns
    that failure mode into a red gate.

    Raises:
        HumExtractionError: any floor in this module was not met.
    """
    chapters = {row.chapter for row in rows}
    with_offset = sum(1 for row in rows if row.offset)
    problems = []
    if len(rows) < MIN_TOTAL_ROWS:
        problems.append(f"{len(rows)} register rows < floor {MIN_TOTAL_ROWS}")
    if len(chapters) < MIN_CHAPTERS_WITH_REGISTERS:
        problems.append(
            f"{len(chapters)} chapters with registers < floor {MIN_CHAPTERS_WITH_REGISTERS}"
        )
    if with_offset < MIN_ROWS_WITH_OFFSET:
        problems.append(f"{with_offset} rows carry an offset < floor {MIN_ROWS_WITH_OFFSET}")
    if problems:
        msg = (
            "HUM register extraction is vacuous -- every downstream check would "
            "pass unconditionally: " + "; ".join(problems)
        )
        raise HumExtractionError(msg)


def to_csv(rows: list[HumRegister]) -> str:
    """Serialise the symbol table to the committed CSV form."""
    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(CSV_FIELDS)
    for row in rows:
        writer.writerow(
            [
                row.chapter,
                row.section,
                row.symbol,
                row.offset,
                row.offset_expr,
                row.page,
                row.page_end,
                row.name,
            ]
        )
    return buffer.getvalue()


def from_csv(text: str) -> list[HumRegister]:
    """Parse the committed CSV form back into rows.

    Raises:
        HumExtractionError: the CSV header does not match :data:`CSV_FIELDS`,
            or the result is vacuous.
    """
    reader = csv.reader(io.StringIO(text, newline=""))
    try:
        header = next(reader)
    except StopIteration as exc:
        msg = "register map CSV is empty"
        raise HumExtractionError(msg) from exc
    if tuple(header) != CSV_FIELDS:
        msg = f"register map CSV header {header} != {list(CSV_FIELDS)}"
        raise HumExtractionError(msg)
    rows = [
        HumRegister(
            chapter=int(record[0]),
            section=record[1],
            symbol=record[2],
            offset=record[3],
            offset_expr=record[4],
            page=int(record[5]),
            page_end=int(record[6]),
            name=record[7],
        )
        for record in reader
        if record
    ]
    assert_not_vacuous(rows)
    return rows


class RegisterMap:
    """Chapter-indexed lookup over the manual's register symbol table."""

    def __init__(self, rows: list[HumRegister]) -> None:
        """Index `rows` by chapter and canonical symbol."""
        assert_not_vacuous(rows)
        self.rows = rows
        self._by_chapter: dict[int, dict[str, list[HumRegister]]] = {}
        for row in rows:
            self._by_chapter.setdefault(row.chapter, {}).setdefault(row.canonical, []).append(row)

    @property
    def chapters(self) -> set[int]:
        """Every chapter that publishes at least one register."""
        return set(self._by_chapter)

    def lookup(self, chapter: int, symbol: str) -> list[HumRegister]:
        """Every description of `symbol` inside `chapter`; empty when absent."""
        return self._by_chapter.get(chapter, {}).get(canonical_symbol(symbol), [])

    def find_anywhere(self, symbol: str) -> list[HumRegister]:
        """Every description of `symbol` in any chapter.

        Used only to sharpen a diagnostic: a symbol that is real but cited
        against the wrong chapter is a different (and much smaller) mistake
        than one the manual never mentions.
        """
        key = canonical_symbol(symbol)
        return [
            row for chapter in self._by_chapter for row in self._by_chapter[chapter].get(key, [])
        ]


def load(csv_path: Path = HUM_CSV) -> RegisterMap:
    """Load the committed register map CSV.

    Raises:
        HumExtractionError: the CSV is missing, malformed or vacuous.
    """
    if not csv_path.is_file():
        msg = f"register map not found: {csv_path} (run scripts/gen/gen_hum_register_map.py)"
        raise HumExtractionError(msg)
    return RegisterMap(from_csv(csv_path.read_text(encoding="utf-8")))
