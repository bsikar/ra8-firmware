#!/usr/bin/env python3
"""Compile a CBZ (a ZIP of comic/manga page images) into an "RCBZ" container.

Where ``cbz_compile.py`` maps a CBZ onto the RABOOK1 content model (one image
chapter per page, riding the existing whole-book RBKC demand-paged reader),
this tool emits the CBZ-native, per-page container issue #209 describes: the
source ZIP central directory becomes a manifest (page count + per-page decode
metadata), each page is pre-decoded to the panel-native 4bpp raster and
independently zlib-compressed, and the firmware reads a page at a time.

Why a second container at all: a ZIP already compresses every entry
independently, so a comic has no whole-archive DEFLATE stream to chunk. The
natural paging unit is therefore one page -- register a page's compressed byte
range as an ``ra8_vsource`` object and inflate it on the page-turn -- and the
manifest lets a shelf/cover view size a page without paging any raster in. The
owner's omnibus volumes can be multi-GB, so the book is read page-paged, never
resident.

Container layout (little-endian; kept in lockstep with
libs/ra8_book/inc/ra8_cbz_container.h):

    [0]  "RCBZ" magic (4 bytes)
    [4]  uint32 page_count
    [8]  uint32 flags            (k_ra8_book_flag_rtl == 0x1)
    [12] uint32 reserved         (0)
    [16] uint64 offset[page_count + 1]   payload-relative zlib-stream bounds
    [..] meta[page_count]        12 bytes each: uint32 raw_size, uint16 width,
         uint16 height, uint8 format, 3 reserved bytes
    [..] payload: page_count concatenated zlib streams, one per page

The gray4 pre-decode is imported from ``epub_compile.BlobBuilder`` (the exact
#212 transcode ``cbz_compile.py`` uses), and the page filter / natural sort are
imported from ``cbz_compile``, so the three tools can never drift.

Usage:
    cbz_container.py INPUT.cbz OUTPUT.rcbz [--rtl] [--max-edge N] [--stats]
    cbz_container.py --selftest

``--selftest`` builds a tiny synthetic CBZ in memory (scrambled entry order,
natural-sort-hostile names, junk entries), compiles it both LTR and RTL, then
re-parses the emitted container byte-for-byte and checks page order, the RTL
flag, preserved dimensions, and that every page's inflated raster is identical
to the direct #212 transcode.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
import zlib
from pathlib import Path
from typing import IO, NoReturn
from zipfile import ZipFile

from cbz_compile import is_page_entry, natural_key
from PIL import Image
from rabook_blob import BlobBuilder
from rabook_format import FLAG_RTL, IMG_GRAY4

# --- RCBZ wire layout (keep in lockstep with libs/ra8_book/inc/ra8_cbz_container.h) ---
CONTAINER_MAGIC = b"RCBZ"
HEADER_FMT = "<4sIII"  # magic, page_count, flags, reserved
HEADER_BYTES = 16
OFFSET_FMT = "<Q"
OFFSET_BYTES = 8
META_FMT = "<IHHBBH"  # raw_size, width, height, format, reserved, reserved2
META_BYTES = 12
# Only the RTL bit is a defined feature flag (k_ra8_book_flag_mask_known).
FLAG_MASK_KNOWN = FLAG_RTL
# zlib level: match wrap_container so a page compresses as tightly as it would
# inside the .rabook path.
ZLIB_LEVEL = 9


def transcode_page(name: str, data: bytes, max_image_edge: int = 0) -> tuple[bytes, int, int, int]:
    """Pre-decode one page to panel-native 4bpp via the #212 transcode.

    Reuses ``BlobBuilder.add_raster_image`` -- the exact gray4 quantize + pack
    the CBZ -> .rabook compiler emits -- so the container and the RABOOK1 arm
    can never drift on pixels. Returns ``(raw_bytes, width, height, format)``.
    """
    bb = BlobBuilder()
    idx = bb.add_raster_image(name, data, max_image_edge)
    _id_off, width, height, fmt, raw, _raw_size = bb.images[idx]
    return raw, width, height, fmt


def wrap_cbz_container(pages: list[tuple[bytes, int, int, int]], flags: int = 0) -> bytes:
    """Assemble the RCBZ container from a list of pre-decoded pages.

    ``pages`` is a list of ``(raw_bytes, width, height, format)``. Each page's
    raster is independently zlib-compressed, so any page inflates alone -- the
    property the per-page demand-paged reader relies on.
    """
    if not pages:
        msg = "no pages"
        raise ValueError(msg)
    if flags & ~FLAG_MASK_KNOWN:
        msg = f"unknown feature-flag bit(s): 0x{flags:08x}"
        raise ValueError(msg)
    streams = [zlib.compress(raw, ZLIB_LEVEL) for (raw, _w, _h, _fmt) in pages]
    offsets = [0]
    for stream in streams:
        offsets.append(offsets[-1] + len(stream))
    header = struct.pack(HEADER_FMT, CONTAINER_MAGIC, len(pages), flags, 0)
    off_table = b"".join(struct.pack(OFFSET_FMT, off) for off in offsets)
    meta_table = b"".join(
        struct.pack(META_FMT, len(raw), w, h, fmt, 0, 0) for (raw, w, h, fmt) in pages
    )
    return header + off_table + meta_table + b"".join(streams)


def compile_cbz_container(
    src: str | Path | IO[bytes], *, rtl: bool = False, max_image_edge: int = 0
) -> tuple[bytes, list[str], list[tuple[bytes, int, int, int]]]:
    """Walk a CBZ central directory and build the RCBZ container.

    ``src`` is a path or file object. Returns ``(container_bytes, page_names,
    pages)``. A page that fails to decode aborts the compile: unlike an EPUB's
    decorative figures, CBZ pages ARE the content, so a hole is corrupt input.
    """
    flags = FLAG_RTL if rtl else 0
    pages = []
    names = []
    with ZipFile(src) as zf:
        page_names = sorted((n for n in zf.namelist() if is_page_entry(n)), key=natural_key)
        if not page_names:
            msg = "no image pages found in the CBZ"
            raise ValueError(msg)
        for name in page_names:
            try:
                page = transcode_page(name, zf.read(name), max_image_edge)
            except (OSError, ValueError) as exc:
                msg = f"page {name!r} failed to decode"
                raise ValueError(msg) from exc
            pages.append(page)
            names.append(name)
    return wrap_cbz_container(pages, flags), names, pages


def parse_cbz_container(data: bytes) -> tuple[int, int, list[tuple[bytes, int, int, int]]]:
    """Inverse view of ``wrap_cbz_container`` for tests and inspection.

    Validates the magic, reserved word, offset monotonicity and every stream's
    inflated length exactly as the firmware reader does, then returns
    ``(page_count, flags, pages)`` where ``pages[i] == (raster, w, h, fmt)``.
    """
    magic, page_count, flags, reserved = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != CONTAINER_MAGIC:
        msg = "bad container magic"
        raise ValueError(msg)
    if reserved != 0 or page_count == 0:
        msg = "bad container header"
        raise ValueError(msg)
    offsets = struct.unpack_from(f"<{page_count + 1}Q", data, HEADER_BYTES)
    meta_at = HEADER_BYTES + ((page_count + 1) * OFFSET_BYTES)
    payload_at = meta_at + (page_count * META_BYTES)
    if offsets[0] != 0 or offsets[-1] != len(data) - payload_at:
        msg = "bad offset table"
        raise ValueError(msg)
    pages = []
    for i in range(page_count):
        raw_size, w, h, fmt, _r1, _r2 = struct.unpack_from(
            META_FMT, data, meta_at + (i * META_BYTES)
        )
        raster = zlib.decompress(data[payload_at + offsets[i] : payload_at + offsets[i + 1]])
        if len(raster) != raw_size:
            msg = f"page {i} inflated to the wrong length"
            raise ValueError(msg)
        pages.append((raster, w, h, fmt))
    return page_count, flags, pages


# --- selftest ------------------------------------------------------------------
def _fail(message: str) -> NoReturn:
    """Print ``message`` to stderr and exit non-zero (checkable, no traceback)."""
    sys.stderr.write(f"cbz_container.py: {message}\n")
    raise SystemExit(1)


def _require(cond: bool, what: str) -> None:
    """Exit non-zero unless ``cond`` holds (a checkable assert for the selftest)."""
    if not cond:
        _fail(f"selftest FAILED: {what}")


def _selftest_build_cbz() -> tuple[io.BytesIO, dict[str, tuple[int, int]]]:
    """Build the in-memory fixture CBZ: 3 pages + entries the filter must drop."""
    dims = {"vol1/p1.png": (7, 5), "vol1/p2.jpeg": (4, 9), "vol1/p10.png": (3, 3)}
    buf = io.BytesIO()
    with ZipFile(buf, "w") as zf:
        for name in ("vol1/p10.png", "vol1/p2.jpeg", "vol1/p1.png"):  # scrambled order
            w, h = dims[name]
            img = Image.new("L", (w, h))
            img.putdata([(x * 37 + y * 11) % 256 for y in range(h) for x in range(w)])
            out = io.BytesIO()
            img.save(out, "PNG" if name.endswith(".png") else "JPEG")
            zf.writestr(name, out.getvalue())
        zf.writestr("vol1/notes.txt", b"not a page")
        zf.writestr("__MACOSX/vol1/._p1.png", b"resource fork")
        zf.writestr("vol1/._p2.jpeg", b"appledouble")
        zf.writestr("vol1/art/", b"")
    buf.seek(0)
    return buf, dims


def _selftest_check(
    container: bytes, pages: list[tuple[bytes, int, int, int]], dims: dict[str, tuple[int, int]]
) -> None:
    """Re-parse a compiled container and check page order, dims, and rasters."""
    want = ["vol1/p1.png", "vol1/p2.jpeg", "vol1/p10.png"]
    page_count, flags, parsed = parse_cbz_container(container)
    _require(page_count == len(want), "page_count == number of pages")
    _require(flags == FLAG_RTL, "--rtl sets exactly the RTL flag bit")
    _require(len(parsed) == len(want), "parsed one raster per page")
    for i, name in enumerate(want):
        raster, w, h, fmt = parsed[i]
        src_raster, sw, sh, _sfmt = pages[i]
        _require((w, h) == dims[name], f"source dims preserved [{name}]")
        _require(fmt == IMG_GRAY4, f"gray4 format [{name}]")
        _require((w, h) == (sw, sh), f"meta dims == transcode dims [{name}]")
        _require(raster == src_raster, f"container raster == #212 transcode [{name}]")
        _require(len(raster) == ((w * h) + 1) // 2, f"4bpp packed size [{name}]")


def _selftest() -> int:
    """Round-trip self-check; exits non-zero on the first failed check."""
    fixture, dims = _selftest_build_cbz()
    want = ["vol1/p1.png", "vol1/p2.jpeg", "vol1/p10.png"]

    container, names, pages = compile_cbz_container(fixture, rtl=True)
    _require(names == want, f"natural page order (got {names})")
    _selftest_check(container, pages, dims)

    fixture.seek(0)
    container_ltr, _names2, _pages2 = compile_cbz_container(fixture, rtl=False)
    _pc, flags_ltr, _p = parse_cbz_container(container_ltr)
    _require(flags_ltr == 0, "default compile leaves flags 0")
    _require(
        container_ltr[HEADER_BYTES:] == container[HEADER_BYTES:],
        "rtl flips only the header flag word",
    )

    sys.stdout.write(
        "cbz_container.py selftest: PASS -- page order, RTL flag, dims, per-page rasters OK.\n"
    )
    return 0


def main() -> int:
    """Parse the command line and write the RCBZ container to disk.

    `--selftest` short-circuits and ignores the positional arguments; otherwise
    both are required.

    There is no `--chunk-bytes` here, unlike the .rabook path, and its absence
    is the point: RCBZ's paging unit is one page, so there is no whole-archive
    stream to slice and nothing to keep in step with the reader's frame size.
    That is what lets a multi-GB omnibus be read page-paged, never resident.

    Returns:
        0 on success; the selftest's own status when `--selftest` is given.
    """
    ap = argparse.ArgumentParser(description="Compile a CBZ into an RCBZ per-page container.")
    ap.add_argument("input", nargs="?", help="source .cbz (a ZIP of page images)")
    ap.add_argument("output", nargs="?", help="destination .rcbz container")
    ap.add_argument("--rtl", action="store_true", help="right-to-left reading order (manga)")
    ap.add_argument(
        "--max-edge",
        type=int,
        default=0,
        help="opt-in: downscale page long edge to at most this many pixels "
        "(default 0 = preserve source resolution)",
    )
    ap.add_argument("--stats", action="store_true", help="print size/structure stats")
    ap.add_argument(
        "--selftest", action="store_true", help="run the built-in round-trip self-check and exit"
    )
    args = ap.parse_args()

    if args.selftest:
        return _selftest()
    if not args.input or not args.output:
        ap.error("input and output are required unless --selftest")

    container, names, pages = compile_cbz_container(
        args.input, rtl=args.rtl, max_image_edge=args.max_edge
    )
    with Path(args.output).open("wb") as fh:
        fh.write(container)

    if args.stats:
        src_size = Path(args.input).stat().st_size
        out = len(container)
        inflated = sum(len(raw) for (raw, _w, _h, _fmt) in pages)
        direction = "rtl" if args.rtl else "ltr"
        print(f"{names[0] if names else '(empty)'} .. {len(pages)} pages [{direction}]")
        print(
            f"  cbz={src_size // 1024} KB -> rcbz={out // 1024} KB "
            f"({100 * out // max(src_size, 1)}%); inflated rasters={inflated // 1024} KB"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
