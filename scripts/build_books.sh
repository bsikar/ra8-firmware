#!/usr/bin/env bash
#
# build_books.sh -- regenerate the compiled e-book library.
#
# Compiles every content/library/*.epub master (the Git-LFS source of truth)
# into content/compiled/*.rabook and regenerates the manifest header
# libs/ra_book/inc/ra_book_library.h. The .rabook blobs and the manifest are
# build artifacts (gitignored): they are 100% derived from the epubs plus
# tools/epub_compile, so they are regenerated rather than committed.
#
# Image downscale target (long edge, pixels) is RA_BOOK_MAX_EDGE; keep it at or
# below the panel resolution so no stored pixels are wasted.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$ROOT/content/library"
OUT_DIR="$ROOT/content/compiled"
COMPILER="$ROOT/tools/epub_compile/epub_compile.py"
MANIFEST_GEN="$ROOT/tools/epub_compile/gen_manifest.py"
MANIFEST_HDR="$ROOT/libs/ra_book/inc/ra_book_library.h"
MAX_EDGE="${RA_BOOK_MAX_EDGE:-1024}"

if ! ls "$SRC_DIR"/*.epub >/dev/null 2>&1; then
    echo "build_books: no .epub masters in $SRC_DIR" >&2
    echo "             (run 'git lfs pull' to fetch them)" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
count=0
for epub in "$SRC_DIR"/*.epub; do
    base="$(basename "$epub" .epub)"
    python3 "$COMPILER" "$epub" "$OUT_DIR/$base.rabook" --max-edge "$MAX_EDGE"
    count=$((count + 1))
done

python3 "$MANIFEST_GEN" "$OUT_DIR" "$MANIFEST_HDR"

total_kb=$(du -sk "$OUT_DIR" | cut -f1)
echo "build_books: $count book(s), max-edge=$MAX_EDGE, $((total_kb / 1024)) MB in $OUT_DIR"
echo "build_books: manifest -> $MANIFEST_HDR"
