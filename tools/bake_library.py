#!/usr/bin/env python3
# Bake a set of compiled .rabook files into a C header of MRAM-resident byte
# arrays plus a lookup table. Each blob is the chunked RBKC container as
# produced by tools/epub_compile; the firmware inflates it into SDRAM on demand
# via ra8_book_open(). Full books (with cover + inline images) are kept compressed
# in MRAM and only expanded when opened, so several fit alongside the firmware.
#
# Each book's cover is also pre-decoded here into a gray8 thumbnail (matching the
# firmware's sh_image_decode_gray8 exactly) and embedded, so boot draws the shelf
# without inflating a single book -- inflation only happens when a book is opened.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Usage: bake_library.py <out.h> <rabook>|<title>|<author> [<rabook>|<title>|<author> ...]
import struct
import sys
import zlib
from pathlib import Path

# Shelf thumbnail box -- MUST match k_sh_thumb_w / k_sh_thumb_h in sh_app.h.
THUMB_W = 130
THUMB_H = 195

# Sentinel value in the .rabook header meaning "no cover image present".
COVER_ABSENT = 0xFFFFFFFF
# Maximum line length (characters) for emitted C array initializer rows.
ARRAY_LINE_LIMIT = 96
# Minimum number of argv entries: script, out.h, at least one spec.
MIN_ARGV_COUNT = 3


def unwrap_container(data):
    """Inflate a chunked RBKC .rabook container back to its flat blob.

    Keep in sync with ra8_book_container_t in libs/ra8_book/inc/ra8_book.h:
    "RBKC" + <I chunk_bytes + <Q total + <I count + <I reserved(0), a
    (count + 1)-entry <Q offset table, then count concatenated zlib streams.
    """
    if data[:4] != b"RBKC":
        msg = "not an RBKC container"
        raise ValueError(msg)
    chunk_bytes, total, count, reserved = struct.unpack_from("<IQII", data, 4)
    if reserved != 0 or chunk_bytes == 0 or count != (total + chunk_bytes - 1) // chunk_bytes:
        msg = "malformed RBKC header"
        raise ValueError(msg)
    offsets = struct.unpack_from(f"<{count + 1}Q", data, 24)
    payload = 24 + 8 * (count + 1)
    blob = b"".join(
        zlib.decompress(data[payload + offsets[i] : payload + offsets[i + 1]]) for i in range(count)
    )
    if len(blob) != total:
        msg = "RBKC inflated size mismatch"
        raise ValueError(msg)
    return blob


