#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate the baked multi-page CBZ fixture for the viewable ereader_comic demo.

Emits a pure 7-bit-ASCII C header (comic_pages_fixture.h) holding one CBZ
(a miniz-decodable ZIP of grayscale PNG pages) as a byte array. The pages are
near-panel-size (aspect ~ 1024:600) and each is visually distinct -- a unique
paper tint, a different comic panel layout, and a huge 7-segment page number --
so a page turn on the panel is unmistakable. Grayscale PNGs of flat-shaded art
DEFLATE to a few hundred bytes each, so the whole archive stays small enough to
bake as an ASCII array under the repo's per-file line cap.

The output is deterministic (fixed art, fixed zlib level), so the render hash
the app prints is identical on host, ra8_emulator, and silicon.

Usage:
  python3 scripts/gen/gen_comic_fixture.py \
      examples/ek_ra8d2/hw_pending/ereader_comic/comic_pages_fixture.h
"""

from __future__ import annotations

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

    def __init__(self, fill: int) -> None:
        """Allocate a full page pre-filled with one gray level (the paper tint)."""
        self.px = bytearray([fill]) * (PAGE_W * PAGE_H)

    def fill_rect(self, rect: tuple[int, int, int, int], v: int) -> None:
        """Paint an ``(x, y, w, h)`` rectangle, clipped to the page.

        Clipping is applied per axis rather than rejecting an out-of-range
        rect, so callers may compute a rectangle that runs off the edge and get
        the visible part -- ``frame_rect`` and ``draw_digit`` both rely on that
        and would otherwise need bounds checks of their own.
        """
        x0, y0, w, h = rect
        for y in range(max(0, y0), min(PAGE_H, y0 + h)):
            row = y * PAGE_W
            for x in range(max(0, x0), min(PAGE_W, x0 + w)):
                self.px[row + x] = v

    def frame_rect(self, rect: tuple[int, int, int, int], t: int, v: int) -> None:
        """Draw a hollow rectangle of border thickness ``t``, inset into ``rect``.

        The four sides are drawn as overlapping bars, so corners are painted
        twice; harmless here because every bar uses the same value ``v``.
        """
        x0, y0, w, h = rect
        self.fill_rect((x0, y0, w, t), v)
        self.fill_rect((x0, y0 + h - t, w, t), v)
        self.fill_rect((x0, y0, t, h), v)
        self.fill_rect((x0 + w - t, y0, t, h), v)

    def draw_digit(self, digit: int, rect: tuple[int, int, int, int], t: int) -> None:
        """Render one decimal digit as a 7-segment figure filling ``rect``.

        Always drawn in INK, so the digit stays legible over any paper tint or
        panel fill; the segment thickness ``t`` is the caller's, not derived
        from the rect, which keeps the stroke weight identical across pages.
        """
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


def draw_layout(canvas: Canvas, page_idx: int, fill: int) -> None:
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


def render_page(page_idx: int) -> bytearray:
    """Build one distinct grayscale page: border, panels, and a huge number."""
    canvas = Canvas(PAGE_PAPER[page_idx])
    canvas.frame_rect((6, 6, PAGE_W - 12, PAGE_H - 12), 6, INK)
    draw_layout(canvas, page_idx, PAGE_FILL[page_idx])
    dw, dh, dt = 90, 150, 20
    canvas.draw_digit(page_idx + 1, ((PAGE_W - dw) // 2, (PAGE_H - dh) // 2, dw, dh), dt)
    return canvas.px


def png_chunk(tag: bytes, data: bytes) -> bytes:
    """Wrap a payload in one PNG chunk: length, type, data, CRC-32.

    The CRC covers the type tag AND the data but NOT the length prefix, which
    is what the PNG spec requires and the easiest detail to get wrong -- a
    chunk whose CRC included the length decodes as corrupt in every reader.
    """
    out = struct.pack(">I", len(data)) + tag + data
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return out + struct.pack(">I", crc)


def encode_png(gray: bytes | bytearray) -> bytes:
    """8-bit grayscale PNG (color type 0), one filter-0 byte per scanline."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", PAGE_W, PAGE_H, PNG_BIT_DEPTH_8, PNG_COLOR_GRAY, 0, 0, 0)
    raw = bytearray()
    for y in range(PAGE_H):
        raw.append(0)
        raw += gray[y * PAGE_W : (y + 1) * PAGE_W]
    idat = zlib.compress(bytes(raw), ZLIB_BEST)
    return sig + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b"")


def _zip_local_entry(idx: int, data: bytes) -> tuple[bytes, tuple[bytes, int, int, int, int]]:
    """Deflate one page and return its local-header record plus its directory entry.

    The offset in the returned entry is relative to the start of the local
    section, so the caller must add records in the same order it later writes
    the central directory -- a ZIP whose offsets disagree is unreadable.
    """
    name = f"page{idx + 1:02d}.png".encode("ascii")
    comp = zlib.compressobj(ZLIB_BEST, zlib.DEFLATED, DEFLATE_RAW_WBITS)
    body = comp.compress(data) + comp.flush()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    record = (
        struct.pack(
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
        + name
        + body
    )
    return record, (name, crc, len(body), len(data), 0)


def _zip_central_record(entry: tuple[bytes, int, int, int, int]) -> bytes:
    """Central-directory record for one already-written local entry."""
    name, crc, csize, usize, offset = entry
    return (
        struct.pack(
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
        + name
    )


def _zip_eocd(count: int, central_len: int, cd_off: int) -> bytes:
    """End-of-central-directory record closing the archive."""
    return struct.pack(
        "<IHHHHIIH",
        ZIP_EOCD_MAGIC,
        0,
        0,
        count,
        count,
        central_len,
        cd_off,
        0,
    )


def build_cbz(pngs: list[bytes]) -> bytes:
    """Minimal deterministic ZIP writer (DEFLATE members) that miniz reads."""
    files: list[tuple[bytes, int, int, int, int]] = []
    local = bytearray()
    for idx, data in enumerate(pngs):
        offset = len(local)
        record, entry = _zip_local_entry(idx, data)
        local += record
        files.append((*entry[:4], offset))
    central = bytearray()
    for entry in files:
        central += _zip_central_record(entry)
    cd_off = len(local)
    return bytes(local) + bytes(central) + _zip_eocd(len(files), len(central), cd_off)


def emit_header(path: str | Path, cbz: bytes) -> None:
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


def main() -> None:
    """Render every page, pack them into a CBZ, and write the C header.

    Takes the output path from argv[1], defaulting to the bare filename in the
    current directory. Returns None and exits 0 unconditionally: every failure
    mode here (unwritable path, non-ASCII output) raises, and there is no
    partial-success state a status code could describe.

    The progress line goes to stderr so stdout stays free for the generated
    header if this is ever redirected.
    """
    out = sys.argv[1] if len(sys.argv) > 1 else "comic_pages_fixture.h"
    pngs = [encode_png(render_page(i)) for i in range(PAGE_COUNT)]
    cbz = build_cbz(pngs)
    emit_header(out, cbz)
    sys.stderr.write(f"wrote {out} ({len(cbz)} bytes CBZ, {PAGE_COUNT} pages)\n")


if __name__ == "__main__":
    main()
