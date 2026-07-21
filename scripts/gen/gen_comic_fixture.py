#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Generate the baked multi-page CBZ fixture for the viewable ereader_comic demo.
#
# Emits a pure 7-bit-ASCII C header (comic_pages_fixture.h) holding one CBZ
# (a miniz-decodable ZIP of grayscale PNG pages) as a byte array. The pages are
# near-panel-size (aspect ~ 1024:600) and each is visually distinct -- a unique
# paper tint, a different comic panel layout, and a huge 7-segment page number --
# so a page turn on the panel is unmistakable. Grayscale PNGs of flat-shaded art
# DEFLATE to a few hundred bytes each, so the whole archive stays small enough to
# bake as an ASCII array under the repo's per-file line cap.
#
# The output is deterministic (fixed art, fixed zlib level), so the render hash
# the app prints is identical on host, board_sim, and silicon.
#
# Usage:
#   python3 scripts/gen/gen_comic_fixture.py \
#       examples/ek_ra8d2/hw_pending/ereader_comic/comic_pages_fixture.h

import struct
import sys
import zlib
from pathlib import Path

PAGE_W = 480
PAGE_H = 240
PAGE_COUNT = 5

# Grayscale palette (0 = black ink, 255 = white paper).
INK = 0

# Per-page paper tint + panel-fill grays, chosen far apart so the page turn is
# visually obvious and every page framebuffer hashes differently.
PAGE_PAPER = [244, 232, 250, 224, 238]
PAGE_FILL = [200, 150, 120, 175, 95]

# PNG / ZIP magic constants (named so the comparisons are self-documenting).
PNG_BIT_DEPTH_8 = 8
PNG_COLOR_GRAY = 0
ZIP_LOCAL_MAGIC = 0x04034B50
ZIP_CENTRAL_MAGIC = 0x02014B50
ZIP_EOCD_MAGIC = 0x06054B50
ZIP_VERSION = 20
ZIP_METHOD_DEFLATE = 8
DEFLATE_RAW_WBITS = -15
ZLIB_BEST = 9
BYTES_PER_ROW = 16

# 7-segment layouts: single-vertical / mid-horizontal grids per page index.
LAYOUT_VBAR = 0
LAYOUT_HBAR = 1
LAYOUT_CROSS = 2
LAYOUT_TRIPTYCH = 3

# Active 7-segment segments per decimal digit: (a, b, c, d, e, f, g).
SEG_MAP = {
    0: (1, 1, 1, 1, 1, 1, 0),
    1: (0, 1, 1, 0, 0, 0, 0),
    2: (1, 1, 0, 1, 1, 0, 1),
    3: (1, 1, 1, 1, 0, 0, 1),
    4: (0, 1, 1, 0, 0, 1, 1),
    5: (1, 0, 1, 1, 0, 1, 1),
    6: (1, 0, 1, 1, 1, 1, 1),
    7: (1, 1, 1, 0, 0, 0, 0),
    8: (1, 1, 1, 1, 1, 1, 1),
    9: (1, 1, 1, 1, 0, 1, 1),
}


