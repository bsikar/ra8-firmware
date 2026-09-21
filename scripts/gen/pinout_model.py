# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Product matrix and pin-list table shape for the RA8D2 and RA8P1 groups.

Everything here is transcribed from the two datasheets' own front matter --
the part-numbering scheme figure, the product list, the package dimensions
appendix and the Function Comparison table -- and nothing here parses. It is
the vocabulary the parser and the renderer share, kept in one file so a
product-matrix fact has exactly one home.

Used by ``gen_pinouts.py``; see that file for the whole pipeline.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "docs" / "pinouts"

# The em dash the datasheet prints for "this variant has no such pin".
# Spelled as an escape so this file stays 7-bit ASCII.
DASH = "\u2014"


class ParseError(RuntimeError):
    """The datasheet did not have the shape this parser requires."""


# ---------------------------------------------------------------------------
# Product matrix, from the "Part numbering scheme" figure of each datasheet
# (RA8D2 Figure 1.2 p 9; RA8P1 Figure 1.2 p 9 -- the two figures are
# character-for-character identical apart from the group digits).
# ---------------------------------------------------------------------------

FEATURE_SETS = {
    "A": ("single", False),  # Single core (CM85 only), no MIPI DSI/CSI
    "B": ("single", True),  # Single core (CM85 only), MIPI DSI/CSI
    "J": ("dual", False),  # Dual core, no MIPI DSI/CSI
    "K": ("dual", True),  # Dual core, MIPI DSI/CSI
}

MRAM_SIZES = {
    "D": "512 KB MRAM",
    "F": "1 MB MRAM",
    "R": "5 MB (1 MB MRAM + 4 MB flash)",
    "S": "9 MB (1 MB MRAM + 8 MB flash)",
}

TEMP_GRADES = {
    "L": "0 to 95 C",
    "D": "-40 to 105 C",
}


@dataclass(frozen=True)
class Package:
    """One package option from the part-number scheme."""

    code: str  # part-number package field, e.g. "AC"
    balls: int  # ball count
    renesas: str  # Renesas package code, e.g. "PLBG0289JA-A"
    jeita: str  # JEITA outline, e.g. "P-LFBGA289-12x12-0.65"
    body: str  # human-readable body/pitch summary
    sip: bool  # True for the MRAM+flash SiP package


PACKAGES = {
    "AB": Package(
        "AB",
        224,
        "PLBG0224JA-A",
        "P-LFBGA224-11x11-0.65",
        "11 mm x 11 mm, 0.65 mm pitch",
        sip=False,
    ),
    "AC": Package(
        "AC",
        289,
        "PLBG0289JA-A",
        "P-LFBGA289-12x12-0.65",
        "12 mm x 12 mm, 0.65 mm pitch",
        sip=False,
    ),
    "AJ": Package(
        "AJ",
        303,
        "PLBG0303GA-A",
        "P-LFBGA303-15x15-0.80",
        "15 mm x 15 mm, 0.80 mm pitch",
        sip=True,
    ),
}


@dataclass(frozen=True)
class Group:
    """One MCU group and the datasheet that defines its pin lists."""

    name: str  # "RA8D2"
    slug: str  # "ra8d2"
    pdf: Path  # committed datasheet
    doc_id: str  # "R01DS0493EJ"
    std_table: str  # table number of the Standard-product pin list
    sip_table: str  # table number of the SiP-product pin list
    tagline: str  # one-line group description


GROUPS = (
    Group(
        name="RA8D2",
        slug="ra8d2",
        pdf=REPO_ROOT / "docs" / "reference" / "ra8d2-datasheet.pdf",
        doc_id="R01DS0493EJ",
        std_table="1.16",
        sip_table="1.17",
        tagline="Arm Cortex-M85 @ 1 GHz (+ Cortex-M33 @ 250 MHz on dual-core "
        "feature sets), graphics and Ethernet MCU",
    ),
    Group(
        name="RA8P1",
        slug="ra8p1",
        pdf=REPO_ROOT / "docs" / "reference" / "ra8p1-datasheet.pdf",
        doc_id="R01DS0439EJ",
        std_table="1.17",
        sip_table="1.18",
        tagline="RA8D2 plus an Arm Ethos-U55 NPU; pin-compatible with the RA8D2 in every package",
    ),
)

# ---------------------------------------------------------------------------
# Pin-list table shape

# Field names for the trailing (non-ball) columns, in printed order. Both
# the Standard and the SiP table carry the same eight; they differ only in
# how many leading ball columns precede them.
FIELDS = ("power", "port", "exbus", "irq", "comms", "timer", "analog", "video")

FIELD_HEADINGS = {
    "port": "I/O port",
    "power": "Power, system, clock, debug, CAC",
    "exbus": "External bus / SDRAM",
    "irq": "External interrupt",
    "comms": "SCI/IIC/I3C/SPI/CANFD/USBFS/USBHS/OSPI/SSIE/SDHI/MMC/ESWM(GMII,RGMII,MII,RMII)/PDMIF",
    "timer": "GPT/AGT/ULPT/RTC",
    "analog": "ADC16H/DAC12/ACMPHS",
    "video": "MIPI/GLCDC/CEU",
}

