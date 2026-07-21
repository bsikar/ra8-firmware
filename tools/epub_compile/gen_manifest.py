#!/usr/bin/env python3
"""Generate libs/ra8_book/inc/ra8_book_library.h from compiled .rabook blobs.

Scans a directory of .rabook files, reads each one's title/author/language and
its on-disk and inflated sizes, and emits a C header exposing a static table
(`g_ra8_book_library[]`) so firmware can list the bundled library and size the
inflate scratch buffer with a single #include. ASCII-only output (titles are
transliterated) to satisfy the repository encoding policy.

@copyright Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import struct
import unicodedata
import zlib
from pathlib import Path


def ascii_only(text: str) -> str:
    """Fold text to 7-bit ASCII that is safe inside a C string literal.

    NFKD decomposition first, so an accented letter splits into a plain letter
    plus a combining mark and the letter survives the ASCII encode; encoding
    without it would drop the whole character. Anything still non-ASCII after
    that -- CJK, emoji, typographic dashes -- is dropped silently, so a title in
    a non-Latin script can come back empty. That is accepted: the repository
    encoding policy requires pure 7-bit ASCII in generated sources.

    Double quotes become apostrophes and backslashes are deleted, which is what
    keeps the result from terminating or escaping the surrounding C literal.
    Callers must still not wrap the result in anything but a plain "..." .

    Args:
        text: Arbitrary Unicode, typically a book title or author.

    Returns:
        ASCII-only text containing neither a double quote nor a backslash. May
        be shorter than the input, and may be empty.
    """
    norm = unicodedata.normalize("NFKD", text)
    out = norm.encode("ascii", "ignore").decode("ascii")
    return out.replace('"', "'").replace("\\", "")


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


def read_meta(path: Path) -> dict[str, object]:
    """Inflate one .rabook and extract the fields the manifest header needs.

    Reads the whole container into memory and inflates all of it just to reach
    the header and a few pool strings -- wasteful per file, but it is the only
    way to learn the inflated size, which is the number the firmware sizes its
    scratch buffer from.

    Note the two different sizes returned and do not confuse them: `file_size`
    is the compressed bytes on the SD card, `inflated_size` is the RAM the
    device must have free to open the book.

    Args:
        path: Path to a .rabook container.

    Returns:
        Dict with "title", "author", "language" (decoded with replacement, so
        never raises on bad UTF-8), "file_size", "inflated_size", "chapters"
        and "images".

    Raises:
        ValueError: Not a valid RBKC container, prefixed with the file path so
            the caller's loop reports which book failed.
        OSError: `path` cannot be read.
    """
    with Path(path).open("rb") as fh:
        container = fh.read()
    try:
        flat = unwrap_container(container)
    except ValueError as exc:
        msg = f"{path}: {exc}"
        raise ValueError(msg) from exc
    inflated_size = len(flat)
    h = struct.unpack("<8s23I", flat[:100])
    string_off = h[19]

    def s(off: int) -> str:
        start = string_off + off
        end = flat.index(b"\x00", start)
        return flat[start:end].decode("utf-8", "replace")

    return {
        "title": s(h[4]),
        "author": s(h[5]),
        "language": s(h[6]),
        "file_size": len(container),
        "inflated_size": inflated_size,
        "chapters": h[9],
        "images": h[17],
    }


#: The fixed half of the generated header: the file doc block and the entry
#: struct. Nothing here depends on the books, so it is a constant rather than
#: 30 more lines inside the generator -- what varies and what does not should
#: be readable apart.
_HEADER_PREAMBLE = (
    "/**",
    " * @file ra8_book_library.h",
    " * @brief Auto-generated index of the compiled e-book library.",
    " *",
    " * @details",
    " * Generated by tools/epub_compile/gen_manifest.py from the .rabook blobs in",
    " * content/compiled. DO NOT EDIT BY HAND -- re-run the generator instead. Each",
    " * entry names a book and the scratch size its `ra8_book_open()` needs.",
    " *",
    " * @since Version 1.0.0",
    " */",
    "#pragma once",
    "",
    "#include <stdint.h>",
    "",
    "/**",
    " * @struct ra8_book_library_entry_t",
    " * @brief One bundled book: display metadata plus storage/inflate sizes.",
    " * @since Version 1.0.0",
    " */",
    "typedef struct {",
    "  const char* title;       /**< Book title (transliterated to ASCII). */",
    "  const char* author;      /**< Author (transliterated to ASCII).     */",
    "  const char* filename;    /**< `.rabook` file name on storage.       */",
    "  uint32_t    file_size;   /**< Compressed size on disk, bytes.       */",
    "  uint32_t    inflated_size; /**< Scratch bytes ra8_book_open() needs. */",
    "} ra8_book_library_entry_t;",
    "",
)


def _generated_constants(entries: list[dict[str, object]], biggest: int) -> list[str]:
    """The two C23 typed enums whose values come from the scanned books."""
    return [
        "/**",
        " * @enum ra8_book_library_count_t",
        " * @brief Number of books in @ref g_ra8_book_library.",
        " * @since Version 1.0.0",
        " */",
        "typedef enum : uint16_t {",
        f"  k_ra8_book_library_count = {len(entries)}U, /**< Bundled book count. */",
        "} ra8_book_library_count_t;",
        "",
        "/**",
        " * @enum ra8_book_library_scratch_t",
        " * @brief Largest inflate scratch any bundled book needs, bytes.",
        " * @details Size a shared open() buffer to this and every book fits.",
        " * @since Version 1.0.0",
        " */",
        "typedef enum : uint32_t {",
        f"  k_ra8_book_library_max_inflated = {biggest}U, /**< Worst-case inflated size. */",
        "} ra8_book_library_scratch_t;",
        "",
    ]


def _generated_table(entries: list[dict[str, object]]) -> list[str]:
    """The book table itself, one initialiser row per book."""
    lines = [
        "/** @brief The bundled book index. Generated; data table, magic numbers ok. */",
        "// NOLINTBEGIN(readability-magic-numbers)",
        "static const ra8_book_library_entry_t g_ra8_book_library[k_ra8_book_library_count] = {",
    ]
    lines.extend(
        f'    {{ "{ascii_only(e["title"])}", "{ascii_only(e["author"])}", '
        f'"{ascii_only(e["filename"])}", {e["file_size"]}U, {e["inflated_size"]}U }},'
        for e in entries
    )
    lines += ["};", "// NOLINTEND(readability-magic-numbers)", ""]
    return lines


def _build_header_lines(entries: list[dict[str, object]]) -> tuple[list[str], int]:
    """Return the C header lines for the given book entries, and the worst-case size."""
    biggest = max((e["inflated_size"] for e in entries), default=0)
    lines = [
        *_HEADER_PREAMBLE,
        *_generated_constants(entries, biggest),
        *_generated_table(entries),
    ]
    return lines, biggest


def main() -> int:
    """Scan a directory of .rabook files and write the generated C header.

    Books are ordered by filename, and that order fixes the indices in
    `g_ra8_book_library[]`. Anything that persists a book index -- a saved
    reading position, a baked fixture -- is invalidated by adding or renaming a
    file in the directory.

    The header is written with `encoding="ascii"`, so a title that survived
    `ascii_only` but still holds a non-ASCII byte raises here rather than
    producing a source file the encoding gate would later reject.

    Non-.rabook files in the directory are ignored; an unreadable or corrupt
    .rabook aborts the whole run rather than being skipped, because a manifest
    that silently omits a book would under-size the inflate scratch buffer.
    """
    ap = argparse.ArgumentParser(description="Generate the ra8_book library manifest header.")
    ap.add_argument("compiled_dir", help="directory of .rabook files")
    ap.add_argument("output", help="path to the generated header")
    args = ap.parse_args()

    compiled_dir = Path(args.compiled_dir)
    files = sorted(f.name for f in compiled_dir.iterdir() if f.name.endswith(".rabook"))
    entries = []
    for name in files:
        meta = read_meta(compiled_dir / name)
        meta["filename"] = name
        entries.append(meta)

    lines, biggest = _build_header_lines(entries)

    with Path(args.output).open("w", encoding="ascii") as fh:
        fh.write("\n".join(lines))
    total_disk = sum(e["file_size"] for e in entries)
    print(
        f"wrote {args.output}: {len(entries)} books, "
        f"{total_disk // 1024 // 1024} MB on disk, max inflate {biggest // 1024} KB"
    )


if __name__ == "__main__":
    main()
