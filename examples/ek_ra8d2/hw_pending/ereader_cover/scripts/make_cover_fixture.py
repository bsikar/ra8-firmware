#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Generate epub_cover_fixture.h: a minimal EPUB3 with a real PNG cover.

The manifest declares the cover with `properties="cover-image"`, and the
on-device ereader_cover gate exercises the whole chain against it: open the
blob in memory with `epub_open()`, pull the cover bytes with
`epub_get_cover_image()`, decode, scale and blit with
`ra8_img_decode_blit()`, then CRC-gate the framebuffer.

The cover is a REAL RGB PNG, and that is the point of this fixture existing
alongside apps/shared_libs/epub/tests/src/test_epub.c. That test uses a 4-byte stand-in, which
exercises only the byte-copy path and never the decoder. Here stb_image has to
actually decode something.

Output is pure 7-bit ASCII -- a C array of the .epub bytes -- baked at 16 bytes
per row so clang-format leaves it byte-identical rather than reflowing it.

Changing the cover changes the framebuffer CRC. Re-run this, then read the CRC
the board prints and update hil.conf in the SAME change, or the gate fails on a
fixture that is perfectly correct.

Usage:
    python3 examples/ek_ra8d2/hw_pending/ereader_cover/scripts/make_cover_fixture.py
"""

from __future__ import annotations

import hashlib
import io
import zipfile
from pathlib import Path

# Canonical portrait cover: 96x144 (2:3, a real book-cover ratio), with four
# horizontal colour bands. These are the approved fixture's exact encoded PNG
# bytes. Freezing them avoids delegating reproducibility to a host image encoder
# or zlib version while still making stb_image decode a real RGB PNG.
COVER_PNG_SHA256 = "e15408252d202bc39e8b00286e730a07f4d410c3141c716c7e7747d8148f9a2f"
COVER_PNG = bytes.fromhex(
    "89504E470D0A1A0A0000000D49484452000000600000009008020000007868F9760000014949444154789CEDD2411583"
    "40100541160539724402129018099182844842019480EDBACE9CFAFD717DF625CFD6975B0A642D080A040582024181A0"
    "405020281014080A040582024181A0405020281014080A040582024181A0405020281014080A040582024181A0405020"
    "28101408C6F63DF433B516040582024181A0405020281014080A040582024181A0405020281014080A040582024181A0"
    "405020281014080A040582024181A0405020281014080A04058202C1D8CF4B3F536B415020281014080A040582024181"
    "A0405020281014080A040582024181A0405020281014080A040582024181A0405020281014080A040582024181A04050"
    "2028108CFF6FD3CFD45A1014080A040582024181A0405020281014080A040582024181A0405020281014080A04058202"
    "4181A0405020281014080A040582024181A0405020281014080AB4BCBB012704058D7A1374B60000000049454E44AE42"
    "6082"
)


def make_cover_png() -> bytes:
    """Return the canonical 96x144 four-band portrait cover PNG.

    The 2:3 aspect is a real book-cover ratio, so the gate exercises the
    scaler's aspect handling rather than a convenient square.

    The digest check prevents a hand edit from silently changing the canonical
    image bytes and therefore the framebuffer CRC contract.

    Returns:
        The byte-identical canonical PNG.

    Raises:
        RuntimeError: The embedded PNG no longer has its approved digest.
    """
    actual_digest = hashlib.sha256(COVER_PNG).hexdigest()
    if actual_digest != COVER_PNG_SHA256:
        _error = f"cover PNG digest {actual_digest} != {COVER_PNG_SHA256}"
        raise RuntimeError(_error)
    return COVER_PNG


# --- Minimal EPUB3 parts (shape mirrors apps/shared_libs/epub/tests/src/test_epub.c) ---
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

    Reproducible byte for byte: every entry carries a fixed 2026-01-01 timestamp
    and fixed permissions, and every member is ZIP_STORED. No host compressor
    participates, so regenerating on a clean tree yields an identical header.

    `mimetype` is first and uncompressed as required by the EPUB spec. Storing
    the other four small members costs little and removes zlib-version drift.

    Returns:
        The complete .epub archive as bytes.
    """
    cover_png = make_cover_png()
    out = io.BytesIO()
    fixed = (2026, 1, 1, 0, 0, 0)

    def add(zf: zipfile.ZipFile, name: str, data: bytes | str) -> None:
        info = zipfile.ZipInfo(name, date_time=fixed)
        info.compress_type = zipfile.ZIP_STORED
        info.external_attr = 0o600 << 16
        zf.writestr(info, data)

    with zipfile.ZipFile(out, "w") as zf:
        # mimetype MUST be the first entry and stored (uncompressed).
        add(zf, "mimetype", b"application/epub+zip")
        add(zf, "META-INF/container.xml", CONTAINER_XML.encode("ascii"))
        add(zf, "OEBPS/content.opf", CONTENT_OPF.encode("ascii"))
        add(zf, "OEBPS/ch1.xhtml", CH1_XHTML.encode("ascii"))
        add(zf, "OEBPS/cover.png", cover_png)
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
        " * (epub_open), extracts the cover (epub_get_cover_image), and\n"
        " * decode+scale+blits it (ra8_img_decode_blit); the framebuffer hash in\n"
        " * hil.conf pins the result. Generated by examples/ek_ra8d2/hw_pending/\n"
        " * ereader_cover/scripts/make_cover_fixture.py. Pure ASCII.\n"
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
        f"enum : size_t {{ k_epub_cover_fixture_len = {len(epub)}U "
        "/**< EPUB cover fixture length. */ };\n"
        "\n"
        "/** @brief Baked cover-art EPUB3 byte stream. */\n"
        "static const uint8_t k_epub_cover_fixture[k_epub_cover_fixture_len] = {\n"
        f"{body}\n"
        "};\n"
    )


def main() -> int:
    """Regenerate the owning component's inc/epub_cover_fixture.h.

    Remember the two-step: this refreshes the fixture, but the framebuffer CRC
    in hil.conf is pinned separately and must be re-read from the board and
    updated in the same change.
    """
    epub = make_epub()
    header = bake_header(epub)
    output = Path(__file__).resolve().parent.parent / "inc" / "epub_cover_fixture.h"
    with output.open("w", encoding="ascii") as f:
        f.write(header)
    print(f"wrote {output} ({len(epub)} epub bytes)")


if __name__ == "__main__":
    main()
