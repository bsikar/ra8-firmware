#!/usr/bin/env python3
# gen_manga_page_fixture.py -- bake the ereader_manga demo page fixture.
#
# Emits examples/ek_ra8d2/hw_pending/ereader_manga/mg_page_fixture.h: one large
# 8-bit grayscale PNG (bigger than the 1024x600 panel) whose content is a
# deterministic tile grid -- each 256x256 tile is a distinct solid gray with a
# black inner frame and a big blocky "CcRr" label -- so that panning the
# viewport across the page visibly changes which labels are on screen. The page
# is the source ra8_tileatlas_produce() transcodes into a JOF atlas at boot;
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
from pathlib import Path

PAGE_W = 1536
PAGE_H = 2048
TILE = 256
COLS = PAGE_W // TILE
ROWS = PAGE_H // TILE

# Tile geometry + emit formatting (named so the arithmetic reads clearly).
FRAME_PX = 4
LABEL_SCALE = 8
BYTES_PER_ROW = 16

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


class Page:
    """A flat 8-bit grayscale page the drawing helpers paint into."""

    def __init__(self):
        self.px = bytearray(PAGE_W * PAGE_H)

    def draw_glyph(self, x0, y0, ch, scale, val):
        """Blit one scaled blocky glyph at (x0, y0) into the page."""
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
                            self.px[y * PAGE_W + x] = val

    def draw_label(self, tx, ty, col, row):
        """Draw 'C<col>R<row>' centred in tile (tx,ty), black on the tile fill."""
        label = f"C{col}R{row}"
        text_w = len(label) * (GLYPH_W + 1) * LABEL_SCALE
        x0 = tx + (TILE - text_w) // 2
        y0 = ty + (TILE - GLYPH_H * LABEL_SCALE) // 2
        for i, ch in enumerate(label):
            gx0 = x0 + i * (GLYPH_W + 1) * LABEL_SCALE
            self.draw_glyph(gx0, y0, ch, LABEL_SCALE, 0)

    def draw_tile(self, col, row):
        """Fill one tile with its solid gray, a black frame, and its label."""
        tx = col * TILE
        ty = row * TILE
        g = tile_gray(col, row)
        for y in range(ty, ty + TILE):
            base = y * PAGE_W + tx
            for x in range(TILE):
                self.px[base + x] = g
        for t in range(FRAME_PX):
            for x in range(tx, tx + TILE):
                self.px[(ty + t) * PAGE_W + x] = 0
                self.px[(ty + TILE - 1 - t) * PAGE_W + x] = 0
            for y in range(ty, ty + TILE):
                self.px[y * PAGE_W + tx + t] = 0
                self.px[y * PAGE_W + tx + TILE - 1 - t] = 0
        self.draw_label(tx, ty, col, row)

    def build(self):
        """Paint every tile and return the raw grayscale pixel buffer."""
        for row in range(ROWS):
            for col in range(COLS):
                self.draw_tile(col, row)
        return self.px


def png_chunk(tag, data):
    """Wrap one PNG chunk (length + tag + data + CRC-32)."""
    out = struct.pack(">I", len(data)) + tag + data
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return out + struct.pack(">I", crc)


def encode_png(px):
    """8-bit grayscale PNG (color type 0), filter type 0 (None) on every row."""
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
    """Write the generated pure-ASCII C header holding the baked PNG bytes."""
    root = Path(__file__).resolve().parents[2]
    dst = root / "examples" / "ek_ra8d2" / "hw_pending" / "ereader_manga" / "mg_page_fixture.h"
    lines = [
        "/**",
        " * @file mg_page_fixture.h",
        " * @brief Baked demo manga page for the ereader_manga viewer (@generated).",
        " *",
        " * @details",
        f" * One {PAGE_W}x{PAGE_H} 8-bit grayscale PNG, larger than the 1024x600 panel, laid out",
        f" * as a {COLS}x{ROWS} grid of {TILE}px tiles. Each tile is a distinct solid gray with a",
        ' * black inner frame and a big blocky "C<col>R<row>" label, so panning the',
        " * viewport across the page visibly changes which labels are on screen. The",
        " * page is the source ra8_tileatlas_produce() transcodes into a JOF atlas",
        " * at boot; it decodes to identical pixels on host, board_sim and silicon.",
        " *",
        " * Regenerate: python3 scripts/utils/gen_manga_page_fixture.py",
        " *",
        " * @copyright Copyright (c) 2026 Brighton Sikarskie",
        " * SPDX-License-Identifier: MIT",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"/** @brief Baked {len(png)}-byte {PAGE_W}x{PAGE_H} grayscale PNG page. */",
        "static const uint8_t k_mg_png[] = {",
    ]
    for start in range(0, len(png), BYTES_PER_ROW):
        chunk = png[start : start + BYTES_PER_ROW]
        lines.append("  " + " ".join(f"0x{byte:02X}," for byte in chunk))
    lines += [
        "};",
        "",
        "/** @brief Length of ::k_mg_png in bytes. */",
        f"static const uint32_t k_mg_png_len = {len(png)}u;",
    ]
    dst.write_text("\n".join(lines) + "\n", encoding="ascii")
    return dst, len(png)


def main():
    px = Page().build()
    png = encode_png(px)
    dst, n = emit_header(png)
    print(f"wrote {dst} ({n} PNG bytes)")


if __name__ == "__main__":
    main()
