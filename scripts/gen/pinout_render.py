#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Render parsed RA8 pin lists into the files under ``docs/pinouts/``.

Two shapes are produced. A per-variant plain-text reference carries three
views of one ball map -- the physical grid as the package is drawn, the full
alternate-function table per ball, and a port-name index for the common
"which ball is P409" lookup. A Markdown index resolves any of the 64 part
numbers to its variant file.

Nothing here reads a PDF or decides a fact; it is handed parsed rows and
formats them. Used by ``gen_pinouts.py``; see that file for the whole
pipeline.
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pinout_model import (  # noqa: E402
    COLUMN_ORDER,
    FIELD_HEADINGS,
    GROUPS,
    MRAM_SIZES,
    PACKAGES,
    REPO_ROOT,
    TEMP_GRADES,
    Group,
    Part,
    ball_key,
    port_key,
)

RULE = "-" * 80

# Column widths of the per-ball function table. COMMS is by far the widest
# field in the source (a single ball can offer a dozen alternates), so it
# gets the slack; anything longer wraps within its own column.
TABLE_WIDTHS = {
    "ball": 5, "port": 6, "power": 12, "exbus": 9, "irq": 11,
    "comms": 44, "timer": 24, "analog": 11, "video": 18,
}

PORTS_PER_LINE = 6


def wrap_cell(value: str, width: int) -> list[str]:
    """Wrap a slash-separated function list to ``width``, breaking on '/'.

    Alternate-function lists are slash-separated, so a break after a slash
    reads naturally and never splits a signal name unless one name alone
    exceeds the column -- in which case it is split rather than allowed to
    push the table out of alignment.
    """
    if not value:
        return ["-"]
    pieces = [p + "/" for p in value.split("/")]
    pieces[-1] = pieces[-1][:-1]
    out: list[str] = []
    line = ""
    for piece in pieces:
        if line and len(line) + len(piece) > width:
            out.append(line)
            line = ""
        while len(piece) > width:
            if line:
                out.append(line)
                line = ""
            out.append(piece[:width])
            piece = piece[width:]
        line += piece
    if line:
        out.append(line)
    return out


