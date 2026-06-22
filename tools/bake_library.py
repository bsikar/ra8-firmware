#!/usr/bin/env python3
# Bake a set of compiled .rabook files into a C header of MRAM-resident byte
# arrays plus a lookup table. Each blob is the DEFLATE-wrapped RBKZ container as
# produced by tools/epub_compile; the firmware inflates it into SDRAM on demand
# via ra_book_open(). Full books (with cover + inline images) are kept compressed
# in MRAM and only expanded when opened, so several fit alongside the firmware.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# Usage: bake_library.py <out.h> <rabook>|<title>|<author> [<rabook>|<title>|<author> ...]
import sys


def emit_array(name, data):
    out = [f"alignas(4) static const uint8_t {name}[{len(data)}U] = {{"]
    line = "  "
    for i, b in enumerate(data):
        line += f"{b}U,"
        if len(line) >= 96:
            out.append(line)
            line = "  "
    if line.strip():
        out.append(line)
    out.append("};")
    return "\n".join(out)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: bake_library.py <out.h> <rabook>|<title>|<author> ...\n")
        return 2
    out_path = argv[1]
    books = []
    for spec in argv[2:]:
        path, title, author = spec.split("|")
        with open(path, "rb") as f:
            books.append((f.read(), title, author))
    parts = [
        "/**",
        " * @file library.h",
        " * @brief Baked full .rabook blobs (cover + images + all chapters) the reader opens (generated).",
        " * @details Each entry is the compressed RBKZ container; ra_book_open() inflates it",
        " *          into SDRAM on demand. Regenerate with tools/bake_library.py.",
        " * @since Version 1.0.0",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "// NOLINTBEGIN(readability-magic-numbers)",
    ]
    names = []
    for i, (data, title, author) in enumerate(books):
        name = f"k_lib_blob{i:02d}"
        names.append((name, len(data), title, author))
        parts.append(f'/** @brief "{title}" by {author} -- {len(data)} bytes. */')
        parts.append(emit_array(name, data))
        parts.append("")
    parts.append("/** @brief One openable baked book: compressed blob + display metadata. */")
    parts.append("typedef struct {")
    parts.append("  const uint8_t* blob;   /**< RBKZ container start.        */")
    parts.append("  uint32_t       len;    /**< Container length in bytes.   */")
    parts.append("  const char*    title;  /**< Display title.               */")
    parts.append("  const char*    author; /**< Display author.              */")
    parts.append("} library_book_t;")
    parts.append("")
    parts.append("typedef enum : uint16_t {")
    parts.append(f"  k_library_count = {len(books)}U,")
    parts.append("} library_count_t;")
    parts.append("")
    parts.append("static const library_book_t k_library[k_library_count] = {")
    for name, n, title, author in names:
        t = title.replace('"', '\\"')
        a = author.replace('"', '\\"')
        parts.append(f'    {{ {name}, {n}U, "{t}", "{a}" }},')
    parts.append("};")
    parts.append("// NOLINTEND(readability-magic-numbers)")
    parts.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(parts))
    sys.stderr.write(f"bake_library: wrote {out_path} ({len(books)} books, "
                     f"{sum(len(d) for d, _, _ in books)} bytes)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
