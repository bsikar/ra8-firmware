#!/usr/bin/env python3
"""Verify v2 RLE decodes to the v1 quantized Pillow masks for every glyph."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from PIL import ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import display_atlases
import literata_atlas

HEADER_BYTES = 10
RECORD_BYTES = 16


def quantize(mask: bytes) -> list[int]:
    return [min(3, (coverage * 3 + 127) // 255) for coverage in mask]


def decode_v2(data: bytes) -> tuple[int, int, int, list[tuple[int, int, int, int, int, int, list[int]]]]:
    if data[:4] != b"R8LA" or data[4:6] != bytes((2, 1)):
        raise AssertionError("wrong R8LA v2 header")
    count, ascent, descent = struct.unpack_from("<HBB", data, 6)
    coverage_start = HEADER_BYTES + count * RECORD_BYTES
    glyphs = []
    for index in range(count):
        cp, left, top, advance, width, height, offset = struct.unpack_from(
            "<IhhhBBI", data, HEADER_BYTES + index * RECORD_BYTES
        )
        expected_pixels = width * height
        decoded: list[int] = []
        cursor = coverage_start + offset
        while len(decoded) < expected_pixels:
            run = data[cursor]
            cursor += 1
            decoded.extend([run >> 6] * ((run & 0x3F) + 1))
        if len(decoded) != expected_pixels:
            raise AssertionError(f"run crosses glyph bounds for U+{cp:04X}")
        glyphs.append((cp, left, top, advance, width, height, decoded))
    return count, ascent, descent, glyphs


def expected_display(path: Path, size: int, cps: list[int], stroke: int):
    font = ImageFont.truetype(str(path), size)
    ascent, descent = font.getmetrics()
    ascent += stroke
    descent += stroke
    rows = []
    for cp in cps:
        char = chr(cp)
        left, top, right, bottom = font.getbbox(char, stroke_width=stroke)
        width, height = right - left, bottom - top
        mask = bytearray(font.getmask(char, stroke_width=stroke))
        if stroke:
            fill_left, fill_top, fill_right, fill_bottom = font.getbbox(char)
            fill_width, fill_height = fill_right - fill_left, fill_bottom - fill_top
            fill = font.getmask(char)
            dx, dy = fill_left - left, fill_top - top
            for y in range(fill_height):
                for x in range(fill_width):
                    target = (dy + y) * width + dx + x
                    mask[target] = max(mask[target], fill[y * fill_width + x])
        advance = round(font.getlength(char)) + 2 * stroke
        rows.append((cp, left, top, advance, width, height, quantize(bytes(mask))))
    return ascent, descent, rows


def expected_literata():
    font = ImageFont.truetype(str(literata_atlas.FONT), literata_atlas.PIXEL_SIZE)
    ascent, descent = font.getmetrics()
    rows = []
    for cp in literata_atlas.codepoints():
        char = chr(cp)
        left, top, right, bottom = font.getbbox(char)
        mask = font.getmask(char)
        rows.append((cp, left, top, round(font.getlength(char)), right - left, bottom - top, quantize(bytes(mask))))
    return ascent, descent, rows


def compare(label: str, data: bytes, expected_ascent: int, expected_descent: int, expected_rows) -> int:
    count, ascent, descent, actual_rows = decode_v2(data)
    if (count, ascent, descent) != (len(expected_rows), expected_ascent, expected_descent):
        raise AssertionError(f"{label}: header differs from v1 metrics")
    for actual, expected in zip(actual_rows, expected_rows, strict=True):
        if actual != expected:
            raise AssertionError(f"{label}: v1/v2 coverage differs for U+{actual[0]:04X}")
    print(f"{label}: {count} glyphs decode byte-for-byte to v1 coverage")
    return len(data)


def main() -> int:
    total_v1 = 0
    total_v2 = 0
    data = literata_atlas.generate()
    expected_ascent, expected_descent, rows = expected_literata()
    total_v2 += compare("literata_atlas.bin", data, expected_ascent, expected_descent, rows)
    total_v1 += 8 + len(rows) * RECORD_BYTES + sum((len(row[6]) + 3) // 4 for row in rows)

    for size_name, (pixel_size, glyph_kind) in display_atlases.SIZES.items():
        cps = display_atlases.codepoints(glyph_kind)
        fonts = display_atlases.FONTS.items()
        if glyph_kind == "reader":
            fonts = ((key, value) for key, value in fonts if key[1] == "regular")
        for (face, weight), (font_path, stroke) in fonts:
            name = f"{face}_{weight}_{size_name}.bin"
            v2 = display_atlases.make_atlas(font_path, pixel_size, cps, stroke)
            ascent, descent, expected_rows = expected_display(font_path, pixel_size, cps, stroke)
            total_v2 += compare(name, v2, ascent, descent, expected_rows)
            total_v1 += 8 + len(cps) * RECORD_BYTES + sum((len(row[6]) + 3) // 4 for row in expected_rows)
    print(f"legacy v1 total {total_v1} bytes; v2 total {total_v2} bytes; saved {100 * (total_v1 - total_v2) / total_v1:.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