class Canvas:
    """A flat 8-bit grayscale page the drawing helpers paint into."""

    def __init__(self, fill):
        self.px = bytearray([fill]) * (PAGE_W * PAGE_H)

    def fill_rect(self, rect, v):
        x0, y0, w, h = rect
        for y in range(max(0, y0), min(PAGE_H, y0 + h)):
            row = y * PAGE_W
            for x in range(max(0, x0), min(PAGE_W, x0 + w)):
                self.px[row + x] = v

    def frame_rect(self, rect, t, v):
        x0, y0, w, h = rect
        self.fill_rect((x0, y0, w, t), v)
        self.fill_rect((x0, y0 + h - t, w, t), v)
        self.fill_rect((x0, y0, t, h), v)
        self.fill_rect((x0 + w - t, y0, t, h), v)

    def draw_digit(self, digit, rect, t):
        a, b, c, d, e, f, g = SEG_MAP[digit]
        x, y, w, h = rect
        mid = y + (h - t) // 2
        half = (h // 2) + t
        if a:
            self.fill_rect((x, y, w, t), INK)
        if g:
            self.fill_rect((x, mid, w, t), INK)
        if d:
            self.fill_rect((x, y + h - t, w, t), INK)
        if f:
            self.fill_rect((x, y, t, half), INK)
        if b:
            self.fill_rect((x + w - t, y, t, half), INK)
        if e:
            self.fill_rect((x, mid, t, half), INK)
        if c:
            self.fill_rect((x + w - t, mid, t, half), INK)


def draw_layout(canvas, page_idx, fill):
    """Draw a distinct comic panel arrangement (ink gutters) over a gray fill."""
    gut = 8
    x0, y0 = 24, 24
    w = PAGE_W - 2 * x0
    h = PAGE_H - 2 * y0
    canvas.fill_rect((x0, y0, w, h), fill)
    layout = page_idx % PAGE_COUNT
    if layout in (LAYOUT_VBAR, LAYOUT_CROSS):
        canvas.fill_rect((x0 + w // 2 - gut // 2, y0, gut, h), INK)
    if layout in (LAYOUT_HBAR, LAYOUT_CROSS):
        canvas.fill_rect((x0, y0 + h // 2 - gut // 2, w, gut), INK)
    if layout == LAYOUT_TRIPTYCH:
        canvas.fill_rect((x0 + w // 3 - gut // 2, y0, gut, h), INK)
        canvas.fill_rect((x0 + 2 * w // 3 - gut // 2, y0, gut, h), INK)
    if layout not in (LAYOUT_VBAR, LAYOUT_HBAR, LAYOUT_CROSS, LAYOUT_TRIPTYCH):
        canvas.fill_rect((x0, y0 + h // 3 - gut // 2, w, gut), INK)
        canvas.fill_rect((x0 + w // 2 - gut // 2, y0 + h // 3, gut, 2 * h // 3), INK)


def render_page(page_idx):
    """Build one distinct grayscale page: border, panels, and a huge number."""
    canvas = Canvas(PAGE_PAPER[page_idx])
    canvas.frame_rect((6, 6, PAGE_W - 12, PAGE_H - 12), 6, INK)
    draw_layout(canvas, page_idx, PAGE_FILL[page_idx])
    dw, dh, dt = 90, 150, 20
    canvas.draw_digit(page_idx + 1, ((PAGE_W - dw) // 2, (PAGE_H - dh) // 2, dw, dh), dt)
    return canvas.px


def png_chunk(tag, data):
    out = struct.pack(">I", len(data)) + tag + data
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return out + struct.pack(">I", crc)


def encode_png(gray):
    """8-bit grayscale PNG (color type 0), one filter-0 byte per scanline."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", PAGE_W, PAGE_H, PNG_BIT_DEPTH_8, PNG_COLOR_GRAY, 0, 0, 0)
    raw = bytearray()
    for y in range(PAGE_H):
        raw.append(0)
        raw += gray[y * PAGE_W : (y + 1) * PAGE_W]
    idat = zlib.compress(bytes(raw), ZLIB_BEST)
    return sig + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


def build_cbz(pngs):
    """Minimal deterministic ZIP writer (DEFLATE members) that miniz reads."""
    files = []
    local = bytearray()
    central = bytearray()
    for idx, data in enumerate(pngs):
        name = f"page{idx + 1:02d}.png".encode("ascii")
        comp = zlib.compressobj(ZLIB_BEST, zlib.DEFLATED, DEFLATE_RAW_WBITS)
        body = comp.compress(data) + comp.flush()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        offset = len(local)
        local += struct.pack(
            "<IHHHHHIIIHH",
            ZIP_LOCAL_MAGIC,
            ZIP_VERSION,
            0,
            ZIP_METHOD_DEFLATE,
            0,
            0,
            crc,
            len(body),
            len(data),
            len(name),
            0,
        )
        local += name + body
        files.append((name, crc, len(body), len(data), offset))
    for name, crc, csize, usize, offset in files:
        central += struct.pack(
            "<IHHHHHHIIIHHHHHII",
            ZIP_CENTRAL_MAGIC,
            ZIP_VERSION,
            ZIP_VERSION,
            0,
            ZIP_METHOD_DEFLATE,
            0,
            0,
            crc,
            csize,
            usize,
            len(name),
            0,
            0,
            0,
            0,
            0,
            offset,
        )
        central += name
    cd_off = len(local)
    eocd = struct.pack(
        "<IHHHHIIH",
        ZIP_EOCD_MAGIC,
        0,
        0,
        len(files),
        len(files),
        len(central),
        cd_off,
        0,
    )
    return bytes(local) + bytes(central) + eocd


def emit_header(path, cbz):
    """Write the generated pure-ASCII C header holding the CBZ byte array."""
    header = (
        "/**\n"
        " * @file comic_pages_fixture.h\n"
        " * @brief Baked multi-page CBZ fixture for the viewable ereader_comic demo\n"
        " *        (generated by scripts/gen/gen_comic_fixture.py -- do not edit).\n"
        " *\n"
        f" * @details A miniz-decodable ZIP of {PAGE_COUNT} near-panel grayscale PNG pages\n"
        f" *          ({PAGE_W}x{PAGE_H} each). Every page carries a distinct paper tint,\n"
        " *          panel layout, and a huge 7-segment page number, so a page turn on the\n"
        " *          1024x600 panel is unmistakable. Pure 7-bit ASCII byte array, like the\n"
        " *          bundled font / cover blobs.\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " */\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"/** @brief Baked {len(cbz)}-byte multi-page CBZ ({PAGE_COUNT} pages). */\n"
        "static const uint8_t k_comic_cbz[] = {\n"
    )
    rows = []
    for start in range(0, len(cbz), BYTES_PER_ROW):
        chunk = cbz[start : start + BYTES_PER_ROW]
        rows.append("  " + " ".join(f"0x{byte:02X}," for byte in chunk))
    footer = (
        "\n};\n\n"
        "/** @brief Length of ::k_comic_cbz in bytes. */\n"
        f"static const uint32_t k_comic_cbz_len = {len(cbz)}U;\n\n"
        "/** @brief Page count baked into ::k_comic_cbz. */\n"
        f"static const uint32_t k_comic_page_count = {PAGE_COUNT}U;\n"
    )
    Path(path).write_text(header + "\n".join(rows) + footer, encoding="ascii")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "comic_pages_fixture.h"
    pngs = [encode_png(render_page(i)) for i in range(PAGE_COUNT)]
    cbz = build_cbz(pngs)
    emit_header(out, cbz)
    sys.stderr.write(f"wrote {out} ({len(cbz)} bytes CBZ, {PAGE_COUNT} pages)\n")


if __name__ == "__main__":
    main()
