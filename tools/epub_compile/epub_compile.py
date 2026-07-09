#!/usr/bin/env python3
"""Compile an EPUB into a flat, execute-in-place .rabook blob.

The on-device reader (libs/ra_book) never unzips or parses XHTML at runtime.
This host tool does it once: it unzips the EPUB, parses every spine document
into a faithful DOM (every tag, attribute and text run preserved), keeps each
stylesheet verbatim, transcodes raster images to the panel-native 4bpp
grayscale (dithered, full resolution, DEFLATE-compressed) and preserves SVG as
vector source, then serializes everything into the binary layout described by
libs/ra_book/inc/ra_book.h.

Fidelity is the rule: nothing in the markup is dropped to match what the
renderer understands today. The only content that changes form is raster
images, because the e-ink panel is physically 4bpp.

Usage:
    epub_compile.py INPUT.epub OUTPUT.rabook [--stats]

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import argparse
import io
import posixpath
import struct
import sys
import zlib
from html.parser import HTMLParser
from pathlib import Path
from xml.etree import ElementTree as ET
from zipfile import ZipFile

from PIL import Image

# The SE masters are trusted local input; some cover scans exceed Pillow's
# default decompression-bomb threshold, so lift it rather than warn.
Image.MAX_IMAGE_PIXELS = None

# --- on-disk constants, kept in lockstep with libs/ra_book/inc/ra_book.h ------
MAGIC = b"RABOOK1\x00"
FORMAT_VERSION = 1
NIL = 0xFFFFFFFF
NODE_ELEMENT = 0
NODE_TEXT = 1
IMG_GRAY4 = 0
IMG_SVG = 1
GRAY_LEVELS = 16
# Full 256-entry RGB palette = 256 * 3 channels.
PALETTE_BYTES = 768
# .rabook chunked container ("RBKC"; keep in sync with ra_book_container_t in
# libs/ra_book/inc/ra_book.h):
#   "RBKC" + <I chunk_bytes + <Q inflated_total + <I chunk_count + <I reserved(0)
#   + <Q offset[chunk_count + 1] (payload-relative stream offsets)
#   + chunk_count concatenated zlib streams, one per chunk_bytes slice of the
#     flat blob (last slice short).
# Every chunk inflates independently, so the device can either inflate all of
# them into SDRAM (resident open) or inflate single chunks on demand into
# ra_vmem cache frames (multi-GB books). chunk_bytes must equal the reader's
# cache frame size; 64 KiB is the current firmware default.
CONTAINER_MAGIC = b"RBKC"
CONTAINER_CHUNK_BYTES = 65536


def wrap_container(blob, chunk_bytes=CONTAINER_CHUNK_BYTES):
    """Wrap a flat RABOOK1 blob in the chunked RBKC container."""
    if not blob:
        msg = "empty blob"
        raise ValueError(msg)
    if chunk_bytes <= 0:
        msg = "chunk_bytes must be positive"
        raise ValueError(msg)
    count = (len(blob) + chunk_bytes - 1) // chunk_bytes
    streams = [
        zlib.compress(blob[i * chunk_bytes : (i + 1) * chunk_bytes], 9) for i in range(count)
    ]
    offsets = [0]
    for stream in streams:
        offsets.append(offsets[-1] + len(stream))
    header = CONTAINER_MAGIC + struct.pack("<IQII", chunk_bytes, len(blob), count, 0)
    table = b"".join(struct.pack("<Q", off) for off in offsets)
    return header + table + b"".join(streams)


# The e-ink panel cannot resolve more than panel-class pixels, so storing
# full-resolution source images just bloats the blob with pixels that never
# render. Downscaling the long edge to this bound is the single biggest size
# lever; FS dithering is left off because its high-frequency noise defeats
# DEFLATE (the renderer can dither at draw time if desired).
MAX_IMAGE_EDGE = 1600
# When true, drop all images (text-only). Yields a tiny inflated blob that fits
# in MRAM as a baked fixture -- used by the on-device reader demo.
SKIP_IMAGES = False

VOID_TAGS = {
    "area",
    "base",
    "br",
    "col",
    "embed",
    "hr",
    "img",
    "input",
    "link",
    "meta",
    "param",
    "source",
    "track",
    "wbr",
}


class StringPool:
    """De-duplicating UTF-8 string pool; offset 0 is always the empty string."""

    def __init__(self):
        self.buf = bytearray()
        self._map = {}
        self.intern("")

    def intern(self, text):
        if text is None:
            text = ""
        raw = text.encode("utf-8")
        got = self._map.get(raw)
        if got is not None:
            return got
        off = len(self.buf)
        self.buf += raw + b"\x00"
        self._map[raw] = off
        return off


class DomBuilder(HTMLParser):
    """Lenient HTML/XHTML parser that builds a generic element/text tree.

    convert_charrefs resolves entities to Unicode, and <style>/<script> bodies
    are captured as text (so inline CSS survives). Tag names and attributes are
    preserved exactly; inline <svg> becomes ordinary elements.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = {"tag": "#root", "attrs": [], "children": []}
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = {"tag": tag, "attrs": attrs, "children": []}
        self.stack[-1]["children"].append(node)
        if tag not in VOID_TAGS:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.stack[-1]["children"].append({"tag": tag, "attrs": attrs, "children": []})

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i]["tag"] == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        if data:
            self.stack[-1]["children"].append({"text": data})


