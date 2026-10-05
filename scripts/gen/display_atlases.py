#!/usr/bin/env python3
"""Build native-resolution ra8_gfx atlases for the UI display text sizes."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path
from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[2]
# R8LA v2 header uses version 2 and flag bit 0 for RLE2 coverage.
R8LA_VERSION = 2
R8LA_FLAG_RLE2 = 1
OUTPUT = ROOT / "libs/ra8_gfx/src/internal/display_atlases"
SERIF = ROOT / "libs/ra8_fonts/Literata-Regular.ttf"
SANS_REGULAR = ROOT / "libs/ra8_fonts/RA8UISans/RA8UISans-Regular.ttf"
SANS_BOLD = ROOT / "libs/ra8_fonts/RA8UISans/RA8UISans-Bold.ttf"
EXTRA_CODEPOINTS = (0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x20AC)
SIZES = {"body": (38, "body"), "title": (68, "title"), "clock": (120, "clock")}
FONTS = {
    ("serif", "regular"): (SERIF, 0),
    ("serif", "bold"): (SERIF, 1),
    ("sans", "regular"): (SANS_REGULAR, 0),
    ("sans", "bold"): (SANS_BOLD, 0),
}


def codepoints(kind: str) -> list[int]:
    if kind == "clock":
        return sorted(ord(c) for c in " 0123456789:.?")
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


def make_atlas(font_path: Path, pixel_size: int, codepoints_: list[int], stroke: int) -> bytes:
    font = ImageFont.truetype(str(font_path), pixel_size)
    ascent, descent = font.getmetrics()
    ascent += stroke
    descent += stroke
    if ascent + descent > 255:
        raise ValueError(f"font height {ascent + descent} exceeds atlas format")
    records = bytearray()
    coverage = bytearray()
    for cp in codepoints_:
        pixel_offset = len(coverage)
        character = chr(cp)
        left, top, right, bottom = font.getbbox(character, stroke_width=stroke)
        stroked_mask = font.getmask(character, stroke_width=stroke)
        mask = bytearray(stroked_mask)
        width, height = right - left, bottom - top
        if stroked_mask.size != (width, height) or width > 255 or height > 255:
            raise ValueError(f"invalid glyph bounds for U+{cp:04X}: {stroked_mask.size} vs {(width, height)}")
        if stroke > 0:
            fill_left, fill_top, fill_right, fill_bottom = font.getbbox(character)
            fill_mask = font.getmask(character)
            fill_width, fill_height = fill_right - fill_left, fill_bottom - fill_top
            if fill_mask.size != (fill_width, fill_height):
                raise ValueError(f"invalid fill bounds for U+{cp:04X}: {fill_mask.size} vs {(fill_width, fill_height)}")
            offset_x, offset_y = fill_left - left, fill_top - top
            for fill_y in range(fill_height):
                for fill_x in range(fill_width):
                    target_x, target_y = offset_x + fill_x, offset_y + fill_y
                    if 0 <= target_x < width and 0 <= target_y < height:
                        target = target_y * width + target_x
                        source = fill_y * fill_width + fill_x
                        mask[target] = max(mask[target], fill_mask[source])
        if len(mask) != width * height:
            raise ValueError(f"invalid glyph mask for U+{cp:04X}: {len(mask)} pixels vs {(width, height)}")
        if left < -32768 or top < -32768 or left > 32767 or top > 32767:
            raise ValueError(f"glyph offset exceeds atlas format for U+{cp:04X}")
        advance = round(font.getlength(character)) + 2 * stroke
        if advance > 32767:
            raise ValueError(f"glyph advance exceeds atlas format for U+{cp:04X}")
        records.extend(struct.pack("<IhhhBBI", cp, left, top, advance, width, height, pixel_offset))
        coverage.extend(packed_coverage(mask))
    return b"R8LA" + struct.pack("<BBHBB", R8LA_VERSION, R8LA_FLAG_RLE2, len(codepoints_), ascent, descent) + records + coverage


def outputs() -> dict[Path, bytes]:
    result: dict[Path, bytes] = {}
    for size_name, (pixel_size, glyph_kind) in SIZES.items():
        cps = codepoints(glyph_kind)
        for (face, weight), (font_path, stroke) in FONTS.items():
            filename = f"{face}_{weight}_{size_name}.bin"
            result[OUTPUT / filename] = make_atlas(font_path, pixel_size, cps, stroke)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="fail if a checked-in atlas is stale")
    args = parser.parse_args()
    generated = outputs()
    if args.check:
        stale = [path for path, data in generated.items() if not path.exists() or path.read_bytes() != data]
        if stale:
            print("stale display atlases: " + ", ".join(str(path.relative_to(ROOT)) for path in stale))
            return 1
        total = sum(len(data) for data in generated.values())
        print(f"{len(generated)} display atlases are current ({total} bytes)")
        return 0
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for path, data in generated.items():
        path.write_bytes(data)
        print(f"wrote {path.relative_to(ROOT)} ({len(data)} bytes)")
    print(f"total display atlas bytes: {sum(len(data) for data in generated.values())}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
