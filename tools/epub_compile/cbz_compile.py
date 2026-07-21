#!/usr/bin/env python3
"""Compile a CBZ (a ZIP of comic/manga page images) into a .rabook blob.

CBZ is the manga interchange format: page order is the filename sort of the
archive entries, and there is no OPF/spine/TOC/CSS -- the images ARE the book.
This tool maps that onto the exact RABOOK1 content model the EPUB compiler
emits for fixed-layout books (issue #196 calls that shape "CBZ-in-EPUB-
clothing"): one spine chapter per page whose DOM is a single full-page
``<img>`` element referencing one manifest image, so the device render path
(``ra8_reflow``'s existing image handling) is shared with EPUB books instead of
growing a second reader. Nothing here parses XHTML; the chapters are
synthesized.

Behavior (issue #212):

* Pages are the archive's image entries (png/jpg/jpeg/webp/bmp via Pillow),
  natural-sorted so ``p2`` orders before ``p10``. Directories, hidden files,
  ``__MACOSX/`` resource forks, and non-image entries are ignored.
* Raster pages are transcoded to panel-native 4bpp grayscale at SOURCE
  resolution -- no downscale by default (issue #210; manga text must survive
  the zoom loupe). ``--max-edge`` remains the opt-in clamp.
* The first page doubles as the shelf cover (``cover_image_index``).
* ``--rtl`` stamps the right-to-left reading-order flag
  (``k_ra8_book_flag_rtl`` in libs/ra8_book/inc/ra8_book.h) into the header;
  mirrored page-turn zones on device are issue #211.
* The flat blob is wrapped in the chunked RBKC container, same as EPUB books,
  so full-resolution volumes ride the demand-paged open.

The heavy lifting (gray4 quantize + packing, string interning, table
serialization, RBKC wrapping) is imported from ``epub_compile.py`` so the two
compilers can never drift apart on the wire format.

Usage:
    cbz_compile.py INPUT.cbz OUTPUT.rabook [--rtl] [--title T] [--author A]
                   [--max-edge N] [--chunk-bytes N] [--stats]
    cbz_compile.py --selftest

``--selftest`` builds a tiny synthetic CBZ in memory (scrambled entry order,
natural-sort-hostile names, junk entries), compiles it both LTR and RTL, then
re-parses the emitted container byte-for-byte and checks page order, the RTL
flag, the cover index, preserved dimensions, and the chapter DOM shape.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import argparse
import io
import posixpath
import re
import struct
import sys
import zlib
from pathlib import Path
from typing import NoReturn
from zipfile import ZipFile

from epub_compile import (
    CONTAINER_CHUNK_BYTES,
    FLAG_RTL,
    IMG_GRAY4,
    MAGIC,
    BlobBuilder,
    wrap_container,
)
from PIL import Image

# Page entries accepted from the archive (decoded via Pillow).
IMAGE_EXTS = frozenset({".png", ".jpg", ".jpeg", ".webp", ".bmp"})

# --- wire-layout mirrors used only by the selftest parser ---------------------
# (kept in lockstep with libs/ra8_book/inc/ra8_book.h and BlobBuilder.serialize)
HEADER_FMT = "<8s23I"
HEADER_BYTES = 100
CHAPTER_FMT = "<3I"
CHAPTER_BYTES = 12
NODE_FMT = "<BBHIIIII"
NODE_BYTES = 24
ATTR_FMT = "<2I"
ATTR_BYTES = 8
IMAGE_FMT = "<IHHBBHIII"
IMAGE_BYTES = 24
CONT_HDR_FMT = "<IQII"
CONT_MAGIC = b"RBKC"
CONT_MAGIC_LEN = 4
CONT_TABLE_OFF = 24
CONT_ENTRY_BYTES = 8


def natural_key(name):
    """Sort key ordering embedded integers numerically (page2 < page10).

    ``re.split`` with a capturing digit group always alternates non-digit and
    digit tokens starting with a (possibly empty) non-digit token, so compared
    keys never pit an int against a str at the same position.
    """
    return [int(tok) if tok.isdigit() else tok.lower() for tok in re.split(r"(\d+)", name)]


def is_page_entry(name):
    """Whether an archive entry is a readable page image.

    Rejects directory entries, hidden/AppleDouble files (``.foo``, ``._foo``),
    anything under ``__MACOSX/``, and non-image extensions.
    """
    if name.endswith("/"):
        return False
    if "__MACOSX" in name.split("/"):
        return False
    base = posixpath.basename(name)
    if not base or base.startswith("."):
        return False
    return posixpath.splitext(base)[1].lower() in IMAGE_EXTS


def page_dom(name):
    """One page's synthetic chapter DOM: ``<body><img src=NAME alt=BASE/></body>``.

    Mirrors the fixed-layout EPUB shape so ``ra8_reflow``'s existing image path
    lays the page out; ``src`` equals the manifest image id exactly, so href
    resolution is an exact string match.
    """
    img = {
        "tag": "img",
        "attrs": [("src", name), ("alt", posixpath.basename(name))],
        "children": [],
    }
    return {"tag": "body", "attrs": [], "children": [img]}


def compile_cbz(src, *, title="", author="", rtl=False, max_image_edge=0):
    """Compile a CBZ (path or file object) to a flat RABOOK1 blob.

    Returns ``(blob, meta, builder)`` like ``epub_compile.compile_epub``.
    A page that fails to decode aborts the compile: unlike an EPUB's
    decorative figures, CBZ pages ARE the content, so a hole is corrupt input.
    """
    bb = BlobBuilder()
    if rtl:
        bb.flags |= FLAG_RTL
    with ZipFile(src) as zf:
        pages = sorted((n for n in zf.namelist() if is_page_entry(n)), key=natural_key)
        if not pages:
            msg = "no image pages found in the CBZ"
            raise ValueError(msg)
        for num, name in enumerate(pages, start=1):
            try:
                idx = bb.add_raster_image(name, zf.read(name), max_image_edge)
            except (OSError, ValueError) as exc:
                msg = f"page {name!r} failed to decode"
                raise ValueError(msg) from exc
            if num == 1:
                bb.cover_index = idx
            bb.add_chapter(page_dom(name), f"Page {num}", name)
    stem = Path(src).stem if isinstance(src, (str, Path)) else ""
    meta = {
        "title": title or stem,
        "author": author,
        "language": "",
        "identifier": stem or (title or ""),
    }
    return bb.serialize(meta), meta, bb


# --- selftest ------------------------------------------------------------------
def _fail(message: str) -> NoReturn:
    """Print ``message`` to stderr and exit non-zero (checkable, no traceback)."""
    sys.stderr.write(f"cbz_compile.py: {message}\n")
    raise SystemExit(1)


def _require(cond: bool, what: str) -> None:
    """Exit non-zero unless ``cond`` holds (a checkable assert for the selftest)."""
    if not cond:
        _fail(f"selftest FAILED: {what}")


def unwrap_container(data):
    """Inverse of ``wrap_container``: inflate an RBKC file back to the flat blob."""
    if data[:CONT_MAGIC_LEN] != CONT_MAGIC:
        msg = "bad container magic"
        raise ValueError(msg)
    chunk_bytes, total, count, reserved = struct.unpack_from(CONT_HDR_FMT, data, CONT_MAGIC_LEN)
    if reserved != 0 or chunk_bytes <= 0:
        msg = "bad container header"
        raise ValueError(msg)
    offs = struct.unpack_from(f"<{count + 1}Q", data, CONT_TABLE_OFF)
    payload = data[CONT_TABLE_OFF + ((count + 1) * CONT_ENTRY_BYTES) :]
    blob = b"".join(zlib.decompress(payload[offs[i] : offs[i + 1]]) for i in range(count))
    if len(blob) != total:
        msg = "container inflated to the wrong length"
        raise ValueError(msg)
    return blob


def _parse_blob(blob):
    """Decode the header + chapter/node/attr/image tables for the selftest."""
    fields = struct.unpack_from(HEADER_FMT, blob, 0)
    hdr = {
        "magic": fields[0],
        "format_version": fields[1],
        "total_size": fields[2],
        "flags": fields[3],
        "title_off": fields[4],
        "cover_image_index": fields[8],
        "chapter_count": fields[9],
        "chapter_off": fields[10],
        "node_off": fields[12],
        "attr_off": fields[14],
        "image_count": fields[17],
        "image_off": fields[18],
        "string_off": fields[19],
        "crc32": fields[23],
    }

    def string_at(off):
        start = hdr["string_off"] + off
        return blob[start : blob.index(b"\x00", start)].decode("utf-8")

    chapters = [
        struct.unpack_from(CHAPTER_FMT, blob, hdr["chapter_off"] + (i * CHAPTER_BYTES))
        for i in range(hdr["chapter_count"])
    ]
    images = [
        struct.unpack_from(IMAGE_FMT, blob, hdr["image_off"] + (i * IMAGE_BYTES))
        for i in range(hdr["image_count"])
    ]
    return hdr, chapters, images, string_at


def _node_at(blob, node_off, idx):
    """Unpack DOM node ``idx`` from the node table."""
    return struct.unpack_from(NODE_FMT, blob, node_off + (idx * NODE_BYTES))


def _selftest_build_cbz():
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


def _selftest_check_pages(blob, dims):
    """Check page order, cover, dims, packing, and the per-page DOM shape."""
    hdr, chapters, images, string_at = _parse_blob(blob)
    want = ["vol1/p1.png", "vol1/p2.jpeg", "vol1/p10.png"]
    _require(hdr["magic"] == MAGIC, "blob magic")
    _require(hdr["total_size"] == len(blob), "blob total_size")
    _require(zlib.crc32(blob[HEADER_BYTES:]) == hdr["crc32"], "body crc32")
    _require(hdr["chapter_count"] == len(want), "one chapter per page")
    _require(hdr["image_count"] == len(want), "one image per page")
    _require(hdr["cover_image_index"] == 0, "first page is the cover")
    hrefs = [string_at(href_off) for (_t, href_off, _r) in chapters]
    _require(hrefs == want, f"natural page order (got {hrefs})")
    for i, name in enumerate(want):
        id_off, w, h, fmt, _r1, _r2, _doff, dsize, rsize = images[i]
        _require(string_at(id_off) == name, f"image id [{i}]")
        _require((w, h) == dims[name], f"source dims preserved [{name}]")
        _require(fmt == IMG_GRAY4, f"gray4 transcode [{name}]")
        _require(dsize == rsize == ((w * h) + 1) // 2, f"4bpp packed size [{name}]")
        # DOM shape: chapter root is <body> whose only child is a full-page
        # <img> whose src equals the manifest image id exactly.
        root = _node_at(blob, hdr["node_off"], chapters[i][2])
        _require(string_at(root[3]) == "body", f"chapter root is body [{i}]")
        child = _node_at(blob, hdr["node_off"], root[6])
        _require(string_at(child[3]) == "img", f"page child is img [{i}]")
        attrs = [
            struct.unpack_from(ATTR_FMT, blob, hdr["attr_off"] + ((child[5] + k) * ATTR_BYTES))
            for k in range(child[2])
        ]
        pairs = {string_at(n): string_at(v) for (n, v) in attrs}
        _require(pairs.get("src") == name, f"img src == image id [{i}]")


def _selftest():
    """Round-trip self-check; exits non-zero on the first failed check."""
    fixture, dims = _selftest_build_cbz()
    blob, meta, bb = compile_cbz(fixture, title="SelfTest Manga", rtl=True)
    container = wrap_container(blob)
    inflated = unwrap_container(container)
    _require(inflated == blob, "container round-trip")
    hdr, _c, _i, string_at = _parse_blob(inflated)
    _require(hdr["flags"] == FLAG_RTL, "--rtl sets exactly the RTL flag bit")
    _require(string_at(hdr["title_off"]) == "SelfTest Manga", "title interned")
    _selftest_check_pages(inflated, dims)
    _require(bb.cover_index == 0, "builder cover index")
    _require(meta["title"] == "SelfTest Manga", "meta title")

    fixture.seek(0)
    blob_ltr, _meta2, _bb2 = compile_cbz(fixture, title="SelfTest Manga", rtl=False)
    hdr_ltr, _c2, _i2, _s2 = _parse_blob(blob_ltr)
    _require(hdr_ltr["flags"] == 0, "default compile leaves flags 0")
    _require(blob_ltr[HEADER_BYTES:] == blob[HEADER_BYTES:], "rtl flips only the header flag")

    sys.stdout.write(
        "cbz_compile.py selftest: PASS -- page order, RTL flag, cover, dims, DOM shape OK.\n"
    )
    return 0


def main():
    """Parse the command line and write the RBKC-wrapped .rabook to disk.

    `--selftest` short-circuits everything else and ignores the positional
    arguments. Otherwise both are required.

    Two flags have consequences past this process. `--rtl` sets a header bit
    the device reads as manga page order, and it is the ONLY difference in the
    output -- the page rasters are byte-identical either way, which the selftest
    asserts. `--chunk-bytes` must equal the reader's `ra8_vmem` frame size; a
    mismatch produces a container the firmware cannot demand-page.

    Note this is the whole-book paging path. For multi-GB omnibus volumes use
    `cbz_container.py`, which pages one image at a time instead.

    Returns:
        0 on success; the selftest's own status when `--selftest` is given.
    """
    ap = argparse.ArgumentParser(description="Compile a CBZ into a .rabook blob.")
    ap.add_argument("input", nargs="?", help="source .cbz (a ZIP of page images)")
    ap.add_argument("output", nargs="?", help="destination .rabook")
    ap.add_argument("--rtl", action="store_true", help="right-to-left reading order (manga)")
    ap.add_argument("--title", default="", help="book title (default: input filename stem)")
    ap.add_argument("--author", default="", help="book author (default: empty)")
    ap.add_argument(
        "--max-edge",
        type=int,
        default=0,
        help="opt-in: downscale page long edge to at most this many pixels "
        "(default 0 = preserve source resolution)",
    )
    ap.add_argument(
        "--chunk-bytes",
        type=int,
        default=CONTAINER_CHUNK_BYTES,
        help="inflated bytes per independently-compressed container chunk "
        "(must equal the reader's ra8_vmem frame size)",
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

    blob, meta, bb = compile_cbz(
        args.input,
        title=args.title,
        author=args.author,
        rtl=args.rtl,
        max_image_edge=args.max_edge,
    )
    container = wrap_container(blob, args.chunk_bytes)
    with Path(args.output).open("wb") as fh:
        fh.write(container)

    if args.stats:
        src_size = Path(args.input).stat().st_size
        out = len(container)
        direction = "rtl" if args.rtl else "ltr"
        print(f"{meta['title']} -- {meta['author'] or '(no author)'} [{direction}]")
        print(f"  pages={len(bb.chapters)} images={len(bb.images)} nodes={len(bb.nodes)}")
        print(
            f"  cbz={src_size // 1024} KB -> rabook={out // 1024} KB "
            f"({100 * out // max(src_size, 1)}%); inflated={len(blob) // 1024} KB"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
