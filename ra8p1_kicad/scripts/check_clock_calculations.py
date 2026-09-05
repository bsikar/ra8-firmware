#!/usr/bin/env python3
"""Verify CLK-001 arithmetic and exported BOM inputs without editing design files.

Run from any directory with Python 3. Uses exact rational arithmetic in pF
and ohms. See design/clock_calculations.md for sources and model limitations.
This is not a simulator or a hardware qualification test.
"""

import csv
from fractions import Fraction
from pathlib import Path


def load_pf(first: Fraction, second: Fraction, stray: Fraction) -> Fraction:
    """Return the lumped load in pF; reject nonphysical model inputs."""
    if first <= 0 or second <= 0:
        raise ValueError("External capacitances must be positive")
    if stray < 0:
        raise ValueError("Effective stray capacitance cannot be negative")
    return first * second / (first + second) + stray


def require_equal(actual: object, expected: object, label: str) -> None:
    """Fail explicitly on mismatch, including when Python runs with -O."""
    if actual != expected:
        raise ValueError(f"{label}: got {actual!r}, expected {expected!r}")


def check_bom() -> None:
    """Bind the calculation to exact populated Y1/C35/C36 exported inputs."""
    bom_path = Path(__file__).resolve().parents[1] / "exports/ereader_rev1_bom.csv"
    with bom_path.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    expected_parts = {
        "Y1": ("24MHz", "XRCGB24M000F3M19R0"),
        "C35": ("8p", "C1005NP01H080D050BA"),
        "C36": ("8p", "C1005NP01H080D050BA"),
    }
    for reference, (value, mpn) in expected_parts.items():
        matches = [row for row in rows if reference in row["Reference"].split(",")]
        require_equal(len(matches), 1, f"{reference} unique BOM occurrence")
        row = matches[0]
        require_equal(row["Value"], value, f"{reference} value")
        require_equal(row["Manufacturer_Part_Number"], mpn, f"{reference} MPN")
        require_equal(row["DNP"], "", f"{reference} populated")
        require_equal(row["Exclude from BOM"], "", f"{reference} BOM inclusion")


def main() -> None:
    """Check nominal, four tolerance corners, sensitivity and ESR screening."""
    target = Fraction(6)
    stray = Fraction(2)
    capacitor = 2 * (target - stray)
    require_equal(capacitor, Fraction(8), "equal capacitor selection, pF")
    require_equal(load_pf(capacitor, capacitor, stray), target, "nominal CL, pF")
    low, high = capacitor - Fraction(1, 2), capacitor + Fraction(1, 2)
    corners = [load_pf(a, b, stray) for a in (low, high) for b in (low, high)]
    require_equal(min(corners), Fraction(23, 4), "minimum CL, pF")
    require_equal(max(corners), Fraction(25, 4), "maximum CL, pF")
    require_equal(corners[1], Fraction(383, 64), "opposite corner CL, pF")
    require_equal(corners[1], corners[2], "opposite corner symmetry")
    require_equal((max(corners) - target) / target * 100, Fraction(25, 6),
                  "positive load deviation, percent")
    for assumed, needed, actual in ((1, 10, 5), (Fraction(3, 2), 9, Fraction(11, 2)),
                                     (2, 8, 6), (Fraction(5, 2), 7, Fraction(13, 2)),
                                     (3, 6, 7)):
        require_equal(2 * (target - assumed), needed, "sensitivity capacitor, pF")
        require_equal(load_pf(capacitor, capacitor, Fraction(assumed)), actual,
                      "sensitivity CL, pF")
    require_equal(Fraction(1050, 5), 210, "reference ESR screening, ohm")
    require_equal(Fraction(1050, 100), Fraction(21, 2), "reference ESR ratio")
    check_bom()
    print("CLK-001 PASS: arithmetic and BOM inputs agree; hardware matching unverified.")
    print("C35=C36=8 pF; CL=6 pF assuming 2 pF effective stray.")
    print("Fixed-stray tolerance: 5.75..6.25 pF; opposite corner: 5.984375 pF.")


if __name__ == "__main__":
    main()