def find_first(node, tag):
    """Depth-first search for the first element with the given tag name."""
    for child in node.get("children", []):
        if child.get("tag") == tag:
            return child
        hit = find_first(child, tag)
        if hit is not None:
            return hit
    return None


class BlobBuilder:
    """Accumulates the node/attr/chapter/image tables and emits the blob."""

    def __init__(self):
        self.sp = StringPool()
        self.nodes = []  # list of dicts (see _emit_node)
        self.attrs = []  # list of (name_off, value_off)
        self.chapters = []  # list of (title_off, href_off, root_node)
        self.stylesheets = []  # list of (source_off, scope_chapter)
        self.images = []  # list of (id_off, w, h, fmt, data, raw_size)
        self.cover_index = NIL

    # -- DOM serialization ----------------------------------------------------
    def add_text(self, text):
        idx = len(self.nodes)
        self.nodes.append(
            {
                "kind": NODE_TEXT,
                "name_off": 0,
                "text_off": self.sp.intern(text),
                "attr_count": 0,
                "first_attr": NIL,
                "first_child": NIL,
                "next_sibling": NIL,
            }
        )
        return idx

    def add_element(self, elem):
        first_attr = NIL
        acount = 0
        if elem["attrs"]:
            first_attr = len(self.attrs)
            for name, value in elem["attrs"]:
                self.attrs.append((self.sp.intern(name), self.sp.intern(value)))
                acount += 1
        idx = len(self.nodes)
        self.nodes.append(
            {
                "kind": NODE_ELEMENT,
                "name_off": self.sp.intern(elem["tag"]),
                "text_off": 0,
                "attr_count": acount,
                "first_attr": first_attr,
                "first_child": NIL,
                "next_sibling": NIL,
            }
        )
        kids = []
        for child in elem["children"]:
            if "text" in child:
                kids.append(self.add_text(child["text"]))
            else:
                kids.append(self.add_element(child))
        if kids:
            self.nodes[idx]["first_child"] = kids[0]
            for cur, nxt in zip(kids, kids[1:]):
                self.nodes[cur]["next_sibling"] = nxt
        return idx

    def add_chapter(self, root_elem, title, href):
        root_idx = self.add_element(root_elem)
        self.chapters.append((self.sp.intern(title), self.sp.intern(href), root_idx))

    # -- assets ---------------------------------------------------------------
    def add_stylesheet(self, css_text):
        self.stylesheets.append((self.sp.intern(css_text), NIL))

    def add_raster_image(self, href, data, max_image_edge=MAX_IMAGE_EDGE):
        im = Image.open(io.BytesIO(data)).convert("L")
        w, h = im.size
        scale = min(1.0, max_image_edge / max(w, h))
        if scale < 1.0:
            im = im.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)
        width, height = im.size
        palette = []
        for i in range(GRAY_LEVELS):
            v = round(i * 255 / (GRAY_LEVELS - 1))
            palette += [v, v, v]
        while len(palette) < PALETTE_BYTES:
            palette += [0, 0, 0]
        pal_img = Image.new("P", (1, 1))
        pal_img.putpalette(palette)
        quantized = (
            im.convert("RGB").quantize(palette=pal_img, dither=Image.Dither.NONE).convert("L")
        )
        nib_table = bytes(min(v // 17, GRAY_LEVELS - 1) for v in range(256))
        nibbles = quantized.tobytes().translate(nib_table)
        even = nibbles[0::2]
        odd = nibbles[1::2]
        packed = bytearray(bytes((e << 4) | o for e, o in zip(even, odd)))
        if len(even) > len(odd):
            packed.append(even[-1] << 4)
        # Stored raw; the whole blob is DEFLATE-wrapped as one stream on disk, so
        # per-image compression would just double-compress for no gain. After the
        # single inflate-on-open these bytes are panel-ready 4bpp, no decode.
        raw = bytes(packed)
        self.images.append((self.sp.intern(href), width, height, IMG_GRAY4, raw, len(raw)))
        return len(self.images) - 1

    def add_svg_image(self, href, data):
        self.images.append((self.sp.intern(href), 0, 0, IMG_SVG, data, len(data)))
        return len(self.images) - 1

    # -- serialization --------------------------------------------------------
    def serialize(self, meta):
        chap = b"".join(struct.pack("<3I", *c) for c in self.chapters)
        node = b"".join(
            struct.pack(
                "<BBHIIIII",
                n["kind"],
                0,
                n["attr_count"],
                n["name_off"],
                n["text_off"],
                n["first_attr"],
                n["first_child"],
                n["next_sibling"],
            )
            for n in self.nodes
        )
        attr = b"".join(struct.pack("<2I", *a) for a in self.attrs)
        style = b"".join(struct.pack("<2I", *s) for s in self.stylesheets)

        # image pool first, so the image table can reference data_off
        pool = bytearray()
        img_records = []
        for id_off, w, h, fmt, data, raw in self.images:
            data_off = len(pool)
            pool += data
            img_records.append(
                struct.pack("<IHHBBHIII", id_off, w, h, fmt, 0, 0, data_off, len(data), raw)
            )
        image = b"".join(img_records)

        # Intern metadata strings BEFORE snapshotting the pool. They may not
        # appear anywhere in the DOM, so interning them later (during the header
        # pack) would append past the captured `strings` and dangle the offsets.
        title_off = self.sp.intern(meta["title"])
        author_off = self.sp.intern(meta["author"])
        language_off = self.sp.intern(meta["language"])
        identifier_off = self.sp.intern(meta["identifier"])
        strings = bytes(self.sp.buf)

        header_size = 100
        off_chap = header_size
        off_node = off_chap + len(chap)
        off_attr = off_node + len(node)
        off_style = off_attr + len(attr)
        off_image = off_style + len(style)
        off_string = off_image + len(image)
        off_pool = off_string + len(strings)
        total = off_pool + len(pool)

        body = chap + node + attr + style + image + strings + bytes(pool)
        crc = zlib.crc32(body) & 0xFFFFFFFF

        header = struct.pack(
            "<8s23I",
            MAGIC,
            FORMAT_VERSION,
            total,
            0,
            title_off,
            author_off,
            language_off,
            identifier_off,
            self.cover_index,
            len(self.chapters),
            off_chap,
            len(self.nodes),
            off_node,
            len(self.attrs),
            off_attr,
            len(self.stylesheets),
            off_style,
            len(self.images),
            off_image,
            off_string,
            len(strings),
            off_pool,
            len(pool),
            crc,
        )
        assert len(header) == header_size  # noqa: S101  # structural invariant: mismatched header_size is a coding error
        return header + body


# --- EPUB unpacking -----------------------------------------------------------
def opf_localname(tag):
    return tag.split("}", 1)[1] if tag and tag[0] == "{" else tag


def parse_opf(zf):
    container = ET.fromstring(zf.read("META-INF/container.xml"))  # noqa: S314  # trusted local EPUB input
    rootfile = None
    for el in container.iter():
        if opf_localname(el.tag) == "rootfile":
            rootfile = el.get("full-path")
            break
    opf = ET.fromstring(zf.read(rootfile))  # noqa: S314  # trusted local EPUB input
    opf_dir = posixpath.dirname(rootfile)

    meta = {"title": "", "author": "", "language": "", "identifier": ""}
    manifest = {}  # id -> (href, media_type, properties)
    spine = []  # list of idref
    cover_id = None
    for el in opf.iter():
        tag = opf_localname(el.tag)
        if tag == "title" and not meta["title"]:
            meta["title"] = (el.text or "").strip()
        elif tag == "creator" and not meta["author"]:
            meta["author"] = (el.text or "").strip()
        elif tag == "language" and not meta["language"]:
            meta["language"] = (el.text or "").strip()
        elif tag == "identifier" and not meta["identifier"]:
            meta["identifier"] = (el.text or "").strip()
        elif tag == "meta" and el.get("name") == "cover":
            cover_id = el.get("content")
        elif tag == "item":
            manifest[el.get("id")] = (
                el.get("href"),
                el.get("media-type", ""),
                el.get("properties", "") or "",
            )
        elif tag == "itemref":
            spine.append(el.get("idref"))
    return opf_dir, meta, manifest, spine, cover_id


def parse_toc(zf, opf_dir, manifest):
    """Map a normalized doc path -> TOC label, from the EPUB3 nav or EPUB2 NCX."""
    labels = {}

    def resolve(href):
        return posixpath.normpath(posixpath.join(opf_dir, href.split("#", 1)[0]))

    nav_href = next((h for (h, _m, p) in manifest.values() if "nav" in p), None)
    ncx_href = next(
        (h for (h, m, _p) in manifest.values() if m == "application/x-dtbncx+xml"), None
    )
    try:
        if nav_href:
            tree = ET.fromstring(zf.read(resolve(nav_href)))  # noqa: S314  # trusted local EPUB input
            base = posixpath.dirname(resolve(nav_href))
            for a in tree.iter():
                if opf_localname(a.tag) == "a" and a.get("href"):
                    tgt = posixpath.normpath(posixpath.join(base, a.get("href").split("#", 1)[0]))
                    labels.setdefault(tgt, "".join(a.itertext()).strip())
        elif ncx_href:
            tree = ET.fromstring(zf.read(resolve(ncx_href)))  # noqa: S314  # trusted local EPUB input
            base = posixpath.dirname(resolve(ncx_href))
            for nav in tree.iter():
                if opf_localname(nav.tag) != "navPoint":
                    continue
                label = ""
                href = None
                for sub in nav.iter():
                    ln = opf_localname(sub.tag)
                    if ln == "text" and not label:
                        label = (sub.text or "").strip()
                    elif ln == "content":
                        href = sub.get("src")
                if href:
                    tgt = posixpath.normpath(posixpath.join(base, href.split("#", 1)[0]))
                    labels.setdefault(tgt, label)
    except (ET.ParseError, KeyError):
        pass
    return labels


def compile_epub(path, max_image_edge=MAX_IMAGE_EDGE, skip_images=SKIP_IMAGES):
    sys.setrecursionlimit(100000)
    bb = BlobBuilder()
    with ZipFile(path) as zf:
        opf_dir, meta, manifest, spine, cover_id = parse_opf(zf)
        labels = parse_toc(zf, opf_dir, manifest)
        names = set(zf.namelist())

        def resolve(href):
            return posixpath.normpath(posixpath.join(opf_dir, href))

        # stylesheets (verbatim)
        for href, media, _props in manifest.values():
            if media == "text/css":
                full = resolve(href)
                if full in names:
                    bb.add_stylesheet(zf.read(full).decode("utf-8", "replace"))

        # images (raster -> 4bpp, svg -> vector); remember manifest-id -> image index
        id_to_image = {}
        for mid, (href, media, _props) in () if skip_images else manifest.items():
            full = resolve(href)
            if full not in names:
                continue
            try:
                if media == "image/svg+xml":
                    id_to_image[mid] = bb.add_svg_image(href, zf.read(full))
                elif media.startswith("image/"):
                    id_to_image[mid] = bb.add_raster_image(href, zf.read(full), max_image_edge)
            except (OSError, ValueError):
                pass
        if cover_id and cover_id in id_to_image:
            bb.cover_index = id_to_image[cover_id]

        # spine chapters -> faithful DOM (body subtree)
        href_by_id = {mid: h for mid, (h, _m, _p) in manifest.items()}
        for idref in spine:
            href = href_by_id.get(idref)
            if not href:
                continue
            full = resolve(href)
            if full not in names:
                continue
            dom = DomBuilder()
            dom.feed(zf.read(full).decode("utf-8", "replace"))
            body = find_first(dom.root, "body") or dom.root
            title = labels.get(full, "")
            bb.add_chapter(body, title, href)

    return bb.serialize(meta), meta, bb


def main():
    ap = argparse.ArgumentParser(description="Compile an EPUB into a .rabook blob.")
    ap.add_argument("input", help="source .epub")
    ap.add_argument("output", help="destination .rabook")
    ap.add_argument("--stats", action="store_true", help="print size/structure stats")
    ap.add_argument(
        "--max-edge",
        type=int,
        default=MAX_IMAGE_EDGE,
        help="downscale raster image long edge to at most this many pixels",
    )
    ap.add_argument(
        "--no-images",
        action="store_true",
        help="drop all images (text-only); tiny blob for a baked fixture",
    )
    ap.add_argument(
        "--chunk-bytes",
        type=int,
        default=CONTAINER_CHUNK_BYTES,
        help="inflated bytes per independently-compressed container chunk "
        "(must equal the reader's ra_vmem frame size)",
    )
    args = ap.parse_args()

    blob, meta, bb = compile_epub(args.input, args.max_edge, args.no_images)
    container = wrap_container(blob, args.chunk_bytes)
    with Path(args.output).open("wb") as fh:
        fh.write(container)

    if args.stats:
        src = Path(args.input).stat().st_size
        out = len(container)
        print(f"{meta['title']} -- {meta['author']}")
        print(
            f"  chapters={len(bb.chapters)} nodes={len(bb.nodes)} "
            f"attrs={len(bb.attrs)} css={len(bb.stylesheets)} images={len(bb.images)}"
        )
        print(
            f"  epub={src // 1024} KB -> rabook={out // 1024} KB "
            f"({100 * out // max(src, 1)}%); inflated={len(blob) // 1024} KB"
        )


if __name__ == "__main__":
    main()
