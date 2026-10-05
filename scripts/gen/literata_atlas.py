#!/usr/bin/env python3
"""Generate the checked-in Literata Latin-1 glyph atlas used by ra8_gfx."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[2]
# R8LA v2 header uses version 2 and flag bit 0 for RLE2 coverage.
R8LA_VERSION = 2
R8LA_FLAG_RLE2 = 1
FONT = ROOT / "libs/ra8_fonts/Literata-Regular.ttf"
OUTPUT = ROOT / "libs/ra8_gfx/src/internal/literata_atlas.bin"
PIXEL_SIZE = 20
EXTRA_CODEPOINTS = (0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x20AC)


def codepoints() -> list[int]:
    return list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + list(EXTRA_CODEPOINTS)


def packed_coverage(values: bytes) -> bytes:
    levels = [min(3, (coverage * 3 + 127) // 255) for coverage in values]
    packed = bytearray()
    index = 0
    while index < len(levels):
        level = levels[index]
        run = 1
        while index + run < len(levels) and levels[index + run] == level and run < 64:
            run += 1
        packed.append((level << 6) | (run - 1))
        index += run
    return bytes(packed)


def generate() -> bytes:
    font = ImageFont.truetype(str(FONT), PIXEL_SIZE)
    ascent, descent = font.getmetrics()
    glyphs: list[tuple[int, int, int, int, int, int, int]] = []
    pixels = bytearray()
    for cp in codepoints():
        pixel_offset = len(pixels)
        character = chr(cp)
        left, top, right, bottom = font.getbbox(character)
        mask = font.getmask(character)
        if mask.size != (right - left, bottom - top):
            raise ValueError(f"unexpected mask bounds for U+{cp:04X}")
        packed = packed_coverage(bytes(mask))
        advance = round(font.getlength(character))
        glyphs.append((cp, left, top, advance, mask.size[0], mask.size[1], pixel_offset))
        pixels.extend(packed)

    records = bytearray()
    for cp, left, top, advance, width, height, offset in glyphs:
        records.extend(struct.pack("<IhhhBBI", cp, left, top, advance, width, height, offset))
    header = b"R8LA" + struct.pack("<BBHBB", R8LA_VERSION, R8LA_FLAG_RLE2, len(glyphs), ascent, descent)
    return header + records + bytes(pixels)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if the checked-in atlas is stale")
    args = parser.parse_args()
    generated = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_bytes() != generated:
            print(f"{OUTPUT.relative_to(ROOT)} is stale; run {Path(__file__).relative_to(ROOT)}")
            return 1
        print(f"{OUTPUT.relative_to(ROOT)} is current")
        return 0
    OUTPUT.write_bytes(generated)
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
