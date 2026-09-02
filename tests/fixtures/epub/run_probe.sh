#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Build + run the host EPUB probe against a real .epub.
#   ./tests/fixtures/epub/run_probe.sh <file.epub>
# Host-only diagnostic (RA8_OFF_TARGET, raw-descriptor streamed); reports what
# epub_open() extracts (parse result, spine, TOC, cover, chapter 0).
#
set -euo pipefail

ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
cd "$ROOT"
EPUB="${1:?usage: run_probe.sh <file.epub>}"
OUT="$(mktemp -d)"
INC=(-Iapps/shared_libs/epub/inc -Iapps/shared_libs/epub/src -Iapps/shared_libs/xml/inc
  -Iapps/shared_libs/xml/src -Ilibs/ra8_core/inc -Iapps/shared_libs/third_party/miniz
  -Iapps/shared_libs/third_party/stb -Iapps/shared_libs/reflow/inc -Itests/support/inc)
# No -w. This loop compiles fifteen translation units, thirteen of them
# first-party, and -w switched every diagnostic off for all of them. Measured
# with it removed under clang-22 and gcc-14 (this script's own two compiler
# choices): the entire set is silent except for ONE informational line, the
# upstream #pragma message at miniz.c:3185 about large-file I/O -- a note
# under gcc, [-W#pragma-messages] under clang. There is no -Wall and no
# -Werror here, so nothing can fail the probe; a blanket that buys one line
# of quiet at the cost of every warning on first-party EPUB/XML code is not a
# trade worth making.
D=(-DRA8_OFF_TARGET=1 -DRA8_LOG_LEVEL=0)
CC_BIN="${CC:-clang}"
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
  CC_BIN=clang-22
fi

C_SRCS=(apps/shared_libs/epub/tests/src/epub_probe.c
  apps/shared_libs/epub/src/epub_open.c
  apps/shared_libs/epub/src/epub_chapter.c
  apps/shared_libs/epub/src/epub_miniz_alloc.c
  apps/shared_libs/epub/src/epub_xml_shim.c
  apps/shared_libs/epub/src/epub_xml_toc.c
  apps/shared_libs/epub/src/epub_zip_guard.c
  libs/ra8_core/src/ra8_decomp_limits.c
  apps/shared_libs/xml/src/xml.c
  apps/shared_libs/xml/src/xml_decode.c
  apps/shared_libs/xml/src/xml_doctype.c
  apps/shared_libs/reflow/src/ra8_img_arena.c
  apps/shared_libs/reflow/src/ra8_stbtt_guard.c
  apps/shared_libs/third_party/miniz/miniz.c
  apps/shared_libs/third_party/stb/stb_truetype_impl.c)

for c in "${C_SRCS[@]}"; do
  "$CC_BIN" -std=gnu2x "${D[@]}" "${INC[@]}" -c "$c" -o "$OUT/$(basename "$c").o"
done
"$CC_BIN" "$OUT"/*.o -lm -o "$OUT/epub_probe"
"$OUT/epub_probe" "$EPUB"
rm -rf "$OUT"
