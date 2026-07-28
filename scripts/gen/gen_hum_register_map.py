#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate docs/reference/HUM_REGISTERS.csv from the committed manual PDF.

The Hardware User's Manual publishes, per chapter, a description subsection
for every register it defines -- symbol, full name, offset and the page the
description starts on. This turns that into a committed, reviewable CSV so a
human can read the manual's symbol table without opening a 4291-page PDF, and
so a diff shows exactly what a manual revision changed.

The CSV is a CONVENIENCE, never an authority. ``check_hum_register_map.py``
re-derives the table from the PDF on every run and byte-compares the committed
copy against it, so a CSV edited to make a violation disappear fails the gate
rather than hiding the defect -- the "no check may compare a constant to
itself" property from the #190 audit.

Usage::

    scripts/gen/gen_hum_register_map.py            # rewrite the committed CSV
    scripts/gen/gen_hum_register_map.py --stdout   # print it instead
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "checks"))

from hum_regmap import HUM_CSV, HUM_PDF, HumExtractionError, extract_from_pdf, to_csv


def main(argv: list[str] | None = None) -> int:
    """Write (or print) the manual's register symbol table."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stdout", action="store_true", help="print the CSV instead of writing it")
    parser.add_argument("--pdf", type=pathlib.Path, default=HUM_PDF, help="manual PDF to parse")
    parser.add_argument("--out", type=pathlib.Path, default=HUM_CSV, help="CSV path to write")
    args = parser.parse_args(argv)

    try:
        rows = extract_from_pdf(args.pdf)
    except HumExtractionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    text = to_csv(rows)
    if args.stdout:
        sys.stdout.write(text)
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    chapters = len({row.chapter for row in rows})
    print(f"wrote {args.out} -- {len(rows)} registers across {chapters} chapters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
