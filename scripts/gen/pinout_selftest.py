#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Selftest for the pinout generator, asserting BOTH directions.

A checker that has quietly stopped matching is also perfectly silent, so
every case below is paired: something that must fire, and something that
must stay quiet. The failure mode that matters most for this generator is
not a crash but a *collapse* -- a table that still parses, into fewer
columns than it has, losing alternate functions without a word. That case is
asserted explicitly (``must fire: too few columns``), as is a figure the
cross-check would otherwise skip.

Run via ``gen_pinouts.py --selftest``; the ``pinout-freshness`` gate runs it
before the scan.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pinout_model import (  # noqa: E402
    DASH,
    FIGURE_VARIANTS,
    ParseError,
    decode_part,
)
from pinout_parse import figure_port_sets, parse_block  # noqa: E402
from pinout_render import wrap_cell  # noqa: E402

# Column origins of the miniature Standard-product page below. Twelve
# left-aligned columns, the shape pdftotext produces.
ORIGINS = (0, 9, 18, 27, 36, 50, 58, 67, 79, 105, 125, 137)


class Recorder:
    """Collects pass/fail so every case runs even after one fails."""

    def __init__(self) -> None:
        self.failures: list[str] = []

    def expect(self, name: str, condition: bool, detail: str = "") -> None:
        if condition:
            print(f"  ok   {name}")
        else:
            self.failures.append(f"{name}: {detail}")
            print(f"  FAIL {name}: {detail}")


def lay(cells) -> str:
    """Lay cells out at ORIGINS, the way pdftotext renders a table row."""
    line = ""
    for origin, cell in zip(ORIGINS, cells):
        line = line.ljust(origin) + cell
    return line


def fixture_page() -> list[str]:
    """A miniature Standard-product page, built from cells not typed text.

    Hand-typed fixed-width fixtures drift out of alignment silently; this one
    cannot, because it is laid out by the same rule the assertions assume.
    """
    d = DASH
    return [
        "         BGA289            BGA224",
        "BGA289   MIPI     BGA224   MIPI     Debug, CAC   ports",
        "",
        lay(["A1", "A1", "C4", "C4", d, "P609", "D7/DQ7", "IRQ29",
             "TXD0_C/SDA0_C/", "GTIU/", d, "LCD_DA"]),
        lay(["", "", "", "", "", "", "", "",
             "MOSI0_C", "GTIOC5B", "", "TA6_A"]),
        lay(["A2", d, "C5", "C5", "VSS", d, d, d, d, d, d, d]),
        lay(["A3", "A3", d, d, d, "P610", d, "IRQ2", d, d, d, d]),
        "",
        "R01DS0493EJ0130 Rev.1.30                          Page 26 of 292",
    ]


def check_table_parse(rec: Recorder) -> None:
    """The pin-list page parser, both directions."""
    rows = parse_block(fixture_page(), 4)
    rec.expect("parses every data row", len(rows) == 3, f"got {len(rows)}")
    if len(rows) == 3:
        rec.expect("joins a wrapped cell",
                   rows[0][8] == "TXD0_C/SDA0_C/MOSI0_C",
                   f"got {rows[0][8]!r}")
        rec.expect("joins a wrapped trailing cell",
                   rows[0][11] == "LCD_DATA6_A", f"got {rows[0][11]!r}")
        rec.expect("keeps the dashed ball column",
                   rows[1][1] == DASH, f"got {rows[1][1]!r}")
        rec.expect("ignores the page footer",
                   rows[2][0] == "A3" and rows[2][5] == "P610",
                   f"got {rows[2]!r}")

    # MUST FIRE: rows that do not agree on a layout.
    try:
        parse_block(["A1  A1  C4  P609", "A2  A2", "A3   A3  C6"], 4)
        rec.expect("rejects a ragged page", False, "no ParseError raised")
    except ParseError:
        rec.expect("rejects a ragged page", True)

    # MUST FIRE: rows that agree on the WRONG layout. This is the collapse
    # mode -- a table that parses cleanly into too few columns silently
    # drops every alternate function past the cut.
    narrow = [lay(["A1", "A1", "C4", "C4", DASH, "P609"]) for _ in range(4)]
    try:
        parse_block(narrow, 4)
        rec.expect("rejects a page with too few columns", False, "accepted")
    except ParseError:
        rec.expect("rejects a page with too few columns", True)


def check_part_numbers(rec: Recorder) -> None:
    """Part-number decoding, both directions."""
    part = decode_part("R7KA8D2KFLCAC")
    rec.expect(
        "decodes the EK-RA8D2 part",
        part.group == "RA8D2" and part.cores == "dual" and part.mipi
        and part.package == "AC",
        f"got {part}",
    )

    # MUST FIRE: a package code the product matrix does not define.
    try:
        decode_part("R7KA8D2ADLCAZ")
        rec.expect("rejects an unknown package code", False, "accepted AZ")
    except ParseError:
        rec.expect("rejects an unknown package code", True)

    # MUST FIRE: the three fields that encode SiP-ness disagreeing.
    try:
        decode_part("R7KA8D2JRLSAJ")  # R7K says MRAM, S/AJ say SiP
        rec.expect("rejects inconsistent SiP encoding", False, "accepted")
    except ParseError:
        rec.expect("rejects inconsistent SiP encoding", True)


def check_figures(rec: Recorder) -> None:
    """The section 1.6 figure cross-check, both directions."""
    doc = "\n".join(
        f"  {label} grid  P0{i:02d}  P1{i:02d}\n"
        f"  Figure 1.{i + 3}      Pin assignment for {label}"
        for i, label in enumerate(FIGURE_VARIANTS)
    )
    figures = figure_port_sets(doc)
    rec.expect("reads every pin-assignment figure",
               set(figures) == set(FIGURE_VARIANTS.values()),
               f"got {sorted(figures)}")
    rec.expect("reads a figure's port pins",
               figures[FIGURE_VARIANTS["BGA 289-pin"]] == {"P000", "P100"},
               f"got {figures[FIGURE_VARIANTS['BGA 289-pin']]}")

    # MUST FIRE: a renamed or dropped figure. Skipping one would disarm the
    # cross-check for that variant while still reporting success.
    try:
        figure_port_sets(doc.replace(
            "Figure 1.3      Pin assignment for BGA 289-pin", ""))
        rec.expect("rejects a missing pin-assignment figure", False,
                   "accepted")
    except ParseError:
        rec.expect("rejects a missing pin-assignment figure", True)


def check_rendering(rec: Recorder) -> None:
    """Cell wrapping: never over width, never loses a character."""
    wrapped = wrap_cell("A" * 30 + "/B/C", 10)
    rec.expect("wrap_cell honours the width",
               all(len(line) <= 10 for line in wrapped), f"got {wrapped}")
    rec.expect("wrap_cell is lossless",
               "".join(wrapped) == "A" * 30 + "/B/C", f"got {wrapped}")
    rec.expect("wrap_cell marks an empty field",
               wrap_cell("", 10) == ["-"], f"got {wrap_cell('', 10)}")


def run() -> int:
    """Run every case and return a process exit code."""
    print("gen_pinouts selftest")
    rec = Recorder()
    for case in (check_table_parse, check_part_numbers, check_figures,
                 check_rendering):
        case(rec)
    if rec.failures:
        print(f"gen_pinouts selftest: {len(rec.failures)} failure(s)")
        return 1
    print("gen_pinouts selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(run())
