#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# scripts/gen/gen_jpeg_fixture.py -- emit a minimal baseline JPEG
# (SOI / APP0 / DQT / SOF0 / DHT / SOS / RST-free entropy / EOI) as
# raw bytes on stdout or to a file. Used to seed the libFuzzer corpus
# for tests/fuzz/fuzz_ra8_jpeg_sw.
#
# This generator only needs to satisfy the parser in libs/ra8_hal/src/
# ra8_jpeg_sw.c well enough for the fuzzer to start from real coverage:
# valid SOI, valid SOF0 with parseable WxH, and either valid entropy
# bytes or an EOI marker right after SOS. The generator does NOT have
# to produce a visually decodable image.
#
# Usage:
#     python3 scripts/gen/gen_jpeg_fixture.py --width 8 --height 8 -o seed.jpg

import argparse
import struct
import sys
from pathlib import Path

# JPEG SOF0 dimension field is a 16-bit unsigned integer (0x0001..0xFFFF).
JPEG_DIM_MAX = 0xFFFF
JPEG_DIM_MIN = 1


def build_minimal_jpeg(width: int, height: int) -> bytes:
    """Build a minimal baseline JPEG with the requested SOF0 dimensions.

    The DCT coefficients in the entropy segment are not meaningful. This
    is a parser-coverage seed only.
    """
    if not (JPEG_DIM_MIN <= width <= JPEG_DIM_MAX) or not (JPEG_DIM_MIN <= height <= JPEG_DIM_MAX):
        msg = "width/height must be in 1..65535"
        raise ValueError(msg)

    out = bytearray()

    # SOI
    out += b"\xff\xd8"

    # APP0 / JFIF 1.01
    app0 = b"JFIF\x00" + b"\x01\x01" + b"\x00" + b"\x00\x01\x00\x01" + b"\x00\x00"
    out += b"\xff\xe0" + struct.pack(">H", 2 + len(app0)) + app0

    # DQT (one 8-bit luma table, all 1s)
    dqt = b"\x00" + (b"\x01" * 64)
    out += b"\xff\xdb" + struct.pack(">H", 2 + len(dqt)) + dqt

    # SOF0 (baseline, 8-bit, single Y component)
    sof0 = (
        b"\x08" + struct.pack(">H", height) + struct.pack(">H", width) + b"\x01" + b"\x01\x11\x00"
    )
    out += b"\xff\xc0" + struct.pack(">H", 2 + len(sof0)) + sof0

    # DHT (DC luma table, minimal valid)
    dht_dc = b"\x00" + bytes([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]) + bytes(range(12))
    out += b"\xff\xc4" + struct.pack(">H", 2 + len(dht_dc)) + dht_dc

    # DHT (AC luma table, minimal valid)
    dht_ac = (
        b"\x10" + bytes([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D]) + bytes(range(0xA2))
    )
    out += b"\xff\xc4" + struct.pack(">H", 2 + len(dht_ac)) + dht_ac

    # SOS (single component)
    sos = b"\x01" + b"\x01\x00" + b"\x00\x3f\x00"
    out += b"\xff\xda" + struct.pack(">H", 2 + len(sos)) + sos

    # Entropy segment -- a few bytes of plausible-looking coefficients.
    # 0xFF in entropy must be followed by 0x00 (byte-stuffing). We avoid
    # 0xFF entirely to keep this trivially valid.
    out += bytes([0x00] * 16)

    # EOI
    out += b"\xff\xd9"

    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Emit a minimal baseline JPEG seed for the libFuzzer corpus."
    )
    parser.add_argument("--width", type=int, default=8)
    parser.add_argument("--height", type=int, default=8)
    parser.add_argument("-o", "--output", default="-", help="Output file (default: stdout).")
    args = parser.parse_args()

    blob = build_minimal_jpeg(args.width, args.height)
    if args.output == "-":
        sys.stdout.buffer.write(blob)
    else:
        with Path(args.output).open("wb") as fh:
            fh.write(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
