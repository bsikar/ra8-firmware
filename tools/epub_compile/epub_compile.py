#!/usr/bin/env python3
"""Compile an EPUB into a flat, execute-in-place .rabook blob.

The on-device reader (libs/ra8_book) never unzips or parses XHTML at runtime.
This host tool does it once: it unzips the EPUB, parses every spine document
into a faithful DOM (every tag, attribute and text run preserved), keeps each
stylesheet verbatim, transcodes raster images to the panel-native 4bpp
grayscale at source resolution (downscale is an opt-in --max-edge knob;
issue #210) and preserves SVG as
vector source, then serializes everything into the binary layout described by
libs/ra8_book/inc/ra8_book.h.

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

from gray4_kernel import gray4_downscale, gray4_encode, gray4_output_dims
from PIL import Image

# The SE source files are trusted local input; some cover scans exceed Pillow's
# default decompression-bomb threshold, so lift it rather than warn.
Image.MAX_IMAGE_PIXELS = None

# --- on-disk constants, kept in lockstep with libs/ra8_book/inc/ra8_book.h ------
MAGIC = b"RABOOK1\x00"
FORMAT_VERSION = 1
NIL = 0xFFFFFFFF
NODE_ELEMENT = 0
NODE_TEXT = 1
IMG_GRAY4 = 0
IMG_SVG = 1
# Header feature-flag bits (ra8_book_flag_t). The firmware validator rejects any
# bit outside its known mask, so only emit bits defined there.
FLAG_RTL = 0x00000001
GRAY_LEVELS = 16
# Full 256-entry RGB palette = 256 * 3 channels.
PALETTE_BYTES = 768
# .rabook chunked container ("RBKC"; keep in sync with ra8_book_container_t in
# libs/ra8_book/inc/ra8_book.h):
#   "RBKC" + <I chunk_bytes + <Q inflated_total + <I chunk_count + <I reserved(0)
#   + <Q offset[chunk_count + 1] (payload-relative stream offsets)
#   + chunk_count concatenated zlib streams, one per chunk_bytes slice of the
#     flat blob (last slice short).
# Every chunk inflates independently, so the device can either inflate all of
# them into SDRAM (resident open) or inflate single chunks on demand into
# ra8_vmem cache frames (multi-GB books). chunk_bytes must equal the reader's
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


# Downscale is OPT-IN (owner decision, issue #210): the default preserves the
# source resolution because any compile-time pixel loss is unrecoverable at
# zoom time (the planned press-and-hold loupe re-magnifies small manga text).
# 0 means no clamp. Pass --max-edge N to opt into a long-edge clamp where the
# smaller blob is worth it (e.g. TFT-class baked fixtures -- see
# scripts/build_books.sh). FS dithering stays off because its high-frequency
# noise defeats DEFLATE (the renderer can dither at draw time if desired).
MAX_IMAGE_EDGE = 0
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
        """Start an empty pool and intern "" so offset 0 is the empty string.

        That first intern is load-bearing, not a convenience: the blob format
        uses offset 0 as its "no string" sentinel (an absent title, a text node
        on an element record). Constructing the pool without it would put real
        text at offset 0 and make every such sentinel read back as that text.
        """
        self.buf = bytearray()
        self._map = {}
        self.intern("")

    def intern(self, text):
        """Return the pool offset of `text`, appending it only if new.

        De-duplication is by exact UTF-8 bytes, so two strings that differ only
        in Unicode normalisation get separate slots. `None` is folded to "" and
        therefore always yields offset 0, which lets callers pass a missing
        metadata field straight through without a guard.

        Offsets are stable for the life of the pool: entries are only appended,
        never moved or removed. Callers may hold an offset across later
        `intern()` calls -- but NOT across a snapshot of `buf`, which is why
        `BlobBuilder.serialize()` interns the metadata strings before it copies
        the buffer.

        Args:
            text: String to intern, or None for the empty string.

        Returns:
            Byte offset of the NUL-terminated UTF-8 copy within `buf`.
        """
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
        """Seed the tree with a synthetic `#root` element and open the stack.

        The sentinel root exists so `handle_data`/`handle_starttag` never have
        to special-case an empty stack: a document with leading text, or one
        with several top-level elements, still has somewhere to attach. It is
        also the fallback chapter root when `compile_epub` finds no `<body>`.
        """
        super().__init__(convert_charrefs=True)
        self.root = {"tag": "#root", "attrs": [], "children": []}
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        """Append an element to the current parent and descend into it.

        Void tags (`<br>`, `<img>`, ...) are appended but NOT pushed, because
        XHTML in the wild spells them both `<br>` and `<br/>`; pushing them
        would leave the stack permanently deeper on the unclosed spelling and
        nest every following sibling inside the void element.

        Args:
            tag: Lowercased tag name from HTMLParser.
            attrs: HTMLParser's (name, value) pairs, preserved verbatim -- order
                and duplicates included, since the blob stores them as written.
        """
        node = {"tag": tag, "attrs": attrs, "children": []}
        self.stack[-1]["children"].append(node)
        if tag not in VOID_TAGS:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        """Append a self-closing element (`<foo/>`) without descending.

        HTMLParser routes the self-closed spelling here instead of through
        `handle_starttag` + `handle_endtag`, so this must not push the stack.
        It applies to any tag, not just the void set -- `<div/>` and inline
        `<svg><path/></svg>` both arrive here.

        Args:
            tag: Lowercased tag name.
            attrs: (name, value) pairs, preserved verbatim.
        """
        self.stack[-1]["children"].append({"tag": tag, "attrs": attrs, "children": []})

    def handle_endtag(self, tag):
        """Close the innermost open element with this name.

        Any elements left open inside it are discarded from the stack.
        Scanning inward rather than popping one frame lets malformed markup
        (`<p><b>text</p>`) close correctly: the unclosed `<b>` is dropped from
        the stack along with `<p>`, but its already-attached subtree survives in
        the tree. A stray end tag with no matching open element is ignored
        rather than raising -- index 0 is excluded from the scan so the
        synthetic `#root` can never be popped.

        Args:
            tag: Lowercased tag name being closed.
        """
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i]["tag"] == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        """Attach a text run to the current parent, dropping only empty runs.

        Whitespace is deliberately kept: the device-side renderer performs its
        own CSS whitespace collapsing, and stripping here would destroy the
        single significant space between two inline elements. Because
        `convert_charrefs=True`, entities have already been resolved to Unicode
        by the time this is called, and one logical text run may still arrive
        split across several calls -- each becomes its own text node, which the
        renderer treats identically to one merged node.

        Args:
            data: Decoded character data.
        """
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
        """Create empty tables with no cover and no feature flags set.

        `cover_index` starts at NIL rather than 0 because 0 is a valid image
        index; a book whose cover is never resolved must serialize as "no
        cover", not as "the first image". `flags` starts clear because the
        firmware validator rejects any bit outside the mask it knows, so bits
        are only ever set deliberately by a caller.
        """
        self.sp = StringPool()
        self.nodes = []  # list of dicts (see _emit_node)
        self.attrs = []  # list of (name_off, value_off)
        self.chapters = []  # list of (title_off, href_off, root_node)
        self.stylesheets = []  # list of (source_off, scope_chapter)
        self.images = []  # list of (id_off, w, h, fmt, data, raw_size)
        self.cover_index = NIL
        self.flags = 0  # ra8_book_flag_t bits (e.g. FLAG_RTL); 0 for EPUB text

    # -- DOM serialization ----------------------------------------------------
    def add_text(self, text):
        """Append a text node and return its index in the node table.

        The node is emitted with no children, no attributes and no sibling
        link: `add_element` owns stitching `next_sibling` once it knows the full
        child list. Node indices are dense and assigned in creation order, and a
        node's index is final once returned.

        Args:
            text: Character data; interned, so repeated runs cost one pool slot.

        Returns:
            Index of the new node in `self.nodes`.
        """
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
        """Flatten a DomBuilder subtree into the node/attr tables, depth first.

        The element's own record is reserved before its children are walked, so
        a parent always has a lower index than its descendants -- the device
        reader relies on that ordering to walk the tree without a stack.
        Attributes of one element are written contiguously, which is why the
        record stores only `first_attr` plus a count.

        Recursion depth tracks markup nesting depth, so `compile_epub` raises
        the interpreter recursion limit before calling this; a deeply nested
        document would otherwise die with RecursionError rather than a
        diagnostic.

        Args:
            elem: DomBuilder node dict with "tag", "attrs" and "children".

        Returns:
            Index of the element's own node record.
        """
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
        """Flatten one spine document and register it as the next chapter.

        Chapters are stored in call order, and that order IS the reading order
        the device presents -- callers must therefore iterate the OPF spine, not
        the manifest (whose order is arbitrary).

        Args:
            root_elem: Subtree to flatten, normally the document's `<body>`.
            title: TOC label, or "" when the TOC has no entry for this
                document. An untitled chapter is legitimate and displays as
                blank rather than being skipped.
            href: Manifest-relative href, kept so the reader can resolve
                intra-book links back to a chapter index.
        """
        root_idx = self.add_element(root_elem)
        self.chapters.append((self.sp.intern(title), self.sp.intern(href), root_idx))

    # -- assets ---------------------------------------------------------------
    def add_stylesheet(self, css_text):
        """Store a stylesheet verbatim, scoped to the whole book.

        The source text is kept uninterpreted -- no minification, no parsing,
        no dropping of rules the device renderer does not implement yet. That
        is the fidelity rule: a rule the renderer learns later must not need the
        book recompiled.

        The scope field is written as NIL (book-wide) rather than a chapter
        index. EPUB per-document `<link rel="stylesheet">` scoping is not
        modelled; every sheet applies everywhere, so two documents with
        conflicting sheets will both see both.

        Args:
            css_text: Stylesheet source, already decoded to text.
        """
        self.stylesheets.append((self.sp.intern(css_text), NIL))

    def add_raster_image(self, href, data, max_image_edge=MAX_IMAGE_EDGE):
        """Transcode an encoded raster image to panel-native 4bpp and store it.

        This is the one place content changes form, because the e-ink panel is
        physically 4bpp. Colour is flattened to luminance, quantized to 16 grey
        levels with dithering OFF (dithered noise survives neither the panel's
        own dynamics nor a later downscale), and packed two pixels per byte,
        high nibble first. An odd width leaves the final byte's low nibble zero.

        Two quantization paths exist and they are NOT interchangeable:

        - Downscaling (`max_image_edge` non-zero) resamples and packs with
          `gray4_kernel`, the exact integer kernel `ra8_rabook_gray4_*` runs on
          device. That keeps a downscaled image byte-identical host-vs-device
          (issue #213) and stable across Pillow versions.
        - The default no-downscale path uses Pillow's palette quantize. It is
          byte-frozen: goldens in this tree hash its output, so changing it
          changes committed fixtures.

        Pixels are stored uncompressed. The container DEFLATEs the whole blob
        once, so per-image compression would only double-compress, and after the
        single inflate-on-open these bytes are handed to the panel with no
        decode step at all.

        Args:
            href: Manifest href, interned as the lookup id the DOM's `<img
                src>` is matched against.
            data: Encoded source bytes in any format Pillow opens.
            max_image_edge: Opt-in long-edge cap in pixels (issue #210). 0 --
                the default -- preserves source resolution, which is what makes
                the manga zoom loupe possible.

        Returns:
            Index of the image in `self.images`, for `cover_index` or a DOM ref.

        Raises:
            OSError: `data` is not a decodable image.
            ValueError: Pillow rejected the decoded image.
        """
        im = Image.open(io.BytesIO(data)).convert("L")
        w, h = im.size
        if max_image_edge:
            ow, oh = gray4_output_dims(w, h, max_image_edge)
            if (ow, oh) != (w, h):
                # Opt-in downscale (issue #210): resample AND quantize/pack with the
                # exact integer kernel the device runs (gray4_kernel mirrors
                # ra8_rabook_gray4_*) -- NOT PIL LANCZOS + PIL palette-snap -- so a
                # downscaled image is byte-identical host-vs-device (issue #213) and
                # deterministic across Pillow versions. The default (no-downscale)
                # path below is byte-for-byte untouched.
                scaled = gray4_downscale(im.tobytes(), w, h, ow, oh)
                raw = gray4_encode(scaled, ow, oh)
                self.images.append((self.sp.intern(href), ow, oh, IMG_GRAY4, raw, len(raw)))
                return len(self.images) - 1
        width, height = w, h
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
        """Store SVG source unchanged, as vector data the device rasterizes.

        Unlike `add_raster_image` this does not transcode: SVG stays vector so
        it can be rendered at whatever size the reflowed layout gives it. The
        bytes are not parsed or validated here either, so a malformed SVG
        reaches the device and is rejected there rather than at compile time.

        Width and height are written as 0 because an SVG has no fixed pixel
        size; the renderer takes its dimensions from the document's own
        viewBox. Consumers must branch on the IMG_SVG format tag before trusting
        the dimension fields.

        Args:
            href: Manifest href, interned as the image's lookup id.
            data: Raw SVG bytes, stored verbatim.

        Returns:
            Index of the image in `self.images`.
        """
        self.images.append((self.sp.intern(href), 0, 0, IMG_SVG, data, len(data)))
        return len(self.images) - 1

    # -- serialization --------------------------------------------------------
    def serialize(self, meta):
        """Pack every table into the final RABOOK1 blob and return its bytes.

        Sections are laid out in a fixed order -- chapters, nodes, attrs,
        styles, images, string pool, image pool -- each section's offset
        computed from the running total, and all of it described by a 100-byte
        header. The image pool is built before the header so the image records
        can carry resolved payload offsets. The trailing CRC32 covers the body
        only, never the header, since the header holds the CRC field itself.

        Order matters in one non-obvious way: the four metadata strings are
        interned BEFORE `self.sp.buf` is snapshotted. They frequently appear
        nowhere in the DOM, so interning them later -- during the header pack --
        would append past the captured copy and leave the header pointing at
        offsets that do not exist in the emitted blob.

        The result is the inflated blob. `wrap_container` applies the chunked
        RBKC compression that actually goes on disk.

        Args:
            meta: Dict with "title", "author", "language" and "identifier";
                all four must be present, and "" is the valid "unknown" value.

        Returns:
            Complete little-endian blob: 100-byte header followed by the body.
        """
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
            self.flags,
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
    """Strip the `{namespace}` prefix ElementTree prepends to a tag name.

    OPF, NCX and XHTML documents all declare namespaces, and publishers use
    different prefixes and even different namespace URIs for the same vocabulary.
    Matching on the local name alone is what lets the parsers below accept real
    EPUBs instead of only spec-perfect ones.

    Args:
        tag: An ElementTree tag, namespaced or not. None and "" pass through.

    Returns:
        The local name, or `tag` unchanged when it carries no namespace.
    """
    return tag.split("}", 1)[1] if tag and tag[0] == "{" else tag


def parse_opf(zf):
    """Read container.xml and the OPF package it points at.

    Walks the OPF once with `iter()` rather than following its schema, so
    elements in unexpected parents are still found. Metadata fields take the
    FIRST value seen and ignore later ones, which is how a book with several
    `<dc:creator>` entries yields one author.

    Cover resolution follows the device's precedence exactly: an EPUB3 manifest
    item whose `properties` contains "cover-image" wins, and the legacy EPUB2
    `<meta name="cover">` is only the fallback. The substring test mirrors
    `ra8_epub_xml_shim.cpp` `find_cover_by_properties()` byte for byte, so the
    host `.rabook` and an on-device compile agree on the cover -- which matters
    for EPUB3-only fixed-layout comics that ship no legacy meta (issue #196).

    Args:
        zf: Open ZipFile for the EPUB.

    Returns:
        Tuple of (opf_dir, meta, manifest, spine, cover_id): the OPF's directory
        for resolving relative hrefs, the four metadata strings (each "" if
        absent), manifest id -> (href, media_type, properties), spine idrefs in
        reading order, and the cover's manifest id or None.

    Raises:
        KeyError: The EPUB has no META-INF/container.xml.
        xml.etree.ElementTree.ParseError: container.xml or the OPF is not
            well-formed XML.
    """
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
    cover_id_meta = None  # legacy EPUB2 <meta name="cover" content="ID">
    cover_id_props = None  # EPUB3 manifest <item properties="cover-image">
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
            cover_id_meta = el.get("content")
        elif tag == "item":
            props = el.get("properties", "") or ""
            manifest[el.get("id")] = (el.get("href"), el.get("media-type", ""), props)
            # First manifest item carrying the EPUB3 cover-image property. The
            # substring test mirrors ra8_epub_xml_shim.cpp find_cover_by_properties()
            # (a strstr over the space-separated properties list) byte-for-byte, so
            # an EPUB3-only book -- no legacy meta, how modern fixed-layout comics
            # ship (issue #196) -- resolves the same cover host-side and on-device.
            if (cover_id_props is None) and ("cover-image" in props):
                cover_id_props = el.get("id")
        elif tag == "itemref":
            spine.append(el.get("idref"))
    # EPUB3 properties="cover-image" wins; the legacy <meta name="cover"> is the
    # fallback -- the exact precedence ra8_epub_xml_shim.cpp uses on device
    # (find_cover_by_properties() then find_cover_by_meta()), so the desktop
    # .rabook and the on-device compile agree on the cover image index.
    cover_id = cover_id_props if cover_id_props is not None else cover_id_meta
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
    """Compile one EPUB into an inflated .rabook blob.

    Assembles in dependency order -- stylesheets, then images (so the cover
    index is known), then spine chapters -- and takes each chapter's `<body>`
    subtree, falling back to the whole document when there is none.

    The pass is deliberately lenient, because a book that fails to open is worse
    than a book missing one asset. A manifest entry absent from the zip is
    skipped; an image Pillow cannot decode is skipped; a spine idref with no
    manifest entry is skipped. All four are silent. The consequence worth
    knowing at 2am: a corrupt EPUB compiles successfully into a blob with
    missing content rather than reporting an error.

    Recursion limit: `add_element` recurses per level of markup nesting, so the
    limit is raised to 100000 here. This mutates interpreter global state and is
    not restored.

    Args:
        path: Path to the source .epub.
        max_image_edge: Long-edge downscale cap in pixels; 0 preserves source
            resolution. See `BlobBuilder.add_raster_image` for why the two
            paths are not byte-equivalent.
        skip_images: Drop every image, producing a text-only blob small enough
            to bake into MRAM as a fixture. The cover is dropped too.

    Returns:
        Tuple of (blob, meta, bb): the serialized inflated blob, the metadata
        dict, and the BlobBuilder itself so callers can report table sizes.

    Raises:
        KeyError: Malformed EPUB with no container.xml.
        xml.etree.ElementTree.ParseError: container.xml or the OPF is not
            well-formed. A broken TOC is tolerated; a broken OPF is not.
        zipfile.BadZipFile: `path` is not a zip archive.
    """
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


# --- fixed-layout selftest (issue #196) ---------------------------------------
# A fixed-layout / image-only EPUB3 is a CBZ in EPUB clothing: each spine
# document is one full-page <img> with no flowable text. This selftest proves
# that content model survives the compiler end to end -- the emitted .rabook has
# the one-image-per-page shape, its 4bpp payload is byte-identical to the
# equivalent CBZ, the EPUB3 properties="cover-image" cover is resolved, and it
# documents the img-src vs manifest-href resolution gap the render path must
# bridge. The fixture lives at tests/fixtures/rabook_fixed_layout/.
FIXED_LAYOUT_FIXTURE = ("tests", "fixtures", "rabook_fixed_layout")


def _fl_fail(message):
    """Print a failure to stderr and raise SystemExit(1) (no traceback)."""
    sys.stderr.write(f"epub_compile.py: {message}\n")
    raise SystemExit(1)


def _fl_require(cond, what):
    """Exit non-zero unless `cond` holds (a checkable assert for the selftest)."""
    if not cond:
        _fl_fail(f"selftest FAILED: {what}")


def _fl_str(bb, off):
    """Resolve a string-pool offset in builder `bb` back to text."""
    raw = bb.sp.buf
    return bytes(raw[off : raw.index(b"\x00", off)]).decode("utf-8")


def _fl_find_image(bb, root_idx):
    """DFS a chapter DOM for the first <img>/<image>; return (tag, attrs) or None.

    Handles both the bare <img src> page and the <svg><image xlink:href> wrapper
    page: the tag name distinguishes them, and either src or xlink:href carries
    the raster reference.
    """
    stack = [root_idx]
    while stack:
        idx = stack.pop()
        if idx == NIL:
            continue
        node = bb.nodes[idx]
        if node["kind"] == NODE_ELEMENT:
            tag = _fl_str(bb, node["name_off"])
            if tag in ("img", "image"):
                attrs = {}
                for k in range(node["attr_count"]):
                    name_off, val_off = bb.attrs[node["first_attr"] + k]
                    attrs[_fl_str(bb, name_off)] = _fl_str(bb, val_off)
                return tag, attrs
        stack.append(node["next_sibling"])
        stack.append(node["first_child"])
    return None


def _fl_zip_dir(directory):
    """Zip a directory tree in memory (sorted for determinism); return BytesIO."""
    buf = io.BytesIO()
    with ZipFile(buf, "w") as zf:
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                zf.writestr(str(path.relative_to(directory)), path.read_bytes())
    buf.seek(0)
    return buf


def _fl_build_equivalent_cbz(page_images):
    """Zip `{basename: png_bytes}` into a CBZ (flat image entries); return BytesIO."""
    buf = io.BytesIO()
    with ZipFile(buf, "w") as zf:
        for name, data in sorted(page_images.items()):
            zf.writestr(name, data)
    buf.seek(0)
    return buf


def selftest():
    """Compile the committed fixed-layout fixture and check the #196 contract.

    Zips tests/fixtures/rabook_fixed_layout/ in memory, compiles it, then:
      * asserts the one-image-per-page shape (one spine chapter per page, each a
        <body> holding a single <img>/<image>, one manifest raster per page);
      * asserts the EPUB3 properties="cover-image" cover resolved to page 1;
      * builds an equivalent CBZ from the SAME page rasters, compiles it with
        cbz_compile, and asserts the 4bpp image payloads are byte-identical
        EPUB-vs-CBZ (so an image that the render path can find draws the same
        pixels either way);
      * documents the img-src gap: the verbatim <img src> does NOT equal the
        manifest image id (cross-directory), but normalising it against the
        chapter path does -- the exact bridge a book-backed image loader needs.
    Exits non-zero on the first failed check.
    """
    root = Path(__file__).resolve().parents[2]
    fixture = root.joinpath(*FIXED_LAYOUT_FIXTURE)
    _fl_require(fixture.is_dir(), f"fixture missing at {fixture}")

    blob, meta, bb = compile_epub(_fl_zip_dir(fixture))
    container = wrap_container(blob)

    # Round-trip the container so the whole on-disk path is exercised, not just
    # the flat blob (mirrors cbz_compile's selftest).
    from cbz_compile import compile_cbz, unwrap_container  # noqa: PLC0415  # avoid import cycle

    _fl_require(unwrap_container(container) == blob, "RBKC container round-trip")

    want_pages = ["text/page1.xhtml", "text/page2.xhtml", "text/page3.xhtml"]
    _fl_require(meta["title"] == "Fixed Layout Spike (#196)", "title interned")
    _fl_require(bb.flags == 0, "no stray header flags on a fixed-layout book")
    _fl_require(len(bb.chapters) == len(want_pages), "one chapter per spine page")
    _fl_require(len(bb.images) == len(want_pages), "one manifest raster per page")
    _fl_require(bb.cover_index == 0, "EPUB3 properties=cover-image resolved to page 1")

    hrefs = [_fl_str(bb, href_off) for (_t, href_off, _r) in bb.chapters]
    _fl_require(hrefs == want_pages, f"spine order preserved (got {hrefs})")

    image_ids = [_fl_str(bb, img[0]) for img in bb.images]
    _fl_require(
        image_ids == ["images/page1.png", "images/page2.png", "images/page3.png"],
        f"manifest image ids (got {image_ids})",
    )

    want_tags = ["img", "img", "image"]  # p1 bare img, p2 bare img, p3 svg<image>
    for i, (_title_off, href_off, root_idx) in enumerate(bb.chapters):
        root_node = bb.nodes[root_idx]
        _fl_require(_fl_str(bb, root_node["name_off"]) == "body", f"chapter root is body [{i}]")
        hit = _fl_find_image(bb, root_idx)
        _fl_require(hit is not None, f"chapter has an image element [{i}]")
        tag, attrs = hit
        _fl_require(tag == want_tags[i], f"page {i} image tag is <{want_tags[i]}> (got <{tag}>)")
        src = attrs.get("src") or attrs.get("xlink:href")
        _fl_require(src is not None, f"image element carries a src/xlink:href [{i}]")
        # The gap: verbatim src != manifest image id, but normalising the src
        # against the chapter's own directory recovers the id exactly.
        chapter_dir = posixpath.dirname(_fl_str(bb, href_off))
        normalised = posixpath.normpath(posixpath.join(chapter_dir, src))
        _fl_require(src not in image_ids, f"verbatim src is NOT a manifest id [{i}] (the gap)")
        _fl_require(normalised in image_ids, f"normalised src recovers the manifest id [{i}]")

    # Equivalent CBZ from the same page rasters -> identical 4bpp payloads.
    page_names = [posixpath.basename(_fl_str(bb, img[0])) for img in bb.images]
    raw_pngs = {name: (fixture / "OEBPS" / "images" / name).read_bytes() for name in page_names}
    _blob_cbz, _meta_cbz, bb_cbz = compile_cbz(
        _fl_build_equivalent_cbz(raw_pngs), title="Fixed Layout Spike (#196)"
    )
    cbz_payload = {posixpath.basename(_fl_str(bb_cbz, img[0])): img for img in bb_cbz.images}
    for img in bb.images:
        name = posixpath.basename(_fl_str(bb, img[0]))
        _fl_require(name in cbz_payload, f"CBZ has a matching page for {name}")
        c_img = cbz_payload[name]
        _fl_require((img[1], img[2]) == (c_img[1], c_img[2]), f"same dims EPUB/CBZ [{name}]")
        _fl_require(img[3] == c_img[3] == IMG_GRAY4, f"both transcode to 4bpp [{name}]")
        _fl_require(img[4] == c_img[4], f"4bpp payload byte-identical EPUB/CBZ [{name}]")

    sys.stdout.write(
        "epub_compile.py selftest: PASS -- fixed-layout shape, EPUB3 cover, "
        "CBZ payload parity, and the img-src normalisation gap all verified.\n"
    )
    return 0


def main():
    """Parse the command line, compile, and write the container to disk.

    Two modes: `--selftest` runs the issue #196 fixed-layout self-check and
    ignores the positional arguments entirely, otherwise both input and output
    are required. The output written is the RBKC-wrapped container, not the raw
    blob -- `--chunk-bytes` must equal the reader's `ra8_vmem` frame size or the
    device cannot page the book.

    Errors are not caught here. A malformed EPUB surfaces as a traceback rather
    than a diagnostic; the exception type names the failing stage.

    Returns:
        0 on success. Non-zero exits arrive as SystemExit from argparse or the
        selftest, not through this return.
    """
    ap = argparse.ArgumentParser(description="Compile an EPUB into a .rabook blob.")
    ap.add_argument("input", nargs="?", help="source .epub")
    ap.add_argument("output", nargs="?", help="destination .rabook")
    ap.add_argument("--stats", action="store_true", help="print size/structure stats")
    ap.add_argument(
        "--max-edge",
        type=int,
        default=MAX_IMAGE_EDGE,
        help="opt-in: downscale raster image long edge to at most this many "
        "pixels (default 0 = preserve source resolution)",
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
        "(must equal the reader's ra8_vmem frame size)",
    )
    ap.add_argument(
        "--selftest",
        action="store_true",
        help="compile the fixed-layout fixture and run the #196 self-check, then exit",
    )
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.input or not args.output:
        ap.error("input and output are required unless --selftest")

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