def render_grid(rows: list[dict], column: tuple) -> list[str]:
    """Render the physical ball grid, top view, one cell per ball."""
    placed = {}
    for row in rows:
        ball = row["balls"][column]
        if ball is not None:
            placed[ball] = row["port"] or row["power"] or "-"

    letters = sorted({b.rstrip("0123456789") for b in placed},
                     key=lambda r: (len(r), r))
    numbers = sorted({int(b.lstrip("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
                      for b in placed})
    width = max(max(len(v) for v in placed.values()),
                max(len(str(n)) for n in numbers))
    pad = max(len(letter) for letter in letters)

    out = [" " * pad + " " + " ".join(str(n).center(width) for n in numbers)]
    for letter in letters:
        cells = [placed.get(f"{letter}{num}", "").center(width)
                 for num in numbers]
        out.append(f"{letter:<{pad}} " + " ".join(cells).rstrip())
    return out


def _variant_header(group: Group, package: str, mipi: bool,
                    balls: int, io_pins: int) -> list[str]:
    """The provenance block every variant file opens with."""
    pkg = PACKAGES[package]
    table = group.sip_table if pkg.sip else group.std_table
    kind = "SiP" if pkg.sip else "Standard"
    title = (f"{group.name} group pinout -- LFBGA {pkg.balls}-pin, "
             f"{'with' if mipi else 'without'} MIPI DSI/CSI")

    lines = ["=" * 80, title, "=" * 80, "",
             "GENERATED FILE -- DO NOT EDIT BY HAND.",
             "  Regenerate with: python3 scripts/gen/gen_pinouts.py",
             "",
             f"Source : {group.name} Group Datasheet {group.doc_id}, "
             f"section 1.7",
             f"         \"Pin Lists\", Table {table} \"Pin list for the "
             f"{kind} product\",",
             f"         column \"BGA{pkg.balls}"
             f"{'' if mipi else ' without MIPI'}\".",
             "",
             f"Package       : {pkg.renesas} ({pkg.jeita})",
             f"                LFBGA {pkg.balls}-pin, {pkg.body}",
             f"Balls         : {balls}",
             f"I/O port pins : {io_pins}",
             f"MIPI DSI/CSI  : {'available' if mipi else 'not available'}"]
    lines += textwrap.wrap(group.tagline, width=64,
                           initial_indent="Group         : ",
                           subsequent_indent="                ")
    return lines + [""]


def _variant_parts(parts: list[Part], package: str, mipi: bool) -> list[str]:
    """The list of part numbers that share this ball map."""
    matching = [p for p in parts if p.package == package and p.mipi == mipi]
    lines = [f"Part numbers using this ball map ({len(matching)}):", "",
             f"    {'Part number':<16} {'Cores':<7} {'Code memory':<32} "
             f"{'Junction temp':<14}".rstrip(),
             f"    {'-' * 16} {'-' * 7} {'-' * 32} {'-' * 14}"]
    for part in matching:
        lines.append(
            f"    {part.number:<16} {part.cores:<7} "
            f"{MRAM_SIZES[part.mram]:<32} {TEMP_GRADES[part.temp]:<14}"
        )
    return lines + [
        "",
        "Every part number above has an identical ball map; they",
        "differ only in core count, memory size and temperature",
        "grade. See docs/pinouts/README.md for the full matrix.",
        "",
    ]


def _function_table(rows: list[dict], column: tuple) -> list[str]:
    """Section 2: every ball with every alternate function it offers."""
    lines = [RULE, "2. Alternate functions per ball", RULE, ""]
    lines += [f"  {key.upper():<7}{FIELD_HEADINGS[key]}"
              for key in COLUMN_ORDER]
    lines += [
        "",
        "  A field reads \"-\" when the ball offers nothing in that",
        "  category. Suffixes _A/_B/_C are the datasheet's pin-",
        "  candidate variants; \"-DS\" marks a deep-standby-capable",
        "  interrupt input.",
        "",
    ]

    header = f"{'BALL':<{TABLE_WIDTHS['ball']}}" + "".join(
        f"{k.upper():<{TABLE_WIDTHS[k] + 1}}" for k in COLUMN_ORDER)
    lines += [header.rstrip(), "-" * len(header.rstrip())]

    for row in rows:
        cells = {k: wrap_cell(row[k], TABLE_WIDTHS[k]) for k in COLUMN_ORDER}
        for i in range(max(len(v) for v in cells.values())):
            ball = row["balls"][column] if i == 0 else ""
            text = f"{ball:<{TABLE_WIDTHS['ball']}}"
            for key in COLUMN_ORDER:
                piece = cells[key][i] if i < len(cells[key]) else ""
                text += f"{piece:<{TABLE_WIDTHS[key] + 1}}"
            lines.append(text.rstrip())
    return lines + [""]


def _port_index(io_pins: list[dict], column: tuple) -> list[str]:
    """Section 3: port name -> ball, the lookup people actually run."""
    lines = [RULE, "3. I/O port pin index", RULE, "",
             f"{len(io_pins)} port pins, in port order.", ""]
    by_port = sorted(io_pins, key=lambda r: port_key(r["port"]))
    for i in range(0, len(by_port), PORTS_PER_LINE):
        chunk = by_port[i:i + PORTS_PER_LINE]
        lines.append("    " + "  ".join(
            f"{r['port']:<5} {r['balls'][column]:<4}" for r in chunk).rstrip())
    return lines + [""]


def render_variant(group: Group, package: str, mipi: bool,
                   rows: list[dict], parts: list[Part]) -> str:
    """Render one variant's whole reference file."""
    column = (package, mipi)
    mine = sorted((r for r in rows if r["balls"][column] is not None),
                  key=lambda r: ball_key(r["balls"][column]))
    io_pins = [r for r in mine if r["port"]]

    lines = _variant_header(group, package, mipi, len(mine), len(io_pins))
    lines += _variant_parts(parts, package, mipi)
    lines += [RULE, "1. Ball grid (top view)", RULE, "",
              "Each cell names the I/O port pin, or the power/system",
              "function for balls that are not port pins. Blank = no ball.",
              ""]
    lines += render_grid(rows, column)
    lines.append("")
    lines += _function_table(mine, column)
    lines += _port_index(io_pins, column)
    return "\n".join(lines) + "\n"


def _index_intro() -> list[str]:
    return [
        "# RA8 pinout reference",
        "",
        "Ball maps for every orderable RA8D2 and RA8P1 part number, parsed",
        "out of section 1.7 \"Pin Lists\" of the two group datasheets by",
        "`scripts/gen/gen_pinouts.py`. **These files are generated -- edit "
        "the",
        "generator, not the output.** `gen_pinouts.py --check` runs in CI, "
        "so",
        "a datasheet revision that moves a ball cannot land without the",
        "reference moving with it.",
        "",
        "## Which file do I want?",
        "",
        "A part number's ball map is fixed by exactly two of its fields: the",
        "**package** and whether its **feature set** bonds out MIPI DSI/CSI",
        "(`B` and `K` do; `A` and `J` do not). Memory size, core count and",
        "temperature grade never move a ball, so the 64 part numbers below",
        "collapse onto 12 ball maps.",
        "",
    ]


def _index_variant_table(variants: list) -> list[str]:
    lines = ["| Group | Package | MIPI DSI/CSI | Balls | I/O | Pinout file |",
             "|---|---|---|---|---|---|"]
    for group, package, mipi, filename, balls, io_pins, _ in variants:
        pkg = PACKAGES[package]
        lines.append(
            f"| {group} | LFBGA {pkg.balls}{' (SiP)' if pkg.sip else ''} | "
            f"{'yes' if mipi else 'no'} | {balls} | {io_pins} | "
            f"[`{filename}`]({filename}) |"
        )
    return lines + [""]


def _index_part_table(parts: list) -> list[str]:
    lines = [
        "## Part number -> ball map",
        "",
        "Decoded from the part-numbering scheme (Figure 1.2 of either",
        "datasheet), cross-checked against the printed product list.",
        "",
        "| Part number | Group | Cores | MIPI | Code memory | SRAM[^sram] "
        "| Junction temp | Package | Pinout file |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for part, sram, filename in parts:
        pkg = PACKAGES[part.package]
        lines.append(
            f"| `{part.number}` | {part.group} | {part.cores} | "
            f"{'yes' if part.mipi else 'no'} | {MRAM_SIZES[part.mram]} | "
            f"{sram} | {TEMP_GRADES[part.temp]} | "
            f"LFBGA {pkg.balls}{' SiP' if pkg.sip else ''} | "
            f"[`{filename}`]({filename}) |"
        )
    return lines + [
        "",
        "[^sram]: SRAM is the one column here that is not pinout data and "
        "not read",
        "    per-part: the Function Comparison table merges it across "
        "columns, giving",
        "    1792 KB for the single-core feature sets (`A`, `B`) and 1664 KB "
        "for the",
        "    dual-core ones (`J`, `K`), the latter spending 128 KB on the "
        "CM33 TCM.",
        "    Every SiP part is dual-core. Take it as orientation and confirm "
        "against",
        "    the datasheet before sizing anything against it.",
        "",
    ]


def _index_scheme() -> list[str]:
    return [
        "## Reading a part number",
        "",
        "```",
        "R 7 K A 8 D 2 A D L C AB",
        "          | | | | | |  +-- package: AB=LFBGA224 AC=LFBGA289 "
        "AJ=LFBGA303",
        "          | | | | | +----- quality grade: C=standard S=SiP",
        "          | | | | +------- junction temp: L=0..95C D=-40..105C",
        "          | | | +--------- code memory: D=512KB F=1MB "
        "R=1MB+4MB S=1MB+8MB",
        "          | | +----------- feature set: A/B single core, "
        "J/K dual core;",
        "          | |              B/K bond out MIPI DSI/CSI, A/J do not",
        "          +-+------------- group: D2=RA8D2, P1=RA8P1",
        "```",
        "",
        "The leading `R7K`/`R7J` also encodes the memory technology "
        "(`K`=MRAM,",
        "`J`=MRAM+flash SiP) and so tracks the quality-grade and package",
        "fields; the generator rejects a part number where the three",
        "disagree.",
        "",
    ]


def _index_sources() -> list[str]:
    lines = ["## Sources", "",
             "| Group | Datasheet | Committed as |", "|---|---|---|"]
    for group in GROUPS:
        lines.append(
            f"| {group.name} | {group.doc_id} | "
            f"`{group.pdf.relative_to(REPO_ROOT)}` |"
        )
    return lines + [
        "",
        "The Hardware User's Manual, not the datasheet, is the authority on",
        "*register* programming for any of these pins -- see",
        "`docs/reference/README.md`. The datasheet is the authority on which",
        "ball carries which function, which is what these files record.",
        "",
    ]


def render_index(data: dict) -> str:
    """Render `docs/pinouts/README.md`, the part-number -> file resolver."""
    lines = _index_intro()
    lines += _index_variant_table(data["variants"])
    lines += _index_part_table(data["parts"])
    lines += _index_scheme()
    lines += ["## Pin compatibility between the two groups", ""]
    lines += data["compat"]
    lines.append("")
    lines += _index_sources()
    return "\n".join(lines) + "\n"
