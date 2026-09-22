# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""The compile pipeline: one EPUB in, one .rabook blob out.

Apart from the CLI so the selftest can drive the real pipeline rather than a
re-implementation of it -- a fixture suite that exercises its own copy of the
compiler proves nothing about the compiler that ships.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import posixpath
import sys
from pathlib import Path
from zipfile import ZipFile

from epub_dom import DomBuilder, find_first
from epub_package import parse_opf, parse_toc
from rabook_blob import MAX_IMAGE_EDGE, SKIP_IMAGES, BlobBuilder
from rabook_format import PIXFMT_GRAY4


def compile_epub(
    path: str | Path,
    max_image_edge: int = MAX_IMAGE_EDGE,
    skip_images: bool = SKIP_IMAGES,
    pixel_format: int = PIXFMT_GRAY4,
) -> tuple[bytes, dict[str, str], BlobBuilder]:
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
        pixel_format: Device-profile raster depth (issue #343): PIXFMT_GRAY4
            (the default 4bpp packing) or PIXFMT_GRAY8 (lossless 8bpp). Only the
            raster image arm reads it; SVG is unaffected.

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

        def resolve(href: str) -> str:
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
                    id_to_image[mid] = bb.add_raster_image(
                        href, zf.read(full), max_image_edge, pixel_format
                    )
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
