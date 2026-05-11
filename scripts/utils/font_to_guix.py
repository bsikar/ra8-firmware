#!/usr/bin/env python3
"""
font_to_guix.py -- convert a TTF / OTF font into a GUIX C source.

Renders glyphs at a chosen point size using Pillow / FreeType, packs
each glyph at the requested bit-depth (1, 4, or 8 bpp), and emits a
C file containing a ``GX_CONST GX_FONT <symbol>`` table compatible
with GUIX's ``gx_display_driver_*_glyph_*_draw`` paths.

The output file links directly into the firmware and the demo
references the symbol via ``extern GX_CONST GX_FONT <symbol>;``.

Examples
--------
::

    # 4 bpp anti-aliased ArnoPro at 18 pt, default ASCII range
    python3 scripts/utils/font_to_guix.py \\
        libs/fonts/ArnoPro-Regular.otf \\
        libs/fonts/arnopro_18_4bpp.c \\
        --size 18 --bpp 4 --symbol g_font_arnopro_18

    # 1 bpp mono at 16 pt, only digits + uppercase letters
    python3 scripts/utils/font_to_guix.py \\
        libs/fonts/SomeMonoFont.ttf \\
        libs/fonts/somemono_16_1bpp.c \\
        --size 16 --bpp 1 --symbol g_font_somemono_16 \\
        --first 0x30 --last 0x5A

Glyph format
------------
The output struct layout matches GUIX's bundled fonts in
``libs/third_party/guix/common/src/gx_system_font_*.c``:

* ``GX_FONT.gx_font_format`` = ``GX_FONT_FORMAT_{1,4,8}BPP``.
* ``GX_FONT.gx_font_line_height`` = ascent + descent at the requested pt.
* ``GX_FONT.gx_font_baseline``    = ascent in pixels.
* Glyph array indexed 0..N-1 where N = last - first + 1.
* Each ``GX_GLYPH`` carries
  ``(map, ascent, descent, advance, leading, width, height)``.

Bitmap packing per bpp:

* 1 bpp -- 8 pixels per byte, MSB = leftmost pixel.  Rows padded
  to the next whole byte.
* 4 bpp -- 2 pixels per byte, high nibble = leftmost pixel.  Rows
  padded to a whole byte.
* 8 bpp -- 1 pixel per byte, no padding.
"""

import argparse
import pathlib
import sys
from typing import List, Tuple


# --------------------------------------------------------------------------
# Pixel packers
# --------------------------------------------------------------------------

def _pack_1bpp_row(row: List[int]) -> List[int]:
    out: List[int] = []
    byte = 0
    bit = 7
    for v in row:
        if v >= 128:
            byte |= (1 << bit)
        bit -= 1
        if bit < 0:
            out.append(byte)
            byte = 0
            bit = 7
    if bit != 7:
        out.append(byte)
    return out


def _pack_4bpp_row(row: List[int]) -> List[int]:
    out: List[int] = []
    for i in range(0, len(row), 2):
        hi = row[i] >> 4
        lo = (row[i + 1] >> 4) if i + 1 < len(row) else 0
        out.append((hi << 4) | lo)
    return out


def _pack_8bpp_row(row: List[int]) -> List[int]:
    return list(row)


PACKERS = {1: _pack_1bpp_row, 4: _pack_4bpp_row, 8: _pack_8bpp_row}


# --------------------------------------------------------------------------
# Glyph rasterisation
# --------------------------------------------------------------------------

def _rasterize(font, char: str, bpp: int) -> Tuple[int, int, List[int], int, int, int]:
    """Return (width, height, packed_bytes, x_offset, y_offset, advance)."""
    from PIL import Image, ImageDraw

    advance = int(round(font.getlength(char)))
    bbox = font.getbbox(char)
    if bbox is None:
        return (0, 0, [], 0, 0, advance)

    x0, y0, x1, y1 = bbox
    width = x1 - x0
    height = y1 - y0
    if width == 0 or height == 0:
        return (0, 0, [], 0, 0, advance)

    img = Image.new("L", (width, height), 0)
    ImageDraw.Draw(img).text((-x0, -y0), char, fill=255, font=font)

    packer = PACKERS[bpp]
    packed: List[int] = []
    for y in range(height):
        row = [img.getpixel((x, y)) for x in range(width)]
        packed.extend(packer(row))
    return (width, height, packed, x0, y0, advance)


# --------------------------------------------------------------------------
# C emitter
# --------------------------------------------------------------------------

def _format_byte_array(name: str, data: List[int], wrap: int = 12) -> str:
    lines = [f"static GX_CONST GX_UBYTE {name}[{len(data)}] = {{"]
    for i in range(0, len(data), wrap):
        chunk = data[i:i + wrap]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    if lines[-1].endswith(","):
        lines[-1] = lines[-1].rstrip(",")
    lines.append("};")
    return "\n".join(lines)


