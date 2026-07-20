#!/usr/bin/env python3
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Generate epub_stress_fixture.h: a synthetic large-STRUCTURE EPUB3 that
# reproduces the on-device miniz+tinyxml2 shared-arena pressure of a big
# real-world book (#144 bug 1) without shipping a copyrighted 7 MB novel.
#
# The pool pressure during ra8_epub_open() comes from (a) miniz's central
# directory (proportional to the file COUNT) and (b) tinyxml2's DOM of the OPF
# (proportional to the number of manifest/spine items) -- NOT the total byte
# size. So this fixture packs many tiny files: 60 chapters (spine, under the
# k_ra8_epub_max_chapters=64 cap) + 60 extra manifest resources + an NCX with 60
# navPoints + a cover. ~120 archive entries / a ~20 KB OPF -- comparable to the
# 108-file, 41-chapter real book -- yet only tens of KB total, so it bakes into
# MRAM and opens in memory like epub_parse.
#
# Output is pure 7-bit ASCII, 16 bytes/row inside a clang-format-off guard.
#
# Usage:  python3 make_stress_fixture.py
import io
import zipfile
from pathlib import Path

N_CHAPTERS = 60  # spine length; must stay < k_ra8_epub_max_chapters (64)
N_RESOURCES = 60  # extra manifest-only files (push the archive file count up)

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


def build_opf() -> str:
    items = [
        '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
        '    <item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/>',
    ]
    spine = []
    for i in range(N_CHAPTERS):
        items.append(
            f'    <item id="ch{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>'
        )
        spine.append(f'    <itemref idref="ch{i}"/>')
    items.extend(
        f'    <item id="res{i}" href="res{i}.css" media-type="text/css"/>'
        for i in range(N_RESOURCES)
    )
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
        'unique-identifier="id">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Stress Structure Book</dc:title>\n"
        "    <dc:creator>Brighton Sikarskie</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="id">urn:test:stress</dc:identifier>\n'
        '    <meta name="cover" content="cover"/>\n'
        "  </metadata>\n"
        "  <manifest>\n" + "\n".join(items) + "\n  </manifest>\n"
        '  <spine toc="ncx">\n' + "\n".join(spine) + "\n  </spine>\n"
        "</package>\n"
    )


def build_ncx() -> str:
    points = [
        f'    <navPoint id="np{i}" playOrder="{i + 1}">\n'
        f"      <navLabel><text>Chapter {i + 1}</text></navLabel>\n"
        f'      <content src="ch{i}.xhtml"/>\n'
        "    </navPoint>"
        for i in range(N_CHAPTERS)
    ]
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">\n'
        '  <head><meta name="dtb:uid" content="urn:test:stress"/></head>\n'
        "  <docTitle><text>Stress Structure Book</text></docTitle>\n"
        "  <navMap>\n" + "\n".join(points) + "\n  </navMap>\n"
        "</ncx>\n"
    )


# A real 1x1 PNG (so cover resolution + a real decodable image are present).
# Hand-aligned PNG byte table, twelve bytes per row -- keep the rows intact.
# fmt: off
COVER_PNG = bytes(
    [
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
        0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
        0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    ]
)
# fmt: on


def build_epub() -> bytes:
    out = io.BytesIO()
    fixed = (2026, 1, 1, 0, 0, 0)

    def add(zf, name, data, store=False):
        info = zipfile.ZipInfo(name, date_time=fixed)
        info.compress_type = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
        info.external_attr = 0o600 << 16
        zf.writestr(info, data)

    with zipfile.ZipFile(out, "w") as zf:
        add(zf, "mimetype", b"application/epub+zip", store=True)
        add(zf, "META-INF/container.xml", CONTAINER_XML.encode("ascii"))
        add(zf, "OEBPS/content.opf", build_opf().encode("ascii"))
        add(zf, "OEBPS/toc.ncx", build_ncx().encode("ascii"))
        add(zf, "OEBPS/cover.png", COVER_PNG, store=True)
        for i in range(N_CHAPTERS):
            body = (
                f'<?xml version="1.0"?><html><body><h1>Chapter {i + 1}</h1>'
                f"<p>Body text for chapter {i + 1}.</p></body></html>"
            )
            add(zf, f"OEBPS/ch{i}.xhtml", body.encode("ascii"))
        for i in range(N_RESOURCES):
            add(zf, f"OEBPS/res{i}.css", f".c{i} {{ margin: 0; }}\n".encode("ascii"))
    return out.getvalue()


def bake_header(epub: bytes) -> str:
    rows = []
    for i in range(0, len(epub), 16):
        chunk = epub[i : i + 16]
        rows.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    body = "\n".join(rows)
    return (
        "/**\n"
        " * @file epub_stress_fixture.h\n"
        " * @brief Baked synthetic large-structure EPUB3 for the #144 pool-stress gate.\n"
        " *\n"
        f" * @details {N_CHAPTERS} chapters + {N_RESOURCES} manifest resources + an NCX\n"
        f" * with {N_CHAPTERS} navPoints + a cover -- ~{N_CHAPTERS + N_RESOURCES + 5} archive\n"
        " * entries / a large OPF, reproducing a big real book's miniz+tinyxml2 shared-\n"
        " * arena pressure in tens of KB. Pure ASCII.\n"
        " *\n"
        " * @generated by make_stress_fixture.py -- do not edit by hand; the generator\n"
        " *            owns this file's length (a baked byte table).\n"
        " *\n"
        " * @copyright Copyright (c) 2026 Brighton Sikarskie\n"
        " * SPDX-License-Identifier: MIT\n"
        " */\n"
        "#pragma once\n"
        "\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "/** @brief Length of the baked stress EPUB blob, bytes. */\n"
        f"enum : size_t {{ k_epub_stress_fixture_len = {len(epub)}U }};\n"
        "\n"
        "/** @brief Baked synthetic large-structure EPUB3 byte stream. */\n"
        "/* clang-format off */\n"
        "static const uint8_t k_epub_stress_fixture[k_epub_stress_fixture_len] = {\n"
        f"{body}\n"
        "};\n"
        "/* clang-format on */\n"
    )


def main():
    epub = build_epub()
    with Path("epub_stress_fixture.h").open("w", encoding="ascii") as f:
        f.write(bake_header(epub))
    print(f"wrote epub_stress_fixture.h ({len(epub)} epub bytes)")


if __name__ == "__main__":
    main()