def decode_cover_thumb(blob):
    """Decode the book cover into a (gray8 bytes, w, h) thumbnail, or None.

    Mirrors sh_image_decode_gray8 / sh_fit_box / sh_gray4_at byte-for-byte so the
    baked thumbnail is identical to a runtime decode (keeps the render hash stable).
    """
    try:
        inflated = unwrap_container(blob)
    except ValueError:
        return None

    def u32(o):
        return struct.unpack_from("<I", inflated, o)[0]

    def u16(o):
        return struct.unpack_from("<H", inflated, o)[0]

    cover = u32(36)
    if cover == COVER_ABSENT:
        return None
    img = u32(76) + (cover * 24)
    src_w, src_h, fmt, data_off = u16(img + 4), u16(img + 6), inflated[img + 8], u32(img + 12)
    if fmt != 0 or src_w == 0 or src_h == 0:
        return None
    data = inflated[u32(88) + data_off :]
    fit_w = THUMB_W
    fit_h = (THUMB_W * src_h) // src_w
    if fit_h > THUMB_H:
        fit_h = THUMB_H
        fit_w = (THUMB_H * src_w) // src_h
    fit_w = max(1, fit_w)
    fit_h = max(1, fit_h)
    out = bytearray(fit_w * fit_h)
    for dy in range(fit_h):
        sy = (dy * src_h) // fit_h
        row = sy * src_w
        for dx in range(fit_w):
            flat = row + ((dx * src_w) // fit_w)
            byte = data[flat >> 1]
            nib = (byte & 0x0F) if (flat & 1) else (byte >> 4)
            out[(dy * fit_w) + dx] = (nib << 4) | nib
    return bytes(out), fit_w, fit_h


def emit_array(name, data):
    out = [f"alignas(4) static const uint8_t {name}[{len(data)}U] = {{"]
    line = "  "
    for b in data:
        line += f"{b}U,"
        if len(line) >= ARRAY_LINE_LIMIT:
            out.append(line)
            line = "  "
    if line.strip():
        out.append(line)
    out.append("};")
    return "\n".join(out)


def main(argv):
    if len(argv) < MIN_ARGV_COUNT:
        sys.stderr.write("usage: bake_library.py <out.h> <rabook>|<title>|<author> ...\n")
        return 2
    out_path = argv[1]
    books = []
    for spec in argv[2:]:
        path, title, author = spec.split("|")
        with Path(path).open("rb") as f:
            blob = f.read()
        books.append((blob, title, author, decode_cover_thumb(blob)))
    parts = [
        "/**",
        " * @file library.h",
        " * @generated tools/bake_library.py -- do not edit by hand.",
        " * @brief Baked full .rabook blobs + pre-decoded cover thumbnails (generated).",
        " * @details Each entry is the chunked RBKC container (ra8_book_open inflates it",
        " *          on demand) plus a gray8 cover thumbnail the shelf blits without any",
        " *          boot-time inflation. Regenerate with tools/bake_library.py (the",
        " *          thumbnail bytes are architecture-dependent to regenerate; see",
        " *          scripts/build_books.sh -- re-pin the fb golden when re-baking).",
        " *",
        " * @copyright Copyright (c) 2026 Brighton Sikarskie",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * @since Version 0.1.0",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "// NOLINTBEGIN(readability-magic-numbers)",
    ]
    names = []
    for i, (data, title, author, thumb) in enumerate(books):
        blob_name = f"k_lib_blob{i:02d}"
        parts.append(f'/** @brief "{title}" by {author} -- {len(data)} bytes. */')
        parts.append(emit_array(blob_name, data))
        thumb_name, tw, th = "nullptr", 0, 0
        if thumb is not None:
            tbytes, tw, th = thumb
            thumb_name = f"k_lib_thumb{i:02d}"
            parts.append(f"/** @brief Cover thumbnail for {blob_name} ({tw}x{th} gray8). */")
            parts.append(emit_array(thumb_name, tbytes))
        names.append((blob_name, len(data), title, author, thumb_name, tw, th))
        parts.append("")
    parts.append(
        "/** @brief One openable baked book: compressed blob + cover thumbnail + metadata. */"
    )
    parts.append("typedef struct {")
    parts.append("  const uint8_t* blob;     /**< RBKC container start.           */")
    parts.append("  uint32_t       len;      /**< Container length in bytes.      */")
    parts.append("  const uint8_t* thumb;    /**< gray8 cover thumbnail, or NULL. */")
    parts.append("  uint16_t       thumb_w;  /**< Thumbnail width in pixels.      */")
    parts.append("  uint16_t       thumb_h;  /**< Thumbnail height in pixels.     */")
    parts.append("  const char*    title;    /**< Display title.                  */")
    parts.append("  const char*    author;   /**< Display author.                 */")
    parts.append("} library_book_t;")
    parts.append("")
    parts.append("typedef enum : uint16_t {")
    parts.append(f"  k_library_count = {len(books)}U,")
    parts.append("} library_count_t;")
    parts.append("")
    parts.append("static const library_book_t k_library[k_library_count] = {")
    for blob_name, n, title, author, thumb_name, tw, th in names:
        t = title.replace('"', '\\"')
        a = author.replace('"', '\\"')
        parts.append(f'    {{ {blob_name}, {n}U, {thumb_name}, {tw}U, {th}U, "{t}", "{a}" }},')
    parts.append("};")
    parts.append("// NOLINTEND(readability-magic-numbers)")
    parts.append("")
    with Path(out_path).open("w") as f:
        f.write("\n".join(parts))
    sys.stderr.write(
        f"bake_library: wrote {out_path} ({len(books)} books, "
        f"{sum(len(d) for d, _, _, _ in books)} blob bytes + thumbnails)\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
