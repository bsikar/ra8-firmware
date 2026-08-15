#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Build + run the host EPUB probe against a real .epub.
#   ./tests/fixtures/epub/run_probe.sh <file.epub>
# Host-only diagnostic (RA8_OFF_TARGET, raw-descriptor streamed); reports what
# ra8_epub_open() extracts (parse result, spine, TOC, cover, chapter 0).
#
set -euo pipefail

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$ROOT"
EPUB="${1:?usage: run_probe.sh <file.epub>}"
OUT="$(mktemp -d)"
INC=(-Ilibs/ra8_epub/inc -Ilibs/ra8_epub/src -Ilibs/ra8_xml/inc
  -Ilibs/ra8_xml/src -Ilibs/ra8_core/inc -Ilibs/third_party/miniz
  -Ilibs/third_party/stb -Ilibs/ra8_reflow/inc -Itests/support)
D=(-DRA8_OFF_TARGET=1 -DRA8_LOG_LEVEL=0 -w)
CC_BIN="${CC:-clang}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
  CC_BIN=clang-22
fi

C_SRCS=(tests/fixtures/epub/epub_probe.c
  libs/ra8_epub/src/ra8_epub_open.c
  libs/ra8_epub/src/ra8_epub_chapter.c
  libs/ra8_epub/src/ra8_epub_miniz_alloc.c
  libs/ra8_epub/src/ra8_epub_xml_shim.c
  libs/ra8_epub/src/ra8_epub_zip_guard.c
  libs/ra8_core/src/ra8_decomp_limits.c
  libs/ra8_xml/src/ra8_xml.c
  libs/ra8_xml/src/ra8_xml_doctype.c
  libs/ra8_reflow/src/ra8_img_arena.c
  libs/ra8_reflow/src/ra8_stbtt_guard.c
  libs/third_party/miniz/miniz.c
  libs/third_party/stb/stb_truetype_impl.c)

for c in "${C_SRCS[@]}"; do
  "$CC_BIN" -std=gnu2x "${D[@]}" "${INC[@]}" -c "$c" -o "$OUT/$(basename "$c").o"
done
"$CC_BIN" "$OUT"/*.o -lm -o "$OUT/epub_probe"
"$OUT/epub_probe" "$EPUB"
rm -rf "$OUT"
