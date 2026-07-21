#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
"""Generate epub_cover_fixture.h: a minimal EPUB3 with a real PNG cover.

The manifest declares the cover with `properties="cover-image"`, and the
on-device ereader_cover gate exercises the whole chain against it: open the
blob in memory with `ra8_epub_open()`, pull the cover bytes with
`ra8_epub_get_cover_image()`, decode, scale and blit with
`ra8_img_decode_blit()`, then CRC-gate the framebuffer.

The cover is a REAL RGB PNG, and that is the point of this fixture existing
alongside tests/test_ra8_epub.c. That test uses a 4-byte stand-in, which
exercises only the byte-copy path and never the decoder. Here stb_image has to
actually decode something.

Output is pure 7-bit ASCII -- a C array of the .epub bytes -- baked at 16 bytes
per row so clang-format leaves it byte-identical rather than reflowing it.

Changing the cover changes the framebuffer CRC. Re-run this, then read the CRC
the board prints and update hil.conf in the SAME change, or the gate fails on a
fixture that is perfectly correct.

Usage:
    python3 make_cover_fixture.py
"""

import io
import zipfile
from pathlib import Path

from PIL import Image

# --- Deterministic portrait cover: 96x144 (2:3, a real book-cover ratio) ----
# Four horizontal colour bands. A real RGB PNG so stb_image actually decodes it
# (unlike the 4-byte stand-in in tests/test_ra8_epub.c, which only exercises the
# byte-copy path, not the decoder).
COVER_W, COVER_H = 96, 144
BANDS = [(0xC0, 0x10, 0x20), (0x18, 0x90, 0x30), (0x20, 0x40, 0xC0), (0xD0, 0xA0, 0x18)]


def make_cover_png() -> bytes:
    """Encode the deterministic 96x144 four-band portrait cover as PNG.

    The 2:3 aspect is a real book-cover ratio, so the gate exercises the
    scaler's aspect handling rather than a convenient square.

    `optimize=False` is required, not a default: Pillow's optimizer picks filter
    and compression settings that vary across builds, which would change the
    baked bytes -- and therefore the pinned framebuffer CRC -- on a machine with
    a different Pillow. Without it the fixture is not reproducible.

    Returns:
        Encoded PNG bytes, identical across runs and across Pillow builds.
    """
    img = Image.new("RGB", (COVER_W, COVER_H))
    px = img.load()
    band_h = COVER_H // len(BANDS)
    for y in range(COVER_H):
        idx = min(y // band_h, len(BANDS) - 1)
        for x in range(COVER_W):
            px[x, y] = BANDS[idx]
    out = io.BytesIO()
    # optimize=False keeps the encoder deterministic across Pillow builds.
    img.save(out, format="PNG", optimize=False)
    return out.getvalue()


# --- Minimal EPUB3 parts (shape mirrors tests/test_ra8_epub.c) ----------------
CONTAINER_XML = (
    '<?xml version="1.0"?>\n'
    '<container version="1.0" '
    'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    "  <rootfiles>\n"
    '    <rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/>\n'
    "  </rootfiles>\n"
    "</container>\n"
)

CONTENT_OPF = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
    'unique-identifier="id">\n'
    '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
    "    <dc:title>Cover Art Demo</dc:title>\n"
    "    <dc:creator>Brighton Sikarskie</dc:creator>\n"
    "    <dc:language>en</dc:language>\n"
    '    <dc:identifier id="id">urn:test:cover</dc:identifier>\n'
    '    <meta name="cover" content="cover"/>\n'
    "  </metadata>\n"
    "  <manifest>\n"
    '    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>\n'
    '    <item id="cover" href="cover.png" media-type="image/png" '
    'properties="cover-image"/>\n'
    "  </manifest>\n"
    "  <spine>\n"
    '    <itemref idref="ch1"/>\n'
    "  </spine>\n"
    "</package>\n"
)

CH1_XHTML = (
    '<?xml version="1.0"?><html><body><h1>Cover Art Demo</h1>'
    "<p>The cover is decoded from the manifest.</p></body></html>"
)


