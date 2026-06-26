#!/usr/bin/env bash
# Build + run the host EPUB probe against a real .epub.
#   ./tests/fixtures/epub/run_probe.sh <file.epub>
# Host-only diagnostic (RA_SIMULATOR_MODE, malloc-backed); reports what
# ra_epub_open() extracts (parse result, spine, TOC, cover, chapter 0).
#
# @copyright Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
set -euo pipefail

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$ROOT"
EPUB="${1:?usage: run_probe.sh <file.epub>}"
OUT="$(mktemp -d)"
INC=(-Ilibs/ra_epub/inc -Ilibs/ra_core/inc -Ilibs/third_party/miniz
  -Ilibs/third_party/tinyxml2 -Ilibs/third_party/stb -Ilibs/ra_reflow/inc)
D=(-DRA_SIMULATOR_MODE=1 -w)

C_SRCS=(tests/fixtures/epub/epub_probe.c
  libs/ra_epub/src/ra_epub_open.c
  libs/ra_epub/src/ra_epub_chapter.c
  libs/ra_epub/src/ra_epub_miniz_alloc.c
  libs/ra_reflow/src/ra_img_arena.c
  libs/third_party/miniz/miniz.c
  libs/third_party/stb/stb_truetype_impl.c)
CXX_SRCS=(libs/ra_epub/src/ra_epub_xml_shim.cpp
  libs/ra_epub/src/ra_epub_cpp_alloc.cpp
  libs/third_party/tinyxml2/tinyxml2.cpp)

for c in "${C_SRCS[@]}"; do
  clang -std=gnu2x "${D[@]}" "${INC[@]}" -c "$c" -o "$OUT/$(basename "$c").o"
done
for cc in "${CXX_SRCS[@]}"; do
  clang++ -std=gnu++2b "${D[@]}" "${INC[@]}" -c "$cc" -o "$OUT/$(basename "$cc").o"
done
clang++ "$OUT"/*.o -o "$OUT/epub_probe"
"$OUT/epub_probe" "$EPUB"
rm -rf "$OUT"
