#!/usr/bin/env bash
#
# build_books.sh -- regenerate the compiled e-book library.
#
# Compiles every content/library/*.epub source (the Git-LFS source of truth)
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
  echo "build_books: no .epub sources in $SRC_DIR" >&2
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

# The generated headers must be stable against the repo's format gates
# (clang-format owns the comment start column, check_comment_format owns the
# trailing */ alignment -- same order scripts/format_code.sh applies), so a
# regeneration never reintroduces formatting drift the gates reject.
format_generated() {
  local hdr="$1"
  if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$hdr"
  fi
  python3 "$ROOT/scripts/utils/check_comment_format.py" --fix "$hdr" >/dev/null
}
format_generated "$MANIFEST_HDR"

# Bake the ereader_shelf demo subset -- a few full books (cover + images) -- into
# its MRAM library header. Like the .rabook blobs and the manifest above, this
# header is generated from the Git-LFS source epubs, not committed.
SHELF_HDR="$ROOT/examples/ek_ra8d2/hw_validated/hil/ereader_shelf/library.h"
python3 "$ROOT/tools/bake_library.py" "$SHELF_HDR" \
  "$OUT_DIR/The Time Machine - H G Wells.rabook|The Time Machine|H. G. Wells" \
  "$OUT_DIR/The Strange Case of Dr Jekyll and Mr Hyde - Robert Louis Stevenson.rabook|The Strange Case of Dr. Jekyll and Mr. Hyde|Robert Louis Stevenson" \
  "$OUT_DIR/Journey to the Center of the Earth - Jules Verne.rabook|Journey to the Center of the Earth|Jules Verne"
format_generated "$SHELF_HDR"

total_kb=$(du -sk "$OUT_DIR" | cut -f1)
echo "build_books: $count book(s), max-edge=$MAX_EDGE, $((total_kb / 1024)) MB in $OUT_DIR"
echo "build_books: manifest -> $MANIFEST_HDR"
echo "build_books: ereader_shelf library -> $SHELF_HDR"
