# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Fixed-layout selftest (issue #196): a CBZ in EPUB clothing must survive intact.

A fixed-layout / image-only EPUB3 has no flowable text at all -- each spine
document is one full-page <img>. That content model exercises a different path
through the compiler than a text book does, and it is the path a manga library
depends on, so it is asserted end to end: the emitted .rabook has the
one-image-per-page shape, its 4bpp payload is byte-identical to the equivalent
CBZ, and the EPUB3 properties="cover-image" cover resolves.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import io
import posixpath
import sys
from pathlib import Path
from zipfile import ZipFile

from epub_pipeline import compile_epub
from rabook_blob import BlobBuilder
from rabook_format import IMG_GRAY4, NIL, NODE_ELEMENT, wrap_container

# --- fixed-layout selftest (issue #196) ---------------------------------------
# A fixed-layout / image-only EPUB3 is a CBZ in EPUB clothing: each spine
# document is one full-page <img> with no flowable text. This selftest proves
# that content model survives the compiler end to end -- the emitted .rabook has
# the one-image-per-page shape, its 4bpp payload is byte-identical to the
# equivalent CBZ, the EPUB3 properties="cover-image" cover is resolved, and it
# documents the img-src vs manifest-href resolution gap the render path must
# bridge. The fixture lives at tests/fixtures/rabook_fixed_layout/.
FIXED_LAYOUT_FIXTURE = ("tests", "fixtures", "rabook_fixed_layout")


def _fl_fail(message: str) -> None:
    """Print a failure to stderr and raise SystemExit(1) (no traceback)."""
    sys.stderr.write(f"epub_compile.py: {message}\n")
    raise SystemExit(1)


def _fl_require(cond: bool, what: str) -> None:
    """Exit non-zero unless `cond` holds (a checkable assert for the selftest)."""
    if not cond:
        _fl_fail(f"selftest FAILED: {what}")


def _fl_str(bb: BlobBuilder, off: int) -> str:
    """Resolve a string-pool offset in builder `bb` back to text."""
    raw = bb.sp.buf
    return bytes(raw[off : raw.index(b"\x00", off)]).decode("utf-8")


def _fl_find_image(bb: BlobBuilder, root_idx: int) -> tuple[str, dict[str, str]] | None:
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


def _fl_zip_dir(directory: Path) -> io.BytesIO:
    """Zip a directory tree in memory (sorted for determinism); return BytesIO."""
    buf = io.BytesIO()
    with ZipFile(buf, "w") as zf:
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                zf.writestr(str(path.relative_to(directory)), path.read_bytes())
    buf.seek(0)
    return buf


def _fl_build_equivalent_cbz(page_images: dict[str, bytes]) -> io.BytesIO:
    """Zip `{basename: png_bytes}` into a CBZ (flat image entries); return BytesIO."""
    buf = io.BytesIO()
    with ZipFile(buf, "w") as zf:
        for name, data in sorted(page_images.items()):
            zf.writestr(name, data)
    buf.seek(0)
    return buf


def _check_page_tables(bb: BlobBuilder, meta: dict[str, str]) -> None:
    """The one-image-per-page SHAPE: spine order, manifest ids, cover."""
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


def _check_page_elements(bb: BlobBuilder) -> None:
    """Each page's image element, and the img-src vs manifest-id gap.

    The gap is asserted in both directions on purpose: the verbatim `src` must
    NOT be a manifest id (cross-directory), and normalising it against the
    chapter's own directory must recover the id exactly. That pair is the
    bridge a book-backed image loader has to implement, so a change that made
    either half true by accident would be caught here.
    """
    image_ids = [_fl_str(bb, img[0]) for img in bb.images]
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


def selftest() -> int:
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

    _check_page_tables(bb, meta)
    _check_page_elements(bb)

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
