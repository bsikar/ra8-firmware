#!/usr/bin/env python3
# gen_manga_page_fixture.py -- bake the ereader_manga demo page fixture.
#
# Emits examples/ek_ra8d2/hw_pending/ereader_manga/mg_page_fixture.h: one large
# 8-bit grayscale PNG (bigger than the 1024x600 panel) whose content is a
# deterministic tile grid -- each 256x256 tile is a distinct solid gray with a
# black inner frame and a big blocky "CcRr" label -- so that panning the
# viewport across the page visibly changes which labels are on screen. The page
# is the source ra8_tileatlas_produce() transcodes into an RTA1 atlas at boot;
# it decodes to the same pixels on host, board_sim and silicon, so the render
# hash in the app banner is identical everywhere.
#
# Solid tile blocks + sparse labels compress to a few KiB of PNG, which keeps
# the baked .rodata blob small enough to sit in the 1 MB code MRAM alongside the
# firmware.
#
# Regenerate:
#   python3 scripts/utils/gen_manga_page_fixture.py
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

import struct
import zlib
import os

PAGE_W = 1536
PAGE_H = 2048
TILE = 256
COLS = PAGE_W // TILE
ROWS = PAGE_H // TILE

# 5x7 blocky font for the tile labels (only the glyphs the labels use).
FONT = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
}
GLYPH_W = 5
GLYPH_H = 7


def tile_gray(col, row):
    """Distinct, well-separated gray for tile (col, row)."""
    idx = (col * 3 + row * 5) % 12
    return 60 + idx * 16  # 60..236


def draw_glyph(px, x0, y0, ch, scale, val):
    rows = FONT.get(ch)
    if rows is None:
        return
    for gy in range(GLYPH_H):
        for gx in range(GLYPH_W):
            if rows[gy][gx] != "1":
                continue
            for sy in range(scale):
                for sx in range(scale):
                    x = x0 + gx * scale + sx
                    y = y0 + gy * scale + sy
                    if 0 <= x < PAGE_W and 0 <= y < PAGE_H:
                        px[y * PAGE_W + x] = val


def draw_label(px, tx, ty, col, row):
    """Draw 'C<col>R<row>' centred in tile (tx,ty), black on the tile fill."""
    label = "C%dR%d" % (col, row)
    scale = 8
    text_w = len(label) * (GLYPH_W + 1) * scale
    x0 = tx + (TILE - text_w) // 2
    y0 = ty + (TILE - GLYPH_H * scale) // 2
    for i, ch in enumerate(label):
        gx0 = x0 + i * (GLYPH_W + 1) * scale
        draw_glyph(px, gx0, y0, ch, scale, 0)


def build_page():
    px = bytearray(PAGE_W * PAGE_H)
    for row in range(ROWS):
        for col in range(COLS):
            tx = col * TILE
            ty = row * TILE
            g = tile_gray(col, row)
            # Solid tile fill.
            for y in range(ty, ty + TILE):
                base = y * PAGE_W + tx
                for x in range(TILE):
                    px[base + x] = g
            # Black inner frame (4 px) so tile boundaries read clearly.
            for t in range(4):
                for x in range(tx, tx + TILE):
                    px[(ty + t) * PAGE_W + x] = 0
                    px[(ty + TILE - 1 - t) * PAGE_W + x] = 0
                for y in range(ty, ty + TILE):
                    px[y * PAGE_W + tx + t] = 0
                    px[y * PAGE_W + tx + TILE - 1 - t] = 0
            draw_label(px, tx, ty, col, row)
    return px


def png_chunk(tag, data):
    out = struct.pack(">I", len(data)) + tag + data
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return out + struct.pack(">I", crc)


def encode_png(px):
    # 8-bit grayscale (color type 0), filter type 0 (None) on every row.
    raw = bytearray()
    for y in range(PAGE_H):
        raw.append(0)
        raw.extend(px[y * PAGE_W : (y + 1) * PAGE_W])
    ihdr = struct.pack(">IIBBBBB", PAGE_W, PAGE_H, 8, 0, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", idat)
        + png_chunk(b"IEND", b"")
    )


def emit_header(png):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    dst = os.path.join(
        root,
        "examples",
        "ek_ra8d2",
        "hw_pending",
        "ereader_manga",
        "mg_page_fixture.h",
    )
    lines = []
    lines.append("/**")
    lines.append(" * @file mg_page_fixture.h")
    lines.append(
        " * @brief Baked demo manga page for the ereader_manga viewer (@generated)."
    )
    lines.append(" *")
    lines.append(" * @details")
    lines.append(
        " * One %dx%d 8-bit grayscale PNG, larger than the 1024x600 panel, laid out"
        % (PAGE_W, PAGE_H)
    )
    lines.append(
        " * as a %dx%d grid of %dpx tiles. Each tile is a distinct solid gray with a"
        % (COLS, ROWS, TILE)
    )
    lines.append(
        ' * black inner frame and a big blocky "C<col>R<row>" label, so panning the'
    )
    lines.append(
        " * viewport across the page visibly changes which labels are on screen. The"
    )
    lines.append(
        " * page is the source ra8_tileatlas_produce() transcodes into an RTA1 atlas"
    )
    lines.append(
        " * at boot; it decodes to identical pixels on host, board_sim and silicon."
    )
    lines.append(" *")
    lines.append(
        " * Regenerate: python3 scripts/utils/gen_manga_page_fixture.py"
    )
    lines.append(" *")
    lines.append(" * @copyright Copyright (c) 2026 Brighton Sikarskie")
    lines.append(" * SPDX-License-Identifier: MIT")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(
        "/** @brief Baked %d-byte %dx%d grayscale PNG page. */" % (len(png), PAGE_W, PAGE_H)
    )
    lines.append("static const uint8_t k_mg_png[] = {")
    row = "  "
    for i, b in enumerate(png):
        row += "0x%02X," % b
        if (i % 16) == 15:
            lines.append(row)
            row = "  "
        else:
            row += " "
    if row.strip():
        lines.append(row.rstrip())
    lines.append("};")
    lines.append("")
    lines.append("/** @brief Length of ::k_mg_png in bytes. */")
    lines.append("static const uint32_t k_mg_png_len = %du;" % len(png))
    with open(dst, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    return dst, len(png)


def main():
    px = build_page()
    png = encode_png(px)
    dst, n = emit_header(png)
    print("wrote %s (%d PNG bytes)" % (dst, n))


if __name__ == "__main__":
    main()
