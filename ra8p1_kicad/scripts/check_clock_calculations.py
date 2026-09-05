#!/usr/bin/env python3
"""Verify CLK-001/002 arithmetic and exported BOM inputs without editing files.

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
        "Y2": ("32.768kHz", "ABS07-LR-32.768KHZ-6-1-T"),
        "C37": ("4p", "C1005NP01H040C050BA"),
        "C38": ("4p", "C1005NP01H040C050BA"),
    }
    for reference, (value, mpn) in expected_parts.items():
        matches = [row for row in rows if reference in row["Reference"].split(",")]
        require_equal(len(matches), 1, f"{reference} unique BOM occurrence")
        row = matches[0]
        require_equal(row["Value"], value, f"{reference} value")
        require_equal(row["Manufacturer_Part_Number"], mpn, f"{reference} MPN")
        require_equal(row["DNP"], "", f"{reference} populated")
        require_equal(row["Exclude from BOM"], "", f"{reference} BOM inclusion")


def check_rtc() -> None:
    """Verify CLK-002 pF, reference ESR ratios and initial ppm conversion."""
    target, stray = Fraction(6), Fraction(4)
    capacitor = 2 * (target - stray)
    require_equal(capacitor, 4, "RTC capacitor selection, pF")
    require_equal(load_pf(capacitor, capacitor, stray), target, "RTC nominal CL")
    low, high = capacitor - Fraction(1, 4), capacitor + Fraction(1, 4)
    corners = [load_pf(a, b, stray) for a in (low, high) for b in (low, high)]
    require_equal(min(corners), Fraction(47, 8), "RTC minimum CL")
    require_equal(max(corners), Fraction(49, 8), "RTC maximum CL")
    require_equal(corners[1], Fraction(767, 128), "RTC opposite corner CL")
    require_equal(corners[1], corners[2], "RTC corner symmetry")
    require_equal((max(corners) - target) / target * 100, Fraction(25, 12),
                  "RTC relative load error, percent")
    for assumed, needed, actual in ((2, 8, 4), (3, 6, 5), (4, 4, 6), (5, 2, 7)):
        require_equal(2 * (target - assumed), needed, "RTC sensitivity C")
        require_equal(load_pf(capacitor, capacitor, Fraction(assumed)), actual,
                      "RTC sensitivity CL")
    require_equal(Fraction(600, 5), 120, "RTC reference ESR ceiling, kohm")
    require_equal(Fraction(600, 50), 12, "RTC reference ESR ratio")
    require_equal(Fraction(32768 * 10, 1000000), Fraction(1024, 3125),
                  "RTC initial frequency tolerance, Hz")
    require_equal(Fraction(86400 * 10, 1000000), Fraction(108, 125),
                  "RTC initial time tolerance, seconds/day")


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
    check_rtc()
    print("CLK-001 PASS: arithmetic and BOM inputs agree; hardware matching unverified.")
    print("C35=C36=8 pF; CL=6 pF assuming 2 pF effective stray.")
    print("Fixed-stray tolerance: 5.75..6.25 pF; opposite corner: 5.984375 pF.")
    print("CLK-002 PASS: arithmetic and BOM inputs agree; hardware matching unverified.")
    print("C37=C38=4 pF; CL=6 pF assuming 4 pF effective stray.")
    print("Fixed-stray tolerance: 5.875..6.125 pF; opposite corner: 5.9921875 pF.")


if __name__ == "__main__":
    main()
