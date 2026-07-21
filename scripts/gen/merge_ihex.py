#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Merge two Intel HEX files into one.

Used by the tz_nsc_cgc_usb two-project TrustZone build (#96) to combine the
Secure image (at 0x0200_0000+) and the Non-Secure image (LMA 0x0208_0000) into
a single flashable hex. The two address ranges do not overlap.

Concatenates every record from both inputs EXCEPT the End-Of-File (type 01)
records, then appends a single EOF. Extended-Linear-Address (type 04) records
are preserved, so each input keeps positioning its own data.

Usage:
    merge_ihex.py <in_a.hex> <in_b.hex> <out.hex>

out.hex may be the same path as in_a.hex (read fully first, then written).
"""

import sys
from pathlib import Path

# Expected argument count: script + in_a + in_b + out
_EXPECTED_ARGC = 4


def data_records(path: str) -> list[str]:
    """Return all non-EOF records (stripped) from an Intel HEX file."""
    out: list[str] = []
    with Path(path).open(encoding="ascii") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or not line.startswith(":"):
                continue
            # Record type is bytes [7:9] of the line (after ':LLAAAA').
            if line[7:9].upper() == "01":  # EOF record -- drop.
                continue
            out.append(line)
    return out


def main() -> int:
    """Concatenate the data records of two Intel HEX files into one image.

    Merges RECORDS rather than text: each input's EOF record is dropped and a
    single one is emitted at the end, so the result is a valid image instead
    of two files with a terminator in the middle that most loaders stop at.

    Returns 0 on success, 2 on a usage error, 1 on an unreadable input.
    """
    if len(sys.argv) != _EXPECTED_ARGC:
        print("usage: merge_ihex.py <in_a.hex> <in_b.hex> <out.hex>", file=sys.stderr)
        return 2
    in_a, in_b, out = sys.argv[1], sys.argv[2], sys.argv[3]
    try:
        records = data_records(in_a) + data_records(in_b)
    except OSError as exc:
        print(f"merge_ihex: {exc}", file=sys.stderr)
        return 1
    with Path(out).open("w", encoding="ascii") as fh:
        fh.writelines(rec + "\n" for rec in records)
        fh.write(":00000001FF\n")  # canonical EOF record.
    print(f"merge_ihex: wrote {len(records)} records -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