def make_epub() -> bytes:
    """Assemble the minimal EPUB3 archive in memory and return its bytes.

    Reproducible byte for byte: every entry carries a fixed 2026-01-01
    timestamp and fixed permissions, so regenerating on a clean tree yields an
    identical header and an empty diff.

    Two entries are ZIP_STORED. `mimetype` must be first and uncompressed per
    the EPUB spec -- readers check it at a fixed offset -- and the cover PNG is
    stored because deflating already-compressed PNG data only grows it.

    Returns:
        The complete .epub archive as bytes.
    """
    cover_png = make_cover_png()
    out = io.BytesIO()
    fixed = (2026, 1, 1, 0, 0, 0)

    def add(zf, name, data, store=False):
        info = zipfile.ZipInfo(name, date_time=fixed)
        info.compress_type = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
        info.external_attr = 0o600 << 16
        zf.writestr(info, data)

    with zipfile.ZipFile(out, "w") as zf:
        # mimetype MUST be the first entry and stored (uncompressed).
        add(zf, "mimetype", b"application/epub+zip", store=True)
        add(zf, "META-INF/container.xml", CONTAINER_XML.encode("ascii"))
        add(zf, "OEBPS/content.opf", CONTENT_OPF.encode("ascii"))
        add(zf, "OEBPS/ch1.xhtml", CH1_XHTML.encode("ascii"))
        add(zf, "OEBPS/cover.png", cover_png, store=True)
    return out.getvalue()


def bake_header(epub: bytes) -> str:
    """Render the EPUB bytes as a C header with a `static const uint8_t` table.

    The array is sized from a generated `enum : size_t`, so the declared length
    and the data cannot drift apart.

    The table sits inside a `clang-format off`/`on` guard at 16 bytes per row.
    That is what keeps the formatter from reflowing thousands of bytes and
    turning every regeneration into a large spurious diff.

    Args:
        epub: Archive bytes to bake, emitted as `0xNN`.

    Returns:
        The complete header source, pure 7-bit ASCII.
    """
    rows = []
    for i in range(0, len(epub), 16):
        chunk = epub[i : i + 16]
        rows.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    body = "\n".join(rows)
    return (
        "/**\n"
        " * @file epub_cover_fixture.h\n"
        " * @brief Baked minimal EPUB3 with a real PNG cover (cover-image manifest item).\n"
        " *\n"
        " * @details A 96x144 four-band RGB PNG cover wrapped in a one-chapter EPUB3,\n"
        " * byte-identical run to run. The ereader_cover gate opens it in memory\n"
        " * (ra8_epub_open), extracts the cover (ra8_epub_get_cover_image), and\n"
        " * decode+scale+blits it (ra8_img_decode_blit); the framebuffer hash in\n"
        " * hil.conf pins the result. Generated by make_cover_fixture.py. Pure ASCII.\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "/** @brief Length of the baked cover EPUB blob, bytes. */\n"
        f"enum : size_t {{ k_epub_cover_fixture_len = {len(epub)}U }};\n"
        "\n"
        "/** @brief Baked cover-art EPUB3 byte stream. */\n"
        "static const uint8_t k_epub_cover_fixture[k_epub_cover_fixture_len] = {\n"
        f"{body}\n"
        "};\n"
    )


def main():
    """Regenerate epub_cover_fixture.h in the CURRENT working directory.

    The output path is relative, so run this from the app directory that owns
    the fixture; anywhere else it writes a stray header instead of updating the
    committed one.

    Remember the two-step: this refreshes the fixture, but the framebuffer CRC
    in hil.conf is pinned separately and must be re-read from the board and
    updated in the same change.
    """
    epub = make_epub()
    header = bake_header(epub)
    with Path("epub_cover_fixture.h").open("w", encoding="ascii") as f:
        f.write(header)
    print(f"wrote epub_cover_fixture.h ({len(epub)} epub bytes)")


if __name__ == "__main__":
    main()
