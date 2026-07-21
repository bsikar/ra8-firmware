"""BlobBuilder: everything the compiler has parsed, laid out as one flat blob.

The device executes this in place -- it never unzips, never parses XHTML, and
never allocates to walk a chapter. So the writer's whole job is to turn a tree
of Python objects into arrays of fixed-width records joined by indices, in the
exact layout libs/ra8_book/inc/ra8_book.h reads back.

Raster images are the one thing whose FORM changes: the panel is physically
4bpp grayscale, so images are transcoded here. Resolution is preserved by
default; downscaling is opt-in via --max-edge (issue #210).

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

import io
import struct
import zlib

from epub_dom import StringPool
from gray4_kernel import gray4_downscale, gray4_encode, gray4_output_dims
from PIL import Image
from rabook_format import (
    FORMAT_VERSION,
    GRAY_LEVELS,
    IMG_GRAY4,
    IMG_SVG,
    MAGIC,
    NIL,
    NODE_ELEMENT,
    NODE_TEXT,
    PALETTE_BYTES,
)

# The SE source files are trusted local input; some cover scans exceed Pillow's
# default decompression-bomb threshold, so lift it rather than warn.
Image.MAX_IMAGE_PIXELS = None

# noise defeats DEFLATE (the renderer can dither at draw time if desired).
MAX_IMAGE_EDGE = 0
# When true, drop all images (text-only). Yields a tiny inflated blob that fits
# in MRAM as a baked fixture -- used by the on-device reader demo.
SKIP_IMAGES = False


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
    def _pack_tables(self):
        """Pack the four fixed-width record tables: chapters, nodes, attrs, styles."""
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
        return chap, node, attr, style

    def _pack_images(self):
        """Pack the image table and its payload pool.

        The pool is built first so every image record can carry a resolved
        `data_off` into it; a record written before its payload is placed
        would point at an offset that does not exist yet.
        """
        pool = bytearray()
        records = []
        for id_off, w, h, fmt, data, raw in self.images:
            data_off = len(pool)
            pool += data
            records.append(
                struct.pack("<IHHBBHIII", id_off, w, h, fmt, 0, 0, data_off, len(data), raw)
            )
        return b"".join(records), pool

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
        chap, node, attr, style = self._pack_tables()
        image, pool = self._pack_images()

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