def _emit(out_path: pathlib.Path,
          symbol: str,
          src_font: pathlib.Path,
          point_size: int,
          bpp: int,
          first_code: int,
          last_code: int) -> None:
    from PIL import ImageFont

    if bpp not in PACKERS:
        sys.exit(f"unsupported --bpp value {bpp}; choose 1, 4, or 8")
    if not (0 < first_code <= last_code <= 0xFFFF):
        sys.exit("character range must satisfy 0 < --first <= --last <= 0xFFFF")

    font = ImageFont.truetype(str(src_font), point_size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    count = last_code - first_code + 1

    glyph_records = []
    for code in range(first_code, last_code + 1):
        ch = chr(code)
        width, height, packed, x_off, y_off, advance = _rasterize(font, ch, bpp)
        glyph_records.append({
            "code": code,
            "name": f"{symbol}_char_{code:02x}",
            "width": width,
            "height": height,
            "packed": packed,
            "ascent": ascent - y_off,
            "descent": (y_off + height) - ascent,
            "advance": min(advance, 255),
            "leading": max(-128, min(127, x_off)),
        })

    fmt_macro = {1: "GX_FONT_FORMAT_1BPP",
                 4: "GX_FONT_FORMAT_4BPP",
                 8: "GX_FONT_FORMAT_8BPP"}[bpp]
    array_name = f"{symbol.upper()}_GLYPHS"

    out_lines: List[str] = [
        "/**",
        f" * @file libs/fonts/{out_path.name}",
        f" * @brief GUIX bitmap font generated from {src_font.name}",
        " *",
        f" * @details Auto-generated by scripts/utils/font_to_guix.py at",
        f" * {point_size} pt, {bpp} bpp.  Do not edit by hand --",
        f" * regenerate via the converter.",
        " *",
        " * @copyright Copyright (c) 2026 Brighton Sikarskie",
        " * SPDX-License-Identifier: MIT",
        " */",
        "",
        '#include "gx_api.h"',
        "",
    ]
    for g in glyph_records:
        if g["packed"]:
            out_lines.append(_format_byte_array(g["name"], g["packed"]))
            out_lines.append("")

    out_lines.append(f"static GX_CONST GX_GLYPH {array_name}[{count}] = {{")
    for g in glyph_records:
        ptr = g["name"] if g["packed"] else "GX_NULL"
        out_lines.append(
            f"    {{{ptr}, {g['ascent']}, {g['descent']}, "
            f"{g['advance']}, {g['leading']}, {g['width']}, {g['height']}}},"
        )
    out_lines.append("};")
    out_lines.append("")
    out_lines.append(f"GX_CONST GX_FONT {symbol} =")
    out_lines.append("{")
    out_lines.append(f"    {fmt_macro},")
    out_lines.append("    0,                                  /* line pre-space  */")
    out_lines.append("    0,                                  /* line post-space */")
    out_lines.append(f"    {line_height},                                 /* line height     */")
    out_lines.append(f"    {ascent},                                 /* baseline offset */")
    out_lines.append(f"    0x{first_code:04x},                             /* first glyph     */")
    out_lines.append(f"    0x{last_code:04x},                             /* last glyph      */")
    out_lines.append(f"    {{{array_name}}},")
    out_lines.append("    GX_NULL")
    out_lines.append("};")
    out_lines.append("")

    out_path.write_text("\n".join(out_lines))
    total_bytes = sum(len(g["packed"]) for g in glyph_records)
    print(f"Wrote {out_path}")
    print(f"  glyphs   : {count} ({hex(first_code)}..{hex(last_code)})")
    print(f"  size     : {point_size} pt at {bpp} bpp")
    print(f"  metrics  : line_height={line_height}px, ascent={ascent}px")
    print(f"  bitmap   : {total_bytes} bytes of glyph data")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__[__doc__.index("Examples"):],
    )
    parser.add_argument("input", type=pathlib.Path, help="Source TTF/OTF font")
    parser.add_argument("output", type=pathlib.Path, help="Destination .c file")
    parser.add_argument("--size", type=int, required=True, help="Point size")
    parser.add_argument(
        "--bpp", type=int, default=4, choices=(1, 4, 8),
        help="Bits per pixel: 1 (mono), 4 (default, AA), 8 (best AA)",
    )
    parser.add_argument(
        "--symbol", required=True,
        help="Exported GX_FONT C identifier (also used as glyph-data prefix)",
    )
    parser.add_argument(
        "--first", type=lambda s: int(s, 0), default=0x20,
        help="First glyph code-point (default 0x20 = space)",
    )
    parser.add_argument(
        "--last", type=lambda s: int(s, 0), default=0x7E,
        help="Last glyph code-point (default 0x7E = ~)",
    )
    args = parser.parse_args()
    if not args.input.is_file():
        sys.exit(f"input font not found: {args.input}")
    try:
        from PIL import ImageFont  # noqa: F401
    except ImportError:
        sys.exit("Pillow is required: pip install pillow")
    _emit(args.output, args.symbol, args.input, args.size,
          args.bpp, args.first, args.last)


if __name__ == "__main__":
    main()
