#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# build_books.sh -- regenerate the compiled e-book library.
#
# Compiles every e-reader content-library .epub source (the Git-LFS source of truth)
# into the e-reader content tree as *.rabook and regenerates the manifest header
# apps/shared_libs/book/inc/book_library.h. The .rabook blobs and the manifest are
# build artifacts (gitignored): they are 100% derived from the epubs plus
# tools/epub_compile, so they are regenerated rather than committed.
#
# Image downscale target (long edge, pixels) is RA8_BOOK_MAX_EDGE. The compiler
# default is now NO downscale (issue #210: full-res sources for the zoom
# loupe); the library build stays an EXPLICIT opt-in use of the knob because
# these baked TFT-class fixtures pin goldens on their pixel content and gain
# nothing from full-res storage.
#
# The ereader_shelf library.h bake is NOT reproducible across machines: the
# cover-thumbnail bytes depend on the JPEG decoder's SIMD rounding (libjpeg
# via Pillow differs between x86_64 and Apple-silicon even at the SAME
# Pillow version -- verified 12.2.0 on both), and the EIL fb-hash golden
# (ereader_shelf hil.conf, fb=FA3AB5B5) covers the rendered thumbnails.
# library.h is therefore COMMITTED as an @generated fixture (see .gitignore
# note) and CI never re-bakes it. Re-run this script only to change the
# library deliberately, and re-pin the fb golden in the same change.
#
# Modes:
#   build_books.sh                regenerate the WHOLE library + manifest, then
#                                 bake the ereader_shelf subset (`just tools::books`).
#   build_books.sh --shelf-only   compile ONLY the three ereader_shelf books and
#                                 bake examples/.../ereader_shelf/inc/library.h, with
#                                 no full-library compile or manifest. This is the
#                                 fast path the EIL suite / CI use to make
#                                 ereader_shelf build on a clean checkout (its
#                                 library.h is the only generated header the app
#                                 needs, and it derives from just three epubs).
#
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC_DIR="$ROOT/apps/board/stand_alone/ereader/content/library"
OUT_DIR="$ROOT/apps/board/stand_alone/ereader/content/compiled"
COMPILER="$ROOT/tools/epub_compile/src/epub_compile.py"
MANIFEST_GEN="$ROOT/tools/epub_compile/src/gen_manifest.py"
MANIFEST_HDR="$ROOT/apps/shared_libs/book/inc/book_library.h"
SHELF_HDR="$ROOT/examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/inc/library.h"
MAX_EDGE="${RA8_BOOK_MAX_EDGE:-1024}"

# Single source of truth for the ereader_shelf demo subset: one entry per baked
# book as "<epub/rabook basename>|<display title>|<author>" (the epub and its
# compiled .rabook share a basename). Consumed by BOTH modes -- full
# `just tools::books`
# and `--shelf-only` bake the same subset from this list, so the shelf contents
# live in exactly one place.
SHELF_BOOKS=(
  "The Time Machine - H G Wells|The Time Machine|H. G. Wells"
  "The Strange Case of Dr Jekyll and Mr Hyde - Robert Louis Stevenson|The Strange Case of Dr. Jekyll and Mr. Hyde|Robert Louis Stevenson"
  "Journey to the Center of the Earth - Jules Verne|Journey to the Center of the Earth|Jules Verne"
)

SHELF_ONLY=0
if [ "${1:-}" = "--shelf-only" ]; then
  SHELF_ONLY=1
elif [ $# -gt 0 ]; then
  echo "build_books: unknown argument '$1' (only --shelf-only is accepted)" >&2
  exit 2
fi

# The generated headers must be stable against the repo's format gates
# (clang-format owns the comment start column, check_comment_format owns the
# trailing */ alignment -- same order scripts/checks/format_code.sh applies), so a
# regeneration never reintroduces formatting drift the gates reject.
format_generated() {
  local hdr="$1"
  if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$hdr"
  fi
  python3 "$ROOT/scripts/checks/check_comment_format.py" --fix "$hdr" >/dev/null
}

# Compile one book's epub to its .rabook (basename shared between the two).
compile_book() {
  local base="$1"
  if [ ! -f "$SRC_DIR/$base.epub" ]; then
    echo "build_books: missing $SRC_DIR/$base.epub" >&2
    echo "             (run 'git lfs pull' to fetch the epub sources)" >&2
    exit 1
  fi
  python3 "$COMPILER" "$SRC_DIR/$base.epub" "$OUT_DIR/$base.rabook" --max-edge "$MAX_EDGE"
}

mkdir -p "$OUT_DIR"

count=0
if [ "$SHELF_ONLY" -eq 1 ]; then
  # Fast path: only the shelf subset, no full-library compile or manifest.
  for spec in "${SHELF_BOOKS[@]}"; do
    compile_book "${spec%%|*}"
    count=$((count + 1))
  done
else
  # Full library: compile every epub, then regenerate the manifest header.
  if ! ls "$SRC_DIR"/*.epub >/dev/null 2>&1; then
    echo "build_books: no .epub sources in $SRC_DIR" >&2
    echo "             (run 'git lfs pull' to fetch them)" >&2
    exit 1
  fi
  for epub in "$SRC_DIR"/*.epub; do
    base="$(basename "$epub" .epub)"
    python3 "$COMPILER" "$epub" "$OUT_DIR/$base.rabook" --max-edge "$MAX_EDGE"
    count=$((count + 1))
  done
  python3 "$MANIFEST_GEN" "$OUT_DIR" "$MANIFEST_HDR"
  format_generated "$MANIFEST_HDR"
fi

# Bake the ereader_shelf demo subset -- a few full books (cover + images) -- into
# its MRAM library header. Like the .rabook blobs and the manifest above, this
# header is generated from the Git-LFS source epubs, not committed. Built in both
# modes from the shared SHELF_BOOKS list.
shelf_args=()
for spec in "${SHELF_BOOKS[@]}"; do
  base="${spec%%|*}"
  meta="${spec#*|}"
  shelf_args+=("$OUT_DIR/$base.rabook|$meta")
done
python3 "$ROOT/tools/epub_compile/src/bake_library.py" "$SHELF_HDR" "${shelf_args[@]}"
format_generated "$SHELF_HDR"

if [ "$SHELF_ONLY" -eq 1 ]; then
  echo "build_books: shelf-only -- $count book(s), max-edge=$MAX_EDGE"
  echo "build_books: ereader_shelf library -> $SHELF_HDR"
else
  total_kb=$(du -sk "$OUT_DIR" | cut -f1)
  echo "build_books: $count book(s), max-edge=$MAX_EDGE, $((total_kb / 1024)) MB in $OUT_DIR"
  echo "build_books: manifest -> $MANIFEST_HDR"
  echo "build_books: ereader_shelf library -> $SHELF_HDR"
fi
