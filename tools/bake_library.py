#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
"""Bake compiled .rabook files into a C header of MRAM-resident byte arrays.

Each blob is embedded as the chunked RBKC container tools/epub_compile emits --
still compressed. The firmware inflates one into SDRAM only when
`ra8_book_open()` is called, which is what lets several full books (covers and
inline images included) sit alongside the firmware in MRAM at all.

Cover thumbnails are the reason this tool does more than embed bytes. Each
book's cover is pre-decoded here into a gray8 thumbnail, matching the firmware's
`sh_image_decode_gray8` exactly, so boot can draw the whole shelf without
inflating a single book. If the two decoders ever diverge, the shelf renders
differently before and after a book is opened.

The emitted thumbnail bytes are architecture-dependent to regenerate, so
re-baking moves the framebuffer golden -- see scripts/builders/books.sh, and re-pin
the golden in the same change.

Usage:
    bake_library.py <out.h> <rabook>|<title>|<author> [<rabook>|<title>|<author> ...]
"""

from __future__ import annotations

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


def unwrap_container(data: bytes) -> bytes:
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


def decode_cover_thumb(blob: bytes) -> tuple[bytes, int, int] | None:
    """Decode the book cover into a (gray8 bytes, w, h) thumbnail, or None.

    Mirrors sh_image_decode_gray8 / sh_fit_box / sh_gray4_at byte-for-byte so the
    baked thumbnail is identical to a runtime decode (keeps the render hash stable).
    """
    try:
        inflated = unwrap_container(blob)
    except ValueError:
        return None

    def u32(o: int) -> int:
        return struct.unpack_from("<I", inflated, o)[0]

    def u16(o: int) -> int:
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


def emit_array(name: str, data: bytes) -> str:
    """Render bytes as a 4-byte-aligned `static const uint8_t` C array.

    The `alignas(4)` is not cosmetic: the firmware casts into these blobs to
    read the container's 32-bit header fields, and an unaligned load faults on
    the target. Every byte is emitted with a `U` suffix and lines are wrapped
    near ARRAY_LINE_LIMIT characters -- a soft limit, checked after appending,
    so a line may overrun by one element's width.

    Output is deterministic for identical input, which is what lets the
    generated header be committed and diffed.

    Args:
        name: C identifier for the array; used unquoted and unvalidated.
        data: Bytes to emit. Empty input still yields a well-formed (if
            zero-length, and therefore not valid C) array.

    Returns:
        The declaration as a newline-joined string, no trailing newline.
    """
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


def _load_books(specs: list[str]) -> list[tuple[bytes, str, str, tuple | None]]:
    """Read every `<path>|<title>|<author>` spec into a blob + metadata tuple.

    The `|` separator is unescaped, so a title containing a pipe splits into
    the wrong fields and raises rather than silently baking a mangled shelf.
    """
    books = []
    for spec in specs:
        path, title, author = spec.split("|")
        with Path(path).open("rb") as f:
            blob = f.read()
        books.append((blob, title, author, decode_cover_thumb(blob)))
    return books


def _header_preamble() -> list[str]:
    """The generated-file banner, include guard and includes."""
    return [
        "/**",
        " * @file library.h",
        " * @generated tools/bake_library.py -- do not edit by hand.",
        " * @brief Baked full .rabook blobs + pre-decoded cover thumbnails (generated).",
        " * @details Each entry is the chunked RBKC container (ra8_book_open inflates it",
        " *          on demand) plus a gray8 cover thumbnail the shelf blits without any",
        " *          boot-time inflation. Regenerate with tools/bake_library.py (the",
        " *          thumbnail bytes are architecture-dependent to regenerate; see",
        " *          scripts/builders/books.sh -- re-pin the fb golden when re-baking).",
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


def _emit_book_arrays(
    books: list[tuple[bytes, str, str, tuple | None]],
) -> tuple[list[str], list[tuple]]:
    """Emit one byte array per blob (and per thumbnail), in shelf order.

    Returns ``(lines, names)``; ``names`` feeds the table below and carries the
    symbol names just emitted, so the two cannot disagree about what exists.
    """
    parts: list[str] = []
    names: list[tuple] = []
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
    return parts, names


def _emit_library_table(count: int, names: list[tuple]) -> list[str]:
    """Emit the library_book_t struct, the count enum and the shelf table.

    Command-line order is shelf order, and this preserves it.
    """
    parts = [
        "/** @brief One openable baked book: compressed blob + cover thumbnail + metadata. */",
        "typedef struct {",
        "  const uint8_t* blob;     /**< RBKC container start.           */",
        "  uint32_t       len;      /**< Container length in bytes.      */",
        "  const uint8_t* thumb;    /**< gray8 cover thumbnail, or NULL. */",
        "  uint16_t       thumb_w;  /**< Thumbnail width in pixels.      */",
        "  uint16_t       thumb_h;  /**< Thumbnail height in pixels.     */",
        "  const char*    title;    /**< Display title.                  */",
        "  const char*    author;   /**< Display author.                 */",
        "} library_book_t;",
        "",
        "typedef enum : uint16_t {",
        f"  k_library_count = {count}U,",
        "} library_count_t;",
        "",
        "static const library_book_t k_library[k_library_count] = {",
    ]
    for blob_name, n, title, author, thumb_name, tw, th in names:
        t = title.replace('"', '\\"')
        a = author.replace('"', '\\"')
        parts.append(f'    {{ {blob_name}, {n}U, {thumb_name}, {tw}U, {th}U, "{t}", "{a}" }},')
    parts.extend(["};", "// NOLINTEND(readability-magic-numbers)", ""])
    return parts


def main(argv: list[str]) -> int:
    """Bake every `<path>|<title>|<author>` spec into the output header.

    Title and author are passed on the command line rather than read from the
    blob because the baked shelf shows them before any book is inflated -- the
    metadata inside the container is not reachable without inflating it.

    The `|` separator is unescaped, so a title containing a pipe splits into the
    wrong fields and raises. Books appear in the header in command-line order,
    and that order is the shelf order.

    Args:
        argv: Full argument vector INCLUDING the program name at index 0.

    Returns:
        0 on success, 2 on a usage error.

    Raises:
        ValueError: A spec did not split into exactly three fields.
        OSError: A .rabook path could not be read, or the header not written.
    """
    if len(argv) < MIN_ARGV_COUNT:
        sys.stderr.write("usage: bake_library.py <out.h> <rabook>|<title>|<author> ...\n")
        return 2
    out_path = argv[1]
    books = _load_books(argv[2:])
    arrays, names = _emit_book_arrays(books)
    parts = [*_header_preamble(), *arrays, *_emit_library_table(len(books), names)]
    with Path(out_path).open("w") as f:
        f.write("\n".join(parts))
    sys.stderr.write(
        f"bake_library: wrote {out_path} ({len(books)} books, "
        f"{sum(len(d) for d, _, _, _ in books)} blob bytes + thumbnails)\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