# The order the columns are printed in, which is not the order the
# datasheet's raw cells arrive in (it prints the port name second).
COLUMN_ORDER = ("port", "power", "exbus", "irq", "comms", "timer", "analog", "video")

# Leading ball columns of each table, in printed order, as (package code,
# has-MIPI). The datasheet prints "<pkg>" then "<pkg> without MIPI".
BALL_COLUMNS = {
    "standard": (("AC", True), ("AC", False), ("AB", True), ("AB", False)),
    "sip": (("AJ", True), ("AJ", False)),
}

# Section 1.6 prints the same information a second way, as one ball-grid
# figure per variant. Its caption sits BELOW the grid, so a figure's text
# runs from the previous caption to its own.
FIGURE_RE = re.compile(r"^\s*Figure 1\.[3-8]\s+Pin assignment for (.+?)\s*$")

# Figure caption -> the variant it draws.
FIGURE_VARIANTS = {
    "BGA 289-pin": ("AC", True),
    "without_MIPI_BGA 289-pin": ("AC", False),
    "BGA 224-pin": ("AB", True),
    "without_MIPI_BGA 224-pin": ("AB", False),
    "BGA 303-pin": ("AJ", True),
    "without_MIPI_BGA 303-pin": ("AJ", False),
}

PORT_RE = re.compile(r"\bP(?:[0-9A-D])\d{2}\b")

# I/O port counts per variant, from the "Function Comparison" table
# (RA8D2 Table 1.14 p 11; RA8P1 Table 1.15 p 11). Used as a parse floor:
# a run that recovers a different count has mis-parsed the pin list.
EXPECTED_IO_PORTS = {
    ("AB", False): 149,
    ("AB", True): 142,
    ("AC", False): 208,
    ("AC", True): 199,
    ("AJ", False): 195,
    ("AJ", True): 186,
}

TABLE_RE = re.compile(
    r"^Table\s+(\d+\.\d+)\s+Pin list for the (Standard|SiP) product\s+"
    r"\((\d+) of (\d+)\)\s*$"
)

BALL_RE = re.compile(r"^[A-Z]{1,2}\d{1,2}$")
ROW_START_RE = re.compile(r"^(?:[A-Z]{1,2}\d{1,2}|" + DASH + r")(?:\s|$)")


@dataclass(frozen=True)
class Part:
    """One orderable part number, decoded from its own characters."""

    number: str
    group: str
    feature: str
    mram: str
    temp: str
    quality: str
    package: str

    @property
    def cores(self) -> str:
        """Return the core-count class encoded by this part's feature set."""
        return FEATURE_SETS[self.feature][0]

    @property
    def mipi(self) -> bool:
        """Return whether this part bonds out the MIPI DSI/CSI pins."""
        return FEATURE_SETS[self.feature][1]


def decode_part(number: str) -> Part:
    """Decode a part number per the datasheet part-numbering scheme."""
    match = re.fullmatch(
        r"R7(?P<mem>[KJ])A8(?P<grp>D2|P1)(?P<feat>[ABJK])(?P<mram>[DFRS])"
        r"(?P<temp>[LD])(?P<qual>[CS])(?P<pkg>A[BCJ])",
        number,
    )
    if not match:
        msg = f"unparsable part number: {number}"
        raise ParseError(msg)
    part = Part(
        number=number,
        group=f"RA8{match['grp']}",
        feature=match["feat"],
        mram=match["mram"],
        temp=match["temp"],
        quality=match["qual"],
        package=match["pkg"],
    )
    # The leading memory letter and the quality grade encode the same fact
    # from two directions; disagreement means a misread part number.
    sip = PACKAGES[part.package].sip
    if (match["mem"] == "J") != sip or (part.quality == "S") != sip:
        msg = f"inconsistent SiP encoding in {number}"
        raise ParseError(msg)
    return part


def variant_slug(package: str, mipi: bool) -> str:
    """Return the stable generated-file slug for one package variant."""
    pkg = PACKAGES[package]
    kind = "mipi" if mipi else "nomipi"
    tail = "_sip" if pkg.sip else ""
    return f"bga{pkg.balls}{tail}_{kind}"


def ball_key(ball: str) -> tuple:
    """Sort key putting balls in printed order: row letter, then number."""
    match = re.fullmatch(r"([A-Z]{1,2})(\d{1,2})", ball)
    row, num = match.group(1), int(match.group(2))
    return (len(row), row, num)


def port_key(port: str) -> tuple:
    """Return a numeric sort key for a port name, after known port names."""
    match = re.fullmatch(r"P([0-9A-F])(\d{2})", port)
    if not match:
        return (1, port, 0)
    return (0, match.group(1), int(match.group(2)))


def sram_for(part: Part) -> str:
    """SRAM size per the Function Comparison table: dual core costs 128 KB."""
    return "1664 KB" if part.cores == "dual" else "1792 KB"
