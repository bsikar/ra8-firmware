# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Recover the RA8 pin-list tables from the datasheet PDFs.

``pdftotext -layout`` renders each printed page as fixed-width text, and
within a single page every table cell is left-aligned on its column. That is
the only layout fact this module relies on: column origins are read back as
the modal tuple of word-start offsets across a page's data rows, so nothing
here carries a hardcoded offset that a document revision could silently
invalidate. A page whose rows do not agree on one tuple raises rather than
producing a mangled table.

Two independent renderings of the same fact are read:

* section 1.7's pin-list TABLES, which carry the full alternate-function set
  per ball and are the parse target, and
* section 1.6's ball-grid FIGURES, whose per-variant port-pin sets
  ``gen_pinouts.py`` diffs against the tables as a cross-check.

Used by ``gen_pinouts.py``; see that file for the whole pipeline.
"""

from __future__ import annotations

import collections
import re
import shutil
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pinout_model import (
    BALL_COLUMNS,
    BALL_RE,
    DASH,
    FIELDS,
    FIGURE_RE,
    FIGURE_VARIANTS,
    PORT_RE,
    ROW_START_RE,
    TABLE_RE,
    Group,
    ParseError,
    Part,
    decode_part,
)

MIN_LAYOUT_VOTES = 2


def pdf_text(pdf: Path) -> str:
    """Return the whole PDF as layout-preserving text."""
    pdftotext = shutil.which("pdftotext")
    if pdftotext is None:
        msg = (
            "pdftotext not found. Install poppler "
            "(apt-get install poppler-utils / brew install poppler)."
        )
        raise ParseError(msg)
    if not pdf.is_file():
        msg = f"datasheet not found: {pdf}"
        raise ParseError(msg)
    proc = subprocess.run(  # noqa: S603 -- executable resolved by shutil.which
        [pdftotext, "-layout", str(pdf), "-"],
        capture_output=True,
        text=True,
        check=True,
    )
    return proc.stdout


def split_table_blocks(text: str) -> Iterator[tuple[str, str, int, int, list[str]]]:
    """Yield ``(table_no, kind, part, total, lines)`` per printed table page.

    Each printed page repeats the table caption, so a block is exactly one
    page of one table -- which is the unit over which ``pdftotext -layout``
    keeps column offsets constant.
    """
    for page in text.split("\f"):
        current = None
        lines: list[str] = []
        for line in page.split("\n"):
            match = TABLE_RE.match(line.strip())
            if match:
                if current is not None:
                    yield (*current, lines)
                current = (
                    match.group(1),
                    match.group(2).lower(),
                    int(match.group(3)),
                    int(match.group(4)),
                )
                lines = []
            elif current is not None:
                lines.append(line)
        if current is not None:
            yield (*current, lines)


def column_origins(rows: list[str], n_cols: int) -> tuple[int, ...]:
    """Recover the column origins of one printed page from its data rows.

    Every cell is left-aligned on its column and no cell value contains a
    space, so a row that is not wrapped contributes exactly ``n_cols``
    word-start offsets -- the column origins. Taking the modal tuple
    tolerates the wrapped rows without trusting any single row.
    """
    votes = collections.Counter(tuple(m.start() for m in re.finditer(r"\S+", row)) for row in rows)
    for origins, count in votes.most_common():
        if len(origins) == n_cols:
            if count < MIN_LAYOUT_VOTES:
                msg = f"only {count} row(s) agree on a {n_cols}-column layout"
                raise ParseError(msg)
            return origins
    msg = f"no row on this page has {n_cols} columns (saw widths {sorted({len(o) for o in votes})})"
    raise ParseError(msg)


def slice_row(line: str, origins: tuple[int, ...]) -> list[str]:
    """Split one physical line into cells at the given column origins."""
    bounds = [*list(origins), len(line) + 1]
    return [line[bounds[i] : bounds[i + 1]].strip() for i in range(len(origins))]


def aligned_continuation(line: str, origins: tuple[int, ...]) -> bool:
    """True if every word on ``line`` starts on a column origin.

    This is what separates a wrapped cell from the running page header and
    the page footer, which are never column-aligned.
    """
    starts = [m.start() for m in re.finditer(r"\S+", line)]
    return bool(starts) and all(s in origins for s in starts)


def parse_block(lines: list[str], n_ball_cols: int) -> list[list[str]]:
    """Parse one printed page into fully joined logical rows."""
    n_cols = n_ball_cols + len(FIELDS)
    data = [line for line in lines if ROW_START_RE.match(line)]
    if not data:
        return []
    origins = column_origins(data, n_cols)

    rows: list[list[str]] = []
    for line in lines:
        if ROW_START_RE.match(line):
            cells = slice_row(line, origins)
            if len(cells) != n_cols:
                msg = f"row split into {len(cells)} cells: {line!r}"
                raise ParseError(msg)
            rows.append(cells)
        elif rows and aligned_continuation(line, origins):
            # A wrapped cell. pdftotext breaks mid-token, so the fragments
            # concatenate with no separator.
            for i, frag in enumerate(slice_row(line, origins)):
                rows[-1][i] += frag
    return rows


def parse_pin_list(text: str, group: Group, kind: str) -> list[dict]:
    """Parse one whole pin-list table into logical rows."""
    ball_cols = BALL_COLUMNS[kind]
    want = group.std_table if kind == "standard" else group.sip_table

    seen_parts: set[int] = set()
    total_parts = None
    rows: list[list[str]] = []
    for table_no, block_kind, part, total, lines in split_table_blocks(text):
        if block_kind != kind:
            continue
        if table_no != want:
            msg = (
                f"{group.name}: expected the {kind} pin list to be Table "
                f"{want}, found Table {table_no}"
            )
            raise ParseError(msg)
        seen_parts.add(part)
        total_parts = total
        rows.extend(parse_block(lines, len(ball_cols)))

    if total_parts is None:
        msg = f"{group.name}: no {kind} pin list found"
        raise ParseError(msg)
    missing = set(range(1, total_parts + 1)) - seen_parts
    if missing:
        msg = f"{group.name} {kind} pin list: missing page(s) {sorted(missing)} of {total_parts}"
        raise ParseError(msg)

    out = []
    for cells in rows:
        balls = [c if c != DASH else None for c in cells[: len(ball_cols)]]
        for ball in balls:
            if ball is not None and not BALL_RE.match(ball):
                msg = f"not a ball coordinate: {ball!r}"
                raise ParseError(msg)
        entry = {"balls": dict(zip(ball_cols, balls))}
        for name, cell in zip(FIELDS, cells[len(ball_cols) :]):
            entry[name] = "" if cell == DASH else cell
        out.append(entry)
    return out


def figure_port_sets(text: str) -> dict:
    """Read the port pins off section 1.6's ball-grid figures.

    Section 1.6 and section 1.7 are two independent renderings of the same
    fact, so agreeing with the figures is evidence the table parse is right
    rather than merely self-consistent. The figures' cells wrap and stack
    unpredictably, which is why the pin lists are the parse target and this
    is only a cross-check -- the SET of port pins per variant survives the
    figures' messy layout, and it is exactly the quantity a mis-sliced
    column would corrupt.
    """
    lines = text.replace("\f", "\n").split("\n")
    captions = []
    for i, line in enumerate(lines):
        match = FIGURE_RE.match(line)
        if match:
            captions.append((i, match.group(1)))
    if len(captions) != len(FIGURE_VARIANTS):
        msg = f"expected {len(FIGURE_VARIANTS)} pin-assignment figures, found {len(captions)}"
        raise ParseError(msg)

    out = {}
    for index, (end, label) in enumerate(captions):
        if label not in FIGURE_VARIANTS:
            msg = f"unknown pin-assignment figure: {label!r}"
            raise ParseError(msg)
        start = captions[index - 1][0] + 1 if index else 0
        ports = set(PORT_RE.findall("\n".join(lines[start:end])))
        out[FIGURE_VARIANTS[label]] = ports
    return out


def extract_parts(text: str, group: Group) -> list[Part]:
    """Pull the product list for one group out of its datasheet."""
    numbers = []
    for token in re.findall(r"\bR7[KJ]A8(?:D2|P1)[A-Z]{4}A[BCJ]\b", text):
        if token not in numbers:
            numbers.append(token)
    parts = [
        decode_part(n)
        for n in numbers
        if n.startswith((f"R7KA8{group.name[3:]}", f"R7JA8{group.name[3:]}"))
    ]
    if not parts:
        msg = f"{group.name}: no part numbers found"
        raise ParseError(msg)
    return parts
